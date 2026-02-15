#include "UEAIAgentEditorModule.h"

#include "SUEAIAgentPanel.h"
#include "Framework/Docking/TabManager.h"
#include "Modules/ModuleManager.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"

DEFINE_LOG_CATEGORY_STATIC(LogUEAIAgentEditor, Log, All);

const FName FUEAIAgentEditorModule::AgentTabName("UEAIAgentMainTab");

void FUEAIAgentEditorModule::StartupModule()
{
    RegisterTab();
    RegisterMenus();
    UE_LOG(LogUEAIAgentEditor, Log, TEXT("UEAIAgentEditor started."));
}

void FUEAIAgentEditorModule::ShutdownModule()
{
    if (UToolMenus::IsToolMenuUIEnabled())
    {
        UToolMenus::UnregisterOwner(this);
    }
    bMenusRegistered = false;

    UnregisterTab();
    UE_LOG(LogUEAIAgentEditor, Log, TEXT("UEAIAgentEditor stopped."));
}

void FUEAIAgentEditorModule::RegisterTab()
{
    // Can happen after hot reload or module reload.
    if (FGlobalTabmanager::Get()->HasTabSpawner(AgentTabName))
    {
        FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(AgentTabName);
    }

    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
        AgentTabName,
        FOnSpawnTab::CreateRaw(this, &FUEAIAgentEditorModule::SpawnAgentTab))
        .SetDisplayName(FText::FromString(TEXT("UE AI Agent")))
        .SetMenuType(ETabSpawnerMenuType::Hidden);
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

void FUEAIAgentEditorModule::RegisterMenus()
{
    if (bMenusRegistered)
    {
        return;
    }

    if (!UToolMenus::IsToolMenuUIEnabled())
    {
        return;
    }

    FToolMenuOwnerScoped OwnerScoped(this);
    UToolMenu* WindowMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window");
    if (!WindowMenu)
    {
        return;
    }

    FToolMenuSection& Section = WindowMenu->FindOrAddSection("LevelEditor");
    Section.AddMenuEntry(
        "UEAIAgent.OpenWindow",
        FText::FromString(TEXT("UE AI Agent")),
        FText::FromString(TEXT("Open UE AI Agent window.")),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateRaw(this, &FUEAIAgentEditorModule::OpenAgentTab)));
    bMenusRegistered = true;
}

void FUEAIAgentEditorModule::OpenAgentTab()
{
    FGlobalTabmanager::Get()->TryInvokeTab(AgentTabName);
}

IMPLEMENT_MODULE(FUEAIAgentEditorModule, UEAIAgentEditor)
