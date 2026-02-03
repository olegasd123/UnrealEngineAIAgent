#include "UEAIAgentSettings.h"

UUEAIAgentSettings::UUEAIAgentSettings()
    : AgentHost(TEXT("127.0.0.1"))
    , AgentPort(4317)
    , DefaultProvider(EUEAIAgentProvider::OpenAI)
{
}

FName UUEAIAgentSettings::GetContainerName() const
{
    return TEXT("Project");
}

FName UUEAIAgentSettings::GetCategoryName() const
{
    return TEXT("Plugins");
}
