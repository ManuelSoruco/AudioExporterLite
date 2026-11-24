// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "AudioMetricListener.generated.h"

DECLARE_MULTICAST_DELEGATE_OneParam(FOnEnvelopeValue, const TArray<float>&);

/**
 * UAudioMetricListener
 * 
 * A simple UObject used to bridge the audio thread's envelope following data
 * to the game thread (UI). It receives amplitude data from a submix and 
 * broadcasts it via a delegate.
 */
UCLASS()
class AUDIOEXPORTERUI_API UAudioMetricListener : public UObject
{
	GENERATED_BODY()

public:
	/** Delegate that broadcasts the latest envelope values to listeners (e.g., UI meters) */
	FOnEnvelopeValue OnEnvelopeValue;

	/** 
	 * Callback function registered with the SoundSubmix's envelope follower.
	 * This is called on the audio thread and then routed to the UI.
	 */
	UFUNCTION()
	void OnSubmixEnvelope(const TArray<float>& Envelope)
	{
		OnEnvelopeValue.Broadcast(Envelope);
	}
};
