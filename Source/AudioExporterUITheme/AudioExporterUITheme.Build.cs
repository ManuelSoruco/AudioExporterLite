// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

using UnrealBuildTool;

public class AudioExporterUITheme : ModuleRules
{
	public AudioExporterUITheme(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Redirect the shared UITheme export macro only while compiling this alias module.
		PrivateDefinitions.Add("AF_UI_API=AUDIOEXPORTERUITHEME_API");

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"AudioWidgets",
				"Core",
				"CoreUObject",
				"Engine",
				"InputCore",
				"Projects",
				"Slate",
				"SlateCore",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"ApplicationCore",
			}
		);
	}
}

