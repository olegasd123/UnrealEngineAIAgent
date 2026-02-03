#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

DECLARE_DELEGATE_TwoParams(FOnUEAIAgentHealthChecked, bool, const FString&);
DECLARE_DELEGATE_TwoParams(FOnUEAIAgentTaskPlanned, bool, const FString&);

class FUEAIAgentTransportModule : public IModuleInterface
{
public:
    static FUEAIAgentTransportModule& Get()
    {
        return FModuleManager::LoadModuleChecked<FUEAIAgentTransportModule>("UEAIAgentTransport");
    }

    static bool IsAvailable()
    {
        return FModuleManager::Get().IsModuleLoaded("UEAIAgentTransport");
    }

    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    void CheckHealth(const FOnUEAIAgentHealthChecked& Callback) const;
    void PlanTask(const FString& Prompt, const TArray<FString>& SelectedActors, const FOnUEAIAgentTaskPlanned& Callback) const;

private:
    FString BuildHealthUrl() const;
    FString BuildPlanUrl() const;
};
