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

int32 FindShadowPartOnRay(const FShadowPlacement& Plan, const FVector& Start, const FVector& Delta, double& Limit)
{
    const FVector PlanStart = Plan.Transform.InverseTransformPosition(Start);
    const FVector PlanDelta = Plan.Transform.InverseTransformVector(Delta);
    int32 Target = INDEX_NONE;
    for (int32 Index = 0; Index < Plan.Parts.Num(); ++Index)
    {
        const FShadowPart& Part = Plan.Parts[Index];
        const FVector Extent = Part.BoundsSize * 0.5;
        const double Distance = FirstBoxSurface(FBox(Part.BoundsCenter - Extent, Part.BoundsCenter + Extent),
            Part.InPlan.InverseTransformPosition(PlanStart), Part.InPlan.InverseTransformVector(PlanDelta), Limit);
        if (Distance < Limit)
        {
            Limit = Distance;
            Target = Index;
        }
    }
    return Target;
}

double AShadowPlanSubsystem::TracePlanAim(const APlayerController& Player, double Range,
    FVector& Start, FVector& Delta) const
{
    FRotator Aim;
    Player.GetPlayerViewPoint(Start, Aim);
    Delta = Aim.Vector() * Range;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(ShadowPlanPick), true, Player.GetPawn());
    if (const APawn* Pawn = Player.GetPawn())
    {
        TArray<AActor*> Attached;
        Pawn->GetAttachedActors(Attached, true, true);
        Query.AddIgnoredActors(Attached);
    }
    if (AFGHologram* Preview = CurrentPreview(); IsValid(Preview))
    {
        Query.AddIgnoredActor(Preview);
        TArray<AActor*> Attached;
        Preview->GetAttachedActors(Attached, true, true);
        Query.AddIgnoredActors(Attached);
    }
    FHitResult Hit;
    GetWorld()->LineTraceSingleByChannel(Hit, Start, Start + Delta, ECC_Visibility, Query);
    return Hit.bBlockingHit ? Hit.Time : 1.0;
}

FGuid AShadowPlanSubsystem::FindPlanUnderCrosshair() const
{
    const APlayerController* PC = GetWorld()->GetFirstPlayerController();
    if (!bVisible || !IsValid(PC) || !PC->IsLocalController()) return {};

    FVector Start, Delta;
    double Best = TracePlanAim(*PC, PlanPickRange, Start, Delta);
    FGuid Target;
    for (const FShadowPlacement& Plan : Plans)
    {
        const AShadowProjection* View = Views.FindRef(Plan.Id).Get();
        if (!View || View->IsHidden()) continue;

        // Keep the same ray parameter through both transforms to compare world distances.
        double Distance = Best;
        if (ViewMode == EShadowView::Footprint)
        {
            Distance = FirstBoxSurface(CalculateFootprint(Plan.Parts), Plan.Transform.InverseTransformPosition(Start),
                Plan.Transform.InverseTransformVector(Delta), Distance);
        }
        else
        {
            FindShadowPartOnRay(Plan, Start, Delta, Distance);
        }
        if (Distance < Best)
        {
            Best = Distance;
            Target = Plan.Id;
        }
    }
    return Target;
}
