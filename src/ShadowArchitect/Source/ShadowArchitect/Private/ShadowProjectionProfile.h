#pragma once

#include "ShadowPlan.h"

class FShadowProjectionProfile
{
public:
    using FViews = TMap<FGuid, TWeakObjectPtr<AShadowProjection>>;

    FShadowProjectionProfile(const TCHAR* Operation, EShadowView From, EShadowView To, int32 Plans, const FViews& Views);

    FShadowProjectionProfile* Enabled() { return bEnabled ? this : nullptr; }
    UStaticMesh* ResolveMesh(const TSoftObjectPtr<UStaticMesh>& Mesh);
    void Sample(const TCHAR* Phase);
    void Finish(EShadowView ActualMode, bool bSuccess, int32 FailedPlans);

    uint64 MaterialCycles = 0;
    uint64 ComponentCycles = 0;
    uint64 RegisterCycles = 0;
    uint64 ReuseCycles = 0;
    int32 ReusedMeshes = 0;
    int32 ProxyParts = 0;

private:
    const TCHAR* Operation;
    EShadowView From;
    EShadowView To;
    int32 Plans;
    const FViews& Views;
    bool bEnabled;
    uint32 Serial = 0;
    uint64 StartCycles = 0;
    uint64 CoexistStartCycles = 0;
    uint64 CoexistEndCycles = 0;
    uint64 MeshCycles = 0;
    int32 MeshRequests = 0;
    int32 ResolvedBefore = 0;
    int32 MissingMeshes = 0;
    TSet<FSoftObjectPath> Paths;
    double MaxCommitMiB = 0;
};
