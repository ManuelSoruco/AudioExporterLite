// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "AudioExporterUI.h"

#include "AudioFactoryUI.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "Runtime/Launch/Resources/Version.h"

using namespace AudioExporter;

#define LOCTEXT_NAMESPACE "FAudioExporterUIModule"

void FAudioExporterUIModule::StartupModule()
{
	FAudioFactoryTheme::SetTheme(EAudioFactoryTheme::Dark);
	// AudioExporter identity accent -- Golden Yellow (matches logo card warm golden waves).
	// Applies the same gamma-space x0.7 darken the registry accents use, to this palette accent's
	// original value (0.82, 0.58, 0.04). NOTE: this does NOT make the two equal. SetPluginAccent stores its
	// FLinearColor verbatim, while the registry authors sRGB hex that FLinearColor(FColor) converts,
	// so the two systems use different colour spaces and remain different colours. Only the
	// darkening AMOUNT is shared. Unifying them (reading the registry accent here) is a possible
	// future change, not implemented here.
	FAudioFactoryTheme::SetPluginAccent(FLinearColor(0.369f, 0.2633f, 0.0207f));

	// AudioExporterTests' LoadingPhase is "None" in
	// AudioExporter.uplugin, so a normal editor launch never auto-loads it and the Module List
	// stays clean. Automation launches are the one case that carries -ExecCmds= on the command
	// line (the automation runner passes it); when present, defer loading the test
	// module to OnPostEngineInit so its automation tests still register for that run.
	if (FCString::Strifind(FCommandLine::Get(), TEXT("-ExecCmds=")))
	{
		auto LoadAutomationTests = []()
		{
			FModuleManager::Get().LoadModule(TEXT("AudioExporterTests"));
		};
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
		FCoreDelegates::GetOnPostEngineInit().AddLambda(LoadAutomationTests);
#else
		FCoreDelegates::OnPostEngineInit.AddLambda(LoadAutomationTests);
#endif
	}
}

void FAudioExporterUIModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FAudioExporterUIModule, AudioExporterUI)
