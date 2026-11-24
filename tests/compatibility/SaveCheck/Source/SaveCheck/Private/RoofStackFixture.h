#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"
#include "InputCoreTypes.h"

class AFGBuildGun;
class AFGBlueprintHologram;
class APlayerController;

struct FRoofStackPart
{
    FString SourceClass;
    FTransform Relative;
    FBox Bounds;
};

struct FRoofStackPlan
{
    FGuid Id;
    FTransform Anchor;
    FString Name;
    FString SourceProof;
    TArray<FRoofStackPart> Parts;

    TOptional<FVector> RoofPoint() const;
};

bool ReadRoofStackPlans(UWorld& World, TArray<FRoofStackPlan>& Out);
int32 RoofStackBuildingCount(UWorld& World);
void RoofStackKey(APlayerController& Player, FKey Key, EInputEvent Event);
AFGBlueprintHologram* RoofStackPreview(AFGBuildGun& Gun);
