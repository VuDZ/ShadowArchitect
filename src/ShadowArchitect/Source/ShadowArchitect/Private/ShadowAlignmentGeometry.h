#pragma once

#include "CoreMinimal.h"

struct FShadowPart;
struct FShadowPlacement;

struct FShadowFoundationGuide
{
    float HitTime = 1;
    FVector LocalSide = FVector::ZeroVector;
    FVector EdgeStart = FVector::ZeroVector;
    FVector EdgeEnd = FVector::ZeroVector;
    FVector NeighborTopCenter = FVector::ZeroVector;
    FTransform NeighborTransform = FTransform::Identity;
};

struct FShadowFoundationGrid
{
    FTransform Frame = FTransform::Identity;
    FVector Size = FVector::ZeroVector;

    bool AimAtEdge(const FVector& WorldStart, const FVector& WorldEnd, FShadowFoundationGuide& Out) const;
};

bool GetShadowFoundationGrid(const FShadowPlacement& Plan, const FShadowPart& Part,
    const UClass& PreviewBuildClass, FShadowFoundationGrid& Out);

bool IsSupportedShadowRoof(const FShadowPart& Part);
bool TraceShadowRoofTop(const FShadowPart& Part, const FTransform& PartFrame, const FVector& Start,
    const FVector& End, double& Limit, FVector& Surface);
