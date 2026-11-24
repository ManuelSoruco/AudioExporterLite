// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "Misc/AutomationTest.h"

#include "Async/Async.h"
#include "AudioExporterBPLibrary.h"
#include "AudioExportPipeline.h"
#include "AudioExporterRuntimeSubsystem.h"
#include "AssetRegistry/AssetData.h"
#include "AudioDeviceManager.h"
#include "AudioMixerDevice.h"
#include "Components/AudioComponent.h"
#include "CoreGlobals.h"
#include "Containers/Ticker.h"
#include "Utilities/AudioExporterLogger.h"
#include "Utilities/AudioExporterSettings.h"
#include "Utilities/AudioExporterUtils.h"
#include "Utilities/AudioRendererRealtime.h"
#include "PCM/AudioPCMUtils.h"
#include "Tests/AudioExporterTestResultSink.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Parse.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "MetasoundSource.h"
#include "Modules/ModuleManager.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundEffectSource.h"
#include "Sound/SoundSubmix.h"
#include "Sound/SoundWave.h"
#include "Sound/SoundWaveProcedural.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#include <limits>

// Same tier selection as the renderer under test: on UE 5.6/5.7 the product's override tier is
// ECVF_SetByCode (the Unset trap tier); on 5.8+ it is ECVF_SetByTemp. Seeding the real engine
// cvar at any other tier is either ignored by the engine (lower priority, logged warning) or
// would outrank the product's own writes.
#include "Misc/EngineVersionComparison.h"
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
#define AF_TEST_CVAR_SETBY_TEMP ECVF_SetByTemp
#else
#define AF_TEST_CVAR_SETBY_TEMP ECVF_SetByCode
#endif

void FAudioExporterAutomationTail::Init(const FSoundEffectSourceInitData& InitData)
{
	bIsActive = true;
	OnPresetChanged();

	NumChannels = FMath::Max(1, InitData.NumSourceChannels);
	TailFramesTotal = FMath::Max(1, FMath::RoundToInt(InitData.SampleRate * RenderSettings.TailSeconds));
	TailFramesRemaining = 0;
	TailPhase = 0.0f;
	TailPhaseIncrement = 2.0f * PI * 440.0f / InitData.SampleRate;
	bSawSourceSignal = false;
}

void FAudioExporterAutomationTail::OnPresetChanged()
{
	GET_EFFECT_SETTINGS(AudioExporterAutomationTail);
	RenderSettings = Settings;
}

void FAudioExporterAutomationTail::ProcessAudio(const FSoundEffectSourceInputData& InData, float* OutAudioBufferData)
{
	if (!InData.InputSourceEffectBufferPtr || !OutAudioBufferData || NumChannels <= 0)
	{
		return;
	}

	bool bBlockHasSignal = false;
	for (int32 SampleIndex = 0; SampleIndex < InData.NumSamples; ++SampleIndex)
	{
		bBlockHasSignal |= FMath::Abs(InData.InputSourceEffectBufferPtr[SampleIndex]) > 1.0e-2f;
	}

	if (bBlockHasSignal)
	{
		bSawSourceSignal = true;
		TailFramesRemaining = TailFramesTotal;
		FMemory::Memcpy(OutAudioBufferData, InData.InputSourceEffectBufferPtr, InData.NumSamples * sizeof(float));
		const int32 NumFrames = FMath::DivideAndRoundUp(InData.NumSamples, NumChannels);
		TailPhase = FMath::Fmod(TailPhase + TailPhaseIncrement * static_cast<float>(NumFrames), 2.0f * PI);
		return;
	}
	for (int32 FrameStart = 0; FrameStart < InData.NumSamples; FrameStart += NumChannels)
	{
		float AddedTailSample = 0.0f;
		if (bSawSourceSignal && TailFramesRemaining > 0)
		{
			AddedTailSample = RenderSettings.TailAmplitude * FMath::Sin(TailPhase);
			--TailFramesRemaining;
		}
		else if (!bSawSourceSignal)
		{
			AddedTailSample = RenderSettings.PilotAmplitude * FMath::Sin(TailPhase);
		}

		TailPhase = FMath::Fmod(TailPhase + TailPhaseIncrement, 2.0f * PI);
		for (int32 ChannelIndex = 0; ChannelIndex < NumChannels && FrameStart + ChannelIndex < InData.NumSamples; ++ChannelIndex)
		{
			OutAudioBufferData[FrameStart + ChannelIndex] = AddedTailSample;
		}
	}

}

namespace AudioExporterRuntimeTestUtils
{
	// A cancellation request must settle the actual terminal result and final writer before the
	// verifier restores the authored SoundWave. Five seconds is a bounded test deadline, not a
	// production timeout.
	constexpr double RealPipelineCancellationDrainSeconds = 5.0;
	// A verifier must not strand the latent chain when a settled export lacks a measurable file.
	constexpr double ModifiedCaptureVerificationTimeoutSeconds = 30.0;

	static USoundWaveProcedural* MakeTransientToneWave(const TCHAR* Name, float DurationSeconds);
	static FAudioExportSoundEntry MakeFixtureEntry(USoundBase* Sound, const TCHAR* Name, const TCHAR* AssetPath);
	static FAudioExporterConfig MakeFailureFixtureConfig(int32 VariationCount);

	class FAudioExporterLogCapture final : public FOutputDevice
	{
	public:
		FAudioExporterLogCapture()
		{
			if (GLog)
			{
				GLog->AddOutputDevice(this);
			}
		}

		virtual ~FAudioExporterLogCapture() override
		{
			if (GLog)
			{
				GLog->RemoveOutputDevice(this);
			}
		}

		// Opting into unbuffered delivery so log lines land INLINE on the emitting thread,
		// matching the engine's own automation capture contract (Engine/Source/Runtime/Core/Public/
		// Misc/AutomationTest.h:1301-1304 "Make it unbuffered by returning true"; see also
		// HAL/OutputDevice.h:201-207). Unbuffered delivery means Serialize can be invoked from ANY
		// logging thread during the registration window, so all member access below is guarded by
		// CriticalSection -- the same internal-thread-safety discipline as the engine's
		// FAutomationTestOutputDevice (AutomationTest.cpp:210-246).
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			FScopeLock Lock(&Critical);
			if (Verbosity == ELogVerbosity::Warning
				|| Verbosity == ELogVerbosity::Error
				|| Verbosity == ELogVerbosity::Fatal)
			{
				++WarningOrErrorCount;
			}

			if (Category != FName(TEXT("LogAudioExporter")))
			{
				return;
			}

			Messages.Add(V ? V : TEXT(""));
		}

		int32 GetWarningOrErrorCount() const
		{
			FScopeLock Lock(&Critical);
			return WarningOrErrorCount;
		}

		bool Contains(const TCHAR* Needle) const
		{
			FScopeLock Lock(&Critical);
			for (const FString& Message : Messages)
			{
				if (Message.Contains(Needle))
				{
					return true;
				}
			}
			return false;
		}

	private:
		mutable FCriticalSection Critical;
		TArray<FString> Messages;
		int32 WarningOrErrorCount = 0;
	};

	struct FWavHeaderInfo
	{
		uint16 AudioFormat = 0;
		uint16 NumChannels = 0;
		uint32 SampleRate = 0;
		uint32 ByteRate = 0;
		uint16 BlockAlign = 0;
		uint16 BitsPerSample = 0;
		uint32 RiffChunkSize = 0;
		uint32 DataSize = 0;
		uint32 DataOffset = 44;
	};

	/** The one sanctioned way to (re)write a committed golden baseline. Never self-heals
	 *  without this explicit switch. */
	static bool IsRegenGoldensRequested()
	{
		return FParse::Param(FCommandLine::Get(), TEXT("RegenGoldens"));
	}

	/** Golden reports embed the fully-resolved absolute project path (via
	 *  FPaths::ConvertRelativePathToFull). That path is machine/drive-specific -- it differs between a
	 *  normal checkout and a `subst`-mounted drive pointed at the same tree -- so a byte-exact golden
	 *  compare would fail purely on the drive letter. This replaces the resolved project-root prefix
	 *  with a stable placeholder in both the freshly generated text and the committed golden text
	 *  before comparing, so the compare (and the committed baseline) are workspace-root independent
	 *  while every other byte, including case, is still asserted exactly.
	 */
	static FString NormalizeWorkspaceRootForGolden(FString InText)
	{
		// FPaths::ProjectDir() itself is often already relative-to-engine-binaries. Strip BOTH the
		// absolute-resolved form and the raw relative-with-dots form so the compare is
		// workspace-root independent regardless of which one a given field used.
		FString AbsoluteRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		AbsoluteRoot.RemoveFromEnd(TEXT("/"));
		InText.ReplaceInline(*AbsoluteRoot, TEXT("<WORKSPACE_ROOT>"), ESearchCase::CaseSensitive);

		FString RawRoot = FPaths::ProjectDir();
		RawRoot.RemoveFromEnd(TEXT("/"));
		InText.ReplaceInline(*RawRoot, TEXT("<WORKSPACE_ROOT>"), ESearchCase::CaseSensitive);

		return InText;
	}

	static uint16 ReadUInt16LE(const TArray<uint8>& Bytes, int32 Offset)
	{
		return static_cast<uint16>(Bytes[Offset] | (Bytes[Offset + 1] << 8));
	}

	static uint32 ReadUInt32LE(const TArray<uint8>& Bytes, int32 Offset)
	{
		return static_cast<uint32>(Bytes[Offset])
			| (static_cast<uint32>(Bytes[Offset + 1]) << 8)
			| (static_cast<uint32>(Bytes[Offset + 2]) << 16)
			| (static_cast<uint32>(Bytes[Offset + 3]) << 24);
	}

	static bool ParseWaveHeader(const TArray<uint8>& WavData, FWavHeaderInfo& OutHeader)
	{
		if (WavData.Num() < 44)
		{
			return false;
		}

		if (FMemory::Memcmp(WavData.GetData(), "RIFF", 4) != 0 || FMemory::Memcmp(WavData.GetData() + 8, "WAVE", 4) != 0)
		{
			return false;
		}
		if (FMemory::Memcmp(WavData.GetData() + 12, "fmt ", 4) != 0
			|| FMemory::Memcmp(WavData.GetData() + 36, "data", 4) != 0)
		{
			return false;
		}

		OutHeader.RiffChunkSize = ReadUInt32LE(WavData, 4);
		OutHeader.AudioFormat = ReadUInt16LE(WavData, 20);
		OutHeader.NumChannels = ReadUInt16LE(WavData, 22);
		OutHeader.SampleRate = ReadUInt32LE(WavData, 24);
		OutHeader.ByteRate = ReadUInt32LE(WavData, 28);
		OutHeader.BlockAlign = ReadUInt16LE(WavData, 32);
		OutHeader.BitsPerSample = ReadUInt16LE(WavData, 34);
		OutHeader.DataSize = ReadUInt32LE(WavData, 40);
		const uint64 RiffEnd = static_cast<uint64>(OutHeader.RiffChunkSize) + 8u;
		const uint64 DataEnd = static_cast<uint64>(OutHeader.DataOffset) + static_cast<uint64>(OutHeader.DataSize);
		return (OutHeader.AudioFormat == 1 || OutHeader.AudioFormat == 3)
			&& RiffEnd <= static_cast<uint64>(WavData.Num())
			&& DataEnd <= static_cast<uint64>(WavData.Num());
	}

	static int16 ReadFirstPcmSample(const TArray<uint8>& WavData)
	{
		return static_cast<int16>(ReadUInt16LE(WavData, 44));
	}

	static int16 ReadPcmSample(const TArray<uint8>& WavData, int32 SampleIndex)
	{
		return static_cast<int16>(ReadUInt16LE(WavData, 44 + (SampleIndex * 2)));
	}

	static int16 ReadMaxAbsPcmSample(const TArray<uint8>& WavData)
	{
		int32 MaxSample = 0;
		const int32 SampleCount = FMath::Max(0, WavData.Num() - 44) / sizeof(int16);
		for (int32 SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
		{
			MaxSample = FMath::Max(MaxSample, FMath::Abs(static_cast<int32>(ReadPcmSample(WavData, SampleIndex))));
		}
		return static_cast<int16>(MaxSample);
	}

	static FString MakeOutputDirectory(const FString& TestName)
	{
		const FString OutputDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("RealPipeline"), TEXT("AudioExporter"), TestName);
		IFileManager::Get().DeleteDirectory(*OutputDirectory, false, true);
		IFileManager::Get().MakeDirectory(*OutputDirectory, true);
		return OutputDirectory;
	}

	static TArray<float> MakeSineBuffer(int32 NumFrames, int32 NumChannels, int32 SampleRate, float Frequency, float Amplitude)
	{
		TArray<float> Buffer;
		Buffer.SetNumUninitialized(NumFrames * NumChannels);
		for (int32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
		{
			const float Sample = Amplitude * FMath::Sin(2.0f * PI * Frequency * static_cast<float>(FrameIndex) / static_cast<float>(SampleRate));
			for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
			{
				Buffer[(FrameIndex * NumChannels) + ChannelIndex] = Sample;
			}
		}
		return Buffer;
	}

	static bool HasNonSilentPcmData(const TArray<uint8>& WavData)
	{
		for (int32 Index = 44; Index < WavData.Num(); ++Index)
		{
			if (WavData[Index] != 0)
			{
				return true;
			}
		}
		return false;
	}

	static bool ValidateWavOutput(FAutomationTestBase& Test, const FString& FilePath, const FString& Label)
	{
		Test.TestTrue(FString::Printf(TEXT("%s WAV should exist on disk."), *Label), IFileManager::Get().FileExists(*FilePath));

		TArray<uint8> WavData;
		if (!Test.TestTrue(FString::Printf(TEXT("%s WAV should load from disk."), *Label), FFileHelper::LoadFileToArray(WavData, *FilePath)))
		{
			return false;
		}

		FWavHeaderInfo Header;
		if (!Test.TestTrue(FString::Printf(TEXT("%s WAV header should parse as a supported PCM or IEEE-float RIFF/WAVE file."), *Label),
			ParseWaveHeader(WavData, Header)))
		{
			return false;
		}
		const bool bSupportedFormat = Header.AudioFormat == 1 || Header.AudioFormat == 3;
		Test.TestTrue(FString::Printf(TEXT("%s WAV must use supported PCM (1) or IEEE float (3) encoding; got %u."), *Label, Header.AudioFormat), bSupportedFormat);
		Test.TestTrue(FString::Printf(TEXT("%s WAV must report a valid observed channel count."), *Label), Header.NumChannels > 0);
		Test.TestTrue(FString::Printf(TEXT("%s WAV must report a valid observed sample rate."), *Label), Header.SampleRate > 0);
		const bool bSupportedBitDepth = (Header.AudioFormat == 1 && (Header.BitsPerSample == 8 || Header.BitsPerSample == 16 || Header.BitsPerSample == 24 || Header.BitsPerSample == 32))
			|| (Header.AudioFormat == 3 && Header.BitsPerSample == 32);
		Test.TestTrue(FString::Printf(TEXT("%s WAV bit depth must match its encoding (PCM 8/16/24/32 or float 32); got format %u, depth %u."), *Label, Header.AudioFormat, Header.BitsPerSample), bSupportedBitDepth);
		const uint32 BytesPerSample = static_cast<uint32>(Header.BitsPerSample / 8u);
		const uint32 ExpectedBlockAlign = static_cast<uint32>(Header.NumChannels) * BytesPerSample;
		const uint64 ExpectedByteRate = static_cast<uint64>(Header.SampleRate) * static_cast<uint64>(ExpectedBlockAlign);
		const bool bFrameLayoutValid = ExpectedBlockAlign > 0
			&& ExpectedBlockAlign <= std::numeric_limits<uint16>::max()
			&& Header.BlockAlign == static_cast<uint16>(ExpectedBlockAlign);
		Test.TestTrue(FString::Printf(TEXT("%s WAV block alignment must match its observed channels and encoding."), *Label), bFrameLayoutValid);
		Test.TestTrue(FString::Printf(TEXT("%s WAV byte rate must match its observed sample rate and frame alignment."), *Label),
			ExpectedByteRate <= std::numeric_limits<uint32>::max() && Header.ByteRate == static_cast<uint32>(ExpectedByteRate));
		Test.TestTrue(FString::Printf(TEXT("%s WAV data chunk must be nonempty."), *Label), Header.DataSize > 0);
		Test.TestTrue(FString::Printf(TEXT("%s WAV data chunk must be frame-aligned for its observed channel count and encoding."), *Label),
			ExpectedBlockAlign > 0 && Header.DataSize % ExpectedBlockAlign == 0);
		Test.TestTrue(FString::Printf(TEXT("%s WAV data chunk must fit inside the file."), *Label),
			static_cast<uint64>(Header.DataOffset) + static_cast<uint64>(Header.DataSize) <= static_cast<uint64>(WavData.Num()));
		const bool bHasAudibleSamples = HasNonSilentPcmData(WavData);
		Test.TestTrue(FString::Printf(TEXT("%s WAV should contain non-silent PCM samples."), *Label), bHasAudibleSamples);
		return bSupportedFormat && Header.NumChannels > 0 && Header.SampleRate > 0 && bSupportedBitDepth
			&& ExpectedBlockAlign > 0 && Header.BlockAlign == ExpectedBlockAlign
			&& ExpectedByteRate <= std::numeric_limits<uint32>::max() && Header.ByteRate == static_cast<uint32>(ExpectedByteRate)
			&& Header.DataSize > 0 && Header.DataSize % ExpectedBlockAlign == 0
			&& static_cast<uint64>(Header.DataOffset) + static_cast<uint64>(Header.DataSize) <= static_cast<uint64>(WavData.Num())
			&& bHasAudibleSamples;
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

	struct FRetainedTestCleanupRecord
	{
		TFunction<bool()> IsReady;
		TFunction<void()> Release;
		TFunction<bool()> ReleaseForShutdown;
		bool bReleased = false;
	};

	class FTestCleanupRegistry final
	{
	public:
		void Register(TFunction<bool()> InIsReady, TFunction<void()> InRelease, TFunction<bool()> InReleaseForShutdown)
		{
			EnsureEngineHooks();
			TSharedRef<FRetainedTestCleanupRecord> Record = MakeShared<FRetainedTestCleanupRecord>();
			Record->IsReady = MoveTemp(InIsReady);
			Record->Release = MoveTemp(InRelease);
			Record->ReleaseForShutdown = MoveTemp(InReleaseForShutdown);
			Records.Add(MoveTemp(Record));
		}

	private:
		void EnsureEngineHooks()
		{
			if (!TickerHandle.IsValid())
			{
				TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FTestCleanupRegistry::Tick));
			}
			if (!ShutdownHandle.IsValid())
			{
				ShutdownHandle = FCoreDelegates::OnEnginePreExit.AddRaw(this, &FTestCleanupRegistry::HandleEnginePreExit);
			}
		}

		bool Tick(float)
		{
			for (int32 Index = Records.Num() - 1; Index >= 0; --Index)
			{
				const TSharedRef<FRetainedTestCleanupRecord>& Record = Records[Index];
				if (!Record->bReleased && Record->IsReady && Record->IsReady())
				{
					Record->Release();
					Record->bReleased = true;
					Records.RemoveAt(Index);
				}
			}
			if (Records.IsEmpty())
			{
				TickerHandle = FTSTicker::FDelegateHandle();
				return false;
			}
			return true;
		}

		void HandleEnginePreExit()
		{
			for (int32 Index = Records.Num() - 1; Index >= 0; --Index)
			{
				const TSharedRef<FRetainedTestCleanupRecord>& Record = Records[Index];
				if (!Record->bReleased && Record->ReleaseForShutdown && Record->ReleaseForShutdown())
				{
					Record->bReleased = true;
					Records.RemoveAt(Index);
				}
			}
			if (Records.IsEmpty())
			{
				TickerHandle = FTSTicker::FDelegateHandle();
			}
		}

		TArray<TSharedRef<FRetainedTestCleanupRecord>> Records;
		FTSTicker::FDelegateHandle TickerHandle;
		FDelegateHandle ShutdownHandle;
	};

	static FTestCleanupRegistry& GetTestCleanupRegistry()
	{
		static FTestCleanupRegistry Registry;
		return Registry;
	}

	static void RegisterRetainedTestCleanup(TFunction<bool()> InIsReady, TFunction<void()> InRelease, TFunction<bool()> InReleaseForShutdown)
	{
		GetTestCleanupRegistry().Register(MoveTemp(InIsReady), MoveTemp(InRelease), MoveTemp(InReleaseForShutdown));
	}

	struct FRealPipelineExportState
	{
		FAutomationTestBase* Test = nullptr;
		UAudioExporterRuntimeSubsystem* RuntimeSubsystem = nullptr;
		FDelegateHandle CompleteHandle;
		FDelegateHandle ResultHandle;
		FDelegateHandle ProgressHandle;
		FString TestName;
		FString OutputRoot;
		FString FinishedPath;
		int32 TerminalEvents = 0;
		int32 StructuredTerminalEvents = 0;
		int32 ProgressEvents = 0;
		FAudioExportProgressInfo LastProgress;
		FAudioExportResult TerminalResult;
		double StartSeconds = 0.0;
		bool bFinished = false;
		TObjectPtr<USoundWave> ContractWave = nullptr;
		TObjectPtr<USoundEffectSourcePresetChain> OriginalSourceEffectChain = nullptr;
		TObjectPtr<USoundEffectSourcePresetChain> ContractSourceEffectChain = nullptr;
		float OriginalVolume = 1.0f;
		float OriginalPitch = 1.0f;
		float ContractPitch = 2.0f;
		float ContractSourceEffectDelaySeconds = 0.25f;
		bool bRestoreContractWave = false;
		bool bContractWaveWasRooted = false;
		bool bContractWaveRootAdded = false;
		bool bSoundWaveContractAssertions = false;
		bool bTerminalSettled = false;
		bool bTestChainSettled = false;
		bool bWriterDrainConfirmed = false;
		bool bWaitTimedOut = false;
		bool bPreserveLiveObjects = false;
		bool bDeferredCleanupScheduled = false;
		bool bCleanupComplete = false;
		bool bCancellationRequested = false;
		double CancellationDeadlineSeconds = 0.0;
		bool bPreflightPending = false;
		bool bPreflightFailed = false;
		int32 PreflightTerminalEvents = 0;
		int32 PreflightStructuredTerminalEvents = 0;
		FAudioExportResult PreflightResult;
		FDelegateHandle PreflightCompleteHandle;
		FDelegateHandle PreflightResultHandle;
		double PreflightDeadlineSeconds = 0.0;
		uint16 MeasuredNumChannels = 0;
		uint32 MeasuredSampleRate = 0;
		double MeasuredDurationSeconds = 0.0;
		double MeasuredNormalizedRms = 0.0;
		double MeasuredFrequencyHz = 0.0;
		double MeasuredWindowRms = 0.0;
		double MeasuredWindowFrequencyHz = 0.0;
		int32 MeasuredWindowFrames = 0;
		int32 MeasuredFrameCount = 0;
		int32 MeasuredFirstAudibleFrame = INDEX_NONE;
		int32 MeasuredLastAudibleFrame = INDEX_NONE;
		bool bMeasuredWav = false;
	};

	static void RemoveRealPipelineDelegates(FRealPipelineExportState& State)
	{
		if (State.RuntimeSubsystem && State.CompleteHandle.IsValid())
		{
			State.RuntimeSubsystem->OnExportCompleteNative.Remove(State.CompleteHandle);
			State.CompleteHandle = FDelegateHandle();
		}
		if (State.RuntimeSubsystem && State.ProgressHandle.IsValid())
		{
			State.RuntimeSubsystem->OnExportProgressNative.Remove(State.ProgressHandle);
			State.ProgressHandle = FDelegateHandle();
		}
		if (State.RuntimeSubsystem && State.ResultHandle.IsValid())
		{
			State.RuntimeSubsystem->OnExportCompleteResultNative.Remove(State.ResultHandle);
			State.ResultHandle = FDelegateHandle();
		}
	}

	static void RestoreRealPipelineSoundWave(FRealPipelineExportState& State)
	{
		if (!State.bRestoreContractWave || !State.ContractWave)
		{
			return;
		}

		State.ContractWave->Volume = State.OriginalVolume;
		State.ContractWave->Pitch = State.OriginalPitch;
		State.ContractWave->SourceEffectChain = State.OriginalSourceEffectChain;
		if (State.bContractWaveRootAdded && !State.bContractWaveWasRooted)
		{
			State.ContractWave->RemoveFromRoot();
		}
		State.bContractWaveRootAdded = false;
		State.bRestoreContractWave = false;
	}

	static void ReleaseRealPipelineOutputResources(FRealPipelineExportState& State)
	{
		State.FinishedPath.Reset();
		State.OutputRoot.Reset();
	}

	static bool HasConfirmedRealPipelineDrain(const FRealPipelineExportState& State)
	{
		return State.bTerminalSettled
			&& State.bWriterDrainConfirmed
			&& State.RuntimeSubsystem
			&& !State.RuntimeSubsystem->IsExporting()
			&& State.RuntimeSubsystem->GetActiveAsyncExportsForTesting() == 0;
	}

	static void CleanupRealPipelineAfterConfirmedDrain(FRealPipelineExportState& State)
	{
		if (State.bCleanupComplete || State.bPreserveLiveObjects || !HasConfirmedRealPipelineDrain(State))
		{
			return;
		}

		RemoveRealPipelineDelegates(State);
		RestoreRealPipelineSoundWave(State);
		State.bCleanupComplete = true;
	}

	struct FRealPipelineDeferredCleanupOwner
	{
		FRealPipelineDeferredCleanupOwner(const TSharedRef<FRealPipelineExportState>& InState, UAudioExporterRuntimeSubsystem* InRuntimeSubsystem)
			: State(InState)
			, RuntimeSubsystem(InRuntimeSubsystem)
		{
		}

		TSharedRef<FRealPipelineExportState> State;
		TWeakObjectPtr<UAudioExporterRuntimeSubsystem> RuntimeSubsystem;
	};

	static void ScheduleRealPipelineDeferredCleanup(const TSharedRef<FRealPipelineExportState>& State)
	{
		const TSharedRef<FRealPipelineDeferredCleanupOwner> DeferredOwner = MakeShared<FRealPipelineDeferredCleanupOwner>(State, State->RuntimeSubsystem);
		const double CleanupDeadlineSeconds = FPlatformTime::Seconds() + RealPipelineCancellationDrainSeconds;
		bool bMissingTerminalReported = false;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([DeferredOwner, CleanupDeadlineSeconds, bMissingTerminalReported](float) mutable
		{
			if (DeferredOwner->State->bCleanupComplete)
			{
				return false;
			}

			UAudioExporterRuntimeSubsystem* RuntimeSubsystem = DeferredOwner->RuntimeSubsystem.Get();
			if (!RuntimeSubsystem)
			{
				// Engine-subsystem destruction is the shutdown terminal state: its delegates and writer
				// bookkeeping no longer exist, so release this test owner without dereferencing a stale UObject.
				DeferredOwner->State->CompleteHandle = FDelegateHandle();
				DeferredOwner->State->ProgressHandle = FDelegateHandle();
				DeferredOwner->State->ResultHandle = FDelegateHandle();
				DeferredOwner->State->RuntimeSubsystem = nullptr;
				RestoreRealPipelineSoundWave(*DeferredOwner->State);
				ReleaseRealPipelineOutputResources(*DeferredOwner->State);
				DeferredOwner->State->bCleanupComplete = true;
				return false;
			}
			if (FPlatformTime::Seconds() > CleanupDeadlineSeconds)
			{
				if (DeferredOwner->State->Test)
				{
					DeferredOwner->State->Test->AddError(FString::Printf(TEXT("%s deferred cleanup exceeded the named writer-drain deadline; live objects remain preserved for safety."), *DeferredOwner->State->TestName));
				}
				AudioExporterRuntimeTestUtils::RegisterRetainedTestCleanup(
					[DeferredOwner]()
					{
						UAudioExporterRuntimeSubsystem* RuntimeSubsystem = DeferredOwner->RuntimeSubsystem.Get();
						return DeferredOwner->State->StructuredTerminalEvents > 0
							&& RuntimeSubsystem
							&& !RuntimeSubsystem->IsExporting()
							&& RuntimeSubsystem->GetActiveAsyncExportsForTesting() == 0;
					},
					[DeferredOwner]()
					{
						RemoveRealPipelineDelegates(*DeferredOwner->State);
						RestoreRealPipelineSoundWave(*DeferredOwner->State);
						ReleaseRealPipelineOutputResources(*DeferredOwner->State);
						DeferredOwner->State->bCleanupComplete = true;
					},
					[DeferredOwner]()
					{
						UAudioExporterRuntimeSubsystem* RuntimeSubsystem = DeferredOwner->RuntimeSubsystem.Get();
						if (RuntimeSubsystem && (RuntimeSubsystem->IsExporting() || RuntimeSubsystem->GetActiveAsyncExportsForTesting() != 0))
						{
							if (DeferredOwner->State->Test)
							{
								DeferredOwner->State->Test->AddError(FString::Printf(TEXT("%s retained live realtime export state during engine shutdown because the pipeline was still active or had async writers."), *DeferredOwner->State->TestName));
							}
							return false;
						}
						if (!DeferredOwner->State->StructuredTerminalEvents && DeferredOwner->State->Test)
						{
							DeferredOwner->State->Test->AddError(FString::Printf(TEXT("%s reached engine shutdown without a structured terminal result; releasing only after shutdown made the backend inactive."), *DeferredOwner->State->TestName));
						}
						if (!RuntimeSubsystem)
						{
							DeferredOwner->State->CompleteHandle = FDelegateHandle();
							DeferredOwner->State->ProgressHandle = FDelegateHandle();
							DeferredOwner->State->ResultHandle = FDelegateHandle();
							DeferredOwner->State->RuntimeSubsystem = nullptr;
						}
						else
						{
							RemoveRealPipelineDelegates(*DeferredOwner->State);
						}
						RestoreRealPipelineSoundWave(*DeferredOwner->State);
						ReleaseRealPipelineOutputResources(*DeferredOwner->State);
						DeferredOwner->State->bCleanupComplete = true;
						return true;
					});
				return false;
			}

			const bool bPipelineInactive = !RuntimeSubsystem->IsExporting();
			const int32 ActiveAsyncExports = RuntimeSubsystem->GetActiveAsyncExportsForTesting();
			const bool bStructuredTerminalDelivered = DeferredOwner->State->StructuredTerminalEvents > 0;
			if (bPipelineInactive && ActiveAsyncExports == 0 && !bStructuredTerminalDelivered)
			{
				if (!bMissingTerminalReported && DeferredOwner->State->Test)
				{
					DeferredOwner->State->Test->AddError(FString::Printf(TEXT("%s became inactive with zero async writers before its structured terminal result was delivered; retaining the terminal-listener cleanup path."), *DeferredOwner->State->TestName));
					bMissingTerminalReported = true;
				}
				return true;
			}
			if (bPipelineInactive && ActiveAsyncExports == 0 && bStructuredTerminalDelivered)
			{
				RemoveRealPipelineDelegates(*DeferredOwner->State);
				RestoreRealPipelineSoundWave(*DeferredOwner->State);
				ReleaseRealPipelineOutputResources(*DeferredOwner->State);
				DeferredOwner->State->bCleanupComplete = true;
				return false;
			}

			return true;
		}));
	}

	static void FindRealPipelineWavFiles(const FRealPipelineExportState& State, TArray<FString>& OutWavFiles)
	{
		OutWavFiles.Reset();

		if (!State.FinishedPath.IsEmpty())
		{
			FString FinishedPathFull = FPaths::ConvertRelativePathToFull(State.FinishedPath);
			FPaths::NormalizeDirectoryName(FinishedPathFull);
			IFileManager::Get().FindFilesRecursive(OutWavFiles, *FinishedPathFull, TEXT("*.wav"), true, false);
		}

		if (OutWavFiles.IsEmpty() && !State.OutputRoot.IsEmpty())
		{
			FString OutputRootFull = FPaths::ConvertRelativePathToFull(State.OutputRoot);
			FPaths::NormalizeDirectoryName(OutputRootFull);
			IFileManager::Get().FindFilesRecursive(OutWavFiles, *OutputRootFull, TEXT("*.wav"), true, false);
		}
	}

	static USoundEffectSourcePresetChain* MakeRealtimeContractSourceEffectChain(FAutomationTestBase& Test)
	{
		// This preset is a real FSoundEffectSource instantiated by the engine. Its delay buffer starts
		// with a bounded pilot tone so UE's sticky envelope-tail detector cannot declare the tail complete
		// before the source's first decoded block arrives. It passes authored audio at unity, then emits
		// a literal 250 ms bounded tail when the source input becomes silent. The produced WAV must
		// contain that real audio-thread output after the dry source boundary.
		UAudioExporterAutomationTailPreset* Preset =
			NewObject<UAudioExporterAutomationTailPreset>(GetTransientPackage());
		if (!Preset)
		{
			Test.AddError(TEXT("The realtime SoundWave contract test could not create its deterministic source-effect preset."));
			return nullptr;
		}
		Preset->Init();

		USoundEffectSourcePresetChain* Chain = NewObject<USoundEffectSourcePresetChain>(GetTransientPackage());
		if (!Chain)
		{
			Test.AddError(TEXT("The realtime SoundWave contract test could not create its source-effect chain."));
			return nullptr;
		}
		FSourceEffectChainEntry Entry;
		Entry.Preset = Preset;
		Entry.bBypass = false;
		Chain->Chain.Add(Entry);
		Chain->bPlayEffectChainTails = true;
		return Chain;
	}

	class FWaitForAudioExporterRealPipelineExport : public IAutomationLatentCommand
	{
	public:
		explicit FWaitForAudioExporterRealPipelineExport(const TSharedRef<FRealPipelineExportState>& InState, const double InTimeoutSeconds = 30.0)
			: State(InState)
			, TimeoutSeconds(InTimeoutSeconds)
		{
		}

		virtual bool Update() override
		{
			if (State->bPreflightFailed)
			{
				State->bTestChainSettled = true;
				return true;
			}
			if (State->bPreflightPending)
			{
				return false;
			}
			const bool bExportSettled = State->RuntimeSubsystem && !State->RuntimeSubsystem->IsExporting();
			const int32 ActiveAsyncExports = State->RuntimeSubsystem
				? State->RuntimeSubsystem->GetActiveAsyncExportsForTesting()
				: -1;
			if (State->bFinished
				&& State->StructuredTerminalEvents > 0
				&& bExportSettled
				&& ActiveAsyncExports == 0)
			{
				State->bTerminalSettled = true;
				State->bTestChainSettled = true;
				State->bWriterDrainConfirmed = true;
				return true;
			}

			const double NowSeconds = FPlatformTime::Seconds();
			if (!State->bCancellationRequested && (NowSeconds - State->StartSeconds) > TimeoutSeconds)
			{
				if (State->Test)
				{
					State->Test->AddError(FString::Printf(TEXT("%s real AudioExporter pipeline export timed out; requesting cancellation and waiting for terminal writer shutdown."), *State->TestName));
				}
				State->bCancellationRequested = true;
				State->CancellationDeadlineSeconds = NowSeconds + RealPipelineCancellationDrainSeconds;
				if (State->RuntimeSubsystem && State->RuntimeSubsystem->IsExporting())
				{
					State->RuntimeSubsystem->CancelExport(false);
				}
				return false;
			}

			if (State->bCancellationRequested && NowSeconds > State->CancellationDeadlineSeconds)
			{
				State->bWaitTimedOut = true;
				State->bTestChainSettled = true;
				State->bPreserveLiveObjects = true;
				if (!State->bDeferredCleanupScheduled)
				{
					ScheduleRealPipelineDeferredCleanup(State);
					State->bDeferredCleanupScheduled = true;
				}
				if (State->Test)
				{
					State->Test->AddError(FString::Printf(TEXT("%s did not settle its terminal result and writer within the bounded cancellation drain deadline."), *State->TestName));
				}
				return true;
			}

			return false;
		}

	private:
		TSharedRef<FRealPipelineExportState> State;
		double TimeoutSeconds = 30.0;
	};

	class FVerifyAudioExporterRealPipelineExport : public IAutomationLatentCommand
	{
	public:
		explicit FVerifyAudioExporterRealPipelineExport(const TSharedRef<FRealPipelineExportState>& InState)
			: State(InState)
		{
		}

		virtual bool Update() override
		{
			FAutomationTestBase* Test = State->Test;
			if (!Test)
			{
				return true;
			}
			if (!State->bWriterDrainConfirmed || State->bWaitTimedOut)
			{
				Test->AddError(FString::Printf(TEXT("%s verification was reached without confirmed structured terminal delivery and writer inactivity."), *State->TestName));
				return true;
			}
			if (!State->bTerminalSettled && State->RuntimeSubsystem && State->RuntimeSubsystem->IsExporting())
			{
				Test->AddError(FString::Printf(TEXT("%s verification was reached before terminal writer shutdown."), *State->TestName));
				return true;
			}
			if (!State->bTerminalSettled || !State->bWriterDrainConfirmed || State->bWaitTimedOut)
			{
				Test->AddError(FString::Printf(TEXT("%s did not produce a settled terminal result within its bounded wait."), *State->TestName));
				return true;
			}
			const int32 ActiveAsyncExports = State->RuntimeSubsystem
				? State->RuntimeSubsystem->GetActiveAsyncExportsForTesting()
				: -1;
			if (!State->RuntimeSubsystem || State->RuntimeSubsystem->IsExporting()
				|| !Test->TestEqual(FString::Printf(TEXT("%s must confirm zero active async exports before cleanup."), *State->TestName), ActiveAsyncExports, 0))
			{
				Test->AddError(FString::Printf(TEXT("%s cleanup was blocked because the realtime writer drain was not confirmed."), *State->TestName));
				return true;
			}

			if (State->RuntimeSubsystem && State->CompleteHandle.IsValid())
			{
				State->RuntimeSubsystem->OnExportCompleteNative.Remove(State->CompleteHandle);
			}
			if (State->RuntimeSubsystem && State->ProgressHandle.IsValid())
			{
				State->RuntimeSubsystem->OnExportProgressNative.Remove(State->ProgressHandle);
			}
			if (State->RuntimeSubsystem && State->ResultHandle.IsValid())
			{
				State->RuntimeSubsystem->OnExportCompleteResultNative.Remove(State->ResultHandle);
			}

			Test->TestEqual(FString::Printf(TEXT("%s should emit exactly one completion delegate."), *State->TestName), State->TerminalEvents, 1);
			Test->TestEqual(FString::Printf(TEXT("%s should emit exactly one structured completion result."), *State->TestName), State->StructuredTerminalEvents, 1);
			Test->TestEqual(FString::Printf(TEXT("%s structured outcome must be Succeeded."), *State->TestName), State->TerminalResult.Outcome, EAudioExportOutcome::Succeeded);
			Test->TestEqual(FString::Printf(TEXT("%s should report exactly one requested successful variation."), *State->TestName), State->TerminalResult.SuccessfulCount, 1);
			Test->TestEqual(FString::Printf(TEXT("%s should report zero failed variations."), *State->TestName), State->TerminalResult.FailedCount, 0);
			Test->TestEqual(FString::Printf(TEXT("%s should report zero skipped variations."), *State->TestName), State->TerminalResult.SkippedCount, 0);
			Test->TestEqual(FString::Printf(TEXT("%s terminal counts must conserve the literal requested total of one."), *State->TestName),
				State->TerminalResult.SuccessfulCount + State->TerminalResult.FailedCount + State->TerminalResult.SkippedCount, 1);
			Test->TestFalse(FString::Printf(TEXT("%s completion path should not be empty."), *State->TestName), State->FinishedPath.IsEmpty());
			Test->TestTrue(FString::Printf(TEXT("%s should emit progress before completion."), *State->TestName), State->ProgressEvents > 0);
			FString FinishedPathFull = FPaths::ConvertRelativePathToFull(State->FinishedPath);
			FPaths::NormalizeDirectoryName(FinishedPathFull);
			Test->TestTrue(FString::Printf(TEXT("%s completion directory should exist."), *State->TestName), IFileManager::Get().DirectoryExists(*FinishedPathFull));

			TArray<FString> WavFiles;
			FindRealPipelineWavFiles(*State, WavFiles);
			if (!Test->TestEqual(FString::Printf(TEXT("%s should produce exactly one WAV through the runtime pipeline."), *State->TestName), WavFiles.Num(), 1))
			{
				return true;
			}
			ValidateWavOutput(*Test, WavFiles[0], State->TestName);

			TArray<FString> TextLogFiles;
			IFileManager::Get().FindFilesRecursive(TextLogFiles, *FinishedPathFull, TEXT("AudioExporterTextLog.txt"), true, false);
			const FString TextLogPath = !TextLogFiles.IsEmpty()
				? TextLogFiles[0]
				: FPaths::Combine(FinishedPathFull, TEXT("AudioExporterTextLog.txt"));
			FString TextLog;
			Test->TestTrue(FString::Printf(TEXT("%s should write the export text log."), *State->TestName), FFileHelper::LoadFileToString(TextLog, *TextLogPath));
			Test->TestTrue(FString::Printf(TEXT("%s text log should include a source asset section."), *State->TestName), TextLog.Contains(TEXT("Source Asset Path")));
			return true;
		}

	private:
		TSharedRef<FRealPipelineExportState> State;
	};

	/**
	 * Independent verifier for the true SoundWave E2E. Unlike the tone-fabricating
	 * SimulateVariationFinishedForTesting seam, every expectation here is computed IN THIS CLASS from
	 * the produced file's raw bytes, against literals derived from the documented fixture identity and
	 * the canonical export file structure the house tests already pin, never from production code:
	 *  - exactly ONE wav: a static SoundWave source settles in exactly one variation (one-file-per-asset);
	 *  - header: RIFF/WAVE PCM with a valid observed channel count and sample rate / 16-bit;
	 *  - duration > 0, computed as frames / the parsed realtime sample rate from DataSize alone;
	 *  - RMS of channel 0 in (0.01, 1.0]: audible signal and int16 full-scale headroom;
	 *  - zero crossings are counted on channel 0, never imported from
	 *    production values.
	 */
	class FVerifySoundWaveRealPipelineExport : public IAutomationLatentCommand
	{
	public:
		explicit FVerifySoundWaveRealPipelineExport(const TSharedRef<FRealPipelineExportState>& InState)
			: State(InState)
		{
		}

		virtual bool Update() override
		{
			FAutomationTestBase* Test = State->Test;
			if (!Test)
			{
				return true;
			}
			const bool bTerminalDelivered = State->bFinished
				&& State->StructuredTerminalEvents > 0
				&& State->bTerminalSettled;
			const bool bPipelineInactive = State->RuntimeSubsystem
				&& !State->RuntimeSubsystem->IsExporting();
			const int32 ActiveAsyncExports = State->RuntimeSubsystem
				? State->RuntimeSubsystem->GetActiveAsyncExportsForTesting()
				: -1;
			if (!bTerminalDelivered || !State->bWriterDrainConfirmed || State->bWaitTimedOut)
			{
				Test->AddError(FString::Printf(TEXT("%s did not produce a settled terminal result within its bounded wait."), *State->TestName));
				return true;
			}
			Test->TestFalse(FString::Printf(TEXT("%s should be inactive before SoundWave cleanup."), *State->TestName), !bPipelineInactive);
			Test->TestEqual(FString::Printf(TEXT("%s must confirm zero active async exports before SoundWave cleanup."), *State->TestName), ActiveAsyncExports, 0);
			if (!bPipelineInactive || ActiveAsyncExports != 0)
			{
				Test->AddError(FString::Printf(TEXT("%s SoundWave cleanup was blocked because the realtime writer drain was not confirmed."), *State->TestName));
				return true;
			}

			CleanupRealPipelineAfterConfirmedDrain(*State);

			const FString Label = State->TestName;
			Test->TestEqual(FString::Printf(TEXT("%s should emit exactly one completion delegate."), *Label), State->TerminalEvents, 1);
			Test->TestEqual(FString::Printf(TEXT("%s should emit exactly one structured completion delegate."), *Label), State->StructuredTerminalEvents, 1);
			Test->TestEqual(FString::Printf(TEXT("%s structured outcome must be Succeeded."), *Label), State->TerminalResult.Outcome, EAudioExportOutcome::Succeeded);
			Test->TestTrue(FString::Printf(TEXT("%s structured completion must carry a valid request identity."), *Label), State->TerminalResult.RequestId.IsValid());
			Test->TestTrue(FString::Printf(TEXT("%s structured completion must carry a valid accepted session identity."), *Label), State->TerminalResult.SessionId.IsValid());
			Test->TestEqual(FString::Printf(TEXT("%s progress and terminal request identities must match."), *Label), State->LastProgress.RequestId, State->TerminalResult.RequestId);
			Test->TestEqual(FString::Printf(TEXT("%s progress and terminal session identities must match."), *Label), State->LastProgress.SessionId, State->TerminalResult.SessionId);
			Test->TestEqual(FString::Printf(TEXT("%s terminal result must report one successful file."), *Label), State->TerminalResult.SuccessfulCount, 1);
			Test->TestEqual(FString::Printf(TEXT("%s terminal result must report no failed files."), *Label), State->TerminalResult.FailedCount, 0);
			Test->TestEqual(FString::Printf(TEXT("%s terminal result must report no skipped files."), *Label), State->TerminalResult.SkippedCount, 0);
			Test->TestFalse(FString::Printf(TEXT("%s completion path should not be empty."), *Label), State->FinishedPath.IsEmpty());
			Test->TestTrue(FString::Printf(TEXT("%s should emit progress before completion."), *Label), State->ProgressEvents > 0);

			TArray<FString> WavFiles;
			FindRealPipelineWavFiles(*State, WavFiles);
			if (!Test->TestEqual(FString::Printf(TEXT("%s static SoundWave source must produce EXACTLY one WAV."), *Label), WavFiles.Num(), 1))
			{
				return true;
			}

			TArray<uint8> WavData;
			if (!Test->TestTrue(FString::Printf(TEXT("%s WAV should load from disk."), *Label), FFileHelper::LoadFileToArray(WavData, *WavFiles[0])))
			{
				return true;
			}

			FWavHeaderInfo Header;
			if (!Test->TestTrue(FString::Printf(TEXT("%s WAV header should parse."), *Label), ParseWaveHeader(WavData, Header)))
			{
				return true;
			}
			Test->TestEqual(FString::Printf(TEXT("%s WAV must be PCM (format tag 1)."), *Label), ReadUInt16LE(WavData, 20), static_cast<uint16>(1));
			Test->TestTrue(FString::Printf(TEXT("%s WAV must carry a valid channel count."), *Label), Header.NumChannels > 0);
			Test->TestTrue(FString::Printf(TEXT("%s WAV must carry a valid sample rate."), *Label), Header.SampleRate > 0);
			if (Header.NumChannels == 0 || Header.SampleRate == 0)
			{
				return true;
			}
			Test->TestEqual(FString::Printf(TEXT("%s WAV must be 16-bit PCM."), *Label), Header.BitsPerSample, static_cast<uint16>(16));

			Test->TestTrue(FString::Printf(TEXT("%s WAV must report positive data size."), *Label), Header.DataSize > 0);
			Test->TestTrue(FString::Printf(TEXT("%s WAV payload must fit inside the file."), *Label), WavData.Num() >= 44 + static_cast<int32>(Header.DataSize));
			const int32 BytesPerFrame = static_cast<int32>(Header.NumChannels * sizeof(int16));
			if (!Test->TestTrue(FString::Printf(TEXT("%s WAV data size must be a whole number of 16-bit frames for its observed channel count."), *Label),
				BytesPerFrame > 0 && Header.DataSize % static_cast<uint32>(BytesPerFrame) == 0))
			{
				return true;
			}

			// Duration computed purely from parsed bytes and the parsed realtime sample rate: no
			// production duration value is involved.
			const int32 TotalSamples = static_cast<int32>(Header.DataSize) / static_cast<int32>(sizeof(int16));
			const int32 NumFrames = TotalSamples / static_cast<int32>(Header.NumChannels);
			const double DurationSeconds = Header.SampleRate > 0
				? static_cast<double>(NumFrames) / static_cast<double>(Header.SampleRate)
				: 0.0;
			Test->TestTrue(FString::Printf(TEXT("%s WAV duration (computed from its bytes) must be positive."), *Label), DurationSeconds > 0.0);

			// Channel-0 arithmetic written here, not imported from production: RMS and zero crossings.
			int64 SumSquares = 0;
			int32 ZeroCrossings = 0;
			int32 ChannelZeroSamples = 0;
			int16 PreviousSample = 0;
			const int32 AnalysisStartFrame = FMath::Min(NumFrames, FMath::Max(0, FMath::RoundToInt(static_cast<float>(Header.SampleRate) * 0.5f)));
			const int32 AnalysisEndFrame = FMath::Min(NumFrames, AnalysisStartFrame + FMath::Max(1, FMath::RoundToInt(static_cast<float>(Header.SampleRate) * 0.25f)));
			int64 WindowSumSquares = 0;
			int32 WindowZeroCrossings = 0;
			int32 WindowSamples = 0;
			int16 PreviousWindowSample = 0;
			constexpr int16 AudibleSampleThreshold = 32;
			int32 FirstAudibleFrame = INDEX_NONE;
			int32 LastAudibleFrame = INDEX_NONE;
			for (int32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
			{
				const int32 SampleIndex = FrameIndex * static_cast<int32>(Header.NumChannels);
				const int16 Sample = ReadPcmSample(WavData, SampleIndex);
				SumSquares += static_cast<int64>(Sample) * static_cast<int64>(Sample);
				if (ChannelZeroSamples > 0
					&& ((PreviousSample < 0 && Sample >= 0) || (PreviousSample > 0 && Sample <= 0)))
				{
					++ZeroCrossings;
				}
				PreviousSample = Sample;
				++ChannelZeroSamples;
				if (FMath::Abs(static_cast<int32>(Sample)) > AudibleSampleThreshold)
				{
					if (FirstAudibleFrame == INDEX_NONE)
					{
						FirstAudibleFrame = FrameIndex;
					}
					LastAudibleFrame = FrameIndex;
				}
				if (FrameIndex >= AnalysisStartFrame && FrameIndex < AnalysisEndFrame)
				{
					WindowSumSquares += static_cast<int64>(Sample) * static_cast<int64>(Sample);
					if (WindowSamples > 0
						&& ((PreviousWindowSample < 0 && Sample >= 0) || (PreviousWindowSample > 0 && Sample <= 0)))
					{
						++WindowZeroCrossings;
					}
					PreviousWindowSample = Sample;
					++WindowSamples;
				}
			}

			if (Test->TestTrue(FString::Printf(TEXT("%s WAV must contain at least one decodable frame."), *Label), ChannelZeroSamples > 0))
			{
				const double NormalizedRms = FMath::Sqrt(static_cast<double>(SumSquares) / static_cast<double>(ChannelZeroSamples)) / 32767.0;
				Test->TestTrue(FString::Printf(TEXT("%s WAV must contain an audible (non-silent) signal; computed RMS=%f."), *Label, NormalizedRms),
					NormalizedRms > 0.01 && NormalizedRms <= 1.0);

				const double MeasuredFrequencyHz = static_cast<double>(ZeroCrossings) / (2.0 * DurationSeconds);
				Test->TestTrue(FString::Printf(TEXT("%s WAV must carry a measurable frequency; measured %f Hz over %f s."), *Label, MeasuredFrequencyHz, DurationSeconds),
					MeasuredFrequencyHz > 0.0);
				const double AnalysisDurationSeconds = static_cast<double>(AnalysisEndFrame - AnalysisStartFrame) / static_cast<double>(Header.SampleRate);
				const double WindowRms = WindowSamples > 0
					? FMath::Sqrt(static_cast<double>(WindowSumSquares) / static_cast<double>(WindowSamples)) / 32767.0
					: 0.0;
				const double WindowFrequencyHz = AnalysisDurationSeconds > 0.0
					? static_cast<double>(WindowZeroCrossings) / (2.0 * AnalysisDurationSeconds)
					: 0.0;
				Test->TestTrue(FString::Printf(TEXT("%s WAV must contain a full analysis window with measurable frequency."), *Label),
					WindowSamples > 0 && WindowFrequencyHz > 0.0);
				State->MeasuredNumChannels = Header.NumChannels;
				State->MeasuredSampleRate = Header.SampleRate;
				State->MeasuredDurationSeconds = DurationSeconds;
				State->MeasuredNormalizedRms = NormalizedRms;
				State->MeasuredFrequencyHz = MeasuredFrequencyHz;
				State->MeasuredWindowRms = WindowRms;
				State->MeasuredWindowFrequencyHz = WindowFrequencyHz;
				State->MeasuredWindowFrames = WindowSamples;
				State->MeasuredFrameCount = NumFrames;
				State->MeasuredFirstAudibleFrame = FirstAudibleFrame;
				State->MeasuredLastAudibleFrame = LastAudibleFrame;
				State->bMeasuredWav = true;

			}

			return true;
		}

	private:
		TSharedRef<FRealPipelineExportState> State;
	};

	static TSharedPtr<FRealPipelineExportState> StartRealPipelineExport(
		FAutomationTestBase& Test,
		const TCHAR* AssetPath,
		const FString& TestName,
		const bool bExerciseSoundWaveContracts = false,
		const TSharedPtr<FRealPipelineExportState>& ExistingState = nullptr);

	class FStartRealPipelineExportAfterPreflight final : public IAutomationLatentCommand
	{
	public:
		FStartRealPipelineExportAfterPreflight(
			FAutomationTestBase& InTest,
			const TSharedRef<FRealPipelineExportState>& InState,
			const TCHAR* InAssetPath,
			const FString& InTestName,
			const bool bInExerciseSoundWaveContracts)
			: Test(InTest)
			, State(InState)
			, AssetPath(InAssetPath)
			, TestName(InTestName)
			, bExerciseSoundWaveContracts(bInExerciseSoundWaveContracts)
		{
		}

		virtual bool Update() override;

	private:
		FAutomationTestBase& Test;
		TSharedRef<FRealPipelineExportState> State;
		FString AssetPath;
		FString TestName;
		bool bExerciseSoundWaveContracts = false;
	};

	static TSharedPtr<FRealPipelineExportState> StartRealPipelineExport(
		FAutomationTestBase& Test,
		const TCHAR* AssetPath,
		const FString& TestName,
		const bool bExerciseSoundWaveContracts,
		const TSharedPtr<FRealPipelineExportState>& ExistingState)
	{
		// Hard fail when the environment is unavailable: a -NoSound launch or any other missing
		// prerequisite AddErrors here and returns null, which the callers must turn into a test
		// failure -- there is no non-gating skip path.
		if (FParse::Param(FCommandLine::Get(), TEXT("NoSound")))
		{
			Test.AddError(FString::Printf(TEXT("%s is unavailable: the required real-pipeline audio test needs an active mixer, but this lane was launched with -NoSound."), *TestName));
			return nullptr;
		}

		UWorld* World = ResolveAutomationWorld();
		Test.TestNotNull(FString::Printf(TEXT("%s should have an editor world for the real runtime pipeline."), *TestName), World);
		if (!World || !GEngine)
		{
			return nullptr;
		}

		UAudioExporterRuntimeSubsystem* RuntimeSubsystem = GEngine->GetEngineSubsystem<UAudioExporterRuntimeSubsystem>();
		Test.TestNotNull(FString::Printf(TEXT("%s should resolve the AudioExporter runtime subsystem."), *TestName), RuntimeSubsystem);
		if (!RuntimeSubsystem)
		{
			return nullptr;
		}

		const TSharedRef<FRealPipelineExportState> State = ExistingState.IsValid()
			? ExistingState.ToSharedRef()
			: MakeShared<FRealPipelineExportState>();
		State->Test = &Test;
		State->RuntimeSubsystem = RuntimeSubsystem;
		State->TestName = TestName;
		State->StartSeconds = FPlatformTime::Seconds();
		if (State->OutputRoot.IsEmpty())
		{
			State->OutputRoot = MakeOutputDirectory(FString::Printf(TEXT("RealPipeline_%s"), *TestName));
		}

		if (!ExistingState.IsValid() && RuntimeSubsystem->IsExporting())
		{
			State->bPreflightPending = true;
			State->PreflightCompleteHandle = RuntimeSubsystem->OnExportCompleteNative.AddLambda([State](const FString&)
			{
				++State->PreflightTerminalEvents;
			});
			State->PreflightResultHandle = RuntimeSubsystem->OnExportCompleteResultNative.AddLambda([State](const FAudioExportResult& InResult)
			{
				++State->PreflightStructuredTerminalEvents;
				State->PreflightResult = InResult;
			});
			State->PreflightDeadlineSeconds = FPlatformTime::Seconds() + RealPipelineCancellationDrainSeconds;
			RuntimeSubsystem->CancelExport(false);
			ADD_LATENT_AUTOMATION_COMMAND(FStartRealPipelineExportAfterPreflight(Test, State, AssetPath, TestName, bExerciseSoundWaveContracts));
			return State;
		}

		// SoundCueOneShot/MetaSoundOneShot ship checked into this repo's Content/ as REQUIRED
		// release-case fixtures for this test (Docs/AutomationFixtures.md). A missing required
		// fixture is a real regression -- AddError + a null state the caller must fail on, never a
		// silent AddInfo-and-pass skip.
		USoundBase* FixtureSound = LoadObject<USoundBase>(nullptr, AssetPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!FixtureSound)
		{
			Test.AddError(FString::Printf(TEXT("%s: required fixture '%s' is not present in this project."), *TestName, AssetPath));
			return nullptr;
		}

		State->StartSeconds = FPlatformTime::Seconds();
		if (const USoundWave* FixtureWave = Cast<USoundWave>(FixtureSound))
		{
			State->OriginalVolume = FixtureWave->Volume;
			State->OriginalPitch = FixtureWave->Pitch;
		}

		if (bExerciseSoundWaveContracts)
		{
			USoundWave* ContractWave = Cast<USoundWave>(FixtureSound);
			if (!ContractWave)
			{
				Test.AddError(FString::Printf(TEXT("%s requires a static SoundWave fixture."), *TestName));
				return nullptr;
			}
			State->ContractWave = ContractWave;
			State->OriginalVolume = ContractWave->Volume;
			State->OriginalPitch = ContractWave->Pitch;
			State->OriginalSourceEffectChain = ContractWave->SourceEffectChain;
			State->ContractSourceEffectChain = MakeRealtimeContractSourceEffectChain(Test);
			if (!State->ContractSourceEffectChain)
			{
				return nullptr;
			}
			State->bContractWaveWasRooted = ContractWave->IsRooted();
			if (!State->bContractWaveWasRooted)
			{
				ContractWave->AddToRoot();
				State->bContractWaveRootAdded = true;
			}
			ContractWave->Volume = 0.25f;
			ContractWave->Pitch = 2.0f;
			ContractWave->SourceEffectChain = State->ContractSourceEffectChain;
			State->bRestoreContractWave = true;
			State->bSoundWaveContractAssertions = true;
		}

		State->CompleteHandle = RuntimeSubsystem->OnExportCompleteNative.AddLambda([State](const FString& InPath)
		{
			++State->TerminalEvents;
			State->FinishedPath = InPath;
			State->bFinished = true;
		});
		State->ResultHandle = RuntimeSubsystem->OnExportCompleteResultNative.AddLambda([State](const FAudioExportResult& InResult)
		{
			++State->StructuredTerminalEvents;
			State->TerminalResult = InResult;
		});
		State->ProgressHandle = RuntimeSubsystem->OnExportProgressNative.AddLambda([State](const FAudioExportProgressInfo& InProgress)
		{
			++State->ProgressEvents;
			State->LastProgress = InProgress;
		});

		FAudioExportSoundEntry Entry;
		Entry.Sound = FixtureSound;
		Entry.SoundName = FixtureSound->GetName();
		Entry.SourceAssetPath = FixtureSound->GetPackage()->GetName();
		Entry.RelativePath = MakeRelativeAssetPath(FixtureSound);

		FAudioExporterConfig Config;
		Config.OneShotVariations = 1;
		Config.LoopVariations = 1;
		Config.MaxLengthSeconds = 5; // The supported minimum; FAudioExporterConfig::Sanitize clamps below it.
		Config.bNormalize = false;
		Config.bMonitor = false;
		Config.bExportTextLog = true;

		TArray<FAudioExportSoundEntry> Entries;
		Entries.Add(Entry);

		const bool bStarted = RuntimeSubsystem->StartExport(World, Entries, Config, State->OutputRoot);
		Test.TestTrue(FString::Printf(TEXT("%s should start the real AudioExporter runtime pipeline."), *TestName), bStarted);
		if (!bStarted)
		{
			RemoveRealPipelineDelegates(*State);
			RestoreRealPipelineSoundWave(*State);
			return nullptr;
		}

		return State;
	}

	bool FStartRealPipelineExportAfterPreflight::Update()
	{
		if (!State->bPreflightPending)
		{
			return true;
		}

		UAudioExporterRuntimeSubsystem* RuntimeSubsystem = State->RuntimeSubsystem;
		const bool bExplicitCancellationOutcome = State->PreflightStructuredTerminalEvents > 0
			&& (State->PreflightResult.Outcome == EAudioExportOutcome::Cancelled
				|| State->PreflightResult.Outcome == EAudioExportOutcome::Rejected);
		const bool bTerminalDelivered = State->PreflightStructuredTerminalEvents > 0 || bExplicitCancellationOutcome;
		const bool bWriterDrainConfirmed = RuntimeSubsystem
			&& bTerminalDelivered
			&& !RuntimeSubsystem->IsExporting()
			&& RuntimeSubsystem->GetActiveAsyncExportsForTesting() == 0;
		if (bWriterDrainConfirmed)
		{
			if (State->PreflightCompleteHandle.IsValid())
			{
				RuntimeSubsystem->OnExportCompleteNative.Remove(State->PreflightCompleteHandle);
			}
			State->PreflightCompleteHandle = FDelegateHandle();
			if (State->PreflightResultHandle.IsValid())
			{
				RuntimeSubsystem->OnExportCompleteResultNative.Remove(State->PreflightResultHandle);
			}
			State->PreflightResultHandle = FDelegateHandle();
			State->bPreflightPending = false;
			const TSharedPtr<FRealPipelineExportState> StartedState = StartRealPipelineExport(
				Test, *AssetPath, TestName, bExerciseSoundWaveContracts, State);
			if (!StartedState.IsValid())
			{
				State->bPreflightFailed = true;
				State->bWaitTimedOut = true;
			}
			return true;
		}

		if (FPlatformTime::Seconds() > State->PreflightDeadlineSeconds)
		{
			State->bPreflightFailed = true;
			State->bPreflightPending = false;
			State->bWaitTimedOut = true;
			if (State->Test)
			{
				State->Test->AddError(FString::Printf(TEXT("%s could not drain the earlier export's terminal cancellation and async writers before the bounded preflight deadline; live state remains preserved."), *TestName));
			}
			const TWeakObjectPtr<UAudioExporterRuntimeSubsystem> PreflightRuntimeSubsystem = State->RuntimeSubsystem;
			const TSharedRef<FRealPipelineExportState> RetainedState = State;
			AudioExporterRuntimeTestUtils::RegisterRetainedTestCleanup(
				[RetainedState, PreflightRuntimeSubsystem]()
				{
					UAudioExporterRuntimeSubsystem* RuntimeSubsystem = PreflightRuntimeSubsystem.Get();
					return RetainedState->PreflightStructuredTerminalEvents > 0
						&& RuntimeSubsystem
						&& !RuntimeSubsystem->IsExporting()
						&& RuntimeSubsystem->GetActiveAsyncExportsForTesting() == 0;
				},
				[RetainedState, PreflightRuntimeSubsystem]()
				{
					if (UAudioExporterRuntimeSubsystem* RuntimeSubsystem = PreflightRuntimeSubsystem.Get())
					{
						if (RetainedState->PreflightCompleteHandle.IsValid())
						{
							RuntimeSubsystem->OnExportCompleteNative.Remove(RetainedState->PreflightCompleteHandle);
						}
						if (RetainedState->PreflightResultHandle.IsValid())
						{
							RuntimeSubsystem->OnExportCompleteResultNative.Remove(RetainedState->PreflightResultHandle);
						}
					}
					RetainedState->PreflightCompleteHandle = FDelegateHandle();
					RetainedState->PreflightResultHandle = FDelegateHandle();
					RetainedState->bPreflightPending = false;
				},
				[RetainedState, PreflightRuntimeSubsystem]()
				{
					if (UAudioExporterRuntimeSubsystem* RuntimeSubsystem = PreflightRuntimeSubsystem.Get())
					{
						if (RuntimeSubsystem->IsExporting() || RuntimeSubsystem->GetActiveAsyncExportsForTesting() != 0)
						{
							if (RetainedState->Test)
							{
								RetainedState->Test->AddError(FString::Printf(TEXT("%s retained preflight delegates during engine shutdown because the earlier export was still active or had async writers."), *RetainedState->TestName));
							}
							return false;
						}
						if (RetainedState->PreflightCompleteHandle.IsValid())
						{
							RuntimeSubsystem->OnExportCompleteNative.Remove(RetainedState->PreflightCompleteHandle);
						}
						if (RetainedState->PreflightResultHandle.IsValid())
						{
							RuntimeSubsystem->OnExportCompleteResultNative.Remove(RetainedState->PreflightResultHandle);
						}
					}
					if (RetainedState->PreflightStructuredTerminalEvents == 0 && RetainedState->Test)
					{
						RetainedState->Test->AddError(FString::Printf(TEXT("%s reached engine shutdown without a structured preflight terminal result; removing the preflight delegates during shutdown."), *RetainedState->TestName));
					}
					RetainedState->PreflightCompleteHandle = FDelegateHandle();
					RetainedState->PreflightResultHandle = FDelegateHandle();
					RetainedState->bPreflightPending = false;
					return true;
				});
			return true;
		}

		return false;
	}

	class FStartRealPipelineExportAfter : public IAutomationLatentCommand
	{
	public:
		FStartRealPipelineExportAfter(
			FAutomationTestBase& InTest,
			const TSharedRef<FRealPipelineExportState>& InPredecessor,
			const TSharedRef<TSharedPtr<FRealPipelineExportState>>& InStateHolder,
			const TSharedRef<bool>& InStartAttempted,
			const TCHAR* InAssetPath,
			const TCHAR* InTestName)
			: Test(InTest)
			, Predecessor(InPredecessor)
			, StateHolder(InStateHolder)
			, StartAttempted(InStartAttempted)
			, AssetPath(InAssetPath)
			, TestName(InTestName)
		{
		}

		virtual bool Update() override
		{
			if (!Predecessor->bTestChainSettled)
			{
				return false;
			}

			*StartAttempted = true;
			if (!Predecessor->bTerminalSettled || Predecessor->bWaitTimedOut)
			{
				if (!Predecessor->bWaitTimedOut)
				{
					Test.AddError(FString::Printf(TEXT("%s control SoundWave capture did not settle before the modified capture could start."), *TestName));
				}
				return true;
			}
			*StateHolder = StartRealPipelineExport(Test, *AssetPath, *TestName, true);
			if (!StateHolder->IsValid())
			{
				Test.AddError(FString::Printf(TEXT("%s could not start its modified SoundWave capture after the control capture settled."), *TestName));
			}
			return true;
		}

	private:
		FAutomationTestBase& Test;
		TSharedRef<FRealPipelineExportState> Predecessor;
		TSharedRef<TSharedPtr<FRealPipelineExportState>> StateHolder;
		TSharedRef<bool> StartAttempted;
		FString AssetPath;
		FString TestName;
	};

	class FWaitForRealPipelineExportHolder : public IAutomationLatentCommand
	{
	public:
		FWaitForRealPipelineExportHolder(const TSharedRef<TSharedPtr<FRealPipelineExportState>>& InStateHolder, const TSharedRef<bool>& InStartAttempted, const double InTimeoutSeconds)
			: StateHolder(InStateHolder)
			, StartAttempted(InStartAttempted)
			, TimeoutSeconds(InTimeoutSeconds)
		{
		}

		virtual bool Update() override
		{
			if (!StateHolder->IsValid())
			{
				return *StartAttempted;
			}
			if (!Inner)
			{
				Inner = MakeUnique<FWaitForAudioExporterRealPipelineExport>(StateHolder->ToSharedRef(), TimeoutSeconds);
			}
			return Inner->Update();
		}

	private:
		TSharedRef<TSharedPtr<FRealPipelineExportState>> StateHolder;
		TSharedRef<bool> StartAttempted;
		TUniquePtr<FWaitForAudioExporterRealPipelineExport> Inner;
		double TimeoutSeconds = 30.0;
	};

	class FVerifyRealPipelineExportHolder : public IAutomationLatentCommand
	{
	public:
		FVerifyRealPipelineExportHolder(const TSharedRef<TSharedPtr<FRealPipelineExportState>>& InStateHolder, const TSharedRef<bool>& InStartAttempted)
			: StateHolder(InStateHolder)
			, StartAttempted(InStartAttempted)
		{
		}

		virtual bool Update() override
		{
			if (!StateHolder->IsValid())
			{
				return *StartAttempted;
			}
			if (!Inner)
			{
				Inner = MakeUnique<FVerifySoundWaveRealPipelineExport>(StateHolder->ToSharedRef());
			}
			return Inner->Update();
		}

	private:
		TSharedRef<TSharedPtr<FRealPipelineExportState>> StateHolder;
		TSharedRef<bool> StartAttempted;
		TUniquePtr<FVerifySoundWaveRealPipelineExport> Inner;
	};

	class FCompareSoundWaveRealPipelineCaptures : public IAutomationLatentCommand
	{
	public:
		FCompareSoundWaveRealPipelineCaptures(
			FAutomationTestBase& InTest,
			const TSharedRef<FRealPipelineExportState>& InControl,
			const TSharedRef<TSharedPtr<FRealPipelineExportState>>& InModifiedHolder,
			const TSharedRef<bool>& InStartAttempted)
			: Test(InTest)
			, Control(InControl)
			, ModifiedHolder(InModifiedHolder)
			, StartAttempted(InStartAttempted)
		{
		}

		virtual bool Update() override
		{
			if (!ModifiedHolder->IsValid())
			{
				return *StartAttempted;
			}
			if (!(*ModifiedHolder)->bMeasuredWav)
			{
				if ((*ModifiedHolder)->bWaitTimedOut || (*ModifiedHolder)->bTestChainSettled)
				{
					Test.AddError(TEXT("The modified SoundWave capture did not produce measurable WAV metrics before its bounded verification completed."));
					return true;
				}
				if (WaitStartSeconds <= 0.0)
				{
					WaitStartSeconds = FPlatformTime::Seconds();
				}
				if ((FPlatformTime::Seconds() - WaitStartSeconds) > ModifiedCaptureVerificationTimeoutSeconds)
				{
					Test.AddError(TEXT("The modified SoundWave capture did not produce measurable WAV metrics before the bounded comparison timeout."));
					return true;
				}
				return false;
			}

			const FRealPipelineExportState& Modified = *(*ModifiedHolder);
			if (!Control->bMeasuredWav)
			{
				Test.AddError(TEXT("The control SoundWave capture did not produce measurable WAV metrics."));
				return true;
			}
			if (Control->MeasuredWindowFrames <= 0 || Modified.MeasuredWindowFrames <= 0
				|| Control->MeasuredFrameCount <= 0 || Modified.MeasuredFrameCount <= 0
				|| Control->MeasuredFirstAudibleFrame == INDEX_NONE || Control->MeasuredLastAudibleFrame == INDEX_NONE
				|| Modified.MeasuredFirstAudibleFrame == INDEX_NONE || Modified.MeasuredLastAudibleFrame == INDEX_NONE
				|| Control->MeasuredWindowRms <= 0.0 || Modified.MeasuredWindowRms <= 0.0
				|| Control->MeasuredWindowFrequencyHz <= 0.0 || Modified.MeasuredWindowFrequencyHz <= 0.0
				|| Control->OriginalVolume <= 0.0f || Control->OriginalPitch <= 0.0f)
			{
				Test.AddError(TEXT("Control and modified SoundWave captures must both provide positive analysis metrics."));
				return true;
			}

			if (!Modified.bSoundWaveContractAssertions)
			{
				Test.AddError(TEXT("The modified SoundWave capture must run the volume, pitch, and source-effect contract fixture."));
				return true;
			}
			if (!Modified.ContractSourceEffectChain)
			{
				Test.AddError(TEXT("The modified SoundWave capture must retain its configured source-effect chain."));
				return true;
			}
			Test.TestTrue(TEXT("The modified SoundWave contract chain must contain exactly one source effect."),
				Modified.ContractSourceEffectChain->Chain.Num() == 1);
			Test.TestTrue(TEXT("The modified SoundWave contract chain must enable effect tails."),
				Modified.ContractSourceEffectChain->bPlayEffectChainTails);
			if (Modified.ContractSourceEffectChain->Chain.Num() != 1
				|| !Modified.ContractSourceEffectChain->bPlayEffectChainTails
				|| Modified.ContractSourceEffectChain->Chain[0].bBypass
				|| !Modified.ContractSourceEffectChain->Chain[0].Preset)
			{
				Test.AddError(TEXT("The modified SoundWave contract chain must contain an active, non-bypassed source effect."));
				return true;
			}

			// Derive the dry pitched audible span from the control WAV itself, then require the modified
			// WAV to contain audible samples at least one configured delay beyond that dry boundary.
			// Recorder shutdown padding is silent and cannot satisfy this proof.
			const double ControlAudibleDuration = static_cast<double>(
				Control->MeasuredLastAudibleFrame - Control->MeasuredFirstAudibleFrame + 1)
				/ static_cast<double>(Control->MeasuredSampleRate);
			const double DryPitchedAudibleDuration = ControlAudibleDuration
				* (static_cast<double>(Control->OriginalPitch) / static_cast<double>(Modified.ContractPitch));
			const double DurationResolution = 1.0 / static_cast<double>(Modified.MeasuredSampleRate);
			const int32 ExpectedTailFrames = FMath::Max(1, FMath::RoundToInt(
				static_cast<float>(Modified.ContractSourceEffectDelaySeconds / DurationResolution)));
			const double ExpectedTailSeconds = static_cast<double>(ExpectedTailFrames) * DurationResolution;
			const double ModifiedAudibleDuration = static_cast<double>(
				Modified.MeasuredLastAudibleFrame - Modified.MeasuredFirstAudibleFrame + 1)
				/ static_cast<double>(Modified.MeasuredSampleRate);
			const double MeasuredTailSeconds = ModifiedAudibleDuration - DryPitchedAudibleDuration;
			const double TailTolerance = 2.0 * DurationResolution;
			Test.TestTrue(
				FString::Printf(TEXT("Modified SoundWave must retain an audible source-effect tail: dry pitched audible span %.9f s plus at least %d frames (%.9f s) within %.9f s; measured audible extension %.9f s."),
					DryPitchedAudibleDuration, ExpectedTailFrames, ExpectedTailSeconds, TailTolerance, MeasuredTailSeconds),
				MeasuredTailSeconds + TailTolerance >= ExpectedTailSeconds);

			Test.TestEqual(TEXT("Control and modified captures must retain the observed sample rate."), Modified.MeasuredSampleRate, Control->MeasuredSampleRate);
			Test.TestEqual(TEXT("Control and modified captures must retain the observed channel count."), Modified.MeasuredNumChannels, Control->MeasuredNumChannels);

			// Use the deterministic worst-case int16 quantization bound. Quantization error may correlate
			// with a periodic fixture, so dividing it by sqrt(frame count) would incorrectly assume random
			// error and reject a correct authored gain.
			const double QuantizationError = 0.5 / 32767.0;
			const double ControlRmsError = QuantizationError;
			const double ModifiedRmsError = QuantizationError;
			const double RmsRatio = Modified.MeasuredWindowRms / Control->MeasuredWindowRms;
			const double RmsRatioTolerance = (ControlRmsError / Control->MeasuredWindowRms)
				+ (ModifiedRmsError / Modified.MeasuredWindowRms);
			// The authored effect passes live source samples at unity in the analysis window. Keep this
			// literal test-side chain math independent of production and include the authored SoundWave
			// volume in the expected ratio.
			constexpr double ContractSteadyStateGain = 1.0;
			const double ExpectedVolumeRatio = (0.25 * ContractSteadyStateGain) / static_cast<double>(Control->OriginalVolume);
			Test.TestTrue(FString::Printf(TEXT("Modified SoundWave RMS ratio must equal the authored volume and source-effect gain ratio %.9f within int16/frame bound; measured %.9f +/- %.9f."), ExpectedVolumeRatio, RmsRatio, RmsRatioTolerance),
				FMath::Abs(RmsRatio - ExpectedVolumeRatio) <= RmsRatioTolerance);

			// Zero-crossing frequency resolution is one half-cycle over the measured window. The
			// ratio bound below is derived from both files' observed sample counts and durations.
			const double ControlFrequencyResolution = static_cast<double>(Control->MeasuredSampleRate)
				/ (2.0 * static_cast<double>(Control->MeasuredWindowFrames));
			const double ModifiedFrequencyResolution = static_cast<double>(Modified.MeasuredSampleRate)
				/ (2.0 * static_cast<double>(Modified.MeasuredWindowFrames));
			const double FrequencyRatio = Modified.MeasuredWindowFrequencyHz / Control->MeasuredWindowFrequencyHz;
			const double FrequencyRatioTolerance = (ModifiedFrequencyResolution / Control->MeasuredWindowFrequencyHz)
				+ (Modified.MeasuredWindowFrequencyHz * ControlFrequencyResolution
					/ (Control->MeasuredWindowFrequencyHz * Control->MeasuredWindowFrequencyHz));
			const double ExpectedPitchRatio = 2.0 / static_cast<double>(Control->OriginalPitch);
			Test.TestTrue(FString::Printf(TEXT("Modified SoundWave frequency ratio must equal the authored to control pitch ratio %.9f within measured zero-crossing resolution; measured %.9f +/- %.9f."), ExpectedPitchRatio, FrequencyRatio, FrequencyRatioTolerance),
				FMath::Abs(FrequencyRatio - ExpectedPitchRatio) <= FrequencyRatioTolerance);
			return true;
		}

	private:
		FAutomationTestBase& Test;
		TSharedRef<FRealPipelineExportState> Control;
		TSharedRef<TSharedPtr<FRealPipelineExportState>> ModifiedHolder;
		TSharedRef<bool> StartAttempted;
		double WaitStartSeconds = 0.0;
	};

	/**
	 * The SoundWave timebase regression. The standard verifier above measures
	 * the produced WAV; this command pins its TIMING against hand-computed expectations:
	 *  - the source attack ("first-frame impulse") sits at the very start of the file: the source-
	 *    listener batch clock is rebased to playback start, so no pre-playback batch setup may be
	 *    baked into the markers;
	 *  - the produced file is neither truncated below the fixture's authored dry span nor padded out
	 *    to the batch span, so the nonzero batch separator (FAudioRendererRealtime::SeparatorSilence)
	 *    never leaks into a one-file source;
	 *  - no silent run as long as the separator may exist inside the produced CONTENT span (between
	 *    the first and last audible frames);
	 *  - the only licensed trailing silence is UE's tail-completion window: with bPlayEffectChainTails
	 *    the mixer keeps the source alive after its audible content ends until the source's peak
	 *    envelope follower (release 100 ms) decays below the -96 dB ENVELOPE_TAIL_THRESHOLD
	 *    (AudioMixerSourceManager.cpp: bEffectTailsDone), and AudioDevice.cpp defers the one-shot
	 *    stop to the mixer, so OnAudioFinished -- and with it the listener capture end -- lands only
	 *    after that window. The listener honestly appends those engine-generated zero quanta
	 *    (measured: 39200 frames / 0.817 s after the bounded 250 ms contract tail).
	 * Deleting the batch-clock rebase slides the markers against the listener buffer: the attack
	 * moves measurably into the file and the separator-tainted span reappears -- the asserts fail.
	 */
	class FVerifySoundWaveExportTiming : public IAutomationLatentCommand
	{
	public:
		static constexpr int32 FirstAudibleToleranceFrames = 480;
		static constexpr double CaptureBlockToleranceSeconds = 0.1;
		// UE 5.8 FMixerSourceManager tail-completion window, derived from engine constants (verified
		// against the local 5.8 install): a bPlayEffectChainTails source stays alive until its peak
		// envelope follower (attack 10 ms / release 100 ms, AudioMixerSourceManager.cpp SourceInfo
		// setup) decays below ENVELOPE_TAIL_THRESHOLD = 1.58489e-5 (-96 dB, AudioMixerSourceManager.cpp:198,
		// threshold check :3550). The follower input is clamped to [0, 1] (:3548), so from ANY level
		// the decay to the threshold is bounded by release * ln(1 / threshold) = 0.1 * ln(63096)
		// = 0.1 * 11.052 ~= 1.106 s. The measured 0.817 s window of the 0.1-amplitude contract tail
		// sits inside that bound with margin; one capture block of quantum granularity is added at
		// the assertion sites.
		static constexpr double EnvelopeFollowerReleaseSeconds = 0.1;
		static constexpr double EnvelopeTailThreshold = 1.58489e-5; // -96 dB
		static constexpr double EngineTailCompletionWindowSeconds = 1.106;

		FVerifySoundWaveExportTiming(
			const TSharedRef<FRealPipelineExportState>& InControl,
			const TSharedRef<TSharedPtr<FRealPipelineExportState>>& InModifiedHolder,
			const TSharedRef<bool>& InStartAttempted,
			double InExpectedDryDurationSeconds)
			: Control(InControl)
			, ModifiedHolder(InModifiedHolder)
			, StartAttempted(InStartAttempted)
			, ExpectedDryDurationSeconds(InExpectedDryDurationSeconds)
		{
		}

		virtual bool Update() override
		{
			if (!ModifiedHolder->IsValid())
			{
				return *StartAttempted;
			}
			FAutomationTestBase* Test = Control->Test;
			if (!Test)
			{
				return true;
			}
			if (!(*ModifiedHolder)->bMeasuredWav)
			{
				// The comparison command before this one owns waiting for and reporting the modified
				// capture's metrics; reaching here without them is already a reported failure.
				Test->AddError(TEXT("The SoundWave timing verification was reached without measured modified-capture metrics."));
				return true;
			}

			VerifyStateTiming(*Test, *Control, ExpectedDryDurationSeconds, TEXT("SoundWave timing (control)"),
				0.0, HasTailsEnabledChain(Control->OriginalSourceEffectChain));

			const FRealPipelineExportState& Modified = *(*ModifiedHolder);
			const double DryPitchedSeconds = ExpectedDryDurationSeconds / static_cast<double>(Modified.ContractPitch);
			VerifyStateTiming(*Test, Modified, DryPitchedSeconds, TEXT("SoundWave timing (modified)"),
				static_cast<double>(Modified.ContractSourceEffectDelaySeconds), HasTailsEnabledChain(Modified.ContractSourceEffectChain));
			return true;
		}

	private:
		// A chain licenses the engine's tail-completion window only when it actually plays tails:
		// a null, empty, or tails-disabled chain lets the one-shot stop fire at the dry boundary,
		// so those states keep the strict separator bounds below.
		static bool HasTailsEnabledChain(const USoundEffectSourcePresetChain* InChain)
		{
			return InChain && InChain->bPlayEffectChainTails && InChain->Chain.Num() > 0;
		}

		void VerifyStateTiming(
			FAutomationTestBase& Test,
			const FRealPipelineExportState& State,
			double ExpectedDrySeconds,
			const FString& Label,
			double AuthoredTailSeconds,
			bool bEngineTailCompletionWindowApplies) const
		{
			if (State.MeasuredFirstAudibleFrame == INDEX_NONE || State.MeasuredSampleRate == 0)
			{
				Test.AddError(FString::Printf(TEXT("%s produced no measurable first-audible frame or sample rate."), *Label));
				return;
			}

			// 1. First-frame impulse: a 440 Hz tone crosses the verifier's audible threshold within
			// two frames of any starting phase; 480 frames (~10 ms at 48 kHz) is generous for block
			// quantization yet 50x tighter than one documented capture block.
			Test.TestTrue(FString::Printf(TEXT("%s must open on the source's first-frame impulse: first audible frame %d, tolerance %d."),
					*Label, State.MeasuredFirstAudibleFrame, FirstAudibleToleranceFrames),
				State.MeasuredFirstAudibleFrame <= FirstAudibleToleranceFrames);

			// 2. Duration bounds against the fixture's authored dry span: not truncated below it,
			// never padded out to the separator-tainted batch span. With a tails-enabled chain the
			// only licensed padding beyond the authored content is the engine's tail-completion
			// window (derived bound above) plus one capture block; the listener capture ends when
			// the mixer releases the source, so the batch separator cannot enter it -- and the
			// window-aware bound's slack stays below one separator span, so a leaked separator
			// would still breach this duration assert.
			const double MinDurationSeconds = ExpectedDrySeconds - CaptureBlockToleranceSeconds;
			const double MaxDurationSeconds = bEngineTailCompletionWindowApplies
				? ExpectedDrySeconds + AuthoredTailSeconds + EngineTailCompletionWindowSeconds + CaptureBlockToleranceSeconds
				: ExpectedDrySeconds + FAudioRendererRealtime::SeparatorSilence;
			const FString DurationBoundReason = bEngineTailCompletionWindowApplies
				? FString::Printf(TEXT("the dry span %.2f s + the authored source-effect tail %.2f s + the engine's tail-completion window %.2f s + one capture block (total %.2f s)"),
					ExpectedDrySeconds, AuthoredTailSeconds, EngineTailCompletionWindowSeconds, MaxDurationSeconds)
				: FString::Printf(TEXT("the dry span plus the nonzero separator (%.2f s)"),
					MaxDurationSeconds);
			Test.TestTrue(FString::Printf(TEXT("%s duration %.6f s must not be truncated below the authored dry span %.6f s (tolerance %.3f s)."),
					*Label, State.MeasuredDurationSeconds, ExpectedDrySeconds, CaptureBlockToleranceSeconds),
				State.MeasuredDurationSeconds >= MinDurationSeconds);
			Test.TestTrue(FString::Printf(TEXT("%s duration %.6f s must stay below %s: the batch separator may never leak into the file."),
					*Label, State.MeasuredDurationSeconds, *DurationBoundReason),
				State.MeasuredDurationSeconds < MaxDurationSeconds);

			// 3. Silent-run guards: separator-sized silence inside the produced audio is always a
			// batch leak. With a tails-enabled chain the engine's tail-completion window (class
			// comment) licenses trailing zeros past the last audible frame and nothing else.
			TArray<FString> WavFiles;
			FindRealPipelineWavFiles(State, WavFiles);
			if (!Test.TestEqual(FString::Printf(TEXT("%s must still resolve exactly one produced WAV for the silent-run scan."), *Label), WavFiles.Num(), 1))
			{
				return;
			}
			TArray<uint8> WavData;
			if (!Test.TestTrue(FString::Printf(TEXT("%s WAV should load from disk for the silent-run scan."), *Label), FFileHelper::LoadFileToArray(WavData, *WavFiles[0])))
			{
				return;
			}
			FWavHeaderInfo Header;
			if (!Test.TestTrue(FString::Printf(TEXT("%s WAV header should parse for the silent-run scan."), *Label), ParseWaveHeader(WavData, Header)))
			{
				return;
			}
			const int32 BytesPerFrame = static_cast<int32>(Header.NumChannels) * static_cast<int32>(sizeof(int16));
			if (!Test.TestTrue(FString::Printf(TEXT("%s WAV must be decodable 16-bit PCM for the silent-run scan."), *Label),
				Header.BitsPerSample == 16 && Header.NumChannels > 0 && BytesPerFrame > 0 && Header.DataSize % static_cast<uint32>(BytesPerFrame) == 0))
			{
				return;
			}
			const int32 NumFrames = static_cast<int32>(Header.DataSize) / BytesPerFrame;
			constexpr int16 SilentSampleThreshold = 32;
			const int32 SeparatorSilentRunFrames = FMath::Max(1, FMath::RoundToInt(FAudioRendererRealtime::SeparatorSilence * static_cast<double>(Header.SampleRate)));
			const int32 TailWindowSilentRunFrames = FMath::Max(1, FMath::RoundToInt((EngineTailCompletionWindowSeconds + CaptureBlockToleranceSeconds) * static_cast<double>(Header.SampleRate)));
			int32 CurrentSilentRun = 0;
			int32 LongestSilentRun = 0;
			int32 LongestContentSilentRun = 0;
			for (int32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
			{
				bool bFrameSilent = true;
				for (int32 ChannelIndex = 0; ChannelIndex < static_cast<int32>(Header.NumChannels); ++ChannelIndex)
				{
					if (FMath::Abs(static_cast<int32>(ReadPcmSample(WavData, FrameIndex * static_cast<int32>(Header.NumChannels) + ChannelIndex))) > SilentSampleThreshold)
					{
						bFrameSilent = false;
						break;
					}
				}
				CurrentSilentRun = bFrameSilent ? CurrentSilentRun + 1 : 0;
				LongestSilentRun = FMath::Max(LongestSilentRun, CurrentSilentRun);
				if (State.MeasuredLastAudibleFrame != INDEX_NONE && FrameIndex <= State.MeasuredLastAudibleFrame)
				{
					LongestContentSilentRun = FMath::Max(LongestContentSilentRun, CurrentSilentRun);
				}
			}
			if (bEngineTailCompletionWindowApplies)
			{
				// Inside the content span the separator guard keeps its original full strength: a
				// separator-sized silent run between audible frames is a batch-clock leak, period.
				Test.TestTrue(FString::Printf(TEXT("%s longest silent run inside the content span (%d frames, audible %d to %d) must stay below the nonzero separator span (%d frames): the variation separator may never appear inside the produced audio."),
						*Label, LongestContentSilentRun, State.MeasuredFirstAudibleFrame, State.MeasuredLastAudibleFrame, SeparatorSilentRunFrames),
					LongestContentSilentRun < SeparatorSilentRunFrames);

				// Past the last audible frame only the engine's tail-completion window is licensed
				// (see the class comment); anything batch-sized back there is padding.
				const int32 TrailingSilentFrames = FMath::Max(0, NumFrames - State.MeasuredLastAudibleFrame - 1);
				Test.TestTrue(FString::Printf(TEXT("%s trailing silence past the last audible frame (%d frames) must stay below the engine tail-completion window bound (%d frames): batch padding may never follow the produced content."),
						*Label, TrailingSilentFrames, TailWindowSilentRunFrames),
					TrailingSilentFrames < TailWindowSilentRunFrames);
			}
			else
			{
				Test.TestTrue(FString::Printf(TEXT("%s longest silent run (%d frames) must stay below the nonzero separator span (%d frames): the variation separator may never appear inside the produced audio."),
						*Label, LongestSilentRun, SeparatorSilentRunFrames),
					LongestSilentRun < SeparatorSilentRunFrames);
			}
		}

		TSharedRef<FRealPipelineExportState> Control;
		TSharedRef<TSharedPtr<FRealPipelineExportState>> ModifiedHolder;
		TSharedRef<bool> StartAttempted;
		double ExpectedDryDurationSeconds = 0.0;
	};

	class FNullRendererExportPipeline final : public FAudioExportPipeline
	{
	protected:
		virtual TUniquePtr<FAudioRendererRealtime> CreateRenderer() const override
		{
			return nullptr;
		}
	};

	struct FGeneratedProceduralCaptureState
	{
		FAutomationTestBase* Test = nullptr;
		TObjectPtr<UAudioComponent> Component = nullptr;
		TObjectPtr<USoundWaveProcedural> Wave = nullptr;
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<USoundSubmix> RecordingSubmix;
		double StartSeconds = 0.0;
	};

	class FFinishGeneratedProceduralSubmixCapture : public IAutomationLatentCommand
	{
	public:
		explicit FFinishGeneratedProceduralSubmixCapture(const TSharedRef<FGeneratedProceduralCaptureState>& InState)
			: State(InState)
		{
		}

		virtual bool Update() override
		{
			if ((FPlatformTime::Seconds() - State->StartSeconds) < 1.0)
			{
				return false;
			}

			FAutomationTestBase* Test = State->Test;
			UWorld* World = State->World.Get();
			USoundSubmix* RecordingSubmix = State->RecordingSubmix.Get();
			Audio::FMixerDevice* MixerDevice = World ? FAudioDeviceManager::GetAudioMixerDeviceFromWorldContext(World) : nullptr;

			if (State->Component)
			{
				State->Component->Stop();
			}

			float NumChannels = 0.0f;
			float SampleRate = 0.0f;
			Audio::FAlignedFloatBuffer RecordedBuffer;
			if (MixerDevice && RecordingSubmix)
			{
				RecordedBuffer = MixerDevice->StopRecording(RecordingSubmix, NumChannels, SampleRate);
			}

			float MaxAbs = 0.0f;
			for (const float Sample : RecordedBuffer)
			{
				MaxAbs = FMath::Max(MaxAbs, FMath::Abs(Sample));
			}

			if (Test)
			{
				Test->TestTrue(TEXT("Generated procedural submix capture should record samples."), RecordedBuffer.Num() > 0);
				Test->TestTrue(TEXT("Generated procedural capture should report a valid observed channel count."), NumChannels > 0.0f);
				Test->TestTrue(TEXT("Generated procedural capture should report a valid observed sample rate."), SampleRate > 0.0f);
				const int32 ObservedChannels = FMath::RoundToInt(NumChannels);
				Test->TestTrue(TEXT("Generated procedural capture should contain whole observed frames."),
					ObservedChannels > 0 && RecordedBuffer.Num() % ObservedChannels == 0);
				Test->TestTrue(TEXT("Generated procedural submix capture should contain non-silent samples."), MaxAbs > 0.01f);
			}

			if (State->Component)
			{
				State->Component->RemoveFromRoot();
				State->Component->UnregisterComponent();
				State->Component->DestroyComponent();
				State->Component = nullptr;
			}
			if (State->Wave)
			{
				State->Wave->RemoveFromRoot();
				State->Wave = nullptr;
			}

			return true;
		}

	private:
		TSharedRef<FGeneratedProceduralCaptureState> State;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterGeneratedProceduralSubmixCaptureTest,
	"AudioFactory.AudioExporter.Runtime.Diagnostics.GeneratedProceduralSubmixCapture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterGeneratedProceduralSubmixCaptureTest::RunTest(const FString& Parameters)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("NoSound")))
	{
		AddError(TEXT("Generated procedural submix capture is unavailable: this required live-audio regression was launched with -NoSound."));
		return false;
	}

	UWorld* World = AudioExporterRuntimeTestUtils::ResolveAutomationWorld();
	TestNotNull(TEXT("Generated procedural capture should have an editor world."), World);
	if (!World)
	{
		return false;
	}

	Audio::FMixerDevice* MixerDevice = FAudioDeviceManager::GetAudioMixerDeviceFromWorldContext(World);
	TestNotNull(TEXT("Generated procedural capture should resolve an audio mixer device."), MixerDevice);
	if (!MixerDevice)
	{
		return false;
	}

	constexpr int32 SampleRate = 48000;
	constexpr int32 NumChannels = 2;
	constexpr int32 NumFrames = SampleRate;
	constexpr float Frequency = 440.0f;
	constexpr float Amplitude = 0.5f;

	TArray<int16> Pcm;
	Pcm.SetNumUninitialized(NumFrames * NumChannels);
	for (int32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
	{
		const float Sample = Amplitude * FMath::Sin(2.0f * PI * Frequency * static_cast<float>(FrameIndex) / static_cast<float>(SampleRate));
		const int16 PcmSample = static_cast<int16>(FMath::Clamp(FMath::RoundToInt(Sample * 32767.0f), -32768, 32767));
		for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
		{
			Pcm[(FrameIndex * NumChannels) + ChannelIndex] = PcmSample;
		}
	}

	USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>(GetTransientPackage(), TEXT("AudioExporterGeneratedProceduralTone"));
	TestNotNull(TEXT("Generated procedural capture should create a procedural wave."), Wave);
	if (!Wave)
	{
		return false;
	}
	Wave->AddToRoot();
	Wave->SetSampleRate(SampleRate);
	Wave->NumChannels = NumChannels;
	Wave->Duration = 1.0f;
	Wave->bLooping = false;
	Wave->VirtualizationMode = EVirtualizationMode::PlayWhenSilent;
	Wave->QueueAudio(reinterpret_cast<const uint8*>(Pcm.GetData()), Pcm.Num() * sizeof(int16));

	UAudioComponent* Component = NewObject<UAudioComponent>(World);
	TestNotNull(TEXT("Generated procedural capture should create an audio component."), Component);
	if (!Component)
	{
		Wave->RemoveFromRoot();
		return false;
	}
	Component->AddToRoot();
	Component->bAutoActivate = false;
	Component->bAutoDestroy = false;
	Component->bAllowSpatialization = false;
	Component->bIsUISound = true;
	Component->bReverb = false;
	Component->bCenterChannelOnly = false;
	Component->bIsPreviewSound = true;
	Component->SetSound(Wave);
	Component->RegisterComponentWithWorld(World);

	USoundSubmix* RecordingSubmix = &MixerDevice->GetMainSubmixObject();
	MixerDevice->StartRecording(RecordingSubmix, 1.0f);
	Component->Play();

	const TSharedRef<AudioExporterRuntimeTestUtils::FGeneratedProceduralCaptureState> State = MakeShared<AudioExporterRuntimeTestUtils::FGeneratedProceduralCaptureState>();
	State->Test = this;
	State->Component = Component;
	State->Wave = Wave;
	State->World = World;
	State->RecordingSubmix = RecordingSubmix;
	State->StartSeconds = FPlatformTime::Seconds();

	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FFinishGeneratedProceduralSubmixCapture(State));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterRuntimeInvalidWorldIsSafeTest,
	"AudioFactory.AudioExporter.Runtime.BPLibraryInvalidWorldIsSafe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterRuntimeInvalidWorldIsSafeTest::RunTest(const FString& Parameters)
{
	FAudioExporterConfig Config;
	// The literally empty array hits the BPLibrary's raw empty-array refusal before the world
	// resolution, so the honest expected warning is the product's actual empty-array message
	// (AudioExporterBPLibrary.cpp: ExportSoundsToWavIdentified refuses before ResolveWorld).
	AddExpectedError(TEXT("UAudioExporterBPLibrary::ExportSoundsToWav: The requested sound array was empty."), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("Null world context should report that no export is active."), UAudioExporterBPLibrary::IsExporting(nullptr));
	UAudioExporterBPLibrary::CancelExport(nullptr);
	UAudioExporterBPLibrary::ExportSoundsToWav(nullptr, TArray<USoundBase*>(), Config, TEXT(""));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterRuntimeSubsystemAvailabilityTest,
	"AudioFactory.AudioExporter.Runtime.SubsystemAvailability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterRuntimeSubsystemAvailabilityTest::RunTest(const FString& Parameters)
{
	UAudioExporterRuntimeSubsystem* RuntimeSubsystem = UAudioExporterBPLibrary::GetRuntimeSubsystem();
	TestNotNull(TEXT("AudioExporter runtime subsystem should be registered as an engine subsystem."), RuntimeSubsystem);
	return RuntimeSubsystem != nullptr;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterRealtimeOverrideLeaseRestoresAndRespectsForeignOwnerTest,
	"AudioFactory.AudioExporter.Runtime.RealtimeOverrideLeaseRestoresAndRespectsForeignOwner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterRealtimeOverrideLeaseRestoresAndRespectsForeignOwnerTest::RunTest(const FString& Parameters)
{
	IConsoleVariable* DisableAppVolumeCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("au.DisableAppVolume"));
	TestNotNull(TEXT("au.DisableAppVolume should exist."), DisableAppVolumeCVar);
	if (!DisableAppVolumeCVar)
	{
		return false;
	}

	const int32 OriginalDisableAppVolume = DisableAppVolumeCVar->GetInt();

	// Pre-seed the cvar at the product's own tier with a foreign tag. On UE 5.6/5.7 AF_CVAR_SETBY_TEMP
	// is ECVF_SetByCode and Unset on that tier is a silent no-op whenever any earlier Code-tier
	// writer touched the cvar, which is the exact restore trap this shared lease replaces.
	DisableAppVolumeCVar->Set(0, AF_TEST_CVAR_SETBY_TEMP, TEXT("LaneBTestCodeWriter"));

	// 1. A clean lease saves the pre-session value and restores it explicitly on release.
	FAudioRendererRealtime::ApplyAudioOverridesForTesting();
	TestEqual(TEXT("au.DisableAppVolume should read 1 while the realtime override lease holds it."), DisableAppVolumeCVar->GetInt(), 1);
	FAudioRendererRealtime::RestoreAudioOverridesForTesting();
	TestEqual(TEXT("au.DisableAppVolume should read 0 after the realtime override lease released it."), DisableAppVolumeCVar->GetInt(), 0);
	IConsoleVariable* OwnersCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("AudioFactory.SharedOverride.Owners.DisableAppVolume"));
	TestNotNull(TEXT("AudioFactory.SharedOverride.Owners.DisableAppVolume should exist after the first lease."), OwnersCVar);
	if (!OwnersCVar)
	{
		return false;
	}
	TestEqual(TEXT("AudioFactory.SharedOverride.Owners.DisableAppVolume should read 0 after the realtime override lease released it."), OwnersCVar->GetInt(), 0);

	// 2. A foreign owner (the shared offline renderer's lease) must survive this product's
	// apply/restore pair untouched.
	OwnersCVar->Set(1, ECVF_SetByCode, TEXT("LaneBTestCodeWriter"));
	IConsoleVariable* SavedCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("AudioFactory.SharedOverride.Saved.DisableAppVolume"));
	TestNotNull(TEXT("AudioFactory.SharedOverride.Saved.DisableAppVolume should exist after the first lease."), SavedCVar);
	if (!SavedCVar)
	{
		OwnersCVar->Set(0, ECVF_SetByCode, TEXT("LaneBTestCodeWriter"));
		return false;
	}
	SavedCVar->Set(0, ECVF_SetByCode, TEXT("LaneBTestCodeWriter"));
	DisableAppVolumeCVar->Set(1, AF_TEST_CVAR_SETBY_TEMP, TEXT("LaneBTestCodeWriter"));

	FAudioRendererRealtime::ApplyAudioOverridesForTesting();
	TestEqual(TEXT("AudioFactory.SharedOverride.Owners.DisableAppVolume should read 2 after this product joined a foreign owner."), OwnersCVar->GetInt(), 2);
	TestEqual(TEXT("au.DisableAppVolume should still read 1 after this product joined a foreign owner."), DisableAppVolumeCVar->GetInt(), 1);
	FAudioRendererRealtime::RestoreAudioOverridesForTesting();
	TestEqual(TEXT("au.DisableAppVolume must still read 1 while the foreign owner remains."), DisableAppVolumeCVar->GetInt(), 1);
	TestEqual(TEXT("AudioFactory.SharedOverride.Owners.DisableAppVolume must still read 1 while the foreign owner remains."), OwnersCVar->GetInt(), 1);

	// 3. Cleanup: hand the lease bookkeeping and the cvar back exactly as they were found.
	OwnersCVar->Set(0, ECVF_SetByCode, TEXT("LaneBTestCodeWriter"));
	SavedCVar->Set(0, ECVF_SetByCode, TEXT("LaneBTestCodeWriter"));
	DisableAppVolumeCVar->Set(OriginalDisableAppVolume, AF_TEST_CVAR_SETBY_TEMP, TEXT("LaneBTestCodeWriter"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterSettingsEffectiveExportDirTest,
	"AudioFactory.AudioExporter.Runtime.Settings.EffectiveExportDirectory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterSettingsEffectiveExportDirTest::RunTest(const FString& Parameters)
{
	// The effective export base directory (used by the "Logs" button) must follow a custom
	// ExportDirectory when one is set, and otherwise fall back to the default Saved/AudioExporter.
	UAudioExporterSettings* Settings = NewObject<UAudioExporterSettings>(GetTransientPackage());
	TestNotNull(TEXT("Transient AudioExporter settings object should be created."), Settings);
	if (!Settings)
	{
		return false;
	}

	Settings->ExportDirectory.Empty();
	const FString DefaultDir = FPaths::ProjectSavedDir() / TEXT("AudioExporter");
	TestEqual(TEXT("An empty ExportDirectory should resolve to the default Saved/AudioExporter location."),
		Settings->ResolveEffectiveExportBaseDirectory(), DefaultDir);

	const FString CustomDir = FPaths::ProjectSavedDir() / TEXT("Automation") / TEXT("CustomExportDir");
	Settings->ExportDirectory = CustomDir;
	TestEqual(TEXT("A custom ExportDirectory should resolve to that directory."),
		Settings->ResolveEffectiveExportBaseDirectory(), CustomDir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterUniqueSessionDirectoryTest,
	"AudioFactory.AudioExporter.Runtime.Lifecycle.UniqueSessionDirectory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterUniqueSessionDirectoryTest::RunTest(const FString& Parameters)
{
	// The 1-second-granular timestamp folder can collide on a cancel-and-restart within the
	// same second. ResolveUniqueSessionDirectory must return a non-colliding folder in that case.
	// Exercise the production default session shape directly: Saved/AudioExporter/<timestamp>.
	const FString BaseDir = FPaths::ProjectSavedDir() / TEXT("AudioExporter") / FString::Printf(
		TEXT("Automation_UniqueSessionDirectory_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*BaseDir, false, true);
	};
	if (!TestTrue(TEXT("The default-root collision fixture should be creatable."),
		IFileManager::Get().MakeDirectory(*BaseDir, true)))
	{
		return false;
	}

	// A folder that does not yet exist is returned unchanged.
	const FString FreshDir = BaseDir / TEXT("Session_Fresh");
	TestEqual(TEXT("A non-existent session directory should be returned unchanged."),
		FAudioExportPipeline::ResolveUniqueSessionDirectory(FreshDir, TEXT("request-fresh")), FreshDir);

	// Once the base folder exists, resolution must pick a suffixed sibling that does not exist.
	const FString CollidingDir = BaseDir / TEXT("Session_Collide");
	TestTrue(TEXT("The colliding session directory fixture should be created."),
		IFileManager::Get().MakeDirectory(*CollidingDir, true));

	const FString StableRequestFacts = TEXT("request-collision|/Game/Test_1kHz|Collision");
	const FString FirstResolved = FAudioExportPipeline::ResolveUniqueSessionDirectory(CollidingDir, StableRequestFacts);
	TestNotEqual(TEXT("An existing session directory must not be reused."), FirstResolved, CollidingDir);
	TestFalse(TEXT("The resolved unique directory should not already exist."),
		IFileManager::Get().DirectoryExists(*FirstResolved));
	TestTrue(TEXT("The resolved unique directory should be a suffixed sibling of the requested folder."),
		FirstResolved.StartsWith(CollidingDir));
	TestTrue(TEXT("The first collision suffix should be the readable numeric _2 suffix."),
		FirstResolved == CollidingDir + TEXT("_2"));

	// After creating the first suffixed folder, resolution must advance to the next free suffix.
	TestTrue(TEXT("The first suffixed session directory should be creatable."),
		IFileManager::Get().MakeDirectory(*FirstResolved, true));
	const FString SecondResolved = FAudioExportPipeline::ResolveUniqueSessionDirectory(CollidingDir, StableRequestFacts);
	TestNotEqual(TEXT("Resolution should skip the now-existing suffixed directory."), SecondResolved, CollidingDir);
	TestNotEqual(TEXT("Resolution should not reuse the first suffixed directory."), SecondResolved, FirstResolved);
	TestFalse(TEXT("The second resolved directory should not already exist."),
		IFileManager::Get().DirectoryExists(*SecondResolved));
	TestTrue(TEXT("The second collision suffix should advance to the readable numeric _3 suffix."),
		SecondResolved == CollidingDir + TEXT("_3"));
	TestTrue(TEXT("The second suffixed session directory should be creatable."),
		IFileManager::Get().MakeDirectory(*SecondResolved, true));

	// The readable collision range is intentionally bounded at _999. Plant every readable suffix
	// so the resolver must take its stable request-hash fallback instead of choosing a random name.
	TArray<FString> PlantedPaths;
	PlantedPaths.Reserve(998);
	PlantedPaths.Add(CollidingDir);
	PlantedPaths.Add(FirstResolved);
	PlantedPaths.Add(SecondResolved);
	for (int32 Suffix = 4; Suffix < 1000; ++Suffix)
	{
		const FString PlantedPath = FString::Printf(TEXT("%s_%d"), *CollidingDir, Suffix);
		TestTrue(TEXT("Every readable collision suffix fixture should be creatable."),
			IFileManager::Get().MakeDirectory(*PlantedPath, true));
		PlantedPaths.Add(PlantedPath);
	}

	const FString FallbackResolved = FAudioExportPipeline::ResolveUniqueSessionDirectory(CollidingDir, StableRequestFacts);
	TestFalse(TEXT("The stable fallback directory must not already exist."),
		IFileManager::Get().DirectoryExists(*FallbackResolved));
	TestEqual(TEXT("The stable fallback must remain beside the requested directory."),
		FPaths::GetPath(FallbackResolved), FPaths::GetPath(CollidingDir));
	for (const FString& PlantedPath : PlantedPaths)
	{
		TestNotEqual(TEXT("The stable fallback must not reuse any planted readable collision path."),
			FallbackResolved, PlantedPath);
	}

	const FString FallbackPrefix = FPaths::GetCleanFilename(CollidingDir) + TEXT("_");
	const FString FallbackLeaf = FPaths::GetCleanFilename(FallbackResolved);
	TestTrue(TEXT("The stable fallback leaf must retain the requested base name."),
		FallbackLeaf.StartsWith(FallbackPrefix));
	const FString StableFallbackAgain = FAudioExportPipeline::ResolveUniqueSessionDirectory(CollidingDir, StableRequestFacts);
	TestEqual(TEXT("The same request and filesystem state must produce the same stable fallback."),
		StableFallbackAgain, FallbackResolved);

	// The hash is derived from request facts, not the timestamped base directory. A second base that
	// represents the same request at another timestamp must therefore receive the same hash suffix.
	const FString LaterCollidingDir = BaseDir / TEXT("Session_Collide_2026-08-27_12-00-01");
	TestTrue(TEXT("The later timestamp collision fixture should be created."),
		IFileManager::Get().MakeDirectory(*LaterCollidingDir, true));
	for (int32 Suffix = 2; Suffix < 1000; ++Suffix)
	{
		const FString PlantedPath = FString::Printf(TEXT("%s_%d"), *LaterCollidingDir, Suffix);
		TestTrue(TEXT("Every later timestamp readable collision suffix fixture should be creatable."),
			IFileManager::Get().MakeDirectory(*PlantedPath, true));
	}
	const FString LaterFallbackResolved = FAudioExportPipeline::ResolveUniqueSessionDirectory(LaterCollidingDir, StableRequestFacts);
	const FString FallbackSuffix = FPaths::GetCleanFilename(FallbackResolved).Mid(FPaths::GetCleanFilename(CollidingDir).Len());
	const FString LaterFallbackSuffix = FPaths::GetCleanFilename(LaterFallbackResolved).Mid(FPaths::GetCleanFilename(LaterCollidingDir).Len());
	TestEqual(TEXT("Equal request facts must produce the same hash suffix across timestamped base directories."),
		LaterFallbackSuffix, FallbackSuffix);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterPipelineDefaultStateTest,
	"AudioFactory.AudioExporter.Runtime.PipelineDefaultState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterPipelineDefaultStateTest::RunTest(const FString& Parameters)
{
	FAudioExportPipeline Pipeline;
	TestFalse(TEXT("A fresh AudioExporter pipeline should not be exporting."), Pipeline.IsExporting());
	TestEqual(TEXT("A fresh AudioExporter pipeline should not report an export path."), Pipeline.GetExportPath(), FString());
	return true;
}

// KILLS: a cancel that stops delivering its structured result -- no OnExportResult broadcast, a
// duplicate delivery on the second cancel call, or a delivered outcome other than Cancelled --
// now fails this test through the previously unasserted listener.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterPipelineCancelTerminalOnceTest,
	"AudioFactory.AudioExporter.Runtime.Lifecycle.CancelReturnsAndBroadcastsOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterPipelineCancelTerminalOnceTest::RunTest(const FString& Parameters)
{
	const FString OutputDirectory = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("Lifecycle_CancelTerminalOnce"));
	const TSharedRef<FAudioExportPipeline> Pipeline = MakeShared<FAudioExportPipeline>();

	int32 TerminalEvents = 0;
	FString TerminalPath = TEXT("unset");
	int32 ResultEvents = 0;
	FAudioExportResult TerminalResult;
	Pipeline->OnExportFinished.AddLambda([&TerminalEvents, &TerminalPath](const FString& InPath)
	{
		++TerminalEvents;
		TerminalPath = InPath;
	});
	Pipeline->OnExportResult.AddLambda([&ResultEvents, &TerminalResult](const FAudioExportResult& InResult)
	{
		++ResultEvents;
		TerminalResult = InResult;
	});

	Pipeline->BeginLifecycleTestSession(OutputDirectory);
	const double StartSeconds = FPlatformTime::Seconds();
	Pipeline->CancelExporting(false);
	const double ElapsedSeconds = FPlatformTime::Seconds() - StartSeconds;
	Pipeline->CancelExporting(false);

	TestTrue(TEXT("AudioExporter cancel should return within the fixed timeout budget."), ElapsedSeconds < 0.5);
	TestFalse(TEXT("AudioExporter pipeline should be inactive before cancel terminal listeners run."), Pipeline->IsExporting());
	TestEqual(TEXT("AudioExporter cancel should emit exactly one terminal event."), TerminalEvents, 1);
	TestEqual(TEXT("AudioExporter cancel terminal path should be empty."), TerminalPath, FString());
	TestEqual(TEXT("AudioExporter cancel should deliver exactly one structured result event."), ResultEvents, 1);
	TestEqual(TEXT("AudioExporter cancel structured result should report the exact Cancelled outcome."), TerminalResult.Outcome, EAudioExportOutcome::Cancelled);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterPipelineFinalEmptyBatchCompletesTest,
	"AudioFactory.AudioExporter.Runtime.Lifecycle.FinalEmptyBatchCompletesInactive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterPipelineFinalEmptyBatchCompletesTest::RunTest(const FString& Parameters)
{
	const FString OutputDirectory = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("Lifecycle_FinalEmptyBatchCompletes"));
	const TSharedRef<FAudioExportPipeline> Pipeline = MakeShared<FAudioExportPipeline>();

	int32 TerminalEvents = 0;
	bool bWasInactiveDuringTerminal = false;
	FString TerminalPath;
	Pipeline->OnExportFinished.AddLambda([&Pipeline, &TerminalEvents, &bWasInactiveDuringTerminal, &TerminalPath](const FString& InPath)
	{
		++TerminalEvents;
		bWasInactiveDuringTerminal = !Pipeline->IsExporting();
		TerminalPath = InPath;
	});

	Pipeline->BeginLifecycleTestSession(OutputDirectory);
	AddExpectedError(TEXT("Audio export finished without writing any files."), EAutomationExpectedErrorFlags::Contains, 1);
	Pipeline->CompleteFinalEmptyBatchForTesting();

	TestEqual(TEXT("Final empty-buffer cleanup should emit exactly one completion event."), TerminalEvents, 1);
	TestTrue(TEXT("Final empty-buffer completion should fire after the pipeline is inactive."), bWasInactiveDuringTerminal);
	TestTrue(TEXT("Final empty-buffer failure should report an empty legacy path."), TerminalPath.IsEmpty());
	TestFalse(TEXT("Final empty-buffer cleanup should leave the pipeline inactive."), Pipeline->IsExporting());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterDurableTerminalLogIdentifiesEmptyAndCancelledRunsTest,
	"AudioFactory.AudioExporter.Runtime.Lifecycle.DurableTerminalLogIdentifiesEmptyAndCancelledRuns",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterDurableTerminalLogIdentifiesEmptyAndCancelledRunsTest::RunTest(const FString& Parameters)
{
	{
		const TSharedRef<FAudioExportPipeline> Pipeline = MakeShared<FAudioExportPipeline>();
		const FString OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("DurableTerminalLog_Empty"));
		FAudioExporterConfig Config;
		Config.bExportTextLog = false;

		AudioExporterRuntimeTestUtils::FAudioExporterLogCapture Capture;
		Pipeline->BeginLifecycleTestSession(TArray<FAudioExportSoundEntry>(), Config, OutputRoot);
		Pipeline->CompleteFinalEmptyBatchForTesting();

		// Belt-and-braces before the first capture read: drain any still-buffered lines
		// (blocking idle flush, engine FOutputDeviceRedirector.cpp:590-608) so the asserts
		// below never depend on redirector flush timing.
		GLog->FlushThreadedLogs();
		TestTrue(TEXT("An empty terminal log must identify the exact Failed outcome."),
			Capture.Contains(TEXT("[AudioExporter][Terminal] Outcome=Failed")));
		TestTrue(TEXT("An empty terminal log must render its unavailable path explicitly."),
			Capture.Contains(TEXT("Path=unavailable")));
		TestFalse(TEXT("An empty terminal log must not render a blank path value."),
			Capture.Contains(TEXT("Path= ")));
	}

	{
		const TSharedRef<FAudioExportPipeline> Pipeline = MakeShared<FAudioExportPipeline>();
		const FString OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("DurableTerminalLog_Cancelled"));
		AudioExporterRuntimeTestUtils::FAudioExporterLogCapture Capture;
		Pipeline->BeginLifecycleTestSession(OutputRoot);
		Pipeline->CancelExporting(false);

		// Same belt-and-braces drain before the first capture read of this block
		// (blocking idle flush, engine FOutputDeviceRedirector.cpp:590-608).
		GLog->FlushThreadedLogs();
		TestTrue(TEXT("A cancelled terminal log must identify the exact Cancelled outcome."),
			Capture.Contains(TEXT("[AudioExporter][Terminal] Outcome=Cancelled")));
		TestTrue(TEXT("A cancelled terminal log must render its unavailable path explicitly."),
			Capture.Contains(TEXT("Path=unavailable")));
		TestFalse(TEXT("A cancelled terminal log must not render a blank path value."),
			Capture.Contains(TEXT("Path= ")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterRuntimeSubsystemInvalidWorldIsSafeTest,
	"AudioFactory.AudioExporter.Runtime.SubsystemInvalidWorldIsSafe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterRuntimeSubsystemInvalidWorldIsSafeTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("UAudioExporterRuntimeSubsystem::EnsurePipeline: A valid world is required to start audio export."), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("UAudioExporterRuntimeSubsystem::StartExport: Runtime export pipeline was not available."), EAutomationExpectedErrorFlags::Contains, 1);

	UAudioExporterRuntimeSubsystem* Subsystem = NewObject<UAudioExporterRuntimeSubsystem>(GetTransientPackage());
	TestNotNull(TEXT("Runtime subsystem test object should be created."), Subsystem);
	if (!Subsystem)
	{
		return false;
	}

	const bool bStarted = Subsystem->StartExport(nullptr, TArray<FAudioExportSoundEntry>(), FAudioExporterConfig(), TEXT(""));
	TestFalse(TEXT("StartExport should reject a null world."), bStarted);
	TestFalse(TEXT("Rejected runtime export requests should leave the subsystem idle."), Subsystem->IsExporting());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterRealSoundCueRuntimePipelineExportTest,
	"AudioFactory.AudioExporter.Runtime.RealPipeline.SoundCue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterRealSoundCueRuntimePipelineExportTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<AudioExporterRuntimeTestUtils::FRealPipelineExportState> State =
		AudioExporterRuntimeTestUtils::StartRealPipelineExport(*this, TEXT("/Game/SoundCueOneShot.SoundCueOneShot"), TEXT("SoundCue"));
	if (!State.IsValid())
	{
		// The helper hard-fails (AddError'd) when the environment or a required fixture is
		// unavailable; there is no skip path, so the test must fail here too.
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FWaitForAudioExporterRealPipelineExport(State.ToSharedRef()));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FVerifyAudioExporterRealPipelineExport(State.ToSharedRef()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterRealMetaSoundRuntimePipelineExportTest,
	"AudioFactory.AudioExporter.Runtime.RealPipeline.MetaSound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

	bool FAudioExporterRealMetaSoundRuntimePipelineExportTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<AudioExporterRuntimeTestUtils::FRealPipelineExportState> State =
		AudioExporterRuntimeTestUtils::StartRealPipelineExport(*this, TEXT("/Game/MetaSoundOneShot.MetaSoundOneShot"), TEXT("MetaSound"));
	if (!State.IsValid())
	{
		// The helper hard-fails (AddError'd) when the environment or a required fixture is
		// unavailable; there is no skip path, so the test must fail here too.
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FWaitForAudioExporterRealPipelineExport(State.ToSharedRef()));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FVerifyAudioExporterRealPipelineExport(State.ToSharedRef()));
	return true;
}

// Regression coverage. This test drives the tracked 2 s, 440 Hz EdgeUnity SoundWave through
// the real subsystem StartExport call. The fixture is imported by the suite's edge QA and missing
// content is a loud failure, never a skip. It then reopens both produced WAVs independently. The
// control capture uses the fixture's authored
// settings; the second capture applies volume, pitch, and a real source-effect delay with tails.
// The comparison proves the authored ratios from raw bytes, never from production-reported values.
constexpr double SoundWaveRealtimeBoundedWaitSeconds = 15.0;
constexpr const TCHAR* RealtimeSoundWaveContractFixture =
	TEXT("/Game/QA_EDGE_1823904da8fe/EdgeUnity_Stereo48k.EdgeUnity_Stereo48k");

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterRealSoundWaveRuntimePipelineExportTest,
	"AudioFactory.AudioExporter.Runtime.RealPipeline.SoundWave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterRealSoundWaveRuntimePipelineExportTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<AudioExporterRuntimeTestUtils::FRealPipelineExportState> ControlState =
		AudioExporterRuntimeTestUtils::StartRealPipelineExport(*this, RealtimeSoundWaveContractFixture, TEXT("SoundWaveControl"), false);
	if (!ControlState.IsValid())
	{
		// The helper hard-fails (AddError'd) when the environment or a required fixture is
		// unavailable; there is no skip path, so the test must fail here too.
		return false;
	}

	const TSharedRef<TSharedPtr<AudioExporterRuntimeTestUtils::FRealPipelineExportState>> ModifiedStateHolder =
		MakeShared<TSharedPtr<AudioExporterRuntimeTestUtils::FRealPipelineExportState>>();
	const TSharedRef<bool> ModifiedStartAttempted = MakeShared<bool>(false);
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FWaitForAudioExporterRealPipelineExport(ControlState.ToSharedRef(), SoundWaveRealtimeBoundedWaitSeconds));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FVerifySoundWaveRealPipelineExport(ControlState.ToSharedRef()));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FStartRealPipelineExportAfter(
		*this,
		ControlState.ToSharedRef(),
		ModifiedStateHolder,
		ModifiedStartAttempted,
		RealtimeSoundWaveContractFixture,
		TEXT("SoundWaveModified")));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FWaitForRealPipelineExportHolder(ModifiedStateHolder, ModifiedStartAttempted, SoundWaveRealtimeBoundedWaitSeconds));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FVerifyRealPipelineExportHolder(ModifiedStateHolder, ModifiedStartAttempted));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FCompareSoundWaveRealPipelineCaptures(
		*this,
		ControlState.ToSharedRef(),
		ModifiedStateHolder,
		ModifiedStartAttempted));
	return true;
}

// Continuous-vs-source-relative timing regression coverage. The sibling SoundWave contract test above proves ratios; this test
// proves the TIMELINE of the produced WAV: a real attended 2 s 440 Hz SoundWave export through the
// public subsystem StartExport must open on the source's first-frame impulse, keep the authored dry
// span (source-effect tail included, separator never leaked), and carry no separator-sized silent
// gap anywhere. The modified run re-exports through the real source-effect chain (bounded tail), so
// the tail is present AND its file stays attack-aligned. Reverting the batch-clock rebase to
// playback start slides the markers against the listener buffer and fails every timing assert.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterRealSoundWaveTimingTest,
	"AudioFactory.AudioExporter.Runtime.RealPipeline.SoundWaveTiming",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterRealSoundWaveTimingTest::RunTest(const FString& Parameters)
{
	USoundWave* FixtureWave = LoadObject<USoundWave>(nullptr, RealtimeSoundWaveContractFixture, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!TestNotNull(TEXT("Required SoundWave timing fixture should be present in this project."), FixtureWave))
	{
		return false;
	}

	// Hand-computed expectation for the timing bounds: the fixture's OWN authored duration
	// (documented fixture identity: 2 s, 440 Hz, stereo, 48 kHz). Nothing from the export pipeline
	// feeds the expected values below.
	const double FixtureDryDurationSeconds = static_cast<double>(FixtureWave->GetDuration());
	TestTrue(FString::Printf(TEXT("The timing fixture should carry a sane authored dry span; measured %.6f s."), FixtureDryDurationSeconds),
		FixtureDryDurationSeconds > 1.0 && FixtureDryDurationSeconds < 4.0);

	const TSharedPtr<AudioExporterRuntimeTestUtils::FRealPipelineExportState> ControlState =
		AudioExporterRuntimeTestUtils::StartRealPipelineExport(*this, RealtimeSoundWaveContractFixture, TEXT("SoundWaveTimingControl"), false);
	if (!ControlState.IsValid())
	{
		// The helper hard-fails (AddError'd) when the environment or a required fixture is
		// unavailable; there is no skip path, so the test must fail here too.
		return false;
	}

	const TSharedRef<TSharedPtr<AudioExporterRuntimeTestUtils::FRealPipelineExportState>> ModifiedStateHolder =
		MakeShared<TSharedPtr<AudioExporterRuntimeTestUtils::FRealPipelineExportState>>();
	const TSharedRef<bool> ModifiedStartAttempted = MakeShared<bool>(false);
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FWaitForAudioExporterRealPipelineExport(ControlState.ToSharedRef(), SoundWaveRealtimeBoundedWaitSeconds));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FVerifySoundWaveRealPipelineExport(ControlState.ToSharedRef()));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FStartRealPipelineExportAfter(
		*this,
		ControlState.ToSharedRef(),
		ModifiedStateHolder,
		ModifiedStartAttempted,
		RealtimeSoundWaveContractFixture,
		TEXT("SoundWaveTimingTail")));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FWaitForRealPipelineExportHolder(ModifiedStateHolder, ModifiedStartAttempted, SoundWaveRealtimeBoundedWaitSeconds));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FVerifyRealPipelineExportHolder(ModifiedStateHolder, ModifiedStartAttempted));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FCompareSoundWaveRealPipelineCaptures(
		*this,
		ControlState.ToSharedRef(),
		ModifiedStateHolder,
		ModifiedStartAttempted));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FVerifySoundWaveExportTiming(
		ControlState.ToSharedRef(),
		ModifiedStateHolder,
		ModifiedStartAttempted,
		FixtureDryDurationSeconds));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterVariationExtractionFixtureTest,
	"AudioFactory.AudioExporter.Runtime.Utility.ExtractVariationBufferFixture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterVariationExtractionFixtureTest::RunTest(const FString& Parameters)
{
	const TArray<float> BigBuffer = {
		0.f, 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f,
		8.f, 9.f, 10.f, 11.f, 12.f, 13.f, 14.f, 15.f
	};

	const TArray<float> VariationBuffer = FAudioExporterUtils::ExtractVariationBuffer(BigBuffer, 4, 2, 0.5, 1.5, 1.0, false);
	TestEqual(TEXT("Variation extraction should copy the expected stereo sample count."), VariationBuffer.Num(), 8);
	TestEqual(TEXT("Variation extraction should begin at the expected sample."), VariationBuffer[0], 4.0f);
	TestEqual(TEXT("Variation extraction should preserve the copied sample sequence."), VariationBuffer.Last(), 11.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterProcessAndSaveVariationFixtureTest,
	"AudioFactory.AudioExporter.Runtime.Utility.ProcessAndSaveVariationFixture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterProcessAndSaveVariationFixtureTest::RunTest(const FString& Parameters)
{
	TArray<float> Buffer = AudioExporterRuntimeTestUtils::MakeSineBuffer(240, 2, 48000, 440.0f, 0.5f);
	float GainAppliedDb = 0.0f;
	float PeakDb = 0.0f;
	float RMSDb = 0.0f;
	FString OutputFilePath;

	const FString OutputDirectory = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("ProcessAndSaveVariationFixture"));
	const bool bSaved = FAudioExporterUtils::ProcessAndSaveVariation(
		Buffer,
		TEXT("FixtureTone"),
		TEXT("Stereo"),
		0,
		OutputDirectory,
		48000,
		2,
		false,
		true,
		GainAppliedDb,
		PeakDb,
		RMSDb,
		OutputFilePath);

	TestTrue(TEXT("ProcessAndSaveVariation should save the generated WAV fixture."), bSaved);
	TestTrue(TEXT("Saved fixture WAV should exist on disk."), IFileManager::Get().FileExists(*OutputFilePath));

	TArray<uint8> WavData;
	TestTrue(TEXT("Saved fixture WAV should load back into memory."), FFileHelper::LoadFileToArray(WavData, *OutputFilePath));

	AudioExporterRuntimeTestUtils::FWavHeaderInfo Header;
	TestTrue(TEXT("Saved fixture WAV header should parse successfully."), AudioExporterRuntimeTestUtils::ParseWaveHeader(WavData, Header));
	TestEqual(TEXT("Saved fixture should preserve stereo channel count."), Header.NumChannels, static_cast<uint16>(2));
	TestEqual(TEXT("Saved fixture should preserve sample rate."), Header.SampleRate, static_cast<uint32>(48000));
	TestEqual(TEXT("Saved fixture should emit 16-bit PCM WAV."), Header.BitsPerSample, static_cast<uint16>(16));
	TestTrue(TEXT("Saved fixture should contain PCM data."), Header.DataSize > 0);
	TestTrue(TEXT("Saved fixture PCM should contain audible signal."), FMath::Abs(AudioExporterRuntimeTestUtils::ReadPcmSample(WavData, 2)) > 0);
	TestTrue(TEXT("Non-normalized fixture should not apply gain."), FMath::IsNearlyZero(GainAppliedDb, 0.001f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterExplicitSoundWaveLayoutTest,
	"AudioFactory.AudioExporter.Runtime.Utility.ExplicitSoundWaveLayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterExplicitSoundWaveLayoutTest::RunTest(const FString& Parameters)
{
	USoundWave* SurroundWave = NewObject<USoundWave>();
	SurroundWave->NumChannels = 6;
	SurroundWave->ChannelSizes.Init(0, SPEAKER_Count);
	SurroundWave->ChannelSizes[SPEAKER_FrontLeft] = 1;
	SurroundWave->ChannelSizes[SPEAKER_FrontRight] = 1;
	SurroundWave->ChannelSizes[SPEAKER_FrontCenter] = 1;
	SurroundWave->ChannelSizes[SPEAKER_LowFrequency] = 1;
	SurroundWave->ChannelSizes[SPEAKER_LeftSurround] = 1;
	SurroundWave->ChannelSizes[SPEAKER_RightSurround] = 1;

	TestEqual(TEXT("Explicit 5.1 speaker identities should resolve to the truthful side-speaker layout."),
		FAudioExporterUtils::ResolveSoundWaveChannelLayout(SurroundWave),
		AudioExporter::EAudioChannelLayout::Surround5_1);

	SurroundWave->ChannelSizes[SPEAKER_RightSurround] = 0;
	TestEqual(TEXT("An incomplete six-channel speaker map must remain unavailable instead of being inferred by count."),
		FAudioExporterUtils::ResolveSoundWaveChannelLayout(SurroundWave),
		AudioExporter::EAudioChannelLayout::Unknown);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterGainSafetyCapIsReportedTest,
	"AudioFactory.AudioExporter.Runtime.Utility.GainSafetyCapIsReported",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterGainSafetyCapIsReportedTest::RunTest(const FString& Parameters)
{
	TArray<float> Buffer = AudioExporterRuntimeTestUtils::MakeSineBuffer(64, 1, 48000, 440.0f, 0.002f);
	float GainAppliedDb = 0.0f;
	float PeakDb = 0.0f;
	float RMSDb = 0.0f;
	FString OutputFilePath;
	float GainRequestedDb = 0.0f;
	bool bGainLimited = false;
	const FString OutputDirectory = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("GainSafetyCapPlainSuccess"));
	AudioExporterRuntimeTestUtils::FAudioExporterLogCapture Capture;

	const bool bSaved = FAudioExporterUtils::ProcessAndSaveVariation(
		Buffer,
		TEXT("GainSafetyCapFixture"),
		TEXT("GainSafetyCap"),
		0,
		OutputDirectory,
		48000,
		1,
		true,
		true,
		GainAppliedDb,
		PeakDb,
		RMSDb,
		OutputFilePath,
		nullptr,
		FGuid(),
		nullptr,
		FString(),
		&GainRequestedDb,
		&bGainLimited);

	TestTrue(TEXT("A gain-limited normalization must remain a successful written-file save."), bSaved);
	TestTrue(TEXT("A gain-limited normalization must expose its limited status."), bGainLimited);
	TestTrue(TEXT("A gain-limited normalization must expose the requested gain."), GainRequestedDb > GainAppliedDb);
	TestTrue(TEXT("A gain-limited normalization must report the applied gain."), GainAppliedDb > 0.0f);
	TestFalse(TEXT("A gain-limited normalization must expose its saved output path."), OutputFilePath.IsEmpty());
	TestTrue(TEXT("A gain-limited normalization must write its WAV file."), IFileManager::Get().FileExists(*OutputFilePath));
	TestTrue(TEXT("The gain-limited success log must record that the gain was limited."),
		Capture.Contains(TEXT("Normalization succeeded with gain limiting.")));
	TestTrue(TEXT("The gain-limited success log must record the requested gain."), Capture.Contains(TEXT("Requested=")));
	TestTrue(TEXT("The gain-limited success log must record the applied gain."), Capture.Contains(TEXT("Applied=")));
	TestEqual(TEXT("A gain-limited written-file success should not be escalated to a warning or error."),
		Capture.GetWarningOrErrorCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterProcessAndSaveVariationReportsFailureReasonTest,
	"AudioFactory.AudioExporter.Runtime.Utility.ProcessAndSaveVariationReportsFailureReason",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterProcessAndSaveVariationReportsFailureReasonTest::RunTest(const FString& Parameters)
{
	TArray<float> Buffer = AudioExporterRuntimeTestUtils::MakeSineBuffer(64, 1, 48000, 440.0f, 0.25f);
	float GainAppliedDb = 0.0f;
	float PeakDb = 0.0f;
	float RMSDb = 0.0f;
	FString OutputFilePath;
	FString FailureReason;

	const FString OutputDirectory = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("ProcessAndSaveVariationReportsFailureReason"));
	const bool bSaved = FAudioExporterUtils::ProcessAndSaveVariation(
		Buffer,
		TEXT("SkippedFixture"),
		TEXT("Mono"),
		0,
		OutputDirectory,
		48000,
		1,
		false,
		false,
		GainAppliedDb,
		PeakDb,
		RMSDb,
		OutputFilePath,
		&FailureReason);

	TestFalse(TEXT("ProcessAndSaveVariation should report false when saving is disabled."), bSaved);
	TestFalse(TEXT("Failure reason should be populated when saving is disabled."), FailureReason.IsEmpty());
	TestTrue(TEXT("A save-vetoed output path must remain unavailable."), OutputFilePath.IsEmpty());
	TestFalse(TEXT("Disabled save should not write a WAV file."), IFileManager::Get().FileExists(*OutputFilePath));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterDuplicateRequestIdentityProducesDistinctRowsTest,
	"AudioFactory.AudioExporter.Runtime.Utility.DuplicateRequestIdentityProducesDistinctRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterDuplicateRequestIdentityProducesDistinctRowsTest::RunTest(const FString& Parameters)
{
	USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>(GetTransientPackage(), TEXT("AudioExporterDuplicateRequestTone"));
	TestNotNull(TEXT("The duplicate-request test should create a real source SoundWave."), Wave);
	if (!Wave)
	{
		return false;
	}
	Wave->AddToRoot();

	const TSharedRef<FAudioExportPipeline> Pipeline = MakeShared<FAudioExportPipeline>();
	FAudioExportSoundEntry FirstEntry;
	FirstEntry.Sound = Wave;
	FirstEntry.SoundName = TEXT("AudioExporterDuplicateRequestTone");
	FirstEntry.SourceAssetPath = TEXT("/Temp/AudioExporterDuplicateRequestTone");
	FirstEntry.RelativePath = TEXT("DuplicateRequest");
	FAudioExportSoundEntry SecondEntry = FirstEntry;
	TArray<FAudioExportSoundEntry> Entries;
	Entries.Add(FirstEntry);
	Entries.Add(SecondEntry);

	FAudioExporterConfig Config;
	Config.OneShotVariations = 1;
	Config.LoopVariations = 1;
	Config.MaxLengthSeconds = 5;
	Config.bNormalize = false;
	Config.bMonitor = false;
	Config.bExportTextLog = false;
	const FString OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("DuplicateRequestIdentity"));
	Pipeline->BeginLifecycleTestSession(Entries, Config, OutputRoot);

	TestTrue(TEXT("The duplicate-request fixture session should be active."), Pipeline->IsExporting());
	Pipeline->SimulateVariationFinishedForTesting();
	Pipeline->SimulateVariationFinishedForTesting();

	TestFalse(TEXT("Both duplicate requests should reach one terminal session."), Pipeline->IsExporting());
	const TArray<FAudioExportAssetLog> Logs = Pipeline->GetExportLogsForTesting();
	TestEqual(TEXT("Duplicate requests for one source path must retain two asset rows."), Logs.Num(), 2);

	TArray<FString> WavFiles;
	FString OutputRootFull = FPaths::ConvertRelativePathToFull(OutputRoot);
	FPaths::NormalizeDirectoryName(OutputRootFull);
	IFileManager::Get().FindFilesRecursive(WavFiles, *OutputRootFull, TEXT("*.wav"), true, false);
	TestEqual(TEXT("Duplicate requests for one source path must write two output files."), WavFiles.Num(), 2);

	if (Logs.Num() == 2)
	{
		TestEqual(TEXT("The first duplicate request must retain one terminal variation row."), Logs[0].Variations.Num(), 1);
		TestEqual(TEXT("The second duplicate request must retain one terminal variation row."), Logs[1].Variations.Num(), 1);
		if (Logs[0].Variations.Num() == 1 && Logs[1].Variations.Num() == 1)
		{
			const FAudioExportLogEntry& FirstRow = Logs[0].Variations[0];
			const FAudioExportLogEntry& SecondRow = Logs[1].Variations[0];
			TestEqual(TEXT("The first duplicate request should be a successful row."), FirstRow.Status, EAudioExportRowStatus::Succeeded);
			TestEqual(TEXT("The second duplicate request should be a successful row."), SecondRow.Status, EAudioExportRowStatus::Succeeded);
			TestFalse(TEXT("The first duplicate request should expose its saved output path."), FirstRow.OutputFilePath.IsEmpty());
			TestFalse(TEXT("The second duplicate request should expose its saved output path."), SecondRow.OutputFilePath.IsEmpty());
			TestTrue(TEXT("The first duplicate request's output file should exist."), IFileManager::Get().FileExists(*FirstRow.OutputFilePath));
			TestTrue(TEXT("The second duplicate request's output file should exist."), IFileManager::Get().FileExists(*SecondRow.OutputFilePath));
			TestNotEqual(TEXT("Distinct request identities must claim distinct output paths."), FirstRow.OutputFilePath, SecondRow.OutputFilePath);
		}
	}

	Wave->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterAtomicSaveFailureLeavesNoArtifactsTest,
	"AudioFactory.AudioExporter.Runtime.Utility.AtomicSaveFailureLeavesNoArtifacts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterAtomicSaveFailureLeavesNoArtifactsTest::RunTest(const FString& Parameters)
{
	const FString OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("AtomicSaveFailureLeavesNoArtifacts"));
	// WAVs are written under the session's Audio subfolder, so the blocking destination has to sit
	// where the save will actually land.
	const FString FinalPath = OutputRoot / AudioExporterAudioSubfolder / TEXT("AtomicFailure_1.wav");
	TestTrue(TEXT("The atomic-save failure fixture should create a blocking destination directory."),
		IFileManager::Get().MakeDirectory(*FinalPath, true));

	TArray<float> Buffer = AudioExporterRuntimeTestUtils::MakeSineBuffer(64, 1, 48000, 440.0f, 0.25f);
	float GainAppliedDb = 0.0f;
	float PeakDb = 0.0f;
	float RMSDb = 0.0f;
	FString OutputFilePath;
	FString FailureReason;
	// The destination directory intentionally makes the atomic move fail, and the assertions below
	// verify the temporary artifact was still cleaned up.
	//
	// KILLS: reverting bDoNotRetryOrError=true on the atomic Move in AudioBufferHandler::SaveToWav.
	// The engine only logs "Error moving file" after exhausting its 10 x 0.5s retry loop
	// (FileManagerGeneric.cpp:330-352). With the flag set it returns before that log, so no such
	// error is emitted and none is expected here. Revert the flag and that error reappears
	// UNEXPECTED, which fails this test automatically -- and the 5s retry stall returns with it.
	AddExpectedError(TEXT("SaveToWav: Failed to atomically move WAV"), EAutomationExpectedErrorFlags::Contains, 1);
	const bool bSaved = FAudioExporterUtils::ProcessAndSaveVariation(
		Buffer,
		TEXT("AtomicFailure"),
		FString(),
		0,
		OutputRoot,
		48000,
		1,
		false,
		true,
		GainAppliedDb,
		PeakDb,
		RMSDb,
		OutputFilePath,
		&FailureReason);

	TestFalse(TEXT("A destination that cannot be claimed must report an atomic-save failure."), bSaved);
	TestFalse(TEXT("An atomic-save failure must provide a failure reason."), FailureReason.IsEmpty());
	TestTrue(TEXT("An atomic-save failure must not expose a successful output path."), OutputFilePath.IsEmpty());
	TestFalse(TEXT("An atomic-save failure must not leave a file at the requested final path."), IFileManager::Get().FileExists(*FinalPath));
	TestTrue(TEXT("The blocking destination must survive the failed save."), IFileManager::Get().DirectoryExists(*FinalPath));

	TArray<FString> PartialFiles;
	IFileManager::Get().FindFilesRecursive(PartialFiles, *OutputRoot, TEXT("*.wav*"), true, false);
	TestEqual(TEXT("An atomic-save failure must leave no final or temporary WAV artifact anywhere."), PartialFiles.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterExistingDestinationSurvivesWithSuffixedOutputTest,
	"AudioFactory.AudioExporter.Runtime.Utility.ExistingDestinationSurvivesWithSuffixedOutput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterExistingDestinationSurvivesWithSuffixedOutputTest::RunTest(const FString& Parameters)
{
	const FString OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("ExistingDestinationSurvives"));
	// The collision has to be planted in the Audio subfolder, which is where saves actually land.
	const FString AudioDir = OutputRoot / AudioExporterAudioSubfolder;
	const FString RequestedPath = AudioDir / TEXT("ExistingDestination_1.wav");
	const FString Sentinel = TEXT("pre-existing destination must survive");
	TestTrue(TEXT("The collision fixture should create the pre-existing destination file."),
		FFileHelper::SaveStringToFile(Sentinel, *RequestedPath));

	TArray<float> Buffer = AudioExporterRuntimeTestUtils::MakeSineBuffer(64, 1, 48000, 440.0f, 0.25f);
	float GainAppliedDb = 0.0f;
	float PeakDb = 0.0f;
	float RMSDb = 0.0f;
	FString OutputFilePath;
	const bool bSaved = FAudioExporterUtils::ProcessAndSaveVariation(
		Buffer,
		TEXT("ExistingDestination"),
		FString(),
		0,
		OutputRoot,
		48000,
		1,
		false,
		true,
		GainAppliedDb,
		PeakDb,
		RMSDb,
		OutputFilePath);

	TestTrue(TEXT("A pre-existing destination must not prevent a successful suffixed save."), bSaved);
	TestNotEqual(TEXT("A pre-existing destination must cause a new output name."), OutputFilePath, RequestedPath);
	TestEqual(TEXT("The first filename collision must use the readable numeric _2 suffix."),
		FPaths::GetCleanFilename(OutputFilePath), FString(TEXT("ExistingDestination_1_2.wav")));
	// Dies if the Audio subfolder is dropped and WAVs go back to the session root beside the report.
	TestEqual(TEXT("Saved WAVs must land in the session's Audio subfolder."),
		FPaths::ConvertRelativePathToFull(FPaths::GetPath(OutputFilePath)),
		FPaths::ConvertRelativePathToFull(AudioDir));
	TestTrue(TEXT("The suffixed output file must exist."), IFileManager::Get().FileExists(*OutputFilePath));
	FString PreservedContents;
	TestTrue(TEXT("The original destination must remain readable after collision resolution."),
		FFileHelper::LoadFileToString(PreservedContents, *RequestedPath));
	TestEqual(TEXT("Collision resolution must preserve the original destination contents."), PreservedContents, Sentinel);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterFilenameCollisionUsesStablePackageHashTest,
	"AudioFactory.AudioExporter.Runtime.Utility.FilenameCollisionUsesStablePackageHash",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterFilenameCollisionUsesStablePackageHashTest::RunTest(const FString& Parameters)
{
	const FString OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("StableFilenameHash"));
	const FString StablePackagePath = TEXT("/Game/Audio/Weapons/Fire.Fire");
	const FString BaseStem = TEXT("Fire_1");
	for (int32 Suffix = 1; Suffix <= 999; ++Suffix)
	{
		const FString Name = Suffix == 1
			? BaseStem + TEXT(".wav")
			: FString::Printf(TEXT("%s_%d.wav"), *BaseStem, Suffix);
		// Planted in the Audio subfolder because that is where the save resolves collisions.
		TestTrue(TEXT("Every bounded readable collision fixture should be planted."),
			FFileHelper::SaveStringToFile(TEXT("occupied"), *(OutputRoot / AudioExporterAudioSubfolder / Name)));
	}

	TArray<float> Buffer = AudioExporterRuntimeTestUtils::MakeSineBuffer(64, 1, 48000, 440.0f, 0.25f);
	float GainAppliedDb = 0.0f;
	float PeakDb = 0.0f;
	float RMSDb = 0.0f;
	FString OutputFilePath;
	const bool bSaved = FAudioExporterUtils::ProcessAndSaveVariation(
		Buffer, TEXT("Fire"), FString(), 0, OutputRoot, 48000, 1, false, true,
		GainAppliedDb, PeakDb, RMSDb, OutputFilePath, nullptr, FGuid(), nullptr, StablePackagePath);

	const FString ExpectedHash = FString::Printf(TEXT("%08x"), FCrc::StrCrc32(*StablePackagePath));
	TestTrue(TEXT("The hash fallback should still write a valid WAV."), bSaved);
	TestEqual(TEXT("Readable collisions beyond _999 must use the short stable package-path hash."),
		FPaths::GetCleanFilename(OutputFilePath), FString::Printf(TEXT("Fire_1_%s.wav"), *ExpectedHash));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterPipelineStartupFailureBroadcastsTerminalTest,
	"AudioFactory.AudioExporter.Runtime.Lifecycle.StartupFailureBroadcastsTerminal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterPipelineStartupFailureBroadcastsTerminalTest::RunTest(const FString& Parameters)
{
	UWorld* World = AudioExporterRuntimeTestUtils::ResolveAutomationWorld();
	if (!World)
	{
		AddError(TEXT("Startup failure terminal test is unavailable: no editor world was available."));
		return false;
	}

	const TSharedRef<AudioExporterRuntimeTestUtils::FNullRendererExportPipeline> Pipeline = MakeShared<AudioExporterRuntimeTestUtils::FNullRendererExportPipeline>();
	Pipeline->Initialize(World);

	int32 TerminalEvents = 0;
	FString TerminalPath = TEXT("unset");
	Pipeline->OnExportFinished.AddLambda([&TerminalEvents, &TerminalPath](const FString& InPath)
	{
		++TerminalEvents;
		TerminalPath = InPath;
	});
	int32 ResultEvents = 0;
	FAudioExportResult TerminalResult;
	Pipeline->OnExportResult.AddLambda([&ResultEvents, &TerminalResult](const FAudioExportResult& InResult)
	{
		++ResultEvents;
		TerminalResult = InResult;
	});

	USoundCue* FixtureSound = NewObject<USoundCue>(GetTransientPackage(), TEXT("AudioExporterStartupFailureCue"));
	TestNotNull(TEXT("Transient fixture Cue should be created."), FixtureSound);

	FAudioExportSoundEntry Entry;
	Entry.Sound = FixtureSound;
	Entry.SoundName = TEXT("AudioExporterStartupFailureCue");
	Entry.SourceAssetPath = TEXT("/Temp/AudioExporterStartupFailureCue");
	Entry.RelativePath = TEXT("StartupFailure");

	TArray<FAudioExportSoundEntry> Entries;
	Entries.Add(Entry);

	FAudioExporterConfig Config;
	Config.bExportTextLog = false;

	AddExpectedError(TEXT("Failed to create renderer"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("Export startup failed because the audio renderer was unavailable"), EAutomationExpectedErrorFlags::Contains, 1);
	const FString StartupFailureOutputRoot = FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("RealPipeline"), TEXT("AudioExporter"),
		FString::Printf(TEXT("StartupFailureCleanup_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	const bool bStarted = Pipeline->StartExporting(
		Entries,
		Config,
		StartupFailureOutputRoot);

	TestFalse(TEXT("Renderer-invalid startup should return false."), bStarted);
	TestFalse(TEXT("Startup failure should leave the pipeline inactive."), Pipeline->IsExporting());
	TestEqual(TEXT("Startup failure should emit exactly one terminal event."), TerminalEvents, 1);
	TestEqual(TEXT("Startup failure terminal path should be empty."), TerminalPath, FString());
	TestEqual(TEXT("Renderer-invalid startup should emit exactly one structured result."), ResultEvents, 1);
	TestEqual(TEXT("Renderer-invalid startup should settle as Rejected."), TerminalResult.Outcome, EAudioExportOutcome::Rejected);
	TestFalse(TEXT("Renderer-invalid startup should report that the start was not accepted."), TerminalResult.bAcceptedStart);
	TestTrue(TEXT("Renderer-invalid startup should leave ExportPath unset."), TerminalResult.ExportPath.IsEmpty());
	TArray<FString> RemainingSessionDirectories;
	IFileManager::Get().FindFiles(RemainingSessionDirectories, *(StartupFailureOutputRoot / TEXT("*")), false, true);
	TestTrue(TEXT("Renderer startup failure should not leave a newly-created empty timestamp directory."), RemainingSessionDirectories.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterNormalizationFixtureTest,
	"AudioFactory.AudioExporter.Runtime.Utility.NormalizationFixture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterNormalizationFixtureTest::RunTest(const FString& Parameters)
{
	TArray<float> Buffer = AudioExporterRuntimeTestUtils::MakeSineBuffer(240, 1, 48000, 1000.0f, 0.25f);
	float GainAppliedDb = 0.0f;
	float PeakDb = 0.0f;
	float RMSDb = 0.0f;
	FString OutputFilePath;

	const FString OutputDirectory = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("NormalizationFixture"));
	const bool bSaved = FAudioExporterUtils::ProcessAndSaveVariation(
		Buffer,
		TEXT("NormalizeFixture"),
		TEXT("Mono"),
		0,
		OutputDirectory,
		48000,
		1,
		true,
		true,
		GainAppliedDb,
		PeakDb,
		RMSDb,
		OutputFilePath);

	TestTrue(TEXT("Normalization fixture should save successfully."), bSaved);

	TArray<uint8> WavData;
	TestTrue(TEXT("Normalized fixture WAV should load back into memory."), FFileHelper::LoadFileToArray(WavData, *OutputFilePath));

	AudioExporterRuntimeTestUtils::FWavHeaderInfo Header;
	TestTrue(TEXT("Normalized fixture WAV header should parse successfully."), AudioExporterRuntimeTestUtils::ParseWaveHeader(WavData, Header));
	TestEqual(TEXT("Normalized fixture should remain mono."), Header.NumChannels, static_cast<uint16>(1));
	TestTrue(TEXT("Normalization should apply a positive gain."), GainAppliedDb > 0.0f);
	TestTrue(TEXT("Normalized fixture peak should approach the -1 dBFS target."), PeakDb > -1.5f && PeakDb < -0.5f);
	TestTrue(TEXT("Normalized fixture PCM should be louder than the original 0.25 amplitude."),
		FMath::Abs(AudioExporterRuntimeTestUtils::ReadPcmSample(WavData, 2)) > 7000);
	const float ExpectedGainDb = -1.0f - (20.0f * FMath::LogX(10.0f, 0.25f));
	const int16 ExpectedPeakSample = static_cast<int16>(FMath::RoundToInt(FMath::Pow(10.0f, -1.0f / 20.0f) * 32768.0f));
	const int16 MaxAbsSample = AudioExporterRuntimeTestUtils::ReadMaxAbsPcmSample(WavData);
	TestTrue(TEXT("Exporter normalization gain should match hand calculation from 0.25 linear amplitude to -1 dBFS."),
		FMath::IsNearlyEqual(GainAppliedDb, ExpectedGainDb, 0.05f));
	TestTrue(TEXT("Exporter normalized WAV peak should match the hand-calculated -1 dBFS sample peak."),
		FMath::Abs(static_cast<int32>(MaxAbsSample) - static_cast<int32>(ExpectedPeakSample)) <= 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterBatchFixtureMatrixTest,
	"AudioFactory.AudioExporter.Runtime.Utility.BatchFixtureMatrix_1_25_200",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterBatchFixtureMatrixTest::RunTest(const FString& Parameters)
{
	const TArray<int32> BatchSizes = { 1, 25, 200 };
	for (const int32 BatchSize : BatchSizes)
	{
		const FString OutputDirectory = AudioExporterRuntimeTestUtils::MakeOutputDirectory(FString::Printf(TEXT("Batch_%d"), BatchSize));
		int32 SavedCount = 0;

		for (int32 Index = 0; Index < BatchSize; ++Index)
		{
			TArray<float> Buffer = AudioExporterRuntimeTestUtils::MakeSineBuffer(128, 1, 48000, 220.0f + static_cast<float>(Index), 0.3f);
			float GainAppliedDb = 0.0f;
			float PeakDb = 0.0f;
			float RMSDb = 0.0f;
			FString OutputFilePath;

			if (FAudioExporterUtils::ProcessAndSaveVariation(
				Buffer,
				FString::Printf(TEXT("BatchSound_%03d"), Index + 1),
				TEXT("Matrix"),
				0,
				OutputDirectory,
				48000,
				1,
				(Index % 2) == 0,
				true,
				GainAppliedDb,
				PeakDb,
				RMSDb,
				OutputFilePath))
			{
				++SavedCount;
			}
		}

		TestEqual(FString::Printf(TEXT("Batch size %d should save every generated variation fixture."), BatchSize), SavedCount, BatchSize);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterConcurrentStartIsRejectedTest,
	"AudioFactory.AudioExporter.Runtime.Lifecycle.ConcurrentStartIsRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterConcurrentStartIsRejectedTest::RunTest(const FString& Parameters)
{
	// Each request owns one token. A rejected B request must settle B's callback
	// without consuming active A's callback, and a later C session must not inherit either callback.
	UWorld* World = AudioExporterRuntimeTestUtils::ResolveAutomationWorld();
	if (!World)
	{
		AddError(TEXT("Concurrent-start rejection test is unavailable: no editor world was available."));
		return false;
	}

	UAudioExporterRuntimeSubsystem* Subsystem = NewObject<UAudioExporterRuntimeSubsystem>(GetTransientPackage());
	TestNotNull(TEXT("Runtime subsystem test object should be created."), Subsystem);
	if (!Subsystem)
	{
		return false;
	}

	int32 AcceptedResultEvents = 0;
	FAudioExportResult AcceptedResult;
	const TSharedRef<int32> ReentrantCallbackEvents = MakeShared<int32>(0);
	const FGuid AcceptedRequestId = FGuid::NewGuid();
	const FGuid RejectedRequestId = FGuid::NewGuid();
	TArray<FAudioExportResult> GlobalResults;
	TArray<FAudioExportProgressInfo> GlobalProgress;
	const FDelegateHandle GlobalResultHandle = Subsystem->OnExportCompleteResultNative.AddLambda(
		[&GlobalResults](const FAudioExportResult& InResult) { GlobalResults.Add(InResult); });
	const FDelegateHandle GlobalProgressHandle = Subsystem->OnExportProgressNative.AddLambda(
		[&GlobalProgress](const FAudioExportProgressInfo& InProgress) { GlobalProgress.Add(InProgress); });
	ON_SCOPE_EXIT
	{
		Subsystem->OnExportCompleteResultNative.Remove(GlobalResultHandle);
		Subsystem->OnExportProgressNative.Remove(GlobalProgressHandle);
	};
	const FAudioExportRequestToken AcceptedRequestToken = Subsystem->AddOneTimeResultCallback(
		[Subsystem, &AcceptedResultEvents, &AcceptedResult, ReentrantCallbackEvents](const FAudioExportResult& InResult)
		{
			++AcceptedResultEvents;
			AcceptedResult = InResult;
			Subsystem->AddOneTimeResultCallback([ReentrantCallbackEvents](const FAudioExportResult&)
			{
				++(*ReentrantCallbackEvents);
			});
		});

	USoundWaveProcedural* AcceptedWave = AudioExporterRuntimeTestUtils::MakeTransientToneWave(TEXT("ConcurrentIdentityAcceptedWave"), 0.25f);
	TestNotNull(TEXT("The accepted A fixture should create a procedural source."), AcceptedWave);
	if (!AcceptedWave)
	{
		return false;
	}
	TArray<FAudioExportSoundEntry> AcceptedEntries;
	AcceptedEntries.Add(AudioExporterRuntimeTestUtils::MakeFixtureEntry(AcceptedWave, TEXT("ConcurrentIdentityAcceptedWave"), TEXT("/Temp/ConcurrentIdentityAcceptedWave")));
	const FAudioExporterConfig AcceptedConfig = AudioExporterRuntimeTestUtils::MakeFailureFixtureConfig(1);
	const FString FirstSessionPath = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("Lifecycle_ConcurrentStart_First"));
	if (!Subsystem->BeginLifecycleTestSession(World, AcceptedEntries, AcceptedConfig, FirstSessionPath, AcceptedRequestToken, AcceptedRequestId))
	{
		AddError(TEXT("Concurrent-start rejection test could not seed its required lifecycle session."));
		return false;
	}

	TestTrue(TEXT("The seeded lifecycle session should report an active export."), Subsystem->IsExporting());

	int32 RejectedResultEvents = 0;
	FAudioExportResult RejectedResult;
	const FAudioExportRequestToken RejectedRequestToken = Subsystem->AddOneTimeResultCallback(
		[&RejectedResultEvents, &RejectedResult](const FAudioExportResult& InResult)
		{
			++RejectedResultEvents;
			RejectedResult = InResult;
		});

	AddExpectedError(TEXT("An export session is already active"), EAutomationExpectedErrorFlags::Contains, 1);
	const bool bSecondStarted = Subsystem->StartExport(
		World,
		TArray<FAudioExportSoundEntry>(),
		FAudioExporterConfig(),
		AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("Lifecycle_ConcurrentStart_Second")),
		RejectedRequestToken,
		RejectedRequestId);
	TestFalse(TEXT("A second StartExport during an active session should be rejected."), bSecondStarted);
	TestEqual(TEXT("B alone should receive exactly one terminal result."), RejectedResultEvents, 1);
	TestEqual(TEXT("B alone should receive Rejected."), RejectedResult.Outcome, EAudioExportOutcome::Rejected);
	TestFalse(TEXT("B's rejected result must report accepted-start false."), RejectedResult.bAcceptedStart);
	TestEqual(TEXT("B's rejection must retain B's request identity."), RejectedResult.RequestId, RejectedRequestId);
	TestFalse(TEXT("A pre-session rejection must not invent an accepted session identity."), RejectedResult.SessionId.IsValid());

	TestTrue(TEXT("A rejected request should leave the active session running."), Subsystem->IsExporting());
	TestEqual(TEXT("A rejected request should not replace the active session's export path."), Subsystem->GetExportPath(), FirstSessionPath);

	Subsystem->SimulateVariationFinishedForTesting();

	TestEqual(TEXT("A alone should receive exactly one terminal result."), AcceptedResultEvents, 1);
	TestEqual(TEXT("A's terminal result should be Succeeded."), AcceptedResult.Outcome, EAudioExportOutcome::Succeeded);
	TestTrue(TEXT("A's cancelled result should report accepted-start true."), AcceptedResult.bAcceptedStart);
	TestEqual(TEXT("A's terminal result must retain A's request identity."), AcceptedResult.RequestId, AcceptedRequestId);
	TestTrue(TEXT("A's accepted terminal result must carry a session identity."), AcceptedResult.SessionId.IsValid());
	TestTrue(TEXT("A must publish at least one progress event through the global listener."), !GlobalProgress.IsEmpty());
	for (const FAudioExportProgressInfo& Progress : GlobalProgress)
	{
		TestEqual(TEXT("Every A progress event must retain A's request identity."), Progress.RequestId, AcceptedRequestId);
		TestEqual(TEXT("Every A progress event must retain A's session identity."), Progress.SessionId, AcceptedResult.SessionId);
	}
	TestEqual(TEXT("The global listener should receive exactly B rejection then A terminal."), GlobalResults.Num(), 2);
	if (GlobalResults.Num() == 2)
	{
		TestEqual(TEXT("The first global event should be identifiable as B."), GlobalResults[0].RequestId, RejectedRequestId);
		TestEqual(TEXT("The second global event should be identifiable as A."), GlobalResults[1].RequestId, AcceptedRequestId);
	}
	TestEqual(TEXT("A callback registered during terminal dispatch must not be serviced by that dispatch."), *ReentrantCallbackEvents, 0);

	// C is unrelated to A and B. It has no callback of its own, and neither A nor B may be serviced
	// by this terminal.
	const FString ThirdSessionPath = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("Lifecycle_ConcurrentStart_Third"));
	TestTrue(TEXT("An unrelated third session should still be able to start."),
		Subsystem->BeginLifecycleTestSession(World, ThirdSessionPath));
	Subsystem->CancelExport(false);
	TestEqual(TEXT("Session C should not receive A or B's callbacks."), AcceptedResultEvents, 1);
	TestEqual(TEXT("Session C should not re-fire B's rejected callback."), RejectedResultEvents, 1);
	TestFalse(TEXT("The subsystem should be idle once the session is cancelled."), Subsystem->IsExporting());
	AcceptedWave->RemoveFromRoot();
	return true;
}

// Fail-first regression: an accepted synchronous request A may settle with no local
// listeners, then request B may fail through the pipeline's FailStartup path. Binding after B must
// not receive a cached terminal attributed to either request. The old replay ticker delivered B's
// overwritten cache on the next frame.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterInterleavedSyncSettleDoesNotReplayTerminalTest,
	"AudioFactory.AudioExporter.Runtime.Lifecycle.InterleavedSyncSettleDoesNotReplayTerminal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterInterleavedSyncSettleDoesNotReplayTerminalTest::RunTest(const FString& Parameters)
{
	UWorld* World = AudioExporterRuntimeTestUtils::ResolveAutomationWorld();
	if (!World)
	{
		AddError(TEXT("The interleaved-terminal test is unavailable: no editor world was available."));
		return false;
	}

	UAudioExporterRuntimeSubsystem* Subsystem = UAudioExporterBPLibrary::GetRuntimeSubsystem();
	TestNotNull(TEXT("The interleaved-terminal test should resolve the engine runtime subsystem."), Subsystem);
	if (!Subsystem)
	{
		return false;
	}
	if (Subsystem->IsExporting())
	{
		Subsystem->CancelExport(false);
	}

	FAudioExportSoundEntry UnavailableEntry;
	UnavailableEntry.SoundName = TEXT("InterleavedSyncSettleRequestA");
	UnavailableEntry.SourceAssetPath = TEXT("/Temp/AudioExporterInterleavedSyncSettleRequestA");
	UnavailableEntry.RelativePath = TEXT("InterleavedSyncSettleRequestA");

	FAudioExporterConfig Config;
	Config.OneShotVariations = 1;
	Config.LoopVariations = 1;
	Config.bNormalize = false;
	Config.bMonitor = false;
	Config.bExportTextLog = false;

	// A is accepted and settles synchronously before any listener is added in this test.
	const bool bRequestAAccepted = Subsystem->StartExport(
		World,
		TArray<FAudioExportSoundEntry>{ UnavailableEntry },
		Config,
		AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("InterleavedSyncSettleRequestA")));
	TestTrue(TEXT("Request A should be accepted through the real subsystem start path."), bRequestAAccepted);
	TestFalse(TEXT("Request A should be idle after its synchronous terminal."), Subsystem->IsExporting());

	// B reaches Pipeline::FailStartup rather than the subsystem's manager preflight rejection: an
	// empty sound list is allowed through EnsurePipeline and rejected by the pipeline itself.
	AddExpectedError(TEXT("Export start rejected because the sound list was empty."), EAutomationExpectedErrorFlags::Contains, 1);
	const bool bRequestBAccepted = Subsystem->StartExport(
		World,
		TArray<FAudioExportSoundEntry>(),
		Config,
		AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("InterleavedSyncSettleRequestB")));
	TestFalse(TEXT("Request B should be rejected through the pipeline startup-failure path."), bRequestBAccepted);
	TestFalse(TEXT("Request B should leave the subsystem idle after rejection."), Subsystem->IsExporting());

	int32 LateStructuredEvents = 0;
	int32 LateLegacyEvents = 0;
	const FDelegateHandle StructuredHandle = Subsystem->OnExportCompleteResultNative.AddLambda(
		[&LateStructuredEvents](const FAudioExportResult&)
		{
			++LateStructuredEvents;
		});
	const FDelegateHandle LegacyHandle = Subsystem->OnExportCompleteNative.AddLambda(
		[&LateLegacyEvents](const FString&)
		{
			++LateLegacyEvents;
		});

	// The old replay ticker consumed the cached B result here. With one real terminal path, a late
	// listener observes no historical request and remains at zero events.
	FTSTicker::GetCoreTicker().Tick(0.0f);

	TestEqual(TEXT("A listener bound after the interleaved requests must not receive a structured replay."), LateStructuredEvents, 0);
	TestEqual(TEXT("A listener bound after the interleaved requests must not receive a legacy replay."), LateLegacyEvents, 0);

	Subsystem->OnExportCompleteResultNative.Remove(StructuredHandle);
	Subsystem->OnExportCompleteNative.Remove(LegacyHandle);
	return true;
}

// Synchronization regression: native registrations happen from worker threads while the
// game-thread terminal path removes the active callback. The callback is moved out of the mutex and
// may register another callback without that reentrant registration being serviced by the same
// terminal. The old unsynchronized TMap access could race or corrupt during this overlap.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterConcurrentResultCallbackRegistrationTest,
	"AudioFactory.AudioExporter.Runtime.Lifecycle.ConcurrentResultCallbackRegistration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterConcurrentResultCallbackRegistrationTest::RunTest(const FString& Parameters)
{
	UWorld* World = AudioExporterRuntimeTestUtils::ResolveAutomationWorld();
	if (!World)
	{
		AddError(TEXT("The concurrent-callback test is unavailable: no editor world was available."));
		return false;
	}

	UAudioExporterRuntimeSubsystem* Subsystem = NewObject<UAudioExporterRuntimeSubsystem>(GetTransientPackage());
	TestNotNull(TEXT("The concurrent-callback test should create a runtime subsystem."), Subsystem);
	if (!Subsystem)
	{
		return false;
	}

	const TSharedRef<int32> ActiveCallbackEvents = MakeShared<int32>(0);
	const TSharedRef<int32> ReentrantCallbackEvents = MakeShared<int32>(0);
	const FAudioExportRequestToken ActiveRequestToken = Subsystem->AddOneTimeResultCallback(
		[Subsystem, ActiveCallbackEvents, ReentrantCallbackEvents](const FAudioExportResult&)
		{
			++(*ActiveCallbackEvents);
			Subsystem->AddOneTimeResultCallback([ReentrantCallbackEvents](const FAudioExportResult&)
			{
				++(*ReentrantCallbackEvents);
			});
		});

	TestTrue(TEXT("The active callback registration should return a nonzero token."), ActiveRequestToken != 0);
	if (ActiveRequestToken == 0 || !Subsystem->BeginLifecycleTestSession(
		World,
		AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("ConcurrentResultCallbackRegistration")),
		ActiveRequestToken))
	{
		AddError(TEXT("The concurrent-callback test could not seed its lifecycle session."));
		return false;
	}

	constexpr int32 WorkerCount = 4;
	constexpr int32 RegistrationsPerWorker = 32;
	TAtomic<int32> ReadyWorkers{ 0 };
	TAtomic<bool> ReleaseWorkers{ false };
	TAtomic<int32> RegisteredCallbacks{ 0 };
	TArray<TFuture<void>> RegistrationTasks;
	RegistrationTasks.Reserve(WorkerCount);

	for (int32 WorkerIndex = 0; WorkerIndex < WorkerCount; ++WorkerIndex)
	{
		RegistrationTasks.Add(Async(EAsyncExecution::ThreadPool, [Subsystem, &ReadyWorkers, &ReleaseWorkers, &RegisteredCallbacks, RegistrationsPerWorker]()
		{
			++ReadyWorkers;
			while (!ReleaseWorkers.Load())
			{
				FPlatformProcess::SleepNoStats(0.001f);
			}

			for (int32 RegistrationIndex = 0; RegistrationIndex < RegistrationsPerWorker; ++RegistrationIndex)
			{
				const FAudioExportRequestToken RequestToken = Subsystem->AddOneTimeResultCallback([](const FAudioExportResult&)
				{
				});
				if (RequestToken != 0)
				{
					++RegisteredCallbacks;
				}
			}
		}));
	}

	const double ReadyDeadlineSeconds = FPlatformTime::Seconds() + 5.0;
	while (ReadyWorkers.Load() < WorkerCount && FPlatformTime::Seconds() < ReadyDeadlineSeconds)
	{
		FPlatformProcess::SleepNoStats(0.001f);
	}
	TestEqual(TEXT("Every callback-registration worker should reach the concurrent window."), ReadyWorkers.Load(), WorkerCount);

	ReleaseWorkers.Store(true);
	Subsystem->CancelExport(false);

	for (TFuture<void>& RegistrationTask : RegistrationTasks)
	{
		RegistrationTask.Get();
	}

	TestEqual(TEXT("The active callback should be invoked exactly once."), *ActiveCallbackEvents, 1);
	TestEqual(TEXT("A callback registered by terminal user code must not run in the same terminal."), *ReentrantCallbackEvents, 0);
	TestEqual(TEXT("All concurrent native callback registrations should return valid tokens."),
		RegisteredCallbacks.Load(), WorkerCount * RegistrationsPerWorker);
	TestFalse(TEXT("The concurrent-callback session should be idle after cancellation."), Subsystem->IsExporting());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterVariationExtractionBoundsTest,
	"AudioFactory.AudioExporter.Runtime.Utility.ExtractVariationBufferBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterVariationExtractionBoundsTest::RunTest(const FString& Parameters)
{
	// A negative start marker must not pass both guards and Memcpy from before the
	// allocation, and a clamped tail must not end mid-frame and rotate the channel order.
	const TArray<float> BigBuffer = {
		0.f, 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f,
		8.f, 9.f, 10.f, 11.f, 12.f, 13.f, 14.f, 15.f
	};

	const TArray<float> NegativeStart = FAudioExporterUtils::ExtractVariationBuffer(BigBuffer, 4, 2, -1.0, 1.0, 1.0, false);
	TestTrue(TEXT("A negative start marker should be rejected as an invalid sample range."), NegativeStart.IsEmpty());

	const TArray<float> PastEnd = FAudioExporterUtils::ExtractVariationBuffer(BigBuffer, 4, 2, 8.0, 9.0, 1.0, false);
	TestTrue(TEXT("A start marker past the end of the buffer should be rejected."), PastEnd.IsEmpty());

	// An odd-length buffer forces the tail clamp: the requested range runs past the end, and the 11
	// remaining samples are not a whole number of stereo frames.
	TArray<float> OddLengthBuffer = BigBuffer;
	OddLengthBuffer.SetNum(15);

	const TArray<float> ClampedTail = FAudioExporterUtils::ExtractVariationBuffer(OddLengthBuffer, 4, 2, 0.5, 2.0, 1.0, false);
	TestEqual(TEXT("A clamped tail should stop on the last whole stereo frame."), ClampedTail.Num(), 10);
	TestEqual(TEXT("A clamped tail should still begin at the requested sample."), ClampedTail[0], 4.0f);
	TestEqual(TEXT("A clamped tail should never include a partial frame."), ClampedTail.Num() % 2, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterConfigSanitizeClampsTest,
	"AudioFactory.AudioExporter.Runtime.Config.SanitizeClampsOutOfRangeValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterConfigSanitizeClampsTest::RunTest(const FString& Parameters)
{
	// ClampMin/ClampMax only constrains the details panel, so the pipeline boundary
	// has to coerce whatever Blueprint hands it (MaxLengthSeconds = 0 truncated every capture).
	FAudioExporterConfig OutOfRange;
	OutOfRange.OneShotVariations = 0;
	OutOfRange.LoopVariations = 100000;
	OutOfRange.MaxLengthSeconds = -30;

	AddExpectedError(TEXT("Export config values were outside their supported ranges"), EAutomationExpectedErrorFlags::Contains, 1);
	OutOfRange.Sanitize();

	TestEqual(TEXT("One-shot variations should clamp up to the supported minimum."), OutOfRange.OneShotVariations, 1);
	TestEqual(TEXT("Loop variations should clamp down to the supported maximum."), OutOfRange.LoopVariations, 5);
	TestEqual(TEXT("Max length should clamp up to the supported minimum."), OutOfRange.MaxLengthSeconds, 5);

	// An in-range config must be left alone, and must not emit the coercion warning a second time.
	FAudioExporterConfig InRange;
	const int32 ExpectedOneShotVariations = InRange.OneShotVariations;
	const int32 ExpectedLoopVariations = InRange.LoopVariations;
	const int32 ExpectedMaxLengthSeconds = InRange.MaxLengthSeconds;
	InRange.Sanitize();

	TestEqual(TEXT("Sanitize should not change in-range one-shot variations."), InRange.OneShotVariations, ExpectedOneShotVariations);
	TestEqual(TEXT("Sanitize should not change in-range loop variations."), InRange.LoopVariations, ExpectedLoopVariations);
	TestEqual(TEXT("Sanitize should not change an in-range max length."), InRange.MaxLengthSeconds, ExpectedMaxLengthSeconds);
	return true;
}

// ---------------------------------------------------------------------------
// Regression pin for the one-shot truncation defect (real edge-QA measurement:
// exp_trunc_oneshot_full_length produced 8.02s/4.98s/8.02s across three identical runs of an 8s
// one-shot exported with MaxLengthSeconds=5). Root cause: FAudioRendererRealtime used to *infer*
// loop-vs-one-shot from which timer fired first -- a watchdog set at MaxLengthSeconds+0.1s racing
// the sound's own OnAudioFinished. A slow-to-start or naturally-long one-shot could lose that race
// and get chopped at MaxLengthSeconds, exactly as if it were a loop.
//
// The fix applies the configured ceiling to procedural sources even when they are not declared
// looping, then determines capture-limit versus natural completion from the observed event. The
// watchdog-delay/end-time decision remains in two pure static
// functions (FAudioRendererRealtime::ComputeWatchdogDelaySeconds/ComputeWatchdogRenderEndTime) --
// the same functions Render()/OnAudioPlayStateChanged()/CompleteRender() call in production, so
// there is no risk of this test drifting from the real logic.
//
// This is a deterministic unit-level pin, not a live-audio integration test: this automation
// harness cannot reliably drive FAudioRendererRealtime's real play-state/watchdog callbacks to
// completion (a real Render() request against a synthetic fixture can sit idle indefinitely here --
// see the async-drain tests below), so exercising this exact regression against a live audio device
// is left to the real edge-QA harness (an end-to-end edge-QA run),
// which is what actually measured the non-determinism above. This test instead pins the exact
// decision logic that caused it, with zero timing dependency and zero flake risk.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterOneShotWatchdogIgnoresMaxLengthTest,
	"AudioFactory.AudioExporter.Runtime.Watchdog.OneShotNeverCappedByMaxLength",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterOneShotWatchdogIgnoresMaxLengthTest::RunTest(const FString& Parameters)
{
	// The safety ceiling must be far beyond FAudioExporterConfig::MaxLengthSeconds's entire legal
	// range (ClampMin=5, ClampMax=60) -- proving it numerically, not just in a comment, is the
	// point: this is what makes it "unreachable in normal use".
	TestTrue(TEXT("OneShotSafetyTimeoutSeconds must be far beyond the largest legal MaxLengthSeconds (60s)."),
		FAudioRendererRealtime::OneShotSafetyTimeoutSeconds >= 300.0f);

	for (const float MaxLengthSeconds : { 5.0f, 30.0f, 60.0f })
	{
		// One-shot: the watchdog delay must be the safety-only ceiling, completely independent of
		// MaxLengthSeconds. If the old racing behaviour returns (delay = MaxLengthSeconds + 0.1s
		// regardless of loop status), this fails immediately -- an 8s one-shot would again race a
		// ~5.1s watchdog.
		const float OneShotDelay = FAudioRendererRealtime::ComputeWatchdogDelaySeconds(/*bCaptureLimitApplies=*/false, MaxLengthSeconds);
		TestEqual(FString::Printf(TEXT("One-shot watchdog delay must equal the safety ceiling, not MaxLengthSeconds=%.0f."), MaxLengthSeconds),
			OneShotDelay, FAudioRendererRealtime::OneShotSafetyTimeoutSeconds);
		TestTrue(FString::Printf(TEXT("One-shot watchdog delay must not equal MaxLengthSeconds(%.0f)+0.1s."), MaxLengthSeconds),
			!FMath::IsNearlyEqual(OneShotDelay, MaxLengthSeconds + 0.1f));

		// A procedural source uses the configured ceiling whether or not its loop flag is set.
		const float ProceduralDelay = FAudioRendererRealtime::ComputeWatchdogDelaySeconds(/*bCaptureLimitApplies=*/true, MaxLengthSeconds);
		TestEqual(FString::Printf(TEXT("Procedural watchdog delay must use MaxLengthSeconds=%.0f (+0.1s)."), MaxLengthSeconds),
			ProceduralDelay, MaxLengthSeconds + 0.1f);

		// End-time calculation for a render the watchdog stopped: a one-shot must land on the
		// safety ceiling (never MaxLengthSeconds); a capture-limited source must land exactly on MaxLengthSeconds.
		constexpr double RenderStartTime = 12.5;
		const double OneShotEndTime = FAudioRendererRealtime::ComputeWatchdogRenderEndTime(/*bCaptureLimitApplies=*/false, RenderStartTime, MaxLengthSeconds);
		TestEqual(FString::Printf(TEXT("One-shot watchdog end time must use the safety ceiling, not MaxLengthSeconds=%.0f."), MaxLengthSeconds),
			OneShotEndTime, RenderStartTime + static_cast<double>(FAudioRendererRealtime::OneShotSafetyTimeoutSeconds));

		const double ProceduralEndTime = FAudioRendererRealtime::ComputeWatchdogRenderEndTime(/*bCaptureLimitApplies=*/true, RenderStartTime, MaxLengthSeconds);
		TestEqual(FString::Printf(TEXT("Procedural watchdog end time must be chopped exactly at MaxLengthSeconds=%.0f."), MaxLengthSeconds),
			ProceduralEndTime, RenderStartTime + static_cast<double>(MaxLengthSeconds));
	}

	return true;
}

// ---------------------------------------------------------------------------
// Async save-branch regression coverage.
//
// Every test above runs under FApp::IsUnattended() == true, which makes ExportCurrentBatch take the
// fully synchronous save branch -- the async drain (FinalizeOrWaitForAsyncSaves), the final-save
// finalize handoff, and the ActiveAsyncExports lifecycle across cancel/restart never execute. These
// tests force the async branch via SetForceAsyncSaveForTesting(). A real renderer cannot be driven to
// completion headless either (FAudioRendererRealtime's play-state/watchdog callbacks never fire under
// -nullrhi, so a real Render() request just sits idle indefinitely) -- SimulateVariationFinishedForTesting()
// stands in for it, fabricating a variation buffer and running the pipeline's real
// ScheduleVariationSave/FinishExporting/ExportCurrentBatch/FinalizeOrWaitForAsyncSaves accounting so the
// async code paths get real, deterministic coverage without any renderer or /Game fixture dependency.
// ---------------------------------------------------------------------------
namespace AudioExporterRuntimeTestUtils
{
	// Provides a valid, real USoundBase for FAudioExportSoundEntry::Sound. The generated PCM is queued
	// into the procedural wave and consumed by the realtime renderer when callers drive public
	// StartExporting; lifecycle-only tests may also reuse the same source while exercising bookkeeping seams.
	static USoundWaveProcedural* MakeTransientToneWave(const TCHAR* Name, float DurationSeconds = 1.0f)
	{
		constexpr int32 SampleRate = 48000;
		constexpr int32 NumChannels = 2;
		constexpr float Frequency = 440.0f;
		constexpr float Amplitude = 0.5f;
		const int32 NumFrames = FMath::Max(1, FMath::RoundToInt(SampleRate * DurationSeconds));

		TArray<int16> Pcm;
		Pcm.SetNumUninitialized(NumFrames * NumChannels);
		for (int32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
		{
			const float Sample = Amplitude * FMath::Sin(2.0f * PI * Frequency * static_cast<float>(FrameIndex) / static_cast<float>(SampleRate));
			const int16 PcmSample = static_cast<int16>(FMath::Clamp(FMath::RoundToInt(Sample * 32767.0f), -32768, 32767));
			for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
			{
				Pcm[(FrameIndex * NumChannels) + ChannelIndex] = PcmSample;
			}
		}

		USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>(GetTransientPackage(), Name);
		if (!Wave)
		{
			return nullptr;
		}

		// Rooted for the life of the (latent, multi-tick) test; callers must RemoveFromRoot() when done.
		Wave->AddToRoot();
		Wave->SetSampleRate(SampleRate);
		Wave->NumChannels = NumChannels;
		Wave->Duration = DurationSeconds;
		Wave->bLooping = false;
		Wave->VirtualizationMode = EVirtualizationMode::PlayWhenSilent;
		Wave->QueueAudio(reinterpret_cast<const uint8*>(Pcm.GetData()), Pcm.Num() * sizeof(int16));
		return Wave;
	}

	/** Real renderer surface used to force the production mid-session restart failure after a save
	 * has already been dispatched. The save itself still runs through ScheduleVariationSave and the
	 * normal async worker; only the renderer's next-batch start is made to fail. */
	class FRestartFailureRenderer final : public FAudioRendererRealtime
	{
	public:
		bool bFailNextBatchStart = true;
		TArray<float> CannedBuffer = MakeSineBuffer(2400, 2, 48000, 440.0f, 0.5f);

		virtual bool IsInBatchRecording() const override { return true; }

		virtual bool StartBatchRecording() override
		{
			if (bFailNextBatchStart)
			{
				bFailNextBatchStart = false;
				return false;
			}
			return true;
		}

		virtual void StopBatchRecording(TArray<float>& OutBuffer, int32& OutNumChannels, int32& OutSampleRate) override
		{
			OutBuffer = CannedBuffer;
			OutNumChannels = 2;
			OutSampleRate = 48000;
		}

		virtual double GetBatchStartTime() const override { return 0.0; }
		virtual double GetBatchEndTime() const override { return 0.05; }
	};

	class FExposedAsyncRestartPipeline final : public FAudioExportPipeline
	{
	public:
		using FAudioExportPipeline::ExportCurrentBatch;
		using FAudioExportPipeline::PendingVariations;
		using FAudioExportPipeline::Renderer;
	};

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

	// --- Test 1: final-batch drain -----------------------------------------------------------------

	struct FAsyncDrainState
	{
		FAutomationTestBase* Test = nullptr;
		TSharedPtr<FAudioExportPipeline> Pipeline;
		TObjectPtr<USoundWaveProcedural> Wave = nullptr;
		FString OutputRoot;
		FString FinishedPath;
		int32 TerminalEvents = 0;
		int32 StructuredTerminalEvents = 0;
		bool bFinished = false;
		bool bWriterDrainConfirmed = false;
		bool bWaitTimedOut = false;
		bool bCancellationRequested = false;
		double CancellationDeadlineSeconds = 0.0;
		double StartSeconds = 0.0;
	};

	class FWaitForAsyncDrainFinish : public IAutomationLatentCommand
	{
	public:
		explicit FWaitForAsyncDrainFinish(const TSharedRef<FAsyncDrainState>& InState, double InTimeoutSeconds = 30.0)
			: State(InState)
			, TimeoutSeconds(InTimeoutSeconds)
		{
		}

		virtual bool Update() override
		{
			if (State->bFinished && State->StructuredTerminalEvents > 0 && State->Pipeline.IsValid()
				&& !State->Pipeline->IsExporting()
				&& State->Pipeline->GetActiveAsyncExportsForTesting() == 0)
			{
				State->bWriterDrainConfirmed = true;
				return true;
			}

			const double NowSeconds = FPlatformTime::Seconds();
			if (!State->bCancellationRequested && (NowSeconds - State->StartSeconds) > TimeoutSeconds)
			{
				State->bCancellationRequested = true;
				State->CancellationDeadlineSeconds = NowSeconds + RealPipelineCancellationDrainSeconds;
				if (State->Test)
				{
					State->Test->AddError(TEXT("Async final-batch drain test timed out waiting for the terminal event."));
				}
				if (State->Pipeline.IsValid() && State->Pipeline->IsExporting())
				{
					State->Pipeline->CancelExporting(false);
				}
				return false;
			}

			if (State->bCancellationRequested && NowSeconds > State->CancellationDeadlineSeconds)
			{
				State->bWaitTimedOut = true;
				if (State->Test)
				{
					State->Test->AddError(TEXT("Async final-batch drain did not confirm terminal delivery and writer inactivity before the cancellation deadline."));
				}
				const TSharedRef<FAsyncDrainState> DeferredState = State;
				const double CleanupDeadlineSeconds = NowSeconds + RealPipelineCancellationDrainSeconds;
				FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([DeferredState, CleanupDeadlineSeconds](float)
				{
					if (!DeferredState->Pipeline.IsValid())
					{
						if (DeferredState->Wave)
						{
							DeferredState->Wave->RemoveFromRoot();
							DeferredState->Wave = nullptr;
						}
						return false;
					}
					const bool bDrained = DeferredState->bFinished
						&& DeferredState->StructuredTerminalEvents > 0
						&& !DeferredState->Pipeline->IsExporting()
						&& DeferredState->Pipeline->GetActiveAsyncExportsForTesting() == 0;
					if (!bDrained)
					{
						if (FPlatformTime::Seconds() > CleanupDeadlineSeconds)
						{
							if (DeferredState->Test)
							{
								DeferredState->Test->AddError(TEXT("Async final-batch retained cleanup exceeded the named writer-drain deadline; live pipeline and rooted source remain preserved."));
							}
							AudioExporterRuntimeTestUtils::RegisterRetainedTestCleanup(
								[DeferredState]()
								{
									return DeferredState->bFinished
										&& DeferredState->StructuredTerminalEvents > 0
										&& DeferredState->Pipeline.IsValid()
										&& !DeferredState->Pipeline->IsExporting()
										&& DeferredState->Pipeline->GetActiveAsyncExportsForTesting() == 0;
								},
								[DeferredState]()
								{
									if (DeferredState->Wave)
									{
										DeferredState->Wave->RemoveFromRoot();
										DeferredState->Wave = nullptr;
									}
									DeferredState->Pipeline.Reset();
								},
								[DeferredState]()
								{
									if (DeferredState->Pipeline.IsValid()
										&& (DeferredState->Pipeline->IsExporting()
											|| DeferredState->Pipeline->GetActiveAsyncExportsForTesting() != 0))
									{
										if (DeferredState->Test)
										{
											DeferredState->Test->AddError(TEXT("Async final-batch retained live pipeline state during engine shutdown because the pipeline was still active or had async writers."));
										}
										return false;
									}
									if ((!DeferredState->bFinished || DeferredState->StructuredTerminalEvents == 0) && DeferredState->Test)
									{
										DeferredState->Test->AddError(TEXT("Async final-batch reached engine shutdown without a structured terminal result; releasing only after shutdown made the backend inactive."));
									}
									if (DeferredState->Wave)
									{
										DeferredState->Wave->RemoveFromRoot();
										DeferredState->Wave = nullptr;
									}
									DeferredState->Pipeline.Reset();
									return true;
								});
							return false;
						}
						return true;
					}
					if (DeferredState->Wave)
					{
						DeferredState->Wave->RemoveFromRoot();
						DeferredState->Wave = nullptr;
					}
					DeferredState->Pipeline.Reset();
					return false;
				}));
				return true;
			}

			return false;
		}

	private:
		TSharedRef<FAsyncDrainState> State;
		double TimeoutSeconds = 30.0;
	};

	class FVerifyAsyncDrainFinish : public IAutomationLatentCommand
	{
	public:
		explicit FVerifyAsyncDrainFinish(const TSharedRef<FAsyncDrainState>& InState)
			: State(InState)
		{
		}

		virtual bool Update() override
		{
			FAutomationTestBase* Test = State->Test;
			if (!Test)
			{
				return true;
			}
			if (!State->bWriterDrainConfirmed || State->bWaitTimedOut)
			{
				Test->AddError(TEXT("The async final-batch drain did not confirm both terminal delivery and writer inactivity."));
				return true;
			}

			Test->TestTrue(TEXT("The async final-batch drain should reach a terminal event."), State->bFinished);
			Test->TestEqual(TEXT("The async final-batch drain should broadcast exactly one terminal event."), State->TerminalEvents, 1);
			Test->TestEqual(TEXT("The async final-batch drain should broadcast exactly one structured terminal result."), State->StructuredTerminalEvents, 1);
			Test->TestFalse(TEXT("The async final-batch drain terminal path should not be empty."), State->FinishedPath.IsEmpty());

			if (State->Pipeline.IsValid())
			{
				Test->TestFalse(TEXT("The pipeline should be inactive once the async drain finalizes."), State->Pipeline->IsExporting());
				Test->TestEqual(TEXT("ActiveAsyncExports should have drained back to zero."), State->Pipeline->GetActiveAsyncExportsForTesting(), 0);
			}

			TArray<FString> WavFiles;
			FindWavFilesUnder(State->OutputRoot, WavFiles);
			Test->TestEqual(TEXT("The async final-batch drain should write exactly one WAV file."), WavFiles.Num(), 1);
			if (!WavFiles.IsEmpty())
			{
				ValidateWavOutput(*Test, WavFiles[0], TEXT("AsyncFinalBatchDrain"));
			}

			if (State->Wave)
			{
				State->Wave->RemoveFromRoot();
				State->Wave = nullptr;
			}
			return true;
		}

	private:
		TSharedRef<FAsyncDrainState> State;
	};

	// --- Test 2: cancel with an async save in flight, then immediately restart ----------

	struct FAsyncCancelRestartState
	{
		FAutomationTestBase* Test = nullptr;
		TSharedPtr<FAudioExportPipeline> Pipeline;
		TObjectPtr<USoundWaveProcedural> WaveA = nullptr;
		TObjectPtr<USoundWaveProcedural> WaveB = nullptr;
		FString OutputRootA;
		FString OutputRootB;
		TArray<FString> TerminalPaths;
		int32 StructuredTerminalEvents = 0;
		bool bSessionBFinished = false;
		bool bWriterDrainConfirmed = false;
		bool bWaitTimedOut = false;
		bool bCancellationRequested = false;
		double CancellationDeadlineSeconds = 0.0;
	};

	class FWaitForSessionBFinish : public IAutomationLatentCommand
	{
	public:
		explicit FWaitForSessionBFinish(const TSharedRef<FAsyncCancelRestartState>& InState, double InTimeoutSeconds = 30.0)
			: State(InState)
			, TimeoutSeconds(InTimeoutSeconds)
			, StartSeconds(FPlatformTime::Seconds())
		{
		}

		virtual bool Update() override
		{
			if (State->bSessionBFinished && State->StructuredTerminalEvents >= 2 && State->Pipeline.IsValid()
				&& !State->Pipeline->IsExporting()
				&& State->Pipeline->GetActiveAsyncExportsForTesting() == 0)
			{
				State->bWriterDrainConfirmed = true;
				return true;
			}

			const double NowSeconds = FPlatformTime::Seconds();
			if (!State->bCancellationRequested && (NowSeconds - StartSeconds) > TimeoutSeconds)
			{
				State->bCancellationRequested = true;
				State->CancellationDeadlineSeconds = NowSeconds + RealPipelineCancellationDrainSeconds;
				if (State->Test)
				{
					State->Test->AddError(TEXT("Timed out waiting for session B to reach its terminal state after the cancel/immediate-restart repro."));
				}
				if (State->Pipeline.IsValid() && State->Pipeline->IsExporting())
				{
					State->Pipeline->CancelExporting(false);
				}
				return false;
			}

			if (State->bCancellationRequested && NowSeconds > State->CancellationDeadlineSeconds)
			{
				State->bWaitTimedOut = true;
				if (State->Test)
				{
					State->Test->AddError(TEXT("Session B did not confirm terminal delivery and writer inactivity before the cancellation deadline."));
				}
				const TSharedRef<FAsyncCancelRestartState> DeferredState = State;
				const double CleanupDeadlineSeconds = NowSeconds + RealPipelineCancellationDrainSeconds;
				FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([DeferredState, CleanupDeadlineSeconds](float)
				{
					if (!DeferredState->Pipeline.IsValid())
					{
						if (DeferredState->WaveA)
						{
							DeferredState->WaveA->RemoveFromRoot();
							DeferredState->WaveA = nullptr;
						}
						if (DeferredState->WaveB)
						{
							DeferredState->WaveB->RemoveFromRoot();
							DeferredState->WaveB = nullptr;
						}
						return false;
					}
					const bool bDrained = DeferredState->bSessionBFinished
						&& DeferredState->StructuredTerminalEvents >= 2
						&& !DeferredState->Pipeline->IsExporting()
						&& DeferredState->Pipeline->GetActiveAsyncExportsForTesting() == 0;
					if (!bDrained)
					{
						if (FPlatformTime::Seconds() > CleanupDeadlineSeconds)
						{
							if (DeferredState->Test)
							{
								DeferredState->Test->AddError(TEXT("Async cancel-restart retained cleanup exceeded the named writer-drain deadline; live pipeline and rooted sources remain preserved."));
							}
							AudioExporterRuntimeTestUtils::RegisterRetainedTestCleanup(
								[DeferredState]()
								{
									return DeferredState->bSessionBFinished
										&& DeferredState->StructuredTerminalEvents >= 2
										&& DeferredState->Pipeline.IsValid()
										&& !DeferredState->Pipeline->IsExporting()
										&& DeferredState->Pipeline->GetActiveAsyncExportsForTesting() == 0;
								},
								[DeferredState]()
								{
									if (DeferredState->WaveA)
									{
										DeferredState->WaveA->RemoveFromRoot();
										DeferredState->WaveA = nullptr;
									}
									if (DeferredState->WaveB)
									{
										DeferredState->WaveB->RemoveFromRoot();
										DeferredState->WaveB = nullptr;
									}
									DeferredState->Pipeline.Reset();
								},
								[DeferredState]()
								{
									if (DeferredState->Pipeline.IsValid()
										&& (DeferredState->Pipeline->IsExporting()
											|| DeferredState->Pipeline->GetActiveAsyncExportsForTesting() != 0))
									{
										if (DeferredState->Test)
										{
											DeferredState->Test->AddError(TEXT("Async cancel-restart retained live pipeline state during engine shutdown because the pipeline was still active or had async writers."));
										}
										return false;
									}
									if ((!DeferredState->bSessionBFinished || DeferredState->StructuredTerminalEvents < 2) && DeferredState->Test)
									{
										DeferredState->Test->AddError(TEXT("Async cancel-restart reached engine shutdown without both structured terminal results; releasing only after shutdown made the backend inactive."));
									}
									if (DeferredState->WaveA)
									{
										DeferredState->WaveA->RemoveFromRoot();
										DeferredState->WaveA = nullptr;
									}
									if (DeferredState->WaveB)
									{
										DeferredState->WaveB->RemoveFromRoot();
										DeferredState->WaveB = nullptr;
									}
									DeferredState->Pipeline.Reset();
									return true;
								});
							return false;
						}
						return true;
					}
					if (DeferredState->WaveA)
					{
						DeferredState->WaveA->RemoveFromRoot();
						DeferredState->WaveA = nullptr;
					}
					if (DeferredState->WaveB)
					{
						DeferredState->WaveB->RemoveFromRoot();
						DeferredState->WaveB = nullptr;
					}
					DeferredState->Pipeline.Reset();
					return false;
				}));
				return true;
			}

			return false;
		}

	private:
		TSharedRef<FAsyncCancelRestartState> State;
		double TimeoutSeconds = 30.0;
		double StartSeconds = 0.0;
	};

	class FVerifySessionBFinish : public IAutomationLatentCommand
	{
	public:
		explicit FVerifySessionBFinish(const TSharedRef<FAsyncCancelRestartState>& InState)
			: State(InState)
		{
		}

		virtual bool Update() override
		{
			FAutomationTestBase* Test = State->Test;
			if (!Test)
			{
				return true;
			}
			if (!State->bWriterDrainConfirmed || State->bWaitTimedOut)
			{
				Test->AddError(TEXT("Session B cleanup was reached without confirmed terminal delivery and writer inactivity."));
				return true;
			}

			Test->TestTrue(TEXT("Session B should reach its terminal state after the cancel/immediate-restart repro."), State->bSessionBFinished);
			Test->TestTrue(TEXT("At least two terminal events should fire: session A's cancel and session B's finish."), State->TerminalPaths.Num() >= 2);
			Test->TestEqual(TEXT("Both sessions should emit one structured terminal result."), State->StructuredTerminalEvents, 2);
			if (!State->TerminalPaths.IsEmpty())
			{
				Test->TestTrue(TEXT("Session A's terminal event (cancel) should report an empty path."), State->TerminalPaths[0].IsEmpty());
			}
			if (State->TerminalPaths.Num() >= 2)
			{
				Test->TestFalse(TEXT("Session B's terminal event should report a non-empty export path."), State->TerminalPaths.Last().IsEmpty());
			}

			if (State->Pipeline.IsValid())
			{
				// A corrupted counter would leave the pipeline stuck waiting forever, so this is
				// the direct regression assertion -- session B must reach IsExporting() == false.
				Test->TestFalse(TEXT("The pipeline should be inactive once session B finalizes."), State->Pipeline->IsExporting());
				Test->TestEqual(TEXT("ActiveAsyncExports should have drained back to zero after both sessions settle."), State->Pipeline->GetActiveAsyncExportsForTesting(), 0);
			}

			// Session B's own output folder should contain exactly its own WAV -- proof the
			// restarted session finalized cleanly and was not corrupted by session A's stale
			// in-flight save.
			TArray<FString> WavFilesB;
			FindWavFilesUnder(State->OutputRootB, WavFilesB);
			Test->TestEqual(TEXT("Session B should write exactly one WAV file."), WavFilesB.Num(), 1);
			if (!WavFilesB.IsEmpty())
			{
				ValidateWavOutput(*Test, WavFilesB[0], TEXT("AsyncCancelRestartSessionB"));
			}

			if (State->WaveA)
			{
				State->WaveA->RemoveFromRoot();
				State->WaveA = nullptr;
			}
			if (State->WaveB)
			{
				State->WaveB->RemoveFromRoot();
				State->WaveB = nullptr;
			}
			return true;
		}

	private:
		TSharedRef<FAsyncCancelRestartState> State;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterAsyncFinalBatchDrainTest,
	"AudioFactory.AudioExporter.Runtime.Lifecycle.AsyncFinalBatchDrain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterAsyncFinalBatchDrainTest::RunTest(const FString& Parameters)
{
	// Production always flushes the last sound's batch with bIsFinal=false (OnVariationFinished),
	// so the immediately following ExportCurrentBatch(true) call (FinishExporting) finds nothing pending
	// and must wait for that batch's async save via FinalizeOrWaitForAsyncSaves() instead of finalizing
	// early. SimulateVariationFinishedForTesting() reproduces that exact sequence deterministically:
	// headless automation cannot drive FAudioRendererRealtime's real playback (no play-state/watchdog
	// callback ever fires under -nullrhi, so a real Render() request just sits idle), so the seam
	// fabricates a variation buffer and runs the real ScheduleVariationSave/FinishExporting/
	// ExportCurrentBatch(true)/FinalizeOrWaitForAsyncSaves cascade in its place.
	UWorld* World = AudioExporterRuntimeTestUtils::ResolveAutomationWorld();
	if (!World)
	{
		AddError(TEXT("Async final-batch drain test is unavailable: no editor world was available."));
		return false;
	}

	USoundWaveProcedural* Wave = AudioExporterRuntimeTestUtils::MakeTransientToneWave(TEXT("AudioExporterAsyncDrainTone"));
	TestNotNull(TEXT("The async drain test should create a transient tone wave."), Wave);
	if (!Wave)
	{
		return false;
	}

	const TSharedRef<FAudioExportPipeline> Pipeline = MakeShared<FAudioExportPipeline>();
	Pipeline->Initialize(World);
	Pipeline->SetForceAsyncSaveForTesting(true);

	const TSharedRef<AudioExporterRuntimeTestUtils::FAsyncDrainState> State = MakeShared<AudioExporterRuntimeTestUtils::FAsyncDrainState>();
	State->Test = this;
	State->Pipeline = Pipeline;
	State->Wave = Wave;
	State->OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("AsyncFinalBatchDrain"));
	State->StartSeconds = FPlatformTime::Seconds();

	Pipeline->OnExportFinished.AddLambda([State](const FString& InPath)
	{
		++State->TerminalEvents;
		State->FinishedPath = InPath;
		State->bFinished = true;
	});
	Pipeline->OnExportResult.AddLambda([State](const FAudioExportResult&)
	{
		++State->StructuredTerminalEvents;
	});

	FAudioExportSoundEntry Entry;
	Entry.Sound = Wave;
	Entry.SoundName = TEXT("AudioExporterAsyncDrainTone");
	Entry.SourceAssetPath = TEXT("/Temp/AudioExporterAsyncDrainTone");
	Entry.RelativePath = TEXT("AsyncDrain");

	TArray<FAudioExportSoundEntry> Entries;
	Entries.Add(Entry);

	FAudioExporterConfig Config;
	Config.OneShotVariations = 1;
	Config.LoopVariations = 1;
	Config.MaxLengthSeconds = 5;
	Config.bNormalize = false;
	Config.bMonitor = false;
	Config.bExportTextLog = true;

	// The fixture session initializes the production queue/log/accounting state without requiring a
	// mixer device. The seam below then drives the pipeline's actual completion bookkeeping.
	Pipeline->BeginLifecycleTestSession(Entries, Config, State->OutputRoot);
	TestTrue(TEXT("The pipeline should be exporting immediately after the fixture session starts."), Pipeline->IsExporting());

	Pipeline->SimulateVariationFinishedForTesting();
	TestEqual(TEXT("An async save should be scheduled immediately after simulating the variation."), Pipeline->GetActiveAsyncExportsForTesting(), 1);
	TestTrue(TEXT("The pipeline should still report exporting while the final async save drains."), Pipeline->IsExporting());

	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FWaitForAsyncDrainFinish(State));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FVerifyAsyncDrainFinish(State));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterAsyncCancelImmediateRestartTest,
	"AudioFactory.AudioExporter.Runtime.Lifecycle.AsyncCancelImmediateRestartCounter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterAsyncCancelImmediateRestartTest::RunTest(const FString& Parameters)
{
	// Session A schedules a background save (ActiveAsyncExports > 0); the user cancels
	// while it is still in flight (CancelExportingInternal correctly skips ResetState() when the
	// counter is non-zero); the user immediately starts session B on the same pipeline. ResetState()
	// must leave ActiveAsyncExports alone -- zeroing it there would drive the counter negative as
	// session A's stale task retires, so session B's finalize gate never fires (no log file, no
	// OnExportFinished broadcast, progress window hung).
	//
	// SimulateVariationFinishedForTesting() makes "an async save is in flight" deterministic instead of
	// polling for a real render that can never complete headless: ActiveAsyncExports becomes non-zero
	// synchronously inside the seam call (ScheduleVariationSave runs on this same call stack), and
	// nothing can decrement it before this function regains control -- the async task's own completion
	// is queued as a GameThread continuation that can only run on a later tick, never mid-stack. That
	// makes the immediately-following CancelExporting()/StartExporting() pair a faithful, race-free
	// reproduction of "cancel while in flight, then immediately restart."
	UWorld* World = AudioExporterRuntimeTestUtils::ResolveAutomationWorld();
	if (!World)
	{
		AddError(TEXT("Cancel/immediate-restart test is unavailable: no editor world was available."));
		return false;
	}

	USoundWaveProcedural* WaveA = AudioExporterRuntimeTestUtils::MakeTransientToneWave(TEXT("AudioExporterCancelRestartToneA"));
	USoundWaveProcedural* WaveB = AudioExporterRuntimeTestUtils::MakeTransientToneWave(TEXT("AudioExporterCancelRestartToneB"));
	TestNotNull(TEXT("Session A's tone wave should be created."), WaveA);
	TestNotNull(TEXT("Session B's tone wave should be created."), WaveB);
	if (!WaveA || !WaveB)
	{
		if (WaveA) { WaveA->RemoveFromRoot(); }
		if (WaveB) { WaveB->RemoveFromRoot(); }
		return false;
	}

	const TSharedRef<FAudioExportPipeline> Pipeline = MakeShared<FAudioExportPipeline>();
	Pipeline->Initialize(World);
	Pipeline->SetForceAsyncSaveForTesting(true);

	const TSharedRef<AudioExporterRuntimeTestUtils::FAsyncCancelRestartState> State = MakeShared<AudioExporterRuntimeTestUtils::FAsyncCancelRestartState>();
	State->Test = this;
	State->Pipeline = Pipeline;
	State->WaveA = WaveA;
	State->WaveB = WaveB;
	State->OutputRootA = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("AsyncCancelRestart_SessionA"));
	State->OutputRootB = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("AsyncCancelRestart_SessionB"));

	Pipeline->OnExportFinished.AddLambda([State](const FString& InPath)
	{
		State->TerminalPaths.Add(InPath);
		if (!InPath.IsEmpty())
		{
			// Only session B ever finalizes with a non-empty path; session A's cancel always
			// broadcasts empty (see CancelExportingInternal).
			State->bSessionBFinished = true;
		}
	});
	Pipeline->OnExportResult.AddLambda([State](const FAudioExportResult&)
	{
		++State->StructuredTerminalEvents;
	});

	// --- Session A: start, simulate its one variation so an async save is genuinely in flight -------
	FAudioExportSoundEntry EntryA;
	EntryA.Sound = WaveA;
	EntryA.SoundName = TEXT("AudioExporterCancelRestartToneA");
	EntryA.SourceAssetPath = TEXT("/Temp/AudioExporterCancelRestartToneA");
	EntryA.RelativePath = TEXT("CancelRestartA");

	TArray<FAudioExportSoundEntry> EntriesA;
	EntriesA.Add(EntryA);

	FAudioExporterConfig ConfigA;
	ConfigA.OneShotVariations = 1;
	ConfigA.LoopVariations = 1;
	ConfigA.MaxLengthSeconds = 5;
	ConfigA.bNormalize = false;
	ConfigA.bMonitor = false;
	ConfigA.bExportTextLog = true;

	Pipeline->BeginLifecycleTestSession(EntriesA, ConfigA, State->OutputRootA);
	TestTrue(TEXT("Session A should be exporting immediately after the fixture session starts."), Pipeline->IsExporting());

	Pipeline->SimulateVariationFinishedForTesting();
	TestEqual(TEXT("Session A should have exactly one async save in flight immediately after the seam call."), Pipeline->GetActiveAsyncExportsForTesting(), 1);
	TestTrue(TEXT("Session A should still report exporting while its final async save drains."), Pipeline->IsExporting());

	// --- Cancel session A while its save is in flight, then immediately start session B -------------
	Pipeline->CancelExporting(false);
	// Deliberately a RANGE, not TestEqual(..., 1): cancel sets bCancelRequested, which is exactly the
	// condition the already-queued background task checks first, so it takes its fast bail-and-decrement
	// path instead of the full encode-and-write. Whether that lands before or after this line is a real
	// race on the background scheduler, and asserting 1 here failed reproducibly under full-suite load
	// while passing on a lighter run. The value cannot prove the ResetState invariant either way (a
	// ResetState that zeroed the counter would also read 0 here), so this only guards against
	// corruption/inflation. The deterministic proof of "ResetState must not touch ActiveAsyncExports"
	// is FVerifySessionBFinish's terminal drain-to-exactly-zero assertion: if ResetState had zeroed the
	// counter, session A's stale task would then decrement it to -1 and that check would catch it.
	const int32 AsyncExportsAfterCancel = Pipeline->GetActiveAsyncExportsForTesting();
	TestTrue(FString::Printf(TEXT("ActiveAsyncExports must stay uncorrupted across the cancel (expected 0 or 1, got %d)."), AsyncExportsAfterCancel),
		AsyncExportsAfterCancel >= 0 && AsyncExportsAfterCancel <= 1);

	FAudioExportSoundEntry EntryB;
	EntryB.Sound = WaveB;
	EntryB.SoundName = TEXT("AudioExporterCancelRestartToneB");
	EntryB.SourceAssetPath = TEXT("/Temp/AudioExporterCancelRestartToneB");
	EntryB.RelativePath = TEXT("CancelRestartB");

	TArray<FAudioExportSoundEntry> EntriesB;
	EntriesB.Add(EntryB);

	FAudioExporterConfig ConfigB;
	ConfigB.OneShotVariations = 1;
	ConfigB.LoopVariations = 1;
	ConfigB.MaxLengthSeconds = 5;
	ConfigB.bNormalize = false;
	ConfigB.bMonitor = false;
	ConfigB.bExportTextLog = true;

	Pipeline->BeginLifecycleTestSession(EntriesB, ConfigB, State->OutputRootB);
	TestTrue(TEXT("Session B should be exporting immediately after the fixture session starts."), Pipeline->IsExporting());

	Pipeline->SimulateVariationFinishedForTesting();
	TestTrue(TEXT("Session B should still report exporting while its own final async save drains."), Pipeline->IsExporting());

	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FWaitForSessionBFinish(State));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FVerifySessionBFinish(State));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterAsyncRestartFailurePreservesOutputTest,
	"AudioFactory.AudioExporter.Runtime.Lifecycle.AsyncRestartFailurePreservesOutput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterAsyncRestartFailurePreservesOutputTest::RunTest(const FString& Parameters)
{
	// The batch restart fails only after the real ScheduleVariationSave has dispatched an async
	// save. The session directory must remain available for that save, and terminal notification
	// must wait for the save's game-thread result instead of resetting the session early.
	const TSharedRef<AudioExporterRuntimeTestUtils::FExposedAsyncRestartPipeline> Pipeline = MakeShared<AudioExporterRuntimeTestUtils::FExposedAsyncRestartPipeline>();
	Pipeline->SetForceAsyncSaveForTesting(true);

	const TSharedRef<AudioExporterRuntimeTestUtils::FAsyncDrainState> State = MakeShared<AudioExporterRuntimeTestUtils::FAsyncDrainState>();
	State->Test = this;
	State->Pipeline = Pipeline;
	State->OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("AsyncRestartFailurePreservesOutput"));
	State->StartSeconds = FPlatformTime::Seconds();

	Pipeline->OnExportFinished.AddLambda([State](const FString& InPath)
	{
		++State->TerminalEvents;
		State->FinishedPath = InPath;
		State->bFinished = true;
	});
	Pipeline->OnExportResult.AddLambda([State](const FAudioExportResult&)
	{
		++State->StructuredTerminalEvents;
	});

	FAudioExportSoundEntry Entry;
	Entry.SoundName = TEXT("AudioExporterAsyncRestartFailureTone");
	Entry.SourceAssetPath = TEXT("/Temp/AudioExporterAsyncRestartFailureTone");
	Entry.RelativePath = TEXT("AsyncRestartFailure");

	FAudioExporterConfig Config;
	Config.OneShotVariations = 1;
	Config.LoopVariations = 1;
	Config.MaxLengthSeconds = 5;
	Config.bNormalize = false;
	Config.bMonitor = false;
	Config.bExportTextLog = false;

	TArray<FAudioExportSoundEntry> Entries;
	Entries.Add(Entry);
	FAudioExportSoundEntry NextRenderedEntry;
	NextRenderedEntry.Sound = NewObject<USoundCue>(GetTransientPackage(), TEXT("AudioExporterAsyncRestartNextCue"));
	NextRenderedEntry.SoundName = TEXT("AudioExporterAsyncRestartNextCue");
	NextRenderedEntry.SourceAssetPath = TEXT("/Temp/AudioExporterAsyncRestartNextCue");
	NextRenderedEntry.RelativePath = TEXT("AsyncRestartFailure");
	Entries.Add(NextRenderedEntry);
	Pipeline->BeginLifecycleTestSession(Entries, Config, State->OutputRoot);

	TUniquePtr<AudioExporterRuntimeTestUtils::FRestartFailureRenderer> Renderer = MakeUnique<AudioExporterRuntimeTestUtils::FRestartFailureRenderer>();
	Pipeline->Renderer = MoveTemp(Renderer);

	FPipelineVariationCaptureData Variation;
	Variation.SoundName = Entry.SoundName;
	Variation.SourceAssetPath = Entry.SourceAssetPath;
	Variation.RelativePath = Entry.RelativePath;
	Variation.VariationIndex = 0;
	Variation.SoundIndex = 0;
	Variation.TotalVariations = 1;
	Variation.StartTime = 0.0;
	Variation.EndTime = 0.05;
	Pipeline->PendingVariations.Add(Variation);

	AddExpectedError(TEXT("FAudioExportPipeline::FailStartup: Failed to restart batch recording for the next batch."), EAutomationExpectedErrorFlags::Contains, 1);
	Pipeline->ExportCurrentBatch(false);

	TestFalse(TEXT("A failed mid-session restart must stop the active session immediately."), Pipeline->IsExporting());
	TestEqual(TEXT("The restart-failure repro must leave one async save in flight before the drain."), Pipeline->GetActiveAsyncExportsForTesting(), 1);
	TestTrue(TEXT("The session directory must remain until the dispatched async save settles."), IFileManager::Get().DirectoryExists(*State->OutputRoot));

	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FWaitForAsyncDrainFinish(State));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FVerifyAsyncDrainFinish(State));
	return true;
}

// ---------------------------------------------------------------------------
// Mid-sound render failure, structured terminal result, rejected-start
// settlement, and golden report fixtures. These tests drive the
// real pipeline/subsystem/logger code (no hand-built report rows): the seam only
// replaces the renderer capture, never the bookkeeping or report writing.
// ---------------------------------------------------------------------------
namespace AudioExporterRuntimeTestUtils
{
	/** Renderer probe used only to prove dispatch policy. It never initializes an audio device. */
	class FDispatchProbeRenderer final : public FAudioRendererRealtime
	{
	public:
		int32 RenderCalls = 0;
		bool bBatchRecording = false;

		virtual bool IsInBatchRecording() const override { return bBatchRecording; }
		virtual bool StartBatchRecording() override
		{
			bBatchRecording = true;
			return true;
		}
		virtual void StopBatchRecording(TArray<float>& OutBuffer, int32& OutNumChannels, int32& OutSampleRate) override
		{
			bBatchRecording = false;
			OutBuffer.Reset();
			OutNumChannels = 0;
			OutSampleRate = 0;
		}
		virtual bool Render(USoundBase*, float) override
		{
			++RenderCalls;
			return true;
		}
	};

	/** Bookkeeping-only probe for invalid SoundWave-volume validation; this is not a live capture contract. */
	class FVolumeValidationDispatchProbePipeline final : public FAudioExportPipeline
	{
	public:
		int32 EnsureRendererCalls = 0;
		FDispatchProbeRenderer* ProbeRenderer = nullptr;

	protected:
		virtual bool EnsureRenderer() override
		{
			++EnsureRendererCalls;
			if (!Renderer.IsValid())
			{
				TUniquePtr<FDispatchProbeRenderer> NewRenderer = MakeUnique<FDispatchProbeRenderer>();
				ProbeRenderer = NewRenderer.Get();
				Renderer = MoveTemp(NewRenderer);
			}
			return true;
		}
	};

	/** Bookkeeping-only probe for dispatch policy; this is not a live capture contract. */
	class FDispatchPolicyProbePipeline final : public FAudioExportPipeline
	{
	public:
		FDispatchProbeRenderer* ProbeRenderer = nullptr;

	protected:
		virtual bool EnsureRenderer() override
		{
			if (!Renderer.IsValid())
			{
				TUniquePtr<FDispatchProbeRenderer> NewRenderer = MakeUnique<FDispatchProbeRenderer>();
				ProbeRenderer = NewRenderer.Get();
				Renderer = MoveTemp(NewRenderer);
			}
			return true;
		}
	};

	struct FDispatchPolicyProbeDrainState
	{
		FAutomationTestBase* Test = nullptr;
		TSharedPtr<FAudioExportPipeline> Pipeline;
		TObjectPtr<USoundBase> Source = nullptr;
		FDispatchProbeRenderer* ProbeRenderer = nullptr;
		int32 TerminalEvents = 0;
		int32 StructuredTerminalEvents = 0;
		double DeadlineSeconds = 0.0;
	};

	class FWaitForDispatchPolicyProbeDrain final : public IAutomationLatentCommand
	{
	public:
		explicit FWaitForDispatchPolicyProbeDrain(const TSharedRef<FDispatchPolicyProbeDrainState>& InState)
			: State(InState)
		{
		}

		virtual bool Update() override
		{
			const bool bDrained = State->TerminalEvents == 1
				&& State->StructuredTerminalEvents == 1
				&& State->Pipeline.IsValid()
				&& !State->Pipeline->IsExporting()
				&& State->Pipeline->GetActiveAsyncExportsForTesting() == 0;
			if (bDrained)
			{
				if (State->Test)
				{
					State->Test->TestNotNull(TEXT("The dispatch-policy probe must initialize its renderer."), State->ProbeRenderer);
					if (State->ProbeRenderer)
					{
						State->Test->TestEqual(TEXT("The dispatch-policy probe must receive exactly one render dispatch."), State->ProbeRenderer->RenderCalls, 1);
					}
					State->Test->TestEqual(TEXT("The dispatch-policy probe must emit exactly one legacy terminal event."), State->TerminalEvents, 1);
					State->Test->TestEqual(TEXT("The dispatch-policy probe must emit exactly one structured terminal result."), State->StructuredTerminalEvents, 1);
					State->Test->TestEqual(TEXT("The dispatch-policy probe must drain all async writers before source release."), State->Pipeline->GetActiveAsyncExportsForTesting(), 0);
				}
				if (State->Source)
				{
					State->Source->RemoveFromRoot();
					State->Source = nullptr;
				}
				State->Pipeline.Reset();
				return true;
			}

			if (FPlatformTime::Seconds() > State->DeadlineSeconds)
			{
				if (State->Test)
				{
					State->Test->AddError(TEXT("The dispatch-policy probe did not drain its terminal result and async writers before the bounded cancellation deadline; the rooted source remains preserved."));
				}
				const TSharedRef<FDispatchPolicyProbeDrainState> RetainedState = State;
				AudioExporterRuntimeTestUtils::RegisterRetainedTestCleanup(
					[RetainedState]()
					{
						return RetainedState->TerminalEvents == 1
							&& RetainedState->StructuredTerminalEvents == 1
							&& RetainedState->Pipeline.IsValid()
							&& !RetainedState->Pipeline->IsExporting()
							&& RetainedState->Pipeline->GetActiveAsyncExportsForTesting() == 0;
					},
					[RetainedState]()
					{
						if (RetainedState->Source)
						{
							RetainedState->Source->RemoveFromRoot();
							RetainedState->Source = nullptr;
						}
						RetainedState->Pipeline.Reset();
					},
					[RetainedState]()
					{
						if (RetainedState->Pipeline.IsValid()
							&& (RetainedState->Pipeline->IsExporting()
								|| RetainedState->Pipeline->GetActiveAsyncExportsForTesting() != 0))
						{
							if (RetainedState->Test)
							{
								RetainedState->Test->AddError(TEXT("The dispatch-policy probe retained live pipeline state during engine shutdown because the pipeline was still active or had async writers."));
							}
							return false;
						}
						if ((RetainedState->TerminalEvents != 1 || RetainedState->StructuredTerminalEvents != 1) && RetainedState->Test)
						{
							RetainedState->Test->AddError(TEXT("The dispatch-policy probe reached engine shutdown without its exact terminal events; releasing only after shutdown made the backend inactive."));
						}
						if (RetainedState->Source)
						{
							RetainedState->Source->RemoveFromRoot();
							RetainedState->Source = nullptr;
						}
						RetainedState->Pipeline.Reset();
						return true;
					});
				return true;
			}
			return false;
		}

	private:
		TSharedRef<FDispatchPolicyProbeDrainState> State;
	};

	/**
	 * Exposes the protected render-error and result-recording entry points so automation can drive
	 * the REAL implementations (mid-batch flush, failure rows, skip counting) without an audio
	 * device. Calling OnRenderError on this class invokes the exact production code path.
	 */
	class FExposedRuntimePipeline final : public FAudioExportPipeline
	{
	public:
		using FAudioExportPipeline::OnRenderError;
		using FAudioExportPipeline::ProcessCurrentSound;
		using FAudioExportPipeline::RecordVariationResult;
		using FAudioExportPipeline::FlushPendingVariations;
		using FAudioExportPipeline::WriteLogFilesToDisk;
		using FAudioExportPipeline::StartNextSound;
		using FAudioExportPipeline::ResetState;
		using FAudioExportPipeline::CurrentSoundToPlay;
		using FAudioExportPipeline::ExportTimestamp;
		using FAudioExportPipeline::PendingVariations;
		using FAudioExportPipeline::Renderer;

		void SetReportTimestampForTesting(const FDateTime& InTimestamp)
		{
			ExportTimestamp = InTimestamp;
		}

		void WriteReportsForTesting()
		{
			WriteLogFilesToDisk();
		}

		void SetCurrentSoundForTesting(USoundBase* InSound)
		{
			CurrentSoundToPlay = InSound;
		}

		USoundBase* GetCurrentSoundForTesting() const
		{
			return CurrentSoundToPlay.Get();
		}

		void ResetStateForTesting()
		{
			ResetState();
		}
	};

	/**
	 * Test-only renderer seam for the all-failure regression. The real public StartExporting path
	 * still owns session setup and current-variation metadata; this renderer only prevents the test
	 * from entering a live audio device while preserving the production error-to-terminal cascade.
	 */
	class FPublicStartFailureRenderer final : public FAudioRendererRealtime
	{
	public:
		virtual bool IsInBatchRecording() const override { return false; }
		virtual bool StartBatchRecording() override { return true; }
	};

	class FPublicStartFailurePipeline final : public FAudioExportPipeline
	{
	public:
		using FAudioExportPipeline::OnRenderError;

		FString GetExportPathForTesting() const
		{
			return ExportPath;
		}

	protected:
		virtual bool EnsureRenderer() override
		{
			Renderer = MakeUnique<FPublicStartFailureRenderer>();
			return true;
		}

		virtual void ProcessCurrentSound() override
		{
			// The regression drives the real OnRenderError callback explicitly after public startup.
		}
	};

	/**
	 * A deterministic test double for FAudioRendererRealtime's batch-recording surface.
	 * Reports "in batch recording" against a canned buffer/duration so FlushPendingVariations's real
	 * salvage-extraction branch (Renderer->IsInBatchRecording()/StopBatchRecording()/GetBatchStartTime()/
	 * GetBatchEndTime()) runs against known data, with no live audio device and no timing dependency.
	 * Render()/StartBatchRecording()/Initialize() are never called by the test that uses this.
	 */
	class FFakeBatchRenderer final : public FAudioRendererRealtime
	{
	public:
		TArray<float> CannedBuffer;
		int32 CannedNumChannels = 0;
		int32 CannedSampleRate = 0;
		double CannedBatchStartTime = 0.0;
		double CannedBatchEndTime = 0.0;

		virtual bool IsInBatchRecording() const override { return true; }

		virtual void StopBatchRecording(TArray<float>& OutBuffer, int32& OutNumChannels, int32& OutSampleRate) override
		{
			OutBuffer = CannedBuffer;
			OutNumChannels = CannedNumChannels;
			OutSampleRate = CannedSampleRate;
		}

		virtual double GetBatchStartTime() const override { return CannedBatchStartTime; }
		virtual double GetBatchEndTime() const override { return CannedBatchEndTime; }
	};

	/** Rows carrying one settled classification, counted from the enum rather than a rendered label. */
	static int32 CountRowsWithStatus(const TArray<FAudioExportAssetLog>& Logs, EAudioExportRowStatus Status)
	{
		int32 Count = 0;
		for (const FAudioExportAssetLog& AssetLog : Logs)
		{
			for (const FAudioExportLogEntry& Entry : AssetLog.Variations)
			{
				if (Entry.Status == Status)
				{
					++Count;
				}
			}
		}
		return Count;
	}

	/** True when any row is still Pending, i.e. a settled session left a row unclassified. */
	static bool HasPendingRow(const TArray<FAudioExportAssetLog>& Logs)
	{
		return CountRowsWithStatus(Logs, EAudioExportRowStatus::Pending) > 0;
	}

	static int32 CountOccurrences(const FString& Haystack, const FString& Needle)
	{
		if (Needle.IsEmpty())
		{
			return 0;
		}

		int32 Count = 0;
		int32 StartIndex = 0;
		while (true)
		{
			const int32 FoundIndex = Haystack.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, StartIndex);
			if (FoundIndex == INDEX_NONE)
			{
				break;
			}
			++Count;
			StartIndex = FoundIndex + Needle.Len();
		}
		return Count;
	}

	static bool LoadReportFile(const FString& ReportDir, const TCHAR* FileName, FString& OutContents)
	{
		const FString ReportPath = ReportDir / FileName;
		return FFileHelper::LoadFileToString(OutContents, *ReportPath);
	}

	/** Builds the fixture sound entry used by the render-failure and report tests. */
	static FAudioExportSoundEntry MakeFixtureEntry(USoundBase* Sound, const TCHAR* Name, const TCHAR* AssetPath)
	{
		FAudioExportSoundEntry Entry;
		Entry.Sound = Sound;
		Entry.SoundName = Name;
		Entry.SourceAssetPath = AssetPath;
		Entry.RelativePath = TEXT("FailureFixture");
		return Entry;
	}

	/** Builds the exporter config used by the render-failure tests. */
	static FAudioExporterConfig MakeFailureFixtureConfig(int32 VariationCount)
	{
		FAudioExporterConfig Config;
		Config.OneShotVariations = VariationCount;
		Config.LoopVariations = VariationCount;
		Config.MaxLengthSeconds = 5;
		Config.bNormalize = false;
		Config.bMonitor = false;
		Config.bExportTextLog = true;
		return Config;
	}

	static FString MakeFixtureVariationData(
		FPipelineVariationCaptureData& OutData,
		const FString& SoundName,
		const FString& SourceAssetPath,
		const FString& RelativePath,
		int32 VariationIndex,
		int32 SoundIndex,
		int32 TotalVariations)
	{
		OutData.StartTime = static_cast<double>(VariationIndex) * 1.0;
		OutData.EndTime = OutData.StartTime + 0.5;
		OutData.SoundName = SoundName;
		OutData.SourceAssetPath = SourceAssetPath;
		OutData.RelativePath = RelativePath;
		OutData.VariationIndex = VariationIndex;
		OutData.SoundIndex = SoundIndex;
		OutData.TotalVariations = TotalVariations;
		OutData.bIsLoop = false;
		return SourceAssetPath;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterDefaultRootSessionPathTest,
	"AudioFactory.AudioExporter.Runtime.Lifecycle.DefaultRootSessionPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterDefaultRootSessionPathTest::RunTest(const FString& Parameters)
{
	UWorld* World = AudioExporterRuntimeTestUtils::ResolveAutomationWorld();
	if (!World)
	{
		AddError(TEXT("Default-root session path test is unavailable: no editor world was available."));
		return false;
	}

	USoundCue* FixtureSound = NewObject<USoundCue>(GetTransientPackage(), TEXT("AudioExporterDefaultRootSessionCue"));
	TestNotNull(TEXT("The default-root session path test should create a transient fixture Cue."), FixtureSound);
	if (!FixtureSound)
	{
		return false;
	}

	const TSharedRef<AudioExporterRuntimeTestUtils::FPublicStartFailurePipeline> Pipeline =
		MakeShared<AudioExporterRuntimeTestUtils::FPublicStartFailurePipeline>();
	Pipeline->Initialize(World);
	const TArray<FAudioExportSoundEntry> Entries = {
		AudioExporterRuntimeTestUtils::MakeFixtureEntry(
			FixtureSound,
			TEXT("AudioExporterDefaultRootSessionCue"),
			TEXT("/Temp/AudioExporterDefaultRootSessionCue"))};

	ON_SCOPE_EXIT
	{
		Pipeline->CancelExporting(true);
	};

	const bool bStarted = Pipeline->StartExporting(Entries, FAudioExporterConfig(), TEXT(""));
	TestTrue(TEXT("An export with an empty custom root should be accepted through public StartExporting."), bStarted);
	if (!bStarted)
	{
		return false;
	}

	FString ExpectedDefaultRoot = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("AudioExporter"));
	FPaths::NormalizeDirectoryName(ExpectedDefaultRoot);
	ExpectedDefaultRoot += TEXT("/");

	FString SessionPath = FPaths::ConvertRelativePathToFull(Pipeline->GetExportPathForTesting());
	FPaths::NormalizeDirectoryName(SessionPath);
	TestTrue(TEXT("An empty custom root should create the session under Saved/AudioExporter."),
		SessionPath.StartsWith(ExpectedDefaultRoot, ESearchCase::IgnoreCase));
	TestTrue(TEXT("The default-root session directory should exist while the accepted export is active."),
		IFileManager::Get().DirectoryExists(*SessionPath));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterNormalTeardownEmitsNoWarningOrErrorTest,
	"AudioFactory.AudioExporter.Runtime.Lifecycle.NormalTeardownEmitsNoWarningOrError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterNormalTeardownEmitsNoWarningOrErrorTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FAudioExportPipeline> Pipeline = MakeShared<FAudioExportPipeline>();
	const FString OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("NormalTeardownNoWarningOrError"));
	AudioExporterRuntimeTestUtils::FAudioExporterLogCapture Capture;

	Pipeline->BeginLifecycleTestSession(OutputRoot);
	Pipeline->CancelExporting(false);

	TestEqual(TEXT("Normal teardown must emit no AudioExporter Warning, Error, or Fatal log."),
		Capture.GetWarningOrErrorCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterActiveUnavailableOperationRemainsVisibleTest,
	"AudioFactory.AudioExporter.Runtime.Lifecycle.ActiveUnavailableOperationRemainsVisible",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterActiveUnavailableOperationRemainsVisibleTest::RunTest(const FString& Parameters)
{
	const TSharedRef<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline> Pipeline = MakeShared<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline>();
	FAudioExportSoundEntry Entry;
	Entry.SoundName = TEXT("AudioExporterActiveUnavailableEntry");
	Entry.SourceAssetPath = TEXT("/Temp/AudioExporterActiveUnavailableEntry");
	Entry.RelativePath = TEXT("ActiveUnavailable");
	TArray<FAudioExportSoundEntry> Entries;
	Entries.Add(Entry);

	FAudioExporterConfig Config;
	Config.bExportTextLog = false;
	const FString OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("ActiveUnavailableVisible"));
	AudioExporterRuntimeTestUtils::FAudioExporterLogCapture Capture;

	Pipeline->BeginLifecycleTestSession(Entries, Config, OutputRoot);
	Pipeline->StartNextSound();

	// Belt-and-braces before the first capture read: drain any still-buffered lines
	// (blocking idle flush, engine FOutputDeviceRedirector.cpp:590-608) so the warning
	// emitted by StartNextSound is visible to the assert regardless of redirector timing.
	GLog->FlushThreadedLogs();
	TestTrue(TEXT("An unavailable sound during an active operation must remain visible as Warning or Error."),
		Capture.GetWarningOrErrorCount() > 0);
	// Kill-power hardening: the count above could also be satisfied by the finalize-path
	// "Saved=0 Failed=1." warning; this assert pins the SPECIFIC unavailable-entry emission
	// (AudioExportPipeline.cpp LogUnavailableSound active branch), so deleting it fails here.
	TestTrue(TEXT("The unavailable-entry warning text itself must have been captured."),
		Capture.Contains(TEXT("was unavailable and could not be exported")));
	return true;
}

// Definition must live at file scope (a member function of a global class cannot be defined inside
// a namespace on MSVC).
void UAudioExporterTestResultSink::HandleExportResult(const FAudioExportResult& InResult)
{
	LastResult = InResult;
	bReceivedResult = true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterMidBatchRenderFailureFlushesCapturedVariationsTest,
	"AudioFactory.AudioExporter.Runtime.MidBatchFailure.FlushesCapturedVariations",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterMidBatchRenderFailureFlushesCapturedVariationsTest::RunTest(const FString& Parameters)
{
	// A 3-variation asset whose variation 3 render fails must preserve the two already-
	// saved variations (2 files on disk, 2 succeeded rows) and write one explicit failure row for the
	// failed variation -- with zero "Pending" text in the terminal report.
	UWorld* World = AudioExporterRuntimeTestUtils::ResolveAutomationWorld();
	if (!World)
	{
		AddError(TEXT("Mid-batch render-failure test is unavailable: no editor world was available."));
		return false;
	}

	USoundWaveProcedural* Wave = AudioExporterRuntimeTestUtils::MakeTransientToneWave(TEXT("AudioExporterMidBatchFailureTone"), 0.25f);
	TestNotNull(TEXT("The mid-batch failure test should create a transient tone wave."), Wave);
	if (!Wave)
	{
		return false;
	}

	const TSharedRef<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline> Pipeline = MakeShared<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline>();
	Pipeline->Initialize(World);

	int32 ResultEvents = 0;
	FAudioExportResult TerminalResult;
	int32 LegacyTerminalEvents = 0;
	FString LegacyTerminalPath;
	Pipeline->OnExportResult.AddLambda([&ResultEvents, &TerminalResult](const FAudioExportResult& InResult)
	{
		++ResultEvents;
		TerminalResult = InResult;
	});
	Pipeline->OnExportFinished.AddLambda([&LegacyTerminalEvents, &LegacyTerminalPath](const FString& InPath)
	{
		++LegacyTerminalEvents;
		LegacyTerminalPath = InPath;
	});

	TArray<FAudioExportSoundEntry> Entries;
	Entries.Add(AudioExporterRuntimeTestUtils::MakeFixtureEntry(Wave, TEXT("AudioExporterMidBatchFailureTone"), TEXT("/Temp/AudioExporterMidBatchFailureTone")));

	const FAudioExporterConfig Config = AudioExporterRuntimeTestUtils::MakeFailureFixtureConfig(3);
	const FString OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("MidBatchRenderFailure"));

	Pipeline->BeginLifecycleTestSession(Entries, Config, OutputRoot);
	const bool bStarted = Pipeline->IsExporting();
	TestTrue(TEXT("The 3-variation fixture session should be accepted."), bStarted);
	if (!bStarted)
	{
		Wave->RemoveFromRoot();
		return false;
	}

	// Variations 1-2 complete successfully through the real bookkeeping seam.
	for (int32 Index = 0; Index < 2; ++Index)
	{
		Pipeline->SimulateVariationFinishedForTesting();
	}

	// Variation 3 fails mid-sound; the pipeline must preserve the saved files and record the failure.
	AddExpectedError(TEXT("FAudioExportPipeline::OnRenderError: Simulated variation 3 render failure"), EAutomationExpectedErrorFlags::Contains, 1);
	Pipeline->OnRenderError(TEXT("Simulated variation 3 render failure"));

	TestEqual(TEXT("Mid-batch failure should settle exactly one structured result."), ResultEvents, 1);
	TestEqual(TEXT("Mid-batch failure should settle exactly one legacy terminal event."), LegacyTerminalEvents, 1);
	TestFalse(TEXT("Mid-batch failure legacy path should be non-empty because files were written."), LegacyTerminalPath.IsEmpty());

	TestEqual(TEXT("Mid-batch failure outcome should be PartialSuccess (2 written, 1 failed)."), TerminalResult.Outcome, EAudioExportOutcome::PartialSuccess);
	TestTrue(TEXT("Mid-batch failure session should have been accepted."), TerminalResult.bAcceptedStart);
	TestEqual(TEXT("Mid-batch failure should report 2 succeeded variations."), TerminalResult.SuccessfulCount, 2);
	TestEqual(TEXT("Mid-batch failure should report 1 failed variation."), TerminalResult.FailedCount, 1);
	TestEqual(TEXT("Mid-batch failure should report 0 skipped variations."), TerminalResult.SkippedCount, 0);
	TestFalse(TEXT("Mid-batch failure path should be set because files were written."), TerminalResult.ExportPath.IsEmpty());

	TArray<FString> WavFiles;
	AudioExporterRuntimeTestUtils::FindWavFilesUnder(OutputRoot, WavFiles);
	TestEqual(TEXT("The two saved variations should remain as files on disk."), WavFiles.Num(), 2);

	const FString ReportDir = OutputRoot;
	FString TextLog;
	TestTrue(TEXT("The terminal report should be written after a mid-batch failure."), AudioExporterRuntimeTestUtils::LoadReportFile(ReportDir, TEXT("AudioExporterTextLog.txt"), TextLog));
	// The TXT report never prints a raw Status token, so "!TextLog.Contains(Pending)" would be a
	// tautology. The row classification itself is the honest surface: assert on the settled
	// EAudioExportRowStatus values, which a leaked Pending row fails regardless of rendering.
	TestFalse(TEXT("No settled row may remain Pending after a mid-batch failure."),
		AudioExporterRuntimeTestUtils::HasPendingRow(Pipeline->GetExportLogsForTesting()));
	TestEqual(TEXT("The report should carry exactly 2 succeeded variation rows."),
		AudioExporterRuntimeTestUtils::CountOccurrences(TextLog, TEXT("Exported File Name")), 2);
	TestEqual(TEXT("The report summary should count 2 succeeded."),
		AudioExporterRuntimeTestUtils::CountOccurrences(TextLog, TEXT("Succeeded                   : 2")), 1);
	TestEqual(TEXT("The report summary should count 1 failed."),
		AudioExporterRuntimeTestUtils::CountOccurrences(TextLog, TEXT("Failed                      : 1")), 1);
	TestEqual(TEXT("The report summary should count 3 requested."),
		AudioExporterRuntimeTestUtils::CountOccurrences(TextLog, TEXT("Requested                   : 3")), 1);
	TestTrue(TEXT("The failure row should carry the render error sentence."),
		TextLog.Contains(TEXT("Render failed: Simulated variation 3 render failure")));

	Wave->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterPartialBatchExactCountsTest,
	"AudioFactory.AudioExporter.Runtime.MidBatchFailure.PartialBatchExactCounts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterPartialBatchExactCountsTest::RunTest(const FString& Parameters)
{
	// A partial batch must assert exact success/failure counts end to end. Two sounds, three
	// variations each, the third variation of each failing: 4 succeeded + 2 failed, zero skipped.
	UWorld* World = AudioExporterRuntimeTestUtils::ResolveAutomationWorld();
	if (!World)
	{
		AddError(TEXT("Partial-batch counts test is unavailable: no editor world was available."));
		return false;
	}

	USoundWaveProcedural* WaveA = AudioExporterRuntimeTestUtils::MakeTransientToneWave(TEXT("AudioExporterPartialBatchToneA"), 0.25f);
	USoundWaveProcedural* WaveB = AudioExporterRuntimeTestUtils::MakeTransientToneWave(TEXT("AudioExporterPartialBatchToneB"), 0.25f);
	TestNotNull(TEXT("Partial-batch tone A should be created."), WaveA);
	TestNotNull(TEXT("Partial-batch tone B should be created."), WaveB);
	if (!WaveA || !WaveB)
	{
		if (WaveA) { WaveA->RemoveFromRoot(); }
		if (WaveB) { WaveB->RemoveFromRoot(); }
		return false;
	}

	const TSharedRef<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline> Pipeline = MakeShared<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline>();
	Pipeline->Initialize(World);

	int32 ResultEvents = 0;
	FAudioExportResult TerminalResult;
	Pipeline->OnExportResult.AddLambda([&ResultEvents, &TerminalResult](const FAudioExportResult& InResult)
	{
		++ResultEvents;
		TerminalResult = InResult;
	});

	TArray<FAudioExportSoundEntry> Entries;
	Entries.Add(AudioExporterRuntimeTestUtils::MakeFixtureEntry(WaveA, TEXT("AudioExporterPartialBatchToneA"), TEXT("/Temp/AudioExporterPartialBatchToneA")));
	Entries.Add(AudioExporterRuntimeTestUtils::MakeFixtureEntry(WaveB, TEXT("AudioExporterPartialBatchToneB"), TEXT("/Temp/AudioExporterPartialBatchToneB")));

	const FAudioExporterConfig Config = AudioExporterRuntimeTestUtils::MakeFailureFixtureConfig(3);
	const FString OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("PartialBatchExactCounts"));

	Pipeline->BeginLifecycleTestSession(Entries, Config, OutputRoot);
	const bool bStarted = Pipeline->IsExporting();
	TestTrue(TEXT("The two-sound partial-batch fixture session should be accepted."), bStarted);
	if (!bStarted)
	{
		WaveA->RemoveFromRoot();
		WaveB->RemoveFromRoot();
		return false;
	}

	// Sound A: variations 1-2 succeed; variation 3 fails.
	Pipeline->SimulateVariationFinishedForTesting();
	Pipeline->SimulateVariationFinishedForTesting();
	AddExpectedError(TEXT("FAudioExportPipeline::OnRenderError: Simulated sound A variation 3 failure"), EAutomationExpectedErrorFlags::Contains, 1);
	Pipeline->OnRenderError(TEXT("Simulated sound A variation 3 failure"));

	// Sound B: variations 1-2 succeed; variation 3 fails.
	Pipeline->SimulateVariationFinishedForTesting();
	Pipeline->SimulateVariationFinishedForTesting();
	AddExpectedError(TEXT("FAudioExportPipeline::OnRenderError: Simulated sound B variation 3 failure"), EAutomationExpectedErrorFlags::Contains, 1);
	Pipeline->OnRenderError(TEXT("Simulated sound B variation 3 failure"));

	TestEqual(TEXT("A partial batch should settle exactly one structured result."), ResultEvents, 1);
	TestEqual(TEXT("A partial batch outcome should be PartialSuccess."), TerminalResult.Outcome, EAudioExportOutcome::PartialSuccess);
	TestEqual(TEXT("A partial batch should count exactly 4 succeeded variations."), TerminalResult.SuccessfulCount, 4);
	TestEqual(TEXT("A partial batch should count exactly 2 failed variations."), TerminalResult.FailedCount, 2);
	TestEqual(TEXT("A partial batch should count 0 skipped variations."), TerminalResult.SkippedCount, 0);
	TestEqual(TEXT("A partial batch terminal result must conserve all 6 planned variations."),
		TerminalResult.SuccessfulCount + TerminalResult.FailedCount + TerminalResult.SkippedCount, 6);
	TestFalse(TEXT("A partial batch path should be set because files were written."), TerminalResult.ExportPath.IsEmpty());

	TArray<FString> WavFiles;
	AudioExporterRuntimeTestUtils::FindWavFilesUnder(OutputRoot, WavFiles);
	TestEqual(TEXT("A partial batch should write exactly 4 WAV files."), WavFiles.Num(), 4);

	const FString ReportDir = OutputRoot;
	FString TextLog;
	TestTrue(TEXT("A partial batch should write its terminal report."), AudioExporterRuntimeTestUtils::LoadReportFile(ReportDir, TEXT("AudioExporterTextLog.txt"), TextLog));
	TestEqual(TEXT("A partial batch report should carry exactly 4 succeeded rows."),
		AudioExporterRuntimeTestUtils::CountOccurrences(TextLog, TEXT("Exported File Name")), 4);
	TestEqual(TEXT("A partial batch report should carry exactly 2 failure rows."),
		AudioExporterRuntimeTestUtils::CountOccurrences(TextLog, TEXT("Render failed")), 2);
	TestEqual(TEXT("A partial batch report summary should count 4 succeeded."),
		AudioExporterRuntimeTestUtils::CountOccurrences(TextLog, TEXT("Succeeded                   : 4")), 1);
	TestEqual(TEXT("A partial batch report summary should count 2 failed."),
		AudioExporterRuntimeTestUtils::CountOccurrences(TextLog, TEXT("Failed                      : 2")), 1);
	// Asserted on the row classification rather than the rendered report: the TXT report never
	// prints a raw Status token, which would make a text assertion a tautology.
	TestFalse(TEXT("No settled row may remain Pending in a partial batch."),
		AudioExporterRuntimeTestUtils::HasPendingRow(Pipeline->GetExportLogsForTesting()));

	WaveA->RemoveFromRoot();
	WaveB->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterAllFailureBatchNoSuccessTest,
	"AudioFactory.AudioExporter.Runtime.MidBatchFailure.AllFailureNoGreen",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterAllFailureBatchNoSuccessTest::RunTest(const FString& Parameters)
{
	// A first-variation render failure must conserve all three planned rows: one explicit failure
	// plus two explicit skips for the variations that never started.
	UWorld* World = AudioExporterRuntimeTestUtils::ResolveAutomationWorld();
	if (!World)
	{
		AddError(TEXT("All-failure batch test is unavailable: no editor world was available."));
		return false;
	}

	USoundWaveProcedural* Wave = AudioExporterRuntimeTestUtils::MakeTransientToneWave(TEXT("AudioExporterAllFailureTone"), 0.25f);
	TestNotNull(TEXT("The all-failure test should create a transient tone wave."), Wave);
	if (!Wave)
	{
		return false;
	}

	const TSharedRef<AudioExporterRuntimeTestUtils::FPublicStartFailurePipeline> Pipeline = MakeShared<AudioExporterRuntimeTestUtils::FPublicStartFailurePipeline>();
	Pipeline->Initialize(World);

	int32 ResultEvents = 0;
	FAudioExportResult TerminalResult;
	Pipeline->OnExportResult.AddLambda([&ResultEvents, &TerminalResult](const FAudioExportResult& InResult)
	{
		++ResultEvents;
		TerminalResult = InResult;
	});

	TArray<FAudioExportSoundEntry> Entries;
	Entries.Add(AudioExporterRuntimeTestUtils::MakeFixtureEntry(Wave, TEXT("AudioExporterAllFailureTone"), TEXT("/Temp/AudioExporterAllFailureTone")));

	const FAudioExporterConfig Config = AudioExporterRuntimeTestUtils::MakeFailureFixtureConfig(3);
	const FString OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("AllFailureBatch"));

	const bool bStarted = Pipeline->StartExporting(Entries, Config, OutputRoot);
	TestTrue(TEXT("The all-failure regression must be accepted through public StartExporting."), bStarted);
	if (!bStarted)
	{
		Wave->RemoveFromRoot();
		return false;
	}

	// Public StartExporting always creates a timestamped child session directory. Capture the
	// production-owned path before the terminal failure resets pipeline state; the report assertions
	// below must exercise that actual directory, never the caller's base root or a guessed timestamp.
	FString ReportDir = FPaths::ConvertRelativePathToFull(Pipeline->GetExportPathForTesting());
	FPaths::NormalizeDirectoryName(ReportDir);
	FString OutputRootFull = FPaths::ConvertRelativePathToFull(OutputRoot);
	FPaths::NormalizeDirectoryName(OutputRootFull);
	TestTrue(TEXT("Public StartExporting should create a production session report directory."), !ReportDir.IsEmpty());
	TestTrue(TEXT("The production session report directory should exist before render failure."), IFileManager::Get().DirectoryExists(*ReportDir));
	TestTrue(TEXT("The production session report directory should be a timestamped child of the requested output root."),
		ReportDir.StartsWith(OutputRootFull + TEXT("/"), ESearchCase::IgnoreCase));

	// The very first variation fails; nothing can succeed after it.
	AddExpectedError(TEXT("FAudioExportPipeline::OnRenderError: Simulated first variation render failure"), EAutomationExpectedErrorFlags::Contains, 1);
	Pipeline->OnRenderError(TEXT("Simulated first variation render failure"));

	TestEqual(TEXT("An all-failure batch should settle exactly one structured result."), ResultEvents, 1);
	TestEqual(TEXT("An all-failure batch outcome should be Failed."), TerminalResult.Outcome, EAudioExportOutcome::Failed);
	TestEqual(TEXT("An all-failure batch should count 0 succeeded variations."), TerminalResult.SuccessfulCount, 0);
	TestEqual(TEXT("A first-variation render failure should count 1 failed variation."), TerminalResult.FailedCount, 1);
	TestEqual(TEXT("A first-variation render failure should count 2 skipped variations."), TerminalResult.SkippedCount, 2);
	TestEqual(TEXT("Terminal counts must conserve all 3 planned variations."),
		TerminalResult.SuccessfulCount + TerminalResult.FailedCount + TerminalResult.SkippedCount, 3);
	TestTrue(TEXT("An all-failure batch must report an unset path (no files were written)."), TerminalResult.ExportPath.IsEmpty());

	const TArray<FAudioExportAssetLog> Logs = Pipeline->GetExportLogsForTesting();
	TestEqual(TEXT("The first-variation failure should preserve one asset log."), Logs.Num(), 1);
	int32 TerminalRowCount = 0;
	int32 FailedRowCount = 0;
	int32 SkippedRowCount = 0;
	if (Logs.Num() == 1)
	{
		TestEqual(TEXT("The report input should contain one terminal row for every planned variation."), Logs[0].Variations.Num(), 3);
		for (const FAudioExportLogEntry& Row : Logs[0].Variations)
		{
			++TerminalRowCount;
			TestTrue(TEXT("Every planned variation must settle to a terminal row."), Row.Status != EAudioExportRowStatus::Pending);
			TestEqual(TEXT("Every terminal row should retain the planned variation total."), Row.TotalVariations, 3);
			if (Row.Status == EAudioExportRowStatus::Failed)
			{
				++FailedRowCount;
			}
			else if (Row.Status == EAudioExportRowStatus::Skipped)
			{
				++SkippedRowCount;
			}
		}
	}
	TestEqual(TEXT("Terminal report rows should conserve all 3 planned variations."), TerminalRowCount, 3);
	TestEqual(TEXT("Terminal rows should contain exactly one failed variation."), FailedRowCount, 1);
	TestEqual(TEXT("Terminal rows should contain exactly two skipped variations."), SkippedRowCount, 2);

	TArray<FString> WavFiles;
	AudioExporterRuntimeTestUtils::FindWavFilesUnder(OutputRoot, WavFiles);
	TestEqual(TEXT("An all-failure batch must not write any WAV files."), WavFiles.Num(), 0);

	FString TextLog;
	TestTrue(TEXT("An all-failure batch should still write its terminal report."), AudioExporterRuntimeTestUtils::LoadReportFile(ReportDir, TEXT("AudioExporterTextLog.txt"), TextLog));
	TestEqual(TEXT("A first-variation failure report must contain zero succeeded rows (no green)."),
		AudioExporterRuntimeTestUtils::CountOccurrences(TextLog, TEXT("Exported File Name")), 0);
	TestEqual(TEXT("A first-variation failure report summary should count 3 requested."),
		AudioExporterRuntimeTestUtils::CountOccurrences(TextLog, TEXT("Requested                   : 3")), 1);
	TestEqual(TEXT("A first-variation failure report summary should count 0 succeeded."),
		AudioExporterRuntimeTestUtils::CountOccurrences(TextLog, TEXT("Succeeded                   : 0")), 1);
	TestEqual(TEXT("A first-variation failure report summary should count 1 failed."),
		AudioExporterRuntimeTestUtils::CountOccurrences(TextLog, TEXT("Failed                      : 1")), 1);
	TestEqual(TEXT("A first-variation failure report summary should count 2 skipped."),
		AudioExporterRuntimeTestUtils::CountOccurrences(TextLog, TEXT("Skipped                     : 2")), 1);
	TestEqual(TEXT("A first-variation failure report should carry exactly 1 failure row."),
		AudioExporterRuntimeTestUtils::CountOccurrences(TextLog, TEXT("Render failed")), 1);
	// Asserted on the row classification rather than the rendered report: the TXT report never
	// prints a raw Status token, which would make a text assertion a tautology.
	const TArray<FAudioExportAssetLog> SettledLogs = Pipeline->GetExportLogsForTesting();
	TestFalse(TEXT("No settled row may remain Pending after a first-variation failure."),
		AudioExporterRuntimeTestUtils::HasPendingRow(SettledLogs));
	TestEqual(TEXT("A first-variation failure should settle exactly one Failed row."),
		AudioExporterRuntimeTestUtils::CountRowsWithStatus(SettledLogs, EAudioExportRowStatus::Failed), 1);
	TestEqual(TEXT("A first-variation failure should settle exactly two Skipped rows."),
		AudioExporterRuntimeTestUtils::CountRowsWithStatus(SettledLogs, EAudioExportRowStatus::Skipped), 2);

	Wave->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterTerminalMessageReportFailurePrecedenceTest,
	"AudioFactory.AudioExporter.Runtime.TerminalMessage.ReportFailurePrecedence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterTerminalMessageReportFailurePrecedenceTest::RunTest(const FString& Parameters)
{
	// Both report destinations are real directories with file names, so the production writers
	// reject them while the pipeline still owns a valid export session. The first variation writes
	// a real WAV; the second records a real audio failure before FinalizeExport writes the reports.
	const TSharedRef<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline> Pipeline = MakeShared<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline>();

	FAudioExportSoundEntry Entry;
	Entry.SoundName = TEXT("AudioExporterTerminalMessageTone");
	Entry.SourceAssetPath = TEXT("/Temp/AudioExporterTerminalMessageTone");
	Entry.RelativePath = TEXT("TerminalMessage");

	const FAudioExporterConfig Config = AudioExporterRuntimeTestUtils::MakeFailureFixtureConfig(2);
	const FString OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("TerminalMessageReportFailure"));
	TArray<FAudioExportSoundEntry> Entries;
	Entries.Add(Entry);
	Pipeline->BeginLifecycleTestSession(Entries, Config, OutputRoot);
	TestTrue(TEXT("The report-precedence fixture session should be accepted."), Pipeline->IsExporting());

	TestTrue(TEXT("The text-report blocking directory should be created."), IFileManager::Get().MakeDirectory(*(OutputRoot / TEXT("AudioExporterTextLog.txt")), true));

	int32 ResultEvents = 0;
	FAudioExportResult TerminalResult;
	Pipeline->OnExportResult.AddLambda([&ResultEvents, &TerminalResult](const FAudioExportResult& InResult)
	{
		++ResultEvents;
		TerminalResult = InResult;
	});

	Pipeline->SimulateVariationFinishedForTesting();
	AddExpectedError(TEXT("AudioExporterLogger::WriteTextLogFile: Failed to write text report"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("FAudioExportPipeline::OnRenderError: Simulated terminal audio failure"), EAutomationExpectedErrorFlags::Contains, 1);
	Pipeline->OnRenderError(TEXT("Simulated terminal audio failure"));

	TestEqual(TEXT("Audio and report failure should settle exactly one structured result."), ResultEvents, 1);
	TestEqual(TEXT("Audio plus report failure should be PartialSuccess because one WAV was written."), TerminalResult.Outcome, EAudioExportOutcome::PartialSuccess);
	TestEqual(TEXT("Audio plus report failure should preserve the one successful WAV count."), TerminalResult.SuccessfulCount, 1);
	TestEqual(TEXT("Audio plus report failure should preserve the one audio failure count."), TerminalResult.FailedCount, 1);
	TestEqual(TEXT("A null fixture sound must not create a skipped variation."), TerminalResult.SkippedCount, 0);
	TestEqual(TEXT("Audio plus report failure should report the one failed report output."), TerminalResult.ReportsFailed, 1);
	TestTrue(TEXT("The terminal message must retain the literal audio failure count."), TerminalResult.ErrorMessage.Contains(TEXT("1 variation(s) failed to export.")));
	TestTrue(TEXT("The terminal message must append the literal report failure count."), TerminalResult.ErrorMessage.Contains(TEXT("1 report output(s) failed")));
	TestTrue(TEXT("The terminal message must retain the text-writer detail."), TerminalResult.ErrorMessage.Contains(TEXT("Failed to write text report")));
	TestTrue(TEXT("The structured report error field must retain the writer detail."),
		TerminalResult.ReportErrorMessage.Contains(TEXT("Failed to write text report")));
	TestFalse(TEXT("A mixed audio failure must not be overwritten by a completion-led report message."),
		TerminalResult.ErrorMessage.StartsWith(TEXT("Audio export completed")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterZeroSuccessReportFailureIsNotSuccessTest,
	"AudioFactory.AudioExporter.Runtime.TerminalMessage.ZeroSuccessReportFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterZeroSuccessReportFailureIsNotSuccessTest::RunTest(const FString& Parameters)
{
	// No WAV is written before the render failure. Report failure detail must still be preserved,
	// but the terminal message must remain failure-led and never begin with "Audio export completed".
	const TSharedRef<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline> Pipeline = MakeShared<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline>();

	FAudioExportSoundEntry Entry;
	Entry.SoundName = TEXT("AudioExporterZeroSuccessReportTone");
	Entry.SourceAssetPath = TEXT("/Temp/AudioExporterZeroSuccessReportTone");
	Entry.RelativePath = TEXT("ZeroSuccessReport");

	const FAudioExporterConfig Config = AudioExporterRuntimeTestUtils::MakeFailureFixtureConfig(1);
	const FString OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("ZeroSuccessReportFailure"));
	TArray<FAudioExportSoundEntry> Entries;
	Entries.Add(Entry);
	Pipeline->BeginLifecycleTestSession(Entries, Config, OutputRoot);
	TestTrue(TEXT("The zero-success report fixture session should be accepted."), Pipeline->IsExporting());

	TestTrue(TEXT("The text-report blocking directory should be created for the zero-success case."), IFileManager::Get().MakeDirectory(*(OutputRoot / TEXT("AudioExporterTextLog.txt")), true));

	int32 ResultEvents = 0;
	FAudioExportResult TerminalResult;
	Pipeline->OnExportResult.AddLambda([&ResultEvents, &TerminalResult](const FAudioExportResult& InResult)
	{
		++ResultEvents;
		TerminalResult = InResult;
	});

	AddExpectedError(TEXT("AudioExporterLogger::WriteTextLogFile: Failed to write text report"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("FAudioExportPipeline::OnRenderError: Simulated zero-success audio failure"), EAutomationExpectedErrorFlags::Contains, 1);
	Pipeline->OnRenderError(TEXT("Simulated zero-success audio failure"));

	TestEqual(TEXT("The zero-success report-failure case should settle exactly one result."), ResultEvents, 1);
	TestEqual(TEXT("Zero successful WAVs must produce Failed, not success or partial."), TerminalResult.Outcome, EAudioExportOutcome::Failed);
	TestEqual(TEXT("Zero successful WAVs must report zero successes."), TerminalResult.SuccessfulCount, 0);
	TestEqual(TEXT("The zero-success case should retain its audio failure count."), TerminalResult.FailedCount, 1);
	TestTrue(TEXT("The zero-success terminal message must include report failure detail."), TerminalResult.ErrorMessage.Contains(TEXT("1 report output(s) failed")));
	TestFalse(TEXT("Zero successful WAVs must never lead with completion wording."), TerminalResult.ErrorMessage.StartsWith(TEXT("Audio export completed")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterReportOnlyFailurePreservesSuccessTest,
	"AudioFactory.AudioExporter.Runtime.TerminalMessage.ReportOnlyFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterReportOnlyFailurePreservesSuccessTest::RunTest(const FString& Parameters)
{
	// A successful WAV plus two failed report writes is a report-only terminal failure. The audio
	// success count remains authoritative, and each writer error appears once in the combined text.
	const TSharedRef<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline> Pipeline = MakeShared<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline>();

	FAudioExportSoundEntry Entry;
	Entry.SoundName = TEXT("AudioExporterReportOnlyTone");
	Entry.SourceAssetPath = TEXT("/Temp/AudioExporterReportOnlyTone");
	Entry.RelativePath = TEXT("ReportOnly");

	const FAudioExporterConfig Config = AudioExporterRuntimeTestUtils::MakeFailureFixtureConfig(1);
	const FString OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("ReportOnlyFailure"));
	TArray<FAudioExportSoundEntry> Entries;
	Entries.Add(Entry);
	Pipeline->BeginLifecycleTestSession(Entries, Config, OutputRoot);
	TestTrue(TEXT("The report-only fixture session should be accepted."), Pipeline->IsExporting());

	TestTrue(TEXT("The text-report blocking directory should be created for the report-only case."), IFileManager::Get().MakeDirectory(*(OutputRoot / TEXT("AudioExporterTextLog.txt")), true));

	int32 ResultEvents = 0;
	FAudioExportResult TerminalResult;
	Pipeline->OnExportResult.AddLambda([&ResultEvents, &TerminalResult](const FAudioExportResult& InResult)
	{
		++ResultEvents;
		TerminalResult = InResult;
	});

	AddExpectedError(TEXT("AudioExporterLogger::WriteTextLogFile: Failed to write text report"), EAutomationExpectedErrorFlags::Contains, 1);
	Pipeline->SimulateVariationFinishedForTesting();

	TestEqual(TEXT("The report-only failure should settle exactly one result."), ResultEvents, 1);
	TestEqual(TEXT("Report-only failure should remain non-green PartialSuccess."), TerminalResult.Outcome, EAudioExportOutcome::PartialSuccess);
	TestEqual(TEXT("Report-only failure must preserve the one successful audio count."), TerminalResult.SuccessfulCount, 1);
	TestEqual(TEXT("Report-only failure must not invent an audio failure count."), TerminalResult.FailedCount, 0);
	TestEqual(TEXT("Each report writer error should appear once in the combined terminal message."),
		AudioExporterRuntimeTestUtils::CountOccurrences(TerminalResult.ErrorMessage, TEXT("Failed to write text report")), 1);
	TestEqual(TEXT("The combined terminal message should contain the report-failure count once."),
		AudioExporterRuntimeTestUtils::CountOccurrences(TerminalResult.ErrorMessage, TEXT("1 report output(s) failed")), 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterRejectedStartSettlesRejectedResultTest,
	"AudioFactory.AudioExporter.Runtime.Lifecycle.RejectedStartSettlesRejectedResult",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterRejectedStartSettlesRejectedResultTest::RunTest(const FString& Parameters)
{
	// A rejected start (concurrent session) must settle completion with an explicit Rejected
	// outcome and an unset path through the subsystem's real result channel.
	UWorld* World = AudioExporterRuntimeTestUtils::ResolveAutomationWorld();
	if (!World)
	{
		AddError(TEXT("Rejected-start settlement test is unavailable: no editor world was available."));
		return false;
	}

	UAudioExporterRuntimeSubsystem* Subsystem = NewObject<UAudioExporterRuntimeSubsystem>(GetTransientPackage());
	TestNotNull(TEXT("The rejected-start test should create a runtime subsystem."), Subsystem);
	if (!Subsystem)
	{
		return false;
	}

	const FString FirstSessionPath = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("Lifecycle_RejectedStartSettles_First"));
	if (!Subsystem->BeginLifecycleTestSession(World, FirstSessionPath))
	{
		AddError(TEXT("Rejected-start settlement test could not seed its required lifecycle session."));
		return false;
	}

	int32 ResultEvents = 0;
	FAudioExportResult CapturedResult;
	Subsystem->OnExportCompleteResultNative.AddLambda([&ResultEvents, &CapturedResult](const FAudioExportResult& InResult)
	{
		++ResultEvents;
		CapturedResult = InResult;
	});

	USoundWaveProcedural* Wave = AudioExporterRuntimeTestUtils::MakeTransientToneWave(TEXT("AudioExporterRejectedStartTone"), 0.25f);
	TestNotNull(TEXT("The rejected-start test should create a transient tone wave."), Wave);
	if (!Wave)
	{
		Subsystem->CancelExport(false);
		return false;
	}

	TArray<FAudioExportSoundEntry> Entries;
	Entries.Add(AudioExporterRuntimeTestUtils::MakeFixtureEntry(Wave, TEXT("AudioExporterRejectedStartTone"), TEXT("/Temp/AudioExporterRejectedStartTone")));

	AddExpectedError(TEXT("An export session is already active"), EAutomationExpectedErrorFlags::Contains, 1);
	const bool bStarted = Subsystem->StartExport(World, Entries, FAudioExporterConfig(),
		AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("Lifecycle_RejectedStartSettles_Second")));
	TestFalse(TEXT("A second StartExport during an active session should be rejected."), bStarted);

	TestEqual(TEXT("A rejected start should settle exactly one Rejected result."), ResultEvents, 1);
	TestEqual(TEXT("A rejected start result outcome should be Rejected."), CapturedResult.Outcome, EAudioExportOutcome::Rejected);
	TestFalse(TEXT("A rejected start result must report accepted-start false."), CapturedResult.bAcceptedStart);
	TestTrue(TEXT("A rejected start result must report an unset path."), CapturedResult.ExportPath.IsEmpty());
	TestEqual(TEXT("A rejected start result must report zero counts."),
		CapturedResult.SuccessfulCount + CapturedResult.FailedCount + CapturedResult.SkippedCount, 0);
	TestFalse(TEXT("A rejected start result must carry an error message."), CapturedResult.ErrorMessage.IsEmpty());

	Subsystem->CancelExport(false);
	Wave->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterBPRoundTripRejectedStartTest,
	"AudioFactory.AudioExporter.Runtime.BPLibrary.RoundTripRejectedStart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterBPRoundTripRejectedStartTest::RunTest(const FString& Parameters)
{
	// Producer->consumer round trip: the BP library's own start path must settle a rejected
	// request with an explicit Rejected result and an unset path through the real one-shot delegate.
	UAudioExporterTestResultSink* Sink = NewObject<UAudioExporterTestResultSink>(GetTransientPackage());
	TestNotNull(TEXT("The BP rejected-start round trip should create a result sink."), Sink);
	if (!Sink)
	{
		return false;
	}

	FOnExportCompleteResult OnComplete;
	OnComplete.BindDynamic(Sink, &UAudioExporterTestResultSink::HandleExportResult);

	FAudioExporterConfig Config;
	UAudioExporterBPLibrary::ExportSoundAsyncWithResult(nullptr, nullptr, Config, TEXT(""), OnComplete);

	TestTrue(TEXT("A BP rejected start should fire the one-shot result callback."), Sink->bReceivedResult);
	TestEqual(TEXT("A BP rejected start result outcome should be Rejected."), Sink->LastResult.Outcome, EAudioExportOutcome::Rejected);
	TestFalse(TEXT("A BP rejected start result must report accepted-start false."), Sink->LastResult.bAcceptedStart);
	TestTrue(TEXT("A BP rejected start result must report an unset path."), Sink->LastResult.ExportPath.IsEmpty());
	TestFalse(TEXT("A BP rejected start result must carry an error message."), Sink->LastResult.ErrorMessage.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterSynchronousCompletionCallbackTest,
	"AudioFactory.AudioExporter.Runtime.Lifecycle.SynchronousCompletionCallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterSynchronousCompletionCallbackTest::RunTest(const FString& Parameters)
{
	// The request token is registered before a fixture completion that runs
	// synchronously, so the callback cannot be stranded after StartExport returns.
	UWorld* World = AudioExporterRuntimeTestUtils::ResolveAutomationWorld();
	if (!World)
	{
		AddError(TEXT("Synchronous-completion callback test is unavailable: no editor world was available."));
		return false;
	}

	UAudioExporterRuntimeSubsystem* Subsystem = NewObject<UAudioExporterRuntimeSubsystem>(GetTransientPackage());
	TestNotNull(TEXT("The synchronous-completion test should create a runtime subsystem."), Subsystem);
	if (!Subsystem)
	{
		return false;
	}

	USoundWave* Wave = NewObject<USoundWave>(GetTransientPackage(), TEXT("AudioExporterSynchronousCompletionWave"));
	TestNotNull(TEXT("The synchronous-completion test should create a transient SoundWave."), Wave);
	if (!Wave)
	{
		return false;
	}
	Wave->AddToRoot();

	TArray<FAudioExportSoundEntry> Entries;
	Entries.Add(AudioExporterRuntimeTestUtils::MakeFixtureEntry(
		Wave,
		TEXT("AudioExporterSynchronousCompletionWave"),
		TEXT("/Temp/AudioExporterSynchronousCompletionWave")));

	FAudioExporterConfig Config;
	Config.OneShotVariations = 1;
	Config.LoopVariations = 1;
	Config.bNormalize = false;
	Config.bMonitor = false;
	Config.bExportTextLog = false;

	int32 CallbackEvents = 0;
	FAudioExportResult CallbackResult;
	const FAudioExportRequestToken RequestToken = Subsystem->AddOneTimeResultCallback(
		[&CallbackEvents, &CallbackResult](const FAudioExportResult& InResult)
		{
			++CallbackEvents;
			CallbackResult = InResult;
		});

	const FString OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("SynchronousCompletionCallback"));
	const bool bStarted = Subsystem->BeginLifecycleTestSession(World, Entries, Config, OutputRoot, RequestToken);
	TestTrue(TEXT("The synchronous-completion fixture should be accepted."), bStarted);
	if (bStarted)
	{
		Subsystem->SetForceAsyncSaveForTesting(false);
		Subsystem->SimulateVariationFinishedForTesting();
		TestEqual(TEXT("Synchronous completion should fire the request callback exactly once."), CallbackEvents, 1);
		TestEqual(TEXT("Synchronous completion should report Succeeded."), CallbackResult.Outcome, EAudioExportOutcome::Succeeded);
		TestTrue(TEXT("Synchronous completion should report accepted-start true."), CallbackResult.bAcceptedStart);
		TestFalse(TEXT("Synchronous completion should set ExportPath after writing."), CallbackResult.ExportPath.IsEmpty());
	}

	Wave->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterBPRoundTripCancelledStartTest,
	"AudioFactory.AudioExporter.Runtime.BPLibrary.RoundTripCancelledStart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterBPRoundTripCancelledStartTest::RunTest(const FString& Parameters)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("NoSound")))
	{
		AddError(TEXT("Blueprint cancelled-start round trip is unavailable: this required live-audio regression was launched with -NoSound."));
		return false;
	}

	// Producer->consumer round trip for an accepted start that is then cancelled: the BP
	// library's one-shot result delegate must receive the Cancelled outcome with an unset path.
	UWorld* World = AudioExporterRuntimeTestUtils::ResolveAutomationWorld();
	if (!World)
	{
		AddError(TEXT("Blueprint cancelled-start round trip is unavailable: no editor world was available."));
		return false;
	}

	UAudioExporterRuntimeSubsystem* Subsystem = GEngine->GetEngineSubsystem<UAudioExporterRuntimeSubsystem>();
	TestNotNull(TEXT("The BP cancelled-start round trip should resolve the runtime subsystem."), Subsystem);
	if (!Subsystem)
	{
		return false;
	}

	if (Subsystem->IsExporting())
	{
		Subsystem->CancelExport(false);
	}

	USoundWaveProcedural* Wave = AudioExporterRuntimeTestUtils::MakeTransientToneWave(TEXT("AudioExporterBPCancelTone"), 0.25f);
	TestNotNull(TEXT("The BP cancelled-start round trip should create a transient tone wave."), Wave);
	if (!Wave)
	{
		return false;
	}

	UAudioExporterTestResultSink* Sink = NewObject<UAudioExporterTestResultSink>(GetTransientPackage());
	TestNotNull(TEXT("The BP cancelled-start round trip should create a result sink."), Sink);
	if (!Sink)
	{
		Wave->RemoveFromRoot();
		return false;
	}

	FOnExportCompleteResult OnComplete;
	OnComplete.BindDynamic(Sink, &UAudioExporterTestResultSink::HandleExportResult);

	FAudioExporterConfig Config;
	Config.OneShotVariations = 1;
	Config.LoopVariations = 1;
	Config.MaxLengthSeconds = 5;
	Config.bNormalize = false;
	Config.bMonitor = false;
	Config.bExportTextLog = false;

	const FString OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("BPRoundTrip_Cancelled"));
	UAudioExporterBPLibrary::ExportSoundAsyncWithResult(World, Wave, Config, OutputRoot, OnComplete);

	if (!Subsystem->IsExporting())
	{
		// The environment could not start the pipeline, so the request settled as Rejected; the
		// result channel must still have settled exactly once with an unset path.
		TestTrue(TEXT("A rejected BP start should still fire the one-shot result callback."), Sink->bReceivedResult);
		TestEqual(TEXT("An unstartable BP session should settle as Rejected."), Sink->LastResult.Outcome, EAudioExportOutcome::Rejected);
		TestTrue(TEXT("An unstartable BP session should report an unset path."), Sink->LastResult.ExportPath.IsEmpty());
		Wave->RemoveFromRoot();
		return true;
	}

	// The start was accepted synchronously; cancel it immediately (no async save was scheduled).
	Subsystem->CancelExport(false);

	TestTrue(TEXT("A cancelled BP session should fire the one-shot result callback."), Sink->bReceivedResult);
	TestEqual(TEXT("A cancelled BP session outcome should be Cancelled."), Sink->LastResult.Outcome, EAudioExportOutcome::Cancelled);
	TestTrue(TEXT("A cancelled BP session should report accepted-start true."), Sink->LastResult.bAcceptedStart);
	TestTrue(TEXT("A cancelled BP session should report an unset path (no files were written)."), Sink->LastResult.ExportPath.IsEmpty());
	TestEqual(TEXT("A cancelled BP session should report one skipped variation."),
		Sink->LastResult.SuccessfulCount + Sink->LastResult.FailedCount + Sink->LastResult.SkippedCount, 1);
	TestEqual(TEXT("A cancelled BP session should report one skipped variation."), Sink->LastResult.SkippedCount, 1);

	Wave->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterBPLibraryRejectedFireAndForgetCompletesExactlyOnceTest,
	"AudioFactory.AudioExporter.Runtime.BPLibrary.RejectedFireAndForgetCompletesExactlyOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterBPLibraryRejectedFireAndForgetCompletesExactlyOnceTest::RunTest(const FString& Parameters)
{
	// Regression coverage. UAudioExporterBPLibrary::ExportSoundToWav is THE fire-and-forget BP
	// entry; its header documents the completion contract as "bind to
	// UAudioExporterRuntimeSubsystem::OnExportComplete for a completion event". That contract must
	// settle EXACTLY ONCE for every request -- including a rejected one -- with context identity
	// preserved: a concurrent-session rejection must neither strand the caller without a completion
	// nor consume or duplicate the ACTIVE session's own completion.
	//
	// Today a rejection refused at subsystem level settles ONLY the structured-result channel:
	// BroadcastRejectedResult fires OnExportCompleteResult but never OnExportCompleteNative, so the
	// fire-and-forget caller's documented completion event never fires at all => RED on the first
	// legacy-count assert below. The structured asserts pin that whichever surface carries the fix,
	// it stays exactly-once and honest.
	UWorld* World = AudioExporterRuntimeTestUtils::ResolveAutomationWorld();
	if (!World)
	{
		AddError(TEXT("The rejected fire-and-forget test is unavailable: no editor world was available."));
		return false;
	}

	UAudioExporterRuntimeSubsystem* Subsystem = GEngine->GetEngineSubsystem<UAudioExporterRuntimeSubsystem>();
	TestNotNull(TEXT("The rejected fire-and-forget test should resolve the runtime subsystem."), Subsystem);
	if (!Subsystem)
	{
		return false;
	}

	if (Subsystem->IsExporting())
	{
		Subsystem->CancelExport(false);
	}

	USoundWave* Wave = NewObject<USoundWave>(GetTransientPackage(), TEXT("AudioExporterBPFireAndForgetWave"));
	TestNotNull(TEXT("The rejected fire-and-forget test should create a transient SoundWave."), Wave);
	if (!Wave)
	{
		return false;
	}
	Wave->AddToRoot();

	TArray<FAudioExportSoundEntry> SessionEntries;
	SessionEntries.Add(AudioExporterRuntimeTestUtils::MakeFixtureEntry(
		Wave,
		TEXT("AudioExporterBPFireAndForgetWave"),
		TEXT("/Temp/AudioExporterBPFireAndForgetWave")));

	FAudioExporterConfig Config;
	Config.OneShotVariations = 1;
	Config.LoopVariations = 1;
	Config.bNormalize = false;
	Config.bMonitor = false;
	Config.bExportTextLog = false;

	// The active session whose completion must NOT be stolen or duplicated by the rejection below.
	const FString ActiveSessionRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("BPFireAndForget_ActiveSession"));
	if (!Subsystem->BeginLifecycleTestSession(World, SessionEntries, Config, ActiveSessionRoot))
	{
		AddError(TEXT("The rejected fire-and-forget test could not seed its required active lifecycle session."));
		Wave->RemoveFromRoot();
		return false;
	}

	int32 LegacyCompleteEvents = 0;
	TArray<FString> LegacyCompletedPaths;
	const FDelegateHandle LegacyHandle = Subsystem->OnExportCompleteNative.AddLambda(
		[&LegacyCompleteEvents, &LegacyCompletedPaths](const FString& InPath)
		{
			++LegacyCompleteEvents;
			LegacyCompletedPaths.Add(InPath);
		});

	int32 StructuredEvents = 0;
	TArray<FAudioExportResult> StructuredResults;
	const FDelegateHandle StructuredHandle = Subsystem->OnExportCompleteResultNative.AddLambda(
		[&StructuredEvents, &StructuredResults](const FAudioExportResult& InResult)
		{
			++StructuredEvents;
			StructuredResults.Add(InResult);
		});

	AddExpectedError(TEXT("An export session is already active"), EAutomationExpectedErrorFlags::Contains, 1);
	UAudioExporterBPLibrary::ExportSoundToWav(World, Wave, Config, AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("BPFireAndForget_RejectedAttempt")));

	// The honest structured surface already settles today; these pins must stay green after the fix.
	TestEqual(TEXT("A rejected fire-and-forget request should settle exactly one structured result."), StructuredEvents, 1);
	if (StructuredResults.Num() == 1)
	{
		TestEqual(TEXT("A rejected fire-and-forget structured outcome should be Rejected."),
			StructuredResults[0].Outcome, EAudioExportOutcome::Rejected);
		TestFalse(TEXT("A rejected fire-and-forget structured result must report accepted-start false."),
			StructuredResults[0].bAcceptedStart);
	}

	// THE KEY ASSERT: the documented fire-and-forget completion surface must have
	// fired EXACTLY ONCE for the rejected request. Today it never fires.
	TestEqual(TEXT("A rejected fire-and-forget request must fire the documented completion event exactly once."),
		LegacyCompleteEvents, 1);

	// Context identity: completing the ACTIVE session afterwards must fire each surface exactly one
	// more time, and the new legacy completion must carry THIS session's path -- the rejection must
	// not have consumed or duplicated it.
	Subsystem->SetForceAsyncSaveForTesting(false);
	Subsystem->SimulateVariationFinishedForTesting();

	TestEqual(TEXT("The active session's own completion must add exactly one further legacy event."),
		LegacyCompleteEvents, 2);
	TestEqual(TEXT("The active session's own completion must add exactly one further structured event."),
		StructuredEvents, 2);
	if (StructuredResults.Num() == 2)
	{
		TestEqual(TEXT("The active session's terminal outcome should be Succeeded."),
			StructuredResults[1].Outcome, EAudioExportOutcome::Succeeded);
		TestTrue(TEXT("The active session's terminal result should report accepted-start true."),
			StructuredResults[1].bAcceptedStart);
		TestFalse(TEXT("The active session's terminal result should carry its output path."),
			StructuredResults[1].ExportPath.IsEmpty());
	}
	if (LegacyCompletedPaths.Num() == 2 && StructuredResults.Num() == 2)
	{
		TestEqual(TEXT("The post-rejection completion must be the ACTIVE session's path, not the rejected request's context."),
			LegacyCompletedPaths[1], StructuredResults[1].ExportPath);
	}

	Subsystem->OnExportCompleteNative.Remove(LegacyHandle);
	Subsystem->OnExportCompleteResultNative.Remove(StructuredHandle);
	if (Subsystem->IsExporting())
	{
		Subsystem->CancelExport(false);
	}
	Wave->RemoveFromRoot();
	return true;
}

// Fire-and-forget input-refusal settlement. Every early return in
// ExportSoundToWav / ExportSoundsToWav must settle the documented completion contract exactly once:
// one structured Rejected result + one legacy completion with an unset path. Each test below drives
// the REAL BP entry point against the real engine subsystem delegates and would fail if
// these refusals ever returned silently (zero events => count asserts fail).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterBPFireAndForgetNullSoundSettlesRejectionTest,
	"AudioFactory.AudioExporter.Runtime.BPLibrary.FireAndForgetNullSoundSettlesRejection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterBPFireAndForgetNullSoundSettlesRejectionTest::RunTest(const FString& Parameters)
{
	UAudioExporterRuntimeSubsystem* Subsystem = UAudioExporterBPLibrary::GetRuntimeSubsystem();
	TestNotNull(TEXT("The null-sound fire-and-forget test should resolve the runtime subsystem."), Subsystem);
	if (!Subsystem)
	{
		return false;
	}

	int32 StructuredEvents = 0;
	FAudioExportResult StructuredResult;
	int32 LegacyEvents = 0;
	TArray<FString> LegacyPaths;
	const FDelegateHandle StructuredHandle = Subsystem->OnExportCompleteResultNative.AddLambda(
		[&StructuredEvents, &StructuredResult](const FAudioExportResult& InResult)
		{
			++StructuredEvents;
			StructuredResult = InResult;
		});
	const FDelegateHandle LegacyHandle = Subsystem->OnExportCompleteNative.AddLambda(
		[&LegacyEvents, &LegacyPaths](const FString& InPath)
		{
			++LegacyEvents;
			LegacyPaths.Add(InPath);
		});

	AddExpectedError(TEXT("UAudioExporterBPLibrary::ExportSoundToWav: Sound is null."), EAutomationExpectedErrorFlags::Contains, 1);
	UAudioExporterBPLibrary::ExportSoundToWav(nullptr, nullptr, FAudioExporterConfig(), TEXT(""));

	Subsystem->OnExportCompleteResultNative.Remove(StructuredHandle);
	Subsystem->OnExportCompleteNative.Remove(LegacyHandle);

	TestEqual(TEXT("A null-sound fire-and-forget request must settle exactly one structured result."), StructuredEvents, 1);
	TestEqual(TEXT("A null-sound fire-and-forget request must settle exactly one legacy completion."), LegacyEvents, 1);
	if (StructuredEvents == 1)
	{
		TestEqual(TEXT("A null-sound fire-and-forget structured outcome should be Rejected."), StructuredResult.Outcome, EAudioExportOutcome::Rejected);
		TestFalse(TEXT("A null-sound fire-and-forget structured result must report accepted-start false."), StructuredResult.bAcceptedStart);
		TestTrue(TEXT("A null-sound fire-and-forget structured result must report an unset path."), StructuredResult.ExportPath.IsEmpty());
		TestFalse(TEXT("A null-sound fire-and-forget structured result must carry an error message."), StructuredResult.ErrorMessage.IsEmpty());
	}
	TestTrue(TEXT("A null-sound fire-and-forget legacy completion must carry an empty path."),
		LegacyPaths.Num() == 1 && LegacyPaths[0].IsEmpty());
	TestFalse(TEXT("A null-sound fire-and-forget rejection must not start an export session."), Subsystem->IsExporting());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterBPFireAndForgetEmptyListSettlesRejectionTest,
	"AudioFactory.AudioExporter.Runtime.BPLibrary.FireAndForgetEmptyListSettlesRejection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterBPFireAndForgetEmptyListSettlesRejectionTest::RunTest(const FString& Parameters)
{
	UAudioExporterRuntimeSubsystem* Subsystem = UAudioExporterBPLibrary::GetRuntimeSubsystem();
	TestNotNull(TEXT("The empty-list fire-and-forget test should resolve the runtime subsystem."), Subsystem);
	if (!Subsystem)
	{
		return false;
	}

	int32 StructuredEvents = 0;
	FAudioExportResult StructuredResult;
	int32 LegacyEvents = 0;
	TArray<FString> LegacyPaths;
	const FDelegateHandle StructuredHandle = Subsystem->OnExportCompleteResultNative.AddLambda(
		[&StructuredEvents, &StructuredResult](const FAudioExportResult& InResult)
		{
			++StructuredEvents;
			StructuredResult = InResult;
		});
	const FDelegateHandle LegacyHandle = Subsystem->OnExportCompleteNative.AddLambda(
		[&LegacyEvents, &LegacyPaths](const FString& InPath)
		{
			++LegacyEvents;
			LegacyPaths.Add(InPath);
		});

	// An all-invalid array must still settle exactly one rejection. MakeSoundEntries does not filter
	// slots (every occurrence keeps its descriptor and its terminal row), so with an
	// also unresolvable world context the world refusal fires first
	// (AudioExporterBPLibrary.cpp: ExportSoundsToWavIdentified) and carries the single rejection.
	TArray<USoundBase*> InvalidSounds;
	InvalidSounds.Add(nullptr);

	AddExpectedError(TEXT("UAudioExporterBPLibrary::ExportSoundsToWav: Could not resolve a valid world from the provided context object."), EAutomationExpectedErrorFlags::Contains, 1);
	UAudioExporterBPLibrary::ExportSoundsToWav(nullptr, InvalidSounds, FAudioExporterConfig(), TEXT(""));

	Subsystem->OnExportCompleteResultNative.Remove(StructuredHandle);
	Subsystem->OnExportCompleteNative.Remove(LegacyHandle);

	TestEqual(TEXT("An empty-list fire-and-forget request must settle exactly one structured result."), StructuredEvents, 1);
	TestEqual(TEXT("An empty-list fire-and-forget request must settle exactly one legacy completion."), LegacyEvents, 1);
	if (StructuredEvents == 1)
	{
		TestEqual(TEXT("An empty-list fire-and-forget structured outcome should be Rejected."), StructuredResult.Outcome, EAudioExportOutcome::Rejected);
		TestFalse(TEXT("An empty-list fire-and-forget structured result must report accepted-start false."), StructuredResult.bAcceptedStart);
		TestTrue(TEXT("An empty-list fire-and-forget structured result must report an unset path."), StructuredResult.ExportPath.IsEmpty());
		TestFalse(TEXT("An empty-list fire-and-forget structured result must carry an error message."), StructuredResult.ErrorMessage.IsEmpty());
	}
	TestTrue(TEXT("An empty-list fire-and-forget legacy completion must carry an empty path."),
		LegacyPaths.Num() == 1 && LegacyPaths[0].IsEmpty());
	TestFalse(TEXT("An empty-list fire-and-forget rejection must not start an export session."), Subsystem->IsExporting());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterBPFireAndForgetNullWorldSettlesRejectionTest,
	"AudioFactory.AudioExporter.Runtime.BPLibrary.FireAndForgetNullWorldSettlesRejection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterBPFireAndForgetNullWorldSettlesRejectionTest::RunTest(const FString& Parameters)
{
	// Covers BOTH unresolvable-world hunks: ExportSoundToWav and ExportSoundsToWav are each driven
	// with valid sounds but a WorldContextObject whose world cannot resolve.
	USoundWaveProcedural* Wave = AudioExporterRuntimeTestUtils::MakeTransientToneWave(TEXT("AudioExporterFireAndForgetNullWorldTone"), 0.25f);
	TestNotNull(TEXT("The null-world fire-and-forget test should create a transient tone wave."), Wave);
	if (!Wave)
	{
		return false;
	}

	UAudioExporterRuntimeSubsystem* Subsystem = UAudioExporterBPLibrary::GetRuntimeSubsystem();
	TestNotNull(TEXT("The null-world fire-and-forget test should resolve the runtime subsystem."), Subsystem);
	if (!Subsystem)
	{
		Wave->RemoveFromRoot();
		return false;
	}

	int32 StructuredEvents = 0;
	TArray<FAudioExportResult> StructuredResults;
	int32 LegacyEvents = 0;
	TArray<FString> LegacyPaths;
	const FDelegateHandle StructuredHandle = Subsystem->OnExportCompleteResultNative.AddLambda(
		[&StructuredEvents, &StructuredResults](const FAudioExportResult& InResult)
		{
			++StructuredEvents;
			StructuredResults.Add(InResult);
		});
	const FDelegateHandle LegacyHandle = Subsystem->OnExportCompleteNative.AddLambda(
		[&LegacyEvents, &LegacyPaths](const FString& InPath)
		{
			++LegacyEvents;
			LegacyPaths.Add(InPath);
		});

	AddExpectedError(TEXT("UAudioExporterBPLibrary::ExportSoundToWav: Could not resolve a valid world from the provided context object."), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("UAudioExporterBPLibrary::ExportSoundsToWav: Could not resolve a valid world from the provided context object."), EAutomationExpectedErrorFlags::Contains, 1);

	UAudioExporterBPLibrary::ExportSoundToWav(nullptr, Wave, FAudioExporterConfig(), TEXT(""));

	TArray<USoundBase*> Sounds;
	Sounds.Add(Wave);
	UAudioExporterBPLibrary::ExportSoundsToWav(nullptr, Sounds, FAudioExporterConfig(), TEXT(""));

	Subsystem->OnExportCompleteResultNative.Remove(StructuredHandle);
	Subsystem->OnExportCompleteNative.Remove(LegacyHandle);

	TestEqual(TEXT("Each null-world fire-and-forget call must settle exactly one structured result (two calls total)."), StructuredEvents, 2);
	TestEqual(TEXT("Each null-world fire-and-forget call must settle exactly one legacy completion (two calls total)."), LegacyEvents, 2);
	if (StructuredResults.Num() == 2)
	{
		TestEqual(TEXT("The first null-world fire-and-forget structured outcome should be Rejected."),
			StructuredResults[0].Outcome, EAudioExportOutcome::Rejected);
		TestEqual(TEXT("The second null-world fire-and-forget structured outcome should be Rejected."),
			StructuredResults[1].Outcome, EAudioExportOutcome::Rejected);
		TestFalse(TEXT("A null-world fire-and-forget structured result must report accepted-start false."),
			StructuredResults[0].bAcceptedStart || StructuredResults[1].bAcceptedStart);
		TestTrue(TEXT("A null-world fire-and-forget structured result must report an unset path."),
			StructuredResults[0].ExportPath.IsEmpty() && StructuredResults[1].ExportPath.IsEmpty());
		TestFalse(TEXT("A null-world fire-and-forget structured result must carry an error message."),
			StructuredResults[0].ErrorMessage.IsEmpty() || StructuredResults[1].ErrorMessage.IsEmpty());
	}
	TestTrue(TEXT("Each null-world fire-and-forget legacy completion must carry an empty path."),
		LegacyPaths.Num() == 2 && LegacyPaths[0].IsEmpty() && LegacyPaths[1].IsEmpty());
	TestFalse(TEXT("A null-world fire-and-forget rejection must not start an export session."), Subsystem->IsExporting());

	Wave->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterReportGoldenFixtureTest,
	"AudioFactory.AudioExporter.Runtime.Report.GoldenFixture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterReportGoldenFixtureTest::RunTest(const FString& Parameters)
{
	// A fixture batch containing success + failure + skip must render a TXT report compared
	// byte-for-byte against the committed golden file. The rows are produced by the REAL
	// RecordVariationResult through the real pipeline (no hand-built report structs), and rendered
	// by the real logger with an injected fixed timestamp/paths so the output is deterministic.
	UWorld* World = AudioExporterRuntimeTestUtils::ResolveAutomationWorld();
	if (!World)
	{
		AddError(TEXT("Report golden fixture test is unavailable: no editor world was available."));
		return false;
	}

	USoundWaveProcedural* Wave = AudioExporterRuntimeTestUtils::MakeTransientToneWave(TEXT("AudioExporterGoldenTone"), 0.25f);
	TestNotNull(TEXT("The golden fixture should create a transient tone wave."), Wave);
	if (!Wave)
	{
		return false;
	}

	const TSharedRef<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline> Pipeline = MakeShared<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline>();
	Pipeline->Initialize(World);

	TArray<FAudioExportSoundEntry> Entries;
	Entries.Add(AudioExporterRuntimeTestUtils::MakeFixtureEntry(Wave, TEXT("GoldenTone"), TEXT("/Game/Golden/GoldenTone")));

	FAudioExporterConfig Config;
	Config.OneShotVariations = 4;
	Config.LoopVariations = 4;
	Config.MaxLengthSeconds = 30;
	Config.bNormalize = false;
	Config.bMonitor = false;
	// The Lite settings block is exercised with bExportTextLog=true so the "Export Text
	// Log" row genuinely reflects the production formatter's live-Config code path instead of a dead
	// always-false literal.
	Config.bExportTextLog = true;

	const FString OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("ReportGoldenFixture"));
	Pipeline->BeginLifecycleTestSession(Entries, Config, OutputRoot);
	const bool bStarted = Pipeline->IsExporting();
	TestTrue(TEXT("The golden fixture session should be accepted."), bStarted);
	if (!bStarted)
	{
		Wave->RemoveFromRoot();
		return false;
	}

	// Deterministic row paths: a fixed session under the project, with an injected fixed timestamp.
	// Output paths include the session's real Audio subfolder so the golden exercises the disk layout.
	const FString GoldenBase = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("Reports"), TEXT("Golden"), TEXT("2026-08-12_10-00-00"), AudioExporterAudioSubfolder));

	const FString AssetPath = TEXT("/Game/Golden/GoldenTone");
	const FString RelativePath = TEXT("Golden");
	const int32 SoundIndex = 0;
	const int32 TotalVariations = 4;

	// Variation 1 (index 0): success with measured audio stats.
	FPipelineVariationCaptureData V1;
	AudioExporterRuntimeTestUtils::MakeFixtureVariationData(V1, TEXT("GoldenTone"), AssetPath, RelativePath, 0, SoundIndex, TotalVariations);
	Pipeline->RecordVariationResult(V1, true, true, TEXT("GoldenTone_1.wav"), GoldenBase / TEXT("GoldenTone_1.wav"), -3.00f, -9.00f, 2.5f, 0.0f, FString());

	// Variation 2 (index 1): success with a small applied gain and a peak under the -1 dBFS ceiling.
	FPipelineVariationCaptureData V2;
	AudioExporterRuntimeTestUtils::MakeFixtureVariationData(V2, TEXT("GoldenTone"), AssetPath, RelativePath, 1, SoundIndex, TotalVariations);
	Pipeline->RecordVariationResult(V2, true, true, TEXT("GoldenTone_2.wav"), GoldenBase / TEXT("GoldenTone_2.wav"), -1.02f, -8.50f, 2.5f, 0.50f, FString());

	// Variation 3 (index 2): failure with a single human sentence, no measurements.
	FPipelineVariationCaptureData V3;
	AudioExporterRuntimeTestUtils::MakeFixtureVariationData(V3, TEXT("GoldenTone"), AssetPath, RelativePath, 2, SoundIndex, TotalVariations);
	Pipeline->RecordVariationResult(V3, false, true, FString(), FString(), 0.0f, 0.0f, 0.0f, 0.0f, TEXT("Render failed: simulated golden fixture failure."));

	// Variation 4 (index 3): skipped (save vetoed), no measurements.
	FPipelineVariationCaptureData V4;
	AudioExporterRuntimeTestUtils::MakeFixtureVariationData(V4, TEXT("GoldenTone"), AssetPath, RelativePath, 3, SoundIndex, TotalVariations);
	Pipeline->RecordVariationResult(V4, false, false, FString(), FString(), 0.0f, 0.0f, 0.0f, 0.0f, TEXT("Save skipped because the export session is no longer active."));

	// Pin the production session timestamp, then exercise WriteLogFilesToDisk itself. This keeps the
	// settings block, report paths, report counters, and both logger calls on the production path.
	Pipeline->SetReportTimestampForTesting(FDateTime(2026, 8, 12, 10, 0, 0));
	Pipeline->WriteReportsForTesting();
	Pipeline->CancelExporting(false);

	const FString ReportDir = OutputRoot;
	const FString TextPath = ReportDir / TEXT("AudioExporterTextLog.txt");

	// Committed golden baselines live in the repository-level Docs/Baselines/Reports folder.
	const FString BaselinesDir = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectDir(), TEXT("Docs"), TEXT("Baselines"), TEXT("Reports")));
	const FString GoldenTextPath = BaselinesDir / TEXT("AudioExporter_Report_Golden.txt");

	FString GeneratedText;
	TestTrue(TEXT("The golden fixture should render a TXT report."), FFileHelper::LoadFileToString(GeneratedText, *TextPath));

	// Lite publishes the TXT report only; a CSV alongside it means the retired Pro-only writer came back.
	TestFalse(TEXT("Lite must not write a CSV report next to its TXT report."),
		IFileManager::Get().FileExists(*(ReportDir / TEXT("AudioExporterCSVLog.csv"))));

	// Normalize the machine/drive-specific project-root prefix out of the generated text
	// before it ever touches a golden file, so the committed baseline is workspace-root independent.
	GeneratedText = AudioExporterRuntimeTestUtils::NormalizeWorkspaceRootForGolden(GeneratedText);

	// -RegenGoldens: the ONLY sanctioned way to (re)write the committed baseline. No
	// self-heal: a missing or mismatched baseline is a loud FAIL without this explicit switch.
	if (AudioExporterRuntimeTestUtils::IsRegenGoldensRequested())
	{
		FFileHelper::SaveStringToFile(GeneratedText, *GoldenTextPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		AddInfo(FString::Printf(TEXT("RegenGoldens: wrote AudioExporter golden baseline to '%s'."), *BaselinesDir));
	}

	FString GoldenText;
	if (!FFileHelper::LoadFileToString(GoldenText, *GoldenTextPath))
	{
		AddError(FString::Printf(TEXT("Missing committed golden report file under '%s'. Required fixture missing = FAIL. Run with -RegenGoldens to (re)generate."), *BaselinesDir));
		Wave->RemoveFromRoot();
		return false;
	}

	// Byte-for-byte, case-sensitive comparison against the committed golden file.
	// FString's operator==/!= is case-INSENSITIVE (Stricmp); Equals(..., ESearchCase::CaseSensitive) is
	// required so a one-byte case flip is caught.
	const bool bTextMatches = GeneratedText.Equals(GoldenText, ESearchCase::CaseSensitive);
	TestTrue(TEXT("The TXT report should match the golden file byte-for-byte (case-sensitive)."), bTextMatches);

	if (!bTextMatches)
	{
		AddError(FString::Printf(TEXT("TXT golden mismatch. Generated %d bytes vs golden %d bytes. Generated file: %s"), GeneratedText.Len(), GoldenText.Len(), *TextPath));
	}

	Wave->RemoveFromRoot();
	return bTextMatches;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterReportWriterFailureReturnsErrorTest,
	"AudioFactory.AudioExporter.Runtime.Report.WriterFailureReturnsError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterReportWriterFailureReturnsErrorTest::RunTest(const FString& Parameters)
{
	const FString Root = FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("Reports"), TEXT("AudioExporter"),
		FString::Printf(TEXT("WriterFailure_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	IFileManager::Get().MakeDirectory(*Root, true);
	const FString BlockingFile = Root / TEXT("blocked");
	FFileHelper::SaveStringToFile(TEXT("not a directory"), *BlockingFile);
	const FString InvalidTextPath = BlockingFile / TEXT("AudioExporterTextLog.txt");

	FString TextError;
	AddExpectedError(TEXT("AudioExporterLogger::WriteTextLogFile: Failed to write text report"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("TXT writer should report failure when its parent is a blocking file."),
		AudioExporterLogger::WriteTextLogFile({}, InvalidTextPath, TEXT("2026-08-12 10:00:00"), FString(), Root, &TextError));
	TestTrue(TEXT("TXT writer failure should provide error text."), !TextError.IsEmpty());

	IFileManager::Get().Delete(*BlockingFile, false, true);
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

// ---------------------------------------------------------------------------
// SoundWave static-source support and request-token lifecycle around rejected starts and stale
// callbacks.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterSoundWaveStaticSourceSupportedTest,
	"AudioFactory.AudioExporter.Runtime.SoundWaveStaticSourceSupported",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterSoundWaveStaticSourceSupportedTest::RunTest(const FString& Parameters)
{
	// SoundWave export ships in Lite. IsAudioExporterSupportedAsset is the single real predicate the
	// module context-menu gate, the settings window filter, and the manager's asset filter all share;
	// this drives it directly, not a re-implemented copy, against real transient FAssetData for a
	// static wave, a procedural wave, and the pre-existing classes. The real StartExporting and WAV
	// capture contract is covered by RealPipeline.SoundWave below, where the produced file is reopened.

	// --- Part 1: the real predicate, exercised against every relevant class. ---
	auto MakeTransientAssetData = [](UObject* Object) -> FAssetData
	{
		UPackage* Package = Object->GetOutermost();
		return FAssetData(
			Package->GetFName(),
			FName(*FPackageName::GetLongPackagePath(Package->GetName())),
			Object->GetFName(),
			Object->GetClass()->GetClassPathName());
	};

	UPackage* WavePackage = NewObject<UPackage>(nullptr, TEXT("/Temp/AudioExporterR5Test/StaticWave"), RF_Public);
	USoundWave* StaticWave = NewObject<USoundWave>(WavePackage, TEXT("StaticWave"), RF_Public);
	TestNotNull(TEXT("A transient static SoundWave should be created."), StaticWave);

	UPackage* ProceduralPackage = NewObject<UPackage>(nullptr, TEXT("/Temp/AudioExporterR5Test/ProceduralWave"), RF_Public);
	USoundWaveProcedural* ProceduralWave = NewObject<USoundWaveProcedural>(ProceduralPackage, TEXT("ProceduralWave"), RF_Public);
	TestNotNull(TEXT("A transient procedural SoundWave should be created."), ProceduralWave);

	UPackage* MetaSoundPackage = NewObject<UPackage>(nullptr, TEXT("/Temp/AudioExporterR5Test/MetaSound"), RF_Public);
	UMetaSoundSource* MetaSound = NewObject<UMetaSoundSource>(MetaSoundPackage, TEXT("MetaSound"), RF_Public);
	TestNotNull(TEXT("A transient MetaSound source should be created."), MetaSound);

	if (StaticWave && ProceduralWave && MetaSound)
	{
		TestTrue(TEXT("A non-procedural SoundWave must be accepted by the real supported-asset predicate."),
			IsAudioExporterSupportedAsset(MakeTransientAssetData(StaticWave)));
		TestFalse(TEXT("A procedural SoundWave must remain rejected by the real supported-asset predicate."),
			IsAudioExporterSupportedAsset(MakeTransientAssetData(ProceduralWave)));
		TestTrue(TEXT("A MetaSound source must remain accepted before the procedural SoundWave exclusion (predicate-order pin)."),
			IsAudioExporterSupportedAsset(MakeTransientAssetData(MetaSound)));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterFlushPendingVariationsSalvagesPlantedTest,
	"AudioFactory.AudioExporter.Runtime.MidBatchFailure.FlushPendingVariationsSalvagesPlanted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterFlushPendingVariationsSalvagesPlantedTest::RunTest(const FString& Parameters)
{
	// FlushPendingVariations's own body is never exercised by SimulateVariationFinishedForTesting:
	// that seam appends then immediately pops its entry, so PendingVariations is empty by the time
	// any other code could observe it, and FlushPendingVariations would just
	// hit its "if (PendingVariations.IsEmpty()) return;" guard and do nothing. This test plants 3
	// variation-capture entries directly into PendingVariations against a deterministic fake
	// renderer holding a known 1-second buffer, then calls the real FlushPendingVariations. Two
	// markers land inside the buffer and must salvage as real WAV files through the real logger; the
	// third (the injected error) targets a marker beyond the buffer's length and must fail
	// extraction. Deleting FlushPendingVariations's body collapses this to 0 WAVs / 0 failure rows
	// and fails the test outright.
	const TSharedRef<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline> Pipeline = MakeShared<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline>();

	TArray<FAudioExportSoundEntry> Entries;
	Entries.Add(AudioExporterRuntimeTestUtils::MakeFixtureEntry(nullptr, TEXT("FlushSalvageFixture"), TEXT("/Temp/AudioExporterFlushSalvageFixture")));

	const FAudioExporterConfig Config = AudioExporterRuntimeTestUtils::MakeFailureFixtureConfig(3);
	const FString OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("FlushSalvagePlanted"));

	Pipeline->BeginLifecycleTestSession(Entries, Config, OutputRoot);
	TestTrue(TEXT("The flush-salvage fixture session should be accepted."), Pipeline->IsExporting());
	if (!Pipeline->IsExporting())
	{
		return false;
	}

	// A deterministic fake renderer: a known 1-second, 48kHz stereo buffer with no live audio device
	// or timing dependency.
	TUniquePtr<AudioExporterRuntimeTestUtils::FFakeBatchRenderer> FakeRenderer = MakeUnique<AudioExporterRuntimeTestUtils::FFakeBatchRenderer>();
	constexpr int32 SampleRate = 48000;
	constexpr int32 NumChannels = 2;
	FakeRenderer->CannedSampleRate = SampleRate;
	FakeRenderer->CannedNumChannels = NumChannels;
	FakeRenderer->CannedBuffer.SetNumZeroed(SampleRate * NumChannels); // Exactly 1.0s.
	FakeRenderer->CannedBatchStartTime = 0.0;
	FakeRenderer->CannedBatchEndTime = 1.0;
	Pipeline->Renderer = MoveTemp(FakeRenderer);

	const FString AssetPath = TEXT("/Temp/AudioExporterFlushSalvageFixture");
	FPipelineVariationCaptureData V1;
	V1.SoundName = TEXT("FlushSalvageFixture");
	V1.SourceAssetPath = AssetPath;
	V1.RelativePath = TEXT("FailureFixture");
	V1.SoundIndex = 0;
	V1.TotalVariations = 3;
	V1.bIsLoop = false;
	V1.VariationIndex = 0;
	V1.StartTime = 0.0;
	V1.EndTime = 0.2;

	FPipelineVariationCaptureData V2 = V1;
	V2.VariationIndex = 1;
	V2.StartTime = 0.3;
	V2.EndTime = 0.5;

	// The injected error: a marker placed well beyond the 1.0s canned buffer, which
	// ExtractVariationBuffer must reject as an invalid sample range.
	FPipelineVariationCaptureData V3 = V1;
	V3.VariationIndex = 2;
	V3.StartTime = 5.0;
	V3.EndTime = 5.2;

	Pipeline->PendingVariations.Add(V1);
	Pipeline->PendingVariations.Add(V2);
	Pipeline->PendingVariations.Add(V3);

	AddExpectedError(TEXT("invalid sample range"), EAutomationExpectedErrorFlags::Contains, 1);
	Pipeline->FlushPendingVariations(TEXT("Simulated mid-batch failure for the flush-salvage test"));

	TestTrue(TEXT("FlushPendingVariations should drain every planted entry out of PendingVariations."), Pipeline->PendingVariations.IsEmpty());

	Pipeline->WriteLogFilesToDisk();

	TArray<FString> WavFiles;
	AudioExporterRuntimeTestUtils::FindWavFilesUnder(OutputRoot, WavFiles);
	TestEqual(TEXT("FlushPendingVariations should salvage exactly 2 real WAV files from the planted buffer."), WavFiles.Num(), 2);

	FString TextLog;
	const bool bLoaded = AudioExporterRuntimeTestUtils::LoadReportFile(OutputRoot, TEXT("AudioExporterTextLog.txt"), TextLog);
	TestTrue(TEXT("FlushPendingVariations should leave a written report behind."), bLoaded);
	if (bLoaded)
	{
		TestEqual(TEXT("The report should carry exactly 2 succeeded rows from the salvage."),
			AudioExporterRuntimeTestUtils::CountOccurrences(TextLog, TEXT("Exported File Name")), 2);
		TestEqual(TEXT("The report should carry exactly 1 failure row for the out-of-range marker."),
			AudioExporterRuntimeTestUtils::CountOccurrences(TextLog, TEXT("invalid sample range")), 1);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterDirectStartExportingRejectedSettlesResultTest,
	"AudioFactory.AudioExporter.Runtime.Lifecycle.DirectStartExportingRejectedSettlesResult",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterDirectStartExportingRejectedSettlesResultTest::RunTest(const FString& Parameters)
{
	// Other lifecycle tests drive a Subsystem-level seam, which never reaches the pipeline's own
	// FailStartup->BroadcastTerminalOnce Rejected branch (AudioExportPipeline.cpp): the subsystem
	// is the only caller that pre-empts a concurrent-session start, so that branch needs a test
	// that bypasses it entirely. This drives StartExporting() directly with an empty
	// sound list, asserting both the returned bool and the real structured FAudioExportResult it
	// settles.
	const TSharedRef<FAudioExportPipeline> Pipeline = MakeShared<FAudioExportPipeline>();

	int32 ResultEvents = 0;
	FAudioExportResult CapturedResult;
	Pipeline->OnExportResult.AddLambda([&ResultEvents, &CapturedResult](const FAudioExportResult& InResult)
	{
		++ResultEvents;
		CapturedResult = InResult;
	});

	AddExpectedError(TEXT("Export start rejected because the sound list was empty."), EAutomationExpectedErrorFlags::Contains, 1);
	const bool bStarted = Pipeline->StartExporting(TArray<FAudioExportSoundEntry>(), FAudioExporterConfig());

	TestFalse(TEXT("A direct StartExporting call with an empty sound list must return false."), bStarted);
	TestEqual(TEXT("A direct rejected StartExporting call must settle exactly one structured result."), ResultEvents, 1);
	TestEqual(TEXT("A direct rejected StartExporting result outcome must be Rejected."), CapturedResult.Outcome, EAudioExportOutcome::Rejected);
	TestFalse(TEXT("A direct rejected StartExporting result must report accepted-start false."), CapturedResult.bAcceptedStart);
	TestTrue(TEXT("A direct rejected StartExporting result must report an unset path."), CapturedResult.ExportPath.IsEmpty());
	TestFalse(TEXT("A direct rejected StartExporting result must carry an error message."), CapturedResult.ErrorMessage.IsEmpty());

	// A direct caller can race an already-active session without going through the subsystem's
	// request-token path. The rejection must still settle a structured result while leaving the
	// accepted session alive for its own later cancellation result.
	const FString ActiveSessionPath = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("DirectConcurrentStart"));
	Pipeline->BeginLifecycleTestSession(ActiveSessionPath);
	TestTrue(TEXT("The direct-concurrent-start fixture should leave the first session active."), Pipeline->IsExporting());
	const bool bConcurrentStarted = Pipeline->StartExporting(TArray<FAudioExportSoundEntry>(), FAudioExporterConfig());
	TestFalse(TEXT("A direct StartExporting call during an active session must be rejected."), bConcurrentStarted);
	TestEqual(TEXT("The direct concurrent rejection must settle exactly one additional structured result."), ResultEvents, 2);
	TestEqual(TEXT("The direct concurrent rejection outcome must be Rejected."), CapturedResult.Outcome, EAudioExportOutcome::Rejected);
	TestFalse(TEXT("The direct concurrent rejection must not report an accepted start."), CapturedResult.bAcceptedStart);
	TestTrue(TEXT("The direct concurrent rejection must leave the original session active."), Pipeline->IsExporting());

	Pipeline->CancelExporting(false);
	TestEqual(TEXT("Cancelling the original direct session must settle its own terminal result."), ResultEvents, 3);
	TestEqual(TEXT("The original direct session should finish as Cancelled."), CapturedResult.Outcome, EAudioExportOutcome::Cancelled);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterStaleResultCallbackDoesNotLeakToNextSessionTest,
	"AudioFactory.AudioExporter.Runtime.Lifecycle.StaleResultCallbackDoesNotLeakToNextSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterStaleResultCallbackDoesNotLeakToNextSessionTest::RunTest(const FString& Parameters)
{
	// BroadcastRejectedResult must drain PendingResultCallbacks exactly like
	// HandlePipelineResult does. Register a callback, force a rejection that settles through
	// BroadcastRejectedResult (a null world -> EnsurePipeline failure), and prove it fires exactly
	// once for that rejection -- then run a real, unrelated session to completion afterward and
	// prove the stale callback does not fire again with that later session's result.
	UWorld* World = AudioExporterRuntimeTestUtils::ResolveAutomationWorld();
	if (!World)
	{
		AddError(TEXT("Stale-callback test is unavailable: no editor world was available."));
		return false;
	}

	UAudioExporterRuntimeSubsystem* Subsystem = NewObject<UAudioExporterRuntimeSubsystem>(GetTransientPackage());
	TestNotNull(TEXT("The stale-callback test should create a runtime subsystem."), Subsystem);
	if (!Subsystem)
	{
		return false;
	}

	int32 StaleCallbackEvents = 0;
	FAudioExportResult StaleResult;
	const FAudioExportRequestToken StaleRequestToken = Subsystem->AddOneTimeResultCallback([&StaleCallbackEvents, &StaleResult](const FAudioExportResult& InResult)
	{
		++StaleCallbackEvents;
		StaleResult = InResult;
	});

	// Force the EnsurePipeline() rejection branch (BroadcastRejectedResult), which the registered
	// callback above should be serviced by exactly once.
	AddExpectedError(TEXT("A valid world is required to start audio export."), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("Runtime export pipeline was not available."), EAutomationExpectedErrorFlags::Contains, 1);
	const bool bStarted = Subsystem->StartExport(nullptr, TArray<FAudioExportSoundEntry>(), FAudioExporterConfig(), TEXT(""), StaleRequestToken);
	TestFalse(TEXT("A null-world start should be rejected."), bStarted);

	TestEqual(TEXT("The registered callback should fire exactly once for its own rejection."), StaleCallbackEvents, 1);
	TestEqual(TEXT("The rejection result outcome should be Rejected."), StaleResult.Outcome, EAudioExportOutcome::Rejected);

	// Now run a real, unrelated session to completion (cancel settles a Cancelled result through the
	// SAME HandlePipelineResult drain path). The stale callback -- already fired and removed from
	// PendingResultCallbacks above -- must not fire a second time with THIS session's result.
	const FString NextSessionPath = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("StaleCallbackNextSession"));
	if (!Subsystem->BeginLifecycleTestSession(World, NextSessionPath))
	{
		AddError(TEXT("Stale-callback test could not seed its required next session."));
		return false;
	}

	Subsystem->CancelExport(false);

	TestEqual(TEXT("The stale callback must not fire again on a later, unrelated session's result."), StaleCallbackEvents, 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterTerminalCallbackCannotResetNewSessionTest,
	"AudioFactory.AudioExporter.Runtime.Lifecycle.TerminalCallbackCannotResetNewSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterTerminalCallbackCannotResetNewSessionTest::RunTest(const FString& Parameters)
{
	// A terminal listener is allowed to start the next request synchronously. The old terminal path
	// must not run ResetState after that callback returns and erase the new request.
	{
		const TSharedRef<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline> Pipeline = MakeShared<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline>();
		const FString NextSessionPath = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("TerminalCallbackRestart_Cancel"));
		bool bStartedNextSession = false;
		Pipeline->OnExportResult.AddLambda([&](const FAudioExportResult& InResult)
		{
			if (!bStartedNextSession && InResult.Outcome == EAudioExportOutcome::Cancelled)
			{
				Pipeline->BeginLifecycleTestSession(NextSessionPath);
				bStartedNextSession = true;
			}
		});

		Pipeline->BeginLifecycleTestSession(AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("TerminalCallbackRestart_Cancel_Old")));
		Pipeline->CancelExporting(false);
		TestTrue(TEXT("A cancel callback should be able to start the next session synchronously."), bStartedNextSession);
		TestTrue(TEXT("The cancel path must not reset a session started by its terminal callback."), Pipeline->IsExporting());
		TestEqual(TEXT("The callback-started session must retain its own output path."), Pipeline->GetExportPath(), NextSessionPath);
		Pipeline->CancelExporting(false);
	}

	{
		const TSharedRef<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline> Pipeline = MakeShared<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline>();
		const FString NextSessionPath = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("TerminalCallbackRestart_Completion"));
		bool bStartedNextSession = false;
		Pipeline->OnExportResult.AddLambda([&](const FAudioExportResult& InResult)
		{
			if (!bStartedNextSession && InResult.Outcome == EAudioExportOutcome::Failed)
			{
				Pipeline->BeginLifecycleTestSession(NextSessionPath);
				bStartedNextSession = true;
			}
		});

		Pipeline->BeginLifecycleTestSession(AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("TerminalCallbackRestart_Completion_Old")));
		AddExpectedError(TEXT("Audio export finished without writing any files."), EAutomationExpectedErrorFlags::Contains, 1);
		Pipeline->CompleteFinalEmptyBatchForTesting();
		TestTrue(TEXT("A completion callback should be able to start the next session synchronously."), bStartedNextSession);
		TestTrue(TEXT("The completion path must not reset a session started by its terminal callback."), Pipeline->IsExporting());
		TestEqual(TEXT("The completion callback-started session must retain its own output path."), Pipeline->GetExportPath(), NextSessionPath);
		Pipeline->CancelExporting(false);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterStartupFailureCallbackCannotResetNewSessionTest,
	"AudioFactory.AudioExporter.Runtime.Lifecycle.StartupFailureCallbackCannotResetNewSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterStartupFailureCallbackCannotResetNewSessionTest::RunTest(const FString& Parameters)
{
	const TSharedRef<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline> Pipeline = MakeShared<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline>();
	const FString NextSessionPath = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("StartupFailureCallbackRestart"));
	bool bStartedNextSession = false;
	Pipeline->OnExportResult.AddLambda([&](const FAudioExportResult& InResult)
	{
		if (!bStartedNextSession && InResult.Outcome == EAudioExportOutcome::Rejected)
		{
			Pipeline->BeginLifecycleTestSession(NextSessionPath);
			bStartedNextSession = true;
		}
	});

	AddExpectedError(TEXT("Export start rejected because the sound list was empty."), EAutomationExpectedErrorFlags::Contains, 1);
	const bool bStarted = Pipeline->StartExporting(TArray<FAudioExportSoundEntry>(), FAudioExporterConfig());
	TestFalse(TEXT("The empty start request should fail through the production startup-failure path."), bStarted);
	TestTrue(TEXT("A startup-failure callback should be able to start the next session synchronously."), bStartedNextSession);
	TestTrue(TEXT("The startup-failure path must not reset a session started by its terminal callback."), Pipeline->IsExporting());
	TestEqual(TEXT("The startup-failure callback-started session must retain its own output path."), Pipeline->GetExportPath(), NextSessionPath);
	Pipeline->CancelExporting(false);
	return true;
}

namespace AudioExporterRuntimeCancelTestUtils
{
	struct FCancelDeleteState
	{
		FAutomationTestBase* Test = nullptr;
		TSharedPtr<FAudioExportPipeline> Pipeline;
		TObjectPtr<USoundWaveProcedural> Wave = nullptr;
		FString OutputRoot;
		int32 ResultEvents = 0;
		FAudioExportResult Result;
		TArray<FAudioExportAssetLog> LogsAtResult;
		double StartSeconds = 0.0;
		bool bWriterDrainConfirmed = false;
		bool bWaitTimedOut = false;
	};

	static void RegisterCancelledSaveCleanup(const TSharedRef<FCancelDeleteState>& State)
	{
		AudioExporterRuntimeTestUtils::RegisterRetainedTestCleanup(
			[State]()
			{
				return State->ResultEvents > 0
					&& State->Pipeline.IsValid()
					&& !State->Pipeline->IsExporting()
					&& State->Pipeline->GetActiveAsyncExportsForTesting() == 0;
			},
			[State]()
			{
				State->Pipeline.Reset();
				if (State->Wave)
				{
					State->Wave->RemoveFromRoot();
					State->Wave = nullptr;
				}
				if (!State->OutputRoot.IsEmpty())
				{
					IFileManager::Get().DeleteDirectory(*State->OutputRoot, false, true);
					State->OutputRoot.Reset();
				}
			},
			[State]()
			{
				if (State->Pipeline.IsValid()
					&& (State->Pipeline->IsExporting()
						|| State->Pipeline->GetActiveAsyncExportsForTesting() != 0))
				{
					State->Test->AddError(TEXT("CancelExporting(true) retained live pipeline state during engine shutdown because the pipeline was still active or had async writers."));
					return false;
				}
				if (State->ResultEvents == 0)
				{
					State->Test->AddError(TEXT("CancelExporting(true) reached engine shutdown without a structured terminal result; releasing after shutdown made the backend inactive."));
				}
				State->Pipeline.Reset();
				if (State->Wave)
				{
					State->Wave->RemoveFromRoot();
					State->Wave = nullptr;
				}
				if (!State->OutputRoot.IsEmpty())
				{
					IFileManager::Get().DeleteDirectory(*State->OutputRoot, false, true);
					State->OutputRoot.Reset();
				}
				return true;
			});
	}

	class FWaitForCancelledAsyncSaveToSettle final : public IAutomationLatentCommand
	{
	public:
		explicit FWaitForCancelledAsyncSaveToSettle(const TSharedRef<FCancelDeleteState>& InState)
			: State(InState)
		{
		}

		virtual bool Update() override
		{
			const bool bWriterActive = State->Pipeline.IsValid() && State->Pipeline->GetActiveAsyncExportsForTesting() > 0;
			const bool bPipelineInactive = State->Pipeline.IsValid() && !State->Pipeline->IsExporting();
			if (bWriterActive || !bPipelineInactive || State->ResultEvents == 0)
			{
				if ((FPlatformTime::Seconds() - State->StartSeconds) > 30.0)
				{
					State->bWaitTimedOut = true;
					State->Test->AddError(TEXT("CancelExporting(true) did not drain the in-flight save within 30 seconds."));
					RegisterCancelledSaveCleanup(State);
					return true;
				}
				return false;
			}
			State->bWriterDrainConfirmed = true;

			State->Test->TestEqual(TEXT("Cancellation should settle exactly one structured result."), State->ResultEvents, 1);
			State->Test->TestEqual(TEXT("Cancellation should settle as Cancelled."), State->Result.Outcome, EAudioExportOutcome::Cancelled);
			State->Test->TestEqual(TEXT("Cancellation should count the in-flight and unstarted variations as skipped."), State->Result.SkippedCount, 2);
			State->Test->TestTrue(TEXT("Cancellation should carry the exact skipped-save detail."),
				State->Result.ErrorMessage.Contains(TEXT("Save skipped because the export session is no longer active.")));
			State->Test->TestEqual(TEXT("Cancellation should preserve one terminal row for each requested variation."),
				State->LogsAtResult.Num(), 1);
			if (State->LogsAtResult.Num() == 1)
			{
				State->Test->TestEqual(TEXT("Cancellation should preserve both the active and unstarted variation rows."),
					State->LogsAtResult[0].Variations.Num(), 2);
				for (const FAudioExportLogEntry& Row : State->LogsAtResult[0].Variations)
				{
					State->Test->TestEqual(TEXT("Every unsettled cancellation row should be explicitly Skipped."), Row.Status, EAudioExportRowStatus::Skipped);
				}
			}
			State->Test->TestFalse(TEXT("CancelExporting(true) should delete the directory after the async save settles."),
				IFileManager::Get().DirectoryExists(*State->OutputRoot));

			if (State->Wave)
			{
				State->Wave->RemoveFromRoot();
				State->Wave = nullptr;
			}
			State->Pipeline.Reset();
			return true;
		}

	private:
		TSharedRef<FCancelDeleteState> State;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterCancelAsyncSaveDeleteDeferredTest,
	"AudioFactory.AudioExporter.Runtime.Lifecycle.CancelAsyncSaveDeleteDeferred",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterCancelAsyncSaveDeleteDeferredTest::RunTest(const FString& Parameters)
{
	// The first variation is a real WAV produced by ScheduleVariationSave's async branch. The test
	// waits until that worker has written the file but before its game-thread retirement, then calls
	// the public CancelExporting(true). The directory and WAV must remain available through the
	// settle point, after which the deferred delete is allowed to remove them.
	USoundWaveProcedural* Wave = AudioExporterRuntimeTestUtils::MakeTransientToneWave(TEXT("AudioExporterCancelDeleteTone"));
	TestNotNull(TEXT("The cancel-delete test should create a transient tone wave."), Wave);
	if (!Wave)
	{
		return false;
	}

	const TSharedRef<FAudioExportPipeline> Pipeline = MakeShared<FAudioExportPipeline>();
	Pipeline->SetForceAsyncSaveForTesting(true);
	const TSharedRef<AudioExporterRuntimeCancelTestUtils::FCancelDeleteState> State = MakeShared<AudioExporterRuntimeCancelTestUtils::FCancelDeleteState>();
	State->Test = this;
	State->Pipeline = Pipeline;
	State->Wave = Wave;
	State->OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("CancelAsyncSaveDeleteDeferred"));
	State->StartSeconds = FPlatformTime::Seconds();

	Pipeline->OnExportResult.AddLambda([State](const FAudioExportResult& InResult)
	{
		++State->ResultEvents;
		State->Result = InResult;
		if (State->Pipeline.IsValid())
		{
			State->LogsAtResult = State->Pipeline->GetExportLogsForTesting();
		}
	});

	FAudioExportSoundEntry Entry = AudioExporterRuntimeTestUtils::MakeFixtureEntry(
		Wave,
		TEXT("AudioExporterCancelDeleteTone"),
		TEXT("/Temp/AudioExporterCancelDeleteTone"));
	TArray<FAudioExportSoundEntry> Entries;
	Entries.Add(Entry);

	FAudioExporterConfig Config;
	Config.OneShotVariations = 2;
	Config.LoopVariations = 2;
	Config.bNormalize = false;
	Config.bMonitor = false;
	Config.bExportTextLog = false;

	Pipeline->BeginLifecycleTestSession(Entries, Config, State->OutputRoot);
	Pipeline->SimulateVariationFinishedForTesting();
	TestEqual(TEXT("The first variation should leave one async save in flight."), Pipeline->GetActiveAsyncExportsForTesting(), 1);
	TestTrue(TEXT("The session should remain active while the second variation is pending."), Pipeline->IsExporting());

	TArray<FString> WavFiles;
	const double OutputDeadline = FPlatformTime::Seconds() + 10.0;
	while (WavFiles.IsEmpty() && Pipeline->GetActiveAsyncExportsForTesting() == 1 && FPlatformTime::Seconds() < OutputDeadline)
	{
		AudioExporterRuntimeTestUtils::FindWavFilesUnder(State->OutputRoot, WavFiles);
		if (WavFiles.IsEmpty())
		{
			FPlatformProcess::Sleep(0.001f);
		}
	}

	TestEqual(TEXT("The async worker should write one real WAV before cancellation."), WavFiles.Num(), 1);
	TestEqual(TEXT("Cancellation must happen while the async save is still in flight."), Pipeline->GetActiveAsyncExportsForTesting(), 1);
	if (WavFiles.Num() != 1 || Pipeline->GetActiveAsyncExportsForTesting() != 1)
	{
		Pipeline->CancelExporting(false);
		AudioExporterRuntimeCancelTestUtils::RegisterCancelledSaveCleanup(State);
		return false;
	}

	Pipeline->CancelExporting(true);
	TestFalse(TEXT("CancelExporting(true) should make the session inactive immediately."), Pipeline->IsExporting());
	TestTrue(TEXT("The export directory must survive until the in-flight save settles."),
		IFileManager::Get().DirectoryExists(*State->OutputRoot));
	TestTrue(TEXT("The real WAV must survive until the in-flight save settles."), IFileManager::Get().FileExists(*WavFiles[0]));

	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeCancelTestUtils::FWaitForCancelledAsyncSaveToSettle(State));
	return true;
}

// Regression coverage: cancellation settlement must consume the OBSERVED procedural
// completion class, never the provisional Max(Loop, OneShot) bound. This procedural source's first
// variation completed naturally, so its observed plan is the configured 2 one-shot variations while
// the configured loop plan is 5. A cancel mid-source must settle exactly the observed plan's one
// un-attempted variation as skipped; reverting settlement to the provisional bound fabricates three
// extra skip rows and fails the counts below.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterCancellationSettlesObservedTotalsTest,
	"AudioFactory.AudioExporter.Runtime.Lifecycle.CancellationSettlesObservedTotals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterCancellationSettlesObservedTotalsTest::RunTest(const FString& Parameters)
{
	USoundWaveProcedural* Wave = AudioExporterRuntimeTestUtils::MakeTransientToneWave(TEXT("AudioExporterObservedCancelTone"), 0.25f);
	TestNotNull(TEXT("The observed-totals cancel test should create a transient tone wave."), Wave);
	if (!Wave)
	{
		return false;
	}

	const TSharedRef<FAudioExportPipeline> Pipeline = MakeShared<FAudioExportPipeline>();
	Pipeline->SetForceAsyncSaveForTesting(false);

	int32 ResultEvents = 0;
	FAudioExportResult TerminalResult;
	TArray<FAudioExportAssetLog> LogsAtResult;
	Pipeline->OnExportResult.AddLambda([&ResultEvents, &TerminalResult, &Pipeline, &LogsAtResult](const FAudioExportResult& InResult)
	{
		++ResultEvents;
		TerminalResult = InResult;
		LogsAtResult = Pipeline->GetExportLogsForTesting();
	});

	FAudioExportSoundEntry Entry = AudioExporterRuntimeTestUtils::MakeFixtureEntry(
		Wave,
		TEXT("AudioExporterObservedCancelTone"),
		TEXT("/Temp/AudioExporterObservedCancelTone"));
	TArray<FAudioExportSoundEntry> Entries;
	Entries.Add(Entry);

	FAudioExporterConfig Config;
	Config.OneShotVariations = 2;
	Config.LoopVariations = 5;
	Config.bNormalize = false;
	Config.bMonitor = false;
	Config.bExportTextLog = false;

	const FString OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("CancellationSettlesObservedTotals"));
	Pipeline->BeginLifecycleTestSession(Entries, Config, OutputRoot);

	// Variation 1 completes naturally: the observed class becomes "natural one-shot" and the
	// request's plan for this source is the 2 one-shot variations -- never Max(2, 5) = 5.
	Pipeline->SimulateProceduralCompletionForTesting(false);

	Pipeline->CancelExporting(false);

	TestEqual(TEXT("Cancellation should settle exactly one structured result."), ResultEvents, 1);
	TestEqual(TEXT("Cancellation should settle as Cancelled."), TerminalResult.Outcome, EAudioExportOutcome::Cancelled);
	TestEqual(TEXT("Cancellation must keep the saved variation counted as successful."), TerminalResult.SuccessfulCount, 1);
	TestEqual(TEXT("Cancellation must settle exactly the observed plan's single un-attempted variation as skipped."), TerminalResult.SkippedCount, 1);
	TestEqual(TEXT("The terminal rows must conserve exactly the observed plan of two variations."),
		TerminalResult.SuccessfulCount + TerminalResult.FailedCount + TerminalResult.SkippedCount, 2);

	TestEqual(TEXT("Cancellation should preserve one asset log."), LogsAtResult.Num(), 1);
	if (LogsAtResult.Num() == 1)
	{
		TestEqual(TEXT("The asset log should hold both observed plan rows."), LogsAtResult[0].Variations.Num(), 2);
		if (LogsAtResult[0].Variations.Num() == 2)
		{
			TestEqual(TEXT("The saved variation row should stay Succeeded."), LogsAtResult[0].Variations[0].Status, EAudioExportRowStatus::Succeeded);
			TestTrue(TEXT("The saved variation row should carry real measured levels."), LogsAtResult[0].Variations[0].bMeasurementsRecorded);
			TestEqual(TEXT("The un-attempted variation row should be explicitly Skipped."), LogsAtResult[0].Variations[1].Status, EAudioExportRowStatus::Skipped);
			TestFalse(TEXT("A row that never measured must not claim measured levels."), LogsAtResult[0].Variations[1].bMeasurementsRecorded);
		}
	}

	Wave->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterSessionProgressDenominatorIsStableTest,
	"AudioFactory.AudioExporter.Runtime.Progress.SessionDenominatorStable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterSessionProgressDenominatorIsStableTest::RunTest(const FString& Parameters)
{
	USoundWave* StaticWave = NewObject<USoundWave>(GetTransientPackage(), TEXT("AudioExporterProgressStaticWave"));
	USoundWaveProcedural* MultiVariationWave = AudioExporterRuntimeTestUtils::MakeTransientToneWave(TEXT("AudioExporterProgressMultiVariationWave"));
	TestNotNull(TEXT("The progress-denominator test should create a static SoundWave."), StaticWave);
	TestNotNull(TEXT("The progress-denominator test should create a multi-variation wave."), MultiVariationWave);
	if (!StaticWave || !MultiVariationWave)
	{
		if (MultiVariationWave)
		{
			MultiVariationWave->RemoveFromRoot();
		}
		return false;
	}
	StaticWave->AddToRoot();
	MultiVariationWave->bLooping = true;

	const TSharedRef<FAudioExportPipeline> Pipeline = MakeShared<FAudioExportPipeline>();
	Pipeline->SetForceAsyncSaveForTesting(false);
	FAudioExportResult Result;
	int32 ResultEvents = 0;
	Pipeline->OnExportResult.AddLambda([&Result, &ResultEvents](const FAudioExportResult& InResult)
	{
		Result = InResult;
		++ResultEvents;
	});
	TArray<int32> SessionDenominators;
	Pipeline->OnExportProgressUpdated.AddLambda([&SessionDenominators](const FAudioExportProgressInfo& Info)
	{
		SessionDenominators.Add(Info.TotalExpectedFiles);
	});

	FAudioExportSoundEntry StaticEntry = AudioExporterRuntimeTestUtils::MakeFixtureEntry(
		StaticWave, TEXT("AudioExporterProgressStaticWave"), TEXT("/Temp/AudioExporterProgressStaticWave"));
	FAudioExportSoundEntry MultiEntry = AudioExporterRuntimeTestUtils::MakeFixtureEntry(
		MultiVariationWave, TEXT("AudioExporterProgressMultiVariationWave"), TEXT("/Temp/AudioExporterProgressMultiVariationWave"));
	TArray<FAudioExportSoundEntry> Entries;
	Entries.Add(StaticEntry);
	Entries.Add(MultiEntry);

	FAudioExporterConfig Config;
	Config.OneShotVariations = 2;
	Config.LoopVariations = 4;
	Config.bNormalize = false;
	Config.bMonitor = false;
	Config.bExportTextLog = false;

	const FString OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("SessionProgressDenominatorStable"));
	Pipeline->BeginLifecycleTestSession(Entries, Config, OutputRoot);
	Pipeline->SimulateVariationFinishedForTesting();
	Pipeline->SimulateProceduralCompletionForTesting(true);
	Pipeline->SimulateVariationFinishedForTesting();
	Pipeline->SimulateVariationFinishedForTesting();
	Pipeline->SimulateVariationFinishedForTesting();

	TestEqual(TEXT("The mixed static/looping session should settle exactly one result."), ResultEvents, 1);
	TestEqual(TEXT("The mixed session should write five variations: one static and four looping."), Result.SuccessfulCount, 5);
	TestTrue(TEXT("Progress should publish at least one row update."), SessionDenominators.Num() > 0);
	for (const int32 Denominator : SessionDenominators)
	{
		TestEqual(TEXT("Every mixed-session progress update must use the same session-owned denominator."), Denominator, 5);
	}

	StaticWave->RemoveFromRoot();
	MultiVariationWave->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterProceduralCompletionTotalsFollowObservedOutcomeTest,
	"AudioFactory.AudioExporter.Runtime.Progress.ProceduralCompletionTotalsFollowObservedOutcome",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

namespace AudioExporterRuntimeTestUtils
{
	struct FProceduralTotalsRealCaptureCase
	{
		FAutomationTestBase* Test = nullptr;
		TSharedPtr<FAudioExportPipeline> Pipeline;
		TObjectPtr<USoundBase> Wave = nullptr;
		FString CaseName;
		FString OutputRoot;
		FString FinishedPath;
		FAudioExportResult Result;
		TArray<int32> ProgressTotals;
		TArray<bool> ProgressHasResult;
		int32 ExpectedFiles = 0;
		int32 TerminalEvents = 0;
		int32 StructuredTerminalEvents = 0;
		bool bFinished = false;
		bool bWriterDrainConfirmed = false;
		bool bWaitTimedOut = false;
		bool bDeferredCleanupScheduled = false;
		bool bCancellationRequested = false;
		bool bWaveRootAdded = false;
		double StartSeconds = 0.0;
		double CancellationDeadlineSeconds = 0.0;
	};

	struct FProceduralTotalsDeferredCleanupOwner
	{
		TSharedPtr<FProceduralTotalsRealCaptureCase> State;
		TSharedPtr<FAudioExportPipeline> Pipeline;
		TObjectPtr<USoundBase> Wave = nullptr;
		bool bWaveRootAdded = false;
	};

	static void ScheduleProceduralTotalsDeferredCleanup(const TSharedPtr<FProceduralTotalsRealCaptureCase>& State)
	{
		const TSharedRef<FProceduralTotalsDeferredCleanupOwner> DeferredOwner = MakeShared<FProceduralTotalsDeferredCleanupOwner>();
		DeferredOwner->State = State;
		DeferredOwner->Pipeline = State->Pipeline;
		DeferredOwner->Wave = State->Wave;
		DeferredOwner->bWaveRootAdded = State->bWaveRootAdded;
		const double CleanupDeadlineSeconds = FPlatformTime::Seconds() + RealPipelineCancellationDrainSeconds;
		FAutomationTestBase* Test = State->Test;
		bool bMissingTerminalReported = false;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([DeferredOwner, CleanupDeadlineSeconds, Test, bMissingTerminalReported](float) mutable
		{
			if (!DeferredOwner->Pipeline.IsValid())
			{
				if (DeferredOwner->Wave)
				{
					if (DeferredOwner->bWaveRootAdded)
					{
						DeferredOwner->Wave->RemoveFromRoot();
					}
					DeferredOwner->Wave = nullptr;
				}
				return false;
			}
			if (FPlatformTime::Seconds() > CleanupDeadlineSeconds)
			{
				if (Test)
				{
					Test->AddError(TEXT("Procedural totals deferred cleanup exceeded the named writer-drain deadline; the live pipeline and source remain preserved for safety."));
				}
				AudioExporterRuntimeTestUtils::RegisterRetainedTestCleanup(
					[DeferredOwner]()
					{
						return DeferredOwner->State.IsValid()
							&& DeferredOwner->State->StructuredTerminalEvents > 0
							&& DeferredOwner->Pipeline.IsValid()
							&& !DeferredOwner->Pipeline->IsExporting()
							&& DeferredOwner->Pipeline->GetActiveAsyncExportsForTesting() == 0;
					},
					[DeferredOwner]()
					{
						if (DeferredOwner->Wave)
						{
							if (DeferredOwner->bWaveRootAdded)
							{
								DeferredOwner->Wave->RemoveFromRoot();
							}
							DeferredOwner->Wave = nullptr;
						}
						if (DeferredOwner->State.IsValid())
						{
							DeferredOwner->State->Wave = nullptr;
							DeferredOwner->State->Pipeline.Reset();
						}
						DeferredOwner->Pipeline.Reset();
					},
					[DeferredOwner, Test]()
					{
						if (DeferredOwner->Pipeline.IsValid()
							&& (DeferredOwner->Pipeline->IsExporting()
								|| DeferredOwner->Pipeline->GetActiveAsyncExportsForTesting() != 0))
						{
							if (Test)
							{
								Test->AddError(TEXT("Procedural totals retained live pipeline state during engine shutdown because the pipeline was still active or had async writers."));
							}
							return false;
						}
						if (DeferredOwner->State.IsValid()
							&& DeferredOwner->State->StructuredTerminalEvents == 0
							&& Test)
						{
							Test->AddError(TEXT("Procedural totals reached engine shutdown without a structured terminal result; releasing only after shutdown made the backend inactive."));
						}
						if (DeferredOwner->Wave)
						{
							if (DeferredOwner->bWaveRootAdded)
							{
								DeferredOwner->Wave->RemoveFromRoot();
							}
							DeferredOwner->Wave = nullptr;
						}
						if (DeferredOwner->State.IsValid())
						{
							DeferredOwner->State->Wave = nullptr;
							DeferredOwner->State->Pipeline.Reset();
						}
						DeferredOwner->Pipeline.Reset();
						return true;
					});
				return false;
			}

			const bool bPipelineInactive = !DeferredOwner->Pipeline->IsExporting();
			const int32 ActiveAsyncExports = DeferredOwner->Pipeline->GetActiveAsyncExportsForTesting();
			const bool bStructuredTerminalDelivered = DeferredOwner->State.IsValid()
				&& DeferredOwner->State->StructuredTerminalEvents > 0;
			if (bPipelineInactive && ActiveAsyncExports == 0 && !bStructuredTerminalDelivered)
			{
				if (!bMissingTerminalReported && Test)
				{
					Test->AddError(TEXT("Procedural totals became inactive with zero async writers before the structured terminal result was delivered; retaining the terminal-listener cleanup path."));
					bMissingTerminalReported = true;
				}
				return true;
			}
			if (bPipelineInactive && ActiveAsyncExports == 0 && bStructuredTerminalDelivered)
			{
				if (DeferredOwner->Wave)
				{
					if (DeferredOwner->bWaveRootAdded)
					{
						DeferredOwner->Wave->RemoveFromRoot();
					}
					DeferredOwner->Wave = nullptr;
				}
				DeferredOwner->Pipeline.Reset();
				return false;
			}

			return true;
		}));
	}

	class FRunRealProceduralCompletionTotals final : public IAutomationLatentCommand
	{
	public:
		explicit FRunRealProceduralCompletionTotals(FAutomationTestBase& InTest)
			: Test(InTest)
		{
		}

		virtual bool Update() override
		{
			if (Phase == EPhase::Start)
			{
				if (CaseIndex >= 2)
				{
					return true;
				}

				const bool bCaptureCeiling = CaseIndex == 1;
				Current = MakeShared<FProceduralTotalsRealCaptureCase>();
				Current->Test = &Test;
				Current->CaseName = bCaptureCeiling ? TEXT("CaptureCeiling") : TEXT("Natural");
				Current->ExpectedFiles = bCaptureCeiling ? 3 : 1;
				const TCHAR* FixturePath = bCaptureCeiling
					? TEXT("/Game/Procedurally_Generated_Wind/MS_Procedurally_Generated_Wind.MS_Procedurally_Generated_Wind")
					: TEXT("/Game/MetaSoundOneShot.MetaSoundOneShot");
				Current->Wave = LoadObject<USoundBase>(nullptr, FixturePath, nullptr, LOAD_NoWarn | LOAD_Quiet);
				Test.TestNotNull(FString::Printf(TEXT("%s required procedural fixture '%s' should load."), *Current->CaseName, FixturePath), Current->Wave.Get());
				if (!Current->Wave)
				{
					return true;
				}
				if (!Current->Wave->IsRooted())
				{
					Current->Wave->AddToRoot();
					Current->bWaveRootAdded = true;
				}
				Current->OutputRoot = MakeOutputDirectory(FString::Printf(TEXT("ProceduralCompletionTotals_Real_%s"), *Current->CaseName));
				Current->Pipeline = MakeShared<FAudioExportPipeline>();
				Current->Pipeline->Initialize(ResolveAutomationWorld());
				Current->StartSeconds = FPlatformTime::Seconds();

				const TWeakPtr<FProceduralTotalsRealCaptureCase> WeakCurrent = Current;
				Current->Pipeline->OnExportFinished.AddLambda([WeakCurrent](const FString& InPath)
				{
					if (const TSharedPtr<FProceduralTotalsRealCaptureCase> State = WeakCurrent.Pin())
					{
						++State->TerminalEvents;
						State->FinishedPath = InPath;
						State->bFinished = true;
					}
				});
				Current->Pipeline->OnExportProgressUpdated.AddLambda([WeakCurrent](const FAudioExportProgressInfo& InProgress)
				{
					if (const TSharedPtr<FProceduralTotalsRealCaptureCase> State = WeakCurrent.Pin())
					{
						State->ProgressTotals.Add(InProgress.TotalExpectedFiles);
						State->ProgressHasResult.Add(InProgress.bHasResult);
					}
				});
				Current->Pipeline->OnExportResult.AddLambda([WeakCurrent](const FAudioExportResult& InResult)
				{
					if (const TSharedPtr<FProceduralTotalsRealCaptureCase> State = WeakCurrent.Pin())
					{
						++State->StructuredTerminalEvents;
						State->Result = InResult;
					}
				});

				const FString WaveName = Current->Wave->GetName();
				const FString WaveAssetPath = Current->Wave->GetPackage()->GetName();
				FAudioExportSoundEntry Entry = MakeFixtureEntry(
					Current->Wave.Get(),
					*WaveName,
					*WaveAssetPath);
				FAudioExporterConfig Config;
				Config.OneShotVariations = 1;
				Config.LoopVariations = 3;
				Config.MaxLengthSeconds = 5;
				Config.bNormalize = false;
				Config.bMonitor = false;
				Config.bExportTextLog = false;
				TArray<FAudioExportSoundEntry> Entries;
				Entries.Add(Entry);

				const bool bStarted = Current->Pipeline->StartExporting(Entries, Config, Current->OutputRoot);
				Test.TestTrue(FString::Printf(TEXT("%s real procedural capture should be accepted through public StartExporting."), *Current->CaseName), bStarted);
				if (!bStarted)
				{
					CleanupCurrent();
					return true;
				}

				Phase = EPhase::Wait;
				return false;
			}

			if (!Current.IsValid())
			{
				return true;
			}

			if (Phase == EPhase::Wait)
			{
				const bool bPipelineInactive = Current->Pipeline.IsValid()
					&& !Current->Pipeline->IsExporting();
				const int32 ActiveAsyncExports = Current->Pipeline.IsValid()
					? Current->Pipeline->GetActiveAsyncExportsForTesting()
					: -1;
				if (Current->bFinished
					&& Current->StructuredTerminalEvents > 0
					&& bPipelineInactive
					&& ActiveAsyncExports == 0)
				{
					Current->bWriterDrainConfirmed = true;
					Phase = EPhase::Verify;
					return false;
				}

				const double NowSeconds = FPlatformTime::Seconds();
				if (!Current->bCancellationRequested && (NowSeconds - Current->StartSeconds) > 30.0)
				{
					Test.AddError(FString::Printf(TEXT("%s real procedural capture timed out; waiting for terminal writer shutdown after cancellation."), *Current->CaseName));
					Current->bCancellationRequested = true;
					Current->CancellationDeadlineSeconds = NowSeconds + 5.0;
					if (Current->Pipeline.IsValid() && Current->Pipeline->IsExporting())
					{
						Current->Pipeline->CancelExporting(false);
					}
					return false;
				}

				if (Current->bCancellationRequested && NowSeconds > Current->CancellationDeadlineSeconds)
				{
					Current->bWaitTimedOut = true;
					if (!Current->bDeferredCleanupScheduled)
					{
						ScheduleProceduralTotalsDeferredCleanup(Current);
						Current->bDeferredCleanupScheduled = true;
					}
					Test.AddError(FString::Printf(TEXT("%s did not settle its terminal result and writer before the cancellation drain deadline."), *Current->CaseName));
					Phase = EPhase::Verify;
					return false;
				}
				return false;
			}

			if (!Current->bWriterDrainConfirmed || Current->bWaitTimedOut)
			{
				Test.AddError(FString::Printf(TEXT("%s verification was reached without confirmed terminal delivery and writer inactivity."), *Current->CaseName));
				return true;
			}

			const bool bPipelineInactive = Current->Pipeline.IsValid()
				&& !Current->Pipeline->IsExporting();
			const int32 ActiveAsyncExports = Current->Pipeline.IsValid()
				? Current->Pipeline->GetActiveAsyncExportsForTesting()
				: -1;
			Test.TestFalse(FString::Printf(TEXT("%s should be inactive before procedural capture cleanup."), *Current->CaseName), !bPipelineInactive);
			Test.TestEqual(FString::Printf(TEXT("%s should have zero active async exports before procedural capture cleanup."), *Current->CaseName), ActiveAsyncExports, 0);
			if (!bPipelineInactive || ActiveAsyncExports != 0)
			{
				Test.AddError(FString::Printf(TEXT("%s cleanup was blocked because the procedural writer drain was not confirmed."), *Current->CaseName));
				return true;
			}

			const FString Label = Current->CaseName;
			Test.TestEqual(FString::Printf(TEXT("%s should emit exactly one legacy terminal event."), *Label), Current->TerminalEvents, 1);
			Test.TestEqual(FString::Printf(TEXT("%s should emit exactly one structured terminal result."), *Label), Current->StructuredTerminalEvents, 1);
			Test.TestEqual(FString::Printf(TEXT("%s structured outcome must be Succeeded."), *Label), Current->Result.Outcome, EAudioExportOutcome::Succeeded);
			Test.TestFalse(FString::Printf(TEXT("%s should report a non-empty export path."), *Label), Current->Result.ExportPath.IsEmpty());
			Test.TestEqual(FString::Printf(TEXT("%s should report its authoritative successful count."), *Label), Current->Result.SuccessfulCount, Current->ExpectedFiles);
			Test.TestEqual(FString::Printf(TEXT("%s should report no failed files."), *Label), Current->Result.FailedCount, 0);
			Test.TestEqual(FString::Printf(TEXT("%s should report no skipped files."), *Label), Current->Result.SkippedCount, 0);
			Test.TestEqual(FString::Printf(TEXT("%s terminal counts must conserve the authoritative requested total."), *Label),
				Current->Result.SuccessfulCount + Current->Result.FailedCount + Current->Result.SkippedCount,
				Current->ExpectedFiles);

			Test.TestTrue(FString::Printf(TEXT("%s should publish progress totals."), *Label), !Current->ProgressTotals.IsEmpty());
			bool bObservedCompletion = false;
			for (int32 ProgressIndex = 0; ProgressIndex < Current->ProgressTotals.Num(); ++ProgressIndex)
			{
				if (Current->ProgressHasResult[ProgressIndex])
				{
					bObservedCompletion = true;
				}
				if (bObservedCompletion)
				{
					Test.TestEqual(FString::Printf(TEXT("%s progress total after observed completion must be authoritative."), *Label),
						Current->ProgressTotals[ProgressIndex], Current->ExpectedFiles);
				}
			}
			Test.TestTrue(FString::Printf(TEXT("%s should publish a post-observation authoritative progress total."), *Label), bObservedCompletion);

			TArray<FString> WavFiles;
			FindWavFilesUnder(Current->OutputRoot, WavFiles);
			Test.TestEqual(FString::Printf(TEXT("%s should emit exactly its authoritative number of physical WAV files."), *Label), WavFiles.Num(), Current->ExpectedFiles);
			for (const FString& WavFile : WavFiles)
			{
				ValidateWavOutput(Test, WavFile, Label);
			}

			CleanupCurrent();
			++CaseIndex;
			Phase = EPhase::Start;
			return false;
		}

	private:
		enum class EPhase : uint8
		{
			Start,
			Wait,
			Verify
		};

		void CleanupCurrent()
		{
			if (Current.IsValid() && Current->Wave)
			{
				if (Current->bWaveRootAdded)
				{
					Current->Wave->RemoveFromRoot();
				}
				Current->Wave = nullptr;
			}
			if (Current.IsValid())
			{
				Current->Pipeline.Reset();
			}
			Current.Reset();
		}

		FAutomationTestBase& Test;
		TSharedPtr<FProceduralTotalsRealCaptureCase> Current;
		EPhase Phase = EPhase::Start;
		int32 CaseIndex = 0;
	};
}

bool FAudioExporterProceduralCompletionTotalsFollowObservedOutcomeTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FRunRealProceduralCompletionTotals(*this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterOutOfOrderRowsConserveCountsTest,
	"AudioFactory.AudioExporter.Runtime.Report.OutOfOrderRowsConserveCounts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterOutOfOrderRowsConserveCountsTest::RunTest(const FString& Parameters)
{
	const TSharedRef<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline> Pipeline = MakeShared<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline>();
	FAudioExportSoundEntry Entry = AudioExporterRuntimeTestUtils::MakeFixtureEntry(
		nullptr, TEXT("AudioExporterOutOfOrderTone"), TEXT("/Temp/AudioExporterOutOfOrderTone"));
	TArray<FAudioExportSoundEntry> Entries;
	Entries.Add(Entry);

	FAudioExporterConfig Config;
	Config.OneShotVariations = 3;
	Config.LoopVariations = 3;
	Config.bNormalize = false;
	Config.bMonitor = false;
	Config.bExportTextLog = true;
	const FString OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("OutOfOrderRowsConserveCounts"));
	Pipeline->BeginLifecycleTestSession(Entries, Config, OutputRoot);

	FPipelineVariationCaptureData VariationThree;
	VariationThree.SoundName = Entry.SoundName;
	VariationThree.SourceAssetPath = Entry.SourceAssetPath;
	VariationThree.RelativePath = Entry.RelativePath;
	VariationThree.VariationIndex = 2;
	VariationThree.SoundIndex = 0;
	VariationThree.TotalVariations = 3;
	Pipeline->RecordVariationResult(VariationThree, true, true, TEXT("AudioExporterOutOfOrderTone_3.wav"), OutputRoot / TEXT("AudioExporterOutOfOrderTone_3.wav"), -6.0f, -12.0f, 0.25f, 0.0f, FString());

	FPipelineVariationCaptureData VariationOne = VariationThree;
	VariationOne.VariationIndex = 0;
	Pipeline->RecordVariationResult(VariationOne, false, true, FString(), FString(), 0.0f, 0.0f, 0.0f, 0.0f, TEXT("Render failed: first variation"));

	const TArray<FAudioExportAssetLog> LogsBeforeFinalize = Pipeline->GetExportLogsForTesting();
	TestEqual(TEXT("Out-of-order recording should keep one asset log."), LogsBeforeFinalize.Num(), 1);
	if (LogsBeforeFinalize.Num() == 1)
	{
		TestEqual(TEXT("Out-of-order recording should produce exactly the two real rows."), LogsBeforeFinalize[0].Variations.Num(), 2);
		for (const FAudioExportLogEntry& Row : LogsBeforeFinalize[0].Variations)
		{
			TestTrue(TEXT("Out-of-order rows must never remain Pending."), Row.Status != EAudioExportRowStatus::Pending);
		}
	}

	int32 ResultEvents = 0;
	FAudioExportResult Result;
	Pipeline->OnExportResult.AddLambda([&ResultEvents, &Result](const FAudioExportResult& InResult)
	{
		++ResultEvents;
		Result = InResult;
	});
	Pipeline->CompleteFinalEmptyBatchForTesting();

	TestEqual(TEXT("Out-of-order rows should settle exactly one terminal result."), ResultEvents, 1);
	TestEqual(TEXT("The terminal success count must equal the succeeded report rows."), Result.SuccessfulCount, 1);
	TestEqual(TEXT("The terminal failure count must equal the failed report rows."), Result.FailedCount, 1);
	TestEqual(TEXT("The report rows must conserve the terminal file counts."), Result.SuccessfulCount + Result.FailedCount + Result.SkippedCount, 2);

	FString TextLog;
	TestTrue(TEXT("The out-of-order row test should write a terminal text report."),
		AudioExporterRuntimeTestUtils::LoadReportFile(OutputRoot, TEXT("AudioExporterTextLog.txt"), TextLog));
	TestTrue(TEXT("The report should request exactly two real rows."), TextLog.Contains(TEXT("Requested                   : 2")));
	TestTrue(TEXT("The report should count one succeeded row."), TextLog.Contains(TEXT("Succeeded                   : 1")));
	TestTrue(TEXT("The report should count one failed row."), TextLog.Contains(TEXT("Failed                      : 1")));
	TestFalse(TEXT("The out-of-order report must not contain Pending rows."), TextLog.Contains(TEXT("Pending")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterNullCurrentSoundTerminatesSessionTest,
	"AudioFactory.AudioExporter.Runtime.Lifecycle.NullCurrentSoundTerminatesSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterNullCurrentSoundTerminatesSessionTest::RunTest(const FString& Parameters)
{
	const TSharedRef<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline> Pipeline = MakeShared<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline>();
	int32 ResultEvents = 0;
	FAudioExportResult Result;
	Pipeline->OnExportResult.AddLambda([&ResultEvents, &Result](const FAudioExportResult& InResult)
	{
		++ResultEvents;
		Result = InResult;
	});

	Pipeline->BeginLifecycleTestSession(AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("NullCurrentSoundTerminatesSession")));
	TestTrue(TEXT("The null-current-sound fixture should begin as an active session."), Pipeline->IsExporting());
	AddExpectedError(TEXT("FAudioExportPipeline::ProcessCurrentSound: Export failed because the current sound was unavailable."), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("FAudioExportPipeline::FailStartup: Export failed because the current sound was unavailable."), EAutomationExpectedErrorFlags::Contains, 1);
	Pipeline->ProcessCurrentSound();

	TestFalse(TEXT("A null CurrentSoundToPlay must not leave the pipeline active."), Pipeline->IsExporting());
	TestEqual(TEXT("A null CurrentSoundToPlay must settle one terminal result."), ResultEvents, 1);
	TestEqual(TEXT("A null CurrentSoundToPlay must settle as Failed."), Result.Outcome, EAudioExportOutcome::Failed);
	TestFalse(TEXT("A null CurrentSoundToPlay failure must carry an explicit error."), Result.ErrorMessage.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterNullQueueEntryProducesTerminalRowTest,
	"AudioFactory.AudioExporter.Runtime.Report.NullQueueEntryProducesTerminalRow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterNullQueueEntryProducesTerminalRowTest::RunTest(const FString& Parameters)
{
	const TSharedRef<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline> Pipeline = MakeShared<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline>();
	FAudioExportResult Result;
	int32 ResultEvents = 0;
	Pipeline->OnExportResult.AddLambda([&](const FAudioExportResult& InResult)
	{
		++ResultEvents;
		Result = InResult;
	});

	FAudioExportSoundEntry NullEntry;
	NullEntry.SoundName = TEXT("AudioExporterNullQueueEntry");
	NullEntry.SourceAssetPath = TEXT("/Temp/AudioExporterNullQueueEntry");
	NullEntry.RelativePath = TEXT("NullQueueEntry");
	TArray<FAudioExportSoundEntry> Entries;
	Entries.Add(NullEntry);

	FAudioExporterConfig Config;
	Config.OneShotVariations = 3;
	Config.LoopVariations = 5;
	Config.bNormalize = false;
	Config.bMonitor = false;
	Config.bExportTextLog = true;
	const FString OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("NullQueueEntryProducesTerminalRow"));
	Pipeline->BeginLifecycleTestSession(Entries, Config, OutputRoot);
	Pipeline->StartNextSound();

	TestEqual(TEXT("A null queue entry should settle exactly one terminal result."), ResultEvents, 1);
	TestEqual(TEXT("A null queue entry should be an explicit failed output."), Result.FailedCount, 1);
	TestEqual(TEXT("A null queue entry should not create an unknown skipped count."), Result.SkippedCount, 0);

	const TArray<FAudioExportAssetLog> Logs = Pipeline->GetExportLogsForTesting();
	TestEqual(TEXT("A null queue entry should preserve one asset log."), Logs.Num(), 1);
	if (Logs.Num() == 1)
	{
		TestEqual(TEXT("A null queue entry should preserve one terminal row."), Logs[0].Variations.Num(), 1);
		if (Logs[0].Variations.Num() == 1)
		{
			TestEqual(TEXT("The null queue row must be explicitly Failed."), Logs[0].Variations[0].Status, EAudioExportRowStatus::Failed);
		}
	}

	FString TextLog;
	TestTrue(TEXT("The null queue entry should write its production text report."),
		AudioExporterRuntimeTestUtils::LoadReportFile(OutputRoot, TEXT("AudioExporterTextLog.txt"), TextLog));
	TestTrue(TEXT("The null queue report should count the explicit failed row."), TextLog.Contains(TEXT("Requested                   : 1")));
	TestTrue(TEXT("The null queue report should count one failed row."), TextLog.Contains(TEXT("Failed                      : 1")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterResetStateClearsCurrentSoundTest,
	"AudioFactory.AudioExporter.Runtime.Lifecycle.ResetStateClearsCurrentSound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterResetStateClearsCurrentSoundTest::RunTest(const FString& Parameters)
{
	const TSharedRef<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline> Pipeline = MakeShared<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline>();
	USoundWave* Wave = NewObject<USoundWave>(GetTransientPackage(), TEXT("AudioExporterResetStateSound"));
	TestNotNull(TEXT("The reset-state test should create a sound object."), Wave);
	if (!Wave)
	{
		return false;
	}

	Pipeline->SetCurrentSoundForTesting(Wave);
	TestNotNull(TEXT("The reset-state fixture should seed CurrentSoundToPlay."), Pipeline->GetCurrentSoundForTesting());
	Pipeline->ResetStateForTesting();
	TestNull(TEXT("ResetState must clear CurrentSoundToPlay."), Pipeline->GetCurrentSoundForTesting());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterDestinationFailureBatchAbortTest,
	"AudioFactory.AudioExporter.Runtime.MidBatchFailure.DestinationFailureBatchAbort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterDestinationFailureBatchAbortTest::RunTest(const FString& Parameters)
{
	// KILLS: removing the destination-failure abort in the free Exporter, or flipping its threshold
	// from two failures to one.
	//
	// Two destination-level save failures must stop the batch instead of repeating a doomed write
	// per remaining variation: the un-attempted variation settles as an explicit skip, the terminal
	// outcome is Failed (never Cancelled -- the user did not cancel), and the terminal message
	// carries the destination reason. The companion case proves a plain per-file failure
	// (bDestinationUnusable=false) does NOT stop the batch: the next variation is still attempted,
	// and the session completes normally as a partial success.
	UWorld* World = AudioExporterRuntimeTestUtils::ResolveAutomationWorld();
	if (!World)
	{
		AddError(TEXT("Destination-abort test is unavailable: no editor world was available."));
		return false;
	}

	USoundWaveProcedural* Wave = AudioExporterRuntimeTestUtils::MakeTransientToneWave(TEXT("AudioExporterDestAbortTone"), 0.25f);
	TestNotNull(TEXT("The destination-abort test should create a transient tone wave."), Wave);
	if (!Wave)
	{
		return false;
	}

	const TSharedRef<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline> Pipeline = MakeShared<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline>();
	Pipeline->Initialize(World);

	int32 ResultEvents = 0;
	FAudioExportResult TerminalResult;
	Pipeline->OnExportResult.AddLambda([&ResultEvents, &TerminalResult](const FAudioExportResult& InResult)
	{
		++ResultEvents;
		TerminalResult = InResult;
	});

	TArray<FAudioExportSoundEntry> Entries;
	Entries.Add(AudioExporterRuntimeTestUtils::MakeFixtureEntry(Wave, TEXT("AudioExporterDestAbortTone"), TEXT("/Temp/AudioExporterDestAbortTone")));

	const FAudioExporterConfig Config = AudioExporterRuntimeTestUtils::MakeFailureFixtureConfig(3);
	const FString OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("DestinationFailureBatchAbort"));
	Pipeline->BeginLifecycleTestSession(Entries, Config, OutputRoot);
	TestTrue(TEXT("The destination-abort fixture session should be accepted."), Pipeline->IsExporting());

	AddExpectedError(TEXT("Output destination unusable"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("FAudioExportPipeline::FailStartup"), EAutomationExpectedErrorFlags::Contains, 1);

	Pipeline->SetSimulatedSaveResultForTesting(false, true, TEXT("Injected unusable output destination"));

	// A single transient destination refusal must not kill the batch...
	Pipeline->SimulateVariationFinishedForTesting();
	TestEqual(TEXT("One destination-level failure must not settle a terminal result."), ResultEvents, 0);
	TestTrue(TEXT("One destination-level failure must leave the session exporting."), Pipeline->IsExporting());

	// ...but a genuinely dead destination fails twice immediately: this latches the abort.
	Pipeline->SimulateVariationFinishedForTesting();
	TestEqual(TEXT("The latched abort should settle exactly one structured terminal result."), ResultEvents, 1);
	if (ResultEvents == 1)
	{
		TestTrue(TEXT("A destination-stopped batch must never be classified as Cancelled."),
			TerminalResult.Outcome != EAudioExportOutcome::Cancelled);
		TestEqual(TEXT("A destination-stopped batch should report the Failed outcome."), TerminalResult.Outcome, EAudioExportOutcome::Failed);
		TestEqual(TEXT("Both destination-level failures should count as failed variations."), TerminalResult.FailedCount, 2);
		TestEqual(TEXT("The un-attempted variation should count as skipped."), TerminalResult.SkippedCount, 1);
		TestEqual(TEXT("No variation of a stopped batch may claim success."), TerminalResult.SuccessfulCount, 0);
		TestEqual(TEXT("The terminal result must conserve all three planned variations."),
			TerminalResult.SuccessfulCount + TerminalResult.FailedCount + TerminalResult.SkippedCount, 3);
		TestTrue(TEXT("The terminal message must carry the destination failure reason."),
			TerminalResult.ErrorMessage.Contains(TEXT("Injected unusable output destination")));
		TestTrue(TEXT("Nothing was written, so no export path may be reported."), TerminalResult.ExportPath.IsEmpty());
	}

	// Once latched, no further variation starts and no extra result fires.
	Pipeline->ClearSimulatedSaveResultForTesting();
	Pipeline->SimulateVariationFinishedForTesting();
	TestEqual(TEXT("A latched batch must refuse further variations."), ResultEvents, 1);

	// The latched abort must wire the same publication fence the cancellation teardown uses: late
	// in-flight saves may no longer publish to the dead destination.
	TestTrue(TEXT("The latched destination abort must fence the session path against late publications."),
		FAudioExporterUtils::IsExportPathCancelled(OutputRoot));

	Wave->RemoveFromRoot();

	// Companion case: a plain per-file failure (bDestinationUnusable=false) does not abort the batch.
	USoundWaveProcedural* PerFileWave = AudioExporterRuntimeTestUtils::MakeTransientToneWave(TEXT("AudioExporterPerFileFailTone"), 0.25f);
	TestNotNull(TEXT("The per-file companion case should create a transient tone wave."), PerFileWave);
	if (!PerFileWave)
	{
		return false;
	}

	const TSharedRef<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline> PerFilePipeline = MakeShared<AudioExporterRuntimeTestUtils::FExposedRuntimePipeline>();
	PerFilePipeline->Initialize(World);

	int32 PerFileResultEvents = 0;
	FAudioExportResult PerFileResult;
	PerFilePipeline->OnExportResult.AddLambda([&PerFileResultEvents, &PerFileResult](const FAudioExportResult& InResult)
	{
		++PerFileResultEvents;
		PerFileResult = InResult;
	});

	TArray<FAudioExportSoundEntry> PerFileEntries;
	PerFileEntries.Add(AudioExporterRuntimeTestUtils::MakeFixtureEntry(PerFileWave, TEXT("AudioExporterPerFileFailTone"), TEXT("/Temp/AudioExporterPerFileFailTone")));

	const FString PerFileOutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("DestinationFailureBatchAbort_PerFile"));
	PerFilePipeline->BeginLifecycleTestSession(PerFileEntries, Config, PerFileOutputRoot);
	TestTrue(TEXT("The per-file companion fixture session should be accepted."), PerFilePipeline->IsExporting());

	PerFilePipeline->SetSimulatedSaveResultForTesting(false, false, TEXT("Injected plain per-file failure"));
	PerFilePipeline->SimulateVariationFinishedForTesting();
	TestEqual(TEXT("A per-file failure must not settle a terminal result early."), PerFileResultEvents, 0);
	PerFilePipeline->ClearSimulatedSaveResultForTesting();

	// Variations 2 and 3 run real saves: the batch continued past the per-file failure and completes
	// through the normal cascade as a partial success.
	PerFilePipeline->SimulateVariationFinishedForTesting();
	PerFilePipeline->SimulateVariationFinishedForTesting();

	TestEqual(TEXT("The companion batch should complete with exactly one structured result."), PerFileResultEvents, 1);
	if (PerFileResultEvents == 1)
	{
		TestEqual(TEXT("A batch that survives a per-file failure should complete as PartialSuccess."), PerFileResult.Outcome, EAudioExportOutcome::PartialSuccess);
		TestEqual(TEXT("Only the injected per-file failure should count as failed."), PerFileResult.FailedCount, 1);
		TestEqual(TEXT("Both post-failure variations should have been attempted and saved."), PerFileResult.SuccessfulCount, 2);
		TestEqual(TEXT("A surviving batch must skip nothing."), PerFileResult.SkippedCount, 0);
		TestEqual(TEXT("The companion terminal result must conserve all three planned variations."),
			PerFileResult.SuccessfulCount + PerFileResult.FailedCount + PerFileResult.SkippedCount, 3);
		TestFalse(TEXT("A surviving batch keeps its export path."), PerFileResult.ExportPath.IsEmpty());
	}

	// Negative control: a batch that survived a per-file failure never fenced its session path.
	TestFalse(TEXT("A per-file failure that did not abort the batch must not fence the session path."),
		FAudioExporterUtils::IsExportPathCancelled(PerFileOutputRoot));

	FAudioExporterUtils::ClearExportPathCancellation(OutputRoot);
	FAudioExporterUtils::ClearExportPathCancellation(PerFileOutputRoot);

	TArray<FString> PerFileWavFiles;
	AudioExporterRuntimeTestUtils::FindWavFilesUnder(PerFileOutputRoot, PerFileWavFiles);
	TestEqual(TEXT("The two post-failure variations should have written their WAV files."), PerFileWavFiles.Num(), 2);

	PerFileWave->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterSoundWaveUsesRealtimeSourcePathTest,
	"AudioFactory.AudioExporter.Runtime.Dispatch.SoundWaveUsesRealtimeSourcePath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterSoundWaveUsesRealtimeSourcePathTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<AudioExporterRuntimeTestUtils::FRealPipelineExportState> State =
		AudioExporterRuntimeTestUtils::StartRealPipelineExport(
			*this,
			RealtimeSoundWaveContractFixture,
			TEXT("SoundWaveUsesRealtimeSourcePath"),
			false);
	if (!State.IsValid())
	{
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FWaitForAudioExporterRealPipelineExport(
		State.ToSharedRef(), SoundWaveRealtimeBoundedWaitSeconds));
	ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FVerifySoundWaveRealPipelineExport(State.ToSharedRef()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterSoundWaveRejectsInvalidLinearVolumeTest,
	"AudioFactory.AudioExporter.Runtime.Dispatch.SoundWaveRejectsInvalidLinearVolume",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterSoundWaveRejectsInvalidLinearVolumeTest::RunTest(const FString& Parameters)
{
	UWorld* World = AudioExporterRuntimeTestUtils::ResolveAutomationWorld();
	if (!World)
	{
		AddError(TEXT("Invalid SoundWave volume test is unavailable: no editor world was available."));
		return false;
	}

	USoundWave* Wave = LoadObject<USoundWave>(nullptr, TEXT("/Game/Test_1kHz.Test_1kHz"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	TestNotNull(TEXT("The invalid-volume test should load the imported PCM fixture."), Wave);
	if (!Wave)
	{
		return false;
	}

	const float OriginalVolume = Wave->Volume;
	ON_SCOPE_EXIT { Wave->Volume = OriginalVolume; };

	const TArray<float> InvalidVolumes = {
		-1.0f,
		std::numeric_limits<float>::quiet_NaN(),
		std::numeric_limits<float>::infinity(),
		-std::numeric_limits<float>::infinity()};
	const TCHAR* Labels[] = {TEXT("Negative"), TEXT("NaN"), TEXT("PositiveInfinity"), TEXT("NegativeInfinity")};
	AddExpectedError(
		TEXT("FAudioExportPipeline::OnRenderError: Sound render rejected because authored linear Volume must be finite and non-negative."),
		EAutomationExpectedErrorFlags::Contains,
		InvalidVolumes.Num());

	for (int32 CaseIndex = 0; CaseIndex < InvalidVolumes.Num(); ++CaseIndex)
	{
		Wave->Volume = InvalidVolumes[CaseIndex];
		const TSharedRef<AudioExporterRuntimeTestUtils::FVolumeValidationDispatchProbePipeline> Pipeline = MakeShared<AudioExporterRuntimeTestUtils::FVolumeValidationDispatchProbePipeline>();
		Pipeline->Initialize(World);

		FAudioExportResult Result;
		TArray<FAudioExportAssetLog> RowsAtTerminal;
		Pipeline->OnExportResult.AddLambda([&](const FAudioExportResult& InResult)
		{
			Result = InResult;
			RowsAtTerminal = Pipeline->GetExportLogsForTesting();
		});

		TArray<FAudioExportSoundEntry> Entries;
		Entries.Add(AudioExporterRuntimeTestUtils::MakeFixtureEntry(Wave, TEXT("NonFiniteVolumeWave"), TEXT("/Game/Test_1kHz")));
		FAudioExporterConfig Config;
		Config.OneShotVariations = 1;
		Config.LoopVariations = 1;
		Config.bNormalize = false;
		Config.bExportTextLog = false;
		const FString OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(FString::Printf(TEXT("NonFiniteVolume_%s"), Labels[CaseIndex]));
		TestTrue(FString::Printf(TEXT("%s case should reach the public SoundWave volume-validation entry point."), Labels[CaseIndex]),
			Pipeline->StartExporting(Entries, Config, OutputRoot));

		TestEqual(FString::Printf(TEXT("%s must settle as failed."), Labels[CaseIndex]), Result.Outcome, EAudioExportOutcome::Failed);
		TestEqual(FString::Printf(TEXT("%s must record one failed variation."), Labels[CaseIndex]), Result.FailedCount, 1);
		TestEqual(FString::Printf(TEXT("%s must write no successful variation."), Labels[CaseIndex]), Result.SuccessfulCount, 0);
		TArray<FString> WavFiles;
		AudioExporterRuntimeTestUtils::FindWavFilesUnder(OutputRoot, WavFiles);
		TestEqual(FString::Printf(TEXT("%s must write no WAV."), Labels[CaseIndex]), WavFiles.Num(), 0);
		TestEqual(FString::Printf(TEXT("%s should retain one asset report."), Labels[CaseIndex]), RowsAtTerminal.Num(), 1);
		if (RowsAtTerminal.Num() == 1)
		{
			TestEqual(FString::Printf(TEXT("%s should retain one failed row."), Labels[CaseIndex]), RowsAtTerminal[0].Variations.Num(), 1);
			if (RowsAtTerminal[0].Variations.Num() == 1)
			{
				TestEqual(FString::Printf(TEXT("%s row should be failed."), Labels[CaseIndex]), RowsAtTerminal[0].Variations[0].Status, EAudioExportRowStatus::Failed);
				TestTrue(FString::Printf(TEXT("%s row should explain invalid authored linear volume."), Labels[CaseIndex]),
					RowsAtTerminal[0].Variations[0].Details.Contains(TEXT("authored linear Volume must be finite and non-negative")));
			}
		}
		TestEqual(FString::Printf(TEXT("%s dispatch-policy probe should initialize once before validating the source."), Labels[CaseIndex]),
			Pipeline->EnsureRendererCalls, 1);
		TestTrue(FString::Printf(TEXT("%s should not repair the authored asset value silently."), Labels[CaseIndex]),
			FMath::IsFinite(InvalidVolumes[CaseIndex]) ? Wave->Volume == InvalidVolumes[CaseIndex] : !FMath::IsFinite(Wave->Volume));
	}

	return true;
}

struct FGainLimitedPipelineExportState
{
	FAutomationTestBase* Test = nullptr;
	TSharedPtr<FAudioExportPipeline> Pipeline;
	TObjectPtr<USoundWave> Wave = nullptr;
	float OriginalVolume = 1.0f;
	FString OutputRoot;
	FString FinishedPath;
	FAudioExportResult Result;
	TArray<FAudioExportAssetLog> LogsAtTerminal;
	TArray<FAudioExportProgressInfo> ProgressEvents;
	int32 LegacyTerminalEvents = 0;
	int32 StructuredTerminalEvents = 0;
	bool bFinished = false;
	bool bCancellationRequested = false;
	bool bWaveRestored = false;
	bool bCleanupRegistered = false;
	bool bResourcesReleased = false;
	FDelegateHandle ProgressHandle;
	FDelegateHandle FinishedHandle;
	FDelegateHandle ResultHandle;
	double StartSeconds = 0.0;
	double CancellationDeadlineSeconds = 0.0;
};

static void ReleaseGainLimitedPipelineResources(FGainLimitedPipelineExportState& State)
{
	if (State.bResourcesReleased)
	{
		return;
	}

	if (State.Pipeline.IsValid())
	{
		if (State.ProgressHandle.IsValid())
		{
			State.Pipeline->OnExportProgressUpdated.Remove(State.ProgressHandle);
		}
		if (State.FinishedHandle.IsValid())
		{
			State.Pipeline->OnExportFinished.Remove(State.FinishedHandle);
		}
		if (State.ResultHandle.IsValid())
		{
			State.Pipeline->OnExportResult.Remove(State.ResultHandle);
		}
	}
	State.ProgressHandle = FDelegateHandle();
	State.FinishedHandle = FDelegateHandle();
	State.ResultHandle = FDelegateHandle();

	if (!State.bWaveRestored && State.Wave)
	{
		State.Wave->Volume = State.OriginalVolume;
		State.bWaveRestored = true;
	}

	State.Wave = nullptr;
	State.Pipeline.Reset();
	State.OutputRoot.Reset();
	State.FinishedPath.Reset();
	State.bResourcesReleased = true;
}

static void RegisterGainLimitedDeferredCleanup(const TSharedRef<FGainLimitedPipelineExportState>& State)
{
	if (State->bCleanupRegistered)
	{
		return;
	}
	State->bCleanupRegistered = true;

	AudioExporterRuntimeTestUtils::RegisterRetainedTestCleanup(
		[State]()
		{
			return State->StructuredTerminalEvents > 0
				&& State->Pipeline.IsValid()
				&& !State->Pipeline->IsExporting()
				&& State->Pipeline->GetActiveAsyncExportsForTesting() == 0;
		},
		[State]()
		{
			ReleaseGainLimitedPipelineResources(*State);
		},
		[State]()
		{
			if (State->Pipeline.IsValid()
				&& (State->Pipeline->IsExporting()
					|| State->Pipeline->GetActiveAsyncExportsForTesting() != 0))
			{
				if (State->Test)
				{
					State->Test->AddError(TEXT("Gain-limited export retained live pipeline state during engine shutdown because the pipeline was still active or had async writers."));
				}
				return false;
			}
			if (State->StructuredTerminalEvents == 0 && State->Test)
			{
				State->Test->AddError(TEXT("Gain-limited export reached engine shutdown without a structured terminal result; releasing only after the pipeline was inactive and all async writers had drained."));
			}
			ReleaseGainLimitedPipelineResources(*State);
			return true;
		});
}

class FWaitForGainLimitedPipelineExport final : public IAutomationLatentCommand
{
public:
	explicit FWaitForGainLimitedPipelineExport(const TSharedRef<FGainLimitedPipelineExportState>& InState, const double InTimeoutSeconds)
		: State(InState)
		, TimeoutSeconds(InTimeoutSeconds)
	{
	}

	virtual bool Update() override
	{
		const bool bDrained = State->bFinished
			&& State->StructuredTerminalEvents == 1
			&& State->Pipeline.IsValid()
			&& !State->Pipeline->IsExporting()
			&& State->Pipeline->GetActiveAsyncExportsForTesting() == 0;
		if (bDrained)
		{
			if (State->Test)
			{
				State->Test->TestEqual(TEXT("Gain-limited export must emit exactly one legacy terminal event."), State->LegacyTerminalEvents, 1);
				State->Test->TestEqual(TEXT("Gain-limited export must emit exactly one structured terminal result."), State->StructuredTerminalEvents, 1);
				State->Test->TestEqual(TEXT("Gain-limited export must settle as Succeeded."), State->Result.Outcome, EAudioExportOutcome::Succeeded);
				State->Test->TestEqual(TEXT("Gain-limited export must report exactly one requested successful variation."), State->Result.SuccessfulCount, 1);
				State->Test->TestEqual(TEXT("Gain-limited export must report zero failed variations."), State->Result.FailedCount, 0);
				State->Test->TestEqual(TEXT("Gain-limited export must report zero skipped variations."), State->Result.SkippedCount, 0);
				State->Test->TestEqual(TEXT("Gain-limited terminal counts must conserve the literal requested total of one."),
					State->Result.SuccessfulCount + State->Result.FailedCount + State->Result.SkippedCount, 1);
				State->Test->TestFalse(TEXT("Gain-limited export must publish a non-empty completion path."), State->Result.ExportPath.IsEmpty());
				State->Test->TestEqual(TEXT("Gain-limited export must retain exactly one asset log."), State->LogsAtTerminal.Num(), 1);
				if (State->LogsAtTerminal.Num() == 1 && State->LogsAtTerminal[0].Variations.Num() == 1)
				{
					const FAudioExportLogEntry& Row = State->LogsAtTerminal[0].Variations[0];
					State->Test->TestEqual(TEXT("The gain-limited row must report Succeeded, Gain Limited."), Row.Status, EAudioExportRowStatus::SucceededGainLimited);
					State->Test->TestTrue(TEXT("The gain-limited row must retain requested gain."), Row.GainRequested > Row.GainApplied);
					State->Test->TestTrue(TEXT("The gain-limited row must retain applied gain."), Row.GainApplied > 0.0f);
					State->Test->TestTrue(TEXT("The gain-limited row must retain the gain-limited flag."), Row.bGainLimited);
				}
				State->Test->TestTrue(TEXT("Progress should expose the gain-limited result, not ordinary success."),
					State->ProgressEvents.ContainsByPredicate([](const FAudioExportProgressInfo& Progress)
					{
						return Progress.bHasResult && Progress.ResultMessage.Contains(TEXT("gain limited"));
					}));
			}

			TArray<FString> WavFiles;
			AudioExporterRuntimeTestUtils::FindWavFilesUnder(State->Result.ExportPath.IsEmpty() ? State->OutputRoot : State->Result.ExportPath, WavFiles);
			if (State->Test && State->Test->TestEqual(TEXT("Gain-limited export must publish exactly one WAV."), WavFiles.Num(), 1))
			{
				AudioExporterRuntimeTestUtils::ValidateWavOutput(*State->Test, WavFiles[0], TEXT("Gain-limited export"));
			}
			if (State->Test)
			{
				const FString ReportRoot = State->Result.ExportPath.IsEmpty() ? State->OutputRoot : State->Result.ExportPath;
				FString TextReport;
				State->Test->TestTrue(TEXT("The gain-limited TXT report should be written."),
					FFileHelper::LoadFileToString(TextReport, *(ReportRoot / TEXT("AudioExporterTextLog.txt"))));
				State->Test->TestTrue(TEXT("TXT must state the gain-limited status."), TextReport.Contains(TEXT("Succeeded, Gain Limited")));
				State->Test->TestTrue(TEXT("TXT must include requested gain."), TextReport.Contains(TEXT("Gain Requested")));
				State->Test->TestTrue(TEXT("TXT must include applied gain."), TextReport.Contains(TEXT("Gain Applied")));
				State->Test->TestFalse(TEXT("Lite must not write a CSV report beside the TXT report."),
					IFileManager::Get().FileExists(*(ReportRoot / TEXT("AudioExporterCSVLog.csv"))));
			}
			ReleaseGainLimitedPipelineResources(*State);
			return true;
		}

		const double NowSeconds = FPlatformTime::Seconds();
		if (!State->bCancellationRequested && NowSeconds - State->StartSeconds > TimeoutSeconds)
		{
			State->bCancellationRequested = true;
			State->CancellationDeadlineSeconds = NowSeconds + AudioExporterRuntimeTestUtils::RealPipelineCancellationDrainSeconds;
			if (State->Test)
			{
				State->Test->AddError(TEXT("Gain-limited export timed out; cancellation requested and the structured terminal plus writer drain is still required."));
			}
			if (State->Pipeline.IsValid() && State->Pipeline->IsExporting())
			{
				State->Pipeline->CancelExporting(false);
			}
		}
		else if (State->bCancellationRequested && NowSeconds > State->CancellationDeadlineSeconds)
		{
			if (State->Test)
			{
				State->Test->AddError(TEXT("Gain-limited export did not drain its terminal result and async writers before the bounded cancellation deadline; deferred cleanup retains the live state safely."));
			}
			RegisterGainLimitedDeferredCleanup(State);
			return true;
		}
		return false;
	}

	private:
	TSharedRef<FGainLimitedPipelineExportState> State;
	double TimeoutSeconds = 0.0;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterGainLimitedPipelineReportIsHonestTest,
	"AudioFactory.AudioExporter.Runtime.Reporting.GainLimitedPipelineReportIsHonest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterGainLimitedPipelineReportIsHonestTest::RunTest(const FString& Parameters)
{
	UWorld* World = AudioExporterRuntimeTestUtils::ResolveAutomationWorld();
	USoundWave* Wave = LoadObject<USoundWave>(nullptr, TEXT("/Game/Test_1kHz.Test_1kHz"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	TestNotNull(TEXT("The gain-limited report test needs an automation world."), World);
	TestNotNull(TEXT("The gain-limited report test needs the imported PCM fixture."), Wave);
	if (!World || !Wave)
	{
		return false;
	}

	const float OriginalVolume = Wave->Volume;
	// Above the shared silence floor, but quiet enough that reaching -1 dBFS requires more than the
	// +12 dB safety cap. This must exercise the limited branch, not the intentional silence skip.
	Wave->Volume = 0.005f;

	const TSharedRef<FAudioExportPipeline> Pipeline = MakeShared<FAudioExportPipeline>();
	Pipeline->Initialize(World);
	const TSharedRef<FGainLimitedPipelineExportState> State = MakeShared<FGainLimitedPipelineExportState>();
	State->Test = this;
	State->Pipeline = Pipeline;
	State->Wave = Wave;
	State->OriginalVolume = OriginalVolume;
	State->OutputRoot = AudioExporterRuntimeTestUtils::MakeOutputDirectory(TEXT("GainLimitedPipelineReport"));
	State->StartSeconds = FPlatformTime::Seconds();
	State->ProgressHandle = Pipeline->OnExportProgressUpdated.AddLambda([State](const FAudioExportProgressInfo& InProgress)
	{
		State->ProgressEvents.Add(InProgress);
	});
	State->FinishedHandle = Pipeline->OnExportFinished.AddLambda([State](const FString& InPath)
	{
		++State->LegacyTerminalEvents;
		State->FinishedPath = InPath;
		State->bFinished = true;
	});
	State->ResultHandle = Pipeline->OnExportResult.AddLambda([State](const FAudioExportResult& InResult)
	{
		++State->StructuredTerminalEvents;
		State->Result = InResult;
		State->LogsAtTerminal = State->Pipeline->GetExportLogsForTesting();
	});

	TArray<FAudioExportSoundEntry> Entries;
	Entries.Add(AudioExporterRuntimeTestUtils::MakeFixtureEntry(Wave, TEXT("GainLimitedWave"), TEXT("/Game/Test_1kHz")));
	FAudioExporterConfig Config;
	Config.OneShotVariations = 1;
	Config.LoopVariations = 1;
	Config.bNormalize = true;
	Config.bExportTextLog = true;
	const bool bStarted = Pipeline->StartExporting(Entries, Config, State->OutputRoot);
	TestTrue(TEXT("The gain-limited fixture should run through the real public realtime export entry point."), bStarted);
	if (!bStarted)
	{
		ReleaseGainLimitedPipelineResources(*State);
		return false;
	}

	// The imported fixture is a long one-shot. Give it its authored duration plus the renderer's
	// named one-shot safety allowance, while retaining the separate short cancellation drain below.
	const double AuthoredDurationSeconds = FMath::Max(0.0, static_cast<double>(Wave->Duration));
	const double InitialTimeoutSeconds = AuthoredDurationSeconds
		+ static_cast<double>(FAudioRendererRealtime::OneShotSafetyTimeoutSeconds);
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForGainLimitedPipelineExport(State, InitialTimeoutSeconds));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterCueAndMetaSoundDispatchPolicyTest,
	"AudioFactory.AudioExporter.Runtime.Dispatch.CueAndMetaSoundDispatchPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterCueAndMetaSoundDispatchPolicyTest::RunTest(const FString& Parameters)
{
	UWorld* World = AudioExporterRuntimeTestUtils::ResolveAutomationWorld();
	if (!World)
	{
		AddError(TEXT("Dispatch-policy test is unavailable: no editor world was available."));
		return false;
	}

	const TArray<USoundBase*> Sources = {
		NewObject<USoundCue>(GetTransientPackage(), TEXT("AudioExporterRenderedCueSource")),
		NewObject<UMetaSoundSource>(GetTransientPackage(), TEXT("AudioExporterRenderedMetaSoundSource"))};
	for (int32 SourceIndex = 0; SourceIndex < Sources.Num(); ++SourceIndex)
	{
		USoundBase* Source = Sources[SourceIndex];
		TestNotNull(TEXT("The dispatch-policy test should create both source asset types."), Source);
		if (!Source)
		{
			continue;
		}

		Source->AddToRoot();
		const TSharedRef<AudioExporterRuntimeTestUtils::FDispatchPolicyProbePipeline> Pipeline = MakeShared<AudioExporterRuntimeTestUtils::FDispatchPolicyProbePipeline>();
		Pipeline->Initialize(World);
		const TSharedRef<AudioExporterRuntimeTestUtils::FDispatchPolicyProbeDrainState> State = MakeShared<AudioExporterRuntimeTestUtils::FDispatchPolicyProbeDrainState>();
		State->Test = this;
		State->Pipeline = Pipeline;
		State->Source = Source;
		State->DeadlineSeconds = FPlatformTime::Seconds() + AudioExporterRuntimeTestUtils::RealPipelineCancellationDrainSeconds;
		Pipeline->OnExportFinished.AddLambda([State](const FString&)
		{
			++State->TerminalEvents;
		});
		Pipeline->OnExportResult.AddLambda([State](const FAudioExportResult&)
		{
			++State->StructuredTerminalEvents;
		});

		FAudioExportSoundEntry Entry;
		Entry.Sound = Source;
		Entry.SoundName = SourceIndex == 0 ? TEXT("AudioExporterDispatchPolicyCueSource") : TEXT("AudioExporterDispatchPolicyMetaSoundSource");
		Entry.SourceAssetPath = SourceIndex == 0 ? TEXT("/Temp/AudioExporterDispatchPolicyCueSource") : TEXT("/Temp/AudioExporterDispatchPolicyMetaSoundSource");
		Entry.RelativePath = TEXT("DispatchPolicy");
		TArray<FAudioExportSoundEntry> Entries;
		Entries.Add(Entry);

		FAudioExporterConfig Config;
		Config.OneShotVariations = 1;
		Config.LoopVariations = 1;
		Config.MaxLengthSeconds = 5;
		Config.bNormalize = false;
		Config.bMonitor = false;
		Config.bExportTextLog = false;

		const bool bStarted = Pipeline->StartExporting(Entries, Config,
			AudioExporterRuntimeTestUtils::MakeOutputDirectory(SourceIndex == 0 ? TEXT("CueDispatchPolicy") : TEXT("MetaSoundDispatchPolicy")));
		TestTrue(TEXT("Cue and MetaSound sources should be accepted by the dispatch policy."), bStarted);
		if (!bStarted)
		{
			Source->RemoveFromRoot();
			continue;
		}
		State->ProbeRenderer = Pipeline->ProbeRenderer;
		Pipeline->CancelExporting(false);
		ADD_LATENT_AUTOMATION_COMMAND(AudioExporterRuntimeTestUtils::FWaitForDispatchPolicyProbeDrain(State));
	}

	return true;
}
