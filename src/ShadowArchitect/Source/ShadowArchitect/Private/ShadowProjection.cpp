#include "ShadowPlan.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"

namespace
{
bool HasValidPartGeometry(const FShadowPart& Part)
{
    return Part.InPlan.IsValid() && !Part.BoundsCenter.ContainsNaN()
        && !Part.BoundsSize.ContainsNaN() && Part.BoundsSize.GetMin() > 0;
}
}

FBox CalculateFootprint(const TArray<FShadowPart>& Parts)
{
    FBox Footprint(ForceInit);
    for (const FShadowPart& Part : Parts)
    {
        if (!HasValidPartGeometry(Part)) return FBox(ForceInit);

        const FVector Extent = Part.BoundsSize * 0.5;
        const FBox PartBounds(Part.BoundsCenter - Extent, Part.BoundsCenter + Extent);
        FVector Corners[8];
        PartBounds.GetVertices(Corners);
        for (const FVector& Corner : Corners)
        {
            const FVector InPlan = Part.InPlan.TransformPosition(Corner);
            if (InPlan.ContainsNaN()) return FBox(ForceInit);
            Footprint += InPlan;
        }
    }
    return Footprint;
}

AShadowProjection::AShadowProjection()
{
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("PlanOrigin")));
    RootComponent->SetMobility(EComponentMobility::Movable);
    PrimaryActorTick.bCanEverTick = false;
    SetActorEnableCollision(false);
}

void AShadowProjection::AddMesh(UStaticMesh& Asset, UMaterialInterface& Material, const FTransform& Relative)
{
    UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
    Mesh->SetupAttachment(GetRootComponent());
    Mesh->SetMobility(EComponentMobility::Movable);
    Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Mesh->SetGenerateOverlapEvents(false);
    Mesh->SetCanEverAffectNavigation(false);
    Mesh->SetCastShadow(false);
    Mesh->SetStaticMesh(&Asset);
    Mesh->SetRelativeTransform(Relative);
    for (int32 Slot = 0; Slot < FMath::Max(1, Mesh->GetNumMaterials()); ++Slot)
        Mesh->SetMaterial(Slot, &Material);
    AddInstanceComponent(Mesh);
    Mesh->RegisterComponent();
}

bool AShadowProjection::BuildFootprint(const FShadowPlacement& Plan, UStaticMesh& Cube, UMaterialInterface& Material)
{
    check(IsInGameThread());
    if (Plan.SchemaVersion != 1 || !Plan.Transform.IsValid()) return false;

    const FBox Footprint = CalculateFootprint(Plan.Parts);
    const FBox CubeBounds = Cube.GetBoundingBox();
    const FVector CubeSize = CubeBounds.GetSize();
    if (!Footprint.IsValid || !CubeBounds.IsValid || CubeSize.GetMin() <= 0) return false;

    const FVector Scale = Footprint.GetSize() / CubeSize;
    const FVector Offset = Footprint.GetCenter() - CubeBounds.GetCenter() * Scale;
    const FTransform Relative(FQuat::Identity, Offset, Scale);
    if (!Relative.IsValid()) return false;

    AddMesh(Cube, Material, Relative);
    return true;
}

bool AShadowProjection::Build(const FShadowPlacement& Plan, UStaticMesh& Cube, UMaterialInterface& Material)
{
    check(IsInGameThread());
    if (Plan.SchemaVersion != 1 || Plan.Parts.IsEmpty() || !Plan.Transform.IsValid()) return false;
    for (const FShadowPart& Part : Plan.Parts)
    {
        if (!HasValidPartGeometry(Part)) return false;

        // Resolve the complete building before adding any components. A removed mod mesh
        // should produce one measured proxy, without overlapping scraps of the building.
        TArray<UStaticMesh*> Assets;
        for (const FShadowMesh& Mesh : Part.Meshes)
        {
            UStaticMesh* Asset = Mesh.Asset.LoadSynchronous();
            if (!IsValid(Asset) || !Mesh.InPart.IsValid()) break;
            Assets.Add(Asset);
        }
        if (Assets.IsEmpty() || Assets.Num() != Part.Meshes.Num())
        {
            const FVector CubeSize = Cube.GetBoundingBox().GetSize();
            if (CubeSize.GetMin() <= 0) return false;
            FTransform Box(FQuat::Identity, Part.BoundsCenter, Part.BoundsSize / CubeSize);
            AddMesh(Cube, Material, Box * Part.InPlan);
        }
        else
        {
            for (int32 Index = 0; Index < Assets.Num(); ++Index)
                AddMesh(*Assets[Index], Material, Part.Meshes[Index].InPart * Part.InPlan);
        }
    }
    return true;
}
