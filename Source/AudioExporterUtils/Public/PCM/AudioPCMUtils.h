// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"

#ifndef AF_UTILS_API
#define AF_UTILS_API
#endif

class USoundWave;

namespace AudioExporter {
enum class EAudioPCMReadStatus : uint8
{
	Succeeded,
	InvalidSoundWave,
	WrongThread,
	ProceduralSoundWaveUnsupported,
	DataUnavailable,
	DecoderUnavailable,
	DecodeFailed,
	MalformedPCMData
};

/**
 * Utility namespace for reading PCM data from SoundWave assets.
 */
namespace AudioPCMUtils
{
	/** Reads resident, cooked compressed, or streamed PCM through the engine audio decoder. */
	AF_UTILS_API EAudioPCMReadStatus ReadSoundWavePCMData(
		USoundWave* InSoundWave,
		TArray<float>& OutSamples,
		int32& OutSampleRate,
		int32& OutNumChannels);

	/**
	 * Reads a SoundWave's imported or cooked/streamed audio and converts it to float samples.
	 * Must be called on the game thread.
	 *
	 * @param InSoundWave     Source SoundWave.
	 * @param OutSamples      Output interleaved float PCM buffer in range [-1, 1].
	 * @param OutSampleRate   Output sample rate.
	 * @param OutNumChannels  Output channel count.
	 */
	AF_UTILS_API bool GetSoundWavePCMData(
		USoundWave* InSoundWave,
		TArray<float>& OutSamples,
		int32& OutSampleRate,
		int32& OutNumChannels);
}
} // namespace AudioExporter
