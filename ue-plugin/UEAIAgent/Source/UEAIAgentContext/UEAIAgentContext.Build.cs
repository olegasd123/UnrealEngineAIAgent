using UnrealBuildTool;

public class UEAIAgentContext : ModuleRules
{
    public UEAIAgentContext(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core"
            }
        );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "CoreUObject",
                "Engine",
                "UnrealEd",
                "Json",
                "JsonUtilities",
                "Landscape",
                "PCG"
            }
        );
    }
}

