#pragma once

#include "CoreMinimal.h"
#include "Command/ChatCommandInstance.h"
#include "FGSaveInterface.h"
#include "Framework/Commands/InputChord.h"
#include "InputCoreTypes.h"
#include "Module/GameWorldModule.h"
#include "Subsystem/ModSubsystem.h"
#include "ShadowPlan.generated.h"

class AFGBlueprintHologram;
class AFGHologram;
class FShadowPlanMenu;
class IFileHandle;
class UFGBuildGunStateBuild;
class UFGBlueprintDescriptor;
class UMaterialInterface;
class UStaticMesh;

USTRUCT()
struct FShadowMesh
{
    GENERATED_BODY()
    UPROPERTY(SaveGame) TSoftObjectPtr<UStaticMesh> Asset;
    UPROPERTY(SaveGame) FTransform InPart;
};

USTRUCT()
struct FShadowPart
{
    GENERATED_BODY()
    UPROPERTY(SaveGame) FString SourceClass;
    UPROPERTY(SaveGame) FTransform InPlan;
    UPROPERTY(SaveGame) FVector BoundsCenter = FVector::ZeroVector;
    UPROPERTY(SaveGame) FVector BoundsSize = FVector::ZeroVector;
    UPROPERTY(SaveGame) TArray<FShadowMesh> Meshes;
};

USTRUCT()
struct FShadowPlacement
{
    GENERATED_BODY()
    UPROPERTY(SaveGame) int32 SchemaVersion = 1;
    UPROPERTY(SaveGame) FGuid Id;
    UPROPERTY(SaveGame) FString BlueprintId;
    UPROPERTY(SaveGame) FTransform Transform;
    UPROPERTY(SaveGame) TArray<FShadowPart> Parts;
    UPROPERTY(SaveGame) FString BlueprintSourceProof;
};

// Only the data above crosses the save boundary.
bool CaptureBlueprint(AFGBlueprintHologram& Preview, FShadowPlacement& Out, FString& Error);
FBox CalculateFootprint(const TArray<FShadowPart>& Parts);

enum class EShadowView : uint8
{
    Ghost,
    Wireframe,
    Footprint
};

UCLASS(Transient, NotPlaceable)
class AShadowProjection : public AActor
{
    GENERATED_BODY()
public:
    AShadowProjection();
    bool Build(const FShadowPlacement& Plan, UStaticMesh& Cube, UMaterialInterface& Material);
    bool BuildFootprint(const FShadowPlacement& Plan, UStaticMesh& Cube, UMaterialInterface& Material);
    bool BuildWireframe(const FShadowPlacement& Plan, UStaticMesh& Cube);
private:
    bool BuildParts(const FShadowPlacement& Plan, UStaticMesh& Cube,
        UMaterialInterface& MeshMaterial, UMaterialInterface& ProxyMaterial);
    void AddMesh(UStaticMesh& Asset, UMaterialInterface& Material, const FTransform& Relative);
};

UCLASS()
class UShadowWorldModule : public UGameWorldModule
{
    GENERATED_BODY()
public:
    UShadowWorldModule();
};

UCLASS(Config=Game)
class AShadowPlanSubsystem : public AModSubsystem, public IFGSaveInterface
{
    GENERATED_BODY()
public:
    AShadowPlanSubsystem();
    bool TryPlace(FString& Result);
    virtual bool ShouldSave_Implementation() const override { return true; }
    virtual bool NeedTransform_Implementation() override { return false; }
    virtual void PostLoadGame_Implementation(int32 SaveVersion, int32 GameVersion) override;
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    friend class FShadowArchitectModule;
    friend class FShadowPlanMenu;

    UPROPERTY(SaveGame) TArray<FShadowPlacement> Plans;
    UPROPERTY(SaveGame) bool bVisible = true;
    UPROPERTY(Transient) TObjectPtr<UStaticMesh> ProxyCube;
    UPROPERTY(Transient) TObjectPtr<UMaterialInterface> ShadowMaterial;
    UPROPERTY(Config) FInputChord BuildPreviewChord;
    EShadowView ViewMode = EShadowView::Ghost;
    TMap<FGuid, TWeakObjectPtr<AShadowProjection>> Views;
    FGuid Selected;
    TSharedPtr<FShadowPlanMenu> PlanMenu;
    TWeakObjectPtr<AFGBlueprintHologram> PlanBuildPreview;
    FGuid PlanBuildId;
    TWeakObjectPtr<UFGBuildGunStateBuild> ClosingBuildState;
    TWeakObjectPtr<UFGBlueprintDescriptor> ClosingBuildSource;
    bool bConfirmingPlanBuild = false;
    FTimerHandle InputRetry;
    void Restore();
    bool ShowPlan(const FShadowPlacement& Plan);
    bool BuildView(AShadowProjection& View, const FShadowPlacement& Plan, EShadowView Mode) const;
    void ClearViews();
    void BindControls();
    static bool CanUsePlanControls(const APlayerController& Player);
    bool ImportLegacyKeybindings(APlayerController& Player);
    FString KeybindingHint(APlayerController& Player, FName Mapping) const;
    void Notify(const FString& Text) const;
    AFGHologram* ActivePreview() const;
    void Place();
    void SelectNext();
    void MoveSelected();
    bool TryMovePlan(const FGuid& Id);
    void DeleteSelected();
    void DeletePlan(const FGuid& Id);
    void ToggleVisibility();
    void CycleView();
    FGuid FindPlanUnderCrosshair() const;
    void OpenPlanMenu();
    void ClosePlanMenu();
    bool ConfirmPendingPlanMove();
    void OpenSelectedBuildPreview();
    void OpenPlanBuildPreview(const FGuid& Id);
    static void InstallPlanConstructionHooks();
    static void RemovePlanConstructionHooks();
    TUniquePtr<IFileHandle> VerifyPlanConstruction(UFGBuildGunStateBuild& State,
        AFGBlueprintHologram& Preview, FString& Error) const;
    void ClosePlanBuildAfterConfirmation(UFGBuildGunStateBuild& BuildState, UFGBlueprintDescriptor* BuildSource);
};

UCLASS()
class AShadowPlaceCommand : public AChatCommandInstance
{
    GENERATED_BODY()
public:
    AShadowPlaceCommand();
    virtual EExecutionStatus ExecuteCommand_Implementation(UCommandSender* Sender,
        const TArray<FString>& Arguments, const FString& Label) override;
};
