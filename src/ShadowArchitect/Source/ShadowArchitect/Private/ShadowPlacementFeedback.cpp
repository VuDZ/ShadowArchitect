#include "ShadowPlan.h"
#include "ShadowAlignmentGeometry.h"
#include "ShadowWorldLines.h"
#include "Components/MeshComponent.h"
#include "Engine/LocalPlayer.h"
#include "FGBlueprintSubsystem.h"
#include "Hologram/FGBlueprintHologram.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Styling/CoreStyle.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ShadowPlacementFeedback"

namespace
{
constexpr double ContactTolerance = 0.1;
const FLinearColor ClearColor(0.07f, 1.0f, 0.24f);
const FLinearColor PossibleColor(1.0f, 0.57f, 0.06f);
const FLinearColor ConflictColor(1.0f, 0.09f, 0.19f);
const FLinearColor OtherPartColor(1.0f, 0.70f, 0.11f);

struct FPlacementPartBox
{
    FTransform Frame;
    FVector HalfSize;
    FBox BroadBounds;

    static FPlacementPartBox Make(const FTransform& Frame, const FVector& LocalHalfSize)
    {
        return { Frame, LocalHalfSize * Frame.GetScale3D().GetAbs(),
            FBox(-LocalHalfSize, LocalHalfSize).TransformBy(Frame) };
    }

    static FPlacementPartBox Make(const FShadowPart& Part, const FTransform& Pose)
    {
        FTransform Frame = Part.InPlan * Pose;
        Frame.SetLocation(Frame.TransformPosition(Part.BoundsCenter));
        return Make(Frame, Part.BoundsSize * 0.5);
    }

    double RadiusAlong(const FVector& Axis) const
    {
        return HalfSize.X * FMath::Abs(Axis | Frame.GetUnitAxis(EAxis::X))
            + HalfSize.Y * FMath::Abs(Axis | Frame.GetUnitAxis(EAxis::Y))
            + HalfSize.Z * FMath::Abs(Axis | Frame.GetUnitAxis(EAxis::Z));
    }
};

bool HaveVolumeIntersection(const FPlacementPartBox& A, const FPlacementPartBox& B)
{
    if (!A.BroadBounds.Intersect(B.BroadBounds)) return false;
    const FVector Delta = B.Frame.GetLocation() - A.Frame.GetLocation();
    const auto Separates = [&A, &B, &Delta](const FVector& Axis)
    {
        return !Axis.IsNearlyZero()
            && FMath::Abs(Delta | Axis) >= A.RadiusAlong(Axis) + B.RadiusAlong(Axis) - ContactTolerance;
    };

    // The face normals and cross products of the two boxes' edges are the separating axes.
    constexpr EAxis::Type Axes[] = { EAxis::X, EAxis::Y, EAxis::Z };
    for (const auto Axis : Axes)
        if (Separates(A.Frame.GetUnitAxis(Axis)) || Separates(B.Frame.GetUnitAxis(Axis))) return false;
    for (const auto AxisA : Axes)
        for (const auto AxisB : Axes)
            if (Separates((A.Frame.GetUnitAxis(AxisA) ^ B.Frame.GetUnitAxis(AxisB)).GetSafeNormal())) return false;
    return true;
}

void OutlinePart(TArray<FShadowAlignmentSegment>& Lines, const FPlacementPartBox& Box, const FLinearColor& Color)
{
    const FVector X = Box.Frame.GetUnitAxis(EAxis::X) * Box.HalfSize.X;
    const FVector Y = Box.Frame.GetUnitAxis(EAxis::Y) * Box.HalfSize.Y;
    const FVector Z = Box.Frame.GetUnitAxis(EAxis::Z) * Box.HalfSize.Z;
    FVector Corners[8];
    for (int32 Vertex = 0; Vertex < 8; ++Vertex)
        Corners[Vertex] = Box.Frame.GetLocation() + (Vertex & 1 ? X : -X)
            + (Vertex & 2 ? Y : -Y) + (Vertex & 4 ? Z : -Z);
    for (int32 Vertex = 0; Vertex < 8; ++Vertex)
        for (int32 Edge = 1; Edge <= 4; Edge *= 2)
            if (!(Vertex & Edge))
            {
                auto& Line = Lines.AddDefaulted_GetRef();
                Line.Start = Corners[Vertex];
                Line.End = Corners[Vertex | Edge];
                Line.Color = Color;
                Line.Thickness = 2.5f;
            }
}
}

class FShadowPlacementFeedback
{
public:
    FShadowPlacementFeedback(APlayerController& PC, ULocalPlayer& Player, UGameViewportClient& Client,
        TFunction<TConstArrayView<FShadowAlignmentSegment>()> GetLines)
        : Viewport(&Client), LocalPlayer(&Player)
    {
        Widget = SNew(SOverlay)
            + SOverlay::Slot()
            [ SAssignNew(LineWidget, SShadowWorldLines).Player(&PC).Clipping(EWidgetClipping::ClipToBounds)
                .GetLines(MoveTemp(GetLines)) ]
            + SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Top).Padding(FMargin(10, 210, 10, 0))
            [ SNew(SBox).MinDesiredWidth(280).MaxDesiredWidth(460)
                [ SAssignNew(LabelBorder, SBorder).Padding(FMargin(11, 5))
                    .BorderBackgroundColor(FLinearColor(0.025f, 0.035f, 0.045f, 0.94f))
                    [ SAssignNew(Label, STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
                        .Tag(TEXT("ShadowArchitect.PlacementFeedback"))
                        .Justification(ETextJustify::Center).WrapTextAt(430) ] ] ];
        Widget->SetVisibility(EVisibility::Collapsed);
        Client.AddViewportWidgetForPlayer(&Player, Widget.ToSharedRef(), 51);
    }

    ~FShadowPlacementFeedback()
    {
        RestoreMaterials();
        if (auto* Client = Viewport.Get())
            if (auto* Player = LocalPlayer.Get())
                Client->RemoveViewportWidgetForPlayer(Player, Widget.ToSharedRef());
    }

    void RefreshGeometry(AFGBlueprintHologram& Current)
    {
        if (Preview.Get() != &Current || Descriptor.Get() != Current.mBlueprintDescriptor.Get()
            || RootCount != Current.mBuildableToNewRoot.Num())
        {
            RestoreMaterials();
            Preview = &Current;
            Descriptor = Current.mBlueprintDescriptor;
            RootCount = Current.mBuildableToNewRoot.Num();
            Parts.Reset();
        }
        if (Parts.IsEmpty() && CaptureBlueprintGeometry(Current, Parts))
            LocalBounds = CalculateFootprint(Parts);
    }

    void Display(AFGBlueprintHologram& Current, const FText& Text, const FLinearColor& Color, bool bNativeValid)
    {
        Label->SetText(Text);
        Label->SetColorAndOpacity(Color);
        LabelBorder->SetVisibility(Text.IsEmpty() ? EVisibility::Collapsed : EVisibility::HitTestInvisible);
        LineWidget->Invalidate(EInvalidateWidgetReason::Paint);
        Widget->SetVisibility(Text.IsEmpty() && Lines.IsEmpty() ? EVisibility::Collapsed : EVisibility::HitTestInvisible);

        if (!bNativeValid || Text.IsEmpty())
        {
            RestoreMaterials();
            return;
        }
        if (!TintMaterial.IsValid())
        {
            auto* Asset = LoadObject<UMaterialInterface>(nullptr,
                TEXT("/ShadowArchitect/Materials/M_ShadowPlacementTint.M_ShadowPlacementTint"));
            if (!Asset) return;
            TintMaterial.Reset(UMaterialInstanceDynamic::Create(Asset, LocalPlayer.Get()));
        }
        TintMaterial->SetVectorParameterValue(TEXT("PlacementColor"), Color);
        if (!OriginalMaterials.IsEmpty()) return;

        for (const auto& Pair : Current.mBuildableToNewRoot)
        {
            if (!IsValid(Pair.Value.Get())) continue;
            TArray<USceneComponent*> Components;
            Pair.Value->GetChildrenComponents(true, Components);
            Components.Add(Pair.Value.Get());
            for (USceneComponent* Child : Components)
                if (auto* Mesh = Cast<UMeshComponent>(Child); IsValid(Mesh) && Mesh->IsVisible())
                    for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
                    {
                        UMaterialInterface* Original = Mesh->OverrideMaterials.IsValidIndex(Slot)
                            ? Mesh->OverrideMaterials[Slot].Get() : nullptr;
                        OriginalMaterials.Add({ Mesh, Slot, TStrongObjectPtr<UMaterialInterface>(Original) });
                        Mesh->SetMaterial(Slot, TintMaterial.Get());
                    }
        }
    }

    TArray<FShadowPart> Parts;
    FBox LocalBounds{ForceInit};
    TArray<FShadowAlignmentSegment> Lines;

private:
    void RestoreMaterials()
    {
        for (const auto& Entry : OriginalMaterials)
            if (auto* Mesh = Entry.Mesh.Get(); Mesh && Mesh->GetMaterial(Entry.Slot) == TintMaterial.Get())
                Mesh->SetMaterial(Entry.Slot, Entry.Material.Get());
        OriginalMaterials.Reset();
    }

    struct FOriginalMaterial
    {
        TWeakObjectPtr<UMeshComponent> Mesh;
        int32 Slot;
        TStrongObjectPtr<UMaterialInterface> Material;
    };

    TWeakObjectPtr<UGameViewportClient> Viewport;
    TWeakObjectPtr<ULocalPlayer> LocalPlayer;
    TSharedPtr<SWidget> Widget;
    TSharedPtr<SShadowWorldLines> LineWidget;
    TSharedPtr<SBorder> LabelBorder;
    TSharedPtr<STextBlock> Label;
    TWeakObjectPtr<AFGBlueprintHologram> Preview;
    TWeakObjectPtr<UFGBlueprintDescriptor> Descriptor;
    int32 RootCount = 0;
    TStrongObjectPtr<UMaterialInstanceDynamic> TintMaterial;
    TArray<FOriginalMaterial> OriginalMaterials;
};

void AShadowPlanSubsystem::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    RefreshPlacementFeedback();
}

void AShadowPlanSubsystem::RefreshPlacementFeedback()
{
    auto* PC = GetWorld()->GetFirstPlayerController();
    auto* Player = PC ? PC->GetLocalPlayer() : nullptr;
    auto* Viewport = GetWorld()->GetGameViewport();
    auto* Preview = Cast<AFGBlueprintHologram>(ActivePreview());
    if (!IsValid(Player) || !IsValid(Viewport) || !Preview || Preview == PlanBuildPreview.Get()
        || PlanMenu || ClearInView || !CanUsePlanControls(*PC))
    {
        PlacementFeedback.Reset();
        return;
    }

    if (!PlacementFeedback)
        PlacementFeedback = MakeShared<FShadowPlacementFeedback>(*PC, *Player, *Viewport,
            [Owner = TWeakObjectPtr<AShadowPlanSubsystem>(this)]() -> TConstArrayView<FShadowAlignmentSegment>
            {
                if (const auto* Subsystem = Owner.Get(); Subsystem && Subsystem->PlacementFeedback)
                    return Subsystem->PlacementFeedback->Lines;
                return {};
            });
    auto& Feedback = *PlacementFeedback;
    Feedback.RefreshGeometry(*Preview);
    Feedback.Lines.Reset();

    const FTransform Pose = Preview->GetActorTransform();
    const FBox PreviewBounds = Feedback.Parts.IsEmpty() ? Preview->GetComponentsBoundingBox(true)
        : Feedback.LocalBounds.TransformBy(Pose);
    const bool bNativeValid = Preview->CanConstruct()
        && Preview->GetHologramMaterialState() == EHologramMaterialState::HMS_OK;

    TArray<FString> Conflicts, PossibleConflicts;
    if (bVisible && PreviewBounds.IsValid)
    {
        TArray<FPlacementPartBox> PreviewBoxes;
        TSet<int32> ConflictingPreviewParts;
        for (const auto& Plan : Plans)
        {
            const auto* View = Views.FindRef(Plan.Id).Get();
            if (!View || View->IsHidden() || !PreviewBounds.Intersect(View->GetLocalPlanBounds().TransformBy(Plan.Transform)))
                continue;
            if (Feedback.Parts.IsEmpty() || Plan.Parts.IsEmpty())
            {
                PossibleConflicts.AddUnique(Plan.BlueprintId);
                continue;
            }
            if (PreviewBoxes.IsEmpty())
                for (const auto& Part : Feedback.Parts)
                    PreviewBoxes.Add(FPlacementPartBox::Make(Part, Pose));

            for (const auto& Part : Plan.Parts)
            {
                const auto PlacedBox = FPlacementPartBox::Make(Part, Plan.Transform);
                // The visible surface and the placement face can have different bounds.
                TArray<FPlacementPartBox, TInlineAllocator<4>> RoofBoxes;
                for (const auto& RoofBox : GetShadowRoofBoxes(Part, Part.InPlan * Plan.Transform))
                    RoofBoxes.Add(FPlacementPartBox::Make(RoofBox.Frame, RoofBox.HalfSize));
                bool bPartConflicts = false;
                for (int32 Index = 0; Index < PreviewBoxes.Num(); ++Index)
                {
                    const auto& CurrentBox = PreviewBoxes[Index];
                    const bool bIntersects = RoofBoxes.IsEmpty() ? HaveVolumeIntersection(CurrentBox, PlacedBox)
                        : RoofBoxes.ContainsByPredicate([&CurrentBox](const auto& Box)
                            { return HaveVolumeIntersection(CurrentBox, Box); });
                    if (bIntersects)
                    {
                        ConflictingPreviewParts.Add(Index);
                        bPartConflicts = true;
                    }
                }
                if (bPartConflicts)
                {
                    Conflicts.AddUnique(Plan.BlueprintId);
                    OutlinePart(Feedback.Lines, PlacedBox, OtherPartColor);
                }
            }
        }
        for (const int32 Index : ConflictingPreviewParts)
            OutlinePart(Feedback.Lines, PreviewBoxes[Index], ConflictColor);
    }

    if (!Conflicts.IsEmpty())
        Feedback.Display(*Preview, FText::Format(LOCTEXT("Conflict", "Пересечение планов · {Plan}"),
            FFormatNamedArguments{{ TEXT("Plan"), FText::FromString(FString::Join(Conflicts, TEXT(", "))) }}),
            ConflictColor, bNativeValid);
    else if (!PossibleConflicts.IsEmpty())
        Feedback.Display(*Preview, FText::Format(LOCTEXT("PossibleConflict", "Возможное пересечение · {Plan}"),
            FFormatNamedArguments{{ TEXT("Plan"), FText::FromString(FString::Join(PossibleConflicts, TEXT(", "))) }}),
            PossibleColor, bNativeValid);
    else
        Feedback.Display(*Preview, bNativeValid && !Feedback.Parts.IsEmpty()
            ? LOCTEXT("Clear", "Свободно") : FText::GetEmpty(), ClearColor, bNativeValid);
}

#undef LOCTEXT_NAMESPACE

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FShadowPlacementOverlapTest, "ShadowArchitect.Placement.Overlap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FShadowPlacementOverlapTest::RunTest(const FString& Parameters)
{
    FShadowPart Slab;
    Slab.SourceClass = TEXT("/Game/FactoryGame/Buildable/Building/Foundation/ConcreteSet/"
        "Build_Foundation_Concrete_8x1.Build_Foundation_Concrete_8x1_C");
    Slab.BoundsSize = FVector(800, 800, 106);
    FShadowPart Foundry;
    Foundry.BoundsSize = FVector(100, 70, 160);
    Foundry.BoundsCenter.Z = 80;
    const auto At = [&Foundry](const FVector& Position)
    {
        return FPlacementPartBox::Make(Foundry, FTransform(FRotator(0, 27, 0), Position));
    };
    const auto SlabBoxes = GetShadowRoofBoxes(Slab, FTransform::Identity);
    if (TestEqual(TEXT("One solid concrete slab"), SlabBoxes.Num(), 1))
    {
        const auto Roof = FPlacementPartBox::Make(SlabBoxes[0].Frame, SlabBoxes[0].HalfSize);
        const auto Visual = FPlacementPartBox::Make(Slab, FTransform::Identity);
        TestTrue(TEXT("The visual slab box extends above the roof"), HaveVolumeIntersection(At(FVector(0, 0, 50)), Visual));
        TestFalse(TEXT("The foundry may touch the roof"), HaveVolumeIntersection(At(FVector(0, 0, 50)), Roof));
        TestTrue(TEXT("Penetrating the roof is a conflict"), HaveVolumeIntersection(At(FVector(0, 0, 49)), Roof));
    }

    FShadowPart Column;
    Column.BoundsSize = FVector(35, 35, 200);
    const auto ColumnAt = [&Column](const FVector& Position)
    {
        return FPlacementPartBox::Make(Column, FTransform(Position));
    };
    const auto Left = ColumnAt(FVector(-300, 0, 100));
    const auto Right = ColumnAt(FVector(300, 0, 100));
    const auto Between = At(FVector(0, 0, 50));
    TestTrue(TEXT("The whole row bounds enclose the gap"),
        (Left.BroadBounds + Right.BroadBounds).Intersect(Between.BroadBounds));
    TestFalse(TEXT("The gap is free"), HaveVolumeIntersection(Between, Left) || HaveVolumeIntersection(Between, Right));
    TestTrue(TEXT("A rotated foundry intersects the column"), HaveVolumeIntersection(At(FVector(260, 0, 50)), Right));

    const auto OutsideCorner = ColumnAt(FVector(65, -48, 100));
    TestTrue(TEXT("World axis bounds overlap in the rotated box's empty corner"),
        Between.BroadBounds.Intersect(OutsideCorner.BroadBounds));
    TestFalse(TEXT("The rotated boxes remain disjoint"), HaveVolumeIntersection(Between, OutsideCorner));
    return true;
}
#endif
