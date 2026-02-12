#include "UEAIAgentSettings.h"

UUEAIAgentSettings::UUEAIAgentSettings()
    : AgentHost(TEXT("127.0.0.1"))
    , AgentPort(4317)
    , DefaultProvider(EUEAIAgentProvider::Local)
    , bShowChatsOnOpen(true)
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
