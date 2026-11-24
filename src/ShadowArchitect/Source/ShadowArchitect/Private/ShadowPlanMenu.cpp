#include "ShadowPlan.h"

#include "Brushes/SlateColorBrush.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "FGPlayerController.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "Hologram/FGBlueprintHologram.h"
#include "Slate/SGameLayerManager.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"
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
    PlaceOnRoof,
    AlignmentGuide,
    Cancel
};

const FButtonStyle& PlanMenuButtonStyle()
{
    static const FButtonStyle Style = FButtonStyle(FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("Button"))
        .SetNormal(FSlateColorBrush(FLinearColor(0.07f, 0.095f, 0.12f)))
        .SetHovered(FSlateColorBrush(FLinearColor(0.11f, 0.26f, 0.34f)))
        .SetPressed(FSlateColorBrush(FLinearColor(0.18f, 0.40f, 0.47f)));
    return Style;
}
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
    bool IsOpen() const { return Widget.IsValid(); }
    bool ConfirmPendingMove();

private:
    void Hide();
    void Cancel();
    FReply Choose(EPlanMenuAction Action, FText Label);
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
    const auto* View = Owner ? Owner->Views.FindRef(Target).Get() : nullptr;
    if (!View || View->IsHidden()) return nullptr;
    return Owner->Plans.FindByPredicate([this](const FShadowPlacement& Plan) { return Plan.Id == Target; });
}

TSharedRef<SButton> FShadowPlanMenu::Button(FText Label, EPlanMenuAction Action)
{
    return SNew(SButton)
        .ButtonStyle(&PlanMenuButtonStyle())
        .HAlign(HAlign_Center)
        .ContentPadding(FMargin(12, 8))
        .OnClicked(FOnClicked::CreateSP(AsShared(), &FShadowPlanMenu::Choose, Action, Label))
        [
            SNew(STextBlock)
            .Text(MoveTemp(Label))
            .Font(FCoreStyle::GetDefaultFontStyle("Regular", 16))
            .ColorAndOpacity(FLinearColor::White)
        ];
}

void FShadowPlanMenu::Show()
{
    auto* PC = Player.Get();
    auto* GameViewport = Viewport.Get();
    const auto Layers = GameViewport ? GameViewport->GetGameLayerManager() : nullptr;
    if (Widget || !PC || !PC->GetLocalPlayer() || !Layers || !AShadowPlanSubsystem::CanUsePlanControls(*PC)) return;

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
        .Font(FCoreStyle::GetDefaultFontStyle("Bold", 22))
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
        const auto* Owner = Plans.Get();
        if (Owner && Owner->ViewMode != EShadowView::Footprint && Cast<AFGBlueprintHologram>(Owner->CurrentPreview()))
            Content->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
            [ Button(LOCTEXT("PlaceOnRoof", "Place Blueprint on roof"), EPlanMenuAction::PlaceOnRoof) ];
        Content->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
        [ Button(Owner && Owner->AlignmentPlan == Target
            ? LOCTEXT("HideAlignmentGuide", "Hide alignment guide")
            : LOCTEXT("ShowAlignmentGuide", "Show alignment guide"), EPlanMenuAction::AlignmentGuide) ];
    }
    Content->AddSlot().AutoHeight()[ CancelButton ];

    const FSlateBrush* Panel = FCoreStyle::Get().GetBrush("WhiteBrush");
    const auto ConsumeClick = [](const FGeometry&, const FPointerEvent&) { return FReply::Handled(); };
    Widget = SNew(SBorder)
        .Cursor(EMouseCursor::Default)
        .BorderImage(Panel)
        .BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.72f))
        .Padding(12)
        .HAlign(HAlign_Center)
        .VAlign(VAlign_Center)
        .OnMouseButtonDown_Lambda(ConsumeClick)
        .OnMouseButtonUp_Lambda(ConsumeClick)
        .OnMouseDoubleClick_Lambda(ConsumeClick)
        [
            SNew(SScaleBox).Stretch(EStretch::ScaleToFit).StretchDirection(EStretchDirection::DownOnly)
            [
                SNew(SBox).WidthOverride(380)
                [
                    SNew(SBorder)
                    .BorderImage(Panel)
                    .BorderBackgroundColor(FLinearColor(0.028f, 0.034f, 0.041f))
                    .Padding(18)
                    [ Content ]
                ]
            ]
        ];
    bPreviousCursor = PC->bShowMouseCursor;
    PC->bShowMouseCursor = true;
    // The Build Gun HUD can render above widgets in the player and viewport overlays.
    Layers->AddGameLayer(Widget.ToSharedRef(), static_cast<int32>(SGameLayerManager::EGameLayerOrder::Debug) + 10);
    PC->SetInputMode(FInputModeUIOnly().SetWidgetToFocus(CancelButton));
    PC->FlushPressedKeys();
    UE_LOG(LogShadowPlanMenu, Display, TEXT("Opened %s for %s [%s]."),
        bPendingMove ? TEXT("move confirmation") : TEXT("plan menu"), *Plan->BlueprintId, *Target.ToString());
}

void FShadowPlanMenu::Hide()
{
    if (!Widget) return;
    if (auto* GameViewport = Viewport.Get())
        if (const auto Layers = GameViewport->GetGameLayerManager())
            Layers->RemoveGameLayer(Widget.ToSharedRef());
    if (auto* PC = Player.Get())
    {
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

FReply FShadowPlanMenu::Choose(EPlanMenuAction Action, FText Label)
{
    if (auto* Owner = Plans.Get(); Owner && Widget)
    {
        UE_LOG(LogShadowPlanMenu, Display, TEXT("Chose %s for plan [%s]."), *Label.ToString(), *Target.ToString());
        if (Action != EPlanMenuAction::Cancel && !FindTarget())
        {
            Owner->ClosePlanMenu();
            Owner->Notify(TEXT("That plan is no longer available."));
            return FReply::Handled();
        }
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
        case EPlanMenuAction::PlaceOnRoof:
        {
            const FGuid Id = Target;
            Owner->ClosePlanMenu();
            Owner->PlaceBlueprintOnRoof(Id);
            break;
        }
        case EPlanMenuAction::AlignmentGuide:
        {
            const FGuid Id = Target;
            Owner->ClosePlanMenu();
            Owner->ToggleAlignmentGuide(Id);
            break;
        }
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
    UE_LOG(LogShadowPlanMenu, Display, TEXT("Closed with Esc for plan [%s]."), *Target.ToString());
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
        FGuid Target = FindPlanUnderCrosshair();
        if (!Target.IsValid()) Target = AlignmentPlan;
        if (!Target.IsValid())
        {
            Notify(TEXT("No visible plan under the crosshair."));
            return;
        }
        auto Menu = MakeShared<FShadowPlanMenu>(*this, *PC, *GameViewport, Target);
        if (!FSlateApplication::Get().RegisterInputPreProcessor(Menu, 0)) return;
        CloseClearInView();
        PlanMenu = MoveTemp(Menu);
    }
    const auto Menu = PlanMenu;
    Menu->Show();
}

void AShadowPlanSubsystem::ClosePlanMenu()
{
    if (auto Closing = MoveTemp(PlanMenu)) Closing->Close();
}

bool AShadowPlanSubsystem::IsPlanMenuOpen() const
{
    return PlanMenu && PlanMenu->IsOpen();
}

bool AShadowPlanSubsystem::ConfirmPendingPlanMove()
{
    const auto Menu = PlanMenu;
    return Menu && Menu->ConfirmPendingMove();
}

#undef LOCTEXT_NAMESPACE
