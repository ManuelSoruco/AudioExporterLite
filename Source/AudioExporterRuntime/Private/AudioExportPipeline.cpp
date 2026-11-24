// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "AudioExportPipeline.h"
#include "Utilities/AudioExporterUtils.h"
#include "Utilities/AudioExporterLogger.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundWave.h"
#include "Sound/SoundWaveProcedural.h"
#include "Logging/LogMacros.h"
#include "UObject/GCObject.h"
#include "Async/Async.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/Crc.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace
{
	bool IsOrdinarySoundWave(const USoundBase* Sound)
	{
		return Sound != nullptr
			&& Sound->IsA<USoundWave>()
			&& !Sound->IsA<USoundWaveProcedural>();
	}

	int32 CalculateExpectedVariationCount(const FAudioExportSoundEntry& Entry, const FAudioExporterConfig& InConfig)
	{
		// An unavailable queue entry is one explicit failed row, not a fabricated variation set.
		if (!Entry.Sound)
		{
			return 1;
		}

		if (IsOrdinarySoundWave(Entry.Sound))
		{
			return 1;
		}

		// Every non-ordinary source has no trustworthy pre-playback loop declaration. Use the larger
		// configured count as a provisional upper bound until the first real completion tells us
		// whether this SoundCue, MetaSound, or procedural wave naturally finished or reached the
		// capture ceiling.
		return FMath::Max(FMath::Max(InConfig.LoopVariations, InConfig.OneShotVariations), 1);
	}

	FString BuildReportFailureDetail(int32 ReportsFailed, const FString& ReportErrorMessage)
	{
		if (ReportsFailed <= 0)
		{
			return FString();
		}

		FString Detail = FString::Printf(TEXT("%d report output(s) failed"), ReportsFailed);
		if (!ReportErrorMessage.IsEmpty())
		{
			Detail += FString::Printf(TEXT(": %s"), *ReportErrorMessage);
		}
		return Detail;
	}

	FString ComposeTerminalMessage(
		const FString& PrimaryMessage,
		int32 FailedExportCount,
		int32 SkippedExportCount,
		const FString& SkippedErrorMessage,
		int32 SuccessfulExportCount,
		int32 ReportsFailed,
		const FString& ReportErrorMessage)
	{
		FString Message = PrimaryMessage;

		if (FailedExportCount > 0)
		{
			if (!Message.IsEmpty())
			{
				Message += TEXT(" ");
			}
			Message += FString::Printf(TEXT("%d variation(s) failed to export."), FailedExportCount);
		}

		if (SkippedExportCount > 0)
		{
			if (!Message.IsEmpty())
			{
				Message += TEXT(" ");
			}
			Message += FString::Printf(TEXT("%d variation(s) were skipped."), SkippedExportCount);
			if (!SkippedErrorMessage.IsEmpty())
			{
				Message += FString::Printf(TEXT(" Skip detail: %s"), *SkippedErrorMessage);
			}
		}

		if (Message.IsEmpty() && SuccessfulExportCount <= 0)
		{
			Message = TEXT("Export finished without writing any files.");
		}

		const FString ReportDetail = BuildReportFailureDetail(ReportsFailed, ReportErrorMessage);
		if (!ReportDetail.IsEmpty())
		{
			if (!Message.IsEmpty())
			{
				Message += TEXT(" Report failure: ");
			}
			Message += ReportDetail;
		}

		return Message;
	}

	const TCHAR* AudioExportOutcomeToString(EAudioExportOutcome Outcome)
	{
		switch (Outcome)
		{
		case EAudioExportOutcome::Succeeded:
			return TEXT("Succeeded");
		case EAudioExportOutcome::PartialSuccess:
			return TEXT("PartialSuccess");
		case EAudioExportOutcome::Failed:
			return TEXT("Failed");
		case EAudioExportOutcome::Cancelled:
			return TEXT("Cancelled");
		case EAudioExportOutcome::Rejected:
		default:
			return TEXT("Rejected");
		}
	}

	void LogTerminalResult(const FAudioExportResult& Result)
	{
		const FString LoggedSavedCount = Result.bAcceptedStart
			? FString::FromInt(Result.SuccessfulCount)
			: FString(TEXT("unavailable"));
		const FString LoggedFailedCount = Result.bAcceptedStart
			? FString::FromInt(Result.FailedCount)
			: FString(TEXT("unavailable"));
		const FString LoggedSkippedCount = Result.bAcceptedStart
			? FString::FromInt(Result.SkippedCount)
			: FString(TEXT("unavailable"));
		const FString LoggedPath = Result.ExportPath.IsEmpty() ? FString(TEXT("unavailable")) : Result.ExportPath;
		const FString TerminalLine = FString::Printf(
			TEXT("[AudioExporter][Terminal] Outcome=%s Saved=%s Failed=%s Skipped=%s Path=%s"),
			AudioExportOutcomeToString(Result.Outcome),
			*LoggedSavedCount,
			*LoggedFailedCount,
			*LoggedSkippedCount,
			*LoggedPath);
		UE_LOG(LogAudioExporter, Display, TEXT("%s"), *TerminalLine);
	}

	void LogUnavailableSound(const FString& Context, const FString& Message, bool bOperationActive)
	{
		if (bOperationActive)
		{
			UE_LOG(LogAudioExporter, Warning, TEXT("%s: %s"), *Context, *Message);
		}
		else
		{
			UE_LOG(LogAudioExporter, Display, TEXT("%s: %s"), *Context, *Message);
		}
	}

	// Abort a batch on the SECOND destination-level save failure, never the first: a single
	// transient refusal (an antivirus scanner momentarily holding the temporary file) must not kill
	// a whole batch, while a genuinely dead destination fails twice immediately.
	constexpr int32 DestinationAbortFailureThreshold = 2;

	const TCHAR* DestinationAbortSkipDetail =
		TEXT("Skipped because the export stopped after repeated output-destination failures.");
}


FAudioExportPipeline::~FAudioExportPipeline()
{
	if (bIsExporting.Load())
	{
		CancelExportingInternal(false);
	}
}

void FAudioExportPipeline::AddReferencedObjects(FReferenceCollector& Collector)
{
	if (CurrentSoundToPlay)
	{
		Collector.AddReferencedObject(CurrentSoundToPlay);
	}

	for (FAudioExportSoundEntry& Entry : SoundQueue)
	{
		if (Entry.Sound)
		{
			Collector.AddReferencedObject(Entry.Sound);
		}
	}
}

void FAudioExportPipeline::Initialize(UWorld* InWorld)
{
	PipelineWorld = InWorld;
	ResetState();
	UE_LOG(LogAudioExporter, Log, TEXT("AudioExportPipeline initialized."));
}

bool FAudioExportPipeline::StartExporting(
	const TArray<FAudioExportSoundEntry>& InSounds,
	const FAudioExporterConfig& InConfig,
	const FString& InOutputPath,
	const FGuid& InRequestId)
{
	const FGuid EffectiveRequestId = InRequestId.IsValid() ? InRequestId : FGuid::NewGuid();
	// 0. Guard against multiple concurrent export sessions. The subsystem normally rejects these
	// before the pipeline is called, but direct pipeline callers receive the same explicit Rejected
	// structured result without disturbing the active session.
	if (bIsExporting.Load())
	{
		const FString ErrorMessage = TEXT("Export start rejected because an export session is already in progress.");
		UE_LOG(LogAudioExporter, Warning, TEXT("%s: %s"), *FString(__FUNCTION__), *ErrorMessage);

		FAudioExportResult RejectedResult;
		RejectedResult.RequestId = EffectiveRequestId;
		RejectedResult.Outcome = EAudioExportOutcome::Rejected;
		RejectedResult.bAcceptedStart = false;
		RejectedResult.ErrorMessage = ErrorMessage;
		LogTerminalResult(RejectedResult);
		OnExportResult.Broadcast(RejectedResult);
		return false;
	}

	// Every non-concurrent request gets a new generation, including requests that fail startup. This
	// fences callbacks from a cancelled session even when the new request is rejected before it can
	// create a renderer or output directory.
	++SessionGeneration;
	bCancelRequested = false;

	// The terminal gate belongs to the new request once the previous session is gone; a stale true
	// value would otherwise swallow the Rejected settlement below.
	bTerminalEventBroadcast = false;
	ResetState();
	CurrentRequestId = EffectiveRequestId;

	// 1. Early checks: ensure we have sounds to export. These are refusal paths, not programmer
	// asserts: every rejected start settles a Rejected terminal result instead of leaving the
	// caller hanging without a completion event.
	if (InSounds.IsEmpty())
	{
		FailStartup(TEXT("Export start rejected because the sound list was empty."));
		return false;
	}

	if (!PipelineWorld.IsValid())
	{
		FailStartup(TEXT("Export start rejected because the pipeline world was invalid."));
		return false;
	}

	// Store config. Blueprint and C++ callers are not bound by the ClampMin/ClampMax metadata, so the
	// pipeline boundary is where out-of-range values have to be coerced.
	Config = InConfig;
	Config.Sanitize();

	// Set the timestamp and export path for this specific session.
	// A timestamped sub-folder is always created so that multiple export sessions
	// never overwrite each other, whether using the default path or a custom one.
	ExportTimestamp = FDateTime::Now();
	const FString TimestampString = ExportTimestamp.ToString(TEXT("%Y-%m-%d_%H-%M-%S"));
	const FString BaseExportPath = !InOutputPath.IsEmpty()
		? InOutputPath / TimestampString
		: FPaths::ProjectSavedDir() / TEXT("AudioExporter") / TimestampString;

	// The timestamp is only 1-second granular; if that folder already exists (e.g. a cancel and
	// immediate restart within the same second) fall back to a suffixed folder so we never
	// overwrite a previous session's exports.
	FString StableRequestFacts = EffectiveRequestId.ToString(EGuidFormats::DigitsWithHyphens);
	for (const FAudioExportSoundEntry& Entry : InSounds)
	{
		StableRequestFacts += TEXT("|");
		StableRequestFacts += Entry.SourceAssetPath;
		StableRequestFacts += TEXT("|");
		StableRequestFacts += Entry.RelativePath;
	}
	ExportPath = ResolveUniqueSessionDirectory(BaseExportPath, StableRequestFacts);

	const bool bSessionDirectoryAlreadyExisted = !ExportPath.IsEmpty() && IFileManager::Get().DirectoryExists(*ExportPath);
	if (ExportPath.IsEmpty() || !IFileManager::Get().MakeDirectory(*ExportPath, true))
	{
		FailStartup(FString::Printf(TEXT("Failed to create export output directory '%s'."), *ExportPath));
		return false;
	}
	bSessionDirectoryCreatedByStart = !bSessionDirectoryAlreadyExisted;

	// 2. Set up the sound queue and progress tracking
	SoundQueue = InSounds;
	SoundRequestIds.Reset();
	SoundRequestIds.Reserve(SoundQueue.Num());
	CurrentProgressInfo.TotalSounds = SoundQueue.Num();
	ObservedProceduralCompletionClass.Init(-1, SoundQueue.Num());
	SessionExpectedFileCount = CalculateSessionExpectedFileCount();
	CurrentProgressInfo.TotalExpectedFiles = SessionExpectedFileCount;

	// 3. Initialize logs for each sound
	ExportLogs.Empty();
	ExportLogs.Reserve(SoundQueue.Num());
	for (const FAudioExportSoundEntry& Entry : SoundQueue)
	{
		const FGuid RequestId = FGuid::NewGuid();
		SoundRequestIds.Add(RequestId);
		FAudioExportAssetLog AssetLog;
		AssetLog.SourceAssetPath = Entry.SourceAssetPath;
		AssetLog.RelativePath = Entry.RelativePath;
		ExportLogs.Add(RequestId, MoveTemp(AssetLog));
	}

	// 4. Every supported source uses the same realtime duplicate and owned recording submix.
	// Ordinary SoundWaves are still one file per asset, but never bypass playback for decoded PCM.
	if (!EnsureRenderer())
	{
		FailStartup(TEXT("Export startup failed because the audio renderer was unavailable."));
		return false;
	}

	SetMonitoring(Config.bMonitor);

	bIsExporting = true;

	bAcceptedStart = true;
	CurrentSessionId = FGuid::NewGuid();
	CurrentProgressInfo.RequestId = CurrentRequestId;
	CurrentProgressInfo.SessionId = CurrentSessionId;
	StartNextSound();
	return true;
}

FString FAudioExportPipeline::ResolveUniqueSessionDirectory(const FString& InBaseDirectory, const FString& InStableRequestFacts)
{
	if (InBaseDirectory.IsEmpty() || !IFileManager::Get().DirectoryExists(*InBaseDirectory))
	{
		return InBaseDirectory;
	}

	// The requested folder already exists. Append an incrementing readable suffix until we find a
	// free name so a new session never lands inside (and overwrites) a previous session's folder.
	for (int32 Suffix = 2; Suffix < 1000; ++Suffix)
	{
		const FString Candidate = FString::Printf(TEXT("%s_%d"), *InBaseDirectory, Suffix);
		if (!IFileManager::Get().DirectoryExists(*Candidate) && !IFileManager::Get().FileExists(*Candidate))
		{
			return Candidate;
		}
	}

	// Under extreme collision pressure, derive every further candidate from request facts supplied by
	// StartExporting. The timestamped output path is deliberately excluded so equal requests remain
	// stable when the session timestamp changes.
	const uint32 StableRequestHash = FCrc::StrCrc32(*InStableRequestFacts);
	for (uint32 Disambiguator = 0;; ++Disambiguator)
	{
		const FString Candidate = Disambiguator == 0
			? FString::Printf(TEXT("%s_%08X"), *InBaseDirectory, StableRequestHash)
			: FString::Printf(TEXT("%s_%08X_%u"), *InBaseDirectory, StableRequestHash, Disambiguator);
		if (!IFileManager::Get().DirectoryExists(*Candidate) && !IFileManager::Get().FileExists(*Candidate))
		{
			return Candidate;
		}
	}

}

void FAudioExportPipeline::CancelExporting(bool bDeleteFiles)
{
	CancelExportingInternal(bDeleteFiles);
}

void FAudioExportPipeline::CancelExportingInternal(bool bDeleteFiles)
{
	if (!bIsExporting.Load() && ActiveAsyncExports.Load() == 0 && !bWaitingForFinalCleanup)
	{
		return;
	}

	if (bTerminalEventBroadcast)
	{
		// A second cancel may upgrade a deferred folder deletion request, but must not emit another
		// terminal event or recount rows already settled by the first cancel.
		bShouldDeleteFolderOnCancel = bShouldDeleteFolderOnCancel.Load() || bDeleteFiles;
		if (bShouldDeleteFolderOnCancel.Load() && !ExportPath.IsEmpty())
		{
			PendingCancelDeleteDirectory = ExportPath;
			FlushDeferredCancelDelete();
		}
		return;
	}

	if (bCancelRequested.Load())
	{
		bShouldDeleteFolderOnCancel = bShouldDeleteFolderOnCancel.Load() || bDeleteFiles;
		if (bShouldDeleteFolderOnCancel.Load() && !ExportPath.IsEmpty())
		{
			PendingCancelDeleteDirectory = ExportPath;
		}
		if (ActiveAsyncExports.Load() == 0)
		{
			RetireStaleAsyncExport();
		}
		return;
	}

	// 1. Mark as no longer exporting and prevent final cleanup from triggering
	++SessionGeneration;
	bCancelRequested = true;
	bIsExporting = false;
	bWaitingForFinalCleanup = ActiveAsyncExports.Load() > 0;
	bShouldDeleteFolderOnCancel = bDeleteFiles;

	// 2. Shutdown the renderer if it's currently active
	if (Renderer.IsValid() && Renderer->IsInitialized())
	{
		Renderer->RequestCancel();
		Renderer->Shutdown();
		Renderer.Reset();
	}

	// 3. Delete already exported files if requested. A background save that is mid-write would
	// re-create the folder we just removed, so the delete is queued and flushed by whichever save
	// retires last; FlushDeferredCancelDelete performs it immediately when nothing is in flight.
	if (bShouldDeleteFolderOnCancel.Load() && !ExportPath.IsEmpty())
	{
		PendingCancelDeleteDirectory = ExportPath;
		FlushDeferredCancelDelete();
	}

	// 4. Settle every row that has not reached a terminal save result. This includes the active
	// renderer variation, captured-but-not-yet-scheduled variations, future queue entries, and saves
	// whose worker result has not retired yet. The row identity check makes this idempotent across all
	// of those categories.
	MarkOutstandingVariationsSkipped();

	// 5. Clean up state
	CurrentSoundToPlay = nullptr;
	PendingVariations.Empty();
	SoundQueue.Empty();
	CurrentSessionAsyncExports = 0;
	CurrentProgressInfo = FAudioExportProgressInfo();

	// Cancellation is a terminal user outcome as soon as every requested row is settled. The old
	// save workers are fenced by SessionGeneration and retain the pipeline lifetime until they drain,
	// so deferring this result allowed an immediate restart to erase the cancelled request entirely.
	// Folder deletion remains deferred until those workers retire.
	const uint32 CancelledGeneration = SessionGeneration.Load();
	BroadcastTerminalOnce(TEXT("")); // Empty string indicates cancellation/failure
	if (SessionGeneration.Load() == CancelledGeneration)
	{
		ResetState();
	}
}

void FAudioExportPipeline::RetireStaleAsyncExport()
{
	if (ActiveAsyncExports.Load() != 0)
	{
		return;
	}

	FlushDeferredCancelDelete();

	if (bWaitingForFinalCleanup && !bCancelRequested.Load())
	{
		FinalizeExport();
		return;
	}

	if (bWaitingForFinalCleanup && bCancelRequested.Load())
	{
		bWaitingForFinalCleanup = false;
		BroadcastTerminalOnce(TEXT("")); // Empty string indicates cancellation/failure
		if (!bIsExporting.Load())
		{
			ResetState();
		}
		return;
	}

	// Do not reset here. This callback belongs to a superseded session, and a new request may have
	// started while its terminal callback was dispatching. The next request owns its reset; clearing
	// state from this stale retirement would erase a newly started session.
}

void FAudioExportPipeline::FlushDeferredCancelDelete()
{
	if (PendingCancelDeleteDirectory.IsEmpty() || ActiveAsyncExports.Load() != 0)
	{
		return;
	}

	const FString DirectoryToDelete = PendingCancelDeleteDirectory;
	PendingCancelDeleteDirectory.Empty();

	// Idempotent: cancel can be requested more than once, and a session folder may already be gone.
	if (!IFileManager::Get().DirectoryExists(*DirectoryToDelete))
	{
		return;
	}

	if (IFileManager::Get().DeleteDirectory(*DirectoryToDelete, false, true))
	{
		UE_LOG(LogAudioExporter, Log, TEXT("Canceled export. Deleted already exported files in: %s"), *DirectoryToDelete);
	}
	else
	{
		UE_LOG(LogAudioExporter, Warning, TEXT("%s: Failed to delete the canceled export folder '%s'."), *FString(__FUNCTION__), *DirectoryToDelete);
	}
}

void FAudioExportPipeline::SetMonitoring(bool bMonitoring) const
{
	if (!Renderer.IsValid() || !Renderer->IsInitialized())
	{
		return;
	}
	Renderer->SetMonitoring(bMonitoring);
}

bool FAudioExportPipeline::EnsureRenderer()
{
	// 1. Early return if we already have an initialized and ready renderer
	if (Renderer.IsValid() && Renderer->IsInitialized())
	{
		return true;
	}

	// 2. Clean up any existing (but perhaps broken/uninitialized) renderer
	if (Renderer.IsValid())
	{
		Renderer->RequestCancel();
		Renderer->Shutdown();
		Renderer.Reset();
	}

	// 3. Create a new renderer instance via the factory method
	Renderer = CreateRenderer();
	if (!Renderer.IsValid())
	{
		UE_LOG(LogAudioExporter, Error, TEXT("%s: Failed to create renderer."), *FString(__FUNCTION__));
		return false;
	}

	// 4. Bind events so we know when a render is finished or if an error occurs
	Renderer->OnVariationFinished.BindRaw(this, &FAudioExportPipeline::OnVariationFinished);
	Renderer->OnRenderError.BindRaw(this, &FAudioExportPipeline::OnRenderError);

	// 5. Initialize the renderer with the pipeline's world context
	if (!Renderer->Initialize(PipelineWorld.Get()))
	{
		UE_LOG(LogAudioExporter, Error, TEXT("%s: Failed to initialize the audio renderer."), *FString(__FUNCTION__));
		Renderer->Shutdown();
		Renderer.Reset();
		return false;
	}

	return true;
}

TUniquePtr<FAudioRendererRealtime> FAudioExportPipeline::CreateRenderer() const
{
	return MakeUnique<FAudioRendererRealtime>();
}

void FAudioExportPipeline::OnRenderError(FString ErrorMessage)
{
	if (bCancelRequested.Load() || !bIsExporting.Load())
	{
		return;
	}

	UE_LOG(LogAudioExporter, Error, TEXT("%s: %s"), *FString(__FUNCTION__), *ErrorMessage);

	if (SoundQueue.IsValidIndex(CurrentSoundIndex))
	{
		const FAudioExportSoundEntry& Entry = SoundQueue[CurrentSoundIndex];
		FPipelineVariationCaptureData FailedVariation(SoundRequestIds[CurrentSoundIndex]);
		FailedVariation.SoundName = Entry.SoundName;
		FailedVariation.SourceAssetPath = Entry.SourceAssetPath;
		FailedVariation.RelativePath = Entry.RelativePath;
		FailedVariation.VariationIndex = CurrentVariationIndex;
		FailedVariation.SoundIndex = CurrentSoundIndex;
		FailedVariation.TotalVariations = FMath::Max(CurrentProgressInfo.TotalVariations, 1);
		FailedVariation.bIsLoop = Renderer.IsValid() && Renderer->WasLastExportLooping();
		FailedVariation.bStaticSource = IsOrdinarySoundWave(CurrentSoundToPlay.Get());
		RecordVariationResult(
			FailedVariation,
			false,
			true,
			FString(),
			FString(),
			0.0f,
			0.0f,
			0.0f,
			0.0f,
			FString::Printf(TEXT("Render failed: %s"), *ErrorMessage));
	}

	// Variations captured earlier in this batch must not vanish with the failed render: salvage the
	// recording buffer while it is still available, or write explicit failure rows for each one.
	FlushPendingVariations(ErrorMessage);

	// The failed render also settles every variation of the current sound that never started. Keep
	// later queue entries available when the renderer can restart, but never leave planned work as
	// an invisible gap in the report.
	MarkVariationsSkippedFrom(
		CurrentSoundIndex,
		CurrentVariationIndex + 1,
		TEXT("Skipped because the render error stopped this sound before this variation started."));

	// If an error occurs during rendering, we skip the current sound and ensure the next one starts fresh.

	// 1. Stop and discard current batch for this failed sound
	DiscardCurrentBatch();

	// 2. Restart recording for every remaining source. SoundWaves use the same realtime capture
	// path as cues and MetaSounds, including after a preceding render failure.
	const bool bNextSoundNeedsRenderer = SoundQueue.IsValidIndex(CurrentSoundIndex + 1)
		&& SoundQueue[CurrentSoundIndex + 1].Sound != nullptr;
	if (bNextSoundNeedsRenderer && bIsExporting.Load() && Renderer.IsValid())
	{
		if (!Renderer->StartBatchRecording())
		{
			MarkVariationsSkippedFrom(
				CurrentSoundIndex + 1,
				0,
				TEXT("Skipped because batch recording could not restart after the render error."));
			FailStartup(TEXT("Failed to restart batch recording after a render error."));
			return;
		}
	}

	CurrentSoundIndex++;
	StartNextSound();
}

void FAudioExportPipeline::FlushPendingVariations(const FString& FailureContext)
{
	if (PendingVariations.IsEmpty())
	{
		return;
	}

	const TArray<FPipelineVariationCaptureData> Variations = MoveTemp(PendingVariations);
	PendingVariations.Empty();

	// When the renderer still holds the batch recording, the already-captured variations are still
	// extractable from its buffer and can be saved. Otherwise nothing remains to salvage and every
	// pending variation becomes an explicit failure row so the report never silently drops them.
	TArray<float> BigBuffer;
	int32 SampleRate = 0;
	int32 NumChannels = 0;
	bool bBufferAvailable = false;
	if (Renderer.IsValid() && Renderer->IsInBatchRecording())
	{
		Renderer->StopBatchRecording(BigBuffer, NumChannels, SampleRate);
		bBufferAvailable = !BigBuffer.IsEmpty() && SampleRate > 0 && NumChannels > 0;
	}

	if (bBufferAvailable)
	{
		const double MeasuredBatchDuration = Renderer->GetBatchEndTime() - Renderer->GetBatchStartTime();
		const double ActualBatchDuration = (SampleRate > 0 && NumChannels > 0)
			                                   ? (static_cast<double>(BigBuffer.Num()) / (SampleRate * NumChannels))
			                                   : 0.0;
		const double TimeCorrectionFactor = (MeasuredBatchDuration > 0.0)
			                                    ? (ActualBatchDuration / MeasuredBatchDuration)
			                                    : 1.0;

		for (const FPipelineVariationCaptureData& VarData : Variations)
		{
			TArray<float> VariationBuffer = FAudioExporterUtils::ExtractVariationBuffer(
				BigBuffer, SampleRate, NumChannels, VarData.StartTime, VarData.EndTime, TimeCorrectionFactor,
				VarData.bIsLoop);
			ScheduleVariationSave(VarData, MoveTemp(VariationBuffer), SampleRate, NumChannels);
		}
	}
	else
	{
		const FString FlushFailureReason = FString::Printf(
			TEXT("Render failed (%s); this variation was captured but could not be exported."), *FailureContext);
		for (const FPipelineVariationCaptureData& VarData : Variations)
		{
			RecordVariationResult(
				VarData,
				false,
				true,
				FString(),
				FString(),
				0.0f,
				0.0f,
				0.0f,
				0.0f,
				FlushFailureReason);
		}
	}
}

int32 FAudioExportPipeline::CalculateRequestVariationCount(int32 SoundIndex) const
{
	if (!SoundQueue.IsValidIndex(SoundIndex))
	{
		return 1;
	}

	const FAudioExportSoundEntry& Entry = SoundQueue[SoundIndex];
	// An observed completion class is the request's authority: a source that finished naturally owns
	// the one-shot plan, one that hit the capture ceiling owns the loop plan. The configured
	// Max(Loop, OneShot) bound must never widen an observed plan again.
	if (Entry.Sound && !IsOrdinarySoundWave(Entry.Sound)
		&& ObservedProceduralCompletionClass.IsValidIndex(SoundIndex)
		&& ObservedProceduralCompletionClass[SoundIndex] >= 0)
	{
		return FMath::Max(
			ObservedProceduralCompletionClass[SoundIndex] == 1 ? Config.LoopVariations : Config.OneShotVariations,
			1);
	}

	// Never-observed, ordinary, or unavailable sources keep the per-entry honest count.
	return CalculateExpectedVariationCount(Entry, Config);
}

bool FAudioExportPipeline::ApplyObservedProceduralCompletionClass(
	int32 SoundIndex,
	bool bReachedCaptureLimit,
	FString& OutError)
{
	OutError.Empty();
	if (!SoundQueue.IsValidIndex(SoundIndex) || !SoundQueue[SoundIndex].Sound
		|| IsOrdinarySoundWave(SoundQueue[SoundIndex].Sound))
	{
		return true;
	}

	if (!ObservedProceduralCompletionClass.IsValidIndex(SoundIndex))
	{
		ObservedProceduralCompletionClass.Init(-1, SoundQueue.Num());
	}

	const int8 NewClass = bReachedCaptureLimit ? 1 : 0;
	int8& ExistingClass = ObservedProceduralCompletionClass[SoundIndex];
	if (ExistingClass >= 0 && ExistingClass != NewClass)
	{
		OutError = FString::Printf(
			TEXT("Procedural source '%s' reported contradictory completion classes: %s then %s."),
			*SoundQueue[SoundIndex].SoundName,
			ExistingClass == 1 ? TEXT("capture ceiling") : TEXT("natural completion"),
			NewClass == 1 ? TEXT("capture ceiling") : TEXT("natural completion"));
		return false;
	}

	ExistingClass = NewClass;
	int64 ExpectedFiles = 0;
	for (int32 Index = 0; Index < SoundQueue.Num(); ++Index)
	{
		ExpectedFiles += CalculateRequestVariationCount(Index);
	}

	SessionExpectedFileCount = static_cast<int32>(FMath::Min<int64>(ExpectedFiles, static_cast<int64>(MAX_int32)));
	CurrentProgressInfo.TotalExpectedFiles = SessionExpectedFileCount;
	return true;
}

FPipelineVariationCaptureData FAudioExportPipeline::AppendPendingVariation(double StartTime, double EndTime, bool bIsLoop)
{
	const FAudioExportSoundEntry& CurrentEntry = SoundQueue[CurrentSoundIndex];

	FPipelineVariationCaptureData Data(SoundRequestIds[CurrentSoundIndex]);
	Data.StartTime = StartTime;
	Data.EndTime = EndTime;
	Data.SoundName = CurrentEntry.SoundName;
	Data.SourceAssetPath = CurrentEntry.SourceAssetPath;
	Data.RelativePath = CurrentEntry.RelativePath;
	Data.VariationIndex = CurrentVariationIndex;
	Data.SoundIndex = CurrentSoundIndex;
	Data.bIsLoop = bIsLoop;
	Data.TotalVariations = GetTotalVariations(bIsLoop);
	Data.bStaticSource = IsOrdinarySoundWave(CurrentSoundToPlay.Get());
	Data.ChannelLayout = FAudioExporterUtils::ResolveSoundWaveChannelLayout(Cast<USoundWave>(CurrentSoundToPlay.Get()));
	PendingVariations.Add(Data);
	return Data;
}

void FAudioExportPipeline::OnVariationFinished()
{
	if (bCancelRequested.Load() || HasDestinationAbortLatched() || !bIsExporting.Load() || !Renderer.IsValid())
	{
		return;
	}

	// A trailing variation callback can arrive once the index has advanced past the last sound: the
	// queue is not cleared until the (now deferred) FinalizeExport, so bIsExporting is still true.
	if (!SoundQueue.IsValidIndex(CurrentSoundIndex))
	{
		return;
	}

	FString CompletionClassError;
	if (!ApplyObservedProceduralCompletionClass(CurrentSoundIndex, Renderer->WasLastExportLooping(), CompletionClassError))
	{
		OnRenderError(CompletionClassError);
		return;
	}

	// 1. Record the metadata for this variation
	const FPipelineVariationCaptureData Data = AppendPendingVariation(
		Renderer->GetLastRenderStartTime(),
		Renderer->GetLastRenderEndTime(),
		Renderer->WasLastExportLooping());

	// 2. Decide what to do next
	const int32 TotalVariationsForCurrent = Data.TotalVariations;
	CurrentProgressInfo.TotalVariations = TotalVariationsForCurrent;

	if (CurrentVariationIndex + 1 < TotalVariationsForCurrent)
	{
		// More variations for the same sound
		CurrentVariationIndex++;
		StartNextVariation();
		return;
	}

	// No more variations for this sound, move to next
	ExportCurrentBatch(false);

	CurrentSoundIndex++;
	StartNextSound();
}

void FAudioExportPipeline::StartNextSound()
{
	// The lifecycle seam marks an active operation with the same IsExporting() state that
	// production callers poll. Keep unavailable sounds visible while that state is active;
	// once the pipeline is no longer exporting, normal teardown may remain quiet.
	const bool bOperationActive = IsExporting();
	if (!bOperationActive)
	{
		if (SoundQueue.IsValidIndex(CurrentSoundIndex) && !SoundQueue[CurrentSoundIndex].Sound)
		{
			const FString FailureReason = FString::Printf(
				TEXT("Sound entry '%s' was unavailable and could not be exported."),
				*SoundQueue[CurrentSoundIndex].SoundName);
			LogUnavailableSound(FString(__FUNCTION__), FailureReason, false);
		}
		return;
	}

	// 1. Check if we have reached the end of the queue
	if (CurrentSoundIndex >= SoundQueue.Num())
	{
		FinishExporting();
		return;
	}

	// Reset variation index for the new sound
	CurrentVariationIndex = 0;

	const FAudioExportSoundEntry& CurrentEntry = SoundQueue[CurrentSoundIndex];

	// 2. Validate the sound is loaded
	if (!CurrentEntry.Sound)
	{
		const FString FailureReason = FString::Printf(TEXT("Sound entry '%s' was unavailable and could not be exported."), *CurrentEntry.SoundName);
		LogUnavailableSound(FString(__FUNCTION__), FailureReason, bOperationActive);
		CurrentSoundToPlay = nullptr;
		FPipelineVariationCaptureData FailedEntry(SoundRequestIds[CurrentSoundIndex]);
		FailedEntry.SoundName = CurrentEntry.SoundName;
		FailedEntry.SourceAssetPath = CurrentEntry.SourceAssetPath;
		FailedEntry.RelativePath = CurrentEntry.RelativePath;
		FailedEntry.VariationIndex = 0;
		FailedEntry.SoundIndex = CurrentSoundIndex;
		FailedEntry.TotalVariations = 1;
		RecordVariationResult(FailedEntry, false, true, FString(), FString(), 0.0f, 0.0f, 0.0f, 0.0f, FailureReason);
		CurrentSoundIndex++;
		StartNextSound();
		return;
	}

	CurrentSoundToPlay = CurrentEntry.Sound;

	// 3. Update progress information
	CurrentProgressInfo.CurrentSoundName = CurrentEntry.SoundName;
	CurrentProgressInfo.CurrentSound = CurrentSoundIndex + 1; // 1-based index for UI

	// The renderer and the session denominator use the current asset's loop classification.
	const int8 ObservedCompletionClass = ObservedProceduralCompletionClass.IsValidIndex(CurrentSoundIndex)
		? ObservedProceduralCompletionClass[CurrentSoundIndex]
		: -1;
	CurrentProgressInfo.TotalVariations = IsOrdinarySoundWave(CurrentEntry.Sound)
		? 1
		: ObservedCompletionClass >= 0
			? FMath::Max(ObservedCompletionClass == 1 ? Config.LoopVariations : Config.OneShotVariations, 1)
			: FMath::Max(FMath::Max(Config.LoopVariations, Config.OneShotVariations), 1);

	// Lifecycle-only automation sessions intentionally have no world or renderer. Production
	// StartExporting rejects that state before reaching this point; preserve the seam's queue setup
	// behavior without manufacturing a renderer error.
	if (!PipelineWorld.IsValid() && !Renderer.IsValid())
	{
		return;
	}

	// Every source enters the same duplicate plus owned-submix recording path. Reuse the renderer
	// initialized at session start so the first sound does not trigger a second setup call.
	if (!Renderer.IsValid() && !EnsureRenderer())
	{
		OnRenderError(TEXT("Renderer was unavailable for the current sound."));
		return;
	}

	if (!Renderer->IsInBatchRecording() && !Renderer->StartBatchRecording())
	{
		OnRenderError(TEXT("Failed to start batch recording for the current sound."));
		return;
	}

	// 4. Kick off the first variation for this sound
	StartNextVariation();
}

void FAudioExportPipeline::StartNextVariation()
{
	if (bCancelRequested.Load() || HasDestinationAbortLatched() || !bIsExporting.Load())
	{
		return;
	}

	// 1. Update the progress UI information
	CurrentProgressInfo.CurrentVariation = CurrentVariationIndex + 1;
	OnExportProgressUpdated.Broadcast(CurrentProgressInfo);

	// 2. Begin the rendering process for this specific variation
	ProcessCurrentSound();
}


void FAudioExportPipeline::FinishExporting()
{
	if (bCancelRequested.Load())
	{
		return;
	}

	ExportCurrentBatch(true);
}

void FAudioExportPipeline::FinalizeExport()
{
	if (bCancelRequested.Load())
	{
		return;
	}

	const FString FullExportPath = FPaths::ConvertRelativePathToFull(ExportPath);

	// 1. Write the final summary report (log)
	WriteLogFilesToDisk();

	bIsExporting = false;
	bWaitingForFinalCleanup = false;

	// 2. Clean up the renderer before listeners are told the session is terminal.
	if (Renderer.IsValid())
	{
		Renderer->Shutdown();
		Renderer.Reset();
	}

	CurrentSoundToPlay = nullptr;
	PendingVariations.Empty();
	SoundQueue.Empty();
	CurrentProgressInfo = FAudioExportProgressInfo();
	const int32 FinalSuccessfulExportCount = SuccessfulExportCount;
	const int32 FinalFailedExportCount = FailedExportCount;

	// 3. Notify that we are done after the pipeline is internally inactive.
	const uint32 FinalizingGeneration = SessionGeneration.Load();
	BroadcastTerminalOnce(FullExportPath);

	// 4. Reset pipeline state for the next session.
	if (SessionGeneration.Load() == FinalizingGeneration)
	{
		ResetState();
	}

	// A run is a success only when it wrote at least one file with no failures. Zero
	// successfully-exported files is a failure/empty outcome even when nothing was counted
	// as an explicit failure; a mix of saves and failures is a partial completion. This must
	// agree with the progress window's terminal classification (OnExportFinishedProgress).
	if (FinalSuccessfulExportCount <= 0)
	{
		UE_LOG(LogAudioExporter, Warning, TEXT("Audio export finished without writing any files. Saved=%d Failed=%d."), FinalSuccessfulExportCount, FinalFailedExportCount);
	}
	else if (FinalFailedExportCount > 0)
	{
		UE_LOG(LogAudioExporter, Warning, TEXT("Audio export completed with failures. Saved=%d Failed=%d."), FinalSuccessfulExportCount, FinalFailedExportCount);
	}
	else
	{
		UE_LOG(LogAudioExporter, Log, TEXT("Audio export completed successfully. Saved=%d."), FinalSuccessfulExportCount);
	}
}

void FAudioExportPipeline::BroadcastTerminalOnce(const FString& FinishedExportPath)
{
	if (bTerminalEventBroadcast)
	{
		return;
	}

	bTerminalEventBroadcast = true;
	const uint32 TerminalGeneration = SessionGeneration.Load();

	FAudioExportResult Result;
	Result.RequestId = CurrentRequestId;
	Result.SessionId = CurrentSessionId;
	Result.bAcceptedStart = bAcceptedStart;
	Result.SuccessfulCount = SuccessfulExportCount;
	Result.FailedCount = FailedExportCount;
	Result.SkippedCount = SkippedExportCount;
	Result.bReportsRequested = bReportsRequested;
	Result.ReportsAttempted = ReportsAttempted;
	Result.ReportsWritten = ReportsWritten;
	Result.ReportsFailed = ReportsFailed;
	Result.ReportErrorMessage = ReportErrorMessage;

	if (bCancelRequested.Load())
	{
		Result.Outcome = EAudioExportOutcome::Cancelled;
	}
	else if (!bAcceptedStart)
	{
		Result.Outcome = EAudioExportOutcome::Rejected;
	}
	else if (SuccessfulExportCount > 0
		&& FailedExportCount == 0
		&& SkippedExportCount == 0
		&& ReportsFailed == 0
		&& TerminalErrorMessage.IsEmpty())
	{
		Result.Outcome = EAudioExportOutcome::Succeeded;
	}
	else if (SuccessfulExportCount > 0)
	{
		Result.Outcome = EAudioExportOutcome::PartialSuccess;
	}
	else
	{
		Result.Outcome = EAudioExportOutcome::Failed;
	}

	// One owner composes every terminal message. Audio failures and skips are always retained as
	// the primary detail; report failures are appended rather than replacing them. In particular,
	// zero successful files starts from the failure/empty wording and can never lead with "completed".
	Result.ErrorMessage = ComposeTerminalMessage(
		TerminalErrorMessage,
		FailedExportCount,
		SkippedExportCount,
		SkippedErrorMessage,
		SuccessfulExportCount,
		ReportsFailed,
		ReportErrorMessage);

	// The output path is only set when at least one file was actually written this session.
	if (SuccessfulExportCount > 0)
	{
		Result.ExportPath = FinishedExportPath;
	}

	LogTerminalResult(Result);

	// The structured result fires FIRST so any listener that binds directly to it (e.g. the
	// progress window's real-outcome hookup) observes the honest terminal outcome
	// before the legacy string delegate below reaches callers that still infer success/failure from
	// an empty path -- both delegates settle synchronously within this one call, so ordering here is
	// the only thing that determines which one a given listener sees first.
	OnExportResult.Broadcast(Result);
	if (SessionGeneration.Load() != TerminalGeneration)
	{
		// A listener started a new session while consuming the structured result. Do not deliver the
		// old legacy path into that new session's callback chain.
		return;
	}

	// The legacy string delegate keeps its exact semantics (empty path on cancel/failure) because
	// the editor layer and existing callers depend on it; the structured result above is the honest
	// completion contract for this session.
	const FString LegacyExportPath = SuccessfulExportCount > 0 ? FinishedExportPath : FString();
	OnExportFinished.Broadcast(LegacyExportPath);
}

void FAudioExportPipeline::FailStartup(const FString& ErrorMessage)
{
	UE_LOG(LogAudioExporter, Error, TEXT("%s: %s"), *FString(__FUNCTION__), *ErrorMessage);

	TerminalErrorMessage = ErrorMessage;
	bIsExporting = false;
	bWaitingForFinalCleanup = false;

	if (Renderer.IsValid())
	{
		Renderer->RequestCancel();
		Renderer->Shutdown();
		Renderer.Reset();
	}

	// Only remove a directory created by this failed start, and only when it is still empty.
	// Tree=false refuses to remove a nonempty directory, preserving pre-existing output and any
	// artifact another actor placed there while startup was settling.
	if (bSessionDirectoryCreatedByStart && !ExportPath.IsEmpty()
		&& !bAsyncSaveDispatched
		&& ActiveAsyncExports.Load() == 0
		&& IFileManager::Get().DirectoryExists(*ExportPath))
	{
		if (!IFileManager::Get().DeleteDirectory(*ExportPath, false, false))
		{
			UE_LOG(LogAudioExporter, Warning, TEXT("%s: Preserved startup directory '%s' because it was no longer empty."), *FString(__FUNCTION__), *ExportPath);
		}
	}

	// A restart failure after an asynchronous save was dispatched cannot finalize or clean up the
	// session yet. The save owns the directory until its game-thread result callback retires. That
	// callback enters FinalizeExport through the existing waiting-for-final-cleanup handoff,
	// preserving the WAV and the failure detail in one terminal result.
	if (ActiveAsyncExports.Load() > 0 && bAcceptedStart)
	{
		bWaitingForFinalCleanup = true;
		return;
	}

	const uint32 StartupFailureGeneration = SessionGeneration.Load();
	BroadcastTerminalOnce(TEXT(""));
	if (SessionGeneration.Load() == StartupFailureGeneration)
	{
		ResetState();
	}
}

void FAudioExportPipeline::MarkOutstandingVariationsSkipped()
{
	MarkVariationsSkippedFrom(
		0,
		0,
		TEXT("Save skipped because the export session is no longer active."));
}

void FAudioExportPipeline::MarkVariationsSkippedFrom(
	int32 StartSoundIndex,
	int32 StartVariationIndex,
	const FString& SkipDetail)
{
	const int32 FirstSoundIndex = FMath::Max(StartSoundIndex, 0);
	const int32 FirstVariationIndex = FMath::Max(StartVariationIndex, 0);
	for (int32 SoundIndex = FirstSoundIndex; SoundIndex < SoundQueue.Num(); ++SoundIndex)
	{
		const FAudioExportSoundEntry& Entry = SoundQueue[SoundIndex];
		const int32 ExpectedVariations = CalculateRequestVariationCount(SoundIndex);
		FAudioExportAssetLog* AssetLog = ExportLogs.Find(SoundRequestIds[SoundIndex]);
		if (!AssetLog)
		{
			continue;
		}

		const int32 FirstVariationForSound = SoundIndex == FirstSoundIndex ? FirstVariationIndex : 0;
		for (int32 VariationIndex = FirstVariationForSound; VariationIndex < ExpectedVariations; ++VariationIndex)
		{
			const FAudioExportLogEntry* ExistingEntry = AssetLog->Variations.FindByPredicate(
				[VariationIndex](const FAudioExportLogEntry& Candidate)
				{
					return Candidate.Variation == VariationIndex + 1;
				});
			if (ExistingEntry && ExistingEntry->Status != EAudioExportRowStatus::Pending)
			{
				continue;
			}

			FPipelineVariationCaptureData SkippedVariation(SoundRequestIds[SoundIndex]);
			SkippedVariation.SoundName = Entry.SoundName;
			SkippedVariation.SourceAssetPath = Entry.SourceAssetPath;
			SkippedVariation.RelativePath = Entry.RelativePath;
			SkippedVariation.VariationIndex = VariationIndex;
			SkippedVariation.SoundIndex = SoundIndex;
			SkippedVariation.TotalVariations = ExpectedVariations;
			// A skipped source never produced a completion event, so its loop class is unknown and must
			// not be inferred from an authored declaration.
			SkippedVariation.bIsLoop = false;
			SkippedVariation.bStaticSource = IsOrdinarySoundWave(Entry.Sound);
			RecordVariationResult(
				SkippedVariation,
				false,
				false,
				FString(),
				FString(),
				0.0f,
				0.0f,
				0.0f,
				0.0f,
				SkipDetail);
		}
	}
}

void FAudioExportPipeline::RecordVariationResult(
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
	float GainRequestedDb,
	bool bGainLimited,
	bool bMeasurementsValid)
{
	// A save vetoed by cancellation is retained as a terminal row. Stale async callbacks are fenced
	// by SessionGeneration before they reach this function, so cancellation can use the same row-
	// writing path for active, pending, and unstarted variations.
	if (!bCanSave)
	{
		const FString EffectiveSkipDetail = FailureReason.IsEmpty()
			? TEXT("Save skipped because the export session is no longer active.")
			: FailureReason;
		if (SkippedErrorMessage.IsEmpty())
		{
			SkippedErrorMessage = EffectiveSkipDetail;
		}
	}
	else if (bCancelRequested.Load())
	{
		return;
	}

	// Production capture data carries the immutable request identity. Test seams and recovery data
	// can arrive with only the queue position, though, so resolve that position back to the same
	// session-owned identity before giving up. Dropping a terminal row because its identity was not
	// copied into a capture record would make report counts diverge from the actual save attempts.
	FAudioExportAssetLog* AssetLog = ExportLogs.Find(VarData.RequestId);
	if (!AssetLog && SoundRequestIds.IsValidIndex(VarData.SoundIndex))
	{
		AssetLog = ExportLogs.Find(SoundRequestIds[VarData.SoundIndex]);
	}
	if (AssetLog)
	{
		// Rows can arrive out of order when background saves or failure recovery complete in a
		// different order than the renderer callbacks. Match an existing variation by its explicit
		// one-based identity and append a new row otherwise. Never pad with Pending rows: those are
		// not real outcomes and would be counted as fabricated failures by the report writer.
		FAudioExportLogEntry* LogEntry = AssetLog->Variations.FindByPredicate(
			[&VarData](const FAudioExportLogEntry& ExistingEntry)
			{
				return ExistingEntry.Variation == VarData.VariationIndex + 1;
			});
		if (!LogEntry)
		{
			LogEntry = &AssetLog->Variations.AddDefaulted_GetRef();
		}
		LogEntry->ExportedFileName = !FileName.IsEmpty()
			? FileName
			: FString::Printf(TEXT("%s_%d.wav"), *VarData.SoundName, VarData.VariationIndex + 1);
		LogEntry->SourceFilePath = VarData.SourceAssetPath;
		LogEntry->OutputFilePath = FilePath;
		LogEntry->Variation = VarData.VariationIndex + 1;
		LogEntry->TotalVariations = FMath::Max(VarData.TotalVariations, 1);
		LogEntry->bStaticSource = VarData.bStaticSource;
		LogEntry->Status = bSaved
			? (bGainLimited ? EAudioExportRowStatus::SucceededGainLimited : EAudioExportRowStatus::Succeeded)
			: (bCanSave ? EAudioExportRowStatus::Failed : EAudioExportRowStatus::Skipped);
		LogEntry->Details = bSaved
			? TEXT("WAV export completed")
			: (FailureReason.IsEmpty()
				? (bCanSave ? TEXT("Failed to save WAV output") : TEXT("Save skipped because the export session is no longer active."))
				: FailureReason);
		LogEntry->GainApplied = GainAppliedDb;
		LogEntry->GainRequested = GainRequestedDb;
		LogEntry->bGainLimited = bGainLimited;
		LogEntry->ExportPeak = PeakDb;
		LogEntry->ExportRMS = RMSDb;
		// A succeeded save always measured its output; every other row only carries levels when the
		// save actually measured before it was vetoed or failed. Zero defaults stay not-measured.
		LogEntry->bMeasurementsRecorded = bSaved || bMeasurementsValid;
		LogEntry->bLooping = VarData.bIsLoop;
		LogEntry->DurationSeconds = Duration;

		// Save callbacks and failure recovery can revisit the same variation identity. The report row
		// is the terminal source of truth, so derive aggregate counts from its final statuses instead
		// of counting callback frequency. This keeps the structured result and the TXT summary in
		// conservation with one another when a row is rewritten during recovery.
		SuccessfulExportCount = 0;
		FailedExportCount = 0;
		SkippedExportCount = 0;
		for (const TPair<FGuid, FAudioExportAssetLog>& AssetPair : ExportLogs)
		{
			for (const FAudioExportLogEntry& ExistingEntry : AssetPair.Value.Variations)
			{
				switch (ExistingEntry.Status)
				{
				case EAudioExportRowStatus::Succeeded:
				case EAudioExportRowStatus::SucceededGainLimited:
					++SuccessfulExportCount;
					break;
				case EAudioExportRowStatus::Failed:
					++FailedExportCount;
					break;
				case EAudioExportRowStatus::Skipped:
					++SkippedExportCount;
					break;
				case EAudioExportRowStatus::Pending:
				default:
					break;
				}
			}
		}
	}
	else if (bSaved)
	{
		// Accepted sessions normally always have a report row. Preserve direct callers' aggregate
		// result when a malformed source path prevents that row from being stored.
		++SuccessfulExportCount;
	}
	else if (bCanSave)
	{
		++FailedExportCount;
	}
	else
	{
		++SkippedExportCount;
	}

	CurrentProgressInfo.CurrentSoundName = VarData.SoundName;
	CurrentProgressInfo.CurrentSound = VarData.SoundIndex + 1;
	CurrentProgressInfo.TotalSounds = FMath::Max(CurrentProgressInfo.TotalSounds, SoundQueue.Num());
	CurrentProgressInfo.TotalExpectedFiles = SessionExpectedFileCount;
	CurrentProgressInfo.CurrentVariation = VarData.VariationIndex + 1;
	CurrentProgressInfo.TotalVariations = FMath::Max(VarData.TotalVariations, 1);
	CurrentProgressInfo.bHasResult = true;
	CurrentProgressInfo.bSucceeded = bSaved;
	CurrentProgressInfo.ResultMessage = bSaved
		? (bGainLimited
			? FString::Printf(TEXT("WAV export completed, gain limited (requested %+.2f dB, applied %+.2f dB)"), GainRequestedDb, GainAppliedDb)
			: TEXT("WAV export completed"))
		: (FailureReason.IsEmpty() ? TEXT("Failed to save WAV output") : FailureReason);
	CurrentProgressInfo.OutputFilePath = FilePath;
	CurrentProgressInfo.ExportedFiles = SuccessfulExportCount;
	CurrentProgressInfo.FailedFiles = FailedExportCount;
	OnExportProgressUpdated.Broadcast(CurrentProgressInfo);

	CurrentProgressInfo.bHasResult = false;
	CurrentProgressInfo.bSucceeded = false;
	CurrentProgressInfo.ResultMessage.Empty();
	CurrentProgressInfo.OutputFilePath.Empty();
}

bool FAudioExportPipeline::HasDestinationAbortLatched() const
{
	return DestinationUnusableFailureCount.Load() >= DestinationAbortFailureThreshold;
}

void FAudioExportPipeline::HandleDestinationUnusableFailure(
	const FPipelineVariationCaptureData& VarData,
	const FString& FailureReason)
{
	++DestinationUnusableFailureCount;
	if (!HasDestinationAbortLatched())
	{
		return;
	}

	UE_LOG(LogAudioExporter, Error,
	       TEXT("%s: Output destination unusable (%s). Stopping the batch; remaining variations are skipped."),
	       *FString(__FUNCTION__), *FailureReason);

	// Arm the same path fence the cancellation teardown uses, BEFORE any settlement: an async save
	// already past its dispatch snapshot observes this fence before or immediately after atomic
	// publication and removes any losing artifact, so a batch stopped by a dead destination cannot
	// keep publishing late WAVs to it.
	FAudioExporterUtils::MarkExportPathCancelled(ExportPath, EAudioExporterPathFenceCause::SessionFailure);

	// Every variation without a terminal row -- later variations of this sound and every later queue
	// entry -- becomes an explicit skipped row so the report conserves planned work. This runs while
	// SoundQueue and ExportLogs are still intact, i.e. before any teardown clears them.
	MarkVariationsSkippedFrom(VarData.SoundIndex, VarData.VariationIndex + 1, DestinationAbortSkipDetail);

	// The existing failure teardown owns the rest: FailStartup records TerminalErrorMessage, shuts
	// the renderer down, defers finalization while async saves are still in flight (their retirements
	// drive FinalizeExport through the existing waiting-for-final-cleanup handoff), and settles the
	// terminal outcome as Failed or PartialSuccess. bCancelRequested stays false throughout, so a
	// batch stopped by a dead destination is never reported as Cancelled.
	FailStartup(FString::Printf(
		TEXT("Export stopped early: the output destination failed repeatedly (%s)."),
		*FailureReason));
}

void FAudioExportPipeline::ExportCurrentBatch(bool bIsFinal)
{
	if (bCancelRequested.Load())
	{
		return;
	}

	// A latched destination-failure abort owns termination through the failure teardown; a flush
	// reaching this point afterwards must not re-dispatch or double-settle anything.
	if (HasDestinationAbortLatched())
	{
		return;
	}

	if (!Renderer.IsValid() || !Renderer->IsInBatchRecording())
	{
		if (bIsFinal)
		{
			FinalizeOrWaitForAsyncSaves();
		}
		return;
	}

	if (PendingVariations.IsEmpty())
	{
		DiscardCurrentBatch();
		if (bIsFinal)
		{
			FinalizeOrWaitForAsyncSaves();
		}
		return;
	}

	// 1. Stop the continuous recording and get the big buffer
	TArray<float> BigBuffer;
	int32 SampleRate = 0;
	int32 NumChannels = 0;
	Renderer->StopBatchRecording(BigBuffer, NumChannels, SampleRate);

	const double MeasuredBatchDuration = Renderer->GetBatchEndTime() - Renderer->GetBatchStartTime();
	const double ActualBatchDuration = (SampleRate > 0 && NumChannels > 0)
		                                   ? (static_cast<double>(BigBuffer.Num()) / (SampleRate * NumChannels))
		                                   : 0.0;
	const double TimeCorrectionFactor = (MeasuredBatchDuration > 0.0)
		                                    ? (ActualBatchDuration / MeasuredBatchDuration)
		                                    : 1.0;

	// This fires per-batch (as frequent as per-variation with a 1-variation config) -- Verbose, not Log.
	UE_LOG(LogAudioExporter, Verbose, TEXT("Batch complete. Measured: %.3fs, Actual: %.3fs, Correction Factor: %.6f"),
	       MeasuredBatchDuration, ActualBatchDuration, TimeCorrectionFactor);
	UE_LOG(LogAudioExporter, Verbose, TEXT("Processing %d variations from continuous recording..."),
	       PendingVariations.Num());

	// 2. Capture and clear the pending variations for this batch
	TArray<FPipelineVariationCaptureData> Variations = MoveTemp(PendingVariations);
	PendingVariations.Empty();

	if (bIsFinal)
	{
		bWaitingForFinalCleanup = true;
	}

	// 3. Process all captured variations
	for (int32 i = 0; i < Variations.Num(); ++i)
	{
		if (HasDestinationAbortLatched())
		{
			// The abort latched mid-flush: HandleDestinationUnusableFailure has already written
			// explicit skip rows for every unattempted variation, including the rest of this
			// captured batch, so stop dispatching instead of repeating the doomed write.
			break;
		}

		const FPipelineVariationCaptureData& VarData = Variations[i];

		TArray<float> VariationBuffer = FAudioExporterUtils::ExtractVariationBuffer(
			BigBuffer, SampleRate, NumChannels, VarData.StartTime, VarData.EndTime, TimeCorrectionFactor,
			VarData.bIsLoop);

		ScheduleVariationSave(VarData, MoveTemp(VariationBuffer), SampleRate, NumChannels);
	}

	// The final batch finalizes as soon as nothing is in flight; FinalizeExport itself ignores a
	// cancel request, so a single branch covers the empty-batch and in-batch cases alike. A latched
	// destination abort has already settled its own termination through the failure teardown.
	if (bIsFinal && ActiveAsyncExports.Load() == 0 && !bCancelRequested.Load() && !HasDestinationAbortLatched())
	{
		FinalizeExport();
		return;
	}

	// 5. Restart recording for the next rendered sound if not finishing. A restart failure mid-batch is a
	// fatal path like the startup one: leaving it unchecked would drop every later variation without
	// any row or completion event.
	const bool bNextSoundNeedsRenderer = !bIsFinal
		&& SoundQueue.IsValidIndex(CurrentSoundIndex + 1)
		&& SoundQueue[CurrentSoundIndex + 1].Sound != nullptr;
	if (bNextSoundNeedsRenderer && bIsExporting.Load())
	{
		if (!Renderer->StartBatchRecording())
		{
			MarkVariationsSkippedFrom(
				CurrentSoundIndex + 1,
				0,
				TEXT("Skipped because batch recording could not restart after the completed batch."));
			FailStartup(TEXT("Failed to restart batch recording for the next batch."));
			return;
		}
	}
}

bool FAudioExportPipeline::ScheduleVariationSave(
	const FPipelineVariationCaptureData& VarData,
	TArray<float> VariationBuffer,
	int32 SampleRate,
	int32 NumChannels)
{
	const TSharedRef<FAudioExportPipeline> PipelineLifetime = AsShared();

	if (VariationBuffer.IsEmpty())
	{
		const FString FailureReason = FString::Printf(TEXT("Failed to extract variation %d due to an invalid sample range."), VarData.VariationIndex + 1);
		UE_LOG(LogAudioExporter, Warning, TEXT("%s: %s Asset='%s'."),
		       *FString(__FUNCTION__), *FailureReason, *VarData.SoundName);
		RecordVariationResult(VarData, false, true, FString(), FString(), 0.0f, 0.0f, 0.0f, 0.0f, FailureReason);
		return false;
	}

	const float Duration = (SampleRate > 0 && NumChannels > 0)
		? static_cast<float>(VariationBuffer.Num()) / (SampleRate * NumChannels)
		: 0.0f;

	// Increment atomic counter before starting async task
	ActiveAsyncExports++;
	++CurrentSessionAsyncExports;

	// Optional: Apply pre-save processing (must be done on Game Thread if it might access UObjects)
	OnPreSaveRenderedBuffer(VariationBuffer, SampleRate, VarData.bIsLoop);

	const uint32 CapturedGeneration = SessionGeneration.Load();

	// bForceAsyncSaveForTesting lets automation exercise the async branch even though every test
	// runs with FApp::IsUnattended() == true; unset,
	// production behavior is unchanged.
	const bool bUseSynchronousSave = bForceAsyncSaveForTesting.IsSet()
		? !bForceAsyncSaveForTesting.GetValue()
		: (FApp::IsUnattended() || !FApp::CanEverRender());

	if (bUseSynchronousSave)
	{
		float GainAppliedDb = 0.0f;
		float GainRequestedDb = 0.0f;
		bool bGainLimited = false;
		float PeakDb = 0.0f;
		float RMSDb = 0.0f;
		FString FilePath;
		FString FailureReason;
		bool bDestinationUnusable = false;
		bool bMeasurementsTaken = false;
		const bool bCanSave = CapturedGeneration == SessionGeneration.Load()
			&& bIsExporting.Load()
			&& !bCancelRequested.Load()
			&& !bShouldDeleteFolderOnCancel.Load();

		bool bSaved = false;
		if (bHasSimulatedSaveResultForTesting)
		{
			// Automation seam: report the injected outcome without touching the file system.
			bSaved = bSimulatedSaveSuccessForTesting;
			bDestinationUnusable = bSimulatedSaveDestinationUnusableForTesting;
			FailureReason = SimulatedSaveFailureReasonForTesting;
		}
		else
		{
			bSaved = FAudioExporterUtils::ProcessAndSaveVariation(
				VariationBuffer, VarData.SoundName, VarData.RelativePath,
				VarData.VariationIndex,
				ExportPath,
				SampleRate, NumChannels, Config.bNormalize, bCanSave, GainAppliedDb, PeakDb, RMSDb, FilePath, &FailureReason,
				VarData.RequestId, &bDestinationUnusable, VarData.SourceAssetPath, &GainRequestedDb, &bGainLimited, &bMeasurementsTaken,
				VarData.ChannelLayout);
		}

		const FString FileName = FPaths::GetCleanFilename(FilePath);
		if (!bCanSave)
		{
					UE_LOG(LogAudioExporter, Display, TEXT("%s: Skipped synchronous save for '%s' because the export session was canceled."),
		       *FString(__FUNCTION__), *VarData.SoundName);
		}
		else if (bSaved)
		{
			UE_LOG(LogAudioExporter, Verbose, TEXT("[Export] Saved '%s' to %s (Synchronous)"),
			       *VarData.SoundName, *FilePath);
		}
		else
		{
			UE_LOG(LogAudioExporter, Warning, TEXT("%s: Failed to save WAV file '%s': %s"),
			       *FString(__FUNCTION__), *FilePath, *FailureReason);
		}

		RecordVariationResult(VarData, bSaved, bCanSave, FileName, FilePath, PeakDb, RMSDb, Duration, GainAppliedDb, FailureReason, GainRequestedDb, bGainLimited, bMeasurementsTaken);

		--ActiveAsyncExports;
		--CurrentSessionAsyncExports;

		// Latch handling runs after this save retires from ActiveAsyncExports so a latched abort
		// observes the true in-flight count: with nothing else outstanding, the failure teardown
		// finalizes immediately instead of deferring to a retirement that would never come on the
		// synchronous path.
		if (!bSaved && bCanSave && bDestinationUnusable)
		{
			HandleDestinationUnusableFailure(VarData, FailureReason);
		}

		return true;
	}

	bAsyncSaveDispatched = true;
	// The seam state is snapshotted at dispatch time: the background task must read copies, not
	// members the game thread may mutate between dispatch and execution.
	const bool bDispatchedInjectSaveResult = bHasSimulatedSaveResultForTesting;
	const bool bDispatchedInjectedSaveSuccess = bSimulatedSaveSuccessForTesting;
	const bool bDispatchedInjectedDestinationUnusable = bSimulatedSaveDestinationUnusableForTesting;
	const FString DispatchedInjectedFailureReason = SimulatedSaveFailureReasonForTesting;
	AsyncTask(ENamedThreads::AnyBackgroundHiPriTask,
	          [this, PipelineLifetime, VariationBuffer = MoveTemp(VariationBuffer), VarData, Duration, SampleRate, NumChannels,
		          ExportPathCopy = ExportPath,
		          bNormalize = Config.bNormalize,
		          CapturedGeneration,
		          bDispatchedInjectSaveResult, bDispatchedInjectedSaveSuccess, bDispatchedInjectedDestinationUnusable, DispatchedInjectedFailureReason]() mutable
	          {
		          if (CapturedGeneration != SessionGeneration.Load() || bCancelRequested.Load())
		          {
			          int32 RemainingTasks = --ActiveAsyncExports;
			          if (RemainingTasks == 0)
			          {
				          AsyncTask(ENamedThreads::GameThread, [this, PipelineLifetime]()
				          {
					          RetireStaleAsyncExport();
				          });
			          }
			          return;
		          }

		          float GainAppliedDb = 0.0f;
		          float GainRequestedDb = 0.0f;
		          bool bGainLimited = false;
		          float PeakDb = 0.0f;
		          float RMSDb = 0.0f;
		          FString FilePath;
		          FString FailureReason;
		          bool bDestinationUnusable = false;
		          bool bMeasurementsTaken = false;
		          // bIsExporting is deliberately not consulted here. The final batch's saves are
		          // still in flight when FinalizeExport() clears the flag, so reading it on the
		          // worker would let a save veto itself and drop the file with no failure row.
		          // The generation and cancel flags already fence work from superseded sessions.
		          const bool bCanSave = CapturedGeneration == SessionGeneration.Load()
			          && !bCancelRequested.Load()
			          && !bShouldDeleteFolderOnCancel.Load();

		          bool bSaved = false;
		          if (bDispatchedInjectSaveResult)
		          {
			          // Automation seam: report the injected outcome without touching the file system.
			          bSaved = bDispatchedInjectedSaveSuccess;
			          bDestinationUnusable = bDispatchedInjectedDestinationUnusable;
			          FailureReason = DispatchedInjectedFailureReason;
		          }
		          else
		          {
			          bSaved = FAudioExporterUtils::ProcessAndSaveVariation(
				          VariationBuffer, VarData.SoundName, VarData.RelativePath,
				          VarData.VariationIndex,
				          ExportPathCopy,
				          SampleRate, NumChannels, bNormalize, bCanSave, GainAppliedDb, PeakDb, RMSDb, FilePath, &FailureReason,
				          VarData.RequestId, &bDestinationUnusable, VarData.SourceAssetPath, &GainRequestedDb, &bGainLimited, &bMeasurementsTaken,
				          VarData.ChannelLayout);
		          }

		          const FString FileName = FPaths::GetCleanFilename(FilePath);

		          // Return to Game Thread for logging and UI updates
		          AsyncTask(ENamedThreads::GameThread,
	                    [this, PipelineLifetime, bSaved, bCanSave, bDestinationUnusable, VarData, FileName, FilePath, PeakDb, RMSDb, Duration, GainAppliedDb, GainRequestedDb, bGainLimited, FailureReason, CapturedGeneration, bMeasurementsTaken]()
		                    {
			                    if (CapturedGeneration != SessionGeneration.Load())
			                    {
				                    if (--ActiveAsyncExports == 0)
				                    {
					                    RetireStaleAsyncExport();
				                    }
				                    return;
			                    }

			                    if (!bCanSave)
			                    {
				                    UE_LOG(LogAudioExporter, Display, TEXT("%s: Skipped async save for '%s' because the export session was canceled."),
				                           *FString(__FUNCTION__), *VarData.SoundName);
			                    }
			                    else if (bSaved)
			                    {
				                    UE_LOG(LogAudioExporter, Verbose, TEXT("[Export] Saved '%s' to %s (Async)"),
				                           *VarData.SoundName, *FilePath);
			                    }
			                    else
			                    {
				                    UE_LOG(LogAudioExporter, Warning, TEXT("%s: Failed to save WAV file '%s': %s"),
				                           *FString(__FUNCTION__), *FilePath, *FailureReason);
			                    }

			                    RecordVariationResult(VarData, bSaved, bCanSave, FileName, FilePath, PeakDb, RMSDb, Duration, GainAppliedDb, FailureReason, GainRequestedDb, bGainLimited, bMeasurementsTaken);
			                    --CurrentSessionAsyncExports;

			                    // A background save discovered that the destination itself is broken. Runs
			                    // before this save retires below so the failure teardown's deferred-finalize
			                    // handoff sees it still in flight and the last retirement finalizes.
			                    if (!bSaved && bCanSave && bDestinationUnusable)
			                    {
				                    HandleDestinationUnusableFailure(VarData, FailureReason);
			                    }

			                    // Decrement counter and check for finalization
			                    if (--ActiveAsyncExports == 0)
			                    {
				                    // An earlier cancelled session may still owe a folder delete that was
				                    // deferred past this session's saves.
				                    FlushDeferredCancelDelete();

				                    if (CapturedGeneration == SessionGeneration.Load() && bWaitingForFinalCleanup && !bCancelRequested.Load())
				                    {
					                    FinalizeExport();
				                    }
				                    else if (!bIsExporting.Load() && CapturedGeneration == SessionGeneration.Load())
				                    {
					                    ResetState();
				                    }
			                    }
		                    });
	          });

	return true;
}

void FAudioExportPipeline::FinalizeOrWaitForAsyncSaves()
{
	if (ActiveAsyncExports.Load() > 0)
	{
		bWaitingForFinalCleanup = true;
		return;
	}

	FinalizeExport();
}

void FAudioExportPipeline::DiscardCurrentBatch()
{
	if (Renderer.IsValid() && Renderer->IsInBatchRecording())
	{
		TArray<float> DummyBuffer;
		int32 DummyChannels = 0;
		int32 DummySampleRate = 0;
		Renderer->StopBatchRecording(DummyBuffer, DummyChannels, DummySampleRate);
	}
	PendingVariations.Empty();
}

void FAudioExportPipeline::WriteLogFilesToDisk()
{
	bReportsRequested = Config.bExportTextLog;
	ReportsAttempted = 0;
	ReportsWritten = 0;
	ReportsFailed = 0;
	ReportErrorMessage.Empty();

	if (!Config.bExportTextLog)
	{
		return;
	}

	// Lite publishes the TXT report only; the CSV report is a Pro feature.
	const FString TextPath = ExportPath / TEXT("AudioExporterTextLog.txt");
	const FString Timestamp = ExportTimestamp.ToString(TEXT("%Y-%m-%d %H:%M:%S"));

	// Create a summary of the settings used for this session. Labels share the report's label
	// width constant (%-28s) so every column in the report aligns.
	FString SettingsSummary;
	SettingsSummary += FString::Printf(TEXT("%-28s: %d\n"), TEXT("One-Shot Variations"), Config.OneShotVariations);
	SettingsSummary += FString::Printf(TEXT("%-28s: %d\n"), TEXT("Loop Variations"), Config.LoopVariations);
	SettingsSummary += FString::Printf(TEXT("%-28s: %d seconds\n"), TEXT("Loop Max Length"), Config.MaxLengthSeconds);
	SettingsSummary += FString::Printf(TEXT("%-28s: %s\n"), TEXT("Normalize"), Config.bNormalize ? TEXT("Yes") : TEXT("No"));
	SettingsSummary += FString::Printf(TEXT("%-28s: %s\n"), TEXT("Export Text Log"), Config.bExportTextLog ? TEXT("Yes") : TEXT("No"));

	// Convert log map to array for the logger
	TArray<FAudioExportAssetLog> LogArray;
	LogArray.Reserve(ExportLogs.Num());
	for (const auto& Pair : ExportLogs)
	{
		LogArray.Add(Pair.Value);
	}

	// The footer names the session FOLDER (ExportPath), not the TXT file's own path.
	TArray<FString> ReportErrors;
	++ReportsAttempted;
	FString WriteError;
	if (AudioExporterLogger::WriteTextLogFile(LogArray, TextPath, Timestamp, SettingsSummary, ExportPath, &WriteError))
	{
		++ReportsWritten;
	}
	else
	{
		++ReportsFailed;
		ReportErrors.Add(WriteError);
	}

	for (const FString& Error : ReportErrors)
	{
		if (!ReportErrorMessage.IsEmpty())
		{
			ReportErrorMessage += TEXT(" ");
		}
		ReportErrorMessage += Error;
	}
}

void FAudioExportPipeline::ResetState()
{
	CurrentSoundIndex = 0;
	CurrentVariationIndex = 0;
	CurrentSoundToPlay = nullptr;
	PendingVariations.Empty();
	SoundQueue.Empty();
	SoundRequestIds.Empty();
	ObservedProceduralCompletionClass.Empty();

	// ActiveAsyncExports is intentionally left alone: it is owned by the task lifecycle, not by
	// session state. Zeroing it here made a restarted session's counter drift negative as the
	// previous session's stale tasks retired, so the finalize gate never fired again.
	bIsExporting = false;
	bWaitingForFinalCleanup = false;
	bAsyncSaveDispatched = false;
	bShouldDeleteFolderOnCancel = false;
	ExportPath.Empty();
	SuccessfulExportCount = 0;
	FailedExportCount = 0;
	SkippedExportCount = 0;
	bReportsRequested = false;
	ReportsAttempted = 0;
	ReportsWritten = 0;
	ReportsFailed = 0;
	ReportErrorMessage.Empty();
	bAcceptedStart = false;
	TerminalErrorMessage.Empty();
	SkippedErrorMessage.Empty();
	SessionExpectedFileCount = 0;
	CurrentSessionAsyncExports = 0;
	bSessionDirectoryCreatedByStart = false;
	CurrentRequestId = FGuid();
	CurrentSessionId = FGuid();

	// Session-scoped abort latch and the automation save seam both reset with the session; neither
	// may leak into a restarted export.
	DestinationUnusableFailureCount = 0;
	bHasSimulatedSaveResultForTesting = false;
	bSimulatedSaveSuccessForTesting = false;
	bSimulatedSaveDestinationUnusableForTesting = false;
	SimulatedSaveFailureReasonForTesting.Empty();

	CurrentProgressInfo = FAudioExportProgressInfo();
	CurrentProgressInfo.TotalSounds = 0;
	CurrentProgressInfo.CurrentSound = 0;
	CurrentProgressInfo.TotalExpectedFiles = 0;
}

#if WITH_DEV_AUTOMATION_TESTS
void FAudioExportPipeline::BeginLifecycleTestSession(const FString& InExportPath)
{
	ResetState();
	Config = FAudioExporterConfig();
	Config.bExportTextLog = false;
	ExportTimestamp = FDateTime::Now();
	ExportPath = InExportPath.IsEmpty()
		? FPaths::ProjectSavedDir() / TEXT("Automation") / TEXT("Lifecycle") / TEXT("AudioExporter")
		: InExportPath;
	++SessionGeneration;
	bCancelRequested = false;
	bTerminalEventBroadcast = false;
	bIsExporting = true;
	bAcceptedStart = true;
	CurrentRequestId = FGuid::NewGuid();
	CurrentSessionId = FGuid::NewGuid();
	CurrentProgressInfo.RequestId = CurrentRequestId;
	CurrentProgressInfo.SessionId = CurrentSessionId;
}

void FAudioExportPipeline::BeginLifecycleTestSession(
	const TArray<FAudioExportSoundEntry>& InSounds,
	const FAudioExporterConfig& InConfig,
	const FString& InExportPath)
{
	BeginLifecycleTestSession(InExportPath);
	Config = InConfig;
	Config.Sanitize();
	SoundQueue = InSounds;
	CurrentProgressInfo.TotalSounds = SoundQueue.Num();
	ObservedProceduralCompletionClass.Init(-1, SoundQueue.Num());
	SessionExpectedFileCount = CalculateSessionExpectedFileCount();
	CurrentProgressInfo.TotalExpectedFiles = SessionExpectedFileCount;
	ExportLogs.Empty();
	ExportLogs.Reserve(SoundQueue.Num());
	SoundRequestIds.Reset();
	SoundRequestIds.Reserve(SoundQueue.Num());
	for (const FAudioExportSoundEntry& Entry : SoundQueue)
	{
		const FGuid RequestId = FGuid::NewGuid();
		SoundRequestIds.Add(RequestId);
		FAudioExportAssetLog AssetLog;
		AssetLog.SourceAssetPath = Entry.SourceAssetPath;
		AssetLog.RelativePath = Entry.RelativePath;
		ExportLogs.Add(RequestId, MoveTemp(AssetLog));
	}

	if (!ExportPath.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*ExportPath, true);
		bSessionDirectoryCreatedByStart = true;
	}
}

void FAudioExportPipeline::CompleteFinalEmptyBatchForTesting()
{
	ExportCurrentBatch(true);
}

TArray<FAudioExportAssetLog> FAudioExportPipeline::GetExportLogsForTesting() const
{
	TArray<FAudioExportAssetLog> Logs;
	Logs.Reserve(ExportLogs.Num());
	for (const auto& Pair : ExportLogs)
	{
		Logs.Add(Pair.Value);
	}
	return Logs;
}

void FAudioExportPipeline::SetSimulatedSaveResultForTesting(bool bInSaved, bool bInDestinationUnusable, const FString& InFailureReason)
{
	bHasSimulatedSaveResultForTesting = true;
	bSimulatedSaveSuccessForTesting = bInSaved;
	bSimulatedSaveDestinationUnusableForTesting = bInDestinationUnusable;
	SimulatedSaveFailureReasonForTesting = InFailureReason;
}

void FAudioExportPipeline::ClearSimulatedSaveResultForTesting()
{
	bHasSimulatedSaveResultForTesting = false;
	bSimulatedSaveSuccessForTesting = false;
	bSimulatedSaveDestinationUnusableForTesting = false;
	SimulatedSaveFailureReasonForTesting.Empty();
}

void FAudioExportPipeline::SimulateVariationFinishedForTesting()
{
	SimulateVariationFinishedForTestingImpl(TOptional<bool>());
}

void FAudioExportPipeline::SimulateProceduralCompletionForTesting(bool bReachedCaptureLimit)
{
	SimulateVariationFinishedForTestingImpl(TOptional<bool>(bReachedCaptureLimit));
}

void FAudioExportPipeline::SimulateVariationFinishedForTestingImpl(TOptional<bool> bReachedCaptureLimit)
{
	if (bCancelRequested.Load() || HasDestinationAbortLatched() || !bIsExporting.Load() || !SoundQueue.IsValidIndex(CurrentSoundIndex))
	{
		return;
	}

	// Mirrors StartNextSound()'s assignment: GetTotalVariations()/ordinary SoundWave detection (via
	// AppendPendingVariation below) classify the CURRENT sound, and BeginLifecycleTestSession never
	// sets this for the seam-only lifecycle path.
	CurrentSoundToPlay = SoundQueue[CurrentSoundIndex].Sound;

	// Fabricate a short stereo tone in place of a real renderer capture. Headless automation cannot
	// drive FAudioRendererRealtime's play-state/watchdog callbacks (no audio device progresses them
	// under -nullrhi), so there is no real buffer or renderer-derived timing to read here.
	// This seam is only for lifecycle bookkeeping. Procedural completion cannot be represented here
	// because no renderer event was observed, so it deliberately never infers that class from the
	// authored loop flag.
	constexpr int32 SampleRate = 48000;
	constexpr int32 NumChannels = 2;
	constexpr float DurationSeconds = 0.25f;
	constexpr float Frequency = 440.0f;
	constexpr float Amplitude = 0.5f;
	const int32 NumFrames = FMath::Max(1, FMath::RoundToInt(SampleRate * DurationSeconds));

	TArray<float> VariationBuffer;
	VariationBuffer.SetNumUninitialized(NumFrames * NumChannels);
	for (int32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
	{
		const float Sample = Amplitude * FMath::Sin(2.0f * PI * Frequency * static_cast<float>(FrameIndex) / static_cast<float>(SampleRate));
		for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
		{
			VariationBuffer[(FrameIndex * NumChannels) + ChannelIndex] = Sample;
		}
	}

	// Mirrors OnVariationFinished's bookkeeping through the SAME shared helper: the
	// metadata lands in PendingVariations exactly as a real completed render would leave it, so a
	// FlushPendingVariations call later in the same test (e.g. from a subsequent OnRenderError) sees
	// truthful state. This seam then immediately extracts/saves that one entry itself -- there is no
	// continuous renderer buffer to batch-extract a fabricated tone from later -- and pops it back out
	// so the already-persisted variation is not left sitting in PendingVariations as a stale, already-
	// saved "pending" entry that a later flush would try to salvage a second time.
	const int8 ExistingObservedClass = ObservedProceduralCompletionClass.IsValidIndex(CurrentSoundIndex)
		? ObservedProceduralCompletionClass[CurrentSoundIndex]
		: -1;
	const bool bIsLoop = bReachedCaptureLimit.IsSet()
		? bReachedCaptureLimit.GetValue()
		: ExistingObservedClass == 1;
	if (bReachedCaptureLimit.IsSet())
	{
		FString CompletionClassError;
		if (!ApplyObservedProceduralCompletionClass(CurrentSoundIndex, bIsLoop, CompletionClassError))
		{
			OnRenderError(CompletionClassError);
			return;
		}
	}
	const FPipelineVariationCaptureData Data = AppendPendingVariation(0.0, DurationSeconds, bIsLoop);
	PendingVariations.Pop();

	// Mirrors OnVariationFinished's bookkeeping (CurrentProgressInfo.TotalVariations, the
	// CurrentVariationIndex/CurrentSoundIndex advance, and the StartNextSound() cascade once a sound's
	// last variation completes), but calls ScheduleVariationSave directly for the fabricated buffer
	// instead of leaving it for a later Renderer-driven batch extraction. This is "the narrowest point
	// that skips only the renderer extraction, not any accounting": the real ScheduleVariationSave
	// (save scheduling, ActiveAsyncExports, RecordVariationResult) and the real
	// FinishExporting()/ExportCurrentBatch(true)/FinalizeOrWaitForAsyncSaves cascade both still run.
	const int32 TotalVariationsForCurrent = Data.TotalVariations;
	CurrentProgressInfo.TotalVariations = TotalVariationsForCurrent;

	ScheduleVariationSave(Data, MoveTemp(VariationBuffer), SampleRate, NumChannels);

	if (CurrentVariationIndex + 1 < TotalVariationsForCurrent)
	{
		// More variations for the same sound. Real playback would continue automatically via
		// StartNextVariation()/Render(); under this seam the caller must invoke
		// SimulateVariationFinishedForTesting() again for the next variation index.
		CurrentVariationIndex++;
		return;
	}

	// No more variations for this sound: advance to the next one exactly like OnVariationFinished does
	// after its ExportCurrentBatch(false) flush. With a single-sound queue this reaches
	// StartNextSound() -> FinishExporting() -> ExportCurrentBatch(true) in the same call, which finds
	// PendingVariations empty (this seam never touches it) and defers to FinalizeOrWaitForAsyncSaves()
	// -- the exact "final call finds nothing pending while the last save is still in flight" path.
	CurrentSoundIndex++;
	StartNextSound();
}
#endif

int32 FAudioExportPipeline::GetTotalVariations(bool bIsLoop) const
{
	// An ordinary (non-procedural) SoundWave produces exactly one exported file per asset,
	// regardless of the configured variation counts -- the source is deterministic, so fabricating
	// extra "variations" of the same identical audio would be dishonest.
	if (IsOrdinarySoundWave(CurrentSoundToPlay.Get()))
	{
		return 1;
	}
	return bIsLoop ? Config.LoopVariations : Config.OneShotVariations;
}

int32 FAudioExportPipeline::CalculateSessionExpectedFileCount() const
{
	int32 ExpectedFiles = 0;
	for (const FAudioExportSoundEntry& Entry : SoundQueue)
	{
		ExpectedFiles += CalculateExpectedVariationCount(Entry, Config);
	}

	return ExpectedFiles;
}

void FAudioExportPipeline::ProcessCurrentSound()
{
	const bool bOperationActive = bIsExporting.Load() && !bCancelRequested.Load();
	if (!bOperationActive)
	{
		if (!CurrentSoundToPlay)
		{
			const FString ErrorMessage = TEXT("Export failed because the current sound was unavailable.");
			LogUnavailableSound(FString(__FUNCTION__), ErrorMessage, false);
		}
		return;
	}

	// 1. Early check: ensure we have a sound ready to play
	if (!CurrentSoundToPlay)
	{
		const FString ErrorMessage = TEXT("Export failed because the current sound was unavailable.");
		UE_LOG(LogAudioExporter, Error, TEXT("%s: %s"), *FString(__FUNCTION__), *ErrorMessage);
		FailStartup(ErrorMessage);
		return;
	}

	const USoundWave* CurrentSoundWave = Cast<USoundWave>(CurrentSoundToPlay.Get());
	if (CurrentSoundWave && (!FMath::IsFinite(CurrentSoundWave->Volume) || CurrentSoundWave->Volume < 0.0f))
	{
		OnRenderError(TEXT("Sound render rejected because authored linear Volume must be finite and non-negative."));
		return;
	}

	// 2. Determine the maximum allowed duration for this recording.
	const float MaxDuration = static_cast<float>(Config.MaxLengthSeconds);

	UE_LOG(LogAudioExporter, Verbose, TEXT("Requesting render for sound '%s' (variation %d)"),
	       *CurrentSoundToPlay->GetName(),
	       CurrentVariationIndex + 1);

	// 3. Start the rendering process
	if (!Renderer.IsValid())
	{
		UE_LOG(LogAudioExporter, Error, TEXT("%s: Renderer was invalid during sound processing."), *FString(__FUNCTION__));
		OnRenderError(TEXT("Renderer was invalid during sound processing."));
		return;
	}

	if (!Renderer->Render(CurrentSoundToPlay, MaxDuration))
	{
		OnRenderError(FString::Printf(TEXT("%s: Failed to start render."), *FString(__FUNCTION__)));
	}
}
