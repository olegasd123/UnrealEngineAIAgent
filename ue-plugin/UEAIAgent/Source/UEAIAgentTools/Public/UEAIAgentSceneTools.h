#pragma once

#include "CoreMinimal.h"

struct FUEAIAgentModifyActorParams
{
    TArray<FString> ActorNames;
    FVector DeltaLocation = FVector::ZeroVector;
    FRotator DeltaRotation = FRotator::ZeroRotator;
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

class UEAIAGENTTOOLS_API FUEAIAgentSceneTools
{
public:
    static bool SceneModifyActor(const FUEAIAgentModifyActorParams& Params, FString& OutMessage);
    static bool SceneCreateActor(const FUEAIAgentCreateActorParams& Params, FString& OutMessage);
    static bool SceneDeleteActor(const FUEAIAgentDeleteActorParams& Params, FString& OutMessage);
};
