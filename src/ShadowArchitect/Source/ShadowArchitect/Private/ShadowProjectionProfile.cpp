#include "ShadowProjectionProfile.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY_STATIC(LogShadowProjectionProfile, Log, All);

namespace
{
const TCHAR* ProfileViewName(EShadowView Mode)
{
    switch (Mode)
    {
    case EShadowView::Ghost: return TEXT("Ghost");
    case EShadowView::Wireframe: return TEXT("Wireframe");
    case EShadowView::Footprint: return TEXT("Footprint");
    }
    return TEXT("unknown");
}
}

FShadowProjectionProfile::FShadowProjectionProfile(const TCHAR* OperationName, EShadowView FromMode,
    EShadowView ToMode, int32 PlanCount, const FViews& InViews)
    : Operation(OperationName), From(FromMode), To(ToMode), Plans(PlanCount), Views(InViews),
      bEnabled(FParse::Param(FCommandLine::Get(), TEXT("ShadowProjectionProfile")))
{
    if (!bEnabled) return;
    static uint32 NextSerial = 0;
    Serial = ++NextSerial;
    Sample(TEXT("before"));
    StartCycles = FPlatformTime::Cycles64();
}

UStaticMesh* FShadowProjectionProfile::ResolveMesh(const TSoftObjectPtr<UStaticMesh>& Mesh)
{
    ++MeshRequests;
    if (Mesh.IsValid()) ++ResolvedBefore;
    if (!Mesh.IsNull()) Paths.Add(Mesh.ToSoftObjectPath());
    const uint64 Start = FPlatformTime::Cycles64();
    UStaticMesh* Result = Mesh.LoadSynchronous();
    MeshCycles += FPlatformTime::Cycles64() - Start;
    if (!IsValid(Result)) ++MissingMeshes;
    return Result;
}

void FShadowProjectionProfile::Sample(const TCHAR* Phase)
{
    if (!bEnabled) return;
    int32 Actors = 0;
    int32 Meshes = 0;
    for (const auto& Entry : Views)
    {
        if (const auto* Actor = Entry.Value.Get())
        {
            ++Actors;
            for (const auto* Component : Actor->GetInstanceComponents())
                if (IsValid(Component) && Component->IsRegistered() && Component->IsA<UStaticMeshComponent>())
                    ++Meshes;
        }
    }
    const double CommitMiB = double(FPlatformMemory::GetStats().UsedVirtual) / (1024 * 1024);
    MaxCommitMiB = FMath::Max(MaxCommitMiB, CommitMiB);
    UE_LOG(LogShadowProjectionProfile, Display,
        TEXT("sample serial=%u op=%s phase=%s from=%s to=%s plans=%d actors=%d meshes=%d commit_mib=%.3f mono_s=%.6f frame=%llu"),
        Serial, Operation, Phase, ProfileViewName(From), ProfileViewName(To), Plans, Actors, Meshes,
        CommitMiB, FPlatformTime::Seconds(), static_cast<unsigned long long>(GFrameCounter));
}

void FShadowProjectionProfile::Finish(EShadowView ActualMode, bool bSuccess, int32 FailedPlans)
{
    if (!bEnabled) return;
    const uint64 End = FPlatformTime::Cycles64();
    Sample(TEXT("after"));
    UE_LOG(LogShadowProjectionProfile, Display,
        TEXT("operation serial=%u op=%s from=%s to=%s actual=%s success=%d failed_plans=%d gt_ms=%.3f")
        TEXT(" mesh_ms=%.3f material_ms=%.3f component_ms=%.3f register_ms=%.3f mesh_requests=%d unique_meshes=%d")
        TEXT(" resolved_before=%d missing_meshes=%d proxy_parts=%d sampled_peak_commit_mib=%.3f reused_meshes=%d reuse_ms=%.3f"),
        Serial, Operation, ProfileViewName(From), ProfileViewName(To), ProfileViewName(ActualMode), bSuccess, FailedPlans,
        FPlatformTime::ToMilliseconds64(End - StartCycles), FPlatformTime::ToMilliseconds64(MeshCycles),
        FPlatformTime::ToMilliseconds64(MaterialCycles), FPlatformTime::ToMilliseconds64(ComponentCycles),
        FPlatformTime::ToMilliseconds64(RegisterCycles), MeshRequests, Paths.Num(), ResolvedBefore, MissingMeshes,
        ProxyParts, MaxCommitMiB, ReusedMeshes, FPlatformTime::ToMilliseconds64(ReuseCycles));
}
