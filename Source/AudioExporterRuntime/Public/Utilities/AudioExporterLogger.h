// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "AudioExporterLogger.generated.h"


AUDIOEXPORTERRUNTIME_API DECLARE_LOG_CATEGORY_EXTERN(LogAudioExporter, Log, All);

/**
 * Row status for the export report. This is the single owned classification:
 * the logger and the Summary counts all compare this enum rather than
 * independently-typed string literals that could silently drift out of sync. Pending is the
 * honest default for a slot that was allocated (e.g. by report padding) but never actually
 * recorded -- it must never survive to a written report; SectionForStatus files it under Failed
 * so it is loud, not silently dropped.
 */
UENUM()
enum class EAudioExportRowStatus : uint8
{
	Pending,
	Succeeded,
	SucceededGainLimited,
	Failed,
	Skipped
};

/**
 * FAudioExportLogEntry stores data about a single exported variation.
 * This includes file names, duration, and audio statistics like Peak and RMS.
 */
USTRUCT()
struct AUDIOEXPORTERRUNTIME_API FAudioExportLogEntry
{
	GENERATED_BODY()

	/** The name of the resulting WAV file. */
	UPROPERTY()
	FString ExportedFileName;

	/** The original asset path. */
	UPROPERTY()
	FString SourceFilePath;

	/** Full disk path for the exported WAV file when available. */
	UPROPERTY()
	FString OutputFilePath;

	/** Which variation this entry refers to (1, 2, 3...). */
	UPROPERTY()
	int32 Variation = 1;

	/** Row status for the export report. Defaults to Pending: honest state for an entry that was
	 * allocated but never actually recorded (see EAudioExportRowStatus). */
	UPROPERTY()
	EAudioExportRowStatus Status = EAudioExportRowStatus::Pending;

	/** Human-readable row details (failure/skip sentences only; success rows carry none). */
	UPROPERTY()
	FString Details;

	/** Total variations for the source; static SoundWaves always report one of one. */
	UPROPERTY()
	int32 TotalVariations = 1;

	/** True when this row represents the deterministic static-source SoundWave path. */
	UPROPERTY()
	bool bStaticSource = false;

	/** Recorded duration in seconds. */
	UPROPERTY()
	float DurationSeconds = 0.f;

	/** True if the sound was treated as a loop during export. */
	UPROPERTY()
	bool bLooping = false;

	/** Peak audio level detected, in decibels (dBFS). */
	UPROPERTY()
	float ExportPeak = 0.f;

	/** Root Mean Square (RMS) audio level detected, in decibels (dBFS). */
	UPROPERTY()
	float ExportRMS = 0.f;

	/** True exactly when ExportPeak/ExportRMS carry real measured levels. A row vetoed before any
	 *  measurement keeps this false so its zero level fields read as not-measured, never as
	 *  silence; report writers already omit levels for every non-succeeded row. */
	UPROPERTY()
	bool bMeasurementsRecorded = false;

	/** The amount of gain (in dB) added during normalization. */
	UPROPERTY()
	float GainApplied = 0.f; // dB

	/** Gain requested to reach the configured peak target. */
	UPROPERTY()
	float GainRequested = 0.f;

	/** True when the +12 dB safety cap prevented the requested normalization gain. */
	UPROPERTY()
	bool bGainLimited = false;
};

/**
 * FAudioExportAssetLog groups all variation log entries for a single exported asset,
 * along with the metadata needed for the text log report.
 */
struct AUDIOEXPORTERRUNTIME_API FAudioExportAssetLog
{
	/** The original source asset path (e.g., "/Game/Sounds/UI/MySound"). */
	FString SourceAssetPath;

	/** Relative export subfolder (e.g., "Sounds/UI"). */
	FString RelativePath;

	/** Log entries for each variation of this asset. */
	TArray<FAudioExportLogEntry> Variations;
};

/** 
 * AudioExporterLogger namespace contains helper functions for saving export reports to disk.
 *
 * The report format follows a unified spec: a shared skeleton (banner, Summary,
 * Settings Used, per-asset sections grouped by outcome, output-location footer), no placeholder
 * tokens, no semicolon Details dumps, honest Summary counts, and normalized absolute paths.
 */
namespace AudioExporterLogger
{
	/**
	 * Generates a formatted .txt report for the entire export session.
	 * @param LogsToExport - The collection of asset logs to write.
	 * @param InExportPath - The full disk path where the .txt file will be saved.
	 * @param InDate - The current date/time string for the report header.
	 * @param SettingsSummary - A string summarizing the exporter settings used (labels already padded).
	 * @param InSessionFolder - The session output FOLDER (not the .txt file path) printed in the
	 *        "Output Location" footer. Distinct from InExportPath, which names the
	 *        specific file this call writes.
	 */
	AUDIOEXPORTERRUNTIME_API bool WriteTextLogFile(
		const TArray<FAudioExportAssetLog>& LogsToExport,
		const FString& InExportPath,
		const FString& InDate,
		const FString& SettingsSummary,
		const FString& InSessionFolder,
		FString* OutError = nullptr);
}
