#include "ShadowPlan.h"
#include "ShadowAlignmentGeometry.h"
#include "Equipment/FGBuildGunBuild.h"
#include "GameFramework/PlayerController.h"
#include "Hologram/FGBlueprintHologram.h"
#include "Patching/NativeHookManager.h"
#include "Subsystem/SubsystemActorManager.h"

namespace
{
#if !WITH_EDITOR
FDelegateHandle TraceHook;
FDelegateHandle ValidHitHook;
FDelegateHandle PositionHook;

AShadowPlanSubsystem* LocalPlacementPlans(UWorld* World)
{
    auto* Manager = World ? World->GetSubsystem<USubsystemActorManager>() : nullptr;
    return Manager ? Manager->GetSubsystemActor<AShadowPlanSubsystem>() : nullptr;
}

bool IsPlanSupportHit(const FHitResult& Hit)
{
    return Hit.bBlockingHit && !Hit.GetComponent() && Cast<AShadowProjection>(Hit.GetActor());
}
#endif
}

void AShadowPlanSubsystem::TogglePlacementSupport()
{
    bUsePlanSupport = !bUsePlanSupport;
    Notify(bUsePlanSupport ? TEXT("Placement support: World and plans.") : TEXT("Placement support: World only."));
}

void AShadowPlanSubsystem::InstallPlacementSupportHooks()
{
#if !WITH_EDITOR
    TraceHook = SUBSCRIBE_METHOD_AFTER(AFGBuildGun::TraceForBuilding,
        [](const AFGBuildGun* Gun, APawn* Pawn, FHitResult& Hit)
        {
            if (!IsInGameThread() || !IsValid(Pawn) || !Pawn->IsLocallyControlled()) return;
            const auto* Build = Gun->IsEquipped() ? Cast<UFGBuildGunStateBuild>(Gun->GetCurrentState()) : nullptr;
            auto* Preview = Build ? Cast<AFGBlueprintHologram>(Build->GetHologram()) : nullptr;
            if (!IsValid(Preview) || Preview->IsHologramLocked() || Preview->IsDisabled()) return;

            auto* World = Gun->GetWorld();
            const auto* Player = World->GetFirstPlayerController();
            auto* PlansActor = LocalPlacementPlans(World);
            if (IsValid(PlansActor) && IsValid(Player) && Player->GetPawn() == Pawn)
                PlansActor->TracePlacementSupport(*Preview, *Player, Gun->GetBuildGunRange(), Hit);
        });

    ValidHitHook = SUBSCRIBE_UOBJECT_METHOD(AFGBlueprintHologram, IsValidHitResult,
        [](auto& Scope, const AFGBlueprintHologram*, const FHitResult& Hit)
        {
            if (IsPlanSupportHit(Hit)) Scope.Override(true);
        });

    PositionHook = SUBSCRIBE_UOBJECT_METHOD(AFGBlueprintHologram, PostHologramPlacement,
        [](auto&, AFGBlueprintHologram* Preview, const FHitResult& Hit, bool)
        {
            if (!IsInGameThread() || !IsPlanSupportHit(Hit)) return;
            const auto* PlansActor = LocalPlacementPlans(Preview->GetWorld());
            if (!IsValid(PlansActor) || PlansActor->SupportGeometryPreview.Get() != Preview
                || !PlansActor->SupportGeometryBounds.IsValid) return;

            // The stock placement has applied scroll rotation. Keep it and seat the measured bottom on the face.
            const FTransform Orientation(Preview->GetActorQuat(), FVector::ZeroVector, Preview->GetActorScale3D());
            FVector Anchor = Hit.ImpactPoint;
            Anchor.Z -= PlansActor->SupportGeometryBounds.TransformBy(Orientation).Min.Z;
            Preview->SetActorLocation(Anchor);
        });
#endif
}

void AShadowPlanSubsystem::RemovePlacementSupportHooks()
{
#if !WITH_EDITOR
    UNSUBSCRIBE_UOBJECT_METHOD(AFGBlueprintHologram, PostHologramPlacement, PositionHook);
    UNSUBSCRIBE_UOBJECT_METHOD(AFGBlueprintHologram, IsValidHitResult, ValidHitHook);
    UNSUBSCRIBE_METHOD(AFGBuildGun::TraceForBuilding, TraceHook);
#endif
}

void AShadowPlanSubsystem::TracePlacementSupport(AFGBlueprintHologram& Preview, const APlayerController& Player,
    double Range, FHitResult& Hit)
{
    if (!bUsePlanSupport || !bVisible || ViewMode == EShadowView::Footprint || Range <= UE_SMALL_NUMBER) return;

    FVector Start, Delta;
    double Closest = TracePlanAim(Player, Range, Start, Delta);
    // Retain blockers found by the stock Build Gun trace even when they do not block Visibility.
    if (Hit.bBlockingHit)
        Closest = FMath::Min(Closest, FVector::DotProduct(Hit.ImpactPoint - Start, Delta) / Delta.SizeSquared());
    if (Closest <= 0) return;

    const FVector End = Start + Delta;
    FVector Surface = FVector::ZeroVector;
    AShadowProjection* Target = nullptr;
    for (const FShadowPlacement& Plan : Plans)
    {
        auto* View = Views.FindRef(Plan.Id).Get();
        if (!View || View->IsHidden()) continue;
        for (const FShadowPart& Part : Plan.Parts)
            if (TraceShadowRoofTop(Part, Part.InPlan * Plan.Transform, Start, End, Closest, Surface)) Target = View;
    }
    if (!Target) return;

    const int32 PartCount = Preview.mBuildableToNewRoot.Num();
    if (SupportGeometryPreview.Get() != &Preview || SupportGeometrySource.Get() != Preview.mBlueprintDescriptor.Get()
        || SupportGeometryPartCount != PartCount || !SupportGeometryBounds.IsValid)
    {
        SupportGeometryPreview = &Preview;
        SupportGeometrySource = Preview.mBlueprintDescriptor.Get();
        SupportGeometryPartCount = PartCount;
        TArray<FShadowPart> Parts;
        SupportGeometryBounds = CaptureBlueprintGeometry(Preview, Parts) ? CalculateFootprint(Parts) : FBox(ForceInit);
    }
    if (!SupportGeometryBounds.IsValid) return;

    Hit = FHitResult(Target, nullptr, Surface, FVector::UpVector);
    Hit.bBlockingHit = true;
    Hit.Time = Closest;
    Hit.Distance = Closest * Range;
    Hit.TraceStart = Start;
    Hit.TraceEnd = End;
}
