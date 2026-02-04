#include "SUEAIAgentPanel.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "GameFramework/Actor.h"
#include "UEAIAgentSceneTools.h"
#include "UEAIAgentTransportModule.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
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
                    .Text(FText::FromString(TEXT("Apply Planned Actions")))
                    .OnClicked(this, &SUEAIAgentPanel::OnApplyPlannedActionClicked)
                ]
            ]
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(8.0f, 0.0f, 8.0f, 8.0f)
        [
            SAssignNew(ActionListBox, SVerticalBox)
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

    UpdateActionApprovalUi();
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
    if (Transport.GetPlannedActionCount() == 0)
    {
        PlanText->SetText(FText::FromString(TEXT("Execute: error\nNo planned actions. Use 'Plan With Selection' first.")));
        return FReply::Handled();
    }

    TArray<FUEAIAgentPlannedSceneAction> ApprovedActions;
    if (!Transport.PopApprovedPlannedActions(ApprovedActions))
    {
        PlanText->SetText(FText::FromString(TEXT("Execute: error\nNo approved actions to execute.")));
        UpdateActionApprovalUi();
        return FReply::Handled();
    }

    FString ExecuteSummary;
    int32 SuccessCount = 0;
    for (const FUEAIAgentPlannedSceneAction& PlannedAction : ApprovedActions)
    {
        FString ResultMessage;
        bool bOk = false;

        if (PlannedAction.Type == EUEAIAgentPlannedActionType::CreateActor)
        {
            FUEAIAgentCreateActorParams Params;
            Params.ActorClass = PlannedAction.ActorClass;
            Params.Location = PlannedAction.SpawnLocation;
            Params.Rotation = PlannedAction.SpawnRotation;
            Params.Count = PlannedAction.SpawnCount;
            bOk = FUEAIAgentSceneTools::SceneCreateActor(Params, ResultMessage);
        }
        else if (PlannedAction.Type == EUEAIAgentPlannedActionType::DeleteActor)
        {
            FUEAIAgentDeleteActorParams Params;
            Params.ActorNames = PlannedAction.ActorNames;
            Params.bUseSelectionIfActorNamesEmpty = false;
            if (Params.ActorNames.IsEmpty())
            {
                ExecuteSummary += TEXT("- Skipped delete action with no target actors.\n");
                continue;
            }
            bOk = FUEAIAgentSceneTools::SceneDeleteActor(Params, ResultMessage);
        }
        else
        {
            FUEAIAgentModifyActorParams Params;
            Params.ActorNames = PlannedAction.ActorNames;
            Params.DeltaLocation = PlannedAction.DeltaLocation;
            Params.DeltaRotation = PlannedAction.DeltaRotation;
            Params.bUseSelectionIfActorNamesEmpty = false;
            if (Params.ActorNames.IsEmpty())
            {
                ExecuteSummary += TEXT("- Skipped modify action with no target actors.\n");
                continue;
            }
            bOk = FUEAIAgentSceneTools::SceneModifyActor(Params, ResultMessage);
        }

        if (bOk)
        {
            ++SuccessCount;
        }

        ExecuteSummary += FString::Printf(TEXT("- %s\n"), *ResultMessage);
    }

    UpdateActionApprovalUi();
    const FString Prefix = SuccessCount > 0 ? TEXT("Execute: ok\n") : TEXT("Execute: error\n");
    PlanText->SetText(FText::FromString(Prefix + ExecuteSummary));

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

    UpdateActionApprovalUi();

    FString PreviewSummary;
    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    const int32 ActionCount = Transport.GetPlannedActionCount();
    for (int32 ActionIndex = 0; ActionIndex < ActionCount; ++ActionIndex)
    {
        PreviewSummary += Transport.GetPlannedActionPreviewText(ActionIndex) + TEXT("\n");
    }

    if (PreviewSummary.IsEmpty())
    {
        PreviewSummary = TEXT("No executable actions were parsed.");
    }

    PlanText->SetText(FText::FromString(TEXT("Plan: ok\n") + Message + TEXT("\n") + PreviewSummary));
}

void SUEAIAgentPanel::HandleActionApprovalChanged(int32 ActionIndex, ECheckBoxState NewState)
{
    FUEAIAgentTransportModule::Get().SetPlannedActionApproved(ActionIndex, NewState == ECheckBoxState::Checked);
}

void SUEAIAgentPanel::UpdateActionApprovalUi()
{
    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    const int32 ActionCount = Transport.GetPlannedActionCount();

    if (ActionChecks.Num() != ActionCount)
    {
        RebuildActionApprovalUi();
    }

    for (int32 ActionIndex = 0; ActionIndex < ActionCount; ++ActionIndex)
    {
        if (!ActionTexts.IsValidIndex(ActionIndex) || !ActionChecks.IsValidIndex(ActionIndex))
        {
            continue;
        }

        if (ActionTexts[ActionIndex].IsValid())
        {
            ActionTexts[ActionIndex]->SetText(FText::FromString(Transport.GetPlannedActionPreviewText(ActionIndex)));
        }
        if (ActionChecks[ActionIndex].IsValid())
        {
            ActionChecks[ActionIndex]->SetIsChecked(
                Transport.IsPlannedActionApproved(ActionIndex) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked);
        }
    }
}

void SUEAIAgentPanel::RebuildActionApprovalUi()
{
    ActionChecks.Empty();
    ActionTexts.Empty();

    if (!ActionListBox.IsValid())
    {
        return;
    }

    ActionListBox->ClearChildren();

    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    const int32 ActionCount = Transport.GetPlannedActionCount();
    if (ActionCount == 0)
    {
        ActionListBox->AddSlot()
        .AutoHeight()
        .Padding(0.0f, 0.0f, 0.0f, 4.0f)
        [
            SNew(STextBlock)
            .Text(FText::FromString(TEXT("No planned actions.")))
        ];
        return;
    }

    for (int32 ActionIndex = 0; ActionIndex < ActionCount; ++ActionIndex)
    {
        TSharedPtr<SCheckBox> RowCheckBox;
        TSharedPtr<STextBlock> RowText;

        ActionListBox->AddSlot()
        .AutoHeight()
        .Padding(0.0f, 0.0f, 0.0f, 4.0f)
        [
            SAssignNew(RowCheckBox, SCheckBox)
            .IsChecked(Transport.IsPlannedActionApproved(ActionIndex) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
            .OnCheckStateChanged_Lambda([this, ActionIndex](ECheckBoxState NewState)
            {
                HandleActionApprovalChanged(ActionIndex, NewState);
            })
            [
                SAssignNew(RowText, STextBlock)
                .AutoWrapText(true)
                .Text(FText::FromString(Transport.GetPlannedActionPreviewText(ActionIndex)))
            ]
        ];

        ActionChecks.Add(RowCheckBox);
        ActionTexts.Add(RowText);
    }
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
