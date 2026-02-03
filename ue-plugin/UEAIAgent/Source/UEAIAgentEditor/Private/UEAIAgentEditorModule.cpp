#include "UEAIAgentEditorModule.h"

#include "SUEAIAgentPanel.h"
#include "Framework/Docking/TabManager.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Docking/SDockTab.h"

DEFINE_LOG_CATEGORY_STATIC(LogUEAIAgentEditor, Log, All);

const FName FUEAIAgentEditorModule::AgentTabName("UEAIAgentMainTab");

void FUEAIAgentEditorModule::StartupModule()
{
    RegisterTab();
    UE_LOG(LogUEAIAgentEditor, Log, TEXT("UEAIAgentEditor started."));
}

void FUEAIAgentEditorModule::ShutdownModule()
{
    UnregisterTab();
    UE_LOG(LogUEAIAgentEditor, Log, TEXT("UEAIAgentEditor stopped."));
}

void FUEAIAgentEditorModule::RegisterTab()
{
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
        AgentTabName,
        FOnSpawnTab::CreateRaw(this, &FUEAIAgentEditorModule::SpawnAgentTab))
        .SetDisplayName(FText::FromString(TEXT("UE AI Agent")))
        .SetMenuType(ETabSpawnerMenuType::Enabled);
}

void FUEAIAgentEditorModule::UnregisterTab()
{
    if (FGlobalTabmanager::Get()->HasTabSpawner(AgentTabName))
    {
        FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(AgentTabName);
    }
}

TSharedRef<SDockTab> FUEAIAgentEditorModule::SpawnAgentTab(const FSpawnTabArgs& SpawnTabArgs)
{
    return SNew(SDockTab)
        .TabRole(ETabRole::NomadTab)
        [
            SNew(SUEAIAgentPanel)
        ];
}

IMPLEMENT_MODULE(FUEAIAgentEditorModule, UEAIAgentEditor)
