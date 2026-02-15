#include "UEAIAgentSceneTools.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "ScopedTransaction.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Engine/StaticMesh.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UObjectGlobals.h"
#include "Subsystems/EditorActorSubsystem.h"

#define LOCTEXT_NAMESPACE "UEAIAgentSceneTools"

namespace
{
    TUniquePtr<FScopedTransaction> GUEAIAgentSessionTransaction;

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

bool FUEAIAgentSceneTools::ContextGetSceneSummary(FString& OutMessage)
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

    int32 ActorCount = 0;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (*It)
        {
            ++ActorCount;
        }
    }

    TArray<AActor*> SelectedActors;
    CollectActorsFromSelection(SelectedActors);

    FString LevelName = TEXT("Unknown");
    if (World->GetCurrentLevel() && World->GetCurrentLevel()->GetOuter())
    {
        LevelName = World->GetCurrentLevel()->GetOuter()->GetName();
    }

    OutMessage = FString::Printf(
        TEXT("Scene summary: map=%s, level=%s, actors=%d, selected=%d"),
        *World->GetMapName(),
        *LevelName,
        ActorCount,
        SelectedActors.Num());
    return true;
}

bool FUEAIAgentSceneTools::ContextGetSelection(FString& OutMessage)
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

    TArray<AActor*> SelectedActors;
    CollectActorsFromSelection(SelectedActors);
    if (SelectedActors.IsEmpty())
    {
        OutMessage = TEXT("No actors selected.");
        return true;
    }

    const int32 MaxPreviewActors = 10;
    TArray<FString> ActorPreviews;
    ActorPreviews.Reserve(FMath::Min(SelectedActors.Num(), MaxPreviewActors));
    for (int32 Index = 0; Index < SelectedActors.Num() && Index < MaxPreviewActors; ++Index)
    {
        const AActor* Actor = SelectedActors[Index];
        if (!Actor)
        {
            continue;
        }

        const FVector Location = Actor->GetActorLocation();
        ActorPreviews.Add(FString::Printf(
            TEXT("%s (%s, X=%.1f Y=%.1f Z=%.1f)"),
            *Actor->GetActorLabel(),
            Actor->GetClass() ? *Actor->GetClass()->GetName() : TEXT("Unknown"),
            Location.X,
            Location.Y,
            Location.Z));
    }

    OutMessage = FString::Printf(
        TEXT("Selected (%d): %s"),
        SelectedActors.Num(),
        *FString::Join(ActorPreviews, TEXT("; ")));
    if (SelectedActors.Num() > MaxPreviewActors)
    {
        OutMessage += FString::Printf(TEXT("; ... +%d more"), SelectedActors.Num() - MaxPreviewActors);
    }

    return true;
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
        if (Params.bHasScale)
        {
            Actor->SetActorScale3D(Params.Scale);
        }
        else
        {
            const FVector NewScale = Actor->GetActorScale3D() + Params.DeltaScale;
            Actor->SetActorScale3D(NewScale);
        }
        ++UpdatedCount;
    }

    OutMessage = FString::Printf(
        TEXT("scene.modifyActor applied to %d actor(s). DeltaLocation: X=%.2f Y=%.2f Z=%.2f, DeltaRotation: Pitch=%.2f Yaw=%.2f Roll=%.2f, DeltaScale: X=%.2f Y=%.2f Z=%.2f, Scale: X=%.2f Y=%.2f Z=%.2f"),
        UpdatedCount,
        Params.DeltaLocation.X,
        Params.DeltaLocation.Y,
        Params.DeltaLocation.Z,
        Params.DeltaRotation.Pitch,
        Params.DeltaRotation.Yaw,
        Params.DeltaRotation.Roll,
        Params.DeltaScale.X,
        Params.DeltaScale.Y,
        Params.DeltaScale.Z,
        Params.Scale.X,
        Params.Scale.Y,
        Params.Scale.Z);

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
                if (Params.bHasScale)
                {
                    SceneComponent->SetRelativeScale3D(Params.Scale);
                    bComponentEdited = true;
                }
                else if (!Params.DeltaScale.IsNearlyZero())
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
        TEXT("scene.modifyComponent updated %d component(s) on %d actor(s). Component: %s, DeltaLocation: X=%.2f Y=%.2f Z=%.2f, DeltaRotation: Pitch=%.2f Yaw=%.2f Roll=%.2f, DeltaScale: X=%.2f Y=%.2f Z=%.2f, Scale: X=%.2f Y=%.2f Z=%.2f, VisibilityEdit: %s"),
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
        Params.Scale.X,
        Params.Scale.Y,
        Params.Scale.Z,
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

bool FUEAIAgentSceneTools::SceneSetComponentMaterial(const FUEAIAgentSetComponentMaterialParams& Params, FString& OutMessage)
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

    if (Params.ComponentName.IsEmpty() || Params.MaterialPath.IsEmpty())
    {
        OutMessage = TEXT("Component name and material path are required.");
        return false;
    }

    UMaterialInterface* Material = nullptr;
    if (Params.MaterialPath.StartsWith(TEXT("/")))
    {
        Material = LoadObject<UMaterialInterface>(nullptr, *Params.MaterialPath);
    }
    if (!Material)
    {
        Material = FindObject<UMaterialInterface>(nullptr, *Params.MaterialPath);
    }
    if (!Material)
    {
        OutMessage = TEXT("Material asset could not be loaded.");
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

    const FScopedTransaction Transaction(LOCTEXT("SceneSetComponentMaterialTransaction", "UE AI Agent Set Component Material"));
    int32 UpdatedComponents = 0;
    for (AActor* Actor : TargetActors)
    {
        if (!Actor)
        {
            continue;
        }

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

            if (UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(Component))
            {
                PrimitiveComponent->Modify();
                PrimitiveComponent->SetMaterial(Params.MaterialSlot, Material);
                UpdatedComponents += 1;
            }
        }
    }

    OutMessage = FString::Printf(
        TEXT("scene.setComponentMaterial updated %d component(s). Component: %s, Material: %s, Slot: %d"),
        UpdatedComponents,
        *Params.ComponentName,
        *Params.MaterialPath,
        Params.MaterialSlot);
    return UpdatedComponents > 0;
}

bool FUEAIAgentSceneTools::SceneSetComponentStaticMesh(const FUEAIAgentSetComponentStaticMeshParams& Params, FString& OutMessage)
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

    if (Params.ComponentName.IsEmpty() || Params.MeshPath.IsEmpty())
    {
        OutMessage = TEXT("Component name and mesh path are required.");
        return false;
    }

    UStaticMesh* Mesh = nullptr;
    if (Params.MeshPath.StartsWith(TEXT("/")))
    {
        Mesh = LoadObject<UStaticMesh>(nullptr, *Params.MeshPath);
    }
    if (!Mesh)
    {
        Mesh = FindObject<UStaticMesh>(nullptr, *Params.MeshPath);
    }
    if (!Mesh)
    {
        OutMessage = TEXT("Static mesh asset could not be loaded.");
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

    const FScopedTransaction Transaction(LOCTEXT("SceneSetComponentStaticMeshTransaction", "UE AI Agent Set Component Mesh"));
    int32 UpdatedComponents = 0;
    for (AActor* Actor : TargetActors)
    {
        if (!Actor)
        {
            continue;
        }

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

            if (UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component))
            {
                StaticMeshComponent->Modify();
                StaticMeshComponent->SetStaticMesh(Mesh);
                UpdatedComponents += 1;
            }
        }
    }

    OutMessage = FString::Printf(
        TEXT("scene.setComponentStaticMesh updated %d component(s). Component: %s, Mesh: %s"),
        UpdatedComponents,
        *Params.ComponentName,
        *Params.MeshPath);
    return UpdatedComponents > 0;
}

bool FUEAIAgentSceneTools::SceneSetActorFolder(const FUEAIAgentSetActorFolderParams& Params, FString& OutMessage)
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

    const FScopedTransaction Transaction(LOCTEXT("SceneSetActorFolderTransaction", "UE AI Agent Set Actor Folder"));
    int32 UpdatedCount = 0;
    const FName FolderName = Params.FolderPath.IsEmpty() ? NAME_None : FName(*Params.FolderPath);
    for (AActor* Actor : TargetActors)
    {
        if (!Actor)
        {
            continue;
        }

        Actor->Modify();
        Actor->SetFolderPath(FolderName);
        UpdatedCount += 1;
    }

    OutMessage = FString::Printf(TEXT("scene.setActorFolder updated %d actor(s) to folder '%s'."), UpdatedCount, *Params.FolderPath);
    return UpdatedCount > 0;
}

bool FUEAIAgentSceneTools::SceneAddActorLabelPrefix(const FUEAIAgentAddActorLabelPrefixParams& Params, FString& OutMessage)
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

    if (Params.Prefix.IsEmpty())
    {
        OutMessage = TEXT("Prefix is required.");
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

    const FScopedTransaction Transaction(LOCTEXT("SceneAddActorLabelPrefixTransaction", "UE AI Agent Add Actor Label Prefix"));
    int32 UpdatedCount = 0;
    for (AActor* Actor : TargetActors)
    {
        if (!Actor)
        {
            continue;
        }

        const FString CurrentLabel = Actor->GetActorLabel();
        if (CurrentLabel.StartsWith(Params.Prefix))
        {
            continue;
        }

        Actor->Modify();
        Actor->SetActorLabel(Params.Prefix + CurrentLabel, true);
        UpdatedCount += 1;
    }

    OutMessage = FString::Printf(TEXT("scene.addActorLabelPrefix added prefix '%s' to %d actor(s)."), *Params.Prefix, UpdatedCount);
    return UpdatedCount > 0;
}

bool FUEAIAgentSceneTools::SceneDuplicateActors(const FUEAIAgentDuplicateActorsParams& Params, FString& OutMessage)
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

    const int32 CopyCount = FMath::Clamp(Params.Count, 1, 20);
    if (CopyCount <= 0)
    {
        OutMessage = TEXT("Duplicate count must be at least 1.");
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

    const FScopedTransaction Transaction(LOCTEXT("SceneDuplicateActorsTransaction", "UE AI Agent Duplicate Actors"));
    int32 DuplicateCount = 0;
    for (AActor* Actor : TargetActors)
    {
        if (!Actor)
        {
            continue;
        }

        const FString BaseLabel = Actor->GetActorLabel();
        FString LabelBase = BaseLabel;
        int32 SuffixIndex = LabelBase.Len() - 1;
        while (SuffixIndex >= 0 && FChar::IsDigit(LabelBase[SuffixIndex]))
        {
            --SuffixIndex;
        }
        if (SuffixIndex >= 0 && LabelBase.IsValidIndex(SuffixIndex) && LabelBase[SuffixIndex] == TEXT('_'))
        {
            LabelBase = LabelBase.Left(SuffixIndex);
        }

        for (int32 CopyIndex = 0; CopyIndex < CopyCount; ++CopyIndex)
        {
            UEditorActorSubsystem* EditorActorSubsystem = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
            if (!EditorActorSubsystem)
            {
                continue;
            }

            AActor* Duplicate = EditorActorSubsystem->DuplicateActor(Actor);
            if (!Duplicate)
            {
                continue;
            }

            const FString NewLabel = FString::Printf(TEXT("%s_%02d"), *LabelBase, CopyIndex + 1);
            Duplicate->SetActorLabel(NewLabel, true);

            if (!Params.Offset.IsNearlyZero())
            {
                const FVector NewLocation = Duplicate->GetActorLocation() + Params.Offset * static_cast<float>(CopyIndex + 1);
                Duplicate->SetActorLocation(NewLocation, false, nullptr, ETeleportType::None);
            }

            DuplicateCount += 1;
        }
    }

    OutMessage = FString::Printf(TEXT("scene.duplicateActors created %d duplicate(s)."), DuplicateCount);
    return DuplicateCount > 0;
}

bool FUEAIAgentSceneTools::SessionBeginTransaction(const FString& Description, FString& OutMessage)
{
    if (!GEditor)
    {
        OutMessage = TEXT("Editor is not available.");
        return false;
    }

    if (GUEAIAgentSessionTransaction)
    {
        OutMessage = TEXT("Session transaction is already active.");
        return false;
    }

    const FString Label = Description.IsEmpty() ? TEXT("UE AI Agent Session") : Description;
    GUEAIAgentSessionTransaction = MakeUnique<FScopedTransaction>(FText::FromString(Label));
    OutMessage = TEXT("session.beginTransaction started.");
    return true;
}

bool FUEAIAgentSceneTools::SessionCommitTransaction(FString& OutMessage)
{
    if (!GUEAIAgentSessionTransaction)
    {
        OutMessage = TEXT("No active session transaction to commit.");
        return false;
    }

    GUEAIAgentSessionTransaction.Reset();
    OutMessage = TEXT("session.commitTransaction committed.");
    return true;
}

bool FUEAIAgentSceneTools::SessionRollbackTransaction(FString& OutMessage)
{
    if (!GUEAIAgentSessionTransaction)
    {
        OutMessage = TEXT("No active session transaction to rollback.");
        return false;
    }

    GUEAIAgentSessionTransaction->Cancel();
    GUEAIAgentSessionTransaction.Reset();
    OutMessage = TEXT("session.rollbackTransaction rolled back.");
    return true;
}

#undef LOCTEXT_NAMESPACE
