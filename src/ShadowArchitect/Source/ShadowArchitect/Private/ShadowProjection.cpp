#include "ShadowPlan.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"

AShadowProjection::AShadowProjection()
{
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("PlanOrigin")));
    RootComponent->SetMobility(EComponentMobility::Movable);
    PrimaryActorTick.bCanEverTick = false;
    SetActorEnableCollision(false);
}

bool AShadowProjection::Build(const FShadowPlacement& Plan, UStaticMesh& Cube, UMaterialInterface& Material)
{
    check(IsInGameThread());
    if (Plan.SchemaVersion != 1 || Plan.Parts.IsEmpty() || !Plan.Transform.IsValid()) return false;
    auto AddMesh = [this, &Material](UStaticMesh* Asset, const FTransform& Relative)
    {
        UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
        Mesh->SetupAttachment(GetRootComponent());
        Mesh->SetMobility(EComponentMobility::Movable);
        Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Mesh->SetGenerateOverlapEvents(false);
        Mesh->SetCanEverAffectNavigation(false);
        Mesh->SetCastShadow(false);
        Mesh->SetStaticMesh(Asset);
        Mesh->SetRelativeTransform(Relative);
        for (int32 Slot = 0; Slot < FMath::Max(1, Mesh->GetNumMaterials()); ++Slot)
            Mesh->SetMaterial(Slot, &Material);
        AddInstanceComponent(Mesh);
        Mesh->RegisterComponent();
    };
    for (const FShadowPart& Part : Plan.Parts)
    {
        if (!Part.InPlan.IsValid() || Part.BoundsCenter.ContainsNaN()
            || Part.BoundsSize.ContainsNaN() || Part.BoundsSize.GetMin() <= 0) return false;

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
            AddMesh(&Cube, Box * Part.InPlan);
        }
        else
        {
            for (int32 Index = 0; Index < Assets.Num(); ++Index)
                AddMesh(Assets[Index], Part.Meshes[Index].InPart * Part.InPlan);
        }
    }
    return true;
}
