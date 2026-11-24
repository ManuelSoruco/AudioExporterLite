// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "Utilities/AudioExporterUtils.h"
#include "Utilities/AudioExporterLogger.h"
#include "PCM/AudioBufferHandler.h"
#include "AssetRegistry/AssetData.h"
#include "DSP/Dsp.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Crc.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "MetasoundSource.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundWave.h"
#include "Sound/SoundWaveProcedural.h"

using namespace AudioExporter;

namespace
{
	FCriticalSection CancelledExportPathsGuard;
	TMap<FString, EAudioExporterPathFenceCause> CancelledExportPaths;

#if WITH_DEV_AUTOMATION_TESTS
	FCriticalSection PostPublicationHookGuard;
	TFunction<void()> PostPublicationHookForTesting;
#endif

	FString MakeExportPathCancellationKey(const FString& ExportPath)
	{
		if (ExportPath.IsEmpty())
		{
			return FString();
		}

		FString NormalizedPath = FPaths::ConvertRelativePathToFull(ExportPath);
		FPaths::NormalizeDirectoryName(NormalizedPath);
#if PLATFORM_WINDOWS
		NormalizedPath.ToLowerInline();
#endif
		return NormalizedPath;
	}

	EAudioExporterPathFenceCause GetStoredPathFenceCause(const FString& ExportPath)
	{
		FScopeLock Lock(&CancelledExportPathsGuard);
		const EAudioExporterPathFenceCause* StoredCause = CancelledExportPaths.Find(MakeExportPathCancellationKey(ExportPath));
		return StoredCause ? *StoredCause : EAudioExporterPathFenceCause::Cancellation;
	}

	/** The reason names the stored fence cause, so a non-cancel teardown never blames cancellation. */
	FString MakeFencedSaveReason(const FString& ExportPath)
	{
		switch (GetStoredPathFenceCause(ExportPath))
		{
		case EAudioExporterPathFenceCause::SessionFailure:
			return FString::Printf(
				TEXT("Save skipped because the export session failed and fenced export session '%s'."),
				*ExportPath);
		case EAudioExporterPathFenceCause::Cancellation:
		default:
			return FString::Printf(
				TEXT("Save skipped because cancellation fenced export session '%s'."),
				*ExportPath);
		}
	}
}

EAudioChannelLayout FAudioExporterUtils::ResolveSoundWaveChannelLayout(const USoundWave* SoundWave)
{
	if (!SoundWave || SoundWave->IsA<USoundWaveProcedural>())
	{
		return EAudioChannelLayout::Unknown;
	}
	if (SoundWave->bIsAmbisonics)
	{
		return EAudioChannelLayout::Ambisonic;
	}
	if (SoundWave->NumChannels == 1)
	{
		return EAudioChannelLayout::Mono;
	}
	if (SoundWave->NumChannels == 2)
	{
		return EAudioChannelLayout::Stereo;
	}

#if WITH_EDITORONLY_DATA
	if (SoundWave->ChannelSizes.Num() == SPEAKER_Count)
	{
		auto MatchesActiveSpeakers = [SoundWave](std::initializer_list<int32> ExpectedSpeakers)
		{
			for (int32 SpeakerIndex = 0; SpeakerIndex < SPEAKER_Count; ++SpeakerIndex)
			{
				bool bExpected = false;
				for (const int32 ExpectedSpeaker : ExpectedSpeakers)
				{
					bExpected |= ExpectedSpeaker == SpeakerIndex;
				}
				if ((SoundWave->ChannelSizes[SpeakerIndex] > 0) != bExpected)
				{
					return false;
				}
			}
			return true;
		};

		if (SoundWave->NumChannels == 4
			&& MatchesActiveSpeakers({ SPEAKER_FrontLeft, SPEAKER_FrontRight, SPEAKER_LeftSurround, SPEAKER_RightSurround }))
		{
			return EAudioChannelLayout::Quad;
		}
		if (SoundWave->NumChannels == 6
			&& MatchesActiveSpeakers({ SPEAKER_FrontLeft, SPEAKER_FrontRight, SPEAKER_FrontCenter,
				SPEAKER_LowFrequency, SPEAKER_LeftSurround, SPEAKER_RightSurround }))
		{
			return EAudioChannelLayout::Surround5_1;
		}
		if (SoundWave->NumChannels == 8
			&& MatchesActiveSpeakers({ SPEAKER_FrontLeft, SPEAKER_FrontRight, SPEAKER_FrontCenter,
				SPEAKER_LowFrequency, SPEAKER_LeftSurround, SPEAKER_RightSurround, SPEAKER_LeftBack, SPEAKER_RightBack }))
		{
			return EAudioChannelLayout::Surround7_1;
		}
	}
#endif

	return EAudioChannelLayout::Unknown;
}

bool IsAudioExporterSupportedAsset(const FAssetData& Asset)
{
	if (!Asset.IsValid())
	{
		return false;
	}

	const UClass* AssetClass = Asset.GetClass();
	if (!AssetClass)
	{
		return false;
	}

	if (AssetClass->IsChildOf(UMetaSoundSource::StaticClass()) || AssetClass->IsChildOf(USoundCue::StaticClass()))
	{
		return true;
	}

	// SoundWave export is supported. Procedural waves are excluded: they carry no
	// deterministic PCM payload an offline renderer could decode.
	return AssetClass->IsChildOf(USoundWave::StaticClass())
		&& !AssetClass->IsChildOf(USoundWaveProcedural::StaticClass());
}

TArray<float> FAudioExporterUtils::ExtractVariationBuffer(const TArray<float>& BigBuffer, int32 SampleRate, int32 NumChannels, double StartTime, double EndTime, double TimeCorrectionFactor, bool bIsLoop)
{
	// Time Correction in Audio Recording:
	// Capture markers (StartTime/EndTime) are recorded against the MIXER AUDIO clock, while the raw
	// captured batch buffer can hold more or fewer content seconds than the audio clock advanced
	// over the batch. The TimeCorrectionFactor (= buffered content seconds / audio-clock seconds)
	// maps the audio-clock markers onto real sample positions in the recording buffer.

	// Apply correction factor to the markers to align with the actual buffer samples
	const double CorrectedStartTime = StartTime * TimeCorrectionFactor;
	const double CorrectedEndTime = EndTime * TimeCorrectionFactor;

	// Calculate sample offsets based on the corrected times
	const int64 StartSample = static_cast<int64>(CorrectedStartTime * SampleRate) * NumChannels;
	const int64 EndSample = static_cast<int64>(CorrectedEndTime * SampleRate) * NumChannels;
	const int64 NumSamples = EndSample - StartSample;

	// A negative StartSample (drifted markers, or a correction factor derived from a negative measured
	// duration) would otherwise Memcpy from before the allocation instead of being reported as an
	// invalid range.
	if (NumSamples <= 0 || NumChannels <= 0 || StartSample < 0 || StartSample >= BigBuffer.Num())
	{
		return TArray<float>();
	}

	// Clamp the tail to the buffer on a whole-frame boundary: a copy that ends mid-frame would rotate
	// the channel order of the truncated variation.
	const int64 AvailableSamples = ((BigBuffer.Num() - StartSample) / NumChannels) * NumChannels;
	int64 ClampedSampleCount = FMath::Min<int64>(NumSamples, AvailableSamples);

	// Loop duration contract: a LOOP variation's exported length is its UNCORRECTED marker span --
	// the documented MaxLengthSeconds cap lives in those markers -- so a correction factor above 1.0
	// (hosts whose captured content outgrows the audio clock) must never stretch the slice's DURATION.
	// The source START/END positions stay correction-mapped exactly as above; only the copied FRAME
	// COUNT is clamped here, and only for loops (one-shot copying is untouched). When the cap exceeds
	// what the corrected span can still supply, the existing copy-what-exists clamp governs; a
	// degenerate non-positive span can never empty an otherwise legal marker because it already
	// failed the NumSamples <= 0 guard above.
	if (bIsLoop)
	{
		const int64 MarkerSpanFrames = static_cast<int64>((EndTime - StartTime) * static_cast<double>(SampleRate));
		if (MarkerSpanFrames > 0)
		{
			ClampedSampleCount = FMath::Min<int64>(ClampedSampleCount, MarkerSpanFrames * NumChannels);
		}
	}

	const int32 CopySampleCount = static_cast<int32>(ClampedSampleCount);

	if (CopySampleCount <= 0)
	{
		return TArray<float>();
	}

	TArray<float> VariationBuffer;
	VariationBuffer.SetNumUninitialized(CopySampleCount);
	FMemory::Memcpy(VariationBuffer.GetData(), BigBuffer.GetData() + StartSample, static_cast<SIZE_T>(CopySampleCount) * sizeof(float));

	return VariationBuffer;
}

void FAudioExporterUtils::MarkExportPathCancelled(const FString& ExportPath, EAudioExporterPathFenceCause Cause)
{
	const FString CancellationKey = MakeExportPathCancellationKey(ExportPath);
	if (CancellationKey.IsEmpty())
	{
		return;
	}

	FScopeLock Lock(&CancelledExportPathsGuard);
	CancelledExportPaths.Add(CancellationKey, Cause);
}

bool FAudioExporterUtils::IsExportPathCancelled(const FString& ExportPath)
{
	const FString CancellationKey = MakeExportPathCancellationKey(ExportPath);
	if (CancellationKey.IsEmpty())
	{
		return false;
	}

	FScopeLock Lock(&CancelledExportPathsGuard);
	return CancelledExportPaths.Contains(CancellationKey);
}

void FAudioExporterUtils::ClearExportPathCancellation(const FString& ExportPath)
{
	const FString CancellationKey = MakeExportPathCancellationKey(ExportPath);
	if (CancellationKey.IsEmpty())
	{
		return;
	}

	FScopeLock Lock(&CancelledExportPathsGuard);
	CancelledExportPaths.Remove(CancellationKey);
}

#if WITH_DEV_AUTOMATION_TESTS
void FAudioExporterUtils::SetPostPublicationHookForTesting(TFunction<void()> InHook)
{
	FScopeLock Lock(&PostPublicationHookGuard);
	PostPublicationHookForTesting = MoveTemp(InHook);
}
#endif

bool FAudioExporterUtils::ProcessAndSaveVariation(TArray<float>& InOutBuffer, const FString& SoundName, const FString& RelativePath, int32 VariationIndex, const FString& ExportPath, int32 SampleRate, int32 NumChannels, bool bNormalize, bool bCanSave, float& OutGainAppliedDb, float& OutPeakDb, float& OutRMSDb, FString& OutFilePath, FString* OutFailureReason, const FGuid& RequestId, bool* OutDestinationUnusable, const FString& StableSourcePath, float* OutRequestedGainDb, bool* OutGainLimited, bool* OutMeasurementsTaken, EAudioChannelLayout ChannelLayout)
{
	(void)RequestId; // Retained for source compatibility; filenames no longer contain request GUIDs.
	if (OutFailureReason)
	{
		OutFailureReason->Empty();
	}
	if (OutDestinationUnusable)
	{
		// Per-buffer problems and name collisions are never destination-level; only the two
		// destination-failure paths below set this flag to true.
		*OutDestinationUnusable = false;
	}
	if (OutMeasurementsTaken)
	{
		// No level below is real until the post-process measurements confirm otherwise.
		*OutMeasurementsTaken = false;
	}
	OutFilePath.Empty();
	if (OutRequestedGainDb)
	{
		*OutRequestedGainDb = 0.0f;
	}
	if (OutGainLimited)
	{
		*OutGainLimited = false;
	}

	if (!bCanSave)
	{
		if (OutFailureReason)
		{
			*OutFailureReason = TEXT("Save skipped because the export session is no longer active.");
		}
		return false;
	}

	if (IsExportPathCancelled(ExportPath))
	{
		if (OutFailureReason)
		{
			*OutFailureReason = MakeFencedSaveReason(ExportPath);
		}
		return false;
	}

	FAudioBufferHandler Handler;
	Handler.LoadBuffer(InOutBuffer, SampleRate, NumChannels, ChannelLayout);

	OutGainAppliedDb = 0.0f;
	const TOptional<float> InitialPeak = Handler.GetPeakLevel();
	// An unavailable peak is a hard stop for this save; do not manufacture a level or write a row
	// that would make an unmeasured value look like silence.
	if (!InitialPeak.IsSet())
	{
		if (OutFailureReason)
		{
			*OutFailureReason = TEXT("Peak measurement unavailable; WAV export was not written.");
		}
		return false;
	}

	if (bNormalize)
	{
		const float Peak = InitialPeak.GetValue();
		if (Peak > 0.001f)
		{
			const float TargetLinear = FMath::Pow(10.0f, -1.0f / 20.0f);
			const float RequestedGainDb = 20.0f * FMath::LogX(10.0f, TargetLinear / Peak);
			const FAudioGainApplicationResult GainResult = Handler.ApplyGainDbWithResult(RequestedGainDb);
			if (OutRequestedGainDb)
			{
				*OutRequestedGainDb = GainResult.RequestedGainDb;
			}
			if (OutGainLimited)
			{
				*OutGainLimited = GainResult.bWasLimited;
			}
			if (GainResult.bWasLimited)
			{
				UE_LOG(LogAudioExporter, Display, TEXT("Normalization succeeded with gain limiting. Requested=%.2f dB Applied=%.2f dB."), GainResult.RequestedGainDb, GainResult.AppliedGainDb);
			}
			// A failed typed gain operation aborts this save; no output path is exposed as successful.
			if (!GainResult.bSucceeded)
			{
				if (OutFailureReason)
				{
					*OutFailureReason = TEXT("Normalization failed; WAV export was not written.");
				}
				return false;
			}
			OutGainAppliedDb = GainResult.AppliedGainDb;
		}
	}

	const TOptional<float> ProcessedPeak = Handler.GetPeakLevel();
	const TOptional<float> ProcessedRMS = Handler.GetRMSLevel();
	const TOptional<float> DurationSeconds = Handler.GetDurationSec();
	// Any missing post-process measurement is a hard stop; the caller records a failed row whose
	// report omits measurements instead of converting an unset optional into a plausible number.
	if (!ProcessedPeak.IsSet() || !ProcessedRMS.IsSet() || !DurationSeconds.IsSet())
	{
		if (OutFailureReason)
		{
			*OutFailureReason = TEXT("Audio measurement unavailable; WAV export was not written.");
		}
		return false;
	}

	OutPeakDb = Audio::ConvertToDecibels(ProcessedPeak.GetValue());
	OutRMSDb  = Audio::ConvertToDecibels(ProcessedRMS.GetValue());
	if (OutMeasurementsTaken)
	{
		// From here on every return carries these real measured levels; a veto can end the save but
		// never un-measure it.
		*OutMeasurementsTaken = true;
	}

	// WAVs live under the session's Audio subfolder; the session root carries the report only. The
	// path fence above stays keyed on ExportPath itself, which is still this directory's ancestor.
	const FString AssetExportDir = ExportPath / AudioExporterAudioSubfolder / RelativePath;
	const FString ReadableStem = FString::Printf(TEXT("%s_%d"), *SoundName, VariationIndex + 1);
	FString RequestedFilePath = AssetExportDir / (ReadableStem + TEXT(".wav"));
	IFileManager& FileManager = IFileManager::Get();
	if (FileManager.FileExists(*RequestedFilePath))
	{
		constexpr int32 MaxReadableCollisionSuffix = 999;
		bool bResolved = false;
		for (int32 Suffix = 2; Suffix <= MaxReadableCollisionSuffix; ++Suffix)
		{
			const FString Candidate = AssetExportDir / FString::Printf(TEXT("%s_%d.wav"), *ReadableStem, Suffix);
			if (!FileManager.FileExists(*Candidate))
			{
				RequestedFilePath = Candidate;
				bResolved = true;
				break;
			}
		}

		if (!bResolved)
		{
			// Package identity is stable across runs, unlike the old per-request GUID. Eight hex digits
			// keep the fallback readable while still making unrelated same-name assets deterministic.
			const FString StableKey = !StableSourcePath.IsEmpty()
				? StableSourcePath
				: FString::Printf(TEXT("%s/%s"), *RelativePath, *SoundName);
			const FString HashSuffix = FString::Printf(TEXT("%08x"), FCrc::StrCrc32(*StableKey));
			RequestedFilePath = AssetExportDir / FString::Printf(TEXT("%s_%s.wav"), *ReadableStem, *HashSuffix);
			for (int32 HashCollision = 2; FileManager.FileExists(*RequestedFilePath) && HashCollision <= 33; ++HashCollision)
			{
				RequestedFilePath = AssetExportDir / FString::Printf(TEXT("%s_%s_%d.wav"), *ReadableStem, *HashSuffix, HashCollision);
			}
		}
	}

	if (!bCanSave)
	{
		if (OutFailureReason)
		{
			*OutFailureReason = TEXT("Save skipped because the export session is no longer active.");
		}
		return false;
	}

	if (IsExportPathCancelled(ExportPath))
	{
		if (OutFailureReason)
		{
			*OutFailureReason = MakeFencedSaveReason(ExportPath);
		}
		return false;
	}

	if (!FileManager.MakeDirectory(*AssetExportDir, true))
	{
		// This pre-check fails before the shared save (which would flag the same condition) is ever
		// reached, so the destination signal has to be raised here as well: an uncreatable output
		// directory dooms every remaining variation in the batch identically.
		if (OutDestinationUnusable)
		{
			*OutDestinationUnusable = true;
		}
		if (OutFailureReason)
		{
			*OutFailureReason = FString::Printf(TEXT("Failed to create export directory '%s'."), *AssetExportDir);
		}
		return false;
	}

	if (IsExportPathCancelled(ExportPath))
	{
		if (OutFailureReason)
		{
			*OutFailureReason = MakeFencedSaveReason(ExportPath);
		}
		return false;
	}

	const FAudioBufferHandler::FSaveResult SaveResult = Handler.SaveToWavWithResult(16, RequestedFilePath);
	if (SaveResult.bSucceeded)
	{
#if WITH_DEV_AUTOMATION_TESTS
		TFunction<void()> PostPublicationHook;
		{
			FScopeLock Lock(&PostPublicationHookGuard);
			PostPublicationHook = MoveTemp(PostPublicationHookForTesting);
		}
		if (PostPublicationHook)
		{
			PostPublicationHook();
		}
#endif

		if (IsExportPathCancelled(ExportPath))
		{
			const bool bSessionFailureFence = GetStoredPathFenceCause(ExportPath) == EAudioExporterPathFenceCause::SessionFailure;
			bool bDeletedLateArtifact = false;
			for (int32 DeleteAttempt = 0; DeleteAttempt < 3 && !bDeletedLateArtifact; ++DeleteAttempt)
			{
				bDeletedLateArtifact = FileManager.Delete(*SaveResult.SavedFilePath, false, true, true)
					|| !FileManager.FileExists(*SaveResult.SavedFilePath);
				if (!bDeletedLateArtifact)
				{
					FPlatformProcess::Sleep(0.01f);
				}
			}

			if (OutFailureReason)
			{
				*OutFailureReason = bDeletedLateArtifact
					? (bSessionFailureFence
						? TEXT("The export session failure won during WAV publication; the late artifact was removed.")
						: TEXT("Cancellation won during WAV publication; the late artifact was removed."))
					: (bSessionFailureFence
						? FString::Printf(
							TEXT("The export session failure won during WAV publication, but the late artifact could not be removed from '%s'."),
							*SaveResult.SavedFilePath)
						: FString::Printf(
							TEXT("Cancellation won during WAV publication, but the late artifact could not be removed from '%s'."),
							*SaveResult.SavedFilePath));
			}

			if (!bDeletedLateArtifact)
			{
				UE_LOG(
					LogAudioExporter,
					Error,
					TEXT("%s: %s fenced session '%s', but late artifact cleanup failed for '%s'."),
					*FString(__FUNCTION__),
					bSessionFailureFence ? TEXT("The export session failure") : TEXT("Cancellation"),
					*ExportPath,
					*SaveResult.SavedFilePath);
			}
			return false;
		}

		OutFilePath = SaveResult.SavedFilePath;
		if (OutFilePath != RequestedFilePath)
		{
			UE_LOG(LogAudioExporter, Display, TEXT("Resolved WAV filename collision at Display. Requested='%s' Claimed='%s'."), *RequestedFilePath, *OutFilePath);
		}
		return true;
	}

	// The shared atomic save leaves the final destination untouched on failure; keep the output path
	// explicitly unavailable so terminal rows cannot imply that a file exists.
	OutFilePath.Empty();
	if (OutFailureReason)
	{
		*OutFailureReason = SaveResult.ErrorMessage;
	}
	if (OutDestinationUnusable)
	{
		// Forward the shared save's destination verdict verbatim: true only for an uncreatable
		// directory, an unwritable temporary sibling, or a non-collision atomic-claim refusal.
		*OutDestinationUnusable = SaveResult.bDestinationUnusable;
	}

	return false;
}
