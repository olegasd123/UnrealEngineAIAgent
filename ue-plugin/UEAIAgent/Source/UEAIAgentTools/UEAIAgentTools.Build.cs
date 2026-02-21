using UnrealBuildTool;
using System.IO;
using System.Text.RegularExpressions;

public class UEAIAgentTools : ModuleRules
{
    public UEAIAgentTools(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bool bEnablePcgTools = IsProjectPluginEnabled(Target, "PCG");

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
                "Landscape",
                "Foliage"
            }
        );

        if (bEnablePcgTools)
        {
            PrivateDependencyModuleNames.AddRange(
                new string[]
                {
                    "AssetTools",
                    "AssetRegistry",
                    "PCG"
                }
            );
        }

        PrivateDefinitions.Add($"UE_AI_AGENT_WITH_PCG={(bEnablePcgTools ? 1 : 0)}");
    }

    private static bool IsProjectPluginEnabled(ReadOnlyTargetRules Target, string PluginName)
    {
        if (Target.ProjectFile == null || string.IsNullOrWhiteSpace(PluginName))
        {
            return false;
        }

        try
        {
            string ProjectJson = File.ReadAllText(Target.ProjectFile.FullName);
            Match PluginBlock = Regex.Match(
                ProjectJson,
                "\\{[^\\{\\}]*\"Name\"\\s*:\\s*\"" + Regex.Escape(PluginName) + "\"[^\\{\\}]*\\}",
                RegexOptions.IgnoreCase);
            if (!PluginBlock.Success)
            {
                return false;
            }

            if (Regex.IsMatch(PluginBlock.Value, "\"Enabled\"\\s*:\\s*true", RegexOptions.IgnoreCase))
            {
                return true;
            }

            if (Regex.IsMatch(PluginBlock.Value, "\"Enabled\"\\s*:\\s*false", RegexOptions.IgnoreCase))
            {
                return false;
            }

            return false;
        }
        catch
        {
            return false;
        }
    }
}
