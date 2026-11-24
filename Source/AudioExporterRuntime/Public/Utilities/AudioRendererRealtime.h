// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once
#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "HAL/ThreadSafeBool.h"
#include "UObject/WeakObjectPtr.h"
#include "UObject/ObjectPtr.h"
#include "Engine/EngineTypes.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundBase.h"

#include "UObject/GCObject.h"

class USoundBase;
class UWorld;
class UAudioComponent;
class USoundSubmix;
class FAudioExporterSourceBufferCapture;

DECLARE_DELEGATE(FOnVariationFinished);
DECLARE_DELEGATE_OneParam(FOnRenderError, FString /*ErrorMessage*/);

/**
 * FAudioRendererRealtime is responsible for playing and recording audio in real-time.
 * It uses an UAudioComponent to play the sound and records the output from a submix.
 * This is useful for assets like MetaSounds that need to be played in a world context to function correctly.
 */
class AUDIOEXPORTERRUNTIME_API FAudioRendererRealtime : public FGCObject
{
public:
	FAudioRendererRealtime() = default;
	virtual ~FAudioRendererRealtime();

	/** FGCObject interface */
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override
	{
		return TEXT("FAudioRendererRealtime");
	}

	/** 
	 * Initializes the renderer with an injected world context.
	 * @param InWorld - The world to use for audio playback. Must be valid.
	 * @return True if initialization was successful.
	 */
	bool Initialize(UWorld* InWorld);

	/** 
	 * Shuts down the renderer and releases any allocated resources.
	 */
	void Shutdown();

	/** Requests cooperative cancellation and fences any pending callbacks. */
	void RequestCancel();

	/**
	 * Starts a batch recording session. Subsequent calls to Render() will
	 * not start/stop individual recordings but will be captured in this session.
	 * @return True if the batch session was successfully started.
	 */
	virtual bool StartBatchRecording();

	/**
	 * Stops the current batch recording session and retrieves the full audio buffer.
	 * @param OutBuffer - The captured audio data.
	 * @param OutNumChannels - Number of channels in the buffer.
	 * @param OutSampleRate - Sample rate of the recording.
	 */
	virtual void StopBatchRecording(TArray<float>& OutBuffer, int32& OutNumChannels, int32& OutSampleRate);

	/** Returns the start time of the batch recording session. Virtual so automation can substitute a
	 * deterministic test double for FlushPendingVariations's salvage-extraction branch
	 * without a live audio device. Ordinary SoundWave listener capture rebases this to source frame
	 * zero when playback actually enters Playing. */
	virtual double GetBatchStartTime() const { return BatchStartTime; }

	/**
	 * Returns the end time on the same clock as GetBatchStartTime. Procedural sources use the
	 * continuous submix batch clock. Ordinary SoundWaves use the source-relative listener clock and
	 * therefore end at RenderEndTime, before the batch separator or stop overhead.
	 */
	virtual double GetBatchEndTime() const { return bCaptureLimitApplies ? BatchEndTime : RenderEndTime; }

	/** Returns the start time of the last render relative to the batch start. */
	double GetLastRenderStartTime() const;

	/** Returns the end time of the last render relative to the batch start. */
	double GetLastRenderEndTime() const;

	/** Returns true if a batch recording session is currently active. See GetBatchStartTime. */
	virtual bool IsInBatchRecording() const { return bInBatchRecording; }

	/** 
	 * Starts the real-time rendering (playback and recording) of a sound.
	 * @param InSound - The sound asset to render.
	 * @param MaxDuration - The maximum time to record for.
	 * @return True if the render process was successfully started.
	 */
	virtual bool Render(USoundBase* InSound, float MaxDuration);

	/** 
	 * Sets whether the audio being rendered should be audible to the user.
	 * @param bInMonitoring - True to enable monitoring.
	 */
	void SetMonitoring(bool bInMonitoring);

	/** Returns true if the renderer is initialized and ready. */
	bool IsInitialized() const { return bInitialized; }

	/** Returns the submix used for recording. */
	USoundSubmix* GetRecordingSubmix() const { return RecordingSubmix; }

	/** Returns true when the last render reached its capture ceiling instead of finishing naturally. */
	bool WasLastExportLooping() const { return bExportingLoop; }

	/**
	 * Safety ceiling, in seconds, for an ordinary SoundWave one-shot that never reaches its natural
	 * OnAudioFinished on its own. Procedural SoundCue and MetaSound sources use the configured
	 * MaxLengthSeconds capture ceiling on their first observed pass whether or not they are declared
	 * looping. This safety ceiling exists only for a pathological ordinary one-shot and is set far
	 * beyond any realistic render length. Public + static so automation can pin its value and the
	 * selection logic below without a live audio device.
	 */
	static constexpr float OneShotSafetyTimeoutSeconds = 600.0f;

	/**
	 * Chooses the watchdog delay, in seconds from render start. The boolean says only whether a
	 * configured capture ceiling applies, not whether the source will ultimately be classified as a
	 * loop. Procedural sources use the configured ceiling even when their loop flag is false; ordinary
	 * one-shots retain the long safety-only ceiling.
	 * @param bCaptureLimitApplies - Whether this source is procedural or otherwise configured to loop.
	 * @param MaxLengthSeconds - FAudioExporterConfig::MaxLengthSeconds.
	 */
	static float ComputeWatchdogDelaySeconds(bool bCaptureLimitApplies, float MaxLengthSeconds);

	/**
	 * Chooses the render end time (audio-clock seconds) for a render that the watchdog timer
	 * stopped. When a configured capture ceiling applies this is exactly MaxLengthSeconds. A
	 * non-procedural one-shot only reaches this path via the safety ceiling. See
	 * ComputeWatchdogDelaySeconds.
	 */
	static double ComputeWatchdogRenderEndTime(bool bCaptureLimitApplies, double RenderStartTime, float MaxLengthSeconds);

#if WITH_DEV_AUTOMATION_TESTS
	/**
	 * Test seam: forces the batch separator to take the ATTENDED timer branch regardless of
	 * FApp::IsUnattended()/FApp::CanEverRender(). Every automation run is unattended, which
	 * otherwise always completes the separator inline and leaves the attended armed-timer flow
	 * (the one shared teardown must not clear) unreachable headlessly. Pass false to restore
	 * production branch selection. Compiles out of Shipping entirely.
	 */
	static void SetForceAttendedSeparatorBranchForTesting(bool bInForce) { bForceAttendedSeparatorBranchForTesting = bInForce; }

	/**
	 * Test seam: runs the exact production override-lease implementation (the static
	 * ApplyRealtimeAudioOverrides/RestoreRealtimeAudioOverrides pair in AudioRendererRealtime.cpp)
	 * without an initialized renderer, so the shared AudioFactory.SharedOverride.* lease can be
	 * exercised headlessly. The pair must be applied/released in order, like the member versions.
	 * Compiled out of Shipping entirely.
	 */
	static void ApplyAudioOverridesForTesting();
	static void RestoreAudioOverridesForTesting();
#endif

	/** Delegate triggered when a variation finishes during a batch recording session. */
	FOnVariationFinished OnVariationFinished;

	/** Delegate triggered when an error occurs during rendering. */
	FOnRenderError OnRenderError;

	/** The duration of silence to include between variations. */
	static constexpr float SeparatorSilence = 0.5f;

protected:
	/** Internal setup for the audio engine and world context. */
	bool SetupAudioBackEnd(UWorld* InWorld);

	/** Internal teardown for the audio engine and world context. */
	void TeardownAudioBackEnd();

	/** Internal helper to prepare the sound and start recording from the submix. */
	bool SetupSoundAndStartRecording(USoundBase* InSound);

	/** Stops the recording and retrieves the audio buffer. */
	void StopRecordingFromSubmix(TArray<float>& OutBuffer, int32& OutNumChannels, int32& OutSampleRate);

private:
	/**
	 * Shared teardown for the per-render playback objects: clears the render timers, unbinds the
	 * audio component callbacks, destroys the component, and releases the duplicated sound. Every
	 * render-completion and error path routes through this single helper so no branch can leak or
	 * double-release the objects.
	 */
	void ReleasePlaybackObjects(UAudioComponent* InComponent, USoundBase* InDuplicateSound);
	/**
	 * Finalizes the render process, gathers the recorded data, and triggers the finished delegate.
	 * @param bIsTimeout - True if the render was stopped by the watchdog timer. This observed event
	 *        becomes the capture-limit completion class; a natural audio-finished callback becomes
	 *        the natural completion class.
	 */
	void CompleteRender(uint32 InRenderGeneration, bool bIsTimeout);

	/** Callback for when the audio component starts playing. */
	void OnAudioPlayStateChanged(const UAudioComponent* InComponent, EAudioComponentPlayState NewState);

	/** Fails a render that never reaches the Playing state. */
	void HandleStartTimeout(uint32 InRenderGeneration);

	/** Returns the current audio device time in seconds, or InvalidAudioTime when the mixer device is unavailable. */
	double GetAudioTime() const;

	/** Helper to get the mixer device from the current world context. */
	class Audio::FMixerDevice* GetMixerDevice() const;

	/** Applies audio CVars needed for unattended realtime playback, and restores them on teardown.
	 *  Both delegate to the static file-local lease implementation; the shared lease bookkeeping
	 *  lives in AudioFactory.SharedOverride.* console variables (see AudioRendererRealtime.cpp). */
	void ApplyAudioOverrides();
	void RestoreAudioOverrides();

	/** Removes any pending watchdog/start-timeout core-ticker entry (safe no-op when unset). */
	void ClearRenderWatchdogTickers();

	/**
	 * Engine core-ticker handle for the render watchdog (the safety timer that stops recording if
	 * the sound doesn't finish on its own). Anchored on FTSTicker -- like
	 * ForcedAttendedSeparatorTickerHandle below -- rather than the world TimerManager: headless
	 * gate hosts never tick the injected editor world, so world timers never fire there and LOOP
	 * captures hung forever with the covering gate test self-skipping.
	 */
	FTSTicker::FDelegateHandle WatchdogTickerHandle;

	/** Engine core-ticker handle for the startup timer that catches sounds that never start playing. See WatchdogTickerHandle. */
	FTSTicker::FDelegateHandle StartTimeoutTickerHandle;
	
	/** Handle for the silence between variations. */
	FTimerHandle SeparatorTimerHandle;

	/**
	 * Engine core-ticker handle for the test-only forced-attended separator (armed in
	 * CompleteRender under SetForceAttendedSeparatorBranchForTesting; see the CLOSE-C execution
	 * record for why the attended anchor cannot be the world TimerManager). Deliberately
	 * UNconditional, mirroring AudioExportPipeline's bForceAsyncSaveForTesting: a
	 * WITH_DEV_AUTOMATION_TESTS-gated data member would change object layout between translation
	 * units that disagree on the define. Only its USES are gated -- production paths never touch it.
	 */
	FTSTicker::FDelegateHandle ForcedAttendedSeparatorTickerHandle;

#if WITH_DEV_AUTOMATION_TESTS
	/** Test-only: removes a pending forced-attended separator ticker so shutdown/cancel/teardown
	 *  cannot leave a raw-this callback registered after this renderer dies (safe no-op when unset). */
	void ClearForcedAttendedSeparatorTicker();
#endif

	/** Maximum wall-clock time to wait for the audio component to enter Playing. */
	static constexpr float StartTimeoutSeconds = 10.0f;

	/** The time when the current render started. */
	double RenderStartTime = 0.0;

	/**
	 * Whether the current render actually reached the Playing state. This cannot be inferred from
	 * RenderStartTime: GetAudioTime() legitimately returns 0.0 on a freshly created mixer device.
	 */
	bool bRenderStarted = false;

	/** The time when the current render ended. */
	double RenderEndTime = 0.0;

	/** The maximum allowed duration for the current render. */
	float CurrentMaxDuration = 0.0f;

	/** Whether monitoring is enabled. */
	bool bMonitor = false;

	/** Initialization state flag. */
	bool bInitialized = false;

	/** Set while shutdown/cancel is invalidating callbacks from the current render. */
	FThreadSafeBool bCancelRequested = false;

	/** Incremented whenever callbacks from a previous render should be ignored. */
	uint32 RenderGeneration = 0;

	/** Whether the current render reached its capture ceiling, assigned from the observed completion. */
	bool bExportingLoop = false;

	/** Whether the configured capture ceiling applies to this render. */
	bool bCaptureLimitApplies = false;

	/** Weak pointer to the world used for playback. */
	TWeakObjectPtr<UWorld> PlaybackWorld;

	/** The audio component used to play the sound. */
	TObjectPtr<UAudioComponent> AudioComponent;

	/** A duplicate of the sound asset to ensure we don't modify the original. */
	TObjectPtr<USoundBase> DuplicateSound;

	/** Whether a batch recording session is currently active. */
	bool bInBatchRecording = false;

	/**
	 * The start of the current capture buffer's timebase. Procedural sources keep the continuous
	 * submix batch start. Ordinary SoundWave listener capture rebases this to RenderStartTime so its
	 * source-relative PCM is never sliced with setup or separator time.
	 */
	double BatchStartTime = 0.0;

	/** The time when the continuous submix batch recording session ended. */
	double BatchEndTime = 0.0;

	/** The transient submix used for the realtime recording path (owned and rooted by this renderer). */
	TObjectPtr<USoundSubmix> RecordingSubmix;

	/**
	 * Ordinary SoundWave capture taken from UE's post-source-effect buffer listener. UE 5.8 keeps
	 * processing source-effect tails after the dry source is done, but excludes done sources from
	 * submix mixing. A SoundWave has exactly one mixer source, so this listener preserves that real
	 * mixer output, including authored volume, pitch, source effects, and tails, before the excluded
	 * attenuation/spatialization/reverb/project/master stages. Procedural and graph sources continue
	 * through the owned recording submix because they may fan out to multiple mixer sources.
	 */
	TSharedPtr<FAudioExporterSourceBufferCapture, ESPMode::ThreadSafe> SourceBufferCapture;
	bool bUseSourceBufferCapture = false;

	/** Sentinel audio time returned when the mixer device is unavailable; never wall-clock time. */
	static constexpr double InvalidAudioTime = -1.0;

	/** Set once a renderer holds the realtime CVar override lease; restored on teardown. */
	bool bAppliedAudioOverrides = false;

#if WITH_DEV_AUTOMATION_TESTS
	/** Test-only: when true the batch separator takes the attended timer branch. See
	 *  SetForceAttendedSeparatorBranchForTesting. Static (no object layout impact) and compiled
	 *  out of Shipping entirely. */
	static bool bForceAttendedSeparatorBranchForTesting;
#endif
};
