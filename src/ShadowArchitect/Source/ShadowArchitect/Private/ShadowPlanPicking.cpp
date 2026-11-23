#include "ShadowPlan.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Hologram/FGHologram.h"

namespace
{
double FirstBoxSurface(const FBox& Bounds, const FVector& Start, const FVector& Delta, double Limit)
{
    if (!Bounds.IsValid) return Limit;
    double Enter = -TNumericLimits<double>::Max();
    double Exit = TNumericLimits<double>::Max();
    for (int32 Axis = 0; Axis < 3; ++Axis)
    {
        if (FMath::IsNearlyZero(Delta[Axis]))
        {
            if (Start[Axis] < Bounds.Min[Axis] || Start[Axis] > Bounds.Max[Axis]) return Limit;
            continue;
        }
        const double A = (Bounds.Min[Axis] - Start[Axis]) / Delta[Axis];
        const double B = (Bounds.Max[Axis] - Start[Axis]) / Delta[Axis];
        Enter = FMath::Max(Enter, FMath::Min(A, B));
        Exit = FMath::Min(Exit, FMath::Max(A, B));
        if (Enter > Exit) return Limit;
    }
    const double Surface = Enter > 0 ? Enter : Exit;
    return Surface > 0 ? FMath::Min(Surface, Limit) : Limit;
}
}

FGuid AShadowPlanSubsystem::FindPlanUnderCrosshair() const
{
    const APlayerController* PC = GetWorld()->GetFirstPlayerController();
    if (!bVisible || !IsValid(PC) || !PC->IsLocalController()) return {};

    FVector Start;
    FRotator Aim;
    PC->GetPlayerViewPoint(Start, Aim);
    constexpr double Range = 10000.0;
    const FVector Delta = Aim.Vector() * Range;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(ShadowPlanPick), true, PC->GetPawn());
    if (const APawn* Pawn = PC->GetPawn())
    {
        TArray<AActor*> Attached;
        Pawn->GetAttachedActors(Attached, true, true);
        Query.AddIgnoredActors(Attached);
    }
    if (AFGHologram* Preview = ActivePreview())
    {
        Query.AddIgnoredActor(Preview);
        TArray<AActor*> Attached;
        Preview->GetAttachedActors(Attached, true, true);
        Query.AddIgnoredActors(Attached);
    }
    FHitResult Hit;
    GetWorld()->LineTraceSingleByChannel(Hit, Start, Start + Delta, ECC_Visibility, Query);
    double Best = Hit.bBlockingHit ? Hit.Time : 1.0;
    FGuid Target;
    for (const FShadowPlacement& Plan : Plans)
    {
        const AShadowProjection* View = Views.FindRef(Plan.Id).Get();
        if (!View || View->IsHidden()) continue;

        // Keep the same ray parameter through both transforms to compare world distances.
        const FVector PlanStart = Plan.Transform.InverseTransformPosition(Start);
        const FVector PlanDelta = Plan.Transform.InverseTransformVector(Delta);
        double Distance = Best;
        if (ViewMode == EShadowView::Footprint)
        {
            Distance = FirstBoxSurface(CalculateFootprint(Plan.Parts), PlanStart, PlanDelta, Distance);
        }
        else
        {
            for (const FShadowPart& Part : Plan.Parts)
            {
                const FVector Extent = Part.BoundsSize * 0.5;
                Distance = FirstBoxSurface(FBox(Part.BoundsCenter - Extent, Part.BoundsCenter + Extent),
                    Part.InPlan.InverseTransformPosition(PlanStart),
                    Part.InPlan.InverseTransformVector(PlanDelta), Distance);
            }
        }
        if (Distance < Best)
        {
            Best = Distance;
            Target = Plan.Id;
        }
    }
    return Target;
}
