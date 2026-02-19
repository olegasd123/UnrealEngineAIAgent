#include "SUEAIAgentPanel.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "GameFramework/Actor.h"
#include "UEAIAgentSceneTools.h"
#include "UEAIAgentSettings.h"
#include "UEAIAgentTransportModule.h"
#include "Misc/MessageDialog.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Text/RichTextLayoutMarshaller.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DateTime.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/SInlineEditableTextBlock.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

namespace
{
    const TCHAR* ChatUiConfigSection = TEXT("UEAIAgent.UI");
    const TCHAR* ShowChatsOnOpenKey = TEXT("ShowChatsOnOpen");
    constexpr int32 MinVisibleChatRows = 3;
    constexpr int32 DefaultMaxVisibleChatRows = 10;
    constexpr float ChatListRowHeight = 27.0f;
    constexpr float ChatListBorderPadding = 2.0f;

    bool ShouldRefreshChatsForAutoTitle(const FUEAIAgentTransportModule& Transport)
    {
        const FString ActiveChatId = Transport.GetActiveChatId();
        if (ActiveChatId.IsEmpty())
        {
            return false;
        }

        const TArray<FUEAIAgentChatSummary>& Chats = Transport.GetChats();
        for (const FUEAIAgentChatSummary& Chat : Chats)
        {
            if (Chat.Id != ActiveChatId)
            {
                continue;
            }

            const FString NormalizedTitle = Chat.Title.TrimStartAndEnd();
            return NormalizedTitle.IsEmpty() || NormalizedTitle.Equals(TEXT("new chat"), ESearchCase::IgnoreCase);
        }

        return false;
    }

    bool IsReferentialPrompt(const FString& Prompt)
    {
        const FString Lower = Prompt.ToLower();
        return Lower.Contains(TEXT(" it ")) ||
            Lower.StartsWith(TEXT("it ")) ||
            Lower.EndsWith(TEXT(" it")) ||
            Lower.Contains(TEXT(" them ")) ||
            Lower.StartsWith(TEXT("them ")) ||
            Lower.EndsWith(TEXT(" them")) ||
            Lower.Contains(TEXT(" selected")) ||
            Lower.Contains(TEXT(" selection")) ||
            Lower.Contains(TEXT(" previous")) ||
            Lower.Contains(TEXT(" same "));
    }

    FString NormalizeSingleLineStatusText(const FString& Input)
    {
        FString Result = Input;
        Result.ReplaceInline(TEXT("\r"), TEXT(" "));
        Result.ReplaceInline(TEXT("\n"), TEXT(" "));
        while (Result.ReplaceInline(TEXT("  "), TEXT(" ")) > 0)
        {
        }
        Result = Result.TrimStartAndEnd();
        if (Result.Len() > 220)
        {
            Result = Result.Left(217) + TEXT("...");
        }
        return Result;
    }

    bool IsUserCanceledSessionMessage(const FString& Message)
    {
        return Message.Contains(TEXT("stopCondition=user_denied"), ESearchCase::IgnoreCase) ||
            Message.Contains(TEXT("Rejected by user."), ESearchCase::IgnoreCase);
    }

    FString ExtractDecisionMessageBody(const FString& Message)
    {
        const FString Prefix = TEXT("Session:");
        if (!Message.StartsWith(Prefix))
        {
            return NormalizeSingleLineStatusText(Message);
        }

        int32 FirstNewline = INDEX_NONE;
        if (!Message.FindChar(TEXT('\n'), FirstNewline))
        {
            return TEXT("");
        }

        const int32 SecondNewline = Message.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, FirstNewline + 1);
        if (SecondNewline == INDEX_NONE)
        {
            return NormalizeSingleLineStatusText(Message.Mid(FirstNewline + 1));
        }

        FString Body = Message.Mid(SecondNewline + 1);
        const int32 AssistantToken = Body.Find(TEXT("\nAssistant:"), ESearchCase::IgnoreCase);
        if (AssistantToken != INDEX_NONE)
        {
            Body = Body.Left(AssistantToken);
        }

        return NormalizeSingleLineStatusText(Body);
    }

    FString ExtractFailedReasonFromSessionMessage(const FString& Message)
    {
        const FString Body = ExtractDecisionMessageBody(Message);
        if (Body.IsEmpty())
        {
            return TEXT("");
        }

        const FString LastErrorToken = TEXT("Last error:");
        const int32 LastErrorPos = Body.Find(LastErrorToken, ESearchCase::IgnoreCase);
        if (LastErrorPos != INDEX_NONE)
        {
            const FString Reason = NormalizeSingleLineStatusText(Body.Mid(LastErrorPos + LastErrorToken.Len()));
            if (!Reason.IsEmpty())
            {
                return Reason;
            }
        }

        const FString StopConditionToken = TEXT("Stopped by stopCondition=");
        const int32 StopConditionPos = Body.Find(StopConditionToken, ESearchCase::IgnoreCase);
        if (StopConditionPos != INDEX_NONE)
        {
            FString Condition = Body.Mid(StopConditionPos + StopConditionToken.Len());
            int32 EndPos = INDEX_NONE;
            if (Condition.FindChar(TEXT('.'), EndPos))
            {
                Condition = Condition.Left(EndPos);
            }
            Condition = Condition.Replace(TEXT("_"), TEXT(" "));
            Condition = NormalizeSingleLineStatusText(Condition);
            if (!Condition.IsEmpty())
            {
                return FString::Printf(TEXT("stopped by %s."), *Condition.ToLower());
            }
        }

        return Body;
    }

    FString ProviderCodeToLabel(const FString& ProviderCode)
    {
        if (ProviderCode.Equals(TEXT("openai"), ESearchCase::IgnoreCase))
        {
            return TEXT("OpenAI");
        }
        if (ProviderCode.Equals(TEXT("gemini"), ESearchCase::IgnoreCase))
        {
            return TEXT("Gemini");
        }
        if (ProviderCode.Equals(TEXT("local"), ESearchCase::IgnoreCase))
        {
            return TEXT("Local");
        }
        return ProviderCode;
    }

    const TCHAR* PlannedActionTypeToText(EUEAIAgentPlannedActionType Type)
    {
        switch (Type)
        {
        case EUEAIAgentPlannedActionType::ContextGetSceneSummary:
            return TEXT("Read Scene Summary");
        case EUEAIAgentPlannedActionType::ContextGetSelection:
            return TEXT("Read Selection");
        case EUEAIAgentPlannedActionType::EditorUndo:
            return TEXT("Undo");
        case EUEAIAgentPlannedActionType::EditorRedo:
            return TEXT("Redo");
        case EUEAIAgentPlannedActionType::ModifyActor:
            return TEXT("Modify Actor");
        case EUEAIAgentPlannedActionType::CreateActor:
            return TEXT("Create Actor");
        case EUEAIAgentPlannedActionType::DeleteActor:
            return TEXT("Delete Actor");
        case EUEAIAgentPlannedActionType::ModifyComponent:
            return TEXT("Modify Component");
        case EUEAIAgentPlannedActionType::AddActorTag:
            return TEXT("Add Actor Tag");
        case EUEAIAgentPlannedActionType::SetComponentMaterial:
            return TEXT("Set Component Material");
        case EUEAIAgentPlannedActionType::SetComponentStaticMesh:
            return TEXT("Set Component Static Mesh");
        case EUEAIAgentPlannedActionType::SetActorFolder:
            return TEXT("Set Actor Folder");
        case EUEAIAgentPlannedActionType::AddActorLabelPrefix:
            return TEXT("Add Label Prefix");
        case EUEAIAgentPlannedActionType::DuplicateActors:
            return TEXT("Duplicate Actors");
        case EUEAIAgentPlannedActionType::SetDirectionalLightIntensity:
            return TEXT("Set Directional Light Intensity");
        case EUEAIAgentPlannedActionType::SetFogDensity:
            return TEXT("Set Fog Density");
        case EUEAIAgentPlannedActionType::SetPostProcessExposureCompensation:
            return TEXT("Set Exposure Compensation");
        case EUEAIAgentPlannedActionType::LandscapeSculpt:
            return TEXT("Landscape Sculpt");
        case EUEAIAgentPlannedActionType::LandscapePaintLayer:
            return TEXT("Landscape Paint Layer");
        case EUEAIAgentPlannedActionType::LandscapeGenerate:
            return TEXT("Landscape Generate");
        case EUEAIAgentPlannedActionType::SessionBeginTransaction:
            return TEXT("Begin Internal Transaction");
        case EUEAIAgentPlannedActionType::SessionCommitTransaction:
            return TEXT("Commit Internal Transaction");
        case EUEAIAgentPlannedActionType::SessionRollbackTransaction:
            return TEXT("Rollback Internal Transaction");
        default:
            return TEXT("Unknown");
        }
    }

    const TCHAR* RiskLevelToText(EUEAIAgentRiskLevel Risk)
    {
        switch (Risk)
        {
        case EUEAIAgentRiskLevel::Low:
            return TEXT("Low");
        case EUEAIAgentRiskLevel::Medium:
            return TEXT("Medium");
        case EUEAIAgentRiskLevel::High:
            return TEXT("High");
        default:
            return TEXT("Unknown");
        }
    }

    const TCHAR* ActionStateToText(EUEAIAgentActionState State)
    {
        switch (State)
        {
        case EUEAIAgentActionState::Pending:
            return TEXT("Pending");
        case EUEAIAgentActionState::Succeeded:
            return TEXT("Succeeded");
        case EUEAIAgentActionState::Failed:
            return TEXT("Failed");
        default:
            return TEXT("Unknown");
        }
    }

    void TryRollbackInternalTransaction()
    {
        FString RollbackMessage;
        FUEAIAgentSceneTools::SessionRollbackTransaction(RollbackMessage);
    }

    void AppendEscapedRichChar(FString& Out, TCHAR Ch)
    {
        if (Ch == TEXT('&'))
        {
            Out += TEXT("&amp;");
            return;
        }
        if (Ch == TEXT('<'))
        {
            Out += TEXT("&lt;");
            return;
        }
        if (Ch == TEXT('>'))
        {
            Out += TEXT("&gt;");
            return;
        }
        Out.AppendChar(Ch);
    }

    FString EscapeRichText(const FString& Source)
    {
        FString Out;
        Out.Reserve(Source.Len());
        for (int32 Index = 0; Index < Source.Len(); ++Index)
        {
            AppendEscapedRichChar(Out, Source[Index]);
        }
        return Out;
    }

    FString ParseInlineMarkdown(const FString& Source)
    {
        FString Out;
        Out.Reserve(Source.Len() + 32);

        int32 Index = 0;
        while (Index < Source.Len())
        {
            if (Source[Index] == TEXT('`'))
            {
                const int32 Close = Source.Find(TEXT("`"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index + 1);
                if (Close != INDEX_NONE && Close > Index + 1)
                {
                    const FString Code = Source.Mid(Index + 1, Close - Index - 1);
                    Out += TEXT("<md.code>");
                    Out += EscapeRichText(Code);
                    Out += TEXT("</>");
                    Index = Close + 1;
                    continue;
                }
            }

            if (Index + 1 < Source.Len() && Source[Index] == TEXT('*') && Source[Index + 1] == TEXT('*'))
            {
                const int32 Close = Source.Find(TEXT("**"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index + 2);
                if (Close != INDEX_NONE && Close > Index + 2)
                {
                    const FString Bold = Source.Mid(Index + 2, Close - Index - 2);
                    Out += TEXT("<md.bold>");
                    Out += EscapeRichText(Bold);
                    Out += TEXT("</>");
                    Index = Close + 2;
                    continue;
                }
            }

            if (Source[Index] == TEXT('*'))
            {
                const int32 Close = Source.Find(TEXT("*"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index + 1);
                if (Close != INDEX_NONE && Close > Index + 1)
                {
                    const FString Italic = Source.Mid(Index + 1, Close - Index - 1);
                    Out += TEXT("<md.italic>");
                    Out += EscapeRichText(Italic);
                    Out += TEXT("</>");
                    Index = Close + 1;
                    continue;
                }
            }

            AppendEscapedRichChar(Out, Source[Index]);
            ++Index;
        }

        return Out;
    }

    bool IsMarkdownTableRow(const FString& Line)
    {
        const FString Trimmed = Line.TrimStartAndEnd();
        if (Trimmed.IsEmpty())
        {
            return false;
        }

        int32 PipeCount = 0;
        for (int32 Index = 0; Index < Trimmed.Len(); ++Index)
        {
            if (Trimmed[Index] == TEXT('|'))
            {
                ++PipeCount;
            }
        }
        return PipeCount >= 2;
    }

    bool IsMarkdownTableSeparator(const FString& Line)
    {
        FString Compact = Line.TrimStartAndEnd().Replace(TEXT(" "), TEXT(""));
        if (Compact.IsEmpty() || !Compact.Contains(TEXT("|")))
        {
            return false;
        }

        bool bHasDash = false;
        for (int32 Index = 0; Index < Compact.Len(); ++Index)
        {
            const TCHAR Ch = Compact[Index];
            if (Ch == TEXT('|'))
            {
                continue;
            }
            if (Ch == TEXT('-'))
            {
                bHasDash = true;
                continue;
            }
            if (Ch == TEXT(':'))
            {
                continue;
            }
            return false;
        }
        return bHasDash;
    }

    void ParseMarkdownTableCells(const FString& Line, TArray<FString>& OutCells)
    {
        OutCells.Empty();

        FString Work = Line.TrimStartAndEnd();
        if (Work.StartsWith(TEXT("|")))
        {
            Work = Work.Mid(1);
        }
        if (Work.EndsWith(TEXT("|")))
        {
            Work = Work.LeftChop(1);
        }

        Work.ParseIntoArray(OutCells, TEXT("|"), false);
        for (FString& Cell : OutCells)
        {
            Cell = Cell.TrimStartAndEnd();
        }
    }

    FString BuildMarkdownTableRowText(const TArray<FString>& Headers, const TArray<FString>& Cells)
    {
        FString RowText;
        for (int32 CellIndex = 0; CellIndex < Cells.Num(); ++CellIndex)
        {
            if (CellIndex > 0)
            {
                RowText += TEXT("  ");
            }

            if (Headers.IsValidIndex(CellIndex))
            {
                const FString Header = Headers[CellIndex].TrimStartAndEnd();
                if (!Header.IsEmpty())
                {
                    RowText += TEXT("<md.bold>");
                    RowText += ParseInlineMarkdown(Header);
                    RowText += TEXT(":</> ");
                }
            }

            RowText += ParseInlineMarkdown(Cells[CellIndex]);
        }

        return RowText;
    }

    FString ConvertMarkdownToRichText(const FString& Source)
    {
        const FString Normalized = Source.Replace(TEXT("\r\n"), TEXT("\n")).Replace(TEXT("\r"), TEXT("\n"));
        TArray<FString> Lines;
        Normalized.ParseIntoArray(Lines, TEXT("\n"), false);

        FString Out;
        bool bInCodeBlock = false;
        for (int32 LineIndex = 0; LineIndex < Lines.Num(); ++LineIndex)
        {
            const FString& Line = Lines[LineIndex];
            const FString Trimmed = Line.TrimStartAndEnd();

            if (Trimmed.StartsWith(TEXT("```")))
            {
                bInCodeBlock = !bInCodeBlock;
                if (LineIndex + 1 < Lines.Num())
                {
                    Out += TEXT("\n");
                }
                continue;
            }

            if (bInCodeBlock)
            {
                Out += TEXT("<md.code>");
                Out += EscapeRichText(Line);
                Out += TEXT("</>");
            }
            else if (LineIndex + 1 < Lines.Num() &&
                IsMarkdownTableRow(Trimmed) &&
                IsMarkdownTableSeparator(Lines[LineIndex + 1].TrimStartAndEnd()))
            {
                TArray<FString> Headers;
                ParseMarkdownTableCells(Trimmed, Headers);

                int32 RowIndex = LineIndex + 2;
                bool bHasDataRows = false;
                while (RowIndex < Lines.Num())
                {
                    const FString RowTrimmed = Lines[RowIndex].TrimStartAndEnd();
                    if (!IsMarkdownTableRow(RowTrimmed))
                    {
                        break;
                    }

                    TArray<FString> Cells;
                    ParseMarkdownTableCells(RowTrimmed, Cells);
                    if (Cells.Num() > 0)
                    {
                        Out += TEXT("• ");
                        Out += BuildMarkdownTableRowText(Headers, Cells);
                        bHasDataRows = true;
                        if (RowIndex + 1 < Lines.Num())
                        {
                            Out += TEXT("\n");
                        }
                    }

                    ++RowIndex;
                }

                if (!bHasDataRows)
                {
                    Out += ParseInlineMarkdown(Trimmed);
                    if (LineIndex + 2 < Lines.Num())
                    {
                        Out += TEXT("\n");
                    }
                }

                LineIndex = bHasDataRows ? (RowIndex - 1) : (LineIndex + 1);
                continue;
            }
            else if (Trimmed.StartsWith(TEXT("# ")))
            {
                Out += TEXT("<md.bold>");
                Out += ParseInlineMarkdown(Trimmed.Mid(2));
                Out += TEXT("</>");
            }
            else if (Trimmed.StartsWith(TEXT("## ")))
            {
                Out += TEXT("<md.bold>");
                Out += ParseInlineMarkdown(Trimmed.Mid(3));
                Out += TEXT("</>");
            }
            else if (Trimmed.StartsWith(TEXT("### ")))
            {
                Out += TEXT("<md.bold>");
                Out += ParseInlineMarkdown(Trimmed.Mid(4));
                Out += TEXT("</>");
            }
            else if (Trimmed.StartsWith(TEXT("- ")) || Trimmed.StartsWith(TEXT("* ")))
            {
                Out += TEXT("• ");
                Out += ParseInlineMarkdown(Trimmed.Mid(2));
            }
            else
            {
                Out += ParseInlineMarkdown(Line);
            }

            if (LineIndex + 1 < Lines.Num())
            {
                Out += TEXT("\n");
            }
        }

        return Out;
    }

    const ISlateStyle& GetChatMarkdownStyle()
    {
        static TSharedPtr<FSlateStyleSet> StyleSet;
        if (!StyleSet.IsValid())
        {
            StyleSet = MakeShared<FSlateStyleSet>(TEXT("UEAIAgentChatMarkdownStyle"));

            const FTextBlockStyle Base = FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>(TEXT("NormalText"));

            FTextBlockStyle Normal = Base;
            Normal.SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 10));
            StyleSet->Set(TEXT("md.normal"), Normal);

            FTextBlockStyle Bold = Base;
            Bold.SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 10));
            StyleSet->Set(TEXT("md.bold"), Bold);

            FTextBlockStyle Italic = Base;
            Italic.SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Italic"), 10));
            StyleSet->Set(TEXT("md.italic"), Italic);

            FTextBlockStyle Code = Base;
            Code.SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Mono"), 10));
            Code.SetColorAndOpacity(FLinearColor(0.84f, 0.91f, 1.0f, 1.0f));
            StyleSet->Set(TEXT("md.code"), Code);
        }
        return *StyleSet.Get();
    }

    const FTableRowStyle& GetChatListRowStyle()
    {
        static FTableRowStyle RowStyle = []()
        {
            FTableRowStyle Style = FCoreStyle::Get().GetWidgetStyle<FTableRowStyle>(TEXT("TableView.Row"));
            const FSlateColorBrush SelectedBrush(FLinearColor(0.02f, 0.02f, 0.02f, 0.95f));
            const FSlateColorBrush SelectedHoveredBrush(FLinearColor(0.03f, 0.03f, 0.03f, 0.95f));
            const FSlateColorBrush TransparentBrush(FLinearColor::Transparent);
            Style.SetActiveBrush(SelectedBrush);
            Style.SetInactiveBrush(SelectedBrush);
            Style.SetActiveHoveredBrush(SelectedHoveredBrush);
            Style.SetInactiveHoveredBrush(SelectedHoveredBrush);
            Style.SetSelectorFocusedBrush(TransparentBrush);
            return Style;
        }();
        return RowStyle;
    }

    FString BuildRelativeTimeLabel(const FString& IsoTimestamp)
    {
        if (IsoTimestamp.IsEmpty())
        {
            return TEXT("");
        }

        FDateTime ActivityUtc;
        if (!FDateTime::ParseIso8601(*IsoTimestamp, ActivityUtc))
        {
            return TEXT("");
        }

        const FDateTime NowUtc = FDateTime::UtcNow();
        if (ActivityUtc >= NowUtc)
        {
            return TEXT("today");
        }

        const FTimespan Delta = NowUtc - ActivityUtc;
        const int32 Days = FMath::Max(0, FMath::FloorToInt(Delta.GetTotalDays()));
        if (Days == 0)
        {
            return TEXT("today");
        }
        if (Days == 1)
        {
            return TEXT("yesterday");
        }
        if (Days < 7)
        {
            return FString::Printf(TEXT("%d days ago"), Days);
        }
        if (Days < 14)
        {
            return TEXT("last week");
        }
        if (Days < 30)
        {
            const int32 Weeks = FMath::Max(2, Days / 7);
            return FString::Printf(TEXT("%d weeks ago"), Weeks);
        }
        if (Days < 60)
        {
            return TEXT("last month");
        }
        if (Days < 365)
        {
            const int32 Months = FMath::Max(2, Days / 30);
            return FString::Printf(TEXT("%d months ago"), Months);
        }
        if (Days < 730)
        {
            return TEXT("a year ago");
        }
        return TEXT("more than a year ago");
    }

}

void SUEAIAgentPanel::Construct(const FArguments& InArgs)
{
    ProviderItems.Empty();
    ProviderItems.Add(MakeShared<FString>(TEXT("OpenAI")));
    ProviderItems.Add(MakeShared<FString>(TEXT("Gemini")));
    ProviderItems.Add(MakeShared<FString>(TEXT("Local")));
    ModeItems.Empty();
    ModeItems.Add(MakeShared<FString>(TEXT("Chat")));
    ModeItems.Add(MakeShared<FString>(TEXT("Agent")));
    SelectedModeItem = ModeItems[1];

    const UUEAIAgentSettings* Settings = GetDefault<UUEAIAgentSettings>();
    if (Settings && Settings->DefaultProvider == EUEAIAgentProvider::Gemini)
    {
        SelectedProviderItem = ProviderItems[1];
    }
    else if (Settings && Settings->DefaultProvider == EUEAIAgentProvider::Local)
    {
        SelectedProviderItem = ProviderItems[2];
    }
    else
    {
        SelectedProviderItem = ProviderItems[0];
    }
    bShowChatControls = !Settings || Settings->bShowChatsOnOpen;
    if (GConfig)
    {
        bool bSavedShowChatsOnOpen = bShowChatControls;
        if (GConfig->GetBool(ChatUiConfigSection, ShowChatsOnOpenKey, bSavedShowChatsOnOpen, GEditorPerProjectIni))
        {
            bShowChatControls = bSavedShowChatsOnOpen;
        }
    }

    ChildSlot
    [
        SAssignNew(ViewSwitcher, SWidgetSwitcher)
        + SWidgetSwitcher::Slot()
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(8.0f, 8.0f, 8.0f, 8.0f)
            [
                SNew(SHorizontalBox)
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
                    .Visibility_Lambda([this]()
                    {
                        return bShowChatControls ? EVisibility::Collapsed : EVisibility::Visible;
                    })
                    .Text(FText::FromString(TEXT("Show Chats")))
                    .OnClicked(this, &SUEAIAgentPanel::OnShowChatsClicked)
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(0.0f, 0.0f, 8.0f, 0.0f)
                [
                    SNew(SButton)
                    .Visibility_Lambda([this]()
                    {
                        return bShowChatControls ? EVisibility::Visible : EVisibility::Collapsed;
                    })
                    .Text(FText::FromString(TEXT("Hide Chats")))
                    .OnClicked(this, &SUEAIAgentPanel::OnHideChatsClicked)
                ]
                + SHorizontalBox::Slot()
                .FillWidth(1.0f)
                [
                    SNew(SBox)
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
            .Padding(8.0f, 0.0f, 8.0f, 0.0f)
            [
                SNew(SBox)
                .Visibility_Lambda([this]()
                {
                    return bShowChatControls ? EVisibility::Visible : EVisibility::Collapsed;
                })
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    .Padding(0.0f, 0.0f, 8.0f, 0.0f)
                    [
                        SNew(SButton)
                        .Text(FText::FromString(TEXT("Refresh")))
                        .IsEnabled_Lambda([this]()
                        {
                            return !bIsRefreshingChats;
                        })
                        .OnClicked(this, &SUEAIAgentPanel::OnRefreshChatsClicked)
                    ]
                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    .Padding(0.0f, 0.0f, 8.0f, 0.0f)
                    [
                        SNew(SCheckBox)
                        .IsChecked_Lambda([this]()
                        {
                            return bIncludeArchivedChats ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                        })
                        .OnCheckStateChanged(this, &SUEAIAgentPanel::HandleArchivedFilterChanged)
                        [
                            SNew(STextBlock)
                            .Text(FText::FromString(TEXT("Archived")))
                        ]
                    ]
                    + SHorizontalBox::Slot()
                    .FillWidth(1.0f)
                    [
                        SAssignNew(ChatSearchInput, SEditableTextBox)
                        .HintText(FText::FromString(TEXT("Search chats by title")))
                        .OnTextChanged(this, &SUEAIAgentPanel::HandleChatSearchTextChanged)
                    ]
                ]
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(8.0f, 2.0f, 8.0f, 2.0f)
            [
                SNew(SBox)
                .Visibility_Lambda([this]()
                {
                    return bShowChatControls ? EVisibility::Visible : EVisibility::Collapsed;
                })
                [
                    SAssignNew(ChatListStateText, STextBlock)
                    .AutoWrapText(true)
                ]
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(8.0f, 0.0f, 8.0f, 4.0f)
            [
                SNew(SBox)
                .Visibility_Lambda([this]()
                {
                    return bShowChatControls ? EVisibility::Visible : EVisibility::Collapsed;
                })
                [
                    SNew(SBox)
                    .HeightOverride_Lambda([this]()
                    {
                        int32 MaxRows = DefaultMaxVisibleChatRows;
                        const UUEAIAgentSettings* Settings = GetDefault<UUEAIAgentSettings>();
                        if (Settings)
                        {
                            MaxRows = Settings->ChatListMaxRows;
                        }
                        MaxRows = FMath::Clamp(MaxRows, MinVisibleChatRows, 50);
                        const int32 VisibleRows = FMath::Clamp(ChatListItems.Num(), MinVisibleChatRows, MaxRows);
                        return ChatListBorderPadding + (ChatListRowHeight * static_cast<float>(VisibleRows));
                    })
                    [
                        SNew(SBorder)
                        .Padding(1.0f)
                        .BorderImage(FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox")))
                        .BorderBackgroundColor(FLinearColor(0.15f, 0.15f, 0.15f, 0.45f))
                        [
                            SAssignNew(ChatListView, SListView<TSharedPtr<FUEAIAgentChatSummary>>)
                            .ListItemsSource(&ChatListItems)
                            .OnGenerateRow(this, &SUEAIAgentPanel::HandleGenerateChatRow)
                            .OnSelectionChanged(this, &SUEAIAgentPanel::HandleChatSelectionChanged)
                            .OnMouseButtonDoubleClick(this, &SUEAIAgentPanel::HandleChatListDoubleClicked)
                            .SelectionMode(ESelectionMode::Single)
                        ]
                    ]
                ]
            ]
            + SVerticalBox::Slot()
            .FillHeight(1.0f)
            .Padding(8.0f, 0.0f, 8.0f, 4.0f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 4.0f)
                [
                    SNew(SBox)
                    .Visibility_Lambda([this]()
                    {
                        return bShowChatControls ? EVisibility::Visible : EVisibility::Collapsed;
                    })
                    [
                        SAssignNew(HistoryStateText, STextBlock)
                        .AutoWrapText(true)
                    ]
                ]
                + SVerticalBox::Slot()
                .FillHeight(1.0f)
                [
                    SNew(SBorder)
                    .Padding(1.0f)
                    .BorderImage(FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox")))
                    .BorderBackgroundColor(FLinearColor(0.15f, 0.15f, 0.15f, 0.45f))
                    [
                        SAssignNew(MainChatHistoryListView, SListView<TSharedPtr<FUEAIAgentChatHistoryEntry>>)
                        .ListItemsSource(&ChatHistoryItems)
                        .OnGenerateRow(this, &SUEAIAgentPanel::HandleGenerateChatHistoryRow)
                        .ScrollIntoViewAlignment(EScrollIntoViewAlignment::BottomOrRight)
                        .SelectionMode(ESelectionMode::None)
                    ]
                ]
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(8.0f, 4.0f, 8.0f, 8.0f)
            [
                SNew(SBox)
                .Visibility_Lambda([this]()
                {
                    return bIsRunInFlight
                        ? EVisibility::Collapsed
                        : EVisibility::Visible;
                })
                .HeightOverride_Lambda([this]()
                {
                    const int32 Lines = FMath::Clamp(PromptVisibleLineCount, 1, 10);
                    return 16.0f + (16.0f * Lines);
                })
                [
                    SAssignNew(PromptInput, SMultiLineEditableTextBox)
                    .HintText(FText::FromString(TEXT("Type what to do, or ask a question")))
                    .OnTextChanged(this, &SUEAIAgentPanel::HandlePromptTextChanged)
                    .OnKeyDownHandler(FOnKeyDown::CreateSP(this, &SUEAIAgentPanel::HandlePromptKeyDown))
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
                    .WidthOverride(280.0f)
                    [
                        SAssignNew(ModelCombo, SComboBox<TSharedPtr<FString>>)
                        .OptionsSource(&ModelItems)
                        .InitiallySelectedItem(SelectedModelItem)
                        .OnGenerateWidget(this, &SUEAIAgentPanel::HandleModelComboGenerateWidget)
                        .OnSelectionChanged(this, &SUEAIAgentPanel::HandleModelComboSelectionChanged)
                        [
                            SNew(STextBlock)
                            .Text_Lambda([this]()
                            {
                                if (SelectedModelItem.IsValid())
                                {
                                    return FText::FromString(*SelectedModelItem);
                                }
                                return FText::FromString(TEXT("Select model"));
                            })
                        ]
                    ]
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(0.0f, 0.0f, 8.0f, 0.0f)
                [
                    SNew(SBox)
                    .WidthOverride(120.0f)
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
                    [
                        SAssignNew(RunButton, SButton)
                        .IsEnabled_Lambda([this]()
                        {
                            return !bIsRunInFlight;
                        })
                        .Text_Lambda([this]()
                        {
                            return FText::FromString(bIsRunInFlight ? TEXT("Run (loading...)") : TEXT("Run"));
                        })
                        .OnClicked(this, &SUEAIAgentPanel::OnRunWithSelectionClicked)
                    ]
                ]
                + SHorizontalBox::Slot()
                .FillWidth(1.0f)
                [
                    SNew(SBox)
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                .HAlign(HAlign_Right)
                .VAlign(VAlign_Bottom)
                .Padding(8.0f, 0.0f, 0.0f, 0.0f)
                [
                    SNew(STextBlock)
                    .Text_Lambda([]()
                    {
                        const FString Label = FUEAIAgentTransportModule::Get().GetLastContextUsageLabel();
                        return FText::FromString(Label.IsEmpty() ? TEXT("-") : Label);
                    })
                    .ToolTipText_Lambda([]()
                    {
                        const FString Tooltip = FUEAIAgentTransportModule::Get().GetLastContextUsageTooltip();
                        return FText::FromString(Tooltip.IsEmpty() ? TEXT("Context usage is not available yet.") : Tooltip);
                    })
                    .Justification(ETextJustify::Left)
                ]
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
                SNew(STextBlock)
                .Text(FText::FromString(TEXT("Preferred Models")))
            ]
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(8.0f, 0.0f, 8.0f, 8.0f)
            [
                SNew(SBox)
                .HeightOverride(240.0f)
                [
                    SNew(SScrollBox)
                    + SScrollBox::Slot()
                    [
                        SAssignNew(ModelChecksBox, SVerticalBox)
                    ]
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

    FUEAIAgentTransportModule::Get().CheckHealth(FOnUEAIAgentHealthChecked::CreateSP(
        this,
        &SUEAIAgentPanel::HandleHealthResult));
    RegisterActiveTimer(10.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SUEAIAgentPanel::HandleHealthTimer));
    RegisterActiveTimer(0.75f, FWidgetActiveTimerDelegate::CreateSP(this, &SUEAIAgentPanel::HandleSelectionTimer));
    UpdateSelectionSummaryText();
    UpdateChatListStateText();
    UpdateHistoryStateText();
    OnRefreshChatsClicked();
    FUEAIAgentTransportModule::Get().RefreshModelOptions(
        TEXT(""),
        FOnUEAIAgentCredentialOpFinished::CreateSP(this, &SUEAIAgentPanel::HandleCredentialOperationResult));
    RebuildModelUi();

    UpdateActionApprovalUi();
}

bool SUEAIAgentPanel::SupportsKeyboardFocus() const
{
    return true;
}

FReply SUEAIAgentPanel::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
    (void)MyGeometry;
    const FKey Key = InKeyEvent.GetKey();

    if ((InKeyEvent.IsControlDown() || InKeyEvent.IsCommandDown()) && Key == EKeys::Enter)
    {
        if (CurrentView == EPanelView::Main)
        {
            return OnRunWithSelectionClicked();
        }
    }

    if (Key == EKeys::Escape)
    {
        if (PromptInput.IsValid())
        {
            FSlateApplication::Get().SetKeyboardFocus(PromptInput, EFocusCause::SetDirectly);
        }
        else
        {
            FSlateApplication::Get().ClearKeyboardFocus(EFocusCause::SetDirectly);
        }
        return FReply::Handled();
    }

    const bool bRenameShortcut = Key == EKeys::F2 || Key == EKeys::Enter;
    if (CurrentView == EPanelView::Main && bRenameShortcut && !InKeyEvent.IsControlDown() && !InKeyEvent.IsCommandDown())
    {
        if (BeginRenameSelectedChat())
        {
            return FReply::Handled();
        }
    }

    return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

void SUEAIAgentPanel::SetCurrentView(EPanelView NewView)
{
    CurrentView = NewView;
    if (!ViewSwitcher.IsValid())
    {
        return;
    }

    int32 Index = 0;
    if (CurrentView == EPanelView::Settings)
    {
        Index = 1;
    }
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
    FUEAIAgentTransportModule::Get().RefreshModelOptions(
        GetSelectedProviderCode(),
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
    if (!PromptInput.IsValid())
    {
        return FReply::Handled();
    }
    if (bIsRunInFlight)
    {
        return FReply::Handled();
    }

    CurrentSessionStatus = ESessionStatus::Unknown;
    const FString Prompt = PromptInput->GetText().ToString().TrimStartAndEnd();
    if (Prompt.IsEmpty())
    {
        AppendPanelStatusToHistory(TEXT("Please enter a prompt first."));
        return FReply::Handled();
    }

    const TArray<FString> SelectedActors = CollectSelectedActorNames();
    TArray<FString> RequestActors = SelectedActors;
    if (SelectedActors.Num() > 0)
    {
        LastNonEmptySelection = SelectedActors;
    }
    else if (IsReferentialPrompt(Prompt) && LastNonEmptySelection.Num() > 0)
    {
        RequestActors = LastNonEmptySelection;
    }
    const FString Mode = GetSelectedModeCode();
    const FString Provider = GetSelectedModelProvider();
    const FString Model = GetSelectedModelName();
    if (Provider.IsEmpty() || Model.IsEmpty())
    {
        AppendPanelStatusToHistory(TEXT("Please select a model in Settings first."));
        return FReply::Handled();
    }

    PromptInput->SetText(FText::GetEmpty());
    bIsRunInFlight = true;
    EnsureActiveChatAndRun(
        Prompt,
        Mode,
        RequestActors,
        Provider,
        Model);
    return FReply::Handled();
}

void SUEAIAgentPanel::EnsureActiveChatAndRun(
    const FString& Prompt,
    const FString& Mode,
    const TArray<FString>& RequestActors,
    const FString& Provider,
    const FString& Model)
{
    if (TryRestoreLatestChatFromTransport())
    {
        RunWithActiveChat(Prompt, Mode, RequestActors, Provider, Model);
        return;
    }
    AppendPanelStatusToHistory(TEXT("Loading..."));

    bIsRefreshingChats = true;
    ChatListErrorMessage.Reset();
    UpdateChatListStateText();
    FUEAIAgentTransportModule::Get().RefreshChats(
        bIncludeArchivedChats,
        FOnUEAIAgentChatOpFinished::CreateLambda([this, Prompt, Mode, RequestActors, Provider, Model](bool bOk, const FString& Message)
        {
            HandleChatOperationResult(bOk, Message);
            if (!bOk)
            {
                bIsRunInFlight = false;
                AppendPanelStatusToHistory(TEXT("Error\n") + Message, true);
                return;
            }

            if (TryRestoreLatestChatFromTransport())
            {
                RunWithActiveChat(Prompt, Mode, RequestActors, Provider, Model);
                return;
            }

            AppendPanelStatusToHistory(TEXT("Creating..."));

            bIsRefreshingChats = true;
            ChatListErrorMessage.Reset();
            UpdateChatListStateText();
            FUEAIAgentTransportModule::Get().CreateChat(
                TEXT(""),
                FOnUEAIAgentChatOpFinished::CreateLambda([this, Prompt, Mode, RequestActors, Provider, Model](bool bCreateOk, const FString& CreateMessage)
                {
                    HandleChatOperationResult(bCreateOk, CreateMessage);
                    if (!bCreateOk)
                    {
                        bIsRunInFlight = false;
                        AppendPanelStatusToHistory(TEXT("Error\n") + CreateMessage, true);
                        return;
                    }

                    RunWithActiveChat(Prompt, Mode, RequestActors, Provider, Model);
                }));
        }));
}

void SUEAIAgentPanel::RunWithActiveChat(
    const FString& Prompt,
    const FString& Mode,
    const TArray<FString>& RequestActors,
    const FString& Provider,
    const FString& Model)
{
    AppendPromptToVisibleHistory(Prompt, Mode, Provider, Model);

    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    if (Mode == TEXT("agent"))
    {
        AppendPanelStatusToHistory(TEXT("Starting session..."));
        Transport.StartSession(
            Prompt,
            TEXT("agent"),
            RequestActors,
            Provider,
            Model,
            FOnUEAIAgentSessionUpdated::CreateSP(this, &SUEAIAgentPanel::HandleSessionUpdate));
        return;
    }

    AppendPanelStatusToHistory(TEXT("Requesting..."));
    Transport.PlanTask(
        Prompt,
        TEXT("chat"),
        RequestActors,
        Provider,
        Model,
        FOnUEAIAgentTaskPlanned::CreateSP(this, &SUEAIAgentPanel::HandlePlanResult));
}

void SUEAIAgentPanel::AppendPromptToVisibleHistory(
    const FString& Prompt,
    const FString& Mode,
    const FString& Provider,
    const FString& Model)
{
    const FString PromptText = Prompt.TrimStartAndEnd();
    if (PromptText.IsEmpty())
    {
        return;
    }

    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    if (Transport.GetActiveChatId().IsEmpty())
    {
        return;
    }

    FUEAIAgentChatHistoryEntry Entry;
    Entry.Kind = TEXT("asked");
    Entry.Route = Mode.Equals(TEXT("agent"), ESearchCase::IgnoreCase) ? TEXT("/v1/session/start") : TEXT("/v1/task/plan");
    Entry.Summary = PromptText;
    Entry.Provider = Provider.TrimStartAndEnd();
    Entry.Model = Model.TrimStartAndEnd();
    Entry.ChatType = Mode.Equals(TEXT("agent"), ESearchCase::IgnoreCase) ? TEXT("agent") : TEXT("chat");
    Entry.DisplayRole = TEXT("user");
    Entry.DisplayText = PromptText;
    Entry.CreatedAt = FDateTime::UtcNow().ToIso8601();

    ChatHistoryItems.Add(MakeShared<FUEAIAgentChatHistoryEntry>(Entry));
    if (MainChatHistoryListView.IsValid())
    {
        MainChatHistoryListView->RequestListRefresh();
    }
    ScrollHistoryViewsToBottom();
    if (!bHistoryAutoScrollPending && ChatHistoryItems.Num() > 0)
    {
        bHistoryAutoScrollPending = true;
        RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SUEAIAgentPanel::HandleDeferredHistoryScroll));
    }
    UpdateHistoryStateText();
}

void SUEAIAgentPanel::AppendPanelStatusToHistory(const FString& StatusText, bool bPersistToChat)
{
    const FString MessageText = StatusText.TrimStartAndEnd();
    if (MessageText.IsEmpty())
    {
        return;
    }

    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    if (Transport.GetActiveChatId().IsEmpty())
    {
        TryRestoreLatestChatFromTransport();
    }

    if (ChatHistoryItems.Num() > 0)
    {
        const TSharedPtr<FUEAIAgentChatHistoryEntry>& LastItem = ChatHistoryItems.Last();
        if (LastItem.IsValid() &&
            LastItem->DisplayRole.Equals(TEXT("assistant"), ESearchCase::IgnoreCase) &&
            LastItem->DisplayText.Equals(MessageText, ESearchCase::CaseSensitive))
        {
            return;
        }
    }

    FUEAIAgentChatHistoryEntry Entry;
    Entry.Kind = TEXT("done");
    Entry.Route = TEXT("/v1/ui/status");
    Entry.Summary = NormalizeSingleLineStatusText(MessageText);
    Entry.Provider = GetSelectedModelProvider();
    Entry.Model = GetSelectedModelName();
    Entry.ChatType = GetSelectedModeCode();
    Entry.DisplayRole = TEXT("assistant");
    Entry.DisplayText = MessageText;
    Entry.CreatedAt = FDateTime::UtcNow().ToIso8601();

    ChatHistoryItems.Add(MakeShared<FUEAIAgentChatHistoryEntry>(Entry));
    if (MainChatHistoryListView.IsValid())
    {
        MainChatHistoryListView->RequestListRefresh();
    }
    ScrollHistoryViewsToBottom();
    if (!bHistoryAutoScrollPending && ChatHistoryItems.Num() > 0)
    {
        bHistoryAutoScrollPending = true;
        RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SUEAIAgentPanel::HandleDeferredHistoryScroll));
    }
    UpdateHistoryStateText();

    if (!bPersistToChat || Transport.GetActiveChatId().IsEmpty())
    {
        return;
    }

    const FString PersistSummary = Entry.Summary.IsEmpty() ? MessageText : Entry.Summary;
    Transport.AppendActiveChatAssistantMessage(
        Entry.Route,
        PersistSummary,
        MessageText,
        Entry.Provider,
        Entry.Model,
        Entry.ChatType,
        FOnUEAIAgentChatOpFinished::CreateLambda([this](bool bOk, const FString& Message)
        {
            (void)Message;
            if (!bOk)
            {
                return;
            }
            RefreshActiveChatHistory();
        }));
}

bool SUEAIAgentPanel::TryRestoreLatestChatFromTransport()
{
    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    if (!Transport.GetActiveChatId().IsEmpty())
    {
        return true;
    }

    const TArray<FUEAIAgentChatSummary>& Chats = Transport.GetChats();
    if (Chats.Num() == 0)
    {
        return false;
    }

    const FUEAIAgentChatSummary* LatestChat = &Chats[0];
    for (const FUEAIAgentChatSummary& Chat : Chats)
    {
        if (Chat.LastActivityAt > LatestChat->LastActivityAt)
        {
            LatestChat = &Chat;
            continue;
        }
        if (Chat.LastActivityAt == LatestChat->LastActivityAt && Chat.Id < LatestChat->Id)
        {
            LatestChat = &Chat;
        }
    }

    if (LatestChat->Id.IsEmpty())
    {
        return false;
    }

    Transport.SetActiveChatId(LatestChat->Id);
    RefreshChatUiFromTransport(true);
    RefreshActiveChatHistory();
    return true;
}

FReply SUEAIAgentPanel::OnCreateChatClicked()
{
    if (bIsRefreshingChats || bIsLoadingHistory)
    {
        return FReply::Handled();
    }

    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    if (!Transport.GetActiveChatId().IsEmpty() && Transport.GetActiveChatHistory().Num() == 0)
    {
        return FReply::Handled();
    }

    bIsRefreshingChats = true;
    bSelectNewestChatOnNextRefresh = true;
    ChatSearchFilter.Reset();
    if (ChatSearchInput.IsValid())
    {
        ChatSearchInput->SetText(FText::GetEmpty());
    }
    ChatListErrorMessage.Reset();
    UpdateChatListStateText();
    Transport.CreateChat(TEXT(""), FOnUEAIAgentChatOpFinished::CreateLambda([this](bool bOk, const FString& Message)
    {
        if (!bOk)
        {
            HandleChatOperationResult(false, Message);
            return;
        }

        HandleChatOperationResult(true, Message);
    }));
    return FReply::Handled();
}

FReply SUEAIAgentPanel::OnShowChatsClicked()
{
    SetChatControlsVisible(true);
    return FReply::Handled();
}

FReply SUEAIAgentPanel::OnHideChatsClicked()
{
    SetChatControlsVisible(false);
    return FReply::Handled();
}

void SUEAIAgentPanel::SetChatControlsVisible(bool bVisible)
{
    bShowChatControls = bVisible;

    UUEAIAgentSettings* Settings = GetMutableDefault<UUEAIAgentSettings>();
    if (Settings && Settings->bShowChatsOnOpen != bVisible)
    {
        Settings->bShowChatsOnOpen = bVisible;
        Settings->SaveConfig();
    }

    if (GConfig)
    {
        GConfig->SetBool(ChatUiConfigSection, ShowChatsOnOpenKey, bVisible, GEditorPerProjectIni);
        GConfig->Flush(false, GEditorPerProjectIni);
    }
}

FReply SUEAIAgentPanel::OnRefreshChatsClicked()
{
    bIsRefreshingChats = true;
    ChatListErrorMessage.Reset();
    UpdateChatListStateText();
    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    Transport.RefreshChats(bIncludeArchivedChats, FOnUEAIAgentChatOpFinished::CreateSP(this, &SUEAIAgentPanel::HandleChatOperationResult));
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
        FOnUEAIAgentCredentialOpFinished::CreateLambda([this](bool bOk, const FString& Message)
        {
            HandleCredentialOperationResult(bOk, Message);
            if (!bOk)
            {
                return;
            }
        }));
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
        FOnUEAIAgentCredentialOpFinished::CreateLambda([this](bool bOk, const FString& Message)
        {
            HandleCredentialOperationResult(bOk, Message);
            if (!bOk)
            {
                return;
            }
        }));
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

void SUEAIAgentPanel::PersistPreferredModels()
{
    if (!CredentialText.IsValid())
    {
        return;
    }

    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    TArray<FUEAIAgentModelOption> SelectedModels;
    const FString CurrentProvider = GetSelectedProviderCode();

    for (const FUEAIAgentModelOption& Existing : Transport.GetPreferredModels())
    {
        if (!Existing.Provider.Equals(CurrentProvider, ESearchCase::IgnoreCase))
        {
            SelectedModels.Add(Existing);
        }
    }

    for (const TPair<FString, TSharedPtr<SCheckBox>>& Entry : ModelChecks)
    {
        if (!Entry.Value.IsValid() || !Entry.Value->IsChecked())
        {
            continue;
        }

        const FUEAIAgentModelOption* Option = ModelKeyToOption.Find(Entry.Key);
        if (!Option)
        {
            continue;
        }
        SelectedModels.Add(*Option);
    }

    CredentialText->SetText(FText::FromString(TEXT("Credential: saving preferred models...")));
    FUEAIAgentTransportModule::Get().SavePreferredModels(
        SelectedModels,
        FOnUEAIAgentCredentialOpFinished::CreateLambda([this](bool bOk, const FString& Message)
        {
            HandleCredentialOperationResult(bOk, Message);
            if (!bOk)
            {
                return;
            }
        }));
}

FReply SUEAIAgentPanel::OnApplyPlannedActionClicked()
{
    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    if (Transport.GetPlannedActionCount() == 0)
    {
        AppendPanelStatusToHistory(TEXT("Execute: error\nNo planned actions. Use 'Run' first."), true);
        return FReply::Handled();
    }

    TArray<FUEAIAgentPlannedSceneAction> ApprovedActions;
    if (!Transport.PopApprovedPlannedActions(ApprovedActions))
    {
        Transport.ClearPlannedActions();
        UpdateActionApprovalUi();
        const FString StatusMessage = TEXT("Canceled.");
        AppendChatOutcomeToHistory(StatusMessage);
        return FReply::Handled();
    }

    int32 SuccessCount = 0;
    int32 FailedCount = 0;
    FString FirstFailureReason;
    FString LastSuccessMessage;
    for (const FUEAIAgentPlannedSceneAction& PlannedAction : ApprovedActions)
    {
        FString ResultMessage;
        const bool bOk = ExecutePlannedAction(PlannedAction, ResultMessage);

        if (bOk)
        {
            ++SuccessCount;
            const FString NormalizedSuccess = NormalizeSingleLineStatusText(ResultMessage);
            if (!NormalizedSuccess.IsEmpty())
            {
                LastSuccessMessage = NormalizedSuccess;
            }
            continue;
        }

        ++FailedCount;
        TryRollbackInternalTransaction();
        if (FirstFailureReason.IsEmpty())
        {
            const FString NormalizedReason = NormalizeSingleLineStatusText(ResultMessage);
            FirstFailureReason = NormalizedReason.IsEmpty() ? TEXT("operation could not be applied.") : NormalizedReason;
        }
    }

    UpdateActionApprovalUi();
    if (FailedCount == 0 && SuccessCount == ApprovedActions.Num())
    {
        const FString StatusMessage = (ApprovedActions.Num() == 1 && !LastSuccessMessage.IsEmpty())
            ? LastSuccessMessage
            : TEXT("Completed.");
        AppendChatOutcomeToHistory(StatusMessage);
    }
    else
    {
        FString StatusMessage;
        if (ApprovedActions.Num() > 1)
        {
            StatusMessage = FString::Printf(TEXT("Failed: %d of %d action(s) failed. %s"), FailedCount, ApprovedActions.Num(), *FirstFailureReason);
        }
        else
        {
            StatusMessage = FString::Printf(TEXT("Failed: %s"), *FirstFailureReason);
        }
        AppendChatOutcomeToHistory(StatusMessage);
    }

    return FReply::Handled();
}

FReply SUEAIAgentPanel::OnCancelPlannedActionClicked()
{
    return OnRejectAllClicked();
}

FReply SUEAIAgentPanel::OnApproveLowRiskClicked()
{
    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    const int32 ActionCount = Transport.GetPlannedActionCount();
    for (int32 ActionIndex = 0; ActionIndex < ActionCount; ++ActionIndex)
    {
        Transport.SetPlannedActionApproved(ActionIndex, true);
    }

    UpdateActionApprovalUi();
    return FReply::Handled();
}

FReply SUEAIAgentPanel::OnRejectAllClicked()
{
    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    const int32 ActionCount = Transport.GetPlannedActionCount();
    if (!Transport.HasActiveSession() && ActionCount > 0)
    {
        Transport.ClearPlannedActions();
        UpdateActionApprovalUi();
        const FString StatusMessage = TEXT("Canceled.");
        AppendChatOutcomeToHistory(StatusMessage);
        return FReply::Handled();
    }

    for (int32 ActionIndex = 0; ActionIndex < ActionCount; ++ActionIndex)
    {
        Transport.SetPlannedActionApproved(ActionIndex, false);
    }

    UpdateActionApprovalUi();
    return FReply::Handled();
}

FReply SUEAIAgentPanel::OnResumeAgentLoopClicked()
{
    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    if (!Transport.HasActiveSession())
    {
        AppendPanelStatusToHistory(TEXT("No active session. Click Run first."), true);
        return FReply::Handled();
    }
    if (bIsResumeInFlight)
    {
        return FReply::Handled();
    }

    bIsResumeInFlight = true;
    const int32 PendingActionIndex = Transport.GetNextPendingActionIndex();
    if (PendingActionIndex != INDEX_NONE && !Transport.IsPlannedActionApproved(PendingActionIndex))
    {
        bIsResumeInFlight = false;
        AppendPanelStatusToHistory(TEXT("Pending action is not approved. Check it or click Reject."), true);
        return FReply::Handled();
    }

    Transport.ApproveCurrentSessionAction(
        true,
        FOnUEAIAgentSessionUpdated::CreateLambda([this](bool bOk, const FString& Message)
        {
            if (!bOk)
            {
                bIsResumeInFlight = false;
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
    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    if (!Transport.HasActiveSession())
    {
        AppendPanelStatusToHistory(TEXT("No active session. Click Run first."), true);
        return FReply::Handled();
    }

    const EAppReturnType::Type ConfirmResult = FMessageDialog::Open(
        EAppMsgType::YesNo,
        FText::FromString(TEXT("Reject the current action? This will cancel the current operation.")));
    if (ConfirmResult != EAppReturnType::Yes)
    {
        return FReply::Handled();
    }

    AppendPanelStatusToHistory(TEXT("Rejecting action..."));
    TryRollbackInternalTransaction();
    Transport.ApproveCurrentSessionAction(
        false,
        FOnUEAIAgentSessionUpdated::CreateSP(this, &SUEAIAgentPanel::HandleSessionUpdate));

    return FReply::Handled();
}

void SUEAIAgentPanel::HandleHealthResult(bool bOk, const FString& Message)
{
    FString DisplayMessage = Message;
    const FString ProviderPrefix = TEXT("Provider:");
    int32 ProviderIndex = INDEX_NONE;
    if (DisplayMessage.FindChar(TEXT('\n'), ProviderIndex))
    {
        DisplayMessage = DisplayMessage.Left(ProviderIndex);
    }
    const int32 ProviderToken = DisplayMessage.Find(ProviderPrefix, ESearchCase::IgnoreCase);
    if (ProviderToken != INDEX_NONE)
    {
        DisplayMessage = DisplayMessage.Left(ProviderToken).TrimEnd();
    }

    if (!bOk)
    {
        AppendPanelStatusToHistory(DisplayMessage);
    }
}

void SUEAIAgentPanel::HandlePlanResult(bool bOk, const FString& Message)
{
    bIsRunInFlight = false;
    CurrentSessionStatus = ESessionStatus::Unknown;
    if (!bOk)
    {
        AppendPanelStatusToHistory(TEXT("Error\n") + Message, true);
        RefreshActiveChatHistory();
        return;
    }

    UpdateActionApprovalUi();

    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    const int32 ActionCount = Transport.GetPlannedActionCount();
    if (ActionCount > 0)
    {
        for (int32 ActionIndex = 0; ActionIndex < ActionCount; ++ActionIndex)
        {
            Transport.SetPlannedActionApproved(ActionIndex, true);
        }
        UpdateActionApprovalUi();
    }

    if (ActionCount <= 0)
    {
        AppendPanelStatusToHistory(TEXT("Done"), true);
    }
    else
    {
        AppendPanelStatusToHistory(Message, true);
    }
    RefreshActiveChatHistory();
    if (!bIsRefreshingChats && ShouldRefreshChatsForAutoTitle(Transport))
    {
        OnRefreshChatsClicked();
    }
}

void SUEAIAgentPanel::HandleSessionUpdate(bool bOk, const FString& Message)
{
    bIsRunInFlight = false;
    bIsResumeInFlight = false;
    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    if (!bIsRefreshingChats && ShouldRefreshChatsForAutoTitle(Transport))
    {
        OnRefreshChatsClicked();
    }
    if (!bOk)
    {
        TryRollbackInternalTransaction();
        CurrentSessionStatus = ESessionStatus::Failed;
        const FString Reason = NormalizeSingleLineStatusText(Message);
        if (Reason.IsEmpty())
        {
            AppendPanelStatusToHistory(TEXT("Failed"), true);
        }
        else
        {
            AppendPanelStatusToHistory(FString::Printf(TEXT("Failed: %s"), *Reason), true);
        }
        RefreshActiveChatHistory();
        return;
    }

    const ESessionStatus PreviousSessionStatus = CurrentSessionStatus;
    CurrentSessionStatus = ParseSessionStatusFromMessage(Message);
    if (PreviousSessionStatus == ESessionStatus::Unknown)
    {
        const int32 ActionCount = Transport.GetPlannedActionCount();
        for (int32 ActionIndex = 0; ActionIndex < ActionCount; ++ActionIndex)
        {
            Transport.SetPlannedActionApproved(ActionIndex, true);
        }
    }
    UpdateActionApprovalUi();

    if (CurrentSessionStatus == ESessionStatus::Failed)
    {
        TryRollbackInternalTransaction();
        const FString DecisionMessage = ExtractDecisionMessageBody(Message);
        if (IsUserCanceledSessionMessage(DecisionMessage))
        {
            AppendPanelStatusToHistory(TEXT("Canceled"), true);
        }
        else
        {
            const FString Reason = ExtractFailedReasonFromSessionMessage(Message);
            if (Reason.IsEmpty())
            {
                AppendPanelStatusToHistory(TEXT("Failed"), true);
            }
            else
            {
                AppendPanelStatusToHistory(FString::Printf(TEXT("Failed: %s"), *Reason), true);
            }
        }
        RefreshActiveChatHistory();
        return;
    }
    if (CurrentSessionStatus == ESessionStatus::Completed)
    {
        TryRollbackInternalTransaction();
        RefreshActiveChatHistory();
        return;
    }
    if (Transport.GetPlannedActionCount() <= 0)
    {
        if (CurrentSessionStatus == ESessionStatus::AwaitingApproval || CurrentSessionStatus == ESessionStatus::ReadyToExecute)
        {
            const FString DecisionMessage = NormalizeSingleLineStatusText(ExtractDecisionMessageBody(Message));
            if (DecisionMessage.IsEmpty())
            {
                AppendPanelStatusToHistory(TEXT("No executable action in session update."));
            }
            else
            {
                AppendPanelStatusToHistory(TEXT("No executable action in session update.\n") + DecisionMessage);
            }
        }
        else
        {
            AppendPanelStatusToHistory(TEXT("Update received."));
        }
        RefreshActiveChatHistory();
        return;
    }

    const int32 PendingActionIndex = Transport.GetNextPendingActionIndex();
    if (PendingActionIndex == INDEX_NONE)
    {
        AppendPanelStatusToHistory(TEXT("Update received."));
        RefreshActiveChatHistory();
        return;
    }

    FUEAIAgentPlannedSceneAction NextAction;
    if (!Transport.GetPendingAction(PendingActionIndex, NextAction))
    {
        AppendPanelStatusToHistory(TEXT("Update received."));
        RefreshActiveChatHistory();
        return;
    }

    if (CurrentSessionStatus == ESessionStatus::AwaitingApproval)
    {
        RefreshActiveChatHistory();
        return;
    }

    if (CurrentSessionStatus != ESessionStatus::ReadyToExecute)
    {
        AppendPanelStatusToHistory(TEXT("Update received."));
        RefreshActiveChatHistory();
        return;
    }

    if (!NextAction.bApproved)
    {
        RefreshActiveChatHistory();
        return;
    }

    FString ExecuteMessage;
    const bool bOkExecute = ExecutePlannedAction(NextAction, ExecuteMessage);
    if (!bOkExecute)
    {
        TryRollbackInternalTransaction();
        CurrentSessionStatus = ESessionStatus::AwaitingApproval;
        UpdateActionApprovalUi();
        AppendPanelStatusToHistory(TEXT("Local execute failed\n") + ExecuteMessage + TEXT("\nFix selection/target and click Resume."), true);
        return;
    }

    AppendPanelStatusToHistory(TEXT("Action executed, syncing..."));
    Transport.NextSession(
        true,
        true,
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
    if (PlannedAction.Type == EUEAIAgentPlannedActionType::ContextGetSceneSummary)
    {
        return FUEAIAgentSceneTools::ContextGetSceneSummary(OutMessage);
    }

    if (PlannedAction.Type == EUEAIAgentPlannedActionType::ContextGetSelection)
    {
        return FUEAIAgentSceneTools::ContextGetSelection(OutMessage);
    }

    if (PlannedAction.Type == EUEAIAgentPlannedActionType::EditorUndo)
    {
        return FUEAIAgentSceneTools::EditorUndo(OutMessage);
    }

    if (PlannedAction.Type == EUEAIAgentPlannedActionType::EditorRedo)
    {
        return FUEAIAgentSceneTools::EditorRedo(OutMessage);
    }

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

    if (PlannedAction.Type == EUEAIAgentPlannedActionType::SetDirectionalLightIntensity)
    {
        FUEAIAgentSetDirectionalLightIntensityParams Params;
        Params.ActorNames = PlannedAction.ActorNames;
        Params.Intensity = PlannedAction.ScalarValue;
        Params.bUseSelectionIfActorNamesEmpty = false;
        if (Params.ActorNames.IsEmpty())
        {
            OutMessage = TEXT("Skipped directional light intensity action with no target actors.");
            return false;
        }
        return FUEAIAgentSceneTools::SceneSetDirectionalLightIntensity(Params, OutMessage);
    }

    if (PlannedAction.Type == EUEAIAgentPlannedActionType::SetFogDensity)
    {
        FUEAIAgentSetFogDensityParams Params;
        Params.ActorNames = PlannedAction.ActorNames;
        Params.Density = PlannedAction.ScalarValue;
        Params.bUseSelectionIfActorNamesEmpty = false;
        if (Params.ActorNames.IsEmpty())
        {
            OutMessage = TEXT("Skipped fog density action with no target actors.");
            return false;
        }
        return FUEAIAgentSceneTools::SceneSetFogDensity(Params, OutMessage);
    }

    if (PlannedAction.Type == EUEAIAgentPlannedActionType::SetPostProcessExposureCompensation)
    {
        FUEAIAgentSetPostProcessExposureCompensationParams Params;
        Params.ActorNames = PlannedAction.ActorNames;
        Params.ExposureCompensation = PlannedAction.ScalarValue;
        Params.bUseSelectionIfActorNamesEmpty = false;
        if (Params.ActorNames.IsEmpty())
        {
            OutMessage = TEXT("Skipped exposure compensation action with no target actors.");
            return false;
        }
        return FUEAIAgentSceneTools::SceneSetPostProcessExposureCompensation(Params, OutMessage);
    }

    if (PlannedAction.Type == EUEAIAgentPlannedActionType::LandscapeSculpt)
    {
        FUEAIAgentLandscapeSculptParams Params;
        Params.ActorNames = PlannedAction.ActorNames;
        Params.Center = PlannedAction.LandscapeCenter;
        Params.Size = PlannedAction.LandscapeSize;
        Params.Strength = PlannedAction.LandscapeStrength;
        Params.Falloff = PlannedAction.LandscapeFalloff;
        Params.bLower = PlannedAction.bLandscapeInvertMode;
        Params.bUseSelectionIfActorNamesEmpty = Params.ActorNames.IsEmpty();
        return FUEAIAgentSceneTools::LandscapeSculpt(Params, OutMessage);
    }

    if (PlannedAction.Type == EUEAIAgentPlannedActionType::LandscapePaintLayer)
    {
        FUEAIAgentLandscapePaintLayerParams Params;
        Params.ActorNames = PlannedAction.ActorNames;
        Params.Center = PlannedAction.LandscapeCenter;
        Params.Size = PlannedAction.LandscapeSize;
        Params.LayerName = PlannedAction.LandscapeLayerName;
        Params.Strength = PlannedAction.LandscapeStrength;
        Params.Falloff = PlannedAction.LandscapeFalloff;
        Params.bRemove = PlannedAction.bLandscapeInvertMode;
        Params.bUseSelectionIfActorNamesEmpty = Params.ActorNames.IsEmpty();
        return FUEAIAgentSceneTools::LandscapePaintLayer(Params, OutMessage);
    }

    if (PlannedAction.Type == EUEAIAgentPlannedActionType::LandscapeGenerate)
    {
        FUEAIAgentLandscapeGenerateParams Params;
        Params.ActorNames = PlannedAction.ActorNames;
        Params.Theme = PlannedAction.LandscapeTheme;
        Params.DetailLevel = PlannedAction.LandscapeDetailLevel;
        Params.MoonProfile = PlannedAction.LandscapeMoonProfile;
        Params.bUseFullArea = PlannedAction.bLandscapeUseFullArea;
        Params.Center = PlannedAction.LandscapeCenter;
        Params.Size = PlannedAction.LandscapeSize;
        Params.Seed = PlannedAction.LandscapeSeed;
        Params.MountainCount = PlannedAction.LandscapeMountainCount;
        Params.MountainStyle = PlannedAction.LandscapeMountainStyle;
        Params.MountainWidthMin = PlannedAction.LandscapeMountainWidthMin;
        Params.MountainWidthMax = PlannedAction.LandscapeMountainWidthMax;
        Params.MaxHeight = PlannedAction.LandscapeMaxHeight;
        Params.CraterCountMin = PlannedAction.LandscapeCraterCountMin;
        Params.CraterCountMax = PlannedAction.LandscapeCraterCountMax;
        Params.CraterWidthMin = PlannedAction.LandscapeCraterWidthMin;
        Params.CraterWidthMax = PlannedAction.LandscapeCraterWidthMax;
        Params.bUseSelectionIfActorNamesEmpty = Params.ActorNames.IsEmpty();
        return FUEAIAgentSceneTools::LandscapeGenerate(Params, OutMessage);
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
    const bool bOkModifyByName = FUEAIAgentSceneTools::SceneModifyActor(Params, OutMessage);
    if (bOkModifyByName)
    {
        return true;
    }

    // Fallback for planner byName misses when the user's active selection is the intended target.
    if (OutMessage.Contains(TEXT("No target actors found."), ESearchCase::IgnoreCase))
    {
        Params.ActorNames.Empty();
        Params.bUseSelectionIfActorNamesEmpty = true;
        FString FallbackMessage;
        const bool bOkSelectionFallback = FUEAIAgentSceneTools::SceneModifyActor(Params, FallbackMessage);
        if (bOkSelectionFallback)
        {
            OutMessage = FString::Printf(TEXT("%s (fallback: used current selection)"), *FallbackMessage);
            return true;
        }
    }

    return false;
}

void SUEAIAgentPanel::AppendChatOutcomeToHistory(const FString& OutcomeText)
{
    const FString NormalizedStatus = NormalizeSingleLineStatusText(OutcomeText);
    if (NormalizedStatus.IsEmpty())
    {
        return;
    }

    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    if (Transport.GetActiveChatId().IsEmpty())
    {
        return;
    }

    const FString PlanSummary = Transport.GetLastPlanSummary().TrimStartAndEnd();
    const FString SummaryText = PlanSummary.IsEmpty() ? NormalizedStatus : PlanSummary;
    const FString DisplayText = PlanSummary.IsEmpty()
        ? NormalizedStatus
        : FString::Printf(TEXT("%s\n%s"), *PlanSummary, *NormalizedStatus);

    Transport.AppendActiveChatAssistantMessage(
        TEXT("/v1/task/apply"),
        SummaryText,
        DisplayText,
        GetSelectedModelProvider(),
        GetSelectedModelName(),
        TEXT("chat"),
        FOnUEAIAgentChatOpFinished::CreateLambda([this](bool bOk, const FString& Message)
        {
            (void)Message;
            if (!bOk)
            {
                return;
            }
            RefreshActiveChatHistory();
        }));
}

void SUEAIAgentPanel::HandleCredentialOperationResult(bool bOk, const FString& Message)
{
    RebuildModelUi();

    if (!CredentialText.IsValid())
    {
        return;
    }

    const FString Prefix = bOk ? TEXT("Credential: ok\n") : TEXT("Credential: error\n");
    CredentialText->SetText(FText::FromString(Prefix + Message));
}

void SUEAIAgentPanel::HandleChatOperationResult(bool bOk, const FString& Message)
{
    bIsRefreshingChats = false;
    if (!bOk)
    {
        bSelectNewestChatOnNextRefresh = false;
        ChatListErrorMessage = TEXT("Chat error: ") + Message;
        UpdateChatListStateText();
        return;
    }

    ChatListErrorMessage.Reset();
    UpdateChatListStateText();
    const bool bKeepCurrentSelection = !bSelectNewestChatOnNextRefresh;
    RefreshChatUiFromTransport(bKeepCurrentSelection);
    bSelectNewestChatOnNextRefresh = false;
    RefreshActiveChatHistory();
}

void SUEAIAgentPanel::HandleChatHistoryResult(bool bOk, const FString& Message)
{
    bIsLoadingHistory = false;
    if (!bOk)
    {
        HistoryErrorMessage = TEXT("Chat history error: ") + Message;
        RebuildHistoryItems();
        UpdateHistoryStateText();
        return;
    }

    HistoryErrorMessage.Reset();
    TryRestoreRunSelectionsFromHistory();
    RebuildHistoryItems();
    UpdateHistoryStateText();
}

void SUEAIAgentPanel::RefreshChatUiFromTransport(bool bKeepCurrentSelection)
{
    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    const FString PreviousActiveId = Transport.GetActiveChatId();

    RebuildChatListItems();
    if (ChatListView.IsValid())
    {
        ChatListView->RequestListRefresh();
    }

    FString ActiveId = PreviousActiveId;
    if (ActiveId.IsEmpty() || !bKeepCurrentSelection)
    {
        ActiveId.Reset();
    }

    bool bFoundActiveChat = false;
    for (const TSharedPtr<FUEAIAgentChatSummary>& Item : ChatListItems)
    {
        if (Item.IsValid() && Item->Id == ActiveId)
        {
            bFoundActiveChat = true;
            break;
        }
    }
    if (!bFoundActiveChat)
    {
        ActiveId = ChatListItems.Num() > 0 && ChatListItems[0].IsValid() ? ChatListItems[0]->Id : TEXT("");
    }
    Transport.SetActiveChatId(ActiveId);

    TSharedPtr<FUEAIAgentChatSummary> ActiveItem;
    for (const TSharedPtr<FUEAIAgentChatSummary>& Item : ChatListItems)
    {
        if (Item.IsValid() && Item->Id == ActiveId)
        {
            ActiveItem = Item;
            break;
        }
    }

    if (ChatListView.IsValid())
    {
        ChatListView->SetSelection(ActiveItem, ESelectInfo::Direct);
    }

    UpdateChatListStateText();
    RebuildHistoryItems();
    UpdateHistoryStateText();
}

void SUEAIAgentPanel::RebuildChatListItems()
{
    ChatListItems.Empty();
    ChatTitleEditors.Empty();

    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    TArray<FUEAIAgentChatSummary> Chats = Transport.GetChats();
    Chats.Sort([](const FUEAIAgentChatSummary& Left, const FUEAIAgentChatSummary& Right)
    {
        if (Left.LastActivityAt != Right.LastActivityAt)
        {
            return Left.LastActivityAt > Right.LastActivityAt;
        }
        return Left.Id < Right.Id;
    });

    const FString FilterLower = ChatSearchFilter.ToLower();
    for (const FUEAIAgentChatSummary& Chat : Chats)
    {
        if (!FilterLower.IsEmpty())
        {
            const FString TitleLower = Chat.Title.ToLower();
            if (!TitleLower.Contains(FilterLower))
            {
                continue;
            }
        }
        ChatListItems.Add(MakeShared<FUEAIAgentChatSummary>(Chat));
    }
}

void SUEAIAgentPanel::RebuildHistoryItems()
{
    ChatHistoryItems.Empty();
    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    const TArray<FUEAIAgentChatHistoryEntry>& Entries = Transport.GetActiveChatHistory();
    for (const FUEAIAgentChatHistoryEntry& Entry : Entries)
    {
        ChatHistoryItems.Add(MakeShared<FUEAIAgentChatHistoryEntry>(Entry));
    }

    if (MainChatHistoryListView.IsValid())
    {
        MainChatHistoryListView->RequestListRefresh();
    }

    ScrollHistoryViewsToBottom();
    if (!bHistoryAutoScrollPending && ChatHistoryItems.Num() > 0)
    {
        bHistoryAutoScrollPending = true;
        RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SUEAIAgentPanel::HandleDeferredHistoryScroll));
    }
}

void SUEAIAgentPanel::ScrollHistoryViewsToBottom()
{
    if (ChatHistoryItems.Num() == 0)
    {
        return;
    }

    const TSharedPtr<FUEAIAgentChatHistoryEntry> LastItem = ChatHistoryItems.Last();
    if (MainChatHistoryListView.IsValid())
    {
        MainChatHistoryListView->ScrollToBottom();
        MainChatHistoryListView->RequestScrollIntoView(LastItem);
    }
}

void SUEAIAgentPanel::RefreshActiveChatHistory()
{
    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    const FString ActiveId = Transport.GetActiveChatId();
    if (ActiveId.IsEmpty())
    {
        bIsLoadingHistory = false;
        bHistoryAutoScrollPending = false;
        HistoryErrorMessage.Reset();
        ChatHistoryItems.Empty();
        if (MainChatHistoryListView.IsValid())
        {
            MainChatHistoryListView->RequestListRefresh();
        }
        UpdateHistoryStateText();
        return;
    }

    bIsLoadingHistory = true;
    HistoryErrorMessage.Reset();
    UpdateHistoryStateText();
    Transport.LoadActiveChatHistory(0, FOnUEAIAgentChatOpFinished::CreateSP(this, &SUEAIAgentPanel::HandleChatHistoryResult));
}

EActiveTimerReturnType SUEAIAgentPanel::HandleDeferredHistoryScroll(double InCurrentTime, float InDeltaTime)
{
    (void)InCurrentTime;
    (void)InDeltaTime;

    bHistoryAutoScrollPending = false;
    ScrollHistoryViewsToBottom();
    return EActiveTimerReturnType::Stop;
}

void SUEAIAgentPanel::UpdateSelectionSummaryText()
{
    const FString NewSummary = BuildSelectionSummary();
    if (CachedSelectionSummary == NewSummary)
    {
        return;
    }

    CachedSelectionSummary = NewSummary;
    if (SelectionSummaryText.IsValid())
    {
        SelectionSummaryText->SetText(FText::FromString(NewSummary));
    }
}

void SUEAIAgentPanel::UpdateChatListStateText()
{
    if (!ChatListStateText.IsValid())
    {
        return;
    }

    if (bIsRefreshingChats)
    {
        ChatListStateText->SetText(FText::FromString(TEXT("Loading chats...")));
        return;
    }

    if (!ChatListErrorMessage.IsEmpty())
    {
        ChatListStateText->SetText(FText::FromString(ChatListErrorMessage));
        return;
    }

    if (ChatListItems.Num() == 0)
    {
        ChatListStateText->SetText(FText::FromString(TEXT("No chats found.")));
        return;
    }

    ChatListStateText->SetText(FText::GetEmpty());
}

void SUEAIAgentPanel::UpdateHistoryStateText()
{
    if (!HistoryStateText.IsValid())
    {
        return;
    }

    if (bIsLoadingHistory)
    {
        HistoryStateText->SetText(FText::FromString(TEXT("Loading history...")));
        return;
    }

    if (!HistoryErrorMessage.IsEmpty())
    {
        HistoryStateText->SetText(FText::FromString(HistoryErrorMessage));
        return;
    }

    if (FUEAIAgentTransportModule::Get().GetActiveChatId().IsEmpty())
    {
        HistoryStateText->SetText(FText::FromString(TEXT("Select a chat to see history.")));
        return;
    }

    if (ChatHistoryItems.Num() == 0)
    {
        HistoryStateText->SetText(FText::GetEmpty());
        return;
    }

    HistoryStateText->SetText(FText::GetEmpty());
}

void SUEAIAgentPanel::HandleChatSelectionChanged(TSharedPtr<FUEAIAgentChatSummary> InItem, ESelectInfo::Type SelectInfo)
{
    (void)SelectInfo;
    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    if (!InItem.IsValid())
    {
        return;
    }

    Transport.SetActiveChatId(InItem->Id);
    RefreshActiveChatHistory();
}

void SUEAIAgentPanel::HandleChatListDoubleClicked(TSharedPtr<FUEAIAgentChatSummary> InItem)
{
    if (!InItem.IsValid())
    {
        return;
    }

    if (ChatListView.IsValid())
    {
        ChatListView->SetSelection(InItem, ESelectInfo::OnMouseClick);
    }

    const FString ChatId = InItem->Id;
    TSharedPtr<SInlineEditableTextBlock> InlineEditor = ChatTitleEditors.FindRef(ChatId).Pin();
    if (InlineEditor.IsValid())
    {
        InlineEditor->EnterEditingMode();
    }
}

TSharedRef<ITableRow> SUEAIAgentPanel::HandleGenerateChatRow(
    TSharedPtr<FUEAIAgentChatSummary> InItem,
    const TSharedRef<STableViewBase>& OwnerTable)
{
    if (!InItem.IsValid())
    {
        return SNew(STableRow<TSharedPtr<FUEAIAgentChatSummary>>, OwnerTable)
        .Style(&GetChatListRowStyle())
        [
            SNew(STextBlock).Text(FText::FromString(TEXT("Invalid chat")))
        ];
    }

    const FString ChatId = InItem->Id;
    const bool bIsArchived = InItem->bArchived;
    TSharedPtr<SInlineEditableTextBlock> InlineTitle;

    TSharedRef<ITableRow> Row = SNew(STableRow<TSharedPtr<FUEAIAgentChatSummary>>, OwnerTable)
    .Style(&GetChatListRowStyle())
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot()
        .AutoWidth()
        .Padding(4.0f, 4.0f, 4.0f, 4.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
            .AutoWidth()
            [
                SAssignNew(InlineTitle, SInlineEditableTextBlock)
                .Text_Lambda([InItem]()
                {
                    return FText::FromString(InItem->Title.IsEmpty() ? TEXT("Untitled chat") : InItem->Title);
                })
                .OnTextCommitted(this, &SUEAIAgentPanel::HandleChatTitleCommitted, ChatId)
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(4.0f, 0.0f, 0.0f, 0.0f)
            [
                SNew(STextBlock)
                .Text_Lambda([InItem]()
                {
                    const FString RelativeTime = BuildRelativeTimeLabel(InItem->LastActivityAt);
                    if (RelativeTime.IsEmpty())
                    {
                        return FText::GetEmpty();
                    }
                    return FText::FromString(FString::Printf(TEXT("(%s)"), *RelativeTime));
                })
                .ColorAndOpacity(FLinearColor(0.22f, 0.22f, 0.22f, 1.0f))
            ]
        ]
        + SHorizontalBox::Slot()
        .FillWidth(1.0f)
        [
            SNew(SBox)
        ]
        + SHorizontalBox::Slot()
        .AutoWidth()
        .Padding(0.0f, 2.0f, 4.0f, 2.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
            .AutoWidth()
            [
                SNew(SButton)
                .Visibility_Lambda([bIsArchived]()
                {
                    return bIsArchived ? EVisibility::Collapsed : EVisibility::Visible;
                })
                .ToolTipText(FText::FromString(TEXT("Archive chat")))
                .ContentPadding(FMargin(4.0f, 2.0f))
                .OnClicked_Lambda([this, ChatId]()
                {
                    const EAppReturnType::Type ConfirmResult = FMessageDialog::Open(
                        EAppMsgType::YesNo,
                        FText::FromString(TEXT("Archive this chat?")));
                    if (ConfirmResult != EAppReturnType::Yes)
                    {
                        return FReply::Handled();
                    }

                    bIsRefreshingChats = true;
                    ChatListErrorMessage.Reset();
                    UpdateChatListStateText();
                    FUEAIAgentTransportModule::Get().ArchiveChat(
                        ChatId,
                        FOnUEAIAgentChatOpFinished::CreateLambda([this](bool bOk, const FString& Message)
                        {
                            HandleChatOperationResult(bOk, Message);
                            if (!bOk)
                            {
                                return;
                            }

                            OnRefreshChatsClicked();
                        }));
                    return FReply::Handled();
                })
                [
                    SNew(SImage)
                    .Image(FAppStyle::Get().GetBrush(TEXT("Icons.Minus")))
                ]
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(0.0f, 0.0f, 6.0f, 0.0f)
            [
                SNew(SButton)
                .Visibility_Lambda([bIsArchived]()
                {
                    return bIsArchived ? EVisibility::Visible : EVisibility::Collapsed;
                })
                .ToolTipText(FText::FromString(TEXT("Restore chat")))
                .ContentPadding(FMargin(4.0f, 2.0f))
                .OnClicked_Lambda([this, ChatId]()
                {
                    const EAppReturnType::Type ConfirmResult = FMessageDialog::Open(
                        EAppMsgType::YesNo,
                        FText::FromString(TEXT("Restore this chat?")));
                    if (ConfirmResult != EAppReturnType::Yes)
                    {
                        return FReply::Handled();
                    }

                    bIsRefreshingChats = true;
                    ChatListErrorMessage.Reset();
                    UpdateChatListStateText();
                    FUEAIAgentTransportModule::Get().RestoreChat(
                        ChatId,
                        FOnUEAIAgentChatOpFinished::CreateSP(this, &SUEAIAgentPanel::HandleChatOperationResult));
                    return FReply::Handled();
                })
                [
                    SNew(SImage)
                    .Image(FAppStyle::Get().GetBrush(TEXT("Icons.Refresh")))
                ]
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            [
                SNew(SButton)
                .Visibility_Lambda([bIsArchived]()
                {
                    return bIsArchived ? EVisibility::Visible : EVisibility::Collapsed;
                })
                .ToolTipText(FText::FromString(TEXT("Delete chat")))
                .ContentPadding(FMargin(4.0f, 2.0f))
                .OnClicked_Lambda([this, ChatId]()
                {
                    const EAppReturnType::Type ConfirmResult = FMessageDialog::Open(
                        EAppMsgType::YesNo,
                        FText::FromString(TEXT("Delete this chat forever? This action cannot be undone.")));
                    if (ConfirmResult != EAppReturnType::Yes)
                    {
                        return FReply::Handled();
                    }

                    bIsRefreshingChats = true;
                    ChatListErrorMessage.Reset();
                    UpdateChatListStateText();
                    FUEAIAgentTransportModule::Get().DeleteChat(
                        ChatId,
                        FOnUEAIAgentChatOpFinished::CreateSP(this, &SUEAIAgentPanel::HandleChatOperationResult));
                    return FReply::Handled();
                })
                [
                    SNew(SImage)
                    .Image(FAppStyle::Get().GetBrush(TEXT("Icons.Delete")))
                ]
            ]
        ]
    ];
    ChatTitleEditors.Add(ChatId, InlineTitle);
    return Row;
}

TSharedRef<ITableRow> SUEAIAgentPanel::HandleGenerateChatHistoryRow(
    TSharedPtr<FUEAIAgentChatHistoryEntry> InItem,
    const TSharedRef<STableViewBase>& OwnerTable)
{
    if (!InItem.IsValid())
    {
        return SNew(STableRow<TSharedPtr<FUEAIAgentChatHistoryEntry>>, OwnerTable)
        [
            SNew(STextBlock).Text(FText::FromString(TEXT("Invalid history item")))
        ];
    }

    const bool bIsUserMessage = InItem->DisplayRole.Equals(TEXT("user"), ESearchCase::IgnoreCase) ||
        (InItem->DisplayRole.IsEmpty() && InItem->Kind.Equals(TEXT("asked"), ESearchCase::IgnoreCase));
    const FString MessageText = InItem->DisplayText.IsEmpty() ? InItem->Summary : InItem->DisplayText;
    const FString RichMessageText = bIsUserMessage ? MessageText : ConvertMarkdownToRichText(MessageText);
    const bool bIsLastHistoryEntry = ChatHistoryItems.Num() > 0 && ChatHistoryItems.Last() == InItem;
    const bool bShowInlineApprovalUi = bIsLastHistoryEntry && ShouldShowApprovalUi();

    auto BuildApprovalUi = [this]() -> TSharedRef<SWidget>
    {
        TSharedRef<SVerticalBox> ApprovalBody = SNew(SVerticalBox);
        ApprovalBody->AddSlot()
        .AutoHeight()
        .Padding(0.0f, 0.0f, 0.0f, 6.0f)
        [
            SNew(SMultiLineEditableText)
            .IsReadOnly(true)
            .ClearTextSelectionOnFocusLoss(false)
            .AutoWrapText(true)
            .Text(FText::FromString(TEXT("Pending actions are shown in this chat. Review and confirm below.")))
        ];

        ApprovalBody->AddSlot()
        .AutoHeight()
        .Padding(0.0f, 0.0f, 0.0f, 6.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(0.0f, 0.0f, 8.0f, 0.0f)
            [
                SNew(SButton)
                .Text(FText::FromString(TEXT("Check all")))
                .OnClicked(this, &SUEAIAgentPanel::OnApproveLowRiskClicked)
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            [
                SNew(SButton)
                .Text(FText::FromString(TEXT("Uncheck all")))
                .OnClicked(this, &SUEAIAgentPanel::OnRejectAllClicked)
            ]
        ];

        const int32 ActionCount = FUEAIAgentTransportModule::Get().GetPlannedActionCount();
        for (int32 ActionIndex = 0; ActionIndex < ActionCount; ++ActionIndex)
        {
            ApprovalBody->AddSlot()
            .AutoHeight()
            .Padding(0.0f, 0.0f, 0.0f, 4.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                [
                    SNew(SCheckBox)
                    .IsChecked_Lambda([ActionIndex]()
                    {
                        return FUEAIAgentTransportModule::Get().IsPlannedActionApproved(ActionIndex)
                            ? ECheckBoxState::Checked
                            : ECheckBoxState::Unchecked;
                    })
                    .OnCheckStateChanged_Lambda([this, ActionIndex](ECheckBoxState NewState)
                    {
                        HandleActionApprovalChanged(ActionIndex, NewState);
                        UpdateActionApprovalUi();
                    })
                ]
                + SHorizontalBox::Slot()
                .FillWidth(1.0f)
                .VAlign(VAlign_Fill)
                .Padding(6.0f, 0.0f, 0.0f, 0.0f)
                [
                    SNew(SMultiLineEditableTextBox)
                    .IsReadOnly(true)
                    .AutoWrapText(true)
                    .Text_Lambda([ActionIndex]()
                    {
                        return FText::FromString(FUEAIAgentTransportModule::Get().GetPlannedActionPreviewText(ActionIndex));
                    })
                    .ToolTipText_Lambda([this, ActionIndex]()
                    {
                        return FText::FromString(BuildActionDetailText(ActionIndex));
                    })
                ]
            ];
        }

        ApprovalBody->AddSlot()
        .AutoHeight()
        .Padding(0.0f, 6.0f, 0.0f, 0.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(0.0f, 0.0f, 8.0f, 0.0f)
            [
                SNew(SButton)
                .Visibility_Lambda([this]()
                {
                    FUEAIAgentTransportModule& InnerTransport = FUEAIAgentTransportModule::Get();
                    const bool bHasPendingSessionAction = InnerTransport.HasActiveSession() &&
                        InnerTransport.GetNextPendingActionIndex() != INDEX_NONE;
                    return bHasPendingSessionAction && CurrentSessionStatus == ESessionStatus::AwaitingApproval
                        ? EVisibility::Visible
                        : EVisibility::Collapsed;
                })
                .Text(FText::FromString(TEXT("Confirm and Continue")))
                .OnClicked(this, &SUEAIAgentPanel::OnResumeAgentLoopClicked)
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(0.0f, 0.0f, 8.0f, 0.0f)
            [
                SNew(SButton)
                .Visibility_Lambda([this]()
                {
                    FUEAIAgentTransportModule& InnerTransport = FUEAIAgentTransportModule::Get();
                    const bool bHasPendingSessionAction = InnerTransport.HasActiveSession() &&
                        InnerTransport.GetNextPendingActionIndex() != INDEX_NONE;
                    return bHasPendingSessionAction && CurrentSessionStatus == ESessionStatus::AwaitingApproval
                        ? EVisibility::Visible
                        : EVisibility::Collapsed;
                })
                .Text(FText::FromString(TEXT("Reject")))
                .OnClicked(this, &SUEAIAgentPanel::OnRejectCurrentActionClicked)
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(0.0f, 0.0f, 8.0f, 0.0f)
            [
                SNew(SButton)
                .Visibility_Lambda([]()
                {
                    FUEAIAgentTransportModule& InnerTransport = FUEAIAgentTransportModule::Get();
                    const bool bHasPendingSessionAction = InnerTransport.HasActiveSession() &&
                        InnerTransport.GetNextPendingActionIndex() != INDEX_NONE;
                    const bool bCanApply = !bHasPendingSessionAction && InnerTransport.GetPlannedActionCount() > 0;
                    return bCanApply ? EVisibility::Visible : EVisibility::Collapsed;
                })
                .Text(FText::FromString(TEXT("Apply")))
                .OnClicked(this, &SUEAIAgentPanel::OnApplyPlannedActionClicked)
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            [
                SNew(SButton)
                .Visibility_Lambda([]()
                {
                    FUEAIAgentTransportModule& InnerTransport = FUEAIAgentTransportModule::Get();
                    const bool bHasPendingSessionAction = InnerTransport.HasActiveSession() &&
                        InnerTransport.GetNextPendingActionIndex() != INDEX_NONE;
                    const bool bCanCancel = !bHasPendingSessionAction && InnerTransport.GetPlannedActionCount() > 0;
                    return bCanCancel ? EVisibility::Visible : EVisibility::Collapsed;
                })
                .Text(FText::FromString(TEXT("Cancel")))
                .OnClicked(this, &SUEAIAgentPanel::OnCancelPlannedActionClicked)
            ]
        ];

        return SNew(SBorder)
            .Padding(8.0f)
            .BorderImage(FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox")))
            .BorderBackgroundColor(FLinearColor(0.14f, 0.14f, 0.14f, 0.35f))
            [
                ApprovalBody
            ];
    };

    if (bIsUserMessage)
    {
        TSharedRef<SVerticalBox> UserBody = SNew(SVerticalBox);
        UserBody->AddSlot()
        .AutoHeight()
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
            .FillWidth(0.2f)
            [
                SNew(SBox)
            ]
            + SHorizontalBox::Slot()
            .FillWidth(0.8f)
            [
                SNew(SBorder)
                .Padding(10.0f)
                .BorderImage(FCoreStyle::Get().GetBrush(TEXT("GenericWhiteBox")))
                .BorderBackgroundColor(FLinearColor(0.18f, 0.18f, 0.18f, 0.20f))
                [
                    SNew(SMultiLineEditableText)
                    .IsReadOnly(true)
                    .AutoWrapText(true)
                    .Text(FText::FromString(MessageText))
                ]
            ]
        ];

        if (bShowInlineApprovalUi)
        {
            UserBody->AddSlot()
            .AutoHeight()
            .Padding(0.0f, 8.0f, 0.0f, 0.0f)
            [
                BuildApprovalUi()
            ];
        }

        return SNew(STableRow<TSharedPtr<FUEAIAgentChatHistoryEntry>>, OwnerTable)
        .Padding(FMargin(8.0f, 8.0f))
        [
            UserBody
        ];
    }

    TSharedPtr<ITextLayoutMarshaller> MarkdownMarshaller = FRichTextLayoutMarshaller::Create(
        TArray<TSharedRef<ITextDecorator>>(),
        &GetChatMarkdownStyle());

    TSharedRef<SVerticalBox> AssistantBody = SNew(SVerticalBox);
    AssistantBody->AddSlot()
    .AutoHeight()
    [
        SNew(SMultiLineEditableText)
        .IsReadOnly(true)
        .ClearTextSelectionOnFocusLoss(false)
        .AutoWrapText(true)
        .Text(FText::FromString(RichMessageText))
        .TextStyle(&GetChatMarkdownStyle().GetWidgetStyle<FTextBlockStyle>(TEXT("md.normal")))
        .Marshaller(MarkdownMarshaller)
    ];

    if (bShowInlineApprovalUi)
    {
        AssistantBody->AddSlot()
        .AutoHeight()
        .Padding(0.0f, 8.0f, 0.0f, 0.0f)
        [
            BuildApprovalUi()
        ];
    }

    return SNew(STableRow<TSharedPtr<FUEAIAgentChatHistoryEntry>>, OwnerTable)
    .Padding(FMargin(4.0f, 8.0f))
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot()
        .FillWidth(1.0f)
        .HAlign(HAlign_Fill)
        [
            SNew(SBorder)
            .BorderBackgroundColor(FLinearColor(0.18f, 0.18f, 0.18f, 0.00f))
            [
                AssistantBody
            ]
        ]
    ];
}

void SUEAIAgentPanel::HandleChatSearchTextChanged(const FText& NewText)
{
    ChatSearchFilter = NewText.ToString().TrimStartAndEnd();
    RefreshChatUiFromTransport(true);
}

void SUEAIAgentPanel::HandleArchivedFilterChanged(ECheckBoxState NewState)
{
    bIncludeArchivedChats = NewState == ECheckBoxState::Checked;
    OnRefreshChatsClicked();
}

void SUEAIAgentPanel::HandleChatTitleCommitted(const FText& NewText, ETextCommit::Type CommitType, FString ChatId)
{
    if (CommitType != ETextCommit::OnEnter && CommitType != ETextCommit::OnUserMovedFocus)
    {
        return;
    }

    const FString NewTitle = NewText.ToString().TrimStartAndEnd();
    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    Transport.SetActiveChatId(ChatId);
    bIsRefreshingChats = true;
    ChatListErrorMessage.Reset();
    UpdateChatListStateText();
    Transport.RenameActiveChat(NewTitle, FOnUEAIAgentChatOpFinished::CreateSP(this, &SUEAIAgentPanel::HandleChatOperationResult));
}

void SUEAIAgentPanel::TryRestoreRunSelectionsFromHistory()
{
    if (!bPendingRunSelectionRestore)
    {
        return;
    }
    bPendingRunSelectionRestore = false;

    const TArray<FUEAIAgentChatHistoryEntry>& Entries = FUEAIAgentTransportModule::Get().GetActiveChatHistory();
    FString RestoredProvider;
    FString RestoredModel;
    FString RestoredChatType;
    for (int32 Index = Entries.Num() - 1; Index >= 0; --Index)
    {
        const FUEAIAgentChatHistoryEntry& Entry = Entries[Index];
        if (RestoredProvider.IsEmpty() && !Entry.Provider.IsEmpty())
        {
            RestoredProvider = Entry.Provider.TrimStartAndEnd().ToLower();
        }
        if (RestoredModel.IsEmpty() && !Entry.Model.IsEmpty())
        {
            RestoredModel = Entry.Model.TrimStartAndEnd();
        }
        if (RestoredChatType.IsEmpty() && !Entry.ChatType.IsEmpty())
        {
            RestoredChatType = Entry.ChatType.TrimStartAndEnd().ToLower();
        }
        if (!RestoredProvider.IsEmpty() && !RestoredModel.IsEmpty() && !RestoredChatType.IsEmpty())
        {
            break;
        }
    }

    if (!RestoredChatType.IsEmpty())
    {
        SelectModeByCode(RestoredChatType);
    }

    if (!RestoredProvider.IsEmpty())
    {
        SelectProviderByCode(RestoredProvider);
    }

    if (!RestoredModel.IsEmpty())
    {
        PendingRestoredModelProvider = RestoredProvider.IsEmpty() ? GetSelectedProviderCode() : RestoredProvider;
        PendingRestoredModelName = RestoredModel;
        if (SelectModelByProviderAndName(PendingRestoredModelProvider, PendingRestoredModelName))
        {
            PendingRestoredModelProvider.Reset();
            PendingRestoredModelName.Reset();
            return;
        }

        FUEAIAgentTransportModule::Get().RefreshModelOptions(
            PendingRestoredModelProvider,
            FOnUEAIAgentCredentialOpFinished::CreateSP(this, &SUEAIAgentPanel::HandleCredentialOperationResult));
        return;
    }

    if (!RestoredProvider.IsEmpty())
    {
        FUEAIAgentTransportModule::Get().RefreshModelOptions(
            RestoredProvider,
            FOnUEAIAgentCredentialOpFinished::CreateSP(this, &SUEAIAgentPanel::HandleCredentialOperationResult));
    }
}

void SUEAIAgentPanel::SelectProviderByCode(const FString& ProviderCode)
{
    const FString Normalized = ProviderCode.TrimStartAndEnd().ToLower();
    TSharedPtr<FString> NewSelection;
    for (const TSharedPtr<FString>& Item : ProviderItems)
    {
        if (!Item.IsValid())
        {
            continue;
        }

        if (Normalized == TEXT("gemini") && Item->Equals(TEXT("Gemini"), ESearchCase::IgnoreCase))
        {
            NewSelection = Item;
            break;
        }
        if (Normalized == TEXT("openai") && Item->Equals(TEXT("OpenAI"), ESearchCase::IgnoreCase))
        {
            NewSelection = Item;
            break;
        }
        if (Normalized == TEXT("local") && Item->Equals(TEXT("Local"), ESearchCase::IgnoreCase))
        {
            NewSelection = Item;
            break;
        }
    }

    if (!NewSelection.IsValid())
    {
        return;
    }

    SelectedProviderItem = NewSelection;
    if (ProviderCombo.IsValid())
    {
        ProviderCombo->SetSelectedItem(NewSelection);
    }
}

void SUEAIAgentPanel::SelectModeByCode(const FString& ModeCode)
{
    const FString Normalized = ModeCode.TrimStartAndEnd().ToLower();
    TSharedPtr<FString> NewSelection;
    for (const TSharedPtr<FString>& Item : ModeItems)
    {
        if (!Item.IsValid())
        {
            continue;
        }
        if (Normalized == TEXT("chat") && Item->Equals(TEXT("Chat"), ESearchCase::IgnoreCase))
        {
            NewSelection = Item;
            break;
        }
        if (Normalized == TEXT("agent") && Item->Equals(TEXT("Agent"), ESearchCase::IgnoreCase))
        {
            NewSelection = Item;
            break;
        }
    }

    if (!NewSelection.IsValid())
    {
        return;
    }

    SelectedModeItem = NewSelection;
    if (ModeCombo.IsValid())
    {
        ModeCombo->SetSelectedItem(NewSelection);
    }
}

bool SUEAIAgentPanel::SelectModelByProviderAndName(const FString& ProviderCode, const FString& ModelName)
{
    const FString NormalizedProvider = ProviderCode.TrimStartAndEnd().ToLower();
    const FString NormalizedModel = ModelName.TrimStartAndEnd().ToLower();
    if (NormalizedProvider.IsEmpty() || NormalizedModel.IsEmpty())
    {
        return false;
    }

    for (const TSharedPtr<FString>& Item : ModelItems)
    {
        if (!Item.IsValid())
        {
            continue;
        }

        const FUEAIAgentModelOption* Option = ModelLabelToOption.Find(*Item);
        if (!Option)
        {
            continue;
        }

        if (Option->Provider.Equals(NormalizedProvider, ESearchCase::IgnoreCase) &&
            Option->Model.Equals(NormalizedModel, ESearchCase::IgnoreCase))
        {
            SelectedModelItem = Item;
            if (ModelCombo.IsValid())
            {
                ModelCombo->SetSelectedItem(Item);
            }
            return true;
        }
    }

    return false;
}

bool SUEAIAgentPanel::BeginRenameSelectedChat()
{
    if (!ChatListView.IsValid())
    {
        return false;
    }

    const TArray<TSharedPtr<FUEAIAgentChatSummary>> SelectedItems = ChatListView->GetSelectedItems();
    if (SelectedItems.Num() == 0 || !SelectedItems[0].IsValid())
    {
        return false;
    }

    const FString SelectedId = SelectedItems[0]->Id;
    TSharedPtr<SInlineEditableTextBlock> InlineTitle = ChatTitleEditors.FindRef(SelectedId).Pin();
    if (!InlineTitle.IsValid())
    {
        return false;
    }

    InlineTitle->EnterEditingMode();
    return true;
}

FString SUEAIAgentPanel::BuildSelectionSummary() const
{
    const TArray<FString> SelectedActors = CollectSelectedActorNames();
    if (SelectedActors.Num() == 0)
    {
        return TEXT("");
    }

    FString NamesText = SelectedActors[0];
    if (SelectedActors.Num() > 1)
    {
        NamesText += TEXT(", ") + SelectedActors[1];
    }
    if (SelectedActors.Num() > 2)
    {
        NamesText += FString::Printf(TEXT(", +%d more"), SelectedActors.Num() - 2);
    }

    return FString::Printf(TEXT("Selection: %d actor(s): %s"), SelectedActors.Num(), *NamesText);
}

FString SUEAIAgentPanel::BuildActionDetailText(int32 ActionIndex) const
{
    FUEAIAgentPlannedSceneAction Action;
    if (!FUEAIAgentTransportModule::Get().GetPlannedAction(ActionIndex, Action))
    {
        return TEXT("Action details are not available.");
    }

    const FString Targets = Action.ActorNames.Num() > 0 ? FString::Join(Action.ActorNames, TEXT(", ")) : TEXT("selection");
    return FString::Printf(
        TEXT("Type=%s, Risk=%s, State=%s, Attempts=%d, Approved=%s, Targets=%s"),
        PlannedActionTypeToText(Action.Type),
        RiskLevelToText(Action.Risk),
        ActionStateToText(Action.State),
        Action.AttemptCount,
        Action.bApproved ? TEXT("true") : TEXT("false"),
        *Targets);
}

bool SUEAIAgentPanel::ShouldShowApprovalUi() const
{
    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    const int32 ActionCount = Transport.GetPlannedActionCount();
    if (ActionCount <= 0)
    {
        return false;
    }

    if (Transport.HasActiveSession())
    {
        const bool bHasPendingSessionAction = Transport.GetNextPendingActionIndex() != INDEX_NONE;
        return bHasPendingSessionAction &&
            CurrentSessionStatus == ESessionStatus::AwaitingApproval;
    }

    return true;
}

void SUEAIAgentPanel::HandleActionApprovalChanged(int32 ActionIndex, ECheckBoxState NewState)
{
    FUEAIAgentTransportModule::Get().SetPlannedActionApproved(ActionIndex, NewState == ECheckBoxState::Checked);
}

void SUEAIAgentPanel::UpdateActionApprovalUi()
{
    if (MainChatHistoryListView.IsValid())
    {
        MainChatHistoryListView->RequestListRefresh();
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
        return TEXT("local");
    }
    if (SelectedProviderItem->Equals(TEXT("Gemini"), ESearchCase::IgnoreCase))
    {
        return TEXT("gemini");
    }
    if (SelectedProviderItem->Equals(TEXT("OpenAI"), ESearchCase::IgnoreCase))
    {
        return TEXT("openai");
    }
    return TEXT("local");
}

FString SUEAIAgentPanel::GetSelectedProviderLabel() const
{
    if (!SelectedProviderItem.IsValid())
    {
        return TEXT("Local");
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

FString SUEAIAgentPanel::GetSelectedModelProvider() const
{
    if (!SelectedModelItem.IsValid())
    {
        return TEXT("");
    }

    const FUEAIAgentModelOption* Option = ModelLabelToOption.Find(*SelectedModelItem);
    return Option ? Option->Provider : TEXT("");
}

FString SUEAIAgentPanel::GetSelectedModelName() const
{
    if (!SelectedModelItem.IsValid())
    {
        return TEXT("");
    }

    const FUEAIAgentModelOption* Option = ModelLabelToOption.Find(*SelectedModelItem);
    return Option ? Option->Model : TEXT("");
}

TSharedRef<SWidget> SUEAIAgentPanel::HandleProviderComboGenerateWidget(TSharedPtr<FString> InItem) const
{
    return SNew(STextBlock).Text(FText::FromString(InItem.IsValid() ? *InItem : TEXT("Unknown")));
}

TSharedRef<SWidget> SUEAIAgentPanel::HandleModeComboGenerateWidget(TSharedPtr<FString> InItem) const
{
    return SNew(STextBlock).Text(FText::FromString(InItem.IsValid() ? *InItem : TEXT("Unknown")));
}

TSharedRef<SWidget> SUEAIAgentPanel::HandleModelComboGenerateWidget(TSharedPtr<FString> InItem) const
{
    return SNew(STextBlock).Text(FText::FromString(InItem.IsValid() ? *InItem : TEXT("Unknown")));
}

void SUEAIAgentPanel::HandleProviderComboSelectionChanged(TSharedPtr<FString> NewValue, ESelectInfo::Type SelectInfo)
{
    (void)SelectInfo;
    if (!NewValue.IsValid())
    {
        return;
    }

    SelectedProviderItem = NewValue;
    FUEAIAgentTransportModule::Get().RefreshModelOptions(
        GetSelectedProviderCode(),
        FOnUEAIAgentCredentialOpFinished::CreateSP(this, &SUEAIAgentPanel::HandleCredentialOperationResult));
}

void SUEAIAgentPanel::HandleModeComboSelectionChanged(TSharedPtr<FString> NewValue, ESelectInfo::Type SelectInfo)
{
    if (!NewValue.IsValid())
    {
        return;
    }

    SelectedModeItem = NewValue;
}

void SUEAIAgentPanel::HandleModelComboSelectionChanged(TSharedPtr<FString> NewValue, ESelectInfo::Type SelectInfo)
{
    if (!NewValue.IsValid())
    {
        return;
    }

    SelectedModelItem = NewValue;
}

FString SUEAIAgentPanel::BuildModelItemLabel(const FUEAIAgentModelOption& Option) const
{
    return FString::Printf(TEXT("%s | %s"), *ProviderCodeToLabel(Option.Provider), *Option.Model);
}

FString SUEAIAgentPanel::BuildModelOptionKey(const FUEAIAgentModelOption& Option) const
{
    return Option.Provider + TEXT("::") + Option.Model;
}

void SUEAIAgentPanel::RebuildModelUi()
{
    FUEAIAgentTransportModule& Transport = FUEAIAgentTransportModule::Get();
    const TArray<FUEAIAgentModelOption>& Available = Transport.GetAvailableModels();
    const TArray<FUEAIAgentModelOption>& Preferred = Transport.GetPreferredModels();
    const FString PreviousSelection = SelectedModelItem.IsValid() ? *SelectedModelItem : FString();

    ModelItems.Empty();
    ModelLabelToOption.Empty();
    for (const FUEAIAgentModelOption& Option : Preferred)
    {
        const FString Label = BuildModelItemLabel(Option);
        if (ModelLabelToOption.Contains(Label))
        {
            continue;
        }
        ModelItems.Add(MakeShared<FString>(Label));
        ModelLabelToOption.Add(Label, Option);
    }

    if (ModelItems.Num() == 0)
    {
        for (const FUEAIAgentModelOption& Option : Available)
        {
            const FString Label = BuildModelItemLabel(Option);
            if (ModelLabelToOption.Contains(Label))
            {
                continue;
            }
            ModelItems.Add(MakeShared<FString>(Label));
            ModelLabelToOption.Add(Label, Option);
        }
    }

    SelectedModelItem.Reset();
    for (const TSharedPtr<FString>& Item : ModelItems)
    {
        if (Item.IsValid() && *Item == PreviousSelection)
        {
            SelectedModelItem = Item;
            break;
        }
    }
    if (!SelectedModelItem.IsValid() && ModelItems.Num() > 0)
    {
        SelectedModelItem = ModelItems[0];
    }

    if (ModelCombo.IsValid())
    {
        ModelCombo->RefreshOptions();
        ModelCombo->SetSelectedItem(SelectedModelItem);
    }

    if (!PendingRestoredModelName.IsEmpty())
    {
        if (SelectModelByProviderAndName(PendingRestoredModelProvider, PendingRestoredModelName))
        {
            PendingRestoredModelProvider.Reset();
            PendingRestoredModelName.Reset();
        }
    }

    if (!ModelChecksBox.IsValid())
    {
        return;
    }

    ModelChecks.Empty();
    ModelKeyToOption.Empty();
    ModelChecksBox->ClearChildren();
    if (Available.Num() == 0)
    {
        ModelChecksBox->AddSlot()
        .AutoHeight()
        [
            SNew(STextBlock)
            .Text(FText::FromString(TEXT("No models available. Add an API key or run local provider.")))
        ];
        return;
    }

    TSet<FString> PreferredLabels;
    for (const FUEAIAgentModelOption& Option : Preferred)
    {
        PreferredLabels.Add(BuildModelOptionKey(Option));
    }

    for (const FUEAIAgentModelOption& Option : Available)
    {
        const FString OptionKey = BuildModelOptionKey(Option);
        TSharedPtr<SCheckBox> CheckBox;
        ModelChecksBox->AddSlot()
        .AutoHeight()
        .Padding(0.0f, 0.0f, 0.0f, 2.0f)
        [
            SAssignNew(CheckBox, SCheckBox)
            .IsChecked(PreferredLabels.Contains(OptionKey) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
            .OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
            {
                (void)NewState;
                PersistPreferredModels();
            })
            [
                SNew(STextBlock)
                .Text(FText::FromString(Option.Model))
            ]
        ];
        ModelChecks.Add(OptionKey, CheckBox);
        ModelKeyToOption.Add(OptionKey, Option);
        const FString Label = BuildModelItemLabel(Option);
        ModelLabelToOption.FindOrAdd(Label, Option);
    }
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

FReply SUEAIAgentPanel::HandlePromptKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
    (void)MyGeometry;
    const FKey Key = InKeyEvent.GetKey();

    if ((InKeyEvent.IsControlDown() || InKeyEvent.IsCommandDown()) && Key == EKeys::Enter)
    {
        return OnRunWithSelectionClicked();
    }

    if (Key == EKeys::Escape)
    {
        if (PromptInput.IsValid())
        {
            FSlateApplication::Get().SetKeyboardFocus(PromptInput, EFocusCause::SetDirectly);
        }
        return FReply::Handled();
    }

    return FReply::Unhandled();
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

EActiveTimerReturnType SUEAIAgentPanel::HandleSelectionTimer(double InCurrentTime, float InDeltaTime)
{
    (void)InCurrentTime;
    (void)InDeltaTime;
    UpdateSelectionSummaryText();
    return EActiveTimerReturnType::Continue;
}
