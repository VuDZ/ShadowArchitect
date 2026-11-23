#include "ShadowPlan.h"
#include "Engine/LocalPlayer.h"
#include "EnhancedInputSubsystems.h"
#include "FGGameUserSettings.h"
#include "FGInputLibrary.h"
#include "FGPlayerController.h"
#include "FGPlayerInput.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Misc/ConfigCacheIni.h"
#include "UserSettings/EnhancedInputUserSettings.h"
#include "UObject/Class.h"

DEFINE_LOG_CATEGORY_STATIC(LogShadowKeybindings, Log, All);

namespace
{
constexpr const TCHAR* LegacySection = TEXT("/Script/ShadowArchitect.ShadowPlanSubsystem");
constexpr const TCHAR* MigrationSection = TEXT("ShadowArchitect.Input");
constexpr const TCHAR* MigrationKey = TEXT("ImportedLegacyKeybindings");
constexpr const TCHAR* CommandActionPath = TEXT("/ShadowArchitect/Inputs/Actions/IA_ShadowChordCmd.IA_ShadowChordCmd");

FInputChord ReadLegacyChord(const TCHAR* Action, bool bUseCtrlShift)
{
    const FString Field(Action);
    FString Value;
    if (GConfig->GetString(LegacySection, *(Field + TEXT("Chord")), Value, GGameIni))
    {
        FInputChord Chord;
        FStringOutputDevice Errors;
        Value.TrimStartAndEndInline();
        const TCHAR* End = FInputChord::StaticStruct()->ImportText(*Value, &Chord, nullptr,
            PPF_None, &Errors, Field);
        if (End && !*End && Errors.IsEmpty() && Chord.IsValidChord()) return Chord;
    }
    if (GConfig->GetString(LegacySection, *(Field + TEXT("Key")), Value, GGameIni))
    {
        FKey Key;
        Value.TrimStartAndEndInline();
        const TCHAR* End = *Value;
        if (Key.ImportTextItem(End, PPF_None, nullptr, nullptr) && !*End)
            return FInputChord(Key, bUseCtrlShift, bUseCtrlShift, false, false);
    }
    return {};
}

bool IsCommandKey(const FKey& Key)
{
    return Key == EKeys::LeftCommand || Key == EKeys::RightCommand;
}

TArray<TWeakObjectPtr<const UInputAction>> FindModifierActions(const UInputMappingContext& Chords, const TArray<FKey>& Keys)
{
    TArray<TWeakObjectPtr<const UInputAction>> Actions;
    for (const FKey& Key : Keys)
    {
        const auto* Match = Chords.GetMappings().FindByPredicate([&Key](const auto& Mapping)
            { return Mapping.Key == Key; });
        const UInputAction* Action = IsCommandKey(Key) ? LoadObject<UInputAction>(nullptr, CommandActionPath)
            : (Match ? Match->Action.Get() : nullptr);
        if (Action) Actions.Add(Action);
    }
    return Actions;
}

bool MapLegacyBinding(UEnhancedInputLocalPlayerSubsystem& Subsystem,
    const FFGCustomInputActionMapping& Binding,
    const TArray<TWeakObjectPtr<const UInputAction>>& ModifierActions)
{
    auto* InputSettings = Subsystem.GetUserSettings();
    if (!InputSettings) return false;
    const auto& Row = InputSettings->FindMappingsInRow(Binding.ActionName);
    if (Row.Num() != 1) return false;
    const FPlayerKeyMapping& Original = *Row.begin();

    FMapPlayerKeyArgs Args;
    Args.MappingName = Binding.ActionName;
    Args.Slot = Original.GetSlot();
    Args.HardwareDeviceId = Original.GetHardwareDeviceId().HardwareDeviceIdentifier;
    Args.NewKey = Binding.PrimaryKey;
    Args.bCreateMatchingSlotIfNeeded = false;
    FGameplayTagContainer Failures;
    InputSettings->MapPlayerKey(Args, Failures);
    if (!Failures.IsEmpty()) return false;

    Subsystem.CSS_AddPlayerMappedChordedActions(Binding.ActionName, ModifierActions);
    Subsystem.RequestRebuildControlMappings();
    return true;
}
}

bool AShadowPlanSubsystem::ImportLegacyKeybindings(APlayerController& Player)
{
    bool bImported = false;
    GConfig->GetBool(MigrationSection, MigrationKey, bImported, GGameUserSettingsIni);

    auto* Settings = UFGGameUserSettings::GetFGGameUserSettings();
    auto* Input = Cast<UFGPlayerInput>(Player.PlayerInput);
    if (!Settings || !Input) return false;

    const TCHAR* Actions[] = { TEXT("Place"), TEXT("Select"), TEXT("Move"), TEXT("Delete"), TEXT("Visibility"), TEXT("View") };
    TArray<FFGCustomInputActionMapping> Pending;
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(Actions); ++Index)
    {
        const FName Mapping(*FString::Printf(TEXT("ShadowArchitect.%s"), Actions[Index]));
        FFGCustomInputActionMapping Existing;
        if (Settings->GetPlayerMappedKey(Mapping, Existing))
        {
            if (Existing.ModifierKeys.ContainsByPredicate(IsCommandKey)) Pending.Add(MoveTemp(Existing));
            continue;
        }
        if (bImported) continue;

        const FInputChord Chord = ReadLegacyChord(Actions[Index], Index != 0);
        if (!Chord.IsValidChord()) continue;
        if (!Input->FindActionByMappableActionName(Mapping)) return false;

        auto& Binding = Pending.Emplace_GetRef(Mapping, Chord.Key, TArray<FKey>{});
        if (Chord.bCtrl) Binding.ModifierKeys.Add(EKeys::LeftControl);
        if (Chord.bShift) Binding.ModifierKeys.Add(EKeys::LeftShift);
        if (Chord.bAlt) Binding.ModifierKeys.Add(EKeys::LeftAlt);
        if (Chord.bCmd) Binding.ModifierKeys.Add(EKeys::LeftCommand);
    }

    int32 ImportedCount = 0;
    const auto* GamePlayer = Cast<AFGPlayerController>(&Player);
    const auto* Chords = GamePlayer ? GamePlayer->GetMappingContextChords() : nullptr;
    auto* InputSubsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(Player.GetLocalPlayer());
    if (!Pending.IsEmpty() && (!Chords || !InputSubsystem)) return false;
    for (const FFGCustomInputActionMapping& Binding : Pending)
    {
        const auto ModifierActions = FindModifierActions(*Chords, Binding.ModifierKeys);
        if (ModifierActions.Num() != Binding.ModifierKeys.Num())
        {
            UE_LOG(LogShadowKeybindings, Warning, TEXT("Kept %s unchanged because the game has no action for a legacy modifier."),
                *Binding.ActionName.ToString());
            continue;
        }

        const bool bUsesCommand = Binding.ModifierKeys.ContainsByPredicate(IsCommandKey);
        TArray<FEnhancedActionKeyMapping> HardConflicts, SoftConflicts;
        if (!bUsesCommand)
            Input->GetOverlappingEnhancedKeyMappings(Binding.ActionName, Binding.PrimaryKey, Binding.ModifierKeys,
                HardConflicts, SoftConflicts);
        if (!bUsesCommand && HardConflicts.IsEmpty())
        {
            UFGInputLibrary::RebindEnhancedKeyMapping(&Player, Binding.ActionName, Binding.PrimaryKey, Binding.ModifierKeys);
        }
        else
        {
            // The stock rebind clears hard conflicts and its stock modifier context has no Windows key.
            if (!MapLegacyBinding(*InputSubsystem, Binding, ModifierActions))
            {
                UE_LOG(LogShadowKeybindings, Warning, TEXT("Could not apply the legacy binding for %s."),
                    *Binding.ActionName.ToString());
                continue;
            }
            // The controller reloads the primary key on startup. Restore the Windows chord for the new input subsystem.
            FFGCustomInputActionMapping Existing;
            if (Settings->GetPlayerMappedKey(Binding.ActionName, Existing)) continue;
            Settings->AddPlayerMappedKey(Binding);
        }
        ++ImportedCount;
    }
    if (bImported) return true;

    // The game's key reset clears custom mappings while leaving this profile marker intact.
    GConfig->SetBool(MigrationSection, MigrationKey, true, GGameUserSettingsIni);
    if (ImportedCount) Settings->SaveSettings();
    GConfig->Flush(false, GGameUserSettingsIni);
    UE_LOG(LogShadowKeybindings, Display, TEXT("Imported %d legacy bindings."), ImportedCount);
    return true;
}

FString AShadowPlanSubsystem::KeybindingHint(APlayerController& Player, FName Mapping) const
{
    FKey Primary;
    TArray<FKey> Modifiers;
    if (!UFGInputLibrary::GetCurrentMappingForAction(&Player, Mapping, Primary, Modifiers) || !Primary.IsValid()) return {};
    return UFGInputLibrary::GetInputActionNameAsText_Keyboard(&Player, Mapping).ToString();
}
