#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class STextBlock;
class SEditableTextBox;
class SCheckBox;
class SVerticalBox;
enum class ECheckBoxState : uint8;

class SUEAIAgentPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SUEAIAgentPanel)
    {
    }
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

private:
    FReply OnCheckHealthClicked();
    FReply OnPlanFromSelectionClicked();
    FReply OnApplyPlannedActionClicked();
    void HandleHealthResult(bool bOk, const FString& Message);
    void HandlePlanResult(bool bOk, const FString& Message);
    void HandleActionApprovalChanged(int32 ActionIndex, ECheckBoxState NewState);
    void UpdateActionApprovalUi();
    void RebuildActionApprovalUi();
    TArray<FString> CollectSelectedActorNames() const;

    TSharedPtr<STextBlock> StatusText;
    TSharedPtr<SEditableTextBox> PromptInput;
    TSharedPtr<STextBlock> PlanText;
    TSharedPtr<SVerticalBox> ActionListBox;
    TArray<TSharedPtr<SCheckBox>> ActionChecks;
    TArray<TSharedPtr<STextBlock>> ActionTexts;
};
