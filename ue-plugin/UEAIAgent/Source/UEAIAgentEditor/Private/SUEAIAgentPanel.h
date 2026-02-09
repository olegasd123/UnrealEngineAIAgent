#pragma once

#include "CoreMinimal.h"
#include "UEAIAgentTransportModule.h"
#include "Types/SlateEnums.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SWidget.h"

class STextBlock;
class SEditableTextBox;
class SMultiLineEditableTextBox;
class SCheckBox;
class SVerticalBox;
class SWidgetSwitcher;
class SButton;
class SInlineEditableTextBlock;
class ITableRow;
class STableViewBase;
template<typename ItemType>
class SListView;
template<typename OptionType>
class SComboBox;
struct FGeometry;
struct FKeyEvent;
enum class ECheckBoxState : uint8;
struct FUEAIAgentPlannedSceneAction;
struct FUEAIAgentChatSummary;
struct FUEAIAgentChatHistoryEntry;

class SUEAIAgentPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SUEAIAgentPanel)
    {
    }
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    virtual bool SupportsKeyboardFocus() const override;
    virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

private:
    enum class EPanelView : uint8
    {
        Main,
        Settings,
        History
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
    FReply OnOpenHistoryClicked();
    FReply OnBackToMainClicked();
    FReply OnRunWithSelectionClicked();
    FReply OnCreateChatClicked();
    FReply OnRefreshChatsClicked();
    FReply OnArchiveChatClicked();
    FReply OnResumeAgentLoopClicked();
    FReply OnRejectCurrentActionClicked();
    FReply OnApplyPlannedActionClicked();
    FReply OnApproveLowRiskClicked();
    FReply OnRejectAllClicked();
    void HandleHealthResult(bool bOk, const FString& Message);
    void HandleCredentialOperationResult(bool bOk, const FString& Message);
    void HandlePlanResult(bool bOk, const FString& Message);
    void HandleSessionUpdate(bool bOk, const FString& Message);
    void HandleChatOperationResult(bool bOk, const FString& Message);
    void HandleChatHistoryResult(bool bOk, const FString& Message);
    void HandleActionApprovalChanged(int32 ActionIndex, ECheckBoxState NewState);
    void HandleChatSelectionChanged(TSharedPtr<FUEAIAgentChatSummary> InItem, ESelectInfo::Type SelectInfo);
    TSharedRef<ITableRow> HandleGenerateChatRow(
        TSharedPtr<FUEAIAgentChatSummary> InItem,
        const TSharedRef<STableViewBase>& OwnerTable);
    TSharedRef<ITableRow> HandleGenerateChatHistoryRow(
        TSharedPtr<FUEAIAgentChatHistoryEntry> InItem,
        const TSharedRef<STableViewBase>& OwnerTable) const;
    void HandleChatSearchTextChanged(const FText& NewText);
    void HandleArchivedFilterChanged(ECheckBoxState NewState);
    void HandleChatTitleCommitted(const FText& NewText, ETextCommit::Type CommitType, FString ChatId);
    FString GetSelectedProviderCode() const;
    FString GetSelectedProviderLabel() const;
    FString GetSelectedModeCode() const;
    FString GetSelectedModeLabel() const;
    FString GetSelectedModelProvider() const;
    FString GetSelectedModelName() const;
    TSharedRef<SWidget> HandleProviderComboGenerateWidget(TSharedPtr<FString> InItem) const;
    TSharedRef<SWidget> HandleModeComboGenerateWidget(TSharedPtr<FString> InItem) const;
    TSharedRef<SWidget> HandleModelComboGenerateWidget(TSharedPtr<FString> InItem) const;
    void HandleProviderComboSelectionChanged(TSharedPtr<FString> NewValue, ESelectInfo::Type SelectInfo);
    void HandleModeComboSelectionChanged(TSharedPtr<FString> NewValue, ESelectInfo::Type SelectInfo);
    void HandleModelComboSelectionChanged(TSharedPtr<FString> NewValue, ESelectInfo::Type SelectInfo);
    void HandlePromptTextChanged(const FText& NewText);
    void PersistPreferredModels();
    void EnsureActiveChatAndRun(
        const FString& Prompt,
        const FString& Mode,
        const TArray<FString>& RequestActors,
        const FString& Provider,
        const FString& Model);
    void RunWithActiveChat(
        const FString& Prompt,
        const FString& Mode,
        const TArray<FString>& RequestActors,
        const FString& Provider,
        const FString& Model);
    bool TryRestoreLatestChatFromTransport();
    void RebuildModelUi();
    FString BuildModelOptionKey(const FUEAIAgentModelOption& Option) const;
    FString BuildModelItemLabel(const FUEAIAgentModelOption& Option) const;
    FReply HandlePromptKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent);
    void RefreshChatUiFromTransport(bool bKeepCurrentSelection);
    void RebuildChatListItems();
    void RebuildHistoryItems();
    void RefreshActiveChatHistory();
    void UpdateSelectionSummaryText();
    void UpdateChatListStateText();
    void UpdateHistoryStateText();
    bool BeginRenameSelectedChat();
    FString BuildSelectionSummary() const;
    FString BuildActionDetailText(int32 ActionIndex) const;
    bool ShouldShowApprovalUi() const;
    ESessionStatus ParseSessionStatusFromMessage(const FString& Message) const;
    void UpdateActionApprovalUi();
    void RebuildActionApprovalUi();
    bool ExecutePlannedAction(const FUEAIAgentPlannedSceneAction& PlannedAction, FString& OutMessage) const;
    TArray<FString> CollectSelectedActorNames() const;
    EActiveTimerReturnType HandleHealthTimer(double InCurrentTime, float InDeltaTime);
    EActiveTimerReturnType HandleSelectionTimer(double InCurrentTime, float InDeltaTime);
    void SetCurrentView(EPanelView NewView);

    TSharedPtr<STextBlock> StatusText;
    TSharedPtr<SMultiLineEditableTextBox> CredentialText;
    TSharedPtr<SEditableTextBox> ApiKeyInput;
    TSharedPtr<STextBlock> SelectionSummaryText;
    TSharedPtr<SWidgetSwitcher> ViewSwitcher;
    TSharedPtr<SListView<TSharedPtr<FUEAIAgentChatSummary>>> ChatListView;
    TArray<TSharedPtr<FUEAIAgentChatSummary>> ChatListItems;
    TSharedPtr<SListView<TSharedPtr<FUEAIAgentChatHistoryEntry>>> ChatHistoryListView;
    TArray<TSharedPtr<FUEAIAgentChatHistoryEntry>> ChatHistoryItems;
    TSharedPtr<STextBlock> ChatListStateText;
    TSharedPtr<STextBlock> HistoryStateText;
    TSharedPtr<STextBlock> SelectedChatHeaderText;
    TMap<FString, TWeakPtr<SInlineEditableTextBlock>> ChatTitleEditors;
    TSharedPtr<SComboBox<TSharedPtr<FString>>> ProviderCombo;
    TSharedPtr<SComboBox<TSharedPtr<FString>>> ModeCombo;
    TSharedPtr<SComboBox<TSharedPtr<FString>>> ModelCombo;
    TArray<TSharedPtr<FString>> ProviderItems;
    TArray<TSharedPtr<FString>> ModeItems;
    TArray<TSharedPtr<FString>> ModelItems;
    TSharedPtr<FString> SelectedProviderItem;
    TSharedPtr<FString> SelectedModeItem;
    TSharedPtr<FString> SelectedModelItem;
    TSharedPtr<SVerticalBox> ModelChecksBox;
    TMap<FString, FUEAIAgentModelOption> ModelLabelToOption;
    TMap<FString, FUEAIAgentModelOption> ModelKeyToOption;
    TMap<FString, TSharedPtr<SCheckBox>> ModelChecks;
    TSharedPtr<SEditableTextBox> ChatSearchInput;
    TSharedPtr<SMultiLineEditableTextBox> PromptInput;
    TSharedPtr<SButton> RunButton;
    TSharedPtr<SButton> ResumeButton;
    TSharedPtr<SMultiLineEditableTextBox> PlanText;
    TSharedPtr<SVerticalBox> ActionListBox;
    TArray<TSharedPtr<SCheckBox>> ActionChecks;
    TArray<TSharedPtr<STextBlock>> ActionTexts;
    TArray<TSharedPtr<STextBlock>> ActionDetailTexts;
    TArray<bool> ActionExpandedStates;
    int32 PromptVisibleLineCount = 1;
    FString CachedSelectionSummary;
    TArray<FString> LastNonEmptySelection;
    FString ChatSearchFilter;
    bool bIncludeArchivedChats = false;
    bool bIsRefreshingChats = false;
    bool bIsLoadingHistory = false;
    bool bIsRunInFlight = false;
    bool bIsResumeInFlight = false;
    bool bSelectNewestChatOnNextRefresh = false;
    FString ChatListErrorMessage;
    FString HistoryErrorMessage;
    ESessionStatus CurrentSessionStatus = ESessionStatus::Unknown;
    EPanelView CurrentView = EPanelView::Main;
};
