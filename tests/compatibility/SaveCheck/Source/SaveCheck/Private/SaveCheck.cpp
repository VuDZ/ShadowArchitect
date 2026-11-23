#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "FGBlueprintSubsystem.h"
#include "FGPlayerController.h"
#include "FGSaveSession.h"
#include "FGSaveSystem.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/SecureHash.h"
#include "UI/FGGameUI.h"
#include "UObject/UnrealType.h"
DEFINE_LOG_CATEGORY_STATIC(LogSaveCheck, Log, All);

FTickerDelegate MakeViewCheck(FString OutputDir, bool bCustomKeys);
FTickerDelegate MakeBuildCheck(FString Mode);
FTickerDelegate MakeKeybindingCheck(FString Mode);
FTickerDelegate MakeMenuCheck(FString OutputDir);
FTickerDelegate MakePlanActionsCheck(FString Mode);
FTickerDelegate MakeAlignmentCheck(FString Frames, bool bGuide);
FTickerDelegate MakeRoofStackCheck(FString Directory);

class FSaveCheck : public IModuleInterface
{
    FTSTicker::FDelegateHandle Handle;
    int32 Frames = 0;
    bool Tick(float)
    {
        if (!GEngine) return true;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            UWorld* W=Context.World();
            if (!W || !W->IsGameWorld() || !W->HasBegunPlay() || !W->GetMapName().Contains(TEXT("Persistent_Level"))) continue;
            auto* Save=UFGSaveSession::Get(W);
            if (!Save) continue;
            Save->SetAutoSaveEnabled(false);
            if (++Frames < 6) return true;
            if (FParse::Param(FCommandLine::Get(), TEXT("SaveCheckOpenPauseMenu")))
            {
                auto* Player = Cast<AFGPlayerController>(W->GetFirstPlayerController());
                auto* UI = Player ? Player->GetGameUI() : nullptr;
                if (!UI || !Player->GetPawn()) return true;
                UI->Native_HandlePauseGamePressed();
                UE_LOG(LogSaveCheck, Display, TEXT("Requested stock pause menu. Open=%d"), UI->IsPauseMenuOpen());
                return false;
            }
            TSharedPtr<IPlugin> Plugin=IPluginManager::Get().FindPlugin(TEXT("ShadowArchitect"));
            UE_LOG(LogSaveCheck, Display, TEXT("version=%s enabled=%d"), Plugin ? *Plugin->GetDescriptor().VersionName : TEXT("missing"), Plugin && Plugin->IsEnabled());
            TArray<UFGBlueprintDescriptor*> Descs;
            AFGBlueprintSubsystem::GetBlueprintDescriptors(Descs,W);
            UE_LOG(LogSaveCheck, Display, TEXT("blueprints=%d"),Descs.Num());
            for (auto* D:Descs) if (D) UE_LOG(LogSaveCheck, Display,TEXT("blueprint=%s"),*D->GetBlueprintNameAsString());
            int32 Views=0, Visible=0, Subsystems=0;
            for (TActorIterator<AActor> It(W);It;++It)
            {
                AActor* A=*It;
                const FString Path=A->GetClass()->GetPathName();
                if (Path==TEXT("/Script/ShadowArchitect.ShadowProjection")) { ++Views; if(!A->IsHidden()) ++Visible; }
                if (Path!=TEXT("/Script/ShadowArchitect.ShadowPlanSubsystem")) continue;
                ++Subsystems;
                FArrayProperty* P=FindFProperty<FArrayProperty>(A->GetClass(),TEXT("Plans"));
                if (!P) continue;
                FScriptArrayHelper Items(P,P->ContainerPtrToValuePtr<void>(A));
                for (int32 I=0;I<Items.Num();++I)
                {
                    FString Record;
                    P->Inner->ExportTextItem_Direct(Record,Items.GetRawPtr(I),nullptr,A,PPF_None);
                    UE_LOG(LogSaveCheck, Display,TEXT("plan=%d digest=%s"),I,*FMD5::HashAnsiString(*Record));
                }
            }
            UE_LOG(LogSaveCheck, Display,TEXT("subsystems=%d views=%d visible=%d"),Subsystems,Views,Visible);
            FString Slot;
            if(FParse::Value(FCommandLine::Get(),TEXT("SaveCheckWrite="),Slot) && Slot.StartsWith(TEXT("SAQ_")))
            {
                FString Existing;
                if (UFGSaveSystem::GetAbsolutePathForSaveGame(W,Slot,Existing)) { UE_LOG(LogSaveCheck,Error,TEXT("Test slot already exists")); FPlatformMisc::RequestExit(false); return false; }
                Save->SaveGame(Slot,FOnSaveGameComplete::CreateLambda([](bool Ok,const FText& E,void*){
                    UE_LOG(LogSaveCheck,Display,TEXT("write=%d %s"),Ok,*E.ToString());
                    FPlatformMisc::RequestExit(false);
                }),nullptr);
            }
            else { FPlatformMisc::RequestExit(false); }
            return false;
        }
        return true;
    }
public:
    virtual void StartupModule() override
    {
        FString RoofStackFrames;
        if (FParse::Value(FCommandLine::Get(), TEXT("SaveCheckRoofStack="), RoofStackFrames))
        {
            Handle = FTSTicker::GetCoreTicker().AddTicker(MakeRoofStackCheck(MoveTemp(RoofStackFrames)), 0.12f);
            return;
        }
        FString AlignmentFrames;
        const bool bAlignmentGuide = FParse::Value(FCommandLine::Get(), TEXT("SaveCheckAlignmentGuide="), AlignmentFrames);
        if (bAlignmentGuide || FParse::Value(FCommandLine::Get(), TEXT("SaveCheckAlignment="), AlignmentFrames))
        {
            Handle = FTSTicker::GetCoreTicker().AddTicker(MakeAlignmentCheck(MoveTemp(AlignmentFrames), bAlignmentGuide), 0.12f);
            return;
        }
        FString KeybindingScenario;
        if (FParse::Value(FCommandLine::Get(), TEXT("SaveCheckKeybindings="), KeybindingScenario))
        {
            Handle = FTSTicker::GetCoreTicker().AddTicker(MakeKeybindingCheck(MoveTemp(KeybindingScenario)), 0.25f);
            return;
        }
        FString MenuFrames;
        if (FParse::Value(FCommandLine::Get(), TEXT("SaveCheckPlanMenu="), MenuFrames))
        {
            Handle = FTSTicker::GetCoreTicker().AddTicker(MakeMenuCheck(MoveTemp(MenuFrames)), 0.25f);
            return;
        }
        FString ActionMode;
        if (FParse::Value(FCommandLine::Get(), TEXT("SaveCheckPlanActions="), ActionMode))
        {
            if (ActionMode != TEXT("defaults") && ActionMode != TEXT("rebind") && ActionMode != TEXT("custom")
                && ActionMode != TEXT("safety") && ActionMode != TEXT("ui"))
            {
                UE_LOG(LogSaveCheck, Error, TEXT("Unknown SaveCheckPlanActions scenario"));
                FPlatformMisc::RequestExit(false);
                return;
            }
            Handle = FTSTicker::GetCoreTicker().AddTicker(MakePlanActionsCheck(MoveTemp(ActionMode)), 0.25f);
            return;
        }
        FString BuildScenario;
        if (FParse::Value(FCommandLine::Get(), TEXT("SaveCheckBuild="), BuildScenario))
        {
            if (BuildScenario != TEXT("preview") && BuildScenario != TEXT("confirm") && BuildScenario != TEXT("confirm-replaced")
                && BuildScenario != TEXT("menu-preview") && BuildScenario != TEXT("menu-confirm")
                && !BuildScenario.StartsWith(TEXT("seed:SAQ_")) && !BuildScenario.StartsWith(TEXT("source:")))
            {
                UE_LOG(LogSaveCheck, Error, TEXT("Unknown SaveCheckBuild scenario"));
                FPlatformMisc::RequestExit(false);
                return;
            }
            Handle = FTSTicker::GetCoreTicker().AddTicker(MakeBuildCheck(MoveTemp(BuildScenario)), 0.12f);
            return;
        }
        FString ViewFrames;
        if (FParse::Value(FCommandLine::Get(), TEXT("SaveCheckViews="), ViewFrames))
        {
            const bool bCustomKeys = FParse::Param(FCommandLine::Get(), TEXT("SaveCheckCustomKeys"));
            Handle = FTSTicker::GetCoreTicker().AddTicker(MakeViewCheck(MoveTemp(ViewFrames), bCustomKeys), 0.35f);
            return;
        }
        if (FParse::Param(FCommandLine::Get(), TEXT("SaveCheckRun"))
            || FParse::Param(FCommandLine::Get(), TEXT("SaveCheckOpenPauseMenu")))
            Handle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FSaveCheck::Tick), 0.6f);
    }
    virtual void ShutdownModule() override { FTSTicker::GetCoreTicker().RemoveTicker(Handle); }
};
IMPLEMENT_MODULE(FSaveCheck, SaveCheck)
