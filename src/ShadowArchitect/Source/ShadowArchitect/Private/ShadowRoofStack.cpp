#include "ShadowPlan.h"
#include "ShadowAlignmentGeometry.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Hologram/FGBlueprintHologram.h"

DEFINE_LOG_CATEGORY_STATIC(LogShadowRoofStack, Log, All);

void AShadowPlanSubsystem::PlaceBlueprintOnRoof(const FGuid& Id)
{
    auto* PC = GetWorld()->GetFirstPlayerController();
    const auto* Lower = Plans.FindByPredicate([&Id](const FShadowPlacement& Plan) { return Plan.Id == Id; });
    const auto* View = Views.FindRef(Id).Get();
    if (!IsValid(PC) || !PC->IsLocalController() || !CanUsePlanControls(*PC)
        || !Lower || !View || View->IsHidden() || ViewMode == EShadowView::Footprint) return;

    auto* Preview = Cast<AFGBlueprintHologram>(CurrentPreview());
    if (!IsValid(Preview))
    {
        Notify(TEXT("Select a blueprint with the Build Gun first."));
        return;
    }

    if (!Lower->Transform.GetUnitAxis(EAxis::Z).Equals(FVector::UpVector, UE_KINDA_SMALL_NUMBER))
    {
        Notify(TEXT("Place the lower plan upright before stacking blueprints."));
        return;
    }

    FVector Start, Delta;
    FVector Surface = FVector::ZeroVector;
    double Closest = TracePlanAim(*PC, PlanPickRange, Start, Delta);
    const double WorldHit = Closest;
    const FVector End = Start + Delta;
    for (const FShadowPart& Part : Lower->Parts)
        TraceShadowRoofTop(Part, Part.InPlan * Lower->Transform, Start, End, Closest, Surface);
    if (Closest == WorldHit)
    {
        UE_LOG(LogShadowRoofStack, Verbose, TEXT("No supported rooftop hit on %s within %.0f cm."),
            *Id.ToString(), WorldHit * PlanPickRange);
        Notify(TEXT("Aim at the top of a flat metal roof or concrete 1 m slab."));
        return;
    }

    FShadowPlacement Upper;
    FString Result;
    if (!CaptureBlueprint(*Preview, Upper, Result))
    {
        Notify(Result);
        return;
    }
    const FBox Bounds = CalculateFootprint(Upper.Parts);
    if (!Bounds.IsValid)
    {
        Notify(TEXT("Could not measure the bottom of the selected blueprint."));
        return;
    }

    FVector Anchor = Lower->Transform.GetLocation();
    Anchor.Z = Surface.Z - Bounds.Min.Z;
    Upper.Transform = FTransform(FRotator(0, Lower->Transform.Rotator().Yaw, 0), Anchor);
    if (AddCapturedPlan(MoveTemp(Upper), Result))
        Result += FString::Printf(TEXT(" Roof height %.1f cm."), Surface.Z);
    Notify(Result);
}
