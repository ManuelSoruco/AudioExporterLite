// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

using UnrealBuildTool;

public class AudioExporterTests : ModuleRules
{
	public AudioExporterTests(ReadOnlyTargetRules target) : base(target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new[]
			{
				"AudioExporter",
				"AudioExporterRuntime",
				"AudioExporterUI",
				"Core",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new[]
			{
				"ApplicationCore",
				"AudioExporterUtils",
				"AudioExporterUITheme",
				// ContentBrowser and ToolMenus back the Content Browser asset context menu tests that
				// assert each product's exact menu literal and which asset classes the action is offered
				// for. Those tests generate the real menu rather than inspecting a predicate, so the menu
				// and context types are needed at link time.
				"ContentBrowser",
				"CoreUObject",
				"ToolMenus",
				"Engine",
				"InputCore",
				// The static-source coverage constructs a UMetaSoundSource directly, so the test
				// module links the same MetaSound module AudioExporterRuntime already depends on.
				"MetasoundEngine",
				"Slate",
				"SlateCore",
				"UnrealEd",
			}
		);

		// WITH_DEV_AUTOMATION_TESTS is deliberately not force-defined here. The engine already sets it
		// for every non-Shipping configuration, and overriding it locally would let this module see a
		// different value than the modules whose test hooks it calls.
	}
}
