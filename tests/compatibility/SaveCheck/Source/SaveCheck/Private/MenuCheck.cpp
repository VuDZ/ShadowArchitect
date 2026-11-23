#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Buildables/FGBuildableFoundation.h"
#include "Components/Button.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "Equipment/FGBuildGunBuild.h"
#include "FGBlueprintSubsystem.h"
#include "FGCharacterPlayer.h"
#include "FGChatManager.h"
#include "FGGameUserSettings.h"
#include "FGInputLibrary.h"
#include "FGPlayerController.h"
#include "FGPlayerInput.h"
#include "FGRecipe.h"
#include "FGSaveInterface.h"
#include "FGSaveSession.h"
#include "FGSaveSystem.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/InputChord.h"
#include "HAL/FileManager.h"
#include "Hologram/FGBlueprintHologram.h"
#include "Interfaces/IPluginManager.h"
#include "InputKeyEventArgs.h"
#include "Layout/Children.h"
#include "Layout/WidgetPath.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "Resources/FGBuildingDescriptor.h"
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
    FBox Bounds = FBox(ForceInit);
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
    const auto* Size = FindFProperty<FStructProperty>(PartType->Struct, TEXT("BoundsSize"));
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
        if (!Size) continue;
        for (int32 PartIndex = 0; PartIndex < PlanParts.Num(); ++PartIndex)
        {
            const auto* Part = PlanParts.GetRawPtr(PartIndex);
            const FVector PartCenter = *Center->ContainerPtrToValuePtr<FVector>(Part);
            const FVector Extent = *Size->ContainerPtrToValuePtr<FVector>(Part) * 0.5;
            Entry.Bounds += FBox(PartCenter - Extent, PartCenter + Extent).TransformBy(
                *PartTransform->ContainerPtrToValuePtr<FTransform>(Part) * Entry.Pose);
        }
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

    bool AimAtBuildSurface(UWorld& World, APlayerController& Player, AFGCharacterPlayer& Character)
    {
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
        return true;
    }

    bool OpenBlueprint(UWorld& World, APlayerController& Player, AFGCharacterPlayer& Character)
    {
        TArray<UFGBlueprintDescriptor*> Library;
        AFGBlueprintSubsystem::GetBlueprintDescriptors(Library, &World);
        if (Library.Num() != 1 || !Library[0] || !AimAtBuildSurface(World, Player, Character)) return false;
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

DEFINE_LOG_CATEGORY_STATIC(LogSaveCheckEditing, Log, All);

namespace
{
struct FEditingProbe : FMenuProbe
{
    enum class EEdit
    {
        Ready, Prepare, Place, CycleA, CycleB, AimA, PickA, PickB, Wall, Hide, HiddenPick, Show,
        MarkA, MarkB, MovePreview, Move, LegacyDismiss, InvalidMove, RefuseMove, SaveMove, AimDelete, DeleteSelect, Delete, DeleteAgain,
        SaveFinal, ReloadPreview, ClearSelect, ClearAsk, ClearCancel, ClearAskAgain, ClearConfirm, ClearHide, ClearHidden, ClearShow
    };

    EEdit Edit = EEdit::Ready;
    FString Mode;
    FString FramesDirectory;
    FString Slot;
    FString MoveSlot;
    FString Blueprint;
    FName RemappedMapAction;
    int32 ExpectedCount = 1;
    int32 Placing = 0;
    bool bRotatedFoundation = false;
    bool bLegacyMoved = true;
    FVector Origin;
    FTransform Fixture;
    FTransform FoundationPose;
    FInputChord PickInput = FInputChord(EKeys::L, true, true, false, false);
    FInputChord ClearInput = FInputChord(EKeys::X, true, true, false, false);
    TWeakObjectPtr<AStaticMeshActor> WallActor;
    TWeakObjectPtr<AFGBuildableFoundation> RealFoundation;
    TSharedPtr<TOptional<bool>> Saved;

    bool Legacy() const { return Mode == TEXT("legacy") || Mode == TEXT("legacy-map"); }
    bool ClearRun() const { return Mode == TEXT("clear"); }

    void Advance(EEdit NextEdit)
    {
        Edit = NextEdit;
        Age = 0;
    }

    static bool End(bool bPass, const TCHAR* Reason)
    {
        UE_LOG(LogSaveCheckEditing, Display, TEXT("pass=%d %s"), bPass, Reason);
        FPlatformMisc::RequestExit(false);
        return false;
    }

    static int32 ChatCount(UWorld& World, const TCHAR* Text)
    {
        auto* Chat = AFGChatManager::Get(&World);
        TArray<FChatMessageStruct> Messages;
        if (Chat) Chat->GetReceivedChatMessages(Messages);
        return Messages.FilterByPredicate([Text](const FChatMessageStruct& Message)
        {
            return Message.MessageSender.ToString() == TEXT("Shadow Architect") && Message.MessageText.ToString() == Text;
        }).Num();
    }

    static bool MatchViews(UWorld& World, const TArray<FPlanMenuRecord>& Records, bool bVisible)
    {
        auto* Type = FindObject<UClass>(nullptr, TEXT("/Script/ShadowArchitect.ShadowProjection"));
        if (!Type) return false;
        int32 Count = 0;
        for (TActorIterator<AActor> View(&World, Type); View; ++View)
        {
            ++Count;
            if (View->IsHidden() == bVisible || !Records.ContainsByPredicate([&View](const auto& Record)
                { return Record.Pose.Equals(View->GetActorTransform(), 0.1); })) return false;
        }
        return Count == Records.Num();
    }

    static bool CheckMarker(UWorld& World, const FPlanMenuRecord* Target, const TCHAR* Case)
    {
        auto* Type = FindObject<UClass>(nullptr, TEXT("/Script/ShadowArchitect.ShadowProjection"));
        const auto* Marker = Type ? FindFProperty<FObjectPropertyBase>(Type, TEXT("SelectionMarker")) : nullptr;
        if (!Marker) return false;
        int32 Visible = 0;
        bool bMatches = false;
        for (TActorIterator<AActor> View(&World, Type); View; ++View)
        {
            const auto* Text = Cast<UTextRenderComponent>(Marker->GetObjectPropertyValue_InContainer(*View));
            if (!IsValid(Text) || !Text->IsVisible() || View->IsHidden()) continue;
            ++Visible;
            bMatches = Target && Target->Pose.Equals(View->GetActorTransform(), 0.1);
        }
        UE_LOG(LogSaveCheckEditing, Display, TEXT("case=%s visible_markers=%d expected_id=%s match=%d"),
            Case, Visible, Target ? *Target->Id.ToString() : TEXT("none"), bMatches);
        return Target ? Visible == 1 && bMatches : Visible == 0;
    }

    static void Report(UWorld& World, const TCHAR* Event)
    {
        const auto Records = ReadMenuPlans(World);
        UE_LOG(LogSaveCheckEditing, Display, TEXT("event=%s plans=%d"), Event, Records.Num());
        for (const auto& Record : Records)
            UE_LOG(LogSaveCheckEditing, Display, TEXT("event=%s id=%s blueprint=%s pose=%s"),
                Event, *Record.Id.ToString(), *Record.Name, *Record.Pose.ToString());
    }

    bool Unchanged(UWorld& World) const
    {
        const auto Records = ReadMenuPlans(World);
        if (Records.Num() != Initial.Num()) return false;
        for (int32 Index = 0; Index < Records.Num(); ++Index)
            if (Records[Index].Id != Initial[Index].Id || !Records[Index].Pose.Equals(Initial[Index].Pose, 0.1)) return false;
        return true;
    }

    static bool ClearInputFixture(UWorld& World)
    {
        auto* Type = FindObject<UClass>(nullptr, TEXT("/Script/ShadowArchitect.ShadowPlanSubsystem"));
        if (!Type) return false;
        TActorIterator<AActor> Subsystem(&World, Type);
        const auto* Plans = FindFProperty<FArrayProperty>(Type, TEXT("Plans"));
        const auto* Visible = FindFProperty<FBoolProperty>(Type, TEXT("bVisible"));
        if (!Subsystem || !Plans || !Visible) return false;
        FScriptArrayHelper Records(Plans, Plans->ContainerPtrToValuePtr<void>(*Subsystem));
        Records.EmptyValues();
        Visible->SetPropertyValue_InContainer(*Subsystem, true);
        IFGSaveInterface::Execute_PostLoadGame(*Subsystem, 0, 0);
        return true;
    }

    bool LookAt(APlayerController& Player, const FVector& Eye, const FVector& Focus)
    {
        const FRotator Rotation = (Focus - Eye).Rotation();
        if (!Camera.IsValid()) Camera = Player.GetWorld()->SpawnActor<ACameraActor>(Eye, Rotation);
        if (!Camera.IsValid()) return false;
        Camera->GetCameraComponent()->SetFieldOfView(90);
        Camera->GetCameraComponent()->SetAspectRatio(16.f / 9.f);
        Camera->GetCameraComponent()->SetConstraintAspectRatio(true);
        Camera->SetActorLocationAndRotation(Eye, Rotation);
        Player.SetViewTarget(Camera.Get());
        UE_LOG(LogSaveCheckEditing, Display, TEXT("camera_cm=%s look=%s"), *Eye.ToString(), *Rotation.ToString());
        return true;
    }

    bool LookEnd(APlayerController& Player, bool bFar)
    {
        const auto& Target = Initial[bFar ? 1 : 0];
        const double Edge = bFar ? Target.Bounds.Max.X : Target.Bounds.Min.X;
        const FVector Eye(Edge + (bFar ? 1200 : -1200), Target.AimPoint.Y, Target.AimPoint.Z);
        return LookAt(Player, Eye, Target.AimPoint);
    }

    bool LookPair(APlayerController& Player)
    {
        const FBox Both = Initial[0].Bounds + Initial[1].Bounds;
        const FVector At = Both.GetCenter();
        const FVector Eye = At + FVector(0, -FMath::Max(Both.GetSize().X, Both.GetSize().Y), Both.GetSize().Z * 0.38);
        return LookAt(Player, Eye, At);
    }

    bool CheckSelection(UWorld& World, int32 Index, const FInputChord& Input, const TCHAR* Case)
    {
        FGuid SelectedId;
        const int32 Count = SelectionMessages(World, SelectedId);
        const bool bMatches = Count == InitialSelectionMessages + 1 && SelectedId == Initial[Index].Id;
        UE_LOG(LogSaveCheckEditing, Display, TEXT("case=%s input=%s selected=%s expected=%s match=%d"),
            Case, *Input.GetInputText().ToString(), *SelectedId.ToString(), *Initial[Index].Id.ToString(), bMatches);
        InitialSelectionMessages = Count;
        return bMatches;
    }

    bool CheckMiss(UWorld& World, const TCHAR* Case, int32 LastMisses)
    {
        FGuid SelectedId;
        const bool bNoSelection = SelectionMessages(World, SelectedId) == InitialSelectionMessages;
        const bool bReported = ChatCount(World, TEXT("No visible plan under the crosshair.")) == LastMisses + 1;
        const bool bUnchanged = Unchanged(World);
        UE_LOG(LogSaveCheckEditing, Display, TEXT("case=%s input=%s miss=%d unchanged=%d"),
            Case, *PickInput.GetInputText().ToString(), bNoSelection && bReported, bUnchanged);
        return bNoSelection && bReported && bUnchanged;
    }

    void OpenFixturePreview(APlayerController& Player, AFGCharacterPlayer& Character)
    {
        Player.SetViewTarget(&Character);
        Player.SetControlRotation(FRotator(-60, 0, 0));
        Character.HotKeyBlueprint(Blueprint);
    }

    static TSubclassOf<UFGRecipe> FoundationRecipe()
    {
        return LoadClass<UFGRecipe>(nullptr,
            TEXT("/Game/FactoryGame/Recipes/Buildings/Foundations/Recipe_Foundation_8x1_01.Recipe_Foundation_8x1_01_C"));
    }

    static AFGHologram* EquippedPreview(AFGCharacterPlayer& Character)
    {
        auto* Gun = Character.GetBuildGun();
        auto* Build = Gun && Gun->IsEquipped() ? Cast<UFGBuildGunStateBuild>(Gun->GetCurrentState()) : nullptr;
        return Build ? Build->GetHologram() : nullptr;
    }

    bool OpenFoundationPreview(UWorld& World, APlayerController& Player, AFGCharacterPlayer& Character)
    {
        const auto Recipe = FoundationRecipe();
        if (!Recipe || !AimAtBuildSurface(World, Player, Character)) return false;
        Character.HotKeyRecipe(Recipe);
        return true;
    }

    static AFGBlueprintHologram* PositionPreview(AFGCharacterPlayer& Character, const FTransform& Target)
    {
        auto* Hologram = Cast<AFGBlueprintHologram>(EquippedPreview(Character));
        if (!Hologram || Hologram->mBuildableToNewRoot.IsEmpty()) return nullptr;
        if (Hologram->IsHologramLocked()) Hologram->LockHologramPosition(false);
        Hologram->SetActorTransform(Target);
        Hologram->LockHologramPosition(true);
        Hologram->SetActorHiddenInGame(false);
        return Hologram->IsHologramLocked() && !Hologram->IsDisabled() ? Hologram : nullptr;
    }

    bool SpawnWall(UWorld& World)
    {
        auto* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        if (!Cube || !Camera.IsValid()) return false;
        const FVector At(Initial[0].Bounds.Min.X - 550, Initial[0].AimPoint.Y, Initial[0].AimPoint.Z);
        auto* Wall = World.SpawnActor<AStaticMeshActor>(At, FRotator::ZeroRotator);
        if (!Wall) return false;
        Wall->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
        Wall->GetStaticMeshComponent()->SetStaticMesh(Cube);
        Wall->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
        Wall->SetActorScale3D(FVector(0.35, 9, 9));
        WallActor = Wall;
        FHitResult Hit;
        const bool bBlocks = World.LineTraceSingleByChannel(Hit, Camera->GetActorLocation(), Initial[0].AimPoint, ECC_Visibility)
            && Hit.GetActor() == Wall;
        UE_LOG(LogSaveCheckEditing, Display, TEXT("opaque_wall=%s blocks_camera_ray=%d"), *At.ToString(), bBlocks);
        return bBlocks;
    }

    bool SpawnFoundation(UWorld& World)
    {
        const auto Recipe = FoundationRecipe();
        if (!Recipe) return false;
        const auto Products = UFGRecipe::GetProducts(Recipe);
        if (Products.IsEmpty()) return false;
        const auto Type = UFGBuildingDescriptor::GetBuildableClass(Products[0].ItemClass.Get());
        if (!Type || !Type->IsChildOf(AFGBuildableFoundation::StaticClass())) return false;
        const FVector At = Initial[0].AimPoint + FVector(450, 0, -50);
        const FTransform Pose(FRotator::ZeroRotator, At);
        auto* Native = World.SpawnActorDeferred<AFGBuildableFoundation>(Type, Pose, nullptr, nullptr,
            ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
        if (!Native) return false;
        Native->SetBuiltWithRecipe(Recipe);
        if (auto* Managed = FindFProperty<FBoolProperty>(AFGBuildable::StaticClass(), TEXT("mManagedByLightweightBuildableSubsystem")))
            Managed->SetPropertyValue_InContainer(Native, false);
        Native->FinishSpawning(Pose);
        RealFoundation = Native;
        FoundationPose = Native->GetActorTransform();
        UE_LOG(LogSaveCheckEditing, Display, TEXT("foundation_class=%s pose=%s"), *Native->GetClass()->GetPathName(),
            *Native->GetActorTransform().ToString());
        return true;
    }

    bool AimForDelete(UWorld& World, APlayerController& Player)
    {
        const auto* Native = RealFoundation.Get();
        if (!Native) return false;
        const FVector At = Initial[0].AimPoint;
        const double Distance = At.X - Initial[0].Bounds.Min.X + 1200;
        const FBox NativeBounds = Native->GetComponentsBoundingBox(true);
        const FVector Center = NativeBounds.IsValid ? NativeBounds.GetCenter() : FoundationPose.GetLocation();
        for (const FVector& Direction : { (Center - At).GetSafeNormal(), FVector(1, 0, 0),
            FVector(1, 0, -0.08), FVector(1, 0, 0.08), FVector(1, 0, -0.16), FVector(1, 0, 0.16) })
        {
            const FVector Eye = At - Direction * Distance;
            FHitResult Hit;
            if (!World.LineTraceSingleByChannel(Hit, Eye, At + Direction * 1100, ECC_Visibility)
                || Hit.GetActor() != Native || FVector::DotProduct(Hit.ImpactPoint - At, Direction) <= 0)
                continue;
            UE_LOG(LogSaveCheckEditing, Display, TEXT("deletion_ray_behind_A=%s hit=%s"),
                *GetNameSafe(Hit.GetActor()), *Hit.ImpactPoint.ToString());
            return LookAt(Player, Eye, At);
        }
        return false;
    }

    bool TakeFrame(const TCHAR* Name) const
    {
        if (FramesDirectory.IsEmpty()) return true;
        const FString Path = FPaths::Combine(FramesDirectory, FString(Name) + TEXT(".png"));
        if (Age == 1) FScreenshotRequest::RequestScreenshot(Path, true, false);
        return IFileManager::Get().FileSize(*Path) > 0;
    }

    bool Write(UWorld& World, const FString& Name)
    {
        FString Existing;
        auto* Save = UFGSaveSession::Get(&World);
        if (!Save || !Name.StartsWith(TEXT("SAQ_")) || UFGSaveSystem::GetAbsolutePathForSaveGame(&World, Name, Existing))
            return false;
        Saved = MakeShared<TOptional<bool>>();
        Save->SaveGame(Name, FOnSaveGameComplete::CreateLambda([Result = Saved, Name](bool Ok, const FText& Error, void*)
        {
            UE_LOG(LogSaveCheckEditing, Display, TEXT("slot=%s write=%d detail=%s"), *Name, Ok, *Error.ToString());
            Result->Emplace(Ok);
        }), nullptr);
        return true;
    }

    bool CheckRemaining(UWorld& World, int32 Index)
    {
        const auto Plans = ReadMenuPlans(World);
        const bool bFoundation = Legacy() || ClearRun()
            || (RealFoundation.IsValid() && RealFoundation->GetActorTransform().Equals(FoundationPose, 0.1));
        return Plans.Num() == 1 && Plans[0].Id == Initial[Index].Id && Plans[0].Pose.Equals(Initial[Index].Pose, 0.1)
            && bFoundation && MatchViews(World, Plans, true);
    }

    bool RemapStockMap(APlayerController& Player)
    {
        const auto* Input = Cast<UFGPlayerInput>(Player.PlayerInput);
        if (!Input) return false;
        for (const auto& Mapping : Input->GetEnhancedActionMappings())
        {
            if (Mapping.Key != EKeys::M || GetFNameSafe(Mapping.Action.Get()) != TEXT("IA_ToggleMap")) continue;
            RemappedMapAction = Mapping.GetMappingName();
            if (RemappedMapAction.IsNone()) return false;
            UFGInputLibrary::RebindEnhancedKeyMapping(&Player, RemappedMapAction, EKeys::F11, {});
            return true;
        }
        return false;
    }

    bool TickEdit(UWorld& World, APlayerController& Player, AFGCharacterPlayer& Character)
    {
        if (++Age > 100)
        {
            UE_LOG(LogSaveCheckEditing, Error, TEXT("mode=%s timeout step=%d placing=%d"),
                *Mode, static_cast<int32>(Edit), Placing);
            return End(false, TEXT("editing step timeout"));
        }
        const FInputChord Cycle(EKeys::J, true, true, false, false);
        switch (Edit)
        {
        case EEdit::Ready:
        {
            if (Age < 10) break;
            if (Mode == TEXT("legacy-map") && RemappedMapAction.IsNone())
            {
                if (!RemapStockMap(Player)) return End(false, TEXT("could not remap the stock map action"));
                Age = 0;
                break;
            }
            if (Mode == TEXT("legacy-map"))
            {
                const auto Mapping = ReadChord(Player, RemappedMapAction);
                UE_LOG(LogSaveCheckEditing, Display, TEXT("stock_map_action=%s remapped=%s"),
                    *RemappedMapAction.ToString(), *Mapping.GetInputText().ToString());
                if (Mapping.Key != EKeys::F11) return End(false, TEXT("stock map did not move from M to F11"));
            }
            const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("ShadowArchitect"));
            const FString Version = Plugin ? Plugin->GetDescriptor().VersionName : TEXT("missing");
            UE_LOG(LogSaveCheckEditing, Display, TEXT("mode=%s version=%s expected_reload=%d"), *Mode, *Version, ExpectedCount);
            if (Legacy() && Version != TEXT("0.1.1")) return End(false, TEXT("legacy requires installed 0.1.1"));
            TArray<UFGBlueprintDescriptor*> Library;
            AFGBlueprintSubsystem::GetBlueprintDescriptors(Library, &World);
            if (Library.Num() != 1 || !Library[0]) return End(false, TEXT("expected one isolated Blueprint"));
            Blueprint = Library[0]->GetBlueprintNameAsString();
            UE_LOG(LogSaveCheckEditing, Display, TEXT("library=%s"), *Blueprint);
            if (Mode == TEXT("reload"))
            {
                const auto Plans = ReadMenuPlans(World);
                Report(World, TEXT("reload"));
                if (Plans.Num() != ExpectedCount || !MatchViews(World, Plans, true))
                    return End(false, TEXT("reloaded plans"));
                OpenFixturePreview(Player, Character);
                Advance(EEdit::ReloadPreview);
                break;
            }
            const auto Baseline = ReadMenuPlans(World);
            Anchor = Baseline.IsEmpty() ? Character.GetActorLocation() : Baseline[0].Pose.GetLocation();
            if (!ClearInputFixture(World)) return End(false, TEXT("reset isolated input plans"));
            Origin = Character.GetActorLocation() + FVector(0, 0, 6200);
            if (Legacy())
            {
                if (!AimAtBuildSurface(World, Player, Character)) return End(false, TEXT("legacy fixture floor"));
                Origin = BuildAim + FVector(-1000, 0, -500);
            }
            Fixture = FTransform(FRotator::ZeroRotator, Origin);
            OpenFixturePreview(Player, Character);
            Advance(EEdit::Prepare);
            break;
        }
        case EEdit::Prepare:
            if (Age > 4 && PositionPreview(Character, Fixture)) Advance(EEdit::Place);
            break;
        case EEdit::Place:
        {
            if (!Stroke(Player, EKeys::F7)) break;
            const auto Plans = ReadMenuPlans(World);
            if (Plans.Num() != Placing + 1 || Plans.Last().Name != Blueprint || !Plans.Last().Bounds.IsValid
                || !Plans.Last().Pose.Equals(Fixture, 0.1) || !MatchViews(World, Plans, true))
                return End(false, TEXT("F7 did not place expected Blueprint and pose"));
            UE_LOG(LogSaveCheckEditing, Display, TEXT("input=F7 placed=%s pose=%s"),
                *Plans.Last().Id.ToString(), *Plans.Last().Pose.ToString());
            Initial = Plans;
            ++Placing;
            if (Placing < (ClearRun() ? 3 : 2))
            {
                const FVector Size = Initial[0].Bounds.GetSize();
                const double ClearDistance = Size.GetMax() * 2 + 1000;
                const FVector Offset = !ClearRun() ? FVector(Size.X + 300, 0, 0)
                    : Placing == 1 ? FVector(0, ClearDistance, 0) : FVector(-2 * ClearDistance, 0, 0);
                Fixture.SetLocation(Origin + Offset);
                Advance(EEdit::Prepare);
                break;
            }
            Character.UnequipBuildGun();
            FGuid Last;
            InitialSelectionMessages = SelectionMessages(World, Last);
            Report(World, TEXT("before"));
            if (ClearRun())
            {
                const FVector At = Initial[0].Bounds.GetCenter();
                const FVector Eye = At - FVector(Initial[0].Bounds.GetSize().GetMax() * 2 + 1000, 0, 0);
                if (!LookAt(Player, Eye, At)) return End(false, TEXT("clear camera"));
                Advance(EEdit::ClearSelect);
            }
            else Advance(Legacy() ? EEdit::CycleA : EEdit::AimA);
            break;
        }
        case EEdit::CycleA:
        case EEdit::CycleB:
        {
            if (!Stroke(Player, Cycle)) break;
            const int32 Index = Edit == EEdit::CycleA ? 0 : 1;
            if (!CheckSelection(World, Index, Cycle, TEXT("cycle"))) return End(false, TEXT("J did not select expected plan"));
            if (Index == 0) Advance(EEdit::CycleB);
            else
            {
                if (!OpenFoundationPreview(World, Player, Character)) return End(false, TEXT("open stock foundation preview"));
                Advance(EEdit::MovePreview);
            }
            break;
        }
        case EEdit::AimA:
            if (!LookEnd(Player, false)) return End(false, TEXT("front camera"));
            Advance(EEdit::PickA);
            break;
        case EEdit::PickA:
            if (!Stroke(Player, PickInput)) break;
            if (!CheckSelection(World, 0, PickInput, TEXT("front nearest")) || !LookEnd(Player, true))
                return End(false, TEXT("front selection"));
            Advance(EEdit::PickB);
            break;
        case EEdit::PickB:
            if (!Stroke(Player, PickInput)) break;
            if (!CheckSelection(World, 1, PickInput, TEXT("rear nearest")) || !LookEnd(Player, false))
                return End(false, TEXT("rear selection"));
            if (!SpawnWall(World)) return End(false, TEXT("opaque wall fixture"));
            OldActionIndex = ChatCount(World, TEXT("No visible plan under the crosshair."));
            Advance(EEdit::Wall);
            break;
        case EEdit::Wall:
            if (!Stroke(Player, PickInput)) break;
            if (!CheckMiss(World, TEXT("opaque wall"), OldActionIndex)) return End(false, TEXT("occlusion"));
            WallActor->Destroy();
            Advance(EEdit::Hide);
            break;
        case EEdit::Hide:
        case EEdit::Show:
        {
            if (!Stroke(Player, EKeys::V, true)) break;
            const bool bShow = Edit == EEdit::Show;
            if (!Unchanged(World) || !MatchViews(World, Initial, bShow)) return End(false, TEXT("global visibility"));
            UE_LOG(LogSaveCheckEditing, Display, TEXT("input=Ctrl+Shift+V visible=%d"), bShow ? 2 : 0);
            if (!Legacy() && !CheckMarker(World, bShow ? &Initial[1] : nullptr, bShow ? TEXT("shown") : TEXT("hidden")))
                return End(false, TEXT("marker after global visibility change"));
            if (!bShow && !Legacy())
            {
                if (!FramesDirectory.IsEmpty())
                    FScreenshotRequest::RequestScreenshot(FPaths::Combine(FramesDirectory, TEXT("E03-hidden.png")), true, false);
                OldActionIndex = ChatCount(World, TEXT("No visible plan under the crosshair."));
                Advance(EEdit::HiddenPick);
            }
            else if (!bShow) Advance(EEdit::Show);
            else if (Legacy()) Advance(EEdit::DeleteSelect);
            else
            {
                if (!LookEnd(Player, false)) return End(false, TEXT("marker front camera"));
                Advance(EEdit::MarkA);
            }
            break;
        }
        case EEdit::HiddenPick:
            if (!Stroke(Player, PickInput)) break;
            if (!CheckMiss(World, TEXT("hidden"), OldActionIndex)) return End(false, TEXT("hidden selection"));
            Advance(EEdit::Show);
            break;
        case EEdit::MarkA:
            if (Age <= 2)
            {
                if (!Stroke(Player, PickInput)) break;
                if (!CheckSelection(World, 0, PickInput, TEXT("marker A")) || !CheckMarker(World, &Initial[0], TEXT("A"))
                    || !LookPair(Player))
                    return End(false, TEXT("marker A selection"));
                break;
            }
            if (Age == 3 && !FramesDirectory.IsEmpty())
                FScreenshotRequest::RequestScreenshot(FPaths::Combine(FramesDirectory, TEXT("E03-A.png")), true, false);
            if (Age > 3 && (FramesDirectory.IsEmpty()
                || IFileManager::Get().FileSize(*FPaths::Combine(FramesDirectory, TEXT("E03-A.png"))) > 0))
                Advance(EEdit::MarkB);
            break;
        case EEdit::MarkB:
            if (Age <= 2)
            {
                if (!Stroke(Player, Cycle)) break;
                if (!CheckSelection(World, 1, Cycle, TEXT("marker B")) || !CheckMarker(World, &Initial[1], TEXT("B")))
                    return End(false, TEXT("marker B selection"));
                break;
            }
            if (Age == 3 && !FramesDirectory.IsEmpty())
                FScreenshotRequest::RequestScreenshot(FPaths::Combine(FramesDirectory, TEXT("E03-B.png")), true, false);
            if (Age > 3 && (FramesDirectory.IsEmpty()
                || IFileManager::Get().FileSize(*FPaths::Combine(FramesDirectory, TEXT("E03-B.png"))) > 0))
            {
                if (!LookEnd(Player, true)) return End(false, TEXT("move target camera"));
                Advance(EEdit::MovePreview);
            }
            break;
        case EEdit::MovePreview:
            if (!Legacy() && Age <= 2)
            {
                if (!Stroke(Player, PickInput)) break;
                if (!CheckSelection(World, 1, PickInput, TEXT("move target B"))) return End(false, TEXT("move target"));
                OpenFixturePreview(Player, Character);
                break;
            }
            if (Age >= 5)
            {
                MovePose = FTransform(FRotator(0, 90, 0), Initial[1].Pose.GetLocation() + FVector(0, 0, 400));
                if (!Legacy())
                {
                    if (PositionPreview(Character, MovePose)) Advance(EEdit::Move);
                    break;
                }
                Player.SetControlRotation((BuildAim - Character.GetPawnViewLocation()).Rotation());
                auto* Hologram = EquippedPreview(Character);
                if (Age == 5 || Age == 20)
                    UE_LOG(LogSaveCheckEditing, Display, TEXT("foundation_preview=%s hidden=%d disabled=%d can_lock=%d"),
                        *GetNameSafe(Hologram), Hologram && Hologram->IsHidden(), Hologram && Hologram->IsDisabled(),
                        Hologram && Hologram->CanLockHologram());
                if (!Hologram || Hologram->IsHidden() || Hologram->IsDisabled() || !Hologram->CanLockHologram()) break;
                if (!bRotatedFoundation)
                {
                    Hologram->ScrollRotate(1, 90);
                    bRotatedFoundation = true;
                    break;
                }
                MovePose = Hologram->GetActorTransform();
                const double YawChange = FMath::FindDeltaAngleDegrees(Initial[1].Pose.Rotator().Yaw, MovePose.Rotator().Yaw);
                if (MovePose.GetLocation().Z <= Initial[0].Pose.GetLocation().Z || FMath::IsNearlyZero(YawChange))
                    return End(false, TEXT("stock foundation requires higher pose and changed rotation"));
                Hologram->LockHologramPosition(true);
                if (Hologram->IsHologramLocked()) Advance(EEdit::Move);
            }
            break;
        case EEdit::Move:
        {
            if (!bPressed)
            {
                const auto* Hologram = EquippedPreview(Character);
                UE_LOG(LogSaveCheckEditing, Display, TEXT("move_preview=%s hidden=%d disabled=%d locked=%d pose=%s"),
                    *GetNameSafe(Hologram), Hologram && Hologram->IsHidden(), Hologram && Hologram->IsDisabled(),
                    Hologram && Hologram->IsHologramLocked(),
                    Hologram ? *Hologram->GetActorTransform().ToString() : TEXT("none"));
                if (Legacy())
                    if (const auto* Input = Cast<UFGPlayerInput>(Player.PlayerInput))
                        for (const auto& Mapping : Input->GetEnhancedActionMappings())
                            if (Mapping.Key == EKeys::M)
                                UE_LOG(LogSaveCheckEditing, Display, TEXT("legacy_stock_M action=%s mapping=%s triggers=%d"),
                                    *GetPathNameSafe(Mapping.Action.Get()), *Mapping.GetMappingName().ToString(),
                                    Mapping.Triggers.Num());
            }
            if (!Stroke(Player, EKeys::M, true)) break;
            const FTransform Original = Initial[1].Pose;
            Initial[1].Pose = MovePose;
            const bool bMoved = Unchanged(World) && MatchViews(World, Initial, true);
            if (!bMoved)
            {
                if (!Legacy()) return End(false, TEXT("M changed wrong GUID or pose"));
                bLegacyMoved = false;
                Initial[1].Pose = Original;
                if (!Unchanged(World) || !MatchViews(World, Initial, true)) return End(false, TEXT("legacy M changed unexpected data"));
                LogMenuInputState(Player, TEXT("after legacy M"));
                const auto* Hologram = EquippedPreview(Character);
                UE_LOG(LogSaveCheckEditing, Display, TEXT("after_legacy_M preview=%s hidden=%d disabled=%d"),
                    *GetNameSafe(Hologram), Hologram && Hologram->IsHidden(), Hologram && Hologram->IsDisabled());
            }
            UE_LOG(LogSaveCheckEditing, Display, TEXT("input=Ctrl+Shift+M id=%s expected=%s moved=%d"),
                *Initial[1].Id.ToString(), *MovePose.ToString(), bMoved);
            Report(World, TEXT("after_move"));
            OldActionIndex = ChatCount(World, TEXT("Position a Build Gun preview at the new origin first."));
            if (Legacy()) Character.UnequipBuildGun();
            Advance(Legacy() ? EEdit::LegacyDismiss : EEdit::InvalidMove);
            break;
        }
        case EEdit::LegacyDismiss:
            if (Age == 1 && (Player.bShowMouseCursor || GEngine->GameViewport->IgnoreInput())) SlateKey(EKeys::Escape);
            if (Age >= 3) Advance(EEdit::Hide);
            break;
        case EEdit::InvalidMove:
        {
            auto* Hologram = EquippedPreview(Character);
            if (!Hologram) return End(false, TEXT("equipped preview required for invalid move probe"));
            Hologram->SetDisabled(true);
            if (!Hologram->IsDisabled()) return End(false, TEXT("could not disable native preview"));
            if (!Stroke(Player, EKeys::M, true)) break;
            const int32 Refusals = ChatCount(World, TEXT("Position a Build Gun preview at the new origin first."));
            if (!Unchanged(World) || !MatchViews(World, Initial, true) || Refusals != OldActionIndex + 1)
                return End(false, TEXT("M with disabled preview did not preserve both plans"));
            UE_LOG(LogSaveCheckEditing, Display, TEXT("input=Ctrl+Shift+M equipped=1 disabled=1 refused=1 unchanged=1"));
            Hologram->SetDisabled(false);
            Character.UnequipBuildGun();
            OldActionIndex = Refusals;
            Advance(EEdit::RefuseMove);
            break;
        }
        case EEdit::RefuseMove:
            if (!Stroke(Player, EKeys::M, true)) break;
            if (!Unchanged(World) || !MatchViews(World, Initial, true) || EquippedPreview(Character)
                || ChatCount(World, TEXT("Position a Build Gun preview at the new origin first.")) != OldActionIndex + 1)
                return End(false, TEXT("M without a preview did not preserve both plans"));
            UE_LOG(LogSaveCheckEditing, Display, TEXT("input=Ctrl+Shift+M preview=0 refused=1 unchanged=1"));
            if (!MoveSlot.IsEmpty() && !Write(World, MoveSlot)) return End(false, TEXT("invalid or existing move slot"));
            Advance(EEdit::SaveMove);
            break;
        case EEdit::SaveMove:
            if (Saved && !Saved->IsSet()) break;
            if (Saved && !Saved->GetValue()) return End(false, TEXT("write move slot"));
            if (!SpawnFoundation(World)) return End(false, TEXT("delete foundation fixture"));
            Advance(EEdit::AimDelete);
            break;
        case EEdit::AimDelete:
            if (Age < 5) break;
            if (!AimForDelete(World, Player))
            {
                UE_LOG(LogSaveCheckEditing, Display, TEXT("deletion_ray_behind_A=no blocking foundation on test ray"));
                if (!LookEnd(Player, false)) return End(false, TEXT("delete camera"));
            }
            Advance(EEdit::DeleteSelect);
            break;
        case EEdit::DeleteSelect:
        {
            const auto Input = Legacy() ? Cycle : PickInput;
            if (!Stroke(Player, Input)) break;
            if (!CheckSelection(World, 0, Input, TEXT("delete target A"))) return End(false, TEXT("delete target"));
            if (!Legacy() && !FramesDirectory.IsEmpty())
                FScreenshotRequest::RequestScreenshot(FPaths::Combine(FramesDirectory, TEXT("E05-target.png")), true, false);
            Advance(EEdit::Delete);
            break;
        }
        case EEdit::Delete:
        case EEdit::DeleteAgain:
        {
            if (!Stroke(Player, EKeys::K, true)) break;
            if (!CheckRemaining(World, 1)) return End(false, TEXT("K changed B or the real foundation"));
            if (!Legacy() && !CheckMarker(World, nullptr, TEXT("deleted"))) return End(false, TEXT("stale selection marker"));
            const bool bRepeated = Edit == EEdit::DeleteAgain;
            UE_LOG(LogSaveCheckEditing, Display, TEXT("input=Ctrl+Shift+K repeat=%d target=%s remaining=%s foundation=%d"),
                bRepeated, *Initial[0].Id.ToString(), *Initial[1].Id.ToString(), RealFoundation.IsValid());
            if (!bRepeated) Advance(EEdit::DeleteAgain);
            else
            {
                Report(World, TEXT("after_delete"));
                Saved.Reset();
                Advance(EEdit::SaveFinal);
            }
            break;
        }
        case EEdit::ClearSelect:
            if (!Stroke(Player, Cycle)) break;
            if (!CheckSelection(World, 0, Cycle, TEXT("clear target selected"))) return End(false, TEXT("select A for clear"));
            Advance(EEdit::ClearAsk);
            break;
        case EEdit::ClearAsk:
        case EEdit::ClearAskAgain:
            if (!Stroke(Player, ClearInput)) break;
            if (ChatCount(World, TEXT("Clear in view: 2 target(s).")) != (Edit == EEdit::ClearAsk ? 1 : 2)
                || !Unchanged(World)) return End(false, TEXT("clear prompt target count"));
            UE_LOG(LogSaveCheckEditing, Display, TEXT("input=Ctrl+Shift+X count=2 unchanged=1"));
            Advance(Edit == EEdit::ClearAsk ? EEdit::ClearCancel : EEdit::ClearConfirm);
            break;
        case EEdit::ClearCancel:
            if (!TakeFrame(TEXT("E06-clear"))) break;
            if (!SlateKey(EKeys::Escape) || !Unchanged(World)
                || ChatCount(World, TEXT("Cancelled clearing plans in view.")) != 1)
                return End(false, TEXT("clear cancellation"));
            UE_LOG(LogSaveCheckEditing, Display, TEXT("input=Escape cancelled=1 unchanged=1"));
            Advance(EEdit::ClearAskAgain);
            break;
        case EEdit::ClearConfirm:
            if (Age == 1)
            {
                if (!Camera.IsValid() || !LookAt(Player, Camera->GetActorLocation(), Initial[2].Bounds.GetCenter()))
                    return End(false, TEXT("turn towards C before confirmation"));
                break;
            }
            if (!Stroke(Player, ClearInput)) break;
            if (!CheckRemaining(World, 2) || !CheckMarker(World, nullptr, TEXT("clear"))
                || ChatCount(World, TEXT("Cleared 2 plan(s) from the requested view.")) != 1)
                return End(false, TEXT("clear confirmation must use original snapshot"));
            UE_LOG(LogSaveCheckEditing, Display, TEXT("input=Ctrl+Shift+X confirmed=2 camera_turned=1"));
            Report(World, TEXT("after_clear"));
            Advance(EEdit::ClearHide);
            break;
        case EEdit::ClearHide:
            if (Stroke(Player, EKeys::V, true)) Advance(EEdit::ClearHidden);
            break;
        case EEdit::ClearHidden:
            if (!Stroke(Player, ClearInput)) break;
            if (ChatCount(World, TEXT("Clear in view: 0 target(s).")) != 1
                || !MatchViews(World, ReadMenuPlans(World), false)) return End(false, TEXT("hidden clear count"));
            UE_LOG(LogSaveCheckEditing, Display, TEXT("input=Ctrl+Shift+X hidden_count=0"));
            Advance(EEdit::ClearShow);
            break;
        case EEdit::ClearShow:
            if (!Stroke(Player, EKeys::V, true)) break;
            if (!CheckRemaining(World, 2)) return End(false, TEXT("C did not return after showing plans"));
            Advance(EEdit::SaveFinal);
            break;
        case EEdit::SaveFinal:
            if (Age == 1 && !FramesDirectory.IsEmpty() && !Legacy() && !ClearRun())
                FScreenshotRequest::RequestScreenshot(FPaths::Combine(FramesDirectory, TEXT("E03-deleted.png")), true, false);
            if (Age < 4) break;
            if (!Saved && !Write(World, Slot)) return End(false, TEXT("invalid or existing final slot"));
            if (Saved->IsSet()) return End(Saved->GetValue() && bLegacyMoved,
                bLegacyMoved ? TEXT("editing and save completed") : TEXT("legacy J/V/K and save completed; M failed"));
            break;
        case EEdit::ReloadPreview:
        {
            auto* Hologram = Cast<AFGBlueprintHologram>(EquippedPreview(Character));
            if (!Hologram || Hologram->mBuildableToNewRoot.IsEmpty()) break;
            for (TActorIterator<AFGBuildableFoundation> It(&World); It; ++It)
                if (It->GetActorLocation().Z > Character.GetActorLocation().Z + 5000)
                    UE_LOG(LogSaveCheckEditing, Display, TEXT("reloaded_foundation=%s pose=%s"),
                        *It->GetClass()->GetPathName(), *It->GetActorTransform().ToString());
            UE_LOG(LogSaveCheckEditing, Display, TEXT("library=%s preview=%s parts=%d"),
                *Blueprint, *GetNameSafe(Hologram), Hologram->mBuildableToNewRoot.Num());
            return End(true, TEXT("reloaded plans and Blueprint preview"));
        }
        }
        return true;
    }

    bool TickEditing()
    {
        if (++Total > 1500) return End(false, TEXT("world timeout"));
        if (!GEngine || !GEngine->GameViewport || !GEngine->GameViewport->Viewport || !FSlateApplication::IsInitialized())
            return true;
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
            if (Character && Player->PlayerInput) return TickEdit(*World, *Player, *Character);
        }
        return true;
    }
};
}

FTickerDelegate MakeEditingCheck(FString Mode, FString Slot, FString MoveSlot, FString Frames, int32 ExpectedCount)
{
    FEditingProbe Probe;
    Probe.Mode = MoveTemp(Mode);
    Probe.Slot = MoveTemp(Slot);
    Probe.MoveSlot = MoveTemp(MoveSlot);
    Probe.ExpectedCount = ExpectedCount;
    if (!Frames.IsEmpty())
    {
        Probe.FramesDirectory = FPaths::ConvertRelativePathToFull(Frames);
        IFileManager::Get().MakeDirectory(*Probe.FramesDirectory, true);
    }
    return FTickerDelegate::CreateLambda([Probe = MoveTemp(Probe)](float) mutable { return Probe.TickEditing(); });
}
