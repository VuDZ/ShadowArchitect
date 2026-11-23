#if WITH_DEV_AUTOMATION_TESTS

#include "ShadowPlan.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShadowWireframeTest, "ShadowArchitect.Wireframe.Parts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShadowWireframeTest::RunTest(const FString& Parameters)
{
    FTestWorldWrapper World;
    if (!TestTrue(TEXT("The test world was created"), World.CreateTestWorld(EWorldType::EditorPreview))) return false;

    auto* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!TestNotNull(TEXT("The proxy cube is available"), Cube)) return false;

    FShadowPlacement Plan;
    Plan.Transform = FTransform(FRotator(0, 90, 0), FVector(1000, 2000, 100));
    Plan.Parts.AddDefaulted(2);
    FShadowPart& Resolved = Plan.Parts[0];
    Resolved.BoundsSize = FVector(100);
    Resolved.InPlan = FTransform(FRotator(0, -30, 0), FVector(300, 20, 50));
    FShadowMesh& Mesh = Resolved.Meshes.AddDefaulted_GetRef();
    Mesh.Asset = Cube;
    Mesh.InPart = FTransform(FVector(15, -10, 40));

    FShadowPart& Missing = Plan.Parts[1];
    Missing = Resolved;
    Missing.InPlan = FTransform(FRotator(0, 50, 0), FVector(-200, 30, 80));
    Missing.BoundsCenter = FVector(10, -15, 20);
    Missing.BoundsSize = FVector(200, 80, 60);
    Missing.Meshes.AddDefaulted();

    auto* View = World.GetTestWorld()->SpawnActor<AShadowProjection>(AShadowProjection::StaticClass(), Plan.Transform);
    if (!TestNotNull(TEXT("The projection was spawned"), View)
        || !TestTrue(TEXT("The wireframe was built using the plugin materials"), View->BuildWireframe(Plan, *Cube))) return false;

    const TInlineComponentArray<UStaticMeshComponent*> Components(View);
    if (!TestEqual(TEXT("One mesh and one proxy"), Components.Num(), 2)) return false;

    int32 WireframeMeshes = 0;
    for (const UStaticMeshComponent* Component : Components)
    {
        const FTransform& Pose = Component->GetRelativeTransform();
        if (Component->GetMaterial(0)->GetFName() == FName(TEXT("M_ShadowWireframe")))
        {
            ++WireframeMeshes;
            TestTrue(TEXT("The real mesh retains its captured pose"), Pose.Equals(Mesh.InPart * Resolved.InPlan));
            TestTrue(TEXT("The real mesh uses the raster wireframe path"), Component->IsDisallowNanite());
        }
        else
        {
            TestEqual(TEXT("A partly missing building uses the box edge material"),
                Component->GetMaterial(0)->GetFName(), FName(TEXT("M_ShadowBoxEdges")));
            const FBox Box = Cube->GetBoundingBox().TransformBy(Pose * Missing.InPlan.Inverse());
            TestEqual(TEXT("The proxy center in its part axes"), Box.GetCenter(), Missing.BoundsCenter);
            TestEqual(TEXT("The proxy size in its part axes"), Box.GetSize(), Missing.BoundsSize);
        }
    }
    TestEqual(TEXT("Only the available building uses its original mesh"), WireframeMeshes, 1);
    return true;
}

#endif
