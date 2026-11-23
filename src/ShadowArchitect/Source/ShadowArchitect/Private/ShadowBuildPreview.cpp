#include "ShadowPlan.h"
#include "ShadowBlueprintSource.h"
#include "Engine/World.h"
#include "Equipment/FGBuildGunBuild.h"
#include "FGBlueprintSubsystem.h"
#include "FGCharacterPlayer.h"
#include "GameFramework/PlayerController.h"
#include "Hologram/FGBlueprintHologram.h"

namespace
{
AFGBlueprintHologram* CurrentBlueprintPreview(const AFGBuildGun& Gun)
{
    const auto* State = Gun.IsEquipped() ? Cast<UFGBuildGunStateBuild>(Gun.GetCurrentState()) : nullptr;
    return IsValid(State) ? Cast<AFGBlueprintHologram>(State->GetHologram()) : nullptr;
}
}

void AShadowPlanSubsystem::OpenSelectedBuildPreview()
{
    const auto* Plan = Plans.FindByPredicate([this](const auto& Placement) { return Placement.Id == Selected; });
    if (!Plan || !Views.FindRef(Selected).IsValid())
    {
        Notify(TEXT("Select a plan first."));
        return;
    }

    APlayerController* PC = GetWorld()->GetFirstPlayerController();
    auto* Player = IsValid(PC) && PC->IsLocalController() ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
    auto* Gun = IsValid(Player) ? Player->GetBuildGun() : nullptr;
    if (!IsValid(Gun) || !Player->CanEquipBuildGun())
    {
        Notify(TEXT("The local player cannot use the Build Gun here."));
        return;
    }

    auto* Library = AFGBlueprintSubsystem::Get(GetWorld());
    FString Error = TEXT("Blueprint library is unavailable.");
    auto* Source = IsValid(Library) ? ResolveBlueprintSource(*Library, *Plan, Error) : nullptr;
    if (!Source)
    {
        Notify(Error);
        return;
    }

    const FTransform Target = Plan->Transform;
    if (FVector::DistSquared(Target.GetLocation(), Player->GetPawnViewLocation())
        > FMath::Square(static_cast<double>(Gun->GetDefaultBuildGunRange())))
    {
        Notify(TEXT("Move within Build Gun range of the selected plan."));
        return;
    }

    // Start a fresh hologram. The hotkey would toggle off an already active Blueprint of this name.
    if (Gun->IsEquipped() && (Gun->IsBlueprintDescriptorActive(Source) || IsValid(CurrentBlueprintPreview(*Gun)))
        && !Player->UnequipBuildGun())
    {
        Notify(TEXT("Could not close the current Blueprint preview."));
        return;
    }
    PlanBuildPreview.Reset();
    PlanBuildId.Invalidate();
    Player->HotKeyBlueprint(Source->GetBlueprintNameAsString());
    auto* Preview = CurrentBlueprintPreview(*Gun);
    if (!IsValid(Preview) || Preview->mBlueprintDescriptor.Get() != Source
        || Preview->mBuildableToNewRoot.Num() != Plan->Parts.Num()
        || !HasBlueprintSourceProof(*Preview, *Plan))
    {
        Player->UnequipBuildGun();
        Notify(TEXT("Could not open the verified Blueprint preview."));
        return;
    }

    // The snapshot and the stock hologram share the actor anchor. Locking keeps the
    // saved pose from being replaced by the next trace and snap update.
    const bool bAligned = Preview->CanLockHologram() && Preview->SetActorTransform(Target);
    if (bAligned)
        Preview->LockHologramPosition(true);
    if (!bAligned || !Preview->IsHologramLocked() || Preview->IsDisabled()
        || !Preview->GetActorTransform().Equals(Target, 0.01))
    {
        Player->UnequipBuildGun();
        Notify(TEXT("Could not lock the Blueprint preview at the selected plan."));
        return;
    }

    Preview->SetActorHiddenInGame(false);
    Preview->ValidatePlacementAndCost(Gun->GetInventory());
    PlanBuildPreview = Preview;
    PlanBuildId = Plan->Id;
    Notify(FString::Printf(TEXT("Opened Blueprint preview [%s]."), *PlanBuildId.ToString()));
}
