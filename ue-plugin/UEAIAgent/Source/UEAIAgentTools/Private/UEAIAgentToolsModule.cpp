#include "UEAIAgentToolsModule.h"

#include "Modules/ModuleManager.h"
#include "UEAIAgentSceneTools.h"

DEFINE_LOG_CATEGORY_STATIC(LogUEAIAgentTools, Log, All);

void FUEAIAgentToolsModule::StartupModule()
{
    UE_LOG(LogUEAIAgentTools, Log, TEXT("UEAIAgentTools started."));
}

void FUEAIAgentToolsModule::ShutdownModule()
{
    FUEAIAgentSceneTools::SessionCleanupForShutdown();
    UE_LOG(LogUEAIAgentTools, Log, TEXT("UEAIAgentTools stopped."));
}

IMPLEMENT_MODULE(FUEAIAgentToolsModule, UEAIAgentTools)
