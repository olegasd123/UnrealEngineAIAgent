#include "UEAIAgentSceneTools.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "EngineUtils.h"
#include "Engine/PostProcessVolume.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "ScopedTransaction.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/PostProcessComponent.h"
#include "Landscape.h"
#include "LandscapeDataAccess.h"
#include "LandscapeEdit.h"
#include "LandscapeEditLayer.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeUtils.h"
#include "Materials/MaterialInterface.h"
#include "Engine/StaticMesh.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UObjectGlobals.h"
#include "Subsystems/EditorActorSubsystem.h"

#define LOCTEXT_NAMESPACE "UEAIAgentSceneTools"

namespace
{
    FScopedTransaction* GUEAIAgentSessionTransaction = nullptr;

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

    template <typename TComponentClass>
    bool ResolveUniqueActorWithComponent(UWorld* World, TArray<AActor*>& InOutActors)
    {
        if (!World || !InOutActors.IsEmpty())
        {
            return InOutActors.Num() > 0;
        }

        AActor* UniqueActor = nullptr;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (!Actor || !Actor->FindComponentByClass<TComponentClass>())
            {
                continue;
            }

            if (UniqueActor)
            {
                return false;
            }
            UniqueActor = Actor;
        }

        if (!UniqueActor)
        {
            return false;
        }

        InOutActors.Add(UniqueActor);
        return true;
    }

    bool ResolveUniquePostProcessActor(UWorld* World, TArray<AActor*>& InOutActors)
    {
        if (!World || !InOutActors.IsEmpty())
        {
            return InOutActors.Num() > 0;
        }

        AActor* UniqueActor = nullptr;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (!Actor)
            {
                continue;
            }

            const bool bHasPostProcess = Cast<APostProcessVolume>(Actor) != nullptr ||
                Actor->FindComponentByClass<UPostProcessComponent>() != nullptr;
            if (!bHasPostProcess)
            {
                continue;
            }

            if (UniqueActor)
            {
                return false;
            }
            UniqueActor = Actor;
        }

        if (!UniqueActor)
        {
            return false;
        }

        InOutActors.Add(UniqueActor);
        return true;
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

    ALandscapeProxy* ResolveLandscapeEditTarget(ALandscapeProxy* Landscape)
    {
        if (!Landscape)
        {
            return nullptr;
        }

        if (ALandscape* RootLandscape = Landscape->GetLandscapeActor())
        {
            return RootLandscape;
        }

        return Landscape;
    }

    void AddLandscapeTargetUnique(ALandscapeProxy* Landscape, TArray<ALandscapeProxy*>& OutLandscapes)
    {
        if (ALandscapeProxy* TargetLandscape = ResolveLandscapeEditTarget(Landscape))
        {
            OutLandscapes.AddUnique(TargetLandscape);
        }
    }

    void CollectLandscapeTargets(
        UWorld* World,
        const TArray<FString>& ActorNames,
        bool bUseSelectionIfActorNamesEmpty,
        TArray<ALandscapeProxy*>& OutLandscapes)
    {
        OutLandscapes.Empty();
        TArray<AActor*> CandidateActors;
        if (!ActorNames.IsEmpty())
        {
            CollectActorsByName(World, ActorNames, CandidateActors);
        }
        else if (bUseSelectionIfActorNamesEmpty)
        {
            CollectActorsFromSelection(CandidateActors);
        }

        for (AActor* Candidate : CandidateActors)
        {
            ALandscapeProxy* Landscape = Cast<ALandscapeProxy>(Candidate);
            if (!Landscape)
            {
                continue;
            }
            AddLandscapeTargetUnique(Landscape, OutLandscapes);
        }
    }

    bool ComputeLandscapeEditRect(
        ALandscapeProxy* Landscape,
        const FVector2D& Center,
        const FVector2D& Size,
        int32& OutMinX,
        int32& OutMinY,
        int32& OutMaxX,
        int32& OutMaxY)
    {
        if (!Landscape)
        {
            return false;
        }

        ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
        if (!LandscapeInfo)
        {
            return false;
        }

        int32 ExtentMinX = 0;
        int32 ExtentMinY = 0;
        int32 ExtentMaxX = 0;
        int32 ExtentMaxY = 0;
        if (!LandscapeInfo->GetLandscapeExtent(ExtentMinX, ExtentMinY, ExtentMaxX, ExtentMaxY))
        {
            return false;
        }

        const FVector LandscapeLocation = Landscape->GetActorLocation();
        const FVector LandscapeScale = Landscape->GetActorScale3D();
        const float ScaleX = FMath::Max(KINDA_SMALL_NUMBER, FMath::Abs(LandscapeScale.X));
        const float ScaleY = FMath::Max(KINDA_SMALL_NUMBER, FMath::Abs(LandscapeScale.Y));

        const float LocalCenterX = (Center.X - LandscapeLocation.X) / ScaleX;
        const float LocalCenterY = (Center.Y - LandscapeLocation.Y) / ScaleY;
        const float HalfSizeX = FMath::Max(1.0f, FMath::Abs(Size.X) * 0.5f / ScaleX);
        const float HalfSizeY = FMath::Max(1.0f, FMath::Abs(Size.Y) * 0.5f / ScaleY);

        const float RequestedMinX = LocalCenterX - HalfSizeX;
        const float RequestedMaxX = LocalCenterX + HalfSizeX;
        const float RequestedMinY = LocalCenterY - HalfSizeY;
        const float RequestedMaxY = LocalCenterY + HalfSizeY;
        const bool bOverlapsLandscape =
            RequestedMaxX >= static_cast<float>(ExtentMinX) &&
            RequestedMinX <= static_cast<float>(ExtentMaxX) &&
            RequestedMaxY >= static_cast<float>(ExtentMinY) &&
            RequestedMinY <= static_cast<float>(ExtentMaxY);
        if (!bOverlapsLandscape)
        {
            return false;
        }

        OutMinX = FMath::Clamp(FMath::FloorToInt(LocalCenterX - HalfSizeX), ExtentMinX, ExtentMaxX);
        OutMaxX = FMath::Clamp(FMath::CeilToInt(LocalCenterX + HalfSizeX), ExtentMinX, ExtentMaxX);
        OutMinY = FMath::Clamp(FMath::FloorToInt(LocalCenterY - HalfSizeY), ExtentMinY, ExtentMaxY);
        OutMaxY = FMath::Clamp(FMath::CeilToInt(LocalCenterY + HalfSizeY), ExtentMinY, ExtentMaxY);
        return OutMaxX >= OutMinX && OutMaxY >= OutMinY;
    }

    bool ResolveLandscapeTargetsForArea(
        UWorld* World,
        const FVector2D& Center,
        const FVector2D& Size,
        TArray<ALandscapeProxy*>& InOutLandscapes)
    {
        if (!World || !InOutLandscapes.IsEmpty())
        {
            return InOutLandscapes.Num() > 0;
        }

        TArray<ALandscapeProxy*> WorldLandscapes;
        for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
        {
            ALandscapeProxy* Landscape = *It;
            AddLandscapeTargetUnique(Landscape, WorldLandscapes);
        }

        if (WorldLandscapes.IsEmpty())
        {
            return false;
        }

        for (ALandscapeProxy* Landscape : WorldLandscapes)
        {
            int32 MinX = 0;
            int32 MinY = 0;
            int32 MaxX = 0;
            int32 MaxY = 0;
            if (ComputeLandscapeEditRect(Landscape, Center, Size, MinX, MinY, MaxX, MaxY))
            {
                AddLandscapeTargetUnique(Landscape, InOutLandscapes);
            }
        }

        if (!InOutLandscapes.IsEmpty())
        {
            return true;
        }

        // Fallback: choose nearest landscape by XY center if bounds-based match failed.
        ALandscapeProxy* NearestLandscape = nullptr;
        float BestDistanceSq = TNumericLimits<float>::Max();
        for (ALandscapeProxy* Landscape : WorldLandscapes)
        {
            if (!Landscape)
            {
                continue;
            }

            const FVector Location = Landscape->GetActorLocation();
            const FVector2D LocationXY(Location.X, Location.Y);
            const float DistanceSq = FVector2D::DistSquared(LocationXY, Center);
            if (DistanceSq < BestDistanceSq)
            {
                BestDistanceSq = DistanceSq;
                NearestLandscape = Landscape;
            }
        }

        if (NearestLandscape)
        {
            AddLandscapeTargetUnique(NearestLandscape, InOutLandscapes);
        }

        return InOutLandscapes.Num() > 0;
    }

    FGuid ResolveLandscapeEditLayerGuid(ALandscapeProxy* Landscape)
    {
        if (!Landscape)
        {
            return FGuid();
        }

        const ALandscape* RootLandscape = Landscape->GetLandscapeActor();
        if (!RootLandscape)
        {
            return FGuid();
        }

        const FGuid CurrentLayerGuid = RootLandscape->GetEditingLayer();
        if (CurrentLayerGuid.IsValid())
        {
            return CurrentLayerGuid;
        }

        const TArray<ULandscapeEditLayerBase*> EditLayers = RootLandscape->GetEditLayers();
        for (const ULandscapeEditLayerBase* EditLayer : EditLayers)
        {
            if (!EditLayer)
            {
                continue;
            }

            const FGuid LayerGuid = EditLayer->GetGuid();
            if (LayerGuid.IsValid())
            {
                return LayerGuid;
            }
        }

        return FGuid();
    }

    void RequestLandscapeLayersContentRefresh(ALandscapeProxy* Landscape)
    {
        if (ALandscape* RootLandscape = Landscape ? Landscape->GetLandscapeActor() : nullptr)
        {
            RootLandscape->RequestLayersContentUpdate(ELandscapeLayerUpdateMode::Update_All);
        }
    }

    FString NormalizeLandscapeLayerLookupKey(const FString& InValue)
    {
        FString Key = InValue.TrimStartAndEnd();
        Key.ToLowerInline();
        Key.ReplaceInline(TEXT(" "), TEXT(""));
        Key.ReplaceInline(TEXT("_"), TEXT(""));
        Key.ReplaceInline(TEXT("-"), TEXT(""));
        Key.ReplaceInline(TEXT("layerinfo"), TEXT(""));
        return Key;
    }

    ULandscapeLayerInfoObject* ResolveLandscapeLayerInfo(
        ULandscapeInfo* LandscapeInfo,
        ALandscapeProxy* Landscape,
        const FString& RequestedLayerName,
        TArray<FString>* OutAvailableLayerNames = nullptr)
    {
        if (!LandscapeInfo)
        {
            return nullptr;
        }

        const FString Requested = RequestedLayerName.TrimStartAndEnd();
        if (Requested.IsEmpty())
        {
            return nullptr;
        }

        if (ULandscapeLayerInfoObject* ExactLayer = LandscapeInfo->GetLayerInfoByName(FName(*Requested), Landscape))
        {
            return ExactLayer;
        }
        if (ULandscapeLayerInfoObject* ExactAnyOwnerLayer = LandscapeInfo->GetLayerInfoByName(FName(*Requested), nullptr))
        {
            return ExactAnyOwnerLayer;
        }

        const FString RequestedKey = NormalizeLandscapeLayerLookupKey(Requested);
        ULandscapeLayerInfoObject* LooseMatchLayer = nullptr;

#if WITH_EDITORONLY_DATA
        for (const FLandscapeInfoLayerSettings& LayerSettings : LandscapeInfo->Layers)
        {
            const FString LayerName = LayerSettings.GetLayerName().ToString().TrimStartAndEnd();
            if (OutAvailableLayerNames && !LayerName.IsEmpty())
            {
                OutAvailableLayerNames->AddUnique(LayerName);
            }

            ULandscapeLayerInfoObject* LayerInfo = LayerSettings.LayerInfoObj;
            if (!LayerInfo)
            {
                continue;
            }

            const FString LayerInfoObjectName = LayerInfo->GetName().TrimStartAndEnd();
            if (OutAvailableLayerNames && !LayerInfoObjectName.IsEmpty())
            {
                OutAvailableLayerNames->AddUnique(LayerInfoObjectName);
            }

            if (LayerName.Equals(Requested, ESearchCase::IgnoreCase) ||
                LayerInfoObjectName.Equals(Requested, ESearchCase::IgnoreCase))
            {
                return LayerInfo;
            }

            if (LooseMatchLayer)
            {
                continue;
            }

            const FString LayerNameKey = NormalizeLandscapeLayerLookupKey(LayerName);
            const FString LayerInfoObjectNameKey = NormalizeLandscapeLayerLookupKey(LayerInfoObjectName);
            if (!RequestedKey.IsEmpty() &&
                (LayerNameKey == RequestedKey || LayerInfoObjectNameKey == RequestedKey))
            {
                LooseMatchLayer = LayerInfo;
            }
        }
#endif

        return LooseMatchLayer;
    }

    bool LandscapeHasEditLayerNamed(ALandscapeProxy* Landscape, const FName& LayerName)
    {
        if (!Landscape || LayerName.IsNone())
        {
            return false;
        }

        const ALandscape* RootLandscape = Landscape->GetLandscapeActor();
        if (!RootLandscape)
        {
            return false;
        }

        return RootLandscape->GetEditLayer(LayerName) != nullptr;
    }

    ULandscapeLayerInfoObject* TryCreateAndAssignPaintLayerInfo(
        ULandscapeInfo* LandscapeInfo,
        ALandscapeProxy* Landscape,
        const FString& RequestedLayerName)
    {
        if (!LandscapeInfo || !Landscape)
        {
            return nullptr;
        }

        const FString Requested = RequestedLayerName.TrimStartAndEnd();
        const FName RequestedFName(*Requested);
        if (RequestedFName.IsNone())
        {
            return nullptr;
        }

        const bool bHasMatchingEditLayer = LandscapeHasEditLayerNamed(Landscape, RequestedFName);
        const bool bHasAnyPaintLayers = Landscape->GetValidTargetLayerObjects().Num() > 0;
        if (!bHasMatchingEditLayer || bHasAnyPaintLayers)
        {
            return nullptr;
        }

        const FString SharedAssetsPath = UE::Landscape::GetSharedAssetsPath(Landscape->GetLevel());
        if (SharedAssetsPath.IsEmpty())
        {
            return nullptr;
        }

        ULandscapeLayerInfoObject* CreatedLayerInfo = UE::Landscape::CreateTargetLayerInfo(RequestedFName, SharedAssetsPath);
        if (!CreatedLayerInfo)
        {
            return nullptr;
        }

        Landscape->Modify();
        const FLandscapeTargetLayerSettings TargetLayerSettings(CreatedLayerInfo);
        if (Landscape->HasTargetLayer(RequestedFName))
        {
            Landscape->UpdateTargetLayer(RequestedFName, TargetLayerSettings);
        }
        else
        {
            Landscape->AddTargetLayer(RequestedFName, TargetLayerSettings);
        }

        LandscapeInfo->CreateTargetLayerSettingsFor(CreatedLayerInfo);
        LandscapeInfo->UpdateLayerInfoMap(Landscape, true);
        return CreatedLayerInfo;
    }

    float ComputeBrushWeight(
        int32 X,
        int32 Y,
        float LocalCenterX,
        float LocalCenterY,
        float RadiusX,
        float RadiusY,
        float Falloff)
    {
        const float SafeRadiusX = FMath::Max(1.0f, RadiusX);
        const float SafeRadiusY = FMath::Max(1.0f, RadiusY);
        const float NormX = FMath::Abs(static_cast<float>(X) - LocalCenterX) / SafeRadiusX;
        const float NormY = FMath::Abs(static_cast<float>(Y) - LocalCenterY) / SafeRadiusY;
        const float Radius = FMath::Sqrt(NormX * NormX + NormY * NormY);
        if (Radius >= 1.0f)
        {
            return 0.0f;
        }

        const float ClampedFalloff = FMath::Clamp(Falloff, 0.0f, 1.0f);
        const float InnerRadius = 1.0f - ClampedFalloff;
        if (Radius <= InnerRadius)
        {
            return 1.0f;
        }

        const float BlendRange = FMath::Max(KINDA_SMALL_NUMBER, 1.0f - InnerRadius);
        return 1.0f - ((Radius - InnerRadius) / BlendRange);
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

bool FUEAIAgentSceneTools::SceneSetDirectionalLightIntensity(
    const FUEAIAgentSetDirectionalLightIntensityParams& Params,
    FString& OutMessage)
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
    bool bUsedAutoResolve = false;
    if (!Params.ActorNames.IsEmpty())
    {
        CollectActorsByName(World, Params.ActorNames, TargetActors);
    }
    else if (Params.bUseSelectionIfActorNamesEmpty)
    {
        CollectActorsFromSelection(TargetActors);
    }
    if (TargetActors.IsEmpty() && Params.ActorNames.IsEmpty() && Params.bUseSelectionIfActorNamesEmpty)
    {
        bUsedAutoResolve = ResolveUniqueActorWithComponent<UDirectionalLightComponent>(World, TargetActors);
    }

    if (TargetActors.IsEmpty())
    {
        OutMessage = TEXT("No target actors found. Select a directional light actor or provide actorNames.");
        return false;
    }

    const float Intensity = FMath::Clamp(Params.Intensity, 0.0f, 200000.0f);
    const FScopedTransaction Transaction(LOCTEXT("SceneSetDirectionalLightIntensity", "UE AI Agent Set Directional Light Intensity"));
    int32 UpdatedCount = 0;
    for (AActor* Actor : TargetActors)
    {
        if (!Actor)
        {
            continue;
        }

        UDirectionalLightComponent* DirectionalComponent = Actor->FindComponentByClass<UDirectionalLightComponent>();
        if (!DirectionalComponent)
        {
            continue;
        }

        Actor->Modify();
        DirectionalComponent->Modify();
        DirectionalComponent->SetIntensity(Intensity);
        UpdatedCount += 1;
    }

    OutMessage = FString::Printf(
        TEXT("Set directional light intensity to %.2f on %d actor(s)."),
        Intensity,
        UpdatedCount);
    if (bUsedAutoResolve && UpdatedCount > 0)
    {
        OutMessage += TEXT(" Target was auto-resolved.");
    }
    return UpdatedCount > 0;
}

bool FUEAIAgentSceneTools::SceneSetFogDensity(const FUEAIAgentSetFogDensityParams& Params, FString& OutMessage)
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
    bool bUsedAutoResolve = false;
    if (!Params.ActorNames.IsEmpty())
    {
        CollectActorsByName(World, Params.ActorNames, TargetActors);
    }
    else if (Params.bUseSelectionIfActorNamesEmpty)
    {
        CollectActorsFromSelection(TargetActors);
    }
    if (TargetActors.IsEmpty() && Params.ActorNames.IsEmpty() && Params.bUseSelectionIfActorNamesEmpty)
    {
        bUsedAutoResolve = ResolveUniqueActorWithComponent<UExponentialHeightFogComponent>(World, TargetActors);
    }

    if (TargetActors.IsEmpty())
    {
        OutMessage = TEXT("No target actors found. Select a fog actor or provide actorNames.");
        return false;
    }

    const float Density = FMath::Clamp(Params.Density, 0.0f, 5.0f);
    const FScopedTransaction Transaction(LOCTEXT("SceneSetFogDensity", "UE AI Agent Set Fog Density"));
    int32 UpdatedCount = 0;
    for (AActor* Actor : TargetActors)
    {
        if (!Actor)
        {
            continue;
        }

        UExponentialHeightFogComponent* FogComponent = Actor->FindComponentByClass<UExponentialHeightFogComponent>();
        if (!FogComponent)
        {
            continue;
        }

        Actor->Modify();
        FogComponent->Modify();
        FogComponent->SetFogDensity(Density);
        UpdatedCount += 1;
    }

    OutMessage = FString::Printf(
        TEXT("Set fog density to %.4f on %d actor(s)."),
        Density,
        UpdatedCount);
    if (bUsedAutoResolve && UpdatedCount > 0)
    {
        OutMessage += TEXT(" Target was auto-resolved.");
    }
    return UpdatedCount > 0;
}

bool FUEAIAgentSceneTools::SceneSetPostProcessExposureCompensation(
    const FUEAIAgentSetPostProcessExposureCompensationParams& Params,
    FString& OutMessage)
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
    bool bUsedAutoResolve = false;
    if (!Params.ActorNames.IsEmpty())
    {
        CollectActorsByName(World, Params.ActorNames, TargetActors);
    }
    else if (Params.bUseSelectionIfActorNamesEmpty)
    {
        CollectActorsFromSelection(TargetActors);
    }
    if (TargetActors.IsEmpty() && Params.ActorNames.IsEmpty() && Params.bUseSelectionIfActorNamesEmpty)
    {
        bUsedAutoResolve = ResolveUniquePostProcessActor(World, TargetActors);
    }

    if (TargetActors.IsEmpty())
    {
        OutMessage = TEXT("No target actors found. Select a post process actor or provide actorNames.");
        return false;
    }

    const float Exposure = FMath::Clamp(Params.ExposureCompensation, -15.0f, 15.0f);
    const FScopedTransaction Transaction(
        LOCTEXT("SceneSetPostProcessExposureCompensation", "UE AI Agent Set Post Process Exposure Compensation"));
    int32 UpdatedCount = 0;
    for (AActor* Actor : TargetActors)
    {
        if (!Actor)
        {
            continue;
        }

        bool bEdited = false;
        if (APostProcessVolume* PostProcessVolume = Cast<APostProcessVolume>(Actor))
        {
            Actor->Modify();
            PostProcessVolume->Settings.bOverride_AutoExposureBias = true;
            PostProcessVolume->Settings.AutoExposureBias = Exposure;
            bEdited = true;
        }

        TArray<UPostProcessComponent*> PostProcessComponents;
        Actor->GetComponents(PostProcessComponents);
        for (UPostProcessComponent* PostProcessComponent : PostProcessComponents)
        {
            if (!PostProcessComponent)
            {
                continue;
            }

            PostProcessComponent->Modify();
            PostProcessComponent->Settings.bOverride_AutoExposureBias = true;
            PostProcessComponent->Settings.AutoExposureBias = Exposure;
            bEdited = true;
        }

        if (bEdited)
        {
            UpdatedCount += 1;
        }
    }

    OutMessage = FString::Printf(
        TEXT("Set post process exposure compensation to %.2f on %d actor(s)."),
        Exposure,
        UpdatedCount);
    if (bUsedAutoResolve && UpdatedCount > 0)
    {
        OutMessage += TEXT(" Target was auto-resolved.");
    }
    return UpdatedCount > 0;
}

bool FUEAIAgentSceneTools::LandscapeSculpt(const FUEAIAgentLandscapeSculptParams& Params, FString& OutMessage)
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

    TArray<ALandscapeProxy*> TargetLandscapes;
    CollectLandscapeTargets(World, Params.ActorNames, Params.bUseSelectionIfActorNamesEmpty, TargetLandscapes);
    bool bUsedAreaFallback = false;
    if (TargetLandscapes.IsEmpty())
    {
        bUsedAreaFallback = ResolveLandscapeTargetsForArea(World, Params.Center, Params.Size, TargetLandscapes);
    }
    if (TargetLandscapes.IsEmpty())
    {
        OutMessage = TEXT("No target landscape actors found. Select a landscape actor or provide actorNames.");
        return false;
    }

    const float Strength = FMath::Clamp(Params.Strength, 0.0f, 1.0f);
    const float Falloff = FMath::Clamp(Params.Falloff, 0.0f, 1.0f);
    const float SignedStrength = Params.bLower ? -Strength : Strength;

    const FScopedTransaction Transaction(LOCTEXT("LandscapeSculptTransaction", "UE AI Agent Landscape Sculpt"));
    int32 UpdatedLandscapes = 0;

    for (ALandscapeProxy* Landscape : TargetLandscapes)
    {
        if (!Landscape)
        {
            continue;
        }

        int32 MinX = 0;
        int32 MinY = 0;
        int32 MaxX = 0;
        int32 MaxY = 0;
        if (!ComputeLandscapeEditRect(Landscape, Params.Center, Params.Size, MinX, MinY, MaxX, MaxY))
        {
            continue;
        }

        ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
        if (!LandscapeInfo)
        {
            continue;
        }

        const int32 Width = MaxX - MinX + 1;
        const int32 Height = MaxY - MinY + 1;
        if (Width <= 0 || Height <= 0)
        {
            continue;
        }

        const FVector LandscapeLocation = Landscape->GetActorLocation();
        const FVector LandscapeScale = Landscape->GetActorScale3D();
        const float ScaleX = FMath::Max(KINDA_SMALL_NUMBER, FMath::Abs(LandscapeScale.X));
        const float ScaleY = FMath::Max(KINDA_SMALL_NUMBER, FMath::Abs(LandscapeScale.Y));
        const float ScaleZ = FMath::Max(KINDA_SMALL_NUMBER, FMath::Abs(LandscapeScale.Z));
        const float LocalCenterX = (Params.Center.X - LandscapeLocation.X) / ScaleX;
        const float LocalCenterY = (Params.Center.Y - LandscapeLocation.Y) / ScaleY;
        const float RadiusX = FMath::Max(1.0f, FMath::Abs(Params.Size.X) * 0.5f / ScaleX);
        const float RadiusY = FMath::Max(1.0f, FMath::Abs(Params.Size.Y) * 0.5f / ScaleY);

        FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo, ResolveLandscapeEditLayerGuid(Landscape));
        TArray<uint16> HeightData;
        HeightData.SetNumUninitialized(Width * Height);
        LandscapeEdit.GetHeightDataFast(MinX, MinY, MaxX, MaxY, HeightData.GetData(), 0);

        bool bEditedLandscape = false;
        for (int32 Y = MinY; Y <= MaxY; ++Y)
        {
            for (int32 X = MinX; X <= MaxX; ++X)
            {
                const float BrushWeight = ComputeBrushWeight(X, Y, LocalCenterX, LocalCenterY, RadiusX, RadiusY, Falloff);
                if (BrushWeight <= 0.0f)
                {
                    continue;
                }

                const int32 DataIndex = (Y - MinY) * Width + (X - MinX);
                const float DeltaWorldZ = 512.0f * SignedStrength * BrushWeight;
                const int32 DeltaHeight = FMath::RoundToInt((DeltaWorldZ * 128.0f) / ScaleZ);
                if (DeltaHeight == 0)
                {
                    continue;
                }

                const int32 CurrentHeight = HeightData[DataIndex];
                const int32 NewHeight = FMath::Clamp(CurrentHeight + DeltaHeight, 0, 65535);
                if (NewHeight == CurrentHeight)
                {
                    continue;
                }

                HeightData[DataIndex] = static_cast<uint16>(NewHeight);
                bEditedLandscape = true;
            }
        }

        if (!bEditedLandscape)
        {
            continue;
        }

        Landscape->Modify();
        LandscapeEdit.SetHeightData(MinX, MinY, MaxX, MaxY, HeightData.GetData(), 0, true);
        LandscapeEdit.Flush();
        RequestLandscapeLayersContentRefresh(Landscape);
        UpdatedLandscapes += 1;
    }

    if (UpdatedLandscapes <= 0)
    {
        OutMessage = TEXT("Could not sculpt landscape in the requested area. Check area bounds and target landscape.");
        return false;
    }

    OutMessage = FString::Printf(
        TEXT("%s landscape in bounded area. Affected landscapes=%d."),
        Params.bLower ? TEXT("Lowered") : TEXT("Sculpted"),
        UpdatedLandscapes);
    if (bUsedAreaFallback)
    {
        OutMessage += TEXT(" Target was auto-resolved.");
    }
    return true;
}

bool FUEAIAgentSceneTools::LandscapePaintLayer(const FUEAIAgentLandscapePaintLayerParams& Params, FString& OutMessage)
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

    if (Params.LayerName.TrimStartAndEnd().IsEmpty())
    {
        OutMessage = TEXT("Layer name is required.");
        return false;
    }

    TArray<ALandscapeProxy*> TargetLandscapes;
    CollectLandscapeTargets(World, Params.ActorNames, Params.bUseSelectionIfActorNamesEmpty, TargetLandscapes);
    bool bUsedAreaFallback = false;
    if (TargetLandscapes.IsEmpty())
    {
        bUsedAreaFallback = ResolveLandscapeTargetsForArea(World, Params.Center, Params.Size, TargetLandscapes);
    }
    if (TargetLandscapes.IsEmpty())
    {
        OutMessage = TEXT("No target landscape actors found. Select a landscape actor or provide actorNames.");
        return false;
    }

    const float Strength = FMath::Clamp(Params.Strength, 0.0f, 1.0f);
    const float Falloff = FMath::Clamp(Params.Falloff, 0.0f, 1.0f);
    const float SignedStrength = Params.bRemove ? -Strength : Strength;

    const FScopedTransaction Transaction(LOCTEXT("LandscapePaintLayerTransaction", "UE AI Agent Landscape Paint Layer"));
    int32 UpdatedLandscapes = 0;
    int32 MissingLayerCount = 0;
    int32 EditLayerNameMatchCount = 0;
    int32 AutoCreatedPaintLayerCount = 0;
    TSet<FString> AvailableLayerNames;

    for (ALandscapeProxy* Landscape : TargetLandscapes)
    {
        if (!Landscape)
        {
            continue;
        }

        ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
        if (!LandscapeInfo)
        {
            continue;
        }

        TArray<FString> LandscapeLayerNames;
        ULandscapeLayerInfoObject* LayerInfo = ResolveLandscapeLayerInfo(
            LandscapeInfo,
            Landscape,
            Params.LayerName,
            &LandscapeLayerNames);
        for (const FString& LayerName : LandscapeLayerNames)
        {
            if (!LayerName.IsEmpty())
            {
                AvailableLayerNames.Add(LayerName);
            }
        }
        if (!LayerInfo)
        {
            if (ULandscapeLayerInfoObject* CreatedLayerInfo = TryCreateAndAssignPaintLayerInfo(LandscapeInfo, Landscape, Params.LayerName))
            {
                LayerInfo = CreatedLayerInfo;
                AutoCreatedPaintLayerCount += 1;
                AvailableLayerNames.Add(CreatedLayerInfo->GetLayerName().ToString());
                AvailableLayerNames.Add(CreatedLayerInfo->GetName());
            }
        }
        if (!LayerInfo)
        {
            MissingLayerCount += 1;
            if (LandscapeHasEditLayerNamed(Landscape, FName(*Params.LayerName)))
            {
                EditLayerNameMatchCount += 1;
            }
            continue;
        }

        int32 MinX = 0;
        int32 MinY = 0;
        int32 MaxX = 0;
        int32 MaxY = 0;
        if (!ComputeLandscapeEditRect(Landscape, Params.Center, Params.Size, MinX, MinY, MaxX, MaxY))
        {
            continue;
        }

        const int32 Width = MaxX - MinX + 1;
        const int32 Height = MaxY - MinY + 1;
        if (Width <= 0 || Height <= 0)
        {
            continue;
        }

        const FVector LandscapeLocation = Landscape->GetActorLocation();
        const FVector LandscapeScale = Landscape->GetActorScale3D();
        const float ScaleX = FMath::Max(KINDA_SMALL_NUMBER, FMath::Abs(LandscapeScale.X));
        const float ScaleY = FMath::Max(KINDA_SMALL_NUMBER, FMath::Abs(LandscapeScale.Y));
        const float LocalCenterX = (Params.Center.X - LandscapeLocation.X) / ScaleX;
        const float LocalCenterY = (Params.Center.Y - LandscapeLocation.Y) / ScaleY;
        const float RadiusX = FMath::Max(1.0f, FMath::Abs(Params.Size.X) * 0.5f / ScaleX);
        const float RadiusY = FMath::Max(1.0f, FMath::Abs(Params.Size.Y) * 0.5f / ScaleY);

        FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo, ResolveLandscapeEditLayerGuid(Landscape));
        TArray<uint8> WeightData;
        WeightData.SetNumZeroed(Width * Height);
        LandscapeEdit.GetWeightData(LayerInfo, MinX, MinY, MaxX, MaxY, WeightData.GetData(), 0);

        bool bEditedLandscape = false;
        for (int32 Y = MinY; Y <= MaxY; ++Y)
        {
            for (int32 X = MinX; X <= MaxX; ++X)
            {
                const float BrushWeight = ComputeBrushWeight(X, Y, LocalCenterX, LocalCenterY, RadiusX, RadiusY, Falloff);
                if (BrushWeight <= 0.0f)
                {
                    continue;
                }

                const int32 DataIndex = (Y - MinY) * Width + (X - MinX);
                const int32 DeltaWeight = FMath::RoundToInt(255.0f * SignedStrength * BrushWeight);
                if (DeltaWeight == 0)
                {
                    continue;
                }

                const int32 CurrentWeight = WeightData[DataIndex];
                const int32 NewWeight = FMath::Clamp(CurrentWeight + DeltaWeight, 0, 255);
                if (NewWeight == CurrentWeight)
                {
                    continue;
                }

                WeightData[DataIndex] = static_cast<uint8>(NewWeight);
                bEditedLandscape = true;
            }
        }

        if (!bEditedLandscape)
        {
            continue;
        }

        Landscape->Modify();
        LandscapeEdit.SetAlphaData(
            LayerInfo,
            MinX,
            MinY,
            MaxX,
            MaxY,
            WeightData.GetData(),
            0,
            ELandscapeLayerPaintingRestriction::None);
        LandscapeEdit.Flush();
        RequestLandscapeLayersContentRefresh(Landscape);
        UpdatedLandscapes += 1;
    }

    if (UpdatedLandscapes <= 0)
    {
        if (MissingLayerCount > 0)
        {
            TArray<FString> AvailableLayerList = AvailableLayerNames.Array();
            AvailableLayerList.Sort();
            const FString AvailableLayersText = AvailableLayerList.Num() > 0
                ? FString::Join(AvailableLayerList, TEXT(", "))
                : TEXT("none");
            if (EditLayerNameMatchCount > 0)
            {
                OutMessage = FString::Printf(
                    TEXT("Could not paint layer '%s': this matches an Edit Layer name, not a Paint Layer name. Available paint layers: %s."),
                    *Params.LayerName,
                    *AvailableLayersText);
            }
            else
            {
                OutMessage = FString::Printf(
                    TEXT("Could not paint layer '%s': paint layer is missing on target landscape. Available paint layers: %s."),
                    *Params.LayerName,
                    *AvailableLayersText);
            }
            return false;
        }

        OutMessage = FString::Printf(
            TEXT("Could not paint layer '%s' in the requested area. Check area bounds, layer name, and target landscape."),
            *Params.LayerName);
        return false;
    }

    OutMessage = FString::Printf(
        TEXT("%s landscape layer '%s' in bounded area. Affected landscapes=%d."),
        Params.bRemove ? TEXT("Removed") : TEXT("Painted"),
        *Params.LayerName,
        UpdatedLandscapes);
    if (bUsedAreaFallback)
    {
        OutMessage += TEXT(" Target was auto-resolved.");
    }
    if (AutoCreatedPaintLayerCount > 0)
    {
        OutMessage += FString::Printf(TEXT(" Paint layer info was auto-created for '%s'."), *Params.LayerName);
    }
    return true;
}

bool FUEAIAgentSceneTools::EditorUndo(FString& OutMessage)
{
    if (!GEditor)
    {
        OutMessage = TEXT("Editor is not available.");
        return false;
    }

    if (GUEAIAgentSessionTransaction)
    {
        return SessionRollbackTransaction(OutMessage);
    }

    const bool bUndid = GEditor->UndoTransaction();
    OutMessage = bUndid ? TEXT("Undid last editor action.") : TEXT("Nothing to undo.");
    return true;
}

bool FUEAIAgentSceneTools::EditorRedo(FString& OutMessage)
{
    if (!GEditor)
    {
        OutMessage = TEXT("Editor is not available.");
        return false;
    }

    if (GUEAIAgentSessionTransaction)
    {
        OutMessage = TEXT("Cannot redo while internal transaction is active.");
        return false;
    }

    const bool bRedid = GEditor->RedoTransaction();
    OutMessage = bRedid ? TEXT("Redid last editor action.") : TEXT("Nothing to redo.");
    return true;
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
        OutMessage = TEXT("Internal transaction is already active.");
        return true;
    }

    const FString Label = Description.IsEmpty() ? TEXT("UE AI Agent Session") : Description;
    GUEAIAgentSessionTransaction = new FScopedTransaction(FText::FromString(Label));
    OutMessage = TEXT("Internal transaction started.");
    return true;
}

bool FUEAIAgentSceneTools::SessionCommitTransaction(FString& OutMessage)
{
    if (!GUEAIAgentSessionTransaction)
    {
        OutMessage = TEXT("No active internal transaction.");
        return true;
    }

    delete GUEAIAgentSessionTransaction;
    GUEAIAgentSessionTransaction = nullptr;
    OutMessage = TEXT("Internal transaction committed.");
    return true;
}

bool FUEAIAgentSceneTools::SessionRollbackTransaction(FString& OutMessage)
{
    if (!GUEAIAgentSessionTransaction)
    {
        OutMessage = TEXT("No active internal transaction.");
        return true;
    }

    GUEAIAgentSessionTransaction->Cancel();
    delete GUEAIAgentSessionTransaction;
    GUEAIAgentSessionTransaction = nullptr;
    OutMessage = TEXT("Internal transaction rolled back.");
    return true;
}

void FUEAIAgentSceneTools::SessionCleanupForShutdown()
{
    if (!GUEAIAgentSessionTransaction)
    {
        return;
    }

    GUEAIAgentSessionTransaction->Cancel();
    // Do not destroy FScopedTransaction during module/app teardown.
    // It may touch editor subsystems that are already shutting down.
    GUEAIAgentSessionTransaction = nullptr;
}

#undef LOCTEXT_NAMESPACE
