#include "AlignmentGuideCheck.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Equipment/FGBuildGunBuild.h"
#include "FGCharacterPlayer.h"
#include "FGRecipe.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Hologram/FGHologram.h"
#include "InputKeyEventArgs.h"
#include "Layout/Children.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "UObject/UnrealType.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY_STATIC(LogSaveCheckAlignmentGuide, Log, All);

bool ClickPlanMenuButton(FStringView Label);

namespace
{
constexpr double RoofFixtureLift = 500.0;

AActor* FindGuideSubsystem(UWorld& World)
{
    auto* Class = FindObject<UClass>(nullptr, TEXT("/Script/ShadowArchitect.ShadowPlanSubsystem"));
    if (!Class) return nullptr;
    TActorIterator<AActor> It(&World, Class);
    return It ? *It : nullptr;
}

struct FMeasuredGuideLine
{
    FVector Start;
    FVector End;
};

TArray<FMeasuredGuideLine> ReadGuideLines(AActor& Subsystem)
{
    const auto* Geometry = FindFProperty<FArrayProperty>(Subsystem.GetClass(), TEXT("AlignmentLines"));
    const auto* Segment = Geometry ? CastField<FStructProperty>(Geometry->Inner) : nullptr;
    const auto* Start = Segment ? FindFProperty<FStructProperty>(Segment->Struct, TEXT("Start")) : nullptr;
    const auto* End = Segment ? FindFProperty<FStructProperty>(Segment->Struct, TEXT("End")) : nullptr;
    TArray<FMeasuredGuideLine> Lines;
    if (!Start || !End) return Lines;

    FScriptArrayHelper Entries(Geometry, Geometry->ContainerPtrToValuePtr<void>(&Subsystem));
    for (int32 Index = 0; Index < Entries.Num(); ++Index)
    {
        const void* Entry = Entries.GetRawPtr(Index);
        Lines.Add({ *Start->ContainerPtrToValuePtr<FVector>(Entry), *End->ContainerPtrToValuePtr<FVector>(Entry) });
    }
    return Lines;
}

bool RaiseGuideFixture(AActor& Subsystem, AActor& Projection)
{
    const auto* Plans = FindFProperty<FArrayProperty>(Subsystem.GetClass(), TEXT("Plans"));
    const auto* Record = Plans ? CastField<FStructProperty>(Plans->Inner) : nullptr;
    const auto* Transform = Record ? FindFProperty<FStructProperty>(Record->Struct, TEXT("Transform")) : nullptr;
    if (!Transform) return false;
    FScriptArrayHelper Entries(Plans, Plans->ContainerPtrToValuePtr<void>(&Subsystem));
    for (int32 Index = 0; Index < Entries.Num(); ++Index)
    {
        auto& Pose = *Transform->ContainerPtrToValuePtr<FTransform>(Entries.GetRawPtr(Index));
        if (!Pose.Equals(Projection.GetActorTransform(), 0.01)) continue;
        Pose.AddToTranslation(FVector(0, 0, RoofFixtureLift));
        return Projection.SetActorTransform(Pose);
    }
    return false;
}

void SendGuideKey(APlayerController& Player, const FKey& Key, EInputEvent Event)
{
    auto* Viewport = GEngine->GameViewport ? GEngine->GameViewport->Viewport : nullptr;
    Player.InputKey(FInputKeyEventArgs(Viewport, FInputDeviceId::CreateFromInternalId(0), Key, Event,
        FPlatformTime::Cycles64()));
}

bool ContainsGuideLabel(SWidget& Widget, const TCHAR* Text)
{
    if (!Widget.GetVisibility().IsVisible()) return false;
    if (Widget.GetType() == TEXT("STextBlock") && static_cast<STextBlock&>(Widget).GetText().ToString().Contains(Text))
        return true;
    if (FChildren* Children = Widget.GetChildren())
        for (int32 Index = 0; Index < Children->Num(); ++Index)
            if (ContainsGuideLabel(Children->GetChildAt(Index).Get(), Text)) return true;
    return false;
}

bool GuideLabelVisible(const TCHAR* Text)
{
    const auto Viewport = GEngine->GameViewport ? GEngine->GameViewport->GetGameViewportWidget() : nullptr;
    const auto Window = Viewport && FSlateApplication::IsInitialized()
        ? FSlateApplication::Get().FindWidgetWindow(Viewport.ToSharedRef()) : nullptr;
    return Window && ContainsGuideLabel(Window.ToSharedRef().Get(), Text);
}

void GuideFrame(const FString& Directory, const TCHAR* Name)
{
    FScreenshotRequest::RequestScreenshot(FPaths::Combine(Directory, FString(Name) + TEXT(".png")), true, false);
}

FVector GuideLineMidpoint(const FMeasuredGuideLine& Line)
{
    return (Line.Start + Line.End) * 0.5;
}

bool ReadFoundationGuide(const TArray<FMeasuredGuideLine>& Lines, const FTransform& Base, int32 Side,
    const TCHAR* Name, const FVector& NativePose, double NativeYaw)
{
    if (Lines.Num() != 9) return false;
    FVector Source = FVector::ZeroVector;
    FVector Neighbor = FVector::ZeroVector;
    for (int32 Index = 0; Index < 4; ++Index)
    {
        Source += Lines[Index].Start * 0.25;
        Neighbor += Lines[Index + 4].Start * 0.25;
    }
    const FVector LocalStep = Base.InverseTransformVectorNoScale(Neighbor - Source);
    const double Yaw = (Lines[4].End - Lines[4].Start).Rotation().Yaw;
    const FVector Seam = GuideLineMidpoint(Lines.Last());
    double Gap = TNumericLimits<double>::Max();
    for (int32 Index = 4; Index < 8; ++Index)
        Gap = FMath::Min(Gap, FVector::Dist(Seam, GuideLineMidpoint(Lines[Index])));
    const double Height = Base.InverseTransformPosition(Neighbor).Z;
    const FVector NativeError = LocalStep - NativePose;
    const double YawError = FMath::FindDeltaAngleDegrees(NativeYaw, Yaw);
    UE_LOG(LogSaveCheckAlignmentGuide, Display,
        TEXT("case=%s lines=%d top_z_cm=%.2f top_above_source_center_cm=%.2f step_cm=%s yaw=%.2f relative_yaw=%.2f edge_gap_cm=%.3f native_step_cm=%s native_yaw=%.2f delta_vs_native_cm=%s yaw_error=%.2f"),
        Name, Lines.Num(), Neighbor.Z, Height, *LocalStep.ToString(), Yaw,
        FMath::FindDeltaAngleDegrees(Base.Rotator().Yaw, Yaw), Gap, *NativePose.ToString(), NativeYaw,
        *NativeError.ToString(), YawError);
    FVector Expected = FVector::ZeroVector;
    Expected[Side] = 800.0;
    return LocalStep.Equals(Expected, 0.1) && FMath::IsNearlyEqual(Height, 50.0, 0.1) && Gap <= 0.1
        && NativeError.IsNearlyZero(0.1) && FMath::Abs(YawError) <= 0.1;
}
}

bool FAlignmentGuideCheck::Tick(UWorld& World, APlayerController& Player, AFGCharacterPlayer& Character,
    TSubclassOf<UFGRecipe> Machine, const FString& Frames, TArrayView<const FAlignmentGuideFixture> Fixtures,
    FSpawnFoundation SpawnFoundation, FReadFoundations ReadFoundations)
{
    constexpr const TCHAR* Names[] = { TEXT("native-roof"), TEXT("roof-guide"), TEXT("roof-guide-raised"),
        TEXT("roof-slope-low"), TEXT("roof-slope-high"), TEXT("native-east"), TEXT("foundation-east"),
        TEXT("native-north"), TEXT("foundation-north"), TEXT("foundation-lock"), TEXT("foundation-nudge"),
        TEXT("foundation-off"), TEXT("foundation-retry"), TEXT("foundation-manual-east"), TEXT("foundation-manual-north") };
    const TCHAR* Name = Names[static_cast<int32>(Step)];
    const auto Fail = [Name](const TCHAR* Detail)
    {
        UE_LOG(LogSaveCheckAlignmentGuide, Error, TEXT("pass=0 case=%s %s"), Name, Detail);
        FPlatformMisc::RequestExit(false);
        return false;
    };
    auto* Subsystem = FindGuideSubsystem(World);
    auto* Gun = Character.GetBuildGun();
    if (!Subsystem || !Gun || Fixtures.Num() != 3) return Fail(TEXT("fixtures unavailable"));
    if (Step >= EStep::ManualEast)
        return TickManualPlacement(*Subsystem, Player, Character, Fixtures[1].GhostPose, Frames, SpawnFoundation,
            ReadFoundations);

    const bool bSlope = Step == EStep::SlopeLow || Step == EStep::SlopeHigh;
    const bool bFoundation = Step >= EStep::NativeEast;
    const bool bNative = Step == EStep::NativeRoof || Step == EStep::NativeEast
        || Step == EStep::NativeNorth || Step == EStep::Lock;
    const auto& Fixture = Fixtures[bFoundation ? 1 : bSlope ? 2 : 0];
    FTransform Base = bNative ? Fixture.NativePose : Fixture.GhostPose;
    if (Step == EStep::RaisedRoof) Base.AddToTranslation(FVector(0, 0, RoofFixtureLift));
    const bool bEast = Step == EStep::NativeEast || Step == EStep::East || Step == EStep::Lock;
    FVector Aim(0, 0, Fixture.MeshBounds.Max.Z);
    FVector Eye(790, 600, Aim.Z + 580);
    if (bSlope)
    {
        Aim = FVector(Step == EStep::SlopeLow ? 230 : -230, 0, 0);
        Eye = FVector(Aim.X, 420, Fixture.MeshBounds.Max.Z + 800);
    }
    if (bFoundation)
    {
        Aim = bEast ? FVector(399, 0, 0) : FVector(0, 399, 0);
        Eye = bEast ? FVector(1450, 0, 350) : FVector(0, 1450, 350);
    }

    const bool bNewRecipe = Step == EStep::NativeRoof || Step == EStep::NativeEast;
    const bool bShow = Step == EStep::Roof || Step == EStep::SlopeLow || Step == EStep::East;
    if (++Age == 1)
    {
        if (bNewRecipe) Character.UnequipBuildGun();
        if (Step == EStep::RaisedRoof && !RaiseGuideFixture(*Subsystem, *Fixture.Ghost))
            return Fail(TEXT("could not lift the ghost fixture"));
        Character.GetCharacterMovement()->StopMovementImmediately();
        if (!Character.TeleportTo(Base.TransformPosition(Eye) - FVector(0, 0, Character.BaseEyeHeight),
            FRotator::ZeroRotator, false, true)) return Fail(TEXT("could not reach camera position"));
        Player.SetViewTarget(&Character);
    }
    Player.SetControlRotation((Base.TransformPosition(Aim) - Character.GetPawnViewLocation()).Rotation());
    if (Age == 2 && bNewRecipe) Character.HotKeyRecipe(bFoundation ? Fixture.Recipe : Machine);
    if (bShow || Step == EStep::Off)
    {
        if (Age == 4) SendGuideKey(Player, EKeys::F6, IE_Pressed);
        if (Age == 5) SendGuideKey(Player, EKeys::F6, IE_Released);
        if (Age == 7)
        {
            const bool bClicked = ClickPlanMenuButton(Step == EStep::Off ? TEXT("Hide alignment guide") : TEXT("Show alignment guide"));
            UE_LOG(LogSaveCheckAlignmentGuide, Display, TEXT("case=%s key=F6 menu_action=%d stock_hit=%s"),
                Name, bClicked, *GetNameSafe(Gun->GetHitResult().GetActor()));
            if (!bClicked) return Fail(TEXT("missing menu action"));
        }
    }

    auto* State = Cast<UFGBuildGunStateBuild>(Gun->GetCurrentState());
    auto* Preview = State ? State->GetHologram() : nullptr;
    if (Age >= 8 && !Preview) return Fail(TEXT("missing stock hologram"));
    if (Age == 8 && Step == EStep::Nudge)
    {
        const ENudgeFailReason Result = Preview->NudgeTowardsWorldDirection(Fixture.GhostPose.GetUnitAxis(EAxis::X));
        bPassed &= Result == ENudgeFailReason::NFR_Success;
        UE_LOG(LogSaveCheckAlignmentGuide, Display, TEXT("case=%s stock_nudge_result=%d"), Name, static_cast<int32>(Result));
    }
    if (Age == 8 && Step == EStep::Retry) Preview->LockHologramPosition(false);
    if (Age < 11) return true;
    if (Age > 11)
    {
        if (Age < 14) return true;
        Step = static_cast<EStep>(static_cast<int32>(Step) + 1);
        Age = 0;
        return true;
    }

    const auto Lines = ReadGuideLines(*Subsystem);
    const FHitResult& Hit = Gun->GetHitResult();
    bool bCase = true;
    switch (Step)
    {
    case EStep::NativeRoof:
        NativeRoofHeight = Hit.ImpactPoint.Z - Base.GetLocation().Z;
        NativeRoofYaw = Preview->GetActorRotation().Yaw;
        bCase = Hit.GetActor() == Fixture.Native;
        UE_LOG(LogSaveCheckAlignmentGuide, Display,
            TEXT("case=%s hit_native=%d surface_z_cm=%.2f preview_z_cm=%.2f yaw=%.2f can_construct=%d"),
            Name, bCase, Hit.ImpactPoint.Z, Preview->GetActorLocation().Z, NativeRoofYaw, Preview->CanConstruct());
        break;
    case EStep::Roof:
    case EStep::RaisedRoof:
        bCase = Lines.Num() == 2 && GuideLabelVisible(TEXT("Build Gun validates placement"));
        if (bCase)
        {
            const auto& Axis = Lines[0];
            const double Height = GuideLineMidpoint(Axis).Z;
            const double Yaw = (Axis.End - Axis.Start).Rotation().Yaw;
            const double Error = Height - (Base.GetLocation().Z + NativeRoofHeight);
            bCase &= FMath::Abs(Error) <= 1.0
                && FMath::Abs(FMath::FindDeltaAngleDegrees(NativeRoofYaw, Yaw)) <= 0.1;
            UE_LOG(LogSaveCheckAlignmentGuide, Display,
                TEXT("case=%s guide_z_cm=%.2f surface_offset_cm=%.2f error_vs_native_cm=%.2f guide_yaw=%.2f native_preview_yaw=%.2f can_construct=%d"),
                Name, Height, Height - Base.GetLocation().Z, Error, Yaw, NativeRoofYaw, Preview->CanConstruct());
        }
        break;
    case EStep::SlopeLow:
    case EStep::SlopeHigh:
        bCase = Lines.IsEmpty() && GuideLabelVisible(TEXT("This roof is unsupported"));
        break;
    case EStep::NativeEast:
    case EStep::NativeNorth:
    {
        const int32 Side = bEast ? 0 : 1;
        NativeFoundationPose[Side] = Base.InverseTransformPosition(Preview->GetActorLocation());
        NativeFoundationYaw[Side] = Preview->GetActorRotation().Yaw;
        bCase = Hit.GetActor() == Fixture.Native && Preview->IsValidHitResult(Hit);
        UE_LOG(LogSaveCheckAlignmentGuide, Display, TEXT("case=%s hit_native=%d preview_local_cm=%s yaw=%.2f"),
            Name, bCase, *NativeFoundationPose[Side].ToString(), NativeFoundationYaw[Side]);
        break;
    }
    case EStep::East:
    case EStep::North:
    {
        const int32 Side = bEast ? 0 : 1;
        bCase = ReadFoundationGuide(Lines, Base, Side, Name, NativeFoundationPose[Side], NativeFoundationYaw[Side]);
        if (Step == EStep::North)
        {
            StockGhostPose = Preview->GetActorTransform();
            StockGhostImpact = Hit.ImpactPoint;
            bStockGhostHit = Hit.bBlockingHit;
            bStockGhostCanConstruct = Preview->CanConstruct();
        }
        break;
    }
    case EStep::Lock:
        if (!Preview->CanLockHologram()) return Fail(TEXT("stock hologram cannot lock"));
        Preview->LockHologramPosition(true);
        LockedPose = Preview->GetActorTransform();
        bCase = Preview->IsHologramLocked();
        break;
    case EStep::Nudge:
        NudgedPose = Preview->GetActorTransform();
        NudgeOffset = Preview->GetNudgeOffset();
        bCase = Preview->IsHologramLocked() && !NudgeOffset.IsNearlyZero() && !NudgedPose.Equals(LockedPose, 0.1)
            && ReadFoundationGuide(Lines, Base, 1, Name, NativeFoundationPose[1], NativeFoundationYaw[1]);
        UE_LOG(LogSaveCheckAlignmentGuide, Display, TEXT("case=%s stock_offset_cm=%s stock_world_delta_cm=%s"),
            Name, *NudgeOffset.ToString(), *(NudgedPose.GetLocation() - LockedPose.GetLocation()).ToString());
        break;
    case EStep::Off:
        bCase = Lines.IsEmpty() && !GuideLabelVisible(TEXT("Foundation grid guide")) && Preview->IsHologramLocked()
            && NudgedPose.Equals(Preview->GetActorTransform(), 0.1) && NudgeOffset.Equals(Preview->GetNudgeOffset(), 0.1);
        break;
    case EStep::Retry:
        bCase = Lines.IsEmpty() && !Preview->IsHologramLocked() && Preview->CanConstruct() == bStockGhostCanConstruct
            && Hit.bBlockingHit == bStockGhostHit
            && (!bStockGhostHit || (StockGhostImpact.Equals(Hit.ImpactPoint, 1.0)
                && StockGhostPose.Equals(Preview->GetActorTransform(), 1.0)));
        UE_LOG(LogSaveCheckAlignmentGuide, Display, TEXT("case=%s stock_hit=%d stock_can_construct=%d"),
            Name, Hit.bBlockingHit, Preview->CanConstruct());
        break;
    default:
        checkNoEntry();
    }
    UE_LOG(LogSaveCheckAlignmentGuide, Display, TEXT("case=%s pass=%d lines=%d"), Name, bCase, Lines.Num());
    GuideFrame(Frames, Name);
    bPassed &= bCase;
    return true;
}

bool FAlignmentGuideCheck::TickManualPlacement(AActor& Subsystem, APlayerController& Player,
    AFGCharacterPlayer& Character, const FTransform& Base, const FString& Frames,
    FSpawnFoundation SpawnFoundation, FReadFoundations ReadFoundations)
{
    const bool bEast = Step == EStep::ManualEast;
    const int32 Side = bEast ? 0 : 1;
    const FVector Axis = bEast ? FVector::ForwardVector : FVector::RightVector;
    const TCHAR* Name = bEast ? TEXT("foundation-manual-east") : TEXT("foundation-manual-north");
    const auto Finish = [Name](const TCHAR* Detail)
    {
        UE_LOG(LogSaveCheckAlignmentGuide, Error, TEXT("pass=0 case=%s %s"), Name, Detail);
        FPlatformMisc::RequestExit(false);
        return false;
    };
    const auto LookFrom = [&Character, &Base](const FVector& Eye)
    {
        Character.GetCharacterMovement()->StopMovementImmediately();
        return Character.TeleportTo(Base.TransformPosition(Eye) - FVector(0, 0, Character.BaseEyeHeight),
            FRotator::ZeroRotator, false, true);
    };
    if (++Age == 1)
    {
        ManualAnchor = SpawnFoundation(FTransform(Base.GetRotation(), Base.TransformPosition(Axis * 1600)));
        if (!ManualAnchor.IsValid() || !LookFrom(Axis * 1450 + FVector(0, 0, 350)))
            return Finish(TEXT("could not prepare the real foundation anchor"));
        Player.SetViewTarget(&Character);
    }
    if (Age == 12 && !LookFrom(Axis * 700 + FVector(0, 0, 400)))
        return Finish(TEXT("could not turn to the real foundation anchor"));
    const FVector Aim = Axis * (Age < 12 ? 399 : 1200);
    Player.SetControlRotation((Base.TransformPosition(Aim) - Character.GetPawnViewLocation()).Rotation());
    if (bEast && Age == 4) SendGuideKey(Player, EKeys::F6, IE_Pressed);
    if (bEast && Age == 5) SendGuideKey(Player, EKeys::F6, IE_Released);
    if (bEast && Age == 7)
    {
        const bool bClicked = ClickPlanMenuButton(TEXT("Show alignment guide"));
        UE_LOG(LogSaveCheckAlignmentGuide, Display, TEXT("case=%s key=F6 menu_action=%d"), Name, bClicked);
        if (!bClicked) return Finish(TEXT("missing menu action"));
    }
    if (Age == 11 && !ReadFoundationGuide(ReadGuideLines(Subsystem), Base, Side, Name,
        NativeFoundationPose[Side], NativeFoundationYaw[Side]))
        return Finish(TEXT("could not select the ghost edge"));

    if (Age == 17)
    {
        auto* Gun = Character.GetBuildGun();
        const auto* State = Gun ? Cast<UFGBuildGunStateBuild>(Gun->GetCurrentState()) : nullptr;
        const auto* Preview = State ? State->GetHologram() : nullptr;
        if (!Preview) return Finish(TEXT("missing stock hologram"));
        const FVector Pose = Base.InverseTransformPosition(Preview->GetActorLocation());
        const double Yaw = Preview->GetActorRotation().Yaw;
        const bool bAnchorHit = Gun->GetHitResult().GetActor() == ManualAnchor.Get();
        const bool bGuide = ReadFoundationGuide(ReadGuideLines(Subsystem), Base, Side, Name,
            NativeFoundationPose[Side], NativeFoundationYaw[Side]);
        UE_LOG(LogSaveCheckAlignmentGuide, Display,
            TEXT("case=%s stock_anchor_hit=%d guide_retained=%d stock_allowed=%d preview_local_cm=%s preview_yaw=%.2f"),
            Name, bAnchorHit, bGuide, Preview->CanConstruct(), *Pose.ToString(), Yaw);
        if (!bAnchorHit || !bGuide || !Preview->CanConstruct() || !Pose.Equals(NativeFoundationPose[Side], 0.1)
            || FMath::Abs(FMath::FindDeltaAngleDegrees(NativeFoundationYaw[Side], Yaw)) > 0.1)
            return Finish(TEXT("stock snap did not match the shown grid"));
        ManualBefore = ReadFoundations(Base.GetLocation());
    }
    if (Age == 18) SendGuideKey(Player, EKeys::LeftMouseButton, IE_Pressed);
    if (Age == 20) SendGuideKey(Player, EKeys::LeftMouseButton, IE_Released);
    if (Age == 26)
    {
        TArray<FTransform> Added;
        const auto ContainsPose = [](const TArray<FTransform>& Poses, const FTransform& Pose)
        {
            return Poses.ContainsByPredicate([&Pose](const FTransform& Other) { return Pose.Equals(Other, 0.1); });
        };
        for (const FTransform& Pose : ReadFoundations(Base.GetLocation()))
            if (!ContainsPose(ManualBefore, Pose) && !ContainsPose(Added, Pose)) Added.Add(Pose);
        bool bCase = Added.Num() == 1;
        if (bCase)
        {
            const FVector Local = Base.InverseTransformPosition(Added[0].GetLocation());
            const double Yaw = Added[0].Rotator().Yaw;
            const double NativeYawError = FMath::FindDeltaAngleDegrees(NativeFoundationYaw[Side], Yaw);
            const double Gap = Local[Side] - 800.0;
            bCase = Local.Equals(NativeFoundationPose[Side], 0.1) && FMath::Abs(NativeYawError) <= 0.1;
            UE_LOG(LogSaveCheckAlignmentGuide, Display,
                TEXT("case=%s input=LMB built_local_cm=%s yaw=%.2f yaw_error=%.2f edge_gap_cm=%.3f delta_vs_native_cm=%s"),
                Name, *Local.ToString(), Yaw, NativeYawError, Gap, *(Local - NativeFoundationPose[Side]).ToString());
        }
        UE_LOG(LogSaveCheckAlignmentGuide, Display, TEXT("case=%s pass=%d new_buildings=%d"), Name, bCase, Added.Num());
        GuideFrame(Frames, Name);
        bPassed &= bCase;
    }
    if (Age < 29) return true;
    if (bEast)
    {
        Step = EStep::ManualNorth;
        Age = 0;
        return true;
    }
    UE_LOG(LogSaveCheckAlignmentGuide, Display, TEXT("pass=%d completed"), bPassed);
    FPlatformMisc::RequestExit(false);
    return false;
}
