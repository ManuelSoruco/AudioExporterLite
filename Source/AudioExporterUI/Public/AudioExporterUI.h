// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * FAudioExporterUIModule
 * 
 * Runtime-capable UI host for the Audio Exporter plugin.
 * Owns only shared-theme/bootstrap concerns; editor menus and launch glue live in
 * the thin `AudioExporter` editor wrapper module.
 */
class AUDIOEXPORTERUI_API FAudioExporterUIModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
