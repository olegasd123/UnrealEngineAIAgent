#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class STextBlock;
class SEditableTextBox;
class SCheckBox;
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
    TArray<FString> CollectSelectedActorNames() const;

    TSharedPtr<STextBlock> StatusText;
    TSharedPtr<SEditableTextBox> PromptInput;
    TSharedPtr<STextBlock> PlanText;
    TSharedPtr<SCheckBox> ActionCheck0;
    TSharedPtr<STextBlock> ActionText0;
    TSharedPtr<SCheckBox> ActionCheck1;
    TSharedPtr<STextBlock> ActionText1;
};
