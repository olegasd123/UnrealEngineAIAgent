#include "SUEAIAgentPanel.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "GameFramework/Actor.h"
#include "UEAIAgentSceneTools.h"
#include "UEAIAgentSettings.h"
#include "UEAIAgentTransportModule.h"
#include "Misc/MessageDialog.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

void SUEAIAgentPanel::Construct(const FArguments& InArgs)
{
    ProviderItems.Empty();
    ProviderItems.Add(MakeShared<FString>(TEXT("OpenAI")));
    ProviderItems.Add(MakeShared<FString>(TEXT("Gemini")));
    ModeItems.Empty();
    ModeItems.Add(MakeShared<FString>(TEXT("Chat")));
    ModeItems.Add(MakeShared<FString>(TEXT("Agent")));
    SelectedModeItem = ModeItems[1];

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
        SAssignNew(ViewSwitcher, SWidgetSwitcher)
        + SWidgetSwitcher::Slot()
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(8.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .FillWidth(1.0f)
                [
                    SAssignNew(StatusText, STextBlock)
                    .Text(FText::FromString(TEXT("Status: not checked")))
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Settings")))
                    .OnClicked(this, &SUEAIAgentPanel::OnOpenSettingsClicked)
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
                    SNew(SBox)
                    .WidthOverride(220.0f)
                    [
                        SAssignNew(ChatCombo, SComboBox<TSharedPtr<FString>>)
                        .OptionsSource(&ChatItems)
                        .InitiallySelectedItem(SelectedChatItem)
                        .OnGenerateWidget(this, &SUEAIAgentPanel::HandleChatComboGenerateWidget)
                        .OnSelectionChanged(this, &SUEAIAgentPanel::HandleChatComboSelectionChanged)
                        [
                            SNew(STextBlock)
                            .Text_Lambda([this]()
                            {
                                return FText::FromString(SelectedChatItem.IsValid() ? *SelectedChatItem : TEXT("No chat"));
                            })
                        ]
                    ]
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(0.0f, 0.0f, 8.0f, 0.0f)
                [
                    SNew(SBox)
                    .WidthOverride(180.0f)
                    [
                        SAssignNew(NewChatTitleInput, SEditableTextBox)
                        .HintText(FText::FromString(TEXT("New chat title")))
                    ]
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(0.0f, 0.0f, 8.0f, 0.0f)
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("New Chat")))
                    .OnClicked(this, &SUEAIAgentPanel::OnCreateChatClicked)
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(0.0f, 0.0f, 8.0f, 0.0f)
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Refresh Chats")))
                    .OnClicked(this, &SUEAIAgentPanel::OnRefreshChatsClicked)
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Archive Chat")))
                    .OnClicked(this, &SUEAIAgentPanel::OnArchiveChatClicked)
                ]
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(8.0f, 0.0f, 8.0f, 8.0f)
            [
                SNew(SBox)
                .HeightOverride_Lambda([this]()
                {
                    const int32 Lines = FMath::Clamp(PromptVisibleLineCount, 1, 10);
                    return 16.0f + (16.0f * Lines);
                })
                [
                    SAssignNew(PromptInput, SMultiLineEditableTextBox)
                    .HintText(FText::FromString(TEXT("Describe what to do with selected actors")))
                    .Text(FText::FromString(TEXT("Move selected actors +250 on X")))
                    .OnTextChanged(this, &SUEAIAgentPanel::HandlePromptTextChanged)
                    .Padding(FMargin(8.0f, 8.0f, 8.0f, 8.0f))
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
                    SNew(SBox)
                    .WidthOverride(120.0f)
                    .Visibility_Lambda([this]()
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
                        SAssignNew(ModeCombo, SComboBox<TSharedPtr<FString>>)
                        .OptionsSource(&ModeItems)
                        .InitiallySelectedItem(SelectedModeItem)
                        .OnGenerateWidget(this, &SUEAIAgentPanel::HandleModeComboGenerateWidget)
                        .OnSelectionChanged(this, &SUEAIAgentPanel::HandleModeComboSelectionChanged)
                        [
                            SNew(STextBlock)
                            .Text_Lambda([this]()
                            {
                                return FText::FromString(GetSelectedModeLabel());
                            })
                        ]
                    ]
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(0.0f, 0.0f, 8.0f, 0.0f)
                [
                    SNew(SBox)
                    .WidthOverride(200.0f)
                    .Visibility_Lambda([this]()
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
                    .Visibility_Lambda([this]()
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
                .Padding(0.0f, 0.0f, 8.0f, 0.0f)
                [
                    SNew(SBox)
                    .WidthOverride(220.0f)
                    .Visibility_Lambda([this]()
                    {
                        FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
                        const bool bHasPendingSessionAction = Transport.HasActiveSession() &&
                            Transport.GetNextPendingActionIndex() != INDEX_NONE;
                        return bHasPendingSessionAction && CurrentSessionStatus == ESessionStatus::AwaitingApproval
                            ? EVisibility::Visible
                            : EVisibility::Collapsed;
                    })
                    [
                        SNew(SButton)
                        .Text(FText::FromString(TEXT("Reject")))
                        .OnClicked(this, &SUEAIAgentPanel::OnRejectCurrentActionClicked)
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
        ]
        + SWidgetSwitcher::Slot()
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(8.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(0.0f, 0.0f, 8.0f, 0.0f)
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Back")))
                    .OnClicked(this, &SUEAIAgentPanel::OnBackToMainClicked)
                ]
                + SHorizontalBox::Slot()
                .FillWidth(1.0f)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(TEXT("Settings")))
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
                SNew(SBox)
                .HeightOverride(88.0f)
                [
                    SAssignNew(CredentialText, SMultiLineEditableTextBox)
                    .IsReadOnly(true)
                    .SelectAllTextWhenFocused(false)
                    .Text(FText::FromString(TEXT("Provider keys: unknown. Click 'Refresh Provider Status'.")))
                ]
            ]
        ]
    ];

    SetCurrentView(EPanelView::Main);
    if (PromptInput.IsValid())
    {
        HandlePromptTextChanged(PromptInput->GetText());
    }

    if (StatusText.IsValid())
    {
        StatusText->SetText(FText::FromString(TEXT("Status: checking...")));
    }
    FUEAIAgentTransportModule::Get().CheckHealth(FOnUEAIAgentHealthChecked::CreateSP(
        this,
        &SUEAIAgentPanel::HandleHealthResult));
    RegisterActiveTimer(10.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SUEAIAgentPanel::HandleHealthTimer));
    OnRefreshChatsClicked();

    UpdateActionApprovalUi();
}

void SUEAIAgentPanel::SetCurrentView(EPanelView NewView)
{
    CurrentView = NewView;
    if (!ViewSwitcher.IsValid())
    {
        return;
    }

    const int32 Index = CurrentView == EPanelView::Settings ? 1 : 0;
    ViewSwitcher->SetActiveWidgetIndex(Index);
}

FReply SUEAIAgentPanel::OnOpenSettingsClicked()
{
    SetCurrentView(EPanelView::Settings);
    if (CredentialText.IsValid())
    {
        CredentialText->SetText(FText::FromString(TEXT("Credential: loading provider status...")));
    }
    FUEAIAgentTransportModule::Get().GetProviderStatus(
        FOnUEAIAgentCredentialOpFinished::CreateSP(this, &SUEAIAgentPanel::HandleCredentialOperationResult));
    return FReply::Handled();
}

FReply SUEAIAgentPanel::OnBackToMainClicked()
{
    SetCurrentView(EPanelView::Main);
    if (ApiKeyInput.IsValid())
    {
        ApiKeyInput->SetText(FText::GetEmpty());
    }
    return FReply::Handled();
}

FReply SUEAIAgentPanel::OnRunWithSelectionClicked()
{
    if (!PromptInput.IsValid() || !PlanText.IsValid())
    {
        return FReply::Handled();
    }

    CurrentSessionStatus = ESessionStatus::Unknown;
    const FString Prompt = PromptInput->GetText().ToString().TrimStartAndEnd();
    if (Prompt.IsEmpty())
    {
        PlanText->SetText(FText::FromString(TEXT("Plan: please enter a prompt first.")));
        return FReply::Handled();
    }

    const TArray<FString> SelectedActors = CollectSelectedActorNames();
    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    const FString Mode = GetSelectedModeCode();

    if (Mode == TEXT("agent"))
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

FReply SUEAIAgentPanel::OnCreateChatClicked()
{
    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    const FString Title = NewChatTitleInput.IsValid() ? NewChatTitleInput->GetText().ToString() : TEXT("");
    Transport.CreateChat(Title, FOnUEAIAgentChatOpFinished::CreateSP(this, &SUEAIAgentPanel::HandleChatOperationResult));
    return FReply::Handled();
}

FReply SUEAIAgentPanel::OnRefreshChatsClicked()
{
    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    Transport.RefreshChats(false, FOnUEAIAgentChatOpFinished::CreateSP(this, &SUEAIAgentPanel::HandleChatOperationResult));
    return FReply::Handled();
}

FReply SUEAIAgentPanel::OnArchiveChatClicked()
{
    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    Transport.ArchiveActiveChat(FOnUEAIAgentChatOpFinished::CreateSP(this, &SUEAIAgentPanel::HandleChatOperationResult));
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
        PlanText->SetText(FText::FromString(TEXT("Execute: error\nNo planned actions. Use 'Run' first.")));
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

FReply SUEAIAgentPanel::OnRejectCurrentActionClicked()
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

    const EAppReturnType::Type ConfirmResult = FMessageDialog::Open(
        EAppMsgType::YesNo,
        FText::FromString(TEXT("Reject the current action? This will mark the session as failed.")));
    if (ConfirmResult != EAppReturnType::Yes)
    {
        return FReply::Handled();
    }

    PlanText->SetText(FText::FromString(TEXT("Agent: rejecting action...")));
    Transport.ApproveCurrentSessionAction(
        false,
        FOnUEAIAgentSessionUpdated::CreateSP(this, &SUEAIAgentPanel::HandleSessionUpdate));

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

    CurrentSessionStatus = ESessionStatus::Unknown;
    if (!bOk)
    {
        PlanText->SetText(FText::FromString(TEXT("Plan: error\n") + Message));
        RefreshActiveChatHistory();
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
    RefreshActiveChatHistory();
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
        CurrentSessionStatus = ESessionStatus::Failed;
        PlanText->SetText(FText::FromString(TEXT("Agent: failed\n") + Message + TEXT("\nClick Run to start over.")));
        RefreshActiveChatHistory();
        return;
    }

    CurrentSessionStatus = ParseSessionStatusFromMessage(Message);
    if (CurrentSessionStatus == ESessionStatus::Failed)
    {
        PlanText->SetText(FText::FromString(TEXT("Agent: failed\n") + Message + TEXT("\nClick Run to start over.")));
        RefreshActiveChatHistory();
        return;
    }
    if (CurrentSessionStatus == ESessionStatus::Completed)
    {
        PlanText->SetText(FText::FromString(TEXT("Agent: completed\n") + Message + TEXT("\nClick Run to start over.")));
        RefreshActiveChatHistory();
        return;
    }

    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    if (Transport.GetPlannedActionCount() <= 0)
    {
        PlanText->SetText(FText::FromString(TEXT("Agent: update\n") + Message));
        RefreshActiveChatHistory();
        return;
    }

    FUEAIAgentPlannedSceneAction NextAction;
    if (!Transport.GetPendingAction(0, NextAction))
    {
        PlanText->SetText(FText::FromString(TEXT("Agent: update\n") + Message));
        RefreshActiveChatHistory();
        return;
    }

    if (!NextAction.bApproved)
    {
        PlanText->SetText(FText::FromString(TEXT("Agent: awaiting approval\n") + Message));
        RefreshActiveChatHistory();
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

SUEAIAgentPanel::ESessionStatus SUEAIAgentPanel::ParseSessionStatusFromMessage(const FString& Message) const
{
    const FString Prefix = TEXT("Session:");
    if (!Message.StartsWith(Prefix))
    {
        return ESessionStatus::Unknown;
    }

    int32 NewlineIndex = INDEX_NONE;
    if (!Message.FindChar(TEXT('\n'), NewlineIndex))
    {
        NewlineIndex = Message.Len();
    }

    FString StatusValue = Message.Mid(Prefix.Len(), NewlineIndex - Prefix.Len()).TrimStartAndEnd();
    if (StatusValue.Equals(TEXT("ready_to_execute"), ESearchCase::IgnoreCase))
    {
        return ESessionStatus::ReadyToExecute;
    }
    if (StatusValue.Equals(TEXT("awaiting_approval"), ESearchCase::IgnoreCase))
    {
        return ESessionStatus::AwaitingApproval;
    }
    if (StatusValue.Equals(TEXT("completed"), ESearchCase::IgnoreCase))
    {
        return ESessionStatus::Completed;
    }
    if (StatusValue.Equals(TEXT("failed"), ESearchCase::IgnoreCase))
    {
        return ESessionStatus::Failed;
    }

    return ESessionStatus::Unknown;
}

bool SUEAIAgentPanel::ExecutePlannedAction(const FUEAIAgentPlannedSceneAction& PlannedAction, FString& OutMessage) const
{
    if (PlannedAction.Type == EUEAIAgentPlannedActionType::SessionBeginTransaction)
    {
        return FUEAIAgentSceneTools::SessionBeginTransaction(PlannedAction.TransactionDescription, OutMessage);
    }

    if (PlannedAction.Type == EUEAIAgentPlannedActionType::SessionCommitTransaction)
    {
        return FUEAIAgentSceneTools::SessionCommitTransaction(OutMessage);
    }

    if (PlannedAction.Type == EUEAIAgentPlannedActionType::SessionRollbackTransaction)
    {
        return FUEAIAgentSceneTools::SessionRollbackTransaction(OutMessage);
    }

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

    if (PlannedAction.Type == EUEAIAgentPlannedActionType::ModifyComponent)
    {
        FUEAIAgentModifyComponentParams Params;
        Params.ActorNames = PlannedAction.ActorNames;
        Params.ComponentName = PlannedAction.ComponentName;
        Params.DeltaLocation = PlannedAction.ComponentDeltaLocation;
        Params.DeltaRotation = PlannedAction.ComponentDeltaRotation;
        Params.DeltaScale = PlannedAction.ComponentDeltaScale;
        Params.Scale = PlannedAction.ComponentScale;
        Params.bHasScale = PlannedAction.bComponentHasScale;
        Params.bSetVisibility = PlannedAction.bComponentVisibilityEdit;
        Params.bVisible = PlannedAction.bComponentVisible;
        Params.bUseSelectionIfActorNamesEmpty = false;
        if (Params.ActorNames.IsEmpty())
        {
            OutMessage = TEXT("Skipped component action with no target actors.");
            return false;
        }
        return FUEAIAgentSceneTools::SceneModifyComponent(Params, OutMessage);
    }

    if (PlannedAction.Type == EUEAIAgentPlannedActionType::AddActorTag)
    {
        FUEAIAgentAddActorTagParams Params;
        Params.ActorNames = PlannedAction.ActorNames;
        Params.Tag = PlannedAction.ActorTag;
        Params.bUseSelectionIfActorNamesEmpty = false;
        if (Params.ActorNames.IsEmpty())
        {
            OutMessage = TEXT("Skipped tag action with no target actors.");
            return false;
        }
        return FUEAIAgentSceneTools::SceneAddActorTag(Params, OutMessage);
    }

    if (PlannedAction.Type == EUEAIAgentPlannedActionType::SetComponentMaterial)
    {
        FUEAIAgentSetComponentMaterialParams Params;
        Params.ActorNames = PlannedAction.ActorNames;
        Params.ComponentName = PlannedAction.ComponentName;
        Params.MaterialPath = PlannedAction.MaterialPath;
        Params.MaterialSlot = PlannedAction.MaterialSlot;
        Params.bUseSelectionIfActorNamesEmpty = false;
        if (Params.ActorNames.IsEmpty())
        {
            OutMessage = TEXT("Skipped material action with no target actors.");
            return false;
        }
        return FUEAIAgentSceneTools::SceneSetComponentMaterial(Params, OutMessage);
    }

    if (PlannedAction.Type == EUEAIAgentPlannedActionType::SetComponentStaticMesh)
    {
        FUEAIAgentSetComponentStaticMeshParams Params;
        Params.ActorNames = PlannedAction.ActorNames;
        Params.ComponentName = PlannedAction.ComponentName;
        Params.MeshPath = PlannedAction.MeshPath;
        Params.bUseSelectionIfActorNamesEmpty = false;
        if (Params.ActorNames.IsEmpty())
        {
            OutMessage = TEXT("Skipped mesh action with no target actors.");
            return false;
        }
        return FUEAIAgentSceneTools::SceneSetComponentStaticMesh(Params, OutMessage);
    }

    if (PlannedAction.Type == EUEAIAgentPlannedActionType::SetActorFolder)
    {
        FUEAIAgentSetActorFolderParams Params;
        Params.ActorNames = PlannedAction.ActorNames;
        Params.FolderPath = PlannedAction.FolderPath;
        Params.bUseSelectionIfActorNamesEmpty = false;
        if (Params.ActorNames.IsEmpty())
        {
            OutMessage = TEXT("Skipped folder action with no target actors.");
            return false;
        }
        return FUEAIAgentSceneTools::SceneSetActorFolder(Params, OutMessage);
    }

    if (PlannedAction.Type == EUEAIAgentPlannedActionType::AddActorLabelPrefix)
    {
        FUEAIAgentAddActorLabelPrefixParams Params;
        Params.ActorNames = PlannedAction.ActorNames;
        Params.Prefix = PlannedAction.LabelPrefix;
        Params.bUseSelectionIfActorNamesEmpty = false;
        if (Params.ActorNames.IsEmpty())
        {
            OutMessage = TEXT("Skipped label prefix action with no target actors.");
            return false;
        }
        return FUEAIAgentSceneTools::SceneAddActorLabelPrefix(Params, OutMessage);
    }

    if (PlannedAction.Type == EUEAIAgentPlannedActionType::DuplicateActors)
    {
        FUEAIAgentDuplicateActorsParams Params;
        Params.ActorNames = PlannedAction.ActorNames;
        Params.Count = PlannedAction.DuplicateCount;
        Params.Offset = PlannedAction.DuplicateOffset;
        Params.bUseSelectionIfActorNamesEmpty = false;
        if (Params.ActorNames.IsEmpty())
        {
            OutMessage = TEXT("Skipped duplicate action with no target actors.");
            return false;
        }
        return FUEAIAgentSceneTools::SceneDuplicateActors(Params, OutMessage);
    }

    FUEAIAgentModifyActorParams Params;
    Params.ActorNames = PlannedAction.ActorNames;
    Params.DeltaLocation = PlannedAction.DeltaLocation;
    Params.DeltaRotation = PlannedAction.DeltaRotation;
    Params.DeltaScale = PlannedAction.DeltaScale;
    Params.Scale = PlannedAction.Scale;
    Params.bHasScale = PlannedAction.bHasScale;
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

void SUEAIAgentPanel::HandleChatOperationResult(bool bOk, const FString& Message)
{
    if (!bOk)
    {
        UpdateChatHistoryText(TEXT("Chat error: ") + Message + TEXT("\n"));
        return;
    }

    RefreshChatUiFromTransport(true);
    RefreshActiveChatHistory();

    if (NewChatTitleInput.IsValid())
    {
        NewChatTitleInput->SetText(FText::GetEmpty());
    }
}

void SUEAIAgentPanel::HandleChatHistoryResult(bool bOk, const FString& Message)
{
    if (!bOk)
    {
        UpdateChatHistoryText(TEXT("Chat history error: ") + Message + TEXT("\n"));
        return;
    }

    UpdateChatHistoryText();
}

void SUEAIAgentPanel::RefreshChatUiFromTransport(bool bKeepCurrentSelection)
{
    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    const FString PreviousLabel = bKeepCurrentSelection && SelectedChatItem.IsValid() ? *SelectedChatItem : TEXT("");
    const FString ActiveId = Transport.GetActiveChatId();

    ChatItems.Empty();
    ChatLabelToId.Empty();
    SelectedChatItem.Reset();

    const TArray<FUEAIAgentChatSummary>& Chats = Transport.GetChats();
    for (const FUEAIAgentChatSummary& Chat : Chats)
    {
        const FString Label = Chat.Title.IsEmpty() ? Chat.Id : FString::Printf(TEXT("%s (%s)"), *Chat.Title, *Chat.Id.Left(8));
        const TSharedPtr<FString> LabelItem = MakeShared<FString>(Label);
        ChatItems.Add(LabelItem);
        ChatLabelToId.Add(Label, Chat.Id);

        if (!ActiveId.IsEmpty() && Chat.Id == ActiveId)
        {
            SelectedChatItem = LabelItem;
        }
        else if (SelectedChatItem.IsValid() == false && !PreviousLabel.IsEmpty() && Label == PreviousLabel)
        {
            SelectedChatItem = LabelItem;
        }
    }

    if (!SelectedChatItem.IsValid() && ChatItems.Num() > 0)
    {
        SelectedChatItem = ChatItems[0];
        const FString* IdPtr = ChatLabelToId.Find(*SelectedChatItem);
        Transport.SetActiveChatId(IdPtr ? *IdPtr : TEXT(""));
    }

    if (ChatCombo.IsValid())
    {
        ChatCombo->RefreshOptions();
        ChatCombo->SetSelectedItem(SelectedChatItem);
    }
}

void SUEAIAgentPanel::RefreshActiveChatHistory()
{
    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    Transport.LoadActiveChatHistory(100, FOnUEAIAgentChatOpFinished::CreateSP(this, &SUEAIAgentPanel::HandleChatHistoryResult));
}

void SUEAIAgentPanel::UpdateChatHistoryText(const FString& PrefixMessage)
{
    if (!ChatHistoryText.IsValid())
    {
        return;
    }

    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    FString ResultText = PrefixMessage;
    const FString ActiveId = Transport.GetActiveChatId();
    if (ActiveId.IsEmpty())
    {
        ChatHistoryText->SetText(FText::FromString(ResultText));
        return;
    }

    const TArray<FUEAIAgentChatHistoryEntry>& Entries = Transport.GetActiveChatHistory();
    if (Entries.Num() == 0)
    {
        ResultText += TEXT("Chat history is empty.");
        ChatHistoryText->SetText(FText::FromString(ResultText));
        return;
    }

    for (const FUEAIAgentChatHistoryEntry& Entry : Entries)
    {
        ResultText += FString::Printf(
            TEXT("[%s] %s %s\n%s\n"),
            *Entry.CreatedAt,
            *Entry.Kind,
            *Entry.Route,
            *Entry.Summary);
    }

    ChatHistoryText->SetText(FText::FromString(ResultText));
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

FString SUEAIAgentPanel::GetSelectedModeCode() const
{
    if (!SelectedModeItem.IsValid())
    {
        return TEXT("agent");
    }

    return SelectedModeItem->Equals(TEXT("Chat"), ESearchCase::IgnoreCase) ? TEXT("chat") : TEXT("agent");
}

FString SUEAIAgentPanel::GetSelectedModeLabel() const
{
    if (!SelectedModeItem.IsValid())
    {
        return TEXT("Agent");
    }

    return *SelectedModeItem;
}

TSharedRef<SWidget> SUEAIAgentPanel::HandleProviderComboGenerateWidget(TSharedPtr<FString> InItem) const
{
    return SNew(STextBlock).Text(FText::FromString(InItem.IsValid() ? *InItem : TEXT("Unknown")));
}

TSharedRef<SWidget> SUEAIAgentPanel::HandleModeComboGenerateWidget(TSharedPtr<FString> InItem) const
{
    return SNew(STextBlock).Text(FText::FromString(InItem.IsValid() ? *InItem : TEXT("Unknown")));
}

TSharedRef<SWidget> SUEAIAgentPanel::HandleChatComboGenerateWidget(TSharedPtr<FString> InItem) const
{
    return SNew(STextBlock).Text(FText::FromString(InItem.IsValid() ? *InItem : TEXT("No chat")));
}

void SUEAIAgentPanel::HandleProviderComboSelectionChanged(TSharedPtr<FString> NewValue, ESelectInfo::Type SelectInfo)
{
    if (!NewValue.IsValid())
    {
        return;
    }

    SelectedProviderItem = NewValue;
}

void SUEAIAgentPanel::HandleModeComboSelectionChanged(TSharedPtr<FString> NewValue, ESelectInfo::Type SelectInfo)
{
    if (!NewValue.IsValid())
    {
        return;
    }

    SelectedModeItem = NewValue;
}

void SUEAIAgentPanel::HandleChatComboSelectionChanged(TSharedPtr<FString> NewValue, ESelectInfo::Type SelectInfo)
{
    (void)SelectInfo;
    if (!NewValue.IsValid())
    {
        FUEAIAgentTransportModule::Get().SetActiveChatId(TEXT(""));
        SelectedChatItem.Reset();
        UpdateChatHistoryText();
        return;
    }

    SelectedChatItem = NewValue;
    const FString* ChatIdPtr = ChatLabelToId.Find(*NewValue);
    FUEAIAgentTransportModule::Get().SetActiveChatId(ChatIdPtr ? *ChatIdPtr : TEXT(""));
    RefreshActiveChatHistory();
}

void SUEAIAgentPanel::HandlePromptTextChanged(const FText& NewText)
{
    const FString TextValue = NewText.ToString();
    int32 Lines = 1;
    for (const TCHAR CharValue : TextValue)
    {
        if (CharValue == TEXT('\n'))
        {
            ++Lines;
        }
    }
    PromptVisibleLineCount = FMath::Clamp(Lines, 1, 10);
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
