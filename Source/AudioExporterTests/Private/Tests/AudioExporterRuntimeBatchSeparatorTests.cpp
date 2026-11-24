// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "Misc/AutomationTest.h"

#include "AudioExportPipeline.h"
#include "AudioExporterRuntimeSubsystem.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Sound/SoundBase.h"
#include "Utilities/AudioRendererRealtime.h"
#include "UObject/UObjectGlobals.h"

namespace AudioExporterBatchSeparatorTestUtils
{
	static UWorld* ResolveAutomationWorld()
	{
		if (GEditor && GEngine)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.WorldType == EWorldType::Editor || Context.WorldType == EWorldType::PIE)
				{
					return Context.World();
				}
			}
		}

		return GWorld;
	}

	static FString MakeOutputDirectory(const FString& TestName)
	{
		const FString OutputDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("RealPipeline"), TEXT("AudioExporter"), TestName);
		IFileManager::Get().DeleteDirectory(*OutputDirectory, false, true);
		IFileManager::Get().MakeDirectory(*OutputDirectory, true);
		return OutputDirectory;
	}

	static FString MakeRelativeAssetPath(const USoundBase* Sound)
	{
		FString RelativeAssetPath = Sound != nullptr
			? FPackageName::GetLongPackagePath(Sound->GetPackage()->GetName())
			: FString();
		if (RelativeAssetPath.StartsWith(TEXT("/Game")))
		{
			RelativeAssetPath.RemoveAt(0, 5);
		}
		if (RelativeAssetPath.StartsWith(TEXT("/")))
		{
			RelativeAssetPath.RemoveAt(0, 1);
		}
		return RelativeAssetPath;
	}

	struct FAttendedBatchState
	{
		FAutomationTestBase* Test = nullptr;
		UAudioExporterRuntimeSubsystem* RuntimeSubsystem = nullptr;
		FDelegateHandle CompleteHandle;
		FDelegateHandle ResultHandle;
		FDelegateHandle ProgressHandle;
		FString TestName;
		FString OutputRoot;
		int32 TerminalEvents = 0;
		int32 MaxVariationSeen = 0;
		int32 SuccessfulCount = 0;
		bool bOutcomeReceived = false;
		EAudioExportOutcome Outcome = EAudioExportOutcome::Failed;
		FString FinishedPath;
		double StartSeconds = 0.0;
		bool bFinished = false;
	};

	/** House bounded-wait idiom (30 s): the attended batch must reach its terminal event within
	 *  the same budget the RealPipeline siblings use; expiry is a loud AddError plus a cancel,
	 *  never a silent pass. */
	class FWaitForAttendedBatchTerminal : public IAutomationLatentCommand
	{
	public:
		explicit FWaitForAttendedBatchTerminal(const TSharedRef<FAttendedBatchState>& InState, const double InTimeoutSeconds = 30.0)
			: State(InState)
			, TimeoutSeconds(InTimeoutSeconds)
		{
		}

		virtual bool Update() override
		{
			if (State->bFinished && State->bOutcomeReceived)
			{
				return true;
			}

			if ((FPlatformTime::Seconds() - State->StartSeconds) > TimeoutSeconds)
			{
				if (State->Test)
				{
					State->Test->AddError(FString::Printf(TEXT("%s: the attended batch separator flow did not reach a terminal result within %.0f s "
						"(terminal events=%d, max variation seen=%d). Headless hosts never tick the world TimerManager, so the separator is "
						"anchored on the engine core ticker -- if this fires again, trace why CompleteRender's "
						"separator callback never executed."),
						*State->TestName, TimeoutSeconds, State->TerminalEvents, State->MaxVariationSeen));
				}
				if (State->RuntimeSubsystem && State->RuntimeSubsystem->IsExporting())
				{
					State->RuntimeSubsystem->CancelExport(false);
				}
				return true;
			}

			return false;
		}

	private:
		TSharedRef<FAttendedBatchState> State;
		double TimeoutSeconds = 30.0;
	};

	class FVerifyAttendedBatchTerminal : public IAutomationLatentCommand
	{
	public:
		explicit FVerifyAttendedBatchTerminal(const TSharedRef<FAttendedBatchState>& InState)
			: State(InState)
		{
		}

		virtual bool Update() override
		{
			// Disarm FIRST on every path so a hung/red run can never leak the forced branch
			// into any later test in the same process.
			FAudioRendererRealtime::SetForceAttendedSeparatorBranchForTesting(false);

			FAutomationTestBase* Test = State->Test;
			if (!Test)
			{
				return true;
			}

			if (State->RuntimeSubsystem && State->CompleteHandle.IsValid())
			{
				State->RuntimeSubsystem->OnExportCompleteNative.Remove(State->CompleteHandle);
			}
			if (State->RuntimeSubsystem && State->ResultHandle.IsValid())
			{
				State->RuntimeSubsystem->OnExportCompleteResultNative.Remove(State->ResultHandle);
			}
			if (State->RuntimeSubsystem && State->ProgressHandle.IsValid())
			{
				State->RuntimeSubsystem->OnExportProgressNative.Remove(State->ProgressHandle);
			}

			const FString Label = State->TestName;
			Test->TestEqual(FString::Printf(TEXT("%s should emit exactly one completion delegate."), *Label), State->TerminalEvents, 1);
			Test->TestTrue(FString::Printf(TEXT("%s should receive the structured terminal result."), *Label), State->bOutcomeReceived);
			Test->TestEqual(FString::Printf(TEXT("%s structured outcome should be Succeeded."), *Label), State->Outcome, EAudioExportOutcome::Succeeded);
			Test->TestEqual(FString::Printf(TEXT("%s should report exactly 2 successful variation files."), *Label), State->SuccessfulCount, 2);
			Test->TestFalse(FString::Printf(TEXT("%s completion path should not be empty."), *Label), State->FinishedPath.IsEmpty());

			// The decisive observable: progress must have advanced INTO variation 2.
			Test->TestEqual(FString::Printf(TEXT("%s progress should advance into variation 2 past the separator."), *Label), State->MaxVariationSeen, 2);

			TArray<FString> WavFiles;
			const FString OutputRootFull = FPaths::ConvertRelativePathToFull(State->OutputRoot);
			IFileManager::Get().FindFilesRecursive(WavFiles, *OutputRootFull, TEXT("*.wav"), true, false);
			Test->TestEqual(FString::Printf(TEXT("%s should produce exactly one WAV per variation (2 total)."), *Label), WavFiles.Num(), 2);

			return true;
		}

	private:
		TSharedRef<FAttendedBatchState> State;
	};
}

// Regression coverage: during an ATTENDED batch
// export, CompleteRender arms the one-shot SeparatorTimerHandle (SeparatorSilence of silence between
// variations) and THEN routes through the shared ReleasePlaybackObjects teardown, which unconditionally
// ClearTimer(SeparatorTimerHandle)s it -- so OnVariationFinished never fires and the session hangs
// forever after the first variation. The unattended branch completes the separator inline, which is
// why every existing headless test is blind to the defect.
//
// This test expresses the ATTENDED path headlessly through the SetForceAttendedSeparatorBranchForTesting
// seam (test-only, compiles out of Shipping): it drives a real 2-variation SoundCue export through the
// public subsystem pipeline with the attended timer branch forced, and requires the session to reach
// terminal SUCCESS -- progress advanced into variation 2, one WAV per variation, Succeeded outcome --
// inside a bounded 30 s wait.
//
// Headless automation hosts never tick the injected world's TimerManager (no separator fire AND no 10 s StartTimeout error across 30 s of
// advanced frames). The runtime fix anchors the forced-attended separator on the engine core ticker.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterRealAttendedBatchSeparatorTest,
	"AudioFactory.AudioExporter.Runtime.RealPipeline.AttendedBatchSeparator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterRealAttendedBatchSeparatorTest::RunTest(const FString& Parameters)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("NoSound")))
	{
		AddError(TEXT("Attended batch separator coverage is unavailable: this required live-audio regression was launched with -NoSound."));
		return false;
	}

	// SoundCueOneShot ships checked into this repo's Content/ as a REQUIRED release-case fixture
	// (Docs/AutomationFixtures.md); a missing required fixture is a real regression -- loud failure,
	// never a skip. A SoundCue is NOT a static source, so GetTotalVariations honours OneShotVariations=2.
	USoundBase* FixtureSound = LoadObject<USoundBase>(nullptr, TEXT("/Game/SoundCueOneShot.SoundCueOneShot"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!TestNotNull(TEXT("Required fixture '/Game/SoundCueOneShot.SoundCueOneShot' should be present in this project."), FixtureSound))
	{
		return false;
	}

	UWorld* World = AudioExporterBatchSeparatorTestUtils::ResolveAutomationWorld();
	if (!TestNotNull(TEXT("Attended batch separator test should have an editor world for the real runtime pipeline."), World))
	{
		return false;
	}

	UAudioExporterRuntimeSubsystem* RuntimeSubsystem = GEngine ? GEngine->GetEngineSubsystem<UAudioExporterRuntimeSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("Attended batch separator test should resolve the AudioExporter runtime subsystem."), RuntimeSubsystem))
	{
		return false;
	}

	if (RuntimeSubsystem->IsExporting())
	{
		RuntimeSubsystem->CancelExport(false);
	}

	const TSharedRef<AudioExporterBatchSeparatorTestUtils::FAttendedBatchState> State =
		MakeShared<AudioExporterBatchSeparatorTestUtils::FAttendedBatchState>();
	State->Test = this;
	State->RuntimeSubsystem = RuntimeSubsystem;
	State->TestName = TEXT("AttendedBatchSeparator");
	State->OutputRoot = AudioExporterBatchSeparatorTestUtils::MakeOutputDirectory(TEXT("RealPipeline_AttendedBatchSeparator"));
	State->StartSeconds = FPlatformTime::Seconds();

	State->CompleteHandle = RuntimeSubsystem->OnExportCompleteNative.AddLambda([State](const FString& InPath)
	{
		++State->TerminalEvents;
		State->FinishedPath = InPath;
		State->bFinished = true;
	});
	State->ResultHandle = RuntimeSubsystem->OnExportCompleteResultNative.AddLambda([State](const FAudioExportResult& InResult)
	{
		State->bOutcomeReceived = true;
		State->Outcome = InResult.Outcome;
		State->SuccessfulCount = InResult.SuccessfulCount;
	});
	State->ProgressHandle = RuntimeSubsystem->OnExportProgressNative.AddLambda([State](const FAudioExportProgressInfo& InInfo)
	{
		State->MaxVariationSeen = FMath::Max(State->MaxVariationSeen, InInfo.CurrentVariation);
	});

	FAudioExportSoundEntry Entry;
	Entry.Sound = FixtureSound;
	Entry.SoundName = FixtureSound->GetName();
	Entry.SourceAssetPath = FixtureSound->GetPackage()->GetName();
	Entry.RelativePath = AudioExporterBatchSeparatorTestUtils::MakeRelativeAssetPath(FixtureSound);

	FAudioExporterConfig Config;
	Config.OneShotVariations = 2;
	Config.LoopVariations = 1;
	Config.MaxLengthSeconds = 5; // The supported minimum; FAudioExporterConfig::Sanitize clamps below it.
	Config.bNormalize = false;
	Config.bMonitor = false;
	Config.bExportTextLog = true;

	TArray<FAudioExportSoundEntry> Entries;
	Entries.Add(Entry);

	// Arm the test-only attended-branch override immediately before the start request so every
	// fallible step above has already passed; the verify latent command disarms it on all paths.
	FAudioRendererRealtime::SetForceAttendedSeparatorBranchForTesting(true);

	const bool bStarted = RuntimeSubsystem->StartExport(World, Entries, Config, State->OutputRoot);
	if (!TestTrue(TEXT("Attended batch separator export should start through the real AudioExporter runtime pipeline."), bStarted))
	{
		FAudioRendererRealtime::SetForceAttendedSeparatorBranchForTesting(false);
		RuntimeSubsystem->OnExportCompleteNative.Remove(State->CompleteHandle);
		RuntimeSubsystem->OnExportCompleteResultNative.Remove(State->ResultHandle);
		RuntimeSubsystem->OnExportProgressNative.Remove(State->ProgressHandle);
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterBatchSeparatorTestUtils::FWaitForAttendedBatchTerminal(State));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterBatchSeparatorTestUtils::FVerifyAttendedBatchTerminal(State));
	return true;
}
