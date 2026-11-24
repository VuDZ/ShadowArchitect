#include "ShadowPlan.h"

#include "ShadowAlignmentGeometry.h"
#include "ShadowWorldLines.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Equipment/FGBuildGunBuild.h"
#include "FGCharacterPlayer.h"
#include "GameFramework/PlayerController.h"
#include "Hologram/FGFactoryHologram.h"
#include "Hologram/FGFoundationHologram.h"
#include "Styling/CoreStyle.h"
#include "TimerManager.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ShadowAlignmentGuide"

namespace
{
const FLinearColor GuideColor(0.12f, 0.9f, 0.95f);
const FLinearColor EdgeColor(1.0f, 0.65f, 0.15f);

void AddLine(TArray<FShadowAlignmentSegment>& Lines, const FVector& A, const FVector& B,
    const FLinearColor& Color = GuideColor, float Thickness = 2.5f)
{
    auto& Line = Lines.AddDefaulted_GetRef();
    Line.Start = A;
    Line.End = B;
    Line.Color = Color;
    Line.Thickness = Thickness;
}

void AddGridCell(TArray<FShadowAlignmentSegment>& Lines, const FTransform& Frame, const FVector& HalfSize)
{
    const FVector A = Frame.TransformPosition(FVector(-HalfSize.X, -HalfSize.Y, HalfSize.Z));
    const FVector B = Frame.TransformPosition(FVector( HalfSize.X, -HalfSize.Y, HalfSize.Z));
    const FVector C = Frame.TransformPosition(FVector( HalfSize.X,  HalfSize.Y, HalfSize.Z));
    const FVector D = Frame.TransformPosition(FVector(-HalfSize.X,  HalfSize.Y, HalfSize.Z));
    AddLine(Lines, A, B);
    AddLine(Lines, B, C);
    AddLine(Lines, C, D);
    AddLine(Lines, D, A);
}
}

class FShadowAlignmentGuide
{
public:
    TWeakObjectPtr<UGameViewportClient> Viewport;
    TWeakObjectPtr<ULocalPlayer> LocalPlayer;
    TSharedPtr<SWidget> Widget;
    TSharedPtr<SShadowWorldLines> LineWidget;
    TSharedPtr<STextBlock> Label;
    FShadowFoundationGrid LastGrid;
    TOptional<FShadowFoundationGuide> Foundation;
    TWeakObjectPtr<UClass> FoundationClass;
    FTransform PlanTransform = FTransform::Identity;

    void Display(FText Message) const
    {
        LineWidget->Invalidate(EInvalidateWidgetReason::Paint);
        const bool bShow = !Message.IsEmpty();
        Label->SetText(MoveTemp(Message));
        Widget->SetVisibility(bShow
            ? EVisibility::HitTestInvisible : EVisibility::Collapsed);
    }
};

void AShadowPlanSubsystem::ToggleAlignmentGuide(const FGuid& Id)
{
    const bool bDisable = AlignmentPlan == Id;
    ClearAlignmentGuide();
    if (bDisable)
    {
        Notify(TEXT("Alignment guide off."));
        return;
    }

    auto* PC = GetWorld()->GetFirstPlayerController();
    auto* Player = PC ? PC->GetLocalPlayer() : nullptr;
    auto* Viewport = GetWorld()->GetGameViewport();
    if (!bVisible || !Views.FindRef(Id).IsValid() || !IsValid(Player) || !IsValid(Viewport)) return;

    AlignmentPlan = Id;
    AlignmentGuide = MakeShared<FShadowAlignmentGuide>();
    auto& Guide = *AlignmentGuide;
    Guide.LocalPlayer = Player;
    Guide.Viewport = Viewport;
    Guide.Widget = SNew(SOverlay)
        + SOverlay::Slot()
        [ SAssignNew(Guide.LineWidget, SShadowWorldLines).Player(PC).Clipping(EWidgetClipping::ClipToBounds)
            .GetLines([Owner = TWeakObjectPtr<AShadowPlanSubsystem>(this)]() -> TConstArrayView<FShadowAlignmentSegment>
            {
                if (const auto* Plans = Owner.Get()) return Plans->AlignmentLines;
                return {};
            }) ]
        + SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Top).Padding(FMargin(10, 168, 10, 0))
        [ SNew(SBorder).Padding(FMargin(12, 6))
            .BorderBackgroundColor(FLinearColor(0.025f, 0.035f, 0.045f, 0.94f))
            [ SAssignNew(Guide.Label, STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold", 15)).AutoWrapText(true) ] ];
    Viewport->AddViewportWidgetForPlayer(Player, Guide.Widget.ToSharedRef(), 50);
    GetWorldTimerManager().SetTimer(AlignmentRefresh, this, &AShadowPlanSubsystem::RefreshAlignmentGuide, 0.05f, true);
    RefreshAlignmentGuide();
}

void AShadowPlanSubsystem::ClearAlignmentGuide()
{
    if (!AlignmentGuide) return;
    GetWorldTimerManager().ClearTimer(AlignmentRefresh);
    if (auto* Viewport = AlignmentGuide->Viewport.Get())
        if (auto* Player = AlignmentGuide->LocalPlayer.Get())
            Viewport->RemoveViewportWidgetForPlayer(Player, AlignmentGuide->Widget.ToSharedRef());
    AlignmentGuide.Reset();
    AlignmentLines.Reset();
    AlignmentPlan.Invalidate();
}

void AShadowPlanSubsystem::RefreshAlignmentGuide()
{
    const auto* Plan = Plans.FindByPredicate([this](const auto& Entry) { return Entry.Id == AlignmentPlan; });
    if (!Plan || !Views.FindRef(AlignmentPlan).IsValid())
    {
        ClearAlignmentGuide();
        return;
    }

    auto& Guide = *AlignmentGuide;
    auto* PC = GetWorld()->GetFirstPlayerController();
    auto& Lines = AlignmentLines;
    Lines.Reset();
    if (!IsValid(PC) || !CanUsePlanControls(*PC))
    {
        Guide.Display(FText::GetEmpty());
        return;
    }
    if (ViewMode == EShadowView::Footprint)
    {
        Guide.Display(LOCTEXT("PartView", "Alignment guide · Switch to Ghost or Wireframe to aim at a part"));
        return;
    }

    auto* Player = Cast<AFGCharacterPlayer>(PC->GetPawn());
    const auto* Gun = IsValid(Player) ? Player->GetBuildGun() : nullptr;
    const auto* Preview = CurrentPreview();
    auto* BuildClass = Preview ? Preview->GetBuildClass().Get() : nullptr;
    if (!BuildClass || !IsValid(Gun))
    {
        Guide.Display(LOCTEXT("PickRecipe", "Alignment guide · Select a machine or FICSIT Foundation 1 m"));
        return;
    }

    FVector Start, Delta;
    const double Limit = TracePlanAim(*PC, Gun->GetDefaultBuildGunRange(), Start, Delta);
    double PartTime = Limit;
    const int32 PartIndex = FindShadowPartOnRay(*Plan, Start, Delta, PartTime);
    if (Preview->IsA<AFGFoundationHologram>())
    {
        if (!Guide.PlanTransform.Equals(Plan->Transform) || Guide.FoundationClass.Get() != BuildClass)
            Guide.Foundation.Reset();
        Guide.PlanTransform = Plan->Transform;
        if (Plan->Parts.IsValidIndex(PartIndex))
        {
            FShadowFoundationGrid Grid;
            FShadowFoundationGuide Edge;
            if (GetShadowFoundationGrid(*Plan, Plan->Parts[PartIndex], *BuildClass, Grid)
                && Grid.AimAtEdge(Start, Start + Delta, Edge) && Edge.HitTime < Limit)
            {
                Guide.LastGrid = Grid;
                Guide.Foundation = Edge;
                Guide.FoundationClass = BuildClass;
            }
            else Guide.Foundation.Reset();
        }
        if (Guide.Foundation)
        {
            const auto& Edge = *Guide.Foundation;
            const FVector HalfSize = Guide.LastGrid.Size * 0.5;
            AddGridCell(Lines, Guide.LastGrid.Frame, HalfSize);
            AddGridCell(Lines, Edge.NeighborTransform, HalfSize);
            AddLine(Lines, Edge.EdgeStart, Edge.EdgeEnd, EdgeColor, 4.5f);
        }
        Guide.Display(Lines.IsEmpty()
            ? LOCTEXT("PickFoundation", "Foundation grid guide · Aim at the edge of a matching FICSIT Foundation 1 m")
            : LOCTEXT("FoundationGuide", "Foundation grid guide · Amber line marks the seam · Build Gun validates placement"));
    }
    else if (Preview->IsA<AFGFactoryHologram>())
    {
        Guide.Foundation.Reset();
        FText Label = LOCTEXT("AimRoof", "Roof height guide · Aim at the top of a flat metal roof or concrete slab");
        if (Plan->Parts.IsValidIndex(PartIndex))
        {
            const auto& Part = Plan->Parts[PartIndex];
            const FTransform PartFrame = Part.InPlan * Plan->Transform;
            FVector Surface;
            double RoofTime = Limit;
            if (TraceShadowRoofTop(Part, PartFrame, Start, Start + Delta, RoofTime, Surface))
            {
                const FVector X = PartFrame.GetUnitAxis(EAxis::X) * 115.0;
                const FVector Y = PartFrame.GetUnitAxis(EAxis::Y) * 115.0;
                AddLine(Lines, Surface - X, Surface + X, EdgeColor, 4.0f);
                AddLine(Lines, Surface - Y, Surface + Y, GuideColor, 4.0f);
                Label = LOCTEXT("RoofGuide", "Roof height guide · Build Gun validates placement");
            }
            else if (Part.SourceClass.StartsWith(TEXT("/Game/FactoryGame/Buildable/Building/Roof/")) && !IsSupportedShadowRoof(Part))
                Label = LOCTEXT("UnsupportedRoof", "Roof height guide · This roof is unsupported. Use a flat metal roof.");
        }
        Guide.Display(MoveTemp(Label));
    }
    else
    {
        Guide.Foundation.Reset();
        Guide.Display(LOCTEXT("PickRecipe", "Alignment guide · Select a machine or FICSIT Foundation 1 m"));
    }
}

#undef LOCTEXT_NAMESPACE
