using UnrealBuildTool;

public class UEAIAgentTransport : ModuleRules
{
    public UEAIAgentTransport(ReadOnlyTargetRules Target) : base(Target)
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
                "DeveloperSettings",
                "HTTP",
                "Json",
                "JsonUtilities"
            }
        );
    }
}
