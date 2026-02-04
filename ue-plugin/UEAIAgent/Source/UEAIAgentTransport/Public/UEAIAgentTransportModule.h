#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

DECLARE_DELEGATE_TwoParams(FOnUEAIAgentHealthChecked, bool, const FString&);
DECLARE_DELEGATE_TwoParams(FOnUEAIAgentTaskPlanned, bool, const FString&);

enum class EUEAIAgentPlannedActionType : uint8
{
    ModifyActor,
    CreateActor,
    DeleteActor
};

struct FUEAIAgentPlannedSceneAction
{
    EUEAIAgentPlannedActionType Type = EUEAIAgentPlannedActionType::ModifyActor;

    // Shared target scope for selection-based actions.
    TArray<FString> ActorNames;

    // scene.modifyActor
    FVector DeltaLocation = FVector::ZeroVector;
    FRotator DeltaRotation = FRotator::ZeroRotator;

    // scene.createActor
    FString ActorClass = TEXT("Actor");
    FVector SpawnLocation = FVector::ZeroVector;
    FRotator SpawnRotation = FRotator::ZeroRotator;
    int32 SpawnCount = 1;

    bool bApproved = true;
};

class UEAIAGENTTRANSPORT_API FUEAIAgentTransportModule : public IModuleInterface
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
    int32 GetPlannedActionCount() const;
    FString GetPlannedActionPreviewText(int32 ActionIndex) const;
    bool IsPlannedActionApproved(int32 ActionIndex) const;
    void SetPlannedActionApproved(int32 ActionIndex, bool bApproved) const;
    bool PopApprovedPlannedActions(TArray<FUEAIAgentPlannedSceneAction>& OutActions) const;

private:
    FString BuildHealthUrl() const;
    FString BuildPlanUrl() const;

    mutable TArray<FUEAIAgentPlannedSceneAction> PlannedActions;
};
