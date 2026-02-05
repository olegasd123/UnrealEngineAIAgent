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
    TSharedRef<SWidget> HandleProviderComboGenerateWidget(TSharedPtr<FString> InItem) const;
    void HandleProviderComboSelectionChanged(TSharedPtr<FString> NewValue, ESelectInfo::Type SelectInfo);
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
    TArray<TSharedPtr<FString>> ProviderItems;
    TSharedPtr<FString> SelectedProviderItem;
    TSharedPtr<SEditableTextBox> PromptInput;
    TSharedPtr<STextBlock> PlanText;
    TSharedPtr<SVerticalBox> ActionListBox;
    TArray<TSharedPtr<SCheckBox>> ActionChecks;
    TArray<TSharedPtr<STextBlock>> ActionTexts;
    bool bAgentModeEnabled = true;
    EPanelView CurrentView = EPanelView::Main;
};
