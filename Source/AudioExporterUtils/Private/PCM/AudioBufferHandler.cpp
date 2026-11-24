// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "PCM/AudioBufferHandler.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
namespace AudioExporter {

DEFINE_LOG_CATEGORY_STATIC(LogAFAudioBufferHandler, Log, All);

namespace
{
	constexpr int32 Surround71WavToUnrealChannelOrder[8] = { 0, 1, 2, 3, 6, 7, 4, 5 };
}

float FAudioBufferHandler::LinearFromDb(float Db)
{
	return FMath::Pow(10.0f, Db / 20.0f);
}

float FAudioBufferHandler::DbFromLinear(float Linear)
{
	return 20.0f * FMath::LogX(10.0f, FMath::Max(1.0e-8f, Linear));
}

void FAudioBufferHandler::WriteInt16(TArray<uint8>& Out, int16 Value)
{
	Out.Append(reinterpret_cast<const uint8*>(&Value), sizeof(int16));
}

void FAudioBufferHandler::WriteInt32(TArray<uint8>& Out, int32 Value)
{
	Out.Append(reinterpret_cast<const uint8*>(&Value), sizeof(int32));
}

void FAudioBufferHandler::WriteUInt16(TArray<uint8>& Out, uint16 Value)
{
	// Little-endian by value (Win64-only target); avoids reinterpreting a wider int as uint16.
	Out.Add(static_cast<uint8>(Value & 0xFF));
	Out.Add(static_cast<uint8>((Value >> 8) & 0xFF));
}

void FAudioBufferHandler::WriteUInt32(TArray<uint8>& Out, uint32 Value)
{
	Out.Append(reinterpret_cast<const uint8*>(&Value), sizeof(uint32));
}

void FAudioBufferHandler::WriteFloat32(TArray<uint8>& Out, float Value)
{
	Out.Append(reinterpret_cast<const uint8*>(&Value), sizeof(float));
}

void FAudioBufferHandler::WritePcm24(TArray<uint8>& Out, float Sample)
{
	const float Clamped = FMath::Clamp(Sample, -1.0f, 1.0f);
	const int32 PCM24 = FMath::RoundToInt(Clamped * 8388607.0f);
	Out.Add(static_cast<uint8>((PCM24 >> 0) & 0xFF));
	Out.Add(static_cast<uint8>((PCM24 >> 8) & 0xFF));
	Out.Add(static_cast<uint8>((PCM24 >> 16) & 0xFF));
}

bool FAudioBufferHandler::TryGetExtensibleChannelMask(EAudioChannelLayout InChannelLayout, int32 InNumChannels, uint32& OutChannelMask)
{
	// These masks match the explicit Unreal speaker identities carried by the renderer:
	// quad FL|FR|SL|SR, 5.1 adds FC|LFE, and 7.1 adds BL|BR. Never infer them from count alone.
	switch (InChannelLayout)
	{
	case EAudioChannelLayout::Quad:
		if (InNumChannels == 4)
		{
			OutChannelMask = 0x00000603u;
			return true;
		}
		break;
	case EAudioChannelLayout::Surround5_1:
		if (InNumChannels == 6)
		{
			OutChannelMask = 0x0000060Fu;
			return true;
		}
		break;
	case EAudioChannelLayout::Surround7_1:
		if (InNumChannels == 8)
		{
			OutChannelMask = 0x0000063Fu;
			return true;
		}
		break;
	default:
		break;
	}

	return false;
}

void FAudioBufferHandler::WriteSubFormatGuid(TArray<uint8>& Out, bool bFloat32)
{
	// KSDATAFORMAT_SUBTYPE_PCM and KSDATAFORMAT_SUBTYPE_IEEE_FLOAT share this GUID layout.
	WriteUInt32(Out, bFloat32 ? 3u : 1u);
	WriteUInt16(Out, 0);
	WriteUInt16(Out, 0x0010u);
	Out.Add(0x80);
	Out.Add(0x00);
	Out.Add(0x00);
	Out.Add(0xAA);
	Out.Add(0x00);
	Out.Add(0x38);
	Out.Add(0x9B);
	Out.Add(0x71);
}

void FAudioBufferHandler::LoadBuffer(const TArray<float>& InBuffer, int32 InSampleRate, int32 InNumChannels)
{
	LoadBuffer(InBuffer, InSampleRate, InNumChannels, EAudioChannelLayout::Unknown);
}

void FAudioBufferHandler::LoadBuffer(const TArray<float>& InBuffer, int32 InSampleRate, int32 InNumChannels, EAudioChannelLayout InChannelLayout)
{
	if (!ensure(InSampleRate > 0 && InNumChannels > 0))
	{
		UE_LOG(LogAFAudioBufferHandler, Error, TEXT("LoadBuffer: invalid sample rate (%d) or channel count (%d)."), InSampleRate, InNumChannels);
		AudioBuffer.Empty();
		ChannelLayout = EAudioChannelLayout::Unknown;
		return;
	}

	SampleRate = InSampleRate;
	NumChannels = InNumChannels;
	ChannelLayout = InChannelLayout;

	if (InBuffer.IsEmpty())
	{
		UE_LOG(LogAFAudioBufferHandler, Warning, TEXT("LoadBuffer: input buffer is empty."));
		AudioBuffer.Empty();
		return;
	}

	if ((InBuffer.Num() % InNumChannels) != 0)
	{
		UE_LOG(LogAFAudioBufferHandler, Error, TEXT("LoadBuffer: buffer size (%d) not divisible by channels (%d)."), InBuffer.Num(), InNumChannels);
		AudioBuffer.Empty();
		ChannelLayout = EAudioChannelLayout::Unknown;
		return;
	}

	AudioBuffer = InBuffer;

	// Scrub non-finite samples (NaN/Inf) at the single ingestion point. A single NaN otherwise
	// poisons peak/RMS/gain math (NaN propagates through the whole buffer) and would otherwise be
	// written as invalid WAV sample bytes. Replace with silence and log once.
	int32 NonFiniteCount = 0;
	for (float& Sample : AudioBuffer)
	{
		if (!FMath::IsFinite(Sample))
		{
			Sample = 0.0f;
			++NonFiniteCount;
		}
	}
	if (NonFiniteCount > 0)
	{
		UE_LOG(LogAFAudioBufferHandler, Warning, TEXT("LoadBuffer: scrubbed %d non-finite (NaN/Inf) sample(s) to silence."), NonFiniteCount);
	}
}

void FAudioBufferHandler::TrimFixedInOut(float InStartSec, float InEndSec)
{
	if (AudioBuffer.IsEmpty() || SampleRate <= 0 || NumChannels <= 0)
	{
		return;
	}

	if (!FMath::IsFinite(InStartSec) || !FMath::IsFinite(InEndSec))
	{
		UE_LOG(LogAFAudioBufferHandler, Error, TEXT("TrimFixedInOut: non-finite trim duration was rejected."));
		return;
	}

	const float StartSec = FMath::Max(0.0f, InStartSec);
	const float EndSec = FMath::Max(0.0f, InEndSec);
	const double TrimFramesInExact = static_cast<double>(StartSec) * static_cast<double>(SampleRate);
	const double TrimFramesOutExact = static_cast<double>(EndSec) * static_cast<double>(SampleRate);
	if (TrimFramesInExact > static_cast<double>(MAX_int32)
		|| TrimFramesOutExact > static_cast<double>(MAX_int32))
	{
		UE_LOG(LogAFAudioBufferHandler, Error, TEXT("TrimFixedInOut: trim duration exceeds the supported frame range."));
		return;
	}

	const int32 TrimFramesIn = FMath::RoundToInt(StartSec * SampleRate);
	const int32 TrimFramesOut = FMath::RoundToInt(EndSec * SampleRate);
	const int32 TotalFrames = AudioBuffer.Num() / NumChannels;
	const int64 NewFrames = static_cast<int64>(TotalFrames) - static_cast<int64>(TrimFramesIn) - static_cast<int64>(TrimFramesOut);
	if (NewFrames <= 0)
	{
		UE_LOG(LogAFAudioBufferHandler, Warning, TEXT("TrimFixedInOut: trim amounts clear the whole buffer."));
		AudioBuffer.Empty();
		return;
	}

	const int32 TrimSamplesIn = TrimFramesIn * NumChannels;
	const int32 TrimSamplesOut = TrimFramesOut * NumChannels;

	if (TrimSamplesIn > 0)
	{
		AudioBuffer.RemoveAt(0, TrimSamplesIn, EAllowShrinking::No);
	}
	if (TrimSamplesOut > 0)
	{
		const int32 TailToRemove = FMath::Min(TrimSamplesOut, AudioBuffer.Num());
		AudioBuffer.RemoveAt(AudioBuffer.Num() - TailToRemove, TailToRemove, EAllowShrinking::No);
	}
}

void FAudioBufferHandler::TrimSilenceInOut(float SilenceThresholdDb)
{
	if (AudioBuffer.IsEmpty() || SampleRate <= 0 || NumChannels <= 0)
	{
		return;
	}

	if (!FMath::IsFinite(SilenceThresholdDb))
	{
		UE_LOG(LogAFAudioBufferHandler, Error, TEXT("TrimSilenceInOut: non-finite silence threshold was rejected."));
		return;
	}

	const float ThresholdLinear = LinearFromDb(SilenceThresholdDb);
	if (!FMath::IsFinite(ThresholdLinear))
	{
		UE_LOG(LogAFAudioBufferHandler, Error, TEXT("TrimSilenceInOut: silence threshold produced a non-finite linear value."));
		return;
	}
	const int32 TotalFrames = AudioBuffer.Num() / NumChannels;

	int32 StartFrame = 0;
	for (; StartFrame < TotalFrames; ++StartFrame)
	{
		float FramePeak = 0.0f;
		const int32 Base = StartFrame * NumChannels;
		for (int32 Ch = 0; Ch < NumChannels; ++Ch)
		{
			FramePeak = FMath::Max(FramePeak, FMath::Abs(AudioBuffer[Base + Ch]));
		}
		if (FramePeak > ThresholdLinear)
		{
			break;
		}
	}

	if (StartFrame >= TotalFrames)
	{
		AudioBuffer.Empty();
		return;
	}

	int32 EndFrame = TotalFrames - 1;
	for (; EndFrame >= StartFrame; --EndFrame)
	{
		float FramePeak = 0.0f;
		const int32 Base = EndFrame * NumChannels;
		for (int32 Ch = 0; Ch < NumChannels; ++Ch)
		{
			FramePeak = FMath::Max(FramePeak, FMath::Abs(AudioBuffer[Base + Ch]));
		}
		if (FramePeak > ThresholdLinear)
		{
			break;
		}
	}

	const int32 StartSample = StartFrame * NumChannels;
	const int32 EndSampleExclusive = (EndFrame + 1) * NumChannels;
	const int32 NewNumSamples = EndSampleExclusive - StartSample;
	if (NewNumSamples <= 0)
	{
		AudioBuffer.Empty();
		return;
	}

	TArray<float> Trimmed;
	Trimmed.Reserve(NewNumSamples);
	Trimmed.Append(AudioBuffer.GetData() + StartSample, NewNumSamples);
	AudioBuffer = MoveTemp(Trimmed);
}

float FAudioBufferHandler::ApplyGainDb(float GainDb)
{
	return ApplyGainDbWithResult(GainDb).AppliedGainDb;
}

FAudioGainApplicationResult FAudioBufferHandler::ApplyGainDbWithResult(float GainDb)
{
	FAudioGainApplicationResult Result;
	Result.RequestedGainDb = GainDb;

	if (AudioBuffer.IsEmpty())
	{
		return Result;
	}

	float GainLinear = LinearFromDb(GainDb);
	if (!FMath::IsFinite(GainLinear) || GainLinear <= 0.0f)
	{
		UE_LOG(LogAFAudioBufferHandler, Warning, TEXT("ApplyGainDb: invalid gain %.2f dB."), GainDb);
		return Result;
	}

	constexpr float MaxAllowedGain = 4.0f; // +12.04 dB safety cap (see NormalizeToPeakDb / header note)
	bool bWasLimited = false;
	if (GainLinear > MaxAllowedGain)
	{
		GainLinear = MaxAllowedGain;
		bWasLimited = true;
	}

	// Preflight every post-gain sample before mutating the buffer. A finite overs multiplied by a
	// finite gain can still overflow float32; returning failure here keeps the buffer unchanged and
	// prevents a later WAV save from publishing NaN/Inf samples.
	for (const float Sample : AudioBuffer)
	{
		const float ScaledSample = Sample * GainLinear;
		if (!FMath::IsFinite(ScaledSample))
		{
			UE_LOG(LogAFAudioBufferHandler, Error, TEXT("ApplyGainDb: gain %.2f dB would produce a non-finite sample; buffer left unchanged."), GainDb);
			return Result;
		}
	}

	if (bWasLimited)
	{
		// Limiting is a successful, bounded gain application; report its numbers without warning severity.
		UE_LOG(LogAFAudioBufferHandler, Display, TEXT("ApplyGainDb: clamping gain %.2f dB to %.2f dB (+12 dB cap)."), GainDb, DbFromLinear(MaxAllowedGain));
		Result.bWasLimited = true;
		Result.LimitDirection = EAudioBufferGainLimitDirection::Upper;
	}

	for (float& Sample : AudioBuffer)
	{
		Sample *= GainLinear;
	}

	// Report the gain actually applied (post-cap), not the requested value, so callers/logs are truthful.
	Result.AppliedGainDb = DbFromLinear(GainLinear);
	Result.bSucceeded = true;
	return Result;
}

float FAudioBufferHandler::NormalizeToPeakDb(float TargetDb)
{
	return NormalizeToPeakDbWithResult(TargetDb).AppliedGainDb;
}

FAudioGainApplicationResult FAudioBufferHandler::NormalizeToPeakDbWithResult(float TargetDb)
{
	FAudioGainApplicationResult Result;
	const TOptional<float> Peak = GetPeakLevel();
	if (!Peak.IsSet() || Peak.GetValue() <= PeakNormalizeSilenceFloorLinear)
	{
		return Result;
	}

	const float TargetLinear = LinearFromDb(TargetDb);
	if (!FMath::IsFinite(TargetLinear) || TargetLinear <= 0.0f)
	{
		UE_LOG(LogAFAudioBufferHandler, Warning, TEXT("NormalizeToPeakDb: invalid target %.2f dB."), TargetDb);
		return Result;
	}

	const float GainDb = DbFromLinear(TargetLinear / Peak.GetValue());
	// Return the gain actually applied (ApplyGainDb caps at +12 dB), never the uncapped request,
	// so "Gain Applied" report fields reflect reality.
	return ApplyGainDbWithResult(GainDb);
}

TOptional<float> FAudioBufferHandler::GetPeakLevel() const
{
	// An empty buffer has no measurable peak; unset distinguishes unavailable from genuine silence.
	if (AudioBuffer.IsEmpty())
	{
		return TOptional<float>();
	}

	float Peak = 0.0f;
	bool bHasFiniteSample = false;
	for (const float Sample : AudioBuffer)
	{
		// Ignore any non-finite sample defensively (LoadBuffer already scrubs, but a caller may
		// query peak on a buffer populated by another path); a NaN must never define the peak.
		if (FMath::IsFinite(Sample))
		{
			bHasFiniteSample = true;
			Peak = FMath::Max(Peak, FMath::Abs(Sample));
		}
	}

	// A buffer containing no finite samples has no trustworthy peak; return unset instead of zero.
	return bHasFiniteSample ? TOptional<float>(Peak) : TOptional<float>();
}

TOptional<float> FAudioBufferHandler::GetRMSLevel() const
{
	// An empty buffer has no measurable RMS; unset distinguishes unavailable from genuine silence.
	if (AudioBuffer.IsEmpty())
	{
		return TOptional<float>();
	}

	double SumSquares = 0.0;
	for (const float Sample : AudioBuffer)
	{
		// A non-finite sample makes the RMS unavailable; do not expose a fabricated numeric result.
		if (!FMath::IsFinite(Sample))
		{
			return TOptional<float>();
		}
		SumSquares += static_cast<double>(Sample) * static_cast<double>(Sample);
	}
	return TOptional<float>(static_cast<float>(FMath::Sqrt(SumSquares / FMath::Max(1, AudioBuffer.Num()))));
}

TOptional<float> FAudioBufferHandler::GetDurationSec() const
{
	// Missing audio or format metadata has no measurable duration; unset distinguishes it from a value.
	if (SampleRate <= 0 || NumChannels <= 0 || AudioBuffer.IsEmpty())
	{
		return TOptional<float>();
	}
	const double SamplesPerSecond = static_cast<double>(SampleRate) * static_cast<double>(NumChannels);
	if (!FMath::IsFinite(SamplesPerSecond) || SamplesPerSecond <= 0.0)
	{
		return TOptional<float>();
	}
	return TOptional<float>(static_cast<float>(static_cast<double>(AudioBuffer.Num()) / SamplesPerSecond));
}

bool FAudioBufferHandler::SaveToWav(int32 BitDepth, const FString& FilePath) const
{
	if (FilePath.IsEmpty())
	{
		UE_LOG(LogAFAudioBufferHandler, Error, TEXT("SaveToWav: Output file path is empty."));
		return false;
	}

	const FSaveResult Result = SaveToWavWithResult(BitDepth, FilePath);
	if (!Result.bSucceeded)
	{
		// Preserve the typed save failure; the legacy API reports only whether any destination was claimed.
		return false;
	}

	// The legacy API has no output-path channel. A collision-safe sibling is still a successful save,
	// so return true and expose the actual claimed path through the log instead of publishing and lying
	// with a false return value.
	if (Result.SavedFilePath != FilePath)
	{
		UE_LOG(LogAFAudioBufferHandler, Display, TEXT("SaveToWav: requested path '%s' was occupied; atomically claimed collision-safe path '%s'."), *FilePath, *Result.SavedFilePath);
	}

	return true;
}

FAudioBufferHandler::FSaveResult FAudioBufferHandler::SaveToWavWithResult(int32 BitDepth, const FString& FilePath) const
{
	if (FilePath.IsEmpty())
	{
		const FString ErrorMessage = TEXT("Output file path is empty.");
		UE_LOG(LogAFAudioBufferHandler, Error, TEXT("SaveToWav: %s"), *ErrorMessage);
		return { false, ErrorMessage, FString(), false, EAudioWavSaveFailureReason::InvalidInput };
	}

	if (AudioBuffer.IsEmpty())
	{
		const FString ErrorMessage = TEXT("No audio data to save.");
		UE_LOG(LogAFAudioBufferHandler, Warning, TEXT("SaveToWav: %s"), *ErrorMessage);
		return { false, ErrorMessage, FString(), false, EAudioWavSaveFailureReason::InvalidInput };
	}

	if (BitDepth != 16 && BitDepth != 24 && BitDepth != 32)
	{
		const FString ErrorMessage = FString::Printf(TEXT("Unsupported bit depth %d."), BitDepth);
		UE_LOG(LogAFAudioBufferHandler, Error, TEXT("SaveToWav: %s"), *ErrorMessage);
		return { false, ErrorMessage, FString(), false, EAudioWavSaveFailureReason::Unsupported };
	}

	const bool bFloat32 = (BitDepth == 32);
	const uint16 BitsPerSample = static_cast<uint16>(BitDepth);
	const uint64 BytesPerSample = static_cast<uint64>(BitsPerSample / 8);
	const uint64 BlockAlign64 = static_cast<uint64>(NumChannels) * BytesPerSample;
	const uint64 ByteRate64 = static_cast<uint64>(SampleRate) * BlockAlign64;
	if (NumChannels <= 0 || NumChannels > MAX_uint16 || BlockAlign64 > MAX_uint16 || ByteRate64 > MAX_uint32)
	{
		const FString ErrorMessage = FString::Printf(TEXT("Unsupported WAV channel or rate geometry: %d channels at %d Hz."), NumChannels, SampleRate);
		UE_LOG(LogAFAudioBufferHandler, Error, TEXT("SaveToWav: %s"), *ErrorMessage);
		return { false, ErrorMessage, FString(), false, EAudioWavSaveFailureReason::Unsupported };
	}

	uint32 ChannelMask = 0;
	const bool bHasExtensibleMask = TryGetExtensibleChannelMask(ChannelLayout, NumChannels, ChannelMask);
	const bool bClassicLayout = (ChannelLayout == EAudioChannelLayout::Unknown)
		? (NumChannels == 1 || NumChannels == 2)
		: ((ChannelLayout == EAudioChannelLayout::Mono && NumChannels == 1)
			|| (ChannelLayout == EAudioChannelLayout::Stereo && NumChannels == 2));
	if (!bClassicLayout && !bHasExtensibleMask)
	{
		const FString ErrorMessage = FString::Printf(TEXT("Unsupported or unidentified channel layout for %d channels; an honest speaker mask is unavailable."), NumChannels);
		UE_LOG(LogAFAudioBufferHandler, Error, TEXT("SaveToWav: %s"), *ErrorMessage);
		return { false, ErrorMessage, FString(), false, EAudioWavSaveFailureReason::Unsupported };
	}

	// RIFF chunk sizes are unsigned 32-bit and TArray is int32-indexed, so all of this has to be
	// computed in 64-bit and rejected before it can wrap. 32-bit float stereo passes 2 GB at about
	// 93 minutes, and the old int32 math silently produced a corrupt header instead of an error.
	const uint64 DataSize64 = static_cast<uint64>(AudioBuffer.Num()) * BytesPerSample;
	// An odd-sized data chunk carries one trailing pad byte. RIFF excludes it from DataSize but
	// counts it in the enclosing chunk size, so it has to be part of ChunkSize below.
	const uint64 PadBytes = (DataSize64 % 2) != 0 ? 1 : 0;
	const uint32 FmtChunkSize = bHasExtensibleMask ? 40u : 16u;
	const uint64 RiffChunkSize64 = static_cast<uint64>(20u)
		+ static_cast<uint64>(FmtChunkSize)
		+ DataSize64
		+ PadBytes;
	const uint64 TotalBytes = static_cast<uint64>(8u) + RiffChunkSize64;
	const uint64 MaxRiffChunkSize = static_cast<uint64>(MAX_uint32);
	const uint64 MaxTotalBytes = static_cast<uint64>(MAX_int32);
	if (RiffChunkSize64 > MaxRiffChunkSize || TotalBytes > MaxTotalBytes)
	{
		const FString ErrorMessage = FString::Printf(
			TEXT("Output exceeds WAV size limit: %llu bytes of audio data at %d-bit (max %d bytes per file)."),
			DataSize64,
			BitDepth,
			MAX_int32);
		UE_LOG(LogAFAudioBufferHandler, Error, TEXT("SaveToWav: %s"), *ErrorMessage);
		return { false, ErrorMessage, FString(), false, EAudioWavSaveFailureReason::Unsupported };
	}

	const uint32 DataSize = static_cast<uint32>(DataSize64);
	const uint32 ChunkSize = static_cast<uint32>(RiffChunkSize64);
	const uint16 BlockAlign = static_cast<uint16>(BlockAlign64);
	const uint32 ByteRate = static_cast<uint32>(ByteRate64);
	const uint16 AudioFormat = bHasExtensibleMask ? 0xFFFEu : (bFloat32 ? 3u : 1u);
	TArray<uint8> WavData;

	WavData.Reserve(static_cast<int32>(TotalBytes));

	// RIFF header
	WavData.Append(reinterpret_cast<const uint8*>("RIFF"), 4);
	WriteUInt32(WavData, ChunkSize);
	WavData.Append(reinterpret_cast<const uint8*>("WAVE"), 4);

	// fmt chunk
	WavData.Append(reinterpret_cast<const uint8*>("fmt "), 4);
	WriteUInt32(WavData, FmtChunkSize);
	WriteUInt16(WavData, AudioFormat);
	WriteUInt16(WavData, static_cast<uint16>(NumChannels)); // NumChannels is int32; write exactly 2 bytes
	WriteInt32(WavData, SampleRate);
	WriteUInt32(WavData, ByteRate);
	WriteUInt16(WavData, BlockAlign);
	WriteUInt16(WavData, BitsPerSample);
	if (bHasExtensibleMask)
	{
		WriteUInt16(WavData, 22); // cbSize for WAVEFORMATEXTENSIBLE
		WriteUInt16(WavData, BitsPerSample); // ValidBitsPerSample equals the container precision.
		WriteUInt32(WavData, ChannelMask);
		WriteSubFormatGuid(WavData, bFloat32);
	}

	// data chunk
	WavData.Append(reinterpret_cast<const uint8*>("data"), 4);
	WriteUInt32(WavData, DataSize);

	const int32 NumFrames = AudioBuffer.Num() / NumChannels;
	const bool bReorderSurround71 = ChannelLayout == EAudioChannelLayout::Surround7_1 && NumChannels == 8;
	for (int32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
	{
		for (int32 WavChannelIndex = 0; WavChannelIndex < NumChannels; ++WavChannelIndex)
		{
			const int32 SourceChannelIndex = bReorderSurround71
				? Surround71WavToUnrealChannelOrder[WavChannelIndex]
				: WavChannelIndex;
			const int32 SampleIndex = (FrameIndex * NumChannels) + SourceChannelIndex;
			const float Sample = AudioBuffer[SampleIndex];

			if (BitDepth == 16)
			{
				const float Clamped = FMath::Clamp(Sample, -1.0f, 1.0f);
				const int16 IntSample = static_cast<int16>(FMath::Clamp(FMath::RoundToInt(Clamped * 32767.0f), -32768, 32767));
				WriteInt16(WavData, IntSample);
			}
			else if (BitDepth == 24)
			{
				WritePcm24(WavData, Sample);
			}
			else
			{
				// IEEE-float WAV preserves every finite source sample, including deliberate overs.
				WriteFloat32(WavData, Sample);
			}
		}
	}

	// RIFF requires each chunk's data to be word-aligned; pad an odd-sized data chunk with a
	// trailing zero byte (24-bit mono with an odd frame count is the reachable case). The pad
	// byte is not counted in DataSize, per spec, but ChunkSize above does include it.
	if (PadBytes > 0)
	{
		WavData.Add(0);
	}

	const FString OutputDirectory = FPaths::GetPath(FilePath);
	// A directory creation failure stops before any destination or temporary file is written.
	if (!OutputDirectory.IsEmpty() && !IFileManager::Get().MakeDirectory(*OutputDirectory, true))
	{
		const FString ErrorMessage = FString::Printf(TEXT("Failed to create output directory '%s'."), *OutputDirectory);
		UE_LOG(LogAFAudioBufferHandler, Error, TEXT("SaveToWav: %s"), *ErrorMessage);
		// Destination-level: every remaining asset in the batch would fail here identically.
		return { false, ErrorMessage, FString(), true, EAudioWavSaveFailureReason::DestinationUnavailable };
	}

	IFileManager& FileManager = IFileManager::Get();
	const FString TempDirectory = OutputDirectory.IsEmpty() ? TEXT(".") : OutputDirectory;
	// CreateTempFilename places the temporary file beside the destination, keeping the eventual move on one volume.
	const FString TempFilePath = FPaths::CreateTempFilename(*TempDirectory, *FPaths::GetBaseFilename(FilePath), TEXT(".wav.tmp"));
	auto GetCleanupFailureDetail = [&]() -> FString
	{
		if (TempFilePath.IsEmpty() || !FileManager.FileExists(*TempFilePath))
		{
			return FString();
		}

		if (FileManager.Delete(*TempFilePath, false, false, true)
			&& !FileManager.FileExists(*TempFilePath))
		{
			return FString();
		}

		return FString::Printf(TEXT(" Temporary cleanup failed for '%s'."), *TempFilePath);
	};
	if (TempFilePath.IsEmpty())
	{
		const FString ErrorMessage = TEXT("Failed to allocate a temporary WAV path.");
		UE_LOG(LogAFAudioBufferHandler, Error, TEXT("SaveToWav: %s"), *ErrorMessage);
		return { false, ErrorMessage, FString(), true, EAudioWavSaveFailureReason::DestinationUnavailable };
	}

	// A failed temporary write is a hard stop; only the temporary file created by this call is cleaned up.
	if (!FFileHelper::SaveArrayToFile(WavData, *TempFilePath, &FileManager, FILEWRITE_NoReplaceExisting))
	{
		FString ErrorMessage = FString::Printf(TEXT("Failed to write temporary WAV for '%s'."), *FilePath);
		ErrorMessage += GetCleanupFailureDetail();
		UE_LOG(LogAFAudioBufferHandler, Error, TEXT("SaveToWav: %s"), *ErrorMessage);
		return { false, ErrorMessage, FString(), false, EAudioWavSaveFailureReason::WriteFailed };
	}

	TArray<uint8> ValidatedWavData;
	// Any readback, byte comparison, or size validation failure stops before claiming a destination.
	if (!FFileHelper::LoadFileToArray(ValidatedWavData, *TempFilePath)
		|| ValidatedWavData != WavData
		|| FileManager.FileSize(*TempFilePath) != static_cast<int64>(WavData.Num()))
	{
		FString ErrorMessage = FString::Printf(TEXT("Temporary WAV validation failed for '%s'."), *FilePath);
		ErrorMessage += GetCleanupFailureDetail();
		UE_LOG(LogAFAudioBufferHandler, Error, TEXT("SaveToWav: %s"), *ErrorMessage);
		return { false, ErrorMessage, FString(), false, EAudioWavSaveFailureReason::ReadbackValidationFailed };
	}

	const FString OriginalFileName = FPaths::GetCleanFilename(FilePath);
	const FString OriginalBaseName = FPaths::GetBaseFilename(OriginalFileName);
	const FString OriginalExtension = FPaths::GetExtension(OriginalFileName, true);
	auto MakeCollisionSafePath = [&](int32 Suffix)
	{
		const FString CandidateName = OriginalBaseName + TEXT("_") + FString::FromInt(Suffix) + OriginalExtension;
		return FPaths::Combine(TempDirectory, CandidateName);
	};

	FString FinalFilePath = FilePath;
	int32 NextCollisionSuffix = 2;
	constexpr int32 MaxNumericCollisionRetries = 32;
	// One initial claim plus at most 32 deterministic numeric siblings. Callers that need a
	// package-derived hash resolve that readable fallback before entering this atomic writer.
	for (int32 ClaimAttempt = 0; ClaimAttempt <= MaxNumericCollisionRetries; ++ClaimAttempt)
	{
		// An existing candidate is never overwritten; a new suffixed sibling is selected instead.
		if (FileManager.FileExists(*FinalFilePath))
		{
			FinalFilePath = MakeCollisionSafePath(NextCollisionSuffix++);
			continue;
		}

		// Move with replacement disabled is the atomic same-volume claim; a collision is handled below.
		// bDoNotRetryOrError is deliberate: the engine's default Move retries a failed MoveFile ten
		// times at 0.5s each ON THE CALLING THREAD (FileManagerGeneric.cpp:330-339), which froze the
		// editor for ~5s per file and turned a broken destination into a ~42-minute hang across a
		// 500-asset batch. The only retry this function actually wants is the name-collision retry
		// it performs itself immediately below. EvenIfReadOnly and Attributes are never read by
		// FFileManagerGeneric::Move, so passing false for both is byte-identical to the previous
		// defaulted call.
		if (FileManager.Move(*FinalFilePath, *TempFilePath,
				/*Replace=*/false, /*EvenIfReadOnly=*/false, /*Attributes=*/false, /*bDoNotRetryOrError=*/true))
		{
			return { true, FString(), FinalFilePath, false, EAudioWavSaveFailureReason::None };
		}

		// A file may have appeared after the preflight check. Never replace it; claim a new
		// suffixed destination and retry the same validated temporary file.
		// A race-created file survives; select another suffix rather than routing around a failed claim.
		if (FileManager.FileExists(*FinalFilePath))
		{
			FinalFilePath = MakeCollisionSafePath(NextCollisionSuffix++);
			continue;
		}

		// A non-collision move failure is a hard stop; clean only this call's temporary file and leave final untouched.
		FString ErrorMessage = FString::Printf(TEXT("Failed to atomically move WAV to '%s'."), *FinalFilePath);
		ErrorMessage += GetCleanupFailureDetail();
		UE_LOG(LogAFAudioBufferHandler, Error, TEXT("SaveToWav: %s"), *ErrorMessage);
		// Destination-level: the claim itself was refused for a reason that is not a name collision
		// (collisions take the suffixed-retry branch above), so the batch caller should stop rather
		// than repeat this for every remaining asset.
		return { false, ErrorMessage, FString(), true, EAudioWavSaveFailureReason::AtomicPublicationFailed };
	}

	// Exhaustion is a hard stop after the documented collision-retry bound; clean only this call's temp file.
	FString ErrorMessage = FString::Printf(TEXT("Unable to claim a non-overwriting WAV destination after %d numeric collision retries for '%s'."), MaxNumericCollisionRetries, *FilePath);
	ErrorMessage += GetCleanupFailureDetail();
	UE_LOG(LogAFAudioBufferHandler, Error, TEXT("SaveToWav: %s"), *ErrorMessage);
	return { false, ErrorMessage, FString(), false, EAudioWavSaveFailureReason::AtomicPublicationFailed };
}
} // namespace AudioExporter
