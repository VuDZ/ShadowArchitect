#include "ShadowPlan.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Materials/Material.h"

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
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = false;
    SetActorEnableCollision(false);
}

void AShadowProjection::SetSelectionMarker(const FBox& Footprint)
{
    if (!Footprint.IsValid)
    {
        if (SelectionMarker) SelectionMarker->DestroyComponent();
        SelectionMarker = nullptr;
        SetActorTickEnabled(false);
        return;
    }

    // Keep the marker outside the instance components replaced when changing display mode.
    SelectionMarker = NewObject<UTextRenderComponent>(this, NAME_None, RF_Transient);
    SelectionMarker->SetupAttachment(GetRootComponent());
    SelectionMarker->SetMobility(EComponentMobility::Movable);
    SelectionMarker->SetAbsolute(false, true, true);
    const FVector Center = Footprint.GetCenter();
    SelectionMarker->SetRelativeLocation(FVector(Center.X, Center.Y, Footprint.Max.Z + 24.0));
    SelectionMarker->SetText(NSLOCTEXT("ShadowArchitect", "SelectedPlanMarker", "SELECTED"));
    SelectionMarker->SetHorizontalAlignment(EHTA_Center);
    SelectionMarker->SetVerticalAlignment(EVRTA_TextBottom);
    SelectionMarker->SetTextRenderColor(FColor(255, 198, 75));
    SelectionMarker->SetWorldSize(32.0f);
    SelectionMarker->SetCanEverAffectNavigation(false);
    SelectionMarker->SetCastShadow(false);
    SelectionMarker->RegisterComponent();
    SetActorTickEnabled(true);
}

void AShadowProjection::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!SelectionMarker || IsHidden()) return;
    const auto* Player = GetWorld()->GetFirstPlayerController();
    if (!IsValid(Player)) return;

    FVector Camera;
    FRotator Aim;
    Player->GetPlayerViewPoint(Camera, Aim);
    const FVector ToCamera = Camera - SelectionMarker->GetComponentLocation();
    SelectionMarker->SetWorldRotation(ToCamera.Rotation());
    SelectionMarker->SetWorldScale3D(FVector(FMath::Clamp(ToCamera.Size() / 1200.0, 0.45, 12.0)));
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
    if (const UMaterial* BaseMaterial = Material.GetMaterial())
        Mesh->bDisallowNanite = BaseMaterial->Wireframe;
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
    return BuildParts(Plan, Cube, Material, Material);
}

bool AShadowProjection::BuildWireframe(const FShadowPlacement& Plan, UStaticMesh& Cube)
{
    check(IsInGameThread());
    auto* Wireframe = LoadObject<UMaterialInterface>(nullptr,
        TEXT("/ShadowArchitect/Materials/M_ShadowWireframe.M_ShadowWireframe"));
    auto* BoxEdges = LoadObject<UMaterialInterface>(nullptr,
        TEXT("/ShadowArchitect/Materials/M_ShadowBoxEdges.M_ShadowBoxEdges"));
    return Wireframe && BoxEdges && BuildParts(Plan, Cube, *Wireframe, *BoxEdges);
}

bool AShadowProjection::BuildParts(const FShadowPlacement& Plan, UStaticMesh& Cube,
    UMaterialInterface& MeshMaterial, UMaterialInterface& ProxyMaterial)
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
            const FBox CubeBounds = Cube.GetBoundingBox();
            const FVector CubeSize = CubeBounds.GetSize();
            if (CubeSize.GetMin() <= 0) return false;
            const FVector Scale = Part.BoundsSize / CubeSize;
            const FTransform Box(FQuat::Identity, Part.BoundsCenter - CubeBounds.GetCenter() * Scale, Scale);
            AddMesh(Cube, ProxyMaterial, Box * Part.InPlan);
        }
        else
        {
            for (int32 Index = 0; Index < Assets.Num(); ++Index)
                AddMesh(*Assets[Index], MeshMaterial, Part.Meshes[Index].InPart * Part.InPlan);
        }
    }
    return true;
}
