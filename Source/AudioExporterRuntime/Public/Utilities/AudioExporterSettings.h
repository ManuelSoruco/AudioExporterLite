// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "AudioExporterConfig.h"
#include "Engine/DeveloperSettings.h"
#include "AudioExporterSettings.generated.h"

/**
 * Native settings page for the Audio Exporter plugin.
 *
 * Settings are project-wide and persist through the product's DefaultConfig file so the Project
 * Settings page, Blueprint accessors, and the settings window share one store.
 */
UCLASS(Config = AudioExporter, DefaultConfig, DisplayName = "Audio Exporter")
class AUDIOEXPORTERRUNTIME_API UAudioExporterSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	/** Keep existing project settings readable after the settings class moved to AudioExporterRuntime. */
	virtual void OverrideConfigSection(FString& OutSectionName) override
	{
		OutSectionName = TEXT("/Script/AudioExporter.AudioExporterSettings");
	}
#if WITH_EDITOR
	virtual FText GetSectionText() const override;
	virtual FText GetSectionDescription() const override;
#endif

	/** Number of variations to generate for each one-shot (non-looping) sound. */
	UPROPERTY(Config, EditAnywhere, Category = "One Shots",
		meta = (DisplayName = "Variations", ToolTip = "Choose how many one-shot variations to export for each selected asset.", ClampMin = "1", ClampMax = "5", UIMin = "1", UIMax = "5"))
	int32 OneShotVariations = 3;

	/** Number of variations to generate for each infinite (looping) sound. */
	UPROPERTY(Config, EditAnywhere, Category = "Loops",
		meta = (DisplayName = "Variations", ToolTip = "Choose how many loop variations to export for each selected looping asset.", ClampMin = "1", ClampMax = "5", UIMin = "1", UIMax = "5"))
	int32 LoopVariations = 3;

	/** Maximum length (in seconds) for recording looping or infinite sounds. */
	UPROPERTY(Config, EditAnywhere, Category = "Loops",
		meta = (DisplayName = "Max Length", ToolTip = "Set the maximum capture duration for each loop export.", ClampMin = "5", ClampMax = "60", UIMin = "5", UIMax = "60", Units = "s"))
	int32 MaxLength = 30;

	/** If enabled, all exported audio files will be normalized to -1 dBFS. */
	UPROPERTY(Config, EditAnywhere, Category = "Normalization",
		meta = (DisplayName = "Normalize Peaks to -1 dBFS", ToolTip = "Normalize exported file peaks to the -1 dBFS ceiling after rendering."))
	bool bNormalize = true;

	/** If enabled, you will hear the audio in real-time as it is being exported. */
	UPROPERTY(Config, EditAnywhere, Category = "Monitoring",
		meta = (DisplayName = "Listen While Exporting", ToolTip = "Monitor the export in real time while Audio Exporter captures each asset."))
	bool bMonitor = false;

	/** If enabled, a detailed text log will be saved alongside the exported audio files. */
	UPROPERTY(Config, EditAnywhere, Category = "Reports",
		meta = (DisplayName = "Export Text Log", ToolTip = "Write a text report alongside the exported audio files."))
	bool bExportTextLog = true;

	/**
	 * Custom base directory for exported audio files.
	 * Leave empty to use the default location (Saved/AudioExporter/).
	 * A timestamped sub-folder is always created inside this directory.
	 */
	UPROPERTY(Config, meta = (ToolTip = "Optional project-wide export directory. When empty, exports use Saved/AudioExporter."))
	FString ExportDirectory;

	/**
	 * Returns the effective base directory that exports (and their logs) are written under.
	 * This is the custom ExportDirectory when one is set, or the default Saved/AudioExporter
	 * location otherwise. Each session still creates its own timestamped sub-folder inside it.
	 */
	FString ResolveEffectiveExportBaseDirectory() const;

	/** Creates a runtime-safe export config from the persisted settings. */
	FAudioExporterConfig ToConfig() const;

	/** Applies a runtime-safe export config onto the persisted settings object. */
	void ApplyFromConfig(const FAudioExporterConfig& InConfig);

	#if WITH_EDITOR
	/** Responds to changes made in the Project Settings UI. */
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	#endif
};
