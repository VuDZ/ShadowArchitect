#include "AlignmentGuideCheck.h"
#include "AlignmentSurface.h"
#include "Buildables/FGBuildableFoundation.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Equipment/FGBuildGunBuild.h"
#include "FGCharacterPlayer.h"
#include "FGConstructDisqualifier.h"
#include "FGGameState.h"
#include "FGLightweightBuildableSubsystem.h"
#include "FGRecipe.h"
#include "FGRecipeManager.h"
#include "FGSaveInterface.h"
#include "FGSaveSession.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/FileManager.h"
#include "Hologram/FGHologram.h"
#include "InputKeyEventArgs.h"
#include "InstanceData.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Resources/FGBuildingDescriptor.h"
#include "UnrealClient.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogSaveCheckAlignment, Log, All);

namespace
{
struct FFixture
{
    TSubclassOf<UFGRecipe> Recipe;
    TSubclassOf<AFGBuildable> BuildClass;
    FTransform NativePose;
    FTransform GhostPose;
    FTransform Anchor;
    TWeakObjectPtr<AFGBuildable> Native;
    TWeakObjectPtr<AActor> Ghost;
    TArray<FInstanceData> Meshes;
    FBox MeshBounds = FBox(ForceInit);
};

bool ReadGeometry(FFixture& Fixture)
{
    const auto* Defaults = Fixture.BuildClass.GetDefaultObject();
    if (!Defaults) return false;
    Fixture.Meshes = Defaults->GetActorLightweightInstanceData_Implementation();
    if (Fixture.Meshes.IsEmpty())
    {
        const TInlineComponentArray<UStaticMeshComponent*> Components(Defaults);
        for (const auto* Component : Components)
        {
            auto& Mesh = Fixture.Meshes.AddDefaulted_GetRef();
            Mesh.StaticMesh = Component->GetStaticMesh();
            Mesh.RelativeTransform = Component->GetRelativeTransform();
        }
    }
    for (const auto& Mesh : Fixture.Meshes)
        if (Mesh.StaticMesh)
            Fixture.MeshBounds += Mesh.StaticMesh->GetBoundingBox().TransformBy(Mesh.RelativeTransform);
    return Fixture.MeshBounds.IsValid && Fixture.MeshBounds.GetSize().GetMin() > 0;
}

TSubclassOf<UFGRecipe> LoadRecipe(const TCHAR* Directory, const TCHAR* Name)
{
    return LoadClass<UFGRecipe>(nullptr, *FString::Printf(TEXT("/Game/FactoryGame/Recipes/Buildings/%s%s.%s_C"),
        Directory, Name, Name));
}

bool ReadFixture(FFixture& Fixture, TSubclassOf<UFGRecipe> Recipe)
{
    if (!Recipe) return false;
    Fixture.Recipe = Recipe;
    for (const auto& Product : UFGRecipe::GetProducts(Recipe))
    {
        if (!Product.ItemClass || !Product.ItemClass->IsChildOf(UFGBuildingDescriptor::StaticClass())) continue;
        Fixture.BuildClass = UFGBuildingDescriptor::GetBuildableClass(Product.ItemClass.Get());
        return ReadGeometry(Fixture);
    }
    return false;
}

AFGBuildable* SpawnReal(UWorld& World, const FFixture& Fixture, const FTransform& Pose)
{
    auto* Actor = World.SpawnActorDeferred<AFGBuildable>(Fixture.BuildClass, Pose, nullptr, nullptr,
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
    if (!Actor) return nullptr;
    Actor->SetBuiltWithRecipe(Fixture.Recipe);
    // Keep the test actor addressable throughout every trial, including dismantle selection.
    auto* Managed = FindFProperty<FBoolProperty>(AFGBuildable::StaticClass(), TEXT("mManagedByLightweightBuildableSubsystem"));
    if (Managed) Managed->SetPropertyValue_InContainer(Actor, false);
    Actor->FinishSpawning(Pose);
    return Actor;
}

bool AddGhost(FScriptArrayHelper& Plans, const UStruct& RecordType, const FFixture& Fixture)
{
    const auto* Id = FindFProperty<FStructProperty>(&RecordType, TEXT("Id"));
    const auto* Pose = FindFProperty<FStructProperty>(&RecordType, TEXT("Transform"));
    const auto* PartsProp = FindFProperty<FArrayProperty>(&RecordType, TEXT("Parts"));
    const auto* PartType = PartsProp ? CastField<FStructProperty>(PartsProp->Inner) : nullptr;
    if (!Id || !Pose || !PartType) return false;
    const auto* Source = FindFProperty<FStrProperty>(PartType->Struct, TEXT("SourceClass"));
    const auto* InPlan = FindFProperty<FStructProperty>(PartType->Struct, TEXT("InPlan"));
    const auto* Center = FindFProperty<FStructProperty>(PartType->Struct, TEXT("BoundsCenter"));
    const auto* Size = FindFProperty<FStructProperty>(PartType->Struct, TEXT("BoundsSize"));
    const auto* MeshProp = FindFProperty<FArrayProperty>(PartType->Struct, TEXT("Meshes"));
    const auto* MeshType = MeshProp ? CastField<FStructProperty>(MeshProp->Inner) : nullptr;
    if (!Source || !InPlan || !Center || !Size || !MeshType) return false;
    const auto* Asset = FindFProperty<FSoftObjectProperty>(MeshType->Struct, TEXT("Asset"));
    const auto* InPart = FindFProperty<FStructProperty>(MeshType->Struct, TEXT("InPart"));
    if (!Asset || !InPart) return false;

    void* Record = Plans.GetRawPtr(Plans.AddValue());
    *Id->ContainerPtrToValuePtr<FGuid>(Record) = FGuid::NewGuid();
    *Pose->ContainerPtrToValuePtr<FTransform>(Record) = Fixture.Anchor;
    FScriptArrayHelper Parts(PartsProp, PartsProp->ContainerPtrToValuePtr<void>(Record));
    void* Part = Parts.GetRawPtr(Parts.AddValue());
    Source->SetPropertyValue_InContainer(Part, Fixture.BuildClass->GetPathName());
    *InPlan->ContainerPtrToValuePtr<FTransform>(Part) = Fixture.GhostPose.GetRelativeTransform(Fixture.Anchor);
    *Center->ContainerPtrToValuePtr<FVector>(Part) = Fixture.MeshBounds.GetCenter();
    *Size->ContainerPtrToValuePtr<FVector>(Part) = Fixture.MeshBounds.GetSize();
    FScriptArrayHelper Meshes(MeshProp, MeshProp->ContainerPtrToValuePtr<void>(Part));
    for (const auto& Data : Fixture.Meshes)
    {
        if (!Data.StaticMesh) continue;
        void* Mesh = Meshes.GetRawPtr(Meshes.AddValue());
        Asset->SetPropertyValue_InContainer(Mesh, FSoftObjectPtr(Data.StaticMesh.Get()));
        *InPart->ContainerPtrToValuePtr<FTransform>(Mesh) = Data.RelativeTransform;
    }
    return Meshes.Num() > 0;
}

bool RestoreGhosts(UWorld& World, TArray<FFixture>& Fixtures)
{
    UClass* SubsystemType = FindObject<UClass>(nullptr, TEXT("/Script/ShadowArchitect.ShadowPlanSubsystem"));
    UClass* ProjectionType = FindObject<UClass>(nullptr, TEXT("/Script/ShadowArchitect.ShadowProjection"));
    if (!SubsystemType || !ProjectionType) return false;
    TActorIterator<AActor> It(&World, SubsystemType);
    if (!It) return false;
    auto* Subsystem = *It;
    const auto* PlansProp = FindFProperty<FArrayProperty>(SubsystemType, TEXT("Plans"));
    const auto* RecordType = PlansProp ? CastField<FStructProperty>(PlansProp->Inner) : nullptr;
    const auto* Visibility = FindFProperty<FBoolProperty>(SubsystemType, TEXT("bVisible"));
    if (!RecordType || !Visibility) return false;
    FScriptArrayHelper Plans(PlansProp, PlansProp->ContainerPtrToValuePtr<void>(Subsystem));
    Visibility->SetPropertyValue_InContainer(Subsystem, true);
    for (const auto& Fixture : Fixtures)
        if (!AddGhost(Plans, *RecordType->Struct, Fixture)) return false;
    IFGSaveInterface::Execute_PostLoadGame(Subsystem, 0, 0);
    for (TActorIterator<AActor> View(&World, ProjectionType); View; ++View)
        for (auto& Fixture : Fixtures)
            if (View->GetActorTransform().Equals(Fixture.Anchor, 0.01)) Fixture.Ghost = *View;
    for (const auto& Fixture : Fixtures)
        if (!Fixture.Ghost.IsValid() || Fixture.Ghost->GetActorEnableCollision()) return false;
    return true;
}

void AlignmentKey(APlayerController& Player, const FKey& Key, EInputEvent Event)
{
    auto* Viewport = GEngine->GameViewport ? GEngine->GameViewport->Viewport : nullptr;
    Player.InputKey(FInputKeyEventArgs(Viewport, FInputDeviceId::CreateFromInternalId(0), Key, Event, FPlatformTime::Cycles64()));
}

TArray<FTransform> NearbyBuildings(UWorld& World, TSubclassOf<AActor> Type, const FVector& Center)
{
    TArray<FTransform> Result;
    if (!Type) return Result;
    auto Add = [&](const FTransform& Pose)
    {
        // A freshly built foundation can be present in the actor and lightweight lists during the handoff.
        if (FVector::DistSquared(Pose.GetLocation(), Center) < FMath::Square(1500.0)
            && !Result.ContainsByPredicate([&Pose](const auto& Other) { return Other.Equals(Pose, 0.1); }))
            Result.Add(Pose);
    };
    for (TActorIterator<AFGBuildable> It(&World, Type.Get()); It; ++It)
        Add(It->GetActorTransform());
    if (auto* Lightweights = AFGLightweightBuildableSubsystem::Get(&World))
        for (const auto& Entry : Lightweights->GetAllLightweightBuildableInstances())
            if (Entry.Key.Get() == Type.Get())
                for (const auto& Data : Entry.Value)
                    if (Data.IsValid()) Add(Data.Transform);
    return Result;
}

enum class ESurface { Native, Ghost, Probe };

struct FTrial
{
    const TCHAR* Name;
    int32 Fixture;
    ESurface Surface;
    FVector Aim;
    FVector Eye;
    bool bFoundationRecipe = false;
    bool bConfirm = false;
    int32 Edge = INDEX_NONE;
};

struct FAlignmentCheck
{
    static constexpr int32 NorthProbe = 3;
    static constexpr int32 PassageProbe = 4;

    FString Frames;
    bool bGuide = false;
    FAlignmentGuideCheck GuideCheck;
    TArray<FFixture> Fixtures;
    TArray<FTrial> Trials;
    TSubclassOf<UFGRecipe> Smelter;
    TWeakObjectPtr<AFGBuildable> DismantleTarget;
    TArray<FTransform> Before;
    TSubclassOf<AActor> ConstructClass;
    FVector Aim;
    int32 InitialAge = 0;
    int32 TrialIndex = 0;
    int32 Age = 0;
    int32 Observed = 0;
    bool bWalk = false;
    bool bConfirming = false;
    bool bTargetsMatched = true;

    bool Finish(bool bPass, const TCHAR* Detail) const
    {
        UE_LOG(LogSaveCheckAlignment, Display, TEXT("pass=%d %s measured=%d total=%d"), bPass, Detail,
            Observed, Trials.Num());
        FPlatformMisc::RequestExit(false);
        return false;
    }

    bool Setup(UWorld& World, AFGCharacterPlayer& Character)
    {
        Smelter = LoadRecipe(TEXT(""), TEXT("Recipe_SmelterMk1"));
        FFixture Foundation;
        if (!Smelter || !ReadFixture(Foundation, LoadRecipe(TEXT("Foundations/"), TEXT("Recipe_Foundation_8x1_01"))))
            return false;
        FFixture Flat;
        FFixture Slope;
        for (int32 Index = 1; Index <= 4; ++Index)
        {
            FFixture Roof;
            const FString Name = FString::Printf(TEXT("Recipe_Roof_A_%02d"), Index);
            if (!ReadFixture(Roof, LoadRecipe(TEXT("Roof/"), *Name))) return false;
            const FVector Size = Roof.MeshBounds.GetSize();
            UE_LOG(LogSaveCheckAlignment, Display, TEXT("roof_candidate=%s bounds_cm=%s"),
                *Roof.BuildClass->GetPathName(), *Size.ToString());
            if (!Flat.BuildClass || Size.Z < Flat.MeshBounds.GetSize().Z) Flat = Roof;
            if (!Slope.BuildClass || Size.Z > Slope.MeshBounds.GetSize().Z) Slope = MoveTemp(Roof);
        }
        Fixtures = { MoveTemp(Flat), Foundation, MoveTemp(Slope) };
        if (!bGuide)
        {
            Fixtures.Add(Foundation);
            Fixtures.Add(Foundation);
        }
        const FVector Origin = Character.GetActorLocation() + FVector(0, 0, bGuide ? 6200 : 2200);
        for (int32 Index = 0; Index < Fixtures.Num(); ++Index)
        {
            auto& Fixture = Fixtures[Index];
            Fixture.NativePose = FTransform(FRotator(0, 30, 0), Origin + FVector(Index * 2800, 0, 0));
            Fixture.GhostPose = FTransform(Fixture.NativePose.GetRotation(),
                Fixture.NativePose.GetLocation() + FVector(0, 2100, Index == NorthProbe ? 1700 : 0));
            Fixture.Anchor = FTransform(FRotator(0, 15, 0), Fixture.GhostPose.GetLocation() + FVector(120, 80, -55));
            if (Index < NorthProbe)
            {
                Fixture.Native = SpawnReal(World, Fixture, Fixture.NativePose);
                if (!Fixture.Native.IsValid()) return false;
            }
            UE_LOG(LogSaveCheckAlignment, Display,
                TEXT("fixture=%d class=%s mesh=%s bounds_center_cm=%s bounds_size_cm=%s native=%s ghost=%s"),
                Index, *Fixture.BuildClass->GetPathName(), *GetNameSafe(Fixture.Meshes[0].StaticMesh.Get()),
                *Fixture.MeshBounds.GetCenter().ToString(), *Fixture.MeshBounds.GetSize().ToString(),
                *Fixture.NativePose.GetLocation().ToString(), *Fixture.GhostPose.GetLocation().ToString());
        }
        if (!RestoreGhosts(World, Fixtures)) return false;
        // The no-cost flag isolates placement from the contents of the copied test slot.
        if (auto* State = World.GetGameState<AFGGameState>()) State->SetCheatNoCost(true);
        if (auto* Recipes = AFGRecipeManager::Get(&World))
        {
            Recipes->AddAvailableRecipe(Smelter);
            Recipes->AddAvailableRecipe(Fixtures[1].Recipe);
        }
        Character.GetCharacterMovement()->SetMovementMode(MOVE_Flying);
        if (!IFileManager::Get().MakeDirectory(*Frames, true)) return false;
        if (bGuide) return true;
        const double Top = Fixtures[0].MeshBounds.Max.Z;
        const FVector RoofAim(0, 0, Top);
        const FVector RoofEye(790, 600, Top + 580);
        const FVector EastAim(399, 0, 0);
        const FVector EastEye(1450, 0, 350);
        const FVector NorthAim(0, 399, 0);
        const FVector NorthEye(0, 1450, 350);
        Trials = {
            {TEXT("native-roof"), 0, ESurface::Native, RoofAim, RoofEye, false, true},
            {TEXT("ghost-roof"), 0, ESurface::Ghost, RoofAim, RoofEye},
            {TEXT("native-east"), 1, ESurface::Native, EastAim, EastEye, true, true, 0},
            {TEXT("ghost-east"), 1, ESurface::Ghost, EastAim, EastEye, true, false, 0},
            {TEXT("native-north"), 1, ESurface::Native, NorthAim, NorthEye, true, true, 1},
            {TEXT("ghost-north"), 1, ESurface::Ghost, NorthAim, NorthEye, true, false, 1},
            {TEXT("native-machine-control"), 1, ESurface::Native, FVector(0, 0, 50), FVector(820, 640, 650), false, true},
            {TEXT("native-slope-low"), 2, ESurface::Native, FVector(230, 0, 0), FVector(230, 800, 650)},
            {TEXT("native-slope-high"), 2, ESurface::Native, FVector(-230, 0, 0), FVector(-230, 800, 650)},
            {TEXT("probe-roof"), 0, ESurface::Probe, RoofAim, RoofEye, false, true},
            {TEXT("probe-roof-miss"), 0, ESurface::Probe, RoofEye + FVector(0, 0, 600), RoofEye},
            {TEXT("probe-east"), 1, ESurface::Probe, EastAim, EastEye, true, true, 0},
            {TEXT("probe-north"), NorthProbe, ESurface::Probe, NorthAim, NorthEye, true, true, 1},
            {TEXT("probe-foundation-miss"), 1, ESurface::Probe, NorthEye + FVector(0, 0, 650), NorthEye, true},
        };
        return true;
    }

    bool Observe(AFGBuildGun& Gun, const FTrial& Trial, const FFixture& Fixture, const FTransform& Base)
    {
        const auto* State = Cast<UFGBuildGunStateBuild>(Gun.GetCurrentState());
        const auto* Preview = State ? State->GetHologram() : nullptr;
        if (!Preview) return false;
        const FHitResult& Hit = Gun.GetHitResult();
        const FVector Pose = Preview->GetActorLocation();
        const FVector Local = Base.InverseTransformPosition(Pose);
        ConstructClass = Preview->GetBuildClass();
        const double Angle = FMath::FindDeltaAngleDegrees(Base.Rotator().Yaw, Preview->GetActorRotation().Yaw);
        const auto* Foundation = Cast<AFGBuildableFoundation>(Fixture.BuildClass.GetDefaultObject());
        const double Step = Foundation && Trial.Edge != INDEX_NONE ? Local[Trial.Edge] : 0;
        double EdgeGap = 0;
        if (Foundation && Trial.Edge != INDEX_NONE)
        {
            const double Along = Trial.Edge == 0 ? Foundation->mWidth : Foundation->mDepth;
            const double Across = Trial.Edge == 0 ? Foundation->mDepth : Foundation->mWidth;
            const double Radians = FMath::DegreesToRadians(Angle);
            const double CandidateExtent = FMath::Abs(FMath::Cos(Radians)) * Along + FMath::Abs(FMath::Sin(Radians)) * Across;
            EdgeGap = Step - (Along + CandidateExtent) * 0.5;
        }
        UE_LOG(LogSaveCheckAlignment, Display,
            TEXT("case=%s hit=%d target=%s target_class=%s impact_cm=%s normal=%s valid=%d visible=%d can_construct=%d pose_cm=%s local_cm=%s yaw=%.2f relative_yaw=%.2f step_cm=%.2f edge_gap_cm=%.2f"),
            Trial.Name, Hit.bBlockingHit, *GetNameSafe(Hit.GetActor()), *GetNameSafe(Hit.GetActor() ? Hit.GetActor()->GetClass() : nullptr),
            *Hit.ImpactPoint.ToString(), *Hit.ImpactNormal.ToString(), Preview->IsValidHitResult(Hit),
            !Preview->IsHidden(), Preview->CanConstruct(), *Pose.ToString(), *Local.ToString(),
            Preview->GetActorRotation().Yaw, Angle, Step, EdgeGap);
        TArray<TSubclassOf<UFGConstructDisqualifier>> Reasons;
        Preview->GetConstructDisqualifiers(Reasons);
        for (const auto& Reason : Reasons)
            UE_LOG(LogSaveCheckAlignment, Display, TEXT("case=%s disqualifier=%s"), Trial.Name, *GetNameSafe(Reason.Get()));
        if (Trial.Surface == ESurface::Probe) LogAlignmentSurface(Trial.Name, Gun, *Fixture.Ghost.Get());
        ++Observed;
        return true;
    }

    bool Tick(UWorld& World, APlayerController& Player, AFGCharacterPlayer& Character)
    {
        auto* Gun = Character.GetBuildGun();
        if (!Gun) return true;
        if (InitialAge < 12 && ++InitialAge < 12) return true;
        if (Fixtures.IsEmpty() && !Setup(World, Character)) return Finish(false, TEXT("fixture setup"));
        if (bGuide)
        {
            TArray<FAlignmentGuideFixture, TInlineAllocator<3>> GuideFixtures;
            for (const auto& Fixture : Fixtures)
                GuideFixtures.Add({Fixture.Recipe, Fixture.NativePose, Fixture.GhostPose,
                    Fixture.Native.Get(), Fixture.Ghost.Get(), Fixture.MeshBounds});
            const FFixture& Foundation = Fixtures[1];
            const auto SpawnFoundation = [&World, &Foundation](const FTransform& Pose) -> AActor*
            {
                return SpawnReal(World, Foundation, Pose);
            };
            const auto ReadFoundations = [&World, &Foundation](const FVector& Center)
            {
                return NearbyBuildings(World, Foundation.BuildClass.Get(), Center);
            };
            return GuideCheck.Tick(World, Player, Character, Smelter, Frames, GuideFixtures,
                SpawnFoundation, ReadFoundations);
        }
        if (TrialIndex == Trials.Num())
        {
            auto& Foundation = Fixtures[PassageProbe];
            const auto& Pose = Foundation.GhostPose;
            if (++Age == 1)
            {
                const FVector Start = Pose.TransformPosition(FVector(-600, 0, 0));
                const FVector End = Pose.TransformPosition(FVector(600, 0, 0));
                ConfigureAlignmentSurface(*Foundation.Ghost.Get(), true);
                bWalk = CheckAlignmentPassage(Character, Start, End);
                ConfigureAlignmentSurface(*Foundation.Ghost.Get(), false);
                const FTransform Behind(Pose.GetRotation(), Pose.TransformPosition(FVector(1400, 0, 0)));
                DismantleTarget = SpawnReal(World, Foundation, Behind);
                if (!DismantleTarget.IsValid()) return Finish(false, TEXT("dismantle fixture"));
                Character.UnequipBuildGun();
                Character.TeleportTo(Start - FVector(0, 0, Character.BaseEyeHeight), FRotator::ZeroRotator, false, true);
                Aim = DismantleTarget->GetActorLocation();
                AlignmentKey(Player, EKeys::F, IE_Pressed);
            }
            Player.SetControlRotation((Aim - Character.GetPawnViewLocation()).Rotation());
            if (Age == 3) AlignmentKey(Player, EKeys::F, IE_Released);
            if (Age == 10)
            {
                const bool bDismantle = CheckAlignmentDismantle(*Gun, *DismantleTarget.Get());
                return Finish(bWalk && bDismantle && bTargetsMatched, TEXT("completed"));
            }
            return true;
        }
        const auto& Trial = Trials[TrialIndex];
        const auto& Fixture = Fixtures[Trial.Fixture];
        const FTransform& Base = Trial.Surface == ESurface::Native ? Fixture.NativePose : Fixture.GhostPose;
        const AActor* Target = Trial.Surface == ESurface::Native
            ? static_cast<AActor*>(Fixture.Native.Get()) : Fixture.Ghost.Get();
        ++Age;
        if (Age == 1)
        {
            if (Trial.Surface == ESurface::Probe) ConfigureAlignmentSurface(*Fixture.Ghost.Get(), true);
            Character.UnequipBuildGun();
            Character.GetCharacterMovement()->StopMovementImmediately();
            Character.TeleportTo(Base.TransformPosition(Trial.Eye) - FVector(0, 0, Character.BaseEyeHeight),
                FRotator::ZeroRotator, false, true);
            Player.SetViewTarget(&Character);
            Aim = Base.TransformPosition(Trial.Aim);
        }
        Player.SetControlRotation((Aim - Character.GetPawnViewLocation()).Rotation());
        if (Age == 2) Character.HotKeyRecipe(Trial.bFoundationRecipe ? Fixtures[1].Recipe : Smelter);
        if (Age == 8)
        {
            if (!Observe(*Gun, Trial, Fixture, Base)) return Finish(false, TEXT("missing hologram"));
            FScreenshotRequest::RequestScreenshot(FPaths::Combine(Frames, FString(Trial.Name) + TEXT(".png")), false, false);
        }
        if (Trial.bConfirm && Age == 10)
        {
            bConfirming = Gun->GetHitResult().GetActor() == Target;
            if (bConfirming)
            {
                Before = NearbyBuildings(World, ConstructClass, Base.GetLocation());
                AlignmentKey(Player, EKeys::LeftMouseButton, IE_Pressed);
            }
            else
            {
                bTargetsMatched = false;
                UE_LOG(LogSaveCheckAlignment, Warning, TEXT("case=%s confirmation_skipped=fixture_not_hit"), Trial.Name);
            }
        }
        if (bConfirming && Age == 12) AlignmentKey(Player, EKeys::LeftMouseButton, IE_Released);
        if (Age < (Trial.bConfirm ? 22 : 12)) return true;
        if (bConfirming)
        {
            const auto After = NearbyBuildings(World, ConstructClass, Base.GetLocation());
            UE_LOG(LogSaveCheckAlignment, Display, TEXT("case=%s input=LMB new_buildings=%d"), Trial.Name,
                After.Num() - Before.Num());
            for (const auto& Pose : After)
                if (!Before.ContainsByPredicate([&Pose](const auto& Old) { return Old.Equals(Pose, 0.1); }))
                    UE_LOG(LogSaveCheckAlignment, Display, TEXT("case=%s built=%s"), Trial.Name, *Pose.ToString());
        }
        if (Trial.Surface == ESurface::Probe) ConfigureAlignmentSurface(*Fixture.Ghost.Get(), false);
        ++TrialIndex;
        Age = 0;
        bConfirming = false;
        return true;
    }
};
}

FTickerDelegate MakeAlignmentCheck(FString Frames, bool bGuide)
{
    return FTickerDelegate::CreateLambda([Check = FAlignmentCheck{ MoveTemp(Frames), bGuide }](float) mutable
    {
        if (!GEngine) return true;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            UWorld* World = Context.World();
            if (!World || !World->IsGameWorld() || !World->HasBegunPlay()
                || !World->GetMapName().Contains(TEXT("Persistent_Level"))) continue;
            auto* Save = UFGSaveSession::Get(World);
            auto* Player = World->GetFirstPlayerController();
            auto* Character = Player ? Cast<AFGCharacterPlayer>(Player->GetPawn()) : nullptr;
            if (!Save || !Character) continue;
            if (Check.InitialAge == 0)
            {
                Save->SetAutoSaveEnabled(false);
                const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("ShadowArchitect"));
                UE_LOG(LogSaveCheckAlignment, Display, TEXT("version=%s"), Plugin ? *Plugin->GetDescriptor().VersionName : TEXT("missing"));
            }
            return Check.Tick(*World, *Player, *Character);
        }
        return true;
    });
}
