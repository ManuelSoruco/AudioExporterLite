// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "Assets/AudioAssetFilteringUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "MetasoundSource.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundWave.h"
#include "Sound/SoundWaveProcedural.h"
namespace AudioExporter {

namespace
{
	struct FDependencyVisit
	{
		FName PackageName;
		FString Chain;
	};

	void GatherDependencyChains(
		const FName RootPackage,
		TFunctionRef<void(FName, TArray<FName>&)> ResolveDependencies,
		TMap<FName, TArray<FString>>& OutPackageToChains)
	{
		TSet<FName> VisitedPackages;
		TArray<FDependencyVisit> PendingDependencies;
		VisitedPackages.Add(RootPackage);
		PendingDependencies.Add({ RootPackage, RootPackage.ToString() });

		for (int32 VisitIndex = 0; VisitIndex < PendingDependencies.Num(); ++VisitIndex)
		{
			const FName CurrentPackage = PendingDependencies[VisitIndex].PackageName;
			const FString CurrentChain = PendingDependencies[VisitIndex].Chain;
			TArray<FName> Dependencies;
			ResolveDependencies(CurrentPackage, Dependencies);
			for (const FName& Dependency : Dependencies)
			{
				if (VisitedPackages.Contains(Dependency))
				{
					continue;
				}

				VisitedPackages.Add(Dependency);
				const FString DependencyChain = CurrentChain + TEXT(" -> ") + Dependency.ToString();
				OutPackageToChains.FindOrAdd(Dependency).Add(DependencyChain);
				PendingDependencies.Add({ Dependency, DependencyChain });
			}
		}
	}
}

EAudioAssetKind AudioAssetFilteringUtils::ClassifyAudioAsset(const FAssetData& InAsset)
{
	const UClass* AssetClass = InAsset.GetClass(EResolveClass::Yes);
	// An unresolved asset class is unsupported; it must not be guessed into a product menu.
	if (!AssetClass)
	{
		return EAudioAssetKind::Unsupported;
	}

	// MetaSound is checked before generic SoundWave/SoundBase inheritance so derived sources never fall through.
	if (AssetClass->IsChildOf(UMetaSoundSource::StaticClass()))
	{
		return EAudioAssetKind::MetaSound;
	}

	// Procedural waves are not imported SoundWaves and are not Converter candidates.
	if (AssetClass->IsChildOf(USoundWaveProcedural::StaticClass()))
	{
		return EAudioAssetKind::ProceduralSoundWave;
	}

	if (AssetClass->IsChildOf(USoundCue::StaticClass()))
	{
		return EAudioAssetKind::SoundCue;
	}

	if (AssetClass->IsChildOf(USoundWave::StaticClass()))
	{
		return EAudioAssetKind::ImportedSoundWave;
	}

	return EAudioAssetKind::Unsupported;
}

void AudioAssetFilteringUtils::FilterAudioAssets(
	const TArray<FAssetData>& InAssets,
	TArray<FAssetData>& OutSoundWaves,
	TArray<FAssetData>& OutSoundCues,
	TArray<FAssetData>& OutMetaSounds,
	const FAudioAssetFilterOptions& InOptions,
	TArray<FAssetData>* OutReferencedWaves,
	TArray<FAudioFilteredAssetInfo>* OutFilteredInfo)
{
	OutSoundWaves.Empty();
	OutSoundCues.Empty();
	OutMetaSounds.Empty();
	if (OutReferencedWaves) OutReferencedWaves->Empty();
	if (OutFilteredInfo) OutFilteredInfo->Empty();

	if (InAssets.IsEmpty())
	{
		return;
	}

	TMap<FName, TArray<FString>> PackageToReferencersMap;
	TArray<FAssetData> PotentialSoundWaves;

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	// Pass 1: categorize selected assets and collect dependencies of higher-level procedural assets.
	// A higher-level asset only suppresses its referenced waves when that asset type is actually
	// included in the run: a disabled SoundCue/MetaSound must never remove an enabled SoundWave,
	// otherwise enabling waves behind a disabled referencer type silently drops them.
	for (const FAssetData& AssetData : InAssets)
	{
		const EAudioAssetKind AssetKind = ClassifyAudioAsset(AssetData);
		bool bIsIncludedHigherLevelAsset = false;

		switch (AssetKind)
		{
		case EAudioAssetKind::MetaSound:
			if (InOptions.bIncludeMetaSounds)
			{
				OutMetaSounds.Add(AssetData);
				bIsIncludedHigherLevelAsset = true;
			}
			break;
		case EAudioAssetKind::SoundCue:
			if (InOptions.bIncludeSoundCues)
			{
				OutSoundCues.Add(AssetData);
				bIsIncludedHigherLevelAsset = true;
			}
			break;
		case EAudioAssetKind::ImportedSoundWave:
			if (InOptions.bIncludeSoundWaves)
			{
				PotentialSoundWaves.Add(AssetData);
			}
			break;
		case EAudioAssetKind::ProceduralSoundWave:
		case EAudioAssetKind::Unsupported:
		default:
			break;
		}

		if (bIsIncludedHigherLevelAsset)
		{
			// Traverse the complete package dependency graph. A direct-only lookup misses waves
			// behind MetaSound presets, nested patches, and chained procedural assets, which lets
			// the same audible source be normalized twice. Keep one visited set per selected root
			// so cycles terminate while independent roots still report their own chains.
			GatherDependencyChains(
				AssetData.PackageName,
				[&AssetRegistry](const FName PackageName, TArray<FName>& OutDependencies)
				{
					AssetRegistry.GetDependencies(PackageName, OutDependencies);
				},
				PackageToReferencersMap);
		}
	}

	// Pass 2: remove SoundWaves already referenced by SoundCue/MetaSound assets.
	for (const FAssetData& AssetData : PotentialSoundWaves)
	{
		if (!PackageToReferencersMap.Contains(AssetData.PackageName))
		{
			OutSoundWaves.Add(AssetData);
		}
		else
		{
			if (OutReferencedWaves)
			{
				OutReferencedWaves->Add(AssetData);
			}

			if (OutFilteredInfo)
			{
				FAudioFilteredAssetInfo Info;
				Info.AssetName = AssetData.AssetName.ToString();
				Info.AssetPath = AssetData.PackageName.ToString();
				Info.ReferencerPaths = PackageToReferencersMap[AssetData.PackageName];
				OutFilteredInfo->Add(MoveTemp(Info));
			}
		}
	}
}

#if WITH_DEV_AUTOMATION_TESTS
void AudioAssetFilteringUtils::GatherDependencyChainsForTesting(
	const FName RootPackage,
	const TMap<FName, TArray<FName>>& DependencyGraph,
	TMap<FName, TArray<FString>>& OutPackageToChains)
{
	OutPackageToChains.Empty();
	GatherDependencyChains(
		RootPackage,
		[&DependencyGraph](const FName PackageName, TArray<FName>& OutDependencies)
		{
			if (const TArray<FName>* Dependencies = DependencyGraph.Find(PackageName))
			{
				OutDependencies = *Dependencies;
			}
		},
		OutPackageToChains);
}
#endif
} // namespace AudioExporter
