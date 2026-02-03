#pragma once

#include "CoreMinimal.h"

struct FUEAIAgentModifyActorParams
{
    TArray<FString> ActorNames;
    FVector DeltaLocation = FVector::ZeroVector;
    FRotator DeltaRotation = FRotator::ZeroRotator;
    bool bUseSelectionIfActorNamesEmpty = true;
};

class UEAIAGENTTOOLS_API FUEAIAgentSceneTools
{
public:
    static bool SceneModifyActor(const FUEAIAgentModifyActorParams& Params, FString& OutMessage);
};
