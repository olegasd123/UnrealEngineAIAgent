using UnrealBuildTool;

public class UEAIAgentEditor : ModuleRules
{
    public UEAIAgentEditor(ReadOnlyTargetRules Target) : base(Target)
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
                "InputCore",
                "Slate",
                "SlateCore",
                "ToolMenus",
                "UnrealEd",
                "UEAIAgentTools",
                "UEAIAgentContext",
                "UEAIAgentTransport"
            }
        );
    }
}
