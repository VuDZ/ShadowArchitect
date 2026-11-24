#pragma once

#include "ShadowPlan.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Rendering/DrawElements.h"
#include "Widgets/SLeafWidget.h"

class SShadowWorldLines : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SShadowWorldLines) {}
        SLATE_ARGUMENT(TWeakObjectPtr<APlayerController>, Player)
        SLATE_ARGUMENT(TFunction<TConstArrayView<FShadowAlignmentSegment>()>, GetLines)
    SLATE_END_ARGS()

    void Construct(const FArguments& Args)
    {
        Player = Args._Player;
        GetLines = Args._GetLines;
    }

    FVector2D ComputeDesiredSize(float) const override { return FVector2D::ZeroVector; }

    int32 OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&, FSlateWindowElementList& Draw,
        int32 Layer, const FWidgetStyle&, bool) const override
    {
        const auto* PC = Player.Get();
        const auto* Viewport = PC ? PC->GetWorld()->GetGameViewport() : nullptr;
        FVector2D ViewportSize = FVector2D::ZeroVector;
        if (Viewport) Viewport->GetViewportSize(ViewportSize);
        if (ViewportSize.X <= 0 || ViewportSize.Y <= 0) return Layer;

        const FVector2D Scale = Geometry.GetLocalSize() / ViewportSize;
        TArray<FVector2D> ScreenPoints;
        ScreenPoints.SetNum(2);
        for (const auto& Line : GetLines())
        {
            if (!PC->ProjectWorldLocationToScreen(Line.Start, ScreenPoints[0], true)
                || !PC->ProjectWorldLocationToScreen(Line.End, ScreenPoints[1], true)) continue;
            ScreenPoints[0] *= Scale;
            ScreenPoints[1] *= Scale;
            FSlateDrawElement::MakeLines(Draw, Layer, Geometry.ToPaintGeometry(), ScreenPoints,
                ESlateDrawEffect::None, FLinearColor(0.02f, 0.025f, 0.03f, 0.9f), true, Line.Thickness + 2);
            FSlateDrawElement::MakeLines(Draw, Layer + 1, Geometry.ToPaintGeometry(), ScreenPoints,
                ESlateDrawEffect::None, Line.Color, true, Line.Thickness);
        }
        return Layer + 1;
    }

private:
    TWeakObjectPtr<APlayerController> Player;
    TFunction<TConstArrayView<FShadowAlignmentSegment>()> GetLines;
};
