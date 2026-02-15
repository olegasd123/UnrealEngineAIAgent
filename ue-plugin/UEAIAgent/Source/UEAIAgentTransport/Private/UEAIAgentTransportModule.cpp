#include "UEAIAgentTransportModule.h"

#include "UEAIAgentSettings.h"
#include "Async/Async.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "GenericPlatform/GenericPlatformHttp.h"
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

    FString NormalizeChatType(const FString& Value)
    {
        if (Value.Equals(TEXT("chat"), ESearchCase::IgnoreCase))
        {
            return TEXT("chat");
        }
        if (Value.Equals(TEXT("agent"), ESearchCase::IgnoreCase))
        {
            return TEXT("agent");
        }
        return TEXT("");
    }

    bool IsNearlyZeroValue(float Value)
    {
        return FMath::Abs(Value) <= KINDA_SMALL_NUMBER;
    }

    FString FormatSignedFloat(float Value)
    {
        if (IsNearlyZeroValue(Value))
        {
            return TEXT("0");
        }
        return Value > 0.0f
            ? FString::Printf(TEXT("+%.0f"), Value)
            : FString::Printf(TEXT("%.0f"), Value);
    }

    FString ResolveActorLabel(const FString& ActorName)
    {
        if (ActorName.IsEmpty() || !GEditor)
        {
            return ActorName;
        }

        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (!World)
        {
            return ActorName;
        }

        FString LookupName = ActorName;
        LookupName.TrimStartAndEndInline();
        int32 SplitIndex = INDEX_NONE;
        if (LookupName.FindLastChar(TEXT('.'), SplitIndex))
        {
            LookupName = LookupName.Mid(SplitIndex + 1);
        }
        if (LookupName.FindLastChar(TEXT('/'), SplitIndex))
        {
            LookupName = LookupName.Mid(SplitIndex + 1);
        }

        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (!Actor)
            {
                continue;
            }

            if (
                Actor->GetName().Equals(LookupName, ESearchCase::IgnoreCase) ||
                Actor->GetActorLabel().Equals(LookupName, ESearchCase::IgnoreCase) ||
                Actor->GetPathName().EndsWith(LookupName, ESearchCase::IgnoreCase))
            {
                const FString Label = Actor->GetActorLabel();
                return Label.IsEmpty() ? ActorName : Label;
            }
        }

        return ActorName;
    }

    FString FormatActorTargetShort(const TArray<FString>& ActorNames)
    {
        if (ActorNames.Num() <= 0)
        {
            return TEXT("selected actor");
        }

        if (ActorNames.Num() == 1)
        {
            return FString::Printf(TEXT("\"%s\""), *ResolveActorLabel(ActorNames[0]));
        }

        return FString::Printf(TEXT("%d actors"), ActorNames.Num());
    }

    void AddVectorDeltaParts(TArray<FString>& OutParts, const FVector& Value)
    {
        if (!IsNearlyZeroValue(Value.X))
        {
            OutParts.Add(FString::Printf(TEXT("%s on X"), *FormatSignedFloat(Value.X)));
        }
        if (!IsNearlyZeroValue(Value.Y))
        {
            OutParts.Add(FString::Printf(TEXT("%s on Y"), *FormatSignedFloat(Value.Y)));
        }
        if (!IsNearlyZeroValue(Value.Z))
        {
            OutParts.Add(FString::Printf(TEXT("%s on Z"), *FormatSignedFloat(Value.Z)));
        }
    }

    void AddRotationDeltaParts(TArray<FString>& OutParts, const FRotator& Value)
    {
        if (!IsNearlyZeroValue(Value.Pitch))
        {
            OutParts.Add(FString::Printf(TEXT("%s pitch"), *FormatSignedFloat(Value.Pitch)));
        }
        if (!IsNearlyZeroValue(Value.Yaw))
        {
            OutParts.Add(FString::Printf(TEXT("%s yaw"), *FormatSignedFloat(Value.Yaw)));
        }
        if (!IsNearlyZeroValue(Value.Roll))
        {
            OutParts.Add(FString::Printf(TEXT("%s roll"), *FormatSignedFloat(Value.Roll)));
        }
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

        if (Command == TEXT("context.getSceneSummary"))
        {
            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::ContextGetSceneSummary;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);
            OutAction = ParsedAction;
            return true;
        }

        if (Command == TEXT("context.getSelection"))
        {
            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::ContextGetSelection;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);
            OutAction = ParsedAction;
            return true;
        }

        if (Command == TEXT("scene.setDirectionalLightIntensity"))
        {
            FString Target;
            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
            {
                return false;
            }

            double Intensity = 0.0;
            if (!(*ParamsObj)->TryGetNumberField(TEXT("intensity"), Intensity))
            {
                return false;
            }

            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::SetDirectionalLightIntensity;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);
            ParsedAction.ScalarValue = static_cast<float>(Intensity);
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

        if (Command == TEXT("scene.setFogDensity"))
        {
            FString Target;
            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
            {
                return false;
            }

            double Density = 0.0;
            if (!(*ParamsObj)->TryGetNumberField(TEXT("density"), Density))
            {
                return false;
            }

            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::SetFogDensity;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);
            ParsedAction.ScalarValue = static_cast<float>(Density);
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

        if (Command == TEXT("scene.setPostProcessExposureCompensation"))
        {
            FString Target;
            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
            {
                return false;
            }

            double Exposure = 0.0;
            if (!(*ParamsObj)->TryGetNumberField(TEXT("exposureCompensation"), Exposure))
            {
                return false;
            }

            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::SetPostProcessExposureCompensation;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);
            ParsedAction.ScalarValue = static_cast<float>(Exposure);
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

        if (Command == TEXT("session.beginTransaction"))
        {
            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::SessionBeginTransaction;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);
            FString Description;
            if ((*ParamsObj)->TryGetStringField(TEXT("description"), Description))
            {
                ParsedAction.TransactionDescription = Description;
            }
            OutAction = ParsedAction;
            return true;
        }

        if (Command == TEXT("session.commitTransaction"))
        {
            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::SessionCommitTransaction;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);
            OutAction = ParsedAction;
            return true;
        }

        if (Command == TEXT("session.rollbackTransaction"))
        {
            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::SessionRollbackTransaction;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);
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

FString FUEAIAgentTransportModule::BuildModelsUrl(const FString& Provider) const
{
    if (Provider.IsEmpty())
    {
        return BuildBaseUrl() + TEXT("/v1/models");
    }
    return BuildBaseUrl() + TEXT("/v1/models?provider=") + FGenericPlatformHttp::UrlEncode(Provider);
}

FString FUEAIAgentTransportModule::BuildModelPreferencesUrl() const
{
    return BuildBaseUrl() + TEXT("/v1/models/preferences");
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

FString FUEAIAgentTransportModule::BuildChatsUrl(bool bIncludeArchived) const
{
    if (bIncludeArchived)
    {
        return BuildBaseUrl() + TEXT("/v1/chats?includeArchived=true");
    }
    return BuildBaseUrl() + TEXT("/v1/chats");
}

FString FUEAIAgentTransportModule::BuildCreateChatUrl() const
{
    return BuildBaseUrl() + TEXT("/v1/chats");
}

FString FUEAIAgentTransportModule::BuildChatDeleteUrl(const FString& ChatId) const
{
    return BuildBaseUrl() + TEXT("/v1/chats/") + FGenericPlatformHttp::UrlEncode(ChatId);
}

FString FUEAIAgentTransportModule::BuildChatUpdateUrl(const FString& ChatId) const
{
    return BuildBaseUrl() + TEXT("/v1/chats/") + FGenericPlatformHttp::UrlEncode(ChatId);
}

FString FUEAIAgentTransportModule::BuildChatDetailsUrl(const FString& ChatId) const
{
    return BuildBaseUrl() + TEXT("/v1/chats/") + FGenericPlatformHttp::UrlEncode(ChatId) + TEXT("/details");
}

FString FUEAIAgentTransportModule::BuildChatHistoryUrl(const FString& ChatId, int32 Limit) const
{
    FString Url = BuildBaseUrl() + TEXT("/v1/chats/") + FGenericPlatformHttp::UrlEncode(ChatId) + TEXT("/details");
    if (Limit > 0)
    {
        Url += FString::Printf(TEXT("?limit=%d"), FMath::Max(1, Limit));
    }
    return Url;
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
    const FString& Provider,
    const FString& Model,
    const FOnUEAIAgentTaskPlanned& Callback) const
{
    PlannedActions.Empty();
    LastPlanSummary.Empty();
    ActiveSessionId.Empty();
    ActiveSessionActionIndex = INDEX_NONE;
    ActiveSessionSelectedActors.Empty();

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("prompt"), Prompt);
    Root->SetStringField(TEXT("mode"), Mode.IsEmpty() ? TEXT("chat") : Mode);
    Root->SetObjectField(TEXT("context"), BuildContextObject(SelectedActors));
    if (!Provider.IsEmpty())
    {
        Root->SetStringField(TEXT("provider"), Provider);
    }
    if (!Model.IsEmpty())
    {
        Root->SetStringField(TEXT("model"), Model);
    }
    if (!ActiveChatId.IsEmpty())
    {
        Root->SetStringField(TEXT("chatId"), ActiveChatId);
    }

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
                LastPlanSummary = Summary.TrimStartAndEnd();
                TArray<FString> Steps;
                const TArray<TSharedPtr<FJsonValue>>* StepValues = nullptr;
                if ((*PlanObj)->TryGetArrayField(TEXT("steps"), StepValues) && StepValues)
                {
                    for (const TSharedPtr<FJsonValue>& StepValue : *StepValues)
                    {
                        if (!StepValue.IsValid())
                        {
                            continue;
                        }
                        FString StepText;
                        if (StepValue->TryGetString(StepText))
                        {
                            StepText = StepText.TrimStartAndEnd();
                            if (!StepText.IsEmpty())
                            {
                                Steps.Add(StepText);
                            }
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

                        if (Command == TEXT("context.getSceneSummary"))
                        {
                            FUEAIAgentPlannedSceneAction ParsedAction;
                            ParsedAction.Type = EUEAIAgentPlannedActionType::ContextGetSceneSummary;
                            ParsedAction.Risk = ParseRiskLevel(ActionObj);
                            ParsedAction.bApproved = true;
                            PlannedActions.Add(ParsedAction);
                            continue;
                        }

                        if (Command == TEXT("context.getSelection"))
                        {
                            FUEAIAgentPlannedSceneAction ParsedAction;
                            ParsedAction.Type = EUEAIAgentPlannedActionType::ContextGetSelection;
                            ParsedAction.Risk = ParseRiskLevel(ActionObj);
                            ParsedAction.bApproved = true;
                            PlannedActions.Add(ParsedAction);
                            continue;
                        }

                        if (Command == TEXT("scene.setDirectionalLightIntensity"))
                        {
                            FString Target;
                            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
                            {
                                continue;
                            }

                            double Intensity = 0.0;
                            if (!(*ParamsObj)->TryGetNumberField(TEXT("intensity"), Intensity))
                            {
                                continue;
                            }

                            FUEAIAgentPlannedSceneAction ParsedAction;
                            ParsedAction.Type = EUEAIAgentPlannedActionType::SetDirectionalLightIntensity;
                            ParsedAction.Risk = ParseRiskLevel(ActionObj);
                            ParsedAction.ScalarValue = static_cast<float>(Intensity);
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

                        if (Command == TEXT("scene.setFogDensity"))
                        {
                            FString Target;
                            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
                            {
                                continue;
                            }

                            double Density = 0.0;
                            if (!(*ParamsObj)->TryGetNumberField(TEXT("density"), Density))
                            {
                                continue;
                            }

                            FUEAIAgentPlannedSceneAction ParsedAction;
                            ParsedAction.Type = EUEAIAgentPlannedActionType::SetFogDensity;
                            ParsedAction.Risk = ParseRiskLevel(ActionObj);
                            ParsedAction.ScalarValue = static_cast<float>(Density);
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

                        if (Command == TEXT("scene.setPostProcessExposureCompensation"))
                        {
                            FString Target;
                            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
                            {
                                continue;
                            }

                            double Exposure = 0.0;
                            if (!(*ParamsObj)->TryGetNumberField(TEXT("exposureCompensation"), Exposure))
                            {
                                continue;
                            }

                            FUEAIAgentPlannedSceneAction ParsedAction;
                            ParsedAction.Type = EUEAIAgentPlannedActionType::SetPostProcessExposureCompensation;
                            ParsedAction.Risk = ParseRiskLevel(ActionObj);
                            ParsedAction.ScalarValue = static_cast<float>(Exposure);
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

                        if (Command == TEXT("session.beginTransaction"))
                        {
                            FUEAIAgentPlannedSceneAction ParsedAction;
                            ParsedAction.Type = EUEAIAgentPlannedActionType::SessionBeginTransaction;
                            ParsedAction.Risk = ParseRiskLevel(ActionObj);
                            ParsedAction.bApproved = ParsedAction.Risk == EUEAIAgentRiskLevel::Low;
                            FString Description;
                            if ((*ParamsObj)->TryGetStringField(TEXT("description"), Description))
                            {
                                ParsedAction.TransactionDescription = Description;
                            }
                            PlannedActions.Add(ParsedAction);
                            continue;
                        }

                        if (Command == TEXT("session.commitTransaction"))
                        {
                            FUEAIAgentPlannedSceneAction ParsedAction;
                            ParsedAction.Type = EUEAIAgentPlannedActionType::SessionCommitTransaction;
                            ParsedAction.Risk = ParseRiskLevel(ActionObj);
                            ParsedAction.bApproved = ParsedAction.Risk == EUEAIAgentRiskLevel::Low;
                            PlannedActions.Add(ParsedAction);
                            continue;
                        }

                        if (Command == TEXT("session.rollbackTransaction"))
                        {
                            FUEAIAgentPlannedSceneAction ParsedAction;
                            ParsedAction.Type = EUEAIAgentPlannedActionType::SessionRollbackTransaction;
                            ParsedAction.Risk = ParseRiskLevel(ActionObj);
                            ParsedAction.bApproved = ParsedAction.Risk == EUEAIAgentRiskLevel::Low;
                            PlannedActions.Add(ParsedAction);
                            continue;
                        }
                    }
                }

                FString AssistantText;
                ResponseJson->TryGetStringField(TEXT("assistantText"), AssistantText);

                FString FinalMessage;
                if (!AssistantText.IsEmpty())
                {
                    FinalMessage = AssistantText;
                }
                else if (PlannedActions.Num() > 0)
                {
                    FinalMessage = LastPlanSummary;
                    if (FinalMessage.IsEmpty())
                    {
                        FinalMessage = FString::Printf(TEXT("Needs approval: %d action(s)"), PlannedActions.Num());
                    }
                }
                else
                {
                    FinalMessage = Summary;
                    const int32 MaxSteps = FMath::Min(3, Steps.Num());
                    for (int32 StepIndex = 0; StepIndex < MaxSteps; ++StepIndex)
                    {
                        if (FinalMessage.IsEmpty())
                        {
                            FinalMessage = Steps[StepIndex];
                        }
                        else
                        {
                            FinalMessage += TEXT("\n");
                            FinalMessage += Steps[StepIndex];
                        }
                    }
                    if (FinalMessage.IsEmpty())
                    {
                        FinalMessage = TEXT("No action needed.");
                    }
                }
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
    FString AssistantText;
    ResponseJson->TryGetStringField(TEXT("assistantText"), AssistantText);
    if (!AssistantText.IsEmpty())
    {
        OutMessage += TEXT("\nAssistant: ");
        OutMessage += AssistantText;
    }
    return true;
}

void FUEAIAgentTransportModule::StartSession(
    const FString& Prompt,
    const FString& Mode,
    const TArray<FString>& SelectedActors,
    const FString& Provider,
    const FString& Model,
    const FOnUEAIAgentSessionUpdated& Callback) const
{
    PlannedActions.Empty();
    LastPlanSummary.Empty();
    ActiveSessionId.Empty();
    ActiveSessionActionIndex = INDEX_NONE;
    ActiveSessionSelectedActors = SelectedActors;

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("prompt"), Prompt);
    Root->SetStringField(TEXT("mode"), Mode.IsEmpty() ? TEXT("agent") : Mode);
    Root->SetNumberField(TEXT("maxRetries"), 2);
    Root->SetObjectField(TEXT("context"), BuildContextObject(SelectedActors));
    if (!Provider.IsEmpty())
    {
        Root->SetStringField(TEXT("provider"), Provider);
    }
    if (!Model.IsEmpty())
    {
        Root->SetStringField(TEXT("model"), Model);
    }
    if (!ActiveChatId.IsEmpty())
    {
        Root->SetStringField(TEXT("chatId"), ActiveChatId);
    }

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
    if (!ActiveChatId.IsEmpty())
    {
        Root->SetStringField(TEXT("chatId"), ActiveChatId);
    }

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
    if (!ActiveChatId.IsEmpty())
    {
        Root->SetStringField(TEXT("chatId"), ActiveChatId);
    }

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
    if (!ActiveChatId.IsEmpty())
    {
        Root->SetStringField(TEXT("chatId"), ActiveChatId);
    }

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
        [this, Callback](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [this, Callback, HttpResponse, bConnectedSuccessfully]()
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
        [this, Callback](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [this, Callback, HttpResponse, bConnectedSuccessfully]()
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
        [this, Callback](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [this, Callback, HttpResponse, bConnectedSuccessfully]()
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

                const FString Message =
                    BuildLine(TEXT("openai")) + TEXT("\n") +
                    BuildLine(TEXT("gemini")) + TEXT("\n") +
                    BuildLine(TEXT("local"));
                Callback.ExecuteIfBound(true, Message);
            });
        });

    Request->ProcessRequest();
}

void FUEAIAgentTransportModule::RefreshModelOptions(
    const FString& Provider,
    const FOnUEAIAgentCredentialOpFinished& Callback) const
{
    const FString ProviderValue = Provider;
    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildModelsUrl(ProviderValue));
    Request->SetVerb(TEXT("GET"));
    Request->OnProcessRequestComplete().BindLambda(
        [this, Callback, ProviderValue](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [this, Callback, ProviderValue, HttpResponse, bConnectedSuccessfully]()
            {
                if (!bConnectedSuccessfully || !HttpResponse.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Could not connect to Agent Core."));
                    return;
                }

                if (HttpResponse->GetResponseCode() < 200 || HttpResponse->GetResponseCode() >= 300)
                {
                    Callback.ExecuteIfBound(false, FString::Printf(TEXT("Load models failed (%d)."), HttpResponse->GetResponseCode()));
                    return;
                }

                TSharedPtr<FJsonObject> ResponseJson;
                const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResponse->GetContentAsString());
                if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Model response is not valid JSON."));
                    return;
                }

                bool bOk = false;
                if (!ResponseJson->TryGetBoolField(TEXT("ok"), bOk) || !bOk)
                {
                    FString ErrorMessage = TEXT("Model request failed.");
                    ResponseJson->TryGetStringField(TEXT("error"), ErrorMessage);
                    Callback.ExecuteIfBound(false, ErrorMessage);
                    return;
                }

                AvailableModels.Empty();
                PreferredModels.Empty();

                const TArray<TSharedPtr<FJsonValue>>* AvailableArray = nullptr;
                FString SelectedProvider;
                ResponseJson->TryGetStringField(TEXT("provider"), SelectedProvider);
                if (ResponseJson->TryGetArrayField(TEXT("models"), AvailableArray) && AvailableArray)
                {
                    for (const TSharedPtr<FJsonValue>& Value : *AvailableArray)
                    {
                        FString ModelName;
                        if (!Value.IsValid() || !Value->TryGetString(ModelName) || ModelName.IsEmpty())
                        {
                            continue;
                        }
                            FUEAIAgentModelOption Option;
                            Option.Provider = SelectedProvider.IsEmpty() ? ProviderValue : SelectedProvider;
                            Option.Model = ModelName;
                            AvailableModels.Add(Option);
                        }
                    }

                const TArray<TSharedPtr<FJsonValue>>* PreferredArray = nullptr;
                if (ResponseJson->TryGetArrayField(TEXT("preferredModels"), PreferredArray) && PreferredArray)
                {
                    for (const TSharedPtr<FJsonValue>& Value : *PreferredArray)
                    {
                        const TSharedPtr<FJsonObject> ItemObj = Value.IsValid() ? Value->AsObject() : nullptr;
                        if (!ItemObj.IsValid())
                        {
                            continue;
                        }

                        FString Provider;
                        FString Model;
                        if (!ItemObj->TryGetStringField(TEXT("provider"), Provider) ||
                            !ItemObj->TryGetStringField(TEXT("model"), Model) ||
                            Provider.IsEmpty() ||
                            Model.IsEmpty())
                        {
                            continue;
                        }

                        FUEAIAgentModelOption Option;
                        Option.Provider = Provider;
                        Option.Model = Model;
                        PreferredModels.Add(Option);
                    }
                }

                Callback.ExecuteIfBound(
                    true,
                    FString::Printf(TEXT("Models loaded: available %d, preferred %d"), AvailableModels.Num(), PreferredModels.Num()));
            });
        });

    Request->ProcessRequest();
}

void FUEAIAgentTransportModule::SavePreferredModels(
    const TArray<FUEAIAgentModelOption>& Models,
    const FOnUEAIAgentCredentialOpFinished& Callback) const
{
    TArray<TSharedPtr<FJsonValue>> ModelValues;
    for (const FUEAIAgentModelOption& Item : Models)
    {
        if (Item.Provider.IsEmpty() || Item.Model.IsEmpty())
        {
            continue;
        }
        TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("provider"), Item.Provider);
        Row->SetStringField(TEXT("model"), Item.Model);
        ModelValues.Add(MakeShared<FJsonValueObject>(Row));
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetArrayField(TEXT("models"), ModelValues);

    FString RequestBody;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
    FJsonSerializer::Serialize(Root, Writer);

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildModelPreferencesUrl());
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

                if (HttpResponse->GetResponseCode() < 200 || HttpResponse->GetResponseCode() >= 300)
                {
                    Callback.ExecuteIfBound(false, FString::Printf(TEXT("Save models failed (%d)."), HttpResponse->GetResponseCode()));
                    return;
                }

                TSharedPtr<FJsonObject> ResponseJson;
                const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResponse->GetContentAsString());
                if (FJsonSerializer::Deserialize(Reader, ResponseJson) && ResponseJson.IsValid())
                {
                    const TArray<TSharedPtr<FJsonValue>>* PreferredArray = nullptr;
                    if (ResponseJson->TryGetArrayField(TEXT("preferredModels"), PreferredArray) && PreferredArray)
                    {
                        PreferredModels.Empty();
                        for (const TSharedPtr<FJsonValue>& Value : *PreferredArray)
                        {
                            const TSharedPtr<FJsonObject> ItemObj = Value.IsValid() ? Value->AsObject() : nullptr;
                            if (!ItemObj.IsValid())
                            {
                                continue;
                            }

                            FString Provider;
                            FString Model;
                            if (!ItemObj->TryGetStringField(TEXT("provider"), Provider) ||
                                !ItemObj->TryGetStringField(TEXT("model"), Model) ||
                                Provider.IsEmpty() ||
                                Model.IsEmpty())
                            {
                                continue;
                            }

                            FUEAIAgentModelOption Option;
                            Option.Provider = Provider;
                            Option.Model = Model;
                            PreferredModels.Add(Option);
                        }
                    }
                }

                Callback.ExecuteIfBound(true, TEXT("Preferred models saved."));
            });
        });

    Request->ProcessRequest();
}

void FUEAIAgentTransportModule::RefreshChats(bool bIncludeArchived, const FOnUEAIAgentChatOpFinished& Callback) const
{
    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildChatsUrl(bIncludeArchived));
    Request->SetVerb(TEXT("GET"));
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

                if (HttpResponse->GetResponseCode() < 200 || HttpResponse->GetResponseCode() >= 300)
                {
                    Callback.ExecuteIfBound(false, FString::Printf(TEXT("Chat list failed (%d)."), HttpResponse->GetResponseCode()));
                    return;
                }

                TSharedPtr<FJsonObject> ResponseJson;
                const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResponse->GetContentAsString());
                if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Chat list response is not valid JSON."));
                    return;
                }

                bool bOk = false;
                if (!ResponseJson->TryGetBoolField(TEXT("ok"), bOk) || !bOk)
                {
                    FString ErrorMessage = TEXT("Agent Core returned a chat list error.");
                    ResponseJson->TryGetStringField(TEXT("error"), ErrorMessage);
                    Callback.ExecuteIfBound(false, ErrorMessage);
                    return;
                }

                Chats.Empty();
                const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
                if (ResponseJson->TryGetArrayField(TEXT("chats"), Items) && Items)
                {
                    for (const TSharedPtr<FJsonValue>& Value : *Items)
                    {
                        if (!Value.IsValid())
                        {
                            continue;
                        }

                        const TSharedPtr<FJsonObject> ChatObj = Value->AsObject();
                        if (!ChatObj.IsValid())
                        {
                            continue;
                        }

                        FString Id;
                        if (!ChatObj->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
                        {
                            continue;
                        }

                        FUEAIAgentChatSummary Chat;
                        Chat.Id = Id;
                        ChatObj->TryGetStringField(TEXT("title"), Chat.Title);
                        ChatObj->TryGetBoolField(TEXT("archived"), Chat.bArchived);
                        ChatObj->TryGetStringField(TEXT("lastActivityAt"), Chat.LastActivityAt);
                        Chats.Add(Chat);
                    }
                }

                if (!ActiveChatId.IsEmpty())
                {
                    const bool bExists = Chats.ContainsByPredicate([this](const FUEAIAgentChatSummary& Chat)
                    {
                        return Chat.Id == ActiveChatId;
                    });
                    if (!bExists)
                    {
                        ActiveChatId.Empty();
                        ActiveChatHistory.Empty();
                    }
                }

                Callback.ExecuteIfBound(true, FString::Printf(TEXT("Chats loaded: %d"), Chats.Num()));
            });
        });

    Request->ProcessRequest();
}

void FUEAIAgentTransportModule::CreateChat(const FString& Title, const FOnUEAIAgentChatOpFinished& Callback) const
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    const FString TrimmedTitle = Title.TrimStartAndEnd();
    if (!TrimmedTitle.IsEmpty())
    {
        Root->SetStringField(TEXT("title"), TrimmedTitle);
    }

    FString RequestBody;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
    FJsonSerializer::Serialize(Root, Writer);

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildCreateChatUrl());
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

                if (HttpResponse->GetResponseCode() < 200 || HttpResponse->GetResponseCode() >= 300)
                {
                    Callback.ExecuteIfBound(false, FString::Printf(TEXT("Create chat failed (%d)."), HttpResponse->GetResponseCode()));
                    return;
                }

                TSharedPtr<FJsonObject> ResponseJson;
                const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResponse->GetContentAsString());
                if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Create chat response is not valid JSON."));
                    return;
                }

                bool bOk = false;
                if (!ResponseJson->TryGetBoolField(TEXT("ok"), bOk) || !bOk)
                {
                    FString ErrorMessage = TEXT("Agent Core returned a create chat error.");
                    ResponseJson->TryGetStringField(TEXT("error"), ErrorMessage);
                    Callback.ExecuteIfBound(false, ErrorMessage);
                    return;
                }

                const TSharedPtr<FJsonObject>* ChatObj = nullptr;
                if (!ResponseJson->TryGetObjectField(TEXT("chat"), ChatObj) || !ChatObj || !ChatObj->IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Create chat response misses chat object."));
                    return;
                }

                FString NewChatId;
                (*ChatObj)->TryGetStringField(TEXT("id"), NewChatId);
                FString NewTitle;
                (*ChatObj)->TryGetStringField(TEXT("title"), NewTitle);
                bool bArchived = false;
                (*ChatObj)->TryGetBoolField(TEXT("archived"), bArchived);
                FString LastActivityAt;
                (*ChatObj)->TryGetStringField(TEXT("lastActivityAt"), LastActivityAt);

                ActiveChatId = NewChatId;
                ActiveChatHistory.Empty();
                if (!NewChatId.IsEmpty())
                {
                    Chats.RemoveAll([&NewChatId](const FUEAIAgentChatSummary& Existing)
                    {
                        return Existing.Id == NewChatId;
                    });

                    FUEAIAgentChatSummary NewChat;
                    NewChat.Id = NewChatId;
                    NewChat.Title = NewTitle;
                    NewChat.bArchived = bArchived;
                    NewChat.LastActivityAt = LastActivityAt;
                    Chats.Insert(NewChat, 0);
                }

                Callback.ExecuteIfBound(true, TEXT("Chat created."));
            });
        });

    Request->ProcessRequest();
}

void FUEAIAgentTransportModule::RenameActiveChat(const FString& NewTitle, const FOnUEAIAgentChatOpFinished& Callback) const
{
    if (ActiveChatId.IsEmpty())
    {
        Callback.ExecuteIfBound(false, TEXT("No active chat selected."));
        return;
    }

    const FString TrimmedTitle = NewTitle.TrimStartAndEnd();
    if (TrimmedTitle.IsEmpty())
    {
        Callback.ExecuteIfBound(false, TEXT("Title must not be empty."));
        return;
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("title"), TrimmedTitle);

    FString RequestBody;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
    FJsonSerializer::Serialize(Root, Writer);

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildChatUpdateUrl(ActiveChatId));
    Request->SetVerb(TEXT("PATCH"));
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

                if (HttpResponse->GetResponseCode() < 200 || HttpResponse->GetResponseCode() >= 300)
                {
                    Callback.ExecuteIfBound(false, FString::Printf(TEXT("Rename chat failed (%d)."), HttpResponse->GetResponseCode()));
                    return;
                }

                TSharedPtr<FJsonObject> ResponseJson;
                const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResponse->GetContentAsString());
                if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Rename chat response is not valid JSON."));
                    return;
                }

                bool bOk = false;
                if (!ResponseJson->TryGetBoolField(TEXT("ok"), bOk) || !bOk)
                {
                    FString ErrorMessage = TEXT("Agent Core returned a rename chat error.");
                    ResponseJson->TryGetStringField(TEXT("error"), ErrorMessage);
                    Callback.ExecuteIfBound(false, ErrorMessage);
                    return;
                }

                const TSharedPtr<FJsonObject>* ChatObj = nullptr;
                if (ResponseJson->TryGetObjectField(TEXT("chat"), ChatObj) && ChatObj && ChatObj->IsValid())
                {
                    FString ChatId;
                    (*ChatObj)->TryGetStringField(TEXT("id"), ChatId);
                    FString ChatTitle;
                    (*ChatObj)->TryGetStringField(TEXT("title"), ChatTitle);

                    for (FUEAIAgentChatSummary& Chat : Chats)
                    {
                        if (Chat.Id == ChatId)
                        {
                            Chat.Title = ChatTitle;
                            break;
                        }
                    }
                }

                Callback.ExecuteIfBound(true, TEXT("Chat title updated."));
            });
        });

    Request->ProcessRequest();
}

void FUEAIAgentTransportModule::ArchiveActiveChat(const FOnUEAIAgentChatOpFinished& Callback) const
{
    if (ActiveChatId.IsEmpty())
    {
        Callback.ExecuteIfBound(false, TEXT("No active chat selected."));
        return;
    }

    ArchiveChat(ActiveChatId, Callback);
}

void FUEAIAgentTransportModule::ArchiveChat(const FString& ChatId, const FOnUEAIAgentChatOpFinished& Callback) const
{
    if (ChatId.IsEmpty())
    {
        Callback.ExecuteIfBound(false, TEXT("No chat selected."));
        return;
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetBoolField(TEXT("archived"), true);

    FString RequestBody;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
    FJsonSerializer::Serialize(Root, Writer);

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildChatUpdateUrl(ChatId));
    Request->SetVerb(TEXT("PATCH"));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Request->SetContentAsString(RequestBody);
    Request->OnProcessRequestComplete().BindLambda(
        [this, Callback, ChatId](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [this, Callback, ChatId, HttpResponse, bConnectedSuccessfully]()
            {
                if (!bConnectedSuccessfully || !HttpResponse.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Could not connect to Agent Core."));
                    return;
                }

                if (HttpResponse->GetResponseCode() < 200 || HttpResponse->GetResponseCode() >= 300)
                {
                    Callback.ExecuteIfBound(false, FString::Printf(TEXT("Archive chat failed (%d)."), HttpResponse->GetResponseCode()));
                    return;
                }

                TSharedPtr<FJsonObject> ResponseJson;
                const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResponse->GetContentAsString());
                if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Archive chat response is not valid JSON."));
                    return;
                }

                bool bOk = false;
                if (!ResponseJson->TryGetBoolField(TEXT("ok"), bOk) || !bOk)
                {
                    FString ErrorMessage = TEXT("Agent Core returned an archive chat error.");
                    ResponseJson->TryGetStringField(TEXT("error"), ErrorMessage);
                    Callback.ExecuteIfBound(false, ErrorMessage);
                    return;
                }

                const TSharedPtr<FJsonObject>* ChatObj = nullptr;
                if (ResponseJson->TryGetObjectField(TEXT("chat"), ChatObj) && ChatObj && ChatObj->IsValid())
                {
                    FString UpdatedChatId;
                    (*ChatObj)->TryGetStringField(TEXT("id"), UpdatedChatId);
                    FString UpdatedTitle;
                    (*ChatObj)->TryGetStringField(TEXT("title"), UpdatedTitle);
                    bool bArchived = false;
                    (*ChatObj)->TryGetBoolField(TEXT("archived"), bArchived);
                    FString LastActivityAt;
                    (*ChatObj)->TryGetStringField(TEXT("lastActivityAt"), LastActivityAt);

                    for (FUEAIAgentChatSummary& Chat : Chats)
                    {
                        if (Chat.Id == UpdatedChatId)
                        {
                            Chat.Title = UpdatedTitle;
                            Chat.bArchived = bArchived;
                            Chat.LastActivityAt = LastActivityAt;
                            break;
                        }
                    }
                }

                Callback.ExecuteIfBound(true, TEXT("Chat archived."));
            });
        });

    Request->ProcessRequest();
}

void FUEAIAgentTransportModule::RestoreChat(const FString& ChatId, const FOnUEAIAgentChatOpFinished& Callback) const
{
    if (ChatId.IsEmpty())
    {
        Callback.ExecuteIfBound(false, TEXT("No chat selected."));
        return;
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetBoolField(TEXT("archived"), false);

    FString RequestBody;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
    FJsonSerializer::Serialize(Root, Writer);

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildChatUpdateUrl(ChatId));
    Request->SetVerb(TEXT("PATCH"));
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

                if (HttpResponse->GetResponseCode() < 200 || HttpResponse->GetResponseCode() >= 300)
                {
                    Callback.ExecuteIfBound(false, FString::Printf(TEXT("Restore chat failed (%d)."), HttpResponse->GetResponseCode()));
                    return;
                }

                TSharedPtr<FJsonObject> ResponseJson;
                const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResponse->GetContentAsString());
                if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Restore chat response is not valid JSON."));
                    return;
                }

                bool bOk = false;
                if (!ResponseJson->TryGetBoolField(TEXT("ok"), bOk) || !bOk)
                {
                    FString ErrorMessage = TEXT("Agent Core returned a restore chat error.");
                    ResponseJson->TryGetStringField(TEXT("error"), ErrorMessage);
                    Callback.ExecuteIfBound(false, ErrorMessage);
                    return;
                }

                const TSharedPtr<FJsonObject>* ChatObj = nullptr;
                if (ResponseJson->TryGetObjectField(TEXT("chat"), ChatObj) && ChatObj && ChatObj->IsValid())
                {
                    FString UpdatedChatId;
                    (*ChatObj)->TryGetStringField(TEXT("id"), UpdatedChatId);
                    FString UpdatedTitle;
                    (*ChatObj)->TryGetStringField(TEXT("title"), UpdatedTitle);
                    bool bArchived = false;
                    (*ChatObj)->TryGetBoolField(TEXT("archived"), bArchived);
                    FString LastActivityAt;
                    (*ChatObj)->TryGetStringField(TEXT("lastActivityAt"), LastActivityAt);

                    for (FUEAIAgentChatSummary& Chat : Chats)
                    {
                        if (Chat.Id == UpdatedChatId)
                        {
                            Chat.Title = UpdatedTitle;
                            Chat.bArchived = bArchived;
                            Chat.LastActivityAt = LastActivityAt;
                            break;
                        }
                    }
                }

                Callback.ExecuteIfBound(true, TEXT("Chat restored."));
            });
        });

    Request->ProcessRequest();
}

void FUEAIAgentTransportModule::DeleteChat(const FString& ChatId, const FOnUEAIAgentChatOpFinished& Callback) const
{
    if (ChatId.IsEmpty())
    {
        Callback.ExecuteIfBound(false, TEXT("No chat selected."));
        return;
    }

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildChatDeleteUrl(ChatId));
    Request->SetVerb(TEXT("DELETE"));
    Request->OnProcessRequestComplete().BindLambda(
        [this, Callback, ChatId](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [this, Callback, ChatId, HttpResponse, bConnectedSuccessfully]()
            {
                if (!bConnectedSuccessfully || !HttpResponse.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Could not connect to Agent Core."));
                    return;
                }

                if (HttpResponse->GetResponseCode() < 200 || HttpResponse->GetResponseCode() >= 300)
                {
                    Callback.ExecuteIfBound(false, FString::Printf(TEXT("Delete chat failed (%d)."), HttpResponse->GetResponseCode()));
                    return;
                }

                if (ActiveChatId == ChatId)
                {
                    ActiveChatId.Empty();
                    ActiveChatHistory.Empty();
                }
                Chats.RemoveAll([&ChatId](const FUEAIAgentChatSummary& Existing)
                {
                    return Existing.Id == ChatId;
                });

                Callback.ExecuteIfBound(true, TEXT("Chat deleted."));
            });
        });

    Request->ProcessRequest();
}

void FUEAIAgentTransportModule::LoadActiveChatHistory(int32 Limit, const FOnUEAIAgentChatOpFinished& Callback) const
{
    if (ActiveChatId.IsEmpty())
    {
        ActiveChatHistory.Empty();
        Callback.ExecuteIfBound(true, TEXT("No active chat selected."));
        return;
    }

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildChatHistoryUrl(ActiveChatId, Limit));
    Request->SetVerb(TEXT("GET"));
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

                if (HttpResponse->GetResponseCode() < 200 || HttpResponse->GetResponseCode() >= 300)
                {
                    Callback.ExecuteIfBound(false, FString::Printf(TEXT("Chat history failed (%d)."), HttpResponse->GetResponseCode()));
                    return;
                }

                TSharedPtr<FJsonObject> ResponseJson;
                const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResponse->GetContentAsString());
                if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Chat history response is not valid JSON."));
                    return;
                }

                bool bOk = false;
                if (!ResponseJson->TryGetBoolField(TEXT("ok"), bOk) || !bOk)
                {
                    FString ErrorMessage = TEXT("Agent Core returned a chat history error.");
                    ResponseJson->TryGetStringField(TEXT("error"), ErrorMessage);
                    Callback.ExecuteIfBound(false, ErrorMessage);
                    return;
                }

                ActiveChatHistory.Empty();
                auto ParseDetails = [this](const TArray<TSharedPtr<FJsonValue>>& SourceItems)
                {
                    for (const TSharedPtr<FJsonValue>& Value : SourceItems)
                    {
                        if (!Value.IsValid())
                        {
                            continue;
                        }

                        const TSharedPtr<FJsonObject> EntryObj = Value->AsObject();
                        if (!EntryObj.IsValid())
                        {
                            continue;
                        }

                        FUEAIAgentChatHistoryEntry Entry;
                        EntryObj->TryGetStringField(TEXT("kind"), Entry.Kind);
                        EntryObj->TryGetStringField(TEXT("route"), Entry.Route);
                        EntryObj->TryGetStringField(TEXT("summary"), Entry.Summary);
                        EntryObj->TryGetStringField(TEXT("provider"), Entry.Provider);
                        EntryObj->TryGetStringField(TEXT("model"), Entry.Model);
                        EntryObj->TryGetStringField(TEXT("chatType"), Entry.ChatType);
                        const TSharedPtr<FJsonObject>* PayloadObj = nullptr;
                        if (EntryObj->TryGetObjectField(TEXT("payload"), PayloadObj) && PayloadObj && PayloadObj->IsValid())
                        {
                            (*PayloadObj)->TryGetStringField(TEXT("displayRole"), Entry.DisplayRole);
                            (*PayloadObj)->TryGetStringField(TEXT("displayText"), Entry.DisplayText);
                            if (Entry.Provider.IsEmpty())
                            {
                                (*PayloadObj)->TryGetStringField(TEXT("provider"), Entry.Provider);
                            }
                            if (Entry.Model.IsEmpty())
                            {
                                (*PayloadObj)->TryGetStringField(TEXT("model"), Entry.Model);
                            }
                            if (Entry.ChatType.IsEmpty())
                            {
                                FString PayloadChatType;
                                if (!(*PayloadObj)->TryGetStringField(TEXT("chatType"), PayloadChatType))
                                {
                                    (*PayloadObj)->TryGetStringField(TEXT("mode"), PayloadChatType);
                                }
                                Entry.ChatType = NormalizeChatType(PayloadChatType);
                            }
                        }
                        Entry.ChatType = NormalizeChatType(Entry.ChatType);
                        if (Entry.DisplayRole.IsEmpty())
                        {
                            Entry.DisplayRole = Entry.Kind.Equals(TEXT("asked"), ESearchCase::IgnoreCase)
                                ? TEXT("user")
                                : TEXT("assistant");
                        }
                        if (Entry.DisplayText.IsEmpty())
                        {
                            Entry.DisplayText = Entry.Summary;
                        }
                        EntryObj->TryGetStringField(TEXT("createdAt"), Entry.CreatedAt);
                        ActiveChatHistory.Add(Entry);
                    }
                };

                const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
                if (ResponseJson->TryGetArrayField(TEXT("details"), Items) && Items)
                {
                    ParseDetails(*Items);
                }

                Callback.ExecuteIfBound(true, FString::Printf(TEXT("History loaded: %d"), ActiveChatHistory.Num()));
            });
        });

    Request->ProcessRequest();
}

void FUEAIAgentTransportModule::AppendActiveChatAssistantMessage(
    const FString& Route,
    const FString& Summary,
    const FString& DisplayText,
    const FString& Provider,
    const FString& Model,
    const FString& ChatType,
    const FOnUEAIAgentChatOpFinished& Callback) const
{
    if (ActiveChatId.IsEmpty())
    {
        Callback.ExecuteIfBound(false, TEXT("No active chat selected."));
        return;
    }

    const FString NormalizedRoute = Route.TrimStartAndEnd().IsEmpty() ? TEXT("/v1/task/apply") : Route.TrimStartAndEnd();
    const FString NormalizedSummary = Summary.TrimStartAndEnd();
    if (NormalizedSummary.IsEmpty())
    {
        Callback.ExecuteIfBound(false, TEXT("Summary must not be empty."));
        return;
    }

    const FString NormalizedText = DisplayText.TrimStartAndEnd().IsEmpty() ? NormalizedSummary : DisplayText.TrimStartAndEnd();

    TSharedRef<FJsonObject> PayloadObj = MakeShared<FJsonObject>();
    PayloadObj->SetStringField(TEXT("displayRole"), TEXT("assistant"));
    PayloadObj->SetStringField(TEXT("displayText"), NormalizedText);
    if (!Provider.TrimStartAndEnd().IsEmpty())
    {
        PayloadObj->SetStringField(TEXT("provider"), Provider.TrimStartAndEnd());
    }
    if (!Model.TrimStartAndEnd().IsEmpty())
    {
        PayloadObj->SetStringField(TEXT("model"), Model.TrimStartAndEnd());
    }
    if (!ChatType.TrimStartAndEnd().IsEmpty())
    {
        PayloadObj->SetStringField(TEXT("chatType"), ChatType.TrimStartAndEnd());
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("route"), NormalizedRoute);
    Root->SetStringField(TEXT("summary"), NormalizedSummary);
    Root->SetObjectField(TEXT("payload"), PayloadObj);

    FString RequestBody;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
    FJsonSerializer::Serialize(Root, Writer);

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildChatDetailsUrl(ActiveChatId));
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
                    Callback.ExecuteIfBound(false, FString::Printf(TEXT("Append chat message failed (%d)."), HttpResponse->GetResponseCode()));
                    return;
                }

                TSharedPtr<FJsonObject> ResponseJson;
                const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResponse->GetContentAsString());
                if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Append chat message response is not valid JSON."));
                    return;
                }

                bool bOk = false;
                if (!ResponseJson->TryGetBoolField(TEXT("ok"), bOk) || !bOk)
                {
                    FString ErrorMessage = TEXT("Agent Core returned an append chat message error.");
                    ResponseJson->TryGetStringField(TEXT("error"), ErrorMessage);
                    Callback.ExecuteIfBound(false, ErrorMessage);
                    return;
                }

                Callback.ExecuteIfBound(true, TEXT("Chat message appended."));
            });
        });

    Request->ProcessRequest();
}

const TArray<FUEAIAgentChatSummary>& FUEAIAgentTransportModule::GetChats() const
{
    return Chats;
}

const TArray<FUEAIAgentChatHistoryEntry>& FUEAIAgentTransportModule::GetActiveChatHistory() const
{
    return ActiveChatHistory;
}

const TArray<FUEAIAgentModelOption>& FUEAIAgentTransportModule::GetAvailableModels() const
{
    return AvailableModels;
}

const TArray<FUEAIAgentModelOption>& FUEAIAgentTransportModule::GetPreferredModels() const
{
    return PreferredModels;
}

void FUEAIAgentTransportModule::SetActiveChatId(const FString& ChatId) const
{
    ActiveChatId = ChatId;
}

FString FUEAIAgentTransportModule::GetActiveChatId() const
{
    return ActiveChatId;
}

FString FUEAIAgentTransportModule::GetLastPlanSummary() const
{
    return LastPlanSummary;
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
    const FString TargetText = FormatActorTargetShort(Action.ActorNames);
    if (Action.Type == EUEAIAgentPlannedActionType::ContextGetSceneSummary)
    {
        return FString::Printf(
            TEXT("Action %d: Read scene summary"),
            ActionIndex + 1);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::ContextGetSelection)
    {
        return FString::Printf(
            TEXT("Action %d: Read current selection"),
            ActionIndex + 1);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::CreateActor)
    {
        const FString SpawnTarget = Action.SpawnCount == 1
            ? FString::Printf(TEXT("1 %s"), *Action.ActorClass)
            : FString::Printf(TEXT("%d %s actors"), Action.SpawnCount, *Action.ActorClass);
        return FString::Printf(
            TEXT("Action %d: Create %s"),
            ActionIndex + 1,
            *SpawnTarget);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::DeleteActor)
    {
        return FString::Printf(
            TEXT("Action %d: Delete %s"),
            ActionIndex + 1,
            *TargetText);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::ModifyComponent)
    {
        TArray<FString> Parts;
        AddVectorDeltaParts(Parts, Action.ComponentDeltaLocation);
        AddRotationDeltaParts(Parts, Action.ComponentDeltaRotation);
        AddVectorDeltaParts(Parts, Action.ComponentDeltaScale);
        if (Action.bComponentVisibilityEdit)
        {
            Parts.Add(Action.bComponentVisible ? TEXT("show") : TEXT("hide"));
        }
        const FString ChangeText = Parts.Num() > 0 ? FString::Join(Parts, TEXT(", ")) : TEXT("update");
        return FString::Printf(
            TEXT("Action %d: Modify component \"%s\" on %s (%s)"),
            ActionIndex + 1,
            *Action.ComponentName,
            *TargetText,
            *ChangeText);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::SetComponentMaterial)
    {
        return FString::Printf(
            TEXT("Action %d: Set material on \"%s\" for %s"),
            ActionIndex + 1,
            *Action.ComponentName,
            *TargetText);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::SetComponentStaticMesh)
    {
        return FString::Printf(
            TEXT("Action %d: Set static mesh on \"%s\" for %s"),
            ActionIndex + 1,
            *Action.ComponentName,
            *TargetText);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::AddActorTag)
    {
        return FString::Printf(
            TEXT("Action %d: Add tag \"%s\" to %s"),
            ActionIndex + 1,
            *Action.ActorTag,
            *TargetText);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::SetActorFolder)
    {
        const FString FolderText = Action.FolderPath.IsEmpty() ? TEXT("root") : Action.FolderPath;
        return FString::Printf(
            TEXT("Action %d: Set folder \"%s\" for %s"),
            ActionIndex + 1,
            *FolderText,
            *TargetText);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::AddActorLabelPrefix)
    {
        return FString::Printf(
            TEXT("Action %d: Add label prefix \"%s\" for %s"),
            ActionIndex + 1,
            *Action.LabelPrefix,
            *TargetText);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::DuplicateActors)
    {
        return FString::Printf(
            TEXT("Action %d: Duplicate %s x%d"),
            ActionIndex + 1,
            *TargetText,
            Action.DuplicateCount);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::SetDirectionalLightIntensity)
    {
        return FString::Printf(
            TEXT("Action %d: Set directional light intensity to %.2f for %s"),
            ActionIndex + 1,
            Action.ScalarValue,
            *TargetText);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::SetFogDensity)
    {
        return FString::Printf(
            TEXT("Action %d: Set fog density to %.4f for %s"),
            ActionIndex + 1,
            Action.ScalarValue,
            *TargetText);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::SetPostProcessExposureCompensation)
    {
        return FString::Printf(
            TEXT("Action %d: Set exposure compensation to %.2f for %s"),
            ActionIndex + 1,
            Action.ScalarValue,
            *TargetText);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::SessionBeginTransaction)
    {
        return FString::Printf(
            TEXT("Action %d: Prepare internal transaction"),
            ActionIndex + 1);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::SessionCommitTransaction)
    {
        return FString::Printf(
            TEXT("Action %d: Finalize internal transaction"),
            ActionIndex + 1);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::SessionRollbackTransaction)
    {
        return FString::Printf(
            TEXT("Action %d: Roll back internal transaction"),
            ActionIndex + 1);
    }

    TArray<FString> Parts;
    AddVectorDeltaParts(Parts, Action.DeltaLocation);
    AddRotationDeltaParts(Parts, Action.DeltaRotation);
    AddVectorDeltaParts(Parts, Action.DeltaScale);
    const FString ChangeText = Parts.Num() > 0 ? FString::Join(Parts, TEXT(", ")) : TEXT("update");
    return FString::Printf(
        TEXT("Action %d: Move %s (%s)"),
        ActionIndex + 1,
        *TargetText,
        *ChangeText);
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

void FUEAIAgentTransportModule::ClearPlannedActions() const
{
    PlannedActions.Empty();
}

bool FUEAIAgentTransportModule::GetPlannedAction(int32 ActionIndex, FUEAIAgentPlannedSceneAction& OutAction) const
{
    if (!PlannedActions.IsValidIndex(ActionIndex))
    {
        return false;
    }

    OutAction = PlannedActions[ActionIndex];
    return true;
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
