#pragma once

#include "Components/InstancedStaticMeshComponent.h"
#include "Containers/BitArray.h"
#include "Engine/StaticMesh.h"

struct FProjectionPiece
{
    FString Asset;
    FTransform Relative;
    int32 Slots;
    bool bProxy;
};

inline bool AppendProjectionPieces(const UStaticMeshComponent& Component, TArray<FProjectionPiece>& Pieces)
{
    const UStaticMesh* Mesh = Component.GetStaticMesh();
    if (!Mesh || !Component.IsRegistered()) return false;
    const bool bProxy = Component.ComponentHasTag(FName(TEXT("ShadowArchitect.ProxyMesh")));
    const FString Asset = Mesh->GetPathName();
    const int32 Slots = Component.GetNumMaterials();
    if (const auto* Group = Cast<UInstancedStaticMeshComponent>(&Component))
    {
        if (bProxy || Group->GetInstanceCount() < 2) return false;
        for (int32 Index = 0; Index < Group->GetInstanceCount(); ++Index)
        {
            FTransform Transform;
            if (!Group->GetInstanceTransform(Index, Transform, false)) return false;
            Pieces.Add({ Asset, Transform * Group->GetRelativeTransform(), Slots, false });
        }
    }
    else Pieces.Add({ Asset, Component.GetRelativeTransform(), Slots, bProxy });
    return true;
}

inline bool MatchProjectionPieces(const TArray<FProjectionPiece>& Expected, const TArray<FProjectionPiece>& Actual)
{
    if (Expected.Num() != Actual.Num()) return false;
    TBitArray<> Matched(false, Actual.Num());
    for (const auto& Piece : Expected)
    {
        int32 Match = 0;
        for (; Match < Actual.Num(); ++Match)
        {
            const auto& Candidate = Actual[Match];
            if (!Matched[Match] && Piece.Asset == Candidate.Asset && Piece.Slots == Candidate.Slots
                && Piece.bProxy == Candidate.bProxy && Piece.Relative.Equals(Candidate.Relative, 0.001)) break;
        }
        if (Match == Actual.Num()) return false;
        Matched[Match] = true;
    }
    return true;
}
