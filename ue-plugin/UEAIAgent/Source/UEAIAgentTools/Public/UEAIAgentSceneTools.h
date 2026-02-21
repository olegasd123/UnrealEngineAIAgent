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

struct FUEAIAgentLandscapeSculptParams
{
    TArray<FString> ActorNames;
    FVector2D Center = FVector2D::ZeroVector;
    FVector2D Size = FVector2D(1000.0f, 1000.0f);
    float Strength = 0.2f;
    float Falloff = 0.5f;
    bool bLower = false;
    bool bUseSelectionIfActorNamesEmpty = true;
};

struct FUEAIAgentLandscapePaintLayerParams
{
    TArray<FString> ActorNames;
    FVector2D Center = FVector2D::ZeroVector;
    FVector2D Size = FVector2D(1000.0f, 1000.0f);
    FString LayerName;
    float Strength = 0.4f;
    float Falloff = 0.5f;
    bool bRemove = false;
    bool bUseSelectionIfActorNamesEmpty = true;
};

struct FUEAIAgentLandscapeGenerateParams
{
    TArray<FString> ActorNames;
    FString Theme = TEXT("nature_island");
    FString DetailLevel = TEXT("medium");
    FString MoonProfile = TEXT("moon_surface");
    bool bUseFullArea = true;
    FVector2D Center = FVector2D::ZeroVector;
    FVector2D Size = FVector2D(1000.0f, 1000.0f);
    int32 Seed = 0;
    int32 MountainCount = 2;
    FString MountainStyle = TEXT("sharp_peaks");
    float MountainWidthMin = 0.0f;
    float MountainWidthMax = 0.0f;
    float MaxHeight = 5000.0f;
    int32 CraterCountMin = 0;
    int32 CraterCountMax = 0;
    float CraterWidthMin = 0.0f;
    float CraterWidthMax = 0.0f;
    bool bUseSelectionIfActorNamesEmpty = true;
};

struct FUEAIAgentPcgCreateGraphParams
{
    FString AssetPath;
    FString TemplatePath;
    bool bOverwrite = false;
};

struct FUEAIAgentPcgPlaceOnLandscapeParams
{
    TArray<FString> ActorNames;
    FString GraphSource = TEXT("last");
    FString GraphPath;
    bool bUseFullArea = false;
    FVector2D Size = FVector2D(3000.0f, 3000.0f);
    bool bHasSize = false;
    bool bTargetAll = false;
    bool bUseSelectionIfActorNamesEmpty = true;
};

struct FUEAIAgentPcgAddConnectCommonNodesParams
{
    FString GraphPath;
    TArray<FString> NodeTypes;
    bool bConnectFromInput = true;
    bool bConnectToOutput = true;
};

struct FUEAIAgentPcgSetKeyParametersParams
{
    FString GraphPath;
    bool bHasSurfacePointsPerSquaredMeter = false;
    float SurfacePointsPerSquaredMeter = 0.0f;
    bool bHasSurfaceLooseness = false;
    float SurfaceLooseness = 0.0f;
    bool bHasSurfacePointExtents = false;
    FVector SurfacePointExtents = FVector(50.0f, 50.0f, 50.0f);
    bool bHasTransformOffsetMin = false;
    FVector TransformOffsetMin = FVector::ZeroVector;
    bool bHasTransformOffsetMax = false;
    FVector TransformOffsetMax = FVector::ZeroVector;
    bool bHasTransformRotationMin = false;
    FRotator TransformRotationMin = FRotator::ZeroRotator;
    bool bHasTransformRotationMax = false;
    FRotator TransformRotationMax = FRotator::ZeroRotator;
    bool bHasTransformScaleMin = false;
    FVector TransformScaleMin = FVector::OneVector;
    bool bHasTransformScaleMax = false;
    FVector TransformScaleMax = FVector::OneVector;
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
    static bool LandscapeSculpt(const FUEAIAgentLandscapeSculptParams& Params, FString& OutMessage);
    static bool LandscapePaintLayer(const FUEAIAgentLandscapePaintLayerParams& Params, FString& OutMessage);
    static bool LandscapeGenerate(const FUEAIAgentLandscapeGenerateParams& Params, FString& OutMessage);
    static bool PcgCreateGraph(const FUEAIAgentPcgCreateGraphParams& Params, FString& OutMessage);
    static bool PcgPlaceOnLandscape(const FUEAIAgentPcgPlaceOnLandscapeParams& Params, FString& OutMessage);
    static bool PcgAddConnectCommonNodes(const FUEAIAgentPcgAddConnectCommonNodesParams& Params, FString& OutMessage);
    static bool PcgSetKeyParameters(const FUEAIAgentPcgSetKeyParametersParams& Params, FString& OutMessage);
    static bool EditorUndo(FString& OutMessage);
    static bool EditorRedo(FString& OutMessage);
    static bool SessionBeginTransaction(const FString& Description, FString& OutMessage);
    static bool SessionCommitTransaction(FString& OutMessage);
    static bool SessionRollbackTransaction(FString& OutMessage);
    static void SessionCleanupForShutdown();
};
