// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateTypes.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "AssetRegistry/AssetData.h"
#include "AudioMeterTypes.h"
#include "AudioMeterStyle.h"
#include "Sound/SoundSubmixSend.h"
#include "HAL/CriticalSection.h"

// Capture-only seams must never inherit the editor target flag. The capture workflow opts in by
// defining this macro for its dedicated build; customer/editor builds remain off by default.
#ifndef WITH_AUDIOFACTORY_DEV_CAPTURE
#define WITH_AUDIOFACTORY_DEV_CAPTURE 0
#endif

class SProgressBar;
class UAudioExporterSettings;
class SAudioMeter;
class USoundSubmix;
class UWorld;
class UAudioMetricListener;
struct FAudioExportProgressInfo;
struct FAudioExportResult;

namespace AudioExporter
{
	class SAudioFactoryProgressBody;
	struct FAudioFactoryTaskLogEntry;
	enum class EAudioFactoryStatusSeverity : uint8;
	struct FAudioFactoryStatus;
}

DECLARE_DELEGATE_OneParam(FOnAudioExporterMonitorChanged, bool);
DECLARE_DELEGATE(FOnAudioExporterCancelRequested);
DECLARE_DELEGATE_RetVal(USoundSubmix*, FOnAudioExporterRequestRecordingSubmix);
DECLARE_DELEGATE_RetVal(UWorld*, FOnAudioExporterRequestWorld);
DECLARE_DELEGATE_RetVal(bool, FOnAudioExporterIsExportActive);

enum class EAudioExporterCaptureProgressState : uint8
{
	Processing,
	Success,
	PartialError,
	Failure,
	Cancelled,
	Rejected,
};

/**
 * SAudioExportProgressWindow
 *
 * Displays real-time export progress with:
 *  - Overall progress bar and status labels
 *  - Time elapsed
 *  - Per-file status list (pending, in-progress, done)
 *  - Audio metering for the live monitoring submix
 *  - Single-step cancel (no confirmation dialogs)
 */
class AUDIOEXPORTERUI_API SAudioExportProgressWindow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioExportProgressWindow)
	{}
	/** The initial list of assets that the batch is processing. */
	SLATE_ARGUMENT(TArray<FAssetData>, Assets)
	SLATE_EVENT(FOnAudioExporterMonitorChanged, OnMonitorChanged)
	SLATE_EVENT(FOnAudioExporterCancelRequested, OnCancelRequested)
	SLATE_EVENT(FOnAudioExporterRequestRecordingSubmix, OnRequestRecordingSubmix)
	SLATE_EVENT(FOnAudioExporterRequestWorld, OnRequestWorld)
	SLATE_EVENT(FOnAudioExporterIsExportActive, OnIsExportActive)
	SLATE_END_ARGS();

	void Construct(const FArguments& InArgs);
	virtual ~SAudioExportProgressWindow();

	// The nested SAudioFactoryPluginWindowFrame's own
	// Escape handling is not reliably reachable unless the top-level widget set as the window's
	// content also claims keyboard focus itself. Overriding here directly
	// -- rather than trusting the nested frame alone -- is the working fix.
	virtual bool SupportsKeyboardFocus() const override;
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

	/**
	 * Utility to spawn a standalone window containing this progress widget.
	 * @param InAssets - Assets being processed.
	 * @return A shared reference to the managing window.
	 */
	static TSharedRef<SWindow> OpenProgressWindow(const TArray<FAssetData>& InAssets,
	                                            const FOnAudioExporterMonitorChanged& InOnMonitorChanged,
	                                            const FOnAudioExporterCancelRequested& InOnCancelRequested,
	                                            const FOnAudioExporterRequestRecordingSubmix& InOnRequestRecordingSubmix,
	                                            const FOnAudioExporterRequestWorld& InOnRequestWorld,
	                                            const FOnAudioExporterIsExportActive& InOnIsExportActive);

	/** Updates the progress bar and labels based on the provided info. */
	void UpdateProgressUI(const FAudioExportProgressInfo& ExportingProgress);

	/** Handles the finalization of the UI once the batch is done. */
	void OnExportFinishedProgress(const FString& InExportPath);

	/**
	 * Consumes the structured terminal result: caches the real EAudioExportOutcome so
	 * ApplyTerminalPresentation uses it instead of inferring success/partial/failure from exported/
	 * failed counts and an empty-path heuristic. Bound directly to the runtime subsystem in
	 * Construct() -- this only wires up the delegate; presentation of the cached outcome happens
	 * separately in ApplyTerminalPresentation.
	 */
	void OnExportFinishedResult(const FAudioExportResult& Result);

#if WITH_AUDIOFACTORY_DEV_CAPTURE
	/** Applies the real terminal presentation without notifications or closing, for capture automation. */
	void SetTerminalStateForCapture(int32 InExportedFiles, int32 InFailedFiles, const FString& InExportPath);
	/** Drives the existing progress/result widgets through a deterministic visual capture state. */
	void SetCaptureStateForCapture(EAudioExporterCaptureProgressState State, const FString& InExportPath);
#endif

#if WITH_DEV_AUTOMATION_TESTS
	FReply InvokeCancelForTesting();
	bool HasReachedTerminalStateForTesting() const;

	/** True only when the terminal outcome was a green success (>0 exported and 0 failed). */
	bool WasTerminalSuccessForTesting() const;

	/** True when the terminal outcome was a partial completion (>0 exported and >0 failed). */
	bool WasTerminalPartialForTesting() const;

	/** The terminal body copy produced from the structured result. */
	FString GetTerminalDetailForTesting() const;

	/** Files the terminal state reported as successfully exported. */
	int32 GetTerminalExportedCountForTesting() const;

	/** Files the terminal state reported as failed. */
	int32 GetTerminalFailedCountForTesting() const;

	/** Files the terminal state reported as skipped. */
	int32 GetTerminalSkippedCountForTesting() const;

	/** Test seam: the real status-strip severity BuildStatusStripStatus produces right
	 * now, so a test can prove it moves off Neutral instead of asserting a fixed value. */
	AudioExporter::EAudioFactoryStatusSeverity GetStatusSeverityForTesting() const;

	/** Test seam: the exact status-strip copy produced from the terminal outcome and counts. */
	FString GetStatusTextForTesting() const;

	/** Test seam: captures the actual terminal toast payload and folder binding. */
	FString GetTerminalNotificationTextForTesting() const;
	bool WasOpenFolderBoundForTesting() const;

	/** Test seam: the real completion state (CS_Success/CS_Fail) OnExportFinishedProgress most
	 *  recently passed to SetCompletionState. */
	SNotificationItem::ECompletionState GetTerminalNotificationCompletionStateForTesting() const;

	/** Test seam: inspect the live per-asset rows after real progress updates. */
	int32 GetFileLogEntryCountForTesting() const;
	AudioExporter::EAudioFactoryStatusSeverity GetFileLogSeverityForTesting(int32 Index) const;
	FString GetFileLogTextForTesting(int32 Index) const;

	/** Test seam: set a deterministic elapsed duration without waiting for the active timer. */
	void SetElapsedSecondsForTesting(double InElapsedSeconds);
	/** Test seam: refresh elapsed text through the same path used by the active timer. */
	void RefreshElapsedTextForTesting();
#endif

private:
	// ---- Per-file status tracking (adopts the shared native task-log view instead
	// of hand-rolled TextBlock rows) --------------------------------------------

	/**
	 * One log row per exported asset, in the same order as the originating InArgs._Assets, fed
	 * directly to the shared SAudioFactoryTaskLogView via ProgressBody. Rows are mutated in place
	 * (Severity/Text) as each asset's real per-variation outcome arrives and refreshed through
	 * ProgressBody->SetLogItems + RequestScrollToBottom -- the array itself is never replaced.
	 */
	TArray<TSharedPtr<AudioExporter::FAudioFactoryTaskLogEntry>> FileLogEntries;

	// ---- Time tracking -------------------------------------------------------

	/** Absolute time (seconds) when the export started. */
	double ExportStartTime = 0.0;

	/** Current status/detail text retained so the elapsed ticker can refresh the same shared subline. */
	FText CurrentStatusSubline;

	/** Current elapsed text rendered beneath the active status in the shared progress body. */
	FText CurrentElapsedText;

	/** Active timer handle for updating the time display. */
	TWeakPtr<FActiveTimerHandle> TimeTimerHandle;

	/** Updates the elapsed text in the shared progress-body subline each second. */
	EActiveTimerReturnType TickElapsedTime(double CurrentTime, float DeltaTime);

	// ---- Progress body ------------------------

	/**
	 * Shared headline + 4px engine progress bar + native task-log widget, consolidating what would
	 * otherwise be a hand-rolled ExportingText/AssetDetailsText/SProgressBar trio. Headline is fixed
	 * at Construct; Subline/ProgressFraction/LogItems are updated live.
	 */
	TSharedPtr<AudioExporter::SAudioFactoryProgressBody> ProgressBody;

	/** Current overall progress fraction (0-1). */
	float CurrentProgress = 0.f;

	int32 TotalAssetsCount = 0;
	int32 ExportedFileCount = 0;
	int32 FailedFileCount = 0;
	int32 SkippedFileCount = 0;
	int32 ReportFailedCount = 0;

	/** Set once the pipeline has delivered success/cancel so window close cannot re-enter cancel. */
	bool bTerminalStateReached = false;

	/**
	 * Terminal classification of the finished export.
	 *  - Success: at least one file exported and none failed.
	 *  - Partial: at least one file exported but some failed.
	 *  - Cancelled: the session stopped before normal completion.
	 *  - Failure: zero files exported (including startup rejection, which forces an empty path).
	 * A run that produced zero successful files is never reported as a green success.
	 */
	enum class ETerminalOutcome : uint8 { Pending, Success, Partial, Cancelled, Failure };
	ETerminalOutcome TerminalOutcome = ETerminalOutcome::Pending;

	/**
	 * True once OnExportFinishedResult has cached a real outcome for the session about to reach
	 * ApplyTerminalPresentation. The structured result broadcasts before the legacy
	 * path string (see AudioExportPipeline.cpp::BroadcastTerminalOnce), so this is always set by the
	 * time OnExportFinishedProgress runs for a session driven through the real subsystem.
	 */
	bool bHasStructuredOutcome = false;

	/** The real outcome cached from OnExportFinishedResult, mapped to this window's own enum. */
	ETerminalOutcome StructuredOutcome = ETerminalOutcome::Pending;

	/** Exact structured terminal details retained for the terminal body and notification. */
	FString TerminalErrorMessage;
	FString TerminalReportErrorMessage;
	FString TerminalDetailForTesting;

	// ---- Audio metering ------------------------------------------------------

	TSharedPtr<SAudioMeter> AudioMeter;
	FAudioMeterStyle MeterStyle;
	TArray<FMeterChannelInfo> MeterChannelInfo;
	mutable FCriticalSection MeterCriticalSection;
	TObjectPtr<UAudioMetricListener> MetricListener;
	FOnSubmixEnvelopeBP SubmixEnvelopeDelegate;
	UAudioExporterSettings* ExporterSettings = nullptr;
	TWeakObjectPtr<USoundSubmix> RecordingSubmix;
	TWeakObjectPtr<UWorld> RecordingWorld;
	FOnAudioExporterMonitorChanged OnMonitorChanged;
	FOnAudioExporterCancelRequested OnCancelRequested;
	FOnAudioExporterRequestRecordingSubmix OnRequestRecordingSubmix;
	FOnAudioExporterRequestWorld OnRequestWorld;
	FOnAudioExporterIsExportActive OnIsExportActive;

	void OnSubmixEnvelope(const TArray<float>& Envelope);
	TArray<FMeterChannelInfo> GetMeterChannelInfo() const;
	void TryAttachMeterSource();

	// ---- Actions -------------------------------------------------------------

	void OnMonitorToggleChanged(bool bOn);

	/** Cancels the export immediately without confirmation dialogs. */
	FReply OnCancelExportClicked();

	/** Intercepts the OS close button to trigger cancellation. */
	void HandleRequestDestroyWindow(const TSharedRef<SWindow>& InWindow);

	/**
	 * Esc handler bound to the frame's OnCancelRequested (every secondary
	 * window closes/cancels on Escape). Resolves the owning SWindow and routes through the exact
	 * same terminal-aware logic as the OS close button (HandleRequestDestroyWindow), so Escape
	 * cancels an active export instead of abruptly destroying the window mid-session.
	 */
	void RequestWindowCloseOrCancel();

	/** Updates terminal labels/classification without performing notification or window lifecycle actions. */
	void ApplyTerminalPresentation(const FString& InExportPath);

	// ---- Status strip -------------------------------------------

	/**
	 * Builds the frame's status-strip content from live run state instead of a fixed Neutral
	 * value: Neutral while running clean, Warning once a failure has been recorded (running or
	 * partial-terminal), Error on a zero-success terminal outcome, Ready on a clean terminal
	 * success.
	 */
	AudioExporter::FAudioFactoryStatus BuildStatusStripStatus() const;

#if WITH_DEV_AUTOMATION_TESTS
	FText LastTerminalNotificationText;
	bool bLastNotificationHadOpenFolder = false;
	SNotificationItem::ECompletionState LastTerminalNotificationCompletionState = SNotificationItem::CS_None;
#endif
};
