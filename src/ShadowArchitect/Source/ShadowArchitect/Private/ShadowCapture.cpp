#include "ShadowPlan.h"
#include "Buildables/FGBuildable.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "FGBlueprintSubsystem.h"
#include "Hologram/FGBlueprintHologram.h"

namespace
{
bool Measurable(const FBox& Box)
{
    return Box.IsValid && !Box.Min.ContainsNaN() && !Box.Max.ContainsNaN()
        && Box.GetSize().GetMin() > UE_SMALL_NUMBER;
}

bool CapturePart(AFGBuildable& Source, USceneComponent& Root, const FTransform& Anchor, FShadowPart& Part)
{
    const FTransform Frame = Root.GetComponentTransform();
    Part.SourceClass = Source.GetClass()->GetPathName();
    Part.InPlan = Frame.GetRelativeTransform(Anchor);
    TArray<USceneComponent*> Components;
    Root.GetChildrenComponents(true, Components);
    Components.Add(&Root);
    FBox Bounds(ForceInit);
    bool bNeedsProxy = false;
    for (USceneComponent* Component : Components)
    {
        UMeshComponent* Visual = Cast<UMeshComponent>(Component);
        if (!IsValid(Visual) || !Visual->IsRegistered() || !Visual->IsVisible()) continue;
        const FTransform InPart = Visual->GetComponentTransform().GetRelativeTransform(Frame);
        const FBox VisualBounds = Component->CalcBounds(InPart).GetBox();
        if (!Measurable(VisualBounds)) continue;
        Bounds += VisualBounds;
        UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Visual);
        if (!Static || Static->IsA<USplineMeshComponent>() || !IsValid(Static->GetStaticMesh())
            || !Static->GetStaticMesh()->IsAsset())
        {
            bNeedsProxy = true;
            continue;
        }
        if (UInstancedStaticMeshComponent* Instances = Cast<UInstancedStaticMeshComponent>(Static))
        {
            for (int32 Index = 0; Index < Instances->GetInstanceCount(); ++Index)
            {
                FTransform World;
                if (!Instances->GetInstanceTransform(Index, World, true)) return false;
                FShadowMesh& Mesh = Part.Meshes.AddDefaulted_GetRef();
                Mesh.Asset = Static->GetStaticMesh();
                Mesh.InPart = World.GetRelativeTransform(Frame);
            }
        }
        else
        {
            FShadowMesh& Mesh = Part.Meshes.AddDefaulted_GetRef();
            Mesh.Asset = Static->GetStaticMesh();
            Mesh.InPart = InPart;
        }
    }
    if (!Measurable(Bounds)) Bounds = Source.CalculateComponentsBoundingBoxInLocalSpace(true, false);
    if (!Measurable(Bounds) || !Part.InPlan.IsValid()) return false;
    if (bNeedsProxy) Part.Meshes.Reset();
    Part.BoundsCenter = Bounds.GetCenter();
    Part.BoundsSize = Bounds.GetSize();
    return true;
}
}

bool CaptureBlueprint(AFGBlueprintHologram& Preview, FShadowPlacement& Out, FString& Error)
{
    check(IsInGameThread());
    if (!IsValid(Preview.mBlueprintDescriptor) || Preview.mBuildableToNewRoot.IsEmpty())
    {
        Error = TEXT("Blueprint preview has no object roots yet. Try again after it appears.");
        return false;
    }
    Out.BlueprintId = Preview.mBlueprintDescriptor->GetBlueprintNameAsString();
    Out.Transform = Preview.GetActorTransform();
    // The temporary world can already be released. While it is still available,
    // use its source list to detect an incomplete preview (including special mod buildings).
    auto* Library = AFGBlueprintSubsystem::Get(Preview.GetWorld());
    if (IsValid(Library) && Library->GetActiveBlueprintDescriptor() == Preview.mBlueprintDescriptor.Get())
    {
        for (AFGBuildable* Building : Library->GetBlueprintWorldBuildables())
        {
            if (IsValid(Building) && !Preview.mBuildableToNewRoot.Contains(Building))
            {
                Error = FString::Printf(TEXT("Preview has no visual root for %s."), *Building->GetName());
                return false;
            }
        }
    }
    for (const auto& Pair : Preview.mBuildableToNewRoot)
    {
        if (!IsValid(Pair.Key.Get()) || !IsValid(Pair.Value.Get()) || !Pair.Value->IsRegistered()
            || !CapturePart(*Pair.Key.Get(), *Pair.Value.Get(), Out.Transform, Out.Parts.AddDefaulted_GetRef()))
        {
            Error = FString::Printf(TEXT("Incomplete preview, no plan placed. Object: %s"), *GetNameSafe(Pair.Key.Get()));
            return false;
        }
    }
    Out.Id = FGuid::NewGuid();
    return true;
}
