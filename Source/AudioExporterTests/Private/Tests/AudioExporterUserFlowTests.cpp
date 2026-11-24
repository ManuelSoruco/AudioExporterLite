// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Containers/Ticker.h"
#include "AudioExportPipeline.h"
#include "AudioExporterRuntimeSubsystem.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/CoreDelegates.h"
#include "Internationalization/Regex.h"
#include "MetasoundSource.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundWaveProcedural.h"
#include "Utilities/AudioExporterUtils.h"
#include "Utilities/AudioRendererRealtime.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"

// High-level user-flow tier: every test here drives a REAL user-visible flow (real assets from
// /Game, real public subsystem StartExport entry, real audio device render, real files on disk)
// and verifies outcomes ONLY through independent disk truth or public delegates -- never through
// pipeline internals. Each test states its kill condition in the comment block above it.

namespace AudioExporterUserFlowTestUtils
{
	constexpr double UserFlowDefaultWaitSeconds = 45.0;
	constexpr double UserFlowBatchWaitSeconds = 90.0;
	constexpr double UserFlowAttendedSessionWaitSeconds = 60.0;
	constexpr double UserFlowCancellationDrainSeconds = 5.0;

	// The wind-loop live-capture budget. The loop watchdog polls elapsed mixer audio time on the
	// engine core ticker on every host, so a capped loop settles in ~MaxLengthSeconds plus overhead
	// everywhere; this is generous. A timeout here is a REAL stall and lands as loud evidence
	// through the tolerated-timeout path below instead of hanging forever.
	constexpr double UserFlowLoopLegWaitSeconds = 45.0;

	// Cold DerivedData caveat: the FIRST suite use of the wind asset can additionally spend minutes
	// rebuilding its BINKA cache synchronously (measured), which alone can exhaust any
	// reasonable budget on a cold host; subsequent runs are fast.

	// Proof margin for "the one-shot was NOT clipped": a reintroduced cap-chop lands at ~cap+0.1 s,
	// while any natural length above cap+0.5 s clears this bar with room on both sides.
	constexpr double UserFlowUncappedProofMarginSeconds = 0.25;

	// The watchdog chops loop captures exactly at MaxLengthSeconds; extraction boundaries add a
	// little jitter around the chop point, absorbed by this tolerance.
	constexpr double UserFlowLoopCapToleranceSeconds = 0.75;

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
		const FString OutputDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("UserFlows"), TEXT("AudioExporter"), TestName);
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

	static void FindWavFilesUnder(const FString& InDirectory, TArray<FString>& OutWavFiles)
	{
		OutWavFiles.Reset();
		if (InDirectory.IsEmpty())
		{
			return;
		}

		FString DirectoryFull = FPaths::ConvertRelativePathToFull(InDirectory);
		FPaths::NormalizeDirectoryName(DirectoryFull);
		IFileManager::Get().FindFilesRecursive(OutWavFiles, *DirectoryFull, TEXT("*.wav"), true, false);
	}

	struct FUserFlowWavInfo
	{
		uint16 NumChannels = 0;
		uint32 SampleRate = 0;
		uint16 BitsPerSample = 0;
		uint32 DataSize = 0;
		int64 FrameCount = 0;
		double DurationSeconds = 0.0;
		int16 PeakAmplitude = 0;
	};

	static uint16 ReadUInt16LE(const TArray<uint8>& Bytes, const int64 Offset)
	{
		return static_cast<uint16>(Bytes[Offset] | (Bytes[Offset + 1] << 8));
	}

	static uint32 ReadUInt32LE(const TArray<uint8>& Bytes, const int64 Offset)
	{
		return static_cast<uint32>(Bytes[Offset])
			| (static_cast<uint32>(Bytes[Offset + 1]) << 8)
			| (static_cast<uint32>(Bytes[Offset + 2]) << 16)
			| (static_cast<uint32>(Bytes[Offset + 3]) << 24);
	}

	// Minimal independent RIFF walker (this file's disk-truth parser): walks chunks instead of
	// assuming fixed offsets, computes frames/duration purely from parsed bytes, and measures the
	// int16 peak amplitude across every sample.
	static bool ParseWaveBytes(const TArray<uint8>& WavData, FUserFlowWavInfo& OutInfo, FString* OutError)
	{
		const auto Fail = [OutError](const TCHAR* Message)
		{
			if (OutError)
			{
				*OutError = Message;
			}
			return false;
		};

		if (WavData.Num() < 12 || FMemory::Memcmp(WavData.GetData(), "RIFF", 4) != 0 || FMemory::Memcmp(WavData.GetData() + 8, "WAVE", 4) != 0)
		{
			return Fail(TEXT("not a RIFF/WAVE file"));
		}

		bool bHaveFmt = false;
		bool bHaveData = false;
		int64 DataOffset = -1;
		int64 Offset = 12;
		while (Offset + 8 <= WavData.Num())
		{
			const char* ChunkId = reinterpret_cast<const char*>(WavData.GetData()) + Offset;
			const uint32 ChunkSize = ReadUInt32LE(WavData, Offset + 4);
			const int64 BodyOffset = Offset + 8;

			if (!bHaveFmt && FMemory::Memcmp(ChunkId, "fmt ", 4) == 0 && BodyOffset + 16 <= WavData.Num())
			{
				// fmt body layout: +0 AudioFormat, +2 NumChannels, +4 SampleRate, +12 BlockAlign,
				// +14 BitsPerSample. NumChannels must read at +2; reading +0 yields the format tag
				// (always 1 for PCM) and silently doubles every computed duration.
				OutInfo.NumChannels = ReadUInt16LE(WavData, BodyOffset + 2);
				OutInfo.SampleRate = ReadUInt32LE(WavData, BodyOffset + 4);
				OutInfo.BitsPerSample = ReadUInt16LE(WavData, BodyOffset + 14);
				bHaveFmt = true;
			}
			else if (!bHaveData && FMemory::Memcmp(ChunkId, "data", 4) == 0)
			{
				DataOffset = BodyOffset;
				OutInfo.DataSize = ChunkSize;
				bHaveData = true;
			}

			Offset = BodyOffset + ChunkSize + (ChunkSize & 1);
		}

		if (!bHaveFmt || !bHaveData)
		{
			return Fail(TEXT("missing fmt or data chunk"));
		}
		if (OutInfo.NumChannels == 0 || OutInfo.SampleRate == 0 || OutInfo.BitsPerSample != 16)
		{
			return Fail(TEXT("unsupported PCM format"));
		}

		const int64 BytesPerFrame = static_cast<int64>(OutInfo.NumChannels) * static_cast<int64>(OutInfo.BitsPerSample / 8);
		OutInfo.FrameCount = static_cast<int64>(OutInfo.DataSize) / BytesPerFrame;
		OutInfo.DurationSeconds = static_cast<double>(OutInfo.FrameCount) / static_cast<double>(OutInfo.SampleRate);

		const int64 AvailableDataSize = FMath::Min<int64>(static_cast<int64>(OutInfo.DataSize), WavData.Num() - DataOffset);
		const int64 TotalSamples = AvailableDataSize / static_cast<int64>(sizeof(int16));
		int16 Peak = 0;
		for (int64 SampleIndex = 0; SampleIndex < TotalSamples; ++SampleIndex)
		{
			const int16 Sample = static_cast<int16>(ReadUInt16LE(WavData, DataOffset + (SampleIndex * sizeof(int16))));
			Peak = static_cast<int16>(FMath::Max<int32>(Peak, FMath::Abs(static_cast<int32>(Sample))));
		}
		OutInfo.PeakAmplitude = Peak;
		return true;
	}

	static bool ParseWaveFile(const FString& FilePath, FUserFlowWavInfo& OutInfo, FString* OutError)
	{
		TArray<uint8> WavData;
		if (!FFileHelper::LoadFileToArray(WavData, *FilePath))
		{
			if (OutError)
			{
				*OutError = TEXT("file could not be loaded");
			}
			return false;
		}
		return ParseWaveBytes(WavData, OutInfo, OutError);
	}

	// Asserts the generic health of one produced WAV: parses, non-empty payload, positive duration,
	// audible peak. Returns the parsed info on success so callers can add flow-specific asserts.
	static bool VerifyProducedWavHealth(FAutomationTestBase& Test, const FString& FilePath, const FString& Label, FUserFlowWavInfo& OutInfo)
	{
		FString ParseError;
		if (!Test.TestTrue(FString::Printf(TEXT("%s WAV should parse as RIFF/WAVE PCM from disk bytes."), *Label),
			ParseWaveFile(FilePath, OutInfo, &ParseError)))
		{
			Test.AddError(FString::Printf(TEXT("%s WAV parse failure detail: %s."), *Label, *ParseError));
			return false;
		}

		Test.TestTrue(FString::Printf(TEXT("%s WAV must be non-empty (frames > 0)."), *Label), OutInfo.FrameCount > 0);
		Test.TestTrue(FString::Printf(TEXT("%s WAV must have a positive computed duration."), *Label), OutInfo.DurationSeconds > 0.0);
		Test.TestTrue(FString::Printf(TEXT("%s WAV channel count must be sane (1..2)."), *Label),
			OutInfo.NumChannels >= 1 && OutInfo.NumChannels <= 2);
		Test.TestTrue(FString::Printf(TEXT("%s WAV sample rate must be sane (>= 8000 Hz)."), *Label), OutInfo.SampleRate >= 8000);
		Test.TestTrue(FString::Printf(TEXT("%s WAV must be 16-bit PCM."), *Label), OutInfo.BitsPerSample == 16);
		Test.TestTrue(FString::Printf(TEXT("%s WAV must be non-silent (peak amplitude %d > 0)."), *Label, OutInfo.PeakAmplitude),
			OutInfo.PeakAmplitude > 0);
		return OutInfo.FrameCount > 0 && OutInfo.DurationSeconds > 0.0 && OutInfo.PeakAmplitude > 0;
	}

	static FAudioExporterConfig MakeUserFlowConfig(const int32 MaxLengthSeconds)
	{
		FAudioExporterConfig Config;
		Config.OneShotVariations = 1;
		Config.LoopVariations = 1;
		Config.MaxLengthSeconds = MaxLengthSeconds;
		Config.bNormalize = false;
		Config.bMonitor = false;
		Config.bExportTextLog = true;
		return Config;
	}

	struct FUserFlowExportState
	{
		FAutomationTestBase* Test = nullptr;
		TWeakObjectPtr<UAudioExporterRuntimeSubsystem> RuntimeSubsystem;
		FDelegateHandle CompleteHandle;
		FDelegateHandle ResultHandle;
		FDelegateHandle ProgressHandle;
		FString TestName;
		FString OutputRoot;
		FString FinishedPath;
		int32 LegacyTerminalEvents = 0;
		int32 StructuredTerminalEvents = 0;
		int32 ProgressEvents = 0;
		int32 MaxVariationSeen = 0;
		bool bOutcomeReceived = false;
		bool bFinished = false;
		bool bWriterDrainConfirmed = false;
		bool bWaitTimedOut = false;
		bool bCancellationRequested = false;
		bool bStartRejected = false;
		bool bRunVerifierWhenRejected = false;
		double CancellationDeadlineSeconds = 0.0;
		EAudioExportOutcome Outcome = EAudioExportOutcome::Failed;
		FAudioExportResult Result;
		double StartSeconds = 0.0;
	};

	struct FUserFlowPreflightDrainState
	{
		FAutomationTestBase* Test = nullptr;
		TWeakObjectPtr<UAudioExporterRuntimeSubsystem> RuntimeSubsystem;
		FString TestName;
		FDelegateHandle CompleteHandle;
		FDelegateHandle ResultHandle;
		int32 LegacyTerminalEvents = 0;
		int32 StructuredTerminalEvents = 0;
		FAudioExportResult TerminalResult;
		bool bCancellationRequested = false;
		double DeadlineSeconds = 0.0;
		TSharedPtr<bool> SucceededFlag;
	};

	// A timed-out preflight command cannot release its delegates while the cancelled render may
	// still be broadcasting or draining writers. Keep one explicitly owned, bounded handoff alive
	// until the structured terminal, inactive subsystem, and zero-writer conditions are observed.
	// The engine-pre-exit callback is the shutdown escape hatch; no process-lifetime state is kept.
	struct FUserFlowPreflightCleanupEntry final
	{
		explicit FUserFlowPreflightCleanupEntry(const TSharedRef<FUserFlowPreflightDrainState>& InState)
			: State(InState)
		{
		}

		TSharedRef<FUserFlowPreflightDrainState> State;
		FDelegateHandle EnginePreExitHandle;
		FTSTicker::FDelegateHandle TickerHandle;
		double HardDeadlineSeconds = 0.0;
		bool bReleased = false;
	};

	static TSharedPtr<FUserFlowPreflightCleanupEntry>& GetActiveUserFlowPreflightCleanup()
	{
		static TSharedPtr<FUserFlowPreflightCleanupEntry> ActiveEntry;
		return ActiveEntry;
	}

	static void RemoveUserFlowPreflightDelegates(FUserFlowPreflightDrainState& State)
	{
		if (UAudioExporterRuntimeSubsystem* Subsystem = State.RuntimeSubsystem.Get())
		{
			if (State.CompleteHandle.IsValid())
			{
				Subsystem->OnExportCompleteNative.Remove(State.CompleteHandle);
			}
			if (State.ResultHandle.IsValid())
			{
				Subsystem->OnExportCompleteResultNative.Remove(State.ResultHandle);
			}
		}
		State.CompleteHandle.Reset();
		State.ResultHandle.Reset();
	}

	static void ReleaseUserFlowPreflightCleanup(const TSharedPtr<FUserFlowPreflightCleanupEntry>& Entry)
	{
		if (!Entry.IsValid() || Entry->bReleased)
		{
			return;
		}

		Entry->bReleased = true;
		RemoveUserFlowPreflightDelegates(*Entry->State);
		if (Entry->TickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(Entry->TickerHandle);
			Entry->TickerHandle.Reset();
		}
		if (Entry->EnginePreExitHandle.IsValid())
		{
			FCoreDelegates::OnEnginePreExit.Remove(Entry->EnginePreExitHandle);
			Entry->EnginePreExitHandle.Reset();
		}
		if (GetActiveUserFlowPreflightCleanup() == Entry)
		{
			GetActiveUserFlowPreflightCleanup().Reset();
		}
	}

	static void RegisterBoundedUserFlowPreflightCleanup(const TSharedRef<FUserFlowPreflightDrainState>& State)
	{
		TSharedPtr<FUserFlowPreflightCleanupEntry>& ActiveEntry = GetActiveUserFlowPreflightCleanup();
		if (ActiveEntry.IsValid())
		{
			ReleaseUserFlowPreflightCleanup(ActiveEntry);
		}

		const TSharedPtr<FUserFlowPreflightCleanupEntry> Entry = MakeShared<FUserFlowPreflightCleanupEntry>(State);
		Entry->HardDeadlineSeconds = FPlatformTime::Seconds() + UserFlowCancellationDrainSeconds;
		ActiveEntry = Entry;
		const TWeakPtr<FUserFlowPreflightCleanupEntry> WeakEntry = Entry;
		Entry->EnginePreExitHandle = FCoreDelegates::OnEnginePreExit.AddLambda([WeakEntry]()
		{
			if (const TSharedPtr<FUserFlowPreflightCleanupEntry> EntryOnExit = WeakEntry.Pin())
			{
				ReleaseUserFlowPreflightCleanup(EntryOnExit);
			}
		});
		Entry->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakEntry](float)
		{
			const TSharedPtr<FUserFlowPreflightCleanupEntry> EntryOnTick = WeakEntry.Pin();
			if (!EntryOnTick.IsValid())
			{
				return false;
			}

			UAudioExporterRuntimeSubsystem* Subsystem = EntryOnTick->State->RuntimeSubsystem.Get();
			const bool bStructuredTerminal = EntryOnTick->State->StructuredTerminalEvents > 0;
			const bool bDrained = Subsystem
				&& !Subsystem->IsExporting()
				&& Subsystem->GetActiveAsyncExportsForTesting() == 0;
			if ((bStructuredTerminal && bDrained) || FPlatformTime::Seconds() >= EntryOnTick->HardDeadlineSeconds)
			{
				ReleaseUserFlowPreflightCleanup(EntryOnTick);
				return false;
			}
			return true;
		}));
	}

	class FWaitForUserFlowPreflightDrain final : public IAutomationLatentCommand
	{
	public:
		FWaitForUserFlowPreflightDrain(
			FAutomationTestBase& InTest,
			UAudioExporterRuntimeSubsystem* InRuntimeSubsystem,
			const TCHAR* InTestName,
			const TSharedPtr<bool>& InSucceededFlag = nullptr)
			: State(MakeShared<FUserFlowPreflightDrainState>())
		{
			State->Test = &InTest;
			State->RuntimeSubsystem = InRuntimeSubsystem;
			State->TestName = InTestName;
			State->SucceededFlag = InSucceededFlag;
			if (State->SucceededFlag.IsValid())
			{
				*State->SucceededFlag = false;
			}
		}

		virtual bool Update() override
		{
			UAudioExporterRuntimeSubsystem* Subsystem = State->RuntimeSubsystem.Get();
			if (!Subsystem)
			{
				State->Test->AddError(FString::Printf(TEXT("%s preflight drain lost the AudioExporter runtime subsystem."), *State->TestName));
				return true;
			}
			if (!State->bCancellationRequested
				&& !Subsystem->IsExporting()
				&& Subsystem->GetActiveAsyncExportsForTesting() == 0)
			{
				if (State->SucceededFlag.IsValid())
				{
					*State->SucceededFlag = true;
				}
				return true;
			}

			if (!State->bCancellationRequested)
			{
				State->bCancellationRequested = true;
				State->DeadlineSeconds = FPlatformTime::Seconds() + UserFlowCancellationDrainSeconds;
				if (Subsystem->IsExporting())
				{
					State->CompleteHandle = Subsystem->OnExportCompleteNative.AddLambda([State = State](const FString&)
					{
						++State->LegacyTerminalEvents;
					});
					State->ResultHandle = Subsystem->OnExportCompleteResultNative.AddLambda([State = State](const FAudioExportResult& InResult)
					{
						++State->StructuredTerminalEvents;
						State->TerminalResult = InResult;
					});
					Subsystem->CancelExport(false);
				}
			}

			const bool bTerminalDelivered = State->LegacyTerminalEvents > 0
				|| (State->StructuredTerminalEvents > 0
					&& (State->TerminalResult.Outcome == EAudioExportOutcome::Cancelled
						|| State->TerminalResult.Outcome == EAudioExportOutcome::Rejected));
			const int32 ActiveAsyncExports = Subsystem->GetActiveAsyncExportsForTesting();
			if (bTerminalDelivered && !Subsystem->IsExporting() && ActiveAsyncExports == 0)
			{
				if (State->CompleteHandle.IsValid())
				{
					Subsystem->OnExportCompleteNative.Remove(State->CompleteHandle);
					State->CompleteHandle.Reset();
				}
				if (State->ResultHandle.IsValid())
				{
					Subsystem->OnExportCompleteResultNative.Remove(State->ResultHandle);
					State->ResultHandle.Reset();
				}
				if (State->SucceededFlag.IsValid())
				{
					*State->SucceededFlag = true;
				}
				return true;
			}

			if (FPlatformTime::Seconds() > State->DeadlineSeconds)
			{
				State->Test->AddError(FString::Printf(TEXT("%s preflight cancellation did not deliver a terminal outcome and drain all async writers before the bounded deadline; bounded deferred cleanup is armed."), *State->TestName));
				RegisterBoundedUserFlowPreflightCleanup(State);
				return true;
			}

			return false;
		}

	private:
		TSharedRef<FUserFlowPreflightDrainState> State;
	};

	static void RemoveUserFlowDelegates(FUserFlowExportState& State)
	{
		if (UAudioExporterRuntimeSubsystem* Subsystem = State.RuntimeSubsystem.Get())
		{
			if (State.CompleteHandle.IsValid())
			{
				Subsystem->OnExportCompleteNative.Remove(State.CompleteHandle);
			}
			if (State.ResultHandle.IsValid())
			{
				Subsystem->OnExportCompleteResultNative.Remove(State.ResultHandle);
			}
			if (State.ProgressHandle.IsValid())
			{
				Subsystem->OnExportProgressNative.Remove(State.ProgressHandle);
			}
		}
		State.CompleteHandle.Reset();
		State.ResultHandle.Reset();
		State.ProgressHandle.Reset();
	}

	// Subscribes the state's terminal listeners and starts ONE real export through the public
	// subsystem entry. Every caller treats a false return as a hard test failure (the helper has
	// already AddError'd).
	static bool BeginUserFlowExport(
		FAutomationTestBase& Test,
		UAudioExporterRuntimeSubsystem* RuntimeSubsystem,
		const TSharedRef<FUserFlowExportState>& State,
		UWorld* World,
		const TArray<FAudioExportSoundEntry>& Entries,
		const FAudioExporterConfig& Config,
		const FString& OutputRoot)
	{
		State->RuntimeSubsystem = RuntimeSubsystem;
		State->OutputRoot = OutputRoot;
		State->StartSeconds = FPlatformTime::Seconds();

		// Terminal guards: subsystem delegates broadcast EVERY session's terminal to every active
		// listener, so a multi-session test whose earlier state stays subscribed while a later
		// session settles would double-count. Each surface counts at most its own FIRST event.
		State->CompleteHandle = RuntimeSubsystem->OnExportCompleteNative.AddLambda([State](const FString& InPath)
		{
			if (State->bFinished)
			{
				return;
			}
			++State->LegacyTerminalEvents;
			State->FinishedPath = InPath;
			State->bFinished = true;
		});
		State->ResultHandle = RuntimeSubsystem->OnExportCompleteResultNative.AddLambda([State](const FAudioExportResult& InResult)
		{
			if (State->bOutcomeReceived)
			{
				return;
			}
			++State->StructuredTerminalEvents;
			State->Result = InResult;
			State->Outcome = InResult.Outcome;
			State->bOutcomeReceived = true;
		});
		State->ProgressHandle = RuntimeSubsystem->OnExportProgressNative.AddLambda([State](const FAudioExportProgressInfo& InInfo)
		{
			++State->ProgressEvents;
			State->MaxVariationSeen = FMath::Max(State->MaxVariationSeen, InInfo.CurrentVariation);
		});

		const bool bStarted = RuntimeSubsystem->StartExport(World, Entries, Config, OutputRoot);
		if (!Test.TestTrue(FString::Printf(TEXT("%s should start through the real AudioExporter runtime subsystem."), *State->TestName), bStarted))
		{
			RemoveUserFlowDelegates(*State);
			return false;
		}
		return true;
	}

	static void RegisterUserFlowDeferredCleanup(
		TFunction<bool()>&& InIsSafe,
		TFunction<void()>&& InDetachOwnedDelegates,
		TFunction<void()>&& InCleanupResources,
		UAudioExporterRuntimeSubsystem* InRuntimeSubsystem);

	class FWaitForUserFlowExportTerminal : public IAutomationLatentCommand
	{
	public:
		FWaitForUserFlowExportTerminal(const TSharedRef<FUserFlowExportState>& InState, const double InTimeoutSeconds)
			: State(InState)
			, TimeoutSeconds(InTimeoutSeconds)
		{
		}

		virtual bool Update() override
		{
			if (State->bStartRejected)
			{
				return true;
			}
			UAudioExporterRuntimeSubsystem* Subsystem = State->RuntimeSubsystem.Get();
			const bool bExportSettled = Subsystem && !Subsystem->IsExporting();
			const int32 ActiveAsyncExports = Subsystem
				? Subsystem->GetActiveAsyncExportsForTesting()
				: -1;
			if (State->bFinished
				&& State->bOutcomeReceived
				&& bExportSettled
				&& ActiveAsyncExports == 0)
			{
				State->bWriterDrainConfirmed = true;
				return true;
			}

			const double NowSeconds = FPlatformTime::Seconds();
			if (!State->bCancellationRequested && (NowSeconds - State->StartSeconds) > TimeoutSeconds)
			{
				if (State->Test)
				{
					State->Test->AddError(FString::Printf(TEXT("%s timed out after %.0f s waiting for a terminal result; requesting cancellation and waiting for writer shutdown "
						"(legacy terminals=%d, structured terminals=%d, max variation seen=%d)."),
						*State->TestName, TimeoutSeconds, State->LegacyTerminalEvents, State->StructuredTerminalEvents, State->MaxVariationSeen));
				}
				State->bCancellationRequested = true;
				State->CancellationDeadlineSeconds = NowSeconds + UserFlowCancellationDrainSeconds;
				if (Subsystem)
				{
					if (Subsystem->IsExporting())
					{
						Subsystem->CancelExport(false);
					}
				}
				return false;
			}

			if (State->bCancellationRequested && NowSeconds > State->CancellationDeadlineSeconds)
			{
				State->bWaitTimedOut = true;
				if (State->Test)
				{
					State->Test->AddError(FString::Printf(TEXT("%s did not settle its terminal result and writer within the bounded cancellation drain deadline."), *State->TestName));
				}
				return true;
			}

			return false;
		}

	private:
		TSharedRef<FUserFlowExportState> State;
		double TimeoutSeconds = 45.0;
	};

	// Verifies that the writer is idle before unsubscribing delegates, then runs the caller's verifier.
	class FRunAfterUserFlowExport : public IAutomationLatentCommand
	{
	public:
		using FVerifier = TFunction<void(FAutomationTestBase&, FUserFlowExportState&)>;
		using FTimeoutCleanup = TFunction<void()>;

		FRunAfterUserFlowExport(
			const TSharedRef<FUserFlowExportState>& InState,
			FVerifier InVerifier,
			FTimeoutCleanup InTimeoutCleanup = nullptr)
			: State(InState)
			, Verifier(MoveTemp(InVerifier))
			, TimeoutCleanup(MoveTemp(InTimeoutCleanup))
		{
		}

		virtual bool Update() override
		{
			if (State->bWaitTimedOut)
			{
				UAudioExporterRuntimeSubsystem* Subsystem = State->RuntimeSubsystem.Get();
				const int32 ActiveAsyncExports = Subsystem
					? Subsystem->GetActiveAsyncExportsForTesting()
					: -1;
				const bool bSafeAfterTimeout = !Subsystem
					|| (!Subsystem->IsExporting() && ActiveAsyncExports == 0);
				if (!bSafeAfterTimeout)
				{
					if (!bTimedOutDrainStarted)
					{
						bTimedOutDrainStarted = true;
						TimedOutDrainDeadlineSeconds = FPlatformTime::Seconds() + UserFlowCancellationDrainSeconds;
					}
					if (FPlatformTime::Seconds() < TimedOutDrainDeadlineSeconds)
					{
						return false;
					}
					if (!bTimedOutCleanupQueued)
					{
						bTimedOutCleanupQueued = true;
						const TSharedRef<FUserFlowExportState> DeferredState = State;
						const FTimeoutCleanup DeferredTimeoutCleanup = TimeoutCleanup;
						const FVerifier DeferredVerifier = Verifier;
						RegisterUserFlowDeferredCleanup(
							[DeferredState]()
							{
								UAudioExporterRuntimeSubsystem* Subsystem = DeferredState->RuntimeSubsystem.Get();
								return !Subsystem || (!Subsystem->IsExporting()
									&& Subsystem->GetActiveAsyncExportsForTesting() == 0);
							},
							[DeferredState]()
							{
								RemoveUserFlowDelegates(*DeferredState);
							},
							[DeferredState, DeferredTimeoutCleanup, DeferredVerifier]()
							{
								if (DeferredTimeoutCleanup)
								{
									DeferredTimeoutCleanup();
								}
								if (DeferredState->Test && DeferredVerifier)
								{
									DeferredVerifier(*DeferredState->Test, *DeferredState);
								}
							},
							DeferredState->RuntimeSubsystem.Get());
					}
					return true;
				}
				if (TimeoutCleanup)
				{
					TimeoutCleanup();
				}
				RemoveUserFlowDelegates(*State);
				if (State->Test && Verifier)
				{
					Verifier(*State->Test, *State);
				}
				return true;
			}
			if (State->bStartRejected)
			{
				RemoveUserFlowDelegates(*State);
				if (State->bRunVerifierWhenRejected && State->Test && Verifier)
				{
					Verifier(*State->Test, *State);
				}
				return true;
			}
			if (!State->bWriterDrainConfirmed)
			{
				if (State->Test)
				{
					State->Test->AddError(FString::Printf(TEXT("%s verification was reached without confirmed terminal delivery and writer inactivity."), *State->TestName));
				}
				return true;
			}
			if (UAudioExporterRuntimeSubsystem* Subsystem = State->RuntimeSubsystem.Get(); Subsystem && Subsystem->IsExporting())
			{
				if (State->Test)
				{
					State->Test->AddError(FString::Printf(TEXT("%s verification was reached before terminal writer shutdown."), *State->TestName));
				}
				return true;
			}
			UAudioExporterRuntimeSubsystem* Subsystem = State->RuntimeSubsystem.Get();
			const int32 ActiveAsyncExports = Subsystem
				? Subsystem->GetActiveAsyncExportsForTesting()
				: -1;
			const bool bZeroAsyncExports = State->Test
				&& State->Test->TestEqual(FString::Printf(TEXT("%s must confirm zero active async exports before cleanup."), *State->TestName), ActiveAsyncExports, 0);
			if (!Subsystem || !bZeroAsyncExports)
			{
				if (State->Test)
				{
					State->Test->AddError(FString::Printf(TEXT("%s cleanup was blocked because the realtime writer drain was not confirmed."), *State->TestName));
				}
				return true;
			}
			RemoveUserFlowDelegates(*State);
			if (State->Test && Verifier)
			{
				Verifier(*State->Test, *State);
			}
			return true;
		}

	private:
		TSharedRef<FUserFlowExportState> State;
		FVerifier Verifier;
		FTimeoutCleanup TimeoutCleanup;
		bool bTimedOutDrainStarted = false;
		bool bTimedOutCleanupQueued = false;
		double TimedOutDrainDeadlineSeconds = 0.0;
	};

	// Starts an export from INSIDE the latent chain (needed whenever a second session may only
	// begin after the previous session reached its terminal).
	class FStartUserFlowExportCommand : public IAutomationLatentCommand
	{
	public:
		using FAfterStart = TFunction<void()>;

		FStartUserFlowExportCommand(
			const TSharedRef<FUserFlowExportState>& InState,
			UAudioExporterRuntimeSubsystem* InRuntimeSubsystem,
			UWorld* InWorld,
			const TArray<FAudioExportSoundEntry>& InEntries,
			const FAudioExporterConfig& InConfig,
			const FString& InOutputRoot,
			const bool bInArmAttendedBranch,
			const TSharedPtr<FUserFlowExportState>& InPrerequisiteState = nullptr,
			const TSharedPtr<bool>& InPreflightSucceeded = nullptr,
			FAfterStart InAfterStart = nullptr)
			: State(InState)
			, RuntimeSubsystem(InRuntimeSubsystem)
			, World(InWorld)
			, Entries(InEntries)
			, Config(InConfig)
			, OutputRoot(InOutputRoot)
			, bArmAttendedBranch(bInArmAttendedBranch)
			, PrerequisiteState(InPrerequisiteState)
			, PreflightSucceeded(InPreflightSucceeded)
			, AfterStart(MoveTemp(InAfterStart))
		{
		}

		virtual bool Update() override
		{
			const bool bPreflightRejected = PreflightSucceeded.IsValid() && !*PreflightSucceeded;
			const bool bPrerequisiteRejected = PrerequisiteState.IsValid()
				&& (!PrerequisiteState->bWriterDrainConfirmed
					|| PrerequisiteState->bWaitTimedOut
					|| !PrerequisiteState->bOutcomeReceived
					|| PrerequisiteState->Outcome != EAudioExportOutcome::Succeeded);
			if (bPreflightRejected || bPrerequisiteRejected)
			{
				if (State->Test)
				{
					State->Test->AddError(FString::Printf(TEXT("%s was not started because its preflight or preceding session did not settle successfully."), *State->TestName));
				}
				State->bStartRejected = true;
				State->bFinished = true;
				State->bOutcomeReceived = true;
				State->Outcome = EAudioExportOutcome::Rejected;
				State->Result.Outcome = EAudioExportOutcome::Rejected;
				State->bWriterDrainConfirmed = false;
				State->RuntimeSubsystem = nullptr;
				return true;
			}
			if (bArmAttendedBranch)
			{
				FAudioRendererRealtime::SetForceAttendedSeparatorBranchForTesting(true);
			}

			UAudioExporterRuntimeSubsystem* Subsystem = RuntimeSubsystem.Get();
			if (!State->Test || !Subsystem || !World.IsValid())
			{
				if (State->Test)
				{
					State->Test->AddError(FString::Printf(TEXT("%s could not start its queued export: subsystem or world unavailable."), *State->TestName));
				}
				return true;
			}

			if (BeginUserFlowExport(*State->Test, Subsystem, State, World.Get(), Entries, Config, OutputRoot)
				&& AfterStart)
			{
				AfterStart();
			}
			else if (!State->CompleteHandle.IsValid() && !State->ResultHandle.IsValid())
			{
				State->bStartRejected = true;
				State->bFinished = true;
				State->bOutcomeReceived = true;
				State->Outcome = EAudioExportOutcome::Rejected;
				State->Result.Outcome = EAudioExportOutcome::Rejected;
				State->bWriterDrainConfirmed = false;
				State->RuntimeSubsystem = nullptr;
			}
			return true;
		}

	private:
		TSharedRef<FUserFlowExportState> State;
		TWeakObjectPtr<UAudioExporterRuntimeSubsystem> RuntimeSubsystem;
		TWeakObjectPtr<UWorld> World;
		TArray<FAudioExportSoundEntry> Entries;
		FAudioExporterConfig Config;
		FString OutputRoot;
		bool bArmAttendedBranch = false;
		TSharedPtr<FUserFlowExportState> PrerequisiteState;
		TSharedPtr<bool> PreflightSucceeded;
		FAfterStart AfterStart;
	};

	struct FUserFlowDeferredCleanupEntry final
	{
		TFunction<bool()> IsSafe;
		TFunction<void()> DetachOwnedDelegates;
		TFunction<void()> CleanupResources;
		TWeakObjectPtr<UAudioExporterRuntimeSubsystem> RuntimeSubsystem;
		FDelegateHandle CompleteHandle;
		FDelegateHandle ResultHandle;
		FDelegateHandle EnginePreExitHandle;
		FTSTicker::FDelegateHandle TickerHandle;
		double DeadlineSeconds = 0.0;
		bool bReleased = false;
	};

	static TArray<TSharedPtr<FUserFlowDeferredCleanupEntry>>& GetUserFlowDeferredCleanupRegistry()
	{
		static TArray<TSharedPtr<FUserFlowDeferredCleanupEntry>> Entries;
		return Entries;
	}

	static FTSTicker::FDelegateHandle& GetUserFlowDeferredCleanupRegistryTickerHandle()
	{
		static FTSTicker::FDelegateHandle Handle;
		return Handle;
	}

	static void ReleaseUserFlowDeferredCleanup(
		const TSharedPtr<FUserFlowDeferredCleanupEntry>& Entry,
		const bool bCleanupIfSafe);

	static void EnsureUserFlowDeferredCleanupRegistryTicker()
	{
		FTSTicker::FDelegateHandle& TickerHandle = GetUserFlowDeferredCleanupRegistryTickerHandle();
		if (TickerHandle.IsValid())
		{
			return;
		}

		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
		{
			TArray<TSharedPtr<FUserFlowDeferredCleanupEntry>>& Entries = GetUserFlowDeferredCleanupRegistry();
			for (int32 Index = Entries.Num() - 1; Index >= 0; --Index)
			{
				const TSharedPtr<FUserFlowDeferredCleanupEntry> Entry = Entries[Index];
				if (!Entry.IsValid() || Entry->bReleased)
				{
					Entries.RemoveAt(Index);
					continue;
				}

				if (Entry->IsSafe && Entry->IsSafe())
				{
					ReleaseUserFlowDeferredCleanup(Entry, /*bCleanupIfSafe=*/true);
				}
			}

			if (Entries.IsEmpty())
			{
				GetUserFlowDeferredCleanupRegistryTickerHandle().Reset();
				return false;
			}
			return true;
		}));
	}

	static void ReleaseUserFlowDeferredCleanup(
		const TSharedPtr<FUserFlowDeferredCleanupEntry>& Entry,
		const bool bCleanupIfSafe)
	{
		if (!Entry.IsValid() || Entry->bReleased)
		{
			return;
		}

		const bool bSafe = Entry->IsSafe && Entry->IsSafe();
		Entry->bReleased = true;
		if (Entry->TickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(Entry->TickerHandle);
			Entry->TickerHandle.Reset();
		}
		if (Entry->RuntimeSubsystem.IsValid())
		{
			if (Entry->CompleteHandle.IsValid())
			{
				Entry->RuntimeSubsystem->OnExportCompleteNative.Remove(Entry->CompleteHandle);
			}
			if (Entry->ResultHandle.IsValid())
			{
				Entry->RuntimeSubsystem->OnExportCompleteResultNative.Remove(Entry->ResultHandle);
			}
		}
		Entry->CompleteHandle.Reset();
		Entry->ResultHandle.Reset();
		if (Entry->EnginePreExitHandle.IsValid())
		{
			FCoreDelegates::OnEnginePreExit.Remove(Entry->EnginePreExitHandle);
			Entry->EnginePreExitHandle.Reset();
		}
		if (Entry->DetachOwnedDelegates)
		{
			Entry->DetachOwnedDelegates();
		}
		if (bCleanupIfSafe && bSafe && Entry->CleanupResources)
		{
			Entry->CleanupResources();
		}
		GetUserFlowDeferredCleanupRegistry().RemoveSingle(Entry);
	}

	static void TryReleaseUserFlowDeferredCleanup(const TSharedPtr<FUserFlowDeferredCleanupEntry>& Entry)
	{
		if (Entry.IsValid() && Entry->IsSafe && Entry->IsSafe())
		{
			ReleaseUserFlowDeferredCleanup(Entry, /*bCleanupIfSafe=*/true);
		}
	}

	static void RegisterUserFlowDeferredCleanup(
		TFunction<bool()>&& InIsSafe,
		TFunction<void()>&& InDetachOwnedDelegates,
		TFunction<void()>&& InCleanupResources,
		UAudioExporterRuntimeSubsystem* InRuntimeSubsystem)
	{
		const TSharedPtr<FUserFlowDeferredCleanupEntry> Entry = MakeShared<FUserFlowDeferredCleanupEntry>();
		Entry->IsSafe = MoveTemp(InIsSafe);
		Entry->DetachOwnedDelegates = MoveTemp(InDetachOwnedDelegates);
		Entry->CleanupResources = MoveTemp(InCleanupResources);
		Entry->RuntimeSubsystem = InRuntimeSubsystem;
		Entry->DeadlineSeconds = FPlatformTime::Seconds() + UserFlowCancellationDrainSeconds;

		const TWeakPtr<FUserFlowDeferredCleanupEntry> WeakEntry = Entry;
		Entry->EnginePreExitHandle = FCoreDelegates::OnEnginePreExit.AddLambda([WeakEntry]()
		{
			if (const TSharedPtr<FUserFlowDeferredCleanupEntry> EntryOnExit = WeakEntry.Pin())
			{
				ReleaseUserFlowDeferredCleanup(EntryOnExit, /*bCleanupIfSafe=*/true);
			}
		});
		if (InRuntimeSubsystem)
		{
			Entry->CompleteHandle = InRuntimeSubsystem->OnExportCompleteNative.AddLambda([WeakEntry](const FString&)
			{
				if (const TSharedPtr<FUserFlowDeferredCleanupEntry> EntryOnComplete = WeakEntry.Pin())
				{
					TryReleaseUserFlowDeferredCleanup(EntryOnComplete);
				}
			});
			Entry->ResultHandle = InRuntimeSubsystem->OnExportCompleteResultNative.AddLambda([WeakEntry](const FAudioExportResult&)
			{
				if (const TSharedPtr<FUserFlowDeferredCleanupEntry> EntryOnResult = WeakEntry.Pin())
				{
					TryReleaseUserFlowDeferredCleanup(EntryOnResult);
				}
			});
		}
		Entry->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Entry, WeakEntry](float)
		{
			const TSharedPtr<FUserFlowDeferredCleanupEntry> EntryOnTick = WeakEntry.Pin();
			if (!EntryOnTick.IsValid())
			{
				return false;
			}
			if (EntryOnTick->IsSafe && EntryOnTick->IsSafe())
			{
				ReleaseUserFlowDeferredCleanup(EntryOnTick, /*bCleanupIfSafe=*/true);
				return false;
			}
			if (FPlatformTime::Seconds() >= EntryOnTick->DeadlineSeconds)
			{
				// The bounded ticker hands still-live ownership to the registry. Its active
				// registry ticker retries until the terminal callbacks have also drained writers.
				TArray<TSharedPtr<FUserFlowDeferredCleanupEntry>>& Registry = GetUserFlowDeferredCleanupRegistry();
				if (!Registry.Contains(EntryOnTick))
				{
					Registry.Add(EntryOnTick);
				}
				EnsureUserFlowDeferredCleanupRegistryTicker();
				EntryOnTick->TickerHandle.Reset();
				return false;
			}
			return true;
		}));
	}

	static void DeleteSessionDirectoryIfOurs(const FString& InPath);

	// Full cleanup of the GUID-named /Game temp packages a test created: the duplicated packages
	// were never saved, so unloading them removes every trace on both success and failure paths.
	class FUnloadTempPackagesCommand : public IAutomationLatentCommand
	{
	public:
		explicit FUnloadTempPackagesCommand(const TArray<TWeakObjectPtr<UPackage>>& InPackages)
			: Packages(InPackages)
		{
		}

		FUnloadTempPackagesCommand(const TArray<TWeakObjectPtr<UPackage>>& InPackages, const TSharedRef<FUserFlowExportState>& InState)
			: Packages(InPackages)
			, State(InState)
		{
		}

		virtual bool Update() override
		{
			if (State.IsValid() && (!State->bWriterDrainConfirmed || State->bWaitTimedOut))
			{
				if (State->Test)
				{
					State->Test->AddError(FString::Printf(TEXT("%s temporary package cleanup was reached without confirmed writer inactivity."), *State->TestName));
				}
				const TArray<TWeakObjectPtr<UPackage>> DeferredPackages = Packages;
				const TSharedPtr<FUserFlowExportState> DeferredState = State;
				RegisterUserFlowDeferredCleanup(
					[DeferredState]()
					{
						UAudioExporterRuntimeSubsystem* Subsystem = DeferredState->RuntimeSubsystem.Get();
						return !Subsystem || (DeferredState->bFinished && DeferredState->bOutcomeReceived
							&& !Subsystem->IsExporting() && Subsystem->GetActiveAsyncExportsForTesting() == 0);
					},
					[DeferredState]()
					{
						RemoveUserFlowDelegates(*DeferredState);
					},
					[DeferredPackages]()
					{
						TArray<UPackage*> LivePackages;
						for (const TWeakObjectPtr<UPackage>& WeakPackage : DeferredPackages)
						{
							if (UPackage* Package = WeakPackage.Get())
							{
								Package->SetDirtyFlag(false);
								LivePackages.Add(Package);
							}
						}
						if (!LivePackages.IsEmpty())
						{
							UPackageTools::UnloadPackages(LivePackages);
						}
					},
					DeferredState->RuntimeSubsystem.Get());
				return true;
			}
			if (State.IsValid())
			{
				UAudioExporterRuntimeSubsystem* Subsystem = State->RuntimeSubsystem.Get();
				const int32 ActiveAsyncExports = Subsystem
					? Subsystem->GetActiveAsyncExportsForTesting()
					: -1;
				if (!Subsystem || Subsystem->IsExporting() || ActiveAsyncExports != 0)
				{
					if (State->Test)
					{
						State->Test->AddError(FString::Printf(TEXT("%s temporary package cleanup was blocked until the writer drain reached zero active async exports."), *State->TestName));
					}
					return true;
				}
			}
			TArray<UPackage*> LivePackages;
			for (const TWeakObjectPtr<UPackage>& WeakPackage : Packages)
			{
				if (UPackage* Package = WeakPackage.Get())
				{
					Package->SetDirtyFlag(false);
					LivePackages.Add(Package);
				}
			}
			if (!LivePackages.IsEmpty())
			{
				UPackageTools::UnloadPackages(LivePackages);
			}
			return true;
		}

	private:
		TArray<TWeakObjectPtr<UPackage>> Packages;
		TSharedPtr<FUserFlowExportState> State;
	};

	// Terminal cleanup for one output root created through MakeOutputDirectory under
	// Saved/Automation/UserFlows/AudioExporter: queued LAST, a latent command runs whether the
	// earlier verifiers went red or green, so the tree is deleted on BOTH success and failure
	// paths (timeout/skip exits included -- those waits complete their latent chain too).
	// Defensive guard mirrors DeleteSessionDirectoryIfOurs: only ever a folder under this file's
	// own automation subtree is eligible, so a stray path can never widen the blast radius.
	// A writer that is still active is an explicit failure, and the directory is left in place so
	// the failing run cannot race a live file handle.
	class FDeleteUserFlowOutputDirectoryCommand : public IAutomationLatentCommand
	{
	public:
		explicit FDeleteUserFlowOutputDirectoryCommand(const FString& InOutputDirectory)
			: OutputDirectory(InOutputDirectory)
		{
		}

		FDeleteUserFlowOutputDirectoryCommand(const FString& InOutputDirectory, const TSharedRef<FUserFlowExportState>& InState)
			: OutputDirectory(InOutputDirectory)
			, State(InState)
		{
		}

		virtual bool Update() override
		{
			if (State.IsValid())
			{
				if (!State->bWriterDrainConfirmed || State->bWaitTimedOut)
				{
					if (State->Test)
					{
						State->Test->AddError(FString::Printf(TEXT("%s output cleanup was reached without confirmed terminal delivery and writer inactivity."), *State->TestName));
					}
					const FString DeferredOutputDirectory = OutputDirectory;
					const TSharedPtr<FUserFlowExportState> DeferredState = State;
					RegisterUserFlowDeferredCleanup(
						[DeferredState]()
						{
							UAudioExporterRuntimeSubsystem* Subsystem = DeferredState->RuntimeSubsystem.Get();
							return !Subsystem || (DeferredState->bFinished && DeferredState->bOutcomeReceived
								&& !Subsystem->IsExporting() && Subsystem->GetActiveAsyncExportsForTesting() == 0);
						},
						[DeferredState]()
						{
							RemoveUserFlowDelegates(*DeferredState);
						},
						[DeferredOutputDirectory]()
						{
							FString NormalizedDeferred = DeferredOutputDirectory;
							FPaths::NormalizeDirectoryName(NormalizedDeferred);
							if (!NormalizedDeferred.IsEmpty()
								&& NormalizedDeferred.Contains(TEXT("/Automation/UserFlows/AudioExporter/"), ESearchCase::IgnoreCase))
							{
								IFileManager::Get().DeleteDirectory(*NormalizedDeferred, /*bRequireExistenceCheck=*/false, /*bTreeDelete=*/true);
							}
						},
						DeferredState->RuntimeSubsystem.Get());
					return true;
				}
				UAudioExporterRuntimeSubsystem* Subsystem = State->RuntimeSubsystem.Get();
				if (Subsystem && Subsystem->IsExporting())
				{
					if (State->Test)
					{
						State->Test->AddError(FString::Printf(TEXT("%s output cleanup was reached before terminal writer shutdown."), *State->TestName));
					}
					return true;
				}
				const int32 ActiveAsyncExports = Subsystem
					? Subsystem->GetActiveAsyncExportsForTesting()
					: -1;
				if (!Subsystem || ActiveAsyncExports != 0)
				{
					if (State->Test)
					{
						State->Test->AddError(FString::Printf(TEXT("%s output cleanup was blocked until the writer drain reached zero active async exports."), *State->TestName));
					}
					return true;
				}
			}

			FString Normalized = OutputDirectory;
			FPaths::NormalizeDirectoryName(Normalized);
			if (!Normalized.IsEmpty()
				&& Normalized.Contains(TEXT("/Automation/UserFlows/AudioExporter/"), ESearchCase::IgnoreCase))
			{
				IFileManager::Get().DeleteDirectory(*Normalized, /*bRequireExistenceCheck=*/false, /*bTreeDelete=*/true);
			}
			return true;
		}

	private:
		FString OutputDirectory;
		TSharedPtr<FUserFlowExportState> State;
	};

	// ---- Report parsing helpers (test 7) ------------------------------------------------------

	// Summary lines render as "<label padded to 28 chars>: N". A section header carries the bare
	// label without a colon, so requiring ':' plus exact trimmed-label equality cannot confuse rows
	// with headers. Returns false unless exactly one such summary line exists.
	static bool ReadReportSummaryCount(const TArray<FString>& Lines, const FString& Label, int32& OutCount)
	{
		OutCount = -1;
		int32 Matches = 0;
		for (const FString& Line : Lines)
		{
			const int32 ColonIndex = Line.Find(TEXT(":"), ESearchCase::CaseSensitive);
			if (ColonIndex == INDEX_NONE)
			{
				continue;
			}
			if (Line.Left(ColonIndex).TrimStartAndEnd() != Label)
			{
				continue;
			}
			OutCount = FCString::Atoi(*Line.Mid(ColonIndex + 1).TrimStartAndEnd());
			++Matches;
		}
		return Matches == 1 && OutCount >= 0;
	}

	// Collects every succeeded row's "Output File Path" value from the text report.
	static void CollectReportOutputRows(const FString& ReportText, TArray<FString>& OutPaths)
	{
		OutPaths.Reset();
		TArray<FString> Lines;
		ReportText.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			const int32 ColonIndex = Line.Find(TEXT(":"), ESearchCase::CaseSensitive);
			if (ColonIndex == INDEX_NONE)
			{
				continue;
			}
			if (Line.Left(ColonIndex).TrimStartAndEnd() == TEXT("Output File Path"))
			{
				OutPaths.Add(Line.Mid(ColonIndex + 1).TrimStartAndEnd());
			}
		}
	}

	// True iff Leaf is exactly YYYY-MM-DD_HH-MM-SS (optionally followed by a "_N" collision suffix).
	static bool IsTimestampSessionLeaf(const FString& Leaf)
	{
		if (Leaf.Len() < 19)
		{
			return false;
		}

		static const FRegexPattern TimestampPattern(TEXT("[0-9]{4}-[0-9]{2}-[0-9]{2}_[0-9]{2}-[0-9]{2}-[0-9]{2}"));
		FRegexMatcher Matcher(TimestampPattern, Leaf);
		Matcher.SetLimits(0, 19);
		if (!(Matcher.FindNext() && Matcher.GetMatchBeginning() == 0 && Matcher.GetMatchEnding() == 19))
		{
			return false;
		}
		return Leaf.Len() == 19 || Leaf[19] == TEXT('_');
	}

	// Normalizes a session path and answers whether its LEAF folder name IS exactly one timestamp
	// expansion; also counts how many times that stamp string occurs inside the whole path.
	static bool LeafIsExactlyOneTimestamp(const FString& InPath, FString& OutLeaf, int32& OutOccurrencesInPath)
	{
		FString Normalized = FPaths::ConvertRelativePathToFull(InPath);
		FPaths::NormalizeDirectoryName(Normalized);
		OutLeaf = FPaths::GetPathLeaf(Normalized);

		static const FRegexPattern TimestampPattern(TEXT("[0-9]{4}-[0-9]{2}-[0-9]{2}_[0-9]{2}-[0-9]{2}-[0-9]{2}"));
		FRegexMatcher Matcher(TimestampPattern, OutLeaf);
		Matcher.SetLimits(0, OutLeaf.Len());
		const bool bLeafIsExactlyTimestamp = Matcher.FindNext()
			&& Matcher.GetMatchBeginning() == 0
			&& Matcher.GetMatchEnding() == OutLeaf.Len();

		OutOccurrencesInPath = 0;
		int32 SearchFrom = 0;
		while (!OutLeaf.IsEmpty())
		{
			const int32 FoundAt = Normalized.Find(OutLeaf, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
			if (FoundAt == INDEX_NONE)
			{
				break;
			}
			++OutOccurrencesInPath;
			SearchFrom = FoundAt + OutLeaf.Len();
		}
		return bLeafIsExactlyTimestamp;
	}

	static void DeleteSessionDirectoryIfOurs(const FString& InPath)
	{
		if (InPath.IsEmpty())
		{
			return;
		}
		FString Normalized = FPaths::ConvertRelativePathToFull(InPath);
		FPaths::NormalizeDirectoryName(Normalized);
		if (!IFileManager::Get().DirectoryExists(*Normalized))
		{
			return;
		}
		// Defensive: only ever delete a folder whose leaf looks like a timestamped session dir.
		if (!IsTimestampSessionLeaf(FPaths::GetPathLeaf(Normalized)))
		{
			return;
		}
		IFileManager::Get().DeleteDirectory(*Normalized, false, true);
	}

	// Terminal cleanup for DEFAULT-destination flows: reads each settled state's FinishedPath at
	// EXECUTION time and runs DeleteSessionDirectoryIfOurs on it (timestamped-leaf guarded).
	// Queued after the verify commands it still lands after every assert, but unlike an inline
	// cleanup inside a verify lambda it cannot be skipped by an early return -- so a timed-out,
	// red, or partially-failed run leaves its default-location session dirs behind just as little
	// as a green one.
	class FDeleteUserFlowDefaultSessionDirectoriesCommand : public IAutomationLatentCommand
	{
	public:
		explicit FDeleteUserFlowDefaultSessionDirectoriesCommand(TArray<TSharedRef<FUserFlowExportState>> InStates)
			: States(MoveTemp(InStates))
		{
		}

		virtual bool Update() override
		{
			bool bAllStatesSafe = true;
			for (int32 StateIndex = 0; StateIndex < States.Num(); ++StateIndex)
			{
				const TSharedRef<FUserFlowExportState>& State = States[StateIndex];
				UAudioExporterRuntimeSubsystem* Subsystem = State->RuntimeSubsystem.Get();
				const bool bSafeToRelease = State->bFinished
					&& State->bOutcomeReceived
					&& (!Subsystem || (!Subsystem->IsExporting() && Subsystem->GetActiveAsyncExportsForTesting() == 0));
				if (bSafeToRelease)
				{
					continue;
				}
				else
				{
					bAllStatesSafe = false;
				}
			}
			if (bAllStatesSafe)
			{
				for (const TSharedRef<FUserFlowExportState>& State : States)
				{
					RemoveUserFlowDelegates(*State);
					DeleteSessionDirectoryIfOurs(State->FinishedPath);
				}
			}
			else
			{
				const TArray<TSharedRef<FUserFlowExportState>> DeferredStates = States;
				RegisterUserFlowDeferredCleanup(
					[DeferredStates]()
					{
						for (const TSharedRef<FUserFlowExportState>& State : DeferredStates)
						{
							UAudioExporterRuntimeSubsystem* Subsystem = State->RuntimeSubsystem.Get();
							if (!State->bFinished || !State->bOutcomeReceived
								|| (Subsystem && (Subsystem->IsExporting() || Subsystem->GetActiveAsyncExportsForTesting() != 0)))
							{
								return false;
							}
						}
						return true;
					},
					[DeferredStates]()
					{
						for (const TSharedRef<FUserFlowExportState>& State : DeferredStates)
						{
							RemoveUserFlowDelegates(*State);
						}
					},
					[DeferredStates]()
					{
						for (const TSharedRef<FUserFlowExportState>& State : DeferredStates)
						{
							DeleteSessionDirectoryIfOurs(State->FinishedPath);
						}
					},
					DeferredStates.Num() > 0 ? DeferredStates[0]->RuntimeSubsystem.Get() : nullptr);
			}
			return true;
		}

	private:
		TArray<TSharedRef<FUserFlowExportState>> States;
	};
}

// ---------------------------------------------------------------------------
// USER FLOW 1: corpus inventory + load health.
//
// KILLS: any change that shrinks /Game/QA_MS_Corpus below its floor of at least 100 real user
// assets (currently 113 on this machine) via deletion or bad migration, breaks MetaSound loading
// of those REAL user assets, or breaks the
// UMetaSoundSource -> USoundWaveProcedural class parentage (e.g. a MetaSound base-class refactor).
// A discovery count below 100 fails loudly with the actual number found.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterUserFlowCorpusInventoryTest,
	"AudioFactory.AudioExporter.UserFlows.CorpusInventoryAndLoadHealth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterUserFlowCorpusInventoryTest::RunTest(const FString& Parameters)
{
	IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
	TestNotNull(TEXT("The asset registry should be available for corpus discovery."), AssetRegistry);
	if (!AssetRegistry)
	{
		return false;
	}

	AssetRegistry->ScanPathsSynchronous(TArray<FString>{ TEXT("/Game/QA_MS_Corpus") }, /*bForceRescan=*/false);

	TArray<FAssetData> CorpusAssets;
	TestTrue(TEXT("The QA_MS_Corpus discovery query should succeed."),
		AssetRegistry->GetAssetsByPath(FName(TEXT("/Game/QA_MS_Corpus")), CorpusAssets, /*bRecursive=*/true));

	if (!TestTrue(FString::Printf(TEXT("/Game/QA_MS_Corpus must contain at least 100 real user assets; discovery found %d."),
		CorpusAssets.Num()), CorpusAssets.Num() >= 100))
	{
		return false;
	}

	// Load-health sample: every 10th discovered asset must load as a UMetaSoundSource whose class
	// chain reaches USoundWaveProcedural (the base the exporter's procedural capture relies on).
	int32 SampledCount = 0;
	for (int32 AssetIndex = 0; AssetIndex < CorpusAssets.Num(); AssetIndex += 10)
	{
		const FAssetData& Asset = CorpusAssets[AssetIndex];
		const FString ObjectPath = Asset.PackageName.ToString() + TEXT(".") + Asset.AssetName.ToString();

		UMetaSoundSource* LoadedSource = LoadObject<UMetaSoundSource>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!TestNotNull(FString::Printf(TEXT("Sampled corpus asset '%s' should load as UMetaSoundSource."), *ObjectPath), LoadedSource))
		{
			return false;
		}

		TestTrue(FString::Printf(TEXT("Loaded corpus asset '%s' should be a UMetaSoundSource instance."), *ObjectPath),
			LoadedSource->IsA<UMetaSoundSource>());

		bool bProceduralParentage = false;
		for (const UClass* ClassIterator = LoadedSource->GetClass(); ClassIterator; ClassIterator = ClassIterator->GetSuperClass())
		{
			if (ClassIterator == USoundWaveProcedural::StaticClass())
			{
				bProceduralParentage = true;
				break;
			}
		}
		if (!TestTrue(FString::Printf(TEXT("The class hierarchy of '%s' should include USoundWaveProcedural parentage."), *ObjectPath),
			bProceduralParentage))
		{
			return false;
		}

		++SampledCount;
	}

	TestTrue(TEXT("The load-health sample should have exercised at least one corpus asset."), SampledCount > 0);
	return true;
}

// ---------------------------------------------------------------------------
// USER FLOW 2: batch export of 4 duplicated MetaSound one-shots.
//
// KILLS: any regression where a batch export reports more successes than it writes (success-count
// lie), drops variations silently (missing files), or writes empty/silent WAVs (broken capture or
// extraction). Every produced file is re-parsed from its raw disk bytes by this file's own RIFF
// parser; the physical census must equal the reported success count EXACTLY.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterUserFlowBatchDuplicatedMetaSoundsTest,
	"AudioFactory.AudioExporter.UserFlows.BatchDuplicatedMetaSoundOneShots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

namespace AudioExporterUserFlowTestUtils
{
	static void VerifyBatchDuplicatedMetaSounds(FAutomationTestBase& Test, AudioExporterUserFlowTestUtils::FUserFlowExportState& State)
	{
		Test.TestEqual(TEXT("The batch export should settle exactly one legacy completion event."),
			State.LegacyTerminalEvents, 1);
		Test.TestEqual(TEXT("The batch export should settle exactly one structured completion event."),
			State.StructuredTerminalEvents, 1);
		if (!Test.TestTrue(TEXT("The batch export should receive a structured terminal result."), State.bOutcomeReceived))
		{
			return;
		}

		Test.TestEqual(TEXT("A fully successful 4-file batch must settle as Succeeded."),
			State.Outcome, EAudioExportOutcome::Succeeded);
		if (!Test.TestEqual(TEXT("The reported success count must equal the literal requested batch size 4."),
			State.Result.SuccessfulCount, 4))
		{
			return;
		}
		Test.TestEqual(TEXT("A fully successful 4-file batch must report zero failed files."),
			State.Result.FailedCount, 0);
		Test.TestEqual(TEXT("A fully successful 4-file batch must report zero skipped files."),
			State.Result.SkippedCount, 0);
		Test.TestEqual(TEXT("The 4-file batch terminal result must conserve all literal requested variations across success, failure, and skip counts."),
			State.Result.SuccessfulCount + State.Result.FailedCount + State.Result.SkippedCount, 4);
		Test.TestFalse(TEXT("A successful batch must report a non-empty completion path."), State.FinishedPath.IsEmpty());

		TArray<FString> WavFiles;
		FindWavFilesUnder(State.FinishedPath.IsEmpty() ? State.OutputRoot : State.FinishedPath, WavFiles);
		if (!Test.TestEqual(FString::Printf(TEXT("Physical WAV census (%d) must match the reported success count (4) EXACTLY."),
			WavFiles.Num()), WavFiles.Num(), 4))
		{
			return;
		}

		for (int32 FileIndex = 0; FileIndex < WavFiles.Num(); ++FileIndex)
		{
			FUserFlowWavInfo Info;
			VerifyProducedWavHealth(Test, WavFiles[FileIndex],
				FString::Printf(TEXT("Batch duplicate #%d ('%s')"), FileIndex + 1, *FPaths::GetCleanFilename(WavFiles[FileIndex])), Info);
		}
	}
}

bool FAudioExporterUserFlowBatchDuplicatedMetaSoundsTest::RunTest(const FString& Parameters)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("NoSound")))
	{
		AddError(TEXT("User-flow batch export is unavailable: this required live-audio flow was launched with -NoSound."));
		return false;
	}

	USoundBase* SourceAsset = LoadObject<USoundBase>(nullptr, TEXT("/Game/MetaSoundOneShot.MetaSoundOneShot"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!TestNotNull(TEXT("Required fixture '/Game/MetaSoundOneShot.MetaSoundOneShot' should be present in this project."), SourceAsset))
	{
		return false;
	}

	UWorld* World = AudioExporterUserFlowTestUtils::ResolveAutomationWorld();
	if (!TestNotNull(TEXT("The batch user-flow test should have an editor world for the real runtime pipeline."), World))
	{
		return false;
	}

	UAudioExporterRuntimeSubsystem* RuntimeSubsystem = GEngine ? GEngine->GetEngineSubsystem<UAudioExporterRuntimeSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("The batch user-flow test should resolve the AudioExporter runtime subsystem."), RuntimeSubsystem))
	{
		return false;
	}
	const TSharedPtr<bool> PreflightSucceeded = MakeShared<bool>(false);
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FWaitForUserFlowPreflightDrain(
		*this, RuntimeSubsystem, TEXT("UserFlows.BatchDuplicatedMetaSoundOneShots"), PreflightSucceeded));

	// Four duplicates of the real one-shot in GUID-named temp packages (never saved; unloaded by a
	// cleanup latent command on success AND failure below).
	const FString GuidDigits = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TArray<TWeakObjectPtr<UPackage>> TempPackages;
	TArray<FAudioExportSoundEntry> Entries;
	for (int32 DuplicateIndex = 0; DuplicateIndex < 4; ++DuplicateIndex)
	{
		const FString TempPackageName = FString::Printf(TEXT("/Game/AudioExporterUserFlowTests/%s/Dup_%d"), *GuidDigits, DuplicateIndex);
		UPackage* TempPackage = CreatePackage(*TempPackageName);
		if (!TestNotNull(FString::Printf(TEXT("Temp package '%s' should be created."), *TempPackageName), TempPackage))
		{
			ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FUnloadTempPackagesCommand(TempPackages));
			return false;
		}
		TempPackages.Add(TempPackage);

		const FName DuplicateName = FName(*FString::Printf(TEXT("MetaSoundOneShot_UserFlow_%d"), DuplicateIndex));
		USoundBase* Duplicate = Cast<USoundBase>(StaticDuplicateObject(SourceAsset, TempPackage, DuplicateName));
		if (!TestNotNull(FString::Printf(TEXT("Duplicate '%s' should be created from the real MetaSoundOneShot."), *DuplicateName.ToString()), Duplicate))
		{
			ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FUnloadTempPackagesCommand(TempPackages));
			return false;
		}

		FAudioExportSoundEntry Entry;
		Entry.Sound = Duplicate;
		Entry.SoundName = DuplicateName.ToString();
		Entry.SourceAssetPath = TempPackageName;
		Entry.RelativePath = TEXT("BatchDups");
		Entries.Add(Entry);
	}

	const TSharedRef<AudioExporterUserFlowTestUtils::FUserFlowExportState> State =
		MakeShared<AudioExporterUserFlowTestUtils::FUserFlowExportState>();
	State->Test = this;
	State->TestName = TEXT("UserFlows.BatchDuplicatedMetaSoundOneShots");

	// Fresh timestamped temp output dir for THIS run only.
	const FString OutputRoot = AudioExporterUserFlowTestUtils::MakeOutputDirectory(
		FString::Printf(TEXT("UserFlows_BatchDups_%s"), *GuidDigits));

	const FAudioExporterConfig Config = AudioExporterUserFlowTestUtils::MakeUserFlowConfig(/*MaxLengthSeconds=*/5);
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FStartUserFlowExportCommand(
		State, RuntimeSubsystem, World, Entries, Config, OutputRoot, /*bArmAttendedBranch=*/false, nullptr, PreflightSucceeded));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FWaitForUserFlowExportTerminal(State, AudioExporterUserFlowTestUtils::UserFlowBatchWaitSeconds));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FRunAfterUserFlowExport(State,
		[](FAutomationTestBase& Test, AudioExporterUserFlowTestUtils::FUserFlowExportState& ExportState)
		{
			AudioExporterUserFlowTestUtils::VerifyBatchDuplicatedMetaSounds(Test, ExportState);
		}));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FUnloadTempPackagesCommand(TempPackages, State));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FDeleteUserFlowOutputDirectoryCommand(OutputRoot, State));

	return true;
}

// ---------------------------------------------------------------------------
// UNIT REGRESSION (companion to USER FLOW 3): loop-slice duration contract at
// extraction -- deterministic, no audio device.
//
// KILLS: any regression that (a) lets the time-correction mapping inflate a LOOP variation's
// exported length past its own uncorrected marker span -- the fast-audio-clock defect where a
// 5 s MaxLengthSeconds-capped loop exported ~9.96 s files once TimeCorrectionFactor rose above
// 1.0 -- or (b) shifts the corrected source POSITION of variations after the first -- the
// accidental-green global-trim defect, where trimming the batch buffer before extraction made
// every later variation slice from wrong offsets. Drives ONLY the real public utility entry
// FAudioExporterUtils::ExtractVariationBuffer with a synthesized batch (no audio device, no
// export session), so every expectation below is a hand-computed literal:
//
//   SampleRate = 1000 Hz, NumChannels = 2, TimeCorrectionFactor = 2.0 (captured content holds
//   TWICE the seconds the audio clock advanced -- the measured fast-audio-clock host shape).
//   Two LOOP markers of 1.0 s sit at audio-clock [0, 1] and [2, 3]; their CORRECTED buffer
//   windows are [0, 2] s and [4, 6] s, so the raw buffer spans 6 s = 12000 interleaved samples.
//   Loop cap per slice = uncorrected marker span = 1.0 s x 1000 Hz x 2 ch = 2000 samples.
//   Sample value == its interleaved index, so position assertions are exact integer floats.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterUserFlowLoopSliceCapPreservesCorrectedOffsetsTest,
	"AudioFactory.AudioExporter.UserFlows.LoopSliceCap.PreservesCorrectedOffsets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterUserFlowLoopSliceCapPreservesCorrectedOffsetsTest::RunTest(const FString& Parameters)
{
	// GUID-named temp package fixture anchor, mirroring this file's batch-dup pattern: never saved,
	// unloaded by the cleanup latent command on BOTH success and failure exit paths.
	const FString GuidDigits = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TArray<TWeakObjectPtr<UPackage>> TempPackages;
	const FString TempPackageName = FString::Printf(TEXT("/Game/AudioExporterUserFlowTests/%s/LoopSliceCap"), *GuidDigits);
	UPackage* TempPackage = CreatePackage(*TempPackageName);
	if (!TestNotNull(FString::Printf(TEXT("Temp package '%s' should be created."), *TempPackageName), TempPackage))
	{
		ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FUnloadTempPackagesCommand(TempPackages));
		return false;
	}
	TempPackages.Add(TempPackage);

	constexpr int32 TestSampleRate = 1000;
	constexpr int32 TestNumChannels = 2;
	constexpr double TestTimeCorrectionFactor = 2.0;

	// Raw captured content: 6 s stereo at 1 kHz = 12000 samples; value = interleaved index.
	TArray<float> BigBuffer;
	BigBuffer.SetNumUninitialized(12000);
	for (int32 SampleIndex = 0; SampleIndex < BigBuffer.Num(); ++SampleIndex)
	{
		BigBuffer[SampleIndex] = static_cast<float>(SampleIndex);
	}

	// LOOP variation 1: markers [0.0, 1.0]. Correction-mapped window = samples [0, 4000); the
	// uncorrected marker span caps the copy at EXACTLY 2000 samples from corrected offset 0.
	const TArray<float> FirstSlice = FAudioExporterUtils::ExtractVariationBuffer(
		BigBuffer, TestSampleRate, TestNumChannels, 0.0, 1.0, TestTimeCorrectionFactor, /*bIsLoop=*/true);
	if (!TestEqual(TEXT("Loop variation 1 must copy EXACTLY its uncorrected marker span "
		"(1.0 s x 1000 Hz x 2 ch = 2000 samples), not the correction-stretched 4000."),
		FirstSlice.Num(), 2000))
	{
		ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FUnloadTempPackagesCommand(TempPackages));
		return false;
	}
	TestEqual(TEXT("Loop variation 1's first sample must come from corrected source offset 0.0 s (sample 0)."),
		FirstSlice[0], 0.0f);
	TestEqual(TEXT("Loop variation 1's tail must be contiguous in the source (sample 1999)."),
		FirstSlice[1999], 1999.0f);

	// LOOP variation 2: markers [2.0, 3.0]. Its corrected SOURCE START is 2.0 s x factor 2.0 x
	// 1000 Hz x 2 ch = sample 8000 -- THE anti-trim assertion: trimming or shifting the batch
	// buffer before extraction moves or empties this slice even though its own cap math is
	// identical to variation 1's.
	const TArray<float> SecondSlice = FAudioExporterUtils::ExtractVariationBuffer(
		BigBuffer, TestSampleRate, TestNumChannels, 2.0, 3.0, TestTimeCorrectionFactor, /*bIsLoop=*/true);
	if (!TestEqual(TEXT("Loop variation 2 must copy EXACTLY its uncorrected marker span (2000 samples), not the stretched 4000."),
		SecondSlice.Num(), 2000))
	{
		ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FUnloadTempPackagesCommand(TempPackages));
		return false;
	}
	TestEqual(TEXT("Loop variation 2 must start at its CORRECTION-MAPPED source offset "
		"(2.0 s x 2.0 x 1000 Hz x 2 ch = sample 8000), not at an uncorrected or trimmed position."),
		SecondSlice[0], 8000.0f);
	TestEqual(TEXT("Loop variation 2's tail must be contiguous in the source (sample 9999)."),
		SecondSlice[1999], 9999.0f);

	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FUnloadTempPackagesCommand(TempPackages));
	return true;
}

// ---------------------------------------------------------------------------
// USER FLOW 3: ordinary SoundWaves are one pass; procedural SoundCue and MetaSound sources use the
// configured MaxLengthSeconds ceiling on their first observed pass whether or not they loop.
//
// KILLS: reintroducing one-shot truncation by max length (the FIX-ONESHOT-TRUNCATION defect: a
// one-shot chopped at the cap like a loop) makes the capped one-shot leg red; breaking the loop
// watchdog cap (loops rendering past the setting, or collapsing toward zero length) makes the
// wind-loop or procedural one-shot leg red.
//
// Contested-corner notes:
// - FAudioExporterConfig::MaxLengthSeconds is an int32 clamped to [5, 60] by Sanitize(); a literal
//   value of 2 is UNREPRESENTABLE at runtime (it would clamp to 5 and emit a coercion warning).
//   This test pins the nuance at the documented supported minimum of 5 -- both looping and
//   non-looping procedural captures must honor the setting once the observed output reaches it.
// - USoundBase::Duration is deliberately NOT trusted for natural length (MetaSoundSource reports a
//   ~9999 s placeholder here): the branch decision runs off an UNCAPPED disk-measured baseline
//   export of the same asset.
	// - The renderer's loop watchdog and start-timeout poll on FTSTicker (the engine core ticker,
	//   ticked unconditionally per engine frame) against elapsed MIXER AUDIO-CLOCK time -- not on
	//   the injected world's TimerManager -- so the wind-loop leg reaches its terminal and runs its
	//   FULL cap assertions on EVERY host, headless gate hosts included.
	// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterUserFlowProceduralCaptureCeilingTest,
	"AudioFactory.AudioExporter.UserFlows.ProceduralCaptureCeiling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterUserFlowProceduralCaptureCeilingTest::RunTest(const FString& Parameters)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("NoSound")))
	{
		AddError(TEXT("User-flow procedural capture-ceiling test is unavailable: this required live-audio flow was launched with -NoSound."));
		return false;
	}

	USoundBase* LoopAsset = LoadObject<USoundBase>(
		nullptr, TEXT("/Game/Procedurally_Generated_Wind/MS_Procedurally_Generated_Wind.MS_Procedurally_Generated_Wind"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!TestNotNull(TEXT("Required fixture '/Game/Procedurally_Generated_Wind/MS_Procedurally_Generated_Wind' should be present in this project."), LoopAsset))
	{
		return false;
	}

	USoundBase* OneShotAsset = LoadObject<USoundBase>(nullptr, TEXT("/Game/MetaSoundOneShot.MetaSoundOneShot"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!TestNotNull(TEXT("Required fixture '/Game/MetaSoundOneShot.MetaSoundOneShot' should be present in this project."), OneShotAsset))
	{
		return false;
	}

	UWorld* World = AudioExporterUserFlowTestUtils::ResolveAutomationWorld();
	if (!TestNotNull(TEXT("The loop-cap user-flow test should have an editor world for the real runtime pipeline."), World))
	{
		return false;
	}

	UAudioExporterRuntimeSubsystem* RuntimeSubsystem = GEngine ? GEngine->GetEngineSubsystem<UAudioExporterRuntimeSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("The loop-cap user-flow test should resolve the AudioExporter runtime subsystem."), RuntimeSubsystem))
	{
		return false;
	}
	const TSharedPtr<bool> PreflightSucceeded = MakeShared<bool>(false);
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FWaitForUserFlowPreflightDrain(
		*this, RuntimeSubsystem, TEXT("UserFlows.ProceduralCaptureCeiling"), PreflightSucceeded));

	// The one-shot's NATURAL length is taken from an UNCAPPED disk-measured export below, never
	// from USoundBase::Duration: measured on this exact fixture the
	// MetaSoundSource reports a ~9999 s placeholder Duration, which would poison any branch built
	// on it. Every expectation here stands on parsed bytes only.
	const TSharedRef<float> SharedBaselineDuration = MakeShared<float>(0.0f);
	const TSharedRef<float> SharedParsedLoopDuration = MakeShared<float>(0.0f);

	auto MakeSingleEntry = [](USoundBase* Sound) -> TArray<FAudioExportSoundEntry>
	{
		FAudioExportSoundEntry Entry;
		Entry.Sound = Sound;
		Entry.SoundName = Sound->GetName();
		Entry.SourceAssetPath = Sound->GetPackage()->GetName();
		Entry.RelativePath = AudioExporterUserFlowTestUtils::MakeRelativeAssetPath(Sound);
		return TArray<FAudioExportSoundEntry>{ Entry };
	};

	const FAudioExporterConfig CappedConfig = AudioExporterUserFlowTestUtils::MakeUserFlowConfig(/*MaxLengthSeconds=*/5);
	const FAudioExporterConfig BaselineConfig = AudioExporterUserFlowTestUtils::MakeUserFlowConfig(/*MaxLengthSeconds=*/60);

	// --- Part A: the LOOP is capped at MaxLengthSeconds ---------------------------------------
	const TSharedRef<AudioExporterUserFlowTestUtils::FUserFlowExportState> LoopState =
		MakeShared<AudioExporterUserFlowTestUtils::FUserFlowExportState>();
	LoopState->Test = this;
	LoopState->TestName = TEXT("UserFlows.ProceduralCaptureCeiling.WindLoop");
	const FString LoopOutputRoot = AudioExporterUserFlowTestUtils::MakeOutputDirectory(
		FString::Printf(TEXT("UserFlows_LoopCap_Loop_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

	// --- Part B0: uncapped BASELINE export of the same one-shot (MaxLengthSeconds=60) ---------
	const TSharedRef<AudioExporterUserFlowTestUtils::FUserFlowExportState> BaselineState =
		MakeShared<AudioExporterUserFlowTestUtils::FUserFlowExportState>();
	BaselineState->Test = this;
	BaselineState->TestName = TEXT("UserFlows.ProceduralCaptureCeiling.Baseline");
	const FString BaselineOutputRoot = AudioExporterUserFlowTestUtils::MakeOutputDirectory(
		FString::Printf(TEXT("UserFlows_LoopCap_Baseline_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

	// --- Part B: the SAME 5 s setting applies to the non-looping procedural source -------------
	const TSharedRef<AudioExporterUserFlowTestUtils::FUserFlowExportState> OneShotState =
		MakeShared<AudioExporterUserFlowTestUtils::FUserFlowExportState>();
	OneShotState->Test = this;
	OneShotState->TestName = TEXT("UserFlows.ProceduralCaptureCeiling.OneShot");
	const FString OneShotOutputRoot = AudioExporterUserFlowTestUtils::MakeOutputDirectory(
		FString::Printf(TEXT("UserFlows_LoopCap_OneShot_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FStartUserFlowExportCommand(
		LoopState, RuntimeSubsystem, World, MakeSingleEntry(LoopAsset), CappedConfig, LoopOutputRoot, /*bArmAttendedBranch=*/false, nullptr, PreflightSucceeded));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FWaitForUserFlowExportTerminal(LoopState, AudioExporterUserFlowTestUtils::UserFlowLoopLegWaitSeconds));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FRunAfterUserFlowExport(LoopState,
		[SharedParsedLoopDuration](FAutomationTestBase& Test, AudioExporterUserFlowTestUtils::FUserFlowExportState& State)
		{
			if (!Test.TestTrue(TEXT("The loop export should receive a structured terminal result."), State.bOutcomeReceived))
			{
				return;
			}
			Test.TestEqual(TEXT("The capped loop export should settle as Succeeded."), State.Outcome, EAudioExportOutcome::Succeeded);
			Test.TestEqual(TEXT("The capped loop should report exactly one successful file."), State.Result.SuccessfulCount, 1);
			Test.TestEqual(TEXT("The capped loop should report no failed files."), State.Result.FailedCount, 0);
			Test.TestEqual(TEXT("The capped loop should report no skipped files."), State.Result.SkippedCount, 0);
			Test.TestEqual(TEXT("The capped loop terminal counts must conserve the literal requested total of one."),
				State.Result.SuccessfulCount + State.Result.FailedCount + State.Result.SkippedCount, 1);

			TArray<FString> WavFiles;
			AudioExporterUserFlowTestUtils::FindWavFilesUnder(State.FinishedPath.IsEmpty() ? State.OutputRoot : State.FinishedPath, WavFiles);
			if (!Test.TestEqual(TEXT("The capped loop export must produce exactly one WAV."), WavFiles.Num(), 1))
			{
				return;
			}

			AudioExporterUserFlowTestUtils::FUserFlowWavInfo Info;
			if (!AudioExporterUserFlowTestUtils::VerifyProducedWavHealth(Test, WavFiles[0], TEXT("Capped wind loop"), Info))
			{
				return;
			}
			*SharedParsedLoopDuration = static_cast<float>(Info.DurationSeconds);

			// Hand-computed literals: a looping source must land AT the 5 s cap (within jitter
			// tolerance) and clearly above zero-length collapse territory (> 0.5 s).
			Test.TestTrue(FString::Printf(TEXT("The capped loop must render <= 5.0 s (+%.2f s tolerance); got %.3f s."),
				AudioExporterUserFlowTestUtils::UserFlowLoopCapToleranceSeconds, Info.DurationSeconds),
				Info.DurationSeconds <= 5.0 + AudioExporterUserFlowTestUtils::UserFlowLoopCapToleranceSeconds);
			Test.TestTrue(FString::Printf(TEXT("The capped loop must render a substantial capture (> 0.5 s); got %.3f s."), Info.DurationSeconds),
				Info.DurationSeconds > 0.5);
		}));

	// Queued behind part A regardless of its outcome: each next session may only start once the
	// previous reached its terminal (the subsystem refuses concurrent starts).
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FStartUserFlowExportCommand(
		BaselineState, RuntimeSubsystem, World, MakeSingleEntry(OneShotAsset), BaselineConfig, BaselineOutputRoot, /*bArmAttendedBranch=*/false, nullptr, PreflightSucceeded));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FWaitForUserFlowExportTerminal(BaselineState, AudioExporterUserFlowTestUtils::UserFlowDefaultWaitSeconds));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FRunAfterUserFlowExport(BaselineState,
		[SharedBaselineDuration](FAutomationTestBase& Test, AudioExporterUserFlowTestUtils::FUserFlowExportState& State)
		{
			if (!Test.TestTrue(TEXT("The baseline export should receive a structured terminal result."), State.bOutcomeReceived))
			{
				return;
			}
			Test.TestEqual(TEXT("The baseline export should settle as Succeeded."), State.Outcome, EAudioExportOutcome::Succeeded);
			Test.TestEqual(TEXT("The natural baseline should report exactly one successful file."), State.Result.SuccessfulCount, 1);
			Test.TestEqual(TEXT("The natural baseline should report no failed files."), State.Result.FailedCount, 0);
			Test.TestEqual(TEXT("The natural baseline should report no skipped files."), State.Result.SkippedCount, 0);
			Test.TestEqual(TEXT("The natural baseline terminal counts must conserve the literal requested total of one."),
				State.Result.SuccessfulCount + State.Result.FailedCount + State.Result.SkippedCount, 1);

			TArray<FString> WavFiles;
			AudioExporterUserFlowTestUtils::FindWavFilesUnder(State.FinishedPath.IsEmpty() ? State.OutputRoot : State.FinishedPath, WavFiles);
			if (!Test.TestEqual(TEXT("The baseline export must produce exactly one WAV."), WavFiles.Num(), 1))
			{
				return;
			}

			AudioExporterUserFlowTestUtils::FUserFlowWavInfo Info;
			if (!AudioExporterUserFlowTestUtils::VerifyProducedWavHealth(Test, WavFiles[0], TEXT("Uncapped baseline one-shot"), Info))
			{
				return;
			}
			*SharedBaselineDuration = static_cast<float>(Info.DurationSeconds);
		}));

	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FStartUserFlowExportCommand(
		OneShotState, RuntimeSubsystem, World, MakeSingleEntry(OneShotAsset), CappedConfig, OneShotOutputRoot, /*bArmAttendedBranch=*/false, nullptr, PreflightSucceeded));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FWaitForUserFlowExportTerminal(OneShotState, AudioExporterUserFlowTestUtils::UserFlowDefaultWaitSeconds));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FRunAfterUserFlowExport(OneShotState,
		[SharedBaselineDuration, SharedParsedLoopDuration](FAutomationTestBase& Test, AudioExporterUserFlowTestUtils::FUserFlowExportState& State)
		{
			if (!Test.TestTrue(TEXT("The one-shot export should receive a structured terminal result."), State.bOutcomeReceived))
			{
				return;
			}
			Test.TestEqual(TEXT("The one-shot export should settle as Succeeded."), State.Outcome, EAudioExportOutcome::Succeeded);
			Test.TestEqual(TEXT("The capped non-looping procedural export should report exactly one successful file."), State.Result.SuccessfulCount, 1);
			Test.TestEqual(TEXT("The capped non-looping procedural export should report no failed files."), State.Result.FailedCount, 0);
			Test.TestEqual(TEXT("The capped non-looping procedural export should report no skipped files."), State.Result.SkippedCount, 0);
			Test.TestEqual(TEXT("The capped non-looping procedural terminal counts must conserve the literal requested total of one."),
				State.Result.SuccessfulCount + State.Result.FailedCount + State.Result.SkippedCount, 1);

			TArray<FString> WavFiles;
			AudioExporterUserFlowTestUtils::FindWavFilesUnder(State.FinishedPath.IsEmpty() ? State.OutputRoot : State.FinishedPath, WavFiles);
			if (!Test.TestEqual(TEXT("The one-shot export must produce exactly one WAV."), WavFiles.Num(), 1))
			{
				return;
			}

			AudioExporterUserFlowTestUtils::FUserFlowWavInfo Info;
			if (!AudioExporterUserFlowTestUtils::VerifyProducedWavHealth(Test, WavFiles[0], TEXT("Same-setting one-shot"), Info))
			{
				return;
			}

			const double BaselineSeconds = static_cast<double>(*SharedBaselineDuration);
			constexpr float CapSeconds = 5.0f;
			if (!Test.TestTrue(TEXT("The uncapped baseline must have measured a positive natural duration first."), BaselineSeconds > 0.0))
			{
				return;
			}

			if (BaselineSeconds > CapSeconds + 0.5)
			{
				// A non-looping procedural source must stop at the configured capture ceiling once
				// its natural output is observed to exceed that ceiling.
				Test.TestTrue(FString::Printf(TEXT("A %.3f s non-looping procedural source exported with MaxLengthSeconds=%d must be capped (<= %.2f s); got %.3f s."),
					BaselineSeconds, static_cast<int32>(CapSeconds),
					CapSeconds + AudioExporterUserFlowTestUtils::UserFlowLoopCapToleranceSeconds, Info.DurationSeconds),
					Info.DurationSeconds <= CapSeconds + AudioExporterUserFlowTestUtils::UserFlowLoopCapToleranceSeconds);
			}
			else
			{
				// Both captures are the same natural fixture. Allow only two observed audio frames or
				// two percent of the measured fixture duration for renderer scheduling granularity.
				// A clipped or materially drifted capture must remain outside this narrow bound.
				const double FrameResolutionSeconds = 1.0 / static_cast<double>(Info.SampleRate);
				const double FixtureDurationSchedulingAllowance = BaselineSeconds * 0.02;
				const double NaturalTolerance = FMath::Max(2.0 * FrameResolutionSeconds, FixtureDurationSchedulingAllowance);
				Test.TestTrue(FString::Printf(TEXT("A non-looping procedural source naturally shorter than the cap (%.3f s) must render its NATURAL length (+/- %.2f s); got %.3f s."),
					BaselineSeconds, NaturalTolerance, Info.DurationSeconds),
					FMath::Abs(Info.DurationSeconds - BaselineSeconds) <= NaturalTolerance);
			}

			// Cross-part honesty: whenever the natural length exceeds the configured ceiling, the
			// non-looping procedural source must be capped at that same ceiling.
			if (*SharedParsedLoopDuration > 0.0f && BaselineSeconds > static_cast<double>(*SharedParsedLoopDuration))
			{
				Test.TestTrue(FString::Printf(TEXT("Non-looping procedural render (%.3f s) must remain no longer than the configured capture ceiling (%.3f s)."),
					Info.DurationSeconds, static_cast<double>(*SharedParsedLoopDuration)),
					Info.DurationSeconds <= 5.0f + AudioExporterUserFlowTestUtils::UserFlowLoopCapToleranceSeconds);
			}
		}));

	// Terminal cleanup of all three output roots this test created, on success AND failure paths.
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FDeleteUserFlowOutputDirectoryCommand(LoopOutputRoot, LoopState));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FDeleteUserFlowOutputDirectoryCommand(BaselineOutputRoot, BaselineState));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FDeleteUserFlowOutputDirectoryCommand(OneShotOutputRoot, OneShotState));

	return true;
}

// ---------------------------------------------------------------------------
// USER FLOW 4: attended multi-batch separator flow (regression guard).
//
// KILLS: the return of the attended-batch hang -- an ATTENDED export stalling after its first variation
// or after the FIRST session (shared teardown clearing the armed separator timer between
// back-to-back batches, or the separator anchor losing its tick again). Both consecutive attended
// sessions must reach Succeeded with progress advanced INTO variation 2 within the bounded wait;
// a hang times out the wait and lands as a loud error.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterUserFlowAttendedMultiBatchTest,
	"AudioFactory.AudioExporter.UserFlows.AttendedMultiBatchSeparator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterUserFlowAttendedMultiBatchTest::RunTest(const FString& Parameters)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("NoSound")))
	{
		AddError(TEXT("User-flow attended multi-batch coverage is unavailable: this required live-audio regression was launched with -NoSound."));
		return false;
	}

	// A SoundCue is NOT a static source, so OneShotVariations=2 yields two real variations per
	// session -- each session must cross the inter-variation separator, and the pair must cross
	// the inter-session boundary.
	USoundBase* FixtureSound = LoadObject<USoundBase>(nullptr, TEXT("/Game/SoundCueOneShot.SoundCueOneShot"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!TestNotNull(TEXT("Required fixture '/Game/SoundCueOneShot.SoundCueOneShot' should be present in this project."), FixtureSound))
	{
		return false;
	}

	UWorld* World = AudioExporterUserFlowTestUtils::ResolveAutomationWorld();
	if (!TestNotNull(TEXT("The attended multi-batch user-flow test should have an editor world for the real runtime pipeline."), World))
	{
		return false;
	}

	UAudioExporterRuntimeSubsystem* RuntimeSubsystem = GEngine ? GEngine->GetEngineSubsystem<UAudioExporterRuntimeSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("The attended multi-batch user-flow test should resolve the AudioExporter runtime subsystem."), RuntimeSubsystem))
	{
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FWaitForUserFlowPreflightDrain(
		*this, RuntimeSubsystem, TEXT("UserFlows.AttendedMultiBatchSeparator")));

	FAudioExportSoundEntry Entry;
	Entry.Sound = FixtureSound;
	Entry.SoundName = FixtureSound->GetName();
	Entry.SourceAssetPath = FixtureSound->GetPackage()->GetName();
	Entry.RelativePath = AudioExporterUserFlowTestUtils::MakeRelativeAssetPath(FixtureSound);
	const TArray<FAudioExportSoundEntry> Entries { Entry };

	// Two variations per one-shot; the supported minimum max length; reports on.
	FAudioExporterConfig AttendedConfig;
	AttendedConfig.OneShotVariations = 2;
	AttendedConfig.LoopVariations = 1;
	AttendedConfig.MaxLengthSeconds = 5;
	AttendedConfig.bNormalize = false;
	AttendedConfig.bMonitor = false;
	AttendedConfig.bExportTextLog = true;

	const TSharedRef<AudioExporterUserFlowTestUtils::FUserFlowExportState> SessionA =
		MakeShared<AudioExporterUserFlowTestUtils::FUserFlowExportState>();
	SessionA->Test = this;
	SessionA->TestName = TEXT("UserFlows.AttendedMultiBatchSeparator.SessionA");
	SessionA->RuntimeSubsystem = RuntimeSubsystem;

	const TSharedRef<AudioExporterUserFlowTestUtils::FUserFlowExportState> SessionB =
		MakeShared<AudioExporterUserFlowTestUtils::FUserFlowExportState>();
	SessionB->Test = this;
	SessionB->TestName = TEXT("UserFlows.AttendedMultiBatchSeparator.SessionB");
	SessionB->RuntimeSubsystem = RuntimeSubsystem;

	const FString RootA = AudioExporterUserFlowTestUtils::MakeOutputDirectory(TEXT("UserFlows_Attended_SessionA"));
	const FString RootB = AudioExporterUserFlowTestUtils::MakeOutputDirectory(TEXT("UserFlows_Attended_SessionB"));

	// Session A arms the test-only attended-branch override at start time (inside its latent
	// start command, immediately before StartExport); the FINAL verify disarms it first thing so
	// no exit path can leak the forced branch into later tests.
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FStartUserFlowExportCommand(SessionA, RuntimeSubsystem, World, Entries, AttendedConfig, RootA, /*bArmAttendedBranch=*/true));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FWaitForUserFlowExportTerminal(SessionA, AudioExporterUserFlowTestUtils::UserFlowAttendedSessionWaitSeconds));
	// Detach ALL of Session A's listeners BEFORE Session B starts: the subsystem broadcasts every
	// terminal to every active listener, and A's handlers must not hear B's terminal.
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FRunAfterUserFlowExport(SessionA,
		[](FAutomationTestBase&, AudioExporterUserFlowTestUtils::FUserFlowExportState&) {}));

	// Session B starts ONLY from inside the chain, right after A's terminal -- exactly the
	// back-to-back shape that used to lose the armed separator.
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FStartUserFlowExportCommand(
		SessionB, RuntimeSubsystem, World, Entries, AttendedConfig, RootB, /*bArmAttendedBranch=*/false, SessionA));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FWaitForUserFlowExportTerminal(SessionB, AudioExporterUserFlowTestUtils::UserFlowAttendedSessionWaitSeconds));

	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FRunAfterUserFlowExport(SessionB,
		[SessionA](FAutomationTestBase& Test, AudioExporterUserFlowTestUtils::FUserFlowExportState& SessionBStateRef)
		{
			// Disarm FIRST on every path so a hung/red run can never leak the forced branch.
			FAudioRendererRealtime::SetForceAttendedSeparatorBranchForTesting(false);
			if (SessionA->bWaitTimedOut
				|| !SessionA->bWriterDrainConfirmed
				|| !SessionA->bOutcomeReceived
				|| SessionA->Outcome != EAudioExportOutcome::Succeeded
				|| SessionBStateRef.bStartRejected)
			{
				return;
			}

			auto VerifyAttendedSession = [&Test](AudioExporterUserFlowTestUtils::FUserFlowExportState& State)
			{
				AudioExporterUserFlowTestUtils::RemoveUserFlowDelegates(State);

				if (!Test.TestTrue(FString::Printf(TEXT("%s should receive a structured terminal result."), *State.TestName), State.bOutcomeReceived))
				{
					return;
				}
				Test.TestEqual(FString::Printf(TEXT("%s should settle exactly one legacy completion event."), *State.TestName), State.LegacyTerminalEvents, 1);
				Test.TestEqual(FString::Printf(TEXT("%s should settle exactly one structured completion event."), *State.TestName), State.StructuredTerminalEvents, 1);
				Test.TestEqual(FString::Printf(TEXT("%s should settle as Succeeded."), *State.TestName), State.Outcome, EAudioExportOutcome::Succeeded);
				Test.TestEqual(FString::Printf(TEXT("%s should report exactly 2 successful variation files."), *State.TestName), State.Result.SuccessfulCount, 2);
				Test.TestEqual(FString::Printf(TEXT("%s should report zero failed variation files."), *State.TestName), State.Result.FailedCount, 0);
				Test.TestEqual(FString::Printf(TEXT("%s should report zero skipped variation files."), *State.TestName), State.Result.SkippedCount, 0);
				Test.TestEqual(FString::Printf(TEXT("%s terminal counts must conserve the literal requested total of 2 variations."), *State.TestName),
					State.Result.SuccessfulCount + State.Result.FailedCount + State.Result.SkippedCount, 2);
				// THE decisive observable: progress advanced INTO variation 2, i.e. the armed
				// separator fired and the session moved past variation 1.
				Test.TestEqual(FString::Printf(TEXT("%s progress should advance into variation 2 past the separator."), *State.TestName), State.MaxVariationSeen, 2);

				TArray<FString> WavFiles;
				AudioExporterUserFlowTestUtils::FindWavFilesUnder(State.FinishedPath.IsEmpty() ? State.OutputRoot : State.FinishedPath, WavFiles);
				if (Test.TestEqual(FString::Printf(TEXT("%s should produce exactly one WAV per variation (2 total)."), *State.TestName), WavFiles.Num(), 2))
				{
					for (int32 FileIndex = 0; FileIndex < WavFiles.Num(); ++FileIndex)
					{
						AudioExporterUserFlowTestUtils::FUserFlowWavInfo Info;
						AudioExporterUserFlowTestUtils::VerifyProducedWavHealth(Test, WavFiles[FileIndex],
							FString::Printf(TEXT("%s variation file #%d"), *State.TestName, FileIndex + 1), Info);
					}
				}
			};

			VerifyAttendedSession(*SessionA);
			VerifyAttendedSession(SessionBStateRef);
		}));

	// Terminal cleanup of both attended-session output roots, success AND failure paths.
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FDeleteUserFlowOutputDirectoryCommand(RootA, SessionA));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FDeleteUserFlowOutputDirectoryCommand(RootB, SessionB));

	return true;
}

// ---------------------------------------------------------------------------
// USER FLOW 5: exactly-once terminal delivery across early- and late-armed listeners.
//
// KILLS: any duplicate-terminal regression (a replay ticker re-delivering a cached completion, a
// double broadcast from the finalize handoff) -- the EARLY listener would count 2+; and any
// delivery model where a listener attached after StartExport misses the settled terminal -- the
// LATE listener would count 0.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterUserFlowTerminalExactlyOnceTest,
	"AudioFactory.AudioExporter.UserFlows.TerminalDeliveryExactlyOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterUserFlowTerminalExactlyOnceTest::RunTest(const FString& Parameters)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("NoSound")))
	{
		AddError(TEXT("User-flow exactly-once delivery test is unavailable: this required live-audio flow was launched with -NoSound."));
		return false;
	}

	USoundBase* FixtureSound = LoadObject<USoundBase>(nullptr, TEXT("/Game/MetaSoundOneShot.MetaSoundOneShot"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!TestNotNull(TEXT("Required fixture '/Game/MetaSoundOneShot.MetaSoundOneShot' should be present in this project."), FixtureSound))
	{
		return false;
	}

	UWorld* World = AudioExporterUserFlowTestUtils::ResolveAutomationWorld();
	if (!TestNotNull(TEXT("The exactly-once user-flow test should have an editor world for the real runtime pipeline."), World))
	{
		return false;
	}

	UAudioExporterRuntimeSubsystem* RuntimeSubsystem = GEngine ? GEngine->GetEngineSubsystem<UAudioExporterRuntimeSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("The exactly-once user-flow test should resolve the AudioExporter runtime subsystem."), RuntimeSubsystem))
	{
		return false;
	}
	const TSharedPtr<bool> PreflightSucceeded = MakeShared<bool>(false);
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FWaitForUserFlowPreflightDrain(
		*this, RuntimeSubsystem, TEXT("UserFlows.ExactlyOnceDelivery"), PreflightSucceeded));

	// Independent listener rigs, kept alive through the terminal via shared refs. The EARLY rig
	// subscribes BEFORE the start request; the LATE rig subscribes synchronously AFTER StartExport
	// returns (long before a seconds-long render can settle).
	struct FTerminalListenerRig
	{
		int32 LegacyEvents = 0;
		int32 StructuredEvents = 0;
	};
	const TSharedRef<FTerminalListenerRig> EarlyRig = MakeShared<FTerminalListenerRig>();
	const TSharedRef<FTerminalListenerRig> LateRig = MakeShared<FTerminalListenerRig>();

	FDelegateHandle EarlyLegacyHandle = RuntimeSubsystem->OnExportCompleteNative.AddLambda([EarlyRig](const FString&)
	{
		++EarlyRig->LegacyEvents;
	});
	FDelegateHandle EarlyStructuredHandle = RuntimeSubsystem->OnExportCompleteResultNative.AddLambda([EarlyRig](const FAudioExportResult&)
	{
		++EarlyRig->StructuredEvents;
	});

	FAudioExportSoundEntry Entry;
	Entry.Sound = FixtureSound;
	Entry.SoundName = FixtureSound->GetName();
	Entry.SourceAssetPath = FixtureSound->GetPackage()->GetName();
	Entry.RelativePath = AudioExporterUserFlowTestUtils::MakeRelativeAssetPath(FixtureSound);

	const TSharedRef<AudioExporterUserFlowTestUtils::FUserFlowExportState> State =
		MakeShared<AudioExporterUserFlowTestUtils::FUserFlowExportState>();
	State->Test = this;
	State->TestName = TEXT("UserFlows.TerminalDeliveryExactlyOnce");
	const FString OutputRoot = AudioExporterUserFlowTestUtils::MakeOutputDirectory(TEXT("UserFlows_TerminalExactlyOnce"));

	const FAudioExporterConfig Config = AudioExporterUserFlowTestUtils::MakeUserFlowConfig(/*MaxLengthSeconds=*/5);
	const TSharedRef<FDelegateHandle> LateLegacyHandle = MakeShared<FDelegateHandle>();
	const TSharedRef<FDelegateHandle> LateStructuredHandle = MakeShared<FDelegateHandle>();
	State->bRunVerifierWhenRejected = true;
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FStartUserFlowExportCommand(
		State, RuntimeSubsystem, World, TArray<FAudioExportSoundEntry> { Entry }, Config, OutputRoot,
		/*bArmAttendedBranch=*/false, nullptr, PreflightSucceeded,
		[RuntimeSubsystem, LateRig, LateLegacyHandle, LateStructuredHandle]()
		{
			*LateLegacyHandle = RuntimeSubsystem->OnExportCompleteNative.AddLambda([LateRig](const FString&)
			{
				++LateRig->LegacyEvents;
			});
			*LateStructuredHandle = RuntimeSubsystem->OnExportCompleteResultNative.AddLambda([LateRig](const FAudioExportResult&)
			{
				++LateRig->StructuredEvents;
			});
		}));

	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FWaitForUserFlowExportTerminal(State, AudioExporterUserFlowTestUtils::UserFlowDefaultWaitSeconds));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FRunAfterUserFlowExport(State,
		[RuntimeSubsystem, EarlyRig, LateRig, EarlyLegacyHandle, EarlyStructuredHandle, LateLegacyHandle, LateStructuredHandle]
		(FAutomationTestBase& Test, AudioExporterUserFlowTestUtils::FUserFlowExportState& ExportState)
		{
			// Disarm both rigs before asserting (a red run must never leak handlers).
			RuntimeSubsystem->OnExportCompleteNative.Remove(EarlyLegacyHandle);
			RuntimeSubsystem->OnExportCompleteResultNative.Remove(EarlyStructuredHandle);
			RuntimeSubsystem->OnExportCompleteNative.Remove(*LateLegacyHandle);
			RuntimeSubsystem->OnExportCompleteResultNative.Remove(*LateStructuredHandle);

			Test.TestTrue(TEXT("Sanity: the driven export should have received its structured terminal."), ExportState.bOutcomeReceived);
			Test.TestEqual(TEXT("The early-armed listener must observe EXACTLY one legacy terminal broadcast."), EarlyRig->LegacyEvents, 1);
			Test.TestEqual(TEXT("The early-armed listener must observe EXACTLY one structured terminal broadcast."), EarlyRig->StructuredEvents, 1);
			Test.TestEqual(TEXT("The late-armed listener must observe EXACTLY one legacy terminal broadcast."), LateRig->LegacyEvents, 1);
			Test.TestEqual(TEXT("The late-armed listener must observe EXACTLY one structured terminal broadcast."), LateRig->StructuredEvents, 1);
			Test.TestEqual(TEXT("The settled outcome should be Succeeded for the exactly-once flow."), ExportState.Outcome, EAudioExportOutcome::Succeeded);
			Test.TestEqual(TEXT("The structured result outcome must match the settled outcome."), ExportState.Result.Outcome, ExportState.Outcome);
			Test.TestEqual(TEXT("The exactly-once flow must report exactly one requested file."),
				ExportState.Result.SuccessfulCount + ExportState.Result.FailedCount + ExportState.Result.SkippedCount, 1);
			Test.TestEqual(TEXT("The exactly-once flow must report exactly one successful file."), ExportState.Result.SuccessfulCount, 1);
			Test.TestEqual(TEXT("The exactly-once flow must report zero failed files."), ExportState.Result.FailedCount, 0);
			Test.TestEqual(TEXT("The exactly-once flow must report zero skipped files."), ExportState.Result.SkippedCount, 0);

			TArray<FString> WavFiles;
			AudioExporterUserFlowTestUtils::FindWavFilesUnder(
				ExportState.FinishedPath.IsEmpty() ? ExportState.OutputRoot : ExportState.FinishedPath, WavFiles);
			if (Test.TestEqual(TEXT("The exactly-once flow must publish exactly one physical WAV."), WavFiles.Num(), 1))
			{
				AudioExporterUserFlowTestUtils::FUserFlowWavInfo WavInfo;
				AudioExporterUserFlowTestUtils::VerifyProducedWavHealth(Test, WavFiles[0], TEXT("Exactly-once flow"), WavInfo);
			}
		}));

	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FDeleteUserFlowOutputDirectoryCommand(OutputRoot, State));

	return true;
}

// ---------------------------------------------------------------------------
// USER FLOW 6: timestamped default-destination honesty.
//
// KILLS: broken <timestamp> expansion on the DEFAULT destination (raw placeholder text in the
// folder name, wrong format, zero or multiple expansions) and session-directory reuse (a second
// identical export landing in the previous run's folder, or failing outright on the collision).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterUserFlowTimestampDestinationTest,
	"AudioFactory.AudioExporter.UserFlows.TimestampedDestinationHonesty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterUserFlowTimestampDestinationTest::RunTest(const FString& Parameters)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("NoSound")))
	{
		AddError(TEXT("User-flow timestamped-destination test is unavailable: this required live-audio flow was launched with -NoSound."));
		return false;
	}

	USoundBase* FixtureSound = LoadObject<USoundBase>(nullptr, TEXT("/Game/MetaSoundOneShot.MetaSoundOneShot"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!TestNotNull(TEXT("Required fixture '/Game/MetaSoundOneShot.MetaSoundOneShot' should be present in this project."), FixtureSound))
	{
		return false;
	}

	UWorld* World = AudioExporterUserFlowTestUtils::ResolveAutomationWorld();
	if (!TestNotNull(TEXT("The timestamped-destination user-flow test should have an editor world for the real runtime pipeline."), World))
	{
		return false;
	}

	UAudioExporterRuntimeSubsystem* RuntimeSubsystem = GEngine ? GEngine->GetEngineSubsystem<UAudioExporterRuntimeSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("The timestamped-destination user-flow test should resolve the AudioExporter runtime subsystem."), RuntimeSubsystem))
	{
		return false;
	}
	const TSharedPtr<bool> PreflightSucceeded = MakeShared<bool>(false);
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FWaitForUserFlowPreflightDrain(
		*this, RuntimeSubsystem, TEXT("UserFlows.TimestampedDestinationHonesty"), PreflightSucceeded));

	FAudioExportSoundEntry Entry;
	Entry.Sound = FixtureSound;
	Entry.SoundName = FixtureSound->GetName();
	Entry.SourceAssetPath = FixtureSound->GetPackage()->GetName();
	Entry.RelativePath = AudioExporterUserFlowTestUtils::MakeRelativeAssetPath(FixtureSound);
	const TArray<FAudioExportSoundEntry> Entries { Entry };
	const FAudioExporterConfig Config = AudioExporterUserFlowTestUtils::MakeUserFlowConfig(/*MaxLengthSeconds=*/5);

	const TSharedRef<AudioExporterUserFlowTestUtils::FUserFlowExportState> FirstState =
		MakeShared<AudioExporterUserFlowTestUtils::FUserFlowExportState>();
	FirstState->Test = this;
	FirstState->TestName = TEXT("UserFlows.TimestampedDestinationHonesty.First");
	const TSharedRef<AudioExporterUserFlowTestUtils::FUserFlowExportState> SecondState =
		MakeShared<AudioExporterUserFlowTestUtils::FUserFlowExportState>();
	SecondState->Test = this;
	SecondState->TestName = TEXT("UserFlows.TimestampedDestinationHonesty.Second");

	// Empty OutputPath => the product's DEFAULT timestamped destination (the flow under test).
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FStartUserFlowExportCommand(
		FirstState, RuntimeSubsystem, World, Entries, Config, /*OutputRoot=*/TEXT(""), /*bArmAttendedBranch=*/false,
		nullptr, PreflightSucceeded));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FWaitForUserFlowExportTerminal(FirstState, AudioExporterUserFlowTestUtils::UserFlowDefaultWaitSeconds));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FRunAfterUserFlowExport(FirstState,
		[](FAutomationTestBase& Test, AudioExporterUserFlowTestUtils::FUserFlowExportState& State)
		{
			if (!Test.TestTrue(TEXT("The first default-destination export should receive a structured terminal."), State.bOutcomeReceived))
			{
				return;
			}
			Test.TestEqual(TEXT("The first default-destination export should settle as Succeeded."), State.Outcome, EAudioExportOutcome::Succeeded);
			Test.TestEqual(TEXT("The first default-destination export should settle exactly one legacy completion event."), State.LegacyTerminalEvents, 1);
			Test.TestEqual(TEXT("The first structured result outcome must match the settled outcome."), State.Result.Outcome, State.Outcome);
			Test.TestEqual(TEXT("The first default-destination export must conserve exactly one requested file."),
				State.Result.SuccessfulCount + State.Result.FailedCount + State.Result.SkippedCount, 1);
			Test.TestEqual(TEXT("The first default-destination export must report exactly one successful file."), State.Result.SuccessfulCount, 1);
			Test.TestEqual(TEXT("The first default-destination export must report zero failed files."), State.Result.FailedCount, 0);
			Test.TestEqual(TEXT("The first default-destination export must report zero skipped files."), State.Result.SkippedCount, 0);
			if (Test.TestFalse(TEXT("The completed default destination path should not be empty."), State.FinishedPath.IsEmpty()))
			{
				FString SessionLeaf;
				int32 Occurrences = 0;
				const bool bLeafIsTimestamp = AudioExporterUserFlowTestUtils::LeafIsExactlyOneTimestamp(State.FinishedPath, SessionLeaf, Occurrences);
				Test.TestTrue(FString::Printf(TEXT("The default destination leaf '%s' must be EXACTLY one YYYY-MM-DD_HH-MM-SS expansion."), *SessionLeaf),
					bLeafIsTimestamp);
				Test.TestEqual(FString::Printf(TEXT("The timestamp expansion must occur exactly ONCE in the destination path ('%s')."),
					*State.FinishedPath), Occurrences, 1);
				Test.TestTrue(FString::Printf(TEXT("The default destination session dir ('%s') must exist on disk."), *State.FinishedPath),
					IFileManager::Get().DirectoryExists(*FPaths::ConvertRelativePathToFull(State.FinishedPath)));
			}
			TArray<FString> WavFiles;
			AudioExporterUserFlowTestUtils::FindWavFilesUnder(State.FinishedPath, WavFiles);
			if (Test.TestEqual(TEXT("The first default-destination export must publish exactly one physical WAV."), WavFiles.Num(), 1))
			{
				AudioExporterUserFlowTestUtils::FUserFlowWavInfo WavInfo;
				AudioExporterUserFlowTestUtils::VerifyProducedWavHealth(Test, WavFiles[0], TEXT("First default-destination export"), WavInfo);
			}
		}));

	// Second identical export, queued behind the first: must create a DIFFERENT session dir (no
	// reuse; a same-second collision must resolve through the deterministic suffix sequence, never
	// overwrite or fail).
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FStartUserFlowExportCommand(
		SecondState, RuntimeSubsystem, World, Entries, Config, /*OutputRoot=*/TEXT(""), /*bArmAttendedBranch=*/false, FirstState));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FWaitForUserFlowExportTerminal(SecondState, AudioExporterUserFlowTestUtils::UserFlowDefaultWaitSeconds));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FRunAfterUserFlowExport(SecondState,
		[FirstState](FAutomationTestBase& Test, AudioExporterUserFlowTestUtils::FUserFlowExportState& State)
		{
			if (!Test.TestTrue(TEXT("The second default-destination export should receive a structured terminal."), State.bOutcomeReceived))
			{
				return;
			}
			Test.TestEqual(TEXT("The second default-destination export should settle as Succeeded."), State.Outcome, EAudioExportOutcome::Succeeded);
			Test.TestEqual(TEXT("The second structured result outcome must match the settled outcome."), State.Result.Outcome, State.Outcome);
			Test.TestEqual(TEXT("The second default-destination export must conserve exactly one requested file."),
				State.Result.SuccessfulCount + State.Result.FailedCount + State.Result.SkippedCount, 1);
			Test.TestEqual(TEXT("The second default-destination export must report exactly one successful file."), State.Result.SuccessfulCount, 1);
			Test.TestEqual(TEXT("The second default-destination export must report zero failed files."), State.Result.FailedCount, 0);
			Test.TestEqual(TEXT("The second default-destination export must report zero skipped files."), State.Result.SkippedCount, 0);
			Test.TestFalse(TEXT("The second default destination path should not be empty."), State.FinishedPath.IsEmpty());
			Test.TestTrue(FString::Printf(TEXT("The second session dir ('%s') must exist on disk."), *State.FinishedPath),
				!State.FinishedPath.IsEmpty() && IFileManager::Get().DirectoryExists(*FPaths::ConvertRelativePathToFull(State.FinishedPath)));
			TArray<FString> WavFiles;
			AudioExporterUserFlowTestUtils::FindWavFilesUnder(State.FinishedPath, WavFiles);
			if (Test.TestEqual(TEXT("The second default-destination export must publish exactly one physical WAV."), WavFiles.Num(), 1))
			{
				AudioExporterUserFlowTestUtils::FUserFlowWavInfo WavInfo;
				AudioExporterUserFlowTestUtils::VerifyProducedWavHealth(Test, WavFiles[0], TEXT("Second default-destination export"), WavInfo);
			}

			// THE key assert: no reuse of the previous session's folder, and no collision
			// failure masquerading as success.
			Test.TestTrue(FString::Printf(TEXT("A second identical export must create a DIFFERENT session dir ('%s' vs '%s'); reuse or overwrite is a defect."),
				*State.FinishedPath, *FirstState->FinishedPath),
				!State.FinishedPath.IsEmpty() && State.FinishedPath != FirstState->FinishedPath);
			Test.TestEqual(TEXT("Matching request facts must retain the same timestamped base directory."),
				FPaths::GetPath(State.FinishedPath), FPaths::GetPath(FirstState->FinishedPath));
			const FString FirstLeaf = FPaths::GetPathLeaf(FirstState->FinishedPath);
			const FString SecondLeaf = FPaths::GetPathLeaf(State.FinishedPath);
			const bool bDifferentTimestamp = FirstLeaf.Len() < 19 || SecondLeaf.Len() < 19
				|| FirstLeaf.Left(19) != SecondLeaf.Left(19);
			const bool bDeterministicCollisionSuffix = SecondLeaf == FirstLeaf + TEXT("_1");
			Test.TestTrue(TEXT("A same-timestamp collision must use the deterministic _1 fallback, while a later timestamp remains valid."),
				bDifferentTimestamp || bDeterministicCollisionSuffix);

			// Self-clean of BOTH default-location session dirs this test created is queued as the
			// latent command BELOW, not inline here: it still runs after every assert above, but a
			// red or timed-out run can no longer skip it via an early return. Only ever folders
			// whose leaf matches the timestamped session pattern are eligible.
		}));

	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FDeleteUserFlowDefaultSessionDirectoriesCommand({ FirstState, SecondState }));

	return true;
}

// ---------------------------------------------------------------------------
// USER FLOW 7: the generated text report must match physical disk reality.
//
// KILLS: any report drift regression -- Summary counts that disagree with the physical WAV census,
// succeeded rows pointing at files that were never written (dead paths), or rows missing for files
// that exist. Counts asserted against hand-computed literals for this exact 2-entry batch.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterUserFlowReportMatchesDiskTest,
	"AudioFactory.AudioExporter.UserFlows.ReportRowsMatchDisk",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterUserFlowReportMatchesDiskTest::RunTest(const FString& Parameters)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("NoSound")))
	{
		AddError(TEXT("User-flow report-vs-disk test is unavailable: this required live-audio flow was launched with -NoSound."));
		return false;
	}

	USoundBase* MetaSoundAsset = LoadObject<USoundBase>(nullptr, TEXT("/Game/MetaSoundOneShot.MetaSoundOneShot"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!TestNotNull(TEXT("Required fixture '/Game/MetaSoundOneShot.MetaSoundOneShot' should be present in this project."), MetaSoundAsset))
	{
		return false;
	}
	USoundBase* CueAsset = LoadObject<USoundBase>(nullptr, TEXT("/Game/SoundCueOneShot.SoundCueOneShot"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!TestNotNull(TEXT("Required fixture '/Game/SoundCueOneShot.SoundCueOneShot' should be present in this project."), CueAsset))
	{
		return false;
	}

	UWorld* World = AudioExporterUserFlowTestUtils::ResolveAutomationWorld();
	if (!TestNotNull(TEXT("The report-vs-disk user-flow test should have an editor world for the real runtime pipeline."), World))
	{
		return false;
	}

	UAudioExporterRuntimeSubsystem* RuntimeSubsystem = GEngine ? GEngine->GetEngineSubsystem<UAudioExporterRuntimeSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("The report-vs-disk user-flow test should resolve the AudioExporter runtime subsystem."), RuntimeSubsystem))
	{
		return false;
	}
	const TSharedPtr<bool> PreflightSucceeded = MakeShared<bool>(false);
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FWaitForUserFlowPreflightDrain(
		*this, RuntimeSubsystem, TEXT("UserFlows.ReportRowsMatchDisk"), PreflightSucceeded));

	TArray<FAudioExportSoundEntry> Entries;
	for (USoundBase* Sound : { MetaSoundAsset, CueAsset })
	{
		FAudioExportSoundEntry Entry;
		Entry.Sound = Sound;
		Entry.SoundName = Sound->GetName();
		Entry.SourceAssetPath = Sound->GetPackage()->GetName();
		Entry.RelativePath = TEXT("ReportCheck");
		Entries.Add(Entry);
	}

	const TSharedRef<AudioExporterUserFlowTestUtils::FUserFlowExportState> State =
		MakeShared<AudioExporterUserFlowTestUtils::FUserFlowExportState>();
	State->Test = this;
	State->TestName = TEXT("UserFlows.ReportRowsMatchDisk");
	const FString OutputRoot = AudioExporterUserFlowTestUtils::MakeOutputDirectory(TEXT("UserFlows_ReportMatchesDisk"));

	const FAudioExporterConfig Config = AudioExporterUserFlowTestUtils::MakeUserFlowConfig(/*MaxLengthSeconds=*/5);
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FStartUserFlowExportCommand(
		State, RuntimeSubsystem, World, Entries, Config, OutputRoot, /*bArmAttendedBranch=*/false, nullptr, PreflightSucceeded));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FWaitForUserFlowExportTerminal(State, AudioExporterUserFlowTestUtils::UserFlowBatchWaitSeconds));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FRunAfterUserFlowExport(State,
		[](FAutomationTestBase& Test, AudioExporterUserFlowTestUtils::FUserFlowExportState& ExportState)
		{
			if (!Test.TestTrue(TEXT("The batch export should receive a structured terminal result."), ExportState.bOutcomeReceived))
			{
				return;
			}
			Test.TestEqual(TEXT("The batch export should settle as Succeeded."), ExportState.Outcome, EAudioExportOutcome::Succeeded);
			Test.TestFalse(TEXT("The completed session path should not be empty."), ExportState.FinishedPath.IsEmpty());

			const FString ReportPath = ExportState.FinishedPath / TEXT("AudioExporterTextLog.txt");
			if (!Test.TestTrue(FString::Printf(TEXT("The generated text report should exist on disk ('%s')."), *ReportPath),
				IFileManager::Get().FileExists(*ReportPath)))
			{
				return;
			}

			FString ReportText;
			if (!Test.TestTrue(TEXT("The generated text report should load from disk."), FFileHelper::LoadFileToString(ReportText, *ReportPath)))
			{
				return;
			}

			TArray<FString> ReportLines;
			ReportText.ParseIntoArrayLines(ReportLines);

			// Hand-computed literals for THIS batch: 2 entries x 1 variation, all successful.
			int32 RequestedCount = -1;
			int32 SucceededCount = -1;
			int32 FailedCount = -1;
			int32 SkippedCount = -1;
			Test.TestTrue(TEXT("The report Summary should contain exactly one 'Requested' count line."),
				AudioExporterUserFlowTestUtils::ReadReportSummaryCount(ReportLines, TEXT("Requested"), RequestedCount));
			Test.TestTrue(TEXT("The report Summary should contain exactly one 'Succeeded' count line."),
				AudioExporterUserFlowTestUtils::ReadReportSummaryCount(ReportLines, TEXT("Succeeded"), SucceededCount));
			Test.TestTrue(TEXT("The report Summary should contain exactly one 'Failed' count line."),
				AudioExporterUserFlowTestUtils::ReadReportSummaryCount(ReportLines, TEXT("Failed"), FailedCount));
			Test.TestTrue(TEXT("The report Summary should contain exactly one 'Skipped' count line."),
				AudioExporterUserFlowTestUtils::ReadReportSummaryCount(ReportLines, TEXT("Skipped"), SkippedCount));

			Test.TestEqual(TEXT("The report 'Requested' count must equal the literal 2-row batch size."), RequestedCount, 2);
			Test.TestEqual(TEXT("The report 'Succeeded' count must equal the literal 2 for this clean batch."), SucceededCount, 2);
			Test.TestEqual(TEXT("The report 'Failed' count must equal the literal 0 for this clean batch."), FailedCount, 0);
			Test.TestEqual(TEXT("The report 'Skipped' count must equal the literal 0 for this clean batch."), SkippedCount, 0);

			// Every output row must reference a WAV that physically exists.
			TArray<FString> OutputRows;
			AudioExporterUserFlowTestUtils::CollectReportOutputRows(ReportText, OutputRows);
			if (!Test.TestEqual(FString::Printf(TEXT("The report must carry exactly one output row per succeeded file (found %d)."), OutputRows.Num()),
				OutputRows.Num(), SucceededCount))
			{
				return;
			}

			for (int32 RowIndex = 0; RowIndex < OutputRows.Num(); ++RowIndex)
			{
				const FString& RowPath = OutputRows[RowIndex];
				Test.TestTrue(FString::Printf(TEXT("Report output row #%d must reference a .wav file ('%s')."), RowIndex + 1, *RowPath),
					RowPath.EndsWith(TEXT(".wav"), ESearchCase::IgnoreCase));
				Test.TestTrue(FString::Printf(TEXT("Report output row #%d references '%s', which must physically exist on disk."),
					RowIndex + 1, *RowPath), !RowPath.IsEmpty() && IFileManager::Get().FileExists(*RowPath));
			}

			// Independent physical census of the session dir must equal the report's arithmetic:
			// succeeded == physical files (failed/skipped are 0 here).
			TArray<FString> PhysicalWavs;
			AudioExporterUserFlowTestUtils::FindWavFilesUnder(ExportState.FinishedPath, PhysicalWavs);
			Test.TestEqual(FString::Printf(TEXT("The physical WAV census (%d) must equal the report's succeeded count (%d)."),
				PhysicalWavs.Num(), SucceededCount), PhysicalWavs.Num(), SucceededCount);
		}));

	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterUserFlowTestUtils::FDeleteUserFlowOutputDirectoryCommand(OutputRoot, State));

	return true;
}
