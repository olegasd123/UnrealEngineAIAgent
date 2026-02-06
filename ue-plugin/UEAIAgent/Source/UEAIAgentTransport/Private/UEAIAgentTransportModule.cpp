#include "UEAIAgentTransportModule.h"

#include "UEAIAgentSettings.h"
#include "Async/Async.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogUEAIAgentTransport, Log, All);

namespace
{
    bool ParseActorNamesField(const TSharedPtr<FJsonObject>& ParamsObj, TArray<FString>& OutActorNames)
    {
        OutActorNames.Empty();
        const TArray<TSharedPtr<FJsonValue>>* NamesArray = nullptr;
        if (!ParamsObj.IsValid() || !ParamsObj->TryGetArrayField(TEXT("actorNames"), NamesArray) || !NamesArray)
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& Value : *NamesArray)
        {
            FString Name;
            if (Value.IsValid() && Value->TryGetString(Name) && !Name.IsEmpty())
            {
                OutActorNames.Add(Name);
            }
        }
        return OutActorNames.Num() > 0;
    }

    EUEAIAgentRiskLevel ParseRiskLevel(const TSharedPtr<FJsonObject>& ActionObj)
    {
        if (!ActionObj.IsValid())
        {
            return EUEAIAgentRiskLevel::Low;
        }

        FString Risk;
        if (!ActionObj->TryGetStringField(TEXT("risk"), Risk))
        {
            return EUEAIAgentRiskLevel::Low;
        }

        if (Risk.Equals(TEXT("high"), ESearchCase::IgnoreCase))
        {
            return EUEAIAgentRiskLevel::High;
        }
        if (Risk.Equals(TEXT("medium"), ESearchCase::IgnoreCase))
        {
            return EUEAIAgentRiskLevel::Medium;
        }
        return EUEAIAgentRiskLevel::Low;
    }

    FString ToRiskText(EUEAIAgentRiskLevel Risk)
    {
        if (Risk == EUEAIAgentRiskLevel::High)
        {
            return TEXT("high");
        }
        if (Risk == EUEAIAgentRiskLevel::Medium)
        {
            return TEXT("medium");
        }
        return TEXT("low");
    }

    FString ToStateText(EUEAIAgentActionState State)
    {
        if (State == EUEAIAgentActionState::Succeeded)
        {
            return TEXT("succeeded");
        }
        if (State == EUEAIAgentActionState::Failed)
        {
            return TEXT("failed");
        }
        return TEXT("pending");
    }

    bool ParsePlannedActionFromJson(
        const TSharedPtr<FJsonObject>& ActionObj,
        const TArray<FString>& SelectedActors,
        FUEAIAgentPlannedSceneAction& OutAction)
    {
        if (!ActionObj.IsValid())
        {
            return false;
        }

        const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
        if (!ActionObj->TryGetObjectField(TEXT("params"), ParamsObj) || !ParamsObj || !ParamsObj->IsValid())
        {
            return false;
        }

        FString Command;
        if (!ActionObj->TryGetStringField(TEXT("command"), Command))
        {
            return false;
        }

        if (Command == TEXT("scene.modifyActor"))
        {
            FString Target;
            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
            {
                return false;
            }

            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::ModifyActor;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);
            if (Target.Equals(TEXT("selection"), ESearchCase::IgnoreCase))
            {
                ParsedAction.ActorNames = SelectedActors;
            }
            else if (Target.Equals(TEXT("byName"), ESearchCase::IgnoreCase))
            {
                if (!ParseActorNamesField(*ParamsObj, ParsedAction.ActorNames))
                {
                    return false;
                }
            }
            else
            {
                return false;
            }

            bool bHasAnyDelta = false;
            const TSharedPtr<FJsonObject>* DeltaLocationObj = nullptr;
            if ((*ParamsObj)->TryGetObjectField(TEXT("deltaLocation"), DeltaLocationObj) &&
                DeltaLocationObj && DeltaLocationObj->IsValid())
            {
                double X = 0.0;
                double Y = 0.0;
                double Z = 0.0;
                if ((*DeltaLocationObj)->TryGetNumberField(TEXT("x"), X) &&
                    (*DeltaLocationObj)->TryGetNumberField(TEXT("y"), Y) &&
                    (*DeltaLocationObj)->TryGetNumberField(TEXT("z"), Z))
                {
                    ParsedAction.DeltaLocation = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
                    bHasAnyDelta = true;
                }
            }

            const TSharedPtr<FJsonObject>* DeltaRotationObj = nullptr;
            if ((*ParamsObj)->TryGetObjectField(TEXT("deltaRotation"), DeltaRotationObj) &&
                DeltaRotationObj && DeltaRotationObj->IsValid())
            {
                double Pitch = 0.0;
                double Yaw = 0.0;
                double Roll = 0.0;
                if ((*DeltaRotationObj)->TryGetNumberField(TEXT("pitch"), Pitch) &&
                    (*DeltaRotationObj)->TryGetNumberField(TEXT("yaw"), Yaw) &&
                    (*DeltaRotationObj)->TryGetNumberField(TEXT("roll"), Roll))
                {
                    ParsedAction.DeltaRotation = FRotator(
                        static_cast<float>(Pitch),
                        static_cast<float>(Yaw),
                        static_cast<float>(Roll));
                    bHasAnyDelta = true;
                }
            }

            const TSharedPtr<FJsonObject>* DeltaScaleObj = nullptr;
            if ((*ParamsObj)->TryGetObjectField(TEXT("deltaScale"), DeltaScaleObj) &&
                DeltaScaleObj && DeltaScaleObj->IsValid())
            {
                double X = 0.0;
                double Y = 0.0;
                double Z = 0.0;
                if ((*DeltaScaleObj)->TryGetNumberField(TEXT("x"), X) &&
                    (*DeltaScaleObj)->TryGetNumberField(TEXT("y"), Y) &&
                    (*DeltaScaleObj)->TryGetNumberField(TEXT("z"), Z))
                {
                    ParsedAction.DeltaScale = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
                    bHasAnyDelta = true;
                }
            }

            const TSharedPtr<FJsonObject>* ScaleObj = nullptr;
            if ((*ParamsObj)->TryGetObjectField(TEXT("scale"), ScaleObj) && ScaleObj && ScaleObj->IsValid())
            {
                double X = 0.0;
                double Y = 0.0;
                double Z = 0.0;
                if ((*ScaleObj)->TryGetNumberField(TEXT("x"), X) &&
                    (*ScaleObj)->TryGetNumberField(TEXT("y"), Y) &&
                    (*ScaleObj)->TryGetNumberField(TEXT("z"), Z))
                {
                    ParsedAction.Scale = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
                    ParsedAction.bHasScale = true;
                    bHasAnyDelta = true;
                }
            }

            if (!bHasAnyDelta)
            {
                return false;
            }

            OutAction = ParsedAction;
            return true;
        }

        if (Command == TEXT("scene.createActor"))
        {
            FString ActorClass;
            if (!(*ParamsObj)->TryGetStringField(TEXT("actorClass"), ActorClass) || ActorClass.IsEmpty())
            {
                return false;
            }

            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::CreateActor;
            ParsedAction.ActorClass = ActorClass;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);

            double Count = 1.0;
            if ((*ParamsObj)->TryGetNumberField(TEXT("count"), Count))
            {
                ParsedAction.SpawnCount = FMath::Clamp(FMath::RoundToInt(static_cast<float>(Count)), 1, 200);
            }

            const TSharedPtr<FJsonObject>* LocationObj = nullptr;
            if ((*ParamsObj)->TryGetObjectField(TEXT("location"), LocationObj) &&
                LocationObj && LocationObj->IsValid())
            {
                double X = 0.0;
                double Y = 0.0;
                double Z = 0.0;
                if ((*LocationObj)->TryGetNumberField(TEXT("x"), X) &&
                    (*LocationObj)->TryGetNumberField(TEXT("y"), Y) &&
                    (*LocationObj)->TryGetNumberField(TEXT("z"), Z))
                {
                    ParsedAction.SpawnLocation = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
                }
            }

            const TSharedPtr<FJsonObject>* RotationObj = nullptr;
            if ((*ParamsObj)->TryGetObjectField(TEXT("rotation"), RotationObj) &&
                RotationObj && RotationObj->IsValid())
            {
                double Pitch = 0.0;
                double Yaw = 0.0;
                double Roll = 0.0;
                if ((*RotationObj)->TryGetNumberField(TEXT("pitch"), Pitch) &&
                    (*RotationObj)->TryGetNumberField(TEXT("yaw"), Yaw) &&
                    (*RotationObj)->TryGetNumberField(TEXT("roll"), Roll))
                {
                    ParsedAction.SpawnRotation = FRotator(
                        static_cast<float>(Pitch),
                        static_cast<float>(Yaw),
                        static_cast<float>(Roll));
                }
            }

            OutAction = ParsedAction;
            return true;
        }

        if (Command == TEXT("scene.deleteActor"))
        {
            FString Target;
            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
            {
                return false;
            }

            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::DeleteActor;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);
            if (Target.Equals(TEXT("selection"), ESearchCase::IgnoreCase))
            {
                ParsedAction.ActorNames = SelectedActors;
            }
            else if (Target.Equals(TEXT("byName"), ESearchCase::IgnoreCase))
            {
                if (!ParseActorNamesField(*ParamsObj, ParsedAction.ActorNames))
                {
                    return false;
                }
            }
            else
            {
                return false;
            }
            OutAction = ParsedAction;
            return true;
        }

        if (Command == TEXT("scene.modifyComponent"))
        {
            FString Target;
            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
            {
                return false;
            }

            FString ComponentName;
            if (!(*ParamsObj)->TryGetStringField(TEXT("componentName"), ComponentName) || ComponentName.IsEmpty())
            {
                return false;
            }

            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::ModifyComponent;
            ParsedAction.ComponentName = ComponentName;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);
            if (Target.Equals(TEXT("selection"), ESearchCase::IgnoreCase))
            {
                ParsedAction.ActorNames = SelectedActors;
            }
            else if (Target.Equals(TEXT("byName"), ESearchCase::IgnoreCase))
            {
                if (!ParseActorNamesField(*ParamsObj, ParsedAction.ActorNames))
                {
                    return false;
                }
            }
            else
            {
                return false;
            }

            bool bHasAnyDelta = false;
            const TSharedPtr<FJsonObject>* DeltaLocationObj = nullptr;
            if ((*ParamsObj)->TryGetObjectField(TEXT("deltaLocation"), DeltaLocationObj) &&
                DeltaLocationObj && DeltaLocationObj->IsValid())
            {
                double X = 0.0;
                double Y = 0.0;
                double Z = 0.0;
                if ((*DeltaLocationObj)->TryGetNumberField(TEXT("x"), X) &&
                    (*DeltaLocationObj)->TryGetNumberField(TEXT("y"), Y) &&
                    (*DeltaLocationObj)->TryGetNumberField(TEXT("z"), Z))
                {
                    ParsedAction.ComponentDeltaLocation = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
                    bHasAnyDelta = true;
                }
            }

            const TSharedPtr<FJsonObject>* DeltaRotationObj = nullptr;
            if ((*ParamsObj)->TryGetObjectField(TEXT("deltaRotation"), DeltaRotationObj) &&
                DeltaRotationObj && DeltaRotationObj->IsValid())
            {
                double Pitch = 0.0;
                double Yaw = 0.0;
                double Roll = 0.0;
                if ((*DeltaRotationObj)->TryGetNumberField(TEXT("pitch"), Pitch) &&
                    (*DeltaRotationObj)->TryGetNumberField(TEXT("yaw"), Yaw) &&
                    (*DeltaRotationObj)->TryGetNumberField(TEXT("roll"), Roll))
                {
                    ParsedAction.ComponentDeltaRotation = FRotator(
                        static_cast<float>(Pitch),
                        static_cast<float>(Yaw),
                        static_cast<float>(Roll));
                    bHasAnyDelta = true;
                }
            }

            const TSharedPtr<FJsonObject>* DeltaScaleObj = nullptr;
            if ((*ParamsObj)->TryGetObjectField(TEXT("deltaScale"), DeltaScaleObj) &&
                DeltaScaleObj && DeltaScaleObj->IsValid())
            {
                double X = 0.0;
                double Y = 0.0;
                double Z = 0.0;
                if ((*DeltaScaleObj)->TryGetNumberField(TEXT("x"), X) &&
                    (*DeltaScaleObj)->TryGetNumberField(TEXT("y"), Y) &&
                    (*DeltaScaleObj)->TryGetNumberField(TEXT("z"), Z))
                {
                    ParsedAction.ComponentDeltaScale = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
                    bHasAnyDelta = true;
                }
            }

            const TSharedPtr<FJsonObject>* ScaleObj = nullptr;
            if ((*ParamsObj)->TryGetObjectField(TEXT("scale"), ScaleObj) && ScaleObj && ScaleObj->IsValid())
            {
                double X = 0.0;
                double Y = 0.0;
                double Z = 0.0;
                if ((*ScaleObj)->TryGetNumberField(TEXT("x"), X) &&
                    (*ScaleObj)->TryGetNumberField(TEXT("y"), Y) &&
                    (*ScaleObj)->TryGetNumberField(TEXT("z"), Z))
                {
                    ParsedAction.ComponentScale = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
                    ParsedAction.bComponentHasScale = true;
                    bHasAnyDelta = true;
                }
            }

            bool bVisibility = false;
            if ((*ParamsObj)->TryGetBoolField(TEXT("visibility"), bVisibility))
            {
                ParsedAction.bComponentVisibilityEdit = true;
                ParsedAction.bComponentVisible = bVisibility;
            }

            if (!bHasAnyDelta && !ParsedAction.bComponentVisibilityEdit)
            {
                return false;
            }

            OutAction = ParsedAction;
            return true;
        }

        if (Command == TEXT("scene.addActorTag"))
        {
            FString Target;
            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
            {
                return false;
            }

            FString Tag;
            if (!(*ParamsObj)->TryGetStringField(TEXT("tag"), Tag) || Tag.IsEmpty())
            {
                return false;
            }

            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::AddActorTag;
            ParsedAction.ActorTag = Tag;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);
            if (Target.Equals(TEXT("selection"), ESearchCase::IgnoreCase))
            {
                ParsedAction.ActorNames = SelectedActors;
            }
            else if (Target.Equals(TEXT("byName"), ESearchCase::IgnoreCase))
            {
                if (!ParseActorNamesField(*ParamsObj, ParsedAction.ActorNames))
                {
                    return false;
                }
            }
            else
            {
                return false;
            }

            OutAction = ParsedAction;
            return true;
        }

        if (Command == TEXT("scene.setComponentMaterial"))
        {
            FString Target;
            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
            {
                return false;
            }

            FString ComponentName;
            if (!(*ParamsObj)->TryGetStringField(TEXT("componentName"), ComponentName) || ComponentName.IsEmpty())
            {
                return false;
            }

            FString MaterialPath;
            if (!(*ParamsObj)->TryGetStringField(TEXT("materialPath"), MaterialPath) || MaterialPath.IsEmpty())
            {
                return false;
            }

            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::SetComponentMaterial;
            ParsedAction.ComponentName = ComponentName;
            ParsedAction.MaterialPath = MaterialPath;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);
            if (Target.Equals(TEXT("selection"), ESearchCase::IgnoreCase))
            {
                ParsedAction.ActorNames = SelectedActors;
            }
            else if (Target.Equals(TEXT("byName"), ESearchCase::IgnoreCase))
            {
                if (!ParseActorNamesField(*ParamsObj, ParsedAction.ActorNames))
                {
                    return false;
                }
            }
            else
            {
                return false;
            }

            double SlotValue = 0.0;
            if ((*ParamsObj)->TryGetNumberField(TEXT("materialSlot"), SlotValue))
            {
                ParsedAction.MaterialSlot = FMath::Max(0, FMath::RoundToInt(static_cast<float>(SlotValue)));
            }

            OutAction = ParsedAction;
            return true;
        }

        if (Command == TEXT("scene.setComponentStaticMesh"))
        {
            FString Target;
            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
            {
                return false;
            }

            FString ComponentName;
            if (!(*ParamsObj)->TryGetStringField(TEXT("componentName"), ComponentName) || ComponentName.IsEmpty())
            {
                return false;
            }

            FString MeshPath;
            if (!(*ParamsObj)->TryGetStringField(TEXT("meshPath"), MeshPath) || MeshPath.IsEmpty())
            {
                return false;
            }

            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::SetComponentStaticMesh;
            ParsedAction.ComponentName = ComponentName;
            ParsedAction.MeshPath = MeshPath;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);
            if (Target.Equals(TEXT("selection"), ESearchCase::IgnoreCase))
            {
                ParsedAction.ActorNames = SelectedActors;
            }
            else if (Target.Equals(TEXT("byName"), ESearchCase::IgnoreCase))
            {
                if (!ParseActorNamesField(*ParamsObj, ParsedAction.ActorNames))
                {
                    return false;
                }
            }
            else
            {
                return false;
            }

            OutAction = ParsedAction;
            return true;
        }

        if (Command == TEXT("scene.setActorFolder"))
        {
            FString Target;
            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
            {
                return false;
            }

            FString FolderPath;
            if (!(*ParamsObj)->TryGetStringField(TEXT("folderPath"), FolderPath))
            {
                FolderPath = TEXT("");
            }

            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::SetActorFolder;
            ParsedAction.FolderPath = FolderPath;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);
            if (Target.Equals(TEXT("selection"), ESearchCase::IgnoreCase))
            {
                ParsedAction.ActorNames = SelectedActors;
            }
            else if (Target.Equals(TEXT("byName"), ESearchCase::IgnoreCase))
            {
                if (!ParseActorNamesField(*ParamsObj, ParsedAction.ActorNames))
                {
                    return false;
                }
            }
            else
            {
                return false;
            }

            OutAction = ParsedAction;
            return true;
        }

        if (Command == TEXT("scene.addActorLabelPrefix"))
        {
            FString Target;
            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
            {
                return false;
            }

            FString Prefix;
            if (!(*ParamsObj)->TryGetStringField(TEXT("prefix"), Prefix) || Prefix.IsEmpty())
            {
                return false;
            }

            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::AddActorLabelPrefix;
            ParsedAction.LabelPrefix = Prefix;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);
            if (Target.Equals(TEXT("selection"), ESearchCase::IgnoreCase))
            {
                ParsedAction.ActorNames = SelectedActors;
            }
            else if (Target.Equals(TEXT("byName"), ESearchCase::IgnoreCase))
            {
                if (!ParseActorNamesField(*ParamsObj, ParsedAction.ActorNames))
                {
                    return false;
                }
            }
            else
            {
                return false;
            }

            OutAction = ParsedAction;
            return true;
        }

        if (Command == TEXT("scene.duplicateActors"))
        {
            FString Target;
            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
            {
                return false;
            }

            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::DuplicateActors;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);
            if (Target.Equals(TEXT("selection"), ESearchCase::IgnoreCase))
            {
                ParsedAction.ActorNames = SelectedActors;
            }
            else if (Target.Equals(TEXT("byName"), ESearchCase::IgnoreCase))
            {
                if (!ParseActorNamesField(*ParamsObj, ParsedAction.ActorNames))
                {
                    return false;
                }
            }
            else
            {
                return false;
            }

            double CountValue = 1.0;
            if ((*ParamsObj)->TryGetNumberField(TEXT("count"), CountValue))
            {
                ParsedAction.DuplicateCount = FMath::Clamp(FMath::RoundToInt(static_cast<float>(CountValue)), 1, 20);
            }

            const TSharedPtr<FJsonObject>* OffsetObj = nullptr;
            if ((*ParamsObj)->TryGetObjectField(TEXT("offset"), OffsetObj) && OffsetObj && OffsetObj->IsValid())
            {
                double X = 0.0;
                double Y = 0.0;
                double Z = 0.0;
                if ((*OffsetObj)->TryGetNumberField(TEXT("x"), X) &&
                    (*OffsetObj)->TryGetNumberField(TEXT("y"), Y) &&
                    (*OffsetObj)->TryGetNumberField(TEXT("z"), Z))
                {
                    ParsedAction.DuplicateOffset = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
                }
            }

            OutAction = ParsedAction;
            return true;
        }

        return false;
    }

    TSharedRef<FJsonObject> BuildContextObject(const TArray<FString>& SelectedActors)
    {
        TSharedRef<FJsonObject> Context = MakeShared<FJsonObject>();

        TArray<TSharedPtr<FJsonValue>> SelectionNames;
        for (const FString& ActorName : SelectedActors)
        {
            SelectionNames.Add(MakeShared<FJsonValueString>(ActorName));
        }
        Context->SetArrayField(TEXT("selectionNames"), SelectionNames);

        TArray<TSharedPtr<FJsonValue>> SelectionDetails;
        if (GEditor)
        {
            UWorld* World = GEditor->GetEditorWorldContext().World();
            if (World)
            {
                for (TActorIterator<AActor> It(World); It; ++It)
                {
                    AActor* Actor = *It;
                    if (!Actor)
                    {
                        continue;
                    }

                    const FString ActorName = Actor->GetName();
                    if (!SelectedActors.Contains(ActorName))
                    {
                        continue;
                    }

                    TSharedRef<FJsonObject> ActorObj = MakeShared<FJsonObject>();
                    ActorObj->SetStringField(TEXT("name"), ActorName);
                    ActorObj->SetStringField(TEXT("label"), Actor->GetActorLabel());
                    ActorObj->SetStringField(TEXT("class"), Actor->GetClass() ? Actor->GetClass()->GetName() : TEXT("Unknown"));

                    const FVector Location = Actor->GetActorLocation();
                    const FRotator Rotation = Actor->GetActorRotation();
                    const FVector Scale = Actor->GetActorScale3D();

                    TSharedRef<FJsonObject> Transform = MakeShared<FJsonObject>();
                    Transform->SetNumberField(TEXT("x"), Location.X);
                    Transform->SetNumberField(TEXT("y"), Location.Y);
                    Transform->SetNumberField(TEXT("z"), Location.Z);
                    ActorObj->SetObjectField(TEXT("location"), Transform);

                    TSharedRef<FJsonObject> RotObj = MakeShared<FJsonObject>();
                    RotObj->SetNumberField(TEXT("pitch"), Rotation.Pitch);
                    RotObj->SetNumberField(TEXT("yaw"), Rotation.Yaw);
                    RotObj->SetNumberField(TEXT("roll"), Rotation.Roll);
                    ActorObj->SetObjectField(TEXT("rotation"), RotObj);

                    TSharedRef<FJsonObject> ScaleObj = MakeShared<FJsonObject>();
                    ScaleObj->SetNumberField(TEXT("x"), Scale.X);
                    ScaleObj->SetNumberField(TEXT("y"), Scale.Y);
                    ScaleObj->SetNumberField(TEXT("z"), Scale.Z);
                    ActorObj->SetObjectField(TEXT("scale"), ScaleObj);

                    TArray<TSharedPtr<FJsonValue>> ComponentsArray;
                    TArray<UActorComponent*> Components;
                    Actor->GetComponents(Components);
                    for (UActorComponent* Component : Components)
                    {
                        if (!Component)
                        {
                            continue;
                        }

                        TSharedRef<FJsonObject> CompObj = MakeShared<FJsonObject>();
                        CompObj->SetStringField(TEXT("name"), Component->GetName());
                        CompObj->SetStringField(TEXT("class"), Component->GetClass() ? Component->GetClass()->GetName() : TEXT("Unknown"));
                        ComponentsArray.Add(MakeShared<FJsonValueObject>(CompObj));
                    }
                    ActorObj->SetArrayField(TEXT("components"), ComponentsArray);

                    SelectionDetails.Add(MakeShared<FJsonValueObject>(ActorObj));
                }
            }
        }

        Context->SetArrayField(TEXT("selection"), SelectionDetails);

        if (GEditor)
        {
            UWorld* World = GEditor->GetEditorWorldContext().World();
            if (World)
            {
                TSharedRef<FJsonObject> LevelObj = MakeShared<FJsonObject>();
                LevelObj->SetStringField(TEXT("mapName"), World->GetMapName());
                if (World->GetCurrentLevel())
                {
                    LevelObj->SetStringField(TEXT("levelName"), World->GetCurrentLevel()->GetOuter()->GetName());
                }
                Context->SetObjectField(TEXT("level"), LevelObj);
            }
        }

        return Context;
    }
}

void FUEAIAgentTransportModule::StartupModule()
{
    UE_LOG(LogUEAIAgentTransport, Log, TEXT("UEAIAgentTransport started."));
}

void FUEAIAgentTransportModule::ShutdownModule()
{
    UE_LOG(LogUEAIAgentTransport, Log, TEXT("UEAIAgentTransport stopped."));
}

FString FUEAIAgentTransportModule::BuildBaseUrl() const
{
    const UUEAIAgentSettings* Settings = GetDefault<UUEAIAgentSettings>();
    const FString Host = Settings ? Settings->AgentHost : TEXT("127.0.0.1");
    const int32 Port = Settings ? Settings->AgentPort : 4317;
    return FString::Printf(TEXT("http://%s:%d"), *Host, Port);
}

FString FUEAIAgentTransportModule::BuildHealthUrl() const
{
    return BuildBaseUrl() + TEXT("/health");
}

FString FUEAIAgentTransportModule::BuildPlanUrl() const
{
    return BuildBaseUrl() + TEXT("/v1/task/plan");
}

FString FUEAIAgentTransportModule::BuildProviderStatusUrl() const
{
    return BuildBaseUrl() + TEXT("/v1/providers/status");
}

FString FUEAIAgentTransportModule::BuildCredentialsSetUrl() const
{
    return BuildBaseUrl() + TEXT("/v1/credentials/set");
}

FString FUEAIAgentTransportModule::BuildCredentialsDeleteUrl() const
{
    return BuildBaseUrl() + TEXT("/v1/credentials/delete");
}

FString FUEAIAgentTransportModule::BuildCredentialsTestUrl() const
{
    return BuildBaseUrl() + TEXT("/v1/credentials/test");
}

FString FUEAIAgentTransportModule::BuildSessionStartUrl() const
{
    return BuildBaseUrl() + TEXT("/v1/session/start");
}

FString FUEAIAgentTransportModule::BuildSessionNextUrl() const
{
    return BuildBaseUrl() + TEXT("/v1/session/next");
}

FString FUEAIAgentTransportModule::BuildSessionApproveUrl() const
{
    return BuildBaseUrl() + TEXT("/v1/session/approve");
}

FString FUEAIAgentTransportModule::BuildSessionResumeUrl() const
{
    return BuildBaseUrl() + TEXT("/v1/session/resume");
}

void FUEAIAgentTransportModule::CheckHealth(const FOnUEAIAgentHealthChecked& Callback) const
{
    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildHealthUrl());
    Request->SetVerb(TEXT("GET"));

    Request->OnProcessRequestComplete().BindLambda(
        [Callback](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [Callback, HttpResponse, bConnectedSuccessfully]()
            {
                if (!bConnectedSuccessfully || !HttpResponse.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Agent Core is not reachable."));
                    return;
                }

                const int32 StatusCode = HttpResponse->GetResponseCode();
                if (StatusCode < 200 || StatusCode >= 300)
                {
                    Callback.ExecuteIfBound(false, FString::Printf(TEXT("Health check failed (%d)."), StatusCode));
                    return;
                }

                TSharedPtr<FJsonObject> ResponseJson;
                const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResponse->GetContentAsString());
                if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Health response is not valid JSON."));
                    return;
                }

                bool bOk = false;
                if (!ResponseJson->TryGetBoolField(TEXT("ok"), bOk))
                {
                    Callback.ExecuteIfBound(false, TEXT("Health response misses 'ok' field."));
                    return;
                }

                FString Provider;
                ResponseJson->TryGetStringField(TEXT("provider"), Provider);
                if (!bOk)
                {
                    Callback.ExecuteIfBound(false, TEXT("Agent Core reports unhealthy state."));
                    return;
                }

                const FString Message = Provider.IsEmpty()
                    ? TEXT("Connected.")
                    : FString::Printf(TEXT("Connected. Provider: %s"), *Provider);
                Callback.ExecuteIfBound(true, Message);
            });
        });

    Request->ProcessRequest();
}

void FUEAIAgentTransportModule::PlanTask(
    const FString& Prompt,
    const FString& Mode,
    const TArray<FString>& SelectedActors,
    const FOnUEAIAgentTaskPlanned& Callback) const
{
    PlannedActions.Empty();
    ActiveSessionId.Empty();
    ActiveSessionActionIndex = INDEX_NONE;
    ActiveSessionSelectedActors.Empty();

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("prompt"), Prompt);
    Root->SetStringField(TEXT("mode"), Mode.IsEmpty() ? TEXT("chat") : Mode);
    Root->SetObjectField(TEXT("context"), BuildContextObject(SelectedActors));

    FString RequestBody;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
    FJsonSerializer::Serialize(Root, Writer);

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildPlanUrl());
    Request->SetVerb(TEXT("POST"));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Request->SetContentAsString(RequestBody);

    Request->OnProcessRequestComplete().BindLambda(
        [this, Callback, SelectedActors](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [this, Callback, SelectedActors, HttpResponse, bConnectedSuccessfully]()
            {
                if (!bConnectedSuccessfully || !HttpResponse.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Could not connect to Agent Core."));
                    return;
                }

                if (HttpResponse->GetResponseCode() < 200 || HttpResponse->GetResponseCode() >= 300)
                {
                    Callback.ExecuteIfBound(
                        false,
                        FString::Printf(TEXT("Plan request failed (%d)."), HttpResponse->GetResponseCode()));
                    return;
                }

                TSharedPtr<FJsonObject> ResponseJson;
                const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResponse->GetContentAsString());
                if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Plan response is not valid JSON."));
                    return;
                }

                bool bOk = false;
                if (!ResponseJson->TryGetBoolField(TEXT("ok"), bOk) || !bOk)
                {
                    FString ErrorMessage = TEXT("Agent Core returned an error.");
                    ResponseJson->TryGetStringField(TEXT("error"), ErrorMessage);
                    Callback.ExecuteIfBound(false, ErrorMessage);
                    return;
                }

                const TSharedPtr<FJsonObject>* PlanObj = nullptr;
                if (!ResponseJson->TryGetObjectField(TEXT("plan"), PlanObj) || !PlanObj || !PlanObj->IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Plan response misses 'plan' object."));
                    return;
                }

                FString Summary;
                (*PlanObj)->TryGetStringField(TEXT("summary"), Summary);

                FString StepsText;
                const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
                if ((*PlanObj)->TryGetArrayField(TEXT("steps"), Steps) && Steps)
                {
                    for (int32 StepIndex = 0; StepIndex < Steps->Num(); ++StepIndex)
                    {
                        FString StepValue;
                        if ((*Steps)[StepIndex].IsValid() && (*Steps)[StepIndex]->TryGetString(StepValue))
                        {
                            StepsText += FString::Printf(TEXT("%d. %s\n"), StepIndex + 1, *StepValue);
                        }
                    }
                }

                const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
                if ((*PlanObj)->TryGetArrayField(TEXT("actions"), Actions) && Actions)
                {
                    for (const TSharedPtr<FJsonValue>& ActionValue : *Actions)
                    {
                        if (!ActionValue.IsValid())
                        {
                            continue;
                        }

                        const TSharedPtr<FJsonObject> ActionObj = ActionValue->AsObject();
                        if (!ActionObj.IsValid())
                        {
                            continue;
                        }

                        const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
                        if (!ActionObj->TryGetObjectField(TEXT("params"), ParamsObj) || !ParamsObj || !ParamsObj->IsValid())
                        {
                            continue;
                        }

                        FString Command;
                        if (!ActionObj->TryGetStringField(TEXT("command"), Command))
                        {
                            continue;
                        }

                        if (Command == TEXT("scene.modifyActor"))
                        {
                            FString Target;
                            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
                            {
                                continue;
                            }

                            FUEAIAgentPlannedSceneAction ParsedAction;
                            ParsedAction.Type = EUEAIAgentPlannedActionType::ModifyActor;
                            ParsedAction.Risk = ParseRiskLevel(ActionObj);
                            ParsedAction.bApproved = ParsedAction.Risk == EUEAIAgentRiskLevel::Low;
                            if (Target.Equals(TEXT("selection"), ESearchCase::IgnoreCase))
                            {
                                ParsedAction.ActorNames = SelectedActors;
                            }
                            else if (Target.Equals(TEXT("byName"), ESearchCase::IgnoreCase))
                            {
                                if (!ParseActorNamesField(*ParamsObj, ParsedAction.ActorNames))
                                {
                                    continue;
                                }
                            }
                            else
                            {
                                continue;
                            }

                            bool bHasAnyDelta = false;
                            const TSharedPtr<FJsonObject>* DeltaLocationObj = nullptr;
                            if ((*ParamsObj)->TryGetObjectField(TEXT("deltaLocation"), DeltaLocationObj) &&
                                DeltaLocationObj && DeltaLocationObj->IsValid())
                            {
                                double X = 0.0;
                                double Y = 0.0;
                                double Z = 0.0;
                                if ((*DeltaLocationObj)->TryGetNumberField(TEXT("x"), X) &&
                                    (*DeltaLocationObj)->TryGetNumberField(TEXT("y"), Y) &&
                                    (*DeltaLocationObj)->TryGetNumberField(TEXT("z"), Z))
                                {
                                    ParsedAction.DeltaLocation = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
                                    bHasAnyDelta = true;
                                }
                            }

                            const TSharedPtr<FJsonObject>* DeltaRotationObj = nullptr;
                            if ((*ParamsObj)->TryGetObjectField(TEXT("deltaRotation"), DeltaRotationObj) &&
                                DeltaRotationObj && DeltaRotationObj->IsValid())
                            {
                                double Pitch = 0.0;
                                double Yaw = 0.0;
                                double Roll = 0.0;
                                if ((*DeltaRotationObj)->TryGetNumberField(TEXT("pitch"), Pitch) &&
                                    (*DeltaRotationObj)->TryGetNumberField(TEXT("yaw"), Yaw) &&
                                    (*DeltaRotationObj)->TryGetNumberField(TEXT("roll"), Roll))
                                {
                                    ParsedAction.DeltaRotation = FRotator(
                                        static_cast<float>(Pitch),
                                        static_cast<float>(Yaw),
                                        static_cast<float>(Roll));
                                    bHasAnyDelta = true;
                                }
                            }

                            const TSharedPtr<FJsonObject>* DeltaScaleObj = nullptr;
                            if ((*ParamsObj)->TryGetObjectField(TEXT("deltaScale"), DeltaScaleObj) &&
                                DeltaScaleObj && DeltaScaleObj->IsValid())
                            {
                                double X = 0.0;
                                double Y = 0.0;
                                double Z = 0.0;
                                if ((*DeltaScaleObj)->TryGetNumberField(TEXT("x"), X) &&
                                    (*DeltaScaleObj)->TryGetNumberField(TEXT("y"), Y) &&
                                    (*DeltaScaleObj)->TryGetNumberField(TEXT("z"), Z))
                                {
                                    ParsedAction.DeltaScale = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
                                    bHasAnyDelta = true;
                                }
                            }

                            const TSharedPtr<FJsonObject>* ScaleObj = nullptr;
                            if ((*ParamsObj)->TryGetObjectField(TEXT("scale"), ScaleObj) &&
                                ScaleObj && ScaleObj->IsValid())
                            {
                                double X = 0.0;
                                double Y = 0.0;
                                double Z = 0.0;
                                if ((*ScaleObj)->TryGetNumberField(TEXT("x"), X) &&
                                    (*ScaleObj)->TryGetNumberField(TEXT("y"), Y) &&
                                    (*ScaleObj)->TryGetNumberField(TEXT("z"), Z))
                                {
                                    ParsedAction.Scale = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
                                    ParsedAction.bHasScale = true;
                                    bHasAnyDelta = true;
                                }
                            }

                            if (bHasAnyDelta)
                            {
                                PlannedActions.Add(ParsedAction);
                            }
                            continue;
                        }

                        if (Command == TEXT("scene.createActor"))
                        {
                            FString ActorClass;
                            if (!(*ParamsObj)->TryGetStringField(TEXT("actorClass"), ActorClass) || ActorClass.IsEmpty())
                            {
                                continue;
                            }

                            FUEAIAgentPlannedSceneAction ParsedAction;
                            ParsedAction.Type = EUEAIAgentPlannedActionType::CreateActor;
                            ParsedAction.ActorClass = ActorClass;
                            ParsedAction.Risk = ParseRiskLevel(ActionObj);
                            ParsedAction.bApproved = ParsedAction.Risk == EUEAIAgentRiskLevel::Low;

                            double Count = 1.0;
                            if ((*ParamsObj)->TryGetNumberField(TEXT("count"), Count))
                            {
                                ParsedAction.SpawnCount = FMath::Clamp(FMath::RoundToInt(static_cast<float>(Count)), 1, 200);
                            }

                            const TSharedPtr<FJsonObject>* LocationObj = nullptr;
                            if ((*ParamsObj)->TryGetObjectField(TEXT("location"), LocationObj) &&
                                LocationObj && LocationObj->IsValid())
                            {
                                double X = 0.0;
                                double Y = 0.0;
                                double Z = 0.0;
                                if ((*LocationObj)->TryGetNumberField(TEXT("x"), X) &&
                                    (*LocationObj)->TryGetNumberField(TEXT("y"), Y) &&
                                    (*LocationObj)->TryGetNumberField(TEXT("z"), Z))
                                {
                                    ParsedAction.SpawnLocation = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
                                }
                            }

                            const TSharedPtr<FJsonObject>* RotationObj = nullptr;
                            if ((*ParamsObj)->TryGetObjectField(TEXT("rotation"), RotationObj) &&
                                RotationObj && RotationObj->IsValid())
                            {
                                double Pitch = 0.0;
                                double Yaw = 0.0;
                                double Roll = 0.0;
                                if ((*RotationObj)->TryGetNumberField(TEXT("pitch"), Pitch) &&
                                    (*RotationObj)->TryGetNumberField(TEXT("yaw"), Yaw) &&
                                    (*RotationObj)->TryGetNumberField(TEXT("roll"), Roll))
                                {
                                    ParsedAction.SpawnRotation = FRotator(
                                        static_cast<float>(Pitch),
                                        static_cast<float>(Yaw),
                                        static_cast<float>(Roll));
                                }
                            }

                            PlannedActions.Add(ParsedAction);
                            continue;
                        }

                        if (Command == TEXT("scene.deleteActor"))
                        {
                            FString Target;
                            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
                            {
                                continue;
                            }

                            FUEAIAgentPlannedSceneAction ParsedAction;
                            ParsedAction.Type = EUEAIAgentPlannedActionType::DeleteActor;
                            ParsedAction.Risk = ParseRiskLevel(ActionObj);
                            ParsedAction.bApproved = false;
                            if (Target.Equals(TEXT("selection"), ESearchCase::IgnoreCase))
                            {
                                ParsedAction.ActorNames = SelectedActors;
                            }
                            else if (Target.Equals(TEXT("byName"), ESearchCase::IgnoreCase))
                            {
                                if (!ParseActorNamesField(*ParamsObj, ParsedAction.ActorNames))
                                {
                                    continue;
                                }
                            }
                            else
                            {
                                continue;
                            }
                            PlannedActions.Add(ParsedAction);
                            continue;
                        }

                        if (Command == TEXT("scene.modifyComponent"))
                        {
                            FString Target;
                            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
                            {
                                continue;
                            }

                            FString ComponentName;
                            if (!(*ParamsObj)->TryGetStringField(TEXT("componentName"), ComponentName) || ComponentName.IsEmpty())
                            {
                                continue;
                            }

                            FUEAIAgentPlannedSceneAction ParsedAction;
                            ParsedAction.Type = EUEAIAgentPlannedActionType::ModifyComponent;
                            ParsedAction.ComponentName = ComponentName;
                            ParsedAction.Risk = ParseRiskLevel(ActionObj);
                            ParsedAction.bApproved = ParsedAction.Risk == EUEAIAgentRiskLevel::Low;
                            if (Target.Equals(TEXT("selection"), ESearchCase::IgnoreCase))
                            {
                                ParsedAction.ActorNames = SelectedActors;
                            }
                            else if (Target.Equals(TEXT("byName"), ESearchCase::IgnoreCase))
                            {
                                if (!ParseActorNamesField(*ParamsObj, ParsedAction.ActorNames))
                                {
                                    continue;
                                }
                            }
                            else
                            {
                                continue;
                            }

                            bool bHasAnyDelta = false;
                            const TSharedPtr<FJsonObject>* DeltaLocationObj = nullptr;
                            if ((*ParamsObj)->TryGetObjectField(TEXT("deltaLocation"), DeltaLocationObj) &&
                                DeltaLocationObj && DeltaLocationObj->IsValid())
                            {
                                double X = 0.0;
                                double Y = 0.0;
                                double Z = 0.0;
                                if ((*DeltaLocationObj)->TryGetNumberField(TEXT("x"), X) &&
                                    (*DeltaLocationObj)->TryGetNumberField(TEXT("y"), Y) &&
                                    (*DeltaLocationObj)->TryGetNumberField(TEXT("z"), Z))
                                {
                                    ParsedAction.ComponentDeltaLocation = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
                                    bHasAnyDelta = true;
                                }
                            }

                            const TSharedPtr<FJsonObject>* DeltaRotationObj = nullptr;
                            if ((*ParamsObj)->TryGetObjectField(TEXT("deltaRotation"), DeltaRotationObj) &&
                                DeltaRotationObj && DeltaRotationObj->IsValid())
                            {
                                double Pitch = 0.0;
                                double Yaw = 0.0;
                                double Roll = 0.0;
                                if ((*DeltaRotationObj)->TryGetNumberField(TEXT("pitch"), Pitch) &&
                                    (*DeltaRotationObj)->TryGetNumberField(TEXT("yaw"), Yaw) &&
                                    (*DeltaRotationObj)->TryGetNumberField(TEXT("roll"), Roll))
                                {
                                    ParsedAction.ComponentDeltaRotation = FRotator(
                                        static_cast<float>(Pitch),
                                        static_cast<float>(Yaw),
                                        static_cast<float>(Roll));
                                    bHasAnyDelta = true;
                                }
                            }

                            const TSharedPtr<FJsonObject>* DeltaScaleObj = nullptr;
                            if ((*ParamsObj)->TryGetObjectField(TEXT("deltaScale"), DeltaScaleObj) &&
                                DeltaScaleObj && DeltaScaleObj->IsValid())
                            {
                                double X = 0.0;
                                double Y = 0.0;
                                double Z = 0.0;
                                if ((*DeltaScaleObj)->TryGetNumberField(TEXT("x"), X) &&
                                    (*DeltaScaleObj)->TryGetNumberField(TEXT("y"), Y) &&
                                    (*DeltaScaleObj)->TryGetNumberField(TEXT("z"), Z))
                                {
                                    ParsedAction.ComponentDeltaScale = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
                                    bHasAnyDelta = true;
                                }
                            }

                            const TSharedPtr<FJsonObject>* ScaleObj = nullptr;
                            if ((*ParamsObj)->TryGetObjectField(TEXT("scale"), ScaleObj) &&
                                ScaleObj && ScaleObj->IsValid())
                            {
                                double X = 0.0;
                                double Y = 0.0;
                                double Z = 0.0;
                                if ((*ScaleObj)->TryGetNumberField(TEXT("x"), X) &&
                                    (*ScaleObj)->TryGetNumberField(TEXT("y"), Y) &&
                                    (*ScaleObj)->TryGetNumberField(TEXT("z"), Z))
                                {
                                    ParsedAction.ComponentScale = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
                                    ParsedAction.bComponentHasScale = true;
                                    bHasAnyDelta = true;
                                }
                            }

                            bool bVisibility = false;
                            if ((*ParamsObj)->TryGetBoolField(TEXT("visibility"), bVisibility))
                            {
                                ParsedAction.bComponentVisibilityEdit = true;
                                ParsedAction.bComponentVisible = bVisibility;
                            }

                            if (!bHasAnyDelta && !ParsedAction.bComponentVisibilityEdit)
                            {
                                continue;
                            }

                            PlannedActions.Add(ParsedAction);
                            continue;
                        }

                        if (Command == TEXT("scene.addActorTag"))
                        {
                            FString Target;
                            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
                            {
                                continue;
                            }

                            FString Tag;
                            if (!(*ParamsObj)->TryGetStringField(TEXT("tag"), Tag) || Tag.IsEmpty())
                            {
                                continue;
                            }

                            FUEAIAgentPlannedSceneAction ParsedAction;
                            ParsedAction.Type = EUEAIAgentPlannedActionType::AddActorTag;
                            ParsedAction.ActorTag = Tag;
                            ParsedAction.Risk = ParseRiskLevel(ActionObj);
                            ParsedAction.bApproved = ParsedAction.Risk == EUEAIAgentRiskLevel::Low;
                            if (Target.Equals(TEXT("selection"), ESearchCase::IgnoreCase))
                            {
                                ParsedAction.ActorNames = SelectedActors;
                            }
                            else if (Target.Equals(TEXT("byName"), ESearchCase::IgnoreCase))
                            {
                                if (!ParseActorNamesField(*ParamsObj, ParsedAction.ActorNames))
                                {
                                    continue;
                                }
                            }
                            else
                            {
                                continue;
                            }

                            PlannedActions.Add(ParsedAction);
                            continue;
                        }

                        if (Command == TEXT("scene.setComponentMaterial"))
                        {
                            FString Target;
                            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
                            {
                                continue;
                            }

                            FString ComponentName;
                            if (!(*ParamsObj)->TryGetStringField(TEXT("componentName"), ComponentName) || ComponentName.IsEmpty())
                            {
                                continue;
                            }

                            FString MaterialPath;
                            if (!(*ParamsObj)->TryGetStringField(TEXT("materialPath"), MaterialPath) || MaterialPath.IsEmpty())
                            {
                                continue;
                            }

                            FUEAIAgentPlannedSceneAction ParsedAction;
                            ParsedAction.Type = EUEAIAgentPlannedActionType::SetComponentMaterial;
                            ParsedAction.ComponentName = ComponentName;
                            ParsedAction.MaterialPath = MaterialPath;
                            ParsedAction.Risk = ParseRiskLevel(ActionObj);
                            ParsedAction.bApproved = ParsedAction.Risk == EUEAIAgentRiskLevel::Low;
                            if (Target.Equals(TEXT("selection"), ESearchCase::IgnoreCase))
                            {
                                ParsedAction.ActorNames = SelectedActors;
                            }
                            else if (Target.Equals(TEXT("byName"), ESearchCase::IgnoreCase))
                            {
                                if (!ParseActorNamesField(*ParamsObj, ParsedAction.ActorNames))
                                {
                                    continue;
                                }
                            }
                            else
                            {
                                continue;
                            }

                            double SlotValue = 0.0;
                            if ((*ParamsObj)->TryGetNumberField(TEXT("materialSlot"), SlotValue))
                            {
                                ParsedAction.MaterialSlot = FMath::Max(0, FMath::RoundToInt(static_cast<float>(SlotValue)));
                            }

                            PlannedActions.Add(ParsedAction);
                            continue;
                        }

                        if (Command == TEXT("scene.setComponentStaticMesh"))
                        {
                            FString Target;
                            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
                            {
                                continue;
                            }

                            FString ComponentName;
                            if (!(*ParamsObj)->TryGetStringField(TEXT("componentName"), ComponentName) || ComponentName.IsEmpty())
                            {
                                continue;
                            }

                            FString MeshPath;
                            if (!(*ParamsObj)->TryGetStringField(TEXT("meshPath"), MeshPath) || MeshPath.IsEmpty())
                            {
                                continue;
                            }

                            FUEAIAgentPlannedSceneAction ParsedAction;
                            ParsedAction.Type = EUEAIAgentPlannedActionType::SetComponentStaticMesh;
                            ParsedAction.ComponentName = ComponentName;
                            ParsedAction.MeshPath = MeshPath;
                            ParsedAction.Risk = ParseRiskLevel(ActionObj);
                            ParsedAction.bApproved = ParsedAction.Risk == EUEAIAgentRiskLevel::Low;
                            if (Target.Equals(TEXT("selection"), ESearchCase::IgnoreCase))
                            {
                                ParsedAction.ActorNames = SelectedActors;
                            }
                            else if (Target.Equals(TEXT("byName"), ESearchCase::IgnoreCase))
                            {
                                if (!ParseActorNamesField(*ParamsObj, ParsedAction.ActorNames))
                                {
                                    continue;
                                }
                            }
                            else
                            {
                                continue;
                            }

                            PlannedActions.Add(ParsedAction);
                            continue;
                        }

                        if (Command == TEXT("scene.setActorFolder"))
                        {
                            FString Target;
                            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
                            {
                                continue;
                            }

                            FString FolderPath;
                            if (!(*ParamsObj)->TryGetStringField(TEXT("folderPath"), FolderPath))
                            {
                                FolderPath = TEXT("");
                            }

                            FUEAIAgentPlannedSceneAction ParsedAction;
                            ParsedAction.Type = EUEAIAgentPlannedActionType::SetActorFolder;
                            ParsedAction.FolderPath = FolderPath;
                            ParsedAction.Risk = ParseRiskLevel(ActionObj);
                            ParsedAction.bApproved = ParsedAction.Risk == EUEAIAgentRiskLevel::Low;
                            if (Target.Equals(TEXT("selection"), ESearchCase::IgnoreCase))
                            {
                                ParsedAction.ActorNames = SelectedActors;
                            }
                            else if (Target.Equals(TEXT("byName"), ESearchCase::IgnoreCase))
                            {
                                if (!ParseActorNamesField(*ParamsObj, ParsedAction.ActorNames))
                                {
                                    continue;
                                }
                            }
                            else
                            {
                                continue;
                            }

                            PlannedActions.Add(ParsedAction);
                            continue;
                        }

                        if (Command == TEXT("scene.addActorLabelPrefix"))
                        {
                            FString Target;
                            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
                            {
                                continue;
                            }

                            FString Prefix;
                            if (!(*ParamsObj)->TryGetStringField(TEXT("prefix"), Prefix) || Prefix.IsEmpty())
                            {
                                continue;
                            }

                            FUEAIAgentPlannedSceneAction ParsedAction;
                            ParsedAction.Type = EUEAIAgentPlannedActionType::AddActorLabelPrefix;
                            ParsedAction.LabelPrefix = Prefix;
                            ParsedAction.Risk = ParseRiskLevel(ActionObj);
                            ParsedAction.bApproved = ParsedAction.Risk == EUEAIAgentRiskLevel::Low;
                            if (Target.Equals(TEXT("selection"), ESearchCase::IgnoreCase))
                            {
                                ParsedAction.ActorNames = SelectedActors;
                            }
                            else if (Target.Equals(TEXT("byName"), ESearchCase::IgnoreCase))
                            {
                                if (!ParseActorNamesField(*ParamsObj, ParsedAction.ActorNames))
                                {
                                    continue;
                                }
                            }
                            else
                            {
                                continue;
                            }

                            PlannedActions.Add(ParsedAction);
                            continue;
                        }

                        if (Command == TEXT("scene.duplicateActors"))
                        {
                            FString Target;
                            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
                            {
                                continue;
                            }

                            FUEAIAgentPlannedSceneAction ParsedAction;
                            ParsedAction.Type = EUEAIAgentPlannedActionType::DuplicateActors;
                            ParsedAction.Risk = ParseRiskLevel(ActionObj);
                            ParsedAction.bApproved = ParsedAction.Risk == EUEAIAgentRiskLevel::Low;
                            if (Target.Equals(TEXT("selection"), ESearchCase::IgnoreCase))
                            {
                                ParsedAction.ActorNames = SelectedActors;
                            }
                            else if (Target.Equals(TEXT("byName"), ESearchCase::IgnoreCase))
                            {
                                if (!ParseActorNamesField(*ParamsObj, ParsedAction.ActorNames))
                                {
                                    continue;
                                }
                            }
                            else
                            {
                                continue;
                            }

                            double CountValue = 1.0;
                            if ((*ParamsObj)->TryGetNumberField(TEXT("count"), CountValue))
                            {
                                ParsedAction.DuplicateCount = FMath::Clamp(FMath::RoundToInt(static_cast<float>(CountValue)), 1, 20);
                            }

                            const TSharedPtr<FJsonObject>* OffsetObj = nullptr;
                            if ((*ParamsObj)->TryGetObjectField(TEXT("offset"), OffsetObj) && OffsetObj && OffsetObj->IsValid())
                            {
                                double X = 0.0;
                                double Y = 0.0;
                                double Z = 0.0;
                                if ((*OffsetObj)->TryGetNumberField(TEXT("x"), X) &&
                                    (*OffsetObj)->TryGetNumberField(TEXT("y"), Y) &&
                                    (*OffsetObj)->TryGetNumberField(TEXT("z"), Z))
                                {
                                    ParsedAction.DuplicateOffset = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
                                }
                            }

                            PlannedActions.Add(ParsedAction);
                            continue;
                        }
                    }
                }

                const FString FinalMessage = StepsText.IsEmpty()
                    ? Summary
                    : FString::Printf(TEXT("%s\n%s"), *Summary, *StepsText);
                Callback.ExecuteIfBound(true, FinalMessage);
            });
        });

    Request->ProcessRequest();
}

bool FUEAIAgentTransportModule::ParseSessionDecision(
    const TSharedPtr<FJsonObject>& ResponseJson,
    const TArray<FString>& SelectedActors,
    FString& OutMessage) const
{
    if (!ResponseJson.IsValid())
    {
        OutMessage = TEXT("Session response is not valid JSON.");
        return false;
    }

    bool bOk = false;
    if (!ResponseJson->TryGetBoolField(TEXT("ok"), bOk) || !bOk)
    {
        FString ErrorMessage = TEXT("Agent Core returned a session error.");
        ResponseJson->TryGetStringField(TEXT("error"), ErrorMessage);
        OutMessage = ErrorMessage;
        return false;
    }

    const TSharedPtr<FJsonObject>* DecisionObj = nullptr;
    if (!ResponseJson->TryGetObjectField(TEXT("decision"), DecisionObj) || !DecisionObj || !DecisionObj->IsValid())
    {
        OutMessage = TEXT("Session response misses decision object.");
        return false;
    }

    FString SessionId;
    if (!(*DecisionObj)->TryGetStringField(TEXT("sessionId"), SessionId) || SessionId.IsEmpty())
    {
        OutMessage = TEXT("Session decision misses sessionId.");
        return false;
    }

    ActiveSessionId = SessionId;
    ActiveSessionActionIndex = INDEX_NONE;
    PlannedActions.Empty();

    FString Status;
    (*DecisionObj)->TryGetStringField(TEXT("status"), Status);
    FString Summary;
    (*DecisionObj)->TryGetStringField(TEXT("summary"), Summary);
    FString Message;
    (*DecisionObj)->TryGetStringField(TEXT("message"), Message);

    double ActionIndex = -1.0;
    (*DecisionObj)->TryGetNumberField(TEXT("nextActionIndex"), ActionIndex);
    ActiveSessionActionIndex = ActionIndex >= 0.0 ? FMath::TruncToInt(ActionIndex) : INDEX_NONE;

    const bool bCanExecute = Status.Equals(TEXT("ready_to_execute"), ESearchCase::IgnoreCase) ||
        Status.Equals(TEXT("awaiting_approval"), ESearchCase::IgnoreCase);
    if (!bCanExecute)
    {
        ActiveSessionActionIndex = INDEX_NONE;
    }

    if (bCanExecute)
    {
        const TSharedPtr<FJsonObject>* NextActionObj = nullptr;
        if ((*DecisionObj)->TryGetObjectField(TEXT("nextAction"), NextActionObj) && NextActionObj && NextActionObj->IsValid())
        {
            FUEAIAgentPlannedSceneAction ParsedAction;
            if (ParsePlannedActionFromJson(*NextActionObj, SelectedActors, ParsedAction))
            {
                bool bApproved = !Status.Equals(TEXT("awaiting_approval"), ESearchCase::IgnoreCase);
                (*DecisionObj)->TryGetBoolField(TEXT("nextActionApproved"), bApproved);
                ParsedAction.bApproved = bApproved;

                FString ActionStateText;
                if ((*DecisionObj)->TryGetStringField(TEXT("nextActionState"), ActionStateText))
                {
                    if (ActionStateText.Equals(TEXT("succeeded"), ESearchCase::IgnoreCase))
                    {
                        ParsedAction.State = EUEAIAgentActionState::Succeeded;
                    }
                    else if (ActionStateText.Equals(TEXT("failed"), ESearchCase::IgnoreCase))
                    {
                        ParsedAction.State = EUEAIAgentActionState::Failed;
                    }
                    else
                    {
                        ParsedAction.State = EUEAIAgentActionState::Pending;
                    }
                }

                double Attempts = 0.0;
                if ((*DecisionObj)->TryGetNumberField(TEXT("nextActionAttempts"), Attempts))
                {
                    ParsedAction.AttemptCount = FMath::Max(0, FMath::RoundToInt(static_cast<float>(Attempts)));
                }
                PlannedActions.Add(ParsedAction);
            }
        }
    }

    OutMessage = FString::Printf(
        TEXT("Session: %s\n%s\n%s"),
        *Status,
        Summary.IsEmpty() ? TEXT("No summary.") : *Summary,
        Message.IsEmpty() ? TEXT("No message.") : *Message);
    return true;
}

void FUEAIAgentTransportModule::StartSession(
    const FString& Prompt,
    const FString& Mode,
    const TArray<FString>& SelectedActors,
    const FOnUEAIAgentSessionUpdated& Callback) const
{
    PlannedActions.Empty();
    ActiveSessionId.Empty();
    ActiveSessionActionIndex = INDEX_NONE;
    ActiveSessionSelectedActors = SelectedActors;

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("prompt"), Prompt);
    Root->SetStringField(TEXT("mode"), Mode.IsEmpty() ? TEXT("agent") : Mode);
    Root->SetNumberField(TEXT("maxRetries"), 2);
    Root->SetObjectField(TEXT("context"), BuildContextObject(SelectedActors));

    FString RequestBody;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
    FJsonSerializer::Serialize(Root, Writer);

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildSessionStartUrl());
    Request->SetVerb(TEXT("POST"));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Request->SetContentAsString(RequestBody);
    Request->OnProcessRequestComplete().BindLambda(
        [this, Callback, SelectedActors](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [this, Callback, SelectedActors, HttpResponse, bConnectedSuccessfully]()
            {
                if (!bConnectedSuccessfully || !HttpResponse.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Could not connect to Agent Core."));
                    return;
                }

                TSharedPtr<FJsonObject> ResponseJson;
                const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResponse->GetContentAsString());
                if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Session start response is not valid JSON."));
                    return;
                }

                FString ParsedMessage;
                const bool bParsed = ParseSessionDecision(ResponseJson, SelectedActors, ParsedMessage);
                Callback.ExecuteIfBound(bParsed, ParsedMessage);
            });
        });

    Request->ProcessRequest();
}

void FUEAIAgentTransportModule::NextSession(
    bool bHasResult,
    bool bResultOk,
    const FString& ResultMessage,
    const FOnUEAIAgentSessionUpdated& Callback) const
{
    if (ActiveSessionId.IsEmpty())
    {
        Callback.ExecuteIfBound(false, TEXT("No active session."));
        return;
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("sessionId"), ActiveSessionId);

    if (bHasResult)
    {
        if (ActiveSessionActionIndex == INDEX_NONE)
        {
            Callback.ExecuteIfBound(false, TEXT("No active session action index."));
            return;
        }

        const int32 CurrentAttempts = GetPlannedActionAttemptCount(ActiveSessionActionIndex);
        UpdateActionResult(ActiveSessionActionIndex, bResultOk, CurrentAttempts + 1);

        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetNumberField(TEXT("actionIndex"), ActiveSessionActionIndex);
        Result->SetBoolField(TEXT("ok"), bResultOk);
        Result->SetStringField(TEXT("message"), ResultMessage);
        Root->SetObjectField(TEXT("result"), Result);
    }

    FString RequestBody;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
    FJsonSerializer::Serialize(Root, Writer);

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildSessionNextUrl());
    Request->SetVerb(TEXT("POST"));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Request->SetContentAsString(RequestBody);
    Request->OnProcessRequestComplete().BindLambda(
        [this, Callback](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [this, Callback, HttpResponse, bConnectedSuccessfully]()
            {
                if (!bConnectedSuccessfully || !HttpResponse.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Could not connect to Agent Core."));
                    return;
                }

                TSharedPtr<FJsonObject> ResponseJson;
                const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResponse->GetContentAsString());
                if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Session next response is not valid JSON."));
                    return;
                }

                FString ParsedMessage;
                const bool bParsed = ParseSessionDecision(ResponseJson, ActiveSessionSelectedActors, ParsedMessage);
                Callback.ExecuteIfBound(bParsed, ParsedMessage);
            });
        });

    Request->ProcessRequest();
}

void FUEAIAgentTransportModule::ApproveCurrentSessionAction(
    bool bApproved,
    const FOnUEAIAgentSessionUpdated& Callback) const
{
    if (ActiveSessionId.IsEmpty() || ActiveSessionActionIndex == INDEX_NONE)
    {
        Callback.ExecuteIfBound(false, TEXT("No active session action to approve."));
        return;
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("sessionId"), ActiveSessionId);
    Root->SetNumberField(TEXT("actionIndex"), ActiveSessionActionIndex);
    Root->SetBoolField(TEXT("approved"), bApproved);

    FString RequestBody;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
    FJsonSerializer::Serialize(Root, Writer);

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildSessionApproveUrl());
    Request->SetVerb(TEXT("POST"));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Request->SetContentAsString(RequestBody);
    Request->OnProcessRequestComplete().BindLambda(
        [this, Callback](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [this, Callback, HttpResponse, bConnectedSuccessfully]()
            {
                if (!bConnectedSuccessfully || !HttpResponse.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Could not connect to Agent Core."));
                    return;
                }

                TSharedPtr<FJsonObject> ResponseJson;
                const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResponse->GetContentAsString());
                if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Session approve response is not valid JSON."));
                    return;
                }

                FString ParsedMessage;
                const bool bParsed = ParseSessionDecision(ResponseJson, ActiveSessionSelectedActors, ParsedMessage);
                Callback.ExecuteIfBound(bParsed, ParsedMessage);
            });
        });

    Request->ProcessRequest();
}

void FUEAIAgentTransportModule::ResumeSession(const FOnUEAIAgentSessionUpdated& Callback) const
{
    if (ActiveSessionId.IsEmpty())
    {
        Callback.ExecuteIfBound(false, TEXT("No active session."));
        return;
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("sessionId"), ActiveSessionId);

    FString RequestBody;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
    FJsonSerializer::Serialize(Root, Writer);

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildSessionResumeUrl());
    Request->SetVerb(TEXT("POST"));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Request->SetContentAsString(RequestBody);
    Request->OnProcessRequestComplete().BindLambda(
        [this, Callback](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [this, Callback, HttpResponse, bConnectedSuccessfully]()
            {
                if (!bConnectedSuccessfully || !HttpResponse.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Could not connect to Agent Core."));
                    return;
                }

                TSharedPtr<FJsonObject> ResponseJson;
                const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResponse->GetContentAsString());
                if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Session resume response is not valid JSON."));
                    return;
                }

                FString ParsedMessage;
                const bool bParsed = ParseSessionDecision(ResponseJson, ActiveSessionSelectedActors, ParsedMessage);
                Callback.ExecuteIfBound(bParsed, ParsedMessage);
            });
        });

    Request->ProcessRequest();
}

void FUEAIAgentTransportModule::SetProviderApiKey(
    const FString& Provider,
    const FString& ApiKey,
    const FOnUEAIAgentCredentialOpFinished& Callback) const
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("provider"), Provider);
    Root->SetStringField(TEXT("apiKey"), ApiKey);

    FString RequestBody;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
    FJsonSerializer::Serialize(Root, Writer);

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildCredentialsSetUrl());
    Request->SetVerb(TEXT("POST"));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Request->SetContentAsString(RequestBody);
    Request->OnProcessRequestComplete().BindLambda(
        [Callback](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [Callback, HttpResponse, bConnectedSuccessfully]()
            {
                if (!bConnectedSuccessfully || !HttpResponse.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Could not connect to Agent Core."));
                    return;
                }

                if (HttpResponse->GetResponseCode() < 200 || HttpResponse->GetResponseCode() >= 300)
                {
                    Callback.ExecuteIfBound(false, FString::Printf(TEXT("Save key failed (%d)."), HttpResponse->GetResponseCode()));
                    return;
                }

                Callback.ExecuteIfBound(true, TEXT("API key saved."));
            });
        });

    Request->ProcessRequest();
}

void FUEAIAgentTransportModule::DeleteProviderApiKey(
    const FString& Provider,
    const FOnUEAIAgentCredentialOpFinished& Callback) const
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("provider"), Provider);

    FString RequestBody;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
    FJsonSerializer::Serialize(Root, Writer);

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildCredentialsDeleteUrl());
    Request->SetVerb(TEXT("POST"));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Request->SetContentAsString(RequestBody);
    Request->OnProcessRequestComplete().BindLambda(
        [Callback](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [Callback, HttpResponse, bConnectedSuccessfully]()
            {
                if (!bConnectedSuccessfully || !HttpResponse.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Could not connect to Agent Core."));
                    return;
                }

                if (HttpResponse->GetResponseCode() < 200 || HttpResponse->GetResponseCode() >= 300)
                {
                    Callback.ExecuteIfBound(false, FString::Printf(TEXT("Delete key failed (%d)."), HttpResponse->GetResponseCode()));
                    return;
                }

                Callback.ExecuteIfBound(true, TEXT("API key removed."));
            });
        });

    Request->ProcessRequest();
}

void FUEAIAgentTransportModule::TestProviderApiKey(
    const FString& Provider,
    const FOnUEAIAgentCredentialOpFinished& Callback) const
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("provider"), Provider);

    FString RequestBody;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
    FJsonSerializer::Serialize(Root, Writer);

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildCredentialsTestUrl());
    Request->SetVerb(TEXT("POST"));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Request->SetContentAsString(RequestBody);
    Request->OnProcessRequestComplete().BindLambda(
        [Callback](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [Callback, HttpResponse, bConnectedSuccessfully]()
            {
                if (!bConnectedSuccessfully || !HttpResponse.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Could not connect to Agent Core."));
                    return;
                }

                if (HttpResponse->GetResponseCode() < 200 || HttpResponse->GetResponseCode() >= 300)
                {
                    Callback.ExecuteIfBound(false, FString::Printf(TEXT("Test key failed (%d)."), HttpResponse->GetResponseCode()));
                    return;
                }

                TSharedPtr<FJsonObject> ResponseJson;
                const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResponse->GetContentAsString());
                if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Test response is not valid JSON."));
                    return;
                }

                bool bOk = false;
                ResponseJson->TryGetBoolField(TEXT("ok"), bOk);
                FString Message = bOk ? TEXT("Provider call succeeded.") : TEXT("Provider call failed.");
                ResponseJson->TryGetStringField(TEXT("message"), Message);
                Callback.ExecuteIfBound(bOk, Message);
            });
        });

    Request->ProcessRequest();
}

void FUEAIAgentTransportModule::GetProviderStatus(const FOnUEAIAgentCredentialOpFinished& Callback) const
{
    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildProviderStatusUrl());
    Request->SetVerb(TEXT("GET"));
    Request->OnProcessRequestComplete().BindLambda(
        [Callback](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [Callback, HttpResponse, bConnectedSuccessfully]()
            {
                if (!bConnectedSuccessfully || !HttpResponse.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Could not connect to Agent Core."));
                    return;
                }

                if (HttpResponse->GetResponseCode() < 200 || HttpResponse->GetResponseCode() >= 300)
                {
                    Callback.ExecuteIfBound(false, FString::Printf(TEXT("Provider status failed (%d)."), HttpResponse->GetResponseCode()));
                    return;
                }

                TSharedPtr<FJsonObject> ResponseJson;
                const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResponse->GetContentAsString());
                if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Provider status response is not valid JSON."));
                    return;
                }

                const TSharedPtr<FJsonObject>* ProvidersObj = nullptr;
                if (!ResponseJson->TryGetObjectField(TEXT("providers"), ProvidersObj) || !ProvidersObj || !ProvidersObj->IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Provider status misses providers object."));
                    return;
                }

                auto BuildLine = [ProvidersObj](const FString& ProviderName) -> FString
                {
                    const TSharedPtr<FJsonObject>* ProviderObj = nullptr;
                    if (!(*ProvidersObj)->TryGetObjectField(ProviderName, ProviderObj) || !ProviderObj || !ProviderObj->IsValid())
                    {
                        return FString::Printf(TEXT("%s: unknown"), *ProviderName);
                    }

                    bool bConfigured = false;
                    (*ProviderObj)->TryGetBoolField(TEXT("configured"), bConfigured);
                    FString Model;
                    (*ProviderObj)->TryGetStringField(TEXT("model"), Model);

                    if (Model.IsEmpty())
                    {
                        return FString::Printf(TEXT("%s: %s"), *ProviderName, bConfigured ? TEXT("configured") : TEXT("not configured"));
                    }

                    return FString::Printf(
                        TEXT("%s: %s (%s)"),
                        *ProviderName,
                        bConfigured ? TEXT("configured") : TEXT("not configured"),
                        *Model);
                };

                const FString Message = BuildLine(TEXT("openai")) + TEXT("\n") + BuildLine(TEXT("gemini"));
                Callback.ExecuteIfBound(true, Message);
            });
        });

    Request->ProcessRequest();
}

int32 FUEAIAgentTransportModule::GetPlannedActionCount() const
{
    return PlannedActions.Num();
}

FString FUEAIAgentTransportModule::GetPlannedActionPreviewText(int32 ActionIndex) const
{
    if (!PlannedActions.IsValidIndex(ActionIndex))
    {
        return TEXT("Invalid action index.");
    }

    const FUEAIAgentPlannedSceneAction& Action = PlannedActions[ActionIndex];
    const FString Suffix = FString::Printf(
        TEXT(" [risk=%s, state=%s, attempts=%d]"),
        *ToRiskText(Action.Risk),
        *ToStateText(Action.State),
        Action.AttemptCount);
    if (Action.Type == EUEAIAgentPlannedActionType::CreateActor)
    {
        return FString::Printf(
            TEXT("Action %d -> scene.createActor count=%d class=%s, Location: X=%.2f Y=%.2f Z=%.2f, Rotation: Pitch=%.2f Yaw=%.2f Roll=%.2f%s"),
            ActionIndex + 1,
            Action.SpawnCount,
            *Action.ActorClass,
            Action.SpawnLocation.X,
            Action.SpawnLocation.Y,
            Action.SpawnLocation.Z,
            Action.SpawnRotation.Pitch,
            Action.SpawnRotation.Yaw,
            Action.SpawnRotation.Roll,
            *Suffix);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::DeleteActor)
    {
        const FString TargetText = Action.ActorNames.Num() > 0
            ? FString::Printf(TEXT("names: %s"), *FString::Join(Action.ActorNames, TEXT(", ")))
            : TEXT("selection");
        return FString::Printf(
            TEXT("Action %d -> scene.deleteActor on %s (%d actor(s))%s"),
            ActionIndex + 1,
            *TargetText,
            Action.ActorNames.Num(),
            *Suffix);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::ModifyComponent)
    {
        const FString TargetText = Action.ActorNames.Num() > 0
            ? FString::Printf(TEXT("names: %s"), *FString::Join(Action.ActorNames, TEXT(", ")))
            : TEXT("selection");
        const FString VisibilityText = Action.bComponentVisibilityEdit
            ? (Action.bComponentVisible ? TEXT("show") : TEXT("hide"))
            : TEXT("none");
        return FString::Printf(
            TEXT("Action %d -> scene.modifyComponent on %s, Component: %s, DeltaLocation: X=%.2f Y=%.2f Z=%.2f, DeltaRotation: Pitch=%.2f Yaw=%.2f Roll=%.2f, DeltaScale: X=%.2f Y=%.2f Z=%.2f, Scale: X=%.2f Y=%.2f Z=%.2f, Visibility: %s%s"),
            ActionIndex + 1,
            *TargetText,
            *Action.ComponentName,
            Action.ComponentDeltaLocation.X,
            Action.ComponentDeltaLocation.Y,
            Action.ComponentDeltaLocation.Z,
            Action.ComponentDeltaRotation.Pitch,
            Action.ComponentDeltaRotation.Yaw,
            Action.ComponentDeltaRotation.Roll,
            Action.ComponentDeltaScale.X,
            Action.ComponentDeltaScale.Y,
            Action.ComponentDeltaScale.Z,
            Action.ComponentScale.X,
            Action.ComponentScale.Y,
            Action.ComponentScale.Z,
            *VisibilityText,
            *Suffix);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::SetComponentMaterial)
    {
        const FString TargetText = Action.ActorNames.Num() > 0
            ? FString::Printf(TEXT("names: %s"), *FString::Join(Action.ActorNames, TEXT(", ")))
            : TEXT("selection");
        return FString::Printf(
            TEXT("Action %d -> scene.setComponentMaterial on %s, Component: %s, Material: %s, Slot: %d%s"),
            ActionIndex + 1,
            *TargetText,
            *Action.ComponentName,
            *Action.MaterialPath,
            Action.MaterialSlot,
            *Suffix);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::SetComponentStaticMesh)
    {
        const FString TargetText = Action.ActorNames.Num() > 0
            ? FString::Printf(TEXT("names: %s"), *FString::Join(Action.ActorNames, TEXT(", ")))
            : TEXT("selection");
        return FString::Printf(
            TEXT("Action %d -> scene.setComponentStaticMesh on %s, Component: %s, Mesh: %s%s"),
            ActionIndex + 1,
            *TargetText,
            *Action.ComponentName,
            *Action.MeshPath,
            *Suffix);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::AddActorTag)
    {
        const FString TargetText = Action.ActorNames.Num() > 0
            ? FString::Printf(TEXT("names: %s"), *FString::Join(Action.ActorNames, TEXT(", ")))
            : TEXT("selection");
        return FString::Printf(
            TEXT("Action %d -> scene.addActorTag on %s tag=%s%s"),
            ActionIndex + 1,
            *TargetText,
            *Action.ActorTag,
            *Suffix);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::SetActorFolder)
    {
        const FString TargetText = Action.ActorNames.Num() > 0
            ? FString::Printf(TEXT("names: %s"), *FString::Join(Action.ActorNames, TEXT(", ")))
            : TEXT("selection");
        return FString::Printf(
            TEXT("Action %d -> scene.setActorFolder on %s folder=%s%s"),
            ActionIndex + 1,
            *TargetText,
            *Action.FolderPath,
            *Suffix);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::AddActorLabelPrefix)
    {
        const FString TargetText = Action.ActorNames.Num() > 0
            ? FString::Printf(TEXT("names: %s"), *FString::Join(Action.ActorNames, TEXT(", ")))
            : TEXT("selection");
        return FString::Printf(
            TEXT("Action %d -> scene.addActorLabelPrefix on %s prefix=%s%s"),
            ActionIndex + 1,
            *TargetText,
            *Action.LabelPrefix,
            *Suffix);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::DuplicateActors)
    {
        const FString TargetText = Action.ActorNames.Num() > 0
            ? FString::Printf(TEXT("names: %s"), *FString::Join(Action.ActorNames, TEXT(", ")))
            : TEXT("selection");
        return FString::Printf(
            TEXT("Action %d -> scene.duplicateActors on %s count=%d, Offset: X=%.2f Y=%.2f Z=%.2f%s"),
            ActionIndex + 1,
            *TargetText,
            Action.DuplicateCount,
            Action.DuplicateOffset.X,
            Action.DuplicateOffset.Y,
            Action.DuplicateOffset.Z,
            *Suffix);
    }

    const FString ModifyTargetText = Action.ActorNames.Num() > 0
        ? FString::Printf(TEXT("names: %s"), *FString::Join(Action.ActorNames, TEXT(", ")))
        : TEXT("selection");
    return FString::Printf(
        TEXT("Action %d -> scene.modifyActor on %s (%d actor(s)), DeltaLocation: X=%.2f Y=%.2f Z=%.2f, DeltaRotation: Pitch=%.2f Yaw=%.2f Roll=%.2f, DeltaScale: X=%.2f Y=%.2f Z=%.2f%s"),
        ActionIndex + 1,
        *ModifyTargetText,
        Action.ActorNames.Num(),
        Action.DeltaLocation.X,
        Action.DeltaLocation.Y,
        Action.DeltaLocation.Z,
        Action.DeltaRotation.Pitch,
        Action.DeltaRotation.Yaw,
        Action.DeltaRotation.Roll,
        Action.DeltaScale.X,
        Action.DeltaScale.Y,
        Action.DeltaScale.Z,
        *Suffix);
}

bool FUEAIAgentTransportModule::IsPlannedActionApproved(int32 ActionIndex) const
{
    if (!PlannedActions.IsValidIndex(ActionIndex))
    {
        return false;
    }

    return PlannedActions[ActionIndex].bApproved;
}

int32 FUEAIAgentTransportModule::GetPlannedActionAttemptCount(int32 ActionIndex) const
{
    if (!PlannedActions.IsValidIndex(ActionIndex))
    {
        return 0;
    }

    return PlannedActions[ActionIndex].AttemptCount;
}

void FUEAIAgentTransportModule::SetPlannedActionApproved(int32 ActionIndex, bool bApproved) const
{
    if (!PlannedActions.IsValidIndex(ActionIndex))
    {
        return;
    }

    PlannedActions[ActionIndex].bApproved = bApproved;
}

bool FUEAIAgentTransportModule::PopApprovedPlannedActions(TArray<FUEAIAgentPlannedSceneAction>& OutActions) const
{
    OutActions.Empty();
    for (const FUEAIAgentPlannedSceneAction& Action : PlannedActions)
    {
        if (Action.bApproved)
        {
            OutActions.Add(Action);
        }
    }

    PlannedActions.Empty();
    return OutActions.Num() > 0;
}

bool FUEAIAgentTransportModule::GetPendingAction(int32 ActionIndex, FUEAIAgentPlannedSceneAction& OutAction) const
{
    if (!PlannedActions.IsValidIndex(ActionIndex))
    {
        return false;
    }

    const FUEAIAgentPlannedSceneAction& Action = PlannedActions[ActionIndex];
    if (Action.State != EUEAIAgentActionState::Pending)
    {
        return false;
    }

    OutAction = Action;
    return true;
}

void FUEAIAgentTransportModule::UpdateActionResult(int32 ActionIndex, bool bSucceeded, int32 AttemptCount) const
{
    if (!PlannedActions.IsValidIndex(ActionIndex))
    {
        return;
    }

    PlannedActions[ActionIndex].State = bSucceeded ? EUEAIAgentActionState::Succeeded : EUEAIAgentActionState::Failed;
    PlannedActions[ActionIndex].AttemptCount = FMath::Max(0, AttemptCount);
}

int32 FUEAIAgentTransportModule::GetNextPendingActionIndex() const
{
    for (int32 Index = 0; Index < PlannedActions.Num(); ++Index)
    {
        if (PlannedActions[Index].State == EUEAIAgentActionState::Pending)
        {
            return Index;
        }
    }
    return INDEX_NONE;
}

bool FUEAIAgentTransportModule::HasActiveSession() const
{
    return !ActiveSessionId.IsEmpty();
}

IMPLEMENT_MODULE(FUEAIAgentTransportModule, UEAIAgentTransport)
