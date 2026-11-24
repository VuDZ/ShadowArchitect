#include "ShadowPlan.h"
#include "EnhancedInputComponent.h"
#include "Engine/GameViewportClient.h"
#include "FGPlayerInput.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"
#include "Widgets/SWidget.h"

bool AShadowPlanSubsystem::CanUsePlanControls(const APlayerController& Player)
{
    UGameViewportClient* Viewport = Player.GetWorld()->GetGameViewport();
    if (Player.IsPaused() || Player.bShowMouseCursor || !Viewport || Viewport->IgnoreInput()) return false;
    if (!FSlateApplication::IsInitialized()) return true;

    const TSharedPtr<SWidget> Focus = FSlateApplication::Get().GetKeyboardFocusedWidget();
    return !Focus || !Focus->GetTypeAsString().Contains(TEXT("EditableText"));
}

void AShadowPlanSubsystem::BindControls()
{
    APlayerController* PC = GetWorld()->GetFirstPlayerController();
    auto* PlayerInput = IsValid(PC) && PC->IsLocalController() ? Cast<UFGPlayerInput>(PC->PlayerInput) : nullptr;
    auto Retry = [this]()
    {
        GetWorldTimerManager().SetTimer(InputRetry, this, &AShadowPlanSubsystem::BindControls, 0.5f, false);
    };
    if (!PlayerInput) { Retry(); return; }

    struct FControl
    {
        FName Mapping;
        void (AShadowPlanSubsystem::*Handler)();
        const UInputAction* Action = nullptr;
    };
    FControl Controls[] = {
        { TEXT("ShadowArchitect.Place"), &AShadowPlanSubsystem::Place },
        { TEXT("ShadowArchitect.PlacementSupport"), &AShadowPlanSubsystem::TogglePlacementSupport },
        { TEXT("ShadowArchitect.Select"), &AShadowPlanSubsystem::SelectNext },
        { TEXT("ShadowArchitect.SelectUnderCrosshair"), &AShadowPlanSubsystem::SelectPlanUnderCrosshair },
        { TEXT("ShadowArchitect.Move"), &AShadowPlanSubsystem::MoveSelected },
        { TEXT("ShadowArchitect.Delete"), &AShadowPlanSubsystem::DeleteSelected },
        { TEXT("ShadowArchitect.Visibility"), &AShadowPlanSubsystem::ToggleVisibility },
        { TEXT("ShadowArchitect.View"), &AShadowPlanSubsystem::CycleView },
        { TEXT("ShadowArchitect.ClearInView"), &AShadowPlanSubsystem::RequestClearInView },
        { TEXT("ShadowArchitect.OpenPlanMenu"), &AShadowPlanSubsystem::OpenPlanMenu }
    };
    for (FControl& Control : Controls)
    {
        Control.Action = PlayerInput->FindActionByMappableActionName(Control.Mapping);
        if (!Control.Action) { Retry(); return; }
    }
    if (!ImportLegacyKeybindings(*PC)) { Retry(); return; }

    CreateInputComponent(UEnhancedInputComponent::StaticClass());
    auto* Input = CastChecked<UEnhancedInputComponent>(InputComponent);
    for (const FControl& Control : Controls)
    {
        Input->BindActionInstanceLambda(Control.Action, ETriggerEvent::Triggered,
            [Owner = TWeakObjectPtr<AShadowPlanSubsystem>(this), Handler = Control.Handler](const FInputActionInstance&)
            {
                if (auto* PlansActor = Owner.Get())
                {
                    const APlayerController* Player = PlansActor->GetWorld()->GetFirstPlayerController();
                    if (IsValid(Player) && CanUsePlanControls(*Player)) (PlansActor->*Handler)();
                }
            });
    }
    const FInputChord BuildBinding = BuildPreviewChord.IsValidChord()
        ? BuildPreviewChord : FInputChord(EKeys::F8, true, true, false, false);
    Input->UInputComponent::BindKey(BuildBinding, IE_Pressed, this,
        &AShadowPlanSubsystem::OpenSelectedBuildPreview).bConsumeInput = false;
    EnableInput(PC);
    StartPlanningControls();

    const FString Key = KeybindingHint(*PC, TEXT("ShadowArchitect.Place"));
    const FString Hint = Key.IsEmpty() ? TEXT("/shadowplace") : Key + TEXT(" or /shadowplace");
    Notify(FString::Printf(TEXT("Ready. %d saved plan(s). Place with %s."), Plans.Num(), *Hint));
}
