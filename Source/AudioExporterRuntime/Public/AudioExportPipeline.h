// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AudioExporterLogger.h"
#include "Utilities/AudioRendererRealtime.h"
#include "AudioExporterConfig.h"
#include "PCM/AudioBufferHandler.h"
#include "UObject/GCObject.h"

#include "AudioExportPipeline.generated.h"

class USoundBase;
class UWorld;
class USoundSubmix;

/**
 * Structure to hold progress information about the ongoing export process.
 * This is used to update the UI and keep track of where we are in the batch export.
 */
USTRUCT(BlueprintType)
struct AUDIOEXPORTERRUNTIME_API FAudioExportProgressInfo
{
	GENERATED_BODY()

	/** Stable identity of the caller invocation that owns this update. */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Progress")
	FGuid RequestId;

	/** Stable identity of the accepted export session that owns this update. */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Progress")
	FGuid SessionId;

	/** Name of the sound currently being exported */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Progress")
	FString CurrentSoundName;
	
	/** Index of the current sound being processed (1-based for UI) */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Progress")
	int32 CurrentSound = 0;
	
	/** Total number of sounds to be exported in this session */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Progress")
	int32 TotalSounds = 0;
	
	/** Current variation index of the current sound (1-based for UI) */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Progress")
	int32 CurrentVariation = 0;
	
	/** Total variations to export for the current sound */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Progress")
	int32 TotalVariations = 0;

	/** Stable total number of files planned for the accepted export session. */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Progress")
	int32 TotalExpectedFiles = 0;

	/** True only for updates that represent a completed save attempt. */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Progress")
	bool bHasResult = false;

	/** Save result for this variation when bHasResult is true. */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Progress")
	bool bSucceeded = false;

	/** Human-readable result detail for the current progress row. */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Progress")
	FString ResultMessage;

	/** Full output path for the current row, when one was resolved. */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Progress")
	FString OutputFilePath;

	/** Number of files successfully written so far. */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Progress")
	int32 ExportedFiles = 0;

	/** Number of files that failed to write so far. */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Progress")
	int32 FailedFiles = 0;
};


DECLARE_MULTICAST_DELEGATE_OneParam(FOnAudioExportProgressUpdated, const FAudioExportProgressInfo&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnAudioExportFinished, const FString& /*ExportPath*/);

// Blueprint-accessible dynamic delegates (declared here so UHT finds them in the same pass as FAudioExportProgressInfo)
DECLARE_DYNAMIC_DELEGATE_OneParam(FOnExportComplete, const FString&, ExportPath);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnExportProgressMulticast, const FAudioExportProgressInfo&, ProgressInfo);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnExportCompleteMulticast, const FString&, ExportPath);


/**
 * Terminal outcome of one export session, stated explicitly. An outcome is never inferred from a
 * path or a bare bool: every terminal path (including startup rejection) settles exactly one
 * FAudioExportResult through the pipeline's OnExportResult delegate.
 */
UENUM(BlueprintType)
enum class EAudioExportOutcome : uint8
{
	/** At least one file was written and nothing failed or was skipped. */
	Succeeded,
	/** At least one file was written and at least one other variation failed or was skipped. */
	PartialSuccess,
	/** The session started but no file was written. */
	Failed,
	/** An accepted session was cancelled before completion. */
	Cancelled,
	/** The start request was refused before any recording began (concurrent session, invalid inputs, renderer unavailable). */
	Rejected
};


/**
 * Structured terminal result for one export session. This is the honest completion contract: the
 * outcome is an explicit enum, the export path is unset unless at least one file was actually
 * written, and the counts/error text are literal session facts (unknown values are never rendered
 * as zero or a placeholder).
 */
USTRUCT(BlueprintType)
struct AUDIOEXPORTERRUNTIME_API FAudioExportResult
{
	GENERATED_BODY()

	/** Stable identity of the caller invocation that produced this result. */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Result")
	FGuid RequestId;

	/** Stable identity of the accepted operation. Invalid for a pre-session rejection. */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Result")
	FGuid SessionId;

	/** Explicit terminal outcome of the session. */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Result")
	EAudioExportOutcome Outcome = EAudioExportOutcome::Failed;

	/** True only when the start request was accepted and a session actually began. */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Result")
	bool bAcceptedStart = false;

	/** Full path of the session directory. Unset (empty) unless at least one file was written. */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Result")
	FString ExportPath;

	/** Number of variations whose WAV was written successfully. */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Result")
	int32 SuccessfulCount = 0;

	/** Number of variations that failed to render or save. */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Result")
	int32 FailedCount = 0;

	/** Number of variations whose save was vetoed (e.g. cancelled mid-session). */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Result")
	int32 SkippedCount = 0;

	/** Human-readable terminal text. Audio failure/skip detail is primary; report failure detail is appended when present. */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Result")
	FString ErrorMessage;

	/** True when this session was configured to write reports. */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Result")
	bool bReportsRequested = false;

	/** Number of report files this session attempted to write. */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Result")
	int32 ReportsAttempted = 0;

	/** Number of report files written successfully. */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Result")
	int32 ReportsWritten = 0;

	/** Number of report files that failed to write. */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Result")
	int32 ReportsFailed = 0;

	/** Human-readable report failure text, empty when every requested report was written. */
	UPROPERTY(BlueprintReadOnly, Category = "AudioFactory|Exporter|Result")
	FString ReportErrorMessage;
};

DECLARE_MULTICAST_DELEGATE_OneParam(FOnAudioExportResult, const FAudioExportResult& /*Result*/);
DECLARE_DYNAMIC_DELEGATE_OneParam(FOnExportCompleteResult, const FAudioExportResult&, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnExportCompleteResultMulticast, const FAudioExportResult&, Result);


/**
 * Internal metadata for a single variation captured during a batch recording session.
 */
struct FPipelineVariationCaptureData
{
	/** Immutable identity assigned when this export request enters the pipeline. */
	const FGuid RequestId = FGuid();

	FPipelineVariationCaptureData() = default;
	explicit FPipelineVariationCaptureData(const FGuid& InRequestId)
		: RequestId(InRequestId)
	{
	}

	/** The timestamp (in seconds) when the variation recording started. */
	double StartTime = 0.0;

	/** The timestamp (in seconds) when the variation recording ended. */
	double EndTime = 0.0;

	/** The name of the sound being recorded. */
	FString SoundName;

	/** The source asset path of the sound. */
	FString SourceAssetPath;

	/** The relative export subfolder for this sound. */
	FString RelativePath;

	/** The index of this variation in the current batch recording. */
	int32 VariationIndex = 0;

	/** The index of the source sound in the current export queue. */
	int32 SoundIndex = 0;

	/** Total variations expected for the source sound. */
	int32 TotalVariations = 0;

	/** Whether this variation is set to loop. */
	bool bIsLoop = false;

	/** Whether this variation came from a deterministic, non-procedural SoundWave source. */
	bool bStaticSource = false;

	/** Explicit source speaker identity carried to the background WAV writer. */
	AudioExporter::EAudioChannelLayout ChannelLayout = AudioExporter::EAudioChannelLayout::Unknown;
};


/**
 * FAudioExportPipeline is the core runtime class responsible for orchestrating the audio export process.
 * It manages the queue of sounds, handles the transition between sounds and variations,
 * and interfaces with the renderer to record, process, and save audio.
 *
 * This class is Editor-agnostic: it receives pre-resolved USoundBase* and UWorld* references
 * from the Editor layer, and operates purely on audio data.
 *
 * It follows a state-machine-like flow:
 * StartExporting -> StartNextSound -> StartNextVariation -> ProcessCurrentSound -> OnVariationFinished -> (Next Variation/Sound) -> FinishExporting
 */
class AUDIOEXPORTERRUNTIME_API FAudioExportPipeline : public FGCObject, public TSharedFromThis<FAudioExportPipeline>
{
public:
	FAudioExportPipeline() = default;
	virtual ~FAudioExportPipeline() override;

	/** FGCObject interface */
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override
	{
		return TEXT("FAudioExportPipeline");
	}

	/** 
	 * Initializes the pipeline with a world context.
	 * @param InWorld - The world to use for audio playback.
	 */
	virtual void Initialize(UWorld* InWorld);

	/**
	 * Starts the export process for a given list of pre-resolved sounds.
	 * @param InSounds      - The list of sounds to export (must be loaded and valid).
	 * @param InConfig      - The export configuration.
	 * @param InOutputPath  - Optional override for the output directory. If empty, a timestamped
	 *                        subdirectory under ProjectSavedDir/AudioExporter is used.
	 * @return True when the request was accepted and a session actually began. A rejected start
	 *         settles completion through the structured result delegate with outcome Rejected and
	 *         returns false. A concurrent direct call reports its rejection without disturbing the
	 *         already-active session.
	 */
	virtual bool StartExporting(
		const TArray<FAudioExportSoundEntry>& InSounds,
		const FAudioExporterConfig& InConfig,
		const FString& InOutputPath = TEXT(""),
		const FGuid& InRequestId = FGuid());

	/** 
	 * Cancels the current export process and cleans up resources.
	 * @param bDeleteFiles - If true, any files already exported in this session will be deleted.
	 */
	virtual void CancelExporting(bool bDeleteFiles = false);

	
	// ***** UTILITY FUNCTIONS ***** //
	
	/** Returns the base directory where the files from this session are being saved */
	FString GetExportPath() const { return ExportPath; }

	/**
	 * Resolves a session directory that does not collide with an existing one. The timestamped
	 * folder name is only 1-second granular, so a cancel-and-restart (or two fast sessions)
	 * within the same second would otherwise reuse the same folder and overwrite prior output.
	 * When the requested directory already exists an incrementing "_N" suffix is appended, followed
	 * by a stable hash suffix under extreme collision pressure. The hash facts must exclude the
	 * timestamped output path so the same request remains stable across timestamp collisions.
	 */
	static FString ResolveUniqueSessionDirectory(const FString& InBaseDirectory, const FString& InStableRequestFacts);

	/** 
	 * Enables or disables audio monitoring (listening) during the export process.
	 * @param bMonitoring - True to hear the audio as it's being recorded.
	 */
	virtual void SetMonitoring(bool bMonitoring) const;

	/** Returns the submix used for recording in the current renderer. */
	USoundSubmix* GetRecordingSubmix() const { return Renderer.IsValid() ? Renderer->GetRecordingSubmix() : nullptr; }
	
	/** Checks if an export session is currently active */
	bool IsExporting() const { return bIsExporting.Load(); }

#if WITH_DEV_AUTOMATION_TESTS
	/** Test hook: starts a lightweight session without touching audio devices. */
	void BeginLifecycleTestSession(const FString& InExportPath);

	/** Test hook: starts a lightweight fixture session with queue/log state but no audio renderer. */
	void BeginLifecycleTestSession(
		const TArray<FAudioExportSoundEntry>& InSounds,
		const FAudioExporterConfig& InConfig,
		const FString& InExportPath);

	/** Test hook: returns a copy of the session's export log structs (the real report inputs). */
	TArray<FAudioExportAssetLog> GetExportLogsForTesting() const;

	/** Test hook: drives the final empty-buffer cleanup path. */
	void CompleteFinalEmptyBatchForTesting();

	/** Test hook: exposes the current callback generation to automation. */
	uint32 GetSessionGenerationForTesting() const { return SessionGeneration.Load(); }

	/** Test hook: exposes the in-flight async-save counter to automation. */
	int32 GetActiveAsyncExportsForTesting() const { return ActiveAsyncExports.Load(); }

	/**
	 * Forces ExportCurrentBatch's save-branch decision under automation. Every
	 * automation test runs with FApp::IsUnattended() == true, which otherwise always takes the
	 * synchronous save branch -- the async drain (FinalizeOrWaitForAsyncSaves), the final-save
	 * finalize handoff, and the ActiveAsyncExports lifecycle across cancel/restart get zero coverage.
	 * Unset (default) leaves production behavior untouched.
	 */
	void SetForceAsyncSaveForTesting(bool bForceAsync) { bForceAsyncSaveForTesting = bForceAsync; }

	/**
	 * Test hook: simulates one variation-save outcome without touching the file system. While armed,
	 * both ScheduleVariationSave call sites report this synthetic result in place of calling
	 * FAudioExporterUtils::ProcessAndSaveVariation, so automation can drive the destination-failure
	 * batch abort -- and its plain per-file-failure counter-case (bDestinationUnusable=false) --
	 * deterministically. The seam substitutes only the save outcome itself; the real latch
	 * accounting, skip settlement, and terminal-result machinery all run unchanged.
	 */
	void SetSimulatedSaveResultForTesting(bool bInSaved, bool bInDestinationUnusable, const FString& InFailureReason);

	/** Test hook: disarms a simulated save result injected by SetSimulatedSaveResultForTesting. */
	void ClearSimulatedSaveResultForTesting();

	/**
	 * Test hook: simulates a successfully rendered variation for the current sound without driving
	 * real-time audio playback. Headless automation cannot reach FAudioRendererRealtime's play-state /
	 * watchdog callbacks (no audio device progresses them under -nullrhi), so this fabricates a short
	 * tone buffer in place of a renderer capture, then runs the exact bookkeeping OnVariationFinished
	 * runs on a real success: same ScheduleVariationSave accounting for the variation, same
	 * CurrentVariationIndex/CurrentSoundIndex advance, same StartNextSound()/FinishExporting() cascade
	 * when the sound's last variation completes. This is what lets automation coverage
	 * reach the real async drain and finalize-lifecycle code without any renderer/audio dependency.
	 * No-op if the pipeline isn't exporting or the sound index is out of range.
	 */
	void SimulateVariationFinishedForTesting();

	/** Test hook: simulates a renderer-observed procedural completion class. */
	void SimulateProceduralCompletionForTesting(bool bReachedCaptureLimit);

	/** Test hook: attaches caller-visible identities to a seeded lifecycle session. */
	void SetCorrelationIdsForTesting(const FGuid& InRequestId, const FGuid& InSessionId)
	{
		CurrentRequestId = InRequestId;
		CurrentSessionId = InSessionId;
		CurrentProgressInfo.RequestId = InRequestId;
		CurrentProgressInfo.SessionId = InSessionId;
	}
#endif
	
	
	// ***** DELEGATES ***** //

	/** Delegate for updating progress */
	FOnAudioExportProgressUpdated OnExportProgressUpdated;

	/** Delegate for when the export process finishes */
	FOnAudioExportFinished OnExportFinished;

	/**
	 * Structured terminal result for the session. Broadcast exactly once through the same terminal
	 * gate as OnExportFinished: every path (success, partial, failure, cancel, startup rejection)
	 * settles one FAudioExportResult here.
	 */
	FOnAudioExportResult OnExportResult;

protected:
	/** 
	 * Checks if the renderer is initialized and ready.
	 * If not, it creates and initializes a new renderer instance.
	 */
	virtual bool EnsureRenderer();

	/** 
	 * Factory method to create the appropriate renderer.
	 * Can be overridden in subclasses for different rendering logic.
	 */
	virtual TUniquePtr<FAudioRendererRealtime> CreateRenderer() const;

	/** 
	 * Callback triggered when an error occurs during rendering.
	 */
	virtual void OnRenderError(FString ErrorMessage);

	/** 
	 * Callback triggered when a variation finishes in batch mode.
	 */
	virtual void OnVariationFinished();

	/** 
	 * Initiates the rendering process for the current sound and variation.
	 */
	virtual void ProcessCurrentSound();

	/** 
	 * Prepares and starts the export for the next sound in the queue.
	 */
	void StartNextSound();
	
	/** 
	 * Prepares and starts the export for the next variation of the current sound.
	 */
	virtual void StartNextVariation();
	
	/** 
	 * Finalizes the entire export session, saves logs, and cleans up.
	 */
	virtual void FinishExporting();
	
	/**
	 * Performs the final cleanup after all recordings and async exports are complete.
	 */
	void FinalizeExport();

	/**
	 * Processes and exports the current batch of variations.
	 * @param bIsFinal - Whether this is the final batch of the session.
	 */
	void ExportCurrentBatch(bool bIsFinal = false);

	/**
	 * Terminal handoff for a final batch that scheduled no saves of its own. Production always
	 * flushes the last sound's batch with bIsFinal=false, so the final call usually finds nothing
	 * pending while the previous batch's saves are still running: finalizing here would broadcast
	 * the terminal result and write the report before those saves record their rows. Finalize only
	 * when nothing is in flight, otherwise arm the drain and let the last save finalize.
	 */
	void FinalizeOrWaitForAsyncSaves();

	/**
	 * Stops the current batch recording and discards the captured data.
	 */
	void DiscardCurrentBatch();

	/**
	 * Salvages variations captured earlier in the current batch after a mid-sound render error:
	 * when the renderer still holds the recording buffer they are extracted and saved, otherwise
	 * every pending variation receives an explicit failure row. Never leaves a captured variation
	 * silently unrecorded.
	 */
	void FlushPendingVariations(const FString& FailureContext);
	
	/** 
	 * Generates and saves the export report (log) to the disk.
	 */
	virtual void WriteLogFilesToDisk();
	
	/** 
	 * Resets all internal state variables and progress info.
	 */
	void ResetState();

	/** Shared implementation used by public cancellation entrypoints and destructor cleanup. */
	void CancelExportingInternal(bool bDeleteFiles);

	/**
	 * Game-thread retirement for a save that belonged to a superseded session and was the last one
	 * in flight. The in-flight counter spans session generations, so a stale save can be the last to
	 * retire while a newer session is already waiting on the drain; that session's finalize gate has
	 * to be honoured here or it would never fire.
	 */
	void RetireStaleAsyncExport();

	/** Performs a cancel-requested directory deletion once no saves can re-create the folder. */
	void FlushDeferredCancelDelete();

	/** Broadcasts the terminal delegate once after the session has been made internally inactive. */
	void BroadcastTerminalOnce(const FString& FinishedExportPath);

	/** Settles every queued variation without a terminal result as an explicit cancellation skip. */
	void MarkOutstandingVariationsSkipped();

	/** Settles queued variations from a specific queue/variation position with an explicit skip detail. */
	void MarkVariationsSkippedFrom(int32 StartSoundIndex, int32 StartVariationIndex, const FString& SkipDetail);

	/** True once two destination-level save failures have latched the destination-abort batch stop. */
	bool HasDestinationAbortLatched() const;

	/**
	 * Records one destination-level save failure (the output directory or volume is broken, not just
	 * this one filename). On reaching the second such failure it latches the batch stop: composes
	 * the terminal reason, settles every not-yet-attempted variation as an explicit skip, and hands
	 * the session to the existing failure teardown so no further variation is started. Deliberately
	 * never touches bCancelRequested -- the user did not cancel, and the terminal outcome must be
	 * Failed/PartialSuccess, not Cancelled.
	 */
	void HandleDestinationUnusableFailure(
		const FPipelineVariationCaptureData& VarData,
		const FString& FailureReason);

	/** Fails startup after the editor/UI has already requested a session. */
	void FailStartup(const FString& ErrorMessage);

	/**
	 * The one per-request requested-count authority. Every settlement path (cancel, abort, failure)
	 * must settle this sound through the OBSERVED procedural completion class when one exists; the
	 * configured bound is only the honest fallback for sources whose completion class was never
	 * observed (progress denominators included). Settlement and report counts must never read the
	 * configured Max(Loop, OneShot) bound around this function.
	 */
	int32 CalculateRequestVariationCount(int32 SoundIndex) const;

	/** Records one save attempt in the logs, progress model, and aggregate result counts. */
	void RecordVariationResult(
		const FPipelineVariationCaptureData& VarData,
		bool bSaved,
		bool bCanSave,
		const FString& FileName,
		const FString& FilePath,
		float PeakDb,
		float RMSDb,
		float Duration,
		float GainAppliedDb,
		const FString& FailureReason,
		float GainRequestedDb = 0.0f,
		bool bGainLimited = false,
		bool bMeasurementsValid = false);

	/**
	 * Schedules the normalize/save step for one already-extracted variation buffer (synchronous or
	 * async, per the current save-branch decision) and records the result. Factored out of
	 * ExportCurrentBatch's per-variation loop so both the real renderer-extraction path and the
	 * automation test seam (SimulateVariationFinishedForTesting) run identical accounting/scheduling
	 * code -- the only thing the seam skips is Renderer::StopBatchRecording + ExtractVariationBuffer.
	 * @return true if the buffer was non-empty and a save was scheduled/attempted; false if the
	 *         buffer was empty and only a failure row was recorded.
	 */
	bool ScheduleVariationSave(
		const FPipelineVariationCaptureData& VarData,
		TArray<float> VariationBuffer,
		int32 SampleRate,
		int32 NumChannels);

	/** 
	 * Optional hook for subclasses to modify the audio buffer before it's saved to a file.
	 * Useful for things like crossfading loops or applying dithering.
	 */
	virtual void OnPreSaveRenderedBuffer(TArray<float>& InOutBuffer, int32 SampleRate, bool bIsLoop) {}

	/**
	 * Returns the total number of variations for the CURRENT sound (CurrentSoundToPlay). An ordinary
	 * non-procedural SoundWave always returns 1 -- the source is deterministic, so
	 * no fake variations are fabricated, regardless of bIsLoop or the configured variation counts.
	 * Every other supported asset type falls back to the configured One-Shot/Loop variation count.
	 */
	int32 GetTotalVariations(bool bIsLoop) const;

	/** Calculates the fixed output-file denominator for the accepted session. */
	int32 CalculateSessionExpectedFileCount() const;

	/**
	 * Builds one variation-capture record for the current sound/variation index and appends it to
	 * PendingVariations -- the exact bookkeeping OnVariationFinished performs on a real render
	 * completion. Factored out so the automation test seam (SimulateVariationFinishedForTesting)
	 * populates PendingVariations identically to production instead of bypassing it, keeping the
	 * test and production paths from silently diverging.
	 */
	FPipelineVariationCaptureData AppendPendingVariation(double StartTime, double EndTime, bool bIsLoop);

	/** Updates provisional procedural totals from the first renderer-observed completion class. */
	bool ApplyObservedProceduralCompletionClass(int32 SoundIndex, bool bReachedCaptureLimit, FString& OutError);

#if WITH_DEV_AUTOMATION_TESTS
	void SimulateVariationFinishedForTestingImpl(TOptional<bool> bReachedCaptureLimit);
#endif


	// ***** INTERNAL STATE VARIABLES ***** //
	
	/** The export configuration for the current session */
	FAudioExporterConfig Config;
	
	/** The world context used for audio playback */
	TWeakObjectPtr<UWorld> PipelineWorld;
	
	/** The list of sounds waiting to be exported */
	TArray<FAudioExportSoundEntry> SoundQueue;
	
	/** Index of the sound currently being processed */
	int32 CurrentSoundIndex = 0;
	
	/** Index of the variation currently being processed for the current sound */
	int32 CurrentVariationIndex = 0;
	
	/** State flag indicating if an export session is currently active */
	TAtomic<bool> bIsExporting{false};

	/** Atomic cancellation flag shared by callbacks and background saves. */
	TAtomic<bool> bCancelRequested{false};

	/**
	 * Destination-level save failures observed this session (the output directory or volume is
	 * broken, not just one filename). TAtomic because one of the two ProcessAndSaveVariation call
	 * sites settles on a background task; mirrors the bCancelRequested idiom above. Reaching TWO
	 * such failures latches the batch stop -- a single transient refusal must not kill a whole
	 * batch, while a genuinely dead destination fails twice immediately.
	 */
	TAtomic<int32> DestinationUnusableFailureCount{0};

	/** Monotonic session id used to fence stale callbacks from old renderer/tasks. */
	TAtomic<uint32> SessionGeneration{0};

	/** Public correlation identities for the current request/session. */
	FGuid CurrentRequestId;
	FGuid CurrentSessionId;

	/** Guards terminal completion/cancel delegates so each session emits exactly one. */
	bool bTerminalEventBroadcast = false;
	
	/** Map storing log data for each request processed in this session (key = immutable RequestId). */
	TMap<FGuid, FAudioExportAssetLog> ExportLogs;

	/** One immutable request identity per SoundQueue entry, assigned once at request creation. */
	TArray<FGuid> SoundRequestIds;

	/** Per-procedural-source completion class: -1 unknown, 0 natural, 1 capture ceiling. */
	TArray<int8> ObservedProceduralCompletionClass;
	
	/** The time when the current export session started */
	FDateTime ExportTimestamp;
	
	/** The base directory where the files from this session are being saved */
	FString ExportPath;

	/** True only when this start request created ExportPath itself. */
	bool bSessionDirectoryCreatedByStart = false;
	
	/** Current progress statistics used for UI updates */
	FAudioExportProgressInfo CurrentProgressInfo;

	/** Pointer to the actual sound object being played/recorded */
	TObjectPtr<USoundBase> CurrentSoundToPlay = nullptr;
	
	/** Metadata for variations captured in the current batch recording */
	TArray<FPipelineVariationCaptureData> PendingVariations;

	/** The renderer instance used to capture audio */
	TUniquePtr<FAudioRendererRealtime> Renderer;

	/** Number of variations currently being processed in background tasks */
	TAtomic<int32> ActiveAsyncExports{0};

	/** Whether we are waiting for async tasks to finish before final cleanup */
	bool bWaitingForFinalCleanup = false;

	/** True once this session has dispatched at least one asynchronous save. */
	bool bAsyncSaveDispatched = false;

	/** If true, the export folder will be deleted when the export is canceled */
	TAtomic<bool> bShouldDeleteFolderOnCancel{false};

	/**
	 * Session directory a cancel asked to delete while background saves were still in flight. It is
	 * tracked separately from ExportPath because a restarted session can replace ExportPath before
	 * the stale saves retire and trigger the deferred delete.
	 */
	FString PendingCancelDeleteDirectory;

	/** Aggregate file result counts for the active session. */
	int32 SuccessfulExportCount = 0;
	int32 FailedExportCount = 0;
	int32 SkippedExportCount = 0;
	bool bReportsRequested = false;
	int32 ReportsAttempted = 0;
	int32 ReportsWritten = 0;
	int32 ReportsFailed = 0;
	FString ReportErrorMessage;

	/** True once a start request passed every startup guard and a session actually began. */
	bool bAcceptedStart = false;

	/** Startup/error text carried into the terminal FAudioExportResult for Rejected outcomes. */
	FString TerminalErrorMessage;

	/** Exact detail for one or more save attempts vetoed by cancellation. */
	FString SkippedErrorMessage;

	/** Stable session-owned denominator used by all progress updates. */
	int32 SessionExpectedFileCount = 0;

	/** Number of in-flight saves belonging to the current session generation. */
	int32 CurrentSessionAsyncExports = 0;

	/**
	 * Overrides ExportCurrentBatch's synchronous-vs-async save branch under automation. Unset uses
	 * production behavior (FApp::IsUnattended() / FApp::CanEverRender()). Deliberately unconditional:
	 * a WITH_DEV_AUTOMATION_TESTS-gated data member in an exported class changes the object layout
	 * between translation units that disagree on the define (mirrors AudioExportProPipeline's
	 * DrainTimeoutSecondsOverride). Only the test-only setter above is conditional.
	 */
	TOptional<bool> bForceAsyncSaveForTesting;

	/**
	 * Automation-only simulated save outcome consumed by ScheduleVariationSave's two save call
	 * sites in place of the real disk write. Deliberately unconditional like
	 * bForceAsyncSaveForTesting: a WITH_DEV_AUTOMATION_TESTS-gated data member changes the object
	 * layout between translation units that disagree on the define. Only the test-only setter above
	 * is conditional.
	 */
	bool bHasSimulatedSaveResultForTesting = false;
	bool bSimulatedSaveSuccessForTesting = false;
	bool bSimulatedSaveDestinationUnusableForTesting = false;
	FString SimulatedSaveFailureReasonForTesting;
};
