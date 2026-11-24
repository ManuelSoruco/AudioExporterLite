// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "AudioExporter.h"
#include "Misc/CommandLine.h"

#include "AudioExporterRuntimeSubsystem.h"
#include "Utilities/AudioExporterManager.h"
#include "Utilities/AudioExporterSettings.h"
#include "Utilities/AudioExporterLogger.h"
#include "Utilities/AudioExporterUtils.h"
#include "Dialogs/AudioExporterProgressWindow.h"
#include "Dialogs/AudioExporterWindow.h"
#include "Dialogs/AudioFactoryPluginWindowAdapter.h"
// The shared adapter's public alias header is fully namespaced; import just the
// two symbols this module file uses.
using AudioExporter::FAudioFactoryPluginWindowAdapter;
using AudioExporter::EAudioFactoryProductId;
#include "ContentBrowserMenuContexts.h"
#include "Framework/Docking/TabManager.h"
#include "DesktopPlatformModule.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/SWindow.h"

#define LOCTEXT_NAMESPACE "FAudioExporterModule"

void FAudioExporterModule::StartupModule()
{
	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FAudioExporterModule::RegisterToolMenus));

#if WITH_DEV_AUTOMATION_TESTS
	// The descriptor pins this module to LoadingPhase None so a normal editor launch
	// never lists a *Tests module. This force-load would silently defeat that claim, so it is
	// gated on the same signal the descriptor side uses: -ExecCmds= is present on every
	// automation invocation and never on an interactive launch.
	if (FCString::Strifind(FCommandLine::Get(), TEXT("-ExecCmds=")) != nullptr)
	{
		FModuleManager::LoadModulePtr<IModuleInterface>(TEXT("AudioExporterTests"));
	}
#endif

	ExportManager = MakeUnique<FAudioExportManager>();
	ExportManager->Initialize();
	ExportManager->OnAudioExportStarted.AddRaw(this, &FAudioExporterModule::OnAudioExportStarted);
	ExportManager->OnAudioExportProgressUpdated.AddRaw(this, &FAudioExporterModule::OnAudioExportProgressUpdated);
	ExportManager->OnAudioExportFinished.AddRaw(this, &FAudioExporterModule::OnAudioExportFinished);
}

void FAudioExporterModule::ShutdownModule()
{
	UToolMenus::UnRegisterStartupCallback(this);
	if (UToolMenus::IsToolMenuUIEnabled())
	{
		UToolMenus::UnregisterOwner(this);
	}

	if (SettingsWindow.IsValid())
	{
		if (FSlateApplication::IsInitialized())
		{
			SettingsWindow.Pin()->RequestDestroyWindow();
		}
		else
		{
			UE_LOG(LogAudioExporter, Error, TEXT("%s: Slate was not initialized while trying to destroy the Audio Exporter settings window."), *FString(__FUNCTION__));
		}
		SettingsWindow.Reset();
	}

	if (ProgressParentWindow.IsValid())
	{
		if (FSlateApplication::IsInitialized())
		{
			// The progress window installs a RequestDestroyWindowOverride that CANCELS an in-flight
			// export instead of closing, which is the right interactive behaviour but the wrong one
			// here: at module shutdown there is no "cancel and keep the window open" option, and a
			// surviving SWindow whose vtable lives in this unloading module crashes on the next Slate
			// tick. Clear the override so the destroy actually happens.
			const TSharedPtr<SWindow> Window = ProgressParentWindow.Pin();
			Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
			Window->RequestDestroyWindow();
		}
		else
		{
			UE_LOG(LogAudioExporter, Error, TEXT("%s: Slate was not initialized while trying to destroy the Audio Exporter progress window."), *FString(__FUNCTION__));
		}
		ProgressParentWindow.Reset();
	}

	if (ExportManager.IsValid())
	{
		ExportManager->OnAudioExportStarted.RemoveAll(this);
		ExportManager->OnAudioExportProgressUpdated.RemoveAll(this);
		ExportManager->OnAudioExportFinished.RemoveAll(this);

		if (IsValid(GEngine) && ExportManager->IsExporting())
		{
			ExportManager->CancelExporting(false);
		}

		ExportManager.Reset();
	}

}

void FAudioExporterModule::RegisterToolMenus()
{
	// Everything registered below must be attributed to this module, or ShutdownModule's
	// UnregisterOwner(this) has nothing to remove and the entries outlive the module.
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("ContentBrowser.AssetContextMenu");
	if (!Menu)
	{
		UE_LOG(LogAudioExporter, Error, TEXT("%s: Failed to extend the Content Browser asset context menu."), *FString(__FUNCTION__));
		return;
	}

	FToolMenuSection& Section = Menu->FindOrAddSection("AudioExporter");
	Section.InsertPosition = FToolMenuInsert("CommonAssetActions", EToolMenuInsertType::After);
	Section.Label = LOCTEXT("AudioExporterSection", "Audio Exporter");

	Section.AddDynamicEntry("AudioExporterBaseEntry", FNewToolMenuSectionDelegate::CreateLambda([this](FToolMenuSection& InSection)
	{
		if (UContentBrowserAssetContextMenuContext* Context = InSection.Context.FindContext<UContentBrowserAssetContextMenuContext>())
		{
			const TArray<FAssetData> SelectedAssets = Context->SelectedAssets;
			const bool bHasSupportedAsset = SelectedAssets.ContainsByPredicate([](const FAssetData& Asset)
			{
				return IsAudioExporterSupportedAsset(Asset);
			});

			if (bHasSupportedAsset)
			{
				InSection.AddMenuEntry(
					"ExportProceduralAudio",
					LOCTEXT("AudioExporter_Export", "Export Audio..."),
					LOCTEXT("AudioExporter_ExportTooltip", "Opens the Audio Exporter tool for the selected audio assets."),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.ExportAll"),
					FUIAction(FExecuteAction::CreateLambda([this, SelectedAssets]()
					{
						OpenSettingsWindow(SelectedAssets);
					}))
				);
			}
		}
	}));
}

FAudioExportManager* FAudioExporterModule::GetExportManager(const TCHAR* FunctionName) const
{
	if (!ExportManager.IsValid())
	{
		UE_LOG(LogAudioExporter, Error, TEXT("%s: AudioExporter manager was unavailable."), FunctionName);
		return nullptr;
	}

	return ExportManager.Get();
}

#if WITH_DEV_AUTOMATION_TESTS
void FAudioExporterModule::OpenSettingsWindowForTesting(const TArray<FAssetData>& SelectedAssets)
{
	OpenSettingsWindow(SelectedAssets);
}

void FAudioExporterModule::SetBusyForTesting(bool bInBusy)
{
	if (ExportManager.IsValid())
	{
		ExportManager->SetExportingForTesting(bInBusy);
	}
}
#endif

void FAudioExporterModule::OpenSettingsWindow(const TArray<FAssetData>& SelectedAssets)
{
	// Re-target the live settings frame for this invocation; busy export sessions refuse the
	// re-target and leave the existing window, staged edits, and persisted settings untouched.
	if (!FSlateApplication::IsInitialized())
	{
		UE_LOG(LogAudioExporter, Error, TEXT("%s: Slate was not initialized when the export window was requested."), *FString(__FUNCTION__));
		return;
	}

	const TSharedPtr<SWindow> ExistingSettingsWindow =
		FAudioFactoryPluginWindowAdapter::FindLiveSettingsWindow(EAudioFactoryProductId::AudioExporter);
	if (ExistingSettingsWindow.IsValid() && ExportManager.IsValid() && ExportManager->IsExporting())
	{
		FAudioFactoryPluginWindowAdapter::ShowLiveSettingsFailure(
			EAudioFactoryProductId::AudioExporter,
			LOCTEXT("SettingsRetargetWhileExporting", "Audio Exporter is already exporting. The existing settings window remains open."));
		SettingsWindow = ExistingSettingsWindow;
		return;
	}
	const bool bSettingsWindowWasLive = ExistingSettingsWindow.IsValid();

	const TSharedRef<SWindow> Window = SAudioExporterWindow::OpenSettingsWindow(
		SelectedAssets,
		FOnAudioExporterRequested::CreateRaw(this, &FAudioExporterModule::StartExportFromWindow),
		FOnAudioExporterRequestExportDirectory::CreateRaw(this, &FAudioExporterModule::PromptForExportDirectory));
	// Parent to the editor root rather than adding bare, so the window behaves as a native
	// child: stays above its owner, minimises with it, and cannot be lost behind the editor.
	if (!bSettingsWindowWasLive)
	{
		if (const TSharedPtr<SWindow> RootWindow = FGlobalTabmanager::Get()->GetRootWindow())
		{
			FSlateApplication::Get().AddWindowAsNativeChild(Window, RootWindow.ToSharedRef());
		}
		else
		{
			FSlateApplication::Get().AddWindow(Window);
		}
	}
	Window->BringToFront(true);
	SettingsWindow = Window;
}

void FAudioExporterModule::StartExportFromWindow(const TArray<FAssetData>& SelectedAssets)
{
	FAudioExportManager* Manager = GetExportManager(*FString(__FUNCTION__));
	if (Manager == nullptr)
	{
		return;
	}

	Manager->StartExporting(SelectedAssets);
}

FString FAudioExporterModule::PromptForExportDirectory(const FString& StartingPath) const
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (DesktopPlatform == nullptr)
	{
		UE_LOG(LogAudioExporter, Error, TEXT("%s: Desktop platform services were unavailable for export-path selection."), *FString(__FUNCTION__));
		return FString();
	}

	// Start the dialog at the path the settings window is currently displaying: the staged
	// ExportDirectory when the user set one, or the default Saved/AudioExporter location. The
	// persisted saved object is intentionally not consulted here -- it can differ from the
	// staged value the row is showing, which is what the user is acting on.
	const FString DefaultPath = !StartingPath.IsEmpty()
		? StartingPath
		: FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AudioExporter"));

	FString OutFolder;
	const void* ParentHandle = nullptr;
	if (SettingsWindow.IsValid())
	{
		if (const TSharedPtr<SWindow> ParentWindow = SettingsWindow.Pin(); ParentWindow.IsValid() && ParentWindow->GetNativeWindow().IsValid())
		{
			ParentHandle = ParentWindow->GetNativeWindow()->GetOSWindowHandle();
		}
	}

	if (DesktopPlatform->OpenDirectoryDialog(ParentHandle, LOCTEXT("ChooseExportDir", "Choose Export Directory").ToString(), DefaultPath, OutFolder))
	{
		return FPaths::ConvertRelativePathToFull(OutFolder);
	}

	return FString();
}

void FAudioExporterModule::OnAudioExportStarted(const TArray<FAssetData>& InAssets)
{
	// Slate check first, for the same reason as OpenSettingsWindow above.
	if (!FSlateApplication::IsInitialized())
	{
		UE_LOG(LogAudioExporter, Error, TEXT("%s: Slate was not initialized when the progress window was requested."), *FString(__FUNCTION__));
		return;
	}

	if (ProgressParentWindow.IsValid())
	{
		ProgressParentWindow.Pin()->RequestDestroyWindow();
		ProgressParentWindow.Reset();
	}

	TSharedRef<SWindow> NewWindow = SAudioExportProgressWindow::OpenProgressWindow(
		InAssets,
		FOnAudioExporterMonitorChanged::CreateLambda([this](const bool bMonitoring)
		{
			if (FAudioExportManager* Manager = GetExportManager(*FString(__FUNCTION__)))
			{
				Manager->SetMonitoring(bMonitoring);
			}
			else
			{
				UE_LOG(LogAudioExporter, Warning, TEXT("%s: AudioExporter manager was unavailable while toggling export monitoring."), *FString(__FUNCTION__));
			}
		}),
		FOnAudioExporterCancelRequested::CreateLambda([this]()
		{
			if (FAudioExportManager* Manager = GetExportManager(*FString(__FUNCTION__)))
			{
				// bDeleteFiles is intentionally false: variations already written to the session
				// folder are kept on cancel so the user does not lose completed work. Partial
				// session folders are therefore retained by design; the unique timestamped/
				// suffixed folder name keeps them from colliding with later sessions.
				Manager->CancelExporting(false);
			}
			else
			{
				UE_LOG(LogAudioExporter, Warning, TEXT("%s: AudioExporter manager was unavailable while cancelling export."), *FString(__FUNCTION__));
			}
		}),
		FOnAudioExporterRequestRecordingSubmix::CreateLambda([this]() -> USoundSubmix*
		{
			if (FAudioExportManager* Manager = GetExportManager(*FString(__FUNCTION__)))
			{
				return Manager->GetRecordingSubmix();
			}

			return nullptr;
		}),
		FOnAudioExporterRequestWorld::CreateLambda([]() -> UWorld*
		{
			if (!IsValid(GEditor))
			{
				return nullptr;
			}

			return GEditor->GetEditorWorldContext().World();
		}),
		FOnAudioExporterIsExportActive::CreateLambda([this]() -> bool
		{
			if (FAudioExportManager* Manager = GetExportManager(*FString(__FUNCTION__)))
			{
				return Manager->IsExporting();
			}

			return false;
		}));
	// Native-child parenting, same reason as the settings window above: the progress window
	// must stay with its owner rather than floating free behind the editor.
	if (const TSharedPtr<SWindow> RootWindow = FGlobalTabmanager::Get()->GetRootWindow())
	{
		FSlateApplication::Get().AddWindowAsNativeChild(NewWindow, RootWindow.ToSharedRef());
	}
	else
	{
		FSlateApplication::Get().AddWindow(NewWindow);
	}
	ProgressParentWindow = NewWindow;
}

void FAudioExporterModule::OnAudioExportProgressUpdated(const FAudioExportProgressInfo& ProgressInfo)
{
	if (!ProgressParentWindow.IsValid())
	{
		return;
	}

	const TSharedPtr<SWindow> Window = ProgressParentWindow.Pin();
	if (!Window.IsValid())
	{
		return;
	}

	const TSharedPtr<SAudioExportProgressWindow> ProgressWidget = StaticCastSharedRef<SAudioExportProgressWindow>(Window->GetContent());
	if (ProgressWidget.IsValid())
	{
		ProgressWidget->UpdateProgressUI(ProgressInfo);
	}
}

void FAudioExporterModule::OnAudioExportFinished(const FString& ExportPath)
{
	if (!ProgressParentWindow.IsValid())
	{
		return;
	}

	const TSharedPtr<SWindow> Window = ProgressParentWindow.Pin();
	if (!Window.IsValid())
	{
		ProgressParentWindow.Reset();
		return;
	}

	const TSharedPtr<SAudioExportProgressWindow> ProgressWidget = StaticCastSharedRef<SAudioExportProgressWindow>(Window->GetContent());
	if (ProgressWidget.IsValid())
	{
		ProgressWidget->OnExportFinishedProgress(ExportPath);
	}

	ProgressParentWindow.Reset();
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FAudioExporterModule, AudioExporter);
