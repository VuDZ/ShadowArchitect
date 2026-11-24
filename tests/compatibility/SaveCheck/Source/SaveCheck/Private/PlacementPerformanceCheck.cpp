#include "RoofStackFixture.h"
#include "Buildables/FGBuildable.h"
#include "Components/MeshComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Equipment/FGBuildGun.h"
#include "FGCharacterPlayer.h"
#include "FGGameState.h"
#include "FGInventoryComponent.h"
#include "FGPlayerState.h"
#include "FGRecipe.h"
#include "FGSaveInterface.h"
#include "FGSaveSession.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Hologram/FGBlueprintHologram.h"
#include "Interfaces/IPluginManager.h"
#include "Layout/Children.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "UnrealClient.h"
#include "UObject/UnrealType.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY_STATIC(LogSaveCheckPlacementPerformance, Log, All);

namespace ShadowPlacementPerformance
{
constexpr TCHAR FoundryBlueprint[] = TEXT("3 литейных");
constexpr double LegSeconds = 1.5;
constexpr double SettleSeconds = 0.6;
constexpr int32 Repeats = 3;
constexpr double StationSpacing = 4400;
const FVector EyeOffset(920, -730, 2250);
const FVector Sweep(34, 25, 0);

enum class EMode { Stowed, Plans, World };
constexpr const TCHAR* Modes[] = { TEXT("stowed"), TEXT("plans"), TEXT("world") };
constexpr const TCHAR* Sections[] = { TEXT("empty"), TEXT("slab"), TEXT("column"), TEXT("multi"), TEXT("free") };

FString ReadLabel(SWidget& Widget)
{
    if (!Widget.GetVisibility().IsVisible()) return {};
    if (Widget.GetTag() == FName(TEXT("ShadowArchitect.PlacementFeedback")))
        return static_cast<STextBlock&>(Widget).GetText().ToString();
    if (FChildren* Children = Widget.GetChildren())
        for (int32 Index = 0; Index < Children->Num(); ++Index)
        {
            FString Label = ReadLabel(Children->GetChildAt(Index).Get());
            if (!Label.IsEmpty()) return Label;
        }
    return {};
}

FString FeedbackLabel()
{
    const auto Viewport = GEngine->GameViewport ? GEngine->GameViewport->GetGameViewportWidget() : nullptr;
    const auto Window = Viewport ? FSlateApplication::Get().FindWidgetWindow(Viewport.ToSharedRef()) : nullptr;
    return Window ? ReadLabel(*Window) : FString();
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

bool PreparePaidInventory(AFGCharacterPlayer& Character, AFGBlueprintHologram& Preview)
{
    if (auto* State = Character.GetWorld()->GetGameState<AFGGameState>()) State->SetCheatNoCost(false);
    if (auto* PlayerState = Character.GetPlayerState<AFGPlayerState>(); PlayerState && PlayerState->GetPlayerRules().NoBuildCost)
    {
        UFunction* SetNoBuildCost = PlayerState->FindFunction(TEXT("Server_SetNoBuildCost"));
        bool bNoBuildCost = false;
        if (SetNoBuildCost) PlayerState->ProcessEvent(SetNoBuildCost, &bNoBuildCost);
    }
    auto& Inventory = *Character.GetInventory();
    if (Inventory.GetNoBuildCost()) return false;
    TMap<TSubclassOf<UFGItemDescriptor>, int32> Cost;
    for (const FItemAmount& Item : Preview.GetCost(true))
        if (Item.ItemClass && Item.Amount > 0) Cost.FindOrAdd(Item.ItemClass) += Item.Amount;
    if (Cost.IsEmpty()) return false;
    for (const auto& Item : Cost)
    {
        const int32 Missing = Item.Value - Inventory.GetNumItems(Item.Key);
        if (Missing > 0 && Inventory.AddStack(FInventoryStack(Missing, Item.Key)) != Missing) return false;
    }
    UE_LOG(LogSaveCheckPlacementPerformance, Display, TEXT("paid_inventory=1 cost_lines=%d"), Cost.Num());
    return true;
}

FBox MeshBounds(USceneComponent& Root, const FTransform& RelativeTo)
{
    TArray<USceneComponent*> Components;
    Root.GetChildrenComponents(true, Components);
    Components.Add(&Root);
    FBox Bounds(ForceInit);
    for (const auto* Component : Components)
        if (const auto* Mesh = Cast<UMeshComponent>(Component); Mesh && Mesh->IsVisible() && Mesh->IsRegistered())
            Bounds += Mesh->CalcBounds(Mesh->GetComponentTransform().GetRelativeTransform(RelativeTo)).GetBox();
    return Bounds;
}

FBox PreviewBounds(AFGBlueprintHologram& Preview)
{
    FBox Bounds(ForceInit);
    for (const auto& Pair : Preview.mBuildableToNewRoot)
        if (IsValid(Pair.Value.Get())) Bounds += MeshBounds(*Pair.Value, FTransform::Identity);
    return Bounds;
}

bool SpawnFloor(UWorld& World, const FVector& Top)
{
    auto* Class = LoadClass<AFGBuildable>(nullptr,
        TEXT("/Game/FactoryGame/Buildable/Building/Foundation/Build_Foundation_8x1_01.Build_Foundation_8x1_01_C"));
    auto* Recipe = LoadClass<UFGRecipe>(nullptr,
        TEXT("/Game/FactoryGame/Recipes/Buildings/Foundations/Recipe_Foundation_8x1_01.Recipe_Foundation_8x1_01_C"));
    if (!Class || !Recipe) return false;
    const FTransform Pose(Top - FVector(0, 0, 50));
    auto* Floor = World.SpawnActorDeferred<AFGBuildable>(Class, Pose, nullptr, nullptr,
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
    if (!Floor) return false;
    Floor->SetBuiltWithRecipe(Recipe);
    if (auto* Managed = FindFProperty<FBoolProperty>(AFGBuildable::StaticClass(), TEXT("mManagedByLightweightBuildableSubsystem")))
        Managed->SetPropertyValue_InContainer(Floor, false);
    Floor->FinishSpawning(Pose);
    return true;
}

struct FRouteFixture
{
    FVector Lane;
    FVector RoofOffset;
    FVector Points[5];

    FVector Base(int32 Station) const
    {
        return Lane + FVector(Station * StationSpacing, 0, 0);
    }

    double FloorHeight() const { return Lane.Z - 200.0; }

    bool Prepare(UWorld& World, const FRoofStackPlan& Template)
    {
        Lane = Template.Anchor.GetLocation() + FVector(-4500, -5500, 0);
        // Use the rightmost top slab, away from the ceiling light inside the deck.
        const FRoofStackPart* Slab = nullptr;
        for (const auto& Part : Template.Parts)
            if (Part.SourceClass.EndsWith(TEXT("Build_Foundation_Concrete_8x1_C")))
            {
                const FVector Position = Part.Relative.GetLocation();
                if (Slab)
                {
                    const FVector Best = Slab->Relative.GetLocation();
                    if (Position.Z < Best.Z - 1.0) continue;
                    if (FMath::IsNearlyEqual(Position.Z, Best.Z, 1.0))
                    {
                        if (Position.X < Best.X - 1.0) continue;
                        if (FMath::IsNearlyEqual(Position.X, Best.X, 1.0)
                            && (FMath::Abs(Position.Y) > FMath::Abs(Best.Y) + 1.0
                                || (FMath::IsNearlyEqual(FMath::Abs(Position.Y), FMath::Abs(Best.Y), 1.0)
                                    && Position.Y >= Best.Y))) continue;
                    }
                }
                Slab = &Part;
            }
        if (!Slab) return false;
        RoofOffset = Slab->Relative.GetLocation() + FVector(145, 100, 50);
        for (int32 Station = 0; Station < UE_ARRAY_COUNT(Points); ++Station)
        {
            Points[Station] = Base(Station) + RoofOffset;
            const double ToFloor = Points[Station].Z - FloorHeight();
            if (!SpawnFloor(World, Points[Station] - (ToFloor / EyeOffset.Z) * EyeOffset)) return false;
        }
        return true;
    }

    bool InstallProjections(UWorld& World, TConstArrayView<FVector> FoundryOffsets)
    {
        auto* Type = FindObject<UClass>(nullptr, TEXT("/Script/ShadowArchitect.ShadowPlanSubsystem"));
        if (!Type || FoundryOffsets.Num() != 2) return false;
        TActorIterator<AActor> Subsystem(&World, Type);
        const auto* Plans = FindFProperty<FArrayProperty>(Type, TEXT("Plans"));
        if (!Subsystem || !Plans) return false;
        const auto* Record = CastField<FStructProperty>(Plans->Inner);
        if (!Record) return false;
        const auto* Pose = FindFProperty<FStructProperty>(Record->Struct, TEXT("Transform"));
        const auto* Parts = FindFProperty<FArrayProperty>(Record->Struct, TEXT("Parts"));
        const auto* Name = FindFProperty<FStrProperty>(Record->Struct, TEXT("BlueprintId"));
        const auto* Part = Parts ? CastField<FStructProperty>(Parts->Inner) : nullptr;
        if (!Pose || !Name || !Part) return false;
        const auto* Relative = FindFProperty<FStructProperty>(Part->Struct, TEXT("InPlan"));
        const auto* Source = FindFProperty<FStrProperty>(Part->Struct, TEXT("SourceClass"));
        const auto* Center = FindFProperty<FStructProperty>(Part->Struct, TEXT("BoundsCenter"));
        const auto* Size = FindFProperty<FStructProperty>(Part->Struct, TEXT("BoundsSize"));
        if (!Relative || !Source || !Center || !Size) return false;
        FScriptArrayHelper Records(Plans, Plans->ContainerPtrToValuePtr<void>(*Subsystem));
        if (Records.Num() < 6) return false;
        FScriptArrayHelper Template(Parts, Parts->ContainerPtrToValuePtr<void>(Records.GetRawPtr(0)));
        const void* Pillar = nullptr;
        for (int32 Index = 0; Index < Template.Num(); ++Index)
            if (Source->GetPropertyValue_InContainer(Template.GetRawPtr(Index)).EndsWith(TEXT("Build_Pillar_Small_Concrete_C")))
            {
                Pillar = Template.GetRawPtr(Index);
                break;
            }
        if (!Pillar) return false;
        for (int32 Station = 1; Station <= 3; ++Station)
            *Pose->ContainerPtrToValuePtr<FTransform>(Records.GetRawPtr(Station - 1)) = FTransform(Base(Station));
        FTransform PillarInPlan = *Relative->ContainerPtrToValuePtr<FTransform>(Pillar);
        PillarInPlan.SetLocation(FVector::ZeroVector);
        const FVector Mid = *Center->ContainerPtrToValuePtr<FVector>(Pillar);
        const FVector Half = *Size->ContainerPtrToValuePtr<FVector>(Pillar) * 0.5;
        const FBox PillarBounds = FBox(Mid - Half, Mid + Half).TransformBy(PillarInPlan);
        if (!PillarBounds.IsValid) return false;

        const TCHAR* Names[] = { TEXT("F04-Column"), TEXT("F04-Multi-A"), TEXT("F04-Multi-B") };
        for (int32 Index = 0; Index < 3; ++Index)
        {
            void* Entry = Records.GetRawPtr(3 + Index);
            FScriptArrayHelper Target(Parts, Parts->ContainerPtrToValuePtr<void>(Entry));
            Target.EmptyValues();
            void* Piece = Target.GetRawPtr(Target.AddValue());
            Part->CopyCompleteValue(Piece, Pillar);
            *Relative->ContainerPtrToValuePtr<FTransform>(Piece) = PillarInPlan;
            Name->SetPropertyValue_InContainer(Entry, Names[Index]);
            const FVector Offset = FoundryOffsets[Index == 2 ? 1 : 0];
            FVector Contact = Points[Index == 0 ? 2 : 3] + FVector(Offset.X, Offset.Y, 0);
            Contact -= FVector(PillarBounds.GetCenter().X, PillarBounds.GetCenter().Y, PillarBounds.Min.Z);
            *Pose->ContainerPtrToValuePtr<FTransform>(Entry) = FTransform(Contact);
            UE_LOG(LogSaveCheckPlacementPerformance, Display, TEXT("fixture=%s anchor_cm=%s"),
                Names[Index], *Contact.ToCompactString());
        }
        IFGSaveInterface::Execute_PostLoadGame(*Subsystem, 0, 0);
        return true;
    }
};

struct FRouteFrame
{
    uint64 Frame;
    double Now;
    double Ms;
    const TCHAR* Phase;
    const TCHAR* Mode;
    const TCHAR* Section;
    int32 Repeat;
};

class FCheck : public TSharedFromThis<FCheck>
{
public:
    FCheck(FString Output, int32 Plans) : Directory(MoveTemp(Output)), ExpectedPlans(Plans) {}
    ~FCheck() { Detach(); }

    void Attach()
    {
        BeginHandle = FCoreDelegates::OnBeginFrame.AddSP(AsShared(), &FCheck::BeginFrame);
        EndHandle = FCoreDelegates::OnEndFrame.AddSP(AsShared(), &FCheck::EndFrame);
        Frames.Reserve(32 * 1024);
    }

    bool Tick()
    {
        if (!GEngine) return true;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            auto* World = Context.World();
            if (!World || !World->IsGameWorld() || !World->HasBegunPlay()
                || !World->GetMapName().Contains(TEXT("Persistent_Level"))) continue;
            auto* Save = UFGSaveSession::Get(World);
            auto* PC = World->GetFirstPlayerController();
            auto* Pawn = PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
            auto* Gun = Pawn ? Pawn->GetBuildGun() : nullptr;
            if (!Save || !Gun || !Pawn->GetInventory()) continue;
            if (Stage == EStage::Setup && Deadline == 0) Save->SetAutoSaveEnabled(false);
            Player = PC;
            Character = Pawn;
            return Advance(*World, *PC, *Pawn, *Gun);
        }
        return true;
    }

private:
    enum class EStage { Setup, ReadyForFirstPreview, FirstPreview, InstallFixture, Mode, Toggle, Release, Capture, Settle, Measure };

    void Detach()
    {
        FCoreDelegates::OnBeginFrame.Remove(BeginHandle);
        FCoreDelegates::OnEndFrame.Remove(EndHandle);
        BeginHandle.Reset();
        EndHandle.Reset();
    }

    void BeginFrame()
    {
        if (!Player.IsValid() || !Character.IsValid() || Stage < EStage::Settle) return;
        const double Wave = FMath::Sin((FPlatformTime::Seconds() - StartedLeg) * 2.0 * UE_PI);
        Player->SetControlRotation((Fixture.Points[Station] + Wave * Sweep - Character->GetPawnViewLocation()).Rotation());
    }

    void EndFrame()
    {
        const double Now = FPlatformTime::Seconds();
        const bool bFirst = Stage == EStage::FirstPreview || Stage == EStage::InstallFixture;
        if (LastFrame > 0 && (bFirst || Stage >= EStage::Settle))
        {
            Frames.Add({ GFrameCounter, Now, (Now - LastFrame) * 1000.0,
                bFirst ? TEXT("open") : Stage == EStage::Measure ? TEXT("measure") : TEXT("settle"),
                bFirst ? TEXT("first-preview") : Modes[static_cast<int32>(Mode)], Sections[Station], Repeat });
        }
        LastFrame = Now;
    }

    bool End(bool bPassed, const TCHAR* Reason)
    {
        Detach();
        FString Csv = TEXT("frame,mono_s,frame_ms,phase,mode,section,repeat\n");
        Csv.Reserve(128 + Frames.Num() * 76);
        for (const auto& S : Frames)
            Csv += FString::Printf(TEXT("%llu,%.6f,%.6f,%s,%s,%s,%d\n"),
                static_cast<unsigned long long>(S.Frame), S.Now, S.Ms, S.Phase, S.Mode, S.Section, S.Repeat);
        const bool bWritten = FFileHelper::SaveStringToFile(Csv, *(Directory / TEXT("frames.csv")),
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        UE_LOG(LogSaveCheckPlacementPerformance, Display, TEXT("pass=%d frames=%d csv_written=%d %s"),
            bPassed && bWritten, Frames.Num(), bWritten, Reason);
        FPlatformMisc::RequestExit(false);
        return false;
    }

    bool MoveCamera(APlayerController& PC, AFGCharacterPlayer& Pawn, const FVector& At) const
    {
        Pawn.GetCharacterMovement()->StopMovementImmediately();
        if (!Pawn.TeleportTo(At + EyeOffset - FVector(0, 0, Pawn.BaseEyeHeight), FRotator::ZeroRotator, false, true))
            return false;
        PC.SetViewTarget(&Pawn);
        PC.SetControlRotation((At - Pawn.GetPawnViewLocation()).Rotation());
        return true;
    }

    bool BeginLeg(APlayerController& PC, AFGCharacterPlayer& Pawn, double Now)
    {
        if (!MoveCamera(PC, Pawn, Fixture.Points[Station])) return End(false, TEXT("route camera"));
        Stage = EStage::Settle;
        StartedLeg = Now;
        Deadline = Now + SettleSeconds;
        return true;
    }

    void VerifyLeg(AFGBuildGun& Gun, AFGBlueprintHologram* Preview)
    {
        const bool bStowed = Mode == EMode::Stowed;
        const bool bPlanHit = Gun.GetHitResult().GetActor()
            && Gun.GetHitResult().GetActor()->GetClass()->GetFName() == TEXT("ShadowProjection");
        const bool bPreview = Preview && !Preview->IsHidden() && !Preview->IsDisabled();
        const FString Label = FeedbackLabel();
        const FBox Bounds = bPreview ? PreviewBounds(*Preview) : FBox(ForceInit);
        const bool bOnRoof = Mode == EMode::Plans && Station >= 1 && Station <= 3;
        const bool bSlab = Bounds.IsValid && FMath::IsNearlyEqual(Bounds.Min.Z, Fixture.Points[Station].Z, 1.0);
        bool bPass = bStowed ? !Gun.IsEquipped() && !Preview
            : bPreview && Gun.GetHitResult().bBlockingHit && bPlanHit == bOnRoof && (!bOnRoof || bSlab);
        if (!bStowed)
        {
            if (Mode == EMode::World)
                bPass &= FMath::IsNearlyEqual(Gun.GetHitResult().ImpactPoint.Z, Fixture.FloorHeight(), 1.0)
                    && Label == TEXT("Пересечение планов · 3х3 пустой");
            else if (Station == 2) bPass &= Label.Contains(TEXT("F04-Column"));
            else if (Station == 3)
                bPass &= Label.Contains(TEXT("F04-Multi-A")) && Label.Contains(TEXT("F04-Multi-B"));
            else bPass &= Label == TEXT("Свободно");
        }
        bGeometryPassed &= bPass;
        const TCHAR* ModeName = Modes[static_cast<int32>(Mode)];
        UE_LOG(LogSaveCheckPlacementPerformance, Display,
            TEXT("repeat=%d mode=%s section=%s valid=%d plan_hit=%d visible=%d bottom_cm=%.2f hit=%s label=\"%s\" pose_cm=%s"),
            Repeat, ModeName, Sections[Station], bPass, bPlanHit, bPreview, Bounds.IsValid ? Bounds.Min.Z : 0.0,
            *Gun.GetHitResult().ImpactPoint.ToCompactString(), *Label,
            Preview ? *Preview->GetActorLocation().ToCompactString() : TEXT("none"));
        if (Repeat == 0 || !bPass)
            FScreenshotRequest::RequestScreenshot(Directory / FString::Printf(TEXT("%02d-%s-%s.png"), Repeat, ModeName,
                Sections[Station]), true, false);
    }

    bool VerifyWorld(UWorld& World, AFGCharacterPlayer& Pawn)
    {
        TArray<FRoofStackPlan> Plans;
        if (!ReadRoofStackPlans(World, Plans) || Plans.Num() != InitialPlans.Num()) return false;
        for (int32 Index = 0; Index < Plans.Num(); ++Index)
            if (Plans[Index].Id != InitialPlans[Index].Id || !Plans[Index].Anchor.Equals(InitialPlans[Index].Anchor, 0.01))
                return false;
        const int32 Buildings = RoofStackBuildingCount(World);
        const bool bStock = InitialStock.OrderIndependentCompareEqual(Stock(Pawn));
        UE_LOG(LogSaveCheckPlacementPerformance, Display, TEXT("plans=%d->%d real_buildings=%d->%d inventory_same=%d"),
            InitialPlans.Num(), Plans.Num(), InitialBuildings, Buildings, bStock);
        return Buildings == InitialBuildings && bStock;
    }

    bool Advance(UWorld& World, APlayerController& PC, AFGCharacterPlayer& Pawn, AFGBuildGun& Gun)
    {
        const double Now = FPlatformTime::Seconds();
        auto* Preview = RoofStackPreview(Gun);
        switch (Stage)
        {
        case EStage::Setup:
        {
            if (Deadline == 0) Deadline = Now + 3.0;
            if (Now < Deadline) return true;
            if ((ExpectedPlans != 50 && ExpectedPlans != 100) || !ReadRoofStackPlans(World, InitialPlans)
                || InitialPlans.Num() != ExpectedPlans || InitialPlans[0].Name != TEXT("3х3 пустой"))
                return End(false, TEXT("load an F02 slot with the expected grid"));
            if (!FApp::CanEverRender() || !IFileManager::Get().MakeDirectory(*Directory, true))
                return End(false, TEXT("rendering or output directory not available"));
            if (!Fixture.Prepare(World, InitialPlans[0])) return End(false, TEXT("route floor fixture"));
            Pawn.GetCharacterMovement()->SetMovementMode(MOVE_Flying);
            if (!MoveCamera(PC, Pawn, Fixture.Points[0])) return End(false, TEXT("initial camera"));
            if (auto* State = World.GetGameState<AFGGameState>()) State->SetCheatNoCost(true);
            const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("ShadowArchitect"));
            UE_LOG(LogSaveCheckPlacementPerformance, Display, TEXT("version=%s fixture_plans=%d"),
                Plugin ? *Plugin->GetDescriptor().VersionName : TEXT("missing"), InitialPlans.Num());
            for (int32 Index = 0; Index < UE_ARRAY_COUNT(Sections); ++Index)
                UE_LOG(LogSaveCheckPlacementPerformance, Display, TEXT("route=%s point_cm=%s eye_offset_cm=%s sweep_cm=%s"),
                    Sections[Index], *Fixture.Points[Index].ToCompactString(), *EyeOffset.ToCompactString(),
                    *Sweep.ToCompactString());
            InitialBuildings = RoofStackBuildingCount(World);
            Deadline = FPlatformTime::Seconds() + SettleSeconds;
            Stage = EStage::ReadyForFirstPreview;
            break;
        }
        case EStage::ReadyForFirstPreview:
            if (Now < Deadline) break;
            StartedPreview = Now;
            PreviewInputFrame = GFrameCounter;
            Stage = EStage::FirstPreview;
            Pawn.HotKeyBlueprint(FoundryBlueprint);
            break;
        case EStage::FirstPreview:
            if (Now - StartedPreview > 20.0) return End(false, TEXT("first foundry preview did not appear"));
            if (!Preview || Preview->IsHidden() || Preview->IsDisabled() || Preview->mBuildableToNewRoot.IsEmpty()
                || !Gun.GetHitResult().bBlockingHit) break;
            UE_LOG(LogSaveCheckPlacementPerformance, Display,
                TEXT("first_preview_ms=%.3f preview_roots=%d input_frame=%llu ready_frame=%llu"),
                (Now - StartedPreview) * 1000, Preview->mBuildableToNewRoot.Num(),
                static_cast<unsigned long long>(PreviewInputFrame), static_cast<unsigned long long>(GFrameCounter));
            Stage = EStage::InstallFixture;
            break;
        case EStage::InstallFixture:
        {
            if (!Preview) return End(false, TEXT("foundry preview disappeared during setup"));
            if (!PreparePaidInventory(Pawn, *Preview)) return End(false, TEXT("foundry inventory fixture"));
            InitialStock = Stock(Pawn);
            TArray<FVector> Centers;
            for (const auto& Pair : Preview->mBuildableToNewRoot)
            {
                if (!IsValid(Pair.Key.Get()) || !IsValid(Pair.Value.Get())
                    || Pair.Key->GetClass()->GetFName() != TEXT("Build_FoundryMk1_C")) continue;
                const FBox Bounds = MeshBounds(*Pair.Value, FTransform::Identity);
                if (Bounds.IsValid) Centers.Add(Bounds.GetCenter() - Preview->GetActorLocation());
            }
            Centers.Sort([](const FVector& A, const FVector& B) { return A.Y > B.Y; });
            if (Centers.Num() != 3 || !Fixture.InstallProjections(World, TConstArrayView<FVector>(Centers.GetData(), 2)))
                return End(false, TEXT("protruding column fixture"));
            if (!ReadRoofStackPlans(World, InitialPlans)) return End(false, TEXT("read the installed route"));
            Pawn.UnequipBuildGun();
            Stage = EStage::Mode;
            break;
        }
        case EStage::Mode:
        {
            Station = Mode == EMode::World ? 1 : 0;
            if (!MoveCamera(PC, Pawn, Fixture.Points[Station])) return End(false, TEXT("mode camera"));
            if (Mode == EMode::Stowed)
            {
                Pawn.UnequipBuildGun();
                return BeginLeg(PC, Pawn, Now);
            }
            if (!Preview) Pawn.HotKeyBlueprint(FoundryBlueprint);
            if (bSupport == (Mode == EMode::Plans)) return BeginLeg(PC, Pawn, Now);
            RoofStackKey(PC, EKeys::LeftControl, IE_Pressed);
            RoofStackKey(PC, EKeys::LeftShift, IE_Pressed);
            Stage = EStage::Toggle;
            break;
        }
        case EStage::Toggle:
            RoofStackKey(PC, EKeys::T, IE_Pressed);
            Stage = EStage::Release;
            break;
        case EStage::Release:
            RoofStackKey(PC, EKeys::T, IE_Released);
            RoofStackKey(PC, EKeys::LeftShift, IE_Released);
            RoofStackKey(PC, EKeys::LeftControl, IE_Released);
            bSupport = !bSupport;
            return BeginLeg(PC, Pawn, Now);
        case EStage::Settle:
            if (Now < Deadline) break;
            Stage = EStage::Measure;
            Deadline = Now + LegSeconds;
            break;
        case EStage::Measure:
            if (Now < Deadline) break;
            VerifyLeg(Gun, Preview);
            Stage = EStage::Capture;
            Deadline = Now + 10.0;
            break;
        case EStage::Capture:
            if (FScreenshotRequest::IsScreenshotRequested())
            {
                if (Now >= Deadline) return End(false, TEXT("route screenshot timed out"));
                break;
            }
            if (Mode != EMode::World && ++Station < UE_ARRAY_COUNT(Sections))
                return BeginLeg(PC, Pawn, Now);
            if (Mode == EMode::Stowed) Mode = EMode::Plans;
            else if (Mode == EMode::Plans) Mode = EMode::World;
            else
            {
                if (++Repeat == Repeats)
                {
                    const bool bWorld = VerifyWorld(World, Pawn);
                    return End(bWorld && bGeometryPassed, bWorld && bGeometryPassed
                        ? TEXT("complete route, stock and plan checks") : TEXT("route or world invariant failed"));
                }
                Mode = EMode::Stowed;
            }
            Stage = EStage::Mode;
            break;
        }
        return true;
    }

    FString Directory;
    int32 ExpectedPlans;
    FRouteFixture Fixture;
    EStage Stage = EStage::Setup;
    EMode Mode = EMode::Stowed;
    TWeakObjectPtr<APlayerController> Player;
    TWeakObjectPtr<AFGCharacterPlayer> Character;
    FDelegateHandle BeginHandle;
    FDelegateHandle EndHandle;
    TArray<FRoofStackPlan> InitialPlans;
    TMap<TSubclassOf<UFGItemDescriptor>, int32> InitialStock;
    TArray<FRouteFrame> Frames;
    double Deadline = 0;
    double StartedPreview = 0;
    uint64 PreviewInputFrame = 0;
    double StartedLeg = 0;
    double LastFrame = 0;
    int32 InitialBuildings = 0;
    int32 Station = 0;
    int32 Repeat = 0;
    bool bSupport = false;
    bool bGeometryPassed = true;
};
}

FTickerDelegate MakePlacementPerformanceCheck(FString Directory, int32 Plans)
{
    const auto Check = MakeShared<ShadowPlacementPerformance::FCheck>(MoveTemp(Directory), Plans);
    Check->Attach();
    return FTickerDelegate::CreateLambda([Check](float) { return Check->Tick(); });
}
