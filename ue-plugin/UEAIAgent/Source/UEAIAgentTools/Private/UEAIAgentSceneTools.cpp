#include "UEAIAgentSceneTools.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "UEAIAgentSceneTools"

namespace
{
    void CollectActorsFromSelection(TArray<AActor*>& OutActors)
    {
        if (!GEditor)
        {
            return;
        }

        for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
        {
            if (AActor* Actor = Cast<AActor>(*It))
            {
                OutActors.Add(Actor);
            }
        }
    }

    void CollectActorsByName(UWorld* World, const TArray<FString>& ActorNames, TArray<AActor*>& OutActors)
    {
        if (!World || ActorNames.IsEmpty())
        {
            return;
        }

        TSet<FString> NameSet;
        for (const FString& Name : ActorNames)
        {
            NameSet.Add(Name);
        }
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (!Actor)
            {
                continue;
            }

            if (NameSet.Contains(Actor->GetName()))
            {
                OutActors.Add(Actor);
            }
        }
    }
}

bool FUEAIAgentSceneTools::SceneModifyActor(const FUEAIAgentModifyActorParams& Params, FString& OutMessage)
{
    if (!GEditor)
    {
        OutMessage = TEXT("Editor is not available.");
        return false;
    }

    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        OutMessage = TEXT("Editor world is not available.");
        return false;
    }

    TArray<AActor*> TargetActors;
    if (!Params.ActorNames.IsEmpty())
    {
        CollectActorsByName(World, Params.ActorNames, TargetActors);
    }
    else if (Params.bUseSelectionIfActorNamesEmpty)
    {
        CollectActorsFromSelection(TargetActors);
    }

    if (TargetActors.IsEmpty())
    {
        OutMessage = TEXT("No target actors found.");
        return false;
    }

    const FScopedTransaction Transaction(LOCTEXT("SceneModifyActorTransaction", "UE AI Agent Scene Modify Actor"));
    int32 UpdatedCount = 0;
    for (AActor* Actor : TargetActors)
    {
        if (!Actor)
        {
            continue;
        }

        Actor->Modify();
        const FVector NewLocation = Actor->GetActorLocation() + Params.DeltaLocation;
        Actor->SetActorLocation(NewLocation, false, nullptr, ETeleportType::None);
        ++UpdatedCount;
    }

    OutMessage = FString::Printf(
        TEXT("scene.modifyActor applied to %d actor(s). Delta: X=%.2f Y=%.2f Z=%.2f"),
        UpdatedCount,
        Params.DeltaLocation.X,
        Params.DeltaLocation.Y,
        Params.DeltaLocation.Z);

    return UpdatedCount > 0;
}

#undef LOCTEXT_NAMESPACE
