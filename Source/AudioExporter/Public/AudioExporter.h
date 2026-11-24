// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "Modules/ModuleManager.h"

class FAudioExportManager;
class SWindow;
struct FAudioExportProgressInfo;

/**
 * FAudioExporterModule
 * 
 * Thin editor wrapper for AudioExporter.
 * Owns Project Settings registration, Content Browser integration, and the
 * editor-only window-launch/progress glue around the runtime/UI modules.
 */
class AUDIOEXPORTER_API FAudioExporterModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;

	virtual void ShutdownModule() override;

#if WITH_DEV_AUTOMATION_TESTS
	/** Opens the real Content Browser settings path for busy-refusal automation. */
	void OpenSettingsWindowForTesting(const TArray<FAssetData>& SelectedAssets);
	void SetBusyForTesting(bool bInBusy);
	TSharedPtr<SWindow> GetSettingsWindowForTesting() const { return SettingsWindow.Pin(); }
#endif

private:
	void RegisterToolMenus();
	FAudioExportManager* GetExportManager(const TCHAR* FunctionName) const;
	void OpenSettingsWindow(const TArray<FAssetData>& SelectedAssets);
	void StartExportFromWindow(const TArray<FAssetData>& SelectedAssets);
	FString PromptForExportDirectory(const FString& StartingPath) const;
	void OnAudioExportStarted(const TArray<FAssetData>& InAssets);
	void OnAudioExportProgressUpdated(const FAudioExportProgressInfo& ProgressInfo);
	void OnAudioExportFinished(const FString& ExportPath);

	TUniquePtr<FAudioExportManager> ExportManager;
	TWeakPtr<SWindow> SettingsWindow;
	TWeakPtr<SWindow> ProgressParentWindow;
};
