#include "UEAIAgentEditorModule.h"

#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogUEAIAgentEditor, Log, All);

void FUEAIAgentEditorModule::StartupModule()
{
    UE_LOG(LogUEAIAgentEditor, Log, TEXT("UEAIAgentEditor started."));
}

void FUEAIAgentEditorModule::ShutdownModule()
{
    UE_LOG(LogUEAIAgentEditor, Log, TEXT("UEAIAgentEditor stopped."));
}

IMPLEMENT_MODULE(FUEAIAgentEditorModule, UEAIAgentEditor)

