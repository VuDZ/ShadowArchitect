#include "Buildables/FGBuildable.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Equipment/FGBuildGunBuild.h"
#include "FGBlueprintProxy.h"
#include "FGBlueprintSubsystem.h"
#include "FGCharacterPlayer.h"
#include "FGConstructDisqualifier.h"
#include "FGGameState.h"
#include "FGLightweightBuildableSubsystem.h"
#include "FGSaveSession.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Hologram/FGBlueprintHologram.h"
#include "HAL/FileManager.h"
#include "InputKeyEventArgs.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogSaveCheckRoofStack, Log, All);

bool ClickPlanMenuButton(FStringView Label);

namespace
{
constexpr TCHAR BlueprintName[] = TEXT("3х3 пустой");
constexpr TCHAR RoofClass[] = TEXT("/Game/FactoryGame/Buildable/Building/Foundation/ConcreteSet/Build_Foundation_Concrete_8x1.Build_Foundation_Concrete_8x1_C");

struct FRoofStackPart
{
    FString SourceClass;
    FTransform Relative;
    FBox Bounds;
};

struct FRoofStackPlan
{
    FGuid Id;
    FTransform Anchor;
    FString Name;
    FString SourceProof;
    TArray<FRoofStackPart> Parts;

    TOptional<FVector> RoofPoint() const
    {
        const FRoofStackPart* Closest = nullptr;
        for (const auto& Part : Parts)
            if (Part.SourceClass == RoofClass && (!Closest
                || Part.Relative.GetLocation().SizeSquared2D() < Closest->Relative.GetLocation().SizeSquared2D()))
                Closest = &Part;
        if (!Closest) return {};
        return Anchor.TransformPosition(Closest->Relative.GetLocation() + FVector(0, 0, 50));
    }
};

bool ReadRoofStackPlans(UWorld& World, TArray<FRoofStackPlan>& Out)
{
    auto* Class = FindObject<UClass>(nullptr, TEXT("/Script/ShadowArchitect.ShadowPlanSubsystem"));
    if (!Class) return false;
    TActorIterator<AActor> It(&World, Class);
    if (!It) return false;
    const auto* Plans = FindFProperty<FArrayProperty>(Class, TEXT("Plans"));
    const auto* Record = Plans ? CastField<FStructProperty>(Plans->Inner) : nullptr;
    if (!Record) return false;
    const auto* Id = FindFProperty<FStructProperty>(Record->Struct, TEXT("Id"));
    const auto* Anchor = FindFProperty<FStructProperty>(Record->Struct, TEXT("Transform"));
    const auto* Name = FindFProperty<FStrProperty>(Record->Struct, TEXT("BlueprintId"));
    const auto* Proof = FindFProperty<FStrProperty>(Record->Struct, TEXT("BlueprintSourceProof"));
    const auto* Parts = FindFProperty<FArrayProperty>(Record->Struct, TEXT("Parts"));
    const auto* Part = Parts ? CastField<FStructProperty>(Parts->Inner) : nullptr;
    const auto* Relative = Part ? FindFProperty<FStructProperty>(Part->Struct, TEXT("InPlan")) : nullptr;
    const auto* Source = Part ? FindFProperty<FStrProperty>(Part->Struct, TEXT("SourceClass")) : nullptr;
    const auto* Center = Part ? FindFProperty<FStructProperty>(Part->Struct, TEXT("BoundsCenter")) : nullptr;
    const auto* Size = Part ? FindFProperty<FStructProperty>(Part->Struct, TEXT("BoundsSize")) : nullptr;
    if (!Id || !Anchor || !Name || !Proof || !Relative || !Source || !Center || !Size) return false;
    Out.Reset();
    FScriptArrayHelper Records(Plans, Plans->ContainerPtrToValuePtr<void>(*It));
    for (int32 Index = 0; Index < Records.Num(); ++Index)
    {
        const auto* Entry = Records.GetRawPtr(Index);
        auto& Plan = Out.AddDefaulted_GetRef();
        Plan.Id = *Id->ContainerPtrToValuePtr<FGuid>(Entry);
        Plan.Anchor = *Anchor->ContainerPtrToValuePtr<FTransform>(Entry);
        Plan.Name = Name->GetPropertyValue_InContainer(Entry);
        Plan.SourceProof = Proof->GetPropertyValue_InContainer(Entry);
        FScriptArrayHelper PartRecords(Parts, Parts->ContainerPtrToValuePtr<void>(Entry));
        for (int32 PartIndex = 0; PartIndex < PartRecords.Num(); ++PartIndex)
        {
            const auto* PartRecord = PartRecords.GetRawPtr(PartIndex);
            const FVector Mid = *Center->ContainerPtrToValuePtr<FVector>(PartRecord);
            const FVector Extent = *Size->ContainerPtrToValuePtr<FVector>(PartRecord) * 0.5;
            Plan.Parts.Add({ Source->GetPropertyValue_InContainer(PartRecord),
                *Relative->ContainerPtrToValuePtr<FTransform>(PartRecord), FBox(Mid - Extent, Mid + Extent) });
        }
    }
    return true;
}

bool SameRoofStackParts(const FRoofStackPlan& Lower, const FRoofStackPlan& Upper)
{
    auto Remaining = Lower.Parts;
    for (const auto& Part : Upper.Parts)
    {
        const int32 Index = Remaining.IndexOfByPredicate([&Part](const auto& Other)
        {
            return Part.SourceClass == Other.SourceClass && Part.Relative.Equals(Other.Relative, 0.1);
        });
        if (Index == INDEX_NONE) return false;
        Remaining.RemoveAtSwap(Index);
    }
    return Remaining.IsEmpty();
}

int32 CountRoofStackContacts(const FRoofStackPlan& Lower, const FRoofStackPlan& Upper)
{
    const auto Roof = Lower.RoofPoint();
    if (!Roof.IsSet()) return 0;
    const double SurfaceZ = Roof.GetValue().Z;
    TSet<FIntPoint> Contacts;
    for (const auto& Part : Upper.Parts)
    {
        if (!Part.SourceClass.EndsWith(TEXT(".Build_Pillar_Small_Concrete_C"))) continue;
        const FTransform Frame = Part.Relative * Upper.Anchor;
        const double Gap = Part.Bounds.TransformBy(Frame).Min.Z - SurfaceZ;
        if (Gap > 200) continue;
        const FVector InLower = Lower.Anchor.InverseTransformPosition(Frame.GetLocation());
        const bool bOnSlab = Lower.Parts.ContainsByPredicate([&](const auto& Slab)
        {
            const FVector Delta = Slab.Relative.GetLocation() - InLower;
            const double SlabZ = Lower.Anchor.TransformPosition(Slab.Relative.GetLocation() + FVector(0, 0, 50)).Z;
            return Slab.SourceClass == RoofClass && FMath::Abs(SlabZ - SurfaceZ) <= 1.0
                && FMath::Abs(Delta.X) <= 401 && FMath::Abs(Delta.Y) <= 401;
        });
        if (bOnSlab && FMath::Abs(Gap) <= 1.0)
            Contacts.Add(FIntPoint(FMath::RoundToInt(InLower.X), FMath::RoundToInt(InLower.Y)));
        UE_LOG(LogSaveCheckRoofStack, Display, TEXT("pillar_local_xy=(%.2f,%.2f) gap_cm=%.2f supported=%d"),
            InLower.X, InLower.Y, Gap, bOnSlab);
    }
    return Contacts.Num();
}

TSet<TWeakObjectPtr<AFGBlueprintProxy>> RoofStackProxies(UWorld& World)
{
    TSet<TWeakObjectPtr<AFGBlueprintProxy>> Result;
    for (TActorIterator<AFGBlueprintProxy> It(&World); It; ++It) Result.Add(*It);
    return Result;
}

int32 RoofStackBuildingCount(UWorld& World)
{
    int32 Count = 0;
    for (TActorIterator<AFGBuildable> It(&World); It; ++It) ++Count;
    if (const auto* Lightweights = AFGLightweightBuildableSubsystem::Get(&World))
        for (const auto& Entry : Lightweights->GetAllLightweightBuildableInstances())
            for (const auto& Item : Entry.Value)
                if (Item.IsValid()) ++Count;
    return Count;
}

bool SameRoofStackProxies(UWorld& World, const TSet<TWeakObjectPtr<AFGBlueprintProxy>>& Before)
{
    const auto After = RoofStackProxies(World);
    if (Before.Num() != After.Num()) return false;
    for (const auto& Proxy : Before)
        if (!After.Contains(Proxy)) return false;
    return true;
}

void RoofStackKey(APlayerController& Player, FKey Key, EInputEvent Event)
{
    auto* Viewport = GEngine->GameViewport ? GEngine->GameViewport->Viewport : nullptr;
    Player.InputKey(FInputKeyEventArgs(Viewport, FInputDeviceId::CreateFromInternalId(0), Key, Event,
        FPlatformTime::Cycles64()));
}

AFGBlueprintHologram* RoofStackPreview(AFGBuildGun& Gun)
{
    const auto* Build = Gun.IsEquipped() ? Cast<UFGBuildGunStateBuild>(Gun.GetCurrentState()) : nullptr;
    return Build ? Cast<AFGBlueprintHologram>(Build->GetHologram()) : nullptr;
}

bool RoofStackHasView(UWorld& World, const FRoofStackPlan& Plan)
{
    auto* Type = FindObject<UClass>(nullptr, TEXT("/Script/ShadowArchitect.ShadowProjection"));
    if (!Type) return false;
    for (TActorIterator<AActor> It(&World, Type); It; ++It)
        if (!It->IsHidden() && !It->GetActorEnableCollision() && It->GetActorTransform().Equals(Plan.Anchor, 0.1))
            return true;
    return false;
}

struct FRoofStackCheck
{
    enum class EStep { Ready, Capture, Menu, Stack, Preview, Confirm, Done };
    enum class EBuild { UpperOnGhost, Lower, UpperOnReal };

    FString Directory;
    EStep Step = EStep::Ready;
    EBuild Build = EBuild::UpperOnGhost;
    int32 Age = 0;
    int32 InitialCount = 0;
    int32 RealBeforeStack = 0;
    bool bLowerPrepared = false;
    FTransform LowerFixture;
    FVector Aim = FVector::ZeroVector;
    FRoofStackPlan Lower;
    FRoofStackPlan Upper;
    TSet<TWeakObjectPtr<AFGBlueprintProxy>> Proxies;

    void Next(EStep NewStep)
    {
        Step = NewStep;
        Age = 0;
    }

    bool End(bool bPass, const TCHAR* Reason) const
    {
        UE_LOG(LogSaveCheckRoofStack, Display, TEXT("pass=%d stage=%d %s"), bPass, static_cast<int32>(Step), Reason);
        FPlatformMisc::RequestExit(false);
        return false;
    }

    void MoveCamera(APlayerController& Player, AFGCharacterPlayer& Character, const FVector& Target, const FVector& Offset)
    {
        Character.GetCharacterMovement()->StopMovementImmediately();
        Character.TeleportTo(Target + Offset - FVector(0, 0, Character.BaseEyeHeight), FRotator::ZeroRotator, false, true);
        Player.SetViewTarget(&Character);
        Aim = Target;
        Look(Player, Character);
    }

    void Look(APlayerController& Player, AFGCharacterPlayer& Character) const
    {
        Player.SetControlRotation((Aim - Character.GetPawnViewLocation()).Rotation());
    }

    void LookAtBothFloors(APlayerController& Player, AFGCharacterPlayer& Character)
    {
        MoveCamera(Player, Character, Lower.Anchor.GetLocation() + FVector(0, 0, 900), FVector(4600, -3900, 1850));
    }

    void Frame(const TCHAR* Name) const
    {
        FScreenshotRequest::RequestScreenshot(FPaths::Combine(Directory, FString(Name) + TEXT(".png")), true, false);
    }

    bool OpenMenu(APlayerController& Player, const TCHAR* Button) const
    {
        if (Age == 4) RoofStackKey(Player, EKeys::F6, IE_Pressed);
        if (Age == 5) RoofStackKey(Player, EKeys::F6, IE_Released);
        return Age > 5 && ClickPlanMenuButton(Button);
    }

    const TCHAR* BuildCase() const
    {
        if (Build == EBuild::Lower) return TEXT("build-lower");
        return Build == EBuild::UpperOnGhost ? TEXT("build-upper-on-ghost") : TEXT("build-upper-on-real");
    }

    bool CheckBuilt(UWorld& World, const FRoofStackPlan& Target, bool& bBuilt) const
    {
        int32 NewGroups = 0;
        int32 ActualParts = 0;
        auto Remaining = Target.Parts;
        auto Match = [&](const UClass* Class, const FTransform& Pose)
        {
            ++ActualParts;
            const int32 Index = Remaining.IndexOfByPredicate([&](const auto& Part)
            {
                return Class && Part.SourceClass == Class->GetPathName()
                    && (Part.Relative * Target.Anchor).GetLocation().Equals(Pose.GetLocation(), 1.0);
            });
            if (Index != INDEX_NONE) Remaining.RemoveAtSwap(Index);
        };
        const auto* Lightweights = AFGLightweightBuildableSubsystem::Get(&World);
        for (TActorIterator<AFGBlueprintProxy> It(&World); It; ++It)
        {
            if (Proxies.Contains(*It)) continue;
            ++NewGroups;
            for (const auto* Actor : It->GetBuildables())
                if (IsValid(Actor)) Match(Actor->GetClass(), Actor->GetActorTransform());
            for (const auto& Entry : It->GetLightweightClassAndIndices())
            {
                const auto* Instances = Lightweights
                    ? Lightweights->GetAllLightweightBuildableInstances().Find(Entry.BuildableClass) : nullptr;
                for (int32 Index : Entry.Indices)
                    if (Instances && Instances->IsValidIndex(Index))
                        Match(Entry.BuildableClass.Get(), (*Instances)[Index].Transform);
            }
        }
        const int32 Matched = Target.Parts.Num() - Remaining.Num();
        UE_LOG(LogSaveCheckRoofStack, Display, TEXT("case=%s input=LMB new_groups=%d new_parts=%d matched_positions=%d/%d"),
            BuildCase(), NewGroups, ActualParts, Matched, Target.Parts.Num());
        bBuilt = NewGroups == 1 && ActualParts == Target.Parts.Num() && Remaining.IsEmpty();
        return bBuilt || (NewGroups == 0 && ActualParts == 0);
    }

    bool Tick(UWorld& World, APlayerController& Player, AFGCharacterPlayer& Character)
    {
        auto* Gun = Character.GetBuildGun();
        if (!Gun) return true;
        if (++Age > 75) return End(false, TEXT("timeout"));
        TArray<FRoofStackPlan> Plans;
        switch (Step)
        {
        case EStep::Ready:
        {
            if (Age < 12 || !ReadRoofStackPlans(World, Plans)) break;
            TArray<UFGBlueprintDescriptor*> Descriptors;
            AFGBlueprintSubsystem::GetBlueprintDescriptors(Descriptors, &World);
            if (Descriptors.Num() != 1 || !Descriptors[0] || Descriptors[0]->GetBlueprintNameAsString() != BlueprintName)
                return End(false, TEXT("expected isolated 3x3 Blueprint library"));
            InitialCount = Plans.Num();
            Proxies = RoofStackProxies(World);
            const FVector Start = Character.GetActorLocation();
            LowerFixture = FTransform(FRotator(0, 30, 0), Start + FVector(0, 0, 6200));
            Character.GetCharacterMovement()->SetMovementMode(MOVE_Flying);
            MoveCamera(Player, Character, Start - FVector(0, 0, 100), FVector(0, -1700, 4800));
            Character.HotKeyBlueprint(BlueprintName);
            if (auto* State = World.GetGameState<AFGGameState>()) State->SetCheatNoCost(true);
            if (!IFileManager::Get().MakeDirectory(*Directory, true)) return End(false, TEXT("cannot create frames directory"));
            const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("ShadowArchitect"));
            UE_LOG(LogSaveCheckRoofStack, Display, TEXT("version=%s blueprint=%s before_plans=%d before_real_blueprints=%d"),
                Plugin ? *Plugin->GetDescriptor().VersionName : TEXT("missing"), BlueprintName, InitialCount, Proxies.Num());
            Next(EStep::Capture);
            break;
        }
        case EStep::Capture:
        {
            Look(Player, Character);
            auto* Preview = RoofStackPreview(*Gun);
            if (!Preview || Preview->mBuildableToNewRoot.IsEmpty()) break;
            if (Age < 9) break;
            if (!bLowerPrepared)
            {
                bLowerPrepared = true;
                Age = 9;
                Preview->SetActorTransform(LowerFixture);
                Preview->LockHologramPosition(true);
                Preview->SetActorHiddenInGame(false);
                UE_LOG(LogSaveCheckRoofStack, Display, TEXT("lower_fixture=%s preview=%s roots=%d locked=%d"),
                    *LowerFixture.ToString(), *Preview->GetActorTransform().ToString(),
                    Preview->mBuildableToNewRoot.Num(), Preview->IsHologramLocked());
            }
            if (Age == 10) RoofStackKey(Player, EKeys::F7, IE_Pressed);
            if (Age == 11) RoofStackKey(Player, EKeys::F7, IE_Released);
            if (Age < 14 || !ReadRoofStackPlans(World, Plans) || Plans.Num() == InitialCount) break;
            if (Plans.Num() != InitialCount + 1) return End(false, TEXT("lower placement count"));
            Lower = MoveTemp(Plans.Last());
            const auto Target = Lower.RoofPoint();
            if (!Target.IsSet() || Lower.Name != BlueprintName || Lower.SourceProof.IsEmpty()
                || !Lower.Anchor.Equals(LowerFixture, 0.1) || !RoofStackHasView(World, Lower))
                return End(false, TEXT("lower plan mismatch"));
            RealBeforeStack = RoofStackBuildingCount(World);
            MoveCamera(Player, Character, Target.GetValue(), FVector(1650, -1050, 1750));
            Next(EStep::Menu);
            break;
        }
        case EStep::Menu:
            Look(Player, Character);
            if (OpenMenu(Player, TEXT("Place Blueprint on roof")))
            {
                UE_LOG(LogSaveCheckRoofStack, Display, TEXT("input=F6 action=Place Blueprint on roof lower=%s aim_cm=%s"),
                    *Lower.Id.ToString(), *Aim.ToString());
                Next(EStep::Stack);
            }
            break;
        case EStep::Stack:
        {
            if (Age == 2)
            {
                if (!ReadRoofStackPlans(World, Plans) || Plans.Num() != InitialCount + 2)
                    return End(false, TEXT("roof command did not add exactly one plan"));
                Upper = MoveTemp(Plans.Last());
                const FVector Difference = Upper.Anchor.GetLocation() - Lower.Anchor.GetLocation();
                const double Yaw = FMath::FindDeltaAngleDegrees(Lower.Anchor.Rotator().Yaw, Upper.Anchor.Rotator().Yaw);
                const auto& Original = Plans[InitialCount];
                const bool bSameParts = SameRoofStackParts(Lower, Upper);
                const int32 RealAfterStack = RoofStackBuildingCount(World);
                const bool bNoRealChange = SameRoofStackProxies(World, Proxies) && RealBeforeStack == RealAfterStack;
                const int32 Contacts = CountRoofStackContacts(Lower, Upper);
                UE_LOG(LogSaveCheckRoofStack, Display,
                    TEXT("lower=%s anchor=%s upper=%s anchor=%s delta_cm=%s delta_yaw=%.2f parts=%d/%d same_parts=%d contacts=%d real_buildings=%d->%d real_unchanged=%d"),
                    *Lower.Id.ToString(), *Lower.Anchor.ToString(), *Upper.Id.ToString(), *Upper.Anchor.ToString(),
                    *Difference.ToString(), Yaw, Lower.Parts.Num(), Upper.Parts.Num(), bSameParts, Contacts,
                    RealBeforeStack, RealAfterStack, bNoRealChange);
                if (!Upper.Id.IsValid() || Upper.Id == Lower.Id || Upper.SourceProof != Lower.SourceProof || Upper.Name != Lower.Name
                    || Original.Id != Lower.Id || !Original.Anchor.Equals(Lower.Anchor, 0.1)
                    || !Difference.Equals(FVector(0, 0, 900), 1.0) || FMath::Abs(Yaw) > 0.01
                    || !bSameParts || Contacts != 4 || !bNoRealChange
                    || !RoofStackHasView(World, Lower) || !RoofStackHasView(World, Upper))
                    return End(false, TEXT("stack mismatch"));
                Character.UnequipBuildGun();
                LookAtBothFloors(Player, Character);
            }
            if (Age > 2) Look(Player, Character);
            if (Age == 6) Frame(TEXT("stacked-ghosts"));
            if (Age == 9) Next(EStep::Preview);
            break;
        }
        case EStep::Preview:
        {
            const FRoofStackPlan& Target = Build == EBuild::Lower ? Lower : Upper;
            if (Age == 1)
            {
                const auto Roof = Target.RoofPoint();
                if (!Roof.IsSet()) return End(false, TEXT("build target missing roof"));
                MoveCamera(Player, Character, Roof.GetValue(), FVector(2650, -1800, 1450));
            }
            Look(Player, Character);
            if (OpenMenu(Player, TEXT("Open build preview"))) Next(EStep::Confirm);
            break;
        }
        case EStep::Confirm:
        {
            Look(Player, Character);
            const FRoofStackPlan& Target = Build == EBuild::Lower ? Lower : Upper;
            if (Age == 7)
            {
                auto* Preview = RoofStackPreview(*Gun);
                if (!Preview || !Preview->IsHologramLocked() || !Preview->GetActorTransform().Equals(Target.Anchor, 0.1))
                    return End(false, TEXT("stock preview did not open at selected plan pose"));
                TArray<TSubclassOf<UFGConstructDisqualifier>> Reasons;
                Preview->GetConstructDisqualifiers(Reasons);
                UE_LOG(LogSaveCheckRoofStack, Display, TEXT("case=%s input=F6 action=Open build preview id=%s can_construct=%d pose=%s"),
                    BuildCase(), *Target.Id.ToString(), Preview->CanConstruct(), *Preview->GetActorTransform().ToString());
                for (const auto& Item : Reasons)
                    UE_LOG(LogSaveCheckRoofStack, Display, TEXT("case=%s disqualifier=%s"), BuildCase(), *GetNameSafe(Item.Get()));
                Proxies = RoofStackProxies(World);
                Frame(BuildCase());
            }
            if (Age == 10) RoofStackKey(Player, EKeys::LeftMouseButton, IE_Pressed);
            if (Age == 11) RoofStackKey(Player, EKeys::LeftMouseButton, IE_Released);
            if (Age < 32) break;
            bool bBuilt = false;
            if (!CheckBuilt(World, Target, bBuilt)) return End(false, TEXT("unexpected real Blueprint construction"));
            if (!ReadRoofStackPlans(World, Plans) || Plans.Num() != InitialCount + 2
                || Plans[InitialCount].Id != Lower.Id || Plans.Last().Id != Upper.Id
                || !Plans[InitialCount].Anchor.Equals(Lower.Anchor, 0.1) || !Plans.Last().Anchor.Equals(Upper.Anchor, 0.1))
                return End(false, TEXT("confirmation changed saved plans"));
            if (bBuilt && Build != EBuild::Lower)
            {
                LookAtBothFloors(Player, Character);
                Next(EStep::Done);
                break;
            }
            if (Build == EBuild::UpperOnGhost) Build = EBuild::Lower;
            else if (Build == EBuild::Lower && bBuilt) Build = EBuild::UpperOnReal;
            else return End(true, TEXT("completed stacked ghosts; stock game rejected real construction"));
            Character.UnequipBuildGun();
            Next(EStep::Preview);
            break;
        }
        case EStep::Done:
            Look(Player, Character);
            if (Age == 5) Frame(TEXT("built-upper"));
            if (Age > 8) return End(true, TEXT("completed stacked ghosts and one real upper Blueprint"));
            break;
        }
        return true;
    }
};
}

FTickerDelegate MakeRoofStackCheck(FString Directory)
{
    return FTickerDelegate::CreateLambda([Check = FRoofStackCheck{ MoveTemp(Directory) }](float) mutable
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
