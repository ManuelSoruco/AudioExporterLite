// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

using UnrealBuildTool;

public class AudioExporter : ModuleRules
{
	public AudioExporter(ReadOnlyTargetRules target) : base(target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new[]
			{
				"AssetRegistry",
				"AudioExporterRuntime",
				"Core"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new[]
			{
				"AudioExporterUI",
				"ContentBrowser",
				"CoreUObject",
				"DesktopPlatform",
				"Engine",
				"MetasoundEngine",
				"Slate",
				"SlateCore",
				"ToolMenus",
				"UnrealEd"
			}
			);
	}
}
