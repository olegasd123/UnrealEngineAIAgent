#include "UEAIAgentContextModule.h"

#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogUEAIAgentContext, Log, All);

void FUEAIAgentContextModule::StartupModule()
{
    UE_LOG(LogUEAIAgentContext, Log, TEXT("UEAIAgentContext started."));
}

void FUEAIAgentContextModule::ShutdownModule()
{
    UE_LOG(LogUEAIAgentContext, Log, TEXT("UEAIAgentContext stopped."));
}

IMPLEMENT_MODULE(FUEAIAgentContextModule, UEAIAgentContext)

