#include "RoofStackFixture.h"
#include "Camera/CameraActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "FGSaveSession.h"
#include "FGSaveSystem.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/PlatformMemory.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "RenderTimer.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "RHIStats.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogSaveCheckProjectionPerformance, Log, All);

namespace
{
constexpr double MemoryIntervalSeconds = 0.10;
constexpr const TCHAR* ProfileModes[] = { TEXT("Ghost"), TEXT("Wireframe"), TEXT("Footprint") };
const FVector GridCenter(-230000, -118000, 10350);
constexpr double GridStep = 3000.0;

bool IsBenchmarkSlot(const FString& Slot)
{
    if (!Slot.StartsWith(TEXT("SAQ_F"))) return false;
    for (const TCHAR Character : Slot)
        if (!FChar::IsAlnum(Character) && Character != TEXT('_')) return false;
    return true;
}

struct FProjectionFrame
{
    uint64 Frame;
    double Time;
    double Ms;
    int32 Draws;
    double GpuMs;
    double RenderMs;
    double CommitMiB;
    const TCHAR* Phase;
    int32 Run;
    int32 Pass;
    int32 Mode;
};

struct FCompletedRenderSample
{
    TAtomic<int32> Draws{-1};
    TAtomic<uint32> GpuCycles{0};
    TAtomic<uint32> RenderCycles{0};
};

struct FProjectionPerformanceCheck : TSharedFromThis<FProjectionPerformanceCheck>
{
    FString Output;
    FString ReloadSlot;
    int32 ExpectedPlans = 0;
    int32 Cycles = 1;
    double WindowSeconds = 5.0;
    TArray<FProjectionFrame> Frames;
    TArray<FRoofStackPlan> InitialPlans;
    TSharedRef<FCompletedRenderSample, ESPMode::ThreadSafe> LastRender = MakeShared<FCompletedRenderSample, ESPMode::ThreadSafe>();
    FDelegateHandle FrameHandle;
    TWeakObjectPtr<UWorld> LoadedWorld;
    double LastFrameTime = 0;
    double NextMemorySample = 0;
    double Deadline = 0;
    int32 WindowStart = 0;
    int32 Pass = 0;
    int32 Run = 0;
    int32 GhostVisuals = INDEX_NONE;
    bool bRequireInstances = FParse::Param(FCommandLine::Get(), TEXT("SaveCheckRequireISM"));
    bool bMeasuredDraws = false;
    bool bCaptureFrames = false;
    bool bCapturedFrame = false;
    bool bCheckedGrid = false;
    enum class EStage { Startup, Settling, Stable, Transition };
    EStage Stage = EStage::Startup;

    ~FProjectionPerformanceCheck()
    {
        FCoreDelegates::OnEndFrame.Remove(FrameHandle);
    }

    void RecordFrame()
    {
        const double Now = FPlatformTime::Seconds();
        double CommitMiB = -1.0;
        if (Now >= NextMemorySample)
        {
            CommitMiB = double(FPlatformMemory::GetStats().UsedVirtual) / (1024 * 1024);
            NextMemorySample = Now + MemoryIntervalSeconds;
        }
        constexpr const TCHAR* Phases[] = { TEXT("startup"), TEXT("settle"), TEXT("steady"), TEXT("switch") };
        if (LastFrameTime > 0)
            Frames.Add({ GFrameCounter, Now, (Now - LastFrameTime) * 1000, LastRender->Draws.Load(),
                FPlatformTime::ToMilliseconds(LastRender->GpuCycles.Load()),
                FPlatformTime::ToMilliseconds(LastRender->RenderCycles.Load()),
                CommitMiB, Phases[static_cast<int32>(Stage)], Run, Pass, Pass % 3 });
        LastFrameTime = Now;

        // Read the last completed GPU-0 draw counter on the RHI command stream.
        ENQUEUE_RENDER_COMMAND(SaveCheckProjectionDraws)([Render = LastRender](FRHICommandListImmediate& Commands)
        {
            Render->RenderCycles.Store(GRenderThreadTime);
            Commands.EnqueueLambda([Render](FRHICommandListBase&)
            {
                Render->Draws.Store(GNumDrawCallsRHI[0]);
                Render->GpuCycles.Store(RHIGetGPUFrameCycles(0));
            });
        });
    }

    bool End(bool bPassed, const TCHAR* Reason)
    {
        FCoreDelegates::OnEndFrame.Remove(FrameHandle);
        FrameHandle.Reset();
        FString Csv = TEXT("frame,mono_s,frame_ms,rhi_draws_gpu0,gpu0_ms,render_thread_ms,commit_mib,phase,run,pass,mode\n");
        Csv.Reserve(128 + Frames.Num() * 120);
        for (const auto& Frame : Frames)
            Csv += FString::Printf(TEXT("%llu,%.6f,%.4f,%d,%.4f,%.4f,%.3f,%s,%d,%d,%s\n"),
                static_cast<unsigned long long>(Frame.Frame), Frame.Time, Frame.Ms, Frame.Draws, Frame.GpuMs,
                Frame.RenderMs, Frame.CommitMiB,
                Frame.Phase, Frame.Run, Frame.Pass, ProfileModes[Frame.Mode]);
        const bool bWritten = FFileHelper::SaveStringToFile(Csv, *Output, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        UE_LOG(LogSaveCheckProjectionPerformance, Display, TEXT("pass=%d frames=%d csv_written=%d %s"),
            bPassed && bWritten, Frames.Num(), bWritten, Reason);
        FPlatformMisc::RequestExit(false);
        return false;
    }

    bool CheckGridInFrame(UWorld& World, APlayerController& Player)
    {
        FVector2D Resolution;
        GEngine->GameViewport->GetViewportSize(Resolution);
        FBox2D Pixels(ForceInit);
        int32 Inside = 0;
        auto* ViewClass = FindObject<UClass>(nullptr, TEXT("/Script/ShadowArchitect.ShadowProjection"));
        for (TActorIterator<AActor> It(&World, ViewClass); It; ++It)
        {
            FVector Vertices[8];
            It->GetComponentsBoundingBox(true).GetVertices(Vertices);
            bool bInside = true;
            for (const FVector& Vertex : Vertices)
            {
                FVector2D Pixel;
                if (Player.ProjectWorldLocationToScreen(Vertex, Pixel, true))
                {
                    Pixels += Pixel;
                    bInside &= Pixel.X >= 0 && Pixel.X < Resolution.X && Pixel.Y >= 0 && Pixel.Y < Resolution.Y;
                }
                else bInside = false;
            }
            if (bInside) ++Inside;
        }
        UE_LOG(LogSaveCheckProjectionPerformance, Display, TEXT("grid_in_frame=%d total=%d pixel_min=%s pixel_max=%s"),
            Inside, ExpectedPlans, *Pixels.Min.ToString(), *Pixels.Max.ToString());
        return Inside == ExpectedPlans;
    }

    bool Inspect(UWorld& World)
    {
        TArray<FRoofStackPlan> Plans;
        if (!ReadRoofStackPlans(World, Plans) || Plans.Num() != ExpectedPlans) return false;
        if (Stage == EStage::Startup && Run == 0) InitialPlans = Plans;
        if (InitialPlans.Num() != Plans.Num()) return false;
        for (int32 Index = 0; Index < Plans.Num(); ++Index)
            if (Plans[Index].Id != InitialPlans[Index].Id || !Plans[Index].Anchor.Equals(InitialPlans[Index].Anchor, 0.01))
                return false;

        auto* ViewClass = FindObject<UClass>(nullptr, TEXT("/Script/ShadowArchitect.ShadowProjection"));
        if (!ViewClass) return false;
        int32 Actors = 0;
        int32 Meshes = 0;
        int32 IsmGroups = 0;
        int32 IsmInstances = 0;
        const int32 Mode = Pass % 3;
        for (TActorIterator<AActor> It(&World, ViewClass); It; ++It)
        {
            ++Actors;
            const FTransform Pose = It->GetActorTransform();
            if (!Plans.ContainsByPredicate([&Pose](const auto& Plan) { return Plan.Anchor.Equals(Pose, 0.01); })) return false;
            int32 ActorMeshes = 0;
            for (const auto* Component : It->GetInstanceComponents())
                if (const auto* Mesh = Cast<UStaticMeshComponent>(Component); IsValid(Mesh) && Mesh->IsRegistered())
                {
                    const auto* Material = Mesh->GetMaterial(0);
                    if (!Material || !Mesh->GetStaticMesh()) return false;
                    const FName Name = Material->GetFName();
                    if (Mode == 1 ? (Name != FName(TEXT("M_ShadowWireframe")) && Name != FName(TEXT("M_ShadowBoxEdges")))
                        : Name != FName(TEXT("Hologram_Blue_Inst"))) return false;
                    if (const auto* Group = Cast<UInstancedStaticMeshComponent>(Mesh))
                    {
                        const int32 Count = Group->GetInstanceCount();
                        if (Count < 2 || Mode == 2) return false;
                        ++IsmGroups;
                        IsmInstances += Count;
                    }
                    ++ActorMeshes;
                }
            if (ActorMeshes == 0 || (Mode == 2 && ActorMeshes != 1) || It->IsHidden()) return false;
            Meshes += ActorMeshes;
        }
        const int32 Visuals = Meshes - IsmGroups + IsmInstances;
        if (GhostVisuals == INDEX_NONE) GhostVisuals = Visuals;
        UE_LOG(LogSaveCheckProjectionPerformance, Display,
            TEXT("snapshot run=%d pass=%d mode=%s plans=%d actors=%d meshes=%d ism_groups=%d ism_instances=%d visual_instances=%d"),
            Run, Pass, ProfileModes[Mode], Plans.Num(), Actors, Meshes, IsmGroups, IsmInstances, Visuals);
        if (Actors != ExpectedPlans) return false;
        if (Mode == 2) return Visuals == ExpectedPlans;
        return Visuals == GhostVisuals && (!bRequireInstances || IsmGroups > 0);
    }

    void SummarizeWindow()
    {
        TArray<double> Ms;
        TArray<double> Gpu;
        TArray<double> Render;
        TArray<int32> Draws;
        double TotalMs = 0;
        double CommitMiB = -1.0;
        for (int32 Index = WindowStart; Index < Frames.Num(); ++Index)
        {
            const auto& Frame = Frames[Index];
            Ms.Add(Frame.Ms);
            TotalMs += Frame.Ms;
            if (Frame.Draws > 0) Draws.Add(Frame.Draws);
            if (Frame.GpuMs > 0) Gpu.Add(Frame.GpuMs);
            if (Frame.RenderMs > 0) Render.Add(Frame.RenderMs);
            if (Frame.CommitMiB >= 0) CommitMiB = Frame.CommitMiB;
        }
        if (Ms.IsEmpty()) return;
        Ms.Sort();
        Draws.Sort();
        Gpu.Sort();
        Render.Sort();
        bMeasuredDraws = bMeasuredDraws || !Draws.IsEmpty();
        const int32 P95 = FMath::CeilToInt(Ms.Num() * 0.95) - 1;
        UE_LOG(LogSaveCheckProjectionPerformance, Display,
            TEXT("window run=%d pass=%d mode=%s frames=%d seconds=%.3f fps=%.2f p50_ms=%.3f p95_ms=%.3f max_ms=%.3f")
            TEXT(" draw_samples=%d draws_gpu0_p50=%d settled_commit_mib=%.3f gpu_samples=%d gpu0_p50_ms=%.3f")
            TEXT(" render_samples=%d render_p50_ms=%.3f"),
            Run, Pass, ProfileModes[Pass % 3], Ms.Num(), TotalMs * 0.001, Ms.Num() * 1000.0 / TotalMs,
            Ms[Ms.Num() / 2], Ms[P95], Ms.Last(), Draws.Num(), Draws.IsEmpty() ? -1 : Draws[Draws.Num() / 2], CommitMiB,
            Gpu.Num(), Gpu.IsEmpty() ? -1 : Gpu[Gpu.Num() / 2], Render.Num(), Render.IsEmpty() ? -1 : Render[Render.Num() / 2]);
    }

    bool Tick()
    {
        if (!GEngine) return true;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            auto* World = Context.World();
            if (!World || !World->IsGameWorld() || !World->HasBegunPlay()
                || !World->GetMapName().Contains(TEXT("Persistent_Level"))) continue;
            auto* Save = UFGSaveSession::Get(World);
            auto* Player = World->GetFirstPlayerController();
            if (!Save || !Player || !Player->GetPawn()) continue;
            Save->SetAutoSaveEnabled(false);
            const double Now = FPlatformTime::Seconds();
            if (Stage == EStage::Startup)
            {
                if (LoadedWorld.Get() == World) return true;
                if (!Inspect(*World)) return End(false, TEXT("initial projection snapshot"));
                if (!FApp::CanEverRender() || GUsingNullRHI) return End(false, TEXT("rendering disabled"));
                auto* Camera = World->SpawnActor<ACameraActor>(
                    FVector(-209000, -142500, 38850), FRotator(-41.0, 130.6, 0));
                if (!Camera) return End(false, TEXT("no benchmark camera"));
                Player->SetViewTarget(Camera);
                FVector2D Resolution;
                GEngine->GameViewport->GetViewportSize(Resolution);
                UE_LOG(LogSaveCheckProjectionPerformance, Display,
                    TEXT("camera_cm=%s rotation=%s viewport=%s memory=commit_mib period_s=%.2f clock=FPlatformTime"),
                    *Camera->GetActorLocation().ToString(), *Camera->GetActorRotation().ToString(),
                    *Resolution.ToString(), MemoryIntervalSeconds);
                LoadedWorld = World;
                Stage = EStage::Settling;
                Deadline = Now + 5.0;
            }
            else if (LoadedWorld.Get() != World) continue;
            else if (Stage == EStage::Transition)
            {
                for (const auto& Key : { EKeys::G, EKeys::LeftShift, EKeys::LeftControl })
                    RoofStackKey(*Player, Key, IE_Released);
                Stage = EStage::Settling;
                Deadline = Now + 1.0;
            }
            else if (Now >= Deadline)
            {
                if (Stage == EStage::Settling)
                {
                    if (!Inspect(*World)) return End(false, TEXT("projection layout changed"));
                    if (!bCapturedFrame && bCaptureFrames && Run == 0 && Pass < 3)
                    {
                        const FString Path = FPaths::ChangeExtension(Output, FString(ProfileModes[Pass]) + TEXT(".png"));
                        FScreenshotRequest::RequestScreenshot(Path, true, false);
                        bCapturedFrame = true;
                        Deadline = Now + 1.0;
                        return true;
                    }
                    if (!bCheckedGrid && ExpectedPlans == 100)
                    {
                        if (!CheckGridInFrame(*World, *Player)) return End(false, TEXT("grid not fully visible"));
                        bCheckedGrid = true;
                    }
                    Stage = EStage::Stable;
                    WindowStart = Frames.Num();
                    Deadline = Now + WindowSeconds;
                }
                else
                {
                    SummarizeWindow();
                    if (Pass == Cycles * 3)
                    {
                        if (!bMeasuredDraws) return End(false, TEXT("no RHI draws"));
                        if (ReloadSlot.IsEmpty() || Run > 0) return End(true, TEXT("complete cycle"));
                        FString SavePath;
                        if (!IsBenchmarkSlot(ReloadSlot) || !UFGSaveSystem::GetAbsolutePathForSaveGame(World, ReloadSlot, SavePath))
                            return End(false, TEXT("reload requires an existing SAQ_F slot"));
                        ++Run;
                        Pass = 0;
                        Stage = EStage::Startup;
                        UE_LOG(LogSaveCheckProjectionPerformance, Display, TEXT("reload run=%d slot=%s mono_s=%.6f frame=%llu"),
                            Run, *ReloadSlot, Now, static_cast<unsigned long long>(GFrameCounter));
                        UGameplayStatics::OpenLevel(World, FName(TEXT("/Game/FactoryGame/Map/GameLevel01/Persistent_Level")),
                            true, FString::Printf(TEXT("loadgame=%s?sessionName=SAQPerfReload"), *ReloadSlot));
                        return true;
                    }
                    ++Pass;
                    Stage = EStage::Transition;
                    bCapturedFrame = false;
                    UE_LOG(LogSaveCheckProjectionPerformance, Display,
                        TEXT("input run=%d pass=%d mode=%s issued_frame=%llu expected_action_frame=%llu mono_s=%.6f"),
                        Run, Pass, ProfileModes[Pass % 3], static_cast<unsigned long long>(GFrameCounter),
                        static_cast<unsigned long long>(GFrameCounter + 1), Now);
                    for (const auto& Key : { EKeys::LeftControl, EKeys::LeftShift, EKeys::G })
                        RoofStackKey(*Player, Key, IE_Pressed);
                }
            }
            return true;
        }
        return true;
    }
};
}

FTickerDelegate MakeProjectionPerformanceCheck(FString Output, int32 ExpectedPlans, double WindowSeconds,
    int32 Cycles, FString ReloadSlot, bool bCaptureFrames)
{
    const auto Check = MakeShared<FProjectionPerformanceCheck>();
    Check->Output = MoveTemp(Output);
    Check->ExpectedPlans = ExpectedPlans;
    Check->WindowSeconds = WindowSeconds;
    Check->Cycles = Cycles;
    Check->ReloadSlot = MoveTemp(ReloadSlot);
    Check->bCaptureFrames = bCaptureFrames;
    Check->FrameHandle = FCoreDelegates::OnEndFrame.AddSP(Check, &FProjectionPerformanceCheck::RecordFrame);
    return FTickerDelegate::CreateLambda([Check](float) { return Check->Tick(); });
}

FTickerDelegate MakeProjectionFixture(FString Slot, int32 Count)
{
    return FTickerDelegate::CreateLambda([Slot = MoveTemp(Slot), Count](float)
    {
        if (!GEngine) return true;
        for (const auto& Context : GEngine->GetWorldContexts())
        {
            auto* World = Context.World();
            if (!World || !World->IsGameWorld() || !World->HasBegunPlay()
                || !World->GetMapName().Contains(TEXT("Persistent_Level"))) continue;
            auto* Save = UFGSaveSession::Get(World);
            auto* Type = FindObject<UClass>(nullptr, TEXT("/Script/ShadowArchitect.ShadowPlanSubsystem"));
            if (!Save || !Type) continue;
            Save->SetAutoSaveEnabled(false);
            FString Existing;
            if (!IsBenchmarkSlot(Slot) || Count < 0 || Count > 100
                || UFGSaveSystem::GetAbsolutePathForSaveGame(World, Slot, Existing))
            {
                UE_LOG(LogSaveCheckProjectionPerformance, Error, TEXT("Choose a fresh SAQ_F slot"));
                FPlatformMisc::RequestExit(false);
                return false;
            }
            TActorIterator<AActor> Subsystem(World, Type);
            const auto* Plans = FindFProperty<FArrayProperty>(Type, TEXT("Plans"));
            if (!Subsystem || !Plans) continue;
            FScriptArrayHelper Records(Plans, Plans->ContainerPtrToValuePtr<void>(*Subsystem));
            if (Count == 0) Records.EmptyValues();
            else
            {
                const auto* Record = CastField<FStructProperty>(Plans->Inner);
                const auto* Name = Record ? FindFProperty<FStrProperty>(Record->Struct, TEXT("BlueprintId")) : nullptr;
                const auto* Id = Record ? FindFProperty<FStructProperty>(Record->Struct, TEXT("Id")) : nullptr;
                const auto* Pose = Record ? FindFProperty<FStructProperty>(Record->Struct, TEXT("Transform")) : nullptr;
                if (!Record || !Name || !Id || !Pose) continue;
                const void* Seed = nullptr;
                for (int32 Index = 0; Index < Records.Num(); ++Index)
                    if (Name->GetPropertyValue_InContainer(Records.GetRawPtr(Index)) == TEXT("3х3 пустой"))
                    {
                        Seed = Records.GetRawPtr(Index);
                        break;
                    }
                if (!Seed)
                {
                    UE_LOG(LogSaveCheckProjectionPerformance, Error, TEXT("The source slot needs the 3x3 plan"));
                    FPlatformMisc::RequestExit(false);
                    return false;
                }
                FStructOnScope Template(Record->Struct);
                Record->CopyCompleteValue(Template.GetStructMemory(), Seed);
                Records.EmptyValues();
                for (int32 Index = 0; Index < Count; ++Index)
                {
                    void* Entry = Records.GetRawPtr(Records.AddValue());
                    Record->CopyCompleteValue(Entry, Template.GetStructMemory());
                    *Id->ContainerPtrToValuePtr<FGuid>(Entry) = FGuid(0x53415146, 0, 0, Index + 1);
                    const FVector Position = GridCenter + GridStep * FVector(Index % 10 - 4.5, Index / 10 - 4.5, 0);
                    *Pose->ContainerPtrToValuePtr<FTransform>(Entry) = FTransform(Position);
                }
            }
            Save->SaveGame(Slot, FOnSaveGameComplete::CreateLambda([Slot, Count](bool bSaved, const FText& Error, void*)
            {
                UE_LOG(LogSaveCheckProjectionPerformance, Display, TEXT("slot=%s plans=%d write=%d %s"),
                    *Slot, Count, bSaved, *Error.ToString());
                FPlatformMisc::RequestExit(false);
            }), nullptr);
            return false;
        }
        return true;
    });
}
