#include "ShadowPlan.h"
#include "ShadowBlueprintSource.h"
#include "Engine/World.h"
#include "Equipment/FGBuildGunBuild.h"
#include "FGBlueprintSubsystem.h"
#include "FGCharacterPlayer.h"
#include "FGPlayerState.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformFileManager.h"
#include "Hologram/FGBlueprintHologram.h"
#include "Patching/NativeHookManager.h"
#include "Subsystem/SubsystemActorManager.h"
#include "TimerManager.h"

namespace
{
#if !WITH_EDITOR
FDelegateHandle ConstructHook;
FDelegateHandle ConstructedHook;

AShadowPlanSubsystem* LocalPlans(UWorld* World)
{
    auto* Manager = World ? World->GetSubsystem<USubsystemActorManager>() : nullptr;
    return Manager ? Manager->GetSubsystemActor<AShadowPlanSubsystem>() : nullptr;
}
#endif
}

void AShadowPlanSubsystem::InstallPlanConstructionHooks()
{
#if !WITH_EDITOR
    ConstructHook = SUBSCRIBE_METHOD(UFGBuildGunStateBuild::Server_ConstructHologram,
        [](auto& Scope, UFGBuildGunStateBuild* State, FNetConstructionID Id, FConstructHologramMessage Message)
        {
            if (!IsInGameThread() || !State->HasAuthority()) return;
            auto* Plans = LocalPlans(State->GetWorld());
            if (!IsValid(Plans)) return;

            auto* Source = State->GetActiveBlueprintDescriptor();
            if (Plans->bConfirmingPlanBuild || (Plans->ClosingBuildState.Get() == State && Plans->ClosingBuildSource.Get() == Source))
            {
                Scope.Cancel();
                return;
            }
            auto* Preview = Plans->PlanBuildPreview.Get();
            if (!Preview || State->GetHologram() != Preview) return;

            FString Error;
            const auto SourceReadLock = Plans->VerifyPlanConstruction(*State, *Preview, Error);
            if (SourceReadLock)
            {
                TGuardValue<bool> Confirming(Plans->bConfirmingPlanBuild, true);
                Scope(State, Id, MoveTemp(Message));
            }
            else
            {
                Scope.Cancel();
                Plans->Notify(Error);
            }
            Plans->ClosePlanBuildAfterConfirmation(*State, Source);
        });

    ConstructedHook = SUBSCRIBE_METHOD_AFTER(AFGPlayerState::Native_OnBlueprintConstructed,
        [](AFGPlayerState* PlayerState, const FString& BlueprintName, int32 Count)
        {
            auto* World = PlayerState->GetWorld();
            auto* Plans = LocalPlans(World);
            if (!IsValid(Plans) || !Plans->bConfirmingPlanBuild || Count <= 0) return;
            const auto* Player = World->GetFirstPlayerController();
            if (!IsValid(Player) || Player->GetPlayerState<AFGPlayerState>() != PlayerState) return;
            Plans->Notify(FString::Printf(TEXT("Built %s for plan [%s] (game count: %d)."),
                *BlueprintName, *Plans->PlanBuildId.ToString(), Count));
        });
#endif
}

void AShadowPlanSubsystem::RemovePlanConstructionHooks()
{
#if !WITH_EDITOR
    UNSUBSCRIBE_METHOD(AFGPlayerState::Native_OnBlueprintConstructed, ConstructedHook);
    UNSUBSCRIBE_METHOD(UFGBuildGunStateBuild::Server_ConstructHologram, ConstructHook);
#endif
}

TUniquePtr<IFileHandle> AShadowPlanSubsystem::VerifyPlanConstruction(UFGBuildGunStateBuild& State,
    AFGBlueprintHologram& Preview, FString& Error) const
{
    const auto* Plan = Plans.FindByPredicate([this](const auto& Placement) { return Placement.Id == PlanBuildId; });
    if (!Plan || !Views.FindRef(PlanBuildId).IsValid() || !Preview.GetActorTransform().Equals(Plan->Transform, 0.01))
    {
        Error = TEXT("The plan or preview position changed. Open its preview again.");
        return nullptr;
    }
    auto* Gun = State.GetBuildGun();
    const auto* Player = Gun ? Cast<AFGCharacterPlayer>(Gun->GetInstigator()) : nullptr;
    if (!IsValid(Player) || !Player->IsLocallyControlled() || !Gun->IsEquipped()
        || FVector::DistSquared(Plan->Transform.GetLocation(), Player->GetPawnViewLocation())
            > FMath::Square(static_cast<double>(Gun->GetBuildGunRange())))
    {
        Error = TEXT("Move within Build Gun range and open the plan preview again.");
        return nullptr;
    }
    auto* Library = AFGBlueprintSubsystem::Get(GetWorld());
    auto* Descriptor = Preview.mBlueprintDescriptor.Get();
    if (!IsValid(Library) || !IsValid(Descriptor))
    {
        Error = TEXT("The Blueprint library or preview source is unavailable.");
        return nullptr;
    }

    // Construction loads the source again. Keep the verified file open without write or delete sharing.
    const FString Name = AFGBlueprintSubsystem::SanitizeBlueprintFileName(Descriptor->GetBlueprintNameAsString());
    const FString Path = Library->GetSessionBlueprintPath() / (Name + TEXT(".sbp"));
    TUniquePtr<IFileHandle> SourceReadLock(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path));
    auto* Resolved = ResolveBlueprintSource(*Library, *Plan, Error);
    if (!Resolved) return nullptr;
    if (!SourceReadLock || Resolved != Descriptor || !HasBlueprintSourceProof(Preview, *Plan))
    {
        Error = TEXT("The Blueprint source changed while the preview was open. Open the plan preview again.");
        return nullptr;
    }
    return SourceReadLock;
}

void AShadowPlanSubsystem::ClosePlanBuildAfterConfirmation(UFGBuildGunStateBuild& BuildState, UFGBlueprintDescriptor* BuildSource)
{
    ClosingBuildState = &BuildState;
    ClosingBuildSource = BuildSource;
    const auto PreviousPreview = PlanBuildPreview;
    // The Build Gun still resets and respawns its hologram inside the native confirmation.
    GetWorldTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this, PreviousPreview]
    {
        auto* State = ClosingBuildState.Get();
        auto* Source = ClosingBuildSource.Get();
        ClosingBuildState.Reset();
        ClosingBuildSource.Reset();
        if (!PlanBuildPreview.HasSameIndexAndSerialNumber(PreviousPreview)) return;
        PlanBuildPreview.Reset();
        PlanBuildId.Invalidate();
        auto* Gun = State ? State->GetBuildGun() : nullptr;
        auto* Player = Gun ? Cast<AFGCharacterPlayer>(Gun->GetInstigator()) : nullptr;
        if (IsValid(Player) && Gun->IsEquipped() && Gun->GetCurrentState() == State
            && State->GetActiveBlueprintDescriptor() == Source) Player->UnequipBuildGun();
    }));
}
