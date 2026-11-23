#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "Templates/SubclassOf.h"

class AActor;
class AFGCharacterPlayer;
class APlayerController;
class UFGRecipe;
class UWorld;

struct FAlignmentGuideFixture
{
    TSubclassOf<UFGRecipe> Recipe;
    FTransform NativePose;
    FTransform GhostPose;
    AActor* Native;
    AActor* Ghost;
    FBox MeshBounds;
};

struct FAlignmentGuideCheck
{
    using FSpawnFoundation = TFunctionRef<AActor*(const FTransform&)>;
    using FReadFoundations = TFunctionRef<TArray<FTransform>(const FVector&)>;

    bool Tick(UWorld& World, APlayerController& Player, AFGCharacterPlayer& Character,
        TSubclassOf<UFGRecipe> Machine, const FString& Frames, TArrayView<const FAlignmentGuideFixture> Fixtures,
        FSpawnFoundation SpawnFoundation, FReadFoundations ReadFoundations);

private:
    bool TickManualPlacement(AActor& Subsystem, APlayerController& Player, AFGCharacterPlayer& Character,
        const FTransform& Base, const FString& Frames, FSpawnFoundation SpawnFoundation, FReadFoundations ReadFoundations);

    enum class EStep
    {
        NativeRoof, Roof, RaisedRoof, SlopeLow, SlopeHigh, NativeEast, East, NativeNorth, North,
        Lock, Nudge, Off, Retry, ManualEast, ManualNorth
    };
    EStep Step = EStep::NativeRoof;
    int32 Age = 0;
    double NativeRoofHeight = 0;
    double NativeRoofYaw = 0;
    FVector NativeFoundationPose[2] = { FVector::ZeroVector, FVector::ZeroVector };
    double NativeFoundationYaw[2] = {};
    FTransform LockedPose = FTransform::Identity;
    FTransform NudgedPose = FTransform::Identity;
    FVector NudgeOffset = FVector::ZeroVector;
    FTransform StockGhostPose = FTransform::Identity;
    FVector StockGhostImpact = FVector::ZeroVector;
    bool bStockGhostHit = false;
    bool bStockGhostCanConstruct = false;
    bool bPassed = true;
    TWeakObjectPtr<AActor> ManualAnchor;
    TArray<FTransform> ManualBefore;
};
