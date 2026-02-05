#include "SUEAIAgentPanel.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "GameFramework/Actor.h"
#include "UEAIAgentSceneTools.h"
#include "UEAIAgentSettings.h"
#include "UEAIAgentTransportModule.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

void SUEAIAgentPanel::Construct(const FArguments& InArgs)
{
    ProviderItems.Empty();
    ProviderItems.Add(MakeShared<FString>(TEXT("OpenAI")));
    ProviderItems.Add(MakeShared<FString>(TEXT("Gemini")));

    const UUEAIAgentSettings* Settings = GetDefault<UUEAIAgentSettings>();
    if (Settings && Settings->DefaultProvider == EUEAIAgentProvider::Gemini)
    {
        SelectedProviderItem = ProviderItems[1];
    }
    else
    {
        SelectedProviderItem = ProviderItems[0];
    }

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
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(0.0f, 0.0f, 8.0f, 0.0f)
            [
                SAssignNew(ProviderCombo, SComboBox<TSharedPtr<FString>>)
                .OptionsSource(&ProviderItems)
                .InitiallySelectedItem(SelectedProviderItem)
                .OnGenerateWidget(this, &SUEAIAgentPanel::HandleProviderComboGenerateWidget)
                .OnSelectionChanged(this, &SUEAIAgentPanel::HandleProviderComboSelectionChanged)
                [
                    SNew(STextBlock)
                    .Text_Lambda([this]()
                    {
                        return FText::FromString(GetSelectedProviderLabel());
                    })
                ]
            ]
            + SHorizontalBox::Slot()
            .FillWidth(1.0f)
            [
                SAssignNew(ApiKeyInput, SEditableTextBox)
                .HintText(FText::FromString(TEXT("Paste API key")))
                .IsPassword(true)
            ]
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
                SNew(SButton)
                .Text(FText::FromString(TEXT("Save API Key")))
                .OnClicked(this, &SUEAIAgentPanel::OnSaveApiKeyClicked)
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(0.0f, 0.0f, 8.0f, 0.0f)
            [
                SNew(SButton)
                .Text(FText::FromString(TEXT("Remove API Key")))
                .OnClicked(this, &SUEAIAgentPanel::OnRemoveApiKeyClicked)
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(0.0f, 0.0f, 8.0f, 0.0f)
            [
                SNew(SButton)
                .Text(FText::FromString(TEXT("Test Provider")))
                .OnClicked(this, &SUEAIAgentPanel::OnTestApiKeyClicked)
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            [
                SNew(SButton)
                .Text(FText::FromString(TEXT("Refresh Provider Status")))
                .OnClicked(this, &SUEAIAgentPanel::OnRefreshProviderStatusClicked)
            ]
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(8.0f, 0.0f, 8.0f, 8.0f)
        [
            SAssignNew(CredentialText, STextBlock)
            .AutoWrapText(true)
            .Text(FText::FromString(TEXT("Provider keys: unknown. Click 'Refresh Provider Status'.")))
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
                .WidthOverride(200.0f)
                .Visibility_Lambda([]()
                {
                    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
                    const bool bHasPendingSessionAction = Transport.HasActiveSession() &&
                        Transport.GetNextPendingActionIndex() != INDEX_NONE;
                    const bool bHasPlannedActions = Transport.GetPlannedActionCount() > 0;
                    if (bHasPendingSessionAction || bHasPlannedActions)
                    {
                        return EVisibility::Collapsed;
                    }
                    return EVisibility::Visible;
                })
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Run")))
                    .OnClicked(this, &SUEAIAgentPanel::OnRunWithSelectionClicked)
                ]
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(0.0f, 0.0f, 8.0f, 0.0f)
            [
                SNew(SBox)
                .WidthOverride(220.0f)
                .Visibility_Lambda([]()
                {
                    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
                    const bool bHasPendingSessionAction = Transport.HasActiveSession() &&
                        Transport.GetNextPendingActionIndex() != INDEX_NONE;
                    return bHasPendingSessionAction ? EVisibility::Visible : EVisibility::Collapsed;
                })
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Resume")))
                    .OnClicked(this, &SUEAIAgentPanel::OnResumeAgentLoopClicked)
                ]
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            [
                SNew(SBox)
                .WidthOverride(240.0f)
                .Visibility_Lambda([]()
                {
                    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
                    const bool bHasPendingSessionAction = Transport.HasActiveSession() &&
                        Transport.GetNextPendingActionIndex() != INDEX_NONE;
                    const bool bCanApply = !bHasPendingSessionAction && Transport.GetPlannedActionCount() > 0;
                    return bCanApply ? EVisibility::Visible : EVisibility::Collapsed;
                })
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Apply")))
                    .OnClicked(this, &SUEAIAgentPanel::OnApplyPlannedActionClicked)
                ]
            ]
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(8.0f, 0.0f, 8.0f, 8.0f)
        [
            SNew(SCheckBox)
            .IsChecked(ECheckBoxState::Checked)
            .OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
            {
                bAgentModeEnabled = (NewState == ECheckBoxState::Checked);
            })
            [
                SNew(STextBlock)
                .Text(FText::FromString(TEXT("Agent mode: auto-run low risk actions, pause on medium/high")))
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

    if (StatusText.IsValid())
    {
        StatusText->SetText(FText::FromString(TEXT("Status: checking...")));
    }
    FUEAIAgentTransportModule::Get().CheckHealth(FOnUEAIAgentHealthChecked::CreateSP(
        this,
        &SUEAIAgentPanel::HandleHealthResult));
    RegisterActiveTimer(10.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SUEAIAgentPanel::HandleHealthTimer));

    UpdateActionApprovalUi();
}

FReply SUEAIAgentPanel::OnRunWithSelectionClicked()
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
    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();

    if (bAgentModeEnabled)
    {
        PlanText->SetText(FText::FromString(TEXT("Agent: starting session...")));
        Transport.StartSession(
            Prompt,
            TEXT("agent"),
            SelectedActors,
            FOnUEAIAgentSessionUpdated::CreateSP(this, &SUEAIAgentPanel::HandleSessionUpdate));
        return FReply::Handled();
    }

    PlanText->SetText(FText::FromString(TEXT("Plan: requesting...")));
    Transport.PlanTask(
        Prompt,
        TEXT("chat"),
        SelectedActors,
        FOnUEAIAgentTaskPlanned::CreateSP(this, &SUEAIAgentPanel::HandlePlanResult));

    return FReply::Handled();
}

FReply SUEAIAgentPanel::OnSaveApiKeyClicked()
{
    if (!ApiKeyInput.IsValid() || !CredentialText.IsValid())
    {
        return FReply::Handled();
    }

    const FString ApiKey = ApiKeyInput->GetText().ToString().TrimStartAndEnd();
    if (ApiKey.IsEmpty())
    {
        CredentialText->SetText(FText::FromString(TEXT("Credential: please enter an API key first.")));
        return FReply::Handled();
    }

    CredentialText->SetText(FText::FromString(TEXT("Credential: saving key...")));
    FUEAIAgentTransportModule::Get().SetProviderApiKey(
        GetSelectedProviderCode(),
        ApiKey,
        FOnUEAIAgentCredentialOpFinished::CreateSP(this, &SUEAIAgentPanel::HandleCredentialOperationResult));
    return FReply::Handled();
}

FReply SUEAIAgentPanel::OnRemoveApiKeyClicked()
{
    if (!CredentialText.IsValid())
    {
        return FReply::Handled();
    }

    CredentialText->SetText(FText::FromString(TEXT("Credential: removing key...")));
    FUEAIAgentTransportModule::Get().DeleteProviderApiKey(
        GetSelectedProviderCode(),
        FOnUEAIAgentCredentialOpFinished::CreateSP(this, &SUEAIAgentPanel::HandleCredentialOperationResult));
    return FReply::Handled();
}

FReply SUEAIAgentPanel::OnTestApiKeyClicked()
{
    if (!CredentialText.IsValid())
    {
        return FReply::Handled();
    }

    CredentialText->SetText(FText::FromString(TEXT("Credential: testing provider...")));
    FUEAIAgentTransportModule::Get().TestProviderApiKey(
        GetSelectedProviderCode(),
        FOnUEAIAgentCredentialOpFinished::CreateSP(this, &SUEAIAgentPanel::HandleCredentialOperationResult));
    return FReply::Handled();
}

FReply SUEAIAgentPanel::OnRefreshProviderStatusClicked()
{
    if (!CredentialText.IsValid())
    {
        return FReply::Handled();
    }

    CredentialText->SetText(FText::FromString(TEXT("Credential: loading provider status...")));
    FUEAIAgentTransportModule::Get().GetProviderStatus(
        FOnUEAIAgentCredentialOpFinished::CreateSP(this, &SUEAIAgentPanel::HandleCredentialOperationResult));
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
        const bool bOk = ExecutePlannedAction(PlannedAction, ResultMessage);

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

FReply SUEAIAgentPanel::OnResumeAgentLoopClicked()
{
    if (!PlanText.IsValid())
    {
        return FReply::Handled();
    }

    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    if (!Transport.HasActiveSession())
    {
        PlanText->SetText(FText::FromString(TEXT("Agent: no active session. Click Run first.")));
        return FReply::Handled();
    }

    const bool bApproved = Transport.GetPlannedActionCount() > 0
        ? Transport.IsPlannedActionApproved(0)
        : true;
    Transport.ApproveCurrentSessionAction(
        bApproved,
        FOnUEAIAgentSessionUpdated::CreateLambda([this](bool bOk, const FString& Message)
        {
            if (!bOk)
            {
                HandleSessionUpdate(false, Message);
                return;
            }

            FUEAIAgentTransportModule::Get().ResumeSession(
                FOnUEAIAgentSessionUpdated::CreateSP(this, &SUEAIAgentPanel::HandleSessionUpdate));
        }));

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

void SUEAIAgentPanel::HandleSessionUpdate(bool bOk, const FString& Message)
{
    if (!PlanText.IsValid())
    {
        return;
    }

    UpdateActionApprovalUi();
    if (!bOk)
    {
        PlanText->SetText(FText::FromString(TEXT("Agent: error\n") + Message));
        return;
    }

    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    if (Transport.GetPlannedActionCount() <= 0)
    {
        PlanText->SetText(FText::FromString(TEXT("Agent: update\n") + Message));
        return;
    }

    FUEAIAgentPlannedSceneAction NextAction;
    if (!Transport.GetPendingAction(0, NextAction))
    {
        PlanText->SetText(FText::FromString(TEXT("Agent: update\n") + Message));
        return;
    }

    if (!NextAction.bApproved)
    {
        PlanText->SetText(FText::FromString(TEXT("Agent: awaiting approval\n") + Message));
        return;
    }

    FString ExecuteMessage;
    const bool bOkExecute = ExecutePlannedAction(NextAction, ExecuteMessage);
    PlanText->SetText(FText::FromString(TEXT("Agent: executed action, syncing...\n") + ExecuteMessage));
    Transport.NextSession(
        true,
        bOkExecute,
        ExecuteMessage,
        FOnUEAIAgentSessionUpdated::CreateSP(this, &SUEAIAgentPanel::HandleSessionUpdate));
}

bool SUEAIAgentPanel::ExecutePlannedAction(const FUEAIAgentPlannedSceneAction& PlannedAction, FString& OutMessage) const
{
    if (PlannedAction.Type == EUEAIAgentPlannedActionType::CreateActor)
    {
        FUEAIAgentCreateActorParams Params;
        Params.ActorClass = PlannedAction.ActorClass;
        Params.Location = PlannedAction.SpawnLocation;
        Params.Rotation = PlannedAction.SpawnRotation;
        Params.Count = PlannedAction.SpawnCount;
        return FUEAIAgentSceneTools::SceneCreateActor(Params, OutMessage);
    }

    if (PlannedAction.Type == EUEAIAgentPlannedActionType::DeleteActor)
    {
        FUEAIAgentDeleteActorParams Params;
        Params.ActorNames = PlannedAction.ActorNames;
        Params.bUseSelectionIfActorNamesEmpty = false;
        if (Params.ActorNames.IsEmpty())
        {
            OutMessage = TEXT("Skipped delete action with no target actors.");
            return false;
        }
        return FUEAIAgentSceneTools::SceneDeleteActor(Params, OutMessage);
    }

    FUEAIAgentModifyActorParams Params;
    Params.ActorNames = PlannedAction.ActorNames;
    Params.DeltaLocation = PlannedAction.DeltaLocation;
    Params.DeltaRotation = PlannedAction.DeltaRotation;
    Params.bUseSelectionIfActorNamesEmpty = false;
    if (Params.ActorNames.IsEmpty())
    {
        OutMessage = TEXT("Skipped modify action with no target actors.");
        return false;
    }
    return FUEAIAgentSceneTools::SceneModifyActor(Params, OutMessage);
}

void SUEAIAgentPanel::HandleCredentialOperationResult(bool bOk, const FString& Message)
{
    if (!CredentialText.IsValid())
    {
        return;
    }

    const FString Prefix = bOk ? TEXT("Credential: ok\n") : TEXT("Credential: error\n");
    CredentialText->SetText(FText::FromString(Prefix + Message));
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

FString SUEAIAgentPanel::GetSelectedProviderCode() const
{
    if (!SelectedProviderItem.IsValid())
    {
        return TEXT("openai");
    }

    return SelectedProviderItem->Equals(TEXT("Gemini"), ESearchCase::IgnoreCase) ? TEXT("gemini") : TEXT("openai");
}

FString SUEAIAgentPanel::GetSelectedProviderLabel() const
{
    if (!SelectedProviderItem.IsValid())
    {
        return TEXT("OpenAI");
    }

    return *SelectedProviderItem;
}

TSharedRef<SWidget> SUEAIAgentPanel::HandleProviderComboGenerateWidget(TSharedPtr<FString> InItem) const
{
    return SNew(STextBlock).Text(FText::FromString(InItem.IsValid() ? *InItem : TEXT("Unknown")));
}

void SUEAIAgentPanel::HandleProviderComboSelectionChanged(TSharedPtr<FString> NewValue, ESelectInfo::Type SelectInfo)
{
    if (!NewValue.IsValid())
    {
        return;
    }

    SelectedProviderItem = NewValue;
}

EActiveTimerReturnType SUEAIAgentPanel::HandleHealthTimer(double InCurrentTime, float InDeltaTime)
{
    (void)InCurrentTime;
    (void)InDeltaTime;
    FUEAIAgentTransportModule::Get().CheckHealth(FOnUEAIAgentHealthChecked::CreateSP(
        this,
        &SUEAIAgentPanel::HandleHealthResult));
    return EActiveTimerReturnType::Continue;
}
