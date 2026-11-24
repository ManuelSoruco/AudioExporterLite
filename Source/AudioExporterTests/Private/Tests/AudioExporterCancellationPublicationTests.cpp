// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Utilities/AudioExporterUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAudioExporterCancellationPublication_LateArtifactRemoved,
	"AudioFactory.AudioExporter.Runtime.Cancellation.LatePublishedArtifactRemoved",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::CommandletContext
		| EAutomationTestFlags::ProductFilter)

bool FAudioExporterCancellationPublication_LateArtifactRemoved::RunTest(const FString& Parameters)
{
	const FString ExportPath = FPaths::ProjectSavedDir()
		/ TEXT("Automation/AudioExporter/LatePublication")
		/ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	IFileManager& FileManager = IFileManager::Get();
	FileManager.DeleteDirectory(*ExportPath, false, true);
	FAudioExporterUtils::ClearExportPathCancellation(ExportPath);

	constexpr int32 SampleRate = 48000;
	constexpr int32 NumChannels = 1;
	constexpr int32 FrameCount = 480;
	TArray<float> Buffer;
	Buffer.SetNumUninitialized(FrameCount * NumChannels);
	for (int32 FrameIndex = 0; FrameIndex < FrameCount; ++FrameIndex)
	{
		Buffer[FrameIndex] = FrameIndex == 0 ? 0.5f : 0.1f;
	}

	FAudioExporterUtils::SetPostPublicationHookForTesting([ExportPath]()
	{
		// This is the exact race: the atomic WAV write has succeeded, but the background save has
		// not yet returned to the pipeline's game-thread retirement callback. Cancellation wins now.
		FAudioExporterUtils::MarkExportPathCancelled(ExportPath);
	});

	float GainAppliedDb = 0.0f;
	float PeakDb = 0.0f;
	float RMSDb = 0.0f;
	FString OutputFilePath;
	FString FailureReason;
	const bool bSaved = FAudioExporterUtils::ProcessAndSaveVariation(
		Buffer,
		TEXT("LatePublicationFixture"),
		FString(),
		0,
		ExportPath,
		SampleRate,
		NumChannels,
		false,
		true,
		GainAppliedDb,
		PeakDb,
		RMSDb,
		OutputFilePath,
		&FailureReason);

	TestFalse(TEXT("A WAV whose publication loses to cancellation must not report success."), bSaved);
	TestTrue(TEXT("A cancelled late publication must expose no successful output path."), OutputFilePath.IsEmpty());
	TestTrue(TEXT("The cancellation result must state that the late artifact was removed."),
		FailureReason.Contains(TEXT("late artifact was removed")));

	TArray<FString> RemainingWavFiles;
	FileManager.FindFilesRecursive(
		RemainingWavFiles,
		*ExportPath,
		TEXT("*.wav"),
		true,
		false);
	TestTrue(TEXT("Cancellation after atomic publication must leave no WAV artifact on disk."), RemainingWavFiles.IsEmpty());

	FAudioExporterUtils::SetPostPublicationHookForTesting(nullptr);
	FAudioExporterUtils::ClearExportPathCancellation(ExportPath);
	FileManager.DeleteDirectory(*ExportPath, false, true);
	return true;
}
