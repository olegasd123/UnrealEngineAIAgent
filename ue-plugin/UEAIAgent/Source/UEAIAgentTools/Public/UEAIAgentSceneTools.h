#pragma once

#include "CoreMinimal.h"

struct FUEAIAgentModifyActorParams
{
    TArray<FString> ActorNames;
    FVector DeltaLocation = FVector::ZeroVector;
    FRotator DeltaRotation = FRotator::ZeroRotator;
    FVector DeltaScale = FVector::ZeroVector;
    FVector Scale = FVector::OneVector;
    bool bHasScale = false;
    bool bUseSelectionIfActorNamesEmpty = true;
};

struct FUEAIAgentCreateActorParams
{
    FString ActorClass = TEXT("Actor");
    FVector Location = FVector::ZeroVector;
    FRotator Rotation = FRotator::ZeroRotator;
    int32 Count = 1;
};

struct FUEAIAgentDeleteActorParams
{
    TArray<FString> ActorNames;
    bool bUseSelectionIfActorNamesEmpty = true;
};

struct FUEAIAgentModifyComponentParams
{
    TArray<FString> ActorNames;
    FString ComponentName;
    FVector DeltaLocation = FVector::ZeroVector;
    FRotator DeltaRotation = FRotator::ZeroRotator;
    FVector DeltaScale = FVector::ZeroVector;
    FVector Scale = FVector::OneVector;
    bool bHasScale = false;
    bool bUseSelectionIfActorNamesEmpty = true;
    bool bSetVisibility = false;
    bool bVisible = true;
};

struct FUEAIAgentAddActorTagParams
{
    TArray<FString> ActorNames;
    FString Tag;
    bool bUseSelectionIfActorNamesEmpty = true;
};

struct FUEAIAgentSetComponentMaterialParams
{
    TArray<FString> ActorNames;
    FString ComponentName;
    FString MaterialPath;
    int32 MaterialSlot = 0;
    bool bUseSelectionIfActorNamesEmpty = true;
};

struct FUEAIAgentSetComponentStaticMeshParams
{
    TArray<FString> ActorNames;
    FString ComponentName;
    FString MeshPath;
    bool bUseSelectionIfActorNamesEmpty = true;
};

struct FUEAIAgentSetActorFolderParams
{
    TArray<FString> ActorNames;
    FString FolderPath;
    bool bUseSelectionIfActorNamesEmpty = true;
};

struct FUEAIAgentAddActorLabelPrefixParams
{
    TArray<FString> ActorNames;
    FString Prefix;
    bool bUseSelectionIfActorNamesEmpty = true;
};

struct FUEAIAgentDuplicateActorsParams
{
    TArray<FString> ActorNames;
    int32 Count = 1;
    FVector Offset = FVector::ZeroVector;
    bool bUseSelectionIfActorNamesEmpty = true;
};

struct FUEAIAgentSetDirectionalLightIntensityParams
{
    TArray<FString> ActorNames;
    float Intensity = 10.0f;
    bool bUseSelectionIfActorNamesEmpty = true;
};

struct FUEAIAgentSetFogDensityParams
{
    TArray<FString> ActorNames;
    float Density = 0.02f;
    bool bUseSelectionIfActorNamesEmpty = true;
};

struct FUEAIAgentSetPostProcessExposureCompensationParams
{
    TArray<FString> ActorNames;
    float ExposureCompensation = 0.0f;
    bool bUseSelectionIfActorNamesEmpty = true;
};

class UEAIAGENTTOOLS_API FUEAIAgentSceneTools
{
public:
    static bool ContextGetSceneSummary(FString& OutMessage);
    static bool ContextGetSelection(FString& OutMessage);
    static bool SceneModifyActor(const FUEAIAgentModifyActorParams& Params, FString& OutMessage);
    static bool SceneCreateActor(const FUEAIAgentCreateActorParams& Params, FString& OutMessage);
    static bool SceneDeleteActor(const FUEAIAgentDeleteActorParams& Params, FString& OutMessage);
    static bool SceneModifyComponent(const FUEAIAgentModifyComponentParams& Params, FString& OutMessage);
    static bool SceneAddActorTag(const FUEAIAgentAddActorTagParams& Params, FString& OutMessage);
    static bool SceneSetComponentMaterial(const FUEAIAgentSetComponentMaterialParams& Params, FString& OutMessage);
    static bool SceneSetComponentStaticMesh(const FUEAIAgentSetComponentStaticMeshParams& Params, FString& OutMessage);
    static bool SceneSetActorFolder(const FUEAIAgentSetActorFolderParams& Params, FString& OutMessage);
    static bool SceneAddActorLabelPrefix(const FUEAIAgentAddActorLabelPrefixParams& Params, FString& OutMessage);
    static bool SceneDuplicateActors(const FUEAIAgentDuplicateActorsParams& Params, FString& OutMessage);
    static bool SceneSetDirectionalLightIntensity(const FUEAIAgentSetDirectionalLightIntensityParams& Params, FString& OutMessage);
    static bool SceneSetFogDensity(const FUEAIAgentSetFogDensityParams& Params, FString& OutMessage);
    static bool SceneSetPostProcessExposureCompensation(
        const FUEAIAgentSetPostProcessExposureCompensationParams& Params,
        FString& OutMessage);
    static bool SessionBeginTransaction(const FString& Description, FString& OutMessage);
    static bool SessionCommitTransaction(FString& OutMessage);
    static bool SessionRollbackTransaction(FString& OutMessage);
};
