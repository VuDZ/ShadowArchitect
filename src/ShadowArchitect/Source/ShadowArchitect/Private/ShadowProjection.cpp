#include "ShadowPlan.h"
#include "ShadowProjectionProfile.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Materials/Material.h"

namespace
{
const FName ShadowProxyMeshTag(TEXT("ShadowArchitect.ProxyMesh"));

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

void AShadowProjection::AddMesh(UStaticMesh& Asset, UMaterialInterface& Material, const FTransform& Relative,
    FShadowProjectionProfile* Profile, bool bProxy)
{
    const uint64 Start = Profile ? FPlatformTime::Cycles64() : 0;
    UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
    if (bProxy) Mesh->ComponentTags.Add(ShadowProxyMeshTag);
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
    const uint64 RegisterStart = Profile ? FPlatformTime::Cycles64() : 0;
    Mesh->RegisterComponent();
    if (Profile)
    {
        const uint64 End = FPlatformTime::Cycles64();
        Profile->ComponentCycles += End - Start;
        Profile->RegisterCycles += End - RegisterStart;
    }
}

bool AShadowProjection::BuildFootprint(const FShadowPlacement& Plan, UStaticMesh& Cube, UMaterialInterface& Material,
    FShadowProjectionProfile* Profile)
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

    AddMesh(Cube, Material, Relative, Profile);
    LocalPlanBounds = Footprint;
    return true;
}

bool AShadowProjection::Build(const FShadowPlacement& Plan, UStaticMesh& Cube, UMaterialInterface& Material,
    FShadowProjectionProfile* Profile)
{
    return BuildParts(Plan, Cube, Material, Material, Profile);
}

bool LoadShadowWireframeMaterials(UMaterialInterface*& Wireframe, UMaterialInterface*& BoxEdges, FShadowProjectionProfile* Profile)
{
    check(IsInGameThread());
    const uint64 Start = Profile ? FPlatformTime::Cycles64() : 0;
    Wireframe = LoadObject<UMaterialInterface>(nullptr,
        TEXT("/ShadowArchitect/Materials/M_ShadowWireframe.M_ShadowWireframe"));
    BoxEdges = LoadObject<UMaterialInterface>(nullptr,
        TEXT("/ShadowArchitect/Materials/M_ShadowBoxEdges.M_ShadowBoxEdges"));
    if (Profile) Profile->MaterialCycles += FPlatformTime::Cycles64() - Start;
    return IsValid(Wireframe) && IsValid(BoxEdges);
}

bool AShadowProjection::BuildWireframe(const FShadowPlacement& Plan, UStaticMesh& Cube, FShadowProjectionProfile* Profile)
{
    UMaterialInterface* Wireframe = nullptr;
    UMaterialInterface* BoxEdges = nullptr;
    return LoadShadowWireframeMaterials(Wireframe, BoxEdges, Profile)
        && BuildParts(Plan, Cube, *Wireframe, *BoxEdges, Profile);
}

bool AShadowProjection::PrepareWireframe(UMaterialInterface* Wireframe, UMaterialInterface* BoxEdges,
    TArray<FShadowWireframeChange>& Changes) const
{
    if (!IsValid(Wireframe) || !IsValid(BoxEdges) || GetInstanceComponents().IsEmpty()) return false;
    const auto* WireframeBase = Wireframe->GetMaterial();
    const auto* BoxBase = BoxEdges->GetMaterial();
    if (!WireframeBase || !BoxBase) return false;
    for (const auto& Component : GetInstanceComponents())
    {
        auto* Mesh = Cast<UStaticMeshComponent>(Component);
        if (!IsValid(Mesh) || !Mesh->IsRegistered() || !IsValid(Mesh->GetStaticMesh())) return false;
        const bool bProxy = Mesh->ComponentHasTag(ShadowProxyMeshTag);
        Changes.Add({ Mesh, bProxy ? BoxEdges : Wireframe, FMath::Max(1, Mesh->GetNumMaterials()),
            bProxy ? bool(BoxBase->Wireframe) : bool(WireframeBase->Wireframe) });
    }
    return true;
}

bool AShadowProjection::BuildParts(const FShadowPlacement& Plan, UStaticMesh& Cube,
    UMaterialInterface& MeshMaterial, UMaterialInterface& ProxyMaterial, FShadowProjectionProfile* Profile)
{
    check(IsInGameThread());
    const FBox Footprint = CalculateFootprint(Plan.Parts);
    if (Plan.SchemaVersion != 1 || !Footprint.IsValid || !Plan.Transform.IsValid()) return false;
    for (const FShadowPart& Part : Plan.Parts)
    {
        // Resolve the complete building before adding any components. A removed mod mesh
        // should produce one measured proxy, without overlapping scraps of the building.
        TArray<UStaticMesh*> Assets;
        for (const FShadowMesh& Mesh : Part.Meshes)
        {
            UStaticMesh* Asset = Profile ? Profile->ResolveMesh(Mesh.Asset) : Mesh.Asset.LoadSynchronous();
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
            if (Profile) ++Profile->ProxyParts;
            AddMesh(Cube, ProxyMaterial, Box * Part.InPlan, Profile, true);
        }
        else
        {
            for (int32 Index = 0; Index < Assets.Num(); ++Index)
                AddMesh(*Assets[Index], MeshMaterial, Part.Meshes[Index].InPart * Part.InPlan, Profile);
        }
    }
    LocalPlanBounds = Footprint;
    return true;
}
