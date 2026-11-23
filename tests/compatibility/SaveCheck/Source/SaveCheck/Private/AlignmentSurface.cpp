#include "AlignmentSurface.h"

#include "Components/StaticMeshComponent.h"
#include "Equipment/FGBuildGun.h"
#include "Equipment/FGBuildGunBuild.h"
#include "Equipment/FGBuildGunDismantle.h"
#include "FGCharacterPlayer.h"
#include "FGConstructDisqualifier.h"
#include "Hologram/FGHologram.h"

DEFINE_LOG_CATEGORY_STATIC(LogSaveCheckSurface, Log, All);

int32 ConfigureAlignmentSurface(AActor& Projection, bool bEnabled)
{
    TInlineComponentArray<UStaticMeshComponent*> Meshes(&Projection);
    if (!bEnabled) Projection.SetActorEnableCollision(false);
    for (UStaticMeshComponent* Mesh : Meshes)
    {
        Mesh->SetGenerateOverlapEvents(false);
        Mesh->SetCollisionResponseToAllChannels(ECR_Ignore);
        Mesh->SetCollisionResponseToChannel(ECC_GameTraceChannel5, bEnabled ? ECR_Block : ECR_Ignore);
        Mesh->SetCollisionEnabled(bEnabled ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
    }
    if (bEnabled) Projection.SetActorEnableCollision(true);
    UE_LOG(LogSaveCheckSurface, Display, TEXT("surface enabled=%d actor=%s meshes=%d"),
        bEnabled, *Projection.GetName(), Meshes.Num());
    return Meshes.Num();
}

void LogAlignmentSurface(const TCHAR* Label, AFGBuildGun& Gun, const AActor& Projection)
{
    const FHitResult& Hit = Gun.GetHitResult();
    const auto* State = Cast<UFGBuildGunStateBuild>(Gun.GetCurrentState());
    const AFGHologram* Preview = State ? State->GetHologram() : nullptr;
    UE_LOG(LogSaveCheckSurface, Display,
        TEXT("case=%s hit=%d target=%s projection_hit=%d impact=%s normal=%s accepted=%d visible=%d pose=%s can_construct=%d"),
        Label, Hit.bBlockingHit, *GetNameSafe(Hit.GetActor()), Hit.GetActor() == &Projection,
        *Hit.ImpactPoint.ToString(), *Hit.ImpactNormal.ToString(), Preview && Preview->IsValidHitResult(Hit),
        Preview && !Preview->IsHidden(), Preview ? *Preview->GetActorTransform().ToString() : TEXT("none"),
        Preview && Preview->CanConstruct());
    if (!Preview) return;
    TArray<TSubclassOf<UFGConstructDisqualifier>> Reasons;
    Preview->GetConstructDisqualifiers(Reasons);
    for (const auto& Reason : Reasons)
        UE_LOG(LogSaveCheckSurface, Display, TEXT("case=%s disqualifier=%s"), Label, *GetNameSafe(Reason.Get()));
}

bool CheckAlignmentPassage(AFGCharacterPlayer& Character, const FVector& Start, const FVector& End)
{
    const FRotator Rotation = Character.GetActorRotation();
    const FVector Original = Character.GetActorLocation();
    const bool bAtStart = Character.TeleportTo(Start, Rotation, false, true);
    FHitResult Hit;
    if (bAtStart) Character.SetActorLocation(End, true, &Hit);
    const FVector Reached = Character.GetActorLocation();
    const bool bPassed = bAtStart && !Hit.bBlockingHit && Reached.Equals(End, 1.0);
    Character.TeleportTo(Original, Rotation, false, true);
    UE_LOG(LogSaveCheckSurface, Display, TEXT("walk=%d start=%s end=%s reached=%s blocking=%s"),
        bPassed, *Start.ToString(), *End.ToString(), *Reached.ToString(), *GetNameSafe(Hit.GetActor()));
    return bPassed;
}

bool CheckAlignmentDismantle(AFGBuildGun& Gun, const AActor& Expected)
{
    const auto* State = Cast<UFGBuildGunStateDismantle>(Gun.GetCurrentState());
    const AActor* Selected = State ? State->GetSelectedActor() : nullptr;
    const bool bMatches = Selected == &Expected;
    UE_LOG(LogSaveCheckSurface, Display, TEXT("dismantle=%d selected=%s expected=%s"),
        bMatches, *GetNameSafe(Selected), *Expected.GetName());
    return bMatches;
}
