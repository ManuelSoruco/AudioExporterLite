// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "Dialogs/AudioExporterWindow.h"

#include "AudioExporterBPLibrary.h"
#include "AudioExporterRuntimeSubsystem.h"
#include "MetasoundSource.h"
#include "Controls/SAudioFactorySelectedAssetsWindow.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "InputCoreTypes.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundWave.h"
#include "Utilities/AudioExporterLogger.h"
#include "Utilities/AudioExporterSettings.h"
#include "Utilities/AudioExporterUtils.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Dialogs/FAudioFactoryDialogWindow.h"

#if WITH_EDITOR
#include "Dialogs/AudioFactoryPluginWindowAdapter.h"
#endif

using namespace AudioExporter;

#define LOCTEXT_NAMESPACE "SAudioExporterWindow"

namespace AudioExporterWindowPrivate
{
	const FVector2D DefaultSettingsWindowSize(640.0f, 426.0f);
	const FVector2D MinimumSettingsWindowSize(540.0f, 340.0f);
	const FVector2D SelectedAssetsWindowSize(760.0f, 540.0f);

	struct FSelectedAudioExporterAssets
	{
		TArray<FAssetData> SupportedAssets;
		int32 NumMetaSounds = 0;
		int32 NumSoundCues = 0;
		// Non-procedural SoundWaves pass the shared
		// IsAudioExporterSupportedAsset predicate (AudioExporterUtils.cpp). Counted separately so
		// the header row and the asset-type support flags below can reflect them honestly instead
		// of silently folding them into "MetaSounds".
		int32 NumSoundWaves = 0;
	};

	static FSelectedAudioExporterAssets FilterSupportedAssets(const TArray<FAssetData>& InAssets)
	{
		FSelectedAudioExporterAssets Result;
		Result.SupportedAssets.Reserve(InAssets.Num());

		for (const FAssetData& Asset : InAssets)
		{
			if (!IsAudioExporterSupportedAsset(Asset))
			{
				continue;
			}

			Result.SupportedAssets.Add(Asset);

			const UClass* AssetClass = Asset.GetClass();
			if (AssetClass && AssetClass->IsChildOf(UMetaSoundSource::StaticClass()))
			{
				++Result.NumMetaSounds;
			}
			else if (AssetClass && AssetClass->IsChildOf(USoundCue::StaticClass()))
			{
				++Result.NumSoundCues;
			}
			else if (AssetClass && AssetClass->IsChildOf(USoundWave::StaticClass()))
			{
				// The shared predicate already excludes procedural waves, so anything reaching
				// here is a static source guaranteed to produce exactly one file.
				++Result.NumSoundWaves;
			}
		}

		return Result;
	}

	static FText MakeAssetCountMessage(const FSelectedAudioExporterAssets& SelectedAssets)
	{
		if (SelectedAssets.SupportedAssets.IsEmpty())
		{
			return LOCTEXT("NoAssetsError", "No supported audio assets selected.");
		}

		return LOCTEXT("ReadyToExport", "Ready to export selected audio.");
	}

	static FText MakeExportPathText(const UAudioExporterSettings* Settings)
	{
		if (IsValid(Settings) && !Settings->ExportDirectory.IsEmpty())
		{
			FString BaseDirectory = Settings->ExportDirectory;
			FPaths::NormalizeDirectoryName(BaseDirectory);
			if (BaseDirectory.EndsWith(TEXT("/<timestamp>"), ESearchCase::IgnoreCase))
			{
				return FText::FromString(BaseDirectory);
			}
			return FText::FromString(BaseDirectory / TEXT("<timestamp>"));
		}

		return FText::FromString(FPaths::ConvertRelativePathToFull(
			FPaths::ProjectSavedDir() / TEXT("AudioExporter") / TEXT("<timestamp>")));
	}

	/**
	 * Resolves the output folder the path row is currently showing, without the "<timestamp>"
	 * session placeholder the path strip appends for display. The folder picker starts here so
	 * the dialog opens where the row points (staged ExportDirectory, else the default
	 * Saved/AudioExporter base) instead of at the literal placeholder, which only exists once an
	 * export session actually runs.
	 */
	static FString ResolveExportStartingDirectory(const UAudioExporterSettings* Settings)
	{
		if (IsValid(Settings) && !Settings->ExportDirectory.IsEmpty())
		{
			FString StartingDirectory = Settings->ExportDirectory;
			FPaths::NormalizeDirectoryName(StartingDirectory);
			StartingDirectory.RemoveFromEnd(TEXT("/<timestamp>"), ESearchCase::IgnoreCase);
			return StartingDirectory;
		}

		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AudioExporter"));
	}

	static FText ResolveAssetClassLabel(const FAssetData& AssetData)
	{
		const UClass* AssetClass = AssetData.GetClass();
		if (AssetClass && AssetClass->IsChildOf(UMetaSoundSource::StaticClass()))
		{
			return LOCTEXT("SupportedMetaSoundClass", "MetaSound");
		}
		if (AssetClass && AssetClass->IsChildOf(USoundCue::StaticClass()))
		{
			return LOCTEXT("SupportedSoundCueClass", "SoundCue");
		}
		if (AssetClass && AssetClass->IsChildOf(USoundWave::StaticClass()))
		{
			return LOCTEXT("SupportedSoundWaveClass", "SoundWave");
		}

		return FText::FromName(AssetData.AssetClassPath.GetAssetName());
	}

	static FName ResolveAssetIconName(const FAssetData& AssetData)
	{
		const UClass* AssetClass = AssetData.GetClass();
		if (AssetClass && AssetClass->IsChildOf(UMetaSoundSource::StaticClass()))
		{
			return FAudioFactoryClassIconNames::MetaSoundSource();
		}
		if (AssetClass && AssetClass->IsChildOf(USoundCue::StaticClass()))
		{
			return FAudioFactoryClassIconNames::SoundCue();
		}
		if (AssetClass && AssetClass->IsChildOf(USoundWave::StaticClass()))
		{
			return FAudioFactoryClassIconNames::SoundWave();
		}

		return NAME_None;
	}

	/**
	 * A selected SoundWave is a static source -- the pipeline produces exactly one
	 * file for it (never the configured One-shot/Loop variation counts). Surfacing that here, on
	 * the same Reason field Normalizer already uses for per-row notes, keeps the wording aligned
	 * with the report's own "1 of 1 (static source)" line (AudioExporterRuntime) instead of
	 * inventing a second phrasing for the same fact.
	 */
	static FText ResolveAssetReason(const FAssetData& AssetData)
	{
		const UClass* AssetClass = AssetData.GetClass();
		if (AssetClass && AssetClass->IsChildOf(USoundWave::StaticClass()))
		{
			return LOCTEXT("SoundWaveStaticSourceReason", "1 of 1 (static source)");
		}

		return FText::GetEmpty();
	}

	static TArray<FAudioFactorySelectedAsset> BuildSelectedAssetRows(const TArray<FAssetData>& SupportedAssets)
	{
		TArray<FAudioFactorySelectedAsset> Assets;
		Assets.Reserve(SupportedAssets.Num());
		for (const FAssetData& AssetData : SupportedAssets)
		{
			const FString AssetPath = AssetData.PackageName.ToString();

			Assets.Add(FAudioFactorySelectedAsset::Make(
				FText::FromName(AssetData.AssetName),
				FText::FromString(AssetPath),
				ResolveAssetClassLabel(AssetData),
				EAudioFactoryAssetInclusionState::Included,
				ResolveAssetReason(AssetData),
				ResolveAssetIconName(AssetData)));
		}

		return Assets;
	}

	/**
	 * Minimal Escape-to-close wrapper: SAudioFactorySelectedAssetsWindow (shared
	 * alias) has no built-in key handling of its own, unlike SAudioFactoryPluginWindowFrame. This
	 * mirrors that frame's own SupportsKeyboardFocus/OnKeyDown pattern locally so the read-only
	 * Selected Assets window still closes on Escape without hand-editing the generated alias.
	 */
	class SEscapeToCloseWrapper : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SEscapeToCloseWrapper) {}
			SLATE_DEFAULT_SLOT(FArguments, Content)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, TWeakPtr<SWindow> InOwnerWindow)
		{
			OwnerWindow = MoveTemp(InOwnerWindow);
			ChildSlot
			[
				InArgs._Content.Widget
			];
		}

		virtual bool SupportsKeyboardFocus() const override
		{
			return true;
		}

		virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override
		{
			if (InKeyEvent.GetKey() == EKeys::Escape)
			{
				if (const TSharedPtr<SWindow> Window = OwnerWindow.Pin())
				{
					Window->RequestDestroyWindow();
				}
				return FReply::Handled();
			}

			return FReply::Unhandled();
		}

	private:
		TWeakPtr<SWindow> OwnerWindow;
	};

	static TSharedRef<SWindow> OpenSelectedAssetsWindow(const TArray<FAudioFactorySelectedAsset>& Assets, const TSharedPtr<SWindow>& ParentWindow)
	{
		FAudioFactoryDialogWindowArgs DialogArgs;
		DialogArgs.ProductId = EAudioFactoryProductId::AudioExporter;
		DialogArgs.TaskTitle = LOCTEXT("SelectedAssetsWindowTask", "Selected Assets");
		DialogArgs.ClientSize = SelectedAssetsWindowSize;
		DialogArgs.MinSize = FVector2D(540.0f, 380.0f);

		TSharedRef<SWindow> SelectedWindow = FAudioFactoryDialogWindow::Create(DialogArgs);
		const TWeakPtr<SWindow> WeakSelectedWindow = SelectedWindow;

		TSharedPtr<SEscapeToCloseWrapper> EscWrapper;
		SelectedWindow->SetContent(
			SAssignNew(EscWrapper, SEscapeToCloseWrapper, WeakSelectedWindow)
			[
				SNew(SAudioFactorySelectedAssetsWindow)
				.Title(LOCTEXT("SelectedAssetsWindowTitle", "Audio Exporter Selected Assets"))
				.Assets(Assets)
			]);
		// The wrapper's own OnKeyDown only fires if
		// something actually gives it keyboard focus on activate.
		SelectedWindow->SetWidgetToFocusOnActivate(EscWrapper);

		// Native-parent behavior: parented so the window can never fall behind the
		// editor, the idiom the shared factory documents.
		FAudioFactoryDialogWindow::Show(SelectedWindow, ParentWindow, /*bModal*/ false);
		return SelectedWindow;
	}

	static void OpenExportLogFolder()
	{
		// Open the effective export base directory so the Logs button lands where the session
		// folders (and their logs) actually live -- the custom ExportDirectory when the user set
		// one, or the default Saved/AudioExporter location otherwise.
		FString ReportDirectory;
		if (const UAudioExporterSettings* Settings = GetDefault<UAudioExporterSettings>())
		{
			ReportDirectory = Settings->ResolveEffectiveExportBaseDirectory();
		}
		else
		{
			ReportDirectory = FPaths::ProjectSavedDir() / TEXT("AudioExporter");
		}

		IFileManager::Get().MakeDirectory(*ReportDirectory, true);
		FPlatformProcess::ExploreFolder(*FPaths::ConvertRelativePathToFull(ReportDirectory));
	}

#if WITH_EDITOR
	static FAudioFactoryPluginWindowValidationResult ValidateSettingsObject(
		const UObject* SettingsObject,
		const FSelectedAudioExporterAssets& SelectedAssets)
	{
		if (SelectedAssets.SupportedAssets.IsEmpty())
		{
			// Empty selection is recoverable (the user can add assets and try again), so this is a
			// Warning rather than a hard Error -- matches the sibling products'
			// NativeNoSelectedAssets handling. bCanRunPrimaryAction stays false so Export remains
			// disabled until something is selected.
			return FAudioFactoryPluginWindowValidationResult::Make(
				EAudioFactoryPluginWindowValidationState::Warning,
				LOCTEXT("NoAssetsValidationError", "No supported MetaSound, SoundCue, or SoundWave assets selected."),
				true,
				false);
		}

		const UAudioExporterSettings* ExporterSettings = Cast<UAudioExporterSettings>(SettingsObject);
		if (!IsValid(ExporterSettings))
		{
			return FAudioFactoryPluginWindowValidationResult::Error(
				LOCTEXT("InvalidSettingsObject", "Audio Exporter settings are unavailable."));
		}

		if (!FMath::IsWithinInclusive(ExporterSettings->OneShotVariations, 1, 5))
		{
			return FAudioFactoryPluginWindowValidationResult::Error(
				LOCTEXT("InvalidOneShotVariations", "One-shot variations must be between 1 and 5."));
		}
		if (!FMath::IsWithinInclusive(ExporterSettings->LoopVariations, 1, 5))
		{
			return FAudioFactoryPluginWindowValidationResult::Error(
				LOCTEXT("InvalidLoopVariations", "Loop variations must be between 1 and 5."));
		}
		if (!FMath::IsWithinInclusive(ExporterSettings->MaxLength, 5, 60))
		{
			return FAudioFactoryPluginWindowValidationResult::Error(
				LOCTEXT("InvalidMaxLength", "Loop max length must be between 5 and 60 seconds."));
		}

		return FAudioFactoryPluginWindowValidationResult::Valid(MakeAssetCountMessage(SelectedAssets));
	}

	static bool ResetStagedSettings(UObject* StagedObject, FText& OutError)
	{
		UAudioExporterSettings* ExporterSettings = Cast<UAudioExporterSettings>(StagedObject);
		if (!IsValid(ExporterSettings))
		{
			OutError = LOCTEXT("ResetInvalidSettingsObject", "Audio Exporter staged settings are unavailable.");
			return false;
		}

		ExporterSettings->ApplyFromConfig(FAudioExporterConfig());
		ExporterSettings->ExportDirectory.Empty();
		return true;
	}

	static TOptional<FAudioFactoryPathContext> BuildExportPathContext(
		UObject* StagedObject,
		const FOnAudioExporterRequestExportDirectory& InOnSelectExportDirectory)
	{
		TWeakObjectPtr<UAudioExporterSettings> WeakSettings(Cast<UAudioExporterSettings>(StagedObject));

		FAudioFactoryPathContext PathContext;
		PathContext.Label = LOCTEXT("OutputPathContextLabel", "OUTPUT");
		PathContext.Path = TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateLambda([WeakSettings]()
		{
			return MakeExportPathText(WeakSettings.Get());
		}));
		// Leave the tooltip empty so the frame falls back to showing the full output path on
		// hover -- important when the path strip clips a long custom directory to its visible width.
		PathContext.ToolTip = FText::GetEmpty();
		PathContext.OnAction = [WeakSettings, SelectExportDirectory = InOnSelectExportDirectory]() mutable
		{
			if (!SelectExportDirectory.IsBound())
			{
				UE_LOG(LogAudioExporter, Warning, TEXT("%s: Export-path selection was requested without a bound callback."), *FString(__FUNCTION__));
				return FReply::Handled();
			}

			// Hand the folder picker the path the row currently shows so it opens at the staged
			// ExportDirectory (or the Saved/AudioExporter default) rather than a stale persisted
			// value.
			const FString OutFolder = SelectExportDirectory.Execute(ResolveExportStartingDirectory(WeakSettings.Get()));
			if (!OutFolder.IsEmpty())
			{
				if (UAudioExporterSettings* ExporterSettings = WeakSettings.Get(); IsValid(ExporterSettings))
				{
					ExporterSettings->ExportDirectory = OutFolder;
				}
				else
				{
					UE_LOG(LogAudioExporter, Warning, TEXT("%s: Export-path selection returned a folder but staged settings were unavailable."), *FString(__FUNCTION__));
				}
			}

			return FReply::Handled();
		};
		PathContext.IsResetToDefaultVisible = TAttribute<bool>::CreateLambda([WeakSettings]()
		{
			const UAudioExporterSettings* ExporterSettings = WeakSettings.Get();
			return IsValid(ExporterSettings) && !ExporterSettings->ExportDirectory.IsEmpty();
		});
		PathContext.OnResetToDefault = [WeakSettings]()
		{
			if (UAudioExporterSettings* ExporterSettings = WeakSettings.Get(); IsValid(ExporterSettings))
			{
				ExporterSettings->ExportDirectory.Empty();
			}
			return FReply::Handled();
		};

		return PathContext;
	}
#endif
}

TSharedRef<SWindow> SAudioExporterWindow::OpenSettingsWindow(
	const TArray<FAssetData>& InAssets,
	const FOnAudioExporterRequested& InOnExportRequested,
	const FOnAudioExporterRequestExportDirectory& InOnSelectExportDirectory)
{
	const AudioExporterWindowPrivate::FSelectedAudioExporterAssets SelectedAssets =
		AudioExporterWindowPrivate::FilterSupportedAssets(InAssets);

	UAudioExporterSettings* Settings = GetMutableDefault<UAudioExporterSettings>();
	if (IsValid(Settings))
	{
		Settings->LoadConfig();
	}
	else
	{
		UE_LOG(LogAudioExporter, Error, TEXT("%s: Audio Exporter settings object was unavailable."), *FString(__FUNCTION__));
	}

	// Every secondary window is built through the shared dialog factory so the OS
	// title follows "<Product> — <Task>" grammar and MinSize/placement/sizing stay consistent
	// with the other AudioFactory windows instead of a hand-rolled SNew(SWindow) chain.
	FAudioFactoryDialogWindowArgs DialogArgs;
	DialogArgs.ProductId = EAudioFactoryProductId::AudioExporter;
	DialogArgs.TaskTitle = LOCTEXT("SettingsWindowTask", "Settings");
	DialogArgs.ClientSize = AudioExporterWindowPrivate::DefaultSettingsWindowSize;
	DialogArgs.MinSize = AudioExporterWindowPrivate::MinimumSettingsWindowSize;

	TSharedRef<SWindow> Window = FAudioFactoryDialogWindow::Create(DialogArgs);

#if WITH_EDITOR
	const TWeakPtr<SWindow> WeakWindow = Window;

	FAudioFactoryPluginWindowSpec Spec;
	Spec.PluginId = TEXT("AudioExporter");
	Spec.WindowTitle = LOCTEXT("WindowTitle", "Audio Exporter");
	Spec.HeaderTitle = LOCTEXT("ToolName", "Audio Exporter");
	Spec.HeaderSubtitle = LOCTEXT("ToolSubtitle", "Native MetaSound, SoundCue, and SoundWave export settings");
	Spec.SourceSettingsObject = Settings;
	Spec.DetailsViewIdentifier = TEXT("AudioFactory.AudioExporter.NativeSettings");
	Spec.InitialSize = AudioExporterWindowPrivate::DefaultSettingsWindowSize;
	Spec.PrimaryActionLabel = LOCTEXT("ExportButtonLabel", "Export");
	Spec.OkLabel = LOCTEXT("OkButtonLabel", "OK");
	Spec.CancelLabel = LOCTEXT("CancelButtonLabel", "Cancel");
	Spec.OpenReportLabel = LOCTEXT("OpenLogsButtonLabel", "Logs");
	Spec.bShowOkButton = false;
	Spec.bShowOpenReportButton = true;

	Spec.OnValidate = [SelectedAssets](const UObject* SettingsObject)
	{
		return AudioExporterWindowPrivate::ValidateSettingsObject(SettingsObject, SelectedAssets);
	};

	Spec.OnResetDefaults = [](UObject* StagedObject, FText& OutError)
	{
		return AudioExporterWindowPrivate::ResetStagedSettings(StagedObject, OutError);
	};

	Spec.OnOpenReport = []()
	{
		AudioExporterWindowPrivate::OpenExportLogFolder();
	};

	Spec.OnPrimaryAction = [SelectedAssets, InOnExportRequested, WeakWindow](const UObject* /*AppliedObject*/) mutable
	{
		if (SelectedAssets.SupportedAssets.IsEmpty())
		{
			UE_LOG(LogAudioExporter, Warning, TEXT("%s: Export was requested without any valid MetaSoundSource, SoundCue, or SoundWave selections."), *FString(__FUNCTION__));
			return;
		}

		if (!InOnExportRequested.IsBound())
		{
			UE_LOG(LogAudioExporter, Error, TEXT("%s: Export was requested without a bound callback."), *FString(__FUNCTION__));
			return;
		}

		// the request delegate returns void, so acceptance cannot travel back through it.
		// A rejected start instead surfaces observably on the structured-result channel (exactly
		// one Rejected result settles synchronously inside the request), so observe that channel
		// across the call and let IT decide whether the window may close.
		bool bRequestWasRejected = false;
		FDelegateHandle RejectionObserverHandle;
		UAudioExporterRuntimeSubsystem* RuntimeSubsystem = UAudioExporterBPLibrary::GetRuntimeSubsystem();
		if (RuntimeSubsystem)
		{
			RejectionObserverHandle = RuntimeSubsystem->OnExportCompleteResultNative.AddLambda(
				[&bRequestWasRejected](const FAudioExportResult& Result)
				{
					if (!Result.bAcceptedStart && Result.Outcome == EAudioExportOutcome::Rejected)
					{
						bRequestWasRejected = true;
					}
				});
		}

		InOnExportRequested.Execute(SelectedAssets.SupportedAssets);

		if (RuntimeSubsystem && RejectionObserverHandle.IsValid())
		{
			RuntimeSubsystem->OnExportCompleteResultNative.Remove(RejectionObserverHandle);
		}

		// A rejected start keeps the window open so the user can correct or abandon the request;
		// only an accepted (or unobservable, degraded-subsystem) request tears it down.
		if (!bRequestWasRejected)
		{
			if (const TSharedPtr<SWindow> PinnedWindow = WeakWindow.Pin())
			{
				PinnedWindow->RequestDestroyWindow();
			}
		}
	};

	FAudioFactoryPluginSettingsFrameSpec FrameSpec;
	FrameSpec.NativeSpec = MoveTemp(Spec);
	FrameSpec.ProductId = EAudioFactoryProductId::AudioExporter;
	FrameSpec.OnBuildExtraFooterActions = [SelectedAssets, WeakWindow](UObject* /*StagedSettingsObject*/)
	{
		TArray<FAudioFactorySelectedAsset> SelectedRows =
			AudioExporterWindowPrivate::BuildSelectedAssetRows(SelectedAssets.SupportedAssets);
		return TArray<FAudioFactoryFooterAction>
		{
			FAudioFactoryFooterAction::MakeShowSelected([Rows = MoveTemp(SelectedRows), WeakWindow]() mutable
			{
				// Native-parent the Selected Assets window against the settings window that opened
				// it, rather than a bare top-level AddWindow.
				AudioExporterWindowPrivate::OpenSelectedAssetsWindow(Rows, WeakWindow.Pin());
				return FReply::Handled();
			})
		};
	};
	FrameSpec.AssetTypes.bSupportsMetaSounds = true;
	FrameSpec.AssetTypes.bSupportsSoundCues = true;
	FrameSpec.AssetTypes.bSupportsSoundWaves = true;
	FrameSpec.Counts.Add(FAudioFactoryInlineCount::MakeWithIcon(
		LOCTEXT("MetaSoundCountLabel", "MetaSounds"),
		FText::AsNumber(SelectedAssets.NumMetaSounds),
		FAudioFactoryClassIconNames::MetaSoundSource(),
		LOCTEXT("MetaSoundCountTooltip", "Selected MetaSoundSource assets that will be exported.")));
	FrameSpec.Counts.Add(FAudioFactoryInlineCount::MakeWithIcon(
		LOCTEXT("SoundCueCountLabel", "SoundCues"),
		FText::AsNumber(SelectedAssets.NumSoundCues),
		FAudioFactoryClassIconNames::SoundCue(),
		LOCTEXT("SoundCueCountTooltip", "Selected SoundCue assets that will be exported.")));
	FrameSpec.Counts.Add(FAudioFactoryInlineCount::MakeWithIcon(
		LOCTEXT("SoundWaveCountLabel", "SoundWaves"),
		FText::AsNumber(SelectedAssets.NumSoundWaves),
		FAudioFactoryClassIconNames::SoundWave(),
		LOCTEXT("SoundWaveCountTooltip", "Selected static SoundWave assets that will be exported 1 file each.")));
	FrameSpec.OnBuildPathContext = [InOnSelectExportDirectory](UObject* StagedObject) -> TOptional<FAudioFactoryPathContext>
	{
		return AudioExporterWindowPrivate::BuildExportPathContext(StagedObject, InOnSelectExportDirectory);
	};

	const TSharedRef<SWidget> SettingsContent = FAudioFactoryPluginWindowAdapter::BuildPluginSettingsFrame(MoveTemp(FrameSpec), WeakWindow);
	Window->SetContent(SettingsContent);
	// The shared frame's OnKeyDown is wired correctly, but nothing gives a window
	// keyboard focus by default, so Slate has no path to route Escape along. Explicitly focusing the content
	// on activate is the fix; the frame's own OnCancelRequested handles Escape from there.
	Window->SetWidgetToFocusOnActivate(SettingsContent);
#else
	Window->SetContent(
		SNew(STextBlock)
		.Text(LOCTEXT("EditorOnlyWindow", "Audio Exporter settings are only available in the editor.")));
#endif

	return Window;
}

#if WITH_DEV_AUTOMATION_TESTS
SAudioExporterWindow::FSupportedAssetCountsForTesting SAudioExporterWindow::GetSupportedAssetCountsForTesting(const TArray<FAssetData>& InAssets)
{
	const AudioExporterWindowPrivate::FSelectedAudioExporterAssets Result =
		AudioExporterWindowPrivate::FilterSupportedAssets(InAssets);

	FSupportedAssetCountsForTesting Out;
	Out.NumSupported = Result.SupportedAssets.Num();
	Out.NumMetaSounds = Result.NumMetaSounds;
	Out.NumSoundCues = Result.NumSoundCues;
	Out.NumSoundWaves = Result.NumSoundWaves;
	return Out;
}

FText SAudioExporterWindow::GetAssetClassLabelForTesting(const FAssetData& AssetData)
{
	return AudioExporterWindowPrivate::ResolveAssetClassLabel(AssetData);
}

FName SAudioExporterWindow::GetAssetIconNameForTesting(const FAssetData& AssetData)
{
	return AudioExporterWindowPrivate::ResolveAssetIconName(AssetData);
}

FText SAudioExporterWindow::GetDisplayedExportPathForTesting(const UAudioExporterSettings* Settings)
{
	return AudioExporterWindowPrivate::MakeExportPathText(Settings);
}

FString SAudioExporterWindow::GetExportBrowseStartingDirectoryForTesting(const UAudioExporterSettings* Settings)
{
	return AudioExporterWindowPrivate::ResolveExportStartingDirectory(Settings);
}

TSharedRef<SWindow> SAudioExporterWindow::OpenSelectedAssetsWindowForTesting(const TArray<FAssetData>& InAssets, const TSharedPtr<SWindow>& ParentWindow)
{
	const TArray<FAudioFactorySelectedAsset> Rows = AudioExporterWindowPrivate::BuildSelectedAssetRows(InAssets);
	return AudioExporterWindowPrivate::OpenSelectedAssetsWindow(Rows, ParentWindow);
}
#endif

#undef LOCTEXT_NAMESPACE
