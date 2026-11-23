#pragma once

#include "CoreMinimal.h"

class AActor;
class AFGBuildGun;
class AFGCharacterPlayer;

// Applies only to the dedicated, initially non-colliding Ghost fixture.
int32 ConfigureAlignmentSurface(AActor& Projection, bool bEnabled);
void LogAlignmentSurface(const TCHAR* Label, AFGBuildGun& Gun, const AActor& Projection);
bool CheckAlignmentPassage(AFGCharacterPlayer& Character, const FVector& Start, const FVector& End);
bool CheckAlignmentDismantle(AFGBuildGun& Gun, const AActor& Expected);
