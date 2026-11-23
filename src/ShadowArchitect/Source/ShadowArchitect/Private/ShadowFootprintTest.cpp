#if WITH_DEV_AUTOMATION_TESTS

#include "ShadowPlan.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShadowFootprintTest, "ShadowArchitect.Footprint.Bounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShadowFootprintTest::RunTest(const FString& Parameters)
{
    TArray<FShadowPart> Parts;
    Parts.AddDefaulted(2);
    Parts[0].BoundsSize = Parts[1].BoundsSize = FVector(100);
    Parts[1].InPlan.SetTranslation(FVector(200, 0, 100));

    const FBox Cubes = CalculateFootprint(Parts);
    TestTrue(TEXT("The cube pair has valid bounds"), Cubes.IsValid != 0);
    TestEqual(TEXT("Cube pair center in cm"), Cubes.GetCenter(), FVector(100, 0, 50));
    TestEqual(TEXT("Cube pair size in cm"), Cubes.GetSize(), FVector(300, 100, 200));

    FShadowPart& LongPart = Parts.AddDefaulted_GetRef();
    LongPart.BoundsCenter = FVector(40, -10, 15);
    LongPart.BoundsSize = FVector(400, 60, 40);
    LongPart.InPlan = FTransform(FRotator(0, 90, 0), FVector(280, 50, 140));

    const FBox WithLongPart = CalculateFootprint(Parts);
    TestEqual(TEXT("Center with the 90-degree part in cm"), WithLongPart.GetCenter(), FVector(135, 90, 62.5));
    TestEqual(TEXT("Size with the 90-degree part in cm"), WithLongPart.GetSize(), FVector(370, 400, 225));

    Parts.Reset();
    FShadowPart& DiagonalPart = Parts.AddDefaulted_GetRef();
    DiagonalPart.BoundsSize = FVector(200, 100, 20);
    DiagonalPart.InPlan = FTransform(FRotator(0, 45, 0), FVector::ZeroVector, FVector(2, -1, 1));

    const FBox Diagonal = CalculateFootprint(Parts);
    TestEqual(TEXT("Size with the 45-degree scaled part in cm"), Diagonal.GetSize(), FVector(353.553390593, 353.553390593, 20));

    Parts.Reset();
    TestFalse(TEXT("No footprint for an empty plan"), CalculateFootprint(Parts).IsValid != 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShadowFootprintProjectionTest, "ShadowArchitect.Footprint.Projection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShadowFootprintProjectionTest::RunTest(const FString& Parameters)
{
    FTestWorldWrapper World;
    if (!TestTrue(TEXT("The test world was created"), World.CreateTestWorld(EWorldType::EditorPreview))) return false;

    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    UMaterial* Material = UMaterial::GetDefaultMaterial(MD_Surface);
    if (!TestNotNull(TEXT("The engine cube is available"), Cube)
        || !TestNotNull(TEXT("The default material is available"), Material)) return false;

    FShadowPlacement Plan;
    Plan.Transform = FTransform(FRotator(0, 90, 0), FVector(20, 30, 40));
    FShadowPart& Part = Plan.Parts.AddDefaulted_GetRef();
    Part.BoundsCenter = FVector(100, 0, 50);
    Part.BoundsSize = FVector(300, 100, 200);

    AShadowProjection* View = World.GetTestWorld()->SpawnActor<AShadowProjection>(AShadowProjection::StaticClass(), Plan.Transform);
    if (!TestNotNull(TEXT("The rotated plan actor was spawned"), View)
        || !TestTrue(TEXT("The footprint mesh was built from the stored bounds"), View->BuildFootprint(Plan, *Cube, *Material))) return false;

    const TInlineComponentArray<UStaticMeshComponent*> Meshes(View);
    if (!TestEqual(TEXT("Number of footprint meshes"), Meshes.Num(), 1)) return false;

    const UStaticMeshComponent& Mesh = *Meshes[0];
    TestTrue(TEXT("The footprint mesh is attached to the plan root"), Mesh.GetAttachParent() == View->GetRootComponent());
    const FBox LocalBounds = Cube->GetBoundingBox().TransformBy(Mesh.GetRelativeTransform());
    const FBox WorldBounds = Cube->GetBoundingBox().TransformBy(Mesh.GetComponentTransform());
    TestEqual(TEXT("Center in the rotated plan axes in cm"), LocalBounds.GetCenter(), FVector(100, 0, 50));
    TestEqual(TEXT("Size in the rotated plan axes in cm"), LocalBounds.GetSize(), FVector(300, 100, 200));
    TestEqual(TEXT("Center in world axes in cm"), WorldBounds.GetCenter(), FVector(20, 130, 90));
    TestEqual(TEXT("Size in world axes in cm"), WorldBounds.GetSize(), FVector(100, 300, 200));
    return true;
}

#endif
