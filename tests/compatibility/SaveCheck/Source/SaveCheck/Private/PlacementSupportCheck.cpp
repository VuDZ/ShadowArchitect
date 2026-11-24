#include "Buildables/FGBuildable.h"
#include "Components/MeshComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Equipment/FGBuildGunBuild.h"
#include "FGCharacterPlayer.h"
#include "FGInputLibrary.h"
#include "FGInventoryComponent.h"
#include "FGRecipe.h"
#include "FGSaveSession.h"
#include "FGSaveSystem.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Hologram/FGBlueprintHologram.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "RoofStackFixture.h"
#include "UnrealClient.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogSaveCheckPlacementSupport, Log, All);

namespace ShadowPlacementProbe
{
constexpr TCHAR LowerBlueprint[] = TEXT("3х3 пустой");
constexpr TCHAR UpperBlueprint[] = TEXT("3 литейных");
constexpr TCHAR ConcreteClass[] = TEXT("/Game/FactoryGame/Buildable/Building/Foundation/ConcreteSet/"
    "Build_Foundation_Concrete_8x1.Build_Foundation_Concrete_8x1_C");
const FVector EyeOffset(920, -730, 2250);

struct FPlan
{
    int32 Count = 0;
    FGuid Id;
    FTransform Pose;
};

bool ReadPlan(UWorld& World, FPlan& Out)
{
    TArray<FRoofStackPlan> Plans;
    if (!ReadRoofStackPlans(World, Plans)) return false;
    Out.Count = Plans.Num();
    if (!Plans.IsEmpty())
    {
        Out.Id = Plans.Last().Id;
        Out.Pose = Plans.Last().Anchor;
    }
    return true;
}

TMap<TSubclassOf<UFGItemDescriptor>, int32> StockOf(UFGInventoryComponent& Inventory)
{
    TArray<FInventoryStack> Stacks;
    Inventory.GetInventoryStacks(Stacks);
    TMap<TSubclassOf<UFGItemDescriptor>, int32> Stock;
    for (const FInventoryStack& Stack : Stacks)
        if (Stack.HasItems()) Stock.FindOrAdd(Stack.Item.GetItemClass()) += Stack.NumItems;
    return Stock;
}

AFGBuildable* SpawnFloor(UWorld& World, const FVector& Top)
{
    auto* Class = LoadClass<AFGBuildable>(nullptr,
        TEXT("/Game/FactoryGame/Buildable/Building/Foundation/Build_Foundation_8x1_01.Build_Foundation_8x1_01_C"));
    auto* Recipe = LoadClass<UFGRecipe>(nullptr,
        TEXT("/Game/FactoryGame/Recipes/Buildings/Foundations/Recipe_Foundation_8x1_01.Recipe_Foundation_8x1_01_C"));
    if (!Class || !Recipe) return nullptr;
    const FTransform Pose(FRotator::ZeroRotator, Top - FVector(0, 0, 50));
    auto* Floor = World.SpawnActorDeferred<AFGBuildable>(Class, Pose, nullptr, nullptr,
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
    if (!Floor) return nullptr;
    Floor->SetBuiltWithRecipe(Recipe);
    if (auto* Managed = FindFProperty<FBoolProperty>(AFGBuildable::StaticClass(), TEXT("mManagedByLightweightBuildableSubsystem")))
        Managed->SetPropertyValue_InContainer(Floor, false);
    Floor->FinishSpawning(Pose);
    return Floor;
}

double VisibleBottom(const AFGBlueprintHologram& Preview)
{
    FBox Bounds(ForceInit);
    for (const auto& Pair : Preview.mBuildableToNewRoot)
    {
        if (!IsValid(Pair.Value.Get())) continue;
        TArray<USceneComponent*> Components;
        Pair.Value->GetChildrenComponents(true, Components);
        Components.Add(Pair.Value.Get());
        for (const auto* Component : Components)
            if (const auto* Mesh = Cast<UMeshComponent>(Component); Mesh && Mesh->IsVisible() && Mesh->IsRegistered())
                Bounds += Mesh->CalcBounds(Mesh->GetComponentTransform()).GetBox();
    }
    return Bounds.IsValid ? Bounds.Min.Z : TNumericLimits<double>::Max();
}

struct FCheck
{
    FString Directory;
    FString OutputSlot;
    bool bReload;
    int32 Stage = 0;
    int32 Age = 0;
    int32 Initial = 0;
    int32 Buildings = 0;
    TMap<TSubclassOf<UFGItemDescriptor>, int32> Stock;
    FTransform LowerPose;
    FTransform Shown;
    FGuid LowerId;
    TWeakObjectPtr<AFGBuildable> Blocker;
    FVector Left = FVector::ZeroVector;
    FVector Right = FVector::ZeroVector;
    FVector Target = FVector::ZeroVector;
    double FloorZ = 0;
    double PreviousYaw = 0;

    bool End(bool bPass, const TCHAR* Message) const
    {
        UE_LOG(LogSaveCheckPlacementSupport, Display, TEXT("pass=%d stage=%d %s"), bPass, Stage, Message);
        FPlatformMisc::RequestExit(false);
        return false;
    }

    void Aim(APlayerController& Player, AFGCharacterPlayer& Character, const FVector& At)
    {
        Target = At;
        Character.GetCharacterMovement()->StopMovementImmediately();
        Character.TeleportTo(At + EyeOffset - FVector(0, 0, Character.BaseEyeHeight), FRotator::ZeroRotator, false, true);
        Player.SetViewTarget(&Character);
        Player.SetControlRotation((At - Character.GetPawnViewLocation()).Rotation());
    }

    bool Measure(AFGBuildGun& Gun, AFGBlueprintHologram* Preview, const TCHAR* Label, double Z, bool bPlan) const
    {
        const auto& Hit = Gun.GetHitResult();
        const bool bVisible = Preview && !Preview->IsHidden();
        const double Bottom = bVisible ? VisibleBottom(*Preview) : 0;
        const bool bHitPlan = Hit.GetActor() && Hit.GetActor()->GetClass()->GetFName() == TEXT("ShadowProjection");
        FVector Eye;
        FRotator View;
        Gun.GetWorld()->GetFirstPlayerController()->GetPlayerViewPoint(Eye, View);
        FHitResult WorldHit;
        FCollisionQueryParams Query(SCENE_QUERY_STAT(PlacementSupportFixture), true, Gun.GetInstigator());
        Gun.GetWorld()->LineTraceSingleByChannel(WorldHit, Eye, Eye + View.Vector() * Gun.GetBuildGunRange(), ECC_Visibility, Query);
        UE_LOG(LogSaveCheckPlacementSupport, Display,
            TEXT("case=%s target=%s expected_z=%.2f hit=%s visible=%d preview_bottom=%.2f aim=%s eye=%s view=%s world_hit=%s pose=%s"), Label,
            *GetNameSafe(Hit.GetActor()), Z, *Hit.ImpactPoint.ToCompactString(), bVisible, Bottom, *Target.ToCompactString(),
            *Eye.ToCompactString(), *View.ToCompactString(), *GetNameSafe(WorldHit.GetActor()),
            Preview ? *Preview->GetActorTransform().ToString() : TEXT("none"));
        FScreenshotRequest::RequestScreenshot(FPaths::Combine(Directory, FString(Label) + TEXT(".png")), true, false);
        return Hit.bBlockingHit && bHitPlan == bPlan && FMath::Abs(Hit.ImpactPoint.Z - Z) <= 1
            && Preview && !Preview->IsHidden() && (!bPlan || (FMath::Abs(Bottom - Z) <= 1
            && FVector::Dist2D(Preview->GetActorLocation(), Target) <= 1));
    }

    bool Tick(UWorld& World, APlayerController& Player, AFGCharacterPlayer& Character, UFGSaveSession& Save)
    {
        if (++Age > 90) return End(false, TEXT("timeout"));
        FPlan Plan;
        if ((bReload && Age < 10) || (Stage == 0 && Age < 46)) return true;
        if (!ReadPlan(World, Plan)) return true;
        if (bReload)
        {
            UE_LOG(LogSaveCheckPlacementSupport, Display, TEXT("reload id=%s pose=%s count=%d"),
                *Plan.Id.ToString(), *Plan.Pose.ToString(), Plan.Count);
            return End(Plan.Id.IsValid(), TEXT("reloaded last saved plan"));
        }
        auto* Gun = Character.GetBuildGun();
        if (!Gun || !Character.GetInventory()) return true;
        if (Stage > 0) Player.SetControlRotation((Target - Character.GetPawnViewLocation()).Rotation());
        auto* Preview = RoofStackPreview(*Gun);
        switch (Stage)
        {
        case 0:
        {
            if (!IFileManager::Get().MakeDirectory(*Directory, true)) return End(false, TEXT("frames directory"));
            Initial = Plan.Count;
            LowerPose = FTransform(FRotator::ZeroRotator, Character.GetActorLocation() + FVector(0, 0, 6200));
            Character.GetCharacterMovement()->SetMovementMode(MOVE_Flying);
            Aim(Player, Character, Character.GetActorLocation() - FVector(0, 0, 100));
            Character.HotKeyBlueprint(LowerBlueprint);
            const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("ShadowArchitect"));
            UE_LOG(LogSaveCheckPlacementSupport, Display, TEXT("version=%s initial_plans=%d"),
                Plugin ? *Plugin->GetDescriptor().VersionName : TEXT("missing"), Initial);
            break;
        }
        case 1:
        {
            if (!Preview || Preview->mBuildableToNewRoot.IsEmpty()) return true;
            Preview->SetActorTransform(LowerPose);
            Preview->LockHologramPosition(true);
            Preview->SetActorHiddenInGame(false);
            TArray<FVector> Tops;
            for (const auto& Pair : Preview->mBuildableToNewRoot)
                if (IsValid(Pair.Key.Get()) && IsValid(Pair.Value.Get()) && Pair.Key->GetClass()->GetPathName() == ConcreteClass)
                    Tops.Add(Pair.Value->GetComponentLocation() + FVector(0, 0, 50));
            if (Tops.IsEmpty()) return End(false, TEXT("missing fixture slabs"));
            Tops.Sort([](const FVector& A, const FVector& B) { return A.Z > B.Z; });
            const double Top = Tops[0].Z;
            Tops.RemoveAll([Top](const FVector& Point) { return Point.Z < Top - 1; });
            Tops.Sort([](const FVector& A, const FVector& B) { return A.X < B.X; });
            if (Tops.Num() < 2) return End(false, TEXT("missing level fixture slabs"));
            Left = Tops[0] + FVector(-145, -140, 0);
            Right = Tops.Last() + FVector(145, 140, 0);
            FloorZ = LowerPose.GetLocation().Z - 200;
            const FVector LeftFloor = Left - (Left.Z - FloorZ) / EyeOffset.Z * EyeOffset;
            const FVector RightFloor = Right - (Right.Z - FloorZ) / EyeOffset.Z * EyeOffset;
            if (!SpawnFloor(World, LeftFloor) || !SpawnFloor(World, RightFloor))
                return End(false, TEXT("cannot spawn fixture floors"));
            RoofStackKey(Player, EKeys::F7, IE_Pressed);
            break;
        }
        case 2:
            if (Age == 1) RoofStackKey(Player, EKeys::F7, IE_Released);
            if (Age < 6) return true;
            if (Plan.Count != Initial + 1 || !Plan.Pose.Equals(LowerPose, 0.01)) return End(false, TEXT("lower fixture placement"));
            LowerId = Plan.Id;
            Buildings = RoofStackBuildingCount(World);
            Stock = StockOf(*Character.GetInventory());
            Character.UnequipBuildGun();
            Aim(Player, Character, Left);
            Character.HotKeyBlueprint(UpperBlueprint);
            break;
        case 3:
            if (Age < 8 || !Preview || Preview->mBuildableToNewRoot.IsEmpty()) return true;
            if (!Measure(*Gun, Preview, TEXT("h01-world-default"), FloorZ, false)) return End(false, TEXT("default floor"));
            RoofStackKey(Player, EKeys::LeftControl, IE_Pressed);
            RoofStackKey(Player, EKeys::LeftShift, IE_Pressed);
            break;
        case 4:
            RoofStackKey(Player, EKeys::T, IE_Pressed);
            break;
        case 5:
            if (Age == 1)
            {
                RoofStackKey(Player, EKeys::T, IE_Released);
                RoofStackKey(Player, EKeys::LeftShift, IE_Released);
                RoofStackKey(Player, EKeys::LeftControl, IE_Released);
            }
            if (Age < 5) return true;
            if (!Measure(*Gun, Preview, TEXT("h01-left-roof"), Left.Z, true)) return End(false, TEXT("left roof"));
            UFGInputLibrary::RebindEnhancedKeyMapping(&Player, TEXT("ShadowArchitect.PlacementSupport"), EKeys::F11, {});
            break;
        case 6:
            if (Age < 3) return true;
            RoofStackKey(Player, EKeys::F11, IE_Pressed);
            break;
        case 7:
            if (Age == 1) RoofStackKey(Player, EKeys::F11, IE_Released);
            if (Age < 5) return true;
            if (Age == 5)
                return Measure(*Gun, Preview, TEXT("h01-world-rebound"), FloorZ, false) || End(false, TEXT("rebound toggle off"));
            RoofStackKey(Player, EKeys::F11, IE_Pressed);
            Aim(Player, Character, Right);
            break;
        case 8:
            if (Age == 1) RoofStackKey(Player, EKeys::F11, IE_Released);
            if (Age < 5) return true;
            if (Age == 5)
                return Measure(*Gun, Preview, TEXT("h01-right-roof"), Right.Z, true) || End(false, TEXT("rebound right roof"));
            RoofStackKey(Player, EKeys::LeftControl, IE_Pressed);
            RoofStackKey(Player, EKeys::LeftShift, IE_Pressed);
            break;
        case 9:
            RoofStackKey(Player, EKeys::V, IE_Pressed);
            break;
        case 10:
            if (Age == 1) RoofStackKey(Player, EKeys::V, IE_Released);
            if (Age < 5) return true;
            if (!Measure(*Gun, Preview, TEXT("h01-hidden"), FloorZ, false)) return End(false, TEXT("hidden plan affected aim"));
            RoofStackKey(Player, EKeys::V, IE_Pressed);
            break;
        case 11:
            if (Age == 1)
            {
                RoofStackKey(Player, EKeys::V, IE_Released);
                RoofStackKey(Player, EKeys::G, IE_Pressed);
            }
            if (Age == 2)
            {
                RoofStackKey(Player, EKeys::G, IE_Released);
                RoofStackKey(Player, EKeys::LeftShift, IE_Released);
                RoofStackKey(Player, EKeys::LeftControl, IE_Released);
            }
            if (Age < 4) return true;
            if (Age == 4)
                return Measure(*Gun, Preview, TEXT("h01-wireframe"), Right.Z, true) || End(false, TEXT("wireframe surface"));
            Aim(Player, Character, Right + FVector(320, 0, 0));
            break;
        case 12:
            if (Age < 4) return true;
            if (Age == 4)
                return Measure(*Gun, Preview, TEXT("h01-miss"), FloorZ, false) || End(false, TEXT("miss did not fall back to floor"));
            Aim(Player, Character, Right);
            Blocker = SpawnFloor(World, Right + FVector(0, 0, 400));
            if (!Blocker.IsValid()) return End(false, TEXT("cannot spawn blocker"));
            break;
        case 13:
            if (Age < 4) return true;
            if (Age == 4)
                return (Measure(*Gun, Preview, TEXT("h01-real-blocker"), Right.Z + 400, false)
                    && Gun->GetHitResult().GetActor() == Blocker.Get()) || End(false, TEXT("blocker lost priority"));
            Blocker->Destroy();
            Character.UnequipBuildGun();
            break;
        case 14:
            if (Age < 4) return true;
            if (Plan.Count != Initial + 1 || Preview) return End(false, TEXT("cancel added a plan or left preview open"));
            Character.HotKeyBlueprint(UpperBlueprint);
            break;
        case 15:
            if (Age < 5 || !Preview || Preview->mBuildableToNewRoot.IsEmpty()) return true;
            if (!Measure(*Gun, Preview, TEXT("h01-roof-after-cancel"), Right.Z, true)) return End(false, TEXT("no roof after cancel"));
            PreviousYaw = Preview->GetActorRotation().Yaw;
            Preview->Scroll(1);
            break;
        case 16:
            if (Age < 5) return true;
            if (Age == 5)
                return Measure(*Gun, Preview, TEXT("h01-right-roof-rotated"), Right.Z, true) || End(false, TEXT("rotated roof"));
            Shown = Preview->GetActorTransform();
            if (FMath::Abs(FMath::FindDeltaAngleDegrees(PreviousYaw, Shown.Rotator().Yaw)) < 1)
                return End(false, TEXT("scroll did not rotate preview"));
            if (Plan.Count != Initial + 1) return End(false, TEXT("aiming added plans"));
            RoofStackKey(Player, EKeys::F7, IE_Pressed);
            break;
        case 17:
        {
            if (Age == 1) RoofStackKey(Player, EKeys::F7, IE_Released);
            if (Age < 5) return true;
            const int32 RealAfter = RoofStackBuildingCount(World);
            const bool bStock = StockOf(*Character.GetInventory()).OrderIndependentCompareEqual(Stock);
            UE_LOG(LogSaveCheckPlacementSupport, Display,
                TEXT("lower=%s upper=%s input=F7 plans=%d->%d real=%d->%d inventory_same=%d shown=%s saved=%s"),
                *LowerId.ToString(), *Plan.Id.ToString(), Initial + 1, Plan.Count, Buildings, RealAfter, bStock,
                *Shown.ToString(), *Plan.Pose.ToString());
            if (Plan.Count != Initial + 2 || Plan.Id == LowerId || !Plan.Pose.GetLocation().Equals(Shown.GetLocation(), 1)
                || Plan.Pose.GetRotation().AngularDistance(Shown.GetRotation()) > FMath::DegreesToRadians(1.0)
                || Buildings != RealAfter || !bStock) return End(false, TEXT("confirmation pose, count or inventory"));
            if (OutputSlot.IsEmpty()) return End(true, TEXT("completed placement"));
            FString Existing;
            if (!OutputSlot.StartsWith(TEXT("SAQ_")) || UFGSaveSystem::GetAbsolutePathForSaveGame(&World, OutputSlot, Existing))
                return End(false, TEXT("invalid or existing output slot"));
            Save.SaveGame(OutputSlot, FOnSaveGameComplete::CreateLambda([](bool Ok, const FText& Error, void*)
            {
                UE_LOG(LogSaveCheckPlacementSupport, Display, TEXT("pass=%d write=%d %s"), Ok, Ok, *Error.ToString());
                FPlatformMisc::RequestExit(false);
            }), nullptr);
            return false;
        }
        }
        ++Stage;
        Age = 0;
        return true;
    }
};
}

FTickerDelegate MakePlacementSupportCheck(FString Directory, FString OutputSlot, bool bReload)
{
    return FTickerDelegate::CreateLambda([Check = ShadowPlacementProbe::FCheck{MoveTemp(Directory), MoveTemp(OutputSlot), bReload}](float) mutable
    {
        if (!GEngine) return true;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            auto* World = Context.World();
            if (!World || !World->IsGameWorld() || !World->HasBegunPlay()
                || !World->GetMapName().Contains(TEXT("Persistent_Level"))) continue;
            auto* Save = UFGSaveSession::Get(World);
            auto* Player = World->GetFirstPlayerController();
            auto* Character = Player ? Cast<AFGCharacterPlayer>(Player->GetPawn()) : nullptr;
            if (!Save || !Character) continue;
            Save->SetAutoSaveEnabled(false);
            return Check.Tick(*World, *Player, *Character, *Save);
        }
        return true;
    });
}
