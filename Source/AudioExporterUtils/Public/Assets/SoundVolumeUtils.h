// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"

#ifndef AF_UTILS_API
#define AF_UTILS_API
#endif

class USoundBase;

/**
 * Product-neutral helpers for getting/setting volume on UE sound assets.
 * Handles:
 * - USoundWave::Volume
 * - USoundCue::VolumeMultiplier
 * MetaSound sources do not expose a supported asset volume property here.
 */
namespace AudioExporter {
namespace SoundVolumeUtils
{
	/** Returns unset when the sound type or input is unavailable; a set 1.0f remains genuine unity volume. */
	AF_UTILS_API TOptional<float> GetSoundVolume(const USoundBase* InSound);
	/** Rejects non-finite or negative linear values before mutating the source asset. A rejection leaves Volume unchanged. */
	AF_UTILS_API bool SetSoundVolume(USoundBase* InSound, float InNewVolume);
	AF_UTILS_API bool ShouldPostEditChangeForVolumeEdit(const USoundBase* InSound);
	AF_UTILS_API void NotifySoundVolumeEdited(USoundBase* InSound, bool bMarkPackageDirty = true);
}
} // namespace AudioExporter
