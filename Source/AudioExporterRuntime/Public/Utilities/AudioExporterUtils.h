// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "PCM/AudioBufferHandler.h"

struct FAssetData;
class USoundWave;

/**
 * Returns true when the asset is an Audio Exporter-supported source: a MetaSound source, a
 * SoundCue, or a non-procedural SoundWave (procedural waves carry no deterministic PCM
 * payload the offline renderer could decode). This is the single owned predicate for "exportable
 * asset"; the module context-menu gate, the settings window filter, and the manager's asset
 * filter all share it so they cannot drift apart.
 */
AUDIOEXPORTERRUNTIME_API bool IsAudioExporterSupportedAsset(const FAssetData& Asset);

/**
 * Session-relative subfolder that holds exported WAVs. Reports stay at the session root, so a
 * session folder reads as "Audio/ plus the report" instead of loose WAVs around a log file.
 */
inline const TCHAR* AudioExporterAudioSubfolder = TEXT("Audio");

/**
 * Why a session path was fenced. The stored cause is what vetoed saves report, so a row settled by
 * the failure teardown never blames cancellation (and vice versa).
 */
enum class EAudioExporterPathFenceCause : uint8
{
	/** A user (or subsystem shutdown) cancellation fenced the path. */
	Cancellation,
	/** The session terminated through the failure teardown (e.g. a dead output destination). */
	SessionFailure
};

/**
 * Runtime utility functions for audio export operations, including normalization and WAV processing.
 * Editor-specific functions (asset filtering, path resolution) remain in the Editor module.
 */
class AUDIOEXPORTERRUNTIME_API FAudioExporterUtils
{
public:

	/**
	 * Resolves explicit speaker identity from an imported SoundWave. Multichannel layouts are
	 * accepted only when the editor-only ChannelSizes map names the exact active speakers;
	 * channel count alone is never treated as layout evidence.
	 */
	static AudioExporter::EAudioChannelLayout ResolveSoundWaveChannelLayout(const USoundWave* SoundWave);

	/**
	 * Extracts a sub-buffer from a larger batch recording buffer based on start/end times and correction factor.
	 */
	static TArray<float> ExtractVariationBuffer(
		const TArray<float>& BigBuffer,
		int32 SampleRate,
		int32 NumChannels,
		double StartTime,
		double EndTime,
		double TimeCorrectionFactor,
		bool bIsLoop);

	/**
	 * Marks one exact session directory as fenced before the pipeline invalidates its generation.
	 * Background saves check this process-local registry immediately before and after atomic WAV
	 * publication so a late task cannot leave an unreported file after the session ends. The cause
	 * is reported verbatim in every veto reason the fenced path produces.
	 */
	static void MarkExportPathCancelled(const FString& ExportPath, EAudioExporterPathFenceCause Cause = EAudioExporterPathFenceCause::Cancellation);

	/** Returns true when MarkExportPathCancelled has fenced this exact normalized session path. */
	static bool IsExportPathCancelled(const FString& ExportPath);

	/** Clears a stale registration before a new accepted session begins at a path. */
	static void ClearExportPathCancellation(const FString& ExportPath);

#if WITH_DEV_AUTOMATION_TESTS
	/** One-shot hook invoked after successful atomic publication and before the cancellation recheck. */
	static void SetPostPublicationHookForTesting(TFunction<void()> InHook);
#endif

	/**
	 * Processes (normalizes) and saves an audio variation to disk.
	 * Typically called from a background thread.
	 *
	 * @param InOutBuffer - The audio buffer to process.
	 * @param SoundName - The name of the sound (used for file naming).
	 * @param RelativePath - The relative export subfolder.
	 * @param VariationIndex - The variation index (0-based).
	 * @param ExportPath - The session directory. WAVs are written under its "Audio" subfolder
	 *                    (see AudioExporterAudioSubfolder), leaving the session root for reports;
	 *                    the cancellation path fence is still keyed on this session directory.
	 * @param SampleRate - The sample rate of the buffer.
	 * @param NumChannels - Number of channels in the buffer.
	 * @param bNormalize - Whether to normalize the audio.
	 * @param bCanSave - Whether to actually save the file to disk.
	 * @param OutGainAppliedDb - The gain applied during normalization.
	 * @param OutPeakDb - The peak level of the processed audio.
	 * @param OutRMSDb - The RMS level of the processed audio.
	 * @param OutFilePath - The full path where the file was saved.
	 * @param OutFailureReason - Optional failure detail when the save cannot be completed.
	 * @param RequestId - Deprecated filename input retained for source compatibility. Request GUIDs
	 *                    are no longer embedded in output names.
	 * @param OutDestinationUnusable - Optional; set to true only when the failure is a property of
	 *                    the DESTINATION itself (output directory uncreatable, or a shared-save
	 *                    refusal that is not a name collision). False for per-buffer problems and
	 *                    name collisions. Batch callers stop early instead of repeating a doomed
	 *                    write for every remaining variation.
	 * @param StableSourcePath - Stable package/object path used only after the bounded readable
	 *                    numeric collision range is exhausted, producing a short deterministic hash.
	 * @param OutMeasurementsTaken - Optional; set to true exactly when OutPeakDb/OutRMSDb carry real
	 *                    measured levels. A save vetoed before measurement leaves it false, and its
	 *                    row must present its level fields as not-measured, never as silence.
	 * @return True if the file was successfully saved.
	 */
	static bool ProcessAndSaveVariation(
		TArray<float>& InOutBuffer,
		const FString& SoundName,
		const FString& RelativePath,
		int32 VariationIndex,
		const FString& ExportPath,
		int32 SampleRate,
		int32 NumChannels,
		bool bNormalize,
		bool bCanSave,
		float& OutGainAppliedDb,
		float& OutPeakDb,
		float& OutRMSDb,
		FString& OutFilePath,
		FString* OutFailureReason = nullptr,
		const FGuid& RequestId = FGuid(),
		bool* OutDestinationUnusable = nullptr,
		const FString& StableSourcePath = FString(),
		float* OutRequestedGainDb = nullptr,
		bool* OutGainLimited = nullptr,
		bool* OutMeasurementsTaken = nullptr,
		AudioExporter::EAudioChannelLayout ChannelLayout = AudioExporter::EAudioChannelLayout::Unknown);
};
