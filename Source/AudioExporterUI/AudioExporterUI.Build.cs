// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

using System.IO;
using UnrealBuildTool;

public class AudioExporterUI : ModuleRules
{
	public AudioExporterUI(ReadOnlyTargetRules target) : base(target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Map the shared window-adapter's export seam to this module's API macro so product
		// module files can link its statics.
		PublicDefinitions.Add("AF_UI_API=AUDIOEXPORTERUI_API");

		PublicDependencyModuleNames.AddRange(
			new[]
			{
				"AudioExporterRuntime",
				"AudioWidgets",
				"AssetRegistry",
				"Core",
				"Slate",
				"SlateCore",
				// Public because the UI module's public window-adapter header exposes UITheme types
				//; dependents include it.
				"AudioExporterUITheme",
			}
		);

		// UE 5.8 moved FAudioMeterWidgetStyle (base of FAudioMeterStyle) into the
		// AudioWidgetsCore module; earlier engines keep it inside AudioWidgets.
		if (Directory.Exists(Path.Combine(EngineDirectory, "Plugins", "Runtime", "AudioWidgets", "Source", "AudioWidgetsCore")))
		{
			PublicDependencyModuleNames.Add("AudioWidgetsCore");
		}

		PrivateDependencyModuleNames.AddRange(
			new[]
			{
				"CoreUObject",
				"Engine",
				"InputCore",
				"MetasoundEngine",
				"Projects",
				"SignalProcessing",
			}
		);

		if (target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(
				new[]
				{
					"PropertyEditor",
				}
			);
		}
	}
}
