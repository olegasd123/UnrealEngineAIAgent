#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

DECLARE_DELEGATE_TwoParams(FOnUEAIAgentHealthChecked, bool, const FString&);
DECLARE_DELEGATE_TwoParams(FOnUEAIAgentTaskPlanned, bool, const FString&);
DECLARE_DELEGATE_TwoParams(FOnUEAIAgentCredentialOpFinished, bool, const FString&);
DECLARE_DELEGATE_TwoParams(FOnUEAIAgentSessionUpdated, bool, const FString&);
DECLARE_DELEGATE_TwoParams(FOnUEAIAgentChatOpFinished, bool, const FString&);

enum class EUEAIAgentPlannedActionType : uint8
{
    ContextGetSceneSummary,
    ContextGetSelection,
    EditorUndo,
    EditorRedo,
    ModifyActor,
    CreateActor,
    DeleteActor,
    ModifyComponent,
    AddActorTag,
    SetComponentMaterial,
    SetComponentStaticMesh,
    SetActorFolder,
    AddActorLabelPrefix,
    DuplicateActors,
    SetDirectionalLightIntensity,
    SetFogDensity,
    SetPostProcessExposureCompensation,
    LandscapeSculpt,
    LandscapePaintLayer,
    LandscapeGenerate,
    SessionBeginTransaction,
    SessionCommitTransaction,
    SessionRollbackTransaction
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
    FVector DeltaScale = FVector::ZeroVector;
    FVector Scale = FVector::OneVector;
    bool bHasScale = false;

    // scene.modifyComponent
    FString ComponentName;
    FVector ComponentDeltaLocation = FVector::ZeroVector;
    FRotator ComponentDeltaRotation = FRotator::ZeroRotator;
    FVector ComponentDeltaScale = FVector::ZeroVector;
    FVector ComponentScale = FVector::OneVector;
    bool bComponentHasScale = false;
    bool bComponentVisibilityEdit = false;
    bool bComponentVisible = true;

    // scene.createActor
    FString ActorClass = TEXT("Actor");
    FVector SpawnLocation = FVector::ZeroVector;
    FRotator SpawnRotation = FRotator::ZeroRotator;
    int32 SpawnCount = 1;

    // scene.addActorTag
    FString ActorTag;

    // scene.setComponentMaterial
    FString MaterialPath;
    int32 MaterialSlot = 0;

    // scene.setComponentStaticMesh
    FString MeshPath;

    // scene.setActorFolder
    FString FolderPath;

    // scene.addActorLabelPrefix
    FString LabelPrefix;

    // scene.duplicateActors
    int32 DuplicateCount = 1;
    FVector DuplicateOffset = FVector::ZeroVector;

    // scene.setDirectionalLightIntensity / scene.setFogDensity / scene.setPostProcessExposureCompensation
    float ScalarValue = 0.0f;

    // landscape.sculpt / landscape.paintLayer / landscape.generate
    FVector2D LandscapeCenter = FVector2D::ZeroVector;
    FVector2D LandscapeSize = FVector2D(1000.0f, 1000.0f);
    float LandscapeStrength = 0.2f;
    float LandscapeFalloff = 0.5f;
    bool bLandscapeInvertMode = false; // sculpt: lower, paint: remove
    FString LandscapeLayerName;
    FString LandscapeTheme;
    FString LandscapeDetailLevel;
    FString LandscapeMoonProfile;
    bool bLandscapeUseFullArea = true;
    int32 LandscapeSeed = 0;
    int32 LandscapeMountainCount = 2;
    FString LandscapeMountainStyle;
    float LandscapeMountainWidthMin = 0.0f;
    float LandscapeMountainWidthMax = 0.0f;
    float LandscapeMaxHeight = 5000.0f;
    int32 LandscapeCraterCountMin = 0;
    int32 LandscapeCraterCountMax = 0;
    float LandscapeCraterWidthMin = 0.0f;
    float LandscapeCraterWidthMax = 0.0f;

    // session.beginTransaction
    FString TransactionDescription;

    EUEAIAgentRiskLevel Risk = EUEAIAgentRiskLevel::Low;
    EUEAIAgentActionState State = EUEAIAgentActionState::Pending;
    int32 AttemptCount = 0;
    bool bApproved = true;
};

struct FUEAIAgentChatSummary
{
    FString Id;
    FString Title;
    bool bArchived = false;
    FString LastActivityAt;
};

struct FUEAIAgentChatHistoryEntry
{
    FString Kind;
    FString Route;
    FString Summary;
    FString Provider;
    FString Model;
    FString ChatType;
    FString DisplayRole;
    FString DisplayText;
    FString CreatedAt;
};

struct FUEAIAgentModelOption
{
    FString Provider;
    FString Model;
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
    void PlanTask(
        const FString& Prompt,
        const FString& Mode,
        const TArray<FString>& SelectedActors,
        const FString& Provider,
        const FString& Model,
        const FOnUEAIAgentTaskPlanned& Callback) const;
    void StartSession(
        const FString& Prompt,
        const FString& Mode,
        const TArray<FString>& SelectedActors,
        const FString& Provider,
        const FString& Model,
        const FOnUEAIAgentSessionUpdated& Callback) const;
    void NextSession(bool bHasResult, bool bResultOk, const FString& ResultMessage, const FOnUEAIAgentSessionUpdated& Callback) const;
    void ApproveCurrentSessionAction(bool bApproved, const FOnUEAIAgentSessionUpdated& Callback) const;
    void ResumeSession(const FOnUEAIAgentSessionUpdated& Callback) const;
    void SetProviderApiKey(const FString& Provider, const FString& ApiKey, const FOnUEAIAgentCredentialOpFinished& Callback) const;
    void DeleteProviderApiKey(const FString& Provider, const FOnUEAIAgentCredentialOpFinished& Callback) const;
    void TestProviderApiKey(const FString& Provider, const FOnUEAIAgentCredentialOpFinished& Callback) const;
    void GetProviderStatus(const FOnUEAIAgentCredentialOpFinished& Callback) const;
    void RefreshModelOptions(const FString& Provider, const FOnUEAIAgentCredentialOpFinished& Callback) const;
    void SavePreferredModels(const TArray<FUEAIAgentModelOption>& Models, const FOnUEAIAgentCredentialOpFinished& Callback) const;
    void RefreshChats(bool bIncludeArchived, const FOnUEAIAgentChatOpFinished& Callback) const;
    void CreateChat(const FString& Title, const FOnUEAIAgentChatOpFinished& Callback) const;
    void RenameActiveChat(const FString& NewTitle, const FOnUEAIAgentChatOpFinished& Callback) const;
    void ArchiveActiveChat(const FOnUEAIAgentChatOpFinished& Callback) const;
    void ArchiveChat(const FString& ChatId, const FOnUEAIAgentChatOpFinished& Callback) const;
    void RestoreChat(const FString& ChatId, const FOnUEAIAgentChatOpFinished& Callback) const;
    void DeleteChat(const FString& ChatId, const FOnUEAIAgentChatOpFinished& Callback) const;
    void LoadActiveChatHistory(int32 Limit, const FOnUEAIAgentChatOpFinished& Callback) const;
    void AppendActiveChatAssistantMessage(
        const FString& Route,
        const FString& Summary,
        const FString& DisplayText,
        const FString& Provider,
        const FString& Model,
        const FString& ChatType,
        const FOnUEAIAgentChatOpFinished& Callback) const;
    const TArray<FUEAIAgentChatSummary>& GetChats() const;
    const TArray<FUEAIAgentChatHistoryEntry>& GetActiveChatHistory() const;
    const TArray<FUEAIAgentModelOption>& GetAvailableModels() const;
    const TArray<FUEAIAgentModelOption>& GetPreferredModels() const;
    FString GetLastContextUsageLabel() const;
    FString GetLastContextUsageTooltip() const;
    void SetActiveChatId(const FString& ChatId) const;
    FString GetActiveChatId() const;
    FString GetLastPlanSummary() const;
    int32 GetPlannedActionCount() const;
    FString GetPlannedActionPreviewText(int32 ActionIndex) const;
    bool IsPlannedActionApproved(int32 ActionIndex) const;
    int32 GetPlannedActionAttemptCount(int32 ActionIndex) const;
    void SetPlannedActionApproved(int32 ActionIndex, bool bApproved) const;
    bool PopApprovedPlannedActions(TArray<FUEAIAgentPlannedSceneAction>& OutActions) const;
    void ClearPlannedActions() const;
    bool GetPlannedAction(int32 ActionIndex, FUEAIAgentPlannedSceneAction& OutAction) const;
    bool GetPendingAction(int32 ActionIndex, FUEAIAgentPlannedSceneAction& OutAction) const;
    void UpdateActionResult(int32 ActionIndex, bool bSucceeded, int32 AttemptCount) const;
    int32 GetNextPendingActionIndex() const;
    bool HasActiveSession() const;

private:
    FString BuildBaseUrl() const;
    FString BuildHealthUrl() const;
    FString BuildPlanUrl() const;
    FString BuildProviderStatusUrl() const;
    FString BuildCredentialsSetUrl() const;
    FString BuildCredentialsDeleteUrl() const;
    FString BuildCredentialsTestUrl() const;
    FString BuildModelsUrl(const FString& Provider) const;
    FString BuildModelPreferencesUrl() const;
    FString BuildSessionStartUrl() const;
    FString BuildSessionNextUrl() const;
    FString BuildSessionApproveUrl() const;
    FString BuildSessionResumeUrl() const;
    FString BuildChatsUrl(bool bIncludeArchived) const;
    FString BuildCreateChatUrl() const;
    FString BuildChatDeleteUrl(const FString& ChatId) const;
    FString BuildChatUpdateUrl(const FString& ChatId) const;
    FString BuildChatDetailsUrl(const FString& ChatId) const;
    FString BuildChatHistoryUrl(const FString& ChatId, int32 Limit) const;
    bool ParseSessionDecision(
        const TSharedPtr<FJsonObject>& ResponseJson,
        const TArray<FString>& SelectedActors,
        FString& OutMessage) const;
    void UpdateContextUsageFromResponse(const TSharedPtr<FJsonObject>& ResponseJson) const;

    mutable TArray<FUEAIAgentPlannedSceneAction> PlannedActions;
    mutable FString ActiveSessionId;
    mutable int32 ActiveSessionActionIndex = INDEX_NONE;
    mutable TArray<FString> ActiveSessionSelectedActors;
    mutable TArray<FUEAIAgentChatSummary> Chats;
    mutable TArray<FUEAIAgentChatHistoryEntry> ActiveChatHistory;
    mutable TArray<FUEAIAgentModelOption> AvailableModels;
    mutable TArray<FUEAIAgentModelOption> PreferredModels;
    mutable FString ActiveChatId;
    mutable FString LastPlanSummary;
    mutable FString LastContextUsageLabel;
    mutable FString LastContextUsageTooltip;
};
