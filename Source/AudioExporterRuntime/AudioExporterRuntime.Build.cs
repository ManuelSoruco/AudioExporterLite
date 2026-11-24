// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

using UnrealBuildTool;

public class AudioExporterRuntime : ModuleRules
{
	public AudioExporterRuntime(ReadOnlyTargetRules target) : base(target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new[]
			{
				"AudioExporterUtils",
				"AudioMixer",
				"Core",
				"CoreUObject",
				"DeveloperSettings",
				"Engine",
				"MetasoundEngine",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new[]
			{
				"AssetRegistry",
				"SignalProcessing"
			}
		);
	}
}
