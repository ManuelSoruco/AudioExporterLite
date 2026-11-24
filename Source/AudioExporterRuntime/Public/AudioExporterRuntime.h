// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * FAudioExporterRuntimeModule
 * 
 * The runtime module for the Audio Exporter plugin.
 * Contains core audio processing logic, utilities, and the export pipeline
 * that can run both in Editor and in packaged builds.
 */
class AUDIOEXPORTERRUNTIME_API FAudioExporterRuntimeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override {}
	virtual void ShutdownModule() override {}
};
