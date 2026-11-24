// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "PCM/AudioPCMUtils.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Sound/SoundWave.h"
#include "Sound/SoundWaveProcedural.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
#include "ContentStreaming.h"
#include "Sound/SoundWaveProxyPlayer.h"
#else
#include "Sound/SoundWaveProxyReader.h"
#endif
// The LAST #include must sit at preprocessor nesting level zero, because the product
// namespace opens directly after it. Keep this unguarded include below any
// engine-version-guarded include block above.
#include "Misc/AssertionMacros.h"
namespace AudioExporter {

namespace
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	EAudioPCMReadStatus DecodeSoundWaveWithProxyPlayer(USoundWave* InSoundWave, TArray<float>& OutSamples, int32& OutSampleRate, int32& OutNumChannels)
	{
		// Keep the immutable proxy alive while obtaining the current data snapshot through its UE5.8 API.
		const TSharedRef<const FSoundWaveProxy> SoundWaveProxy = InSoundWave->GetSoundWaveProxy();
		const TSharedRef<const FSoundWaveData> SoundWaveData = SoundWaveProxy->GetSoundWaveDataRef();

		TArray<FAudioChunkHandle> LoadedChunkHandles;
		if (SoundWaveData->IsStreaming())
		{
			const TArrayView<const uint8> ZerothChunk = SoundWaveData->GetZerothChunk(true);
			if (ZerothChunk.IsEmpty())
			{
				return EAudioPCMReadStatus::DataUnavailable;
			}

			const uint32 NumChunks = SoundWaveData->GetNumChunks();
			LoadedChunkHandles.Reserve(static_cast<int32>(NumChunks > 0 ? NumChunks - 1 : 0));
			for (uint32 ChunkIndex = 1; ChunkIndex < NumChunks; ++ChunkIndex)
			{
				FAudioChunkHandle ChunkHandle = IStreamingManager::Get().GetAudioStreamingManager().GetLoadedChunk(
					SoundWaveData,
					ChunkIndex,
					true,
					true);
				if (!ChunkHandle.IsValid() || ChunkHandle.Num() == 0)
				{
					return EAudioPCMReadStatus::DataUnavailable;
				}
				LoadedChunkHandles.Add(MoveTemp(ChunkHandle));
			}
		}

		// Matching the output format to the source format keeps the decode path resample-free and channel-conversion-free.
		FSoundWaveProxyPlayer::FSettings PlayerSettings(
			SoundWaveData->GetSampleRate(),
			static_cast<int32>(SoundWaveData->GetNumChannels()));
		TUniquePtr<FSoundWaveProxyPlayer> Player = FSoundWaveProxyPlayer::Create(PlayerSettings);
		if (!Player.IsValid() || !Player->SetSoundWave(SoundWaveData))
		{
			// The engine has no usable decoder for this source. Do not return a silent buffer.
			return EAudioPCMReadStatus::DecoderUnavailable;
		}

		const int32 SampleRate = FMath::RoundToInt(Player->GetSourceSampleRate());
		const int32 NumChannels = Player->GetSourceNumChannels();
		const int32 NumFrames = Player->GetSourceNumFrames();
		const int64 NumSamples64 = static_cast<int64>(NumFrames) * static_cast<int64>(NumChannels);
		if (SampleRate <= 0 || NumChannels <= 0 || NumFrames <= 0 || NumSamples64 <= 0 || NumSamples64 > MAX_int32)
		{
			// Invalid engine metadata is rejected as malformed PCM rather than truncated or fabricated output.
			return EAudioPCMReadStatus::MalformedPCMData;
		}
		if (NumChannels > 8)
		{
			// FSoundWaveProxyPlayer rejects more than eight channels in its settings; report the typed failure instead of asserting.
			return EAudioPCMReadStatus::DecoderUnavailable;
		}

		Audio::FAlignedFloatBuffer DecodedSamples;
		DecodedSamples.SetNumUninitialized(static_cast<int32>(NumSamples64));
		int32 NumSamplesDecoded = 0;
		const int32 MaxBlockSamples = PlayerSettings.MaxDecodeSizeFrames * NumChannels;
		while (NumSamplesDecoded < DecodedSamples.Num())
		{
			const int32 RemainingSamples = DecodedSamples.Num() - NumSamplesDecoded;
			const int32 RequestedSamples = FMath::Min(RemainingSamples, MaxBlockSamples);
			TArrayView<float> RemainingView(DecodedSamples.GetData() + NumSamplesDecoded, RequestedSamples);

			// Generates interleaved source-rate frames into the view, reports frames generated, and never exceeds the view or the wave.
			const int32 NumFramesGenerated = Player->GenerateSourceAudio(RemainingView);
			if (NumFramesGenerated == 0)
			{
				if (Player->IsFinished() && NumSamplesDecoded >= DecodedSamples.Num())
				{
					break;
				}
				// Zero decoder progress before the target sample count is reached is unavailable data.
				return EAudioPCMReadStatus::DataUnavailable;
			}
			if (NumFramesGenerated < 0 || NumFramesGenerated > RequestedSamples / NumChannels)
			{
				return EAudioPCMReadStatus::DecodeFailed;
			}
			NumSamplesDecoded += NumFramesGenerated * NumChannels;
		}
		if (NumSamplesDecoded != DecodedSamples.Num())
		{
			return EAudioPCMReadStatus::DataUnavailable;
		}

		// Copy from the engine's aligned buffer into the public default-allocator buffer; this avoids allocator-type coupling.
		OutSamples.SetNumUninitialized(DecodedSamples.Num());
		FMemory::Memcpy(OutSamples.GetData(), DecodedSamples.GetData(), static_cast<SIZE_T>(DecodedSamples.Num()) * sizeof(float));
		OutSampleRate = SampleRate;
		OutNumChannels = NumChannels;
		return EAudioPCMReadStatus::Succeeded;
	}
#else
	EAudioPCMReadStatus DecodeSoundWaveWithProxyReader(USoundWave* InSoundWave, TArray<float>& OutSamples, int32& OutSampleRate, int32& OutNumChannels)
	{
		const FSoundWaveProxyPtr SoundWaveProxy = InSoundWave->CreateSoundWaveProxy();
		if (!SoundWaveProxy.IsValid())
		{
			// The cooked or streamed source could not be obtained. Leave outputs empty and stop with typed failure.
			return EAudioPCMReadStatus::DataUnavailable;
		}

		FSoundWaveProxyReader::FSettings ReaderSettings;
		TUniquePtr<FSoundWaveProxyReader> Reader = FSoundWaveProxyReader::Create(SoundWaveProxy.ToSharedRef(), ReaderSettings);
		if (!Reader.IsValid())
		{
			// The engine has no usable reader for this source. Do not return a silent buffer.
			return EAudioPCMReadStatus::DecoderUnavailable;
		}

		const int32 SampleRate = FMath::RoundToInt(Reader->GetSampleRate());
		const int32 NumChannels = Reader->GetNumChannels();
		const int32 NumFrames = Reader->GetNumFramesInWave();
		const int64 NumSamples64 = static_cast<int64>(NumFrames) * static_cast<int64>(NumChannels);
		if (SampleRate <= 0 || NumChannels <= 0 || NumFrames <= 0 || NumSamples64 <= 0 || NumSamples64 > MAX_int32)
		{
			// Invalid engine metadata is rejected as malformed PCM rather than truncated or fabricated output.
			return EAudioPCMReadStatus::MalformedPCMData;
		}

		Audio::FAlignedFloatBuffer DecodedSamples;
		DecodedSamples.SetNumUninitialized(static_cast<int32>(NumSamples64));
		int32 NumSamplesDecoded = 0;
		while (NumSamplesDecoded < DecodedSamples.Num())
		{
			if (Reader->HasFailed())
			{
				// A decoder-reported failure is terminal; callers receive a typed failure and no samples.
				return EAudioPCMReadStatus::DecodeFailed;
			}

			// A reader that cannot produce more data is a hard stop; do not call PopAudio again and risk an unbounded wait.
			if (!Reader->CanProduceMoreAudio())
			{
				return EAudioPCMReadStatus::DataUnavailable;
			}

			const int32 RemainingSamples = DecodedSamples.Num() - NumSamplesDecoded;
			const int32 FrameIndexBeforeRead = Reader->GetFrameIndex();
			Audio::FAlignedFloatBuffer ReadBuffer;
			ReadBuffer.SetNumUninitialized(RemainingSamples);
			const int32 NumSamplesRead = Reader->PopAudio(ReadBuffer);
			if (Reader->HasFailed())
			{
				// A decoder-reported failure is terminal; callers receive a typed failure and no samples.
				return EAudioPCMReadStatus::DecodeFailed;
			}

			// Zero progress is a hard stop; this bounds streamed reads by the known frame count instead of a wall-clock constant.
			if (NumSamplesRead <= 0)
			{
				return EAudioPCMReadStatus::DataUnavailable;
			}
			if (Reader->GetFrameIndex() <= FrameIndexBeforeRead)
			{
				// Positive output without frame progress is a stalled reader; stop instead of retrying indefinitely.
				return EAudioPCMReadStatus::DataUnavailable;
			}

			// A reader must return complete interleaved frames; reject malformed progress rather than truncating output.
			if (NumSamplesRead > RemainingSamples || (NumSamplesRead % NumChannels) != 0)
			{
				return EAudioPCMReadStatus::DecodeFailed;
			}

			FMemory::Memcpy(
				DecodedSamples.GetData() + NumSamplesDecoded,
				ReadBuffer.GetData(),
				static_cast<SIZE_T>(NumSamplesRead) * sizeof(float));
			NumSamplesDecoded += NumSamplesRead;
		}

		// Copy from the engine's aligned buffer into the public default-allocator buffer; this avoids allocator-type coupling.
		OutSamples.SetNumUninitialized(DecodedSamples.Num());
		FMemory::Memcpy(OutSamples.GetData(), DecodedSamples.GetData(), static_cast<SIZE_T>(DecodedSamples.Num()) * sizeof(float));
		OutSampleRate = SampleRate;
		OutNumChannels = NumChannels;
		return EAudioPCMReadStatus::Succeeded;
	}
#endif
}

namespace AudioPCMUtils
{
	EAudioPCMReadStatus ReadSoundWavePCMData(USoundWave* InSoundWave, TArray<float>& OutSamples, int32& OutSampleRate, int32& OutNumChannels)
	{
		OutSamples.Empty();
		OutSampleRate = 0;
		OutNumChannels = 0;

		// A null source is rejected with cleared outputs; callers must not interpret an empty buffer as silence.
		if (!InSoundWave)
		{
			return EAudioPCMReadStatus::InvalidSoundWave;
		}

		// Procedural waves have no imported or cooked source file for this read path.
		if (InSoundWave->IsA<USoundWaveProcedural>())
		{
			return EAudioPCMReadStatus::ProceduralSoundWaveUnsupported;
		}

		// The SoundWave decompression and streaming facilities used here are game-thread APIs.
		if (!IsInGameThread())
		{
			return EAudioPCMReadStatus::WrongThread;
		}

		TArray<uint8> RawData;
		uint32 SoundWaveSampleRate = 0;
		uint16 SoundWaveNumChannels = 0;
		#if WITH_EDITOR
		InSoundWave->GetImportedSoundWaveData(RawData, SoundWaveSampleRate, SoundWaveNumChannels);
		#endif

		if (RawData.IsEmpty())
		{
			if (InSoundWave->RawPCMData != nullptr && InSoundWave->RawPCMDataSize > 0)
			{
				RawData.Append(InSoundWave->RawPCMData, InSoundWave->RawPCMDataSize);

				const float PlatformSampleRate = InSoundWave->GetSampleRateForCurrentPlatform();
				if (PlatformSampleRate > 0.0f)
				{
					SoundWaveSampleRate = static_cast<uint32>(FMath::RoundToInt(PlatformSampleRate));
				}
				else if (InSoundWave->GetImportedSampleRate() > 0)
				{
					SoundWaveSampleRate = InSoundWave->GetImportedSampleRate();
				}

				SoundWaveNumChannels = static_cast<uint16>(InSoundWave->NumChannels);
			}
		}

		if (RawData.IsEmpty())
		{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
			// FSoundWaveProxyPlayer delegates cooked compressed and streamed decoding to the engine's registered audio format.
			return DecodeSoundWaveWithProxyPlayer(InSoundWave, OutSamples, OutSampleRate, OutNumChannels);
#else
			// FSoundWaveProxyReader delegates cooked compressed and streamed decoding to the engine's registered audio format.
			return DecodeSoundWaveWithProxyReader(InSoundWave, OutSamples, OutSampleRate, OutNumChannels);
#endif
		}

		// A raw buffer with missing format metadata is rejected and cannot become a silent success.
		if (SoundWaveSampleRate == 0 || SoundWaveNumChannels == 0)
		{
			return EAudioPCMReadStatus::MalformedPCMData;
		}

		OutSampleRate = static_cast<int32>(SoundWaveSampleRate);
		OutNumChannels = static_cast<int32>(SoundWaveNumChannels);

		// UE's imported/RawPCMData is canonically interleaved signed 16-bit LPCM.
		constexpr int32 BytesPerSample = 2;
		// A partial 16-bit sample is rejected rather than over-read.
		if ((RawData.Num() % BytesPerSample) != 0)
		{
			OutSamples.Empty();
			OutSampleRate = 0;
			OutNumChannels = 0;
			return EAudioPCMReadStatus::MalformedPCMData;
		}
		const int32 NumSamples = RawData.Num() / BytesPerSample;
		// A partial interleaved frame is rejected so callers never receive silently truncated audio.
		if (NumSamples <= 0 || (NumSamples % OutNumChannels) != 0)
		{
			OutSamples.Empty();
			OutSampleRate = 0;
			OutNumChannels = 0;
			return EAudioPCMReadStatus::MalformedPCMData;
		}

		OutSamples.SetNumUninitialized(NumSamples);
		const int16* SamplePtr = reinterpret_cast<const int16*>(RawData.GetData());
		for (int32 i = 0; i < NumSamples; ++i)
		{
			OutSamples[i] = static_cast<float>(SamplePtr[i]) / 32768.0f;
		}

		return EAudioPCMReadStatus::Succeeded;
	}

	bool GetSoundWavePCMData(USoundWave* InSoundWave, TArray<float>& OutSamples, int32& OutSampleRate, int32& OutNumChannels)
	{
		return ReadSoundWavePCMData(InSoundWave, OutSamples, OutSampleRate, OutNumChannels) == EAudioPCMReadStatus::Succeeded;
	}
}
} // namespace AudioExporter
