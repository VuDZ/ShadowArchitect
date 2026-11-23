#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "EnhancedPlayerInput.h"
#include "FGGameUserSettings.h"
#include "FGInputLibrary.h"
#include "FGSaveSession.h"
#include "Framework/Commands/InputChord.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputTriggers.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogSaveCheckKeybindings, Log, All);

namespace
{
enum class ELayout { Defaults, Legacy, Mixed, Overlap, Command, Custom, Unbound };

const FName Place(TEXT("ShadowArchitect.Place"));
const FName Select(TEXT("ShadowArchitect.Select"));
const FKey CustomPlacement = EKeys::F10;

bool HasModifier(const TArray<FKey>& Modifiers, const FKey& Left, const FKey& Right)
{
    return Modifiers.Contains(Left) || Modifiers.Contains(Right);
}

bool CheckLayout(APlayerController& Player, ELayout Layout)
{
    struct FBinding
    {
        FName Mapping;
        FInputChord Chord;
    };
    FBinding Bindings[] = {
        { Place, FInputChord(EKeys::F7) },
        { Select, FInputChord(EKeys::J, true, true, false, false) },
        { TEXT("ShadowArchitect.Move"), FInputChord(EKeys::M, true, true, false, false) },
        { TEXT("ShadowArchitect.Delete"), FInputChord(EKeys::K, true, true, false, false) },
        { TEXT("ShadowArchitect.Visibility"), FInputChord(EKeys::V, true, true, false, false) },
        { TEXT("ShadowArchitect.View"), FInputChord(EKeys::G, true, true, false, false) }
    };
    if (Layout == ELayout::Legacy) Bindings[0].Chord = FInputChord(EKeys::F8, false, false, true, false);
    if (Layout == ELayout::Legacy || Layout == ELayout::Mixed) Bindings[1].Chord.Key = EKeys::F5;
    if (Layout == ELayout::Overlap)
    {
        Bindings[1].Chord.Key = EKeys::M;
        Bindings[2].Chord.Key = EKeys::J;
    }
    if (Layout == ELayout::Command) Bindings[0].Chord = FInputChord(EKeys::F8, false, false, false, true);
    if (Layout == ELayout::Custom) Bindings[0].Chord = FInputChord(CustomPlacement, false, true, true, false);
    if (Layout == ELayout::Unbound) Bindings[0].Chord = FInputChord();
    if (Layout == ELayout::Custom || Layout == ELayout::Unbound) Bindings[1].Chord = FInputChord();

    bool bPassed = true;
    for (const auto& Binding : Bindings)
    {
        FKey Primary;
        TArray<FKey> Modifiers;
        const bool bFound = UFGInputLibrary::GetCurrentMappingForAction(&Player, Binding.Mapping, Primary, Modifiers);
        const FInputChord Actual(Primary,
            HasModifier(Modifiers, EKeys::LeftShift, EKeys::RightShift),
            HasModifier(Modifiers, EKeys::LeftControl, EKeys::RightControl),
            HasModifier(Modifiers, EKeys::LeftAlt, EKeys::RightAlt),
            HasModifier(Modifiers, EKeys::LeftCommand, EKeys::RightCommand));
        const bool bMatch = bFound && Actual == Binding.Chord;
        UE_LOG(LogSaveCheckKeybindings, Display, TEXT("binding=%s actual=%s expected=%s match=%d"),
            *Binding.Mapping.ToString(), Actual.Key.IsValid() ? *Actual.GetInputText().ToString() : TEXT("unbound"),
            Binding.Chord.Key.IsValid() ? *Binding.Chord.GetInputText().ToString() : TEXT("unbound"), bMatch);
        bPassed &= bMatch;
    }
    return bPassed;
}

bool CheckCommandMapping(APlayerController& Player)
{
    const auto* Input = Cast<UEnhancedPlayerInput>(Player.PlayerInput);
    if (!Input) return false;
    const FName CommandAction(TEXT("IA_ShadowChordCmd"));
    const auto& Mappings = Input->GetEnhancedActionMappings();
    const auto* PlacementMapping = Mappings.FindByPredicate([](const auto& Mapping)
        { return Mapping.GetMappingName() == Place && Mapping.Key == EKeys::F8; });
    const bool bUsesCommand = PlacementMapping && PlacementMapping->Triggers.ContainsByPredicate([&CommandAction](const auto& Trigger)
    {
        const auto* Chord = Cast<UInputTriggerChordAction>(Trigger.Get());
        return Chord && Chord->ChordAction && Chord->ChordAction->GetFName() == CommandAction;
    });
    const bool bModifierMapped = Mappings.ContainsByPredicate([&CommandAction](const auto& Mapping)
        { return Mapping.Key == EKeys::LeftCommand && Mapping.Action && Mapping.Action->GetFName() == CommandAction; });
    UE_LOG(LogSaveCheckKeybindings, Display, TEXT("physical_command_mapping=%d placement_chord=%d"), bModifierMapped, bUsesCommand);
    return bModifierMapped && bUsesCommand;
}

bool Finish(bool bPassed, const FString& Scenario)
{
    UE_LOG(LogSaveCheckKeybindings, Display, TEXT("pass=%d scenario=%s"), bPassed, *Scenario);
    FPlatformMisc::RequestExit(false);
    return false;
}
}

FTickerDelegate MakeKeybindingCheck(FString Mode)
{
    return FTickerDelegate::CreateLambda([Mode = MoveTemp(Mode), Ticks = 0, ReadyTicks = 0](float) mutable
    {
        if (!GEngine) return true;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            UWorld* World = Context.World();
            if (!World || !World->IsGameWorld() || !World->HasBegunPlay()
                || !World->GetMapName().Contains(TEXT("Persistent_Level"))) continue;
            UFGSaveSession* Save = UFGSaveSession::Get(World);
            APlayerController* Player = World->GetFirstPlayerController();
            if (!Save || !Player || !Player->PlayerInput) continue;
            Save->SetAutoSaveEnabled(false);

            bool bImported = false;
            GConfig->GetBool(TEXT("ShadowArchitect.Input"), TEXT("ImportedLegacyKeybindings"), bImported, GGameUserSettingsIni);
            if (!bImported) return ++Ticks < 120 || Finish(false, TEXT("migration marker missing"));
            if (++ReadyTicks < 3) return true;

            if (Mode == TEXT("clean") || Mode == TEXT("reset-check")) return Finish(CheckLayout(*Player, ELayout::Defaults), Mode);
            if (Mode == TEXT("legacy")) return Finish(CheckLayout(*Player, ELayout::Legacy), Mode);
            if (Mode == TEXT("mixed")) return Finish(CheckLayout(*Player, ELayout::Mixed), Mode);
            if (Mode == TEXT("legacy-overlap")) return Finish(CheckLayout(*Player, ELayout::Overlap), Mode);
            if (Mode == TEXT("legacy-command"))
                return Finish(CheckLayout(*Player, ELayout::Command) && CheckCommandMapping(*Player), Mode);
            if (Mode == TEXT("custom")) return Finish(CheckLayout(*Player, ELayout::Custom), Mode);
            if (Mode == TEXT("unbound-place")) return Finish(CheckLayout(*Player, ELayout::Unbound), Mode);

            FString Profile;
            const FConfigBranch* UserSettings = GConfig->FindBranch(FName(*GGameUserSettingsIni), GGameUserSettingsIni);
            if (!FParse::Value(FCommandLine::Get(), TEXT("UserDir="), Profile) || Profile.IsEmpty()
                || !UserSettings || UserSettings->IniPath.IsEmpty()
                || !FPaths::IsUnderDirectory(UserSettings->IniPath, FPaths::ProjectUserDir()))
                return Finish(false, TEXT("write test requires a separate -UserDir profile"));

            auto* Settings = UFGGameUserSettings::GetFGGameUserSettings();
            if (!Settings) return Finish(false, TEXT("game settings missing"));
            if (Mode == TEXT("customize"))
            {
                if (!CheckLayout(*Player, ELayout::Legacy)) return Finish(false, Mode);
                UFGInputLibrary::RebindEnhancedKeyMapping(Player, Place, CustomPlacement, { EKeys::LeftControl, EKeys::LeftAlt });
                UFGInputLibrary::RebindEnhancedKeyMapping(Player, Select, EKeys::Invalid, {});
                Settings->SaveSettings();
                return Finish(CheckLayout(*Player, ELayout::Custom), Mode);
            }
            if (Mode == TEXT("unbind-place"))
            {
                if (!CheckLayout(*Player, ELayout::Custom)) return Finish(false, Mode);
                UFGInputLibrary::RebindEnhancedKeyMapping(Player, Place, EKeys::Invalid, {});
                Settings->SaveSettings();
                return Finish(CheckLayout(*Player, ELayout::Unbound), Mode);
            }
            if (Mode == TEXT("reset") || Mode == TEXT("reset-command"))
            {
                const auto Layout = Mode == TEXT("reset") ? ELayout::Unbound : ELayout::Command;
                if (!CheckLayout(*Player, Layout)) return Finish(false, Mode);
                UFGInputLibrary::ResetAllEnhancedKeyBindings(Player);
                Settings->SaveSettings();
                return Finish(CheckLayout(*Player, ELayout::Defaults), Mode);
            }
            return Finish(false, TEXT("unknown keybinding scenario"));
        }
        return true;
    });
}
