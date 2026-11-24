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
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/PlatformMemory.h"
#include "HAL/MemoryMisc.h"
#include "HAL/UnrealMemory.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "RenderTimer.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "RHIStats.h"
#include "UObject/GarbageCollection.h"
#include "UObject/ReferenceChainSearch.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectArray.h"
#include "UObject/UnrealType.h"
#include "UnrealClient.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <d3d12.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogSaveCheckProjectionPerformance, Log, All);
DEFINE_LOG_CATEGORY_STATIC(LogSaveCheckProjectionMemory, Log, All);

namespace
{
constexpr double MemoryIntervalSeconds = 0.10;
constexpr const TCHAR* ProfileModes[] = { TEXT("Ghost"), TEXT("Wireframe"), TEXT("Footprint") };
const FVector GridCenter(-230000, -118000, 10350);
constexpr double GridStep = 3000.0;
const FName GridMap(TEXT("/Game/FactoryGame/Map/GameLevel01/Persistent_Level"));
const FName MenuMap(TEXT("/Game/FactoryGame/Map/MenuScenes/Map_Menu_1_02"));

ACameraActor* SetProjectionCamera(UWorld& World, APlayerController& Player)
{
    auto* Camera = World.SpawnActor<ACameraActor>(FVector(-209000, -142500, 38850), FRotator(-41.0, 130.6, 0));
    if (Camera) Player.SetViewTarget(Camera);
    return Camera;
}

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

struct FProjectionCounts
{
    int32 Actors = 0;
    int32 Meshes = 0;
    int32 IsmGroups = 0;
    int32 IsmInstances = 0;

    int32 Visuals() const { return Meshes - IsmGroups + IsmInstances; }
};

bool CountProjectionViews(UWorld& World, const TArray<FRoofStackPlan>& Plans, int32 Mode, FProjectionCounts& Counts)
{
    auto* ViewClass = FindObject<UClass>(nullptr, TEXT("/Script/ShadowArchitect.ShadowProjection"));
    if (!ViewClass) return false;
    for (TActorIterator<AActor> It(&World, ViewClass); It; ++It)
    {
        ++Counts.Actors;
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
                    ++Counts.IsmGroups;
                    Counts.IsmInstances += Count;
                }
                ++ActorMeshes;
            }
        if (ActorMeshes == 0 || (Mode == 2 && ActorMeshes != 1) || It->IsHidden()) return false;
        Counts.Meshes += ActorMeshes;
    }
    return true;
}

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

        const int32 Mode = Pass % 3;
        FProjectionCounts Counts;
        if (!CountProjectionViews(World, Plans, Mode, Counts)) return false;
        const int32 Visuals = Counts.Visuals();
        if (GhostVisuals == INDEX_NONE) GhostVisuals = Visuals;
        UE_LOG(LogSaveCheckProjectionPerformance, Display,
            TEXT("snapshot run=%d pass=%d mode=%s plans=%d actors=%d meshes=%d ism_groups=%d ism_instances=%d visual_instances=%d"),
            Run, Pass, ProfileModes[Mode], Plans.Num(), Counts.Actors, Counts.Meshes, Counts.IsmGroups, Counts.IsmInstances, Visuals);
        if (Counts.Actors != ExpectedPlans) return false;
        if (Mode == 2) return Visuals == ExpectedPlans;
        return Visuals == GhostVisuals && (!bRequireInstances || Counts.IsmGroups > 0);
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
                auto* Camera = SetProjectionCamera(*World, *Player);
                if (!Camera) return End(false, TEXT("no benchmark camera"));
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
                        UGameplayStatics::OpenLevel(World, GridMap,
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

struct FProjectionMemoryCheck : TSharedFromThis<FProjectionMemoryCheck>
{
    static constexpr const TCHAR* EmptySlot = TEXT("SAQ_F02_000_PILOT");
    static constexpr const TCHAR* GridSlot = TEXT("SAQ_F02_100_PILOT");
    enum class EStage { FindWorld, ReleaseKeys, Settle, Observe };

    double SettleSeconds = 4;
    double WindowSeconds = 3;
    double Deadline = FPlatformTime::Seconds() + 75;
    double GridDeadline = 0;
    int32 Cycles = 5;
    int32 Rounds = 1;
    int32 Round = 1;
    int32 GridFirst = 2;
    int32 Step = 0;
    bool bTrace = false;
    bool bIdle = false;
    EStage Stage = EStage::FindWorld;
    FDelegateHandle CleanupHandle;
    TWeakObjectPtr<UWorld> PreviousWorld;
    TWeakObjectPtr<UWorld> GridWorld;
    bool bGridUnloaded = false;
    const bool bIsm = FParse::Param(FCommandLine::Get(), TEXT("ShadowPerPlanISM"));
    const bool bEmptyControl = FParse::Param(FCommandLine::Get(), TEXT("SaveCheckMemoryEmptyControl"));
    const bool bSparseChecks = FParse::Param(FCommandLine::Get(), TEXT("SaveCheckMemorySparseChecks"));
    const bool bInsights = FParse::Param(FCommandLine::Get(), TEXT("SaveCheckMemoryBookmarks"));

    ~FProjectionMemoryCheck() { FWorldDelegates::OnWorldCleanup.Remove(CleanupHandle); }

    int32 MenuAfter() const { return GridFirst + Cycles * 3 + 1; }
    int32 EmptyAfter() const { return MenuAfter() + (bTrace ? 2 : 1); }
    bool IsMenu() const { return Step == 0 || (Step >= MenuAfter() && Step < EmptyAfter()); }
    bool IsGrid() const { return Step >= GridFirst && Step < MenuAfter(); }
    int32 Mode() const { return IsGrid() && !bIdle ? (Step - GridFirst) % 3 : 0; }

    const TCHAR* Phase() const
    {
        if (IsGrid()) return TEXT("grid");
        if (Step == 0) return TEXT("menu_before");
        if (Step == 1) return TEXT("empty_before");
        if (Step == MenuAfter()) return TEXT("menu_after");
        return IsMenu() ? TEXT("menu_after_gc") : TEXT("empty_after");
    }

    void Mark(const TCHAR* Event) const
    {
        const uint64 Qpc = FPlatformTime::Cycles64();
        const TCHAR* WorldPhase = Phase();
        UE_LOG(LogSaveCheckProjectionMemory, Display, TEXT("event=%s phase=%s step=%d cycle=%d mode=%s qpc=%llu"),
            Event, WorldPhase, Step, IsGrid() ? (Step - GridFirst + 2) / 3 : 0,
            IsMenu() ? TEXT("menu") : ProfileModes[Mode()], static_cast<unsigned long long>(Qpc));

        if (!bInsights) return;
        const bool bCheckpoint = FCString::Strcmp(Event, TEXT("window_begin")) == 0
            && (Step == 1 || Step == GridFirst || Step >= MenuAfter() - 1);
        const bool bGridLoad = Step == GridFirst && FCString::Strcmp(Event, TEXT("travel")) == 0;
        const bool bGridEnd = Step == MenuAfter() - 1 && FCString::Strcmp(Event, TEXT("window_end")) == 0;
        if (bCheckpoint || bGridLoad || bGridEnd || FCString::Strncmp(Event, TEXT("gc_"), 3) == 0)
        {
            TRACE_BOOKMARK_CYCLES(Qpc, TEXT("F11 %s %s step=%d qpc=%llu"),
                Event, WorldPhase, Step, static_cast<unsigned long long>(Qpc));
        }
    }

    void OnWorldCleanup(UWorld* World, bool, bool)
    {
        if (GridWorld.Get() != World || Step != MenuAfter()) return;
        bGridUnloaded = true;
        Mark(TEXT("grid_cleanup"));
    }

    bool Identify() const
    {
#if PLATFORM_WINDOWS
        if (!GDynamicRHI || RHIGetInterfaceType() != ERHIInterfaceType::D3D12 || !FApp::CanEverRender()) return false;
        const auto* Plugin = IPluginManager::Get().FindPlugin(TEXT("ShadowArchitect")).Get();
        FModuleStatus Module;
        if (!Plugin || !Plugin->IsEnabled()
            || !FModuleManager::Get().QueryModule(TEXT("ShadowArchitect"), Module) || !Module.bIsLoaded) return false;
        // Adapter identity does not require flushing the render thread's command list.
        auto* Device = static_cast<ID3D12Device*>(GDynamicRHI->RHIGetNativeDevice());
        if (!Device) return false;
        const LUID Adapter = Device->GetAdapterLuid();
        UE_LOG(LogSaveCheckProjectionMemory, Display, TEXT("identity pid=%u luid=luid_0x%08x_0x%08x ism=%d version=%s adapter=\"%s\" dll=\"%s\""),
            FPlatformProcess::GetCurrentProcessId(), static_cast<uint32>(Adapter.HighPart), Adapter.LowPart, bIsm,
            *Plugin->GetDescriptor().VersionName, *GRHIAdapterName, *FPaths::ConvertRelativePathToFull(Module.FilePath));
        return true;
#else
        return false;
#endif
    }

    bool AcceptLayout(UWorld& World) const
    {
        TArray<FRoofStackPlan> Plans;
        FProjectionCounts Counts;
        if (!ReadRoofStackPlans(World, Plans) || !CountProjectionViews(World, Plans, Mode(), Counts)) return false;
        UE_LOG(LogSaveCheckProjectionMemory, Display,
            TEXT("snapshot step=%d phase=%s mode=%s plans=%d actors=%d meshes=%d ism_groups=%d ism_instances=%d visual_instances=%d"),
            Step, Phase(), ProfileModes[Mode()], Plans.Num(), Counts.Actors, Counts.Meshes,
            Counts.IsmGroups, Counts.IsmInstances, Counts.Visuals());
        const int32 Expected = IsGrid() && !bEmptyControl ? 100 : 0;
        if (Plans.Num() != Expected || Counts.Actors != Expected) return false;
        if (!Expected || Mode() == 2) return Counts.Meshes == Expected && Counts.IsmGroups == 0;
        return Counts.Visuals() == 3800 && Counts.Meshes == (bIsm ? 600 : 3800)
            && Counts.IsmGroups == (bIsm ? 500 : 0) && Counts.IsmInstances == (bIsm ? 3700 : 0);
    }

    bool End(bool bPassed, const TCHAR* Reason) const
    {
        UE_LOG(LogSaveCheckProjectionMemory, Display, TEXT("pass=%d step=%d reason=%s"), bPassed, Step, Reason);
        FPlatformMisc::RequestExit(false);
        return false;
    }

    void TraceObjects(UWorld& World) const
    {
        auto* ViewClass = FindObject<UClass>(nullptr, TEXT("/Script/ShadowArchitect.ShadowProjection"));
        struct FViews
        {
            int32 Actors = 0;
            int32 Meshes = 0;
            int32 Ism = 0;
            int32 Registered = 0;
            int32 Instances = 0;
            int32 Pending = 0;
            SIZE_T Bytes = 0;
        };
        FViews Current, Removed, Previous;
        TWeakObjectPtr<UObject> Survivor;

        // The ordinary UObject iterator skips unreachable objects during incremental purge.
        GUObjectArray.LockInternalArray();
        for (FUObjectArray::TIterator It(GUObjectArray, false); It; ++It)
        {
            auto* Object = static_cast<UObject*>(GUObjectArray.IndexToObject(It.GetIndex())->GetObject());
            if (Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)
                || Object->HasAnyInternalFlags(EInternalObjectFlags::PendingConstruction
                    | EInternalObjectFlags::AutoRTFMConstructionAborted | EInternalObjectFlags::Remote
                    | UE::GetAsyncLoadingInternalFlagsExclusion())) continue;
            const bool bActor = Object->IsA(ViewClass);
            auto* Mesh = bActor ? nullptr : Cast<UStaticMeshComponent>(Object);
            const auto* Owner = bActor ? Object : Mesh ? Object->GetOuter() : nullptr;
            if (!Owner || !Owner->IsA(ViewClass)) continue;
            const bool bPending = !IsValid(Object) || Object->IsUnreachable()
                || Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed);
            const bool bRegistered = Mesh && Mesh->IsRegistered();
            const bool bThisWorld = Owner->GetTypedOuter<UWorld>() == &World;
            FViews& Views = !bThisWorld ? Previous : bPending || (Mesh && !bRegistered) ? Removed : Current;
            const auto* Ism = Cast<UInstancedStaticMeshComponent>(Mesh);
            Views.Actors += bActor;
            Views.Meshes += Mesh != nullptr && !Ism;
            Views.Ism += Ism != nullptr;
            Views.Registered += bRegistered;
            Views.Instances += Ism ? Ism->GetInstanceCount() : 0;
            Views.Pending += bPending;
            Views.Bytes += Object->GetClass()->GetPropertiesSize() + (Ism ? Ism->PerInstanceSMData.GetAllocatedSize() : 0);
            if (Step == MenuAfter() + 1 && !bThisWorld && !bPending && !Survivor.IsValid()) Survivor = Object;
        }
        GUObjectArray.UnlockInternalArray();

        const auto LogViews = [this](const TCHAR* Cohort, const FViews& Views)
        {
            UE_LOG(LogSaveCheckProjectionMemory, Display,
                TEXT("views step=%d cohort=%s actors=%d mesh=%d ism=%d registered=%d instances=%d pending=%d object_bytes=%llu"),
                Step, Cohort, Views.Actors, Views.Meshes, Views.Ism, Views.Registered, Views.Instances, Views.Pending,
                static_cast<unsigned long long>(Views.Bytes));
        };
        LogViews(TEXT("current"), Current);
        LogViews(TEXT("removed"), Removed);
        LogViews(TEXT("previous"), Previous);
        FGenericMemoryStats Stats;
        GMalloc->GetAllocatorStats(Stats);
        UE_LOG(LogSaveCheckProjectionMemory, Display,
            TEXT("allocator step=%d name=%s counters=%d allocated=%llu os_allocated=%llu os_cache=%llu immediate_cache=%llu"),
            Step, GMalloc->GetDescriptiveName(), Stats.Find(TEXT("TotalAllocated")) && Stats.Find(TEXT("TotalOSAllocated")),
            static_cast<unsigned long long>(Stats.FindRef(TEXT("TotalAllocated"))),
            static_cast<unsigned long long>(Stats.FindRef(TEXT("TotalOSAllocated"))),
            static_cast<unsigned long long>(GMalloc->GetTotalFreeCachedMemorySize()),
            static_cast<unsigned long long>(GMalloc->GetImmediatelyFreeableCachedMemorySize()));

        if (auto* Object = Survivor.Get())
        {
            UE_LOG(LogSaveCheckProjectionMemory, Display, TEXT("survivor step=%d object=%s"), Step, *Object->GetFullName());
            FReferenceChainSearch Search(Object, EReferenceChainSearchMode::Minimal);
            Search.PrintResults(false, Object, ELogVerbosity::Display);
        }
    }

    void Settle(double Now)
    {
        Mark(TEXT("ready"));
        Stage = EStage::Settle;
        const bool bTimedGrid = bTrace && IsGrid();
        if (bTimedGrid && Step == GridFirst) GridDeadline = Now + FMath::Max(3.0, SettleSeconds);
        Deadline = bTimedGrid ? GridDeadline : Now + (bTrace ? FMath::Max(3.0, SettleSeconds) : SettleSeconds);
    }

    void Travel(UWorld& World, double Now)
    {
        const FName Map = IsMenu() ? MenuMap : GridMap;
        const FString Options = IsMenu() ? FString()
            : FString::Printf(TEXT("loadgame=%s?sessionName=SAQMemory"), IsGrid() && !bEmptyControl ? GridSlot : EmptySlot);
        PreviousWorld = &World;
        Stage = EStage::FindWorld;
        Deadline = Now + 75;
        Mark(TEXT("travel"));
        UGameplayStatics::OpenLevel(&World, Map, true, Options);
    }

    bool Tick()
    {
        const double Now = FPlatformTime::Seconds();
        if (Now > Deadline + 10) return End(false, TEXT("world or player did not become ready"));
        if (!GEngine) return true;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            auto* World = Context.World();
            if (!World || !World->IsGameWorld() || !World->HasBegunPlay() || PreviousWorld.Get() == World) continue;
            const FString Map = World->GetMapName();
            if (IsMenu() ? !Map.Contains(TEXT("Map_Menu_1_02")) : !Map.Contains(TEXT("Persistent_Level"))) continue;
            auto* Player = World->GetFirstPlayerController();
            if (!IsMenu() && (!Player || !Player->GetPawn())) continue;

            if (Stage == EStage::FindWorld)
            {
                if (Step == 0 && !Identify()) return End(false, TEXT("D3D12 adapter or mod module missing"));
                if (Step == MenuAfter() && !bGridUnloaded) return End(false, TEXT("grid world cleanup missing"));
                if (!IsMenu())
                {
                    auto* Save = UFGSaveSession::Get(World);
                    if (!Save) continue;
                    Save->SetAutoSaveEnabled(false);
                    if (!SetProjectionCamera(*World, *Player)) return End(false, TEXT("benchmark camera missing"));
                    if (IsGrid())
                    {
                        GridWorld = World;
                        bGridUnloaded = false;
                    }
                }
                UE_LOG(LogSaveCheckProjectionMemory, Display, TEXT("world step=%d map=%s"), Step, *Map);
                Settle(Now);
            }
            else if (Stage == EStage::ReleaseKeys)
            {
                if (!bIdle)
                    for (const auto& Key : { EKeys::G, EKeys::LeftShift, EKeys::LeftControl })
                        RoofStackKey(*Player, Key, IE_Released);
                Settle(Now);
            }
            else if (Now >= Deadline)
            {
                if (Stage == EStage::Settle)
                {
                    const bool bGridMiddle = IsGrid() && Step != GridFirst && Step != MenuAfter() - 1;
                    if (!IsMenu() && (!bSparseChecks || !bGridMiddle) && !AcceptLayout(*World))
                        return End(false, TEXT("unexpected projection layout"));
                    if (bTrace && !bGridMiddle) TraceObjects(*World);
                    Mark(TEXT("window_begin"));
                    Stage = EStage::Observe;
                    Deadline = (bTrace && IsGrid() ? GridDeadline : Now) + WindowSeconds;
                }
                else
                {
                    Mark(TEXT("window_end"));
                    if (Step == EmptyAfter())
                    {
                        if (Round == Rounds) return End(true, TEXT("complete memory route"));
                        ++Round;
                        GridFirst = ++Step;
                        Travel(*World, Now);
                        return true;
                    }
                    ++Step;
                    if (bTrace && Step == MenuAfter() + 1)
                    {
                        Mark(TEXT("gc_begin"));
                        CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
                        FlushRenderingCommands();
                        Mark(TEXT("gc_end"));
                        Settle(FPlatformTime::Seconds());
                    }
                    else if (Step <= GridFirst || Step >= MenuAfter())
                        Travel(*World, Now);
                    else
                    {
                        Mark(bIdle ? TEXT("hold") : TEXT("switch"));
                        if (!bIdle)
                            for (const auto& Key : { EKeys::LeftControl, EKeys::LeftShift, EKeys::G })
                                RoofStackKey(*Player, Key, IE_Pressed);
                        Stage = EStage::ReleaseKeys;
                        GridDeadline += WindowSeconds + SettleSeconds;
                        Deadline = Now + 5;
                    }
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

FTickerDelegate MakeProjectionMemoryCheck(double SettleSeconds, double WindowSeconds, int32 Cycles, int32 Rounds, bool bTrace, bool bIdle)
{
    const auto Check = MakeShared<FProjectionMemoryCheck>();
    Check->SettleSeconds = SettleSeconds;
    Check->WindowSeconds = WindowSeconds;
    Check->Cycles = Cycles;
    Check->Rounds = Rounds;
    Check->bTrace = bTrace;
    Check->bIdle = bIdle;
    Check->CleanupHandle = FWorldDelegates::OnWorldCleanup.AddSP(Check, &FProjectionMemoryCheck::OnWorldCleanup);
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
