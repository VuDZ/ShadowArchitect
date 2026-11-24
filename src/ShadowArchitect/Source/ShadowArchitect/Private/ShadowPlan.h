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
class FShadowAlignmentGuide;
class FShadowClearInView;
class FShadowPlanMenu;
class FShadowPlacementFeedback;
class FShadowPlanningControls;
class IFileHandle;
class UFGBuildGunStateBuild;
class UFGBlueprintDescriptor;
class UMaterialInterface;
class UStaticMesh;
class UTextRenderComponent;

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

// Alignment geometry belongs to the current viewport, outside the save data.
USTRUCT()
struct FShadowAlignmentSegment
{
    GENERATED_BODY()
    UPROPERTY() FVector Start = FVector::ZeroVector;
    UPROPERTY() FVector End = FVector::ZeroVector;
    FLinearColor Color = FLinearColor::White;
    float Thickness = 2.0f;
};

bool CaptureBlueprint(AFGBlueprintHologram& Preview, FShadowPlacement& Out, FString& Error);
bool CaptureBlueprintGeometry(AFGBlueprintHologram& Preview, TArray<FShadowPart>& Parts);
FBox CalculateFootprint(const TArray<FShadowPart>& Parts);
int32 FindShadowPartOnRay(const FShadowPlacement& Plan, const FVector& Start, const FVector& Delta, double& Limit);

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
    const FBox& GetLocalPlanBounds() const { return LocalPlanBounds; }
    void SetSelectionMarker(const FBox& Footprint);
protected:
    virtual void Tick(float DeltaSeconds) override;
private:
    UPROPERTY(Transient) TObjectPtr<UTextRenderComponent> SelectionMarker;
    FBox LocalPlanBounds{ForceInit};
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
    virtual void Tick(float DeltaSeconds) override;
private:
    friend class FShadowArchitectModule;
    friend class FShadowClearInView;
    friend class FShadowPlanMenu;

    static constexpr double PlanPickRange = 10000.0;
    UPROPERTY(SaveGame) TArray<FShadowPlacement> Plans;
    UPROPERTY(SaveGame) bool bVisible = true;
    UPROPERTY(Transient) TObjectPtr<UStaticMesh> ProxyCube;
    UPROPERTY(Transient) TObjectPtr<UMaterialInterface> ShadowMaterial;
    UPROPERTY(Config) FInputChord BuildPreviewChord;
    EShadowView ViewMode = EShadowView::Ghost;
    bool bUsePlanSupport = false;
    TWeakObjectPtr<AFGBlueprintHologram> SupportGeometryPreview;
    TWeakObjectPtr<UFGBlueprintDescriptor> SupportGeometrySource;
    int32 SupportGeometryPartCount = 0;
    FBox SupportGeometryBounds{ForceInit};
    TMap<FGuid, TWeakObjectPtr<AShadowProjection>> Views;
    FGuid Selected;
    TSharedPtr<FShadowClearInView> ClearInView;
    TSharedPtr<FShadowPlanMenu> PlanMenu;
    TSharedPtr<FShadowAlignmentGuide> AlignmentGuide;
    TSharedPtr<FShadowPlacementFeedback> PlacementFeedback;
    UPROPERTY(Transient) TArray<FShadowAlignmentSegment> AlignmentLines;
    FGuid AlignmentPlan;
    FTimerHandle AlignmentRefresh;
    TWeakObjectPtr<AFGBlueprintHologram> PlanBuildPreview;
    FGuid PlanBuildId;
    TWeakObjectPtr<UFGBuildGunStateBuild> ClosingBuildState;
    TWeakObjectPtr<UFGBlueprintDescriptor> ClosingBuildSource;
    bool bConfirmingPlanBuild = false;
    FTimerHandle InputRetry;
    TSharedPtr<FShadowPlanningControls> PlanningControls;
    void Restore();
    bool ShowPlan(const FShadowPlacement& Plan);
    bool BuildView(AShadowProjection& View, const FShadowPlacement& Plan, EShadowView Mode) const;
    void ClearViews();
    void BindControls();
    void StartPlanningControls();
    bool RefreshPlanningControls(float DeltaTime);
    static bool CanUsePlanControls(const APlayerController& Player);
    bool ImportLegacyKeybindings(APlayerController& Player);
    FString KeybindingHint(APlayerController& Player, FName Mapping) const;
    void Notify(const FString& Text) const;
    AFGHologram* CurrentPreview() const;
    AFGHologram* ActivePreview() const;
    bool AddCapturedPlan(FShadowPlacement Plan, FString& Result);
    void Place();
    void TogglePlacementSupport();
    static void InstallPlacementSupportHooks();
    static void RemovePlacementSupportHooks();
    void TracePlacementSupport(AFGBlueprintHologram& Preview, const APlayerController& Player, double Range, FHitResult& Hit);
    void PlaceBlueprintOnRoof(const FGuid& Id);
    void SelectNext();
    void SelectPlanUnderCrosshair();
    void SelectPlan(const FShadowPlacement& Plan);
    void SetSelectedPlan(const FShadowPlacement* Plan);
    void MoveSelected();
    bool TryMovePlan(const FGuid& Id);
    void DeleteSelected();
    void DeletePlan(const FGuid& Id);
    void ToggleVisibility();
    void CycleView();
    double TracePlanAim(const APlayerController& Player, double Range, FVector& Start, FVector& Delta) const;
    FGuid FindPlanUnderCrosshair() const;
    void ToggleAlignmentGuide(const FGuid& Id);
    void RefreshAlignmentGuide();
    void ClearAlignmentGuide();
    void RefreshPlacementFeedback();
    void OpenPlanMenu();
    void ClosePlanMenu();
    bool IsPlanMenuOpen() const;
    void RequestClearInView();
    void CloseClearInView();
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
