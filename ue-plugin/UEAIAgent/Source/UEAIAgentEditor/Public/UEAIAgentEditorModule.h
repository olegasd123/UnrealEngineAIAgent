#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

class FUEAIAgentEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    TSharedRef<class SDockTab> SpawnAgentTab(const class FSpawnTabArgs& SpawnTabArgs);
    void RegisterTab();
    void UnregisterTab();

    static const FName AgentTabName;
};
