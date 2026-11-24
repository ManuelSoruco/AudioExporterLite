// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "AudioExportPipeline.h"
#include "Sound/SoundEffectSource.h"

#include "AudioExporterTestResultSink.generated.h"

/**
 * Minimal automation sink for the AudioExporter runtime tests. Dynamic BP-style delegates cannot
 * bind lambdas or plain functions (they require a UFUNCTION on a UObject), so this UObject provides
 * the UFUNCTION the test delegates bind to when exercising the real BP-library one-shot result
 * callback (ExportSoundAsyncWithResult) end to end.
 */
UCLASS()
class UAudioExporterTestResultSink : public UObject
{
	GENERATED_BODY()

public:
	/** Receives the structured terminal result from a bound FOnExportCompleteResult delegate. */
	UFUNCTION()
	void HandleExportResult(const FAudioExportResult& InResult);

	/** The last result received through HandleExportResult. */
	FAudioExportResult LastResult;

	/** True once at least one result was received. */
	bool bReceivedResult = false;
};

/** Settings for the deterministic source-effect tail used by the real Lite renderer regression. */
USTRUCT()
struct FAudioExporterAutomationTailSettings
{
	GENERATED_BODY()

	UPROPERTY()
	float TailSeconds = 0.25f;

	UPROPERTY()
	float TailAmplitude = 0.1f;

	UPROPERTY()
	float PilotAmplitude = 0.1f;
};

/** Real audio-thread source effect with a deterministic, bounded audible tail. */
class FAudioExporterAutomationTail : public FSoundEffectSource
{
public:
	virtual void Init(const FSoundEffectSourceInitData& InitData) override;
	virtual void OnPresetChanged() override;
	virtual void ProcessAudio(const FSoundEffectSourceInputData& InData, float* OutAudioBufferData) override;

private:
	FAudioExporterAutomationTailSettings RenderSettings;
	int32 NumChannels = 1;
	int32 TailFramesTotal = 1;
	int32 TailFramesRemaining = 0;
	float TailPhase = 0.0f;
	float TailPhaseIncrement = 0.0f;
	bool bSawSourceSignal = false;
};

/** Test-only preset that lets the engine instantiate FAudioExporterAutomationTail normally. */
UCLASS()
class UAudioExporterAutomationTailPreset : public USoundEffectSourcePreset
{
	GENERATED_BODY()

public:
	EFFECT_PRESET_METHODS(AudioExporterAutomationTail)

	UPROPERTY()
	FAudioExporterAutomationTailSettings Settings;
};
