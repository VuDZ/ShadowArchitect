#include "RoofStackFixture.h"
#include "Buildables/FGBuildable.h"
#include "Components/MeshComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Equipment/FGBuildGunBuild.h"
#include "FGCharacterPlayer.h"
#include "FGConstructDisqualifier.h"
#include "FGGameState.h"
#include "FGInventoryComponent.h"
#include "FGRecipe.h"
#include "FGSaveSession.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/FileManager.h"
#include "Hologram/FGBlueprintHologram.h"
#include "Interfaces/IPluginManager.h"
#include "Layout/Children.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UnrealClient.h"
#include "UObject/UnrealType.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY_STATIC(LogSaveCheckPlacementConflict, Log, All);

namespace ShadowConflictProbe
{
constexpr TCHAR LowerBlueprint[] = TEXT("3х3 пустой");
constexpr TCHAR UpperBlueprint[] = TEXT("3 литейных");
constexpr TCHAR Clear[] = TEXT("Свободно");

FString FindFeedback(SWidget& Widget)
{
    if (!Widget.GetVisibility().IsVisible()) return {};
    if (Widget.GetTag() == FName(TEXT("ShadowArchitect.PlacementFeedback")))
        return static_cast<const STextBlock&>(Widget).GetText().ToString();
    if (FChildren* Children = Widget.GetChildren())
        for (int32 Index = 0; Index < Children->Num(); ++Index)
        {
            FString Text = FindFeedback(Children->GetChildAt(Index).Get());
            if (!Text.IsEmpty()) return Text;
        }
    return {};
}

FString FeedbackText()
{
    const auto Viewport = GEngine->GameViewport ? GEngine->GameViewport->GetGameViewportWidget() : nullptr;
    const auto Window = Viewport ? FSlateApplication::Get().FindWidgetWindow(Viewport.ToSharedRef()) : nullptr;
    return Window ? FindFeedback(*Window) : FString();
}

TOptional<FLinearColor> PreviewTint(USceneComponent& Root)
{
    TArray<USceneComponent*> Components;
    Root.GetChildrenComponents(true, Components);
    Components.Add(&Root);
    for (auto* Component : Components)
        if (const auto* Mesh = Cast<UMeshComponent>(Component); Mesh && Mesh->IsVisible())
            for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
                if (auto* Material = Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(Slot));
                    Material && GetNameSafe(Material->GetMaterial()) == TEXT("M_ShadowPlacementTint"))
                    return Material->K2_GetVectorParameterValue(TEXT("PlacementColor"));
    return {};
}

FBox VisibleBounds(USceneComponent& Root)
{
    FBox Bounds(ForceInit);
    TArray<USceneComponent*> Components;
    Root.GetChildrenComponents(true, Components);
    Components.Add(&Root);
    for (const auto* Component : Components)
        if (const auto* Mesh = Cast<UMeshComponent>(Component); Mesh && Mesh->IsVisible() && Mesh->IsRegistered())
            Bounds += Mesh->CalcBounds(Mesh->GetComponentTransform()).GetBox();
    return Bounds;
}

TMap<TSubclassOf<UFGItemDescriptor>, int32> Stock(AFGCharacterPlayer& Character)
{
    TArray<FInventoryStack> Stacks;
    Character.GetInventory()->GetInventoryStacks(Stacks);
    TMap<TSubclassOf<UFGItemDescriptor>, int32> Result;
    for (const FInventoryStack& Stack : Stacks)
        if (Stack.HasItems()) Result.FindOrAdd(Stack.Item.GetItemClass()) += Stack.NumItems;
    return Result;
}

void SetPose(AFGBlueprintHologram& Preview, const FTransform& Pose)
{
    Preview.LockHologramPosition(false);
    Preview.SetActorTransform(Pose);
    Preview.LockHologramPosition(true);
    Preview.SetActorHiddenInGame(false);
}

struct FCheck
{
    enum class EStage { Ready, Lower, Foundries, Free, Touch, Conflict, Confirm, NativeSetup, Native };
    FString Directory;
    EStage Stage = EStage::Ready;
    int32 Age = 0;
    int32 InitialPlans = 0;
    int32 InitialBuildings = 0;
    bool bLowerPrepared = false;
    bool bCasePassed = true;
    bool bFeedbackPassed = true;
    FTransform LowerPose;
    FTransform TouchPose;
    FTransform ConflictPose;
    FVector Roof = FVector::ZeroVector;
    FVector Aim = FVector::ZeroVector;
    FGuid LowerId;
    TWeakObjectPtr<USceneComponent> FoundryRoot;
    TWeakObjectPtr<AFGBuildable> FoundrySource;
    TMap<TSubclassOf<UFGItemDescriptor>, int32> InitialStock;

    void Next(EStage State) { Stage = State; Age = 0; }

    bool End(bool bPassed, const TCHAR* Reason) const
    {
        UE_LOG(LogSaveCheckPlacementConflict, Display, TEXT("pass=%d stage=%d %s"), bPassed, static_cast<int32>(Stage), Reason);
        FPlatformMisc::RequestExit(false);
        return false;
    }

    void MoveCamera(APlayerController& Player, AFGCharacterPlayer& Character, const FVector& At, const FVector& Offset)
    {
        Aim = At;
        Character.GetCharacterMovement()->StopMovementImmediately();
        Character.TeleportTo(At + Offset - FVector(0, 0, Character.BaseEyeHeight), FRotator::ZeroRotator, false, true);
        Player.SetViewTarget(&Character);
        Player.SetControlRotation((Aim - Character.GetPawnViewLocation()).Rotation());
    }

    void Frame(const TCHAR* Name) const
    {
        FScreenshotRequest::RequestScreenshot(Directory / (FString(Name) + TEXT(".png")), true, false);
    }

    bool Measure(AFGBlueprintHologram& Preview, const TCHAR* Case, bool bOverlap, bool bBlocked) const
    {
        auto* Root = FoundryRoot.Get();
        if (!Root) return false;
        const FString Label = FeedbackText();
        const auto Tint = PreviewTint(*Root);
        const FLinearColor Color = Tint.Get(FLinearColor::Black);
        TArray<TSubclassOf<UFGConstructDisqualifier>> Reasons;
        Preview.GetConstructDisqualifiers(Reasons);
        for (const auto& Reason : Reasons)
            UE_LOG(LogSaveCheckPlacementConflict, Display, TEXT("case=%s native_reason=%s"), Case, *GetNameSafe(Reason.Get()));
        FBox Bounds(ForceInit);
        for (const auto& Part : Preview.mBuildableToNewRoot)
            if (IsValid(Part.Value.Get())) Bounds += VisibleBounds(*Part.Value);
        UE_LOG(LogSaveCheckPlacementConflict, Display,
            TEXT("case=%s target=%s label=\"%s\" tint=%d rgba=%s can_construct=%d native_material=%d pose=%s bottom_cm=%.2f roof_cm=%.2f"),
            Case, *LowerId.ToString(), *Label, Tint.IsSet(), *Color.ToString(), Preview.CanConstruct(),
            static_cast<int32>(Preview.GetHologramMaterialState()), *Preview.GetActorTransform().ToString(),
            Bounds.Min.Z, Roof.Z);
        if (bBlocked) return !Preview.CanConstruct() && !Reasons.IsEmpty() && Label.IsEmpty() && !Tint.IsSet();
        if (!Tint.IsSet() || !Preview.CanConstruct()) return false;
        return bOverlap ? Label.Contains(LowerBlueprint) && Label.Contains(TEXT("Пересечение")) && Color.R > Color.G
            : Label == Clear && Color.G > Color.R;
    }

    bool Tick(UWorld& World, APlayerController& Player, AFGCharacterPlayer& Character)
    {
        auto* Gun = Character.GetBuildGun();
        if (!Gun || !Character.GetInventory()) return true;
        if (++Age > 100) return End(false, TEXT("timeout"));
        auto* Preview = RoofStackPreview(*Gun);
        TArray<FRoofStackPlan> Plans;
        if (Stage != EStage::Ready && !Aim.IsZero()) Player.SetControlRotation((Aim - Character.GetPawnViewLocation()).Rotation());
        switch (Stage)
        {
        case EStage::Ready:
        {
            if (Age < 12 || !ReadRoofStackPlans(World, Plans)) return true;
            if (Directory.IsEmpty() || !IFileManager::Get().MakeDirectory(*Directory, true))
                return End(false, TEXT("cannot create frames directory"));
            auto* State = World.GetGameState<AFGGameState>();
            if (!State) return End(false, TEXT("stock game state missing"));
            State->SetCheatNoCost(true);
            InitialPlans = Plans.Num();
            InitialBuildings = RoofStackBuildingCount(World);
            InitialStock = Stock(Character);
            const FVector Start = Character.GetActorLocation();
            LowerPose = FTransform(FRotator(0, 30, 0), Start + FVector(0, 0, 6200));
            Character.GetCharacterMovement()->SetMovementMode(MOVE_Flying);
            MoveCamera(Player, Character, Start - FVector(0, 0, 100), FVector(0, -1700, 4800));
            Character.HotKeyBlueprint(LowerBlueprint);
            const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("ShadowArchitect"));
            UE_LOG(LogSaveCheckPlacementConflict, Display, TEXT("version=%s initial_plans=%d real_buildings=%d"),
                Plugin ? *Plugin->GetDescriptor().VersionName : TEXT("missing"), InitialPlans, InitialBuildings);
            Next(EStage::Lower);
            break;
        }
        case EStage::Lower:
            if (!Preview || Preview->mBuildableToNewRoot.IsEmpty()) break;
            if (Age < 9) break;
            if (!bLowerPrepared)
            {
                bLowerPrepared = true;
                Age = 9;
                SetPose(*Preview, LowerPose);
            }
            if (Age == 10) RoofStackKey(Player, EKeys::F7, IE_Pressed);
            if (Age == 11) RoofStackKey(Player, EKeys::F7, IE_Released);
            if (Age < 14 || !ReadRoofStackPlans(World, Plans) || Plans.Num() == InitialPlans) break;
            if (Plans.Num() != InitialPlans + 1 || Plans.Last().Name != LowerBlueprint
                || !Plans.Last().Anchor.Equals(LowerPose, 0.1) || !Plans.Last().RoofPoint().IsSet())
                return End(false, TEXT("lower Blueprint fixture mismatched"));
            LowerId = Plans.Last().Id;
            // Use the outer slab seam, away from the ceiling light under the center of this fixture.
            Roof = LowerPose.TransformPosition(FVector(1200, 0,
                LowerPose.InverseTransformPosition(Plans.Last().RoofPoint().GetValue()).Z));
            MoveCamera(Player, Character, Roof, FVector(1850, -1700, 1100));
            Character.UnequipBuildGun();
            Character.HotKeyBlueprint(UpperBlueprint);
            Next(EStage::Foundries);
            break;
        case EStage::Foundries:
        {
            if (!Preview || Preview->mBuildableToNewRoot.IsEmpty() || Age < 7) break;
            SetPose(*Preview, FTransform(FRotator(0, 33, 0), Roof));
            FBox Lowest(ForceInit);
            for (const auto& Pair : Preview->mBuildableToNewRoot)
            {
                if (!IsValid(Pair.Value.Get()) || !IsValid(Pair.Key.Get())) continue;
                const FBox Bounds = VisibleBounds(*Pair.Value);
                if (!FoundryRoot.IsValid() && Pair.Key->GetClass()->GetFName() == TEXT("Build_FoundryMk1_C"))
                {
                    FoundryRoot = Pair.Value.Get();
                    FoundrySource = Pair.Key.Get();
                }
                if (!Bounds.IsValid || (Lowest.IsValid && Lowest.Min.Z <= Bounds.Min.Z)) continue;
                Lowest = Bounds;
            }
            if (!Lowest.IsValid || !FoundryRoot.IsValid()) return End(false, TEXT("cannot measure foundry part"));
            TouchPose = Preview->GetActorTransform();
            const FVector PartBottom(Lowest.GetCenter().X, Lowest.GetCenter().Y, Lowest.Min.Z);
            TouchPose.AddToTranslation(Roof - PartBottom);
            ConflictPose = TouchPose;
            ConflictPose.AddToTranslation(FVector(0, 0, -60));
            FTransform FreePose = TouchPose;
            FreePose.AddToTranslation(FVector(0, 0, 180));
            SetPose(*Preview, FreePose);
            Next(EStage::Free);
            break;
        }
        case EStage::Free:
        case EStage::Touch:
        case EStage::Conflict:
            if (!Preview) return End(false, TEXT("foundry preview disappeared"));
            if (Age == 6)
            {
                const TCHAR* Case = Stage == EStage::Free ? TEXT("free") : Stage == EStage::Touch ? TEXT("touch") : TEXT("overlap");
                Frame(Case);
                bCasePassed = Measure(*Preview, Case, Stage == EStage::Conflict, false)
                    && ReadRoofStackPlans(World, Plans) && Plans.Num() == InitialPlans + 1;
                bFeedbackPassed &= bCasePassed;
            }
            if (Age < 9) break;
            if (Stage == EStage::Free)
            {
                SetPose(*Preview, TouchPose);
                Next(EStage::Touch);
            }
            else if (Stage == EStage::Touch)
            {
                SetPose(*Preview, ConflictPose);
                Next(EStage::Conflict);
            }
            else Next(EStage::Confirm);
            break;
        case EStage::Confirm:
            if (Age == 1) RoofStackKey(Player, EKeys::F7, IE_Pressed);
            if (Age == 2) RoofStackKey(Player, EKeys::F7, IE_Released);
            if (Age < 6 || !ReadRoofStackPlans(World, Plans)) break;
            {
                const int32 Real = RoofStackBuildingCount(World);
                const bool bStock = InitialStock.OrderIndependentCompareEqual(Stock(Character));
                UE_LOG(LogSaveCheckPlacementConflict, Display,
                    TEXT("input=F7 plans=%d->%d new=%s real_buildings=%d->%d inventory_same=%d"),
                    InitialPlans + 1, Plans.Num(), Plans.IsEmpty() ? TEXT("none") : *Plans.Last().Id.ToString(),
                    InitialBuildings, Real, bStock);
                if (Plans.Num() != InitialPlans + 2 || Plans[InitialPlans].Id != LowerId
                    || Plans.Last().Name != UpperBlueprint || !Plans.Last().Anchor.Equals(ConflictPose, 0.1)
                    || Real != InitialBuildings || !bStock)
                    return End(false, TEXT("overlap did not add exactly one plan"));
            }
            Next(EStage::NativeSetup);
            break;
        case EStage::NativeSetup:
            if (!Preview || !FoundryRoot.IsValid() || !FoundrySource.IsValid())
                return End(false, TEXT("cannot stage native overlap"));
            {
                FTransform Pose = TouchPose;
                Pose.AddToTranslation(FVector(5000, 0, 0));
                SetPose(*Preview, Pose);
                auto& Source = *FoundrySource.Get();
                const FTransform RealPose = FoundryRoot->GetComponentTransform();
                auto* Real = World.SpawnActorDeferred<AFGBuildable>(Source.GetClass(), RealPose, nullptr, nullptr,
                    ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
                if (!Real) return End(false, TEXT("real foundry fixture could not be spawned"));
                Real->SetBuiltWithRecipe(Source.GetBuiltWithRecipe());
                if (auto* Managed = FindFProperty<FBoolProperty>(AFGBuildable::StaticClass(), TEXT("mManagedByLightweightBuildableSubsystem")))
                    Managed->SetPropertyValue_InContainer(Real, false);
                Real->FinishSpawning(RealPose);
                MoveCamera(Player, Character, RealPose.GetLocation(), FVector(1100, -1300, 800));
            }
            Next(EStage::Native);
            break;
        case EStage::Native:
            if (!Preview) return End(false, TEXT("native test preview disappeared"));
            if (Age == 8)
            {
                Frame(TEXT("native-overlap"));
                bCasePassed = Measure(*Preview, TEXT("native-overlap"), false, true);
            }
            if (Age > 10 && !bCasePassed)
                return End(false, TEXT("native overlap did not keep the stock prohibition and suppress clear"));
            if (Age > 11) return End(bFeedbackPassed, bFeedbackPassed
                ? TEXT("free, touch, overlap, F7 and native prohibition")
                : TEXT("F7 and native prohibition passed; preview feedback or plan count mismatched"));
            break;
        }
        return true;
    }
};
}

FTickerDelegate MakePlacementConflictCheck(FString Directory)
{
    return FTickerDelegate::CreateLambda([Check = ShadowConflictProbe::FCheck{ MoveTemp(Directory) }](float) mutable
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
            return Check.Tick(*World, *Player, *Character);
        }
        return true;
    });
}
