#include "Camera/CameraActor.h"
#include "Components/Button.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Equipment/FGBuildGunBuild.h"
#include "FGBlueprintSubsystem.h"
#include "FGCharacterPlayer.h"
#include "FGChatManager.h"
#include "FGGameUserSettings.h"
#include "FGInputLibrary.h"
#include "FGPlayerController.h"
#include "FGPlayerInput.h"
#include "FGSaveSession.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/InputChord.h"
#include "HAL/FileManager.h"
#include "Hologram/FGBlueprintHologram.h"
#include "InputKeyEventArgs.h"
#include "Layout/Children.h"
#include "Layout/WidgetPath.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "UI/FGButtonWidget.h"
#include "UI/FGGameUI.h"
#include "UnrealClient.h"
#include "UObject/UnrealType.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY_STATIC(LogSaveCheckMenu, Log, All);

namespace
{
bool HasText(SWidget& Widget, FStringView Label)
{
    if (Widget.GetType() == TEXT("STextBlock")
        && FStringView(static_cast<STextBlock&>(Widget).GetText().ToString()) == Label) return true;
    FChildren* Children = Widget.GetChildren();
    if (Children)
        for (int32 Index = 0; Index < Children->Num(); ++Index)
            if (HasText(Children->GetChildAt(Index).Get(), Label)) return true;
    return false;
}

TSharedPtr<SWidget> FindButton(const TSharedRef<SWidget>& Widget, FStringView Label)
{
    if (Widget->GetType() == TEXT("SButton") && HasText(Widget.Get(), Label)) return Widget;
    FChildren* Children = Widget->GetChildren();
    if (Children)
        for (int32 Index = 0; Index < Children->Num(); ++Index)
            if (auto Button = FindButton(Children->GetChildAt(Index), Label)) return Button;
    return nullptr;
}

TSharedPtr<SWidget> FindMenuButton(FStringView Label)
{
    if (!GEngine || !GEngine->GameViewport || !FSlateApplication::IsInitialized()) return nullptr;
    const auto Viewport = GEngine->GameViewport->GetGameViewportWidget();
    const auto Window = Viewport ? FSlateApplication::Get().FindWidgetWindow(Viewport.ToSharedRef()) : nullptr;
    return Window ? FindButton(Window.ToSharedRef(), Label) : nullptr;
}

bool SlateKey(FKey Key)
{
    const FKeyEvent Event(Key, FModifierKeysState(), FInputDeviceId::CreateFromInternalId(0), false, 0, 0, 0);
    auto& Slate = FSlateApplication::Get();
    const bool bDown = Slate.ProcessKeyDownEvent(Event);
    return Slate.ProcessKeyUpEvent(Event) && bDown;
}
}

bool ClickPlanMenuButton(FStringView Label)
{
    const auto Button = FindMenuButton(Label);
    if (!Button) return false;
    // Offscreen windows cannot always take Slate focus. Exercise the rendered button's normal Accept handler.
    for (const FKey& Key : { EKeys::Enter, EKeys::SpaceBar, EKeys::Virtual_Accept })
    {
        const FKeyEvent Event(Key, FModifierKeysState(), FInputDeviceId::CreateFromInternalId(0), false, 0, 0, 0);
        if (FSlateApplication::Get().GetNavigationActionFromKey(Event) != EUINavigationAction::Accept) continue;
        const FGeometry& Geometry = Button->GetTickSpaceGeometry();
        const bool bPressed = Button->OnKeyDown(Geometry, Event).IsEventHandled();
        return Button->OnKeyUp(Geometry, Event).IsEventHandled() && bPressed;
    }
    return false;
}

namespace
{
struct FPlanMenuRecord
{
    FGuid Id;
    FTransform Pose;
    FString Name;
    FVector AimPoint;
};

TArray<FPlanMenuRecord> ReadMenuPlans(UWorld& World)
{
    TArray<FPlanMenuRecord> Result;
    auto* Type = FindObject<UClass>(nullptr, TEXT("/Script/ShadowArchitect.ShadowPlanSubsystem"));
    if (!Type) return Result;
    TActorIterator<AActor> Subsystem(&World, Type);
    const auto* Plans = FindFProperty<FArrayProperty>(Type, TEXT("Plans"));
    const auto* RecordType = CastField<FStructProperty>(Plans ? Plans->Inner : nullptr);
    if (!Subsystem || !RecordType) return Result;
    const auto* Id = FindFProperty<FStructProperty>(RecordType->Struct, TEXT("Id"));
    const auto* Transform = FindFProperty<FStructProperty>(RecordType->Struct, TEXT("Transform"));
    const auto* Name = FindFProperty<FStrProperty>(RecordType->Struct, TEXT("BlueprintId"));
    const auto* Parts = FindFProperty<FArrayProperty>(RecordType->Struct, TEXT("Parts"));
    const auto* PartType = CastField<FStructProperty>(Parts ? Parts->Inner : nullptr);
    if (!Id || !Transform || !Name || !PartType) return Result;
    const auto* PartTransform = FindFProperty<FStructProperty>(PartType->Struct, TEXT("InPlan"));
    const auto* Center = FindFProperty<FStructProperty>(PartType->Struct, TEXT("BoundsCenter"));
    if (!PartTransform || !Center) return Result;

    FScriptArrayHelper Records(Plans, Plans->ContainerPtrToValuePtr<void>(*Subsystem));
    for (int32 Index = 0; Index < Records.Num(); ++Index)
    {
        const void* Record = Records.GetRawPtr(Index);
        FScriptArrayHelper PlanParts(Parts, Parts->ContainerPtrToValuePtr<void>(Record));
        if (PlanParts.Num() == 0) return {};
        const void* FirstPart = PlanParts.GetRawPtr(0);
        auto& Entry = Result.AddDefaulted_GetRef();
        Entry.Id = *Id->ContainerPtrToValuePtr<FGuid>(Record);
        Entry.Pose = *Transform->ContainerPtrToValuePtr<FTransform>(Record);
        Entry.Name = Name->GetPropertyValue_InContainer(Record);
        Entry.AimPoint = Entry.Pose.TransformPosition(
            PartTransform->ContainerPtrToValuePtr<FTransform>(FirstPart)->TransformPosition(
                *Center->ContainerPtrToValuePtr<FVector>(FirstPart)));
    }
    return Result;
}

struct FMenuProbe
{
    enum class EStep
    {
        Ready, SelectA, TargetB, Frame, Close, Miss, TargetMove, BeginMove, PlacePreview,
        CancelMove, TargetAgain, BeginAgain, PositionAgain, Reopen, Confirm, AtDestination,
        OldKey, NewKey, Remove, Result
    };
    EStep Step = EStep::Ready;
    int32 Age = 0;
    int32 Total = 0;
    bool bPressed = false;
    bool bRequestedFrame = false;
    int32 Approach = 0;
    FInputChord MenuInput;
    FInputChord SelectInput;
    FKey ReboundMenuKey = EKeys::F5;
    FString Frame;
    TArray<FPlanMenuRecord> Initial;
    TWeakObjectPtr<ACameraActor> Camera;
    FVector BuildAim;
    FTransform MovePose;
    FVector Anchor;
    FHitResult Foundation;
    FString ActionsMode;
    enum class EActionStep
    {
        Ready, SelectA, SelectB, Preview, PreviousKeys, RestorePreview, Move, Hide, Show, View, Delete, Result,
        Chat, CloseChat, OpenOptions, Options, CloseOptions, SafetyResult
    };
    EActionStep ActionStep = EActionStep::Ready;
    int32 OldActionIndex = 0;
    int32 InitialSelectionMessages = 0;

    void Next(EStep State) { Step = State; Age = 0; }

    static bool Finish(bool bPassed, const TCHAR* Reason)
    {
        UE_LOG(LogSaveCheckMenu, Display, TEXT("pass=%d %s"), bPassed, Reason);
        FPlatformMisc::RequestExit(false);
        return false;
    }

    static FInputChord ReadChord(APlayerController& Player, FName Mapping)
    {
        FKey Primary;
        TArray<FKey> Modifiers;
        if (!UFGInputLibrary::GetCurrentMappingForAction(&Player, Mapping, Primary, Modifiers)) return {};
        return FInputChord(Primary,
            Modifiers.Contains(EKeys::LeftShift) || Modifiers.Contains(EKeys::RightShift),
            Modifiers.Contains(EKeys::LeftControl) || Modifiers.Contains(EKeys::RightControl),
            Modifiers.Contains(EKeys::LeftAlt) || Modifiers.Contains(EKeys::RightAlt),
            Modifiers.Contains(EKeys::LeftCommand) || Modifiers.Contains(EKeys::RightCommand));
    }

    bool Stroke(APlayerController& Player, const FInputChord& Chord)
    {
        auto Send = [&Player](FKey Input, EInputEvent Event)
        {
            Player.InputKey(FInputKeyEventArgs(GEngine->GameViewport->Viewport,
                FInputDeviceId::CreateFromInternalId(0), Input, Event, FPlatformTime::Cycles64()));
        };
        const FKey Modifiers[] = {
            Chord.bCtrl ? EKeys::LeftControl : EKeys::Invalid,
            Chord.bShift ? EKeys::LeftShift : EKeys::Invalid,
            Chord.bAlt ? EKeys::LeftAlt : EKeys::Invalid,
            Chord.bCmd ? EKeys::LeftCommand : EKeys::Invalid
        };
        if (!bPressed)
            for (const auto& Key : Modifiers)
                if (Key.IsValid()) Send(Key, IE_Pressed);
        Send(Chord.Key, bPressed ? IE_Released : IE_Pressed);
        if (bPressed)
            for (const auto& Key : Modifiers)
                if (Key.IsValid()) Send(Key, IE_Released);
        bPressed = !bPressed;
        return !bPressed;
    }

    bool Stroke(APlayerController& Player, FKey Key, bool bCtrlShift = false)
    {
        return Stroke(Player, FInputChord(Key, bCtrlShift, bCtrlShift, false, false));
    }

    bool Aim(APlayerController& Player, const FVector& At, bool bMiss = false)
    {
        // The saved 3×3 platform covers some approaches to the cable assemblers.
        const FVector Offsets[] = {
            FVector(350, -900, 200), FVector(-350, -900, 150), FVector(900, 0, 100),
            FVector(0, -700, 50), FVector(350, 0, 600)
        };
        if (Approach >= UE_ARRAY_COUNT(Offsets)) return false;
        const FVector Eye = At + (bMiss ? FVector(0, 0, 3000) : Offsets[Approach]);
        const FRotator Rotation = bMiss ? FRotator(85, 0, 0) : (At - Eye).Rotation();
        if (!Camera.IsValid()) Camera = Player.GetWorld()->SpawnActor<ACameraActor>(Eye, Rotation);
        if (!Camera.IsValid()) return false;
        Camera->SetActorLocationAndRotation(Eye, Rotation);
        Player.SetViewTarget(Camera.Get());
        UE_LOG(LogSaveCheckMenu, Display, TEXT("camera_cm=%s look=%s"), *Eye.ToString(), *Rotation.ToString());
        return true;
    }

    bool CheckTarget(const FPlanMenuRecord& Plan) const
    {
        const auto Cancel = FindMenuButton(TEXT("Cancel (Esc)"));
        const auto Panel = Cancel ? Cancel->GetParentWidget() : nullptr;
        return Panel && HasText(*Panel, Plan.Name);
    }

    bool CheckPoses(UWorld& World, bool bMoved = false, bool bDeleted = false) const
    {
        const auto Current = ReadMenuPlans(World);
        if (Current.Num() != (bDeleted ? 1 : 2) || Current[0].Id != Initial[0].Id
            || !Current[0].Pose.Equals(Initial[0].Pose, 0.01)) return false;
        return bDeleted || (Current[1].Id == Initial[1].Id
            && Current[1].Pose.Equals(bMoved ? MovePose : Initial[1].Pose, 0.01));
    }

    bool TraceSurface(UWorld& World, AActor& Player, const FVector& Point,
        ECollisionChannel Channel, FHitResult& Hit) const
    {
        const FCollisionQueryParams Query(SCENE_QUERY_STAT(ShadowMenuProbe), false, &Player);
        return World.LineTraceSingleByChannel(Hit, Point + FVector(0, 0, 1800),
            Point - FVector(0, 0, 800), Channel, Query) && Hit.ImpactNormal.Z > 0.9;
    }

    bool OpenBlueprint(UWorld& World, APlayerController& Player, AFGCharacterPlayer& Character)
    {
        TArray<UFGBlueprintDescriptor*> Library;
        AFGBlueprintSubsystem::GetBlueprintDescriptors(Library, &World);
        if (Library.Num() != 1 || !Library[0]) return false;
        const FVector Point = Anchor + FVector(3200, 2400, 0);
        FHitResult Floor;
        if (!TraceSurface(World, Character, Point, ECC_GameTraceChannel5, Floor)
            || !TraceSurface(World, Character, Point - FVector(500, 0, 0), ECC_Visibility, Foundation))
            return false;
        const double Height = Character.GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 12;
        if (!Character.TeleportTo(Foundation.ImpactPoint + FVector(0, 0, Height), FRotator::ZeroRotator))
            return false;
        BuildAim = Floor.ImpactPoint;
        Player.SetViewTarget(&Character);
        Player.SetControlRotation((BuildAim - Character.GetPawnViewLocation()).Rotation());
        if (!Preview(Character)) Character.HotKeyBlueprint(Library[0]->GetBlueprintNameAsString());
        return true;
    }

    static AFGBlueprintHologram* Preview(AFGCharacterPlayer& Character)
    {
        auto* Gun = Character.GetBuildGun();
        auto* State = Gun && Gun->IsEquipped() ? Cast<UFGBuildGunStateBuild>(Gun->GetCurrentState()) : nullptr;
        auto* Hologram = State ? Cast<AFGBlueprintHologram>(State->GetHologram()) : nullptr;
        return Hologram && !Hologram->IsHidden() && !Hologram->IsDisabled() ? Hologram : nullptr;
    }

    static bool CheckViews(UWorld& World, bool bVisible, bool bWireframe)
    {
        const auto* Type = FindObject<UClass>(nullptr, TEXT("/Script/ShadowArchitect.ShadowProjection"));
        if (!Type) return false;
        int32 Count = 0;
        for (TActorIterator<AActor> It(&World); It; ++It)
        {
            const AActor* View = *It;
            if (View->GetClass() != Type) continue;
            ++Count;
            if (View->IsHidden() == bVisible) return false;
            const TInlineComponentArray<UStaticMeshComponent*> Meshes(View);
            if (Meshes.IsEmpty()) return false;
            for (auto* Mesh : Meshes)
            {
                const auto* Material = Mesh->GetMaterial(0);
                if (!Material) return false;
                const FName Name = Material->GetFName();
                if (bWireframe ? Name != TEXT("M_ShadowWireframe") && Name != TEXT("M_ShadowBoxEdges")
                    : Name != TEXT("Hologram_Blue_Inst")) return false;
            }
        }
        return Count == 2;
    }

    static int32 SelectionMessages(UWorld& World, FGuid& LastSelected)
    {
        auto* Chat = AFGChatManager::Get(&World);
        if (!Chat) return 0;
        TArray<FChatMessageStruct> Messages;
        Chat->GetReceivedChatMessages(Messages);
        int32 Count = 0;
        for (const auto& Message : Messages)
        {
            const FString Text = Message.MessageText.ToString();
            if (Message.MessageSender.ToString() != TEXT("Shadow Architect") || !Text.StartsWith(TEXT("Selected ")))
                continue;
            int32 End = INDEX_NONE;
            const int32 Start = Text.Find(TEXT(" ["), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
            if (Start != INDEX_NONE && Text.FindLastChar(TEXT(']'), End)
                && FGuid::Parse(Text.Mid(Start + 2, End - Start - 2), LastSelected)) ++Count;
        }
        return Count;
    }

    static bool HasMoveCancellation(UWorld& World, const FGuid& Id)
    {
        auto* Chat = AFGChatManager::Get(&World);
        if (!Chat) return false;
        TArray<FChatMessageStruct> Messages;
        Chat->GetReceivedChatMessages(Messages);
        const FString Expected = FString::Printf(TEXT("Cancelled move for plan [%s]."), *Id.ToString());
        return Messages.ContainsByPredicate([&Expected](const FChatMessageStruct& Message)
        {
            return Message.MessageSender.ToString() == TEXT("Shadow Architect") && Message.MessageText.ToString() == Expected;
        });
    }

    static void LogMenuInputState(APlayerController& Player, const TCHAR* StepName)
    {
        auto* FG = Cast<AFGPlayerController>(&Player);
        auto* UI = FG ? FG->GetGameUI() : nullptr;
        auto& Slate = FSlateApplication::Get();
        const auto Keyboard = Slate.GetKeyboardFocusedWidget();
        const auto User = Slate.GetUserFocusedWidget(0);
        UE_LOG(LogSaveCheckMenu, Display,
            TEXT("step=%s keyboard_focus=%s user_focus=%s cursor=%d ignore=%d popup=%d interact=%d pause=%d"),
            StepName, Keyboard ? *Keyboard->GetTypeAsString() : TEXT("none"),
            User ? *User->GetTypeAsString() : TEXT("none"),
            Player.bShowMouseCursor, GEngine->GameViewport->IgnoreInput(),
            UI && UI->GetPopup(), UI && UI->HasActiveInteractWidget(), UI && UI->IsPauseMenuOpen());
    }

    bool TickActions(UWorld& World, APlayerController& Player, AFGCharacterPlayer& Character)
    {
        static const FName ActionNames[] = {
            TEXT("ShadowArchitect.Select"), TEXT("ShadowArchitect.Move"), TEXT("ShadowArchitect.Delete"),
            TEXT("ShadowArchitect.Visibility"), TEXT("ShadowArchitect.View")
        };
        const FKey Defaults[] = { EKeys::J, EKeys::M, EKeys::K, EKeys::V, EKeys::G };
        const FKey Rebound[] = { EKeys::F2, EKeys::F3, EKeys::F4, EKeys::F5, EKeys::F9 };
        const bool bSafety = ActionsMode == TEXT("safety");
        const bool bPlain = ActionsMode == TEXT("ui");
        const bool bCustom = bSafety || bPlain || ActionsMode == TEXT("custom");
        const auto* Keys = bCustom ? Rebound : Defaults;
        if (++Age > 65)
        {
            UE_LOG(LogSaveCheckMenu, Error, TEXT("actions=%s timeout step=%d"),
                *ActionsMode, static_cast<int32>(ActionStep));
            return Finish(false, TEXT("plan action step timeout"));
        }
        auto Advance = [this](EActionStep NextStep) { ActionStep = NextStep; Age = 0; };
        auto Report = [this, Keys, bPlain](int32 Index, const TCHAR* Result)
        {
            const FInputChord Chord(Keys[Index], !bPlain, !bPlain, false, false);
            UE_LOG(LogSaveCheckMenu, Display, TEXT("actions=%s input=%s %s"),
                *ActionsMode, *Chord.GetInputText().ToString(), Result);
        };
        auto CloseStockUI = [&Player]()
        {
            if (!Player.bShowMouseCursor && !GEngine->GameViewport->IgnoreInput() && !Player.IsPaused()) return false;
            if (auto* PC = Cast<AFGPlayerController>(&Player))
                if (auto* UI = PC->GetGameUI()) UI->Native_HandlePauseGamePressed();
            return true;
        };
        switch (ActionStep)
        {
        case EActionStep::Ready:
        {
            if (Age < 10) break;
            for (int32 Index = 0; Index < UE_ARRAY_COUNT(ActionNames); ++Index)
            {
                const FInputChord Mapping = ReadChord(Player, ActionNames[Index]);
                const FInputChord Expected(Keys[Index], !bPlain, !bPlain, false, false);
                if (Mapping != Expected)
                {
                    UE_LOG(LogSaveCheckMenu, Error, TEXT("actions=%s mapping=%s actual=%s expected=%s"),
                        *ActionsMode, *ActionNames[Index].ToString(), *Mapping.GetInputText().ToString(),
                        *Expected.GetInputText().ToString());
                    return Finish(false, TEXT("unexpected action mapping"));
                }
            }
            if (ActionsMode == TEXT("rebind"))
            {
                for (int32 Index = 0; Index < UE_ARRAY_COUNT(ActionNames); ++Index)
                    UFGInputLibrary::RebindEnhancedKeyMapping(&Player, ActionNames[Index], Rebound[Index],
                        { EKeys::LeftControl, EKeys::LeftShift });
                UGameUserSettings::GetGameUserSettings()->SaveSettings();
                return Finish(true, TEXT("saved five assignments for restart"));
            }
            Initial = ReadMenuPlans(World);
            if (Initial.Num() != 2 || !CheckViews(World, true, false))
                return Finish(false, TEXT("expected two visible Ghost plans"));
            for (const auto& Plan : Initial)
                UE_LOG(LogSaveCheckMenu, Display, TEXT("actions=%s before id=%s transform=%s"),
                    *ActionsMode, *Plan.Id.ToString(), *Plan.Pose.ToString());
            Anchor = Initial[0].Pose.GetLocation();
            FGuid Last;
            InitialSelectionMessages = SelectionMessages(World, Last);
            Advance(EActionStep::SelectA);
            break;
        }
        case EActionStep::SelectA:
        case EActionStep::SelectB:
        {
            if (!Stroke(Player, Keys[0], !bPlain)) break;
            const int32 Index = ActionStep == EActionStep::SelectA ? 0 : 1;
            FGuid Last;
            if (SelectionMessages(World, Last) != InitialSelectionMessages + Index + 1 || Last != Initial[Index].Id)
                return Finish(false, TEXT("select did not advance exactly once"));
            Report(0, *FString::Printf(TEXT("selected=%s"), *Last.ToString()));
            if (Index == 0) Advance(EActionStep::SelectB);
            else if (bSafety)
            {
                auto* Chat = Player.FindFunction(TEXT("OnChat"));
                if (!Chat) return Finish(false, TEXT("stock chat action unavailable"));
                Player.ProcessEvent(Chat, nullptr);
                Advance(EActionStep::Chat);
            }
            else
            {
                if (!OpenBlueprint(World, Player, Character)) return Finish(false, TEXT("open stock Blueprint"));
                Advance(EActionStep::Preview);
            }
            break;
        }
        case EActionStep::Preview:
            Player.SetControlRotation((BuildAim - Character.GetPawnViewLocation()).Rotation());
            if (auto* Hologram = Preview(Character); Hologram && Hologram->CanLockHologram())
            {
                Hologram->LockHologramPosition(true);
                if (!Hologram->IsHologramLocked()) break;
                MovePose = Hologram->GetActorTransform();
                Advance(bCustom && OldActionIndex < UE_ARRAY_COUNT(Defaults)
                    ? EActionStep::PreviousKeys : EActionStep::Move);
            }
            break;
        case EActionStep::PreviousKeys:
        {
            if (!bPressed && CloseStockUI()) break;
            if (!Stroke(Player, Defaults[OldActionIndex], true)) break;
            FGuid Last;
            if (!CheckPoses(World) || !CheckViews(World, true, false)
                || SelectionMessages(World, Last) != InitialSelectionMessages + 2 || Last != Initial[1].Id)
                return Finish(false, TEXT("previous input still activated an action"));
            UE_LOG(LogSaveCheckMenu, Display, TEXT("old_input=Ctrl+Shift+%s inactive=1"),
                *Defaults[OldActionIndex].ToString());
            if (++OldActionIndex == UE_ARRAY_COUNT(Defaults)) Advance(EActionStep::RestorePreview);
            break;
        }
        case EActionStep::RestorePreview:
            if (CloseStockUI()) break;
            if (const auto* Hologram = Preview(Character); Hologram && Hologram->IsHologramLocked())
            {
                MovePose = Hologram->GetActorTransform();
                Advance(EActionStep::Move);
            }
            else
            {
                if (!OpenBlueprint(World, Player, Character)) return Finish(false, TEXT("restore stock Blueprint"));
                Advance(EActionStep::Preview);
            }
            break;
        case EActionStep::Move:
            if (!bPressed)
            {
                const auto Focus = FSlateApplication::Get().GetKeyboardFocusedWidget();
                UE_LOG(LogSaveCheckMenu, Display, TEXT("move_preview=%d cursor=%d focus=%s"),
                    Preview(Character) != nullptr, Player.bShowMouseCursor,
                    Focus ? *Focus->GetTypeAsString() : TEXT("none"));
                if (const auto* Input = Cast<UFGPlayerInput>(Player.PlayerInput))
                    for (const auto& Mapping : Input->GetEnhancedActionMappings())
                        if (Mapping.Key == Keys[1])
                            UE_LOG(LogSaveCheckMenu, Display, TEXT("active_move_key=%s action=%s triggers=%d"),
                                *Mapping.Key.ToString(), *GetNameSafe(Mapping.Action.Get()), Mapping.Triggers.Num());
            }
            if (!Stroke(Player, Keys[1], !bPlain)) break;
            if (!CheckPoses(World, true)) return Finish(false, TEXT("Move did not affect only B"));
            Report(1, *FString::Printf(TEXT("moved=%s transform=%s"), *Initial[1].Id.ToString(), *MovePose.ToString()));
            Character.UnequipBuildGun();
            Advance(EActionStep::Hide);
            break;
        case EActionStep::Hide:
        case EActionStep::Show:
        {
            if (!Stroke(Player, Keys[3], !bPlain)) break;
            const bool bVisible = ActionStep == EActionStep::Show;
            if (!CheckViews(World, bVisible, false)) return Finish(false, TEXT("Visibility did not toggle both plans"));
            Report(3, bVisible ? TEXT("visible=2") : TEXT("visible=0"));
            Advance(bVisible ? EActionStep::View : EActionStep::Show);
            break;
        }
        case EActionStep::View:
            if (!Stroke(Player, Keys[4], !bPlain)) break;
            if (!CheckViews(World, true, true)) return Finish(false, TEXT("View did not advance to Wireframe"));
            Report(4, TEXT("views=2 Ghost->Wireframe"));
            Advance(EActionStep::Delete);
            break;
        case EActionStep::Delete:
            if (Stroke(Player, Keys[2], !bPlain)) Advance(EActionStep::Result);
            break;
        case EActionStep::Result:
            if (!CheckPoses(World, true, true)) return Finish(false, TEXT("Delete did not affect only B"));
            Report(2, *FString::Printf(TEXT("deleted=%s remaining=%s"),
                *Initial[1].Id.ToString(), *Initial[0].Id.ToString()));
            return Finish(true, TEXT("five plan actions"));
        case EActionStep::Chat:
        {
            const auto Focus = FSlateApplication::Get().GetKeyboardFocusedWidget();
            if (!Focus || !Focus->GetTypeAsString().Contains(TEXT("EditableText"))) break;
            if (Stroke(Player, Keys[2], !bPlain)) Advance(EActionStep::CloseChat);
            break;
        }
        case EActionStep::CloseChat:
            if (!CheckPoses(World)) return Finish(false, TEXT("delete changed plans while typing in chat"));
            Report(2, TEXT("chat=1 plans=2 unchanged=1"));
            SlateKey(EKeys::Escape);
            Advance(EActionStep::OpenOptions);
            break;
        case EActionStep::OpenOptions:
        {
            auto* PC = Cast<AFGPlayerController>(&Player);
            auto* UI = PC ? PC->GetGameUI() : nullptr;
            if (!UI) return Finish(false, TEXT("stock game UI unavailable"));
            if (!UI->IsPauseMenuOpen() && Age % 6 == 1) UI->Native_HandlePauseGamePressed();
            if (UI->IsPauseMenuOpen())
            {
                auto* Pause = UI->GetPauseMenu();
                auto* Options = Pause ? Pause->GetWidgetFromName(TEXT("mOptions")) : nullptr;
                if (auto* Button = Cast<UFGButtonWidget>(Options))
                {
                    Button->SimulateKeyPress();
                    Advance(EActionStep::Options);
                }
                else if (auto* NativeButton = Cast<UButton>(Options))
                {
                    NativeButton->OnClicked.Broadcast();
                    Advance(EActionStep::Options);
                }
                else if (ClickPlanMenuButton(TEXT("Options")) || ClickPlanMenuButton(TEXT("OPTIONS")))
                {
                    Advance(EActionStep::Options);
                }
                else if (auto* Open = Pause ? Pause->FindFunction(TEXT("OnOptionsClicked")) : nullptr;
                    Open && Open->ParmsSize == 0)
                {
                    Pause->ProcessEvent(Open, nullptr);
                    Advance(EActionStep::Options);
                }
            }
            break;
        }
        case EActionStep::Options:
        {
            if (!bPressed)
            {
                auto* PC = Cast<AFGPlayerController>(&Player);
                auto* UI = PC ? PC->GetGameUI() : nullptr;
                auto* Pause = UI ? UI->GetPauseMenu() : nullptr;
                auto* Settings = Pause ? Pause->GetWidgetFromName(TEXT("Widget_Options")) : nullptr;
                if (!Settings && Pause) Settings = Pause->GetWidgetFromName(TEXT("mOptionsList"));
                const auto Widget = Settings ? Settings->GetCachedWidget() : nullptr;
                FWidgetPath Path;
                if (!Settings || !Settings->IsVisible() || !Widget
                    || !FSlateApplication::Get().GeneratePathToWidgetUnchecked(Widget.ToSharedRef(), Path)) break;
                UE_LOG(LogSaveCheckMenu, Display, TEXT("visible_options=%s"), *Settings->GetName());
            }
            if (Stroke(Player, Keys[2], !bPlain)) Advance(EActionStep::CloseOptions);
            break;
        }
        case EActionStep::CloseOptions:
        {
            if (!CheckPoses(World)) return Finish(false, TEXT("delete changed plans in options"));
            Report(2, TEXT("options=1 plans=2 unchanged=1"));
            auto* PC = Cast<AFGPlayerController>(&Player);
            auto* UI = PC ? PC->GetGameUI() : nullptr;
            if (!UI) return Finish(false, TEXT("stock game UI unavailable"));
            auto* Pause = UI->GetPauseMenu();
            auto* Resume = Pause ? Pause->FindFunction(TEXT("DoResume")) : nullptr;
            if (Resume && Resume->ParmsSize == 0) Pause->ProcessEvent(Resume, nullptr);
            else UI->ResumeGame();
            Advance(EActionStep::SafetyResult);
            break;
        }
        case EActionStep::SafetyResult:
            if (Player.bShowMouseCursor || GEngine->GameViewport->IgnoreInput() || Player.IsPaused())
            {
                if (Age % 6 == 1) CloseStockUI();
                break;
            }
            return Finish(CheckPoses(World), TEXT("chat and options preserve both GUIDs and poses"));
        }
        return true;
    }

    bool TickWorld(UWorld& World, APlayerController& Player, AFGCharacterPlayer& Character)
    {
        if (++Age > 65)
        {
            UE_LOG(LogSaveCheckMenu, Error, TEXT("timeout step=%d cursor=%d"), static_cast<int32>(Step), Player.bShowMouseCursor);
            return Finish(false, TEXT("step timeout"));
        }
        switch (Step)
        {
        case EStep::Ready:
            if (Age < 50) break;
            MenuInput = ReadChord(Player, TEXT("ShadowArchitect.OpenPlanMenu"));
            SelectInput = ReadChord(Player, TEXT("ShadowArchitect.Select"));
            ReboundMenuKey = MenuInput.Key == EKeys::F10 ? EKeys::F6 : EKeys::F5;
            if (!MenuInput.IsValidChord() || !SelectInput.IsValidChord() || MenuInput == FInputChord(ReboundMenuKey))
                return Finish(false, TEXT("menu and select require assigned keys distinct from the final test key"));
            UE_LOG(LogSaveCheckMenu, Display, TEXT("initial_open=%s initial_select=%s"),
                *MenuInput.GetInputText().ToString(), *SelectInput.GetInputText().ToString());
            Initial = ReadMenuPlans(World);
            if (Initial.Num() != 2) return Finish(false, TEXT("expected two plans"));
            Anchor = Initial[0].Pose.GetLocation();
            for (const auto& Plan : Initial)
                UE_LOG(LogSaveCheckMenu, Display, TEXT("before id=%s title=%s transform=%s"),
                    *Plan.Id.ToString(), *Plan.Name, *Plan.Pose.ToString());
            if (!Aim(Player, Initial[1].AimPoint)) return Finish(false, TEXT("camera"));
            Next(EStep::SelectA);
            break;
        case EStep::SelectA:
            if (Stroke(Player, SelectInput)) Next(EStep::TargetB);
            break;
        case EStep::TargetB:
            if (Stroke(Player, MenuInput)) Next(EStep::Frame);
            break;
        case EStep::Frame:
            if (!CheckTarget(Initial[1]))
            {
                if (Age < 5) break;
                if (FindMenuButton(TEXT("Cancel (Esc)")) && !SlateKey(EKeys::Escape))
                    return Finish(false, TEXT("could not dismiss menu for overlapping plan"));
                ++Approach;
                if (!Aim(Player, Initial[1].AimPoint)) return Finish(false, TEXT("no clear approach to B"));
                Next(EStep::TargetB);
                break;
            }
            if (!bRequestedFrame)
            {
                FScreenshotRequest::RequestScreenshot(Frame, true, false);
                bRequestedFrame = true;
                break;
            }
            if (IFileManager::Get().FileSize(*Frame) > 0) Next(EStep::Close);
            break;
        case EStep::Close:
            if (!SlateKey(EKeys::Escape)) return Finish(false, TEXT("Escape was not handled"));
            Next(EStep::Miss);
            break;
        case EStep::Miss:
            if (Age == 1)
            {
                if (Player.bShowMouseCursor || GEngine->GameViewport->IgnoreInput()
                    || !CheckPoses(World) || !Aim(Player, Initial[1].AimPoint, true))
                    return Finish(false, TEXT("Escape did not restore the game and both poses"));
                UE_LOG(LogSaveCheckMenu, Display, TEXT("input=Escape modal_closed=1 unchanged=1"));
                break;
            }
            if (Stroke(Player, MenuInput))
            {
                if (FindMenuButton(TEXT("Cancel (Esc)")) || !Aim(Player, Initial[1].AimPoint))
                    return Finish(false, TEXT("crosshair miss opened an action"));
                UE_LOG(LogSaveCheckMenu, Display, TEXT("input=%s crosshair_miss=1"), *MenuInput.GetInputText().ToString());
                Next(EStep::TargetMove);
            }
            break;
        case EStep::TargetMove:
        case EStep::TargetAgain:
            if (Age == 1 && Step == EStep::TargetAgain) LogMenuInputState(Player, TEXT("before second menu opening"));
            if (Stroke(Player, MenuInput))
                Next(Step == EStep::TargetMove ? EStep::BeginMove : EStep::BeginAgain);
            break;
        case EStep::BeginMove:
        case EStep::BeginAgain:
            if (!ClickPlanMenuButton(TEXT("Move"))) break;
            if (!OpenBlueprint(World, Player, Character)) return Finish(false, TEXT("open stock Blueprint"));
            Next(Step == EStep::BeginMove ? EStep::PlacePreview : EStep::PositionAgain);
            break;
        case EStep::PlacePreview:
        case EStep::PositionAgain:
            Player.SetControlRotation((BuildAim - Character.GetPawnViewLocation()).Rotation());
            if (auto* Hologram = Preview(Character))
            {
                MovePose = Hologram->GetActorTransform();
                if (!MovePose.Equals(Initial[1].Pose, 0.1))
                {
                    if (!Hologram->IsHologramLocked() && Hologram->CanLockHologram())
                        Hologram->LockHologramPosition(true);
                    if (Hologram->IsHologramLocked())
                        Next(Step == EStep::PlacePreview ? EStep::CancelMove : EStep::Reopen);
                }
            }
            break;
        case EStep::CancelMove:
            LogMenuInputState(Player, TEXT("before pending Escape"));
            if (!SlateKey(EKeys::Escape) || !HasMoveCancellation(World, Initial[1].Id)
                || !CheckPoses(World) || !Aim(Player, Initial[1].AimPoint))
                return Finish(false, TEXT("pending move was not cancelled without changes"));
            UE_LOG(LogSaveCheckMenu, Display, TEXT("input=Escape pending_cancelled=1 unchanged=1"));
            Next(EStep::TargetAgain);
            break;
        case EStep::Reopen:
            if (Stroke(Player, MenuInput)) Next(EStep::Confirm);
            break;
        case EStep::Confirm:
            if (!ClickPlanMenuButton(TEXT("Confirm move"))) break;
            Next(EStep::AtDestination);
            break;
        case EStep::AtDestination:
        {
            const auto Current = ReadMenuPlans(World);
            if (!CheckPoses(World, true)) return Finish(false, TEXT("confirmed move did not affect only B"));
            UE_LOG(LogSaveCheckMenu, Display, TEXT("moved id=%s transform=%s"),
                *Current[1].Id.ToString(), *Current[1].Pose.ToString());
            Character.UnequipBuildGun();
            if (!Aim(Player, Current[1].AimPoint)) return Finish(false, TEXT("camera at destination"));
            UFGInputLibrary::RebindEnhancedKeyMapping(&Player, TEXT("ShadowArchitect.OpenPlanMenu"), ReboundMenuKey, {});
            Next(EStep::OldKey);
            break;
        }
        case EStep::OldKey:
            if (!Stroke(Player, MenuInput)) break;
            if (FindMenuButton(TEXT("Cancel (Esc)"))) return Finish(false, TEXT("old menu key opened menu"));
            Next(EStep::NewKey);
            break;
        case EStep::NewKey:
            if (Stroke(Player, ReboundMenuKey)) Next(EStep::Remove);
            break;
        case EStep::Remove:
            if (!ClickPlanMenuButton(TEXT("Delete"))) break;
            UE_LOG(LogSaveCheckMenu, Display, TEXT("input=%s opened rebound menu"), *ReboundMenuKey.ToString());
            Next(EStep::Result);
            break;
        case EStep::Result:
        {
            FHitResult Floor;
            if (!CheckPoses(World, true, true) || !TraceSurface(World, Character, Foundation.ImpactPoint, ECC_Visibility, Floor)
                || !Floor.ImpactPoint.Equals(Foundation.ImpactPoint, 0.1) || Floor.Component != Foundation.Component)
                return Finish(false, TEXT("deleting B changed A or removed foundation"));
            UE_LOG(LogSaveCheckMenu, Display, TEXT("deleted=%s remaining=%s foundation=%s"),
                *Initial[1].Id.ToString(), *Initial[0].Id.ToString(), *GetNameSafe(Floor.GetActor()));
            return Finish(true, TEXT("completed"));
        }
        }
        return true;
    }

    bool Tick()
    {
        if (++Total > 650) return Finish(false, TEXT("world timeout"));
        if (!GEngine || !GEngine->GameViewport || !FSlateApplication::IsInitialized()) return true;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            auto* World = Context.World();
            if (!World || !World->IsGameWorld() || !World->HasBegunPlay()
                || !World->GetMapName().Contains(TEXT("Persistent_Level"))) continue;
            auto* Save = UFGSaveSession::Get(World);
            if (!Save) continue;
            Save->SetAutoSaveEnabled(false);
            auto* Player = World->GetFirstPlayerController();
            auto* Character = Player ? Cast<AFGCharacterPlayer>(Player->GetPawn()) : nullptr;
            if (Character && Player->PlayerInput)
                return ActionsMode.IsEmpty() ? TickWorld(*World, *Player, *Character) : TickActions(*World, *Player, *Character);
        }
        return true;
    }
};
}

FTickerDelegate MakeMenuCheck(FString OutputDir)
{
    IFileManager::Get().MakeDirectory(*OutputDir, true);
    FMenuProbe Probe;
    Probe.Frame = FPaths::ConvertRelativePathToFull(FPaths::Combine(OutputDir, TEXT("Plan-menu.png")));
    return FTickerDelegate::CreateLambda([Probe = MoveTemp(Probe)](float) mutable { return Probe.Tick(); });
}

FTickerDelegate MakePlanActionsCheck(FString Mode)
{
    FMenuProbe Probe;
    Probe.ActionsMode = MoveTemp(Mode);
    return FTickerDelegate::CreateLambda([Probe = MoveTemp(Probe)](float) mutable { return Probe.Tick(); });
}
