#include "ShadowPlan.h"
#include "ShadowProjectionProfile.h"
#include "ShadowBlueprintSource.h"
#include "Command/CommandSender.h"
#include "Components/ActorComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Equipment/FGBuildGunBuild.h"
#include "FGCharacterPlayer.h"
#include "FGChatManager.h"
#include "FGPlayerController.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "Hologram/FGBlueprintHologram.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "Subsystem/SubsystemActorManager.h"
#include "TimerManager.h"

class FShadowArchitectModule : public IModuleInterface
{
public:
    void StartupModule() override
    {
        InstallBlueprintSourceHooks();
        AShadowPlanSubsystem::InstallPlanConstructionHooks();
        AShadowPlanSubsystem::InstallPlacementSupportHooks();
    }

    void ShutdownModule() override
    {
        AShadowPlanSubsystem::RemovePlacementSupportHooks();
        AShadowPlanSubsystem::RemovePlanConstructionHooks();
        RemoveBlueprintSourceHooks();
    }
};

IMPLEMENT_MODULE(FShadowArchitectModule, ShadowArchitect)
DEFINE_LOG_CATEGORY_STATIC(LogShadowArchitect, Log, All);

UShadowWorldModule::UShadowWorldModule()
{
    bRootModule = true;
    ModSubsystems.Add(AShadowPlanSubsystem::StaticClass());
    mChatCommands.Add(AShadowPlaceCommand::StaticClass());
}

AShadowPlaceCommand::AShadowPlaceCommand()
{
    CommandName = TEXT("shadowplace");
    Usage = FText::FromString(TEXT("/shadowplace"));
    bOnlyUsableByPlayer = true;
}

EExecutionStatus AShadowPlaceCommand::ExecuteCommand_Implementation(UCommandSender* Sender,
    const TArray<FString>& Arguments, const FString& Label)
{
    if (!Arguments.IsEmpty()) return EExecutionStatus::BAD_ARGUMENTS;
    auto* Player = Sender->GetPlayer();
    if (!IsValid(Player) || !Player->IsLocalController() || Player != GetWorld()->GetFirstPlayerController())
    {
        Sender->SendChatMessage(TEXT("Shadow Architect placement requires the local host player."));
        return EExecutionStatus::UNCOMPLETED;
    }
    auto* Manager = GetWorld()->GetSubsystem<USubsystemActorManager>();
    auto* Plans = Manager ? Manager->GetSubsystemActor<AShadowPlanSubsystem>() : nullptr;
    FString Result;
    const bool bPlaced = IsValid(Plans) && Plans->TryPlace(Result);
    if (!IsValid(Plans)) Result = TEXT("Shadow Architect is not ready in this world.");
    UE_LOG(LogShadowArchitect, Display, TEXT("/shadowplace: %s"), *Result);
    Sender->SendChatMessage(Result);
    return bPlaced ? EExecutionStatus::COMPLETED : EExecutionStatus::UNCOMPLETED;
}

AShadowPlanSubsystem::AShadowPlanSubsystem()
{
    ReplicationPolicy = ESubsystemReplicationPolicy::SpawnOnServer;
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = false;
    PrimaryActorTick.TickGroup = TG_PostUpdateWork;
}

void AShadowPlanSubsystem::BeginPlay()
{
    Super::BeginPlay();
    if (GetNetMode() == NM_DedicatedServer) return;
    ProxyCube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    ShadowMaterial = LoadObject<UMaterialInterface>(nullptr,
        TEXT("/Game/FactoryGame/Equipment/BuildGun/Material/Hologram_Blue_Inst.Hologram_Blue_Inst"));
    if (!ProxyCube || !ShadowMaterial)
        UE_LOG(LogShadowArchitect, Error, TEXT("Projection assets unavailable: Cube=%s Material=%s"),
            *GetNameSafe(ProxyCube.Get()), *GetNameSafe(ShadowMaterial.Get()));
    Restore();
    BindControls();
    SetActorTickEnabled(true);
}

AFGHologram* AShadowPlanSubsystem::CurrentPreview() const
{
    APlayerController* PC = GetWorld()->GetFirstPlayerController();
    AFGCharacterPlayer* Player = PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
    AFGBuildGun* Gun = IsValid(Player) ? Player->GetBuildGun() : nullptr;
    auto* State = IsValid(Gun) && Gun->IsEquipped() ? Cast<UFGBuildGunStateBuild>(Gun->GetCurrentState()) : nullptr;
    return IsValid(State) ? State->GetHologram() : nullptr;
}

AFGHologram* AShadowPlanSubsystem::ActivePreview() const
{
    AFGHologram* Preview = CurrentPreview();
    return IsValid(Preview) && !Preview->IsHidden() && !Preview->IsDisabled() ? Preview : nullptr;
}

void AShadowPlanSubsystem::Notify(const FString& Text) const
{
    UE_LOG(LogShadowArchitect, Display, TEXT("%s"), *Text);
    if (AFGChatManager* Chat = AFGChatManager::Get(GetWorld()))
    {
        FChatMessageStruct Message;
        Message.MessageText = FText::FromString(Text);
        Message.MessageType = EFGChatMessageType::CMT_CustomMessage;
        const auto* GameState = GetWorld()->GetGameState();
        Message.ServerTimeStamp = GameState ? GameState->GetServerWorldTimeSeconds() : GetWorld()->GetTimeSeconds();
        Message.MessageSender = FText::FromString(TEXT("Shadow Architect"));
        Message.MessageSenderColor = FLinearColor::Green;
        Chat->AddChatMessageToReceived(Message);
    }
}

bool AShadowPlanSubsystem::ShowPlan(const FShadowPlacement& Plan, FShadowProjectionProfile* Profile)
{
    if (!Plan.Id.IsValid() || !Plan.Transform.IsValid()
        || Plan.SchemaVersion != 1 || Views.Contains(Plan.Id)) return false;
    FActorSpawnParameters Params;
    Params.ObjectFlags |= RF_Transient;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* View = GetWorld()->SpawnActor<AShadowProjection>(AShadowProjection::StaticClass(), Plan.Transform, Params);
    if (!IsValid(View)) return false;
    if (!BuildView(*View, Plan, ViewMode, Profile))
    {
        View->Destroy();
        return false;
    }
    View->SetActorHiddenInGame(!bVisible);
    Views.Add(Plan.Id, View);
    return true;
}

bool AShadowPlanSubsystem::BuildView(AShadowProjection& View, const FShadowPlacement& Plan, EShadowView Mode,
    FShadowProjectionProfile* Profile) const
{
    if (!ProxyCube) return false;
    switch (Mode)
    {
    case EShadowView::Ghost:
        return ShadowMaterial && View.Build(Plan, *ProxyCube, *ShadowMaterial, Profile);
    case EShadowView::Wireframe:
        return View.BuildWireframe(Plan, *ProxyCube, Profile);
    case EShadowView::Footprint:
        return ShadowMaterial && View.BuildFootprint(Plan, *ProxyCube, *ShadowMaterial, Profile);
    }
    return false;
}

void AShadowPlanSubsystem::Place()
{
    FString Result;
    TryPlace(Result);
    Notify(Result);
}

bool AShadowPlanSubsystem::TryPlace(FString& Result)
{
    auto* Preview = Cast<AFGBlueprintHologram>(ActivePreview());
    if (!Preview) { Result = TEXT("Select a blueprint with the Build Gun first."); return false; }
    FShadowPlacement Plan;
    if (!CaptureBlueprint(*Preview, Plan, Result)) return false;
    return AddCapturedPlan(MoveTemp(Plan), Result);
}

bool AShadowPlanSubsystem::AddCapturedPlan(FShadowPlacement Plan, FString& Result)
{
    if (!ShowPlan(Plan)) { Result = TEXT("Could not render the snapshot. Check the mesh/material and log."); return false; }
    SetSelectedPlan(&Plan);
    Result = FString::Printf(TEXT("Placed %s [%s], %d objects.%s"), *Plan.BlueprintId,
        *Selected.ToString(), Plan.Parts.Num(), bVisible ? TEXT("") : TEXT(" Plans are hidden."));
    Plans.Add(MoveTemp(Plan));
    return true;
}

void AShadowPlanSubsystem::SelectNext()
{
    if (Plans.IsEmpty()) { Notify(TEXT("No plans in this world.")); return; }
    const int32 Current = Plans.IndexOfByPredicate([this](const auto& P) { return P.Id == Selected; });
    for (int32 Offset = 1; Offset <= Plans.Num(); ++Offset)
    {
        const auto& Plan = Plans[(Current + Offset) % Plans.Num()];
        if (!Views.FindRef(Plan.Id).IsValid()) continue;
        SelectPlan(Plan);
        return;
    }
    Notify(TEXT("No renderable plan to select."));
}

void AShadowPlanSubsystem::SelectPlan(const FShadowPlacement& Plan)
{
    SetSelectedPlan(&Plan);
    Notify(FString::Printf(TEXT("Selected %s [%s] at %s cm, rotation %s"), *Plan.BlueprintId,
        *Plan.Id.ToString(), *Plan.Transform.GetLocation().ToCompactString(),
        *Plan.Transform.Rotator().ToCompactString()));
}

void AShadowPlanSubsystem::SetSelectedPlan(const FShadowPlacement* Plan)
{
    if (Plan) ClosePlanMenu();
    const FGuid Id = Plan ? Plan->Id : FGuid();
    if (Selected == Id) return;

    if (auto* Previous = Views.FindRef(Selected).Get()) Previous->SetSelectionMarker(FBox(ForceInit));
    Selected = Id;
    if (Plan)
        if (auto* View = Views.FindRef(Id).Get()) View->SetSelectionMarker(CalculateFootprint(Plan->Parts));
}

void AShadowPlanSubsystem::MoveSelected()
{
    if (!ConfirmPendingPlanMove()) TryMovePlan(Selected);
}

bool AShadowPlanSubsystem::TryMovePlan(const FGuid& Id)
{
    auto* Plan = Plans.FindByPredicate([&Id](const auto& P) { return P.Id == Id; });
    auto View = Views.FindRef(Id);
    if (!Plan || !View.IsValid()) { Notify(TEXT("Select a plan first.")); return false; }
    AFGHologram* Preview = ActivePreview();
    if (!Preview) { Notify(TEXT("Position a Build Gun preview at the new origin first.")); return false; }
    const FTransform Target = Preview->GetActorTransform();
    if (!Target.IsValid() || !View->SetActorTransform(Target)) { Notify(TEXT("Cannot move plan.")); return false; }
    Plan->Transform = Target;
    Notify(FString::Printf(TEXT("Moved %s [%s] to %s cm"), *Plan->BlueprintId,
        *Id.ToString(), *Target.GetLocation().ToCompactString()));
    return true;
}

void AShadowPlanSubsystem::DeleteSelected()
{
    DeletePlan(Selected);
}

void AShadowPlanSubsystem::DeletePlan(const FGuid& Id)
{
    const int32 Index = Plans.IndexOfByPredicate([&Id](const auto& P) { return P.Id == Id; });
    if (!Id.IsValid() || Index == INDEX_NONE) { Notify(TEXT("Select a plan first.")); return; }
    if (AlignmentPlan == Id) ClearAlignmentGuide();
    if (auto View = Views.FindRef(Id); View.IsValid()) View->Destroy();
    Views.Remove(Id);
    Plans.RemoveAt(Index);
    Notify(FString::Printf(TEXT("Removed plan [%s]."), *Id.ToString()));
    if (Selected == Id) SetSelectedPlan(nullptr);
}

void AShadowPlanSubsystem::ToggleVisibility()
{
    bVisible = !bVisible;
    if (!bVisible) ClearAlignmentGuide();
    for (const auto& Entry : Views)
        if (Entry.Value.IsValid()) Entry.Value->SetActorHiddenInGame(!bVisible);
    Notify(bVisible ? TEXT("Plans visible.") : TEXT("Plans hidden."));
}

void AShadowPlanSubsystem::CycleView()
{
    constexpr const TCHAR* ModeNames[] = { TEXT("Ghost"), TEXT("Wireframe"), TEXT("Footprint") };
    const int32 NextIndex = (static_cast<int32>(ViewMode) + 1) % UE_ARRAY_COUNT(ModeNames);
    const auto NextMode = static_cast<EShadowView>(NextIndex);
    FShadowProjectionProfile Profile(TEXT("cycle"), ViewMode, NextMode, Plans.Num(), Views);

    if (ViewMode == EShadowView::Ghost)
    {
        const bool bSuccess = ReuseProjectionMeshesForWireframe(Profile);
        if (bSuccess) ViewMode = NextMode;
        Profile.Finish(ViewMode, bSuccess, bSuccess ? 0 : 1);
        Notify(bSuccess ? TEXT("Plan view: Wireframe.") : TEXT("Could not display plans in Wireframe view."));
        return;
    }

    struct FViewChange
    {
        AShadowProjection* Actor;
        int32 PreviousComponents;
    };
    TArray<FViewChange> Changes;
    Changes.Reserve(Views.Num());
    for (const FShadowPlacement& Plan : Plans)
    {
        auto* View = Views.FindRef(Plan.Id).Get();
        if (!View) continue;
        Changes.Add({ View, View->GetInstanceComponents().Num() });
        if (!BuildView(*View, Plan, NextMode, Profile.Enabled()))
        {
            Profile.Sample(TEXT("partial"));
            for (const FViewChange& Change : Changes)
                for (int32 Index = Change.Actor->GetInstanceComponents().Num() - 1; Index >= Change.PreviousComponents; --Index)
                    Change.Actor->GetInstanceComponents()[Index]->DestroyComponent();
            Profile.Finish(ViewMode, false, 1);
            Notify(FString::Printf(TEXT("Could not display plans in %s view."), ModeNames[NextIndex]));
            return;
        }
    }

    // Keep the previous components until every plan can use the new mode.
    Profile.Sample(TEXT("coexist"));
    for (const FViewChange& Change : Changes)
        for (int32 Index = Change.PreviousComponents - 1; Index >= 0; --Index)
            Change.Actor->GetInstanceComponents()[Index]->DestroyComponent();
    ViewMode = NextMode;
    Profile.Finish(ViewMode, true, 0);
    Notify(FString::Printf(TEXT("Plan view: %s."), ModeNames[NextIndex]));
}

bool AShadowPlanSubsystem::ReuseProjectionMeshesForWireframe(FShadowProjectionProfile& Profile)
{
    if (Views.IsEmpty()) return true;
    UMaterialInterface* Wireframe = nullptr;
    UMaterialInterface* BoxEdges = nullptr;
    if (!LoadShadowWireframeMaterials(Wireframe, BoxEdges, Profile.Enabled())) return false;
    static bool bRejectSecond = FParse::Param(FCommandLine::Get(), TEXT("ShadowTestRejectSecondWireframePlan"));
    TArray<FShadowWireframeChange> Changes;
    for (const auto& Plan : Plans)
    {
        const auto* View = Views.FindRef(Plan.Id).Get();
        if (!View) continue;
        auto* ProxyMaterial = BoxEdges;
        if (bRejectSecond && !Changes.IsEmpty())
        {
            bRejectSecond = false;
            ProxyMaterial = nullptr;
        }
        if (!View->PrepareWireframe(Wireframe, ProxyMaterial, Changes)) return false;
    }

    Profile.Sample(TEXT("prepared"));
    const uint64 Start = Profile.Enabled() ? FPlatformTime::Cycles64() : 0;
    for (const auto& Change : Changes)
    {
        Change.Component->bDisallowNanite = Change.bDisallowNanite;
        for (int32 Slot = 0; Slot < Change.Slots; ++Slot)
            Change.Component->SetMaterial(Slot, Change.Material);
    }
    if (Profile.Enabled())
    {
        Profile.ReuseCycles = FPlatformTime::Cycles64() - Start;
        Profile.ReusedMeshes = Changes.Num();
    }
    return true;
}

void AShadowPlanSubsystem::ClearViews()
{
    ClosePlanMenu();
    CloseClearInView();
    ClearAlignmentGuide();
    for (const auto& Entry : Views)
        if (Entry.Value.IsValid()) Entry.Value->Destroy();
    Views.Reset();
    SetSelectedPlan(nullptr);
}

void AShadowPlanSubsystem::Restore()
{
    FShadowProjectionProfile Profile(TEXT("restore"), ViewMode, ViewMode, Plans.Num(), Views);
    ClearViews();
    Profile.Sample(TEXT("cleared"));
    int32 FailedPlans = 0;
    for (const FShadowPlacement& Plan : Plans)
        if (!ShowPlan(Plan, Profile.Enabled()))
        {
            ++FailedPlans;
            UE_LOG(LogShadowArchitect, Warning, TEXT("Could not display plan %s (schema %d)"),
                *Plan.Id.ToString(), Plan.SchemaVersion);
        }
    Profile.Finish(ViewMode, FailedPlans == 0, FailedPlans);
}

void AShadowPlanSubsystem::PostLoadGame_Implementation(int32 SaveVersion, int32 GameVersion)
{
    if (HasActorBegunPlay() && GetNetMode() != NM_DedicatedServer) Restore();
}

void AShadowPlanSubsystem::EndPlay(const EEndPlayReason::Type Reason)
{
    PlacementFeedback.Reset();
    GetWorldTimerManager().ClearTimer(InputRetry);
    PlanningControls.Reset();
    DisableInput(GetWorld()->GetFirstPlayerController());
    ClearViews();
    Super::EndPlay(Reason);
}
