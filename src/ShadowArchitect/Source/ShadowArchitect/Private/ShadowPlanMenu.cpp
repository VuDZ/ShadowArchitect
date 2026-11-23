#include "ShadowPlan.h"

#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "FGPlayerController.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ShadowPlanMenu"

DEFINE_LOG_CATEGORY_STATIC(LogShadowPlanMenu, Log, All);

namespace
{
enum class EPlanMenuAction
{
    Move,
    ConfirmMove,
    Delete,
    BuildPreview,
    Cancel
};

}

class FShadowPlanMenu : public IInputProcessor, public TSharedFromThis<FShadowPlanMenu>
{
public:
    FShadowPlanMenu(AShadowPlanSubsystem& InPlans, AFGPlayerController& InPlayer,
        UGameViewportClient& InViewport, const FGuid& InTarget)
        : Plans(&InPlans), Player(&InPlayer), Viewport(&InViewport), Target(InTarget)
    {
    }

    void Show();
    void Close();
    bool ConfirmPendingMove();

private:
    void Hide();
    void Cancel();
    FReply Choose(EPlanMenuAction Action);
    TSharedRef<SButton> Button(FText Label, EPlanMenuAction Action);
    const FShadowPlacement* FindTarget() const;

    void Tick(float, FSlateApplication&, TSharedRef<ICursor>) override {}
    bool HandleKeyDownEvent(FSlateApplication& Slate, const FKeyEvent& Event) override;
    bool HandleKeyUpEvent(FSlateApplication& Slate, const FKeyEvent& Event) override;
    const TCHAR* GetDebugName() const override { return TEXT("Shadow Architect plan menu"); }

    TWeakObjectPtr<AShadowPlanSubsystem> Plans;
    TWeakObjectPtr<AFGPlayerController> Player;
    TWeakObjectPtr<UGameViewportClient> Viewport;
    FGuid Target;
    TSharedPtr<SWidget> Widget;
    bool bPendingMove = false;
    bool bPreviousCursor = false;
    bool bCapturedEscape = false;
};

const FShadowPlacement* FShadowPlanMenu::FindTarget() const
{
    const auto* Owner = Plans.Get();
    if (!Owner || !Owner->Views.FindRef(Target).IsValid()) return nullptr;
    return Owner->Plans.FindByPredicate([this](const FShadowPlacement& Plan) { return Plan.Id == Target; });
}

TSharedRef<SButton> FShadowPlanMenu::Button(FText Label, EPlanMenuAction Action)
{
    return SNew(SButton)
        .HAlign(HAlign_Center)
        .ContentPadding(FMargin(12, 8))
        .Text(MoveTemp(Label))
        .OnClicked(FOnClicked::CreateSP(AsShared(), &FShadowPlanMenu::Choose, Action));
}

void FShadowPlanMenu::Show()
{
    auto* PC = Player.Get();
    auto* GameViewport = Viewport.Get();
    auto* LocalPlayer = PC ? PC->GetLocalPlayer() : nullptr;
    if (Widget || !LocalPlayer || !GameViewport || !AShadowPlanSubsystem::CanUsePlanControls(*PC)) return;

    const auto* Plan = FindTarget();
    if (!Plan)
    {
        if (auto* Owner = Plans.Get())
        {
            Owner->Notify(TEXT("That plan is no longer available."));
            Owner->ClosePlanMenu();
        }
        return;
    }

    const FText Title = FText::FromString(Plan->BlueprintId);
    const auto CancelButton = Button(LOCTEXT("Cancel", "Cancel (Esc)"), EPlanMenuAction::Cancel);
    auto Content = SNew(SVerticalBox);
    Content->AddSlot().AutoHeight().Padding(0, 0, 0, 12)
    [
        SNew(STextBlock)
        .Text(Title)
        .Font(FCoreStyle::GetDefaultFontStyle("Bold", 18))
        .AutoWrapText(true)
    ];
    if (bPendingMove)
    {
        Content->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
        [ Button(LOCTEXT("ConfirmMove", "Confirm move"), EPlanMenuAction::ConfirmMove) ];
    }
    else
    {
        Content->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
        [ Button(LOCTEXT("Move", "Move"), EPlanMenuAction::Move) ];
        Content->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
        [ Button(LOCTEXT("Delete", "Delete"), EPlanMenuAction::Delete) ];
        Content->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
        [ Button(LOCTEXT("BuildPreview", "Open build preview"), EPlanMenuAction::BuildPreview) ];
    }
    Content->AddSlot().AutoHeight()[ CancelButton ];

    const FSlateBrush* Panel = FCoreStyle::Get().GetBrush("WhiteBrush");
    Widget = SNew(SBorder)
        .BorderImage(Panel)
        .BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.55f))
        .HAlign(HAlign_Center)
        .VAlign(VAlign_Center)
        [
            SNew(SBox).WidthOverride(340)
            [
                SNew(SBorder)
                .BorderImage(Panel)
                .BorderBackgroundColor(FLinearColor(0.028f, 0.034f, 0.041f))
                .Padding(18)
                [ Content ]
            ]
        ];
    bPreviousCursor = PC->bShowMouseCursor;
    PC->bShowMouseCursor = true;
    GameViewport->AddViewportWidgetForPlayer(LocalPlayer, Widget.ToSharedRef(), 100);
    PC->SetInputMode(FInputModeUIOnly().SetWidgetToFocus(CancelButton));
    UE_LOG(LogShadowPlanMenu, Display, TEXT("Opened %s for %s [%s]."),
        bPendingMove ? TEXT("move confirmation") : TEXT("plan menu"), *Plan->BlueprintId, *Target.ToString());
}

void FShadowPlanMenu::Hide()
{
    if (!Widget) return;
    if (auto* PC = Player.Get())
    {
        auto* GameViewport = Viewport.Get();
        if (GameViewport && PC->GetLocalPlayer())
            GameViewport->RemoveViewportWidgetForPlayer(PC->GetLocalPlayer(), Widget.ToSharedRef());
        PC->bShowMouseCursor = bPreviousCursor;
        PC->SetInputMode(FInputModeGameOnly());
    }
    Widget.Reset();
}

void FShadowPlanMenu::Close()
{
    Hide();
    if (FSlateApplication::IsInitialized())
        FSlateApplication::Get().UnregisterInputPreProcessor(AsShared());
}

void FShadowPlanMenu::Cancel()
{
    if (auto* Owner = Plans.Get())
    {
        if (bPendingMove)
            Owner->Notify(FString::Printf(TEXT("Cancelled move for plan [%s]."), *Target.ToString()));
        Owner->ClosePlanMenu();
    }
}

bool FShadowPlanMenu::ConfirmPendingMove()
{
    if (!bPendingMove) return false;
    if (auto* Owner = Plans.Get())
    {
        Hide();
        const bool bTargetExists = FindTarget() != nullptr;
        if (Owner->TryMovePlan(Target) || !bTargetExists) Owner->ClosePlanMenu();
    }
    return true;
}

FReply FShadowPlanMenu::Choose(EPlanMenuAction Action)
{
    if (auto* Owner = Plans.Get(); Owner && Widget)
    {
        switch (Action)
        {
        case EPlanMenuAction::Move:
        {
            bPendingMove = true;
            Hide();
            auto* PC = Player.Get();
            const FString MenuKey = PC
                ? Owner->KeybindingHint(*PC, TEXT("ShadowArchitect.OpenPlanMenu")) : FString();
            const FString Confirm = MenuKey.IsEmpty() ? TEXT("Open the plan menu again to confirm.")
                : FString::Printf(TEXT("Press %s to open move confirmation."), *MenuKey);
            Owner->Notify(FString::Printf(TEXT("Position a Build Gun preview. %s Press Esc to cancel."), *Confirm));
            break;
        }
        case EPlanMenuAction::ConfirmMove:
            ConfirmPendingMove();
            break;
        case EPlanMenuAction::Delete:
            Owner->ClosePlanMenu();
            Owner->DeletePlan(Target);
            break;
        case EPlanMenuAction::BuildPreview:
            Owner->ClosePlanMenu();
            Owner->OpenPlanBuildPreview(Target);
            break;
        case EPlanMenuAction::Cancel:
            Cancel();
            break;
        }
    }
    return FReply::Handled();
}

bool FShadowPlanMenu::HandleKeyDownEvent(FSlateApplication&, const FKeyEvent& Event)
{
    if (Event.GetKey() != EKeys::Escape) return false;
    if (bCapturedEscape) return true;

    auto* PC = Player.Get();
    if (!PC) return false;
    bCapturedEscape = Widget.IsValid() || (bPendingMove && AShadowPlanSubsystem::CanUsePlanControls(*PC));
    return bCapturedEscape;
}

bool FShadowPlanMenu::HandleKeyUpEvent(FSlateApplication&, const FKeyEvent& Event)
{
    if (Event.GetKey() != EKeys::Escape || !bCapturedEscape) return false;
    bCapturedEscape = false;
    const auto KeepAlive = AsShared();
    Cancel();
    return true;
}

void AShadowPlanSubsystem::OpenPlanMenu()
{
    auto* PC = Cast<AFGPlayerController>(GetWorld()->GetFirstPlayerController());
    auto* GameViewport = GetWorld()->GetGameViewport();
    if (!PC || !PC->GetLocalPlayer() || !GameViewport || !FSlateApplication::IsInitialized()
        || !CanUsePlanControls(*PC)) return;

    if (!PlanMenu)
    {
        const FGuid Target = FindPlanUnderCrosshair();
        if (!Target.IsValid())
        {
            Notify(TEXT("No visible plan under the crosshair."));
            return;
        }
        auto Menu = MakeShared<FShadowPlanMenu>(*this, *PC, *GameViewport, Target);
        if (!FSlateApplication::Get().RegisterInputPreProcessor(Menu, 0)) return;
        PlanMenu = MoveTemp(Menu);
    }
    const auto Menu = PlanMenu;
    Menu->Show();
}

void AShadowPlanSubsystem::ClosePlanMenu()
{
    if (auto Closing = MoveTemp(PlanMenu)) Closing->Close();
}

bool AShadowPlanSubsystem::ConfirmPendingPlanMove()
{
    const auto Menu = PlanMenu;
    return Menu && Menu->ConfirmPendingMove();
}

#undef LOCTEXT_NAMESPACE
