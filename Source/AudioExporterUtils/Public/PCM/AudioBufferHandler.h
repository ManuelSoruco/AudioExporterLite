// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"

#ifndef AF_UTILS_API
#define AF_UTILS_API
#endif

/**
 * Explicit speaker identity for layouts that require WAVE_FORMAT_EXTENSIBLE.
 *
 * Multichannel buffers use Unreal source order:
 * - Quad: FL, FR, SL, SR
 * - Surround5_1: FL, FR, FC, LFE, SL, SR
 * - Surround7_1: FL, FR, FC, LFE, SL, SR, BL, BR
 *
 * WAVE_FORMAT_EXTENSIBLE stores channels in ascending speaker-mask bit order. The WAV writer
 * therefore permutes Surround7_1 to FL, FR, FC, LFE, BL, BR, SL, SR during encoding. Layout
 * identity is never inferred from channel count alone.
 */
namespace AudioExporter {
enum class EAudioChannelLayout : uint8
{
	Unknown,
	Mono,
	Stereo,
	Quad,
	Surround5_1,
	Surround7_1,
	Ambisonic,
};

enum class EAudioWavSaveFailureReason : uint8
{
	None,
	InvalidInput,
	Unsupported,
	DestinationUnavailable,
	WriteFailed,
	ReadbackValidationFailed,
	AtomicPublicationFailed,
};

/*
 * This cap-direction type intentionally remains distinct from LUFS's EAudioGainLimitDirection.
 * The value mapping is None -> None and Upper -> Boost: Utils only caps positive gain and has no
 * attenuation limit. Utils cannot include the LUFS-owned enum because their generated alias roots
 * are not available in the same products.
 */
enum class EAudioBufferGainLimitDirection : uint8
{
	None,
	Upper,
};

struct AF_UTILS_API FAudioGainApplicationResult
{
	float RequestedGainDb = 0.0f;
	float AppliedGainDb = 0.0f;
	bool bWasLimited = false;
	EAudioBufferGainLimitDirection LimitDirection = EAudioBufferGainLimitDirection::None;
	bool bSucceeded = false;
};

/**
 * FAudioBufferHandler
 *
 * Holds interleaved finite float PCM samples, including legitimate overs outside [-1.0, 1.0], and provides
 * common destructive edits and analysis helpers:
 * - fixed trimming
 * - silence trimming
 * - gain in dB (capped at +12 dB; see ApplyGainDb)
 * - peak / RMS / duration queries
 * - WAV file saving (16-bit PCM, 24-bit PCM, 32-bit float)
 *
 * Non-finite (NaN/Inf) input samples are scrubbed to silence in LoadBuffer.
 * Channel order is standard interleaved and follows the explicit EAudioChannelLayout contract above.
 * Not thread-safe.
 */
class AF_UTILS_API FAudioBufferHandler
{
public:
	struct FSaveResult
	{
		bool bSucceeded = false;
		FString ErrorMessage;
		// The actual claimed path, which may have a collision suffix.
		FString SavedFilePath;
		// True when the failure is a property of the DESTINATION rather than of this one filename:
		// the output directory, temporary path allocation, or atomic claim is unavailable. A
		// per-file write failure and a name collision are never this. Batch callers use it to stop
		// early instead of repeating a doomed destination operation once per remaining asset.
		bool bDestinationUnusable = false;
		// Typed failure lets callers distinguish unsupported layout/RIFF from destination failures.
		EAudioWavSaveFailureReason FailureReason = EAudioWavSaveFailureReason::None;
	};

	// Product loudness policy: Exporters peak-normalize to -1 dBFS; Converter uses a user threshold; Normalizer uses LUFS + dBTP.
	static constexpr float PeakNormalizeTargetDb = -1.0f;
	static constexpr float PeakNormalizeSilenceFloorLinear = 0.001f;

	/** Compatibility overload. Layout remains unknown until downstream product callers wire explicit identity. */
	void LoadBuffer(const TArray<float>& InBuffer, int32 InSampleRate, int32 InNumChannels);
	/** Loads finite interleaved PCM with explicit layout identity; unknown multichannel layouts remain unsupported for WAV. */
	void LoadBuffer(const TArray<float>& InBuffer, int32 InSampleRate, int32 InNumChannels, EAudioChannelLayout InChannelLayout);
	void TrimFixedInOut(float InStartSec, float InEndSec);
	void TrimSilenceInOut(float SilenceThresholdDb);
	// Applies gain, capped at +12 dB (4x linear). Returns the gain ACTUALLY applied in dB
	// (0 dB if the buffer is empty or the requested gain is invalid), so callers can log the truth.
	float ApplyGainDb(float GainDb);
	FAudioGainApplicationResult ApplyGainDbWithResult(float GainDb);
	// Returns the gain actually applied (capped), or 0 dB if the buffer is at/under the silence floor.
	float NormalizeToPeakDb(float TargetDb = PeakNormalizeTargetDb);
	FAudioGainApplicationResult NormalizeToPeakDbWithResult(float TargetDb = PeakNormalizeTargetDb);

	const TArray<float>& GetBuffer() const { return AudioBuffer; }
	// Returns unset when the requested metric is unavailable; a set zero remains genuine silence.
	TOptional<float> GetPeakLevel() const;
	TOptional<float> GetRMSLevel() const;
	TOptional<float> GetDurationSec() const;
	bool SaveToWav(int32 BitDepth, const FString& FilePath) const;
	FSaveResult SaveToWavWithResult(int32 BitDepth, const FString& FilePath) const;

	int32 GetSampleRate() const { return SampleRate; }
	int32 GetNumChannels() const { return NumChannels; }
	int32 GetNumFrames() const { return NumChannels > 0 ? AudioBuffer.Num() / NumChannels : 0; }

private:
	static float LinearFromDb(float Db);
	static float DbFromLinear(float Linear);
	static void WriteInt16(TArray<uint8>& Out, int16 Value);
	static void WriteInt32(TArray<uint8>& Out, int32 Value);
	static void WriteUInt16(TArray<uint8>& Out, uint16 Value);
	static void WriteUInt32(TArray<uint8>& Out, uint32 Value);
	static void WriteFloat32(TArray<uint8>& Out, float Value);
	static void WritePcm24(TArray<uint8>& Out, float Sample);
	static bool TryGetExtensibleChannelMask(EAudioChannelLayout InChannelLayout, int32 InNumChannels, uint32& OutChannelMask);
	static void WriteSubFormatGuid(TArray<uint8>& Out, bool bFloat32);

	TArray<float> AudioBuffer;
	int32 SampleRate = 48000;
	int32 NumChannels = 2;
	EAudioChannelLayout ChannelLayout = EAudioChannelLayout::Unknown;
};
} // namespace AudioExporter
