// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "Misc/AutomationTest.h"

#include "AudioExporterRuntimeSubsystem.h"
#include "Engine/Engine.h"

#include "AssetRegistry/AssetData.h"
#include "Misc/PackageName.h"
#include "Sound/SoundCue.h"
#include "UObject/Package.h"
#include "Utilities/AudioExporterManager.h"
#include "Utilities/AudioExporterSettings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterEditorRuntimeSubsystemAvailabilityTest,
	"AudioFactory.AudioExporter.Editor.RuntimeSubsystemAvailability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterEditorRuntimeSubsystemAvailabilityTest::RunTest(const FString& Parameters)
{
	TestNotNull(TEXT("GEngine should be available in editor automation tests."), GEngine);
	if (!GEngine)
	{
		return false;
	}

	UAudioExporterRuntimeSubsystem* Subsystem = GEngine->GetEngineSubsystem<UAudioExporterRuntimeSubsystem>();
	TestNotNull(TEXT("AudioExporter runtime subsystem should be registered."), Subsystem);
	if (!Subsystem)
	{
		return false;
	}

	TestFalse(TEXT("Runtime subsystem should not report an active export at test start."), Subsystem->IsExporting());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterManagerResolvedOrderMatchesPipelineQueueTest,
	"AudioFactory.AudioExporter.Editor.Manager.ResolvedOrderMatchesPipelineQueue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterManagerResolvedOrderMatchesPipelineQueueTest::RunTest(const FString& Parameters)
{
	// ResolveAssets() sorts a multi-folder selection and drops assets that fail to resolve to a
	// USoundBase; StartExporting must broadcast that exact resolved/sorted/filtered list via
	// OnAudioExportStarted, not the raw selection, or the progress window's rows (built from the
	// broadcast, in broadcast order) land on the wrong pipeline index (which walks SoundQueue, built
	// from the same resolved list). This exercises ResolveAssets() through the same public test seam
	// StartExporting uses internally, with no /Game fixtures required.

	auto MakeTransientAssetData = [](UPackage* Package, UObject* Object, const FTopLevelAssetPath& ClassPath) -> FAssetData
	{
		return FAssetData(
			Package->GetFName(),
			FName(*FPackageName::GetLongPackagePath(Package->GetName())),
			Object->GetFName(),
			ClassPath);
	};

	const FTopLevelAssetPath SoundCueClassPath = USoundCue::StaticClass()->GetClassPathName();

	// Three real, transient SoundCue assets under different folders so FAssetData::operator< (the
	// same comparator ResolveAssets' FilteredAssets.Sort() uses) reorders them relative to the
	// deliberately shuffled input order below.
	UPackage* PackageA = NewObject<UPackage>(nullptr, TEXT("/Temp/AudioExporterAEL3Test/A_Folder/SoundA"), RF_Public);
	UPackage* PackageB = NewObject<UPackage>(nullptr, TEXT("/Temp/AudioExporterAEL3Test/B_Folder/SoundB"), RF_Public);
	UPackage* PackageC = NewObject<UPackage>(nullptr, TEXT("/Temp/AudioExporterAEL3Test/C_Folder/SoundC"), RF_Public);
	TestNotNull(TEXT("Transient package A should be created."), PackageA);
	TestNotNull(TEXT("Transient package B should be created."), PackageB);
	TestNotNull(TEXT("Transient package C should be created."), PackageC);
	if (!PackageA || !PackageB || !PackageC)
	{
		return false;
	}

	USoundCue* SoundA = NewObject<USoundCue>(PackageA, TEXT("SoundA"), RF_Public);
	USoundCue* SoundB = NewObject<USoundCue>(PackageB, TEXT("SoundB"), RF_Public);
	USoundCue* SoundC = NewObject<USoundCue>(PackageC, TEXT("SoundC"), RF_Public);
	TestNotNull(TEXT("Transient SoundCue A should be created."), SoundA);
	TestNotNull(TEXT("Transient SoundCue B should be created."), SoundB);
	TestNotNull(TEXT("Transient SoundCue C should be created."), SoundC);
	if (!SoundA || !SoundB || !SoundC)
	{
		return false;
	}

	// A "load failure" entry: it claims (via AssetClassPath) to be a SoundCue, so it passes the
	// upstream class filter exactly like a real asset would, but the object actually at that path is
	// not a USoundBase -- ResolveAssets' Cast<USoundBase> fails and IsValid() rejects it, mirroring a
	// real failed asset load without depending on engine load-failure logging. Uses a concrete,
	// non-abstract UObject subtype (plain UObject trips UE 5.6's "abstract class" NewObject ensure).
	UPackage* DecoyPackage = NewObject<UPackage>(nullptr, TEXT("/Temp/AudioExporterAEL3Test/A_Folder/Decoy"), RF_Public);
	TestNotNull(TEXT("Transient decoy package should be created."), DecoyPackage);
	if (!DecoyPackage)
	{
		return false;
	}
	UAudioExporterSettings* DecoyObject = NewObject<UAudioExporterSettings>(DecoyPackage, TEXT("Decoy"), RF_Public);
	TestNotNull(TEXT("Transient decoy object should be created."), DecoyObject);
	if (!DecoyObject)
	{
		return false;
	}

	const FAssetData AssetDataA = MakeTransientAssetData(PackageA, SoundA, SoundCueClassPath);
	const FAssetData AssetDataB = MakeTransientAssetData(PackageB, SoundB, SoundCueClassPath);
	const FAssetData AssetDataC = MakeTransientAssetData(PackageC, SoundC, SoundCueClassPath);
	const FAssetData AssetDataDecoy = MakeTransientAssetData(DecoyPackage, DecoyObject, SoundCueClassPath);

	// Deliberately unsorted, multi-folder selection with the unloadable decoy in the middle -- exactly
	// a multi-folder Content Browser selection with one failed load.
	const TArray<FAssetData> Selection = { AssetDataC, AssetDataDecoy, AssetDataA, AssetDataB };

	const FAudioExportManager Manager;
	const TArray<FAssetData> Resolved = Manager.ResolveOrderedAssetsForTesting(Selection);

	TestEqual(TEXT("The unloadable decoy entry should be dropped from the resolved/broadcast order."), Resolved.Num(), 3);
	if (Resolved.Num() == 3)
	{
		TestEqual(TEXT("The resolved order should be sorted, not selection order (first entry)."), Resolved[0].AssetName, FName(TEXT("SoundA")));
		TestEqual(TEXT("The resolved order should be sorted, not selection order (second entry)."), Resolved[1].AssetName, FName(TEXT("SoundB")));
		TestEqual(TEXT("The resolved order should be sorted, not selection order (third entry)."), Resolved[2].AssetName, FName(TEXT("SoundC")));
	}

	return true;
}

