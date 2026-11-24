// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "AudioExporterConfig.h"

#include "Utilities/AudioExporterLogger.h"

void FAudioExporterConfig::Sanitize()
{
	const int32 RequestedOneShotVariations = OneShotVariations;
	const int32 RequestedLoopVariations = LoopVariations;
	const int32 RequestedMaxLengthSeconds = MaxLengthSeconds;

	OneShotVariations = FMath::Clamp(OneShotVariations, 1, 5);
	LoopVariations = FMath::Clamp(LoopVariations, 1, 5);
	MaxLengthSeconds = FMath::Clamp(MaxLengthSeconds, 5, 60);

	if (OneShotVariations != RequestedOneShotVariations
		|| LoopVariations != RequestedLoopVariations
		|| MaxLengthSeconds != RequestedMaxLengthSeconds)
	{
		UE_LOG(LogAudioExporter, Warning,
		       TEXT("%s: Export config values were outside their supported ranges and have been clamped. OneShotVariations %d -> %d, LoopVariations %d -> %d, MaxLengthSeconds %d -> %d."),
		       *FString(__FUNCTION__),
		       RequestedOneShotVariations, OneShotVariations,
		       RequestedLoopVariations, LoopVariations,
		       RequestedMaxLengthSeconds, MaxLengthSeconds);
	}
}
