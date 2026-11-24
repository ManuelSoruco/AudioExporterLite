// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Sound/SoundBase.h"
#include "AudioExporterConfig.generated.h"

/**
 * FAudioExporterConfig holds the configuration values for the Audio Exporter pipeline.
 * This is a plain data struct (no Editor deps) that can be used in Runtime contexts.
 * In the Editor, UAudioExporterSettings populates this struct from its Config properties.
 */
USTRUCT(BlueprintType)
struct AUDIOEXPORTERRUNTIME_API FAudioExporterConfig
{
	GENERATED_BODY()

	/** Number of variations to generate for each one-shot (non-looping) sound. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AudioFactory|Exporter|One Shots", meta = (ClampMin = "1", ClampMax = "5"))
	int32 OneShotVariations = 3;

	/** Number of variations to generate for each infinite (looping) sound. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AudioFactory|Exporter|Loops", meta = (ClampMin = "1", ClampMax = "5"))
	int32 LoopVariations = 3;

	/** Maximum length (in seconds) for recording looping or infinite sounds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AudioFactory|Exporter|Common", meta = (ClampMin = "5", ClampMax = "60"))
	int32 MaxLengthSeconds = 30;

	/** If enabled, all exported audio files will be normalized to -1 dBFS. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AudioFactory|Exporter|Common")
	bool bNormalize = true;

	/** If enabled, you will hear the audio in real-time as it is being exported. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AudioFactory|Exporter|Common")
	bool bMonitor = false;

	/** If enabled, a detailed text log will be saved alongside the exported audio files. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AudioFactory|Exporter|Common")
	bool bExportTextLog = true;

	/**
	 * Clamps every numeric field into the range its ClampMin/ClampMax metadata advertises, logging
	 * once when a value had to be coerced. The metadata only constrains the details panel, so
	 * Blueprint and C++ callers can otherwise hand the pipeline a MaxLengthSeconds of 0 (which
	 * truncates every capture) or a five-figure variation count.
	 */
	void Sanitize();
};

/**
 * FAudioExportSoundEntry pairs a loaded sound with its export metadata.
 * Used as input to the FAudioExportPipeline.
 */
struct AUDIOEXPORTERRUNTIME_API FAudioExportSoundEntry
{
	/** The sound to export (must be valid and loaded). */
	TObjectPtr<USoundBase> Sound = nullptr;

	/** Display name for file naming (e.g., "MyMetaSound"). */
	FString SoundName;

	/** Source asset path for logging (e.g., "/Game/Sounds/UI/MyMetaSound"). */
	FString SourceAssetPath;

	/** Relative export subfolder (e.g., "Sounds/UI"). */
	FString RelativePath;
};
