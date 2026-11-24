// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "Dialogs/AudioExporterProgressWindow.h"
#include "AudioExportPipeline.h"
#include "AudioExporterBPLibrary.h"
#include "AudioExporterRuntimeSubsystem.h"
#include "Utilities/AudioExporterLogger.h"
#include "Utilities/AudioExporterSettings.h"
#include "AudioFactoryUI.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "SAudioMeter.h"
#include "AudioMeterTypes.h"
#include "Sound/SoundSubmix.h"
#include "Dialogs/AudioMetricListener.h"
#include "Dialogs/FAudioFactoryDialogWindow.h"
#include "Engine/World.h"
#include "DSP/Dsp.h"
#include "HAL/PlatformTime.h"
#include "InputCoreTypes.h"

using namespace AudioExporter;

#define LOCTEXT_NAMESPACE "AudioExporterProgress"

namespace
{
	constexpr float kTerminalToastExpireSeconds = 10.0f;

	/** Combines the current progress status and elapsed duration into the shared body subline. */
	FText FormatProgressSubline(const FText& StatusText, const FText& ElapsedText)
	{
		return FText::Format(LOCTEXT("ProgressSublineFmt", "{0}\n{1}"), StatusText, ElapsedText);
	}

	FText FormatFileCount(const int32 Count)
	{
		return FText::Format(
			LOCTEXT("FileCount", "{0} {0}|plural(one=file,other=files)"),
			FText::AsNumber(Count));
	}

	FText FormatReportOutputFailureCount(const int32 Count)
	{
		return FText::Format(
			LOCTEXT("ReportOutputFailureCount", "{0} {0}|plural(one=report output failed,other=report outputs failed)"),
			FText::AsNumber(Count));
	}

	FText MergeTerminalDetails(const FString& ErrorMessage, const FString& ReportErrorMessage)
	{
		if (ErrorMessage.IsEmpty())
		{
			return FText::FromString(ReportErrorMessage);
		}
		if (ReportErrorMessage.IsEmpty() || ErrorMessage.Contains(ReportErrorMessage, ESearchCase::CaseSensitive))
		{
			return FText::FromString(ErrorMessage);
		}

		return FText::Format(
			LOCTEXT("ReportFailureDetail", "{0} Report failure: {1}"),
			FText::FromString(ErrorMessage),
			FText::FromString(ReportErrorMessage));
	}
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
SAudioExportProgressWindow::~SAudioExportProgressWindow()
{
	if (MetricListener)
	{
		MetricListener->OnEnvelopeValue.RemoveAll(this);
		MetricListener->RemoveFromRoot();
		MetricListener = nullptr;
	}

	if (RecordingSubmix.IsValid() && RecordingWorld.IsValid())
	{
		RecordingSubmix->RemoveEnvelopeFollowerDelegate(RecordingWorld.Get(), SubmixEnvelopeDelegate);
	}

	if (UAudioExporterRuntimeSubsystem* RuntimeSubsystem = UAudioExporterBPLibrary::GetRuntimeSubsystem())
	{
		RuntimeSubsystem->OnExportCompleteResultNative.RemoveAll(this);
	}
}

// ---------------------------------------------------------------------------
// Construct
// ---------------------------------------------------------------------------
void SAudioExportProgressWindow::Construct(const FArguments& InArgs)
{
	OnMonitorChanged = InArgs._OnMonitorChanged;
	OnCancelRequested = InArgs._OnCancelRequested;
	OnRequestRecordingSubmix = InArgs._OnRequestRecordingSubmix;
	OnRequestWorld = InArgs._OnRequestWorld;
	OnIsExportActive = InArgs._OnIsExportActive;

	ExporterSettings = GetMutableDefault<UAudioExporterSettings>();
	if (!ensureMsgf(ExporterSettings, TEXT("%s: Audio Exporter settings were not available."), *FString(__FUNCTION__)))
	{
		UE_LOG(LogAudioExporter, Error, TEXT("%s: Audio Exporter settings were not available."), *FString(__FUNCTION__));
		return;
	}
	ExporterSettings->LoadConfig();

	TotalAssetsCount = InArgs._Assets.Num();
	ExportStartTime  = FPlatformTime::Seconds();
	CurrentStatusSubline = FText::Format(
		LOCTEXT("InitSummary", "Preparing to export {0} out of {1}"),
		FText::AsNumber(0),
		FText::AsNumber(TotalAssetsCount));
	CurrentElapsedText = LOCTEXT("TimeInitial", "Elapsed: 00:00");

	// Consume the structured terminal result directly instead of leaving the UI to
	// infer success/partial/failure from the legacy empty-string path. This lane owns only the
	// delegate hookup (not the caller that opens this window, and not the visual presentation the
	// cached outcome below feeds); the window resolves the same runtime subsystem the BP surface
	// uses so no other file needs to change for this binding to take effect.
	if (UAudioExporterRuntimeSubsystem* RuntimeSubsystem = UAudioExporterBPLibrary::GetRuntimeSubsystem())
	{
		RuntimeSubsystem->OnExportCompleteResultNative.AddSP(this, &SAudioExportProgressWindow::OnExportFinishedResult);
	}

	// One task-log entry per asset, fed to the shared SAudioFactoryTaskLogView via
	// ProgressBody below instead of hand-rolled icon/name TextBlock rows. Starts Neutral (pending);
	// UpdateProgressUI mutates Severity/Text in place as each row's real outcome arrives.
	FileLogEntries.Reserve(InArgs._Assets.Num());
	for (const FAssetData& Asset : InArgs._Assets)
	{
		FileLogEntries.Add(MakeShared<AudioExporter::FAudioFactoryTaskLogEntry>(
			AudioExporter::EAudioFactoryStatusSeverity::Neutral,
			FText::FromString(Asset.AssetName.ToString())));
	}

	// Audio metering setup
	MetricListener = NewObject<UAudioMetricListener>();
	MetricListener->AddToRoot();
	MetricListener->OnEnvelopeValue.AddSP(this, &SAudioExportProgressWindow::OnSubmixEnvelope);

	MeterStyle = FAudioMeterStyle::GetDefault();
	MeterStyle.SetDecibelsPerHash(10.0f);
	MeterStyle.SetValueRangeDb(FVector2D(-40.0f, 0.0f));
	MeterStyle.SetShowScale(false);
	MeterStyle.SetMeterSize(FVector2D(200.0f, 6.0f));
	MeterStyle.SetScaleHashHeight(2.f);
	MeterStyle.SetMeterValuePadding(1.f);

	TryAttachMeterSource();

	// Build the full widget tree
	TArray<FAudioFactoryInlineCount> HeaderCounts;
	HeaderCounts.Add(FAudioFactoryInlineCount::Make(
		LOCTEXT("SelectedCount", "Selected"),
		TAttribute<FText>::CreateLambda([this]()
		{
			return FText::AsNumber(TotalAssetsCount);
		}),
		LOCTEXT("SelectedCountTooltip", "Assets included in this export batch.")));

	ChildSlot
	[
		SNew(SAudioFactoryPluginWindowFrame)
		.ProductId(EAudioFactoryProductId::AudioExporter)
		.Counts(HeaderCounts)
		// No frame Status feed: the v2.0 design removed the bottom status strip from every
		// product window, so nothing renders it. BuildStatusStripStatus below survives only as
		// the summary computation UI automation asserts against.
		// Escape cancels/closes this window, routed through the same
		// terminal-aware logic as the OS close button so an active export is cancelled rather
		// than the window being torn down mid-session.
		.OnCancelRequested([this]()
		{
			RequestWindowCloseOrCancel();
		})
		.Body()
		[
			SNew(SVerticalBox)

			// -- Progress section (shared scaffold) ----------------
			+ SVerticalBox::Slot()
			.FillHeight(1.f)
			.Padding(FMargin(20.f, 14.f, 20.f, 4.f))
			[
				SAssignNew(ProgressBody, AudioExporter::SAudioFactoryProgressBody)
				.Headline(LOCTEXT("ProgressHeadline", "Exporting selected audio"))
				.Subline(FormatProgressSubline(CurrentStatusSubline, CurrentElapsedText))
				.ProgressFraction(TOptional<float>())
				.LogItems(&FileLogEntries)
			]

			// -- Audio monitor section -----------------------------------------
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(20.f, 4.f, 20.f, 4.f))
			[
				SNew(SVerticalBox)

				// Audio meter
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 0.f, 0.f, 6.f)
				[
					SAssignNew(AudioMeter, SAudioMeter)
					.Orientation(Orient_Horizontal)
					.MeterChannelInfo(this, &SAudioExportProgressWindow::GetMeterChannelInfo)
					.BackgroundColor(FLinearColor(0.f, 0.f, 0.f, 0.f))
					.MeterValueColor(FAudioFactoryTheme::GetAccentSecondary())
					.Style(&MeterStyle)
				]

	// "Listen while exporting" row
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				.Padding(0.f, 0.f, 0.f, 12.f)
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.f, 0.f, 10.f, 0.f)
					[
						SNew(SAudioFactoryToggle)
						.bChecked(ExporterSettings->bMonitor)
						.OnToggleChanged(FOnAudioFactoryToggleChanged::CreateSP(this, &SAudioExportProgressWindow::OnMonitorToggleChanged))
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
					SNew(STextBlock)
		.Text(LOCTEXT("MonitorLabel", "Listen while exporting"))
					.ToolTipText(LOCTEXT("MonitorTooltip", "Monitor the recording submix while audio is exported."))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", FAudioFactoryConstants::FontSizeSmall))
						.ColorAndOpacity(FSlateColor(FAudioFactoryTheme::GetLabel()))
					]
				]

			]
		]
		.CancelAction()
		[
			SNew(SButton)
			.Text(LOCTEXT("CancelButton", "Cancel"))
			.ToolTipText(LOCTEXT("CancelTooltip", "Cancel the active export."))
			.OnClicked(this, &SAudioExportProgressWindow::OnCancelExportClicked)
		]
	];

	// Activate the meter's render timer
	if (AudioMeter.IsValid())
	{
		AudioMeter->RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateLambda(
			[](double, float) { return EActiveTimerReturnType::Continue; }));
	}

	// Start the per-second elapsed-time ticker.
	RegisterActiveTimer(1.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SAudioExportProgressWindow::TickElapsedTime));
}


// ---------------------------------------------------------------------------
// UpdateProgressUI
// ---------------------------------------------------------------------------
void SAudioExportProgressWindow::UpdateProgressUI(const FAudioExportProgressInfo& ExportingProgress)
{
	TryAttachMeterSource();

	if (ExportingProgress.bHasResult)
	{
		ExportedFileCount = ExportingProgress.ExportedFiles;
		FailedFileCount = ExportingProgress.FailedFiles;
	}

	// 1. A total is known only when the session publishes its stable file denominator; until then
	// the shared body receives an unset fraction and intentionally renders its marquee state.
	const bool bHasKnownTotal = ExportingProgress.TotalExpectedFiles > 0;
	if (bHasKnownTotal)
	{
		CurrentProgress = FMath::Clamp(
			static_cast<float>(ExportingProgress.ExportedFiles + ExportingProgress.FailedFiles)
				/ static_cast<float>(ExportingProgress.TotalExpectedFiles),
			0.0f,
			1.0f);
	}

	if (ProgressBody.IsValid())
	{
		ProgressBody->SetProgressFraction(bHasKnownTotal
			? TOptional<float>(CurrentProgress)
			: TOptional<float>());
	}

	// 2. Update the subline (the scaffold's Headline is fixed at Construct, so the
	// two text blocks -- overall summary and current-asset detail -- are combined here).
	FText SummaryLine;
	if (FailedFileCount > 0)
	{
		SummaryLine = FText::Format(
			LOCTEXT("ProgressSummaryWithFailures", "Exporting sound {0} out of {1}  |  {2} failed"),
			FText::AsNumber(ExportingProgress.CurrentSound),
			FText::AsNumber(ExportingProgress.TotalSounds),
			FText::AsNumber(FailedFileCount));
	}
	else
	{
		SummaryLine = FText::Format(
			LOCTEXT("ProgressSummary", "Exporting sound {0} out of {1}"),
			FText::AsNumber(ExportingProgress.CurrentSound),
			FText::AsNumber(ExportingProgress.TotalSounds));
	}

	FText DetailLine;
	if (ExportingProgress.bHasResult && !ExportingProgress.bSucceeded)
	{
		DetailLine = FText::Format(
			LOCTEXT("ProgressFailureDetails", "{0}  |  Variation {1}: {2}"),
			FText::FromString(ExportingProgress.CurrentSoundName),
			FText::AsNumber(ExportingProgress.CurrentVariation),
			FText::FromString(ExportingProgress.ResultMessage));
	}
	else
	{
		DetailLine = FText::Format(
			LOCTEXT("ProgressDetails", "{0}  |  Variation {1}"),
			FText::FromString(ExportingProgress.CurrentSoundName),
			FText::AsNumber(ExportingProgress.CurrentVariation));
	}

	CurrentStatusSubline = FText::Format(LOCTEXT("ProgressStatusFmt", "{0}  |  {1}"), SummaryLine, DetailLine);
	if (ProgressBody.IsValid())
	{
		ProgressBody->SetSubline(FormatProgressSubline(CurrentStatusSubline, CurrentElapsedText));
	}

	// 3. Update the per-file task-log row for the sound this update refers to (the
	// row's severity carries the visual state -- Neutral while pending/in-progress, Ready on
	// a real per-variation success, Error on a real per-variation failure -- through the shared
	// SAudioFactoryTaskLogView instead of hand-painted icon/name TextBlocks). Highlight the
	// current row, and only mark completion after the pipeline reports a save result.
	const int32 CurrentFileIndex = ExportingProgress.CurrentSound - 1; // 0-based

	if (FileLogEntries.IsValidIndex(CurrentFileIndex) && FileLogEntries[CurrentFileIndex].IsValid())
	{
		AudioExporter::FAudioFactoryTaskLogEntry& Entry = *FileLogEntries[CurrentFileIndex];

		if (ExportingProgress.bHasResult)
		{
			const bool bRowSucceeded = ExportingProgress.bSucceeded;
			const bool bRowAlreadyFailed = Entry.Severity == AudioExporter::EAudioFactoryStatusSeverity::Error;
			if (!bRowAlreadyFailed)
			{
				Entry.Severity = bRowSucceeded
					? AudioExporter::EAudioFactoryStatusSeverity::Ready
					: AudioExporter::EAudioFactoryStatusSeverity::Error;
			}

			if (!bRowSucceeded && !bRowAlreadyFailed)
			{
				// Capture the plain name BEFORE overwriting Entry.Text with the annotated failure
				// line -- it was set once at Construct from InArgs._Assets and never touched by the
				// Neutral (in-progress) branch below, so it is still the row's real asset name here.
				const FText OriginalName = Entry.Text;
				const FText Reason = ExportingProgress.ResultMessage.IsEmpty()
					? LOCTEXT("FileRowFailureDefault", "Export failed")
					: FText::FromString(ExportingProgress.ResultMessage);
				Entry.Text = FText::Format(
					LOCTEXT("FileRowFailure", "{0}  |  {1}"),
					OriginalName,
					Reason);
			}
			// Succeeded rows keep the plain asset name they were constructed with.
		}
		else if (Entry.Severity != AudioExporter::EAudioFactoryStatusSeverity::Error)
		{
			// In progress / not yet reached: Neutral is both the pending default and the active
			// row's color, matching the shared vocabulary's four-severity model (no separate
			// "in-progress" tone exists in FAudioFactoryStatusIcon). An asset that already failed
			// keeps its Error row while later variations of that same asset run.
			Entry.Severity = AudioExporter::EAudioFactoryStatusSeverity::Neutral;
		}

		if (ProgressBody.IsValid())
		{
			// Refresh only (no forced scroll-to-bottom): the row list is fixed-size and ordered by
			// asset, so jumping the view to the last row mid-batch would hide the row that is
			// actually updating right now.
			ProgressBody->SetLogItems(&FileLogEntries);
		}
	}
}

// ---------------------------------------------------------------------------
// OnExportFinishedProgress
// ---------------------------------------------------------------------------
void SAudioExportProgressWindow::OnExportFinishedProgress(const FString& InExportPath)
{
	ApplyTerminalPresentation(InExportPath);

	const bool bEmptyPath = InExportPath.IsEmpty();
	const bool bAnyExported = ExportedFileCount > 0;
	const bool bSuccess = TerminalOutcome == ETerminalOutcome::Success;
	const FText StructuredDetail = MergeTerminalDetails(TerminalErrorMessage, TerminalReportErrorMessage);

	// Show a terminal notification; startup/cancel/empty outcomes have no folder to open.
	FText NotificationText;
	switch (TerminalOutcome)
	{
	case ETerminalOutcome::Success:
		NotificationText = FText::Format(
			LOCTEXT("NotificationSuccess", "Audio export completed! {0} exported."),
			FormatFileCount(ExportedFileCount));
		break;
	case ETerminalOutcome::Partial:
		NotificationText = FText::Format(
			LOCTEXT("NotificationPartial", "Audio export finished with failures. {0} exported."),
			FormatFileCount(ExportedFileCount));
		if (FailedFileCount > 0)
		{
			NotificationText = FText::Format(
				LOCTEXT("NotificationAppendFailed", "{0} {1} failed."),
				NotificationText,
				FText::AsNumber(FailedFileCount));
		}
		if (SkippedFileCount > 0)
		{
			NotificationText = FText::Format(
				LOCTEXT("NotificationAppendSkipped", "{0} {1} skipped."),
				NotificationText,
				FText::AsNumber(SkippedFileCount));
		}
		if (ReportFailedCount > 0)
		{
			NotificationText = FText::Format(
				LOCTEXT("NotificationAppendReportFailure", "{0} {1}."),
				NotificationText,
				FormatReportOutputFailureCount(ReportFailedCount));
		}
		break;
	case ETerminalOutcome::Cancelled:
		NotificationText = ExportedFileCount > 0
			? FText::Format(
				LOCTEXT("NotificationCancelledWithExports", "Audio export cancelled. {0} exported."),
				FormatFileCount(ExportedFileCount))
			: LOCTEXT("NotificationCancelledWithoutExports", "Audio export cancelled. No files were exported.");
		if (FailedFileCount > 0)
		{
			NotificationText = FText::Format(
				LOCTEXT("NotificationAppendCancelledFailed", "{0} {1} failed."),
				NotificationText,
				FText::AsNumber(FailedFileCount));
		}
		if (SkippedFileCount > 0)
		{
			NotificationText = FText::Format(
				LOCTEXT("NotificationAppendCancelledSkipped", "{0} {1} skipped."),
				NotificationText,
				FText::AsNumber(SkippedFileCount));
		}
		break;
	default:
		if (FailedFileCount > 0 && SkippedFileCount > 0)
		{
			NotificationText = FText::Format(
				LOCTEXT("NotificationFailureWithFailedAndSkipped", "Audio export failed. No files were exported, {0} failed, {1} skipped."),
				FText::AsNumber(FailedFileCount),
				FText::AsNumber(SkippedFileCount));
		}
		else if (FailedFileCount > 0)
		{
			NotificationText = FText::Format(
				LOCTEXT("NotificationFailureWithFailed", "Audio export failed. No files were exported, {0} failed."),
				FText::AsNumber(FailedFileCount));
		}
		else if (SkippedFileCount > 0)
		{
			NotificationText = FText::Format(
				LOCTEXT("NotificationFailureWithSkipped", "Audio export failed. No files were exported, {0} skipped."),
				FText::AsNumber(SkippedFileCount));
		}
		else
		{
			NotificationText = LOCTEXT("NotificationFailure", "Audio export failed. No files were exported.");
		}
		break;
	}
	if (!StructuredDetail.IsEmpty())
	{
		NotificationText = FText::Format(
			LOCTEXT("NotificationAppendDetail", "{0} {1}"),
			NotificationText,
			StructuredDetail);
	}

	FNotificationInfo Info(NotificationText);
	Info.ExpireDuration = kTerminalToastExpireSeconds;
	Info.bUseSuccessFailIcons = true;
	// Only surface the Open-Folder link when at least one file actually landed in the folder.
	if (bAnyExported && !bEmptyPath)
	{
		Info.Hyperlink = FSimpleDelegate::CreateLambda([InExportPath]()
		{
			FPlatformProcess::ExploreFolder(*InExportPath);
		});
		Info.HyperlinkText = LOCTEXT("OpenFolderLink", "Open Folder");
	}

	const SNotificationItem::ECompletionState TerminalCompletionState =
		bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail;

#if WITH_DEV_AUTOMATION_TESTS
	LastTerminalNotificationText = NotificationText;
	bLastNotificationHadOpenFolder = bAnyExported && !bEmptyPath;
	LastTerminalNotificationCompletionState = TerminalCompletionState;
#endif

	TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
	if (Notification.IsValid())
	{
		Notification->SetCompletionState(TerminalCompletionState);
	}

	// Close the progress window
	TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(SharedThis(this));
	if (Window.IsValid())
	{
		Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
		FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
	}
}

void SAudioExportProgressWindow::OnExportFinishedResult(const FAudioExportResult& Result)
{
	// Cache the real outcome ahead of ApplyTerminalPresentation. BroadcastTerminalOnce
	// fires the structured result before the legacy path string (see AudioExportPipeline.cpp), so
	// this always arrives first for a session driven through the real subsystem.
	bHasStructuredOutcome = true;
	ExportedFileCount = FMath::Max(0, Result.SuccessfulCount);
	FailedFileCount = FMath::Max(0, Result.FailedCount);
	SkippedFileCount = FMath::Max(0, Result.SkippedCount);
	ReportFailedCount = FMath::Max(0, Result.ReportsFailed);
	TerminalErrorMessage = Result.ErrorMessage;
	TerminalReportErrorMessage = Result.ReportErrorMessage;
	switch (Result.Outcome)
	{
	case EAudioExportOutcome::Succeeded:
		StructuredOutcome = ETerminalOutcome::Success;
		break;
	case EAudioExportOutcome::PartialSuccess:
		StructuredOutcome = ETerminalOutcome::Partial;
		break;
	case EAudioExportOutcome::Cancelled:
		StructuredOutcome = ETerminalOutcome::Cancelled;
		break;
	case EAudioExportOutcome::Failed:
	case EAudioExportOutcome::Rejected:
	default:
		// All three present as the same non-green terminal state today; no further
		// visual differentiation between them is implemented.
		StructuredOutcome = ETerminalOutcome::Failure;
		break;
	}
}

void SAudioExportProgressWindow::ApplyTerminalPresentation(const FString& InExportPath)
{
	bTerminalStateReached = true;
	OnCancelRequested.Unbind();

	UE_LOG(LogAudioExporter, Log, TEXT("UI: Export finished. Path: %s"), *InExportPath);

	if (bHasStructuredOutcome)
	{
		// The real terminal outcome, not inferred from counts and an empty-path
		// heuristic.
		TerminalOutcome = StructuredOutcome;
	}
	else
	{
		// Defensive fallback only: every production terminal path settles a structured result
		// before this runs (see OnExportFinishedResult above). This keeps callers that invoke
		// OnExportFinishedProgress directly without ever driving the real subsystem -- automation
		// (AudioExporterUITests.cpp) and the WITH_EDITOR capture-state harness above -- working
		// exactly as before.
		// A finished run is a green success ONLY when it wrote at least one file and had no
		// failures. Zero successfully-exported files is a failure/empty outcome even when
		// FailedFileCount == 0 and the path is non-empty (e.g. a read-only or MAX_PATH target
		// silently produced nothing). Cancel/startup aborts force an empty path and land here too.
		// A run with some exports and some failures is a partial completion, not a success.
		const bool bEmptyPath = InExportPath.IsEmpty();
		const bool bAnyExported = ExportedFileCount > 0;
		const bool bSuccess = bAnyExported && FailedFileCount == 0 && !bEmptyPath;
		const bool bPartial = bAnyExported && FailedFileCount > 0 && !bEmptyPath;

		TerminalOutcome = bSuccess
			? ETerminalOutcome::Success
			: (bPartial ? ETerminalOutcome::Partial : ETerminalOutcome::Failure);
	}

	FText TerminalHeadline;
	switch (TerminalOutcome)
	{
	case ETerminalOutcome::Success:
		TerminalHeadline = LOCTEXT("ExportCompleteText", "Export complete");
		break;
	case ETerminalOutcome::Partial:
		TerminalHeadline = LOCTEXT("ExportCompleteWithFailuresText", "Export finished with failures");
		break;
	case ETerminalOutcome::Cancelled:
		TerminalHeadline = LOCTEXT("ExportCancelledText", "Export cancelled");
		break;
	default:
		TerminalHeadline = LOCTEXT("ExportFailedText", "Export failed");
		break;
	}

	FText TerminalDetail;
	switch (TerminalOutcome)
	{
	case ETerminalOutcome::Success:
		TerminalDetail = FText::Format(
			LOCTEXT("TerminalSuccessDetail", "{0} exported"),
			FormatFileCount(ExportedFileCount));
		break;
	case ETerminalOutcome::Partial:
		TerminalDetail = FText::Format(
			LOCTEXT("TerminalPartialDetail", "{0} exported"),
			FormatFileCount(ExportedFileCount));
		if (FailedFileCount > 0)
		{
			TerminalDetail = FText::Format(
				LOCTEXT("TerminalAppendFailed", "{0}, {1} failed"),
				TerminalDetail,
				FText::AsNumber(FailedFileCount));
		}
		if (SkippedFileCount > 0)
		{
			TerminalDetail = FText::Format(
				LOCTEXT("TerminalAppendSkipped", "{0}, {1} skipped"),
				TerminalDetail,
				FText::AsNumber(SkippedFileCount));
		}
		if (ReportFailedCount > 0)
		{
			TerminalDetail = FText::Format(
				LOCTEXT("TerminalAppendReportFailure", "{0}, {1}"),
				TerminalDetail,
				FormatReportOutputFailureCount(ReportFailedCount));
		}
		break;
	case ETerminalOutcome::Cancelled:
		TerminalDetail = ExportedFileCount > 0
			? FText::Format(
				LOCTEXT("TerminalCancelledWithExports", "{0} exported before cancellation"),
				FormatFileCount(ExportedFileCount))
			: LOCTEXT("TerminalCancelledWithoutExports", "No files were exported");
		if (FailedFileCount > 0)
		{
			TerminalDetail = FText::Format(
				LOCTEXT("TerminalAppendCancelledFailed", "{0}, {1} failed"),
				TerminalDetail,
				FText::AsNumber(FailedFileCount));
		}
		if (SkippedFileCount > 0)
		{
			TerminalDetail = FText::Format(
				LOCTEXT("TerminalAppendCancelledSkipped", "{0}, {1} skipped"),
				TerminalDetail,
				FText::AsNumber(SkippedFileCount));
		}
		break;
	default:
		TerminalDetail = LOCTEXT("TerminalFailureDetail", "No files were exported");
		if (FailedFileCount > 0)
		{
			TerminalDetail = FText::Format(
				LOCTEXT("TerminalAppendFailureFailed", "{0}, {1} failed"),
				TerminalDetail,
				FText::AsNumber(FailedFileCount));
		}
		if (SkippedFileCount > 0)
		{
			TerminalDetail = FText::Format(
				LOCTEXT("TerminalAppendFailureSkipped", "{0}, {1} skipped"),
				TerminalDetail,
				FText::AsNumber(SkippedFileCount));
		}
		break;
	}

	const FText StructuredDetail = MergeTerminalDetails(TerminalErrorMessage, TerminalReportErrorMessage);
	if (!StructuredDetail.IsEmpty())
	{
		TerminalDetail = FText::Format(
			LOCTEXT("TerminalDetailWithStructuredError", "{0}  |  {1}"),
			TerminalDetail,
			StructuredDetail);
	}
	TerminalDetailForTesting = TerminalDetail.ToString();

	if (ProgressBody.IsValid())
	{
		CurrentStatusSubline = FText::Format(LOCTEXT("TerminalStatusFmt", "{0}  |  {1}"), TerminalHeadline, TerminalDetail);
		ProgressBody->SetSubline(FormatProgressSubline(CurrentStatusSubline, CurrentElapsedText));
	}
}

#if WITH_AUDIOFACTORY_DEV_CAPTURE
void SAudioExportProgressWindow::SetTerminalStateForCapture(
	const int32 InExportedFiles,
	const int32 InFailedFiles,
	const FString& InExportPath)
{
	// This legacy capture helper intentionally uses the count-based fallback. Any
	// structured outcome belongs to the explicit cancellation seam below and must
	// not leak across repeated state drives on one widget.
	bHasStructuredOutcome = false;
	StructuredOutcome = ETerminalOutcome::Pending;
	ExportedFileCount = FMath::Max(0, InExportedFiles);
	FailedFileCount = FMath::Max(0, InFailedFiles);
	SkippedFileCount = 0;
	ReportFailedCount = 0;
	CurrentProgress = 1.0f;
	if (ProgressBody.IsValid())
	{
		ProgressBody->SetProgressFraction(TOptional<float>(CurrentProgress));
	}
	ApplyTerminalPresentation(InExportPath);
}

void SAudioExportProgressWindow::SetCaptureStateForCapture(
	const EAudioExporterCaptureProgressState State,
	const FString& InExportPath)
{
	auto PushResult = [this](
		const int32 CurrentSound,
		const TCHAR* SoundName,
		const bool bSucceeded,
		const int32 ExportedFiles,
		const int32 FailedFiles,
		const TCHAR* Message)
	{
		FAudioExportProgressInfo Info;
		Info.CurrentSound = CurrentSound;
		Info.TotalSounds = FMath::Max(3, TotalAssetsCount);
		Info.CurrentVariation = 1;
		Info.TotalVariations = 1;
		Info.CurrentSoundName = SoundName;
		Info.bHasResult = true;
		Info.bSucceeded = bSucceeded;
		Info.ExportedFiles = ExportedFiles;
		Info.FailedFiles = FailedFiles;
		Info.ResultMessage = Message;
		UpdateProgressUI(Info);
	};

	if (State == EAudioExporterCaptureProgressState::Processing)
	{
		PushResult(1, TEXT("Preview_Impact"), true, 1, 0, TEXT("Saved Preview_Impact.wav"));
		FAudioExportProgressInfo Processing;
		Processing.CurrentSound = 2;
		Processing.TotalSounds = FMath::Max(3, TotalAssetsCount);
		Processing.CurrentVariation = 1;
		Processing.TotalVariations = 1;
		Processing.CurrentSoundName = TEXT("Preview_Loop");
		UpdateProgressUI(Processing);
		return;
	}

	if (State == EAudioExporterCaptureProgressState::Success)
	{
		PushResult(1, TEXT("Preview_Impact"), true, 1, 0, TEXT("Saved Preview_Impact.wav"));
		PushResult(2, TEXT("Preview_Loop"), true, 2, 0, TEXT("Saved Preview_Loop.wav"));
		PushResult(3, TEXT("Preview_Ambience"), true, 3, 0, TEXT("Saved Preview_Ambience.wav"));
		SetTerminalStateForCapture(3, 0, InExportPath);
		return;
	}

	if (State == EAudioExporterCaptureProgressState::PartialError)
	{
		PushResult(1, TEXT("Preview_Impact"), true, 1, 0, TEXT("Saved Preview_Impact.wav"));
		PushResult(2, TEXT("Preview_Loop"), false, 1, 1, TEXT("Source payload unavailable"));
		PushResult(3, TEXT("Preview_Ambience"), true, 2, 1, TEXT("Saved Preview_Ambience.wav"));
		SetTerminalStateForCapture(2, 1, InExportPath);
		return;
	}

	if (State == EAudioExporterCaptureProgressState::Failure)
	{
		// A failure capture must not show a green row left over from the success
		// fixture. Every selected asset is represented as failed, preserving the
		// three-asset conservation invariant in both the rows and terminal strip.
		PushResult(1, TEXT("Preview_Impact"), false, 0, 1, TEXT("Renderer failed to start"));
		PushResult(2, TEXT("Preview_Loop"), false, 0, 2, TEXT("Not attempted after startup failure"));
		PushResult(3, TEXT("Preview_Ambience"), false, 0, 3, TEXT("Not attempted after startup failure"));
		SetTerminalStateForCapture(0, 3, FString());
		return;
	}

	if (State == EAudioExporterCaptureProgressState::Cancelled)
	{
		// Cancellation preserves the one completed export and marks the remaining
		// two selections as neutral/pending rows. The structured outcome supplies
		// the skipped count that the legacy progress rows do not carry. Do not send
		// false progress results here because the progress payload has no skipped
		// flag and the UI would render those rows as failures.
		PushResult(1, TEXT("Preview_Impact"), true, 1, 0, TEXT("Saved Preview_Impact.wav"));
		bHasStructuredOutcome = true;
		StructuredOutcome = ETerminalOutcome::Cancelled;
		ExportedFileCount = 1;
		FailedFileCount = 0;
		SkippedFileCount = 2;
		ReportFailedCount = 0;
		TerminalErrorMessage.Reset();
		TerminalReportErrorMessage.Reset();
		CurrentProgress = 1.0f;
		if (ProgressBody.IsValid())
		{
			ProgressBody->SetProgressFraction(TOptional<float>(CurrentProgress));
		}
		ApplyTerminalPresentation(FString());
		return;
	}

	// Rejected is a non-green failure presentation. No asset may appear as a
	// successful export when the request never started.
	PushResult(1, TEXT("Preview_Impact"), false, 0, 1, TEXT("Export request rejected"));
	PushResult(2, TEXT("Preview_Loop"), false, 0, 2, TEXT("Export request rejected"));
	PushResult(3, TEXT("Preview_Ambience"), false, 0, 3, TEXT("Export request rejected"));
	SetTerminalStateForCapture(0, 3, FString());
}
#endif

#if WITH_DEV_AUTOMATION_TESTS
FReply SAudioExportProgressWindow::InvokeCancelForTesting()
{
	return OnCancelExportClicked();
}

bool SAudioExportProgressWindow::HasReachedTerminalStateForTesting() const
{
	return bTerminalStateReached;
}

bool SAudioExportProgressWindow::WasTerminalSuccessForTesting() const
{
	return TerminalOutcome == ETerminalOutcome::Success;
}

bool SAudioExportProgressWindow::WasTerminalPartialForTesting() const
{
	return TerminalOutcome == ETerminalOutcome::Partial;
}

FString SAudioExportProgressWindow::GetTerminalDetailForTesting() const
{
	return TerminalDetailForTesting;
}

int32 SAudioExportProgressWindow::GetTerminalExportedCountForTesting() const
{
	return ExportedFileCount;
}

int32 SAudioExportProgressWindow::GetTerminalFailedCountForTesting() const
{
	return FailedFileCount;
}

int32 SAudioExportProgressWindow::GetTerminalSkippedCountForTesting() const
{
	return SkippedFileCount;
}

AudioExporter::EAudioFactoryStatusSeverity SAudioExportProgressWindow::GetStatusSeverityForTesting() const
{
	return BuildStatusStripStatus().Severity;
}

FString SAudioExportProgressWindow::GetStatusTextForTesting() const
{
	return BuildStatusStripStatus().Message.ToString();
}

FString SAudioExportProgressWindow::GetTerminalNotificationTextForTesting() const
{
	return LastTerminalNotificationText.ToString();
}

bool SAudioExportProgressWindow::WasOpenFolderBoundForTesting() const
{
	return bLastNotificationHadOpenFolder;
}

SNotificationItem::ECompletionState SAudioExportProgressWindow::GetTerminalNotificationCompletionStateForTesting() const
{
	return LastTerminalNotificationCompletionState;
}

int32 SAudioExportProgressWindow::GetFileLogEntryCountForTesting() const
{
	return FileLogEntries.Num();
}

AudioExporter::EAudioFactoryStatusSeverity SAudioExportProgressWindow::GetFileLogSeverityForTesting(const int32 Index) const
{
	return FileLogEntries.IsValidIndex(Index) && FileLogEntries[Index].IsValid()
		? FileLogEntries[Index]->Severity
		: AudioExporter::EAudioFactoryStatusSeverity::Neutral;
}

FString SAudioExportProgressWindow::GetFileLogTextForTesting(const int32 Index) const
{
	return FileLogEntries.IsValidIndex(Index) && FileLogEntries[Index].IsValid()
		? FileLogEntries[Index]->Text.ToString()
		: FString();
}

void SAudioExportProgressWindow::SetElapsedSecondsForTesting(const double InElapsedSeconds)
{
	ExportStartTime = FPlatformTime::Seconds() - InElapsedSeconds;
}

void SAudioExportProgressWindow::RefreshElapsedTextForTesting()
{
	TickElapsedTime(FPlatformTime::Seconds(), 0.0f);
}
#endif

// ---------------------------------------------------------------------------
// OnMonitorToggleChanged
// ---------------------------------------------------------------------------
void SAudioExportProgressWindow::OnMonitorToggleChanged(bool bOn)
{
	if (!IsValid(ExporterSettings))
	{
		UE_LOG(LogAudioExporter, Warning, TEXT("%s: Monitor toggle was requested without valid settings."), *FString(__FUNCTION__));
		return;
	}

	ExporterSettings->bMonitor = bOn;
	ExporterSettings->TryUpdateDefaultConfigFile();

	if (OnMonitorChanged.IsBound())
	{
		OnMonitorChanged.Execute(bOn);
	}
	else
	{
		UE_LOG(LogAudioExporter, Warning, TEXT("%s: Monitor toggle was requested without a bound callback."), *FString(__FUNCTION__));
	}
}

void SAudioExportProgressWindow::OnSubmixEnvelope(const TArray<float>& Envelope)
{
	FScopeLock Lock(&MeterCriticalSection);
	MeterChannelInfo.Empty();
	for (float Value : Envelope)
	{
		FMeterChannelInfo Info;
		Info.MeterValue = Audio::ConvertToDecibels(Value);
		Info.PeakValue  = Info.MeterValue;
		MeterChannelInfo.Add(Info);
	}
}

TArray<FMeterChannelInfo> SAudioExportProgressWindow::GetMeterChannelInfo() const
{
	FScopeLock Lock(&MeterCriticalSection);
	return MeterChannelInfo;
}

void SAudioExportProgressWindow::TryAttachMeterSource()
{
	if (!MetricListener || RecordingSubmix.IsValid() || !OnRequestRecordingSubmix.IsBound() || !OnRequestWorld.IsBound())
	{
		return;
	}

	RecordingSubmix = OnRequestRecordingSubmix.Execute();
	RecordingWorld = OnRequestWorld.Execute();
	if (RecordingSubmix.IsValid() && RecordingWorld.IsValid())
	{
		RecordingSubmix->StartEnvelopeFollowing(RecordingWorld.Get());
		SubmixEnvelopeDelegate.BindUFunction(MetricListener, FName("OnSubmixEnvelope"));
		RecordingSubmix->AddEnvelopeFollowerDelegate(RecordingWorld.Get(), SubmixEnvelopeDelegate);
	}
}

// ---------------------------------------------------------------------------
// Cancel (single-step -- no confirmation dialogs)
// ---------------------------------------------------------------------------
FReply SAudioExportProgressWindow::OnCancelExportClicked()
{
	if (!OnCancelRequested.IsBound())
	{
		UE_LOG(LogAudioExporter, Warning, TEXT("%s: Cancel was requested without a bound callback."), *FString(__FUNCTION__));
		return FReply::Handled();
	}

	OnCancelRequested.Execute();
	return FReply::Handled();
}

void SAudioExportProgressWindow::HandleRequestDestroyWindow(const TSharedRef<SWindow>& InWindow)
{
	if (bTerminalStateReached)
	{
		InWindow->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
		FSlateApplication::Get().RequestDestroyWindow(InWindow);
		return;
	}

	if (OnIsExportActive.IsBound() && !OnIsExportActive.Execute())
	{
		InWindow->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
		FSlateApplication::Get().RequestDestroyWindow(InWindow);
		return;
	}

	OnCancelExportClicked();
}

// ---------------------------------------------------------------------------
// Esc handling (direct override necessary -- see the header comment)
// ---------------------------------------------------------------------------
bool SAudioExportProgressWindow::SupportsKeyboardFocus() const
{
	return true;
}

FReply SAudioExportProgressWindow::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		RequestWindowCloseOrCancel();
		return FReply::Handled();
	}

	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

void SAudioExportProgressWindow::RequestWindowCloseOrCancel()
{
	if (const TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(SharedThis(this)))
	{
		HandleRequestDestroyWindow(Window.ToSharedRef());
	}
}

// ---------------------------------------------------------------------------
// Status summary computation. Nothing renders this since the v2.0 design change
// removed the bottom status strip from all product windows; it stays because
// UI automation asserts its severity/text via GetStatusSeverityForTesting /
// GetStatusTextForTesting (AudioExporterUITests.cpp).
// ---------------------------------------------------------------------------
AudioExporter::FAudioFactoryStatus SAudioExportProgressWindow::BuildStatusStripStatus() const
{
	using namespace AudioExporter;

	if (bTerminalStateReached)
	{
		switch (TerminalOutcome)
		{
		case ETerminalOutcome::Success:
			return FAudioFactoryStatus(
				EAudioFactoryStatusSeverity::Ready,
				FText::Format(
					LOCTEXT("StatusTerminalSuccess", "Export complete. {0} exported."),
					FormatFileCount(ExportedFileCount)));
		case ETerminalOutcome::Partial:
		{
			FText StatusText = FText::Format(
				LOCTEXT("StatusTerminalPartial", "Export finished with failures. {0} exported, {1} failed."),
				FText::AsNumber(ExportedFileCount),
				FText::AsNumber(FailedFileCount));
			if (SkippedFileCount > 0)
			{
				StatusText = FText::Format(
					LOCTEXT("StatusTerminalPartialSkipped", "{0} {1} skipped."),
					StatusText,
					FText::AsNumber(SkippedFileCount));
			}
			return FAudioFactoryStatus(
				EAudioFactoryStatusSeverity::Warning,
				StatusText);
		}
		case ETerminalOutcome::Cancelled:
			return FAudioFactoryStatus(
				EAudioFactoryStatusSeverity::Warning,
				FText::Format(
					LOCTEXT("StatusTerminalCancelled", "Export cancelled. {0} exported, {1} failed, {2} skipped."),
					FormatFileCount(ExportedFileCount),
					FText::AsNumber(FailedFileCount),
					FText::AsNumber(SkippedFileCount)));
		default:
		{
			if (FailedFileCount > 0 || SkippedFileCount > 0)
			{
				FText StatusText = LOCTEXT("StatusTerminalFailureWithCounts", "Export failed. No files were exported.");
				if (FailedFileCount > 0)
				{
					StatusText = FText::Format(
						LOCTEXT("StatusTerminalFailureFailed", "{0} {1} failed."),
						StatusText,
						FText::AsNumber(FailedFileCount));
				}
				if (SkippedFileCount > 0)
				{
					StatusText = FText::Format(
						LOCTEXT("StatusTerminalFailureSkipped", "{0} {1} skipped."),
						StatusText,
						FText::AsNumber(SkippedFileCount));
				}
				return FAudioFactoryStatus(EAudioFactoryStatusSeverity::Error, StatusText);
			}
			return FAudioFactoryStatus(
				EAudioFactoryStatusSeverity::Error,
				LOCTEXT("StatusTerminalFailure", "Export failed. No files were exported."));
		}
		}
	}

	if (FailedFileCount > 0)
	{
		return FAudioFactoryStatus(
			EAudioFactoryStatusSeverity::Warning,
			FText::Format(
				LOCTEXT("StatusRunningWithFailures", "Exporting {0} selected assets  |  {1} failed so far."),
				FText::AsNumber(TotalAssetsCount),
				FText::AsNumber(FailedFileCount)));
	}

	return FAudioFactoryStatus(
		EAudioFactoryStatusSeverity::Neutral,
		FText::Format(
			LOCTEXT("ProgressFrameStatus", "Exporting {0} selected assets."),
			FText::AsNumber(TotalAssetsCount)));
}

// ---------------------------------------------------------------------------
// Elapsed time ticker
// ---------------------------------------------------------------------------
EActiveTimerReturnType SAudioExportProgressWindow::TickElapsedTime(double /*CurrentTime*/, float /*DeltaTime*/)
{
	if (!ProgressBody.IsValid())
	{
		return EActiveTimerReturnType::Stop;
	}

	const double ElapsedSeconds = FPlatformTime::Seconds() - ExportStartTime;
	const int32 ElapsedWholeSeconds = FMath::FloorToInt(ElapsedSeconds);
	const FString ElapsedString = FString::Printf(TEXT("%02d:%02d"), ElapsedWholeSeconds / 60, ElapsedWholeSeconds % 60);

	// Elapsed only. A remaining-time estimate extrapolated from completed-work fraction is wrong
	// whenever the queue is not uniform: one long asset left at the end makes the estimate climb
	// while the export runs, which reads as a stall rather than progress.
	CurrentElapsedText = FText::Format(LOCTEXT("TimeElapsedFmt", "Elapsed: {0}"),
		FText::FromString(ElapsedString));

	ProgressBody->SetSubline(FormatProgressSubline(CurrentStatusSubline, CurrentElapsedText));
	return EActiveTimerReturnType::Continue;
}

// ---------------------------------------------------------------------------
// OpenProgressWindow
// ---------------------------------------------------------------------------
TSharedRef<SWindow> SAudioExportProgressWindow::OpenProgressWindow(const TArray<FAssetData>& InAssets,
	                                                              const FOnAudioExporterMonitorChanged& InOnMonitorChanged,
	                                                              const FOnAudioExporterCancelRequested& InOnCancelRequested,
	                                                              const FOnAudioExporterRequestRecordingSubmix& InOnRequestRecordingSubmix,
	                                                              const FOnAudioExporterRequestWorld& InOnRequestWorld,
	                                                              const FOnAudioExporterIsExportActive& InOnIsExportActive)
{
	TSharedPtr<SAudioExportProgressWindow> Widget;

	// Shared dialog factory instead of a hand-rolled SNew(SWindow) chain -- OS title
	// follows "<Product> — <Task>" grammar and the window gets a real MinSize/placement contract.
	AudioExporter::FAudioFactoryDialogWindowArgs DialogArgs;
	DialogArgs.ProductId = AudioExporter::EAudioFactoryProductId::AudioExporter;
	DialogArgs.TaskTitle = LOCTEXT("ProgressWindowTask", "Exporting");
	DialogArgs.ClientSize = FVector2D(520.0f, 460.0f);
	DialogArgs.MinSize = FVector2D(460.0f, 360.0f);
	TSharedRef<SWindow> Window = AudioExporter::FAudioFactoryDialogWindow::Create(DialogArgs);

	Window->SetContent(
		SAssignNew(Widget, SAudioExportProgressWindow)
		.Assets(InAssets)
		.OnMonitorChanged(InOnMonitorChanged)
		.OnCancelRequested(InOnCancelRequested)
		.OnRequestRecordingSubmix(InOnRequestRecordingSubmix)
		.OnRequestWorld(InOnRequestWorld)
		.OnIsExportActive(InOnIsExportActive)
	);
	// This widget's own OnKeyDown override only fires if something gives it keyboard focus on
	// activate.
	Window->SetWidgetToFocusOnActivate(Widget);

	Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride::CreateSP(
		Widget.ToSharedRef(), &SAudioExportProgressWindow::HandleRequestDestroyWindow));

	return Window;
}

#undef LOCTEXT_NAMESPACE
