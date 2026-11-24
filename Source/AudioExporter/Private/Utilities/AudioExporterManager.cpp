// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "Utilities/AudioExporterManager.h"
#include "AudioExporterRuntimeSubsystem.h"
#include "Utilities/AudioExportSettingLease.h"
#include "Utilities/AudioExporterSettings.h"
#include "Utilities/AudioExporterLogger.h"
#include "Utilities/AudioExporterUtils.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundSubmix.h"
#include "Logging/LogMacros.h"
#include "Settings/LevelEditorMiscSettings.h"
#include "Editor/EditorPerformanceSettings.h"
#include "Kismet/GameplayStatics.h"
#include "Editor.h"

FAudioExportManager::FAudioExportManager()
{
}

FAudioExportManager::~FAudioExportManager()
{
	// During editor shutdown GEngine may already be gone. That is a normal teardown path, so do not
	// call the subsystem resolver or emit its active-use error. If our tracked operation is still
	// active, the unavailability remains visible as a warning instead of being blanket-silenced.
	if (!IsValid(GEngine))
	{
		if (bExportActive)
		{
			UE_LOG(LogAudioExporter, Warning, TEXT("%s: GEngine became unavailable while an export was active; runtime cancellation was not available."), *FString(__FUNCTION__));
		}
		// Deliberately NOT RestoreEditorSettings() here. This branch is reached from
		// ShutdownModule during UnloadModulesAtShutdown, and RestoreEditorSettings resolves editor
		// settings CDOs via GetMutableDefault<>. Touching a CDO once the UObject system is tearing
		// down crashes the process (measured: EXCEPTION_ACCESS_VIOLATION in
		// UObject::PostEditChangeChainProperty via UClass::InternalCreateDefaultObjectWrapper).
		// There is no live editor left to restore settings for on this path, so skipping the
		// restore loses nothing: the settings objects die with the process.
		bRuntimeDelegatesBound = false;
		bExportActive = false;
		return;
	}

	UAudioExporterRuntimeSubsystem* RuntimeSubsystem = GEngine->GetEngineSubsystem<UAudioExporterRuntimeSubsystem>();
	const bool bRuntimeExportActive = bExportActive || (IsValid(RuntimeSubsystem) && RuntimeSubsystem->IsExporting());
	if (bRuntimeExportActive)
	{
		if (IsValid(RuntimeSubsystem))
		{
			RuntimeSubsystem->CancelExport();
		}
		else
		{
			UE_LOG(LogAudioExporter, Warning, TEXT("%s: Runtime subsystem became unavailable while an export was active."), *FString(__FUNCTION__));
		}
		RestoreEditorSettings();
	}

	UnbindRuntimeDelegates();
	bExportActive = false;
}

void FAudioExportManager::Initialize()
{
	ExporterSettings = GetMutableDefault<UAudioExporterSettings>();
	if (ensureMsgf(ExporterSettings, TEXT("FAudioExportManager::Initialize - Settings are null.")))
	{
		ExporterSettings->LoadConfig();
	}

	BindRuntimeDelegates();
	UE_LOG(LogAudioExporter, Log, TEXT("AudioExportManager initialized (engine-subsystem wrapper mode)."));
}

void FAudioExportManager::StartExporting(const TArray<FAssetData>& InAssets)
{
	UAudioExporterRuntimeSubsystem* RuntimeSubsystem = GetRuntimeSubsystem(*FString(__FUNCTION__));
	if (!RuntimeSubsystem)
	{
		return;
	}

	if (RuntimeSubsystem->IsExporting())
	{
		UE_LOG(LogAudioExporter, Warning, TEXT("%s: An export is already in progress."), *FString(__FUNCTION__));
		// surface the refusal observably so UI consumers (the settings window) can keep
		// themselves open for a corrected retry instead of tearing down on a rejected start.
		RuntimeSubsystem->BroadcastRejectedStart(TEXT("An export is already in progress; this request was rejected."));
		return;
	}

	if (!ensureMsgf(!InAssets.IsEmpty(), TEXT("FAudioExportManager::StartExporting - InAssets is empty.")))
	{
		RuntimeSubsystem->BroadcastRejectedStart(TEXT("The export request listed no assets."));
		return;
	}

	// Refresh settings
	Initialize();

	if (!ensureMsgf(ExporterSettings, TEXT("FAudioExportManager::StartExporting - ExporterSettings is null.")))
	{
		return;
	}

	TArray<FAssetData> ResolvedAssets;
	TArray<FAudioExportSoundEntry> SoundEntries = ResolveAssets(InAssets, ResolvedAssets);
	if (SoundEntries.IsEmpty())
	{
		UE_LOG(LogAudioExporter, Warning, TEXT("%s: No valid audio assets were available for export."), *FString(__FUNCTION__));
		RuntimeSubsystem->BroadcastRejectedStart(TEXT("No valid audio assets were available for export."));
		return;
	}

	ULevelEditorMiscSettings* LevelEditorSettings = GetMutableDefault<ULevelEditorMiscSettings>();
	if (LevelEditorSettings)
	{
		// lease the shared setting instead of snapshotting it. The first holder records
		// the user's true original; overlapping sessions (this manager and/or the Pro manager)
		// only increment the hold count, and exactly one restore happens -- by the last releaser.
		const bool bPreLeaseBackgroundAudio = LevelEditorSettings->bAllowBackgroundAudio;
		AudioExportLease_AcquireBackgroundAudio(bPreLeaseBackgroundAudio);
		bHoldsBackgroundAudioLease = true;
		LevelEditorSettings->bAllowBackgroundAudio = true;
	}

	UEditorPerformanceSettings* PerfSettings = GetMutableDefault<UEditorPerformanceSettings>();
	if (PerfSettings)
	{
		bWasThrottleCPUWhenNotForegroundEnabled = PerfSettings->bThrottleCPUWhenNotForeground;
		PerfSettings->bThrottleCPUWhenNotForeground = false;
		PerfSettings->PostEditChange();
	}

	UWorld* World = ResolveExportWorld();
	if (World)
	{
		bWasWorldRenderingEnabled = UGameplayStatics::GetEnableWorldRendering(World);
		UGameplayStatics::SetEnableWorldRendering(World, false);
	}
	else
	{
		UE_LOG(LogAudioExporter, Warning, TEXT("%s: Export world was unavailable; world rendering could not be disabled."), *FString(__FUNCTION__));
	}

	const FAudioExporterConfig Config = ExporterSettings->ToConfig();
	const FString OutputPath = ExporterSettings->ExportDirectory;
	// Manager-side preflight is complete. Arm and open the progress consumer before entering the
	// runtime so a synchronous terminal result has a live consumer and owned cleanup path.
	bExportActive = true;

	// The progress rows must be wired before the runtime call because the pipeline may settle while
	// StartExporting is still on this stack. The resolved order remains the pipeline's sole row order.
	OnAudioExportStarted.Broadcast(ResolvedAssets);

	if (!RuntimeSubsystem->StartExport(World, SoundEntries, Config, OutputPath))
	{
		UE_LOG(LogAudioExporter, Error, TEXT("%s: Runtime subsystem rejected the export request."), *FString(__FUNCTION__));
		// Runtime rejection may already have run the terminal cleanup synchronously. Both operations
		// are intentionally idempotent so every rejection path leaves the manager idle.
		RestoreEditorSettings();
		bExportActive = false;
		return;
	}
}

void FAudioExportManager::CancelExporting(bool bDeleteFiles)
{
	if (UAudioExporterRuntimeSubsystem* RuntimeSubsystem = GetRuntimeSubsystem(*FString(__FUNCTION__)))
	{
		RuntimeSubsystem->CancelExport(bDeleteFiles);
	}

	RestoreEditorSettings();
	bExportActive = false;
}

void FAudioExportManager::SetMonitoring(bool bMonitoring) const
{
	if (UAudioExporterRuntimeSubsystem* RuntimeSubsystem = GetRuntimeSubsystem(*FString(__FUNCTION__)))
	{
		RuntimeSubsystem->SetMonitoring(bMonitoring);
	}
}

void FAudioExportManager::RestoreEditorSettings()
{
	// the background-audio original is released through the shared lease. Only the last
	// releaser writes the value back; bHoldsBackgroundAudioLease keeps this idempotent across the
	// multiple restore paths (cancel, finish callback, synchronous rejection, destructor).
	if (bHoldsBackgroundAudioLease)
	{
		bHoldsBackgroundAudioLease = false;
		bool bOriginalBackgroundAudio = false;
		if (AudioExportLease_ReleaseBackgroundAudio(bOriginalBackgroundAudio))
		{
			if (ULevelEditorMiscSettings* LevelEditorSettings = GetMutableDefault<ULevelEditorMiscSettings>())
			{
				LevelEditorSettings->bAllowBackgroundAudio = bOriginalBackgroundAudio;
			}
		}
	}

	UEditorPerformanceSettings* PerfSettings = GetMutableDefault<UEditorPerformanceSettings>();
	if (PerfSettings)
	{
		PerfSettings->bThrottleCPUWhenNotForeground = bWasThrottleCPUWhenNotForegroundEnabled;
		PerfSettings->PostEditChange();
	}

	UWorld* World = ResolveExportWorld();
	if (World)
	{
		UGameplayStatics::SetEnableWorldRendering(World, bWasWorldRenderingEnabled);
	}
}


TArray<FAudioExportSoundEntry> FAudioExportManager::ResolveAssets(const TArray<FAssetData>& InAssets, TArray<FAssetData>& OutResolvedAssets) const
{
	TArray<FAssetData> FilteredAssets = FilterAudioAssets(InAssets);
	FilteredAssets.Sort();

	TArray<FAudioExportSoundEntry> Entries;
	Entries.Reserve(FilteredAssets.Num());
	OutResolvedAssets.Reset();
	OutResolvedAssets.Reserve(FilteredAssets.Num());

	for (const FAssetData& AssetData : FilteredAssets)
	{
		USoundBase* Sound = Cast<USoundBase>(AssetData.GetAsset());
		if (!IsValid(Sound))
		{
			UE_LOG(LogAudioExporter, Warning, TEXT("%s: Failed to load sound asset '%s'."), *FString(__FUNCTION__), *AssetData.AssetName.ToString());
			continue;
		}

		FAudioExportSoundEntry Entry;
		Entry.Sound = Sound;
		Entry.SoundName = AssetData.AssetName.ToString();
		Entry.SourceAssetPath = AssetData.PackageName.ToString();
		Entry.RelativePath = GetAssetRelativePath(AssetData);
		Entries.Add(Entry);

		// Kept in lock-step with Entries: this is the single source of truth for pipeline order that
		// both the pipeline's SoundQueue and the OnAudioExportStarted broadcast are built from.
		OutResolvedAssets.Add(AssetData);
	}

	return Entries;
}

#if WITH_DEV_AUTOMATION_TESTS
TArray<FAssetData> FAudioExportManager::ResolveOrderedAssetsForTesting(const TArray<FAssetData>& InAssets) const
{
	TArray<FAssetData> ResolvedAssets;
	ResolveAssets(InAssets, ResolvedAssets);
	return ResolvedAssets;
}
#endif

TArray<FAssetData> FAudioExportManager::FilterAudioAssets(const TArray<FAssetData>& InAssets)
{
	TArray<FAssetData> FilteredAssets;
	for (const FAssetData& Asset : InAssets)
	{
		if (IsAudioExporterSupportedAsset(Asset))
		{
			FilteredAssets.Add(Asset);
		}
	}
	return FilteredAssets;
}

FString FAudioExportManager::GetAssetRelativePath(const FAssetData& AssetData)
{
	FString PackagePath = AssetData.PackagePath.ToString();

	if (PackagePath.StartsWith(TEXT("/Game")))
	{
		PackagePath.RemoveAt(0, 5);
	}

	if (PackagePath.StartsWith(TEXT("/")))
	{
		PackagePath.RemoveAt(0, 1);
	}

	return PackagePath;
}

FString FAudioExportManager::GetExportPath() const
{
	if (const UAudioExporterRuntimeSubsystem* RuntimeSubsystem = GetRuntimeSubsystem(*FString(__FUNCTION__)))
	{
		return RuntimeSubsystem->GetExportPath();
	}

	return FString();
}

USoundSubmix* FAudioExportManager::GetRecordingSubmix() const
{
	if (const UAudioExporterRuntimeSubsystem* RuntimeSubsystem = GetRuntimeSubsystem(*FString(__FUNCTION__)))
	{
		return RuntimeSubsystem->GetRecordingSubmix();
	}

	return nullptr;
}

bool FAudioExportManager::IsExporting() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (bExportingForTesting)
	{
		return true;
	}
#endif
	if (const UAudioExporterRuntimeSubsystem* RuntimeSubsystem = GetRuntimeSubsystem(*FString(__FUNCTION__)))
	{
		return RuntimeSubsystem->IsExporting();
	}

	return false;
}

void FAudioExportManager::BindRuntimeDelegates()
{
	if (bRuntimeDelegatesBound)
	{
		return;
	}

	if (!IsValid(GEngine))
	{
		UE_LOG(LogAudioExporter, Verbose, TEXT("%s: GEngine is not ready yet; runtime delegate binding will be retried on first use."), *FString(__FUNCTION__));
		return;
	}

	if (UAudioExporterRuntimeSubsystem* RuntimeSubsystem = GEngine->GetEngineSubsystem<UAudioExporterRuntimeSubsystem>())
	{
		RuntimeSubsystem->OnExportProgressNative.AddRaw(this, &FAudioExportManager::HandleRuntimeExportProgress);
		RuntimeSubsystem->OnExportCompleteNative.AddRaw(this, &FAudioExportManager::HandleRuntimeExportFinished);
		RuntimeSubsystem->OnExportCompleteResultNative.AddRaw(this, &FAudioExportManager::HandleRuntimeExportResult);
		bRuntimeDelegatesBound = true;
	}
	else
	{
		UE_LOG(LogAudioExporter, Verbose, TEXT("%s: Runtime subsystem is not ready yet; runtime delegate binding will be retried on first use."), *FString(__FUNCTION__));
	}
}

void FAudioExportManager::UnbindRuntimeDelegates()
{
	if (!bRuntimeDelegatesBound)
	{
		return;
	}

	if (IsValid(GEngine))
	{
		if (UAudioExporterRuntimeSubsystem* RuntimeSubsystem = GEngine->GetEngineSubsystem<UAudioExporterRuntimeSubsystem>())
		{
			RuntimeSubsystem->OnExportProgressNative.RemoveAll(this);
			RuntimeSubsystem->OnExportCompleteNative.RemoveAll(this);
			RuntimeSubsystem->OnExportCompleteResultNative.RemoveAll(this);
		}
		else
		{
			UE_LOG(LogAudioExporter, Display, TEXT("%s: Runtime subsystem was unavailable during normal delegate teardown."), *FString(__FUNCTION__));
		}
	}
	else
	{
		// GEngine absence is normal during editor shutdown; the native delegate owner is being
		// destroyed, so clearing the local binding flag is the complete safe teardown behavior.
		UE_LOG(LogAudioExporter, Display, TEXT("%s: GEngine was unavailable during normal delegate teardown."), *FString(__FUNCTION__));
	}

	bRuntimeDelegatesBound = false;
}

void FAudioExportManager::HandleRuntimeExportProgress(const FAudioExportProgressInfo& ProgressInfo)
{
	OnAudioExportProgressUpdated.Broadcast(ProgressInfo);
}

void FAudioExportManager::HandleRuntimeExportResult(const FAudioExportResult& Result)
{
	// Structured always settles immediately before legacy within one terminal delivery (both
	// BroadcastTerminalOnce and BroadcastRejectedResult), so this flag is reliable when the paired
	// legacy event arrives. The paired handler below distinguishes an armed manager rejection from
	// a foreign rejection by the manager/runtime active state.
	bLastSettledResultWasForeignRejection = !Result.bAcceptedStart && Result.Outcome == EAudioExportOutcome::Rejected;
}

void FAudioExportManager::HandleRuntimeExportFinished(const FString& ExportPath)
{
	// A rejection from an armed manager request owns the terminal only when the runtime is already
	// idle. A foreign rejection either arrives while another session is active or while this manager
	// is idle; neither may close the manager's window or release its settings lease.
	bool bSkipForeignRejection = false;
	if (bLastSettledResultWasForeignRejection)
	{
		UAudioExporterRuntimeSubsystem* RuntimeSubsystem = nullptr;
		if (IsValid(GEngine))
		{
			RuntimeSubsystem = GEngine->GetEngineSubsystem<UAudioExporterRuntimeSubsystem>();
		}

		bSkipForeignRejection = !bExportActive || (IsValid(RuntimeSubsystem) && RuntimeSubsystem->IsExporting());
	}
	bLastSettledResultWasForeignRejection = false;
	if (bSkipForeignRejection)
	{
		return;
	}

	bExportActive = false;
	RestoreEditorSettings();
	OnAudioExportFinished.Broadcast(ExportPath);
}

UAudioExporterRuntimeSubsystem* FAudioExportManager::GetRuntimeSubsystem(const TCHAR* FunctionName) const
{
	if (!IsValid(GEngine))
	{
		UE_LOG(LogAudioExporter, Error, TEXT("%s: GEngine was unavailable while resolving UAudioExporterRuntimeSubsystem."), FunctionName);
		return nullptr;
	}

	UAudioExporterRuntimeSubsystem* RuntimeSubsystem = GEngine->GetEngineSubsystem<UAudioExporterRuntimeSubsystem>();
	if (!IsValid(RuntimeSubsystem))
	{
		UE_LOG(LogAudioExporter, Error, TEXT("%s: UAudioExporterRuntimeSubsystem was unavailable."), FunctionName);
		return nullptr;
	}

	return RuntimeSubsystem;
}

UWorld* FAudioExportManager::ResolveExportWorld() const
{
	if (GEditor && IsValid(GEngine))
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::Editor)
			{
				return Context.World();
			}
		}
	}

	return GWorld;
}
