// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "Utilities/AudioRendererRealtime.h"
#include "Audio.h"
#include "Logging/LogMacros.h"
#include "Sound/SoundBase.h"
#include "HAL/PlatformProcess.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "AudioDeviceManager.h"
#include "AudioMixerDevice.h"
#include "Components/AudioComponent.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Misc/ScopeLock.h"
#include "TimerManager.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundSubmix.h"
#include "Sound/SoundWave.h"
#include "Sound/SoundWaveProcedural.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Containers/Ticker.h"
#endif
#include "Misc/EngineVersionComparison.h"

// UE 5.8 introduced the ECVF_SetByTemp tier ("used for temporarily replacing a
// variable"). UE 5.6/5.7 have no temporary tier; the nearest semantic match there is
// ECVF_SetByCode ("used for high priority temporary debugging or operation modes"),
// which sits at the same relative priority: above ini/commandline/game settings,
// below the interactive console only. Every Set/Unset pair in this file passes the
// same tier, so the temporary-override-and-restore pattern behaves identically on
// all three supported engine versions.
#if UE_VERSION_OLDER_THAN(5, 8, 0)
#define AF_CVAR_SETBY_TEMP ECVF_SetByCode
#else
#define AF_CVAR_SETBY_TEMP ECVF_SetByTemp
#endif

DEFINE_LOG_CATEGORY_STATIC(LogAudioRendererRealtime, Log, All);

class FAudioExporterSourceBufferCapture final : public ISourceBufferListener
{
public:
	virtual void OnNewBuffer(const FOnNewBufferParams& InParams) override
	{
		if (!InParams.AudioData || InParams.NumSamples <= 0 || InParams.NumChannels <= 0 || InParams.SampleRate <= 0)
		{
			return;
		}

		FScopeLock Lock(&CaptureGuard);
		if (NumChannels == 0 && SampleRate == 0)
		{
			NumChannels = InParams.NumChannels;
			SampleRate = InParams.SampleRate;
		}
		else if (NumChannels != InParams.NumChannels || SampleRate != InParams.SampleRate)
		{
			bFormatMismatch = true;
			return;
		}

		CapturedAudio.Append(InParams.AudioData, InParams.NumSamples);
	}

	virtual void OnSourceReleased(const int32) override {}

	bool CopyCapturedAudio(TArray<float>& OutBuffer, int32& OutNumChannels, int32& OutSampleRate) const
	{
		FScopeLock Lock(&CaptureGuard);
		if (bFormatMismatch || CapturedAudio.IsEmpty() || NumChannels <= 0 || SampleRate <= 0)
		{
			return false;
		}

		OutBuffer = CapturedAudio;
		OutNumChannels = NumChannels;
		OutSampleRate = SampleRate;
		return true;
	}

private:
	mutable FCriticalSection CaptureGuard;
	TArray<float> CapturedAudio;
	int32 NumChannels = 0;
	int32 SampleRate = 0;
	bool bFormatMismatch = false;
};

#if WITH_DEV_AUTOMATION_TESTS
// Definition of the test-only separator-branch override (see
// SetForceAttendedSeparatorBranchForTesting). Guarded so it compiles out of Shipping entirely.
bool FAudioRendererRealtime::bForceAttendedSeparatorBranchForTesting = false;
#endif

namespace
{
	FCriticalSection RealtimeAudioOverrideGuard;
	int32 RealtimeAudioOverrideOwners = 0;
	const FName RealtimeAudioOverrideTag(TEXT("AudioExporterRealtimeOverride"));

#if WITH_DEV_AUTOMATION_TESTS
	// Test-seam mirror of the per-renderer bAppliedAudioOverrides instance flag (the seams below
	// run the lease without a renderer object). Guarded so it compiles out of Shipping entirely.
	bool RealtimeAudioOverrideAppliedForTesting = false;
#endif

	// Tag for the shared-lease bookkeeping writes below. Distinct from RealtimeAudioOverrideTag so
	// the cvar log history can tell the owner-count bookkeeping from the cvar overrides themselves.
	// The shared-lease bookkeeping ints are written by BOTH products (this file and the shared
	// offline renderer used by Pro and Normalizer). On UE 5.8 the console-variable history is a
	// per-tier, per-tag array and a read returns the highest tier's last entry, so the two
	// writers must use the SAME tier and the SAME tag or one product's writes shadow the
	// other's. This tier and tag are the offline renderer's (AudioBackendManager.cpp,
	// SharedStateCVarTag / WriteSharedInt); keep them identical on both sides.
	const FName RealtimeAudioSharedOverrideTag(TEXT("AudioFactory.OfflineRenderer.SharedState"));

	// ---------------------------------------------------------------------------
	// Shared override lease.
	//
	// au.DisableAppVolume and au.streamcaching.ForceBlockForLoad are may also be overridden by another
	// AudioFactory renderer that runs the identical protocol from a second module in the same editor process. Registered int console
	// variables are process-wide, so whichever product registers
	// AudioFactory.SharedOverride.Owners.<Key>/AudioFactory.SharedOverride.Saved.<Key> first creates
	// them and both products read and write the same ints. The Owners count decides when a setting
	// is written back, so the first product to finish can no longer restore a setting the second
	// still holds. The MetaSound keys are private to this module; they ride the same helper so the
	// lease has one code path. The ints are private bookkeeping and are never Unset -- on
	// UE 5.6/5.7 AF_CVAR_SETBY_TEMP is ECVF_SetByCode, where Unset is a silent no-op once any
	// earlier Code-tier writer has touched the cvar. That trap is exactly why the overridden cvars
	// themselves are restored below with an explicit Set of the saved value instead of Unset.
	// ---------------------------------------------------------------------------
	IConsoleVariable* FindOrCreateSharedIntCVar(const TCHAR* InScope, const TCHAR* InKey)
	{
		const FString CVarName = FString::Printf(TEXT("AudioFactory.SharedOverride.%s.%s"), InScope, InKey);
		IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*CVarName);
		if (!CVar)
		{
			// Registering an already-registered name returns the existing object with its value
			// preserved, so a racing module's duplicate registration still resolves to these ints.
			CVar = IConsoleManager::Get().RegisterConsoleVariable(*CVarName, 0,
				TEXT("AudioFactory shared override lease bookkeeping (Owners count or Saved value; shared across products in this process)."));
		}
		return CVar;
	}

	int32 ReadSharedInt(const TCHAR* InScope, const TCHAR* InKey)
	{
		IConsoleVariable* CVar = FindOrCreateSharedIntCVar(InScope, InKey);
		return CVar ? CVar->GetInt() : 0;
	}

	void WriteSharedInt(const TCHAR* InScope, const TCHAR* InKey, int32 InValue)
	{
		IConsoleVariable* CVar = FindOrCreateSharedIntCVar(InScope, InKey);
		if (CVar)
		{
			CVar->Set(InValue, ECVF_SetByCode, RealtimeAudioSharedOverrideTag);
		}
	}

	// Acquires one shared lease entry: saves the pre-session value when this is the first owner,
	// bumps the owner count, then writes the override. A Set that does not stick is a warning and
	// never a lease failure (the export continues; determinism may be degraded).
	void AcquireSharedOverride(IConsoleVariable* InCVar, const TCHAR* InCVarName, const TCHAR* InKey, int32 InValue)
	{
		const int32 PriorOwnerCount = ReadSharedInt(TEXT("Owners"), InKey);
		if (PriorOwnerCount == 0)
		{
			WriteSharedInt(TEXT("Saved"), InKey, InCVar->GetInt());
		}
		WriteSharedInt(TEXT("Owners"), InKey, PriorOwnerCount + 1);
		InCVar->Set(InValue, AF_CVAR_SETBY_TEMP, RealtimeAudioOverrideTag);
		const int32 ReadBackValue = InCVar->GetInt();
		if (ReadBackValue != InValue)
		{
			UE_LOG(LogAudioRendererRealtime, Warning,
				TEXT("ApplyAudioOverrides: setting %s to %d did not stick (read back %d); continuing without failing the lease."),
				InCVarName, InValue, ReadBackValue);
		}
	}

	// Releases one shared lease entry and, on the transition to zero owners, writes the saved
	// pre-session value back with an explicit Set. Never Unset: see the shared-lease comment above.
	void ReleaseSharedOverride(IConsoleVariable* InCVar, const TCHAR* InCVarName, const TCHAR* InKey)
	{
		const int32 PriorOwnerCount = ReadSharedInt(TEXT("Owners"), InKey);
		if (PriorOwnerCount <= 0)
		{
			UE_LOG(LogAudioRendererRealtime, Warning,
				TEXT("RestoreAudioOverrides: %s has no outstanding shared-override owner; skipping the release."),
				InCVarName);
			return;
		}
		WriteSharedInt(TEXT("Owners"), InKey, PriorOwnerCount - 1);
		if (PriorOwnerCount == 1)
		{
			InCVar->Set(ReadSharedInt(TEXT("Saved"), InKey), AF_CVAR_SETBY_TEMP, RealtimeAudioOverrideTag);
		}
	}

	void ApplyRealtimeAudioOverrides(bool& bAppliedAudioOverrides)
	{
		if (bAppliedAudioOverrides)
		{
			return;
		}
		FScopeLock Lock(&RealtimeAudioOverrideGuard);

		IConsoleVariable* MetaSoundAsyncBuilderCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("au.MetaSound.EnableAsyncGeneratorBuilder"));
		IConsoleVariable* MetaSoundCommandletExecutionCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("au.MetaSound.EnableCommandletExecution"));
		IConsoleVariable* DisableAppVolumeCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("au.DisableAppVolume"));
		IConsoleVariable* ForceBlockForLoadCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("au.streamcaching.ForceBlockForLoad"));
		bAppliedAudioOverrides = MetaSoundAsyncBuilderCVar || MetaSoundCommandletExecutionCVar || DisableAppVolumeCVar || ForceBlockForLoadCVar;
		if (!bAppliedAudioOverrides)
		{
			return;
		}

		if (RealtimeAudioOverrideOwners == 0)
		{
			if ((FApp::IsUnattended() || IsRunningCommandlet()) && MetaSoundAsyncBuilderCVar)
			{
				AcquireSharedOverride(MetaSoundAsyncBuilderCVar, TEXT("au.MetaSound.EnableAsyncGeneratorBuilder"), TEXT("MetaSoundAsyncBuilder"), 0);
			}

			if ((FApp::IsUnattended() || IsRunningCommandlet()) && MetaSoundCommandletExecutionCVar)
			{
				AcquireSharedOverride(MetaSoundCommandletExecutionCVar, TEXT("au.MetaSound.EnableCommandletExecution"), TEXT("MetaSoundCommandletExecution"), 1);
			}

			// A non-preview component is required for UE's bPlayEffectChainTails completion path. Disable
			// application focus volume while any realtime renderer owns this process-wide lease.
			if (DisableAppVolumeCVar)
			{
				AcquireSharedOverride(DisableAppVolumeCVar, TEXT("au.DisableAppVolume"), TEXT("DisableAppVolume"), 1);
			}

			// When a streamed sound's first chunk is not yet loaded, AudioDecompress.cpp zero-fills the
			// decode buffer instead of waiting, prepending digital silence to exports. AudioStreamingCache.cpp's
			// ForceBlockForLoadCVar makes the chunk fetch block until the read completes; export latency is
			// irrelevant and silence is corruption, so force it while any realtime renderer owns this lease.
			if (ForceBlockForLoadCVar)
			{
				AcquireSharedOverride(ForceBlockForLoadCVar, TEXT("au.streamcaching.ForceBlockForLoad"), TEXT("ForceBlockForLoad"), 1);
			}
		}

		++RealtimeAudioOverrideOwners;
		if (bAppliedAudioOverrides)
		{
			UE_LOG(LogAudioRendererRealtime, Log, TEXT("ApplyAudioOverrides: enabled deterministic realtime playback overrides."));
		}
	}

	void RestoreRealtimeAudioOverrides(bool& bAppliedAudioOverrides)
	{
		if (!bAppliedAudioOverrides)
		{
			return;
		}
		FScopeLock Lock(&RealtimeAudioOverrideGuard);
		RealtimeAudioOverrideOwners = FMath::Max(0, RealtimeAudioOverrideOwners - 1);

		if (RealtimeAudioOverrideOwners == 0)
		{
			// Mirror the acquisition gates exactly: a cvar that was gated out at apply time was
			// never acquired, so it must not release (and would warn against a zero owner count).
			// Unattended/commandlet state cannot flip between apply and restore of one session.
			if ((FApp::IsUnattended() || IsRunningCommandlet()))
			{
				if (IConsoleVariable* MetaSoundAsyncBuilderCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("au.MetaSound.EnableAsyncGeneratorBuilder")))
				{
					ReleaseSharedOverride(MetaSoundAsyncBuilderCVar, TEXT("au.MetaSound.EnableAsyncGeneratorBuilder"), TEXT("MetaSoundAsyncBuilder"));
				}
				if (IConsoleVariable* MetaSoundCommandletExecutionCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("au.MetaSound.EnableCommandletExecution")))
				{
					ReleaseSharedOverride(MetaSoundCommandletExecutionCVar, TEXT("au.MetaSound.EnableCommandletExecution"), TEXT("MetaSoundCommandletExecution"));
				}
			}
			if (IConsoleVariable* DisableAppVolumeCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("au.DisableAppVolume")))
			{
				ReleaseSharedOverride(DisableAppVolumeCVar, TEXT("au.DisableAppVolume"), TEXT("DisableAppVolume"));
			}
			if (IConsoleVariable* ForceBlockForLoadCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("au.streamcaching.ForceBlockForLoad")))
			{
				ReleaseSharedOverride(ForceBlockForLoadCVar, TEXT("au.streamcaching.ForceBlockForLoad"), TEXT("ForceBlockForLoad"));
			}
		}

		bAppliedAudioOverrides = false;
	}

	void PrimeSoundCueWavePlayers(USoundBase* InSound, FAudioDevice* AudioDevice)
	{
		USoundCue* SoundCue = Cast<USoundCue>(InSound);
		if (!SoundCue || !SoundCue->FirstNode)
		{
			return;
		}

		TArray<USoundNodeWavePlayer*> WavePlayers;
		SoundCue->RecursiveFindNode<USoundNodeWavePlayer>(SoundCue->FirstNode, WavePlayers);
		if (WavePlayers.IsEmpty())
		{
			return;
		}

		for (USoundNodeWavePlayer* WavePlayer : WavePlayers)
		{
			if (WavePlayer)
			{
				WavePlayer->LoadAsset(false);
			}
		}

		FlushAsyncLoading();

		int32 LoadedWaveCount = 0;
		for (USoundNodeWavePlayer* WavePlayer : WavePlayers)
		{
			if (!WavePlayer)
			{
				continue;
			}

			WavePlayer->LoadAsset(false);
			if (USoundWave* LoadedWave = WavePlayer->GetSoundWave())
			{
				if (AudioDevice)
				{
					AudioDevice->Precache(LoadedWave, true, false, true);
				}
				else
				{
					LoadedWave->InitResources();
				}
				++LoadedWaveCount;
			}
		}

		UE_LOG(LogAudioRendererRealtime, Log, TEXT("PrimeSoundCueWavePlayers: initialized %d/%d wave-player asset(s) for %s."),
			LoadedWaveCount, WavePlayers.Num(), *SoundCue->GetName());
	}
}

void FAudioRendererRealtime::AddReferencedObjects(FReferenceCollector& Collector)
{
	if (AudioComponent)
	{
		Collector.AddReferencedObject(AudioComponent);
	}

	if (DuplicateSound)
	{
		Collector.AddReferencedObject(DuplicateSound);
	}

	if (RecordingSubmix)
	{
		Collector.AddReferencedObject(RecordingSubmix);
	}
}

FAudioRendererRealtime::~FAudioRendererRealtime()
{
	Shutdown();
}

bool FAudioRendererRealtime::Initialize(UWorld* InWorld)
{
	// 1. Early return if already initialized
	if (bInitialized)
	{
		return true;
	}

	// 2. Validate the injected world
	if (!InWorld)
	{
		UE_LOG(LogAudioRendererRealtime, Error, TEXT("%s: No valid world was provided."), *FString(__FUNCTION__));
		return false;
	}

	// 3. Setup the necessary audio backend (World context and Submixes)
	if (!SetupAudioBackEnd(InWorld))
	{
		return false;
	}

	bInitialized = true;
	return true;
}

void FAudioRendererRealtime::Shutdown()
{
	// 1. Early return if not initialized
	if (!bInitialized)
	{
		return;
	}

	RequestCancel();

	ClearRenderWatchdogTickers();
	if (PlaybackWorld.IsValid())
	{
		PlaybackWorld->GetTimerManager().ClearTimer(SeparatorTimerHandle);
	}
#if WITH_DEV_AUTOMATION_TESTS
	ClearForcedAttendedSeparatorTicker();
#endif

	// 2. If we are in batch mode, stop the recording session
	if (bInBatchRecording)
	{
		TArray<float> DummyBuffer;
		int32 DummyChannels = 0;
		int32 DummySampleRate = 0;
		StopBatchRecording(DummyBuffer, DummyChannels, DummySampleRate);
	}

	// 3. Clean up the audio backend resources
	TeardownAudioBackEnd();

	// 4. Release the playback objects through the shared teardown path
	ReleasePlaybackObjects(AudioComponent, DuplicateSound);
	AudioComponent = nullptr;
	DuplicateSound = nullptr;

	bInitialized = false;
}

void FAudioRendererRealtime::RequestCancel()
{
	bCancelRequested = true;
	++RenderGeneration;
	OnVariationFinished.Unbind();
	OnRenderError.Unbind();
}

bool FAudioRendererRealtime::SetupAudioBackEnd(UWorld* InWorld)
{
	// 1. Store the injected world context
	PlaybackWorld = InWorld;
	if (!InWorld)
	{
		UE_LOG(LogAudioRendererRealtime, Error, TEXT("%s: No valid world was provided."), *FString(__FUNCTION__));
		return false;
	}

	// 2. Resolve the Mixer Device from the world context
	Audio::FMixerDevice* MixerDevice = GetMixerDevice();
	if (!MixerDevice)
	{
		UE_LOG(LogAudioRendererRealtime, Error, TEXT("%s: Failed to resolve the mixer device."), *FString(__FUNCTION__));
		return false;
	}

	ApplyAudioOverrides();

	RecordingSubmix = NewObject<USoundSubmix>(
		GetTransientPackage(),
		*FString::Printf(TEXT("AudioExporter_RecordingSubmix_%s"), *FGuid::NewGuid().ToString()),
		RF_Public);
	if (!RecordingSubmix)
	{
		UE_LOG(LogAudioRendererRealtime, Error, TEXT("%s: Failed to resolve the mixer recording submix."), *FString(__FUNCTION__));
		RestoreAudioOverrides();
		return false;
	}

	RecordingSubmix->AddToRoot();
	RecordingSubmix->SetParentSubmix(&MixerDevice->GetMainSubmixObject(), false);
	MixerDevice->RegisterSoundSubmix(RecordingSubmix, true);

	UE_LOG(LogAudioRendererRealtime, Log, TEXT("SetupAudioBackEnd: Recording realtime output from owned submix: %s"),
		*RecordingSubmix->GetName());

	return true;
}

void FAudioRendererRealtime::TeardownAudioBackEnd()
{
	// 1. Clear any active timers
	ClearRenderWatchdogTickers();
	if (PlaybackWorld.IsValid())
	{
		PlaybackWorld->GetTimerManager().ClearTimer(SeparatorTimerHandle);
	}
#if WITH_DEV_AUTOMATION_TESTS
	ClearForcedAttendedSeparatorTicker();
#endif

	if (RecordingSubmix)
	{
		RecordingSubmix->SetSubmixOutputVolume(PlaybackWorld.Get(), 1.0f);

		if (Audio::FMixerDevice* MixerDevice = GetMixerDevice())
		{
			MixerDevice->UnregisterSoundSubmix(RecordingSubmix, false);
		}
		RecordingSubmix->SetParentSubmix(nullptr, false);
		RecordingSubmix->RemoveFromRoot();
	}

	// 2. Reset pointers to external objects
	PlaybackWorld.Reset();
	RecordingSubmix = nullptr;
	RestoreAudioOverrides();
}

bool FAudioRendererRealtime::StartBatchRecording()
{
	if (!PlaybackWorld.IsValid() || !RecordingSubmix)
	{
		UE_LOG(LogAudioRendererRealtime, Warning, TEXT("%s: Batch recording could not start because the playback world or recording submix was invalid."), *FString(__FUNCTION__));
		return false;
	}

	Audio::FMixerDevice* MixerDevice = GetMixerDevice();
	if (!MixerDevice)
	{
		UE_LOG(LogAudioRendererRealtime, Error, TEXT("%s: Failed to resolve the mixer device for batch recording."), *FString(__FUNCTION__));
		return false;
	}

	// Resolve the audio clock before recording starts; a sentinel time means the mixer clock is
	// unavailable and a batch recorded against wall-clock markers would corrupt every variation.
	BatchStartTime = GetAudioTime();
	if (BatchStartTime < 0.0)
	{
		UE_LOG(LogAudioRendererRealtime, Error, TEXT("%s: Failed to resolve the audio device time; batch recording aborted."), *FString(__FUNCTION__));
		bInBatchRecording = false;
		return false;
	}

	MixerDevice->StartRecording(RecordingSubmix.Get(), 0.0f);
	bInBatchRecording = true;

	return true;
}

void FAudioRendererRealtime::StopBatchRecording(TArray<float>& OutBuffer, int32& OutNumChannels, int32& OutSampleRate)
{
	BatchEndTime = GetAudioTime();
	if (BatchEndTime < 0.0)
	{
		// The mixer disappeared mid-batch; keep the last valid time so duration math stays sane and
		// let the (empty) captured buffer surface as an explicit extraction failure downstream.
		BatchEndTime = BatchStartTime;
	}
	StopRecordingFromSubmix(OutBuffer, OutNumChannels, OutSampleRate);
	bInBatchRecording = false;
}

double FAudioRendererRealtime::GetLastRenderStartTime() const
{
	if (!bInBatchRecording)
	{
		return 0.0;
	}
	return RenderStartTime - BatchStartTime;
}

double FAudioRendererRealtime::GetLastRenderEndTime() const
{
	if (!bInBatchRecording)
	{
		return 0.0;
	}
	return RenderEndTime - BatchStartTime;
}

bool FAudioRendererRealtime::Render(USoundBase* InSound, float MaxDuration)
{
	const USoundWave* SoundWave = Cast<USoundWave>(InSound);
	if (!InSound || (SoundWave && (!FMath::IsFinite(SoundWave->Volume) || SoundWave->Volume < 0.0f)))
	{
		UE_LOG(LogAudioRendererRealtime, Error,
			TEXT("%s: Sound render rejected because authored linear Volume must be finite and non-negative."),
			*FString(__FUNCTION__));
		return false;
	}

	bCancelRequested = false;
	const uint32 CurrentRenderGeneration = ++RenderGeneration;

	// 1. Prepare the sound for playback and start the submix recording. Failure here is an expected
	// condition (invalid inputs, failed duplication), already logged by SetupSoundAndStartRecording;
	// it must not trip an assert/ensure -- the caller reports the render error itself.
	if (!SetupSoundAndStartRecording(InSound))
	{
		return false;
	}

	// 2. Apply monitoring (mute/unmute)
	SetMonitoring(bMonitor);

	// 3. Set up render timing
	CurrentMaxDuration = MaxDuration;
	RenderStartTime = 0.0; // Reset, will be set in OnAudioPlayStateChanged
	RenderEndTime = 0.0;
	bRenderStarted = false;

	// Every non-ordinary source gets the configured capture ceiling, even when an authored loop flag
	// says false. SoundCue and MetaSound graphs are procedural too; only an ordinary SoundWave gets
	// the long one-shot safety ceiling. Completion classification is assigned only by the callback
	// that actually ends this render.
	bExportingLoop = false;
	bCaptureLimitApplies = !(InSound->IsA<USoundWave>() && !InSound->IsA<USoundWaveProcedural>());

	// 4. Listen for the sound starting and finishing
	if (AudioComponent)
	{
		// Bind to the native play state change to know exactly when it starts
		AudioComponent->OnAudioPlayStateChangedNative.AddRaw(this, &FAudioRendererRealtime::OnAudioPlayStateChanged);

		AudioComponent->OnAudioFinishedNative.AddLambda([this, CurrentRenderGeneration](UAudioComponent*)
		{
			CompleteRender(CurrentRenderGeneration, false); // false = finished naturally on its own (the normal one-shot path)
		});

		// 5. Start the actual playback
		ClearRenderWatchdogTickers();
		StartTimeoutTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			TEXT("AudioExporter.RenderStartTimeout"),
			StartTimeoutSeconds,
			[this, CurrentRenderGeneration](float)
			{
				// One-shot: returning false releases the ticker entry. HandleStartTimeout's guards
				// (initialized/cancel/generation/bRenderStarted) keep a stale fire inert.
				HandleStartTimeout(CurrentRenderGeneration);
				return false;
			});
		AudioComponent->Play();
	}

	return true;
}

void FAudioRendererRealtime::OnAudioPlayStateChanged(const UAudioComponent* InComponent, EAudioComponentPlayState NewState)
{
	if (bCancelRequested || NewState != EAudioComponentPlayState::Playing || bRenderStarted)
	{
		return;
	}

	RenderStartTime = GetAudioTime();
	if (RenderStartTime < 0.0)
	{
		// No valid audio clock means the capture markers cannot be trusted; fail the render instead
		// of exporting a variation with fabricated timing.
		UE_LOG(LogAudioRendererRealtime, Error, TEXT("%s: Failed to resolve the audio device time when the sound started; failing the render."), *FString(__FUNCTION__));
		ReleasePlaybackObjects(AudioComponent, DuplicateSound);
		AudioComponent = nullptr;
		DuplicateSound = nullptr;
		if (OnRenderError.IsBound())
		{
			OnRenderError.Execute(TEXT("Failed to resolve the audio device time when the sound started."));
		}
		return;
	}

	// Ordinary SoundWave output is replaced at batch stop by the post-source-effect listener buffer.
	// That buffer begins at source frame zero and contains neither pre-playback setup nor the batch
	// separator. Rebase its batch clock here so the existing marker extraction maps [0, duration]
	// across the complete listener buffer instead of trimming the attack and effect tail.
	if (!bCaptureLimitApplies)
	{
		BatchStartTime = RenderStartTime;
	}

	bRenderStarted = true;
	UE_LOG(LogAudioRendererRealtime, Verbose, TEXT("Sound started playing at batch time: %f"), RenderStartTime - BatchStartTime);

	// Start the watchdog timer now that we actually started playing. The timer decides only whether
	// a configured capture ceiling applies. Natural-finish versus capture-limit classification is
	// assigned later from the observed completion event.
	if (!PlaybackWorld.IsValid())
	{
		return;
	}

	ClearRenderWatchdogTickers();
	const uint32 CurrentRenderGeneration = RenderGeneration;
	const float WatchdogDelaySeconds = ComputeWatchdogDelaySeconds(bCaptureLimitApplies, CurrentMaxDuration);
	// The capture ceiling is defined on RECORDED CONTENT -- ComputeWatchdogRenderEndTime expresses it in
	// AUDIO-CLOCK seconds -- so the trigger polls elapsed audio time instead of trusting a
	// wall-anchored one-shot. On headless gate hosts the mix clock advances faster than the wall
	// clock (measured: a 5 s cap produced a 9.958 s capture in a 5.09 s wall window,
	// deterministically across two runs), so a wall-anchored watchdog overshoots the documented
	// MaxLengthSeconds exactly there. On hosts where audio time tracks wall time this fires at the
	// same instant a 5.1 s loop would (within the 50 ms poll granularity, far below the 0.75 s
	// test tolerance).
	WatchdogTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		TEXT("AudioExporter.RenderWatchdog"),
		0.05f,
		[this, CurrentRenderGeneration, WatchdogDelaySeconds](float)
		{
			if (bCancelRequested || CurrentRenderGeneration != RenderGeneration || !bRenderStarted)
			{
				return false; // stale poll from a superseded render: release the entry
			}
			const double AudioTimeNow = GetAudioTime();
			if (AudioTimeNow < 0.0 || (AudioTimeNow - RenderStartTime) < WatchdogDelaySeconds)
			{
				return true; // keep polling until the audio-clock cap is reached
			}
			CompleteRender(CurrentRenderGeneration, true); // true = watchdog fired (see ComputeWatchdogDelaySeconds)
			return false;
		});
}

float FAudioRendererRealtime::ComputeWatchdogDelaySeconds(bool bCaptureLimitApplies, float MaxLengthSeconds)
{
	// Procedural and looping sources use the configured capture ceiling. Non-procedural one-shots
	// retain the long safety-only ceiling and still finish from their observed callback whenever it
	// arrives first.
	return bCaptureLimitApplies ? (MaxLengthSeconds + 0.1f) : OneShotSafetyTimeoutSeconds;
}

double FAudioRendererRealtime::ComputeWatchdogRenderEndTime(bool bCaptureLimitApplies, double RenderStartTime, float MaxLengthSeconds)
{
	return bCaptureLimitApplies
		? (RenderStartTime + static_cast<double>(MaxLengthSeconds))
		: (RenderStartTime + static_cast<double>(OneShotSafetyTimeoutSeconds));
}

void FAudioRendererRealtime::HandleStartTimeout(uint32 InRenderGeneration)
{
	if (!bInitialized || bCancelRequested || InRenderGeneration != RenderGeneration || bRenderStarted)
	{
		return;
	}

	UE_LOG(LogAudioRendererRealtime, Error, TEXT("%s: Audio component did not enter Playing within %.1f seconds."), *FString(__FUNCTION__), StartTimeoutSeconds);

	ReleasePlaybackObjects(AudioComponent, DuplicateSound);
	AudioComponent = nullptr;
	DuplicateSound = nullptr;

	if (OnRenderError.IsBound())
	{
		OnRenderError.Execute(TEXT("Timed out waiting for audio playback to start."));
	}
}

void FAudioRendererRealtime::SetMonitoring(bool bInMonitoring)
{
	bMonitor = bInMonitoring;
	
	if (RecordingSubmix)
	{
		// FMixerSubmix records before output volume is applied, so this controls monitoring
		// without muting the captured PCM.
		RecordingSubmix->SetSubmixOutputVolume(PlaybackWorld.Get(), bInMonitoring ? 1.0f : 0.0f);
		UE_LOG(LogAudioRendererRealtime, Log, TEXT("SetMonitoring: %s, Submix: %s"),
			bInMonitoring ? TEXT("Enabled") : TEXT("Disabled"), *RecordingSubmix->GetName());
	}
	else
	{
		UE_LOG(LogAudioRendererRealtime, Warning, TEXT("%s: RecordingSubmix was null during monitoring update."), *FString(__FUNCTION__));
	}

	if (AudioComponent)
	{
		AudioComponent->SetVolumeMultiplier(1.0f);
	}
}

void FAudioRendererRealtime::CompleteRender(uint32 InRenderGeneration, bool bIsTimeout)
{
	// 1. Early return and guard against re-entry
	if (!bInitialized || bCancelRequested || InRenderGeneration != RenderGeneration || !AudioComponent)
	{
		return;
	}

	// Capture and clear member pointers immediately to prevent double processing
	TObjectPtr<UAudioComponent> LocalAudioComponent = AudioComponent;
	TObjectPtr<USoundBase> LocalDuplicateSound = DuplicateSound;
	AudioComponent = nullptr;
	DuplicateSound = nullptr;

	UE_LOG(LogAudioRendererRealtime, Verbose, TEXT("CompleteRender started. bIsTimeout: %s"), bIsTimeout ? TEXT("True") : TEXT("False"));

	// 2. Unbind all events and clear timers.
	ClearRenderWatchdogTickers();
	LocalAudioComponent->OnAudioPlayStateChangedNative.RemoveAll(this);
	LocalAudioComponent->OnAudioFinishedNative.Clear();

	// 3. Calculate markers. The completion class is observed here from the event that won the
	// race: a watchdog means the configured ceiling was reached, while OnAudioFinished means the
	// source naturally finished. No pre-playback loop declaration is used as the result.

	if (!bRenderStarted)
	{
		// Fallback if the sound never actually started playing
		RenderStartTime = GetAudioTime();
		if (RenderStartTime < 0.0)
		{
			UE_LOG(LogAudioRendererRealtime, Error, TEXT("%s: Failed to resolve the audio device time for the finished render."), *FString(__FUNCTION__));
			ReleasePlaybackObjects(LocalAudioComponent, LocalDuplicateSound);
			if (OnRenderError.IsBound())
			{
				OnRenderError.Execute(TEXT("Failed to resolve the audio device time for the finished render."));
			}
			return;
		}
		RenderEndTime = RenderStartTime;
	}
	else if (bIsTimeout)
	{
		bExportingLoop = true;
		RenderEndTime = ComputeWatchdogRenderEndTime(bCaptureLimitApplies, RenderStartTime, CurrentMaxDuration);
		if (!bCaptureLimitApplies)
		{
			UE_LOG(LogAudioRendererRealtime, Warning, TEXT("%s: one-shot render did not finish naturally within the %.0fs safety ceiling; stopping and exporting what was captured. The source may never finish on its own."),
				*FString(__FUNCTION__), OneShotSafetyTimeoutSeconds);
		}
	}
	else
	{
		// The sound finished on its own (the common one-shot path; a well-formed loop should
		// never reach here since it never fires OnAudioFinished). The end time is now.
		RenderEndTime = GetAudioTime();
		if (RenderEndTime < 0.0)
		{
			UE_LOG(LogAudioRendererRealtime, Error, TEXT("%s: Failed to resolve the audio device time at the end of the render."), *FString(__FUNCTION__));
			ReleasePlaybackObjects(LocalAudioComponent, LocalDuplicateSound);
			if (OnRenderError.IsBound())
			{
				OnRenderError.Execute(TEXT("Failed to resolve the audio device time at the end of the render."));
			}
			return;
		}
	}

	// 4. Stop the sound playback
	LocalAudioComponent->Stop();

	UE_LOG(LogAudioRendererRealtime, Verbose, TEXT("CompleteRender: Audio stopped. Duration: %f"), RenderEndTime - RenderStartTime);

	// 5. Release the playback objects through the shared teardown path BEFORE finalizing the
	// variation. ReleasePlaybackObjects clears every pending render timer -- including
	// SeparatorTimerHandle -- so the previous order (arm the attended separator here, THEN route
	// through shared teardown) let the teardown kill the just-armed separator before the timer
	// manager ever ticked: OnVariationFinished never fired and an attended batch hung forever
	// after its first variation. Releasing first leaves every cancel/error path's
	// clearing semantics unchanged while guaranteeing the armed separator survives; shutdown and
	// cancellation still clear a pending separator explicitly (RequestCancel/Shutdown/
	// TeardownAudioBackEnd).
	ReleasePlaybackObjects(LocalAudioComponent, LocalDuplicateSound);

	if (bInBatchRecording)
	{
		// In batch mode, we wait for a separator silence before finishing this variation.
		// This ensures there is clear silence between variations in the continuous buffer.
		auto FinishBatchVariation = [this, InRenderGeneration]()
		{
			if (!bCancelRequested && InRenderGeneration == RenderGeneration && OnVariationFinished.IsBound())
			{
				OnVariationFinished.Execute();
			}
		};

#if WITH_DEV_AUTOMATION_TESTS
		// Test-only override: force the attended timer branch under
		// automation. When unset this reduces to exactly the production condition below, and in
		// Shipping the whole override compiles out.
		const bool bFinishSeparatorInline = !bForceAttendedSeparatorBranchForTesting && (FApp::IsUnattended() || !FApp::CanEverRender());
#else
		const bool bFinishSeparatorInline = FApp::IsUnattended() || !FApp::CanEverRender();
#endif
		if (bFinishSeparatorInline)
		{
			FinishBatchVariation();
		}
#if WITH_DEV_AUTOMATION_TESTS
		else if (bForceAttendedSeparatorBranchForTesting)
		{
			// The attended separator must wait on something that ticks in every
			// host. The world TimerManager below only advances inside UWorld::Tick, and the headless
			// automation host never ticks the injected editor world -- measured in the 16:27
			// AudioSuiteTests_UE5.6.log run: variation 1 rendered (SetMonitoring +
			// PrimeSoundCueWavePlayers logged exactly once), then 30 s of total silence with NO
			// separator fire AND no 10 s StartTimeout error either, i.e. no world timer fired while
			// frames [57]->[859] advanced. Every passing sibling completes via OnAudioFinished
			// (audio-device driven) plus the inline path above, so none of them ever proved a world
			// timer fires here -- which is why both arm/clear-order fixes changed nothing. The engine
			// core ticker IS ticked unconditionally per engine frame (UE 5.6 LaunchEngineLoop.cpp:5852,
			// FTSTicker::GetCoreTicker().Tick), independent of any world, making it the deterministic
			// anchor for this test-only branch. Production branches are untouched: this arm requires
			// the test-only flag, and real attended hosts tick their worlds.
			ClearForcedAttendedSeparatorTicker();
			ForcedAttendedSeparatorTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
				TEXT("AudioExporter.AttendedBatchSeparator"),
				SeparatorSilence,
				[this, InRenderGeneration, FinishBatchVariation](float)
				{
					// One-shot: returning false releases the ticker entry. FinishBatchVariation's
					// guards (cancel/generation/binding) keep a stale fire inert, exactly like the
					// world-timer path below.
					FinishBatchVariation();
					return false;
				});
		}
#endif
		else if (PlaybackWorld.IsValid())
		{
			PlaybackWorld->GetTimerManager().SetTimer(SeparatorTimerHandle, FTimerDelegate::CreateLambda(MoveTemp(FinishBatchVariation)), SeparatorSilence, false);
		}
	}
}

void FAudioRendererRealtime::ClearRenderWatchdogTickers()
{
	if (WatchdogTickerHandle.IsValid())
	{
		// RemoveTicker guarantees a removal racing the callback blocks until execution finishes,
		// so shutdown/cancel can never observe a post-mortem tick of these raw-this callbacks;
		// removing an already-fired one-shot entry is a safe no-op.
		FTSTicker::GetCoreTicker().RemoveTicker(WatchdogTickerHandle);
		WatchdogTickerHandle.Reset();
	}
	if (StartTimeoutTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(StartTimeoutTickerHandle);
		StartTimeoutTickerHandle.Reset();
	}
}

#if WITH_DEV_AUTOMATION_TESTS
void FAudioRendererRealtime::ClearForcedAttendedSeparatorTicker()
{
	if (ForcedAttendedSeparatorTickerHandle.IsValid())
	{
		// RemoveTicker guarantees a removal racing the callback blocks until execution finishes, so a
		// shutdown can never observe a post-mortem tick of this raw-this callback; removing an
		// already-fired one-shot entry is a safe no-op.
		FTSTicker::GetCoreTicker().RemoveTicker(ForcedAttendedSeparatorTickerHandle);
		ForcedAttendedSeparatorTickerHandle.Reset();
	}
}
#endif

void FAudioRendererRealtime::ReleasePlaybackObjects(UAudioComponent* InComponent, USoundBase* InDuplicateSound)
{
	ClearRenderWatchdogTickers();
	if (PlaybackWorld.IsValid())
	{
		PlaybackWorld->GetTimerManager().ClearTimer(SeparatorTimerHandle);
	}
#if WITH_DEV_AUTOMATION_TESTS
	ClearForcedAttendedSeparatorTicker();
#endif

	if (InComponent)
	{
		InComponent->OnAudioPlayStateChangedNative.RemoveAll(this);
		InComponent->OnAudioFinishedNative.Clear();
		InComponent->Stop();
		InComponent->RemoveFromRoot();
		InComponent->UnregisterComponent();
		InComponent->DestroyComponent();
	}

	if (InDuplicateSound)
	{
		UPackage* DuplicatePackage = InDuplicateSound->GetPackage();
		InDuplicateSound->RemoveFromRoot();

		// If it's in a temporary package we created, we should mark it for deletion
		if (DuplicatePackage && DuplicatePackage->GetName().StartsWith(TEXT("/Temp/AudioExporter/")))
		{
			DuplicatePackage->MarkAsGarbage();
		}
	}
}

bool FAudioRendererRealtime::SetupSoundAndStartRecording(USoundBase* InSound)
{
	bUseSourceBufferCapture = false;
	SourceBufferCapture.Reset();

	// Runtime build note: keep this duplication path free of editor-only transaction symbols.
	// 1. Early return if inputs are invalid or world context is missing
	if (!InSound || !PlaybackWorld.IsValid() || !RecordingSubmix)
	{
		UE_LOG(LogAudioRendererRealtime, Error, TEXT("%s: Sound setup failed because the sound, playback world, or recording submix was invalid."), *FString(__FUNCTION__));
		return false;
	}

	// 2. Create a new Audio Component to play the sound.
	// We use the world as the outer since this is a transient component without an actor owner.
	AudioComponent = NewObject<UAudioComponent>(PlaybackWorld.Get());
	if (!AudioComponent)
	{
		UE_LOG(LogAudioRendererRealtime, Error, TEXT("%s: Failed to create the transient audio component."), *FString(__FUNCTION__));
		return false;
	}
	
	// Ensure it doesn't get garbage collected during our process
	AudioComponent->AddToRoot();

	AudioComponent->bAutoActivate = false;
	AudioComponent->bAutoDestroy = false;
	AudioComponent->bAllowSpatialization = false;
	AudioComponent->bOverrideAttenuation = true;
	AudioComponent->AttenuationOverrides.bAttenuate = false;
	AudioComponent->AttenuationOverrides.bSpatialize = false;
	AudioComponent->AttenuationOverrides.bAttenuateWithLPF = false;
	AudioComponent->AttenuationOverrides.bEnableListenerFocus = false;
	AudioComponent->AttenuationOverrides.bEnableOcclusion = false;
	AudioComponent->AttenuationOverrides.bEnableReverbSend = false;
	AudioComponent->AttenuationOverrides.bEnableSourceDataOverride = false;
	AudioComponent->bAlwaysPlay = true;
	AudioComponent->ConcurrencySet.Empty();
	AudioComponent->SoundClassOverride = nullptr;
	AudioComponent->bIsUISound = true;
	AudioComponent->bReverb = false;
	AudioComponent->bCenterChannelOnly = false;
	// Preview sounds bypass FAudioDevice's one-shot source-effect-tail protection. This component is
	// already a UI sound, so it advances in unattended/editor worlds without using preview semantics;
	// keeping it non-preview lets bPlayEffectChainTails defer the engine completion callback until the
	// mixer has drained the authored source effects.
	AudioComponent->bIsPreviewSound = false;

	// 3. Duplicate the sound asset. 
	// This allows us to change its output submix for recording without affecting the original asset.
	
	// MetaSound Duplication Challenges:
	// Duplicating MetaSounds in the Editor requires special care.
	// 1. MetaSounds must be part of an "Asset" package to correctly initialize their internal resources.
	//    Standard transient packages (GetTransientPackage()) return false for IsAsset().
	// 2. To avoid crashing or failed registration, we create a temporary package that is NOT marked as transient.
	// 3. Keep the runtime duplication path free of editor-only transaction helpers.
	// 4. We manually call InitResources() to ensure the internal MetaSound graph is compiled and ready for playback.
	
	USoundBase* SoundToPlay = InSound;
	UPackage* DuplicatePackage = NewObject<UPackage>(nullptr, *FString::Printf(TEXT("/Temp/AudioExporter/%s"), *FGuid::NewGuid().ToString()), RF_Public);
	DuplicateSound = Cast<USoundBase>(StaticDuplicateObject(InSound, DuplicatePackage));
	
	if (DuplicateSound)
	{
		DuplicateSound->SetFlags(RF_Public);
		DuplicateSound->ClearFlags(RF_Transient);
		DuplicateSound->AddToRoot();
		DuplicateSound->VirtualizationMode = EVirtualizationMode::PlayWhenSilent;
		DuplicateSound->bOverrideConcurrency = true;
		DuplicateSound->ConcurrencySet.Empty();
		DuplicateSound->ConcurrencyOverrides = GetDefault<USoundWave>()->ConcurrencyOverrides;
#if UE_VERSION_OLDER_THAN(5, 7, 0)
		DuplicateSound->ConcurrencyOverrides.MaxCount = TNumericLimits<int32>::Max();
#else
		DuplicateSound->ConcurrencyOverrides.SetMaxCount(TNumericLimits<int32>::Max());
#endif
		DuplicateSound->AttenuationSettings = nullptr;

		// Redirect the duplicate's output to the recording submix without touching the source asset.
		DuplicateSound->SoundSubmixObject = RecordingSubmix.Get();
		// Lite deliberately excludes project SoundClass/SoundMix state, auxiliary bus sends,
		// downstream submix routing, attenuation, and concurrency. Keep the source effect chain and
		// source-local graph generation intact while making the duplicate's owned capture route the
		// only policy that can affect whether the selected source is heard.
		DuplicateSound->SoundClassObject = nullptr;
		DuplicateSound->bEnableBusSends = false;
		DuplicateSound->bEnableSubmixSends = false;
		DuplicateSound->SoundSubmixSends.Reset();
		DuplicateSound->BusSends.Reset();
		DuplicateSound->PreEffectBusSends.Reset();
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
		DuplicateSound->PostAttenuationBusSends.Reset();
#endif
		DuplicateSound->bEnableBaseSubmix = true;
		PrimeSoundCueWavePlayers(DuplicateSound, GetMixerDevice());
		UGameplayStatics::PrimeSound(DuplicateSound);

		// Ensure resources are initialized for the duplicated sound (crucial for MetaSounds)
		DuplicateSound->InitResources();
		SoundToPlay = DuplicateSound.Get();

		// This fires once per Render() call, i.e. per variation -- Verbose, not Log.
		UE_LOG(LogAudioRendererRealtime, Verbose, TEXT("SetupSoundAndStartRecording: Assigned submix %s to duplicate sound %s and initialized resources."),
			*RecordingSubmix->GetName(), *DuplicateSound->GetName());
	}
	else
	{
		// Duplication failed, so we cannot redirect this sound's output to the recording submix.
		// Playing the original sound here would capture nothing on the recording submix and export
		// a silent WAV with no error. Treat this as a render failure instead: returning false makes
		// Render() fail, which the pipeline surfaces through its OnRenderError path.
		UE_LOG(LogAudioRendererRealtime, Error, TEXT("%s: Failed to duplicate sound '%s' for submix-redirected recording; treating as a render error instead of exporting silence."),
			*FString(__FUNCTION__), *InSound->GetName());

		ReleasePlaybackObjects(AudioComponent, DuplicateSound);
		AudioComponent = nullptr;
		DuplicateSound = nullptr;
		return false;
	}

	AudioComponent->SetSound(SoundToPlay);

	// UE 5.8 processes authored source-effect tails after a SoundWave's dry source is done, but
	// FMixerSourceManager::MixOutputBuffers excludes bIsDone sources from every submix. Capture the
	// real post-source-effect buffer directly for the one source an ordinary SoundWave owns. This is
	// still the live mixer path and deliberately sits before the global routing stages Lite excludes.
	bUseSourceBufferCapture = InSound->IsA<USoundWave>() && !InSound->IsA<USoundWaveProcedural>();
	if (bUseSourceBufferCapture)
	{
		SourceBufferCapture = MakeShared<FAudioExporterSourceBufferCapture, ESPMode::ThreadSafe>();
		AudioComponent->SetSourceBufferListener(SourceBufferCapture, false);
	}

	// 4. Register the component so it's ready to play in the world.
	
	// Registering Components without Actors:
	// Normally, you would call AudioComponent->RegisterComponent(), but that requires
	// the component to have an Owner Actor. Since we created this component
	// with the World as its outer (to keep it transient and decoupled from any actor),
	// we must use RegisterComponentWithWorld() to manually associate it with the 
	// world's rendering and audio systems.
	AudioComponent->RegisterComponentWithWorld(PlaybackWorld.Get());

	// 5. Start the actual recording process on the submix
	// If we are in batch mode, the recording is already running
	if (bInBatchRecording)
	{
		return true;
	}

	Audio::FMixerDevice* MixerDevice = GetMixerDevice();
	if (!MixerDevice)
	{
		UE_LOG(LogAudioRendererRealtime, Error, TEXT("%s: Failed to resolve the mixer device before starting recording."), *FString(__FUNCTION__));
		ReleasePlaybackObjects(AudioComponent, DuplicateSound);
		AudioComponent = nullptr;
		DuplicateSound = nullptr;
		return false;
	}

	MixerDevice->StartRecording(RecordingSubmix.Get(), 0.0f);

	return true;
}

void FAudioRendererRealtime::StopRecordingFromSubmix(TArray<float>& OutBuffer, int32& OutNumChannels, int32& OutSampleRate)
{
	// 1. Early return if world or submix is missing
	if (!PlaybackWorld.IsValid() || !RecordingSubmix)
	{
		UE_LOG(LogAudioRendererRealtime, Warning, TEXT("%s: StopRecordingFromSubmix was called without a valid playback world or recording submix."), *FString(__FUNCTION__));
		return;
	}

	// 2. Resolve the mixer device
	Audio::FMixerDevice* MixerDevice = GetMixerDevice();
	if (!MixerDevice)
	{
		UE_LOG(LogAudioRendererRealtime, Error, TEXT("%s: Failed to resolve the mixer device while stopping submix recording."), *FString(__FUNCTION__));
		return;
	}

	// 3. Stop recording and retrieve the captured buffer
	float SampleRate = 0.0f;
	float NumChannels = 0.0f;
	Audio::FAlignedFloatBuffer& RecordedBuffer = MixerDevice->StopRecording(RecordingSubmix.Get(), NumChannels, SampleRate);

	// This fires once per batch stop (as frequent as per-variation with a 1-variation
	// config) -- Verbose, not Log.
	UE_LOG(LogAudioRendererRealtime, Verbose, TEXT("StopRecordingFromSubmix: Captured %d samples. Channels: %f, SampleRate: %f"), RecordedBuffer.Num(), NumChannels, SampleRate);

	// 4. Ordinary SoundWave capture comes from UE's post-source-effect listener so engine-processed
	// tails are not discarded by UE 5.8's done-source submix exclusion. Other source types keep the
	// continuous submix buffer because a graph may own more than one mixer source.
	if (bUseSourceBufferCapture)
	{
		const bool bCopiedSourceBuffer = SourceBufferCapture.IsValid()
			&& SourceBufferCapture->CopyCapturedAudio(OutBuffer, OutNumChannels, OutSampleRate);
		SourceBufferCapture.Reset();
		bUseSourceBufferCapture = false;
		if (!bCopiedSourceBuffer)
		{
			OutBuffer.Reset();
			OutNumChannels = 0;
			OutSampleRate = 0;
			UE_LOG(LogAudioRendererRealtime, Error,
				TEXT("%s: Ordinary SoundWave post-source-effect capture was empty or changed format."),
				*FString(__FUNCTION__));
		}
		return;
	}

	OutBuffer = RecordedBuffer;
	OutNumChannels = static_cast<int32>(NumChannels);
	OutSampleRate = static_cast<int32>(SampleRate);
}

double FAudioRendererRealtime::GetAudioTime() const
{
	Audio::FMixerDevice* MixerDevice = GetMixerDevice();
	if (!MixerDevice)
	{
		// Never fall back to the wall clock: mixing epoch-based and audio-clock timestamps would
		// silently corrupt every variation marker. Return the sentinel and let the caller fail.
		return InvalidAudioTime;
	}
	return MixerDevice->GetAudioTime();
}

Audio::FMixerDevice* FAudioRendererRealtime::GetMixerDevice() const
{
	if (!PlaybackWorld.IsValid())
	{
		return nullptr;
	}

	return FAudioDeviceManager::GetAudioMixerDeviceFromWorldContext(PlaybackWorld.Get());
}

void FAudioRendererRealtime::ApplyAudioOverrides()
{
	ApplyRealtimeAudioOverrides(bAppliedAudioOverrides);
}

void FAudioRendererRealtime::RestoreAudioOverrides()
{
	RestoreRealtimeAudioOverrides(bAppliedAudioOverrides);
}

#if WITH_DEV_AUTOMATION_TESTS
void FAudioRendererRealtime::ApplyAudioOverridesForTesting()
{
	ApplyRealtimeAudioOverrides(RealtimeAudioOverrideAppliedForTesting);
}

void FAudioRendererRealtime::RestoreAudioOverridesForTesting()
{
	RestoreRealtimeAudioOverrides(RealtimeAudioOverrideAppliedForTesting);
}
#endif
