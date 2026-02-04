#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

DECLARE_DELEGATE_TwoParams(FOnUEAIAgentHealthChecked, bool, const FString&);
DECLARE_DELEGATE_TwoParams(FOnUEAIAgentTaskPlanned, bool, const FString&);
DECLARE_DELEGATE_TwoParams(FOnUEAIAgentCredentialOpFinished, bool, const FString&);

enum class EUEAIAgentPlannedActionType : uint8
{
    ModifyActor,
    CreateActor,
    DeleteActor
};

enum class EUEAIAgentRiskLevel : uint8
{
    Low,
    Medium,
    High
};

enum class EUEAIAgentActionState : uint8
{
    Pending,
    Succeeded,
    Failed
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

    EUEAIAgentRiskLevel Risk = EUEAIAgentRiskLevel::Low;
    EUEAIAgentActionState State = EUEAIAgentActionState::Pending;
    int32 AttemptCount = 0;
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
    void PlanTask(const FString& Prompt, const FString& Mode, const TArray<FString>& SelectedActors, const FOnUEAIAgentTaskPlanned& Callback) const;
    void SetProviderApiKey(const FString& Provider, const FString& ApiKey, const FOnUEAIAgentCredentialOpFinished& Callback) const;
    void DeleteProviderApiKey(const FString& Provider, const FOnUEAIAgentCredentialOpFinished& Callback) const;
    void TestProviderApiKey(const FString& Provider, const FOnUEAIAgentCredentialOpFinished& Callback) const;
    void GetProviderStatus(const FOnUEAIAgentCredentialOpFinished& Callback) const;
    int32 GetPlannedActionCount() const;
    FString GetPlannedActionPreviewText(int32 ActionIndex) const;
    bool IsPlannedActionApproved(int32 ActionIndex) const;
    void SetPlannedActionApproved(int32 ActionIndex, bool bApproved) const;
    bool PopApprovedPlannedActions(TArray<FUEAIAgentPlannedSceneAction>& OutActions) const;
    bool GetPendingAction(int32 ActionIndex, FUEAIAgentPlannedSceneAction& OutAction) const;
    void UpdateActionResult(int32 ActionIndex, bool bSucceeded, int32 AttemptCount) const;
    int32 GetNextPendingActionIndex() const;

private:
    FString BuildBaseUrl() const;
    FString BuildHealthUrl() const;
    FString BuildPlanUrl() const;
    FString BuildProviderStatusUrl() const;
    FString BuildCredentialsSetUrl() const;
    FString BuildCredentialsDeleteUrl() const;
    FString BuildCredentialsTestUrl() const;

    mutable TArray<FUEAIAgentPlannedSceneAction> PlannedActions;
};
