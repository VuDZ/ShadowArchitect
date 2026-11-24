#include "ShadowAlignmentGeometry.h"
#include "Buildables/FGBuildableFoundation.h"
#include "Engine/StaticMesh.h"
#include "PhysicsEngine/BodySetup.h"
#include "ShadowPlan.h"

namespace
{
constexpr TCHAR FicsitFoundation[] = TEXT("/Game/FactoryGame/Buildable/Building/Foundation/"
    "Build_Foundation_8x1_01.Build_Foundation_8x1_01_C");
constexpr TCHAR ConcreteFoundation[] = TEXT("/Game/FactoryGame/Buildable/Building/Foundation/ConcreteSet/"
    "Build_Foundation_Concrete_8x1.Build_Foundation_Concrete_8x1_C");
constexpr TCHAR FlatRoof[] = TEXT("/Game/FactoryGame/Buildable/Building/Roof/Build_Roof_A_01.Build_Roof_A_01_C");

void TraceRoofBox(const FTransform& Frame, const FVector& HalfSize, const FVector& Start, const FVector& End,
    double& Limit, FVector& Surface, FVector& Normal)
{
    if (!Frame.IsValid() || HalfSize.GetMin() <= UE_SMALL_NUMBER) return;
    FVector LocalHit, LocalNormal;
    float Time;
    if (FMath::LineExtentBoxIntersection(FBox(-HalfSize, HalfSize), Frame.InverseTransformPosition(Start),
        Frame.InverseTransformPosition(End), FVector::ZeroVector, LocalHit, LocalNormal, Time)
        && Time > 0 && Time < Limit)
    {
        Limit = Time;
        Surface = Frame.TransformPosition(LocalHit);
        Normal = Frame.TransformVectorNoScale(LocalNormal);
    }
}
}

bool IsSupportedShadowRoof(const FShadowPart& Part)
{
    return Part.SourceClass == FlatRoof || Part.SourceClass == ConcreteFoundation;
}

TArray<FShadowRoofBox, TInlineAllocator<4>> GetShadowRoofBoxes(const FShadowPart& Part, const FTransform& PartFrame)
{
    TArray<FShadowRoofBox, TInlineAllocator<4>> Boxes;
    if (!IsSupportedShadowRoof(Part) || !PartFrame.IsValid()) return Boxes;
    if (Part.SourceClass == ConcreteFoundation)
    {
        if (PartFrame.GetScale3D().GetAbs().Equals(FVector::OneVector, UE_KINDA_SMALL_NUMBER)
            && PartFrame.GetUnitAxis(EAxis::Z).Equals(FVector::UpVector, UE_KINDA_SMALL_NUMBER))
            Boxes.Add({ PartFrame, FVector(400, 400, 50) });
        return Boxes;
    }

    if (!PartFrame.GetScale3D().Equals(FVector::OneVector, UE_KINDA_SMALL_NUMBER)) return Boxes;
    for (const FShadowMesh& Mesh : Part.Meshes)
    {
        if (!Mesh.InPart.IsValid() || Mesh.Asset.ToSoftObjectPath().GetAssetName() != TEXT("SM_Roof_Steel_01")) continue;
        UStaticMesh* Asset = Mesh.Asset.LoadSynchronous();
        const UBodySetup* Body = IsValid(Asset) ? Asset->GetBodySetup() : nullptr;
        if (!Body) continue;

        const FTransform MeshFrame = Mesh.InPart * PartFrame;
        for (const FKBoxElem& Box : Body->AggGeom.BoxElems)
            Boxes.Add({ Box.GetTransform() * MeshFrame, FVector(Box.X, Box.Y, Box.Z) * 0.5 });
    }
    return Boxes;
}

bool TraceShadowRoofTop(const FShadowPart& Part, const FTransform& PartFrame, const FVector& Start,
    const FVector& End, double& Limit, FVector& Surface)
{
    const auto Boxes = GetShadowRoofBoxes(Part, PartFrame);
    if (Boxes.IsEmpty()) return false;
    if (Part.SourceClass == ConcreteFoundation)
    {
        const FVector& HalfSize = Boxes[0].HalfSize;
        const double Top = PartFrame.GetLocation().Z + HalfSize.Z;
        if (Start.Z <= Top || End.Z >= Start.Z) return false;
        const double Time = (Top - Start.Z) / (End.Z - Start.Z);
        if (Time >= Limit) return false;
        const FVector Hit = FMath::Lerp(Start, End, Time);
        const FVector Local = PartFrame.InverseTransformPosition(Hit);
        if (FMath::Abs(Local.X) > HalfSize.X || FMath::Abs(Local.Y) > HalfSize.Y) return false;
        Limit = Time;
        Surface = Hit;
        return true;
    }

    FVector Normal = FVector::ZeroVector;
    FVector Hit = FVector::ZeroVector;
    double Closest = Limit;
    for (const auto& Box : Boxes)
        TraceRoofBox(Box.Frame, Box.HalfSize, Start, End, Closest, Hit, Normal);
    if (!Normal.Equals(FVector::UpVector, UE_KINDA_SMALL_NUMBER)) return false;
    Limit = Closest;
    Surface = Hit;
    return true;
}

bool GetShadowFoundationGrid(const FShadowPlacement& Plan, const FShadowPart& Part,
    const UClass& PreviewBuildClass, FShadowFoundationGrid& Out)
{
    if (Part.SourceClass != FicsitFoundation || PreviewBuildClass.GetPathName() != FicsitFoundation
        || !Plan.Transform.IsValid() || !Part.InPlan.IsValid()) return false;

    const auto* Foundation = PreviewBuildClass.GetDefaultObject<AFGBuildableFoundation>();
    if (!Foundation || Foundation->mIsFrame || !FMath::IsNearlyZero(Foundation->mElevation)) return false;

    const FVector Size(Foundation->mDepth, Foundation->mWidth, Foundation->mHeight);
    const FTransform Frame = Part.InPlan * Plan.Transform;
    if (Size.ContainsNaN() || Size.GetMin() <= UE_SMALL_NUMBER || !Frame.IsValid()
        || !Frame.GetScale3D().Equals(FVector::OneVector, UE_KINDA_SMALL_NUMBER)
        || !Frame.GetUnitAxis(EAxis::Z).Equals(FVector::UpVector, UE_KINDA_SMALL_NUMBER)) return false;

    Out.Frame = Frame;
    Out.Size = Size;
    return true;
}

bool FShadowFoundationGrid::AimAtEdge(const FVector& WorldStart, const FVector& WorldEnd,
    FShadowFoundationGuide& Out) const
{
    const FVector HalfSize = Size * 0.5;
    const FVector Start = Frame.InverseTransformPosition(WorldStart);
    const FVector End = Frame.InverseTransformPosition(WorldEnd);
    FVector Hit;
    FVector Side;
    float HitTime;
    if (!FMath::LineExtentBoxIntersection(FBox(-HalfSize, HalfSize), Start, End, FVector::ZeroVector,
        Hit, Side, HitTime) || HitTime <= 0 || Side.Z < 0) return false;

    if (Side.Z > 0)
    {
        const double DistanceX = HalfSize.X - FMath::Abs(Hit.X);
        const double DistanceY = HalfSize.Y - FMath::Abs(Hit.Y);
        Side = DistanceX <= DistanceY ? FVector(Hit.X < 0 ? -1 : 1, 0, 0)
                                     : FVector(0, Hit.Y < 0 ? -1 : 1, 0);
    }

    const FVector TopOffset(0, 0, HalfSize.Z);
    const FVector EdgeCenter = Side * HalfSize + TopOffset;
    const FVector EdgeTangent = FVector::CrossProduct(FVector::UpVector, Side) * HalfSize;
    const FVector NeighborCenter = Side * Size;
    Out.HitTime = HitTime;
    Out.LocalSide = Side;
    Out.EdgeStart = Frame.TransformPosition(EdgeCenter - EdgeTangent);
    Out.EdgeEnd = Frame.TransformPosition(EdgeCenter + EdgeTangent);
    Out.NeighborTransform = Frame;
    Out.NeighborTransform.SetLocation(Frame.TransformPosition(NeighborCenter));
    Out.NeighborTopCenter = Frame.TransformPosition(NeighborCenter + TopOffset);
    return true;
}
