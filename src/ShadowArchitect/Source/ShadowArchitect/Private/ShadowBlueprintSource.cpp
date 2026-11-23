#include "ShadowBlueprintSource.h"
#include "ShadowPlan.h"
#include "FGBlueprintSubsystem.h"
#include "HAL/FileManager.h"
#include "Hash/Blake3.h"
#include "Hologram/FGBlueprintHologram.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Patching/NativeHookManager.h"

namespace
{
constexpr TCHAR ProofPrefix[] = TEXT("blake3-sbp-v1:");
constexpr TCHAR BlueprintExtension[] = TEXT(".sbp");
TMap<TWeakObjectPtr<const AFGBlueprintHologram>, FString> PreviewProofs;

FString MakeProof(const TArray<uint8>& Bytes)
{
    return ProofPrefix + LexToString(FBlake3::HashBuffer(Bytes.GetData(), Bytes.Num()));
}

bool IsSupportedProof(const FString& Proof)
{
    constexpr int32 PrefixLength = UE_ARRAY_COUNT(ProofPrefix) - 1;
    constexpr int32 HashLength = sizeof(FBlake3Hash::ByteArray) * 2;
    if (Proof.Len() != PrefixLength + HashLength || !Proof.StartsWith(ProofPrefix, ESearchCase::CaseSensitive))
        return false;
    for (int32 Index = PrefixLength; Index < Proof.Len(); ++Index)
        if (!FChar::IsHexDigit(Proof[Index])) return false;
    return true;
}

#if !WITH_EDITOR
struct FPreviewLoad
{
    AFGBlueprintSubsystem* Library;
    FString Name;
    FString Proof;
    int32 Reads = 0;
};

FPreviewLoad* ActiveLoad = nullptr;
FDelegateHandle PreviewHook;
FDelegateHandle FileHook;
#endif
}

void InstallBlueprintSourceHooks()
{
#if !WITH_EDITOR
    // Observe the bytes that build the preview. F7 may run after that file has changed.
    FileHook = SUBSCRIBE_METHOD_AFTER(AFGBlueprintSubsystem::LoadFileFromDisk,
        [](const bool& bLoaded, AFGBlueprintSubsystem* Library, const FString& Name,
            const FString& Extension, TArray<uint8>& Bytes)
        {
            if (!IsInGameThread() || !ActiveLoad || Library != ActiveLoad->Library
                || !Extension.Equals(BlueprintExtension, ESearchCase::IgnoreCase)
                || !AFGBlueprintSubsystem::AreNamesConsideredEqual(Name, ActiveLoad->Name)) return;
            ++ActiveLoad->Reads;
            if (bLoaded && !Bytes.IsEmpty()) ActiveLoad->Proof = MakeProof(Bytes);
        });
    PreviewHook = SUBSCRIBE_METHOD(AFGBlueprintHologram::LoadBlueprintToOtherWorld,
        [](auto& Scope, AFGBlueprintHologram* Preview)
        {
            if (!IsInGameThread()) return;
            for (auto It = PreviewProofs.CreateIterator(); It; ++It)
                if (!It.Key().IsValid()) It.RemoveCurrent();
            PreviewProofs.Remove(Preview);
            auto* Descriptor = Preview->mBlueprintDescriptor.Get();
            auto* Library = AFGBlueprintSubsystem::Get(Preview->GetWorld());
            if (!IsValid(Descriptor) || !IsValid(Library) || !Preview->mBuildableToNewRoot.IsEmpty()) return;

            FPreviewLoad Load(Library, Descriptor->GetBlueprintNameAsString());
            TGuardValue<FPreviewLoad*> Loading(ActiveLoad, &Load);
            Scope(Preview);
            if (Preview->mBlueprintDescriptor.Get() == Descriptor && !Preview->mBuildableToNewRoot.IsEmpty()
                && Load.Reads == 1 && !Load.Proof.IsEmpty())
                PreviewProofs.Add(Preview, MoveTemp(Load.Proof));
        });
#endif
}

void RemoveBlueprintSourceHooks()
{
#if !WITH_EDITOR
    UNSUBSCRIBE_METHOD(AFGBlueprintHologram::LoadBlueprintToOtherWorld, PreviewHook);
    UNSUBSCRIBE_METHOD(AFGBlueprintSubsystem::LoadFileFromDisk, FileHook);
    PreviewProofs.Reset();
#endif
}

FString GetBlueprintSourceProof(const AFGBlueprintHologram& Preview)
{
    check(IsInGameThread());
    return PreviewProofs.FindRef(&Preview);
}

bool HasBlueprintSourceProof(const AFGBlueprintHologram& Preview, const FShadowPlacement& Plan)
{
    return !Plan.BlueprintSourceProof.IsEmpty() && GetBlueprintSourceProof(Preview) == Plan.BlueprintSourceProof;
}

UFGBlueprintDescriptor* ResolveBlueprintSource(AFGBlueprintSubsystem& Library, const FShadowPlacement& Plan,
    FString& Error)
{
    check(IsInGameThread());
    Error.Reset();
    if (!IsSupportedProof(Plan.BlueprintSourceProof))
    {
        Error = TEXT("This plan has no verifiable Blueprint source. Place a new plan from the Blueprint.");
        return nullptr;
    }

    const FString Directory = Library.GetSessionBlueprintPath();
    if (Directory.IsEmpty())
    {
        Error = TEXT("The Blueprint library is unavailable.");
        return nullptr;
    }
    TArray<FString> Files;
    IFileManager::Get().FindFiles(Files, *(Directory / TEXT("*.sbp")), true, false);
    FString Match;
    bool bOriginalNameExists = false;
    TArray<uint8> Bytes;
    for (const FString& File : Files)
    {
        const FString Name = FPaths::GetBaseFilename(File);
        bOriginalNameExists |= AFGBlueprintSubsystem::AreNamesConsideredEqual(Name, Plan.BlueprintId);
        if (!FFileHelper::LoadFileToArray(Bytes, *(Directory / File)))
        {
            Error = FString::Printf(TEXT("Could not read Blueprint %s to verify the source."), *Name);
            return nullptr;
        }
        if (MakeProof(Bytes) != Plan.BlueprintSourceProof) continue;
        if (!Match.IsEmpty())
        {
            Error = TEXT("The Blueprint source is ambiguous. Multiple library files have the same contents.");
            return nullptr;
        }
        Match = Name;
    }
    if (Match.IsEmpty())
    {
        Error = bOriginalNameExists ? TEXT("The Blueprint source has changed.") : TEXT("The Blueprint source is missing.");
        return nullptr;
    }

    Library.RefreshBlueprintsAndDescriptors();
    UFGBlueprintDescriptor* Descriptor = nullptr;
    TArray<UFGBlueprintDescriptor*> Descriptors;
    Library.GetBlueprintDescriptors_Internal(Descriptors);
    for (auto* Candidate : Descriptors)
    {
        if (!IsValid(Candidate)
            || !AFGBlueprintSubsystem::AreNamesConsideredEqual(Candidate->GetBlueprintNameAsString(), Match)) continue;
        if (Descriptor && Descriptor != Candidate)
        {
            Error = TEXT("The Blueprint source is ambiguous. Several game descriptors have the same name.");
            return nullptr;
        }
        Descriptor = Candidate;
    }
    const FBlueprintHeader* Header = Library.GetHeaderByName(Match);
    if (!IsValid(Descriptor) || !Header || !Library.LoadFileFromDisk(Match, BlueprintExtension, Bytes)
        || MakeProof(Bytes) != Plan.BlueprintSourceProof)
    {
        Error = TEXT("The verified Blueprint source is unavailable in the game library. Try again.");
        return nullptr;
    }
    // The refresh adds new descriptors but retains existing ones, whose cost may be stale.
    Descriptor->InitializeHeaderInfo(*Header);
    return Descriptor;
}
