// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"

#ifndef AF_UTILS_API
#define AF_UTILS_API
#endif

/**
 * Product-neutral filtering options for audio asset categorization.
 */
namespace AudioExporter {
struct AF_UTILS_API FAudioAssetFilterOptions
{
	bool bIncludeSoundWaves = true;
	bool bIncludeSoundCues = true;
	bool bIncludeMetaSounds = true;
};

/**
 * Optional detailed info for a SoundWave filtered out because it is referenced by a higher-level asset.
 */
struct AF_UTILS_API FAudioFilteredAssetInfo
{
	FString AssetName;
	FString AssetPath;
	TArray<FString> ReferencerPaths;
};

enum class EAudioAssetKind : uint8
{
	Unsupported,
	ImportedSoundWave,
	ProceduralSoundWave,
	MetaSound,
	SoundCue
};

/**
 * AudioAssetFilteringUtils
 *
 * Classifies by UClass inheritance, checking MetaSounds before their SoundWave base class,
 * then filters FAssetData into SoundWaves, SoundCues, and MetaSounds, while identifying
 * SoundWaves that are redundant because they are already referenced by higher-level assets.
 *
 * Product-neutral:
 * - no dependency on product settings classes
 * - no dependency on product log/CSV structs
 */
namespace AudioAssetFilteringUtils
{
	/** Classifies by inheritance. MetaSound and procedural waves are checked before generic SoundWave. */
	AF_UTILS_API EAudioAssetKind ClassifyAudioAsset(const FAssetData& InAsset);

	AF_UTILS_API void FilterAudioAssets(
		const TArray<FAssetData>& InAssets,
		TArray<FAssetData>& OutSoundWaves,
		TArray<FAssetData>& OutSoundCues,
		TArray<FAssetData>& OutMetaSounds,
		const FAudioAssetFilterOptions& InOptions = FAudioAssetFilterOptions(),
		TArray<FAssetData>* OutReferencedWaves = nullptr,
		TArray<FAudioFilteredAssetInfo>* OutFilteredInfo = nullptr);

#if WITH_DEV_AUTOMATION_TESTS
	/** Exercises the production dependency traversal against a controlled package graph. */
	AF_UTILS_API void GatherDependencyChainsForTesting(
		FName RootPackage,
		const TMap<FName, TArray<FName>>& DependencyGraph,
		TMap<FName, TArray<FString>>& OutPackageToChains);
#endif
}
} // namespace AudioExporter
