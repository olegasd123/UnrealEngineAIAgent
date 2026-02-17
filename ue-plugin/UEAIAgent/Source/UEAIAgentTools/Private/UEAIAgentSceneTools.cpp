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

    void CollectAllLandscapeTargets(UWorld* World, TArray<ALandscapeProxy*>& OutLandscapes)
    {
        OutLandscapes.Empty();
        if (!World)
        {
            return;
        }

        for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
        {
            AddLandscapeTargetUnique(*It, OutLandscapes);
        }
    }

    bool ComputeLandscapeFullRect(
        ALandscapeProxy* Landscape,
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

        return LandscapeInfo->GetLandscapeExtent(OutMinX, OutMinY, OutMaxX, OutMaxY);
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

    enum class EUEAIAgentLandscapeDetailTier : uint8
    {
        Low,
        Medium,
        High,
        Cinematic
    };

    enum class EUEAIAgentMoonProfile : uint8
    {
        AncientHeavilyCratered
    };

    struct FMoonCraterFeature
    {
        FVector2D Center = FVector2D(0.5f, 0.5f);
        float Radius = 0.05f;
        float Depth = 0.5f;
        float Age = 0.5f; // 0 = fresh impact, 1 = ancient softened crater
        float Ejecta = 0.5f;
        float Terrace = 0.0f;
        float Aspect = 1.0f;
        float RotationRad = 0.0f;
    };

    struct FNatureLakeFeature
    {
        FVector2D Center = FVector2D(0.5f, 0.5f);
        float Radius = 0.08f;
        float Depth = 0.15f;
        float RimHeight = 0.04f;
    };

    struct FNatureRiverFeature
    {
        TArray<FVector2D> PathPoints;
        float Width = 0.03f;
        float Depth = 0.10f;
        float BankHeight = 0.03f;
    };

    EUEAIAgentLandscapeDetailTier ResolveLandscapeDetailTier(const FString& InDetailLevel, bool bMoonSurface)
    {
        const FString DetailLevel = InDetailLevel.TrimStartAndEnd().ToLower();
        if (DetailLevel == TEXT("low"))
        {
            return EUEAIAgentLandscapeDetailTier::Low;
        }
        if (DetailLevel == TEXT("high"))
        {
            return EUEAIAgentLandscapeDetailTier::High;
        }
        if (DetailLevel == TEXT("cinematic"))
        {
            return EUEAIAgentLandscapeDetailTier::Cinematic;
        }
        if (DetailLevel == TEXT("medium"))
        {
            return EUEAIAgentLandscapeDetailTier::Medium;
        }
        return bMoonSurface ? EUEAIAgentLandscapeDetailTier::High : EUEAIAgentLandscapeDetailTier::Medium;
    }

    float LandscapeDetailScale(EUEAIAgentLandscapeDetailTier DetailTier)
    {
        switch (DetailTier)
        {
            case EUEAIAgentLandscapeDetailTier::Low:
                return 0.72f;
            case EUEAIAgentLandscapeDetailTier::High:
                return 1.28f;
            case EUEAIAgentLandscapeDetailTier::Cinematic:
                return 1.62f;
            case EUEAIAgentLandscapeDetailTier::Medium:
            default:
                return 1.0f;
        }
    }

    const TCHAR* LandscapeDetailTierToText(EUEAIAgentLandscapeDetailTier DetailTier)
    {
        switch (DetailTier)
        {
            case EUEAIAgentLandscapeDetailTier::Low:
                return TEXT("low");
            case EUEAIAgentLandscapeDetailTier::High:
                return TEXT("high");
            case EUEAIAgentLandscapeDetailTier::Cinematic:
                return TEXT("cinematic");
            case EUEAIAgentLandscapeDetailTier::Medium:
            default:
                return TEXT("medium");
        }
    }

    EUEAIAgentMoonProfile ResolveMoonProfile(const FString& InMoonProfile, bool bMoonSurface)
    {
        (void)bMoonSurface;

        const FString MoonProfile = InMoonProfile.TrimStartAndEnd().ToLower();
        if (
            MoonProfile == TEXT("moon_surface") ||
            MoonProfile == TEXT("ancient-heavily-cratered") ||
            MoonProfile == TEXT("ancient heavily cratered") ||
            MoonProfile == TEXT("heavily_cratered") ||
            MoonProfile == TEXT("heavily-cratered") ||
            MoonProfile == TEXT("heavily cratered") ||
            MoonProfile == TEXT("ancient"))
        {
            return EUEAIAgentMoonProfile::AncientHeavilyCratered;
        }

        // Ancient heavily cratered is the default moon profile.
        return EUEAIAgentMoonProfile::AncientHeavilyCratered;
    }

    const TCHAR* MoonProfileToText(EUEAIAgentMoonProfile MoonProfile)
    {
        switch (MoonProfile)
        {
            case EUEAIAgentMoonProfile::AncientHeavilyCratered:
                return TEXT("moon_surface");
            default:
                return TEXT("moon_surface");
        }
    }

    float SampleFractalNoise(const FVector2D& Position, int32 Seed, float BaseFrequency, int32 Octaves)
    {
        const FVector2D SeedOffset(
            static_cast<float>(Seed % 1000) * 0.123f,
            static_cast<float>((Seed / 1000) % 1000) * 0.157f);

        float Total = 0.0f;
        float TotalWeight = 0.0f;
        float Amplitude = 1.0f;
        float Frequency = FMath::Max(0.001f, BaseFrequency);
        for (int32 OctaveIndex = 0; OctaveIndex < FMath::Max(1, Octaves); ++OctaveIndex)
        {
            const FVector2D OctaveShift(
                static_cast<float>(OctaveIndex) * 17.0f,
                static_cast<float>(OctaveIndex) * 23.0f);
            Total += Amplitude * FMath::PerlinNoise2D((Position * Frequency) + SeedOffset + OctaveShift);
            TotalWeight += Amplitude;
            Amplitude *= 0.5f;
            Frequency *= 2.0f;
        }

        return TotalWeight > KINDA_SMALL_NUMBER ? (Total / TotalWeight) : 0.0f;
    }

    float HashSigned(uint32 Value)
    {
        uint32 N = Value;
        N = (N << 13U) ^ N;
        const uint32 Hash = N * (N * N * 15731U + 789221U) + 1376312589U;
        return 1.0f - static_cast<float>(Hash & 0x7fffffffU) / 1073741824.0f;
    }

    float SampleMicroCraterField(const FVector2D& UV, int32 Seed, float CellCount)
    {
        const float SafeCellCount = FMath::Max(4.0f, CellCount);
        const FVector2D GridPosition = UV * SafeCellCount;
        const int32 BaseX = FMath::FloorToInt(GridPosition.X);
        const int32 BaseY = FMath::FloorToInt(GridPosition.Y);

        float NearestNormalizedDistance = TNumericLimits<float>::Max();
        float BestDepth = 0.5f;
        for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY)
        {
            for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX)
            {
                const int32 CellX = BaseX + OffsetX;
                const int32 CellY = BaseY + OffsetY;
                const uint32 HashX = static_cast<uint32>(static_cast<int64>(CellX) * 92837111LL);
                const uint32 HashY = static_cast<uint32>(static_cast<int64>(CellY) * 689287499LL);
                const uint32 HashSeed = static_cast<uint32>(static_cast<int64>(Seed) * 283923481LL);
                const uint32 HashBase =
                    HashX ^
                    HashY ^
                    HashSeed;
                const float JitterX = 0.5f + (0.5f * HashSigned(HashBase));
                const float JitterY = 0.5f + (0.5f * HashSigned(HashBase + 1013U));
                const float RadiusAlpha = 0.5f + (0.5f * HashSigned(HashBase + 3571U));
                const float DepthAlpha = 0.5f + (0.5f * HashSigned(HashBase + 9151U));
                const float Radius = FMath::Lerp(0.18f, 0.48f, RadiusAlpha);
                const float Depth = FMath::Lerp(0.30f, 0.95f, DepthAlpha);

                const FVector2D Center(static_cast<float>(CellX) + JitterX, static_cast<float>(CellY) + JitterY);
                const float Distance = FVector2D::Distance(GridPosition, Center);
                const float NormalizedDistance = Distance / FMath::Max(0.05f, Radius);
                if (NormalizedDistance < NearestNormalizedDistance)
                {
                    NearestNormalizedDistance = NormalizedDistance;
                    BestDepth = Depth;
                }
            }
        }

        if (!FMath::IsFinite(NearestNormalizedDistance))
        {
            return 0.0f;
        }

        const float Bowl =
            NearestNormalizedDistance < 1.0f
                ? -BestDepth * FMath::Square(1.0f - NearestNormalizedDistance)
                : 0.0f;
        const float RimSigma = 0.22f;
        const float RimDistance = NearestNormalizedDistance - 1.02f;
        const float Rim = (0.52f * BestDepth) *
            FMath::Exp(-(RimDistance * RimDistance) / (2.0f * RimSigma * RimSigma));
        return Bowl + Rim;
    }

    float EvaluateMoonSurfaceRaw(
        const FVector2D& UV,
        const TArray<FMoonCraterFeature>& Craters,
        int32 Seed,
        float DetailScale,
        EUEAIAgentMoonProfile MoonProfile,
        float MicroCraterScale)
    {
        const bool bAncientCratered = MoonProfile == EUEAIAgentMoonProfile::AncientHeavilyCratered;
        const float MacroNoise = SampleFractalNoise(UV, Seed + 19, bAncientCratered ? 2.2f : 2.8f, 4);
        const float RidgeNoise = 1.0f - FMath::Abs(SampleFractalNoise(UV, Seed + 137, bAncientCratered ? 7.6f : 6.4f, 4));
        const float ChannelNoise = -FMath::Abs(SampleFractalNoise(UV + FVector2D(0.13f, 0.07f), Seed + 251, 14.0f * DetailScale, 2));
        const float RegolithNoise = SampleFractalNoise(UV, Seed + 503, 34.0f * DetailScale, 3);
        const float GranularNoise = SampleFractalNoise(UV + FVector2D(0.23f, 0.41f), Seed + 587, 58.0f * DetailScale, 2);
        const float RockyPatches = 1.0f - FMath::Abs(SampleFractalNoise(UV + FVector2D(0.31f, 0.17f), Seed + 809, 19.0f * DetailScale, 3));

        // Keep craters strong while flattening broad base relief.
        const float GroundReliefScale = 0.1f;
        float Height = GroundReliefScale * (bAncientCratered
            ? (0.30f * MacroNoise) + (0.22f * (RidgeNoise - 0.5f)) + (0.09f * ChannelNoise) + (0.14f * RegolithNoise) + (0.10f * GranularNoise) + (0.10f * (RockyPatches - 0.5f))
            : (0.40f * MacroNoise) + (0.24f * (RidgeNoise - 0.5f)) + (0.14f * RegolithNoise) + (0.05f * GranularNoise));

        for (const FMoonCraterFeature& Crater : Craters)
        {
            const FVector2D Delta = UV - Crater.Center;
            const float CosA = FMath::Cos(Crater.RotationRad);
            const float SinA = FMath::Sin(Crater.RotationRad);
            const float RotX = (Delta.X * CosA) + (Delta.Y * SinA);
            const float RotY = (-Delta.X * SinA) + (Delta.Y * CosA);
            const float RadiusX = FMath::Max(0.001f, Crater.Radius * Crater.Aspect);
            const float RadiusY = FMath::Max(0.001f, Crater.Radius / FMath::Max(0.2f, Crater.Aspect));
            const float NormalizedDistance = FMath::Sqrt(
                FMath::Square(RotX / RadiusX) +
                FMath::Square(RotY / RadiusY));

            if (NormalizedDistance <= 1.0f)
            {
                const float AgeSoftening = FMath::Lerp(1.0f, 0.58f, Crater.Age);
                const float BowlWeight = 1.0f - (NormalizedDistance * NormalizedDistance);
                Height -= Crater.Depth * BowlWeight * AgeSoftening;

                if (Crater.Terrace > 0.01f && NormalizedDistance > 0.35f && NormalizedDistance < 0.96f)
                {
                    const float WallAlpha = (NormalizedDistance - 0.35f) / 0.61f;
                    const float QuantizedWall = FMath::FloorToFloat(WallAlpha * 4.0f) / 4.0f;
                    Height += Crater.Depth * Crater.Terrace * (0.08f - (0.06f * QuantizedWall));
                }

                // Large craters get a flatter floor.
                if (Crater.Radius > 0.11f && NormalizedDistance < 0.42f)
                {
                    const float FloorAlpha = 1.0f - (NormalizedDistance / 0.42f);
                    Height += Crater.Depth * 0.11f * FloorAlpha;
                }
            }

            const float RimCenter = 1.0f + FMath::Lerp(0.03f, 0.09f, Crater.Age);
            const float RimSigma = FMath::Lerp(0.07f, 0.16f, Crater.Age);
            const float RimDistance = NormalizedDistance - RimCenter;
            const float RimAmplitude = Crater.Depth * FMath::Lerp(0.38f, 0.16f, Crater.Age);
            Height += RimAmplitude *
                FMath::Exp(-(RimDistance * RimDistance) / (2.0f * RimSigma * RimSigma));

            // Ejecta around rims with uneven spread.
            const float EjectaDistance = NormalizedDistance - 1.0f;
            if (EjectaDistance > 0.0f && EjectaDistance < 1.8f)
            {
                const float DirectionNoise = 0.65f + (0.35f * SampleFractalNoise(UV + (Crater.Center * 3.0f), Seed + 1207, 9.0f, 1));
                const float EjectaFalloff = FMath::Exp(-1.8f * EjectaDistance);
                const float EjectaStrength = Crater.Ejecta * Crater.Depth * DirectionNoise * EjectaFalloff;
                Height += EjectaStrength * FMath::Lerp(0.26f, 0.10f, Crater.Age);
            }
        }

        const float ClampedMicroCraterScale = FMath::Clamp(MicroCraterScale, 0.0f, 1.0f);
        if (ClampedMicroCraterScale > KINDA_SMALL_NUMBER)
        {
            const float MicroCratersA = SampleMicroCraterField(UV, Seed + 701, (bAncientCratered ? 20.0f : 14.0f) * DetailScale);
            const float MicroCratersB = SampleMicroCraterField(UV + FVector2D(0.137f, 0.271f), Seed + 977, (bAncientCratered ? 34.0f : 24.0f) * DetailScale);
            Height += ((bAncientCratered ? 0.14f : 0.10f) * ClampedMicroCraterScale) * MicroCratersA;
            Height += ((bAncientCratered ? 0.10f : 0.06f) * ClampedMicroCraterScale) * MicroCratersB;
        }

        return Height;
    }

    float DistanceToSegment2D(const FVector2D& Point, const FVector2D& SegmentStart, const FVector2D& SegmentEnd, float& OutT)
    {
        const FVector2D Segment = SegmentEnd - SegmentStart;
        const float SegmentLengthSq = Segment.SizeSquared();
        if (SegmentLengthSq <= KINDA_SMALL_NUMBER)
        {
            OutT = 0.0f;
            return FVector2D::Distance(Point, SegmentStart);
        }

        OutT = FMath::Clamp(FVector2D::DotProduct(Point - SegmentStart, Segment) / SegmentLengthSq, 0.0f, 1.0f);
        const FVector2D ClosestPoint = SegmentStart + (Segment * OutT);
        return FVector2D::Distance(Point, ClosestPoint);
    }

    float EvaluateRiverCarve(
        const FVector2D& UV,
        const FNatureRiverFeature& River,
        int32 Seed,
        int32 RiverIndex)
    {
        if (River.PathPoints.Num() < 2)
        {
            return 0.0f;
        }

        float ClosestDistance = TNumericLimits<float>::Max();
        float RiverProgress = 0.0f;
        const int32 SegmentCount = River.PathPoints.Num() - 1;
        for (int32 SegmentIndex = 0; SegmentIndex < SegmentCount; ++SegmentIndex)
        {
            float SegmentT = 0.0f;
            const float SegmentDistance = DistanceToSegment2D(
                UV,
                River.PathPoints[SegmentIndex],
                River.PathPoints[SegmentIndex + 1],
                SegmentT);
            if (SegmentDistance < ClosestDistance)
            {
                ClosestDistance = SegmentDistance;
                RiverProgress = (static_cast<float>(SegmentIndex) + SegmentT) / static_cast<float>(FMath::Max(1, SegmentCount));
            }
        }

        const float HalfWidth = FMath::Max(0.004f, River.Width * 0.5f);
        if (ClosestDistance > HalfWidth * 2.4f)
        {
            return 0.0f;
        }

        const float WidthAlpha = 1.0f - FMath::Clamp(ClosestDistance / HalfWidth, 0.0f, 1.0f);
        const float FlowDepthScale = FMath::Lerp(1.0f, 0.42f, FMath::Clamp(RiverProgress, 0.0f, 1.0f));
        float HeightDelta = -River.Depth * FlowDepthScale * FMath::Square(WidthAlpha);

        const float BankDistance = ClosestDistance - HalfWidth;
        if (BankDistance > 0.0f && BankDistance < (HalfWidth * 1.4f))
        {
            const float BankAlpha = 1.0f - FMath::Clamp(BankDistance / (HalfWidth * 1.4f), 0.0f, 1.0f);
            HeightDelta += River.BankHeight * BankAlpha * 0.75f;
        }

        const float RiverNoise = SampleFractalNoise(UV + FVector2D(0.09f, 0.17f), Seed + (RiverIndex * 911), 24.0f, 2);
        HeightDelta += 0.015f * RiverNoise * WidthAlpha;
        return HeightDelta;
    }

    float EvaluateNatureIslandRaw(
        const FVector2D& UV,
        const TArray<FVector4f>& Mountains,
        const TArray<FNatureLakeFeature>& Lakes,
        const TArray<FNatureRiverFeature>& Rivers,
        int32 Seed)
    {
        const float DX = UV.X - 0.5f;
        const float DY = UV.Y - 0.5f;
        const float DistanceFromCenter = FMath::Sqrt(DX * DX + DY * DY) * 1.8f;
        const float IslandMask = FMath::Pow(FMath::Clamp(1.0f - DistanceFromCenter, 0.0f, 1.0f), 1.6f);

        const float BaseNoise = 0.33f * SampleFractalNoise(UV, Seed + 71, 3.0f, 3) +
            0.20f * SampleFractalNoise(UV, Seed + 211, 10.0f, 2);

        float MountainHeight = 0.0f;
        for (const FVector4f& Mountain : Mountains)
        {
            const FVector2D PeakCenter(Mountain.X, Mountain.Y);
            const float Radius = FMath::Max(0.01f, Mountain.Z);
            const float Amplitude = Mountain.W;
            const float DistSq = FVector2D::DistSquared(UV, PeakCenter);
            const float SigmaSq = Radius * Radius;
            MountainHeight += Amplitude * FMath::Exp(-DistSq / (2.0f * SigmaSq));
        }

        float LakeHeight = 0.0f;
        for (const FNatureLakeFeature& Lake : Lakes)
        {
            const float SafeRadius = FMath::Max(0.01f, Lake.Radius);
            const float Distance = FVector2D::Distance(UV, Lake.Center) / SafeRadius;
            if (Distance <= 1.0f)
            {
                const float BowlAlpha = 1.0f - (Distance * Distance);
                LakeHeight -= Lake.Depth * BowlAlpha;
            }

            const float RimDistance = Distance - 1.0f;
            if (RimDistance > -0.4f && RimDistance < 0.8f)
            {
                const float RimSigma = 0.22f;
                LakeHeight += Lake.RimHeight *
                    FMath::Exp(-(RimDistance * RimDistance) / (2.0f * RimSigma * RimSigma));
            }
        }

        float RiverHeight = 0.0f;
        for (int32 RiverIndex = 0; RiverIndex < Rivers.Num(); ++RiverIndex)
        {
            RiverHeight += EvaluateRiverCarve(UV, Rivers[RiverIndex], Seed + 4001, RiverIndex);
        }

        const float SurfaceNoise = 0.06f * SampleFractalNoise(UV + FVector2D(0.24f, 0.18f), Seed + 401, 18.0f, 3);
        const float ShoreDrop = (1.0f - IslandMask) * 0.35f;
        const float RawHeight = 0.25f + BaseNoise + MountainHeight + LakeHeight + RiverHeight + SurfaceNoise;
        return (IslandMask * RawHeight) - ShoreDrop;
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

bool FUEAIAgentSceneTools::LandscapeGenerate(const FUEAIAgentLandscapeGenerateParams& Params, FString& OutMessage)
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

    FString Theme = Params.Theme.TrimStartAndEnd().ToLower();
    if (Theme.IsEmpty())
    {
        Theme = TEXT("nature_island");
    }

    const bool bMoonSurface =
        Theme == TEXT("moon_surface") ||
        Theme == TEXT("moon") ||
        Theme == TEXT("lunar");
    const bool bNatureIsland =
        Theme == TEXT("nature_island") ||
        Theme == TEXT("nature") ||
        Theme == TEXT("island");
    if (!bMoonSurface && !bNatureIsland)
    {
        OutMessage = TEXT("Unsupported landscape theme. Use moon_surface or nature_island.");
        return false;
    }
    const EUEAIAgentLandscapeDetailTier DetailTier = ResolveLandscapeDetailTier(Params.DetailLevel, bMoonSurface);
    const float DetailScale = LandscapeDetailScale(DetailTier);
    const EUEAIAgentMoonProfile MoonProfile = ResolveMoonProfile(Params.MoonProfile, bMoonSurface);

    TArray<ALandscapeProxy*> TargetLandscapes;
    CollectLandscapeTargets(World, Params.ActorNames, Params.bUseSelectionIfActorNamesEmpty, TargetLandscapes);
    bool bUsedAreaFallback = false;
    if (TargetLandscapes.IsEmpty())
    {
        if (Params.bUseFullArea)
        {
            CollectAllLandscapeTargets(World, TargetLandscapes);
        }
        else
        {
            bUsedAreaFallback = ResolveLandscapeTargetsForArea(World, Params.Center, Params.Size, TargetLandscapes);
        }
    }

    if (TargetLandscapes.IsEmpty())
    {
        OutMessage = TEXT("No target landscape actors found. Select a landscape actor or provide actorNames.");
        return false;
    }

    const float MaxHeight = FMath::Clamp(Params.MaxHeight, 100.0f, 10000.0f);
    int32 MountainCount = FMath::Clamp(Params.MountainCount, 1, 8);
    float MountainWidthMin = Params.MountainWidthMin > 0.0f ? FMath::Clamp(Params.MountainWidthMin, 1.0f, 200000.0f) : 0.0f;
    float MountainWidthMax = Params.MountainWidthMax > 0.0f ? FMath::Clamp(Params.MountainWidthMax, 1.0f, 200000.0f) : 0.0f;
    if (MountainWidthMin > 0.0f && MountainWidthMax > 0.0f && MountainWidthMin > MountainWidthMax)
    {
        Swap(MountainWidthMin, MountainWidthMax);
    }
    int32 RiverCountMin = Params.RiverCountMin > 0 ? FMath::Clamp(Params.RiverCountMin, 0, 32) : 0;
    int32 RiverCountMax = Params.RiverCountMax > 0 ? FMath::Clamp(Params.RiverCountMax, 0, 32) : 0;
    if (RiverCountMin > 0 && RiverCountMax > 0 && RiverCountMin > RiverCountMax)
    {
        Swap(RiverCountMin, RiverCountMax);
    }
    float RiverWidthMin = Params.RiverWidthMin > 0.0f ? FMath::Clamp(Params.RiverWidthMin, 1.0f, 200000.0f) : 0.0f;
    float RiverWidthMax = Params.RiverWidthMax > 0.0f ? FMath::Clamp(Params.RiverWidthMax, 1.0f, 200000.0f) : 0.0f;
    if (RiverWidthMin > 0.0f && RiverWidthMax > 0.0f && RiverWidthMin > RiverWidthMax)
    {
        Swap(RiverWidthMin, RiverWidthMax);
    }
    int32 LakeCountMin = Params.LakeCountMin > 0 ? FMath::Clamp(Params.LakeCountMin, 0, 32) : 0;
    int32 LakeCountMax = Params.LakeCountMax > 0 ? FMath::Clamp(Params.LakeCountMax, 0, 32) : 0;
    if (LakeCountMin > 0 && LakeCountMax > 0 && LakeCountMin > LakeCountMax)
    {
        Swap(LakeCountMin, LakeCountMax);
    }
    float LakeWidthMin = Params.LakeWidthMin > 0.0f ? FMath::Clamp(Params.LakeWidthMin, 1.0f, 200000.0f) : 0.0f;
    float LakeWidthMax = Params.LakeWidthMax > 0.0f ? FMath::Clamp(Params.LakeWidthMax, 1.0f, 200000.0f) : 0.0f;
    if (LakeWidthMin > 0.0f && LakeWidthMax > 0.0f && LakeWidthMin > LakeWidthMax)
    {
        Swap(LakeWidthMin, LakeWidthMax);
    }
    int32 CraterCountMin = Params.CraterCountMin > 0 ? FMath::Clamp(Params.CraterCountMin, 1, 500) : 0;
    int32 CraterCountMax = Params.CraterCountMax > 0 ? FMath::Clamp(Params.CraterCountMax, 1, 500) : 0;
    if (CraterCountMin > 0 && CraterCountMax > 0 && CraterCountMin > CraterCountMax)
    {
        Swap(CraterCountMin, CraterCountMax);
    }
    float CraterWidthMin = Params.CraterWidthMin > 0.0f ? FMath::Clamp(Params.CraterWidthMin, 1.0f, 200000.0f) : 0.0f;
    float CraterWidthMax = Params.CraterWidthMax > 0.0f ? FMath::Clamp(Params.CraterWidthMax, 1.0f, 200000.0f) : 0.0f;
    if (CraterWidthMin > 0.0f && CraterWidthMax > 0.0f && CraterWidthMin > CraterWidthMax)
    {
        Swap(CraterWidthMin, CraterWidthMax);
    }
    const bool bHasExplicitCraterCount = CraterCountMin > 0 || CraterCountMax > 0;
    const bool bHasExplicitCraterWidth = CraterWidthMin > 0.0f || CraterWidthMax > 0.0f;
    const bool bHasStrictCraterConstraints = bHasExplicitCraterCount || bHasExplicitCraterWidth;
    const float MicroCraterScale = !bHasStrictCraterConstraints
        ? 1.0f
        : (CraterCountMax > 0 && CraterCountMax <= 20)
        ? 0.0f
        : (CraterWidthMin > 0.0f)
        ? 0.15f
        : 0.35f;
    int32 Seed = Params.Seed;
    if (Seed == 0)
    {
        Seed = FMath::RandRange(1, TNumericLimits<int32>::Max() - 1);
    }
    if (bNatureIsland && Params.MountainCount <= 0)
    {
        FRandomStream MountainCountStream(Seed + 17);
        MountainCount = MountainCountStream.RandRange(1, 3);
    }

    const FScopedTransaction Transaction(LOCTEXT("LandscapeGenerateTransaction", "UE AI Agent Landscape Generate"));
    int32 UpdatedLandscapes = 0;
    int32 SkippedTooLarge = 0;

    for (int32 LandscapeIndex = 0; LandscapeIndex < TargetLandscapes.Num(); ++LandscapeIndex)
    {
        ALandscapeProxy* Landscape = TargetLandscapes[LandscapeIndex];
        if (!Landscape)
        {
            continue;
        }

        int32 MinX = 0;
        int32 MinY = 0;
        int32 MaxX = 0;
        int32 MaxY = 0;
        const bool bHasRect = Params.bUseFullArea
            ? ComputeLandscapeFullRect(Landscape, MinX, MinY, MaxX, MaxY)
            : ComputeLandscapeEditRect(Landscape, Params.Center, Params.Size, MinX, MinY, MaxX, MaxY);
        if (!bHasRect)
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
        if (Width <= 1 || Height <= 1)
        {
            continue;
        }
        const int64 SampleCount = static_cast<int64>(Width) * static_cast<int64>(Height);
        if (SampleCount > 12000000)
        {
            SkippedTooLarge += 1;
            continue;
        }

        const FVector LandscapeScale = Landscape->GetActorScale3D();
        const float ScaleX = FMath::Max(KINDA_SMALL_NUMBER, FMath::Abs(LandscapeScale.X));
        const float ScaleY = FMath::Max(KINDA_SMALL_NUMBER, FMath::Abs(LandscapeScale.Y));
        const float ScaleZ = FMath::Max(KINDA_SMALL_NUMBER, FMath::Abs(LandscapeScale.Z));

        FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo, ResolveLandscapeEditLayerGuid(Landscape));
        TArray<uint16> HeightData;
        HeightData.SetNumUninitialized(Width * Height);
        LandscapeEdit.GetHeightDataFast(MinX, MinY, MaxX, MaxY, HeightData.GetData(), 0);

        int64 HeightSum = 0;
        for (const uint16 HeightValue : HeightData)
        {
            HeightSum += HeightValue;
        }
        const int32 BaseHeight = HeightData.Num() > 0
            ? static_cast<int32>(HeightSum / HeightData.Num())
            : 32768;

        TArray<FMoonCraterFeature> MoonCraters;
        TArray<FVector4f> NatureMountains;
        TArray<FNatureLakeFeature> NatureLakes;
        TArray<FNatureRiverFeature> NatureRivers;
        const int32 LandscapeSeed = Seed + (LandscapeIndex * 1013);
        FRandomStream Stream(LandscapeSeed);
        if (bMoonSurface)
        {
            const float DensityFromCount = FMath::Lerp(0.65f, 1.35f, static_cast<float>(MountainCount - 1) / 7.0f);
            const int32 BaseCraterCount = FMath::Clamp((Width * Height) / 65000, 10, 44);
            int32 CraterCount = FMath::Clamp(
                FMath::RoundToInt(static_cast<float>(BaseCraterCount) * DetailScale * DensityFromCount),
                10,
                500);
            const bool bAncientCratered = MoonProfile == EUEAIAgentMoonProfile::AncientHeavilyCratered;
            if (bAncientCratered)
            {
                CraterCount = FMath::Clamp(FMath::RoundToInt(static_cast<float>(CraterCount) * 2.3f), 70, 500);
            }
            if (CraterCountMin > 0)
            {
                CraterCount = FMath::Max(CraterCount, CraterCountMin);
            }
            if (CraterCountMax > 0)
            {
                CraterCount = FMath::Min(CraterCount, CraterCountMax);
            }
            CraterCount = FMath::Clamp(CraterCount, 1, 500);

            float RadiusMin = FMath::Clamp(0.012f / FMath::Sqrt(DetailScale), 0.008f, 0.020f);
            float RadiusMax = FMath::Clamp(0.082f / FMath::Sqrt(DetailScale), 0.045f, 0.110f);
            if (CraterWidthMin > 0.0f || CraterWidthMax > 0.0f)
            {
                const float AreaWorldWidth = FMath::Max(1.0f, static_cast<float>(Width - 1) * ScaleX);
                const float AreaWorldHeight = FMath::Max(1.0f, static_cast<float>(Height - 1) * ScaleY);
                const float AreaWorldSpan = FMath::Max(1.0f, 0.5f * (AreaWorldWidth + AreaWorldHeight));
                if (CraterWidthMin > 0.0f)
                {
                    const float RequestedRadiusMin = 0.5f * (CraterWidthMin / AreaWorldSpan);
                    RadiusMin = FMath::Max(RadiusMin, RequestedRadiusMin);
                }
                if (CraterWidthMax > 0.0f)
                {
                    const float RequestedRadiusMax = 0.5f * (CraterWidthMax / AreaWorldSpan);
                    RadiusMax = FMath::Min(RadiusMax, RequestedRadiusMax);
                }
            }
            RadiusMin = FMath::Clamp(RadiusMin, 0.003f, 0.45f);
            RadiusMax = FMath::Clamp(RadiusMax, RadiusMin, 0.49f);
            const float DepthMin = FMath::Clamp(0.18f * DetailScale, 0.15f, 0.95f);
            const float DepthMax = FMath::Clamp(0.65f * DetailScale, 0.30f, 1.35f);
            if (bAncientCratered && !bHasStrictCraterConstraints)
            {
                const FMoonCraterFeature DominantCrater = [&]()
                {
                    FMoonCraterFeature Feature;
                    const bool bLeftSide = Stream.FRand() < 0.5f;
                    Feature.Center = FVector2D(
                        bLeftSide ? Stream.FRandRange(0.10f, 0.28f) : Stream.FRandRange(0.72f, 0.90f),
                        Stream.FRandRange(0.22f, 0.78f));
                    Feature.Radius = FMath::Clamp(RadiusMax * Stream.FRandRange(1.35f, 1.95f), 0.14f, 0.34f);
                    Feature.Depth = FMath::Clamp(DepthMax * Stream.FRandRange(0.8f, 1.05f), 0.35f, 1.5f);
                    Feature.Age = Stream.FRandRange(0.25f, 0.65f);
                    Feature.Ejecta = Stream.FRandRange(0.35f, 0.80f);
                    Feature.Terrace = Stream.FRandRange(0.55f, 1.0f);
                    Feature.Aspect = Stream.FRandRange(0.88f, 1.12f);
                    Feature.RotationRad = Stream.FRandRange(0.0f, 2.0f * PI);
                    return Feature;
                }();
                MoonCraters.Add(DominantCrater);
            }

            MoonCraters.Reserve(CraterCount + (CraterCount / 3) + 2);
            for (int32 CraterIndex = 0; CraterIndex < CraterCount; ++CraterIndex)
            {
                const float SizeSelector = Stream.FRand();
                const float SmallMax = FMath::Lerp(RadiusMin, RadiusMax, bAncientCratered ? 0.34f : 0.45f);
                const float MediumMax = FMath::Lerp(RadiusMin, RadiusMax, bAncientCratered ? 0.68f : 0.78f);

                FMoonCraterFeature Feature;
                if (SizeSelector < (bAncientCratered ? 0.76f : 0.66f))
                {
                    Feature.Radius = Stream.FRandRange(RadiusMin, SmallMax);
                }
                else if (SizeSelector < 0.95f)
                {
                    Feature.Radius = Stream.FRandRange(SmallMax, MediumMax);
                }
                else
                {
                    Feature.Radius = Stream.FRandRange(MediumMax, RadiusMax);
                }

                Feature.Center = FVector2D(
                    Stream.FRandRange(0.03f, 0.97f),
                    Stream.FRandRange(0.03f, 0.97f));
                Feature.Age = bAncientCratered
                    ? Stream.FRandRange(0.35f, 1.0f)
                    : Stream.FRandRange(0.08f, 0.90f);
                const float RadiusDepthAlpha = FMath::Clamp((Feature.Radius - RadiusMin) / FMath::Max(0.001f, RadiusMax - RadiusMin), 0.0f, 1.0f);
                const float BaseDepth = FMath::Lerp(DepthMin, DepthMax, RadiusDepthAlpha);
                Feature.Depth = BaseDepth * FMath::Lerp(1.05f, 0.55f, Feature.Age);
                Feature.Ejecta = Stream.FRandRange(0.35f, 1.0f) * FMath::Lerp(1.0f, 0.35f, Feature.Age);
                Feature.Terrace = Feature.Radius > (0.55f * RadiusMax) ? Stream.FRandRange(0.20f, 1.0f) : 0.0f;
                Feature.Aspect = bAncientCratered
                    ? Stream.FRandRange(0.82f, 1.20f)
                    : Stream.FRandRange(0.90f, 1.12f);
                Feature.RotationRad = Stream.FRandRange(0.0f, 2.0f * PI);
                MoonCraters.Add(Feature);

                const bool bAllowNestedCrater = !bHasStrictCraterConstraints;
                const bool bCanHaveNestedCrater = bAllowNestedCrater && Feature.Radius > (SmallMax * 0.8f);
                if (bCanHaveNestedCrater && Stream.FRand() < (bAncientCratered ? 0.42f : 0.24f))
                {
                    const int32 NestedCount = Stream.RandRange(1, bAncientCratered ? 3 : 2);
                    for (int32 NestedIndex = 0; NestedIndex < NestedCount; ++NestedIndex)
                    {
                        FMoonCraterFeature NestedFeature;
                        const float OffsetRadius = Feature.Radius * Stream.FRandRange(0.08f, 0.55f);
                        const float OffsetAngle = Stream.FRandRange(0.0f, 2.0f * PI);
                        NestedFeature.Center = Feature.Center + FVector2D(
                            OffsetRadius * FMath::Cos(OffsetAngle),
                            OffsetRadius * FMath::Sin(OffsetAngle));
                        NestedFeature.Center.X = FMath::Clamp(NestedFeature.Center.X, 0.02f, 0.98f);
                        NestedFeature.Center.Y = FMath::Clamp(NestedFeature.Center.Y, 0.02f, 0.98f);
                        NestedFeature.Radius = FMath::Clamp(
                            Feature.Radius * Stream.FRandRange(0.14f, 0.34f),
                            RadiusMin,
                            RadiusMax);
                        NestedFeature.Age = Stream.FRandRange(0.03f, 0.60f);
                        NestedFeature.Depth = Feature.Depth * Stream.FRandRange(0.35f, 0.78f);
                        NestedFeature.Ejecta = Stream.FRandRange(0.45f, 1.0f);
                        NestedFeature.Terrace = 0.0f;
                        NestedFeature.Aspect = Stream.FRandRange(0.90f, 1.10f);
                        NestedFeature.RotationRad = Stream.FRandRange(0.0f, 2.0f * PI);
                        MoonCraters.Add(NestedFeature);
                    }
                }
            }
        }
        else
        {
            const float AreaWorldWidth = FMath::Max(1.0f, static_cast<float>(Width - 1) * ScaleX);
            const float AreaWorldHeight = FMath::Max(1.0f, static_cast<float>(Height - 1) * ScaleY);
            const float AreaWorldSpan = FMath::Max(1.0f, 0.5f * (AreaWorldWidth + AreaWorldHeight));
            const float DetailAlpha = FMath::Clamp((DetailScale - 0.72f) / (1.62f - 0.72f), 0.0f, 1.0f);

            float MountainRadiusMin = DetailTier == EUEAIAgentLandscapeDetailTier::Low
                ? 0.10f
                : DetailTier == EUEAIAgentLandscapeDetailTier::Medium
                ? 0.07f
                : DetailTier == EUEAIAgentLandscapeDetailTier::High
                ? 0.05f
                : 0.04f;
            float MountainRadiusMax = DetailTier == EUEAIAgentLandscapeDetailTier::Low
                ? 0.20f
                : DetailTier == EUEAIAgentLandscapeDetailTier::Medium
                ? 0.16f
                : DetailTier == EUEAIAgentLandscapeDetailTier::High
                ? 0.14f
                : 0.12f;
            if (MountainWidthMin > 0.0f || MountainWidthMax > 0.0f)
            {
                if (MountainWidthMin > 0.0f)
                {
                    MountainRadiusMin = FMath::Max(MountainRadiusMin, 0.5f * (MountainWidthMin / AreaWorldSpan));
                }
                if (MountainWidthMax > 0.0f)
                {
                    MountainRadiusMax = FMath::Min(MountainRadiusMax, 0.5f * (MountainWidthMax / AreaWorldSpan));
                }
            }
            MountainRadiusMin = FMath::Clamp(MountainRadiusMin, 0.02f, 0.45f);
            MountainRadiusMax = FMath::Clamp(MountainRadiusMax, MountainRadiusMin, 0.48f);

            NatureMountains.Reserve(MountainCount);
            for (int32 MountainIndex = 0; MountainIndex < MountainCount; ++MountainIndex)
            {
                const float Angle = Stream.FRandRange(0.0f, 2.0f * PI);
                const float RadiusOffset = Stream.FRandRange(0.0f, 0.22f);
                const FVector2D Center(0.5f + RadiusOffset * FMath::Cos(Angle), 0.5f + RadiusOffset * FMath::Sin(Angle));
                NatureMountains.Add(FVector4f(
                    FMath::Clamp(Center.X, 0.08f, 0.92f),
                    FMath::Clamp(Center.Y, 0.08f, 0.92f),
                    Stream.FRandRange(MountainRadiusMin, MountainRadiusMax),
                    Stream.FRandRange(0.55f, 1.10f)));
            }

            int32 ResolvedLakeMin = LakeCountMin;
            int32 ResolvedLakeMax = LakeCountMax;
            if (ResolvedLakeMin > ResolvedLakeMax)
            {
                Swap(ResolvedLakeMin, ResolvedLakeMax);
            }
            const int32 LakeCount = ResolvedLakeMax > 0 ? Stream.RandRange(ResolvedLakeMin, ResolvedLakeMax) : 0;

            float LakeDiameterMin = DetailTier == EUEAIAgentLandscapeDetailTier::Low
                ? 0.12f
                : DetailTier == EUEAIAgentLandscapeDetailTier::Medium
                ? 0.09f
                : DetailTier == EUEAIAgentLandscapeDetailTier::High
                ? 0.07f
                : 0.05f;
            float LakeDiameterMax = DetailTier == EUEAIAgentLandscapeDetailTier::Low
                ? 0.22f
                : DetailTier == EUEAIAgentLandscapeDetailTier::Medium
                ? 0.18f
                : DetailTier == EUEAIAgentLandscapeDetailTier::High
                ? 0.15f
                : 0.12f;
            if (LakeWidthMin > 0.0f || LakeWidthMax > 0.0f)
            {
                if (LakeWidthMin > 0.0f)
                {
                    LakeDiameterMin = FMath::Max(LakeDiameterMin, LakeWidthMin / AreaWorldSpan);
                }
                if (LakeWidthMax > 0.0f)
                {
                    LakeDiameterMax = FMath::Min(LakeDiameterMax, LakeWidthMax / AreaWorldSpan);
                }
            }
            LakeDiameterMin = FMath::Clamp(LakeDiameterMin, 0.03f, 0.84f);
            LakeDiameterMax = FMath::Clamp(LakeDiameterMax, LakeDiameterMin, 0.88f);

            NatureLakes.Reserve(LakeCount);
            for (int32 LakeIndex = 0; LakeIndex < LakeCount; ++LakeIndex)
            {
                FNatureLakeFeature Lake;
                const float Angle = Stream.FRandRange(0.0f, 2.0f * PI);
                const float Offset = Stream.FRandRange(0.05f, 0.30f);
                Lake.Center = FVector2D(0.5f + Offset * FMath::Cos(Angle), 0.5f + Offset * FMath::Sin(Angle));
                Lake.Center.X = FMath::Clamp(Lake.Center.X, 0.10f, 0.90f);
                Lake.Center.Y = FMath::Clamp(Lake.Center.Y, 0.10f, 0.90f);
                Lake.Radius = 0.5f * Stream.FRandRange(LakeDiameterMin, LakeDiameterMax);
                Lake.Depth = Stream.FRandRange(0.10f, 0.28f) * FMath::Lerp(0.88f, 1.15f, DetailAlpha);
                Lake.RimHeight = Stream.FRandRange(0.02f, 0.07f);
                NatureLakes.Add(Lake);
            }

            int32 ResolvedRiverMin = RiverCountMin;
            int32 ResolvedRiverMax = RiverCountMax;
            if (ResolvedRiverMin > ResolvedRiverMax)
            {
                Swap(ResolvedRiverMin, ResolvedRiverMax);
            }
            int32 RiverCount = ResolvedRiverMax > 0 ? Stream.RandRange(ResolvedRiverMin, ResolvedRiverMax) : 0;
            RiverCount = FMath::Clamp(RiverCount, 0, 32);

            float RiverWidthNormMin = DetailTier == EUEAIAgentLandscapeDetailTier::Low
                ? 0.028f
                : DetailTier == EUEAIAgentLandscapeDetailTier::Medium
                ? 0.020f
                : DetailTier == EUEAIAgentLandscapeDetailTier::High
                ? 0.014f
                : 0.010f;
            float RiverWidthNormMax = DetailTier == EUEAIAgentLandscapeDetailTier::Low
                ? 0.045f
                : DetailTier == EUEAIAgentLandscapeDetailTier::Medium
                ? 0.035f
                : DetailTier == EUEAIAgentLandscapeDetailTier::High
                ? 0.028f
                : 0.022f;
            if (RiverWidthMin > 0.0f || RiverWidthMax > 0.0f)
            {
                if (RiverWidthMin > 0.0f)
                {
                    RiverWidthNormMin = FMath::Max(RiverWidthNormMin, RiverWidthMin / AreaWorldSpan);
                }
                if (RiverWidthMax > 0.0f)
                {
                    RiverWidthNormMax = FMath::Min(RiverWidthNormMax, RiverWidthMax / AreaWorldSpan);
                }
            }
            RiverWidthNormMin = FMath::Clamp(RiverWidthNormMin, 0.006f, 0.26f);
            RiverWidthNormMax = FMath::Clamp(RiverWidthNormMax, RiverWidthNormMin, 0.30f);

            NatureRivers.Reserve(RiverCount);
            for (int32 RiverIndex = 0; RiverIndex < RiverCount; ++RiverIndex)
            {
                FNatureRiverFeature River;
                const FVector4f& SourceMountain = NatureMountains[Stream.RandRange(0, NatureMountains.Num() - 1)];
                const FVector2D Start(SourceMountain.X, SourceMountain.Y);
                FVector2D End;
                if (!NatureLakes.IsEmpty() && Stream.FRand() < 0.45f)
                {
                    const FNatureLakeFeature& TargetLake = NatureLakes[Stream.RandRange(0, NatureLakes.Num() - 1)];
                    End = TargetLake.Center;
                }
                else
                {
                    FVector2D Outward = Start - FVector2D(0.5f, 0.5f);
                    if (Outward.SizeSquared() < KINDA_SMALL_NUMBER)
                    {
                        Outward = FVector2D(Stream.FRandRange(-1.0f, 1.0f), Stream.FRandRange(-1.0f, 1.0f));
                    }
                    Outward.Normalize();
                    End = FVector2D(0.5f, 0.5f) + (Outward * Stream.FRandRange(0.42f, 0.52f));
                }
                End.X = FMath::Clamp(End.X, 0.04f, 0.96f);
                End.Y = FMath::Clamp(End.Y, 0.04f, 0.96f);

                const int32 PathSteps = Stream.RandRange(6, 10);
                const FVector2D Direction = End - Start;
                FVector2D Perp(-Direction.Y, Direction.X);
                if (Perp.SizeSquared() < KINDA_SMALL_NUMBER)
                {
                    Perp = FVector2D(0.0f, 1.0f);
                }
                Perp.Normalize();
                const float Meander = Stream.FRandRange(0.02f, 0.08f) * (DetailScale * 0.8f);

                River.PathPoints.Reserve(PathSteps + 1);
                for (int32 StepIndex = 0; StepIndex <= PathSteps; ++StepIndex)
                {
                    const float T = static_cast<float>(StepIndex) / static_cast<float>(FMath::Max(1, PathSteps));
                    FVector2D Point = FMath::Lerp(Start, End, T);
                    if (StepIndex > 0 && StepIndex < PathSteps)
                    {
                        const float Wave = FMath::Sin((T * PI * 2.0f) + Stream.FRandRange(0.0f, PI * 2.0f));
                        Point += Perp * (Wave * Meander * FMath::Lerp(1.0f, 0.4f, T));
                    }
                    Point.X = FMath::Clamp(Point.X, 0.03f, 0.97f);
                    Point.Y = FMath::Clamp(Point.Y, 0.03f, 0.97f);
                    River.PathPoints.Add(Point);
                }

                River.Width = Stream.FRandRange(RiverWidthNormMin, RiverWidthNormMax);
                River.Depth = Stream.FRandRange(0.10f, 0.26f);
                River.BankHeight = Stream.FRandRange(0.02f, 0.06f);
                NatureRivers.Add(River);
            }
        }

        TArray<float> RawHeightData;
        RawHeightData.SetNumUninitialized(Width * Height);
        float RawMin = TNumericLimits<float>::Max();
        float RawMax = TNumericLimits<float>::Lowest();
        double RawSum = 0.0;
        double RawSquaredSum = 0.0;
        for (int32 Y = MinY; Y <= MaxY; ++Y)
        {
            const float V = Height > 1
                ? static_cast<float>(Y - MinY) / static_cast<float>(Height - 1)
                : 0.5f;
            for (int32 X = MinX; X <= MaxX; ++X)
            {
                const float U = Width > 1
                    ? static_cast<float>(X - MinX) / static_cast<float>(Width - 1)
                    : 0.5f;
                const FVector2D UV(U, V);
                const int32 DataIndex = (Y - MinY) * Width + (X - MinX);
                const float RawHeight = bMoonSurface
                    ? EvaluateMoonSurfaceRaw(UV, MoonCraters, LandscapeSeed, DetailScale, MoonProfile, MicroCraterScale)
                    : EvaluateNatureIslandRaw(UV, NatureMountains, NatureLakes, NatureRivers, LandscapeSeed);
                RawHeightData[DataIndex] = RawHeight;
                RawMin = FMath::Min(RawMin, RawHeight);
                RawMax = FMath::Max(RawMax, RawHeight);
                RawSum += static_cast<double>(RawHeight);
                RawSquaredSum += static_cast<double>(RawHeight) * static_cast<double>(RawHeight);
            }
        }

        if (RawMax <= RawMin)
        {
            continue;
        }

        const float DetailAlpha = FMath::Clamp((DetailScale - 0.72f) / (1.62f - 0.72f), 0.0f, 1.0f);
        const float TargetMinHeight = bMoonSurface
            ? (FMath::Lerp(-0.30f, -0.52f, DetailAlpha) * MaxHeight)
            : (-0.12f * MaxHeight);
        const float TargetMaxHeight = bMoonSurface
            ? (FMath::Lerp(0.56f, 0.95f, DetailAlpha) * MaxHeight)
            : MaxHeight;
        const float RawRange = FMath::Max(KINDA_SMALL_NUMBER, RawMax - RawMin);
        const double SafeSampleCount = RawHeightData.Num() > 0 ? static_cast<double>(RawHeightData.Num()) : 1.0;
        const float RawMean = static_cast<float>(RawSum / SafeSampleCount);
        const double RawVarianceRaw = (RawSquaredSum / SafeSampleCount) - (static_cast<double>(RawMean) * static_cast<double>(RawMean));
        const float RawStdDev = FMath::Sqrt(FMath::Max(0.0f, static_cast<float>(RawVarianceRaw)));
        const float StdRange = FMath::Max(0.18f, 2.35f * RawStdDev);
        const float StdMin = RawMean - StdRange;
        const float StdMax = RawMean + StdRange;
        const float StdBlend = DetailTier == EUEAIAgentLandscapeDetailTier::Low
            ? 0.35f
            : DetailTier == EUEAIAgentLandscapeDetailTier::Medium
            ? 0.55f
            : DetailTier == EUEAIAgentLandscapeDetailTier::High
            ? 0.72f
            : 0.82f;

        bool bEditedLandscape = false;
        for (int32 DataIndex = 0; DataIndex < RawHeightData.Num(); ++DataIndex)
        {
            const float MinMaxNormalized = (RawHeightData[DataIndex] - RawMin) / RawRange;
            float Normalized = MinMaxNormalized;
            if (bMoonSurface && RawStdDev > KINDA_SMALL_NUMBER)
            {
                const float StdNormalized = FMath::Clamp(
                    (RawHeightData[DataIndex] - StdMin) / FMath::Max(KINDA_SMALL_NUMBER, StdMax - StdMin),
                    0.0f,
                    1.0f);
                Normalized = FMath::Lerp(MinMaxNormalized, StdNormalized, StdBlend);
            }
            const float WorldOffset = FMath::Lerp(TargetMinHeight, TargetMaxHeight, Normalized);
            const int32 HeightDelta = FMath::RoundToInt((WorldOffset * 128.0f) / ScaleZ);
            const int32 NewHeight = FMath::Clamp(BaseHeight + HeightDelta, 0, 65535);
            const int32 CurrentHeight = HeightData[DataIndex];
            if (NewHeight == CurrentHeight)
            {
                continue;
            }

            HeightData[DataIndex] = static_cast<uint16>(NewHeight);
            bEditedLandscape = true;
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
        OutMessage = TEXT("Could not generate landscape in the requested area. Check target landscape and bounds.");
        if (SkippedTooLarge > 0)
        {
            OutMessage += TEXT(" Generation area is too large for safe execution.");
        }
        return false;
    }

    if (bMoonSurface)
    {
        const FString CraterCountText = (CraterCountMin > 0 || CraterCountMax > 0)
            ? FString::Printf(TEXT("%d-%d"), CraterCountMin > 0 ? CraterCountMin : 1, CraterCountMax > 0 ? CraterCountMax : 500)
            : TEXT("auto");
        const FString CraterWidthText = (CraterWidthMin > 0.0f || CraterWidthMax > 0.0f)
            ? FString::Printf(TEXT("%.0f-%.0f"), CraterWidthMin > 0.0f ? CraterWidthMin : 1.0f, CraterWidthMax > 0.0f ? CraterWidthMax : 200000.0f)
            : TEXT("auto");
        OutMessage = FString::Printf(
            TEXT("Generated moon surface over %s. Affected landscapes=%d, detail=%s, profile=%s, maxHeight=%.0f, craterDensity=%d, craterCount=%s, craterWidth=%s, seed=%d."),
            Params.bUseFullArea ? TEXT("full landscape area") : TEXT("bounded area"),
            UpdatedLandscapes,
            LandscapeDetailTierToText(DetailTier),
            MoonProfileToText(MoonProfile),
            MaxHeight,
            MountainCount,
            *CraterCountText,
            *CraterWidthText,
            Seed);
    }
    else
    {
        const FString MountainWidthText = (MountainWidthMin > 0.0f || MountainWidthMax > 0.0f)
            ? FString::Printf(TEXT("%.0f-%.0f"), MountainWidthMin > 0.0f ? MountainWidthMin : 1.0f, MountainWidthMax > 0.0f ? MountainWidthMax : 200000.0f)
            : TEXT("auto");
        const FString RiverCountText = (RiverCountMin > 0 || RiverCountMax > 0)
            ? FString::Printf(TEXT("%d-%d"), RiverCountMin > 0 ? RiverCountMin : 0, RiverCountMax > 0 ? RiverCountMax : 32)
            : TEXT("none");
        const FString RiverWidthText = (RiverWidthMin > 0.0f || RiverWidthMax > 0.0f)
            ? FString::Printf(TEXT("%.0f-%.0f"), RiverWidthMin > 0.0f ? RiverWidthMin : 1.0f, RiverWidthMax > 0.0f ? RiverWidthMax : 200000.0f)
            : TEXT("n/a");
        const FString LakeCountText = (LakeCountMin > 0 || LakeCountMax > 0)
            ? FString::Printf(TEXT("%d-%d"), LakeCountMin > 0 ? LakeCountMin : 0, LakeCountMax > 0 ? LakeCountMax : 32)
            : TEXT("none");
        const FString LakeWidthText = (LakeWidthMin > 0.0f || LakeWidthMax > 0.0f)
            ? FString::Printf(TEXT("%.0f-%.0f"), LakeWidthMin > 0.0f ? LakeWidthMin : 1.0f, LakeWidthMax > 0.0f ? LakeWidthMax : 200000.0f)
            : TEXT("n/a");
        OutMessage = FString::Printf(
            TEXT("Generated nature island over %s. Affected landscapes=%d, detail=%s, maxHeight=%.0f, mountains=%d, mountainWidth=%s, rivers=%s, riverWidth=%s, lakes=%s, lakeWidth=%s, seed=%d."),
            Params.bUseFullArea ? TEXT("full landscape area") : TEXT("bounded area"),
            UpdatedLandscapes,
            LandscapeDetailTierToText(DetailTier),
            MaxHeight,
            MountainCount,
            *MountainWidthText,
            *RiverCountText,
            *RiverWidthText,
            *LakeCountText,
            *LakeWidthText,
            Seed);
    }
    if (bUsedAreaFallback)
    {
        OutMessage += TEXT(" Target was auto-resolved.");
    }
    if (SkippedTooLarge > 0)
    {
        OutMessage += FString::Printf(TEXT(" Skipped %d oversized landscape area(s)."), SkippedTooLarge);
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
