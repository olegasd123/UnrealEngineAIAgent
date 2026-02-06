#include "UEAIAgentSceneTools.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "ScopedTransaction.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
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
        const FVector NewScale = Actor->GetActorScale3D() + Params.DeltaScale;
        Actor->SetActorScale3D(NewScale);
        ++UpdatedCount;
    }

    OutMessage = FString::Printf(
        TEXT("scene.modifyActor applied to %d actor(s). DeltaLocation: X=%.2f Y=%.2f Z=%.2f, DeltaRotation: Pitch=%.2f Yaw=%.2f Roll=%.2f, DeltaScale: X=%.2f Y=%.2f Z=%.2f"),
        UpdatedCount,
        Params.DeltaLocation.X,
        Params.DeltaLocation.Y,
        Params.DeltaLocation.Z,
        Params.DeltaRotation.Pitch,
        Params.DeltaRotation.Yaw,
        Params.DeltaRotation.Roll,
        Params.DeltaScale.X,
        Params.DeltaScale.Y,
        Params.DeltaScale.Z);

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

bool FUEAIAgentSceneTools::SceneModifyComponent(const FUEAIAgentModifyComponentParams& Params, FString& OutMessage)
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

    if (Params.ComponentName.IsEmpty())
    {
        OutMessage = TEXT("Component name is required.");
        return false;
    }

    const bool bHasDelta =
        !Params.DeltaLocation.IsNearlyZero() ||
        !Params.DeltaRotation.IsNearlyZero() ||
        !Params.DeltaScale.IsNearlyZero();
    if (!bHasDelta && !Params.bSetVisibility)
    {
        OutMessage = TEXT("No component edits specified.");
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

    const FScopedTransaction Transaction(LOCTEXT("SceneModifyComponentTransaction", "UE AI Agent Modify Component"));
    int32 UpdatedComponents = 0;
    int32 UpdatedActors = 0;
    for (AActor* Actor : TargetActors)
    {
        if (!Actor)
        {
            continue;
        }

        bool bActorTouched = false;
        TArray<UActorComponent*> Components;
        Actor->GetComponents(Components);
        for (UActorComponent* Component : Components)
        {
            if (!Component)
            {
                continue;
            }

            const FString ComponentName = Component->GetName();
            if (!ComponentName.Equals(Params.ComponentName, ESearchCase::IgnoreCase))
            {
                continue;
            }

            bool bComponentEdited = false;
            Component->Modify();
            if (USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
            {
                if (!Params.DeltaLocation.IsNearlyZero())
                {
                    SceneComponent->SetRelativeLocation(SceneComponent->GetRelativeLocation() + Params.DeltaLocation);
                    bComponentEdited = true;
                }
                if (!Params.DeltaRotation.IsNearlyZero())
                {
                    SceneComponent->SetRelativeRotation(SceneComponent->GetRelativeRotation() + Params.DeltaRotation);
                    bComponentEdited = true;
                }
                if (!Params.DeltaScale.IsNearlyZero())
                {
                    SceneComponent->SetRelativeScale3D(SceneComponent->GetRelativeScale3D() + Params.DeltaScale);
                    bComponentEdited = true;
                }
            }

            if (Params.bSetVisibility)
            {
                if (UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(Component))
                {
                    PrimitiveComponent->SetVisibility(Params.bVisible, true);
                    bComponentEdited = true;
                }
            }

            if (bComponentEdited)
            {
                UpdatedComponents += 1;
                bActorTouched = true;
            }
        }

        if (bActorTouched)
        {
            Actor->Modify();
            UpdatedActors += 1;
        }
    }

    OutMessage = FString::Printf(
        TEXT("scene.modifyComponent updated %d component(s) on %d actor(s). Component: %s, DeltaLocation: X=%.2f Y=%.2f Z=%.2f, DeltaRotation: Pitch=%.2f Yaw=%.2f Roll=%.2f, DeltaScale: X=%.2f Y=%.2f Z=%.2f, VisibilityEdit: %s"),
        UpdatedComponents,
        UpdatedActors,
        *Params.ComponentName,
        Params.DeltaLocation.X,
        Params.DeltaLocation.Y,
        Params.DeltaLocation.Z,
        Params.DeltaRotation.Pitch,
        Params.DeltaRotation.Yaw,
        Params.DeltaRotation.Roll,
        Params.DeltaScale.X,
        Params.DeltaScale.Y,
        Params.DeltaScale.Z,
        Params.bSetVisibility ? (Params.bVisible ? TEXT("show") : TEXT("hide")) : TEXT("none"));

    return UpdatedComponents > 0;
}

bool FUEAIAgentSceneTools::SceneAddActorTag(const FUEAIAgentAddActorTagParams& Params, FString& OutMessage)
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

    if (Params.Tag.IsEmpty())
    {
        OutMessage = TEXT("Tag is required.");
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

    const FScopedTransaction Transaction(LOCTEXT("SceneAddActorTagTransaction", "UE AI Agent Add Actor Tag"));
    int32 UpdatedCount = 0;
    const FName TagName(*Params.Tag);
    for (AActor* Actor : TargetActors)
    {
        if (!Actor)
        {
            continue;
        }

        if (Actor->Tags.Contains(TagName))
        {
            continue;
        }

        Actor->Modify();
        Actor->Tags.Add(TagName);
        UpdatedCount += 1;
    }

    OutMessage = FString::Printf(TEXT("scene.addActorTag added tag '%s' to %d actor(s)."), *Params.Tag, UpdatedCount);
    return UpdatedCount > 0;
}

#undef LOCTEXT_NAMESPACE
