// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "AudioExporterConfig.h"
#include "AudioExportPipeline.h"
#include "AudioExporterBPLibrary.generated.h"

class USoundBase;

/**
 * UAudioExporterBPLibrary
 *
 * Blueprint-callable utility functions for the Audio Exporter plugin.
 * The export service itself is engine-owned; the world context object is used only
 * to resolve the playback world required by the runtime pipeline.
 *
 * Typical Blueprint usage:
 *   1. (Optional) Bind to the subsystem's OnExportComplete / OnExportProgress delegates
 *      to receive progress and completion events.
 *   2. Call ExportSoundToWav / ExportSoundsToWav for fire-and-forget exports.
 *   3. Call ExportSoundAsync when you need a single one-shot completion callback.
 *   4. Call IsExporting to poll state, or CancelExport to abort.
 */
UCLASS()
class AUDIOEXPORTERRUNTIME_API UAudioExporterBPLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Returns the engine-owned runtime subsystem when it is available. */
	UFUNCTION(BlueprintPure, Category = "AudioFactory|Exporter",
	          meta = (DisplayName = "Get Audio Exporter Runtime Subsystem"))
	static UAudioExporterRuntimeSubsystem* GetRuntimeSubsystem();

	/**
	 * Starts exporting a single sound to WAV files.
	 * The operation is asynchronous; bind to UAudioExporterRuntimeSubsystem::OnExportComplete
	 * for a completion event. A rejected start (invalid input or refused start) settles both
	 * completion surfaces exactly once with outcome Rejected and an unset path.
	 *
	 * @param WorldContextObject - Any valid UObject in the world being exported from.
	 * @param Sound              - The sound asset to export.
	 * @param Config             - Export settings (variations, normalization, etc.).
	 * @param OutputPath         - Target directory on disk. Empty string uses the default
	 *                             timestamped folder under ProjectSavedDir/AudioExporter.
	 */
	UFUNCTION(BlueprintCallable, Category = "AudioFactory|Exporter",
	          meta = (WorldContext = "WorldContextObject", DisplayName = "Export Sound To WAV"))
	static void ExportSoundToWav(UObject* WorldContextObject,
	                             USoundBase* Sound,
	                             const FAudioExporterConfig& Config,
	                             const FString& OutputPath);

	/** Identified form of ExportSoundToWav. Returns the same RequestId carried by progress/results. */
	UFUNCTION(BlueprintCallable, Category = "AudioFactory|Exporter",
	          meta = (WorldContext = "WorldContextObject", DisplayName = "Export Sound To WAV (Identified)"))
	static FGuid ExportSoundToWavIdentified(UObject* WorldContextObject,
	                                       USoundBase* Sound,
	                                       const FAudioExporterConfig& Config,
	                                       const FString& OutputPath);

	/**
	 * Starts exporting a batch of sounds to WAV files.
	 * The operation is asynchronous; bind to UAudioExporterRuntimeSubsystem::OnExportComplete
	 * for a completion event. A rejected start (invalid input or refused start) settles both
	 * completion surfaces exactly once with outcome Rejected and an unset path.
	 *
	 * @param WorldContextObject - Any valid UObject in the world being exported from.
	 * @param Sounds             - Array of sound assets to export.
	 * @param Config             - Export settings.
	 * @param OutputPath         - Target directory. Empty string uses the default timestamped folder.
	 */
	UFUNCTION(BlueprintCallable, Category = "AudioFactory|Exporter",
	          meta = (WorldContext = "WorldContextObject", DisplayName = "Export Sounds To WAV"))
	static void ExportSoundsToWav(UObject* WorldContextObject,
	                              const TArray<USoundBase*>& Sounds,
	                              const FAudioExporterConfig& Config,
	                              const FString& OutputPath);

	/** Identified form of ExportSoundsToWav. Returns the same RequestId carried by progress/results. */
	UFUNCTION(BlueprintCallable, Category = "AudioFactory|Exporter",
	          meta = (WorldContext = "WorldContextObject", DisplayName = "Export Sounds To WAV (Identified)"))
	static FGuid ExportSoundsToWavIdentified(UObject* WorldContextObject,
	                                        const TArray<USoundBase*>& Sounds,
	                                        const FAudioExporterConfig& Config,
	                                        const FString& OutputPath);

	/**
	 * Starts exporting a single sound asynchronously and invokes OnComplete when done.
	 * Also fires UAudioExporterRuntimeSubsystem::OnExportComplete for any other listeners.
	 *
	 * @param WorldContextObject - Any valid UObject in the world being exported from.
	 * @param Sound              - The sound asset to export.
	 * @param Config             - Export settings.
	 * @param OutputPath         - Target directory. Empty string uses the default timestamped folder.
	 * @param OnComplete         - One-shot delegate called when the export finishes or is cancelled.
	 */
	UFUNCTION(BlueprintCallable, Category = "AudioFactory|Exporter",
	          meta = (WorldContext = "WorldContextObject", DisplayName = "Export Sound Async"))
	static void ExportSoundAsync(UObject* WorldContextObject,
	                             USoundBase* Sound,
	                             const FAudioExporterConfig& Config,
	                             const FString& OutputPath,
	                             const FOnExportComplete& OnComplete);

	/**
	 * Starts exporting a single sound asynchronously and invokes OnComplete with the structured
	 * terminal result (explicit outcome, accepted-start flag, path set only when files were written,
	 * literal success/failure/skipped counts, error text). Also fires
	 * UAudioExporterRuntimeSubsystem::OnExportCompleteResult for any other listeners. A rejected
	 * start settles completion immediately with outcome Rejected and an unset path.
	 *
	 * @param WorldContextObject - Any valid UObject in the world being exported from.
	 * @param Sound              - The sound asset to export.
	 * @param Config             - Export settings.
	 * @param OutputPath         - Target directory. Empty string uses the default timestamped folder.
	 * @param OnComplete         - One-shot delegate called once with the session's terminal result.
	 */
	UFUNCTION(BlueprintCallable, Category = "AudioFactory|Exporter",
	          meta = (WorldContext = "WorldContextObject", DisplayName = "Export Sound Async With Result"))
	static void ExportSoundAsyncWithResult(UObject* WorldContextObject,
	                                       USoundBase* Sound,
	                                       const FAudioExporterConfig& Config,
	                                       const FString& OutputPath,
	                                       const FOnExportCompleteResult& OnComplete);

	/**
	 * Returns true while the engine-owned export service is running.
	 *
	 * @param WorldContextObject - Any valid UObject in the world being exported from.
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "AudioFactory|Exporter",
	          meta = (WorldContext = "WorldContextObject", DisplayName = "Is Exporting"))
	static bool IsExporting(UObject* WorldContextObject);

	/**
	 * Cancels the active export session. Already-saved files are preserved.
	 *
	 * @param WorldContextObject - Any valid UObject in the world being exported from.
	 */
	UFUNCTION(BlueprintCallable, Category = "AudioFactory|Exporter",
	          meta = (WorldContext = "WorldContextObject", DisplayName = "Cancel Export"))
	static void CancelExport(UObject* WorldContextObject);
};
