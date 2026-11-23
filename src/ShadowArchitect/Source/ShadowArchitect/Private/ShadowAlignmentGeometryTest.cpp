#if WITH_DEV_AUTOMATION_TESTS

#include "ShadowAlignmentGeometry.h"
#include "Misc/AutomationTest.h"
#include "ShadowPlan.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShadowFoundationGuideTest, "ShadowArchitect.Alignment.FoundationGuide",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShadowFoundationGuideTest::RunTest(const FString& Parameters)
{
    const FTransform Plan(FRotator(0, 27, 0), FVector(250, -720, 390));
    const FTransform Part(FRotator(0, 10, 0), FVector(-310, 950, 120));
    FShadowFoundationGrid Grid;
    Grid.Frame = Part * Plan;
    Grid.Size = FVector(800, 800, 100);

    FShadowFoundationGuide Guide;
    const auto AimAt = [&Grid, &Guide](const FVector& Start, const FVector& End)
    {
        return Grid.AimAtEdge(Grid.Frame.TransformPosition(Start), Grid.Frame.TransformPosition(End), Guide);
    };
    const auto CheckGuide = [this, &Grid, &Guide](const FVector& Side, const FVector& EdgeStart,
        const FVector& EdgeEnd)
    {
        TestEqual(TEXT("Selected side in the part axes"), Guide.LocalSide, Side);
        TestEqual(TEXT("Step between the centers in cm"),
            FVector::Dist(Guide.NeighborTransform.GetLocation(), Grid.Frame.GetLocation()), 800.0);
        TestTrue(TEXT("Rotation stays on the part axes"),
            Guide.NeighborTransform.GetRotation().Equals(Grid.Frame.GetRotation()));
        TestEqual(TEXT("Neighbor top is level with the source"), Guide.NeighborTopCenter.Z,
            Grid.Frame.GetLocation().Z + 50.0);
        TestEqual(TEXT("First end of the source seam in world coordinates"), Guide.EdgeStart,
            Grid.Frame.TransformPosition(EdgeStart));
        TestEqual(TEXT("Second end of the source seam in world coordinates"), Guide.EdgeEnd,
            Grid.Frame.TransformPosition(EdgeEnd));
        const FVector TestNeighborEdge = Guide.NeighborTransform.TransformPosition(-Side * 400 + FVector(0, 0, 50));
        TestEqual(TEXT("No distance between adjacent edges"), (Guide.EdgeStart + Guide.EdgeEnd) * 0.5,
            TestNeighborEdge);
    };

    if (TestTrue(TEXT("A horizontal ray hits the +X face"), AimAt(FVector(600, 45, 10), FVector(0, 45, 10))))
        CheckGuide(FVector::ForwardVector, FVector(400, -400, 50), FVector(400, 400, 50));
    if (TestTrue(TEXT("A downward ray hits the top near the +Y face"),
        AimAt(FVector(30, 350, 600), FVector(30, 350, 0))))
        CheckGuide(FVector::RightVector, FVector(400, 400, 50), FVector(-400, 400, 50));

    TestFalse(TEXT("A ray outside the cell misses"), AimAt(FVector(500, 0, 600), FVector(500, 0, 0)));
    TestFalse(TEXT("The bottom face is ignored"), AimAt(FVector(0, 0, -600), FVector::ZeroVector));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShadowConcreteRoofTest, "ShadowArchitect.Alignment.ConcreteRoof",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShadowConcreteRoofTest::RunTest(const FString& Parameters)
{
    FShadowPart Part;
    Part.SourceClass = TEXT("/Game/FactoryGame/Buildable/Building/Foundation/ConcreteSet/"
        "Build_Foundation_Concrete_8x1.Build_Foundation_Concrete_8x1_C");
    const FTransform Frame(FRotator(0, 30, 0), FVector(-1200, 1750, 850));
    const FVector Start = Frame.TransformPosition(FVector(150, -150, 500));
    const FVector End = Frame.TransformPosition(FVector(150, -150, -100));
    FVector Surface;
    double Limit = 1.0;
    if (TestTrue(TEXT("A downward ray hits the supported concrete slab"), TraceShadowRoofTop(Part, Frame, Start, End, Limit, Surface)))
    {
        TestTrue(TEXT("The surface hit is 50 cm over the slab pivot"),
            Surface.Equals(Frame.TransformPosition(FVector(150, -150, 50)), UE_KINDA_SMALL_NUMBER));
        TestEqual(TEXT("The closer rooftop hit becomes the new ray limit"), Limit, 0.75);
    }
    Limit = 0.6;
    TestFalse(TEXT("An obstruction in front of the roof hides the target"), TraceShadowRoofTop(Part, Frame, Start, End, Limit, Surface));
    Limit = 1.0;
    TestFalse(TEXT("A ray from below cannot select the roof"), TraceShadowRoofTop(Part, Frame, End, Start, Limit, Surface));
    const FVector Outside = Frame.TransformVector(FVector(300, 0, 0));
    TestFalse(TEXT("A ray outside the slab misses"), TraceShadowRoofTop(Part, Frame, Start + Outside, End + Outside, Limit, Surface));
    return true;
}

#endif
