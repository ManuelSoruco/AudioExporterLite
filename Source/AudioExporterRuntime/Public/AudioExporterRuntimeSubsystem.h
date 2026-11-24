// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "AudioExportPipeline.h"
#include "Engine/World.h"
#include "HAL/CriticalSection.h"
#include "Subsystems/EngineSubsystem.h"
#include "AudioExporterRuntimeSubsystem.generated.h"

class USoundSubmix;

using FAudioExportRequestToken = uint64;

/**
 * UAudioExporterRuntimeSubsystem
 *
 * An engine subsystem that manages the lifetime of the audio export pipeline across editor and runtime callers.
 * It exposes BlueprintAssignable multicast delegates so that Blueprints can react to progress
 * updates and completion events without needing to poll.
 *
 * Access it via:
 *   GEngine->GetEngineSubsystem<UAudioExporterRuntimeSubsystem>()
 * or from Blueprint through the UAudioExporterBPLibrary helper functions.
 */
UCLASS()
class AUDIOEXPORTERRUNTIME_API UAudioExporterRuntimeSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	// ---------------------------------------------------------------------------
	// UEngineSubsystem interface
	// ---------------------------------------------------------------------------
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ---------------------------------------------------------------------------
	// Blueprint-assignable delegates
	// ---------------------------------------------------------------------------

	/** Fired whenever export progress changes (current sound, current variation, etc.). */
	UPROPERTY(BlueprintAssignable, Category = "AudioFactory|Exporter")
	FOnExportProgressMulticast OnExportProgress;

	/** Native progress delegate used by the editor wrapper without Dynamic delegate restrictions. */
	FOnAudioExportProgressUpdated OnExportProgressNative;

	/** Fired when the export session finishes or is cancelled. ExportPath is empty on cancel/failure. */
	UPROPERTY(BlueprintAssignable, Category = "AudioFactory|Exporter")
	FOnExportCompleteMulticast OnExportComplete;

	/**
	 * Structured terminal result for the finished, cancelled, failed, or rejected session. This is
	 * the honest completion contract: outcome is an explicit enum, the path is unset unless files
	 * were written, and the counts are literal session facts. Fires exactly once per request.
	 */
	UPROPERTY(BlueprintAssignable, Category = "AudioFactory|Exporter")
	FOnExportCompleteResultMulticast OnExportCompleteResult;

	/** Native completion delegate used by the editor wrapper without Dynamic delegate restrictions. */
	FOnAudioExportFinished OnExportCompleteNative;

	/** Native structured-result delegate used without Dynamic delegate restrictions. */
	FOnAudioExportResult OnExportCompleteResultNative;

	// ---------------------------------------------------------------------------
	// Core API (called by UAudioExporterBPLibrary)
	// ---------------------------------------------------------------------------

	/**
	 * Starts an export session for the given sounds.
	 * @param World       - World used for audio playback during export.
	 * @param Sounds      - Pre-resolved sound entries to export.
	 * @param Config      - Export configuration.
	 * @param OutputPath  - Target directory. Empty string uses the auto-generated timestamped path.
	 */
	bool StartExport(UWorld* World,
	                 const TArray<FAudioExportSoundEntry>& Sounds,
	                 const FAudioExporterConfig& Config,
	                 const FString& OutputPath = TEXT(""),
	                 FAudioExportRequestToken RequestToken = 0,
	                 const FGuid& RequestId = FGuid());

	/** Cancels the active export session. Optionally deletes files already exported in this session. */
	void CancelExport(bool bDeleteFiles = false);

	/** Returns true while an export session is active. */
	bool IsExporting() const;

	/** Returns the export folder for the active session, or an empty string when idle. */
	FString GetExportPath() const;

	/** Returns the recording submix used by the active renderer when available. */
	USoundSubmix* GetRecordingSubmix() const;

	/** Applies monitor routing without muting the capture submix. */
	void SetMonitoring(bool bMonitoring) const;

	/** Registers a one-shot legacy path callback and returns its request-owned token. */
	FAudioExportRequestToken AddOneTimeCompleteCallback(TFunction<void(const FString&)> Callback);

	/** Registers a one-shot structured callback and returns its request-owned token. */
	FAudioExportRequestToken AddOneTimeResultCallback(TFunction<void(const FAudioExportResult&)> Callback);

	/**
	 * Settles a Rejected terminal result for a start request refused OUTSIDE the pipeline -- the
	 * editor manager's own refuse paths (concurrent session, unusable selection). Fires both
	 * completion surfaces (structured, then legacy with an empty path) exactly once for THIS
	 * request so fire-and-forget and window callers are never left without an observable
	 * rejection. Never touches ActiveRequestToken or any pending callback
	 * of a running session.
	 */
	void BroadcastRejectedStart(const FString& ErrorMessage, const FGuid& RequestId = FGuid());

#if WITH_DEV_AUTOMATION_TESTS
	/** Test hook: seeds an active pipeline session on this subsystem without touching audio devices. */
	bool BeginLifecycleTestSession(
		UWorld* World,
		const FString& InExportPath,
		FAudioExportRequestToken RequestToken = 0,
		const FGuid& RequestId = FGuid());

	/** Test hook: seeds a fixture session and keeps the request token attached to its terminal. */
	bool BeginLifecycleTestSession(
		UWorld* World,
		const TArray<FAudioExportSoundEntry>& Sounds,
		const FAudioExporterConfig& Config,
		const FString& InExportPath,
		FAudioExportRequestToken RequestToken = 0,
		const FGuid& RequestId = FGuid());

	/** Test hook: drives the pipeline's synchronous completion seam. */
	void SimulateVariationFinishedForTesting();

	/** Test hook: selects the synchronous save branch for deterministic callback coverage. */
	void SetForceAsyncSaveForTesting(bool bForceAsync);

	/** Test hook: returns the active pipeline's in-flight async export count. */
	int32 GetActiveAsyncExportsForTesting() const;
#endif

private:
	// ---------------------------------------------------------------------------
	// Pipeline helpers / callbacks
	// ---------------------------------------------------------------------------
	bool EnsurePipeline(UWorld* World);
	void HandlePipelineFinished(const FString& FinishedExportPath);
	void HandlePipelineResult(const FAudioExportResult& Result);
	void HandlePipelineProgress(const FAudioExportProgressInfo& ProgressInfo);

	/** Broadcasts a Rejected terminal result for a start request refused before the pipeline saw it. */
	void BroadcastRejectedResult(const FString& ErrorMessage, FAudioExportRequestToken RequestToken, const FGuid& RequestId);

	// ---------------------------------------------------------------------------
	// Internal state
	// ---------------------------------------------------------------------------

	/** The pipeline instance owned by this subsystem. */
	TSharedPtr<FAudioExportPipeline> Pipeline;

	/** The world used to initialize the current pipeline instance. */
	TWeakObjectPtr<UWorld> PipelineWorld;

	/** One-shot callbacks keyed by the request token that owns them. */
	TMap<FAudioExportRequestToken, TFunction<void(const FAudioExportResult&)>> PendingResultCallbacks;

	/** Synchronizes every PendingResultCallbacks mutation and lookup without holding it during user code. */
	FCriticalSection PendingResultCallbacksMutex;

	/** Monotonic token source. Registration is atomic even if a caller arrives off the game thread. */
	TAtomic<FAudioExportRequestToken> NextRequestToken{0};

	/** Token attached to the currently active pipeline session, if any. */
	FAudioExportRequestToken ActiveRequestToken = 0;
};
