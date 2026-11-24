// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "AssetRegistry/AssetData.h"

class UAudioExporterSettings;

DECLARE_DELEGATE_OneParam(FOnAudioExporterRequested, const TArray<FAssetData>&);
DECLARE_DELEGATE_RetVal_OneParam(FString, FOnAudioExporterRequestExportDirectory, const FString& /*StartingPath*/);

/**
 * Compatibility entry point for the native Audio Exporter settings shell.
 */
class AUDIOEXPORTERUI_API SAudioExporterWindow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAudioExporterWindow)
	{}
		/** The assets selected for export. */
		SLATE_ARGUMENT(TArray<FAssetData>, Assets)
		SLATE_EVENT(FOnAudioExporterRequested, OnExportRequested)
		SLATE_EVENT(FOnAudioExporterRequestExportDirectory, OnSelectExportDirectory)
	SLATE_END_ARGS()

	/** Creates a settings window hosting this widget. */
	static TSharedRef<SWindow> OpenSettingsWindow(
		const TArray<FAssetData>& InAssets,
		const FOnAudioExporterRequested& InOnExportRequested,
		const FOnAudioExporterRequestExportDirectory& InOnSelectExportDirectory);

#if WITH_DEV_AUTOMATION_TESTS
	/**
	 * Test seam: drives the exact same asset filter/counter
	 * OpenSettingsWindow uses (AudioExporterWindowPrivate::FilterSupportedAssets) so SoundWave
	 * discovery and counting can be asserted against the real production code path instead of a
	 * re-implemented copy.
	 */
	struct FSupportedAssetCountsForTesting
	{
		int32 NumSupported = 0;
		int32 NumMetaSounds = 0;
		int32 NumSoundCues = 0;
		int32 NumSoundWaves = 0;
	};
	static FSupportedAssetCountsForTesting GetSupportedAssetCountsForTesting(const TArray<FAssetData>& InAssets);

	/** Test seam for the real per-asset class label/icon resolution (same functions BuildSelectedAssetRows uses). */
	static FText GetAssetClassLabelForTesting(const FAssetData& AssetData);
	static FName GetAssetIconNameForTesting(const FAssetData& AssetData);
	static FText GetDisplayedExportPathForTesting(const UAudioExporterSettings* Settings);
	static FString GetExportBrowseStartingDirectoryForTesting(const UAudioExporterSettings* Settings);

	/**
	 * The Selected Assets window (AudioExporterWindowPrivate::OpenSelectedAssetsWindow) otherwise
	 * has no entry point reachable outside the "Show Selected" footer action nested inside the
	 * adapter-built settings frame, so its title / Esc-close cannot be exercised by a test through
	 * the real settings-window flow. This drives the exact same window-construction code directly.
	 */
	static TSharedRef<SWindow> OpenSelectedAssetsWindowForTesting(const TArray<FAssetData>& InAssets, const TSharedPtr<SWindow>& ParentWindow = nullptr);
#endif
};
