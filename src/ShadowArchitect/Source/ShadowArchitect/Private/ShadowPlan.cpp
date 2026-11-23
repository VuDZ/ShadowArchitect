#include "ShadowPlan.h"
#include "Command/CommandSender.h"
#include "Components/InputComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Equipment/FGBuildGunBuild.h"
#include "FGCharacterPlayer.h"
#include "FGChatManager.h"
#include "FGPlayerController.h"
#include "GameFramework/GameStateBase.h"
#include "Framework/Commands/InputChord.h"
#include "GameFramework/PlayerController.h"
#include "Hologram/FGBlueprintHologram.h"
#include "Materials/MaterialInterface.h"
#include "Modules/ModuleManager.h"
#include "Subsystem/SubsystemActorManager.h"
#include "TimerManager.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, ShadowArchitect)
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
    PrimaryActorTick.bCanEverTick = false;
    PlaceKey = EKeys::F7;
    SelectKey = EKeys::J;
    MoveKey = EKeys::M;
    DeleteKey = EKeys::K;
    VisibilityKey = EKeys::V;
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
}

void AShadowPlanSubsystem::BindControls()
{
    APlayerController* PC = GetWorld()->GetFirstPlayerController();
    if (!IsValid(PC) || !PC->IsLocalController() || !PC->PlayerInput)
    {
        GetWorldTimerManager().SetTimer(InputRetry, this, &AShadowPlanSubsystem::BindControls, 0.5f, false);
        return;
    }
    EnableInput(PC);
    if (!InputComponent)
    {
        UE_LOG(LogShadowArchitect, Error, TEXT("Unable to attach local input"));
        return;
    }
    auto Bind = [this](FKey Key, void (AShadowPlanSubsystem::*Action)())
    {
        if (Key.IsValid())
            InputComponent->BindKey(FInputChord(Key, true, true, false, false), IE_Pressed, this, Action).bConsumeInput = false;
    };
    if (PlaceKey.IsValid())
        InputComponent->BindKey(PlaceKey, IE_Pressed, this, &AShadowPlanSubsystem::Place).bConsumeInput = false;
    Bind(SelectKey, &AShadowPlanSubsystem::SelectNext);
    Bind(MoveKey, &AShadowPlanSubsystem::MoveSelected);
    Bind(DeleteKey, &AShadowPlanSubsystem::DeleteSelected);
    Bind(VisibilityKey, &AShadowPlanSubsystem::ToggleVisibility);
    Notify(FString::Printf(TEXT("Ready. %d saved plan(s). Place with %s or /shadowplace."),
        Plans.Num(), *PlaceKey.ToString()));
}

AFGHologram* AShadowPlanSubsystem::ActivePreview() const
{
    APlayerController* PC = GetWorld()->GetFirstPlayerController();
    AFGCharacterPlayer* Player = PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
    AFGBuildGun* Gun = IsValid(Player) ? Player->GetBuildGun() : nullptr;
    auto* State = IsValid(Gun) && Gun->IsEquipped() ? Cast<UFGBuildGunStateBuild>(Gun->GetCurrentState()) : nullptr;
    AFGHologram* Preview = IsValid(State) ? State->GetHologram() : nullptr;
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

bool AShadowPlanSubsystem::ShowPlan(const FShadowPlacement& Plan)
{
    if (!ProxyCube || !ShadowMaterial || !Plan.Id.IsValid() || !Plan.Transform.IsValid()
        || Plan.SchemaVersion != 1 || Views.Contains(Plan.Id)) return false;
    FActorSpawnParameters Params;
    Params.ObjectFlags |= RF_Transient;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* View = GetWorld()->SpawnActor<AShadowProjection>(AShadowProjection::StaticClass(), Plan.Transform, Params);
    if (!IsValid(View)) return false;
    if (!View->BuildFootprint(Plan, *ProxyCube, *ShadowMaterial))
    {
        View->Destroy();
        return false;
    }
    View->SetActorHiddenInGame(!bVisible);
    Views.Add(Plan.Id, View);
    return true;
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
    if (!ShowPlan(Plan)) { Result = TEXT("Could not render the snapshot. Check the mesh/material and log."); return false; }
    Selected = Plan.Id;
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
        Selected = Plan.Id;
        Notify(FString::Printf(TEXT("Selected %s [%s] at %s cm, rotation %s"), *Plan.BlueprintId,
            *Plan.Id.ToString(), *Plan.Transform.GetLocation().ToCompactString(),
            *Plan.Transform.Rotator().ToCompactString()));
        return;
    }
    Notify(TEXT("No renderable plan to select."));
}

void AShadowPlanSubsystem::MoveSelected()
{
    auto* Plan = Plans.FindByPredicate([this](const auto& P) { return P.Id == Selected; });
    auto View = Views.FindRef(Selected);
    if (!Plan || !View.IsValid()) { Notify(TEXT("Select a plan first.")); return; }
    AFGHologram* Preview = ActivePreview();
    if (!Preview) { Notify(TEXT("Position a Build Gun preview at the new origin first.")); return; }
    const FTransform Target = Preview->GetActorTransform();
    if (!Target.IsValid() || !View->SetActorTransform(Target)) { Notify(TEXT("Cannot move plan.")); return; }
    Plan->Transform = Target;
    Notify(FString::Printf(TEXT("Moved %s to %s cm"), *Plan->BlueprintId, *Target.GetLocation().ToCompactString()));
}

void AShadowPlanSubsystem::DeleteSelected()
{
    const int32 Index = Plans.IndexOfByPredicate([this](const auto& P) { return P.Id == Selected; });
    if (!Selected.IsValid() || Index == INDEX_NONE) { Notify(TEXT("Select a plan first.")); return; }
    if (auto View = Views.FindRef(Selected); View.IsValid()) View->Destroy();
    Views.Remove(Selected);
    Plans.RemoveAt(Index);
    Notify(FString::Printf(TEXT("Removed plan [%s]."), *Selected.ToString()));
    Selected.Invalidate();
}

void AShadowPlanSubsystem::ToggleVisibility()
{
    bVisible = !bVisible;
    for (const auto& Entry : Views)
        if (Entry.Value.IsValid()) Entry.Value->SetActorHiddenInGame(!bVisible);
    Notify(bVisible ? TEXT("Plans visible.") : TEXT("Plans hidden."));
}

void AShadowPlanSubsystem::ClearViews()
{
    for (const auto& Entry : Views)
        if (Entry.Value.IsValid()) Entry.Value->Destroy();
    Views.Reset();
    Selected.Invalidate();
}

void AShadowPlanSubsystem::Restore()
{
    ClearViews();
    for (const FShadowPlacement& Plan : Plans)
        if (!ShowPlan(Plan))
            UE_LOG(LogShadowArchitect, Warning, TEXT("Could not display plan %s (schema %d)"),
                *Plan.Id.ToString(), Plan.SchemaVersion);
}

void AShadowPlanSubsystem::PostLoadGame_Implementation(int32 SaveVersion, int32 GameVersion)
{
    if (HasActorBegunPlay() && GetNetMode() != NM_DedicatedServer) Restore();
}

void AShadowPlanSubsystem::EndPlay(const EEndPlayReason::Type Reason)
{
    GetWorldTimerManager().ClearTimer(InputRetry);
    DisableInput(GetWorld()->GetFirstPlayerController());
    ClearViews();
    Super::EndPlay(Reason);
}
