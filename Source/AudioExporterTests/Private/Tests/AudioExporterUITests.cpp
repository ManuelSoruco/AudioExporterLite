// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "Misc/AutomationTest.h"

#include "AudioExporter.h"
#include "ContentBrowserMenuContexts.h"
#include "Foundation/AudioFactoryPluginWindowTypes.h"
#include "Framework/Application/SlateApplication.h"
#include "Application/SlateWindowHelper.h"
#include "InputCoreTypes.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/IToolTip.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Dialogs/AudioFactoryPluginWindowAdapter.h"
#define private public
#include "Dialogs/AudioExporterProgressWindow.h"
#undef private
#include "Dialogs/AudioExporterWindow.h"
#include "AudioExporterBPLibrary.h"
#include "AudioExporterRuntimeSubsystem.h"
#include "AudioExportPipeline.h"
#include "Containers/Ticker.h"
#include "Utilities/AudioExporterManager.h"
#include "Utilities/AudioExporterSettings.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "ToolMenus.h"
#include "Modules/ModuleManager.h"

namespace AudioExporterUITestUtils
{
	static void CollectWidgetTree(const TSharedRef<SWidget>& Root, TArray<TSharedRef<SWidget>>& OutWidgets)
	{
		OutWidgets.Add(Root);
		FChildren* Children = Root->GetChildren();
		for (int32 Index = 0; Index < Children->Num(); ++Index)
		{
			CollectWidgetTree(Children->GetChildAt(Index), OutWidgets);
		}
	}

	static void TickSlateFrames(const int32 NumFrames = 2)
	{
		for (int32 Index = 0; Index < NumFrames; ++Index)
		{
			FSlateApplication::Get().Tick();
		}
	}

	static int32 CountOccurrences(const FString& Haystack, const FString& Needle)
	{
		int32 Count = 0;
		int32 SearchFrom = 0;
		while (!Needle.IsEmpty())
		{
			const int32 FoundAt = Haystack.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
			if (FoundAt == INDEX_NONE)
			{
				break;
			}
			++Count;
			SearchFrom = FoundAt + Needle.Len();
		}
		return Count;
	}

	static void DestroyWindow(const TSharedRef<SWindow>& Window)
	{
		FSlateApplication::Get().RequestDestroyWindow(Window);
		TickSlateFrames();
	}

	static void AssertWindowFitsPreferredWorkArea(FAutomationTestBase& Test, const TSharedRef<SWindow>& Window)
	{
		const FSlateRect WorkArea = FSlateApplication::Get().GetPreferredWorkArea();
		const FVector2f Position = Window->GetPositionInScreen();
		const FVector2f Size = Window->GetSizeInScreen();

		Test.TestTrue(TEXT("Settings window should be positioned within the preferred work area horizontally."),
			Position.X >= WorkArea.Left && Position.X + Size.X <= WorkArea.Right + 1.0f);
		Test.TestTrue(TEXT("Settings window should be positioned within the preferred work area vertically."),
			Position.Y >= WorkArea.Top && Position.Y + Size.Y <= WorkArea.Bottom + 1.0f);
	}

	static TSharedRef<SAudioExportProgressWindow> GetProgressWidget(const TSharedRef<SWindow>& Window)
	{
		return StaticCastSharedRef<SAudioExportProgressWindow>(Window->GetContent());
	}

	/** Simulates a real Escape keypress through Slate's own dispatch, not a direct method call --
	 *  proves the settings window's Esc handling is actually reachable via keyboard focus
	 *  (Window->SetWidgetToFocusOnActivate is what makes this reach the shared adapter's OnKeyDown
	 *  at all). Simulates a real Escape key press. */
	static void SimulateEscapeKeyPress(const TSharedRef<SWindow>& Window)
	{
		FSlateApplication::Get().SetKeyboardFocus(Window->GetContent());
		const FKeyEvent EscapeEvent(EKeys::Escape, FModifierKeysState(), 0, false, 0, 0);
		FSlateApplication::Get().ProcessKeyDownEvent(EscapeEvent);
		TickSlateFrames();
	}

	/** Simulates a real Enter keypress through Slate's own dispatch, not a direct callback call. */
	static void SimulateEnterKeyPress(const TSharedRef<SWindow>& Window)
	{
		FSlateApplication::Get().SetKeyboardFocus(Window->GetContent());
		const FKeyEvent EnterEvent(EKeys::Enter, FModifierKeysState(), 0, false, 0, 0);
		FSlateApplication::Get().ProcessKeyDownEvent(EnterEvent);
		TickSlateFrames();
	}

	static FAssetData MakeTransientAssetData(UObject* Object)
	{
		UPackage* Package = Object->GetOutermost();
		return FAssetData(
			Package->GetFName(),
			FName(*FPackageName::GetLongPackagePath(Package->GetName())),
			Object->GetFName(),
			Object->GetClass()->GetClassPathName());
	}

	static void CollectRenderedText(const TSharedRef<SWidget>& Widget, TArray<FString>& OutText)
	{
		if (Widget->GetTypeAsString() == TEXT("STextBlock"))
		{
			OutText.Add(StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString());
		}

		FChildren* Children = Widget->GetChildren();
		for (int32 Index = 0; Index < Children->Num(); ++Index)
		{
			CollectRenderedText(Children->GetChildAt(Index), OutText);
		}
	}

	// SButton::ExecuteOnClick() is protected; SimulateClick() is the public entry point and
	// returns void, so a click is proven by the state it changes rather than by a returned FReply.
	static bool ClickButtonWithText(
		const TSharedRef<SWidget>& Widget,
		const FString& ButtonText)
	{
		if (Widget->GetTypeAsString() == TEXT("SButton"))
		{
			TArray<FString> ButtonTextBlocks;
			CollectRenderedText(Widget, ButtonTextBlocks);
			if (ButtonTextBlocks.Contains(ButtonText))
			{
				StaticCastSharedRef<SButton>(Widget)->SimulateClick();
				return true;
			}
		}

		FChildren* Children = Widget->GetChildren();
		for (int32 Index = 0; Index < Children->Num(); ++Index)
		{
			if (ClickButtonWithText(Children->GetChildAt(Index), ButtonText))
			{
				return true;
			}
		}

		return false;
	}

	static FString RenderedText(const TSharedRef<SWidget>& Widget)
	{
		TArray<FString> TextBlocks;
		CollectRenderedText(Widget, TextBlocks);
		return FString::Join(TextBlocks, TEXT("\n"));
	}

	static int32 CountEnabledResetToDefaultButtons(const TSharedRef<SWidget>& Widget)
	{
		int32 Count = 0;
		if (Widget->GetTypeAsString() == TEXT("SButton")
			&& Widget->IsEnabled()
			&& RenderedText(Widget).Contains(TEXT("Reset to Default")))
		{
			++Count;
		}

		FChildren* Children = Widget->GetChildren();
		for (int32 Index = 0; Index < Children->Num(); ++Index)
		{
			Count += CountEnabledResetToDefaultButtons(Children->GetChildAt(Index));
		}
		return Count;
	}

	static bool ClickUniqueEnabledResetToDefaultButton(const TSharedRef<SWidget>& Widget, int32& OutEnabledCount)
	{
		TArray<TSharedRef<SWidget>> MatchingButtons;
		TArray<TSharedRef<SWidget>> Widgets;
		CollectWidgetTree(Widget, Widgets);
		for (const TSharedRef<SWidget>& Candidate : Widgets)
		{
			if (Candidate->GetTypeAsString() == TEXT("SButton")
				&& RenderedText(Candidate).Contains(TEXT("Reset to Default")))
			{
				MatchingButtons.Add(Candidate);
			}
		}

		OutEnabledCount = 0;
		TSharedPtr<SButton> EnabledButton;
		for (const TSharedRef<SWidget>& Candidate : MatchingButtons)
		{
			if (Candidate->IsEnabled())
			{
				++OutEnabledCount;
				EnabledButton = StaticCastSharedRef<SButton>(Candidate);
			}
		}

		if (OutEnabledCount != 1 || !EnabledButton.IsValid())
		{
			return false;
		}

		EnabledButton->SimulateClick();
		return true;
	}

	struct FPrimaryActionState
	{
		bool bIsEnabled = false;
		bool bHasToolTip = false;
		FString ToolTipText;
	};

	static TOptional<FPrimaryActionState> GetPrimaryActionState(const TSharedRef<SWidget>& Root)
	{
		TArray<TSharedRef<SWidget>> Widgets;
		CollectWidgetTree(Root, Widgets);
		for (const TSharedRef<SWidget>& Widget : Widgets)
		{
			if (!Widget->GetTypeAsString().Contains(TEXT("SAudioFactoryPrimaryActionButton")))
			{
				continue;
			}

			TArray<TSharedRef<SWidget>> ButtonWidgets;
			CollectWidgetTree(Widget, ButtonWidgets);
			for (const TSharedRef<SWidget>& ButtonWidget : ButtonWidgets)
			{
				if (!ButtonWidget->GetTypeAsString().Contains(TEXT("SButton")))
				{
					continue;
				}

				FPrimaryActionState State;
				State.bIsEnabled = ButtonWidget->IsEnabled();
				if (const TSharedPtr<IToolTip> ToolTip = ButtonWidget->GetToolTip())
				{
					State.bHasToolTip = true;
					State.ToolTipText = RenderedText(ToolTip->GetContentWidget());
				}
				return State;
			}
		}
		return TOptional<FPrimaryActionState>();
	}

	static bool FindStatusStripText(const TSharedRef<SWidget>& Widget, FString& OutText)
	{
		if (Widget->GetTypeAsString() == TEXT("SAudioFactoryStatusStrip"))
		{
			OutText = RenderedText(Widget);
			return true;
		}

		FChildren* Children = Widget->GetChildren();
		for (int32 Index = 0; Index < Children->Num(); ++Index)
		{
			if (FindStatusStripText(Children->GetChildAt(Index), OutText))
			{
				return true;
			}
		}
		return false;
	}

	/** SWindow::IsVisible() is a native-platform cache only Show()/Hide() ever mutate; a real
	 *  Destroy() never touches it, so it stays stuck true after a real close. Removal from
	 *  FSlateApplication's SlateWindows registry (GetInteractiveTopLevelWindows()) is the
	 *  synchronous, reliable signal that production code actually tore the window down. Mirrors
	 *  a live Slate registration check. */
	static bool IsWindowStillRegisteredWithSlate(const TSharedRef<SWindow>& Window)
	{
		return FSlateWindowHelper::ContainsWindow(
			FSlateApplication::Get().GetInteractiveTopLevelWindows(), Window);
	}

	static TSharedRef<SWindow> OpenNullCallbackProgressWindow()
	{
		return SAudioExportProgressWindow::OpenProgressWindow(
			TArray<FAssetData>(),
			FOnAudioExporterMonitorChanged::CreateLambda([](bool) {}),
			FOnAudioExporterCancelRequested::CreateLambda([]() {}),
			FOnAudioExporterRequestRecordingSubmix::CreateLambda([]() -> USoundSubmix*
			{
				return nullptr;
			}),
			FOnAudioExporterRequestWorld::CreateLambda([]() -> UWorld*
			{
				return nullptr;
			}),
			FOnAudioExporterIsExportActive::CreateLambda([]()
			{
				return true;
			}));
	}

	// Drives the progress widget to a terminal state with the given exported/failed counts on
	// a valid (non-empty) path, then returns the resulting green-success verdict.
	static void DriveTerminalOutcome(
		const TSharedRef<SAudioExportProgressWindow>& Widget,
		int32 ExportedFiles,
		int32 FailedFiles)
	{
		FAudioExportProgressInfo Result;
		Result.bHasResult = true;
		Result.bSucceeded = FailedFiles == 0;
		Result.ExportedFiles = ExportedFiles;
		Result.FailedFiles = FailedFiles;
		Widget->UpdateProgressUI(Result);
		Widget->OnExportFinishedProgress(FPaths::ProjectSavedDir());
	}
}

class FAudioExporterCaptureTerminalStatesTestBase : public FAutomationTestBase
{
public:
	FAudioExporterCaptureTerminalStatesTestBase(const FString& InName, const bool bInComplexTask)
		: FAutomationTestBase(InName, bInComplexTask)
	{
	}

	virtual bool CanRunInEnvironment(const FString& TestParameters, FString* OutReason, bool* OutWarn) const override
	{
		(void)TestParameters;
#if !WITH_AUDIOFACTORY_DEV_CAPTURE
		if (OutReason)
		{
			*OutReason = TEXT("Capture-only terminal-state assertions require WITH_AUDIOFACTORY_DEV_CAPTURE.");
		}
		if (OutWarn)
		{
			*OutWarn = false;
		}
		return false;
#else
		(void)OutReason;
		(void)OutWarn;
		return true;
#endif
	}
};

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FAudioExporterCaptureTerminalStatesTest,
	FAudioExporterCaptureTerminalStatesTestBase,
	"AudioFactory.AudioExporter.UI.CaptureTerminalStates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterCaptureTerminalStatesTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Capture terminal-state test is unavailable: SlateApplication was not initialized."));
		return false;
	}

#if !WITH_AUDIOFACTORY_DEV_CAPTURE
	AddInfo(TEXT("SKIPPED: Capture-only terminal-state assertions require WITH_AUDIOFACTORY_DEV_CAPTURE."));
	return false;
#else
	const TPair<int32, int32> Outcomes[] = {
		TPair<int32, int32>(3, 0),
		TPair<int32, int32>(2, 1),
	};
	for (const TPair<int32, int32>& Outcome : Outcomes)
	{
		const TSharedRef<SWindow> Window = AudioExporterUITestUtils::OpenNullCallbackProgressWindow();
		FSlateApplication::Get().AddWindow(Window);
		const TSharedRef<SAudioExportProgressWindow> Widget = AudioExporterUITestUtils::GetProgressWidget(Window);
		Widget->SetTerminalStateForCapture(Outcome.Key, Outcome.Value, FPaths::ProjectSavedDir());
		AudioExporterUITestUtils::TickSlateFrames();
		TestTrue(TEXT("Capture terminal state should remain visible for screenshotting"), Window->IsVisible());
#if WITH_DEV_AUTOMATION_TESTS
		TestTrue(TEXT("Capture terminal state should be terminal"), Widget->HasReachedTerminalStateForTesting());
		TestEqual(TEXT("Capture terminal exported count"), Widget->GetTerminalExportedCountForTesting(), Outcome.Key);
		TestEqual(TEXT("Capture terminal failed count"), Widget->GetTerminalFailedCountForTesting(), Outcome.Value);
#endif
		AudioExporterUITestUtils::DestroyWindow(Window);
	}
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterSettingsWindowSmokeTest,
	"AudioFactory.AudioExporter.UI.SettingsWindowSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterSettingsWindowSmokeTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Settings window smoke test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	const TSharedRef<SWindow> Window = SAudioExporterWindow::OpenSettingsWindow(
		TArray<FAssetData>(),
		FOnAudioExporterRequested::CreateLambda([](const TArray<FAssetData>& Assets)
		{
			(void)Assets;
		}),
		FOnAudioExporterRequestExportDirectory::CreateLambda([](const FString& /*StartingPath*/)
		{
			return FString();
		}));
	FSlateApplication::Get().AddWindow(Window);
	AudioExporterUITestUtils::TickSlateFrames();
	// "<Product> — <Task>" grammar; there is no "AudioFactory /" prefix and no bare "Audio Exporter"
	// title.
	TestEqual(TEXT("Audio Exporter settings window should use the expected title."), Window->GetTitle().ToString(), FString(TEXT("Audio Exporter — Settings")));
	TestTrue(TEXT("Audio Exporter settings window should be user-resizable."), Window->IsUserSized());
	TestTrue(TEXT("Audio Exporter settings window should become visible after being added."), Window->IsVisible());
	AudioExporterUITestUtils::AssertWindowFitsPreferredWorkArea(*this, Window);
	AudioExporterUITestUtils::DestroyWindow(Window);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterSettingsWindowRetargetsInPlaceTest,
	"AudioFactory.AudioExporter.UI.SettingsWindow.RetargetsInPlace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterSettingsWindowRetargetsInPlaceTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Settings-window singleton test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	UPackage* FirstPackage = NewObject<UPackage>(nullptr, TEXT("/Temp/AudioExporterUITest/RetargetFirst"), RF_Public);
	USoundWave* FirstWave = NewObject<USoundWave>(FirstPackage, TEXT("RetargetFirst"), RF_Public);
	UPackage* SecondPackage = NewObject<UPackage>(nullptr, TEXT("/Temp/AudioExporterUITest/RetargetSecond"), RF_Public);
	USoundCue* SecondCue = NewObject<USoundCue>(SecondPackage, TEXT("RetargetSecond"), RF_Public);
	TestNotNull(TEXT("The singleton test should construct the first real SoundWave asset."), FirstWave);
	TestNotNull(TEXT("The singleton test should construct the second real SoundCue asset."), SecondCue);
	if (!FirstWave || !SecondCue)
	{
		return false;
	}

	const FOnAudioExporterRequested NoOpExport = FOnAudioExporterRequested::CreateLambda([](const TArray<FAssetData>&) {});
	const FOnAudioExporterRequestExportDirectory NoOpBrowse = FOnAudioExporterRequestExportDirectory::CreateLambda([](const FString&)
	{
		return FString();
	});
	const TSharedRef<SWindow> FirstWindow = SAudioExporterWindow::OpenSettingsWindow(
		TArray<FAssetData>{ AudioExporterUITestUtils::MakeTransientAssetData(FirstWave) },
		NoOpExport,
		NoOpBrowse);
	FSlateApplication::Get().AddWindow(FirstWindow);
	AudioExporterUITestUtils::TickSlateFrames();
	const FVector2f FirstPosition = FirstWindow->GetPositionInScreen();
	const FVector2f FirstSize = FirstWindow->GetSizeInScreen();
	const FString FirstRenderedText = AudioExporterUITestUtils::RenderedText(FirstWindow->GetContent());

	const TSharedRef<SWindow> SecondWindow = SAudioExporterWindow::OpenSettingsWindow(
		TArray<FAssetData>{ AudioExporterUITestUtils::MakeTransientAssetData(SecondCue) },
		NoOpExport,
		NoOpBrowse);
	AudioExporterUITestUtils::TickSlateFrames();
	const FString SecondRenderedText = AudioExporterUITestUtils::RenderedText(SecondWindow->GetContent());

	TestTrue(TEXT("Two Audio Exporter invocations must return one live settings window."), &FirstWindow.Get() == &SecondWindow.Get());
	TestTrue(TEXT("The second invocation must replace the first invocation's bound asset summary."), FirstRenderedText != SecondRenderedText);
	TestTrue(TEXT("The second invocation's SoundCue selection must be visible in the same settings window."), SecondRenderedText.Contains(TEXT("SoundCues")));
	TestTrue(TEXT("Retargeting must preserve the existing window position."), FirstPosition.Equals(SecondWindow->GetPositionInScreen(), 0.01f));
	TestTrue(TEXT("Retargeting must preserve the existing window size."), FirstSize.Equals(SecondWindow->GetSizeInScreen(), 0.01f));

	AudioExporterUITestUtils::DestroyWindow(FirstWindow);
	return true;
}

// The shared SAudioFactoryPluginSettingsFrameAdapter implements SupportsKeyboardFocus/
// OnKeyDown itself (forwarding to the inner SAudioFactoryPluginWindowFrame), so
// Window->SetWidgetToFocusOnActivate(SettingsContent) at the settings window's construction site
// actually gives Escape a focus path to travel along. Mirrors
// the matching Escape-closes test in the sibling products so the same regression cannot return silently here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterSettingsWindowEscClosesTest,
	"AudioFactory.AudioExporter.UI.SettingsWindow.EscCloses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterSettingsWindowEscClosesTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Settings-window Esc test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	const TSharedRef<SWindow> Window = SAudioExporterWindow::OpenSettingsWindow(
		TArray<FAssetData>(),
		FOnAudioExporterRequested::CreateLambda([](const TArray<FAssetData>&) {}),
		FOnAudioExporterRequestExportDirectory::CreateLambda([](const FString& /*StartingPath*/)
		{
			return FString();
		}));
	FSlateApplication::Get().AddWindow(Window);
	AudioExporterUITestUtils::TickSlateFrames();
	TestTrue(TEXT("Settings window should be visible before Esc."), Window->IsVisible());

	AudioExporterUITestUtils::SimulateEscapeKeyPress(Window);

	// IsVisible() is a native-platform cache Destroy() never
	// clears, so log both signals side by side to make the discrepancy visible in the automation
	// log rather than asserting on the unreliable one.
	AddInfo(FString::Printf(TEXT("Post-Esc settings window state: IsVisible=%s StillRegisteredWithSlate=%s"),
		Window->IsVisible() ? TEXT("true") : TEXT("false"),
		AudioExporterUITestUtils::IsWindowStillRegisteredWithSlate(Window) ? TEXT("true") : TEXT("false")));

	TestFalse(TEXT("Esc should close the Audio Exporter settings window (Window->SetWidgetToFocusOnActivate makes the shared adapter's existing OnKeyDown reachable)."),
		AudioExporterUITestUtils::IsWindowStillRegisteredWithSlate(Window));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterSettingsWindowEnterRunsEnabledPrimaryTest,
	"AudioFactory.AudioExporter.UI.SettingsWindowEnterRunsEnabledPrimary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterSettingsWindowEnterRunsEnabledPrimaryTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Settings-window Enter test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	UPackage* WavePackage = NewObject<UPackage>(nullptr, TEXT("/Temp/AudioExporterUITest/EnterEnabledWave"), RF_Public);
	USoundWave* Wave = NewObject<USoundWave>(WavePackage, TEXT("EnterEnabledWave"), RF_Public);
	TestNotNull(TEXT("The Enter test should construct a real SoundWave asset."), Wave);
	if (!Wave)
	{
		return false;
	}
	Wave->AddToRoot();

	const FAssetData WaveAsset = AudioExporterUITestUtils::MakeTransientAssetData(Wave);
	int32 ExportRequests = 0;
	TArray<FAssetData> RequestedAssets;
	const TSharedRef<SWindow> Window = SAudioExporterWindow::OpenSettingsWindow(
		TArray<FAssetData>{ WaveAsset },
		FOnAudioExporterRequested::CreateLambda([&ExportRequests, &RequestedAssets](const TArray<FAssetData>& Assets)
		{
			++ExportRequests;
			RequestedAssets = Assets;
		}),
		FOnAudioExporterRequestExportDirectory::CreateLambda([](const FString&)
		{
			return FString();
		}));
	FSlateApplication::Get().AddWindow(Window);
	AudioExporterUITestUtils::TickSlateFrames();

	AudioExporterUITestUtils::SimulateEnterKeyPress(Window);

	TestEqual(TEXT("Enter should invoke the real primary action exactly once when it is enabled."), ExportRequests, 1);
	TestEqual(TEXT("The enabled primary action should receive the selected real asset."), RequestedAssets.Num(), 1);
	if (RequestedAssets.Num() == 1)
	{
		TestEqual(TEXT("The enabled primary action should receive the original SoundWave asset name."),
			RequestedAssets[0].AssetName, WaveAsset.AssetName);
	}
	TestFalse(TEXT("The real primary action should close the settings window after a successful request."),
		AudioExporterUITestUtils::IsWindowStillRegisteredWithSlate(Window));

	if (AudioExporterUITestUtils::IsWindowStillRegisteredWithSlate(Window))
	{
		AudioExporterUITestUtils::DestroyWindow(Window);
	}
	Wave->RemoveFromRoot();
	return true;
}

// Regression coverage. The settings window's OnPrimaryAction invokes the export-request
// delegate and then destroys the window UNCONDITIONALLY (SAudioExporterWindow::OpenSettingsWindow's
// Spec.OnPrimaryAction), and FOnAudioExporterRequested returns void, so a REJECTED start has no way
// to keep the window open: the user's staged settings vanish with no acceptance signal anywhere in
// the UI. This test rejects a start through the window's REAL confirm path -- a seeded active export
// session makes the real subsystem start API refuse the request synchronously -- and asserts both
// halves of the contract: the rejection surfaces through the observable structured-result delegate,
// AND the window remains open. Today the second half fails because the window tears itself down even
// on the rejected request.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterSettingsWindowRejectedStartKeepsWindowOpenTest,
	"AudioFactory.AudioExporter.UI.SettingsWindow.RejectedStartKeepsWindowOpen",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterSettingsWindowRejectedStartKeepsWindowOpenTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Settings-window rejected-start test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	UAudioExporterRuntimeSubsystem* RuntimeSubsystem = UAudioExporterBPLibrary::GetRuntimeSubsystem();
	TestNotNull(TEXT("The rejected-start test should resolve the real AudioExporter runtime subsystem."), RuntimeSubsystem);
	if (!RuntimeSubsystem)
	{
		return false;
	}
	if (RuntimeSubsystem->IsExporting())
	{
		RuntimeSubsystem->CancelExport(false);
	}

	UWorld* World = GWorld;
	TestNotNull(TEXT("The rejected-start test should have a live world for the real subsystem session."), World);
	if (!World)
	{
		return false;
	}

	// An active lifecycle session is what makes the production start path reject deterministically
	// and synchronously (UAudioExporterRuntimeSubsystem::StartExport refuses concurrent requests
	// before anything else), without touching any audio device.
	const FString ActiveSessionPath = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("Automation"),
		TEXT("UI"),
		FString::Printf(TEXT("SettingsWindowRejectedStart_Active_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	IFileManager::Get().MakeDirectory(*ActiveSessionPath, true);
	if (!RuntimeSubsystem->BeginLifecycleTestSession(World, ActiveSessionPath))
	{
		AddError(TEXT("The rejected-start test could not seed its required active lifecycle session."));
		return false;
	}

	int32 StructuredRejectEvents = 0;
	FAudioExportResult CapturedResult;
	const FDelegateHandle StructuredResultHandle = RuntimeSubsystem->OnExportCompleteResultNative.AddLambda(
		[&StructuredRejectEvents, &CapturedResult](const FAudioExportResult& InResult)
		{
			++StructuredRejectEvents;
			CapturedResult = InResult;
		});

	UPackage* WavePackage = NewObject<UPackage>(nullptr, TEXT("/Temp/AudioExporterUITest/RejectedStartWave"), RF_Public);
	USoundWave* Wave = NewObject<USoundWave>(WavePackage, TEXT("RejectedStartWave"), RF_Public);
	TestNotNull(TEXT("The rejected-start test should construct a real SoundWave asset."), Wave);
	if (!Wave)
	{
		RuntimeSubsystem->OnExportCompleteResultNative.Remove(StructuredResultHandle);
		RuntimeSubsystem->CancelExport(false);
		return false;
	}
	Wave->AddToRoot();

	int32 ExportRequests = 0;
	const TSharedRef<SWindow> Window = SAudioExporterWindow::OpenSettingsWindow(
		TArray<FAssetData>{ AudioExporterUITestUtils::MakeTransientAssetData(Wave) },
		FOnAudioExporterRequested::CreateLambda([World, RuntimeSubsystem, &ExportRequests](const TArray<FAssetData>& Assets)
		{
			(void)Assets;

			// The same real start API production's manager drives; the seeded active session makes
			// this exact call refuse the request synchronously.
			++ExportRequests;
			RuntimeSubsystem->StartExport(World, TArray<FAudioExportSoundEntry>(), FAudioExporterConfig(), FString());
		}),
		FOnAudioExporterRequestExportDirectory::CreateLambda([](const FString&)
		{
			return FString();
		}));
	FSlateApplication::Get().AddWindow(Window);
	AudioExporterUITestUtils::TickSlateFrames();
	const FString BeforeRejectedRenderedText = AudioExporterUITestUtils::RenderedText(Window->GetContent());
	const TOptional<AudioExporterUITestUtils::FPrimaryActionState> BeforeRejectedPrimaryState =
		AudioExporterUITestUtils::GetPrimaryActionState(Window->GetContent());
	TestTrue(TEXT("The real Export button must expose its state before refusal."), BeforeRejectedPrimaryState.IsSet());

	AddExpectedError(TEXT("An export session is already active"), EAutomationExpectedErrorFlags::Contains, 1);
	AudioExporterUITestUtils::SimulateEnterKeyPress(Window);
	const FString AfterRejectedRenderedText = AudioExporterUITestUtils::RenderedText(Window->GetContent());
	const TOptional<AudioExporterUITestUtils::FPrimaryActionState> AfterRejectedPrimaryState =
		AudioExporterUITestUtils::GetPrimaryActionState(Window->GetContent());

	TestEqual(TEXT("Enter should invoke the real primary action exactly once."), ExportRequests, 1);
	TestEqual(TEXT("The rejected start should settle exactly one structured result."), StructuredRejectEvents, 1);
	TestEqual(TEXT("The settled result outcome should be Rejected."), CapturedResult.Outcome, EAudioExportOutcome::Rejected);
	TestFalse(TEXT("The settled result must report accepted-start false."), CapturedResult.bAcceptedStart);

	// THE KEY ASSERT: today OnPrimaryAction calls RequestDestroyWindow()
	// unconditionally after Execute(), so even a rejected request closes the window.
	TestTrue(TEXT("A rejected start must keep the settings window open so the user can correct or abandon the request."),
		AudioExporterUITestUtils::IsWindowStillRegisteredWithSlate(Window));
	TestEqual(TEXT("A rejected start must leave rendered text unchanged."),
		AfterRejectedRenderedText, BeforeRejectedRenderedText);
	TestTrue(TEXT("The real Export button must still expose its state after refusal."), AfterRejectedPrimaryState.IsSet());
	if (BeforeRejectedPrimaryState.IsSet() && AfterRejectedPrimaryState.IsSet())
	{
		TestEqual(TEXT("A rejected start must not change the primary button enabled state."),
			AfterRejectedPrimaryState->bIsEnabled, BeforeRejectedPrimaryState->bIsEnabled);
		TestEqual(TEXT("A rejected start must not change primary tooltip presence."),
			AfterRejectedPrimaryState->bHasToolTip, BeforeRejectedPrimaryState->bHasToolTip);
		TestEqual(TEXT("A rejected start must not change the primary button tooltip."),
			AfterRejectedPrimaryState->ToolTipText, BeforeRejectedPrimaryState->ToolTipText);
	}
	FString StatusStripText;
	const bool bFoundStatusStrip = AudioExporterUITestUtils::FindStatusStripText(Window->GetContent(), StatusStripText);
	TestFalse(TEXT("A rejected start must not create a settings status strip."), bFoundStatusStrip);

	RuntimeSubsystem->OnExportCompleteResultNative.Remove(StructuredResultHandle);
	RuntimeSubsystem->CancelExport(false);
	if (AudioExporterUITestUtils::IsWindowStillRegisteredWithSlate(Window))
	{
		AudioExporterUITestUtils::DestroyWindow(Window);
	}
	Wave->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterSettingsWindowTimestampPlaceholderTest,
	"AudioFactory.AudioExporter.UI.SettingsWindow.TimestampPlaceholder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterSettingsWindowTimestampPlaceholderTest::RunTest(const FString& Parameters)
{
	UAudioExporterSettings* Settings = GetMutableDefault<UAudioExporterSettings>();
	TestNotNull(TEXT("The timestamp placeholder test should resolve the real settings object."), Settings);
	if (!Settings)
	{
		return false;
	}

	const FString OriginalDirectory = Settings->ExportDirectory;
	Settings->ExportDirectory = TEXT("D:/ChosenExportFolder");
	TestEqual(TEXT("A chosen base folder should display exactly one timestamp placeholder."),
		SAudioExporterWindow::GetDisplayedExportPathForTesting(Settings).ToString(),
		FString(TEXT("D:/ChosenExportFolder/<timestamp>")));

	Settings->ExportDirectory = TEXT("D:/ChosenExportFolder/<timestamp>");
	TestEqual(TEXT("An existing timestamp placeholder should not be duplicated."),
		SAudioExporterWindow::GetDisplayedExportPathForTesting(Settings).ToString(),
		FString(TEXT("D:/ChosenExportFolder/<timestamp>")));
	TestEqual(TEXT("The folder browser should start at the real base folder, not the placeholder."),
		SAudioExporterWindow::GetExportBrowseStartingDirectoryForTesting(Settings),
		FString(TEXT("D:/ChosenExportFolder")));

	Settings->ExportDirectory = OriginalDirectory;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterSettingsWindowResetDefaultsRestoresThreeVariationsTest,
	"AudioFactory.AudioExporter.UI.SettingsWindow.PerPropertyReset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterSettingsWindowResetDefaultsRestoresThreeVariationsTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Settings-window reset test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	UAudioExporterSettings* Settings = GetMutableDefault<UAudioExporterSettings>();
	TestNotNull(TEXT("The reset test should resolve the real Audio Exporter settings object."), Settings);
	if (!Settings)
	{
		return false;
	}

	Settings->LoadConfig();
	const int32 OriginalOneShotVariations = Settings->OneShotVariations;
	Settings->OneShotVariations = 1;
	const int32 OriginalLoopVariations = Settings->LoopVariations;
	Settings->LoopVariations = 2;
	Settings->SaveConfig();

	UPackage* WavePackage = NewObject<UPackage>(nullptr, TEXT("/Temp/AudioExporterUITest/ResetDefaultsWave"), RF_Public);
	USoundWave* Wave = NewObject<USoundWave>(WavePackage, TEXT("ResetDefaultsWave"), RF_Public);
	TestNotNull(TEXT("The reset test should construct a real SoundWave asset."), Wave);
	if (!Wave)
	{
		Settings->OneShotVariations = OriginalOneShotVariations;
		Settings->LoopVariations = OriginalLoopVariations;
		Settings->SaveConfig();
		return false;
	}
	Wave->AddToRoot();

	const TSharedRef<SWindow> Window = SAudioExporterWindow::OpenSettingsWindow(
		TArray<FAssetData>{ AudioExporterUITestUtils::MakeTransientAssetData(Wave) },
		FOnAudioExporterRequested::CreateLambda([](const TArray<FAssetData>&) {}),
		FOnAudioExporterRequestExportDirectory::CreateLambda([](const FString&)
		{
			return FString();
		}));
	FSlateApplication::Get().AddWindow(Window);
	AudioExporterUITestUtils::TickSlateFrames();

	// This is the exact frame returned by the real settings window. Clicking its public footer
	// action invokes the same reset handler bound to the production UI.
	const bool bFoundResetButton = AudioExporterUITestUtils::ClickButtonWithText(
		Window->GetContent(), TEXT("Reset Defaults"));
	TestFalse(TEXT("The public Audio Exporter settings window must not expose a Reset Defaults footer action."), bFoundResetButton);
	TestTrue(TEXT("The public Audio Exporter settings window must expose the Logs footer action."),
		AudioExporterUITestUtils::RenderedText(Window->GetContent()).Contains(TEXT("Logs")));
	TestTrue(TEXT("The native per-property reset path should restore only OneShotVariations."),
		AudioExporter::FAudioFactoryPluginWindowAdapter::ResetPropertyToDefaultForAutomation(
			Window->GetContent(), GET_MEMBER_NAME_CHECKED(UAudioExporterSettings, OneShotVariations)));
	AudioExporterUITestUtils::SimulateEnterKeyPress(Window);

	TestEqual(TEXT("Per-property reset must commit the literal OneShotVariations value 3."),
		Settings->OneShotVariations, 3);
	TestEqual(TEXT("Per-property reset must preserve the unrelated LoopVariations value."),
		Settings->LoopVariations, 2);

	if (AudioExporterUITestUtils::IsWindowStillRegisteredWithSlate(Window))
	{
		AudioExporterUITestUtils::DestroyWindow(Window);
	}
	Wave->RemoveFromRoot();
	Settings->OneShotVariations = OriginalOneShotVariations;
	Settings->LoopVariations = OriginalLoopVariations;
	Settings->SaveConfig();
	return true;
}

// KILL CONDITION: a regression in the per-property reset-arrow visibility wiring (an inverted or
// short-circuited IsDifferentFromProductDefault predicate, or a product-defaults snapshot that
// fails to build) makes at least one assertion below fail: the arrow predicate must report
// VISIBLE for a staged value differing from the plugin default and HIDDEN once the value is
// restored to that default. Rendered-row scope: this test also finds the engine-owned reset button
// by its hidden "Reset to Default" label and drives the one enabled button through Slate, so
// deleting the OverrideResetToDefault registration leaves no enabled button and fails here while
// the read-only seam alone would stay green because it evaluates the predicate body directly,
// bypassing the registration.
//
// PATH DRIVEN: the modification goes through this suite's established entry point (direct
// mutation of the real persisted settings object before window open, exactly as
// FAudioExporterSettingsWindowResetDefaultsRestoresThreeVariationsTest does); the rendered reset
// click invokes the engine-owned row's registered FResetToDefaultHandler, and the real Enter key
// path applies that changed staged object back to the settings object; the two visibility reads
// evaluate the exact predicate body of the registered FIsResetToDefaultVisible lambda via the
// new read-only seam.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterSettingsWindowResetArrowVisibilityFollowsModificationTest,
	"AudioFactory.AudioExporter.UI.SettingsWindow.PerPropertyResetArrowVisibility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterSettingsWindowResetArrowVisibilityFollowsModificationTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Reset-arrow visibility test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	UAudioExporterSettings* Settings = GetMutableDefault<UAudioExporterSettings>();
	TestNotNull(TEXT("The reset-arrow visibility test should resolve the real Audio Exporter settings object."), Settings);
	if (!Settings)
	{
		return false;
	}

	Settings->LoadConfig();
	const FAudioExporterConfig OriginalConfig = Settings->ToConfig();
	const FString OriginalExportDirectory = Settings->ExportDirectory;
	Settings->ApplyFromConfig(FAudioExporterConfig());
	Settings->ExportDirectory.Empty();
	Settings->OneShotVariations = 1;
	Settings->SaveConfig();
	UPackage* WavePackage = NewObject<UPackage>(nullptr, TEXT("/Temp/AudioExporterUITest/ResetArrowVisibilityWave"), RF_Public);
	USoundWave* Wave = NewObject<USoundWave>(WavePackage, TEXT("ResetArrowVisibilityWave"), RF_Public);
	TestNotNull(TEXT("The reset-arrow visibility test should construct a real SoundWave asset."), Wave);
	if (!Wave)
	{
		Settings->ApplyFromConfig(OriginalConfig);
		Settings->ExportDirectory = OriginalExportDirectory;
		Settings->SaveConfig();
		return false;
	}
	Wave->AddToRoot();

	const TSharedRef<SWindow> Window = SAudioExporterWindow::OpenSettingsWindow(
		TArray<FAssetData>{ AudioExporterUITestUtils::MakeTransientAssetData(Wave) },
		FOnAudioExporterRequested::CreateLambda([](const TArray<FAssetData>&) {}),
		FOnAudioExporterRequestExportDirectory::CreateLambda([](const FString&)
		{
			return FString();
		}));
	FSlateApplication::Get().AddWindow(Window);
	IConsoleVariable* SlateSkipWidgetDrawing =
		IConsoleManager::Get().FindConsoleVariable(TEXT("Slate.SkipWidgetDrawingInHeadlessMode"));
	const int32 OriginalSlateSkipWidgetDrawing = SlateSkipWidgetDrawing
		? SlateSkipWidgetDrawing->GetInt()
		: 0;
	if (SlateSkipWidgetDrawing)
	{
		// -NullRHI skips Slate drawing by default, which leaves SDetailTree's virtualized rows
		// unrealized. Enable the normal draw/prepass path so this test observes the real row-owned
		// reset button and its registered action rather than a read-only duplicate seam.
		SlateSkipWidgetDrawing->Set(0, ECVF_SetByCode);
	}
	AudioExporterUITestUtils::TickSlateFrames();

	const FName OneShotVariationsName = GET_MEMBER_NAME_CHECKED(UAudioExporterSettings, OneShotVariations);

	// Staged OneShotVariations (1) differs from the plugin default snapshot (3): arrow visible.
	TestTrue(TEXT("A staged value differing from the plugin default must report the reset arrow visible."),
		AudioExporter::FAudioFactoryPluginWindowAdapter::IsResetToDefaultVisibleByNameForTesting(
			Window->GetContent(), OneShotVariationsName));

	// Rendered kill power for the override registration itself: exactly one engine-owned reset
	// button is enabled for the one staged property that differs from the plugin default. Stock
	// Unreal leaves these buttons disabled for CPF_Config properties when the registration is gone.
	int32 EnabledResetArrowsWhileModified = 0;
	TestTrue(TEXT("A staged value differing from the plugin default must expose one enabled rendered reset arrow."),
		AudioExporterUITestUtils::ClickUniqueEnabledResetToDefaultButton(
			Window->GetContent(), EnabledResetArrowsWhileModified));
	TestEqual(TEXT("Only the staged OneShotVariations property should expose an enabled rendered reset arrow."),
		EnabledResetArrowsWhileModified, 1);

	// ExecuteCustomResetToDefault is deferred by the engine-owned button; after the Slate tick the
	// clicked property must have returned to the product default and no reset arrow may remain enabled.
	AudioExporterUITestUtils::TickSlateFrames();
	TestEqual(TEXT("The rendered reset click must disable all reset arrows after restoring the property."),
		AudioExporterUITestUtils::CountEnabledResetToDefaultButtons(Window->GetContent()), 0);

	// Enter uses the real primary-action path to apply the staged object back to the persisted
	// settings object. The literal default proves the rendered reset changed the value that Apply
	// consumes, not just the arrow's presentation.
	AudioExporterUITestUtils::SimulateEnterKeyPress(Window);
	TestEqual(TEXT("The real Enter/apply path must commit the literal OneShotVariations default 3."),
		Settings->OneShotVariations, 3);

	// Staged value now equals the plugin default snapshot: arrow hidden.
	TestFalse(TEXT("Once restored to the plugin default the same predicate must hide the reset arrow."),
		AudioExporter::FAudioFactoryPluginWindowAdapter::IsResetToDefaultVisibleByNameForTesting(
			Window->GetContent(), OneShotVariationsName));

	if (AudioExporterUITestUtils::IsWindowStillRegisteredWithSlate(Window))
	{
		AudioExporterUITestUtils::DestroyWindow(Window);
	}
	if (SlateSkipWidgetDrawing)
	{
		SlateSkipWidgetDrawing->Set(OriginalSlateSkipWidgetDrawing, ECVF_SetByCode);
	}
	Wave->RemoveFromRoot();
	Settings->ApplyFromConfig(OriginalConfig);
	Settings->ExportDirectory = OriginalExportDirectory;
	Settings->SaveConfig();
	return true;
}

// KILLS: renaming or deleting production's LOCTEXT("AudioExporter_Export", "Export Audio...") or
// its AddDynamicEntry registration in AudioExporter.cpp RegisterToolMenus now fails this test --
// the asserted label is read off an entry materialized by production's real dynamic-entry lambda,
// not a local macro.
//
// This probe must NEVER call GenerateMenu("ContentBrowser.AssetContextMenu", ...): generating that
// menu by name assembles the ENTIRE registered hierarchy, including every OTHER plugin's
// extensions, and PluginBrowser's drag-over lambda dereferences null in a headless -NullRHI editor
// (EXCEPTION_ACCESS_VIOLATION, whole suite lost). Instead it registers an EMPTY private menu,
// "AudioFactoryTest.MenuLiteralProbe", with NO parent, copies ONLY production's "AudioExporter"
// section into a test-owned menu of that same name, and runs UToolMenus::GenerateMenuAsBuilder on
// that single menu object -- nothing foreign is ever assembled. Registration is required because
// GenerateMenuAsBuilder resolves the custom menu's name against the registry (CollectHierarchy)
// and logs "Failed to find menu" at WARNING level for unregistered names -- a warning that would
// land in the committed release warning baseline and fail the gate; Parent = NAME_None keeps the
// resolved hierarchy to the probe alone. The section stays in the crafted object rather than the
// registered one because GenerateMenuAsBuilder additionally inserts its argument into the
// by-name-collected hierarchy, so a section-holding registered argument would be assembled TWICE
// (production's dynamic-entry lambda would run twice and duplicate ExportProceduralAudio). The
// section copy drops InsertPosition ("CommonAssetActions", After): that anchor names a foreign
// section intentionally absent from the isolated menu, and a missing anchor would get the section
// silently dropped during assembly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterMenuLiteralTest,
	"AudioFactory.AudioExporter.UI.MenuLiteral",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterMenuLiteralTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Menu-literal test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	UToolMenus* ToolMenus = UToolMenus::Get();
	TestNotNull(TEXT("The ToolMenus singleton should be available."), ToolMenus);
	if (!ToolMenus)
	{
		return false;
	}

	// Read-only lookups into the REAL registry: production's RegisterToolMenus (AudioExporter.cpp)
	// created this section at startup. Deleting or renaming that registration fails these asserts.
	UToolMenu* SourceMenu = ToolMenus->FindMenu(TEXT("ContentBrowser.AssetContextMenu"));
	TestNotNull(TEXT("AudioExporter.cpp RegisterToolMenus must have extended the real Content Browser asset context menu."), SourceMenu);
	if (!SourceMenu)
	{
		return false;
	}

	FToolMenuSection* SourceSection = SourceMenu->FindSection(TEXT("AudioExporter"));
	TestNotNull(TEXT("AudioExporter.cpp RegisterToolMenus must add its \"AudioExporter\" section to the real ToolMenus registry."), SourceSection);
	if (!SourceSection)
	{
		return false;
	}

	UPackage* WavePackage = NewObject<UPackage>(nullptr, TEXT("/Temp/AudioExporterUITest/MenuLiteralWave"), RF_Public);
	USoundWave* Wave = NewObject<USoundWave>(WavePackage, TEXT("MenuLiteralWave"), RF_Public);
	TestNotNull(TEXT("The menu-literal test should construct a real SoundWave asset."), Wave);
	if (!Wave)
	{
		return false;
	}
	Wave->AddToRoot();

	// Production's dynamic entry only adds its item when the selection holds a supported audio
	// asset; with an empty/unsupported selection NO entry appears. Feeding the supported asset is
	// what makes the entry assertion below kill, not silently pass on a missing entry.
	UContentBrowserAssetContextMenuContext* ContextObject = NewObject<UContentBrowserAssetContextMenuContext>();
	ContextObject->SelectedAssets.Add(AudioExporterUITestUtils::MakeTransientAssetData(Wave));
	ContextObject->CommonClass = USoundWave::StaticClass();
	ContextObject->bCanBeModified = true;
	ContextObject->bCanView = true;

	FToolMenuContext MenuContext;
	MenuContext.AddObject(ContextObject);

	// Empty registered placeholder: satisfies the registry lookup inside GenerateMenuAsBuilder
	// without contributing sections of its own. bWarnIfAlreadyRegistered=false keeps even a stale
	// registration from a crashed earlier run silent.
	UToolMenu* RegisteredProbeMenu = ToolMenus->RegisterMenu(
		TEXT("AudioFactoryTest.MenuLiteralProbe"), NAME_None, EMultiBoxType::Menu, false);
	TestNotNull(TEXT("The probe menu placeholder should register in the ToolMenus registry."), RegisteredProbeMenu);
	if (!RegisteredProbeMenu)
	{
		return false;
	}

	// The test-owned carrier of the copied section; GenerateMenuAsBuilder places it ahead of the
	// (empty) registered namesake, so assembly sees the section exactly once -- production's real
	// dynamic entry runs, and no other plugin's menu code ever does.
	UToolMenu* ProbeMenu = NewObject<UToolMenu>(ToolMenus);
	ProbeMenu->AddToRoot();
	ProbeMenu->InitMenu(FToolMenuOwner(), TEXT("AudioFactoryTest.MenuLiteralProbe"), NAME_None, EMultiBoxType::Menu);
	ProbeMenu->Sections.Add_GetRef(*SourceSection).InsertPosition = FToolMenuInsert();

	UToolMenu* GeneratedMenu = ToolMenus->GenerateMenuAsBuilder(ProbeMenu, MenuContext);

	// Teardown sits immediately after generation with no branch between registration and here,
	// so every exit path below leaves the registry clean for sibling tests.
	ToolMenus->RemoveMenu(TEXT("AudioFactoryTest.MenuLiteralProbe"));
	ProbeMenu->RemoveFromRoot();
	Wave->RemoveFromRoot();

	TestNotNull(TEXT("The isolated probe menu should generate."), GeneratedMenu);
	if (!GeneratedMenu)
	{
		return false;
	}

	// Never pass on "no entry": a missing entry means production's AddDynamicEntry lambda did not
	// run or gated out, and that must fail loudly here.
	const FToolMenuSection* GeneratedSection = GeneratedMenu->FindSection(TEXT("AudioExporter"));
	TestTrue(TEXT("The generated probe menu must contain the AudioExporter section."), GeneratedSection != nullptr);

	const FToolMenuEntry* Entry = GeneratedSection ? GeneratedSection->FindEntry(TEXT("ExportProceduralAudio")) : nullptr;
	TestTrue(TEXT("Production's AddDynamicEntry lambda must materialize ExportProceduralAudio for a supported SoundWave selection."), Entry != nullptr);

	if (Entry && Entry->Label.IsSet())
	{
		const FText RegisteredLabel = Entry->Label.Get();
		TestEqual(TEXT("AudioExporter.cpp must register exactly Export Audio..."),
			RegisteredLabel.ToString(), FString(TEXT("Export Audio...")));
		TestFalse(TEXT("The registered Audio Exporter label must not expose the removed internal build marker."),
			RegisteredLabel.ToString().Contains(TEXT("(R5)")));
	}
	else if (Entry)
	{
		AddError(TEXT("The materialized ExportProceduralAudio entry must carry a label attribute."));
	}

	// The generated menu is a transient object owned by the UToolMenus singleton, exactly like
	// every other Generate* result; the registered placeholder and the crafted probe were both
	// torn down immediately after generation, so nothing leaks into any other test.
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterProgressWindowSmokeTest,
	"AudioFactory.AudioExporter.UI.ProgressWindowSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterProgressWindowSmokeTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Progress-window smoke test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	const TSharedRef<SWindow> Window = SAudioExportProgressWindow::OpenProgressWindow(
		TArray<FAssetData>(),
		FOnAudioExporterMonitorChanged::CreateLambda([](bool bMonitoring)
		{
			(void)bMonitoring;
		}),
		FOnAudioExporterCancelRequested::CreateLambda([]() {}),
		FOnAudioExporterRequestRecordingSubmix::CreateLambda([]() -> USoundSubmix*
		{
			return nullptr;
		}),
		FOnAudioExporterRequestWorld::CreateLambda([]() -> UWorld*
		{
			return nullptr;
		}),
		FOnAudioExporterIsExportActive::CreateLambda([]()
		{
			return true;
		}));
	// "<Product> — <Task>" grammar via the shared FAudioFactoryDialogWindow factory; there is no
	// "AudioFactory / ..." OS-title prefix.
	TestEqual(TEXT("Audio Exporter progress window should use the expected title."), Window->GetTitle().ToString(), FString(TEXT("Audio Exporter — Exporting")));
	TestTrue(TEXT("Audio Exporter progress window should be user-resizable (MinSize/placement via the shared dialog factory)."), Window->IsUserSized());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterProgressWindowRenderedSublineUsesNewlineTest,
	"AudioFactory.AudioExporter.UI.ProgressWindowRenderedSublineUsesNewline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterProgressWindowRenderedSublineUsesNewlineTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Rendered-subline test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	const TSharedRef<SWindow> Window = AudioExporterUITestUtils::OpenNullCallbackProgressWindow();
	FSlateApplication::Get().AddWindow(Window);
	AudioExporterUITestUtils::TickSlateFrames();

	const FString VisibleText = AudioExporterUITestUtils::RenderedText(Window->GetContent());
	TestTrue(TEXT("The rendered progress subline must put the status before a newline and zero-padded elapsed time."),
		VisibleText.Contains(TEXT("Preparing to export 0 out of 0\nElapsed: 00:00")));

	Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
	AudioExporterUITestUtils::DestroyWindow(Window);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterProgressWindowElapsedFormatPadsMinutesTest,
	"AudioFactory.AudioExporter.UI.ProgressWindowElapsedFormatPadsMinutes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterProgressWindowElapsedFormatPadsMinutesTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Elapsed-format test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	const TSharedRef<SWindow> Window = AudioExporterUITestUtils::OpenNullCallbackProgressWindow();
	FSlateApplication::Get().AddWindow(Window);
	AudioExporterUITestUtils::TickSlateFrames();

	const TSharedRef<SAudioExportProgressWindow> Widget = AudioExporterUITestUtils::GetProgressWidget(Window);
	Widget->SetElapsedSecondsForTesting(67.0);
	Widget->RefreshElapsedTextForTesting();
	AudioExporterUITestUtils::TickSlateFrames();

	const FString VisibleText = AudioExporterUITestUtils::RenderedText(Window->GetContent());
	TestTrue(TEXT("67 elapsed seconds must render as a one-minute, seven-second zero-padded subline."),
		VisibleText.Contains(TEXT("Preparing to export 0 out of 0\nElapsed: 01:07")));

	Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
	AudioExporterUITestUtils::DestroyWindow(Window);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterProgressWindowCancelButtonRequestsCancelTest,
	"AudioFactory.AudioExporter.UI.ProgressWindowCancelButtonRequestsCancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterProgressWindowCancelButtonRequestsCancelTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Progress-window cancel-button test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	int32 CancelRequests = 0;
	const TSharedRef<SWindow> Window = SAudioExportProgressWindow::OpenProgressWindow(
		TArray<FAssetData>(),
		FOnAudioExporterMonitorChanged::CreateLambda([](bool) {}),
		FOnAudioExporterCancelRequested::CreateLambda([&CancelRequests]()
		{
			++CancelRequests;
		}),
		FOnAudioExporterRequestRecordingSubmix::CreateLambda([]() -> USoundSubmix*
		{
			return nullptr;
		}),
		FOnAudioExporterRequestWorld::CreateLambda([]() -> UWorld*
		{
			return nullptr;
		}),
		FOnAudioExporterIsExportActive::CreateLambda([]()
		{
			return true;
		}));
	FSlateApplication::Get().AddWindow(Window);
	AudioExporterUITestUtils::TickSlateFrames();

	AudioExporterUITestUtils::GetProgressWidget(Window)->InvokeCancelForTesting();
	TestEqual(TEXT("The progress-window cancel action should invoke the cancel delegate once."), CancelRequests, 1);

	Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
	AudioExporterUITestUtils::DestroyWindow(Window);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterProgressWindowOsCloseRequestsCancelTest,
	"AudioFactory.AudioExporter.UI.ProgressWindowOsCloseRequestsCancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterProgressWindowOsCloseRequestsCancelTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Progress-window OS-close test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	int32 CancelRequests = 0;
	const TSharedRef<SWindow> Window = SAudioExportProgressWindow::OpenProgressWindow(
		TArray<FAssetData>(),
		FOnAudioExporterMonitorChanged::CreateLambda([](bool) {}),
		FOnAudioExporterCancelRequested::CreateLambda([&CancelRequests]()
		{
			++CancelRequests;
		}),
		FOnAudioExporterRequestRecordingSubmix::CreateLambda([]() -> USoundSubmix*
		{
			return nullptr;
		}),
		FOnAudioExporterRequestWorld::CreateLambda([]() -> UWorld*
		{
			return nullptr;
		}),
		FOnAudioExporterIsExportActive::CreateLambda([]()
		{
			return true;
		}));
	FSlateApplication::Get().AddWindow(Window);
	AudioExporterUITestUtils::TickSlateFrames();

	Window->RequestDestroyWindow();
	AudioExporterUITestUtils::TickSlateFrames();
	TestEqual(TEXT("The progress-window OS close path should request export cancellation once."), CancelRequests, 1);

	Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
	AudioExporterUITestUtils::DestroyWindow(Window);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterProgressWindowZeroExportedIsNotSuccessTest,
	"AudioFactory.AudioExporter.UI.ProgressWindowZeroExportedIsNotSuccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterProgressWindowZeroExportedIsNotSuccessTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Zero-exported terminal test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	// Case 1: a valid path finishing with zero exported and zero failed must NOT be a green
	// success -- this guards the empty-run-reported-as-success regression.
	{
		const TSharedRef<SWindow> Window = AudioExporterUITestUtils::OpenNullCallbackProgressWindow();
		FSlateApplication::Get().AddWindow(Window);
		AudioExporterUITestUtils::TickSlateFrames();

		const TSharedRef<SAudioExportProgressWindow> Widget = AudioExporterUITestUtils::GetProgressWidget(Window);
		AudioExporterUITestUtils::DriveTerminalOutcome(Widget, /*Exported*/ 0, /*Failed*/ 0);
		AudioExporterUITestUtils::TickSlateFrames();

		TestTrue(TEXT("A terminal state should be reached after finishing."), Widget->HasReachedTerminalStateForTesting());
		TestFalse(TEXT("Zero exported files on a valid path must NOT report a green success."), Widget->WasTerminalSuccessForTesting());
		TestFalse(TEXT("Zero exported files must not be classified as a partial completion."), Widget->WasTerminalPartialForTesting());
		TestEqual(TEXT("The terminal exported count should be zero for an empty run."), Widget->GetTerminalExportedCountForTesting(), 0);

		Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
		AudioExporterUITestUtils::DestroyWindow(Window);
	}

	// Case 2: zero exported but with recorded failures is likewise a failure, not a success.
	{
		const TSharedRef<SWindow> Window = AudioExporterUITestUtils::OpenNullCallbackProgressWindow();
		FSlateApplication::Get().AddWindow(Window);
		AudioExporterUITestUtils::TickSlateFrames();

		const TSharedRef<SAudioExportProgressWindow> Widget = AudioExporterUITestUtils::GetProgressWidget(Window);
		AudioExporterUITestUtils::DriveTerminalOutcome(Widget, /*Exported*/ 0, /*Failed*/ 3);
		AudioExporterUITestUtils::TickSlateFrames();

		TestFalse(TEXT("Zero exported with failures must NOT report a green success."), Widget->WasTerminalSuccessForTesting());
		TestFalse(TEXT("Zero exported with failures is a failure, not a partial completion."), Widget->WasTerminalPartialForTesting());

		Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
		AudioExporterUITestUtils::DestroyWindow(Window);
	}

	// Case 3: a render failure with unstarted planned variations must surface both terminal counts.
	{
		const TSharedRef<SWindow> Window = AudioExporterUITestUtils::OpenNullCallbackProgressWindow();
		FSlateApplication::Get().AddWindow(Window);
		AudioExporterUITestUtils::TickSlateFrames();

		const TSharedRef<SAudioExportProgressWindow> Widget = AudioExporterUITestUtils::GetProgressWidget(Window);
		FAudioExportResult Result;
		Result.Outcome = EAudioExportOutcome::Failed;
		Result.bAcceptedStart = true;
		Result.FailedCount = 1;
		Result.SkippedCount = 2;
		Result.ErrorMessage = TEXT("1 variation(s) failed to export. 2 variation(s) were skipped.");
		Widget->OnExportFinishedResult(Result);
		Widget->OnExportFinishedProgress(FPaths::ProjectSavedDir());
		AudioExporterUITestUtils::TickSlateFrames();

		TestTrue(TEXT("A render failure with unstarted work must show the failed count in the terminal body."),
			Widget->GetTerminalDetailForTesting().Contains(TEXT("1 failed")));
		TestTrue(TEXT("A render failure with unstarted work must show the skipped count in the terminal body."),
			Widget->GetTerminalDetailForTesting().Contains(TEXT("2 skipped")));
		TestTrue(TEXT("A render failure notification must show both failed and skipped counts."),
			Widget->GetTerminalNotificationTextForTesting().Contains(TEXT("1 failed"))
			&& Widget->GetTerminalNotificationTextForTesting().Contains(TEXT("2 skipped")));
		TestTrue(TEXT("A render failure status strip must show both failed and skipped counts."),
			Widget->GetStatusTextForTesting().Contains(TEXT("1 failed"))
			&& Widget->GetStatusTextForTesting().Contains(TEXT("2 skipped")));

		Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
		AudioExporterUITestUtils::DestroyWindow(Window);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterProgressWindowExportedFilesAreSuccessTest,
	"AudioFactory.AudioExporter.UI.ProgressWindowExportedFilesAreSuccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterProgressWindowExportedFilesAreSuccessTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Exported-files success test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	// A normal multi-asset run (>0 exported, 0 failed) on a valid path must still be a success.
	{
		const TSharedRef<SWindow> Window = AudioExporterUITestUtils::OpenNullCallbackProgressWindow();
		FSlateApplication::Get().AddWindow(Window);
		AudioExporterUITestUtils::TickSlateFrames();

		const TSharedRef<SAudioExportProgressWindow> Widget = AudioExporterUITestUtils::GetProgressWidget(Window);
		AudioExporterUITestUtils::DriveTerminalOutcome(Widget, /*Exported*/ 4, /*Failed*/ 0);
		AudioExporterUITestUtils::TickSlateFrames();

		TestTrue(TEXT("A run that exported files with no failures must report a green success."), Widget->WasTerminalSuccessForTesting());
		TestFalse(TEXT("A clean successful run should not be classified as partial."), Widget->WasTerminalPartialForTesting());
		TestEqual(TEXT("The terminal exported count should match the reported exports."), Widget->GetTerminalExportedCountForTesting(), 4);
		TestTrue(TEXT("A successful export toast should report the exported file count."),
			Widget->GetTerminalNotificationTextForTesting().Contains(TEXT("4 files exported")));
		TestTrue(TEXT("A successful export toast should bind Open Folder."), Widget->WasOpenFolderBoundForTesting());
		TestTrue(TEXT("A clean success (exported files, no failures) must carry the success icon."),
			Widget->GetTerminalNotificationCompletionStateForTesting() == SNotificationItem::CS_Success);

		Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
		AudioExporterUITestUtils::DestroyWindow(Window);
	}

	// A mixed run (>0 exported, >0 failed) is a partial completion -- not a green success.
	{
		const TSharedRef<SWindow> Window = AudioExporterUITestUtils::OpenNullCallbackProgressWindow();
		FSlateApplication::Get().AddWindow(Window);
		AudioExporterUITestUtils::TickSlateFrames();

		const TSharedRef<SAudioExportProgressWindow> Widget = AudioExporterUITestUtils::GetProgressWidget(Window);
		AudioExporterUITestUtils::DriveTerminalOutcome(Widget, /*Exported*/ 3, /*Failed*/ 1);
		AudioExporterUITestUtils::TickSlateFrames();

		TestFalse(TEXT("A run with failures must NOT report a green success."), Widget->WasTerminalSuccessForTesting());
		TestTrue(TEXT("A run with some exports and some failures should be a partial completion."), Widget->WasTerminalPartialForTesting());
		TestTrue(TEXT("A partial completion (some exports, some failures) must NOT carry the success icon."),
			Widget->GetTerminalNotificationCompletionStateForTesting() == SNotificationItem::CS_Fail);

		Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
		AudioExporterUITestUtils::DestroyWindow(Window);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterProgressWindowTerminalPluralizationTest,
	"AudioFactory.AudioExporter.UI.ProgressWindowTerminalPluralization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterProgressWindowTerminalPluralizationTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Terminal-pluralization test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	struct FExpectedTerminalCopy
	{
		int32 ExportedFiles;
		const TCHAR* Notification;
		const TCHAR* Detail;
		const TCHAR* Status;
	};

	const FExpectedTerminalCopy Expected[] = {
		{ 0, TEXT("Audio export failed. No files were exported."), TEXT("No files were exported"), TEXT("Export failed. No files were exported.") },
		{ 1, TEXT("Audio export completed! 1 file exported."), TEXT("1 file exported"), TEXT("Export complete. 1 file exported.") },
		{ 2, TEXT("Audio export completed! 2 files exported."), TEXT("2 files exported"), TEXT("Export complete. 2 files exported.") },
	};

	for (const FExpectedTerminalCopy& Case : Expected)
	{
		const TSharedRef<SWindow> Window = AudioExporterUITestUtils::OpenNullCallbackProgressWindow();
		FSlateApplication::Get().AddWindow(Window);
		AudioExporterUITestUtils::TickSlateFrames();

		const TSharedRef<SAudioExportProgressWindow> Widget = AudioExporterUITestUtils::GetProgressWidget(Window);
		AudioExporterUITestUtils::DriveTerminalOutcome(Widget, Case.ExportedFiles, 0);
		AudioExporterUITestUtils::TickSlateFrames();

		TestEqual(TEXT("The terminal notification must preserve the expected zero/singular/plural English copy."),
			Widget->GetTerminalNotificationTextForTesting(), FString(Case.Notification));
		TestEqual(TEXT("The terminal detail must preserve the expected zero/singular/plural English copy."),
			Widget->GetTerminalDetailForTesting(), FString(Case.Detail));
		TestEqual(TEXT("The status strip must preserve the expected zero/singular/plural English copy."),
			Widget->GetStatusTextForTesting(), FString(Case.Status));
		TestFalse(TEXT("Terminal pluralization must never regress to file(s) syntax."),
			Widget->GetTerminalNotificationTextForTesting().Contains(TEXT("file(s)")));
		TestFalse(TEXT("Terminal pluralization must never render an unavailable zero count as a file count."),
			Case.ExportedFiles == 0 && Widget->GetTerminalNotificationTextForTesting().Contains(TEXT("0 file")));

		if (AudioExporterUITestUtils::IsWindowStillRegisteredWithSlate(Window))
		{
			AudioExporterUITestUtils::DestroyWindow(Window);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterProgressWindowApprovedCopyTest,
	"AudioFactory.AudioExporter.UI.ProgressWindowApprovedCopy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterProgressWindowApprovedCopyTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Approved-copy test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	UPackage* FirstPackage = NewObject<UPackage>(nullptr, TEXT("/Temp/AudioExporterUITest/ApprovedCopyFirst"), RF_Public);
	USoundWave* FirstWave = NewObject<USoundWave>(FirstPackage, TEXT("ApprovedCopyFirst"), RF_Public);
	UPackage* SecondPackage = NewObject<UPackage>(nullptr, TEXT("/Temp/AudioExporterUITest/ApprovedCopySecond"), RF_Public);
	USoundWave* SecondWave = NewObject<USoundWave>(SecondPackage, TEXT("ApprovedCopySecond"), RF_Public);
	TestNotNull(TEXT("The copy test should construct the first real SoundWave asset."), FirstWave);
	TestNotNull(TEXT("The copy test should construct the second real SoundWave asset."), SecondWave);
	if (!FirstWave || !SecondWave)
	{
		return false;
	}
	FirstWave->AddToRoot();
	SecondWave->AddToRoot();

	const TArray<FAssetData> Assets = {
		AudioExporterUITestUtils::MakeTransientAssetData(FirstWave),
		AudioExporterUITestUtils::MakeTransientAssetData(SecondWave),
	};
	const TSharedRef<SWindow> Window = SAudioExportProgressWindow::OpenProgressWindow(
		Assets,
		FOnAudioExporterMonitorChanged::CreateLambda([](bool) {}),
		FOnAudioExporterCancelRequested::CreateLambda([]() {}),
		FOnAudioExporterRequestRecordingSubmix::CreateLambda([]() -> USoundSubmix*
		{
			return nullptr;
		}),
		FOnAudioExporterRequestWorld::CreateLambda([]() -> UWorld*
		{
			return nullptr;
		}),
		FOnAudioExporterIsExportActive::CreateLambda([]()
		{
			return true;
		}));
	FSlateApplication::Get().AddWindow(Window);
	AudioExporterUITestUtils::TickSlateFrames();

	const TSharedRef<SAudioExportProgressWindow> Widget = AudioExporterUITestUtils::GetProgressWidget(Window);
	FAudioExportProgressInfo FailureTick;
	FailureTick.CurrentSound = 1;
	FailureTick.TotalSounds = 2;
	FailureTick.bHasResult = true;
	FailureTick.bSucceeded = false;
	FailureTick.FailedFiles = 1;
	FailureTick.ResultMessage = TEXT("Synthetic failure");
	Widget->UpdateProgressUI(FailureTick);

	TestEqual(TEXT("The running status must preserve the expected separator pattern."),
		Widget->GetStatusTextForTesting(), FString(TEXT("Exporting 2 selected assets  |  1 failed so far.")));
	const FString VisibleText = AudioExporterUITestUtils::RenderedText(Window->GetContent());
	TestTrue(TEXT("The real progress window must render the exact Cancel label."),
		VisibleText.Contains(TEXT("Cancel")));
	TestFalse(TEXT("The progress window must not expose the removed internal build marker."),
		VisibleText.Contains(TEXT("(R5)")) || Window->GetTitle().ToString().Contains(TEXT("(R5)")));

	Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
	AudioExporterUITestUtils::DestroyWindow(Window);
	FirstWave->RemoveFromRoot();
	SecondWave->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterProgressWindowCancelledIsNotSuccessTest,
	"AudioFactory.AudioExporter.UI.ProgressWindowCancelledIsNotSuccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterProgressWindowCancelledIsNotSuccessTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Cancelled-is-not-success test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	// Drives the SAME two-delegate sequence the real subsystem uses in production
	// (OnExportFinishedResult with the structured outcome, then the legacy OnExportFinishedProgress
	// path string -- see the class comment on OnExportFinishedResult), rather than only exercising
	// the defensive count-inferred fallback the other tests in this file use. A cancelled export
	// must never carry the success icon, however many files it got through before the cancellation.
	const TSharedRef<SWindow> Window = AudioExporterUITestUtils::OpenNullCallbackProgressWindow();
	FSlateApplication::Get().AddWindow(Window);
	AudioExporterUITestUtils::TickSlateFrames();

	const TSharedRef<SAudioExportProgressWindow> Widget = AudioExporterUITestUtils::GetProgressWidget(Window);

	FAudioExportResult Result;
	Result.Outcome = EAudioExportOutcome::Cancelled;
	Result.bAcceptedStart = true;
	Result.SuccessfulCount = 2;
	Result.FailedCount = 3;
	Result.SkippedCount = 1;
	Result.ErrorMessage = TEXT("3 variation(s) failed to export. 1 variation(s) were skipped. Skip detail: Save skipped because the export session is no longer active.");
	Widget->OnExportFinishedResult(Result);
	Widget->OnExportFinishedProgress(FPaths::ProjectSavedDir());
	AudioExporterUITestUtils::TickSlateFrames();

	TestFalse(TEXT("A cancelled export must NOT report a green success, even with 2 files exported and 0 failures."),
		Widget->WasTerminalSuccessForTesting());
	TestTrue(TEXT("A cancelled export must NEVER carry the success icon."),
		Widget->GetTerminalNotificationCompletionStateForTesting() != SNotificationItem::CS_Success);
	TestEqual(TEXT("A cancelled export terminal UI must expose the structured skipped count."),
		Widget->GetTerminalSkippedCountForTesting(), 1);
	TestTrue(TEXT("A cancelled export terminal UI must show the exact skipped-save detail."),
		Widget->GetTerminalDetailForTesting().Contains(TEXT("Save skipped because the export session is no longer active.")));
	TestTrue(TEXT("The cancelled export notification must include the skipped count."),
		Widget->GetTerminalNotificationTextForTesting().Contains(TEXT("1 skipped")));
	TestTrue(TEXT("The cancelled status strip must say cancelled rather than failed."),
		Widget->GetStatusTextForTesting().Contains(TEXT("Export cancelled")));
	TestTrue(TEXT("The cancelled status strip must preserve the exported count."),
		Widget->GetStatusTextForTesting().Contains(TEXT("2 files exported")));
	TestTrue(TEXT("The cancelled status strip must preserve the failed count."),
		Widget->GetStatusTextForTesting().Contains(TEXT("3 failed")));
	TestTrue(TEXT("The cancelled status strip must preserve the skipped count."),
		Widget->GetStatusTextForTesting().Contains(TEXT("1 skipped")));
	TestEqual(TEXT("A cancelled terminal status should use the warning severity."),
		Widget->GetStatusSeverityForTesting(), AudioExporter::EAudioFactoryStatusSeverity::Warning);

	Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
	AudioExporterUITestUtils::DestroyWindow(Window);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterProgressWindowStructuredResultDetailsTest,
	"AudioFactory.AudioExporter.UI.ProgressWindowStructuredResultDetails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterProgressWindowStructuredResultDetailsTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Structured-result detail test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	const TSharedRef<SWindow> Window = AudioExporterUITestUtils::OpenNullCallbackProgressWindow();
	FSlateApplication::Get().AddWindow(Window);
	AudioExporterUITestUtils::TickSlateFrames();

	const TSharedRef<SAudioExportProgressWindow> Widget = AudioExporterUITestUtils::GetProgressWidget(Window);
	FAudioExportResult Result;
	Result.Outcome = EAudioExportOutcome::PartialSuccess;
	Result.bAcceptedStart = true;
	Result.SuccessfulCount = 5;
	Result.FailedCount = 0;
	Result.ErrorMessage = TEXT("1 report output(s) failed: Report writer detail");
	Result.ReportsFailed = 1;
	Result.ReportErrorMessage = TEXT("Report writer detail");
	Widget->OnExportFinishedResult(Result);
	Widget->OnExportFinishedProgress(FPaths::ProjectSavedDir());
	AudioExporterUITestUtils::TickSlateFrames();

	TestFalse(TEXT("A structured partial result with a failure detail must not be green."), Widget->WasTerminalSuccessForTesting());
	TestTrue(TEXT("A structured partial result must remain partial even when its failed count is zero."), Widget->WasTerminalPartialForTesting());
	TestEqual(TEXT("The terminal exported count must come from the real result object."), Widget->GetTerminalExportedCountForTesting(), 5);
	TestEqual(TEXT("The terminal failed count must come from the real result object."), Widget->GetTerminalFailedCountForTesting(), 0);
	TestTrue(TEXT("The terminal body must show the report failure count with singular grammar."), Widget->GetTerminalDetailForTesting().Contains(TEXT("1 report output failed")));
	TestTrue(TEXT("The terminal body must show the exact report error detail."), Widget->GetTerminalDetailForTesting().Contains(TEXT("Report writer detail")));
	TestEqual(TEXT("The report error detail must appear exactly once in the terminal body."),
		AudioExporterUITestUtils::CountOccurrences(Widget->GetTerminalDetailForTesting(), TEXT("Report writer detail")), 1);
	TestFalse(TEXT("A report-only failure must not display zero file failures."),
		Widget->GetTerminalNotificationTextForTesting().Contains(TEXT("0 failed")));
	TestTrue(TEXT("The notification must explain the report-only failure with singular grammar."),
		Widget->GetTerminalNotificationTextForTesting().Contains(TEXT("1 report output failed"))
		&& Widget->GetTerminalNotificationTextForTesting().Contains(TEXT("Report writer detail")));

	Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
	AudioExporterUITestUtils::DestroyWindow(Window);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterProgressWindowSuccessCloseDoesNotCancelTest,
	"AudioFactory.AudioExporter.UI.ProgressWindowSuccessCloseDoesNotCancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterProgressWindowSuccessCloseDoesNotCancelTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Progress-window success-close test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	UAudioExporterRuntimeSubsystem* RuntimeSubsystem = UAudioExporterBPLibrary::GetRuntimeSubsystem();
	TestNotNull(TEXT("The success-close test should resolve the real AudioExporter runtime subsystem."), RuntimeSubsystem);
	if (!RuntimeSubsystem)
	{
		return false;
	}
	if (RuntimeSubsystem->IsExporting())
	{
		RuntimeSubsystem->CancelExport(false);
	}

	UWorld* World = GWorld;
	TestNotNull(TEXT("The success-close test should have a live world for the real subsystem session."), World);
	if (!World)
	{
		return false;
	}

	USoundWave* Wave = NewObject<USoundWave>(GetTransientPackage(), TEXT("AudioExporterProgressWindowSuccessWave"));
	TestNotNull(TEXT("The success-close test should create a real static SoundWave."), Wave);
	if (!Wave)
	{
		return false;
	}
	Wave->AddToRoot();

	int32 CancelRequests = 0;
	const TSharedRef<SWindow> Window = SAudioExportProgressWindow::OpenProgressWindow(
		TArray<FAssetData>(),
		FOnAudioExporterMonitorChanged::CreateLambda([](bool) {}),
		FOnAudioExporterCancelRequested::CreateLambda([&CancelRequests]()
		{
			++CancelRequests;
		}),
		FOnAudioExporterRequestRecordingSubmix::CreateLambda([]() -> USoundSubmix*
		{
			return nullptr;
		}),
		FOnAudioExporterRequestWorld::CreateLambda([]() -> UWorld*
		{
			return GWorld;
		}),
		FOnAudioExporterIsExportActive::CreateLambda([RuntimeSubsystem]()
		{
			return RuntimeSubsystem->IsExporting();
		}));
	FSlateApplication::Get().AddWindow(Window);
	AudioExporterUITestUtils::TickSlateFrames();

	const TSharedRef<SAudioExportProgressWindow> ProgressWidget = AudioExporterUITestUtils::GetProgressWidget(Window);
	int32 StructuredResultEvents = 0;
	FAudioExportResult StructuredResult;
	const FDelegateHandle StructuredResultHandle = RuntimeSubsystem->OnExportCompleteResultNative.AddLambda([&StructuredResultEvents, &StructuredResult](const FAudioExportResult& InResult)
	{
		++StructuredResultEvents;
		StructuredResult = InResult;
	});

	FAudioExportSoundEntry Entry;
	Entry.Sound = Wave;
	Entry.SoundName = TEXT("AudioExporterProgressWindowSuccessWave");
	Entry.SourceAssetPath = TEXT("/Temp/AudioExporterProgressWindowSuccessWave");
	Entry.RelativePath = TEXT("ProgressWindowSuccess");
	TArray<FAudioExportSoundEntry> Entries;
	Entries.Add(Entry);

	FAudioExporterConfig Config;
	Config.OneShotVariations = 1;
	Config.LoopVariations = 1;
	Config.bNormalize = false;
	Config.bMonitor = false;
	Config.bExportTextLog = false;
	const FString OutputRoot = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("Automation"),
		TEXT("UI"),
		FString::Printf(TEXT("ProgressWindowSuccessClose_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	IFileManager::Get().MakeDirectory(*OutputRoot, true);

	const bool bStarted = RuntimeSubsystem->BeginLifecycleTestSession(World, Entries, Config, OutputRoot);
	TestTrue(TEXT("The success-close test should accept the real subsystem session."), bStarted);
	if (bStarted)
	{
		RuntimeSubsystem->SetForceAsyncSaveForTesting(false);
		RuntimeSubsystem->SimulateVariationFinishedForTesting();
	}

	TestEqual(TEXT("The real subsystem should emit exactly one structured result event."), StructuredResultEvents, 1);
	TestEqual(TEXT("The real successful output should report Succeeded."), StructuredResult.Outcome, EAudioExportOutcome::Succeeded);
	TestEqual(TEXT("The structured result should report one successful file."), StructuredResult.SuccessfulCount, 1);
	TestFalse(TEXT("The real successful result should carry its output path."), StructuredResult.ExportPath.IsEmpty());

	TArray<FString> WavFiles;
	IFileManager::Get().FindFilesRecursive(WavFiles, *OutputRoot, TEXT("*.wav"), true, false);
	TestEqual(TEXT("The success-close test should produce exactly one real WAV."), WavFiles.Num(), 1);
	if (WavFiles.Num() == 1)
	{
		TestTrue(TEXT("The real successful WAV should contain audio data."), IFileManager::Get().FileSize(*WavFiles[0]) > 44);
	}

	// The actual result event above populated the window through Construct()'s production binding.
	// This legacy path is only the existing presentation/close handoff; it must consume the cached
	// structured outcome instead of re-inferring success from synthetic counts.
	ProgressWidget->OnExportFinishedProgress(StructuredResult.ExportPath);
	AudioExporterUITestUtils::TickSlateFrames();

	TestTrue(TEXT("Success completion should mark the progress window terminal."), ProgressWidget->HasReachedTerminalStateForTesting());
	TestTrue(TEXT("A real successful subsystem result should classify the progress window as success."), ProgressWidget->WasTerminalSuccessForTesting());
	TestEqual(TEXT("Success-driven progress-window close should not invoke cancellation."), CancelRequests, 0);

	RuntimeSubsystem->OnExportCompleteResultNative.Remove(StructuredResultHandle);
	if (RuntimeSubsystem->IsExporting())
	{
		RuntimeSubsystem->CancelExport(false);
	}
	if (AudioExporterUITestUtils::IsWindowStillRegisteredWithSlate(Window))
	{
		Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
		AudioExporterUITestUtils::DestroyWindow(Window);
	}
	Wave->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterProgressWindowInactiveCloseDoesNotCancelTest,
	"AudioFactory.AudioExporter.UI.ProgressWindowInactiveCloseDoesNotCancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterProgressWindowInactiveCloseDoesNotCancelTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Progress-window inactive-close test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	int32 CancelRequests = 0;
	const TSharedRef<SWindow> Window = SAudioExportProgressWindow::OpenProgressWindow(
		TArray<FAssetData>(),
		FOnAudioExporterMonitorChanged::CreateLambda([](bool) {}),
		FOnAudioExporterCancelRequested::CreateLambda([&CancelRequests]()
		{
			++CancelRequests;
		}),
		FOnAudioExporterRequestRecordingSubmix::CreateLambda([]() -> USoundSubmix*
		{
			return nullptr;
		}),
		FOnAudioExporterRequestWorld::CreateLambda([]() -> UWorld*
		{
			return nullptr;
		}),
		FOnAudioExporterIsExportActive::CreateLambda([]()
		{
			return false;
		}));
	FSlateApplication::Get().AddWindow(Window);
	AudioExporterUITestUtils::TickSlateFrames();

	Window->RequestDestroyWindow();
	AudioExporterUITestUtils::TickSlateFrames();
	TestEqual(TEXT("Inactive progress-window close should not invoke cancellation."), CancelRequests, 0);

	Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
	AudioExporterUITestUtils::DestroyWindow(Window);
	return true;
}

namespace AudioExporterUITestUtils
{
	/** Shared state for the real manager start path and the progress window it opens. Delegate
	 * handlers stay bound until Cleanup so the terminal counters remain valid across latent frames. */
	struct FManagerProgressWindowState
	{
		UAudioExporterRuntimeSubsystem* RuntimeSubsystem = nullptr;
		TSharedPtr<FAudioExportManager> Manager;
		FDelegateHandle ResultHandle;
		FDelegateHandle FinishedHandle;
		FDelegateHandle ManagerStartedHandle;
		FDelegateHandle ManagerFinishedHandle;
		int32 TerminalEvents = 0;
		int32 LegacyFinishedEvents = 0;
		int32 ManagerFinishedEvents = 0;
		/** Legacy finish events actually forwarded into the live progress window after it was
		 * opened. The window's own terminal path (OnExportFinishedProgress) applies the terminal
		 * presentation and closes the window synchronously inside one game-thread task, so a
		 * per-tick latent command can never observe the widget between those two steps; this
		 * counter is the durable proof that the live window received its completion and
		 * drove its terminal path before self-closing. */
		int32 PostOpenFinishedForwards = 0;
		int32 StartedEvents = 0;
		FAudioExportResult TerminalResult;
		TWeakPtr<SWindow> ProgressWindow;
		bool bCleanedUp = false;

		void Cleanup()
		{
			if (bCleanedUp)
			{
				return;
			}
			bCleanedUp = true;

			if (Manager.IsValid())
			{
				if (ManagerStartedHandle.IsValid())
				{
					Manager->OnAudioExportStarted.Remove(ManagerStartedHandle);
				}
				if (ManagerFinishedHandle.IsValid())
				{
					Manager->OnAudioExportFinished.Remove(ManagerFinishedHandle);
				}
				if (Manager->IsExporting())
				{
					Manager->CancelExporting(false);
				}
				Manager.Reset();
			}

			if (RuntimeSubsystem)
			{
				if (ResultHandle.IsValid())
				{
					RuntimeSubsystem->OnExportCompleteResultNative.Remove(ResultHandle);
				}
				if (FinishedHandle.IsValid())
				{
					RuntimeSubsystem->OnExportCompleteNative.Remove(FinishedHandle);
				}
			}

			const TSharedPtr<SWindow> Window = ProgressWindow.Pin();
			if (Window.IsValid() && FSlateApplication::IsInitialized())
			{
				// The progress window installs a cancel-on-close override in production; clear it so
				// the teardown actually happens (same idiom as every sibling progress-window test).
				Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
				DestroyWindow(Window.ToSharedRef());
			}
		}
	};

}

// Fail-first regression: the real manager must arm its editor state and broadcast Started
// before entering the runtime. This test uses a valid transient SoundCue whose empty wave-player
// branch settles synchronously through the production renderer error path. The old ordering delivered
// the terminal pair before the manager opened the window, then delivered the cached pair again from
// the replay ticker. The new contract is one structured and one legacy terminal, delivered into the
// live window that Started opened on the same manager call stack.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterProgressWindowSyncSettleBeforeStartedStillTerminatesTest,
	"AudioFactory.AudioExporter.UI.ProgressWindow.SyncSettleBeforeStartedStillTerminates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterProgressWindowSyncSettleBeforeStartedStillTerminatesTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Sync-settle-before-Started test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	UAudioExporterRuntimeSubsystem* RuntimeSubsystem = UAudioExporterBPLibrary::GetRuntimeSubsystem();
	TestNotNull(TEXT("The manager-order test should resolve the real AudioExporter runtime subsystem."), RuntimeSubsystem);
	if (!RuntimeSubsystem)
	{
		return false;
	}
	if (RuntimeSubsystem->IsExporting())
	{
		RuntimeSubsystem->CancelExport(false);
	}

	const TSharedRef<AudioExporterUITestUtils::FManagerProgressWindowState> ProgressState =
		MakeShared<AudioExporterUITestUtils::FManagerProgressWindowState>();
	ProgressState->RuntimeSubsystem = RuntimeSubsystem;
	ProgressState->Manager = MakeShared<FAudioExportManager>();
	ProgressState->Manager->Initialize();

	ProgressState->ResultHandle = RuntimeSubsystem->OnExportCompleteResultNative.AddLambda(
		[ProgressState](const FAudioExportResult& InResult)
		{
			++ProgressState->TerminalEvents;
			ProgressState->TerminalResult = InResult;
		});
	ProgressState->FinishedHandle = RuntimeSubsystem->OnExportCompleteNative.AddLambda(
		[ProgressState](const FString&)
		{
			++ProgressState->LegacyFinishedEvents;
		});

	ProgressState->ManagerFinishedHandle = ProgressState->Manager->OnAudioExportFinished.AddLambda(
		[ProgressState](const FString& ExportPath)
		{
			++ProgressState->ManagerFinishedEvents;
			if (const TSharedPtr<SWindow> Window = ProgressState->ProgressWindow.Pin())
			{
				++ProgressState->PostOpenFinishedForwards;
				StaticCastSharedRef<SAudioExportProgressWindow>(Window->GetContent())->OnExportFinishedProgress(ExportPath);
			}
		});

	ProgressState->ManagerStartedHandle = ProgressState->Manager->OnAudioExportStarted.AddLambda(
		[ProgressState](const TArray<FAssetData>& Assets)
		{
			++ProgressState->StartedEvents;
			const TSharedRef<SWindow> ProgressWindow = SAudioExportProgressWindow::OpenProgressWindow(
				Assets,
				FOnAudioExporterMonitorChanged::CreateLambda([](bool) {}),
				FOnAudioExporterCancelRequested::CreateLambda([ProgressState]()
				{
					if (ProgressState->Manager.IsValid())
					{
						ProgressState->Manager->CancelExporting(false);
					}
				}),
				FOnAudioExporterRequestRecordingSubmix::CreateLambda([ProgressState]() -> USoundSubmix*
				{
					return ProgressState->Manager.IsValid() ? ProgressState->Manager->GetRecordingSubmix() : nullptr;
				}),
				FOnAudioExporterRequestWorld::CreateLambda([]() -> UWorld*
				{
					return GWorld;
				}),
				FOnAudioExporterIsExportActive::CreateLambda([ProgressState]()
				{
					return ProgressState->Manager.IsValid() && ProgressState->Manager->IsExporting();
				}));
			ProgressState->ProgressWindow = ProgressWindow;
			FSlateApplication::Get().AddWindow(ProgressWindow);
			AudioExporterUITestUtils::TickSlateFrames();
		});

	UPackage* CuePackage = NewObject<UPackage>(nullptr, TEXT("/Temp/AudioExporterUITest/SyncSettleBeforeStartedCue"), RF_Public);
	USoundCue* SoundCue = CuePackage
		? NewObject<USoundCue>(CuePackage, TEXT("SyncSettleBeforeStartedCue"), RF_Public)
		: nullptr;
	TestNotNull(TEXT("The manager-order test should create a transient SoundCue asset."), SoundCue);
	if (!SoundCue)
	{
		ProgressState->Cleanup();
		return false;
	}
	SoundCue->AddToRoot();
	USoundNodeWavePlayer* EmptyWavePlayer = NewObject<USoundNodeWavePlayer>(SoundCue, TEXT("EmptyWavePlayer"));
	SoundCue->FirstNode = EmptyWavePlayer;

	// Part A -- ordering through the REAL manager. A healthy asset runs the realtime renderer, so
	// the session is genuinely asynchronous: Started (and the window it opens) must precede any
	// terminal, and the cancel terminal must arrive exactly once with no replay a frame later.
	ProgressState->Manager->StartExporting(TArray<FAssetData>{ AudioExporterUITestUtils::MakeTransientAssetData(SoundCue) });

	TestEqual(TEXT("The manager start path should broadcast Started once before the runtime call."), ProgressState->StartedEvents, 1);
	TestTrue(TEXT("The progress window must be live before any terminal is delivered."), ProgressState->ProgressWindow.IsValid());
	TestEqual(TEXT("An asynchronous session must not settle a terminal during the start call."), ProgressState->TerminalEvents, 0);

	ProgressState->Manager->CancelExporting(false);
	AudioExporterUITestUtils::TickSlateFrames();
	TestEqual(TEXT("Cancelling the live session should deliver one structured terminal."), ProgressState->TerminalEvents, 1);
	TestEqual(TEXT("Cancelling the live session should deliver one runtime legacy terminal."), ProgressState->LegacyFinishedEvents, 1);
	TestEqual(TEXT("The manager should forward one legacy terminal to its listeners."), ProgressState->ManagerFinishedEvents, 1);
	TestEqual(TEXT("The live progress window should receive the forwarded terminal."), ProgressState->PostOpenFinishedForwards, 1);

	// A frame is enough to expose any resurrected replay machinery: counts must remain unchanged.
	FTSTicker::GetCoreTicker().Tick(0.0f);
	AudioExporterUITestUtils::TickSlateFrames();
	TestEqual(TEXT("The terminal pair must remain exactly once after the next engine frame."), ProgressState->TerminalEvents, 1);
	TestEqual(TEXT("The legacy terminal must remain exactly once after the next engine frame."), ProgressState->LegacyFinishedEvents, 1);
	TestEqual(TEXT("The manager legacy forward must remain exactly once after the next engine frame."), ProgressState->ManagerFinishedEvents, 1);

	// Part B -- the synchronous settle, through the real subsystem entry point.
	// A named entry whose sound is unavailable settles terminally INSIDE StartExport under
	// unattended automation; with listeners bound up front (the manager's new order), that settle
	// must deliver exactly one structured + one legacy terminal, and a later engine frame must not
	// deliver a second pair (the deleted replay cache would have).
	ProgressState->TerminalEvents = 0;
	ProgressState->LegacyFinishedEvents = 0;
	ProgressState->ManagerFinishedEvents = 0;

	FAudioExportSoundEntry UnavailableEntry;
	UnavailableEntry.Sound = nullptr;
	UnavailableEntry.SoundName = TEXT("SyncSettleBeforeStartedEntry");
	const bool bSyncAccepted = RuntimeSubsystem->StartExport(
		GWorld, TArray<FAudioExportSoundEntry>{ UnavailableEntry }, FAudioExporterConfig(), FString());
	TestTrue(TEXT("The unavailable-entry request must be accepted into a real session."), bSyncAccepted);
	TestTrue(TEXT("The unavailable-entry session must settle synchronously inside the start call."),
		ProgressState->TerminalEvents == 1 && ProgressState->TerminalResult.bAcceptedStart && !RuntimeSubsystem->IsExporting());
	TestEqual(TEXT("A synchronous settle should deliver one structured terminal."), ProgressState->TerminalEvents, 1);
	TestEqual(TEXT("A synchronous settle should deliver one runtime legacy terminal."), ProgressState->LegacyFinishedEvents, 1);

	FTSTicker::GetCoreTicker().Tick(0.0f);
	AudioExporterUITestUtils::TickSlateFrames();
	TestEqual(TEXT("The synchronous terminal must remain exactly once after the next engine frame."), ProgressState->TerminalEvents, 1);
	TestEqual(TEXT("The synchronous legacy terminal must remain exactly once after the next engine frame."), ProgressState->LegacyFinishedEvents, 1);

	const TSharedPtr<SWindow> ProgressWindow = ProgressState->ProgressWindow.Pin();
	if (ProgressWindow.IsValid() && AudioExporterUITestUtils::IsWindowStillRegisteredWithSlate(ProgressWindow.ToSharedRef()))
	{
		AudioExporterUITestUtils::DestroyWindow(ProgressWindow.ToSharedRef());
	}
	ProgressState->Cleanup();
	SoundCue->RemoveFromRoot();
	return true;
}

// KILLS: deleting the production Esc binding (SAudioExportProgressWindow::OnKeyDown ->
// RequestWindowCloseOrCancel -> HandleRequestDestroyWindow) now fails this test three independent
// ways: the Escape FReply is no longer Handled, the cancel delegate is never invoked (active
// scenario), and the inactive window is never deregistered from Slate.
//
// Esc closes/cancels every secondary window. Drives Escape through the exact
// OnKeyDown a real keypress reaches, not just the Cancel
// button's already-covered handler above.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterProgressWindowEscCancelsTest,
	"AudioFactory.AudioExporter.UI.ProgressWindowEscCancels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterProgressWindowEscCancelsTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Progress-window Esc test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	int32 CancelRequests = 0;
	const TSharedRef<SWindow> Window = SAudioExportProgressWindow::OpenProgressWindow(
		TArray<FAssetData>(),
		FOnAudioExporterMonitorChanged::CreateLambda([](bool) {}),
		FOnAudioExporterCancelRequested::CreateLambda([&CancelRequests]()
		{
			++CancelRequests;
		}),
		FOnAudioExporterRequestRecordingSubmix::CreateLambda([]() -> USoundSubmix*
		{
			return nullptr;
		}),
		FOnAudioExporterRequestWorld::CreateLambda([]() -> UWorld*
		{
			return nullptr;
		}),
		FOnAudioExporterIsExportActive::CreateLambda([]()
		{
			return true;
		}));
	FSlateApplication::Get().AddWindow(Window);
	AudioExporterUITestUtils::TickSlateFrames();

	const TSharedRef<SAudioExportProgressWindow> Widget = AudioExporterUITestUtils::GetProgressWidget(Window);
	const FKeyEvent EscapeEvent(EKeys::Escape, FModifierKeysState(), 0, false, 0, 0);
	const FReply EscapeReply = Widget->OnKeyDown(FGeometry(), EscapeEvent);
	AudioExporterUITestUtils::TickSlateFrames();

	TestTrue(TEXT("Esc on the active progress window should be handled by the production Esc binding rather than falling through unhandled."), EscapeReply.IsEventHandled());
	TestEqual(TEXT("Esc on the active progress window should request cancellation exactly once."), CancelRequests, 1);
	TestTrue(TEXT("Esc during an active export must cancel without tearing down the in-flight progress window."),
		AudioExporterUITestUtils::IsWindowStillRegisteredWithSlate(Window));

	Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
	AudioExporterUITestUtils::DestroyWindow(Window);

	// Inactive export: the same Esc handler routes through the same terminal-aware close logic as
	// the OS close button, tearing the window down WITHOUT ever confirming a cancellation.
	int32 InactiveCancelRequests = 0;
	const TSharedRef<SWindow> InactiveWindow = SAudioExportProgressWindow::OpenProgressWindow(
		TArray<FAssetData>(),
		FOnAudioExporterMonitorChanged::CreateLambda([](bool) {}),
		FOnAudioExporterCancelRequested::CreateLambda([&InactiveCancelRequests]()
		{
			++InactiveCancelRequests;
		}),
		FOnAudioExporterRequestRecordingSubmix::CreateLambda([]() -> USoundSubmix*
		{
			return nullptr;
		}),
		FOnAudioExporterRequestWorld::CreateLambda([]() -> UWorld*
		{
			return nullptr;
		}),
		FOnAudioExporterIsExportActive::CreateLambda([]()
		{
			return false;
		}));
	FSlateApplication::Get().AddWindow(InactiveWindow);
	AudioExporterUITestUtils::TickSlateFrames();

	const TSharedRef<SAudioExportProgressWindow> InactiveWidget = AudioExporterUITestUtils::GetProgressWidget(InactiveWindow);
	const FReply InactiveEscapeReply = InactiveWidget->OnKeyDown(FGeometry(), EscapeEvent);
	AudioExporterUITestUtils::TickSlateFrames();

	TestTrue(TEXT("Esc on the inactive progress window should be handled by the production Esc binding rather than falling through unhandled."), InactiveEscapeReply.IsEventHandled());
	TestFalse(TEXT("Esc on the inactive progress window should tear it down like the OS close button does."),
		AudioExporterUITestUtils::IsWindowStillRegisteredWithSlate(InactiveWindow));
	TestEqual(TEXT("Esc on the inactive progress window must close it without requesting export cancellation."), InactiveCancelRequests, 0);

	if (AudioExporterUITestUtils::IsWindowStillRegisteredWithSlate(InactiveWindow))
	{
		InactiveWindow->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
		AudioExporterUITestUtils::DestroyWindow(InactiveWindow);
	}
	return true;
}

// The status strip must carry real severity, not a fixed Neutral value, once a
// failure has been recorded and again once the run reaches a non-success terminal state.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterProgressWindowStatusSeverityTest,
	"AudioFactory.AudioExporter.UI.ProgressWindowStatusSeverity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterProgressWindowStatusSeverityTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Status-severity test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	const TSharedRef<SWindow> Window = AudioExporterUITestUtils::OpenNullCallbackProgressWindow();
	FSlateApplication::Get().AddWindow(Window);
	AudioExporterUITestUtils::TickSlateFrames();
	const TSharedRef<SAudioExportProgressWindow> Widget = AudioExporterUITestUtils::GetProgressWidget(Window);

	TestEqual(TEXT("A clean run in progress should read Neutral."), Widget->GetStatusSeverityForTesting(), AudioExporter::EAudioFactoryStatusSeverity::Neutral);

	FAudioExportProgressInfo FailureTick;
	FailureTick.CurrentSound = 1;
	FailureTick.TotalSounds = 2;
	FailureTick.bHasResult = true;
	FailureTick.bSucceeded = false;
	FailureTick.FailedFiles = 1;
	FailureTick.ResultMessage = TEXT("Synthetic failure");
	Widget->UpdateProgressUI(FailureTick);
	TestEqual(TEXT("A running batch with a recorded failure should read Warning, not the fixed Neutral value."), Widget->GetStatusSeverityForTesting(), AudioExporter::EAudioFactoryStatusSeverity::Warning);

	Widget->OnExportFinishedProgress(FPaths::ProjectSavedDir());
	TestEqual(TEXT("A zero-success terminal outcome should read Error."), Widget->GetStatusSeverityForTesting(), AudioExporter::EAudioFactoryStatusSeverity::Error);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterProgressWindowFailureRowIsStickyTest,
	"AudioFactory.AudioExporter.UI.ProgressWindowFailureRowIsSticky",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterProgressWindowFailureRowIsStickyTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Failure-row stickiness test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	FAssetData Asset;
	Asset.AssetName = FName(TEXT("FailureThenSuccessAsset"));
	TArray<FAssetData> Assets;
	Assets.Add(Asset);

	const TSharedRef<SWindow> Window = SAudioExportProgressWindow::OpenProgressWindow(
		Assets,
		FOnAudioExporterMonitorChanged::CreateLambda([](bool) {}),
		FOnAudioExporterCancelRequested::CreateLambda([]() {}),
		FOnAudioExporterRequestRecordingSubmix::CreateLambda([]() -> USoundSubmix*
		{
			return nullptr;
		}),
		FOnAudioExporterRequestWorld::CreateLambda([]() -> UWorld*
		{
			return nullptr;
		}),
		FOnAudioExporterIsExportActive::CreateLambda([]()
		{
			return true;
		}));
	FSlateApplication::Get().AddWindow(Window);
	AudioExporterUITestUtils::TickSlateFrames();

	const TSharedRef<SAudioExportProgressWindow> Widget = AudioExporterUITestUtils::GetProgressWidget(Window);

	FAudioExportProgressInfo Failure;
	Failure.CurrentSound = 1;
	Failure.TotalSounds = 1;
	Failure.TotalExpectedFiles = 2;
	Failure.CurrentVariation = 1;
	Failure.TotalVariations = 2;
	Failure.CurrentSoundName = TEXT("FailureThenSuccessAsset");
	Failure.bHasResult = true;
	Failure.bSucceeded = false;
	Failure.FailedFiles = 1;
	Failure.ResultMessage = TEXT("Render failed: variation 1");
	Widget->UpdateProgressUI(Failure);

	TestEqual(TEXT("The failure-row fixture should create exactly one asset row."), Widget->GetFileLogEntryCountForTesting(), 1);
	const FString FailureText = Widget->GetFileLogTextForTesting(0);
	TestEqual(TEXT("A failed variation should mark its asset row Error."),
		Widget->GetFileLogSeverityForTesting(0), AudioExporter::EAudioFactoryStatusSeverity::Error);
	TestTrue(TEXT("A failed variation should put its failure text in the asset row."), FailureText.Contains(TEXT("Render failed: variation 1")));

	FAudioExportProgressInfo LaterVariationPending = Failure;
	LaterVariationPending.CurrentVariation = 2;
	LaterVariationPending.bHasResult = false;
	LaterVariationPending.bSucceeded = false;
	LaterVariationPending.ResultMessage.Empty();
	Widget->UpdateProgressUI(LaterVariationPending);
	TestEqual(TEXT("A later variation in progress must not clear the earlier failure severity."),
		Widget->GetFileLogSeverityForTesting(0), AudioExporter::EAudioFactoryStatusSeverity::Error);
	TestEqual(TEXT("A later variation in progress must not clear the earlier failure text."),
		Widget->GetFileLogTextForTesting(0), FailureText);

	FAudioExportProgressInfo LaterSuccess = LaterVariationPending;
	LaterSuccess.bHasResult = true;
	LaterSuccess.bSucceeded = true;
	LaterSuccess.ExportedFiles = 1;
	LaterSuccess.ResultMessage = TEXT("WAV export completed");
	Widget->UpdateProgressUI(LaterSuccess);
	TestEqual(TEXT("A later successful variation must not overwrite the asset row's failure severity."),
		Widget->GetFileLogSeverityForTesting(0), AudioExporter::EAudioFactoryStatusSeverity::Error);
	TestEqual(TEXT("A later successful variation must not overwrite the asset row's failure text."),
		Widget->GetFileLogTextForTesting(0), FailureText);

	Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
	AudioExporterUITestUtils::DestroyWindow(Window);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterModuleBusyRefusalKeepsSingletonTest,
	"AudioFactory.AudioExporter.UI.ModuleEntry.BusyRefusalKeepsSingleton",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterModuleBusyRefusalKeepsSingletonTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Audio Exporter module-entry busy test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	USoundWave* TestSound = NewObject<USoundWave>(GetTransientPackage(), TEXT("AudioExporterModuleBusySound"));
	TestNotNull(TEXT("A real SoundWave should be available for module-entry busy refusal."), TestSound);
	if (!TestSound)
	{
		return false;
	}

	FAudioExporterModule& Module = FModuleManager::LoadModuleChecked<FAudioExporterModule>(TEXT("AudioExporter"));
	const FAssetData Asset = AudioExporterUITestUtils::MakeTransientAssetData(TestSound);
	Module.SetBusyForTesting(false);
	Module.OpenSettingsWindowForTesting({ Asset });
	AudioExporterUITestUtils::TickSlateFrames();
	const TSharedPtr<SWindow> FirstWindow = Module.GetSettingsWindowForTesting();
	TestTrue(TEXT("The module entry should create the first settings window."), FirstWindow.IsValid());
	if (!FirstWindow.IsValid())
	{
		return false;
	}
	const FString BeforeBusyRenderedText = AudioExporterUITestUtils::RenderedText(FirstWindow->GetContent());
	const TOptional<AudioExporterUITestUtils::FPrimaryActionState> BeforeBusyPrimaryState =
		AudioExporterUITestUtils::GetPrimaryActionState(FirstWindow->GetContent());
	TestTrue(TEXT("The existing settings window must expose its real primary button before refusal."), BeforeBusyPrimaryState.IsSet());

	Module.SetBusyForTesting(true);
	Module.OpenSettingsWindowForTesting({ Asset });
	AudioExporterUITestUtils::TickSlateFrames();
	const TSharedPtr<SWindow> SecondWindow = Module.GetSettingsWindowForTesting();
	FString StatusStripText;
	const bool bFoundStatusStrip = AudioExporterUITestUtils::FindStatusStripText(FirstWindow->GetContent(), StatusStripText);
	const FString AfterBusyRenderedText = AudioExporterUITestUtils::RenderedText(FirstWindow->GetContent());
	const TOptional<AudioExporterUITestUtils::FPrimaryActionState> AfterBusyPrimaryState =
		AudioExporterUITestUtils::GetPrimaryActionState(FirstWindow->GetContent());
	TestTrue(TEXT("A busy module entry must keep the same settings window."), SecondWindow == FirstWindow);
	TestTrue(TEXT("A busy module entry must keep the existing settings window registered with Slate."),
		AudioExporterUITestUtils::IsWindowStillRegisteredWithSlate(FirstWindow.ToSharedRef()));
	TestEqual(TEXT("A busy module entry must not mutate the existing settings UI."), AfterBusyRenderedText, BeforeBusyRenderedText);
	TestTrue(TEXT("The existing settings window must expose its real primary button after refusal."), AfterBusyPrimaryState.IsSet());
	if (BeforeBusyPrimaryState.IsSet() && AfterBusyPrimaryState.IsSet())
	{
		TestEqual(TEXT("A busy refusal must not change the primary button enabled state."),
			AfterBusyPrimaryState->bIsEnabled, BeforeBusyPrimaryState->bIsEnabled);
		TestEqual(TEXT("A busy refusal must not change whether the primary button has a tooltip."),
			AfterBusyPrimaryState->bHasToolTip, BeforeBusyPrimaryState->bHasToolTip);
		TestEqual(TEXT("A busy refusal must not change the primary button tooltip."),
			AfterBusyPrimaryState->ToolTipText, BeforeBusyPrimaryState->ToolTipText);
	}
	TestFalse(TEXT("A busy module entry must not create a settings status strip."), bFoundStatusStrip);
	Module.SetBusyForTesting(false);
	AudioExporterUITestUtils::DestroyWindow(FirstWindow.ToSharedRef());
	return true;
}

// Discovery, counting, class-label microcopy, and the type icon for
// a non-procedural SoundWave, driven through the real production filter/resolver functions
// OpenSettingsWindow itself uses (not a re-implemented copy).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterSettingsWindowSoundWaveSupportTest,
	"AudioFactory.AudioExporter.UI.SettingsWindowSoundWaveSupport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterSettingsWindowSoundWaveSupportTest::RunTest(const FString& Parameters)
{
	auto MakeTransientAssetData = [](UObject* Object) -> FAssetData
	{
		UPackage* Package = Object->GetOutermost();
		return FAssetData(
			Package->GetFName(),
			FName(*FPackageName::GetLongPackagePath(Package->GetName())),
			Object->GetFName(),
			Object->GetClass()->GetClassPathName());
	};

	UPackage* WavePackage = NewObject<UPackage>(nullptr, TEXT("/Temp/AudioExporterUITest/SoundWaveSupportWave"), RF_Public);
	USoundWave* Wave = NewObject<USoundWave>(WavePackage, TEXT("SoundWaveSupportWave"), RF_Public);
	TestNotNull(TEXT("A transient static SoundWave should be created."), Wave);

	UPackage* CuePackage = NewObject<UPackage>(nullptr, TEXT("/Temp/AudioExporterUITest/SoundWaveSupportCue"), RF_Public);
	USoundCue* Cue = NewObject<USoundCue>(CuePackage, TEXT("SoundWaveSupportCue"), RF_Public);
	TestNotNull(TEXT("A transient SoundCue should be created."), Cue);

	if (!Wave || !Cue)
	{
		return false;
	}

	// Discovery + count: the real filter (the same one OpenSettingsWindow drives) must not drop
	// the SoundWave, and must count it separately from the SoundCue.
	TArray<FAssetData> Assets;
	Assets.Add(MakeTransientAssetData(Wave));
	Assets.Add(MakeTransientAssetData(Cue));
	const SAudioExporterWindow::FSupportedAssetCountsForTesting Counts = SAudioExporterWindow::GetSupportedAssetCountsForTesting(Assets);
	TestEqual(TEXT("Both the SoundWave and the SoundCue should be discovered as supported."), Counts.NumSupported, 2);
	TestEqual(TEXT("The SoundWave should be counted once, separately from SoundCues."), Counts.NumSoundWaves, 1);
	TestEqual(TEXT("The SoundCue should be counted once, separately from SoundWaves."), Counts.NumSoundCues, 1);
	TestEqual(TEXT("MetaSound count should stay zero when none were selected."), Counts.NumMetaSounds, 0);

	// Icon + microcopy: the real per-asset resolver functions BuildSelectedAssetRows uses.
	const FAssetData WaveAssetData = MakeTransientAssetData(Wave);
	TestEqual(TEXT("A SoundWave should resolve to the SoundWave class icon, not a generic/empty one."),
		SAudioExporterWindow::GetAssetIconNameForTesting(WaveAssetData), AudioExporter::FAudioFactoryClassIconNames::SoundWave());
	TestEqual(TEXT("A SoundWave should resolve to the 'SoundWave' class label."),
		SAudioExporterWindow::GetAssetClassLabelForTesting(WaveAssetData).ToString(), FString(TEXT("SoundWave")));

	return true;
}

// Title for the Selected Assets window. There is no public entry point to this window through
// the real settings-window flow (it only opens via the "Show Selected" footer action nested
// inside the adapter-built frame), so this drives the exact same window-construction code
// (OpenSelectedAssetsWindowForTesting) the footer action calls.
//
// Drives Escape through the existing Slate focus helper so this asserts the real
// Selected-Assets window path rather than a direct wrapper call.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioExporterSelectedAssetsWindowTitleTest,
	"AudioFactory.AudioExporter.UI.SelectedAssetsWindowTitle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioExporterSelectedAssetsWindowTitleTest::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Selected Assets window test is unavailable: SlateApplication was not initialized."));
		return false;
	}

	const TSharedRef<SWindow> Window = SAudioExporterWindow::OpenSelectedAssetsWindowForTesting(TArray<FAssetData>(), nullptr);
	AudioExporterUITestUtils::TickSlateFrames();

	TestEqual(TEXT("The Selected Assets window should use the expected title."), Window->GetTitle().ToString(), FString(TEXT("Audio Exporter — Selected Assets")));
	TestTrue(TEXT("The Selected Assets window should become visible after being shown."), Window->IsVisible());
	AudioExporterUITestUtils::SimulateEscapeKeyPress(Window);
	TestFalse(TEXT("Escape should close the Selected Assets window."),
		AudioExporterUITestUtils::IsWindowStillRegisteredWithSlate(Window));

	if (AudioExporterUITestUtils::IsWindowStillRegisteredWithSlate(Window))
	{
		AudioExporterUITestUtils::DestroyWindow(Window);
	}
	return true;
}
