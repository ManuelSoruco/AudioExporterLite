// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "AudioExporterRuntimeSubsystem.h"

#include "Misc/ScopeLock.h"
#include "Utilities/AudioExporterLogger.h"
#include "Utilities/AudioExporterUtils.h"

// ---------------------------------------------------------------------------
// UEngineSubsystem interface
// ---------------------------------------------------------------------------

void UAudioExporterRuntimeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

void UAudioExporterRuntimeSubsystem::Deinitialize()
{
	if (Pipeline.IsValid())
	{
		if (Pipeline->IsExporting())
		{
			FAudioExporterUtils::MarkExportPathCancelled(Pipeline->GetExportPath());
			Pipeline->CancelExporting(false);
		}

		Pipeline.Reset();
	}

	PipelineWorld.Reset();
	{
		FScopeLock PendingCallbacksLock(&PendingResultCallbacksMutex);
		PendingResultCallbacks.Empty();
	}
	ActiveRequestToken = 0;

	Super::Deinitialize();
}

// ---------------------------------------------------------------------------
// Core API
// ---------------------------------------------------------------------------

bool UAudioExporterRuntimeSubsystem::StartExport(UWorld* World,
                                                 const TArray<FAudioExportSoundEntry>& Sounds,
                                                 const FAudioExporterConfig& Config,
                                                 const FString& OutputPath,
                                                 FAudioExportRequestToken RequestToken,
                                                 const FGuid& RequestId)
{
	const FGuid EffectiveRequestId = RequestId.IsValid() ? RequestId : FGuid::NewGuid();
	// Reject before anything is registered against the request. Reporting Pipeline->IsExporting()
	// for a concurrent caller would hand it the *first* session's state, and the caller would then
	// treat that session's completion (and its export path) as its own.
	if (Pipeline.IsValid() && Pipeline->IsExporting())
	{
		UE_LOG(LogAudioExporter, Warning, TEXT("%s: An export session is already active; this request was rejected."), *FString(__FUNCTION__));
		BroadcastRejectedResult(TEXT("An export session is already active; this request was rejected."), RequestToken, EffectiveRequestId);
		return false;
	}

	if (!EnsurePipeline(World))
	{
		UE_LOG(LogAudioExporter, Error, TEXT("%s: Runtime export pipeline was not available."), *FString(__FUNCTION__));
		BroadcastRejectedResult(TEXT("Runtime export pipeline was not available."), RequestToken, EffectiveRequestId);
		return false;
	}

	ActiveRequestToken = RequestToken;
	const bool bAccepted = Pipeline->StartExporting(Sounds, Config, OutputPath, EffectiveRequestId);
	if (bAccepted && Pipeline->IsExporting())
	{
		// Session directories are collision-safe and normally new, but clearing here makes the
		// publication fence explicit if a test or caller intentionally reuses an exact path.
		FAudioExporterUtils::ClearExportPathCancellation(Pipeline->GetExportPath());
	}
	return bAccepted;
}

void UAudioExporterRuntimeSubsystem::CancelExport(bool bDeleteFiles)
{
	if (!Pipeline.IsValid())
	{
		UE_LOG(LogAudioExporter, Warning, TEXT("%s: Cancel was requested without a valid runtime pipeline."), *FString(__FUNCTION__));
		return;
	}

	if (Pipeline->IsExporting())
	{
		// Fence the exact session path BEFORE CancelExporting invalidates callback generation. A
		// background save that already crossed its initial bCanSave snapshot will observe this path
		// fence before or immediately after atomic publication and remove any losing artifact.
		FAudioExporterUtils::MarkExportPathCancelled(Pipeline->GetExportPath());
		Pipeline->CancelExporting(bDeleteFiles);
	}
}

bool UAudioExporterRuntimeSubsystem::IsExporting() const
{
	return Pipeline.IsValid() && Pipeline->IsExporting();
}

FString UAudioExporterRuntimeSubsystem::GetExportPath() const
{
	return Pipeline.IsValid() ? Pipeline->GetExportPath() : FString();
}

USoundSubmix* UAudioExporterRuntimeSubsystem::GetRecordingSubmix() const
{
	return Pipeline.IsValid() ? Pipeline->GetRecordingSubmix() : nullptr;
}

void UAudioExporterRuntimeSubsystem::SetMonitoring(bool bMonitoring) const
{
	if (Pipeline.IsValid())
	{
		Pipeline->SetMonitoring(bMonitoring);
	}
}

FAudioExportRequestToken UAudioExporterRuntimeSubsystem::AddOneTimeCompleteCallback(TFunction<void(const FString&)> Callback)
{
	if (!Callback)
	{
		return 0;
	}

	const FAudioExportRequestToken RequestToken = ++NextRequestToken;
	{
		FScopeLock PendingCallbacksLock(&PendingResultCallbacksMutex);
		PendingResultCallbacks.Add(
			RequestToken,
			[Callback = MoveTemp(Callback)](const FAudioExportResult& Result)
			{
				Callback(Result.ExportPath);
			});
	}
	return RequestToken;
}

FAudioExportRequestToken UAudioExporterRuntimeSubsystem::AddOneTimeResultCallback(TFunction<void(const FAudioExportResult&)> Callback)
{
	if (!Callback)
	{
		return 0;
	}

	const FAudioExportRequestToken RequestToken = ++NextRequestToken;
	{
		FScopeLock PendingCallbacksLock(&PendingResultCallbacksMutex);
		PendingResultCallbacks.Add(RequestToken, MoveTemp(Callback));
	}
	return RequestToken;
}

#if WITH_DEV_AUTOMATION_TESTS
bool UAudioExporterRuntimeSubsystem::BeginLifecycleTestSession(
	UWorld* World,
	const FString& InExportPath,
	FAudioExportRequestToken RequestToken,
	const FGuid& RequestId)
{
	if (!EnsurePipeline(World))
	{
		return false;
	}

	ActiveRequestToken = RequestToken;
	Pipeline->BeginLifecycleTestSession(InExportPath);
	Pipeline->SetCorrelationIdsForTesting(RequestId.IsValid() ? RequestId : FGuid::NewGuid(), FGuid::NewGuid());
	return Pipeline->IsExporting();
}

bool UAudioExporterRuntimeSubsystem::BeginLifecycleTestSession(
	UWorld* World,
	const TArray<FAudioExportSoundEntry>& Sounds,
	const FAudioExporterConfig& Config,
	const FString& InExportPath,
	FAudioExportRequestToken RequestToken,
	const FGuid& RequestId)
{
	if (!EnsurePipeline(World))
	{
		return false;
	}

	ActiveRequestToken = RequestToken;
	Pipeline->BeginLifecycleTestSession(Sounds, Config, InExportPath);
	Pipeline->SetCorrelationIdsForTesting(RequestId.IsValid() ? RequestId : FGuid::NewGuid(), FGuid::NewGuid());
	return Pipeline->IsExporting();
}

void UAudioExporterRuntimeSubsystem::SimulateVariationFinishedForTesting()
{
	if (Pipeline.IsValid())
	{
		Pipeline->SimulateVariationFinishedForTesting();
	}
}

void UAudioExporterRuntimeSubsystem::SetForceAsyncSaveForTesting(bool bForceAsync)
{
	if (Pipeline.IsValid())
	{
		Pipeline->SetForceAsyncSaveForTesting(bForceAsync);
	}
}

int32 UAudioExporterRuntimeSubsystem::GetActiveAsyncExportsForTesting() const
{
	return Pipeline.IsValid() ? Pipeline->GetActiveAsyncExportsForTesting() : 0;
}
#endif

// ---------------------------------------------------------------------------
// Pipeline helpers / callbacks
// ---------------------------------------------------------------------------

bool UAudioExporterRuntimeSubsystem::EnsurePipeline(UWorld* World)
{
	if (World == nullptr)
	{
		UE_LOG(LogAudioExporter, Error, TEXT("%s: A valid world is required to start audio export."), *FString(__FUNCTION__));
		return false;
	}

	if (Pipeline.IsValid() && Pipeline->IsExporting())
	{
		if (PipelineWorld.Get() != World)
		{
			UE_LOG(LogAudioExporter, Error, TEXT("%s: A different world was supplied while an export session is already active."), *FString(__FUNCTION__));
			return false;
		}

		return true;
	}

	if (!Pipeline.IsValid())
	{
		Pipeline = MakeShared<FAudioExportPipeline>();
		Pipeline->OnExportFinished.AddUObject(this, &UAudioExporterRuntimeSubsystem::HandlePipelineFinished);
		Pipeline->OnExportResult.AddUObject(this, &UAudioExporterRuntimeSubsystem::HandlePipelineResult);
		Pipeline->OnExportProgressUpdated.AddUObject(this, &UAudioExporterRuntimeSubsystem::HandlePipelineProgress);
	}

	PipelineWorld = TWeakObjectPtr<UWorld>(World);
	Pipeline->Initialize(World);
	return true;
}

void UAudioExporterRuntimeSubsystem::HandlePipelineFinished(const FString& FinishedExportPath)
{
	// The pipeline owns the terminal ordering. Its structured result has already settled before this
	// legacy compatibility event reaches listeners.
	OnExportCompleteNative.Broadcast(FinishedExportPath);
	OnExportComplete.Broadcast(FinishedExportPath);
}

void UAudioExporterRuntimeSubsystem::HandlePipelineResult(const FAudioExportResult& Result)
{
	// Detach only the callback owned by this pipeline session. A rejected request that arrives while
	// another session is active has already been settled by BroadcastRejectedResult and must not
	// consume this token or the active session's callback.
	const FAudioExportRequestToken RequestToken = ActiveRequestToken;
	ActiveRequestToken = 0;
	TFunction<void(const FAudioExportResult&)> Callback;
	if (RequestToken != 0)
	{
		FScopeLock PendingCallbacksLock(&PendingResultCallbacksMutex);
		if (TFunction<void(const FAudioExportResult&)>* PendingCallback = PendingResultCallbacks.Find(RequestToken))
		{
			Callback = MoveTemp(*PendingCallback);
			PendingResultCallbacks.Remove(RequestToken);
		}
	}

	if (Callback)
	{
		Callback(Result);
	}

	OnExportCompleteResultNative.Broadcast(Result);
	OnExportCompleteResult.Broadcast(Result);
}

void UAudioExporterRuntimeSubsystem::BroadcastRejectedResult(
	const FString& ErrorMessage,
	FAudioExportRequestToken RequestToken,
	const FGuid& RequestId)
{
	FAudioExportResult Result;
	Result.RequestId = RequestId.IsValid() ? RequestId : FGuid::NewGuid();
	Result.Outcome = EAudioExportOutcome::Rejected;
	Result.bAcceptedStart = false;
	Result.ErrorMessage = ErrorMessage;

	// Settle only the callback registered for THIS request. The active session's token
	// stays attached to its pipeline, so its terminal callback cannot be stolen by this rejection.
	TFunction<void(const FAudioExportResult&)> Callback;
	if (RequestToken != 0)
	{
		FScopeLock PendingCallbacksLock(&PendingResultCallbacksMutex);
		if (TFunction<void(const FAudioExportResult&)>* PendingCallback = PendingResultCallbacks.Find(RequestToken))
		{
			Callback = MoveTemp(*PendingCallback);
			PendingResultCallbacks.Remove(RequestToken);
		}
	}

	if (Callback)
	{
		Callback(Result);
	}

	OnExportCompleteResultNative.Broadcast(Result);
	OnExportCompleteResult.Broadcast(Result);

	// the legacy completion surface is part of the documented per-request contract
	// ("bind to OnExportComplete for a completion event"), so a subsystem-refused request must
	// settle it too -- exactly once, with an empty path per legacy failure semantics. Structured
	// fires first, mirroring BroadcastTerminalOnce's canonical ordering. This never touches the
	// active session: its own terminal will still deliver its own pair exactly once later.
	OnExportCompleteNative.Broadcast(Result.ExportPath);
	OnExportComplete.Broadcast(Result.ExportPath);
}

void UAudioExporterRuntimeSubsystem::BroadcastRejectedStart(const FString& ErrorMessage, const FGuid& RequestId)
{
	BroadcastRejectedResult(ErrorMessage, /*RequestToken*/ 0, RequestId);
}

void UAudioExporterRuntimeSubsystem::HandlePipelineProgress(const FAudioExportProgressInfo& ProgressInfo)
{
	OnExportProgressNative.Broadcast(ProgressInfo);
	OnExportProgress.Broadcast(ProgressInfo);
}
