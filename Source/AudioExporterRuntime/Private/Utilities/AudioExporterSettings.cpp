// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "Utilities/AudioExporterSettings.h"

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_EDITOR
#include "Misc/ConfigCacheIni.h"
#include "UObject/UnrealType.h"
#endif

#define LOCTEXT_NAMESPACE "AudioExporterSettings"

#if WITH_EDITOR
FText UAudioExporterSettings::GetSectionText() const
{
	return LOCTEXT("SettingsSectionName", "Audio Exporter");
}

FText UAudioExporterSettings::GetSectionDescription() const
{
	return LOCTEXT("SettingsSectionDescription",
		"Configure Audio Exporter variation, loop capture, export, report, and monitoring defaults.");
}
#endif

#if WITH_EDITOR
void UAudioExporterSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (HasAnyFlags(RF_Transient) && !HasAnyFlags(RF_ClassDefaultObject))
	{
		return;
	}

	// Suite isolation: suite-driven window commits reach the
	// real class default object through the shared adapter's PostEditChange(), and without this
	// guard that persists whatever the loaded config hierarchy held at that moment -- test
	// fixtures, cross-test mutations, or stale user-layer overrides -- straight into the committed
	// Default ini, resurrecting rejected defaults over manual reverts. GetCurrentTest() is
	// non-null for a test's whole lifecycle and null for every real-user edit, so Project Settings
	// persistence outside automation behaves exactly as before.
	if (FAutomationTestFramework::Get().GetCurrentTest() != nullptr)
	{
		return;
	}

	TryUpdateDefaultConfigFile();
}
#endif

FString UAudioExporterSettings::ResolveEffectiveExportBaseDirectory() const
{
	if (!ExportDirectory.IsEmpty())
	{
		return ExportDirectory;
	}

	return FPaths::ProjectSavedDir() / TEXT("AudioExporter");
}

FAudioExporterConfig UAudioExporterSettings::ToConfig() const
{
	FAudioExporterConfig Config;
	Config.OneShotVariations = OneShotVariations;
	Config.LoopVariations = LoopVariations;
	Config.MaxLengthSeconds = MaxLength;
	Config.bNormalize = bNormalize;
	Config.bMonitor = bMonitor;
	Config.bExportTextLog = bExportTextLog;
	return Config;
}

void UAudioExporterSettings::ApplyFromConfig(const FAudioExporterConfig& InConfig)
{
	OneShotVariations = InConfig.OneShotVariations;
	LoopVariations = InConfig.LoopVariations;
	MaxLength = InConfig.MaxLengthSeconds;
	bNormalize = InConfig.bNormalize;
	bMonitor = InConfig.bMonitor;
	bExportTextLog = InConfig.bExportTextLog;
}

#undef LOCTEXT_NAMESPACE
