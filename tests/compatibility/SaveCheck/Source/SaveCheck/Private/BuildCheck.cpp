#include "Buildables/FGBuildable.h"
#include "Camera/CameraActor.h"
#include "Components/CapsuleComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Equipment/FGBuildGunBuild.h"
#include "FGBlueprintProxy.h"
#include "FGBlueprintSubsystem.h"
#include "FGCentralStorageSubsystem.h"
#include "FGCharacterPlayer.h"
#include "FGConstructDisqualifier.h"
#include "FGGameState.h"
#include "FGItemPickup_Spawnable.h"
#include "FGLightweightBuildableSubsystem.h"
#include "FGPlayerState.h"
#include "FGSaveSession.h"
#include "FGSaveSystem.h"
#include "Hologram/FGBlueprintHologram.h"
#include "HAL/FileManager.h"
#include "InputKeyEventArgs.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogSaveCheckBuild, Log, All);

bool ClickPlanMenuButton(FStringView Label);

namespace
{
struct FPlanPose
{
    int32 Count = 0;
    FGuid Id;
    FTransform Anchor;
    TArray<FTransform> Parts;
    FString SourceDigest;
    FVector AimPoint = FVector::ZeroVector;
};

FPlanPose ReadPlan(UWorld& World, int32 RecordIndex = INDEX_NONE)
{
    FPlanPose Result;
    UClass* Type = FindObject<UClass>(nullptr, TEXT("/Script/ShadowArchitect.ShadowPlanSubsystem"));
    if (!Type) return Result;
    TActorIterator<AActor> It(&World, Type);
    if (!It) return Result;
    const FArrayProperty* Plans = FindFProperty<FArrayProperty>(Type, TEXT("Plans"));
    const auto* RecordType = CastField<FStructProperty>(Plans ? Plans->Inner : nullptr);
    if (!RecordType) return Result;
    FScriptArrayHelper Records(Plans, Plans->ContainerPtrToValuePtr<void>(*It));
    Result.Count = Records.Num();
    if (Records.Num() == 0) return Result;
    const auto* Id = FindFProperty<FStructProperty>(RecordType->Struct, TEXT("Id"));
    const auto* Transform = FindFProperty<FStructProperty>(RecordType->Struct, TEXT("Transform"));
    const auto* Parts = FindFProperty<FArrayProperty>(RecordType->Struct, TEXT("Parts"));
    const auto* PartType = CastField<FStructProperty>(Parts ? Parts->Inner : nullptr);
    const auto* InPlan = PartType ? FindFProperty<FStructProperty>(PartType->Struct, TEXT("InPlan")) : nullptr;
    const auto* BoundsCenter = PartType ? FindFProperty<FStructProperty>(PartType->Struct, TEXT("BoundsCenter")) : nullptr;
    const auto* BoundsSize = PartType ? FindFProperty<FStructProperty>(PartType->Struct, TEXT("BoundsSize")) : nullptr;
    if (!Id || !Transform || !InPlan) return Result;
    const void* Record = Records.GetRawPtr(RecordIndex == INDEX_NONE ? Records.Num() - 1 : RecordIndex);
    Result.Id = *Id->ContainerPtrToValuePtr<FGuid>(Record);
    Result.Anchor = *Transform->ContainerPtrToValuePtr<FTransform>(Record);
    if (const auto* Digest = FindFProperty<FStrProperty>(RecordType->Struct, TEXT("BlueprintSourceProof")))
        Result.SourceDigest = Digest->GetPropertyValue_InContainer(Record);
    FScriptArrayHelper PartArray(Parts, Parts->ContainerPtrToValuePtr<void>(Record));
    double LargestPart = 0;
    for (int32 Index = 0; Index < PartArray.Num(); ++Index)
    {
        const auto* Part = PartArray.GetRawPtr(Index);
        const FTransform& Pose = Result.Parts.Add_GetRef(*InPlan->ContainerPtrToValuePtr<FTransform>(Part));
        if (BoundsCenter && BoundsSize)
        {
            const FVector Size = *BoundsSize->ContainerPtrToValuePtr<FVector>(Part);
            const double Volume = Size.X * Size.Y * Size.Z;
            if (Volume > LargestPart)
            {
                LargestPart = Volume;
                Result.AimPoint = Result.Anchor.TransformPosition(Pose.TransformPosition(
                    *BoundsCenter->ContainerPtrToValuePtr<FVector>(Part)));
            }
        }
    }
    return Result;
}

struct FBuildings
{
    int32 Count = 0;
    int32 Blueprints = 0;
    int32 BlueprintParts = 0;
};

FBuildings ReadBuildings(UWorld& World)
{
    FBuildings Result;
    for (TActorIterator<AFGBuildable> It(&World); It; ++It) ++Result.Count;
    if (auto* Lightweights = AFGLightweightBuildableSubsystem::Get(&World))
        for (const auto& Entry : Lightweights->GetAllLightweightBuildableInstances())
            for (const auto& Data : Entry.Value)
                if (Data.IsValid()) ++Result.Count;
    for (TActorIterator<AFGBlueprintProxy> It(&World); It; ++It)
    {
        ++Result.Blueprints;
        Result.BlueprintParts += It->GetBuildables().Num();
        for (const auto& Entry : It->GetLightweightClassAndIndices()) Result.BlueprintParts += Entry.Indices.Num();
    }
    return Result;
}

void Key(APlayerController& Player, FKey Input, EInputEvent Event)
{
    auto* Viewport = GEngine->GameViewport ? GEngine->GameViewport->Viewport : nullptr;
    Player.InputKey(FInputKeyEventArgs(Viewport, FInputDeviceId::CreateFromInternalId(0), Input, Event, FPlatformTime::Cycles64()));
}

void PlanChord(APlayerController& Player, FKey Input, EInputEvent Event)
{
    if (Event == IE_Pressed)
    {
        Key(Player, EKeys::LeftControl, Event);
        Key(Player, EKeys::LeftShift, Event);
    }
    Key(Player, Input, Event);
    if (Event == IE_Released)
    {
        Key(Player, EKeys::LeftShift, Event);
        Key(Player, EKeys::LeftControl, Event);
    }
}

TMap<TSubclassOf<UFGItemDescriptor>, int32> StockOf(UFGInventoryComponent& Inventory)
{
    TArray<FInventoryStack> Stacks;
    Inventory.GetInventoryStacks(Stacks);
    TMap<TSubclassOf<UFGItemDescriptor>, int32> Amounts;
    for (const FInventoryStack& Stack : Stacks)
        if (Stack.HasItems()) Amounts.FindOrAdd(Stack.Item.GetItemClass()) += Stack.NumItems;
    return Amounts;
}

struct FSourceProbe
{
    FString ExpectedSource;
    int32 Age = 0;
    int32 Selections = 0;
    FPlanPose Plan;
    FBuildings Before;
    int32 NearbyBefore = 0;
    TMap<TSubclassOf<UFGItemDescriptor>, int32> Stock;
    FString OpenedSource;
    bool bSawPreview = false;
    bool bReplaced = false;

    int32 CountNearbyBuildings(UWorld& World) const
    {
        FBox Bounds(ForceInit);
        Bounds += Plan.Anchor.GetLocation();
        for (const FTransform& Part : Plan.Parts)
            Bounds += (Part * Plan.Anchor).GetLocation();
        Bounds = Bounds.ExpandBy(800);

        int32 Count = 0;
        for (TActorIterator<AFGBuildable> It(&World); It; ++It)
            if (Bounds.IsInsideOrOn(It->GetActorLocation())) ++Count;
        if (auto* Lightweights = AFGLightweightBuildableSubsystem::Get(&World))
            for (const auto& Entry : Lightweights->GetAllLightweightBuildableInstances())
                for (const auto& Data : Entry.Value)
                    if (Data.IsValid() && Bounds.IsInsideOrOn(Data.Transform.GetLocation())) ++Count;
        return Count;
    }

    static bool ReplaceFixture(UWorld& World)
    {
        FString Directory;
        auto* Library = AFGBlueprintSubsystem::Get(&World);
        if (!Library || !FParse::Value(FCommandLine::Get(), TEXT("FGBlueprintDir="), Directory)) return false;
        FString LibraryDirectory = Library->GetSessionBlueprintPath();
        FPaths::NormalizeDirectoryName(Directory);
        FPaths::NormalizeDirectoryName(LibraryDirectory);
        if (!FPaths::IsSamePath(Directory, LibraryDirectory)) return false;
        const FString Original = Directory / TEXT("QA_A.sbp");
        const FString Replacement = Directory / TEXT("QA_B.replacement");
        auto& Files = IFileManager::Get();
        return Files.FileExists(*Original) && Files.FileExists(*Replacement)
            && Files.Copy(*Original, *Replacement, true, false) == COPY_OK;
    }

    bool Tick(UWorld& World, APlayerController& Player, AFGCharacterPlayer& Character)
    {
        const bool bLegacy = ExpectedSource == TEXT("legacy");
        const bool bLive = ExpectedSource == TEXT("live-replaced");
        const bool bFound = ExpectedSource.StartsWith(TEXT("found:")) || (bLive && !bReplaced);
        auto& Inventory = *Character.GetInventory();
        ++Age;
        if (Age == 12)
        {
            Plan = ReadPlan(World, bLegacy ? 0 : INDEX_NONE);
            if (!Plan.Id.IsValid() || Plan.SourceDigest.IsEmpty() != bLegacy)
            {
                UE_LOG(LogSaveCheckBuild, Error, TEXT("source_case=%s invalid input slot"), *ExpectedSource);
                FPlatformMisc::RequestExit(false);
                return false;
            }
            Before = ReadBuildings(World);
            NearbyBefore = CountNearbyBuildings(World);
            Stock = StockOf(Inventory);
            Selections = bLegacy ? 1 : Plan.Count;
        }
        if (Age < 12) return true;
        const int32 InputFrame = Age - 12;
        const int32 RequestFrame = 2 * Selections + (bReplaced ? 16 : 0);
        if (InputFrame < 2 * Selections)
            PlanChord(Player, EKeys::J, InputFrame % 2 == 0 ? IE_Pressed : IE_Released);
        else if (InputFrame == RequestFrame || InputFrame == RequestFrame + 1)
            PlanChord(Player, EKeys::F8, InputFrame == RequestFrame ? IE_Pressed : IE_Released);
        else if (InputFrame == RequestFrame + 8)
        {
            auto* Gun = Character.GetBuildGun();
            auto* State = Gun && Gun->IsEquipped() ? Cast<UFGBuildGunStateBuild>(Gun->GetCurrentState()) : nullptr;
            auto* Preview = State ? Cast<AFGBlueprintHologram>(State->GetHologram()) : nullptr;
            const auto* Descriptor = Preview ? Preview->mBlueprintDescriptor.Get() : nullptr;
            OpenedSource = Descriptor ? Descriptor->GetBlueprintNameAsString() : TEXT("none");
            bSawPreview = IsValid(Preview);
            if (bSawPreview) Character.UnequipBuildGun();
        }
        else if (InputFrame == RequestFrame + 14)
        {
            const FBuildings After = ReadBuildings(World);
            const int32 NearbyAfter = CountNearbyBuildings(World);
            const FPlanPose Current = ReadPlan(World, bLegacy ? 0 : INDEX_NONE);
            UClass* ViewClass = FindObject<UClass>(nullptr, TEXT("/Script/ShadowArchitect.ShadowProjection"));
            bool bVisible = false;
            if (ViewClass)
                for (TActorIterator<AActor> View(&World, ViewClass); View; ++View)
                    if (!View->IsHidden() && View->GetActorTransform().Equals(Plan.Anchor, 0.01)) bVisible = true;
            const bool bInventorySame = Stock.OrderIndependentCompareEqual(StockOf(Inventory));
            const bool bPlanSame = Current.Id == Plan.Id && Current.Count == Plan.Count;
            const bool bUnchanged = NearbyBefore == NearbyAfter && Before.Blueprints == After.Blueprints
                && Before.BlueprintParts == After.BlueprintParts && bInventorySame && bPlanSame;
            const auto* Gun = Character.GetBuildGun();
            const bool bPass = bVisible && bUnchanged
                && (bFound ? bSawPreview && OpenedSource == (bLive ? TEXT("QA_A") : ExpectedSource.Mid(6)) : !bSawPreview)
                && (!bSawPreview || !IsValid(Gun) || !Gun->IsEquipped());
            UE_LOG(LogSaveCheckBuild, Display,
                TEXT("source_case=%s buildings=%d->%d near_plan=%d->%d proxies=%d->%d parts=%d->%d inventory_same=%d plan_same=%d"),
                *ExpectedSource, Before.Count, After.Count, NearbyBefore, NearbyAfter, Before.Blueprints, After.Blueprints,
                Before.BlueprintParts, After.BlueprintParts, bInventorySame, bPlanSame);
            if (bPass && bLive && !bReplaced)
            {
                bReplaced = ReplaceFixture(World);
                UE_LOG(LogSaveCheckBuild, Display, TEXT("source_live_replaced=%d id=%s preview=%s"),
                    bReplaced, *Plan.Id.ToString(), *OpenedSource);
                if (bReplaced) return true;
            }
            UE_LOG(LogSaveCheckBuild, Display, TEXT("source_case=%s id=%s digest=%s preview=%s visible=%d unchanged=%d pass=%d"),
                *ExpectedSource, *Plan.Id.ToString(), *Plan.SourceDigest, *OpenedSource, bVisible, bUnchanged,
                bPass && (!bLive || bReplaced));
            FPlatformMisc::RequestExit(false);
            return false;
        }
        return true;
    }
};

struct FBuildProbe
{
    enum class EStep { Ready, PrepareMenuFixture, Native, Capture, Open, Aligned, Confirm, Result, Use };
    enum class ECase { Preview, Shortage, Success, Occupied, SourceChanged };

    EStep Step = EStep::Ready;
    ECase Scenario = ECase::Shortage;
    int32 Frames = 0;
    int32 Age = 0;
    int32 PreviewPass = 0;
    int32 BeforeProjections = 0;
    FPlanPose Plan;
    FPlanPose UnrelatedPlan;
    FBuildings Before;
    TSet<const AFGBlueprintProxy*> ExistingBlueprints;
    FString Source;
    FString SeedSlot;
    TOptional<FSourceProbe> SourceProbe;
    FVector Aim;
    TMap<TSubclassOf<UFGItemDescriptor>, int32> Cost;
    TMap<TSubclassOf<UFGItemDescriptor>, int32> Stock;
    TMap<TSubclassOf<UFGItemDescriptor>, int32> CentralStock;
    TSubclassOf<UFGItemDescriptor> Missing;
    FTransform SiteOrigin;
    int32 SiteAttempt = 0;
    TWeakObjectPtr<AFGItemPickup_Spawnable> Pickup;
    int32 PickupStock = 0;
    bool bViaMenu = false;
    TWeakObjectPtr<ACameraActor> MenuCamera;

    void Next(EStep Value) { Step = Value; Age = 0; }

    static bool Finish(bool bPassed, const TCHAR* Reason)
    {
        UE_LOG(LogSaveCheckBuild, Display, TEXT("pass=%d %s"), bPassed, Reason);
        FPlatformMisc::RequestExit(false);
        return false;
    }

    static int32 CountProjections(UWorld& World)
    {
        UClass* Type = FindObject<UClass>(nullptr, TEXT("/Script/ShadowArchitect.ShadowProjection"));
        if (!Type) return INDEX_NONE;
        int32 Count = 0;
        for (TActorIterator<AActor> It(&World, Type); It; ++It) ++Count;
        return Count;
    }

    const TCHAR* CaseName() const
    {
        switch (Scenario)
        {
        case ECase::Preview: return TEXT("preview_cancel");
        case ECase::Shortage: return TEXT("shortage");
        case ECase::Success: return TEXT("success_repeat_hold");
        case ECase::Occupied: return TEXT("occupied");
        case ECase::SourceChanged: return TEXT("source_changed_before_confirm");
        }
        return TEXT("unknown");
    }

    bool PrepareAim(UWorld& World, AFGCharacterPlayer& Character)
    {
        const FPlanPose First = ReadPlan(World, 0);
        if (!First.Id.IsValid()) return false;
        const FVector Anchor = First.Anchor.GetLocation();
        const FCollisionQueryParams Query(SCENE_QUERY_STAT(ShadowBuildProbe), false, &Character);
        for (const FVector Offset : { FVector(-3200, 2400, 0), FVector(3200, 2400, 0), FVector(0, 2800, 0), FVector(-1200, 1600, 0) })
        {
            FHitResult Target;
            FHitResult Foot;
            const FVector Point = Anchor + Offset;
            const FVector Standing = Point - FVector(500, 0, 0);
            if (!World.LineTraceSingleByChannel(Target, Point + FVector(0, 0, 1800), Point - FVector(0, 0, 1100), ECC_GameTraceChannel5, Query)
                || !World.LineTraceSingleByChannel(Foot, Standing + FVector(0, 0, 1800), Standing - FVector(0, 0, 1100), ECC_Visibility, Query)
                || Target.ImpactNormal.Z < 0.9 || Foot.ImpactNormal.Z < 0.9
                || FMath::Abs(Target.ImpactPoint.Z - Anchor.Z) > 10
                || FMath::Abs(Foot.ImpactPoint.Z - Target.ImpactPoint.Z) > 10) continue;
            const double Height = Character.GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 12;
            if (!Character.TeleportTo(Foot.ImpactPoint + FVector(0, 0, Height), FRotator::ZeroRotator)) continue;
            Aim = Target.ImpactPoint;
            UE_LOG(LogSaveCheckBuild, Display, TEXT("fixture_anchor=%s fixture_pawn=%s aim_actor=%s"),
                *Anchor.ToString(), *Character.GetActorLocation().ToString(), *GetNameSafe(Target.GetActor()));
            return true;
        }
        return false;
    }

    static int32 DepotStock(UFGInventoryComponent& Inventory, TSubclassOf<UFGItemDescriptor> Item)
    {
        const auto* Central = AFGCentralStorageSubsystem::Get(Inventory.GetWorld());
        return Central ? Central->GetNumItemsFromCentralStorage(Item) : 0;
    }

    bool ReadCost(AFGBlueprintHologram& Preview, AFGCharacterPlayer& Character, UFGInventoryComponent& Inventory)
    {
        auto* SaveState = Preview.GetWorld()->GetGameState<AFGGameState>();
        if (SaveState && SaveState->GetCheatNoCost()) SaveState->SetCheatNoCost(false);
        auto* PlayerState = Character.GetPlayerState<AFGPlayerState>();
        if (PlayerState) PlayerState->SetTakeFromInventoryBeforeCentralStorage(true);
        if (PlayerState && PlayerState->GetPlayerRules().NoBuildCost)
        {
            UFunction* SetNoBuildCost = PlayerState->FindFunction(TEXT("Server_SetNoBuildCost"));
            bool bNoBuildCost = false;
            if (SetNoBuildCost) PlayerState->ProcessEvent(SetNoBuildCost, &bNoBuildCost);
        }
        if (Inventory.GetNoBuildCost())
        {
            UE_LOG(LogSaveCheckBuild, Error, TEXT("fixture build costs disabled"));
            return false;
        }
        for (const FItemAmount& Item : Preview.GetCost(true))
            if (Item.ItemClass && Item.Amount > 0) Cost.FindOrAdd(Item.ItemClass) += Item.Amount;
        if (Cost.IsEmpty())
        {
            UE_LOG(LogSaveCheckBuild, Error, TEXT("fixture hologram cost empty"));
            return false;
        }
        Missing = Cost.CreateConstIterator().Key();
        for (const auto& Item : Cost)
            if (DepotStock(Inventory, Item.Key) == 0) { Missing = Item.Key; break; }
        for (const auto& Item : Cost)
        {
            const int32 Extra = Item.Value * 2 - Inventory.GetNumItems(Item.Key);
            if (Extra > 0 && Inventory.AddStack(FInventoryStack(Extra, Item.Key)) != Extra)
            {
                UE_LOG(LogSaveCheckBuild, Error, TEXT("fixture cannot add %d %s"), Extra, *GetNameSafe(Item.Key.Get()));
                return false;
            }
        }
        return true;
    }

    bool ClearMissingStock(UFGInventoryComponent& Inventory) const
    {
        Inventory.Remove(Missing, Inventory.GetNumItems(Missing));
        if (auto* Central = AFGCentralStorageSubsystem::Get(Inventory.GetWorld()))
            Central->TryRemoveItemsFromCentralStorage(Missing, Central->GetNumItemsFromCentralStorage(Missing));
        return Inventory.GetNumItems(Missing) == 0 && DepotStock(Inventory, Missing) == 0;
    }

    bool NextFreeSite(AFGCharacterPlayer& Character, AFGBlueprintHologram& Preview, double BuildRange)
    {
        FVector Away = FVector::ZeroVector;
        for (const auto& Pair : Preview.mBuildableToNewRoot)
            Away += Preview.GetActorLocation() - Pair.Value->GetComponentLocation();
        Away = Away.GetSafeNormal2D();
        if (Away.IsNearlyZero()) Away = FVector::ForwardVector;
        UWorld& World = *Character.GetWorld();
        const FCollisionQueryParams Query(SCENE_QUERY_STAT(ShadowBuildFixture), false, &Character);
        constexpr int32 Directions = 12;
        while (SiteAttempt < Directions * 4)
        {
            const double Angle = SiteAttempt % Directions * UE_TWO_PI / Directions;
            const int32 Radius = 2200 << (SiteAttempt / Directions);
            ++SiteAttempt;
            const FVector Point = SiteOrigin.GetLocation() + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0) * Radius;
            const FVector Standing = Point + Away * FMath::Min(1800.0, BuildRange * 0.75);
            FHitResult Floor;
            FHitResult Foot;
            if (!World.LineTraceSingleByChannel(Floor, Point + FVector(0, 0, 6000), Point - FVector(0, 0, 6000), ECC_GameTraceChannel5, Query)
                || !World.LineTraceSingleByChannel(Foot, Standing + FVector(0, 0, 6000), Standing - FVector(0, 0, 6000), ECC_Visibility, Query)
                || Floor.ImpactNormal.Z < 0.9 || Foot.ImpactNormal.Z < 0.9
                || FMath::Abs(Foot.ImpactPoint.Z - Floor.ImpactPoint.Z) > 250) continue;
            const double Height = Character.GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 10;
            if (!Character.TeleportTo(Foot.ImpactPoint + FVector(0, 0, Height), FRotator::ZeroRotator)) continue;
            Aim = Floor.ImpactPoint;
            Preview.LockHologramPosition(false);
            Preview.SetActorLocationAndRotation(Aim + FVector(0, 0, 10), SiteOrigin.GetRotation());
            Preview.LockHologramPosition(true);
            UE_LOG(LogSaveCheckBuild, Display, TEXT("fixture site_attempt=%d anchor=%s pawn=%s"),
                SiteAttempt, *Preview.GetActorLocation().ToString(), *Character.GetActorLocation().ToString());
            return Preview.IsHologramLocked();
        }
        return false;
    }

    bool CheckPose(AFGBlueprintHologram& Preview) const
    {
        if (!Preview.GetActorTransform().Equals(Plan.Anchor, 0.1) || Preview.mBuildableToNewRoot.Num() != Plan.Parts.Num()) return false;
        TArray<FTransform> Remaining = Plan.Parts;
        for (const auto& Pair : Preview.mBuildableToNewRoot)
        {
            const FTransform Actual = Pair.Value->GetComponentTransform();
            const int32 Index = Remaining.IndexOfByPredicate([&](const FTransform& Part) { return Actual.Equals(Part * Plan.Anchor, 0.1); });
            if (Index == INDEX_NONE) return false;
            UE_LOG(LogSaveCheckBuild, Display, TEXT("pose id=%s part=%s expected=%s actual=%s"), *Plan.Id.ToString(),
                *GetNameSafe(Pair.Key->GetClass()), *(Remaining[Index] * Plan.Anchor).ToString(), *Actual.ToString());
            Remaining.RemoveAtSwap(Index);
        }
        UE_LOG(LogSaveCheckBuild, Display, TEXT("pose id=%s anchor=%s parts=%d"), *Plan.Id.ToString(), *Plan.Anchor.ToString(), Plan.Parts.Num());
        return true;
    }

    bool CheckValidation(AFGBlueprintHologram& Preview) const
    {
        TArray<TSubclassOf<UFGConstructDisqualifier>> Reasons;
        Preview.GetConstructDisqualifiers(Reasons);
        for (auto Reason : Reasons)
            UE_LOG(LogSaveCheckBuild, Display, TEXT("case=%s disqualifier=%s"), CaseName(), *GetNameSafe(Reason.Get()));
        const bool bCanBuild = Preview.CanConstruct();
        const bool bUnaffordable = Reasons.Contains(UFGCDUnaffordable::StaticClass());
        if (Scenario == ECase::Preview) return true;
        if (Scenario == ECase::Success || Scenario == ECase::SourceChanged) return bCanBuild;
        if (Scenario == ECase::Shortage) return !bCanBuild && bUnaffordable;
        return !bCanBuild && !bUnaffordable && !Reasons.Contains(UFGCDInvalidAimLocation::StaticClass());
    }

    bool CheckConstructedPositions(UWorld& World) const
    {
        TArray<FTransform> Remaining = Plan.Parts;
        auto Match = [&](const FTransform& Actual)
        {
            const int32 Index = Remaining.IndexOfByPredicate([&](const FTransform& Part)
            {
                return Actual.GetLocation().Equals((Part * Plan.Anchor).GetLocation(), 1.0);
            });
            if (Index == INDEX_NONE)
            {
                UE_LOG(LogSaveCheckBuild, Error, TEXT("menu_target=%s unexpected_building_at=%s"),
                    *Plan.Id.ToString(), *Actual.GetLocation().ToString());
                return false;
            }
            Remaining.RemoveAtSwap(Index);
            return true;
        };

        const auto* Lightweights = AFGLightweightBuildableSubsystem::Get(&World);
        for (TActorIterator<AFGBlueprintProxy> It(&World); It; ++It)
        {
            if (ExistingBlueprints.Contains(*It)) continue;
            for (const auto* Building : It->GetBuildables())
                if (!IsValid(Building) || !Match(Building->GetActorTransform())) return false;
            for (const auto& Entry : It->GetLightweightClassAndIndices())
            {
                const auto* Instances = Lightweights
                    ? Lightweights->GetAllLightweightBuildableInstances().Find(Entry.BuildableClass) : nullptr;
                for (int32 Index : Entry.Indices)
                    if (!Instances || !Instances->IsValidIndex(Index) || !Match((*Instances)[Index].Transform)) return false;
            }
        }
        UE_LOG(LogSaveCheckBuild, Display, TEXT("menu_target=%s matched_real_positions=%d/%d anchor_cm=%s"),
            *Plan.Id.ToString(), Plan.Parts.Num() - Remaining.Num(), Plan.Parts.Num(), *Plan.Anchor.GetLocation().ToString());
        return Remaining.IsEmpty();
    }

    bool CheckResult(UWorld& World, UFGInventoryComponent& Inventory) const
    {
        const bool bSuccess = Scenario == ECase::Success;
        const FBuildings After = ReadBuildings(World);
        const FPlanPose AfterPlan = ReadPlan(World);
        bool bGood = AfterPlan.Count == Plan.Count && AfterPlan.Id == Plan.Id
            && After.Count - Before.Count == (bSuccess ? Plan.Parts.Num() : 0)
            && After.Blueprints - Before.Blueprints == (bSuccess ? 1 : 0)
            && After.BlueprintParts - Before.BlueprintParts == (bSuccess ? Plan.Parts.Num() : 0);
        if (bViaMenu)
        {
            const FPlanPose Other = ReadPlan(World, 0);
            const bool bUnchanged = Other.Id == UnrelatedPlan.Id && Other.Anchor.Equals(UnrelatedPlan.Anchor, 0.01);
            UE_LOG(LogSaveCheckBuild, Display, TEXT("menu_target=%s other=%s other_unchanged=%d"),
                *Plan.Id.ToString(), *Other.Id.ToString(), bUnchanged);
            bGood &= bUnchanged && AfterPlan.Anchor.Equals(Plan.Anchor, 0.01);
            if (bSuccess) bGood &= CheckConstructedPositions(World);
        }
        UE_LOG(LogSaveCheckBuild, Display, TEXT("case=%s id=%s buildings=%d->%d proxies=%d->%d blueprint_parts=%d->%d plans=%d->%d"),
            CaseName(), *Plan.Id.ToString(), Before.Count, After.Count, Before.Blueprints, After.Blueprints,
            Before.BlueprintParts, After.BlueprintParts, Plan.Count, AfterPlan.Count);
        if (Scenario == ECase::Preview)
        {
            const auto AfterStock = StockOf(Inventory);
            const bool bStockUnchanged = Stock.OrderIndependentCompareEqual(AfterStock);
            const int32 AfterProjections = CountProjections(World);
            UE_LOG(LogSaveCheckBuild, Display, TEXT("case=%s projection_actors=%d->%d inventory_item_types=%d->%d inventory_unchanged=%d"),
                CaseName(), BeforeProjections, AfterProjections, Stock.Num(), AfterStock.Num(), bStockUnchanged);
            return bGood && bStockUnchanged && BeforeProjections == Plan.Count && AfterProjections == BeforeProjections;
        }
        for (const auto& Item : Cost)
        {
            const int32 Start = Stock.FindChecked(Item.Key);
            const int32 End = Inventory.GetNumItems(Item.Key);
            const int32 DepotBefore = CentralStock.FindChecked(Item.Key);
            const int32 DepotAfter = DepotStock(Inventory, Item.Key);
            UE_LOG(LogSaveCheckBuild, Display, TEXT("case=%s item=%s before=%d after=%d debit=%d price=%d depot=%d->%d"),
                CaseName(), *GetNameSafe(Item.Key.Get()), Start, End, Start - End, Item.Value, DepotBefore, DepotAfter);
            bGood &= Start - End == (bSuccess ? Item.Value : 0) && DepotAfter >= DepotBefore;
        }
        return bGood;
    }

    bool TickWorld(UWorld& World, APlayerController& Player, AFGCharacterPlayer& Character)
    {
        if (SourceProbe) return SourceProbe->Tick(World, Player, Character);
        if (++Age > 72)
        {
            UE_LOG(LogSaveCheckBuild, Error, TEXT("timeout step=%d case=%s"), static_cast<int32>(Step), CaseName());
            return Finish(false, TEXT("step timeout"));
        }
        auto* Gun = Character.GetBuildGun();
        auto* Build = Gun && Gun->IsEquipped() ? Cast<UFGBuildGunStateBuild>(Gun->GetCurrentState()) : nullptr;
        auto* Preview = Build ? Cast<AFGBlueprintHologram>(Build->GetHologram()) : nullptr;
        UFGInventoryComponent& Inventory = *Character.GetInventory();
        switch (Step)
        {
        case EStep::Ready:
        {
            if (Age < 12) break;
            const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("ShadowArchitect"));
            Plan = ReadPlan(World);
            UnrelatedPlan = ReadPlan(World, 0);
            if (bViaMenu && Plan.Count == 2)
            {
                Next(EStep::PrepareMenuFixture);
                break;
            }
            TArray<UFGBlueprintDescriptor*> Blueprints;
            AFGBlueprintSubsystem::GetBlueprintDescriptors(Blueprints, &World);
            if (!Plugin || !Plugin->IsEnabled() || !Plan.Id.IsValid() || Blueprints.Num() != 1
                || !Blueprints[0] || !PrepareAim(World, Character)) return Finish(false, TEXT("world fixture"));
            Source = Blueprints[0]->GetBlueprintNameAsString();
            UE_LOG(LogSaveCheckBuild, Display, TEXT("version=%s blueprint=%s aim=%s"),
                *Plugin->GetDescriptor().VersionName, *Source, *Aim.ToString());
            Player.SetViewTarget(&Character);
            Player.SetControlRotation((Aim - Character.GetPawnViewLocation()).Rotation());
            Character.HotKeyBlueprint(Source);
            Next(EStep::Native);
            break;
        }
        case EStep::PrepareMenuFixture:
            if (Age <= 6)
            {
                // Remove the second fixture plan before capturing the menu's build target.
                PlanChord(Player, Age <= 4 ? EKeys::J : EKeys::K, Age % 2 ? IE_Pressed : IE_Released);
            }
            else
            {
                const FPlanPose Remaining = ReadPlan(World);
                if (Remaining.Count != 1 || Remaining.Id != UnrelatedPlan.Id
                    || !Remaining.Anchor.Equals(UnrelatedPlan.Anchor, 0.01))
                    return Finish(false, TEXT("expected only fixture plan A before capture"));
                Next(EStep::Ready);
            }
            break;
        case EStep::Native:
        {
            Player.SetControlRotation((Aim - Character.GetPawnViewLocation()).Rotation());
            if (Age == 1 || Age == 12 || Age == 50)
            {
                FVector Eye;
                FRotator Rotation;
                Player.GetPlayerViewPoint(Eye, Rotation);
                const FHitResult* Hit = Build ? &Gun->GetHitResult() : nullptr;
                UE_LOG(LogSaveCheckBuild, Display, TEXT("native_age=%d preview=%s hidden=%d disabled=%d roots=%d eye=%s rotation=%s hit=%s valid_hit=%d"),
                    Age, *GetNameSafe(Preview), Preview && Preview->IsHidden(), Preview && Preview->IsDisabled(),
                    Preview ? Preview->mBuildableToNewRoot.Num() : 0, *Eye.ToString(), *Rotation.ToString(),
                    *GetNameSafe(Hit ? Hit->GetActor() : nullptr), Preview && Hit && Preview->IsValidHitResult(*Hit));
            }
            // For a source-only seed the native loader and F7 capture are the conditions under test.
            if (!SeedSlot.IsEmpty() && Age >= 12 && Preview && Preview->IsHidden() && !Preview->IsDisabled()
                && Preview->mBuildableToNewRoot.Num() >= 2)
            {
                Preview->SetActorLocation(Aim);
                Preview->LockHologramPosition(true);
                if (Preview->IsHologramLocked()) Preview->SetActorHiddenInGame(false);
            }
            if (!Preview || Preview->IsHidden() || Preview->IsDisabled() || Preview->mBuildableToNewRoot.Num() < 2) break;
            if (Scenario != ECase::Preview)
            {
                if (Cost.IsEmpty())
                {
                    if (!ReadCost(*Preview, Character, Inventory)) return Finish(false, TEXT("price fixture"));
                    SiteOrigin = Preview->GetActorTransform();
                    UE_LOG(LogSaveCheckBuild, Display, TEXT("fixture cost_types=%d gun_inventory_matches=%d gun_range_cm=%.0f"),
                        Cost.Num(), Gun->GetInventory() == &Inventory, Gun->GetBuildGunRange());
                }
                Preview->ValidatePlacementAndCost(Gun->GetInventory());
                if (SiteAttempt == 0 || !Preview->CanConstruct())
                {
                    TArray<TSubclassOf<UFGConstructDisqualifier>> Reasons;
                    Preview->GetConstructDisqualifiers(Reasons);
                    FString Names;
                    for (const auto Reason : Reasons) Names += TEXT(" ") + GetNameSafe(Reason.Get());
                    UE_LOG(LogSaveCheckBuild, Display, TEXT("fixture site_attempt=%d rejected=%s"), SiteAttempt, *Names);
                    if (!NextFreeSite(Character, *Preview, Gun->GetBuildGunRange()))
                        return Finish(false, TEXT("could not find a clear placement"));
                    break;
                }
                UE_LOG(LogSaveCheckBuild, Display, TEXT("fixture free_site=%s"), *Preview->GetActorTransform().ToString());
                if (Scenario == ECase::Shortage && !ClearMissingStock(Inventory)) return Finish(false, TEXT("shortage fixture"));
            }
            if (PreviewPass == 1)
            {
                const FQuat Turn(FVector::UpVector, UE_PI / 2.0);
                Preview->SetActorLocationAndRotation(Plan.Anchor.GetLocation() + FVector(0, 0, 250), Turn * Plan.Anchor.GetRotation());
                Preview->LockHologramPosition(true);
            }
            Key(Player, EKeys::F7, IE_Pressed);
            Next(EStep::Capture);
            break;
        }
        case EStep::Capture:
            if (Age == 1) Key(Player, EKeys::F7, IE_Released);
            if (Age < 3) break;
            {
                const FPlanPose Captured = ReadPlan(World);
                if (Captured.Count != Plan.Count + 1 || !Captured.Id.IsValid() || Captured.Parts.Num() < 2)
                    return Finish(false, TEXT("F7 capture"));
                if (PreviewPass == 1 && (Captured.Anchor.GetLocation().Z - Plan.Anchor.GetLocation().Z < 200
                    || FMath::Abs(FMath::RadiansToDegrees(Captured.Anchor.GetRotation().AngularDistance(Plan.Anchor.GetRotation())) - 90) > 0.1))
                    return Finish(false, TEXT("second pose fixture"));
                Plan = Captured;
            }
            Character.UnequipBuildGun();
            if (!SeedSlot.IsEmpty())
            {
                FString Existing;
                auto* Save = UFGSaveSession::Get(&World);
                if (!SeedSlot.StartsWith(TEXT("SAQ_")) || Plan.SourceDigest.IsEmpty()
                    || !Save || UFGSaveSystem::GetAbsolutePathForSaveGame(&World, SeedSlot, Existing))
                    return Finish(false, TEXT("source seed requires a fresh SAQ_ slot and a captured digest"));
                UE_LOG(LogSaveCheckBuild, Display, TEXT("seed=%s id=%s digest=%s"), *SeedSlot, *Plan.Id.ToString(), *Plan.SourceDigest);
                Save->SaveGame(SeedSlot, FOnSaveGameComplete::CreateLambda([](bool bSuccess, const FText&, void*)
                {
                    Finish(bSuccess, TEXT("write source seed"));
                }), nullptr);
                return false;
            }
            Next(EStep::Open);
            break;
        case EStep::Open:
            if (Age == 1)
            {
                Before = ReadBuildings(World);
                BeforeProjections = CountProjections(World);
                if (Scenario == ECase::Preview)
                {
                    Stock = StockOf(Inventory);
                }
                else
                    for (const auto& Item : Cost)
                    {
                        Stock.Add(Item.Key, Inventory.GetNumItems(Item.Key));
                        CentralStock.Add(Item.Key, DepotStock(Inventory, Item.Key));
                    }
                if (bViaMenu)
                {
                    ExistingBlueprints.Reset();
                    for (TActorIterator<AFGBlueprintProxy> It(&World); It; ++It) ExistingBlueprints.Add(*It);
                    const FVector Eye = Plan.AimPoint + FVector(350, -500, 200);
                    const FRotator Look = (Plan.AimPoint - Eye).Rotation();
                    if (!MenuCamera.IsValid()) MenuCamera = World.SpawnActor<ACameraActor>(Eye, Look);
                    if (!MenuCamera.IsValid()) return Finish(false, TEXT("menu camera fixture"));
                    MenuCamera->SetActorLocationAndRotation(Eye, Look);
                    Player.SetViewTarget(MenuCamera.Get());
                }
                else
                    PlanChord(Player, EKeys::F8, IE_Pressed);
            }
            if (bViaMenu)
            {
                if (Age == 2) Key(Player, EKeys::F6, IE_Pressed);
                if (Age == 3) Key(Player, EKeys::F6, IE_Released);
                if (Age > 3 && ClickPlanMenuButton(TEXT("Open build preview")))
                {
                    Player.SetViewTarget(&Character);
                    Next(EStep::Aligned);
                }
            }
            else if (Age == 2)
            {
                PlanChord(Player, EKeys::F8, IE_Released);
                Next(EStep::Aligned);
            }
            break;
        case EStep::Aligned:
            if (Age < 5 || !Preview || Preview->IsHidden() || Preview->IsDisabled()) break;
            if (!CheckPose(*Preview)) return Finish(false, TEXT("aligned preview"));
            if (!CheckValidation(*Preview)) return Finish(false, TEXT("stock validation"));
            if (Scenario == ECase::Preview)
            {
                Key(Player, EKeys::Escape, IE_Pressed);
                Next(EStep::Result);
            }
            else
            {
                if (Scenario == ECase::SourceChanged)
                {
                    if (Source != TEXT("QA_A") || !FSourceProbe::ReplaceFixture(World))
                        return Finish(false, TEXT("replacement fixture"));
                    UE_LOG(LogSaveCheckBuild, Display, TEXT("source_changed_before_confirm id=%s input=LMB"), *Plan.Id.ToString());
                }
                Key(Player, EKeys::LeftMouseButton, IE_Pressed);
                Next(EStep::Confirm);
            }
            break;
        case EStep::Confirm:
            if (Scenario == ECase::Success && Age <= 4) Key(Player, EKeys::LeftMouseButton, IE_Repeat);
            if (Age == 5) Key(Player, EKeys::LeftMouseButton, IE_Released);
            if (Scenario == ECase::Success && Age == 6) Key(Player, EKeys::LeftMouseButton, IE_Pressed);
            if (Age == 7)
            {
                if (Scenario == ECase::Success) Key(Player, EKeys::LeftMouseButton, IE_Released);
                Next(EStep::Result);
            }
            break;
        case EStep::Result:
            if (Scenario == ECase::Preview && Age == 1) Key(Player, EKeys::Escape, IE_Released);
            if (Age < 12) break;
            if (!CheckResult(World, Inventory)) return Finish(false, TEXT("construction outcome"));
            if (Scenario == ECase::SourceChanged)
                return Finish(!Build && CountProjections(World) == BeforeProjections, TEXT("replacement before confirmation"));
            if (Scenario == ECase::Preview)
            {
                if (Build) return Finish(false, TEXT("Escape cancellation"));
                if (bViaMenu) return Finish(true, TEXT("menu preview cancelled"));
                if (++PreviewPass == 2) return Finish(true, TEXT("two previews opened and cancelled"));
                Character.HotKeyBlueprint(Source);
                Next(EStep::Native);
                break;
            }
            if (Scenario == ECase::Shortage)
            {
                const int32 Refill = Cost.FindChecked(Missing) * 2;
                if (Inventory.AddStack(FInventoryStack(Refill, Missing)) != Refill)
                    return Finish(false, TEXT("replenishment"));
                Scenario = ECase::Success;
            }
            else if (Scenario == ECase::Success)
            {
                if (Build) return Finish(false, TEXT("successful plan preview still open"));
                if (bViaMenu) return Finish(true, TEXT("menu construction"));
                Scenario = ECase::Occupied;
            }
            else
            {
                Character.UnequipBuildGun();
                PickupStock = Inventory.GetNumItems(Missing);
                const FVector Front = Character.GetActorLocation() - FVector(140, 0, 0);
                Pickup = AFGItemPickup_Spawnable::CreateItemDrop(&World, FInventoryStack(1, Missing), Front, FRotator::ZeroRotator);
                if (!Pickup.IsValid()) return Finish(false, TEXT("pickup fixture"));
                Next(EStep::Use);
                break;
            }
            if (Build) Character.UnequipBuildGun();
            Next(EStep::Open);
            break;
        case EStep::Use:
            if (Age <= 10 && Pickup.IsValid())
                Player.SetControlRotation((Pickup->GetActorLocation() - Character.GetPawnViewLocation()).Rotation());
            if (Age == 11) Key(Player, EKeys::E, IE_Pressed);
            if (Age == 12) Key(Player, EKeys::E, IE_Released);
            if (Age == 20)
            {
                const int32 Added = Inventory.GetNumItems(Missing) - PickupStock;
                UE_LOG(LogSaveCheckBuild, Display, TEXT("input=E ordinary_pickup_added=%d"), Added);
                return Finish(Added == 1, TEXT("ordinary interaction"));
            }
            break;
        }
        return true;
    }

    bool Tick()
    {
        if (++Frames > 900) return Finish(false, TEXT("world timeout"));
        if (!GEngine) return true;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            UWorld* World = Context.World();
            if (!World || !World->IsGameWorld() || !World->HasBegunPlay() || !World->GetMapName().Contains(TEXT("Persistent_Level"))) continue;
            auto* Save = UFGSaveSession::Get(World);
            if (!Save) continue;
            Save->SetAutoSaveEnabled(false);
            auto* Player = World->GetFirstPlayerController();
            auto* Character = Player ? Cast<AFGCharacterPlayer>(Player->GetPawn()) : nullptr;
            if (Character && Character->GetInventory() && Player->PlayerInput) return TickWorld(*World, *Player, *Character);
        }
        return true;
    }
};
}

FTickerDelegate MakeBuildCheck(FString Mode)
{
    FBuildProbe Probe;
    Probe.bViaMenu = Mode == TEXT("menu-confirm") || Mode == TEXT("menu-preview");
    if (Mode == TEXT("confirm-replaced")) Probe.Scenario = FBuildProbe::ECase::SourceChanged;
    else if (Mode != TEXT("confirm") && Mode != TEXT("menu-confirm")) Probe.Scenario = FBuildProbe::ECase::Preview;
    if (Mode.RemoveFromStart(TEXT("seed:")))
        Probe.SeedSlot = MoveTemp(Mode);
    else if (Mode.RemoveFromStart(TEXT("source:")))
        Probe.SourceProbe.Emplace().ExpectedSource = MoveTemp(Mode);
    return FTickerDelegate::CreateLambda([Probe = MoveTemp(Probe)](float) mutable { return Probe.Tick(); });
}
