#pragma once

#include "CoreMinimal.h"

class AFGBlueprintHologram;
class AFGBlueprintSubsystem;
class UFGBlueprintDescriptor;
struct FShadowPlacement;

void InstallBlueprintSourceHooks();
void RemoveBlueprintSourceHooks();
FString GetBlueprintSourceProof(const AFGBlueprintHologram& Preview);
bool HasBlueprintSourceProof(const AFGBlueprintHologram& Preview, const FShadowPlacement& Plan);
UFGBlueprintDescriptor* ResolveBlueprintSource(AFGBlueprintSubsystem& Library, const FShadowPlacement& Plan,
    FString& Error);
