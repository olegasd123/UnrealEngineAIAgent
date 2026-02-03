#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class STextBlock;
class SEditableTextBox;

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
    FReply OnRunSceneModifyActorClicked();
    void HandleHealthResult(bool bOk, const FString& Message);
    void HandlePlanResult(bool bOk, const FString& Message);
    TArray<FString> CollectSelectedActorNames() const;

    TSharedPtr<STextBlock> StatusText;
    TSharedPtr<SEditableTextBox> PromptInput;
    TSharedPtr<STextBlock> PlanText;
};
