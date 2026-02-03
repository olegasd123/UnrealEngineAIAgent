#include "SUEAIAgentPanel.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "GameFramework/Actor.h"
#include "UEAIAgentSceneTools.h"
#include "UEAIAgentTransportModule.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

void SUEAIAgentPanel::Construct(const FArguments& InArgs)
{
    ChildSlot
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(8.0f)
        [
            SAssignNew(StatusText, STextBlock)
            .Text(FText::FromString(TEXT("Status: not checked")))
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(8.0f, 0.0f, 8.0f, 8.0f)
        [
            SAssignNew(PromptInput, SEditableTextBox)
            .HintText(FText::FromString(TEXT("Describe what to do with selected actors")))
            .Text(FText::FromString(TEXT("Move selected actors +250 on X")))
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(8.0f, 0.0f, 8.0f, 8.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(0.0f, 0.0f, 8.0f, 0.0f)
            [
                SNew(SBox)
                .WidthOverride(180.0f)
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Check Local Agent")))
                    .OnClicked(this, &SUEAIAgentPanel::OnCheckHealthClicked)
                ]
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(0.0f, 0.0f, 8.0f, 0.0f)
            [
                SNew(SBox)
                .WidthOverride(220.0f)
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Plan With Selection")))
                    .OnClicked(this, &SUEAIAgentPanel::OnPlanFromSelectionClicked)
                ]
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            [
                SNew(SBox)
                .WidthOverride(240.0f)
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Apply Planned Action")))
                    .OnClicked(this, &SUEAIAgentPanel::OnApplyPlannedActionClicked)
                ]
            ]
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(8.0f, 0.0f, 8.0f, 8.0f)
        [
            SAssignNew(PlanText, STextBlock)
            .AutoWrapText(true)
            .Text(FText::FromString(TEXT("Plan: not requested")))
        ]
    ];
}

FReply SUEAIAgentPanel::OnCheckHealthClicked()
{
    if (StatusText.IsValid())
    {
        StatusText->SetText(FText::FromString(TEXT("Status: checking...")));
    }

    FUEAIAgentTransportModule::Get().CheckHealth(FOnUEAIAgentHealthChecked::CreateSP(
        this,
        &SUEAIAgentPanel::HandleHealthResult));

    return FReply::Handled();
}

FReply SUEAIAgentPanel::OnPlanFromSelectionClicked()
{
    if (!PromptInput.IsValid() || !PlanText.IsValid())
    {
        return FReply::Handled();
    }

    const FString Prompt = PromptInput->GetText().ToString().TrimStartAndEnd();
    if (Prompt.IsEmpty())
    {
        PlanText->SetText(FText::FromString(TEXT("Plan: please enter a prompt first.")));
        return FReply::Handled();
    }

    const TArray<FString> SelectedActors = CollectSelectedActorNames();
    PlanText->SetText(FText::FromString(TEXT("Plan: requesting...")));

    FUEAIAgentTransportModule::Get().PlanTask(
        Prompt,
        SelectedActors,
        FOnUEAIAgentTaskPlanned::CreateSP(this, &SUEAIAgentPanel::HandlePlanResult));

    return FReply::Handled();
}

FReply SUEAIAgentPanel::OnApplyPlannedActionClicked()
{
    if (!PlanText.IsValid())
    {
        return FReply::Handled();
    }

    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    if (!Transport.HasPlannedMoveAction())
    {
        PlanText->SetText(FText::FromString(TEXT("Execute: error\nNo planned executable action. Use 'Plan With Selection' first.")));
        return FReply::Handled();
    }

    FUEAIAgentModifyActorParams Params;
    if (!Transport.PopPlannedMoveAction(Params.ActorNames, Params.DeltaLocation))
    {
        PlanText->SetText(FText::FromString(TEXT("Execute: error\nCould not read planned action.")));
        return FReply::Handled();
    }

    if (Params.ActorNames.IsEmpty())
    {
        PlanText->SetText(FText::FromString(TEXT("Execute: error\nPlan has no target actors. Select actors and plan again.")));
        return FReply::Handled();
    }

    Params.bUseSelectionIfActorNamesEmpty = false;

    FString ResultMessage;
    const bool bOk = FUEAIAgentSceneTools::SceneModifyActor(Params, ResultMessage);
    const FString Prefix = bOk ? TEXT("Execute: ok\n") : TEXT("Execute: error\n");
    PlanText->SetText(FText::FromString(Prefix + ResultMessage));

    return FReply::Handled();
}

void SUEAIAgentPanel::HandleHealthResult(bool bOk, const FString& Message)
{
    if (!StatusText.IsValid())
    {
        return;
    }

    const FString Prefix = bOk ? TEXT("Status: ok - ") : TEXT("Status: error - ");
    StatusText->SetText(FText::FromString(Prefix + Message));
}

void SUEAIAgentPanel::HandlePlanResult(bool bOk, const FString& Message)
{
    if (!PlanText.IsValid())
    {
        return;
    }

    if (!bOk)
    {
        PlanText->SetText(FText::FromString(TEXT("Plan: error\n") + Message));
        return;
    }

    const FString PreviewText = FUEAIAgentTransportModule::Get().GetPlannedMovePreviewText();
    PlanText->SetText(FText::FromString(TEXT("Plan: ok\n") + Message + TEXT("\n") + PreviewText));
}

TArray<FString> SUEAIAgentPanel::CollectSelectedActorNames() const
{
    TArray<FString> Names;
    if (!GEditor)
    {
        return Names;
    }

    for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
    {
        const AActor* Actor = Cast<AActor>(*It);
        if (Actor)
        {
            Names.Add(Actor->GetName());
        }
    }

    return Names;
}
