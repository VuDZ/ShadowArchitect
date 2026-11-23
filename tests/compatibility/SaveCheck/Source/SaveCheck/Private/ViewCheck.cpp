#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "CollisionQueryParams.h"
#include "Components/CapsuleComponent.h"
#include "Components/InputComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Equipment/FGBuildGunBuild.h"
#include "FGBlueprintSubsystem.h"
#include "FGCharacterPlayer.h"
#include "FGSaveSession.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Hologram/FGBlueprintHologram.h"
#include "InputKeyEventArgs.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UnrealClient.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogSaveCheckViews, Log, All);

namespace
{
const TCHAR* ModeNames[] = { TEXT("Ghost"), TEXT("Wireframe"), TEXT("Footprint") };

struct FProjectionState
{
    FGuid Id;
    FTransform Transform;
    TWeakObjectPtr<AActor> View;
    TArray<TWeakObjectPtr<UStaticMeshComponent>> Meshes;
    TArray<FString> Geometry;
};

AActor* FindPlanSubsystem(UWorld& World)
{
    UClass* Class = FindObject<UClass>(nullptr, TEXT("/Script/ShadowArchitect.ShadowPlanSubsystem"));
    if (!Class) return nullptr;
    TActorIterator<AActor> It(&World, Class);
    AActor* Subsystem = It ? *It : nullptr;
    if (It) ++It;
    return It ? nullptr : Subsystem;
}

bool CheckLoadedModule()
{
    const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("ShadowArchitect"));
    FModuleStatus Module;
    if (!Plugin || !Plugin->IsEnabled()
        || !FModuleManager::Get().QueryModule(TEXT("ShadowArchitect"), Module) || !Module.bIsLoaded) return false;
    const FString LoadedDll = FPaths::ConvertRelativePathToFull(Module.FilePath);
    const FString InstalledDll = FPaths::ConvertRelativePathToFull(FPaths::Combine(Plugin->GetBaseDir(),
        TEXT("Binaries/Win64/FactoryGameSteam-ShadowArchitect-Win64-Shipping.dll")));
    const bool bInstalled = FPaths::IsSamePath(LoadedDll, InstalledDll);
    UE_LOG(LogSaveCheckViews, Display, TEXT("version=%s loaded_module=%s installed_module=%d"),
        *Plugin->GetDescriptor().VersionName, *LoadedDll, bInstalled);
    return bInstalled;
}

bool Snapshot(UWorld& World, int32 Mode, int32 Count, bool bVisible, TArray<FProjectionState>& Out)
{
    AActor* Subsystem = FindPlanSubsystem(World);
    if (!Subsystem) return false;
    const auto* Plans = FindFProperty<FArrayProperty>(Subsystem->GetClass(), TEXT("Plans"));
    const auto* RecordType = Plans ? CastField<FStructProperty>(Plans->Inner) : nullptr;
    if (!RecordType) return false;
    const auto* Id = FindFProperty<FStructProperty>(RecordType->Struct, TEXT("Id"));
    const auto* Transform = FindFProperty<FStructProperty>(RecordType->Struct, TEXT("Transform"));
    if (!Id || Id->Struct->GetFName() != FName(TEXT("Guid"))
        || !Transform || Transform->Struct->GetFName() != FName(TEXT("Transform"))) return false;
    FScriptArrayHelper Records(Plans, Plans->ContainerPtrToValuePtr<void>(Subsystem));
    if (Records.Num() != Count) return false;

    UClass* ViewClass = FindObject<UClass>(nullptr, TEXT("/Script/ShadowArchitect.ShadowProjection"));
    if (!ViewClass) return false;
    TSet<AActor*> Views;
    for (TActorIterator<AActor> It(&World, ViewClass); It; ++It) Views.Add(*It);
    if (Views.Num() != Count) return false;
    Out.Reset();
    for (int32 Index = 0; Index < Records.Num(); ++Index)
    {
        const void* Record = Records.GetRawPtr(Index);
        const FTransform& Pose = *Transform->ContainerPtrToValuePtr<FTransform>(Record);
        AActor* View = nullptr;
        for (AActor* Candidate : Views)
            if (Candidate->GetActorTransform().Equals(Pose, 0.01)) { View = Candidate; break; }
        if (!View || View->IsHidden() == bVisible) return false;
        Views.Remove(View);

        auto& State = Out.AddDefaulted_GetRef();
        State.Id = *Id->ContainerPtrToValuePtr<FGuid>(Record);
        State.Transform = Pose;
        State.View = View;
        const TInlineComponentArray<UStaticMeshComponent*> Components(View);
        if (Components.IsEmpty() || Components.Num() != View->GetInstanceComponents().Num()
            || (Mode == 2 && Components.Num() != 1)) return false;

        for (UStaticMeshComponent* Mesh : Components)
        {
            UMaterialInterface* Material = Mesh->GetMaterial(0);
            if (!Material || !Mesh->GetStaticMesh() || !Mesh->IsRegistered()
                || Mesh->GetAttachParent() != View->GetRootComponent()) return false;
            const FName Name = Material->GetFName();
            if (Mode == 1 ? (Name != FName(TEXT("M_ShadowWireframe")) && Name != FName(TEXT("M_ShadowBoxEdges")))
                : Name != FName(TEXT("Hologram_Blue_Inst"))) return false;
            if (Mode == 2 && Mesh->GetStaticMesh()->GetFName() != FName(TEXT("Cube"))) return false;
            State.Meshes.Add(Mesh);
            State.Geometry.Add(Mesh->GetStaticMesh()->GetPathName() + TEXT(" ") + Mesh->GetRelativeTransform().ToString());
        }
        State.Geometry.Sort();
    }
    return true;
}

void SendKey(APlayerController& PC, FKey Key, EInputEvent Event)
{
    auto* Viewport = GEngine->GameViewport ? GEngine->GameViewport->Viewport : nullptr;
    PC.InputKey(FInputKeyEventArgs(Viewport, FInputDeviceId::CreateFromInternalId(0), Key, Event, FPlatformTime::Cycles64()));
}

struct FViewProbe
{
    enum class EStep { Ready, Before, Cycle, After, Wireframe, Blueprint, Place, Hide, Footprint, Show };
    EStep Step = EStep::Ready;
    FString OutputDir;
    TArray<FProjectionState> Initial;
    TArray<FProjectionState> Previous;
    int32 Ticks = 0;
    int32 Wait = 0;
    int32 Cycles = 0;
    bool bPressed = false;
    bool bCustomKeys = false;
    TOptional<FVector> BuildTarget;

    FInputChord PlaceInput() const { return bCustomKeys ? FInputChord(EKeys::F8, false, false, true, false) : FInputChord(EKeys::F7); }
    FInputChord ViewInput() const { return bCustomKeys ? FInputChord(EKeys::F11, false, false, true, false) : FInputChord(EKeys::G, true, true, false, false); }
    FInputChord VisibilityInput() const { return bCustomKeys ? FInputChord(EKeys::F9, false, false, true, false) : FInputChord(EKeys::V, true, true, false, false); }

    bool CheckBindings(UWorld& World) const
    {
        const FInputChord Keys[] = {
            PlaceInput(),
            FInputChord(bCustomKeys ? EKeys::F5 : EKeys::J, true, true, false, false),
            bCustomKeys ? FInputChord(EKeys::F6, true, false, false, false) : FInputChord(EKeys::M, true, true, false, false),
            bCustomKeys ? FInputChord(EKeys::F10, false, true, false, false) : FInputChord(EKeys::K, true, true, false, false),
            VisibilityInput(),
            ViewInput()
        };
        const TCHAR* Names[] = { TEXT("Place"), TEXT("Select"), TEXT("Move"), TEXT("Delete"), TEXT("Visibility"), TEXT("View") };
        const auto* Subsystem = FindPlanSubsystem(World);
        const auto* Input = Subsystem ? Subsystem->InputComponent.Get() : nullptr;
        if (!Input || Input->KeyBindings.Num() < UE_ARRAY_COUNT(Keys)) return false;
        bool bMatch = true;
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(Keys); ++Index)
        {
            const auto& Chord = Input->KeyBindings[Index].Chord;
            UE_LOG(LogSaveCheckViews, Display, TEXT("binding=%s chord=%s match=%d"),
                Names[Index], *Chord.GetInputText().ToString(), Chord == Keys[Index]);
            bMatch &= Chord == Keys[Index];
        }
        return bMatch;
    }

    static bool Finish(bool bPassed, const TCHAR* Reason)
    {
        UE_LOG(LogSaveCheckViews, Display, TEXT("pass=%d %s"), bPassed, Reason);
        FPlatformMisc::RequestExit(false);
        return false;
    }

    void SetStep(EStep Next)
    {
        Step = Next;
        Wait = 0;
    }

    bool Stroke(APlayerController& Player, const FInputChord& Chord)
    {
        if (!bPressed)
        {
            if (Chord.bCtrl) SendKey(Player, EKeys::LeftControl, IE_Pressed);
            if (Chord.bShift) SendKey(Player, EKeys::LeftShift, IE_Pressed);
            if (Chord.bAlt) SendKey(Player, EKeys::LeftAlt, IE_Pressed);
            SendKey(Player, Chord.Key, IE_Pressed);
        }
        else
        {
            SendKey(Player, Chord.Key, IE_Released);
            if (Chord.bAlt) SendKey(Player, EKeys::LeftAlt, IE_Released);
            if (Chord.bShift) SendKey(Player, EKeys::LeftShift, IE_Released);
            if (Chord.bCtrl) SendKey(Player, EKeys::LeftControl, IE_Released);
        }
        bPressed = !bPressed;
        return !bPressed;
    }

    bool Confirm(UWorld& World, const TCHAR* Action, int32 Mode, int32 Count, bool bVisible, bool bReplaced = false)
    {
        TArray<FProjectionState> Current;
        if (!Snapshot(World, Mode, Count, bVisible, Current))
        {
            UE_LOG(LogSaveCheckViews, Error, TEXT("action=%s snapshot failed mode=%s count=%d visible=%d"),
                Action, ModeNames[Mode], Count, bVisible);
            return false;
        }
        for (int32 Index = 0; Index < Previous.Num(); ++Index)
        {
            if (Previous[Index].Id != Current[Index].Id || Previous[Index].View != Current[Index].View
                || !Previous[Index].Transform.Equals(Current[Index].Transform, 0.01)) return false;
            if (bReplaced)
            {
                for (const auto& Mesh : Previous[Index].Meshes)
                    if (Mesh.IsValid()) return false;
            }
            else if (Previous[Index].Meshes != Current[Index].Meshes) return false;
        }

        TArray<FString> MeshCounts;
        for (const auto& State : Current) MeshCounts.Add(FString::FromInt(State.Meshes.Num()));
        UE_LOG(LogSaveCheckViews, Display, TEXT("action=%s mode=%s plans=%d actors=%d visible=%d meshes=%s old_components_released=%d"),
            Action, ModeNames[Mode], Count, Current.Num(), bVisible ? Count : 0,
            *FString::Join(MeshCounts, TEXT(",")), bReplaced);
        Previous = MoveTemp(Current);
        return true;
    }

    bool Frame(const TCHAR* File)
    {
        const FString Path = FPaths::Combine(OutputDir, File);
        if (Wait == 3) FScreenshotRequest::RequestScreenshot(Path, false, false);
        return Wait > 3 && IFileManager::Get().FileSize(*Path) > 0;
    }

    bool SetCamera(UWorld& World, APlayerController& Player)
    {
        FBox Bounds(ForceInit);
        for (const FProjectionState& State : Initial)
            for (const auto& Mesh : State.Meshes) Bounds += Mesh->Bounds.GetBox();
        const FVector Target = Bounds.GetCenter();
        const FVector Eye = Target + FVector(0.70, 0.57, 0.47).GetSafeNormal() * Bounds.GetSize().Size() * 1.4;
        auto* Camera = World.SpawnActor<ACameraActor>(Eye, (Target - Eye).Rotation());
        if (!Camera) return false;
        Camera->GetCameraComponent()->SetFieldOfView(65);
        Player.SetViewTarget(Camera);
        return true;
    }

    bool OpenPreview(UWorld& World, APlayerController& Player)
    {
        auto* Character = Cast<AFGCharacterPlayer>(Player.GetPawn());
        if (!Character) return false;
        TArray<UFGBlueprintDescriptor*> Descriptors;
        AFGBlueprintSubsystem::GetBlueprintDescriptors(Descriptors, &World);
        UE_LOG(LogSaveCheckViews, Display, TEXT("blueprints=%d pawn_cm=%s"), Descriptors.Num(), *Character->GetActorLocation().ToString());
        if (Descriptors.Num() != 1 || !Descriptors[0]) return false;
        Player.SetViewTarget(Character);
        const FVector Anchor = Initial[0].Transform.GetLocation();
        const FCollisionQueryParams Query(SCENE_QUERY_STAT(ShadowViewProbe), false, Character);
        auto SurfaceBelow = [&](const FVector& Position, FHitResult& Hit, ECollisionChannel Channel)
        {
            return World.LineTraceSingleByChannel(Hit, Position + FVector(0, 0, 1800), Position - FVector(0, 0, 1100), Channel, Query)
                && Hit.ImpactNormal.Z > 0.9;
        };
        for (const FVector Offset : { FVector(-3200, 2400, 0), FVector(3200, 2400, 0), FVector(0, 2800, 0), FVector(-1200, 1600, 0) })
        {
            FHitResult Aim;
            FHitResult Foot;
            if (!SurfaceBelow(Anchor + Offset, Aim, ECC_GameTraceChannel5)
                || !SurfaceBelow(Anchor + Offset - FVector(500, 0, 0), Foot, ECC_Visibility)) continue;
            const double Height = Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 12;
            if (!Character->TeleportTo(Foot.ImpactPoint + FVector(0, 0, Height), FRotator::ZeroRotator)) continue;
            BuildTarget = Aim.ImpactPoint;
            UE_LOG(LogSaveCheckViews, Display, TEXT("aim_target_cm=%s fixture_pawn_cm=%s"),
                *BuildTarget->ToString(), *Character->GetActorLocation().ToString());
            break;
        }
        Player.SetControlRotation(BuildTarget ? (*BuildTarget - Character->GetPawnViewLocation()).Rotation() : FRotator(-55, 0, 0));
        Character->HotKeyBlueprint(Descriptors[0]->GetBlueprintNameAsString());
        return true;
    }

    bool PreviewReady(APlayerController& Player)
    {
        auto* Character = Cast<AFGCharacterPlayer>(Player.GetPawn());
        auto* Gun = Character ? Character->GetBuildGun() : nullptr;
        auto* State = Gun ? Cast<UFGBuildGunStateBuild>(Gun->GetCurrentState()) : nullptr;
        auto* Preview = State ? Cast<AFGBlueprintHologram>(State->GetHologram()) : nullptr;
        if (BuildTarget && Character) Player.SetControlRotation((*BuildTarget - Character->GetPawnViewLocation()).Rotation());
        if (Wait == 1 || Wait == 20 || Wait == 50)
        {
            FVector Eye;
            FRotator Look;
            Player.GetPlayerViewPoint(Eye, Look);
            FHitResult Hit;
            const FCollisionQueryParams Query(SCENE_QUERY_STAT(ShadowViewProbe), false, Character);
            Player.GetWorld()->LineTraceSingleByChannel(Hit, Eye, Eye + Look.Vector() * 8000, ECC_GameTraceChannel5, Query);
            UE_LOG(LogSaveCheckViews, Display, TEXT("preview=%s hidden=%d disabled=%d roots=%d viewpoint_cm=%s rotation=%s hit=%s valid_hit=%d"),
                *GetNameSafe(Preview), Preview && Preview->IsHidden(), Preview && Preview->IsDisabled(),
                Preview ? Preview->mBuildableToNewRoot.Num() : 0, *Eye.ToString(), *Look.ToString(),
                *GetNameSafe(Hit.GetActor()), Preview && Preview->IsValidHitResult(Hit));
        }
        if (!Preview || Preview->IsHidden() || Preview->IsDisabled() || Preview->mBuildableToNewRoot.IsEmpty()) return false;
        UE_LOG(LogSaveCheckViews, Display, TEXT("preview_roots=%d placement_cm=%s"),
            Preview->mBuildableToNewRoot.Num(), *Preview->GetActorLocation().ToString());
        return true;
    }

    bool TickWorld(UWorld& World, APlayerController& Player)
    {
        ++Wait;
        if (Wait > 55) return Finish(false, TEXT("step timeout"));
        switch (Step)
        {
        case EStep::Ready:
            if (Wait < 7) break;
            if (!CheckLoadedModule() || !CheckBindings(World) || !Confirm(World, TEXT("initial"), 0, 2, true))
                return Finish(false, TEXT("loaded module, bindings or initial Ghost"));
            Initial = Previous;
            for (const auto& State : Initial)
                UE_LOG(LogSaveCheckViews, Display, TEXT("initial id=%s transform=%s"), *State.Id.ToString(), *State.Transform.ToString());
            if (!SetCamera(World, Player)) return Finish(false, TEXT("camera"));
            SetStep(EStep::Before);
            break;
        case EStep::Before:
            if (Frame(TEXT("Ghost-before.png"))) SetStep(EStep::Cycle);
            break;
        case EStep::Cycle:
            if (!Stroke(Player, ViewInput())) break;
            ++Cycles;
            if (!Confirm(World, *ViewInput().GetInputText().ToString(), Cycles % 3, 2, true, true)) return Finish(false, TEXT("cycle"));
            if (Cycles == 9)
            {
                for (int32 Index = 0; Index < Initial.Num(); ++Index)
                {
                    if (Initial[Index].Geometry != Previous[Index].Geometry) return Finish(false, TEXT("Ghost geometry changed"));
                    UE_LOG(LogSaveCheckViews, Display, TEXT("after_three_cycles id=%s transform=%s same_actor=1 same_geometry=1"),
                        *Previous[Index].Id.ToString(), *Previous[Index].Transform.ToString());
                }
                SetStep(EStep::After);
            }
            break;
        case EStep::After:
            if (Frame(TEXT("Ghost-after.png"))) SetStep(EStep::Wireframe);
            break;
        case EStep::Wireframe:
            if (!Stroke(Player, ViewInput())) break;
            if (!Confirm(World, *ViewInput().GetInputText().ToString(), 1, 2, true, true) || !OpenPreview(World, Player)) return Finish(false, TEXT("open Blueprint"));
            SetStep(EStep::Blueprint);
            break;
        case EStep::Blueprint:
            if (PreviewReady(Player)) SetStep(EStep::Place);
            break;
        case EStep::Place:
            if (!Stroke(Player, PlaceInput())) break;
            if (!Confirm(World, *PlaceInput().GetInputText().ToString(), 1, 3, true)) return Finish(false, TEXT("third Wireframe"));
            if (auto* Character = Cast<AFGCharacterPlayer>(Player.GetPawn())) Character->UnequipBuildGun();
            SetStep(EStep::Hide);
            break;
        case EStep::Hide:
            if (!Stroke(Player, VisibilityInput())) break;
            if (!Confirm(World, *VisibilityInput().GetInputText().ToString(), 1, 3, false)) return Finish(false, TEXT("hide Wireframe"));
            SetStep(EStep::Footprint);
            break;
        case EStep::Footprint:
            if (!Stroke(Player, ViewInput())) break;
            if (!Confirm(World, *ViewInput().GetInputText().ToString(), 2, 3, false, true)) return Finish(false, TEXT("hidden Footprint"));
            SetStep(EStep::Show);
            break;
        case EStep::Show:
            if (!Stroke(Player, VisibilityInput())) break;
            return Confirm(World, *VisibilityInput().GetInputText().ToString(), 2, 3, true) ? Finish(true, TEXT("completed")) : Finish(false, TEXT("show Footprint"));
        }
        return true;
    }

    bool Tick()
    {
        if (++Ticks > 420) return Finish(false, TEXT("world timeout"));
        if (!GEngine) return true;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            UWorld* World = Context.World();
            if (!World || !World->IsGameWorld() || !World->HasBegunPlay()
                || !World->GetMapName().Contains(TEXT("Persistent_Level"))) continue;
            UFGSaveSession* Save = UFGSaveSession::Get(World);
            if (!Save) continue;
            Save->SetAutoSaveEnabled(false);
            APlayerController* Player = World->GetFirstPlayerController();
            if (Player && Player->GetPawn() && Player->PlayerInput) return TickWorld(*World, *Player);
        }
        return true;
    }
};
}

FTickerDelegate MakeViewCheck(FString OutputDir, bool bCustomKeys)
{
    FViewProbe Probe;
    Probe.OutputDir = MoveTemp(OutputDir);
    Probe.bCustomKeys = bCustomKeys;
    return FTickerDelegate::CreateLambda([Probe = MoveTemp(Probe)](float) mutable { return Probe.Tick(); });
}
