// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

using UnrealBuildTool;

public class AudioExporterUtils : ModuleRules
{
    public AudioExporterUtils(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

            // Redirect the shared macro guard only while compiling this module.
            // Public headers fall back to an empty AF_UTILS_API for consumers.
            PrivateDefinitions.Add("AF_UTILS_API=AUDIOEXPORTERUTILS_API");

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "AssetRegistry",
                "MetasoundEngine",
            }
        );
    }
}
