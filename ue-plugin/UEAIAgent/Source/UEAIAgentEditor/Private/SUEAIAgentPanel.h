#pragma once

#include "CoreMinimal.h"
#include "Types/SlateEnums.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SWidget.h"

class STextBlock;
class SEditableTextBox;
class SMultiLineEditableTextBox;
class SCheckBox;
class SVerticalBox;
class SWidgetSwitcher;
template<typename OptionType>
class SComboBox;
enum class ECheckBoxState : uint8;
struct FUEAIAgentPlannedSceneAction;

class SUEAIAgentPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SUEAIAgentPanel)
    {
    }
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

private:
    enum class EPanelView : uint8
    {
        Main,
        Settings
    };

    enum class ESessionStatus : uint8
    {
        Unknown,
        ReadyToExecute,
        AwaitingApproval,
        Completed,
        Failed
    };

    FReply OnSaveApiKeyClicked();
    FReply OnRemoveApiKeyClicked();
    FReply OnTestApiKeyClicked();
    FReply OnRefreshProviderStatusClicked();
    FReply OnOpenSettingsClicked();
    FReply OnBackToMainClicked();
    FReply OnRunWithSelectionClicked();
    FReply OnResumeAgentLoopClicked();
    FReply OnApplyPlannedActionClicked();
    void HandleHealthResult(bool bOk, const FString& Message);
    void HandleCredentialOperationResult(bool bOk, const FString& Message);
    void HandlePlanResult(bool bOk, const FString& Message);
    void HandleSessionUpdate(bool bOk, const FString& Message);
    void HandleActionApprovalChanged(int32 ActionIndex, ECheckBoxState NewState);
    FString GetSelectedProviderCode() const;
    FString GetSelectedProviderLabel() const;
    FString GetSelectedModeCode() const;
    FString GetSelectedModeLabel() const;
    TSharedRef<SWidget> HandleProviderComboGenerateWidget(TSharedPtr<FString> InItem) const;
    TSharedRef<SWidget> HandleModeComboGenerateWidget(TSharedPtr<FString> InItem) const;
    void HandleProviderComboSelectionChanged(TSharedPtr<FString> NewValue, ESelectInfo::Type SelectInfo);
    void HandleModeComboSelectionChanged(TSharedPtr<FString> NewValue, ESelectInfo::Type SelectInfo);
    void HandlePromptTextChanged(const FText& NewText);
    ESessionStatus ParseSessionStatusFromMessage(const FString& Message) const;
    void UpdateActionApprovalUi();
    void RebuildActionApprovalUi();
    bool ExecutePlannedAction(const FUEAIAgentPlannedSceneAction& PlannedAction, FString& OutMessage) const;
    TArray<FString> CollectSelectedActorNames() const;
    EActiveTimerReturnType HandleHealthTimer(double InCurrentTime, float InDeltaTime);
    void SetCurrentView(EPanelView NewView);

    TSharedPtr<STextBlock> StatusText;
    TSharedPtr<SMultiLineEditableTextBox> CredentialText;
    TSharedPtr<SEditableTextBox> ApiKeyInput;
    TSharedPtr<SWidgetSwitcher> ViewSwitcher;
    TSharedPtr<SComboBox<TSharedPtr<FString>>> ProviderCombo;
    TSharedPtr<SComboBox<TSharedPtr<FString>>> ModeCombo;
    TArray<TSharedPtr<FString>> ProviderItems;
    TArray<TSharedPtr<FString>> ModeItems;
    TSharedPtr<FString> SelectedProviderItem;
    TSharedPtr<FString> SelectedModeItem;
    TSharedPtr<SMultiLineEditableTextBox> PromptInput;
    TSharedPtr<STextBlock> PlanText;
    TSharedPtr<SVerticalBox> ActionListBox;
    TArray<TSharedPtr<SCheckBox>> ActionChecks;
    TArray<TSharedPtr<STextBlock>> ActionTexts;
    int32 PromptVisibleLineCount = 1;
    ESessionStatus CurrentSessionStatus = ESessionStatus::Unknown;
    EPanelView CurrentView = EPanelView::Main;
};
