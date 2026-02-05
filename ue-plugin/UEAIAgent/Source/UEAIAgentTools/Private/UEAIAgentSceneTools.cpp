#include "UEAIAgentSceneTools.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UObjectGlobals.h"

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
            NameSet.Add(Name.ToLower());
        }
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (!Actor)
            {
                continue;
            }

            const FString ActorName = Actor->GetName();
            const FString ActorLabel = Actor->GetActorLabel();
            const FString ActorNameLower = ActorName.ToLower();
            const FString ActorLabelLower = ActorLabel.ToLower();

            if (NameSet.Contains(ActorName) ||
                NameSet.Contains(ActorLabel) ||
                NameSet.Contains(ActorNameLower) ||
                NameSet.Contains(ActorLabelLower))
            {
                OutActors.Add(Actor);
            }
        }
    }

    UClass* ResolveActorClass(const FString& ActorClassNameOrPath)
    {
        if (ActorClassNameOrPath.IsEmpty())
        {
            return AActor::StaticClass();
        }

        UClass* ResolvedClass = nullptr;
        if (ActorClassNameOrPath.StartsWith(TEXT("/")))
        {
            ResolvedClass = LoadClass<AActor>(nullptr, *ActorClassNameOrPath);
            if (ResolvedClass && ResolvedClass->IsChildOf(AActor::StaticClass()))
            {
                return ResolvedClass;
            }
        }

        const FString ScriptPath = FString::Printf(TEXT("/Script/Engine.%s"), *ActorClassNameOrPath);
        ResolvedClass = FindObject<UClass>(nullptr, *ScriptPath);
        if (ResolvedClass && ResolvedClass->IsChildOf(AActor::StaticClass()))
        {
            return ResolvedClass;
        }

        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Candidate = *It;
            if (!Candidate || !Candidate->IsChildOf(AActor::StaticClass()))
            {
                continue;
            }

            if (Candidate->GetName().Equals(ActorClassNameOrPath, ESearchCase::IgnoreCase))
            {
                return Candidate;
            }
        }

        return AActor::StaticClass();
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
        const FRotator NewRotation = Actor->GetActorRotation() + Params.DeltaRotation;
        Actor->SetActorRotation(NewRotation, ETeleportType::None);
        ++UpdatedCount;
    }

    OutMessage = FString::Printf(
        TEXT("scene.modifyActor applied to %d actor(s). DeltaLocation: X=%.2f Y=%.2f Z=%.2f, DeltaRotation: Pitch=%.2f Yaw=%.2f Roll=%.2f"),
        UpdatedCount,
        Params.DeltaLocation.X,
        Params.DeltaLocation.Y,
        Params.DeltaLocation.Z,
        Params.DeltaRotation.Pitch,
        Params.DeltaRotation.Yaw,
        Params.DeltaRotation.Roll);

    return UpdatedCount > 0;
}

bool FUEAIAgentSceneTools::SceneCreateActor(const FUEAIAgentCreateActorParams& Params, FString& OutMessage)
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

    const int32 SpawnCount = FMath::Clamp(Params.Count, 1, 200);
    UClass* ActorClass = ResolveActorClass(Params.ActorClass);
    if (!ActorClass || !ActorClass->IsChildOf(AActor::StaticClass()))
    {
        OutMessage = TEXT("Actor class is invalid.");
        return false;
    }

    const FScopedTransaction Transaction(LOCTEXT("SceneCreateActorTransaction", "UE AI Agent Scene Create Actor"));
    int32 CreatedCount = 0;
    for (int32 Index = 0; Index < SpawnCount; ++Index)
    {
        FActorSpawnParameters SpawnParams;
        SpawnParams.Name = NAME_None;
        SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

        AActor* Spawned = World->SpawnActor<AActor>(ActorClass, Params.Location, Params.Rotation, SpawnParams);
        if (!Spawned)
        {
            continue;
        }

        Spawned->Modify();
        ++CreatedCount;
    }

    OutMessage = FString::Printf(
        TEXT("scene.createActor created %d/%d actor(s). Class: %s, Location: X=%.2f Y=%.2f Z=%.2f, Rotation: Pitch=%.2f Yaw=%.2f Roll=%.2f"),
        CreatedCount,
        SpawnCount,
        *ActorClass->GetName(),
        Params.Location.X,
        Params.Location.Y,
        Params.Location.Z,
        Params.Rotation.Pitch,
        Params.Rotation.Yaw,
        Params.Rotation.Roll);

    return CreatedCount > 0;
}

bool FUEAIAgentSceneTools::SceneDeleteActor(const FUEAIAgentDeleteActorParams& Params, FString& OutMessage)
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

    const FScopedTransaction Transaction(LOCTEXT("SceneDeleteActorTransaction", "UE AI Agent Scene Delete Actor"));
    int32 DeletedCount = 0;
    for (AActor* Actor : TargetActors)
    {
        if (!Actor)
        {
            continue;
        }

        Actor->Modify();
        if (Actor->Destroy())
        {
            ++DeletedCount;
        }
    }

    OutMessage = FString::Printf(TEXT("scene.deleteActor deleted %d actor(s)."), DeletedCount);
    return DeletedCount > 0;
}

#undef LOCTEXT_NAMESPACE
