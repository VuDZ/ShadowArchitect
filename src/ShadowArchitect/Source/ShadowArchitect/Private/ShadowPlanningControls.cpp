#include "ShadowPlan.h"

#include "Containers/Ticker.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "Hologram/FGBlueprintHologram.h"
#include "Slate/SGameLayerManager.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ShadowPlanningControls"

namespace
{
struct FPlanningControlRow
{
    FText Description;
    FText Value;

    bool operator==(const FPlanningControlRow& Other) const
    {
        return Description.EqualTo(Other.Description) && Value.EqualTo(Other.Value);
    }
};
}

class FShadowPlanningControls
{
public:
    ~FShadowPlanningControls()
    {
        FTSTicker::RemoveTicker(Refresh);
        if (Widget && Viewport.IsValid() && LocalPlayer.IsValid())
            Viewport->RemoveViewportWidgetForPlayer(LocalPlayer.Get(), Widget.ToSharedRef());
    }

    void Show(UGameViewportClient& GameViewport, ULocalPlayer& Player, TArray<FPlanningControlRow> Rows, bool bUnbound)
    {
        if (!Widget)
        {
            Viewport = &GameViewport;
            LocalPlayer = &Player;
            const auto* Brush = FCoreStyle::Get().GetBrush("WhiteBrush");
            Widget = SNew(SOverlay)
                + SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Top).Padding(FMargin(16, 104, 0, 0))
                [
                    SAssignNew(Box, SBox)
                    [
                        SNew(SBorder).BorderImage(Brush)
                        .BorderBackgroundColor(FLinearColor(0.26f, 0.52f, 0.63f)).Padding(2)
                        [
                            SNew(SBorder).BorderImage(Brush)
                            .BorderBackgroundColor(FLinearColor(0.025f, 0.035f, 0.045f, 0.97f)).Padding(FMargin(12, 8))
                            [
                                SAssignNew(Content, SVerticalBox).Tag(TEXT("ShadowArchitect.PlanningControls"))
                            ]
                        ]
                    ]
                ];
            GameViewport.AddViewportWidgetForPlayer(&Player, Widget.ToSharedRef(), 55);
        }

        const auto Layers = GameViewport.GetGameLayerManager();
        const double Width = Layers ? Layers->GetPlayerWidgetHostGeometry(&Player).GetLocalSize().X : 0.0;
        const float PanelWidth = Width > 0.0 ? FMath::Clamp(float(Width * 0.42 - 16.0), 116.0f, 460.0f) : 460.0f;
        if (PanelWidth != LastWidth)
        {
            LastWidth = PanelWidth;
            Box->SetWidthOverride(PanelWidth);
        }

        if (Rows != LastRows || bUnbound != bLastUnbound)
        {
            Content->ClearChildren();
            for (const auto& Row : Rows)
            {
                Content->AddSlot().AutoHeight().Padding(0, 2)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().FillWidth(0.42f).VAlign(VAlign_Center).Padding(0, 0, 12, 0)
                    [
                        SNew(STextBlock).Text(Row.Description)
                        .Font(FCoreStyle::GetDefaultFontStyle("Regular", 14))
                        .ColorAndOpacity(FLinearColor(0.66f, 0.74f, 0.80f))
                        .AutoWrapText(true).WrappingPolicy(ETextWrappingPolicy::AllowPerCharacterWrapping)
                    ]
                    + SHorizontalBox::Slot().FillWidth(0.58f).VAlign(VAlign_Center)
                    [
                        SNew(STextBlock).Text(Row.Value)
                        .Font(FCoreStyle::GetDefaultFontStyle("Bold", 14)).ColorAndOpacity(FLinearColor::White)
                        .Justification(ETextJustify::Right)
                        .AutoWrapText(true).WrappingPolicy(ETextWrappingPolicy::AllowPerCharacterWrapping)
                    ]
                ];
            }
            if (bUnbound)
            {
                Content->AddSlot().AutoHeight().Padding(0, 6, 0, 2)
                [
                    SNew(STextBlock).Text(LOCTEXT("Settings", "Настройки → Назначение клавиш → Shadow Architect"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular", 12))
                    .ColorAndOpacity(FLinearColor(0.66f, 0.74f, 0.80f)).AutoWrapText(true)
                ];
            }
            LastRows = MoveTemp(Rows);
            bLastUnbound = bUnbound;
        }
        Widget->SetVisibility(EVisibility::HitTestInvisible);
    }

    void Hide() const
    {
        if (Widget) Widget->SetVisibility(EVisibility::Collapsed);
    }

    FTSTicker::FDelegateHandle Refresh;

private:
    TWeakObjectPtr<UGameViewportClient> Viewport;
    TWeakObjectPtr<ULocalPlayer> LocalPlayer;
    TSharedPtr<SWidget> Widget;
    TSharedPtr<SBox> Box;
    TSharedPtr<SVerticalBox> Content;
    TArray<FPlanningControlRow> LastRows;
    float LastWidth = 0.0f;
    bool bLastUnbound = false;
};

void AShadowPlanSubsystem::StartPlanningControls()
{
    if (PlanningControls || !FSlateApplication::IsInitialized()) return;
    PlanningControls = MakeShared<FShadowPlanningControls>();
    PlanningControls->Refresh = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateUObject(this, &AShadowPlanSubsystem::RefreshPlanningControls), 0.1f);
}

bool AShadowPlanSubsystem::RefreshPlanningControls(float)
{
    auto* PC = GetWorld()->GetFirstPlayerController();
    auto* Player = IsValid(PC) && PC->IsLocalController() ? PC->GetLocalPlayer() : nullptr;
    auto* Viewport = GetWorld()->GetGameViewport();
    if (!Player || !Viewport || !PC->GetPawn() || IsPlanMenuOpen() || ClearInView || !CanUsePlanControls(*PC))
    {
        PlanningControls->Hide();
        return true;
    }

    const bool bBlueprint = IsValid(Cast<AFGBlueprintHologram>(CurrentPreview()));
    const FGuid TargetId = Views.IsEmpty() ? FGuid() : FindPlanUnderCrosshair();
    const auto* Target = TargetId.IsValid()
        ? Plans.FindByPredicate([&TargetId](const auto& Plan) { return Plan.Id == TargetId; }) : nullptr;
    if (!bBlueprint && !Target)
    {
        PlanningControls->Hide();
        return true;
    }

    TArray<FPlanningControlRow> Rows;
    bool bUnbound = false;
    const auto AddBinding = [this, PC, &Rows, &bUnbound](FText Action, FName Mapping)
    {
        const FString Key = KeybindingHint(*PC, Mapping);
        bUnbound |= Key.IsEmpty();
        Rows.Add({ MoveTemp(Action), Key.IsEmpty() ? LOCTEXT("Unassigned", "Не назначено") : FText::FromString(Key) });
    };

    Rows.Add({ LOCTEXT("SupportModeLabel", "Опора"), bUsePlanSupport
        ? LOCTEXT("WorldAndPlans", "Мир и планы") : LOCTEXT("WorldOnly", "Только мир") });
    if (bBlueprint && ActivePreview()) AddBinding(LOCTEXT("Place", "Разместить план"), TEXT("ShadowArchitect.Place"));
    AddBinding(LOCTEXT("ToggleSupport", "Сменить опору"), TEXT("ShadowArchitect.PlacementSupport"));
    if (Target || PlanMenu || AlignmentPlan.IsValid())
        AddBinding(LOCTEXT("OpenMenu", "Меню плана"), TEXT("ShadowArchitect.OpenPlanMenu"));
    if (Target) Rows.Add({ LOCTEXT("AimedPlanLabel", "Под прицелом"), FText::FromString(Target->BlueprintId) });
    PlanningControls->Show(*Viewport, *Player, MoveTemp(Rows), bUnbound);
    return true;
}

#undef LOCTEXT_NAMESPACE
