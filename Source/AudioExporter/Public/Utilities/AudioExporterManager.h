// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "AudioExportPipeline.h"
#include "AssetRegistry/AssetData.h"

class UAudioExporterSettings;
class UAudioExporterRuntimeSubsystem;
class USoundSubmix;
class UWorld;

DECLARE_MULTICAST_DELEGATE_OneParam(FOnAudioExportStarted, const TArray<FAssetData>& /*Assets*/);

/**
 * FAudioExportManager is the Editor-side wrapper for the audio export process.
 * It resolves FAssetData -> USoundBase*, manages Editor-specific settings (background audio,
 * CPU throttling, world rendering), and delegates the actual audio processing to FAudioExportPipeline.
 *
 * Thin orchestrator pattern: all audio processing lives in the Runtime module.
 */
class AUDIOEXPORTER_API FAudioExportManager
{
public:
	FAudioExportManager();
	virtual ~FAudioExportManager();

	/** 
	 * Initializes the manager, loading settings.
	 * Should be called before starting any export operations.
	 */
	virtual void Initialize();

	/** 
	 * Starts the export process for a given list of assets.
	 * Resolves FAssetData to USoundBase*, configures editor settings, then delegates to the pipeline.
	 * @param InAssets - The list of audio assets (SoundCues, MetaSounds, etc.) to export.
	 */
	virtual void StartExporting(const TArray<FAssetData>& InAssets);

	/** 
	 * Cancels the current export process and cleans up resources.
	 * @param bDeleteFiles - If true, any files already exported in this session will be deleted.
	 */
	virtual void CancelExporting(bool bDeleteFiles = false);

	
	// ***** UTILITY FUNCTIONS ***** //
	
	/** Returns the base directory where the files from this session are being saved */
	FString GetExportPath() const;

	/** 
	 * Enables or disables audio monitoring (listening) during the export process.
	 * @param bMonitoring - True to hear the audio as it's being recorded.
	 */
	virtual void SetMonitoring(bool bMonitoring) const;

	/** Returns the submix used for recording in the current renderer. */
	USoundSubmix* GetRecordingSubmix() const;
	
	/** Checks if an export session is currently active */
	bool IsExporting() const;

#if WITH_DEV_AUTOMATION_TESTS
	void SetExportingForTesting(bool bInExporting) { bExportingForTesting = bInExporting; }
	/**
	 * Test hook: returns the exact ordered/filtered asset list StartExporting would hand the pipeline
	 * and broadcast via OnAudioExportStarted for a given raw selection. Exercises the same
	 * ResolveAssets() call used in production, so a passing test proves the broadcast and the
	 * pipeline's SoundQueue can never disagree on ordering.
	 */
	TArray<FAssetData> ResolveOrderedAssetsForTesting(const TArray<FAssetData>& InAssets) const;
#endif


	// ***** DELEGATES ***** //

	/** Delegate for when the export process starts (Editor-specific, carries FAssetData) */
	FOnAudioExportStarted OnAudioExportStarted;

	/** Delegate for updating progress (forwarded from pipeline) */
	FOnAudioExportProgressUpdated OnAudioExportProgressUpdated;

	/** Delegate for when the export process finishes (forwarded from pipeline) */
	FOnAudioExportFinished OnAudioExportFinished;

protected:
	/** 
	 * Restores the editor settings to their original state.
	 */
	void RestoreEditorSettings();
	void BindRuntimeDelegates();
	void UnbindRuntimeDelegates();
	void HandleRuntimeExportProgress(const FAudioExportProgressInfo& ProgressInfo);
	void HandleRuntimeExportFinished(const FString& ExportPath);

	/**
	 * Remembers whether the most recent structured settle was a foreign request's synchronous
	 * rejection. BroadcastTerminalOnce and BroadcastRejectedResult both settle structured BEFORE
	 * legacy within one call, so this lets HandleRuntimeExportFinished tell a real session end
	 * from a concurrent stranger's rejection completing on the same legacy surface.
	 */
	void HandleRuntimeExportResult(const FAudioExportResult& Result);

	/**
	 * Resolves a list of FAssetData into FAudioExportSoundEntry for the pipeline.
	 * Filters invalid assets, sorts them, and loads the USoundBase* for each.
	 * @param OutResolvedAssets - Receives the exact FAssetData entries that resolved successfully, in
	 *        the same sorted/filtered order as the returned FAudioExportSoundEntry array. This is the
	 *        single source of truth for pipeline order: StartExporting broadcasts this list via
	 *        OnAudioExportStarted so progress rows are keyed by the same order the pipeline runs.
	 */
	TArray<FAudioExportSoundEntry> ResolveAssets(const TArray<FAssetData>& InAssets, TArray<FAssetData>& OutResolvedAssets) const;

	/**
	 * Filters a list of assets down to the Audio Exporter-supported sources (SoundCues or MetaSounds).
	 */
	static TArray<FAssetData> FilterAudioAssets(const TArray<FAssetData>& InAssets);

	/**
	 * Gets the relative path of an asset (e.g., /Game/Sounds/UI -> Sounds/UI).
	 */
	static FString GetAssetRelativePath(const FAssetData& AssetData);
	UAudioExporterRuntimeSubsystem* GetRuntimeSubsystem(const TCHAR* FunctionName) const;
	UWorld* ResolveExportWorld() const;


	// ***** INTERNAL STATE VARIABLES ***** //
	
	/** Cached pointer to the exporter settings object */
	UAudioExporterSettings* ExporterSettings = nullptr;

	/**
	 * True while this manager holds a lease on the shared editor background-audio setting.
	 * Custody of the original value lives in the process-wide lease registry; this
	 * flag only makes release idempotent for THIS instance, so the double restore paths (cancel +
	 * finish callback, or a replayed completion) can never release twice.
	 */
	bool bHoldsBackgroundAudioLease = false;

	/** True when the last structured settle was a foreign request's rejection (see handler doc). */
	bool bLastSettledResultWasForeignRejection = false;

	/** Stores the original state of the editor's CPU throttling setting */
	bool bWasThrottleCPUWhenNotForegroundEnabled = false;

	/** Stores the original state of the world rendering setting */
	bool bWasWorldRenderingEnabled = false;

	/** True once native runtime delegates have been bound for the current manager instance. */
	bool bRuntimeDelegatesBound = false;

	/** True while the runtime subsystem has accepted an export that this manager must tear down. */
	bool bExportActive = false;

#if WITH_DEV_AUTOMATION_TESTS
	bool bExportingForTesting = false;
#endif
	
};
