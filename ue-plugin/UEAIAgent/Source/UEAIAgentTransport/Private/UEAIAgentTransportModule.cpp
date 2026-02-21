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
    const FString GlobalChatStateKey = TEXT("__global__");

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

    struct FContextUsageDisplay
    {
        FString Label;
        FString Tooltip;
    };

    FContextUsageDisplay BuildContextUsageDisplay(const TSharedPtr<FJsonObject>& UsageObj)
    {
        FContextUsageDisplay Result;
        if (!UsageObj.IsValid())
        {
            return Result;
        }

        double UsedPercent = 0.0;
        const bool bHasPercent = UsageObj->TryGetNumberField(TEXT("usedPercent"), UsedPercent);

        double UsedTokens = 0.0;
        const bool bHasUsedTokens = UsageObj->TryGetNumberField(TEXT("usedTokens"), UsedTokens);

        double ContextWindowTokens = 0.0;
        const bool bHasContextWindow = UsageObj->TryGetNumberField(TEXT("contextWindowTokens"), ContextWindowTokens);

        if (!bHasPercent && (!bHasUsedTokens || !bHasContextWindow || ContextWindowTokens <= 0.0))
        {
            return Result;
        }

        if (!bHasPercent && bHasUsedTokens && bHasContextWindow && ContextWindowTokens > 0.0)
        {
            UsedPercent = (UsedTokens / ContextWindowTokens) * 100.0;
        }

        UsedPercent = FMath::Max(0.0, UsedPercent);
        const int32 RoundedPercent = FMath::Max(0, FMath::RoundToInt(static_cast<float>(UsedPercent)));
        Result.Label = FString::Printf(TEXT("%d%%"), RoundedPercent);

        if (bHasUsedTokens && bHasContextWindow && ContextWindowTokens > 0.0)
        {
            Result.Tooltip = FString::Printf(
                TEXT("Context: %.0f%% full (%d/%d tokens)"),
                UsedPercent,
                FMath::Max(0, FMath::RoundToInt(static_cast<float>(UsedTokens))),
                FMath::Max(1, FMath::RoundToInt(static_cast<float>(ContextWindowTokens))));
            return Result;
        }

        Result.Tooltip = FString::Printf(TEXT("Context: %.0f%%"), UsedPercent);
        return Result;
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

    bool ParseJsonVectorField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, FVector& OutVector)
    {
        const TSharedPtr<FJsonObject>* VectorObj = nullptr;
        if (!Obj.IsValid() || !Obj->TryGetObjectField(FieldName, VectorObj) || !VectorObj || !VectorObj->IsValid())
        {
            return false;
        }

        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        if (!(*VectorObj)->TryGetNumberField(TEXT("x"), X) ||
            !(*VectorObj)->TryGetNumberField(TEXT("y"), Y) ||
            !(*VectorObj)->TryGetNumberField(TEXT("z"), Z))
        {
            return false;
        }

        OutVector = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
        return true;
    }

    bool ParseJsonRotatorField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, FRotator& OutRotator)
    {
        const TSharedPtr<FJsonObject>* RotatorObj = nullptr;
        if (!Obj.IsValid() || !Obj->TryGetObjectField(FieldName, RotatorObj) || !RotatorObj || !RotatorObj->IsValid())
        {
            return false;
        }

        double Pitch = 0.0;
        double Yaw = 0.0;
        double Roll = 0.0;
        if (!(*RotatorObj)->TryGetNumberField(TEXT("pitch"), Pitch) ||
            !(*RotatorObj)->TryGetNumberField(TEXT("yaw"), Yaw) ||
            !(*RotatorObj)->TryGetNumberField(TEXT("roll"), Roll))
        {
            return false;
        }

        OutRotator = FRotator(static_cast<float>(Pitch), static_cast<float>(Yaw), static_cast<float>(Roll));
        return true;
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

        if (Command == TEXT("editor.undo"))
        {
            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::EditorUndo;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);
            OutAction = ParsedAction;
            return true;
        }

        if (Command == TEXT("editor.redo"))
        {
            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::EditorRedo;
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

        if (Command == TEXT("landscape.sculpt"))
        {
            FString Target;
            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
            {
                return false;
            }

            const TSharedPtr<FJsonObject>* CenterObj = nullptr;
            const TSharedPtr<FJsonObject>* SizeObj = nullptr;
            if (!(*ParamsObj)->TryGetObjectField(TEXT("center"), CenterObj) || !CenterObj || !CenterObj->IsValid())
            {
                return false;
            }
            if (!(*ParamsObj)->TryGetObjectField(TEXT("size"), SizeObj) || !SizeObj || !SizeObj->IsValid())
            {
                return false;
            }

            double CenterX = 0.0;
            double CenterY = 0.0;
            double SizeX = 0.0;
            double SizeY = 0.0;
            if (!(*CenterObj)->TryGetNumberField(TEXT("x"), CenterX) ||
                !(*CenterObj)->TryGetNumberField(TEXT("y"), CenterY) ||
                !(*SizeObj)->TryGetNumberField(TEXT("x"), SizeX) ||
                !(*SizeObj)->TryGetNumberField(TEXT("y"), SizeY))
            {
                return false;
            }

            double Strength = 0.0;
            if (!(*ParamsObj)->TryGetNumberField(TEXT("strength"), Strength))
            {
                return false;
            }

            double Falloff = 0.5;
            (*ParamsObj)->TryGetNumberField(TEXT("falloff"), Falloff);

            FString Mode = TEXT("raise");
            (*ParamsObj)->TryGetStringField(TEXT("mode"), Mode);

            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::LandscapeSculpt;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);
            ParsedAction.LandscapeCenter = FVector2D(static_cast<float>(CenterX), static_cast<float>(CenterY));
            ParsedAction.LandscapeSize = FVector2D(
                FMath::Abs(static_cast<float>(SizeX)),
                FMath::Abs(static_cast<float>(SizeY)));
            ParsedAction.LandscapeStrength = FMath::Clamp(static_cast<float>(Strength), 0.0f, 1.0f);
            ParsedAction.LandscapeFalloff = FMath::Clamp(static_cast<float>(Falloff), 0.0f, 1.0f);
            ParsedAction.bLandscapeInvertMode = Mode.Equals(TEXT("lower"), ESearchCase::IgnoreCase);
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

        if (Command == TEXT("landscape.paintLayer"))
        {
            FString Target;
            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target))
            {
                return false;
            }

            FString LayerName;
            if (!(*ParamsObj)->TryGetStringField(TEXT("layerName"), LayerName) || LayerName.IsEmpty())
            {
                return false;
            }

            const TSharedPtr<FJsonObject>* CenterObj = nullptr;
            const TSharedPtr<FJsonObject>* SizeObj = nullptr;
            if (!(*ParamsObj)->TryGetObjectField(TEXT("center"), CenterObj) || !CenterObj || !CenterObj->IsValid())
            {
                return false;
            }
            if (!(*ParamsObj)->TryGetObjectField(TEXT("size"), SizeObj) || !SizeObj || !SizeObj->IsValid())
            {
                return false;
            }

            double CenterX = 0.0;
            double CenterY = 0.0;
            double SizeX = 0.0;
            double SizeY = 0.0;
            if (!(*CenterObj)->TryGetNumberField(TEXT("x"), CenterX) ||
                !(*CenterObj)->TryGetNumberField(TEXT("y"), CenterY) ||
                !(*SizeObj)->TryGetNumberField(TEXT("x"), SizeX) ||
                !(*SizeObj)->TryGetNumberField(TEXT("y"), SizeY))
            {
                return false;
            }

            double Strength = 0.0;
            if (!(*ParamsObj)->TryGetNumberField(TEXT("strength"), Strength))
            {
                return false;
            }

            double Falloff = 0.5;
            (*ParamsObj)->TryGetNumberField(TEXT("falloff"), Falloff);

            FString Mode = TEXT("add");
            (*ParamsObj)->TryGetStringField(TEXT("mode"), Mode);

            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::LandscapePaintLayer;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);
            ParsedAction.LandscapeLayerName = LayerName;
            ParsedAction.LandscapeCenter = FVector2D(static_cast<float>(CenterX), static_cast<float>(CenterY));
            ParsedAction.LandscapeSize = FVector2D(
                FMath::Abs(static_cast<float>(SizeX)),
                FMath::Abs(static_cast<float>(SizeY)));
            ParsedAction.LandscapeStrength = FMath::Clamp(static_cast<float>(Strength), 0.0f, 1.0f);
            ParsedAction.LandscapeFalloff = FMath::Clamp(static_cast<float>(Falloff), 0.0f, 1.0f);
            ParsedAction.bLandscapeInvertMode = Mode.Equals(TEXT("remove"), ESearchCase::IgnoreCase);
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

        if (Command == TEXT("landscape.generate"))
        {
            FString Target;
            const bool bHasTarget = (*ParamsObj)->TryGetStringField(TEXT("target"), Target);
            Target = Target.TrimStartAndEnd();
            if (!bHasTarget || Target.IsEmpty())
            {
                Target = TEXT("selection");
            }

            FString Theme;
            if (!(*ParamsObj)->TryGetStringField(TEXT("theme"), Theme) || Theme.IsEmpty())
            {
                return false;
            }
            const FString ThemeLower = Theme.TrimStartAndEnd().ToLower();
            const bool bMoonTheme =
                ThemeLower == TEXT("moon_surface") ||
                ThemeLower == TEXT("moon") ||
                ThemeLower == TEXT("lunar");
            const float ThemeDefaultMaxHeight = bMoonTheme ? 600.0f : 5000.0f;

            FString DetailLevel;
            (*ParamsObj)->TryGetStringField(TEXT("detailLevel"), DetailLevel);

            FString MoonProfile;
            (*ParamsObj)->TryGetStringField(TEXT("moonProfile"), MoonProfile);

            FString MountainStyle;
            (*ParamsObj)->TryGetStringField(TEXT("mountainStyle"), MountainStyle);

            bool bUseFullArea = true;
            (*ParamsObj)->TryGetBoolField(TEXT("useFullArea"), bUseFullArea);

            FVector2D Center = FVector2D::ZeroVector;
            FVector2D Size = FVector2D(1000.0f, 1000.0f);
            bool bHasBounds = false;

            const TSharedPtr<FJsonObject>* CenterObj = nullptr;
            const TSharedPtr<FJsonObject>* SizeObj = nullptr;
            const bool bHasCenter = (*ParamsObj)->TryGetObjectField(TEXT("center"), CenterObj) && CenterObj && CenterObj->IsValid();
            const bool bHasSize = (*ParamsObj)->TryGetObjectField(TEXT("size"), SizeObj) && SizeObj && SizeObj->IsValid();
            if (bHasCenter || bHasSize)
            {
                if (!bHasCenter || !bHasSize)
                {
                    return false;
                }

                double CenterX = 0.0;
                double CenterY = 0.0;
                double SizeX = 0.0;
                double SizeY = 0.0;
                if (!(*CenterObj)->TryGetNumberField(TEXT("x"), CenterX) ||
                    !(*CenterObj)->TryGetNumberField(TEXT("y"), CenterY) ||
                    !(*SizeObj)->TryGetNumberField(TEXT("x"), SizeX) ||
                    !(*SizeObj)->TryGetNumberField(TEXT("y"), SizeY))
                {
                    return false;
                }

                Center = FVector2D(static_cast<float>(CenterX), static_cast<float>(CenterY));
                Size = FVector2D(
                    FMath::Max(1.0f, FMath::Abs(static_cast<float>(SizeX))),
                    FMath::Max(1.0f, FMath::Abs(static_cast<float>(SizeY))));
                bHasBounds = true;
            }

            if (!bUseFullArea && !bHasBounds)
            {
                return false;
            }

            double SeedValue = 0.0;
            const bool bHasSeed = (*ParamsObj)->TryGetNumberField(TEXT("seed"), SeedValue);

            double MountainCountValue = 2.0;
            const bool bHasMountainCount = (*ParamsObj)->TryGetNumberField(TEXT("mountainCount"), MountainCountValue);

            double MountainWidthMinValue = 0.0;
            const bool bHasMountainWidthMin = (*ParamsObj)->TryGetNumberField(TEXT("mountainWidthMin"), MountainWidthMinValue);

            double MountainWidthMaxValue = 0.0;
            const bool bHasMountainWidthMax = (*ParamsObj)->TryGetNumberField(TEXT("mountainWidthMax"), MountainWidthMaxValue);

            double MaxHeightValue = static_cast<double>(ThemeDefaultMaxHeight);
            const bool bHasMaxHeight = (*ParamsObj)->TryGetNumberField(TEXT("maxHeight"), MaxHeightValue);

            double CraterCountMinValue = 0.0;
            const bool bHasCraterCountMin = (*ParamsObj)->TryGetNumberField(TEXT("craterCountMin"), CraterCountMinValue);

            double CraterCountMaxValue = 0.0;
            const bool bHasCraterCountMax = (*ParamsObj)->TryGetNumberField(TEXT("craterCountMax"), CraterCountMaxValue);

            double CraterWidthMinValue = 0.0;
            const bool bHasCraterWidthMin = (*ParamsObj)->TryGetNumberField(TEXT("craterWidthMin"), CraterWidthMinValue);

            double CraterWidthMaxValue = 0.0;
            const bool bHasCraterWidthMax = (*ParamsObj)->TryGetNumberField(TEXT("craterWidthMax"), CraterWidthMaxValue);

            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::LandscapeGenerate;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);
            ParsedAction.LandscapeTheme = ThemeLower;
            ParsedAction.LandscapeDetailLevel = DetailLevel.TrimStartAndEnd().ToLower();
            ParsedAction.LandscapeMoonProfile = MoonProfile.TrimStartAndEnd().ToLower();
            ParsedAction.LandscapeMountainStyle = MountainStyle.TrimStartAndEnd().ToLower();
            if (bMoonTheme && ParsedAction.LandscapeMoonProfile.IsEmpty())
            {
                ParsedAction.LandscapeMoonProfile = TEXT("moon_surface");
            }
            if (!bMoonTheme)
            {
                if (
                    !ParsedAction.LandscapeMountainStyle.Equals(TEXT("hills"), ESearchCase::IgnoreCase) &&
                    !ParsedAction.LandscapeMountainStyle.Equals(TEXT("sharp_peaks"), ESearchCase::IgnoreCase))
                {
                    ParsedAction.LandscapeMountainStyle = TEXT("sharp_peaks");
                }
            }
            else
            {
                ParsedAction.LandscapeMountainStyle.Empty();
            }
            ParsedAction.bLandscapeUseFullArea = bUseFullArea;
            ParsedAction.LandscapeCenter = Center;
            ParsedAction.LandscapeSize = Size;
            ParsedAction.LandscapeSeed = bHasSeed ? FMath::TruncToInt(static_cast<float>(SeedValue)) : 0;
            ParsedAction.LandscapeMountainCount = bHasMountainCount
                ? FMath::Clamp(FMath::RoundToInt(static_cast<float>(MountainCountValue)), 1, 8)
                : (bMoonTheme ? 2 : 0);
            ParsedAction.LandscapeMountainWidthMin = bHasMountainWidthMin
                ? FMath::Clamp(static_cast<float>(MountainWidthMinValue), 1.0f, 200000.0f)
                : 0.0f;
            ParsedAction.LandscapeMountainWidthMax = bHasMountainWidthMax
                ? FMath::Clamp(static_cast<float>(MountainWidthMaxValue), 1.0f, 200000.0f)
                : 0.0f;
            ParsedAction.LandscapeMaxHeight = bHasMaxHeight
                ? FMath::Clamp(static_cast<float>(MaxHeightValue), 100.0f, 20000.0f)
                : ThemeDefaultMaxHeight;
            ParsedAction.LandscapeCraterCountMin = bHasCraterCountMin
                ? FMath::Clamp(FMath::RoundToInt(static_cast<float>(CraterCountMinValue)), 1, 500)
                : 0;
            ParsedAction.LandscapeCraterCountMax = bHasCraterCountMax
                ? FMath::Clamp(FMath::RoundToInt(static_cast<float>(CraterCountMaxValue)), 1, 500)
                : 0;
            ParsedAction.LandscapeCraterWidthMin = bHasCraterWidthMin
                ? FMath::Clamp(static_cast<float>(CraterWidthMinValue), 1.0f, 200000.0f)
                : 0.0f;
            ParsedAction.LandscapeCraterWidthMax = bHasCraterWidthMax
                ? FMath::Clamp(static_cast<float>(CraterWidthMaxValue), 1.0f, 200000.0f)
                : 0.0f;
            if (
                ParsedAction.LandscapeMountainWidthMin > 0.0f &&
                ParsedAction.LandscapeMountainWidthMax > 0.0f &&
                ParsedAction.LandscapeMountainWidthMin > ParsedAction.LandscapeMountainWidthMax)
            {
                Swap(ParsedAction.LandscapeMountainWidthMin, ParsedAction.LandscapeMountainWidthMax);
            }
            if (
                ParsedAction.LandscapeCraterCountMin > 0 &&
                ParsedAction.LandscapeCraterCountMax > 0 &&
                ParsedAction.LandscapeCraterCountMin > ParsedAction.LandscapeCraterCountMax)
            {
                Swap(ParsedAction.LandscapeCraterCountMin, ParsedAction.LandscapeCraterCountMax);
            }
            if (
                ParsedAction.LandscapeCraterWidthMin > 0.0f &&
                ParsedAction.LandscapeCraterWidthMax > 0.0f &&
                ParsedAction.LandscapeCraterWidthMin > ParsedAction.LandscapeCraterWidthMax)
            {
                Swap(ParsedAction.LandscapeCraterWidthMin, ParsedAction.LandscapeCraterWidthMax);
            }
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
            else if (
                Target.Equals(TEXT("all"), ESearchCase::IgnoreCase) ||
                Target.Equals(TEXT("full"), ESearchCase::IgnoreCase) ||
                Target.Equals(TEXT("full_area"), ESearchCase::IgnoreCase))
            {
                ParsedAction.ActorNames.Empty();
            }
            else
            {
                return false;
            }

            OutAction = ParsedAction;
            return true;
        }

        if (Command == TEXT("pcg.createGraph"))
        {
            FString AssetPath;
            if (!(*ParamsObj)->TryGetStringField(TEXT("assetPath"), AssetPath) || AssetPath.IsEmpty())
            {
                return false;
            }

            bool bOverwrite = false;
            (*ParamsObj)->TryGetBoolField(TEXT("overwrite"), bOverwrite);

            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::PcgCreateGraph;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);
            ParsedAction.PcgGraphPath = AssetPath;
            (*ParamsObj)->TryGetStringField(TEXT("templatePath"), ParsedAction.PcgTemplatePath);
            ParsedAction.PcgTemplatePath = ParsedAction.PcgTemplatePath.TrimStartAndEnd();
            ParsedAction.bPcgOverwrite = bOverwrite;
            OutAction = ParsedAction;
            return true;
        }

        if (Command == TEXT("pcg.placeOnLandscape"))
        {
            FString Target;
            const bool bHasTarget = (*ParamsObj)->TryGetStringField(TEXT("target"), Target);
            Target = Target.TrimStartAndEnd();
            if (!bHasTarget || Target.IsEmpty())
            {
                Target = TEXT("selection");
            }

            FString GraphSource;
            (*ParamsObj)->TryGetStringField(TEXT("graphSource"), GraphSource);
            GraphSource = GraphSource.TrimStartAndEnd().ToLower();
            if (GraphSource.IsEmpty())
            {
                GraphSource = TEXT("last");
            }
            if (
                !GraphSource.Equals(TEXT("path"), ESearchCase::IgnoreCase) &&
                !GraphSource.Equals(TEXT("last"), ESearchCase::IgnoreCase) &&
                !GraphSource.Equals(TEXT("selected"), ESearchCase::IgnoreCase))
            {
                return false;
            }

            FString GraphPath;
            (*ParamsObj)->TryGetStringField(TEXT("graphPath"), GraphPath);
            GraphPath = GraphPath.TrimStartAndEnd();
            if (GraphSource.Equals(TEXT("path"), ESearchCase::IgnoreCase) && GraphPath.IsEmpty())
            {
                return false;
            }

            FString PlacementMode;
            (*ParamsObj)->TryGetStringField(TEXT("placementMode"), PlacementMode);
            PlacementMode = PlacementMode.TrimStartAndEnd().ToLower();
            if (PlacementMode.IsEmpty())
            {
                PlacementMode = TEXT("center");
            }
            if (!PlacementMode.Equals(TEXT("center"), ESearchCase::IgnoreCase) && !PlacementMode.Equals(TEXT("full"), ESearchCase::IgnoreCase))
            {
                return false;
            }

            FVector2D ParsedSize = FVector2D(3000.0f, 3000.0f);
            bool bHasSize = false;
            const TSharedPtr<FJsonObject>* SizeObj = nullptr;
            if ((*ParamsObj)->TryGetObjectField(TEXT("size"), SizeObj) && SizeObj && SizeObj->IsValid())
            {
                double SizeX = 0.0;
                double SizeY = 0.0;
                if (!(*SizeObj)->TryGetNumberField(TEXT("x"), SizeX) || !(*SizeObj)->TryGetNumberField(TEXT("y"), SizeY))
                {
                    return false;
                }

                ParsedSize = FVector2D(
                    FMath::Max(1.0f, FMath::Abs(static_cast<float>(SizeX))),
                    FMath::Max(1.0f, FMath::Abs(static_cast<float>(SizeY))));
                bHasSize = true;
            }

            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::PcgPlaceOnLandscape;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);
            ParsedAction.PcgGraphSource = GraphSource;
            ParsedAction.PcgGraphPath = GraphPath;
            ParsedAction.bPcgPlaceUseFullArea = PlacementMode.Equals(TEXT("full"), ESearchCase::IgnoreCase);
            ParsedAction.bPcgPlaceHasSize = bHasSize;
            ParsedAction.PcgPlaceSize = ParsedSize;
            ParsedAction.bPcgPlaceTargetAll =
                Target.Equals(TEXT("all"), ESearchCase::IgnoreCase) ||
                Target.Equals(TEXT("full"), ESearchCase::IgnoreCase) ||
                Target.Equals(TEXT("full_area"), ESearchCase::IgnoreCase);

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
            else if (!ParsedAction.bPcgPlaceTargetAll)
            {
                return false;
            }

            OutAction = ParsedAction;
            return true;
        }

        if (Command == TEXT("pcg.addConnectCommonNodes"))
        {
            FString GraphPath;
            if (!(*ParamsObj)->TryGetStringField(TEXT("graphPath"), GraphPath) || GraphPath.IsEmpty())
            {
                return false;
            }

            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::PcgAddConnectCommonNodes;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);
            ParsedAction.PcgGraphPath = GraphPath;
            ParsedAction.bPcgConnectFromInput = true;
            ParsedAction.bPcgConnectToOutput = true;
            (*ParamsObj)->TryGetBoolField(TEXT("connectFromInput"), ParsedAction.bPcgConnectFromInput);
            (*ParamsObj)->TryGetBoolField(TEXT("connectToOutput"), ParsedAction.bPcgConnectToOutput);

            const TArray<TSharedPtr<FJsonValue>>* NodeTypesArray = nullptr;
            if ((*ParamsObj)->TryGetArrayField(TEXT("nodeTypes"), NodeTypesArray) && NodeTypesArray)
            {
                for (const TSharedPtr<FJsonValue>& NodeTypeValue : *NodeTypesArray)
                {
                    FString NodeType;
                    if (NodeTypeValue.IsValid() && NodeTypeValue->TryGetString(NodeType))
                    {
                        NodeType = NodeType.TrimStartAndEnd().ToLower();
                        if (!NodeType.IsEmpty())
                        {
                            ParsedAction.PcgNodeTypes.AddUnique(NodeType);
                        }
                    }
                }
            }

            OutAction = ParsedAction;
            return true;
        }

        if (Command == TEXT("pcg.setKeyParameters"))
        {
            FString GraphPath;
            if (!(*ParamsObj)->TryGetStringField(TEXT("graphPath"), GraphPath) || GraphPath.IsEmpty())
            {
                return false;
            }

            FUEAIAgentPlannedSceneAction ParsedAction;
            ParsedAction.Type = EUEAIAgentPlannedActionType::PcgSetKeyParameters;
            ParsedAction.Risk = ParseRiskLevel(ActionObj);
            ParsedAction.PcgGraphPath = GraphPath;

            bool bHasAnyParameter = false;
            double NumberValue = 0.0;
            if ((*ParamsObj)->TryGetNumberField(TEXT("surfacePointsPerSquaredMeter"), NumberValue))
            {
                ParsedAction.PcgSurfacePointsPerSquaredMeter =
                    FMath::Clamp(static_cast<float>(NumberValue), 0.0001f, 1000.0f);
                ParsedAction.bPcgHasSurfacePointsPerSquaredMeter = true;
                bHasAnyParameter = true;
            }

            if ((*ParamsObj)->TryGetNumberField(TEXT("surfaceLooseness"), NumberValue))
            {
                ParsedAction.PcgSurfaceLooseness = FMath::Clamp(static_cast<float>(NumberValue), 0.0f, 1.0f);
                ParsedAction.bPcgHasSurfaceLooseness = true;
                bHasAnyParameter = true;
            }

            FVector ParsedVector = FVector::ZeroVector;
            if (ParseJsonVectorField(*ParamsObj, TEXT("surfacePointExtents"), ParsedVector))
            {
                ParsedAction.PcgSurfacePointExtents = FVector(
                    FMath::Max(0.001f, FMath::Abs(ParsedVector.X)),
                    FMath::Max(0.001f, FMath::Abs(ParsedVector.Y)),
                    FMath::Max(0.001f, FMath::Abs(ParsedVector.Z)));
                ParsedAction.bPcgHasSurfacePointExtents = true;
                bHasAnyParameter = true;
            }

            if (ParseJsonVectorField(*ParamsObj, TEXT("transformOffsetMin"), ParsedVector))
            {
                ParsedAction.PcgTransformOffsetMin = ParsedVector;
                ParsedAction.bPcgHasTransformOffsetMin = true;
                bHasAnyParameter = true;
            }

            if (ParseJsonVectorField(*ParamsObj, TEXT("transformOffsetMax"), ParsedVector))
            {
                ParsedAction.PcgTransformOffsetMax = ParsedVector;
                ParsedAction.bPcgHasTransformOffsetMax = true;
                bHasAnyParameter = true;
            }

            FRotator ParsedRotator = FRotator::ZeroRotator;
            if (ParseJsonRotatorField(*ParamsObj, TEXT("transformRotationMin"), ParsedRotator))
            {
                ParsedAction.PcgTransformRotationMin = ParsedRotator;
                ParsedAction.bPcgHasTransformRotationMin = true;
                bHasAnyParameter = true;
            }

            if (ParseJsonRotatorField(*ParamsObj, TEXT("transformRotationMax"), ParsedRotator))
            {
                ParsedAction.PcgTransformRotationMax = ParsedRotator;
                ParsedAction.bPcgHasTransformRotationMax = true;
                bHasAnyParameter = true;
            }

            if (ParseJsonVectorField(*ParamsObj, TEXT("transformScaleMin"), ParsedVector))
            {
                ParsedAction.PcgTransformScaleMin = FVector(
                    FMath::Max(0.001f, ParsedVector.X),
                    FMath::Max(0.001f, ParsedVector.Y),
                    FMath::Max(0.001f, ParsedVector.Z));
                ParsedAction.bPcgHasTransformScaleMin = true;
                bHasAnyParameter = true;
            }

            if (ParseJsonVectorField(*ParamsObj, TEXT("transformScaleMax"), ParsedVector))
            {
                ParsedAction.PcgTransformScaleMax = FVector(
                    FMath::Max(0.001f, ParsedVector.X),
                    FMath::Max(0.001f, ParsedVector.Y),
                    FMath::Max(0.001f, ParsedVector.Z));
                ParsedAction.bPcgHasTransformScaleMax = true;
                bHasAnyParameter = true;
            }

            if (!bHasAnyParameter)
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
    const FString RequestChatId = ActiveChatId;
    FUEAIAgentChatExecutionState& ChatState = AccessChatExecutionState(RequestChatId);
    ChatState.PlannedActions.Empty();
    ChatState.LastPlanSummary.Empty();
    ChatState.ActiveSessionId.Empty();
    ChatState.ActiveSessionActionIndex = INDEX_NONE;
    ChatState.ActiveSessionSelectedActors.Empty();

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
    if (!RequestChatId.IsEmpty())
    {
        Root->SetStringField(TEXT("chatId"), RequestChatId);
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
        [this, Callback, SelectedActors, RequestChatId](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [this, Callback, SelectedActors, RequestChatId, HttpResponse, bConnectedSuccessfully]()
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

                UpdateContextUsageFromResponse(ResponseJson, RequestChatId);
                FUEAIAgentChatExecutionState& RequestState = AccessChatExecutionState(RequestChatId);

                const TSharedPtr<FJsonObject>* PlanObj = nullptr;
                if (!ResponseJson->TryGetObjectField(TEXT("plan"), PlanObj) || !PlanObj || !PlanObj->IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Plan response misses 'plan' object."));
                    return;
                }

                FString Summary;
                (*PlanObj)->TryGetStringField(TEXT("summary"), Summary);
                RequestState.LastPlanSummary = Summary.TrimStartAndEnd();
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

                        FUEAIAgentPlannedSceneAction ParsedAction;
                        if (!ParsePlannedActionFromJson(ActionObj, SelectedActors, ParsedAction))
                        {
                            continue;
                        }

                        if (
                            ParsedAction.Type == EUEAIAgentPlannedActionType::ContextGetSceneSummary ||
                            ParsedAction.Type == EUEAIAgentPlannedActionType::ContextGetSelection)
                        {
                            ParsedAction.bApproved = true;
                        }
                        else if (ParsedAction.Type == EUEAIAgentPlannedActionType::DeleteActor)
                        {
                            ParsedAction.bApproved = false;
                        }
                        else
                        {
                            ParsedAction.bApproved = ParsedAction.Risk == EUEAIAgentRiskLevel::Low;
                        }

                        RequestState.PlannedActions.Add(ParsedAction);
                    }
                }

                FString AssistantText;
                ResponseJson->TryGetStringField(TEXT("assistantText"), AssistantText);

                FString FinalMessage;
                if (!AssistantText.IsEmpty())
                {
                    FinalMessage = AssistantText;
                }
                else if (RequestState.PlannedActions.Num() > 0)
                {
                    FinalMessage = RequestState.LastPlanSummary;
                    if (FinalMessage.IsEmpty())
                    {
                        FinalMessage = FString::Printf(TEXT("Needs approval: %d action(s)"), RequestState.PlannedActions.Num());
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
    const FString& ChatId,
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

    UpdateContextUsageFromResponse(ResponseJson, ChatId);

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

    FUEAIAgentChatExecutionState& ChatState = AccessChatExecutionState(ChatId);
    ChatState.ActiveSessionId = SessionId;
    ChatState.ActiveSessionActionIndex = INDEX_NONE;
    ChatState.PlannedActions.Empty();
    ChatState.ActiveSessionSelectedActors = SelectedActors;

    FString Status;
    (*DecisionObj)->TryGetStringField(TEXT("status"), Status);
    if (Status.IsEmpty())
    {
        (*DecisionObj)->TryGetStringField(TEXT("state"), Status);
    }
    FString Summary;
    (*DecisionObj)->TryGetStringField(TEXT("summary"), Summary);
    FString Message;
    (*DecisionObj)->TryGetStringField(TEXT("message"), Message);

    double ActionIndex = -1.0;
    const bool bHasNextActionIndex = (*DecisionObj)->TryGetNumberField(TEXT("nextActionIndex"), ActionIndex);
    if (!bHasNextActionIndex)
    {
        (*DecisionObj)->TryGetNumberField(TEXT("actionIndex"), ActionIndex);
    }
    ChatState.ActiveSessionActionIndex = ActionIndex >= 0.0 ? FMath::TruncToInt(ActionIndex) : INDEX_NONE;

    const TSharedPtr<FJsonObject>* NextActionObj = nullptr;
    bool bHasNextActionObj = (*DecisionObj)->TryGetObjectField(TEXT("nextAction"), NextActionObj) && NextActionObj && NextActionObj->IsValid();
    if (!bHasNextActionObj)
    {
        bHasNextActionObj = (*DecisionObj)->TryGetObjectField(TEXT("action"), NextActionObj) && NextActionObj && NextActionObj->IsValid();
    }

    bool bDerivedApproved = false;
    bool bHasDerivedApproved = false;
    if ((*DecisionObj)->TryGetBoolField(TEXT("nextActionApproved"), bDerivedApproved))
    {
        bHasDerivedApproved = true;
    }
    else if ((*DecisionObj)->TryGetBoolField(TEXT("approved"), bDerivedApproved))
    {
        bHasDerivedApproved = true;
    }

    if (Status.IsEmpty() && bHasNextActionObj)
    {
        Status = bHasDerivedApproved && !bDerivedApproved
            ? TEXT("awaiting_approval")
            : TEXT("ready_to_execute");
    }

    const bool bCanExecute = Status.Equals(TEXT("ready_to_execute"), ESearchCase::IgnoreCase) ||
        Status.Equals(TEXT("awaiting_approval"), ESearchCase::IgnoreCase);
    if (!bCanExecute)
    {
        ChatState.ActiveSessionActionIndex = INDEX_NONE;
    }

    if (bCanExecute)
    {
        if (bHasNextActionObj)
        {
            FUEAIAgentPlannedSceneAction ParsedAction;
            if (ParsePlannedActionFromJson(*NextActionObj, SelectedActors, ParsedAction))
            {
                bool bApproved = !Status.Equals(TEXT("awaiting_approval"), ESearchCase::IgnoreCase);
                if (!(*DecisionObj)->TryGetBoolField(TEXT("nextActionApproved"), bApproved))
                {
                    (*DecisionObj)->TryGetBoolField(TEXT("approved"), bApproved);
                }
                ParsedAction.bApproved = bApproved;

                FString ActionStateText;
                if (
                    (*DecisionObj)->TryGetStringField(TEXT("nextActionState"), ActionStateText) ||
                    (*DecisionObj)->TryGetStringField(TEXT("actionState"), ActionStateText))
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
                if (
                    (*DecisionObj)->TryGetNumberField(TEXT("nextActionAttempts"), Attempts) ||
                    (*DecisionObj)->TryGetNumberField(TEXT("attempts"), Attempts))
                {
                    ParsedAction.AttemptCount = FMath::Max(0, FMath::RoundToInt(static_cast<float>(Attempts)));
                }
                ChatState.PlannedActions.Add(ParsedAction);
            }
        }
    }

    OutMessage = FString::Printf(
        TEXT("Session: %s\n%s\n%s"),
        Status.IsEmpty() ? TEXT("unknown") : *Status,
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
    const FString RequestChatId = ActiveChatId;
    FUEAIAgentChatExecutionState& ChatState = AccessChatExecutionState(RequestChatId);
    ChatState.PlannedActions.Empty();
    ChatState.LastPlanSummary.Empty();
    ChatState.ActiveSessionId.Empty();
    ChatState.ActiveSessionActionIndex = INDEX_NONE;
    ChatState.ActiveSessionSelectedActors = SelectedActors;

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
    if (!RequestChatId.IsEmpty())
    {
        Root->SetStringField(TEXT("chatId"), RequestChatId);
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
        [this, Callback, SelectedActors, RequestChatId](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [this, Callback, SelectedActors, RequestChatId, HttpResponse, bConnectedSuccessfully]()
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
                const bool bParsed = ParseSessionDecision(ResponseJson, RequestChatId, SelectedActors, ParsedMessage);
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
    const FString RequestChatId = ActiveChatId;
    FUEAIAgentChatExecutionState& ChatState = AccessChatExecutionState(RequestChatId);
    if (ChatState.ActiveSessionId.IsEmpty())
    {
        Callback.ExecuteIfBound(false, TEXT("No active session."));
        return;
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("sessionId"), ChatState.ActiveSessionId);
    if (!RequestChatId.IsEmpty())
    {
        Root->SetStringField(TEXT("chatId"), RequestChatId);
    }

    if (bHasResult)
    {
        if (ChatState.ActiveSessionActionIndex == INDEX_NONE)
        {
            Callback.ExecuteIfBound(false, TEXT("No active session action index."));
            return;
        }

        const int32 CurrentAttempts = GetPlannedActionAttemptCount(ChatState.ActiveSessionActionIndex);
        UpdateActionResult(ChatState.ActiveSessionActionIndex, bResultOk, CurrentAttempts + 1);

        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetNumberField(TEXT("actionIndex"), ChatState.ActiveSessionActionIndex);
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
        [this, Callback, RequestChatId](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [this, Callback, RequestChatId, HttpResponse, bConnectedSuccessfully]()
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
                const FUEAIAgentChatExecutionState& ResponseState = AccessChatExecutionState(RequestChatId);
                const bool bParsed = ParseSessionDecision(
                    ResponseJson,
                    RequestChatId,
                    ResponseState.ActiveSessionSelectedActors,
                    ParsedMessage);
                Callback.ExecuteIfBound(bParsed, ParsedMessage);
            });
        });

    Request->ProcessRequest();
}

void FUEAIAgentTransportModule::ApproveCurrentSessionAction(
    bool bApproved,
    const FOnUEAIAgentSessionUpdated& Callback) const
{
    const FString RequestChatId = ActiveChatId;
    const FUEAIAgentChatExecutionState& ChatState = AccessChatExecutionState(RequestChatId);
    if (ChatState.ActiveSessionId.IsEmpty() || ChatState.ActiveSessionActionIndex == INDEX_NONE)
    {
        Callback.ExecuteIfBound(false, TEXT("No active session action to approve."));
        return;
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("sessionId"), ChatState.ActiveSessionId);
    Root->SetNumberField(TEXT("actionIndex"), ChatState.ActiveSessionActionIndex);
    Root->SetBoolField(TEXT("approved"), bApproved);
    if (!RequestChatId.IsEmpty())
    {
        Root->SetStringField(TEXT("chatId"), RequestChatId);
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
        [this, Callback, RequestChatId](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [this, Callback, RequestChatId, HttpResponse, bConnectedSuccessfully]()
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
                const FUEAIAgentChatExecutionState& ResponseState = AccessChatExecutionState(RequestChatId);
                const bool bParsed = ParseSessionDecision(
                    ResponseJson,
                    RequestChatId,
                    ResponseState.ActiveSessionSelectedActors,
                    ParsedMessage);
                Callback.ExecuteIfBound(bParsed, ParsedMessage);
            });
        });

    Request->ProcessRequest();
}

void FUEAIAgentTransportModule::ResumeSession(const FOnUEAIAgentSessionUpdated& Callback) const
{
    const FString RequestChatId = ActiveChatId;
    const FUEAIAgentChatExecutionState& ChatState = AccessChatExecutionState(RequestChatId);
    if (ChatState.ActiveSessionId.IsEmpty())
    {
        Callback.ExecuteIfBound(false, TEXT("No active session."));
        return;
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("sessionId"), ChatState.ActiveSessionId);
    if (!RequestChatId.IsEmpty())
    {
        Root->SetStringField(TEXT("chatId"), RequestChatId);
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
        [this, Callback, RequestChatId](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [this, Callback, RequestChatId, HttpResponse, bConnectedSuccessfully]()
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
                const FUEAIAgentChatExecutionState& ResponseState = AccessChatExecutionState(RequestChatId);
                const bool bParsed = ParseSessionDecision(
                    ResponseJson,
                    RequestChatId,
                    ResponseState.ActiveSessionSelectedActors,
                    ParsedMessage);
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
                RemoveChatExecutionState(ChatId);
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

    const FString RequestChatId = ActiveChatId;
    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildChatHistoryUrl(RequestChatId, Limit));
    Request->SetVerb(TEXT("GET"));
    Request->OnProcessRequestComplete().BindLambda(
        [this, Callback, RequestChatId](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [this, Callback, RequestChatId, HttpResponse, bConnectedSuccessfully]()
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

                UpdateContextUsageFromResponse(ResponseJson, RequestChatId);
                if (ActiveChatId != RequestChatId)
                {
                    Callback.ExecuteIfBound(true, TEXT("Ignored inactive chat history response."));
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

FString FUEAIAgentTransportModule::GetLastContextUsageLabel() const
{
    if (const FUEAIAgentChatExecutionState* ChatState = FindActiveChatExecutionState())
    {
        return ChatState->LastContextUsageLabel;
    }
    return TEXT("");
}

FString FUEAIAgentTransportModule::GetLastContextUsageTooltip() const
{
    if (const FUEAIAgentChatExecutionState* ChatState = FindActiveChatExecutionState())
    {
        return ChatState->LastContextUsageTooltip;
    }
    return TEXT("");
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
    if (const FUEAIAgentChatExecutionState* ChatState = FindActiveChatExecutionState())
    {
        return ChatState->LastPlanSummary;
    }
    return TEXT("");
}

int32 FUEAIAgentTransportModule::GetPlannedActionCount() const
{
    if (const FUEAIAgentChatExecutionState* ChatState = FindActiveChatExecutionState())
    {
        return ChatState->PlannedActions.Num();
    }
    return 0;
}

FString FUEAIAgentTransportModule::GetPlannedActionPreviewText(int32 ActionIndex) const
{
    const FUEAIAgentChatExecutionState* ChatState = FindActiveChatExecutionState();
    if (!ChatState || !ChatState->PlannedActions.IsValidIndex(ActionIndex))
    {
        return TEXT("Invalid action index.");
    }

    const FUEAIAgentPlannedSceneAction& Action = ChatState->PlannedActions[ActionIndex];
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

    if (Action.Type == EUEAIAgentPlannedActionType::EditorUndo)
    {
        return FString::Printf(
            TEXT("Action %d: Undo last editor action"),
            ActionIndex + 1);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::EditorRedo)
    {
        return FString::Printf(
            TEXT("Action %d: Redo last editor action"),
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

    if (Action.Type == EUEAIAgentPlannedActionType::LandscapeSculpt)
    {
        return FString::Printf(
            TEXT("Action %d: Sculpt landscape (%s) center=(%.0f, %.0f), size=(%.0f, %.0f), strength=%.2f"),
            ActionIndex + 1,
            Action.bLandscapeInvertMode ? TEXT("lower") : TEXT("raise"),
            Action.LandscapeCenter.X,
            Action.LandscapeCenter.Y,
            Action.LandscapeSize.X,
            Action.LandscapeSize.Y,
            Action.LandscapeStrength);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::LandscapePaintLayer)
    {
        return FString::Printf(
            TEXT("Action %d: Paint landscape layer \"%s\" (%s) center=(%.0f, %.0f), size=(%.0f, %.0f), strength=%.2f"),
            ActionIndex + 1,
            *Action.LandscapeLayerName,
            Action.bLandscapeInvertMode ? TEXT("remove") : TEXT("add"),
            Action.LandscapeCenter.X,
            Action.LandscapeCenter.Y,
            Action.LandscapeSize.X,
            Action.LandscapeSize.Y,
            Action.LandscapeStrength);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::LandscapeGenerate)
    {
        const FString ThemeDisplayText = Action.LandscapeTheme.Replace(TEXT("_"), TEXT(" "));
        const bool bMoonTheme =
            Action.LandscapeTheme.Equals(TEXT("moon_surface"), ESearchCase::IgnoreCase) ||
            Action.LandscapeTheme.Equals(TEXT("moon"), ESearchCase::IgnoreCase) ||
            Action.LandscapeTheme.Equals(TEXT("lunar"), ESearchCase::IgnoreCase);
        const FString AreaText = Action.bLandscapeUseFullArea
            ? TEXT("full landscape")
            : FString::Printf(TEXT("center=(%.0f, %.0f), size=(%.0f, %.0f)"),
                Action.LandscapeCenter.X,
                Action.LandscapeCenter.Y,
                Action.LandscapeSize.X,
                Action.LandscapeSize.Y);
        const FString SeedText = Action.LandscapeSeed == 0
            ? TEXT("auto")
            : FString::FromInt(Action.LandscapeSeed);
        const FString DetailText = Action.LandscapeDetailLevel.IsEmpty()
            ? TEXT("auto")
            : Action.LandscapeDetailLevel;
        const FString ProfileText = Action.LandscapeMoonProfile.IsEmpty()
            ? TEXT("auto")
            : Action.LandscapeMoonProfile;
        const FString MountainWidthText =
            Action.LandscapeMountainWidthMin > 0.0f || Action.LandscapeMountainWidthMax > 0.0f
                ? FString::Printf(TEXT("%.0f-%.0f"),
                    Action.LandscapeMountainWidthMin > 0.0f ? Action.LandscapeMountainWidthMin : 1.0f,
                    Action.LandscapeMountainWidthMax > 0.0f ? Action.LandscapeMountainWidthMax : 200000.0f)
                : TEXT("auto");
        const FString MountainCountText = Action.LandscapeMountainCount > 0
            ? FString::FromInt(Action.LandscapeMountainCount)
            : TEXT("1-3(auto)");
        const FString MountainStyleText =
            Action.LandscapeMountainStyle.IsEmpty() ? TEXT("sharp_peaks") : Action.LandscapeMountainStyle;
        const FString CraterCountText =
            Action.LandscapeCraterCountMin > 0 || Action.LandscapeCraterCountMax > 0
                ? FString::Printf(TEXT("%d-%d"),
                    Action.LandscapeCraterCountMin > 0 ? Action.LandscapeCraterCountMin : 1,
                    Action.LandscapeCraterCountMax > 0 ? Action.LandscapeCraterCountMax : 500)
                : TEXT("auto");
        const FString CraterWidthText =
            Action.LandscapeCraterWidthMin > 0.0f || Action.LandscapeCraterWidthMax > 0.0f
                ? FString::Printf(TEXT("%.0f-%.0f"),
                    Action.LandscapeCraterWidthMin > 0.0f ? Action.LandscapeCraterWidthMin : 1.0f,
                    Action.LandscapeCraterWidthMax > 0.0f ? Action.LandscapeCraterWidthMax : 200000.0f)
                : TEXT("auto");
        if (bMoonTheme)
        {
            return FString::Printf(
                TEXT("Action %d: Generate %s (%s), detail=%s, profile=%s, maxHeight=%.0f, craterDensity=%d, craters=%s, craterWidth=%s, seed=%s"),
                ActionIndex + 1,
                *ThemeDisplayText,
                *AreaText,
                *DetailText,
                *ProfileText,
                Action.LandscapeMaxHeight,
                Action.LandscapeMountainCount,
                *CraterCountText,
                *CraterWidthText,
                *SeedText);
        }

        return FString::Printf(
            TEXT("Action %d: Generate %s (%s), detail=%s, maxHeight=%.0f, mountains=%s, mountainStyle=%s, mountainWidth=%s, seed=%s"),
            ActionIndex + 1,
            *ThemeDisplayText,
            *AreaText,
            *DetailText,
            Action.LandscapeMaxHeight,
            *MountainCountText,
            *MountainStyleText,
            *MountainWidthText,
            *SeedText);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::PcgCreateGraph)
    {
        const FString TemplateSuffix = Action.PcgTemplatePath.IsEmpty()
            ? TEXT("")
            : FString::Printf(TEXT(" from template \"%s\""), *Action.PcgTemplatePath);
        return FString::Printf(
            TEXT("Action %d: Create PCG graph \"%s\"%s%s"),
            ActionIndex + 1,
            *Action.PcgGraphPath,
            *TemplateSuffix,
            Action.bPcgOverwrite ? TEXT(" (overwrite)") : TEXT(""));
    }

    if (Action.Type == EUEAIAgentPlannedActionType::PcgPlaceOnLandscape)
    {
        const FString SourceText = Action.PcgGraphSource.Equals(TEXT("path"), ESearchCase::IgnoreCase)
            ? FString::Printf(TEXT("path \"%s\""), *Action.PcgGraphPath)
            : Action.PcgGraphSource.Equals(TEXT("selected"), ESearchCase::IgnoreCase)
            ? TEXT("selected graph")
            : TEXT("last graph");
        const FString AreaText = Action.bPcgPlaceUseFullArea
            ? TEXT("full landscape area")
            : Action.bPcgPlaceHasSize
            ? FString::Printf(TEXT("landscape center, size=(%.0f, %.0f)"), Action.PcgPlaceSize.X, Action.PcgPlaceSize.Y)
            : TEXT("landscape center");
        const FString LandscapeTargetText = Action.bPcgPlaceTargetAll
            ? TEXT("all landscapes")
            : FormatActorTargetShort(Action.ActorNames);
        return FString::Printf(
            TEXT("Action %d: Place PCG from %s on %s for %s"),
            ActionIndex + 1,
            *SourceText,
            *AreaText,
            *LandscapeTargetText);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::PcgAddConnectCommonNodes)
    {
        const FString NodeList = Action.PcgNodeTypes.Num() > 0
            ? FString::Join(Action.PcgNodeTypes, TEXT(", "))
            : TEXT("surfaceSampler, transformPoints");
        return FString::Printf(
            TEXT("Action %d: Add/connect PCG nodes (%s) in \"%s\""),
            ActionIndex + 1,
            *NodeList,
            *Action.PcgGraphPath);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::PcgSetKeyParameters)
    {
        TArray<FString> Parts;
        if (Action.bPcgHasSurfacePointsPerSquaredMeter)
        {
            Parts.Add(FString::Printf(TEXT("pointsPerSqM=%.3f"), Action.PcgSurfacePointsPerSquaredMeter));
        }
        if (Action.bPcgHasSurfaceLooseness)
        {
            Parts.Add(FString::Printf(TEXT("looseness=%.3f"), Action.PcgSurfaceLooseness));
        }
        if (Action.bPcgHasSurfacePointExtents)
        {
            Parts.Add(FString::Printf(
                TEXT("pointExtents=(%.1f, %.1f, %.1f)"),
                Action.PcgSurfacePointExtents.X,
                Action.PcgSurfacePointExtents.Y,
                Action.PcgSurfacePointExtents.Z));
        }
        if (Action.bPcgHasTransformOffsetMin || Action.bPcgHasTransformOffsetMax)
        {
            Parts.Add(TEXT("offset range"));
        }
        if (Action.bPcgHasTransformRotationMin || Action.bPcgHasTransformRotationMax)
        {
            Parts.Add(TEXT("rotation range"));
        }
        if (Action.bPcgHasTransformScaleMin || Action.bPcgHasTransformScaleMax)
        {
            Parts.Add(TEXT("scale range"));
        }

        const FString ChangeText = Parts.Num() > 0 ? FString::Join(Parts, TEXT(", ")) : TEXT("key params");
        return FString::Printf(
            TEXT("Action %d: Set PCG parameters in \"%s\" (%s)"),
            ActionIndex + 1,
            *Action.PcgGraphPath,
            *ChangeText);
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
    const FUEAIAgentChatExecutionState* ChatState = FindActiveChatExecutionState();
    if (!ChatState || !ChatState->PlannedActions.IsValidIndex(ActionIndex))
    {
        return false;
    }

    return ChatState->PlannedActions[ActionIndex].bApproved;
}

int32 FUEAIAgentTransportModule::GetPlannedActionAttemptCount(int32 ActionIndex) const
{
    const FUEAIAgentChatExecutionState* ChatState = FindActiveChatExecutionState();
    if (!ChatState || !ChatState->PlannedActions.IsValidIndex(ActionIndex))
    {
        return 0;
    }

    return ChatState->PlannedActions[ActionIndex].AttemptCount;
}

void FUEAIAgentTransportModule::SetPlannedActionApproved(int32 ActionIndex, bool bApproved) const
{
    FUEAIAgentChatExecutionState& ChatState = AccessActiveChatExecutionState();
    if (!ChatState.PlannedActions.IsValidIndex(ActionIndex))
    {
        return;
    }

    ChatState.PlannedActions[ActionIndex].bApproved = bApproved;
}

bool FUEAIAgentTransportModule::PopApprovedPlannedActions(TArray<FUEAIAgentPlannedSceneAction>& OutActions) const
{
    FUEAIAgentChatExecutionState& ChatState = AccessActiveChatExecutionState();
    OutActions.Empty();
    for (const FUEAIAgentPlannedSceneAction& Action : ChatState.PlannedActions)
    {
        if (Action.bApproved)
        {
            OutActions.Add(Action);
        }
    }

    ChatState.PlannedActions.Empty();
    return OutActions.Num() > 0;
}

void FUEAIAgentTransportModule::ClearPlannedActions() const
{
    AccessActiveChatExecutionState().PlannedActions.Empty();
}

bool FUEAIAgentTransportModule::GetPlannedAction(int32 ActionIndex, FUEAIAgentPlannedSceneAction& OutAction) const
{
    const FUEAIAgentChatExecutionState* ChatState = FindActiveChatExecutionState();
    if (!ChatState || !ChatState->PlannedActions.IsValidIndex(ActionIndex))
    {
        return false;
    }

    OutAction = ChatState->PlannedActions[ActionIndex];
    return true;
}

bool FUEAIAgentTransportModule::GetPendingAction(int32 ActionIndex, FUEAIAgentPlannedSceneAction& OutAction) const
{
    const FUEAIAgentChatExecutionState* ChatState = FindActiveChatExecutionState();
    if (!ChatState || !ChatState->PlannedActions.IsValidIndex(ActionIndex))
    {
        return false;
    }

    const FUEAIAgentPlannedSceneAction& Action = ChatState->PlannedActions[ActionIndex];
    if (Action.State != EUEAIAgentActionState::Pending)
    {
        return false;
    }

    OutAction = Action;
    return true;
}

void FUEAIAgentTransportModule::UpdateActionResult(int32 ActionIndex, bool bSucceeded, int32 AttemptCount) const
{
    FUEAIAgentChatExecutionState& ChatState = AccessActiveChatExecutionState();
    if (!ChatState.PlannedActions.IsValidIndex(ActionIndex))
    {
        return;
    }

    ChatState.PlannedActions[ActionIndex].State = bSucceeded ? EUEAIAgentActionState::Succeeded : EUEAIAgentActionState::Failed;
    ChatState.PlannedActions[ActionIndex].AttemptCount = FMath::Max(0, AttemptCount);
}

int32 FUEAIAgentTransportModule::GetNextPendingActionIndex() const
{
    const FUEAIAgentChatExecutionState* ChatState = FindActiveChatExecutionState();
    if (!ChatState)
    {
        return INDEX_NONE;
    }

    for (int32 Index = 0; Index < ChatState->PlannedActions.Num(); ++Index)
    {
        if (ChatState->PlannedActions[Index].State == EUEAIAgentActionState::Pending)
        {
            return Index;
        }
    }
    return INDEX_NONE;
}

bool FUEAIAgentTransportModule::HasActiveSession() const
{
    if (const FUEAIAgentChatExecutionState* ChatState = FindActiveChatExecutionState())
    {
        return !ChatState->ActiveSessionId.IsEmpty();
    }
    return false;
}

FString FUEAIAgentTransportModule::ResolveChatStateKey(const FString& ChatId) const
{
    const FString Normalized = ChatId.TrimStartAndEnd();
    return Normalized.IsEmpty() ? GlobalChatStateKey : Normalized;
}

FUEAIAgentTransportModule::FUEAIAgentChatExecutionState& FUEAIAgentTransportModule::AccessChatExecutionState(const FString& ChatId) const
{
    const FString Key = ResolveChatStateKey(ChatId);
    if (FUEAIAgentChatExecutionState* Existing = ChatExecutionStates.Find(Key))
    {
        return *Existing;
    }
    return ChatExecutionStates.Add(Key, FUEAIAgentChatExecutionState());
}

FUEAIAgentTransportModule::FUEAIAgentChatExecutionState& FUEAIAgentTransportModule::AccessActiveChatExecutionState() const
{
    return AccessChatExecutionState(ActiveChatId);
}

const FUEAIAgentTransportModule::FUEAIAgentChatExecutionState* FUEAIAgentTransportModule::FindActiveChatExecutionState() const
{
    return ChatExecutionStates.Find(ResolveChatStateKey(ActiveChatId));
}

void FUEAIAgentTransportModule::RemoveChatExecutionState(const FString& ChatId) const
{
    ChatExecutionStates.Remove(ResolveChatStateKey(ChatId));
}

void FUEAIAgentTransportModule::UpdateContextUsageFromResponse(
    const TSharedPtr<FJsonObject>& ResponseJson,
    const FString& ChatId) const
{
    FUEAIAgentChatExecutionState& ChatState = AccessChatExecutionState(ChatId);
    ChatState.LastContextUsageLabel.Reset();
    ChatState.LastContextUsageTooltip.Reset();
    if (!ResponseJson.IsValid())
    {
        return;
    }

    const TSharedPtr<FJsonObject>* ContextUsageObj = nullptr;
    if (!ResponseJson->TryGetObjectField(TEXT("contextUsage"), ContextUsageObj) || !ContextUsageObj || !ContextUsageObj->IsValid())
    {
        return;
    }

    const FContextUsageDisplay Display = BuildContextUsageDisplay(*ContextUsageObj);
    ChatState.LastContextUsageLabel = Display.Label;
    ChatState.LastContextUsageTooltip = Display.Tooltip;
}

IMPLEMENT_MODULE(FUEAIAgentTransportModule, UEAIAgentTransport)
