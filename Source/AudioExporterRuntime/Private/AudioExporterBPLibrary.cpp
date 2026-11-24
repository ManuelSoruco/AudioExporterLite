// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "AudioExporterBPLibrary.h"
#include "AudioExporterRuntimeSubsystem.h"
#include "Utilities/AudioExporterLogger.h"
#include "Engine/Engine.h"
#include "Sound/SoundBase.h"
#include "Engine/World.h"

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace AudioExporterBPLibraryInternal
{
	static UWorld* ResolveWorld(UObject* WorldContextObject)
	{
		if (!IsValid(WorldContextObject))
		{
			return nullptr;
		}

		return WorldContextObject->GetWorld();
	}

	/**
	 * Converts a USoundBase* array to FAudioExportSoundEntry array without erasing input slots.
	 * Every caller-supplied occurrence owns one request identity and one terminal row. Invalid
	 * references are carried as explicit null entries so the runtime pipeline can settle them as
	 * failures instead of silently shrinking RequestedCount before admission.
	 */
	static TArray<FAudioExportSoundEntry> MakeSoundEntries(const TArray<USoundBase*>& Sounds)
	{
		TArray<FAudioExportSoundEntry> Entries;
		Entries.Reserve(Sounds.Num());

		for (int32 SoundIndex = 0; SoundIndex < Sounds.Num(); ++SoundIndex)
		{
			USoundBase* Sound = Sounds[SoundIndex];
			FAudioExportSoundEntry Entry;
			Entry.Sound = Sound;
			Entry.RelativePath = TEXT("");

			if (IsValid(Sound))
			{
				Entry.SoundName = Sound->GetName();
				Entry.SourceAssetPath = Sound->GetPathName();
			}
			else
			{
				Entry.SoundName = FString::Printf(TEXT("InvalidSound_%d"), SoundIndex + 1);
				Entry.SourceAssetPath = FString();
			}

			Entries.Add(MoveTemp(Entry));
		}

		return Entries;
	}
} // namespace AudioExporterBPLibraryInternal

// ---------------------------------------------------------------------------
// UAudioExporterBPLibrary implementation
// ---------------------------------------------------------------------------

UAudioExporterRuntimeSubsystem* UAudioExporterBPLibrary::GetRuntimeSubsystem()
{
	if (!IsValid(GEngine))
	{
		UE_LOG(LogAudioExporter, Warning, TEXT("%s: GEngine was unavailable while resolving the AudioExporter runtime subsystem."), *FString(__FUNCTION__));
		return nullptr;
	}

	return GEngine->GetEngineSubsystem<UAudioExporterRuntimeSubsystem>();
}

void UAudioExporterBPLibrary::ExportSoundToWav(UObject* WorldContextObject,
                                               USoundBase* Sound,
                                               const FAudioExporterConfig& Config,
                                               const FString& OutputPath)
{
	ExportSoundToWavIdentified(WorldContextObject, Sound, Config, OutputPath);
}

FGuid UAudioExporterBPLibrary::ExportSoundToWavIdentified(UObject* WorldContextObject,
                                                USoundBase* Sound,
                                                const FAudioExporterConfig& Config,
                                                const FString& OutputPath)
{
	const FGuid RequestId = FGuid::NewGuid();
	UAudioExporterRuntimeSubsystem* Subsystem = GetRuntimeSubsystem();
	if (!IsValid(Subsystem))
	{
		UE_LOG(LogAudioExporter, Warning,
		       TEXT("UAudioExporterBPLibrary::ExportSoundToWav: Could not find UAudioExporterRuntimeSubsystem."));
		return RequestId;
	}

	// Fire-and-forget callers own no callback, so every refusal below settles the
	// documented completion contract itself via BroadcastRejectedStart (structured result first,
	// then legacy completion, exactly once) -- mirroring ExportSoundAsyncWithResult's Reject shape.
	auto Reject = [Subsystem, RequestId](const FString& Message)
	{
		Subsystem->BroadcastRejectedStart(Message, RequestId);
	};

	if (!IsValid(Sound))
	{
		UE_LOG(LogAudioExporter, Warning, TEXT("UAudioExporterBPLibrary::ExportSoundToWav: Sound is null."));
		Reject(TEXT("Sound is null."));
		return RequestId;
	}

	const TArray<FAudioExportSoundEntry> Entries = AudioExporterBPLibraryInternal::MakeSoundEntries({Sound});
	UWorld* World = AudioExporterBPLibraryInternal::ResolveWorld(WorldContextObject);
	if (!IsValid(World))
	{
		UE_LOG(LogAudioExporter, Warning, TEXT("UAudioExporterBPLibrary::ExportSoundToWav: Could not resolve a valid world from the provided context object."));
		Reject(TEXT("Could not resolve a valid world from the provided context object."));
		return RequestId;
	}

	Subsystem->StartExport(World, Entries, Config, OutputPath, 0, RequestId);
	return RequestId;
}

void UAudioExporterBPLibrary::ExportSoundsToWav(UObject* WorldContextObject,
                                                const TArray<USoundBase*>& Sounds,
                                                const FAudioExporterConfig& Config,
                                                const FString& OutputPath)
{
	ExportSoundsToWavIdentified(WorldContextObject, Sounds, Config, OutputPath);
}

FGuid UAudioExporterBPLibrary::ExportSoundsToWavIdentified(UObject* WorldContextObject,
                                                 const TArray<USoundBase*>& Sounds,
                                                 const FAudioExporterConfig& Config,
                                                 const FString& OutputPath)
{
	const FGuid RequestId = FGuid::NewGuid();
	UAudioExporterRuntimeSubsystem* Subsystem = GetRuntimeSubsystem();
	if (!IsValid(Subsystem))
	{
		UE_LOG(LogAudioExporter, Warning,
		       TEXT("UAudioExporterBPLibrary::ExportSoundsToWav: Could not find UAudioExporterRuntimeSubsystem."));
		return RequestId;
	}

	// Same fire-and-forget rejection contract as ExportSoundToWav above.
	auto Reject = [Subsystem, RequestId](const FString& Message)
	{
		Subsystem->BroadcastRejectedStart(Message, RequestId);
	};

	if (Sounds.IsEmpty())
	{
		UE_LOG(LogAudioExporter, Warning,
		       TEXT("UAudioExporterBPLibrary::ExportSoundsToWav: The requested sound array was empty."));
		Reject(TEXT("The requested sound array was empty."));
		return RequestId;
	}

	const TArray<FAudioExportSoundEntry> Entries = AudioExporterBPLibraryInternal::MakeSoundEntries(Sounds);
	UWorld* World = AudioExporterBPLibraryInternal::ResolveWorld(WorldContextObject);
	if (!IsValid(World))
	{
		UE_LOG(LogAudioExporter, Warning, TEXT("UAudioExporterBPLibrary::ExportSoundsToWav: Could not resolve a valid world from the provided context object."));
		Reject(TEXT("Could not resolve a valid world from the provided context object."));
		return RequestId;
	}

	Subsystem->StartExport(World, Entries, Config, OutputPath, 0, RequestId);
	return RequestId;
}

void UAudioExporterBPLibrary::ExportSoundAsync(UObject* WorldContextObject,
                                                USoundBase* Sound,
                                                const FAudioExporterConfig& Config,
                                                const FString& OutputPath,
                                                const FOnExportComplete& OnComplete)
{
	const FGuid RequestId = FGuid::NewGuid();
	if (!IsValid(Sound))
	{
		UE_LOG(LogAudioExporter, Warning, TEXT("%s: Sound is null."), *FString(__FUNCTION__));
		if (OnComplete.IsBound())
		{
			OnComplete.Execute(TEXT(""));
		}
		return;
	}

	UAudioExporterRuntimeSubsystem* Subsystem = GetRuntimeSubsystem();
	if (!IsValid(Subsystem))
	{
		UE_LOG(LogAudioExporter, Warning,
		       TEXT("%s: Could not find UAudioExporterRuntimeSubsystem."), *FString(__FUNCTION__));
		if (OnComplete.IsBound())
		{
			OnComplete.Execute(TEXT(""));
		}
		return;
	}

	const TArray<FAudioExportSoundEntry> Entries = AudioExporterBPLibraryInternal::MakeSoundEntries({Sound});
	UWorld* World = AudioExporterBPLibraryInternal::ResolveWorld(WorldContextObject);
	if (!IsValid(World))
	{
		UE_LOG(LogAudioExporter, Warning, TEXT("%s: Could not resolve a valid world from the provided context object."), *FString(__FUNCTION__));
		if (OnComplete.IsBound())
		{
			OnComplete.Execute(TEXT(""));
		}
		return;
	}

	// Register before StartExport because unattended/editor automation can complete a session
	// synchronously. AddOneTimeCompleteCallback stores this legacy path adapter under the same
	// request-owned structured-result token used by ExportSoundAsyncWithResult.
	FAudioExportRequestToken RequestToken = 0;
	if (OnComplete.IsBound())
	{
		const FOnExportComplete CapturedOnComplete = OnComplete;
		RequestToken = Subsystem->AddOneTimeCompleteCallback(
			[CapturedOnComplete](const FString& FinishedPath)
			{
				if (CapturedOnComplete.IsBound())
				{
					CapturedOnComplete.Execute(FinishedPath);
				}
			});
	}

	Subsystem->StartExport(World, Entries, Config, OutputPath, RequestToken, RequestId);
}

void UAudioExporterBPLibrary::ExportSoundAsyncWithResult(UObject* WorldContextObject,
                                                         USoundBase* Sound,
                                                         const FAudioExporterConfig& Config,
                                                         const FString& OutputPath,
                                                         const FOnExportCompleteResult& OnComplete)
{
	const FGuid RequestId = FGuid::NewGuid();
	// Rejected starts settle completion immediately with an explicit Rejected result and an unset
	// path; the request is never left hanging without a completion event.
	FOnExportCompleteResult CapturedOnComplete = OnComplete;
	auto Reject = [&CapturedOnComplete, RequestId](const FString& Message)
	{
		if (CapturedOnComplete.IsBound())
		{
			FAudioExportResult Rejected;
			Rejected.RequestId = RequestId;
			Rejected.Outcome = EAudioExportOutcome::Rejected;
			Rejected.bAcceptedStart = false;
			Rejected.ErrorMessage = Message;
			CapturedOnComplete.Execute(Rejected);
		}
	};

	if (!IsValid(Sound))
	{
		UE_LOG(LogAudioExporter, Warning, TEXT("%s: Sound is null."), *FString(__FUNCTION__));
		Reject(TEXT("Sound is null."));
		return;
	}

	UAudioExporterRuntimeSubsystem* Subsystem = GetRuntimeSubsystem();
	if (!IsValid(Subsystem))
	{
		UE_LOG(LogAudioExporter, Warning,
		       TEXT("%s: Could not find UAudioExporterRuntimeSubsystem."), *FString(__FUNCTION__));
		Reject(TEXT("Could not find UAudioExporterRuntimeSubsystem."));
		return;
	}

	const TArray<FAudioExportSoundEntry> Entries = AudioExporterBPLibraryInternal::MakeSoundEntries({Sound});
	UWorld* World = AudioExporterBPLibraryInternal::ResolveWorld(WorldContextObject);
	if (!IsValid(World))
	{
		UE_LOG(LogAudioExporter, Warning, TEXT("%s: Could not resolve a valid world from the provided context object."), *FString(__FUNCTION__));
		Reject(TEXT("Could not resolve a valid world from the provided context object."));
		return;
	}

	// Register BEFORE calling StartExport, not after. Every automation run (and any
	// commandlet) has FApp::IsUnattended() == true, which lets a render finish synchronously inside
	// AudioRendererRealtime::CompleteRender -- an accepted session can broadcast its terminal
	// FAudioExportResult before StartExport() even returns to this function. Registering afterwards
	// would strand that callback in PendingResultCallbacks (it never fires, or fires later with an
	// unrelated session's result). A rejection settles synchronously too, through this exact
	// callback: both FailStartup->BroadcastTerminalOnce (pipeline-level) and BroadcastRejectedResult
	// (subsystem-level) drain PendingResultCallbacks before StartExport returns, so the
	// separate local Reject() below is no longer needed for that path.
	FAudioExportRequestToken RequestToken = 0;
	if (CapturedOnComplete.IsBound())
	{
		RequestToken = Subsystem->AddOneTimeResultCallback(
			[CapturedOnComplete](const FAudioExportResult& Result)
			{
				if (CapturedOnComplete.IsBound())
				{
					CapturedOnComplete.Execute(Result);
				}
			});
	}

	Subsystem->StartExport(World, Entries, Config, OutputPath, RequestToken, RequestId);
}

bool UAudioExporterBPLibrary::IsExporting(UObject* /*WorldContextObject*/)
{
	const UAudioExporterRuntimeSubsystem* Subsystem = GetRuntimeSubsystem();
	return Subsystem ? Subsystem->IsExporting() : false;
}

void UAudioExporterBPLibrary::CancelExport(UObject* /*WorldContextObject*/)
{
	UAudioExporterRuntimeSubsystem* Subsystem = GetRuntimeSubsystem();
	if (IsValid(Subsystem))
	{
		Subsystem->CancelExport(false);
	}
}
