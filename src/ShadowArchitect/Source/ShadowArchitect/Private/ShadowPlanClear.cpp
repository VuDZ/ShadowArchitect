#include "ShadowPlan.h"

#include "Components/PrimitiveComponent.h"
#include "ConvexVolume.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "SceneView.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ShadowPlanClear"

class FShadowClearInView : public IInputProcessor, public TSharedFromThis<FShadowClearInView>
{
public:
    FShadowClearInView(AShadowPlanSubsystem& InPlans, APlayerController& InPlayer,
        UGameViewportClient& InViewport, TArray<FGuid> InTargets)
        : Targets(MoveTemp(InTargets)), Plans(&InPlans), Player(&InPlayer), Viewport(&InViewport)
    {
    }

    void Show(const FString& Key);
    void Close();
    TArray<FGuid> Targets;

private:
    void Tick(float, FSlateApplication&, TSharedRef<ICursor>) override {}
    bool HandleKeyDownEvent(FSlateApplication&, const FKeyEvent& Event) override;
    bool HandleKeyUpEvent(FSlateApplication&, const FKeyEvent& Event) override;
    const TCHAR* GetDebugName() const override { return TEXT("Shadow Architect clear in view"); }

    TWeakObjectPtr<AShadowPlanSubsystem> Plans;
    TWeakObjectPtr<APlayerController> Player;
    TWeakObjectPtr<UGameViewportClient> Viewport;
    TSharedPtr<SWidget> Widget;
    bool bCapturedEscape = false;
};

void FShadowClearInView::Show(const FString& Key)
{
    const FText Title = FText::Format(LOCTEXT("Count", "Plans in view: {0}"), Targets.Num());
    const FText Hint = Key.IsEmpty()
        ? LOCTEXT("GenericHint", "Repeat Clear plans in view to confirm. Esc to cancel.")
        : FText::Format(LOCTEXT("KeyHint", "{0} to confirm. Esc to cancel."), FText::FromString(Key));
    Widget = SNew(SOverlay)
        .Visibility(EVisibility::HitTestInvisible)
        + SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Top).Padding(FMargin(10, 210, 10, 0))
        [
            SNew(SBorder)
            .Padding(FMargin(12, 8))
            .BorderBackgroundColor(FLinearColor(0.025f, 0.035f, 0.045f, 0.94f))
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
                [
                    SNew(STextBlock).Text(Title).Font(FCoreStyle::GetDefaultFontStyle("Bold", 15))
                ]
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(STextBlock).Text(Hint)
                ]
            ]
        ];
    Viewport->AddViewportWidgetForPlayer(Player->GetLocalPlayer(), Widget.ToSharedRef(), 60);
}

void FShadowClearInView::Close()
{
    if (Widget && Viewport.IsValid())
        if (const auto* PC = Player.Get(); PC && PC->GetLocalPlayer())
            Viewport->RemoveViewportWidgetForPlayer(PC->GetLocalPlayer(), Widget.ToSharedRef());
    Widget.Reset();
    if (FSlateApplication::IsInitialized())
        FSlateApplication::Get().UnregisterInputPreProcessor(AsShared());
}

bool FShadowClearInView::HandleKeyDownEvent(FSlateApplication&, const FKeyEvent& Event)
{
    if (Event.GetKey() != EKeys::Escape) return false;
    if (bCapturedEscape) return true;
    const auto* PC = Player.Get();
    bCapturedEscape = !Event.IsRepeat() && PC && AShadowPlanSubsystem::CanUsePlanControls(*PC);
    return bCapturedEscape;
}

bool FShadowClearInView::HandleKeyUpEvent(FSlateApplication&, const FKeyEvent& Event)
{
    if (Event.GetKey() != EKeys::Escape || !bCapturedEscape) return false;
    const auto KeepAlive = AsShared();
    if (auto* Owner = Plans.Get())
    {
        Owner->CloseClearInView();
        Owner->Notify(TEXT("Cancelled clearing plans in view."));
    }
    return true;
}

void AShadowPlanSubsystem::RequestClearInView()
{
    auto* PC = GetWorld()->GetFirstPlayerController();
    auto* Player = PC ? PC->GetLocalPlayer() : nullptr;
    auto* Viewport = GetWorld()->GetGameViewport();
    if (!Player || !Viewport || !CanUsePlanControls(*PC) || !FSlateApplication::IsInitialized()) return;

    if (ClearInView)
    {
        const TArray<FGuid> Targets = MoveTemp(ClearInView->Targets);
        CloseClearInView();
        int32 Removed = 0;
        for (const FGuid& Id : Targets)
        {
            if (!Plans.ContainsByPredicate([&Id](const FShadowPlacement& Plan) { return Plan.Id == Id; })) continue;
            DeletePlan(Id);
            ++Removed;
        }
        Notify(FString::Printf(TEXT("Cleared %d plan(s) from the requested view."), Removed));
        return;
    }

    TArray<FGuid> Targets;
    if (bVisible)
    {
        FSceneViewProjectionData Projection;
        if (!Viewport->Viewport || !Player->GetProjectionData(Viewport->Viewport, Projection)) return;

        FConvexVolume Frustum;
        GetViewFrustumBounds(Frustum, Projection.ComputeViewProjectionMatrix(), true);
        if (Frustum.Planes.IsEmpty()) return;
        for (const auto& Entry : Views)
        {
            const auto* View = Entry.Value.Get();
            if (!View || View->IsHidden()) continue;

            // Instance components contain the projection geometry in every display mode.
            FBox Bounds(ForceInit);
            for (const UActorComponent* Component : View->GetInstanceComponents())
            {
                const auto* Primitive = Cast<UPrimitiveComponent>(Component);
                if (IsValid(Primitive) && Primitive->IsRegistered() && Primitive->IsVisible())
                    Bounds += Primitive->Bounds.GetBox();
            }
            if (Bounds.IsValid && Frustum.IntersectBox(Bounds.GetCenter(), Bounds.GetExtent()))
                Targets.Add(Entry.Key);
        }
    }

    Notify(FString::Printf(TEXT("Clear in view: %d target(s)."), Targets.Num()));
    if (Targets.IsEmpty()) return;
    auto Confirmation = MakeShared<FShadowClearInView>(*this, *PC, *Viewport, MoveTemp(Targets));
    if (!FSlateApplication::Get().RegisterInputPreProcessor(Confirmation, 0)) return;
    ClosePlanMenu();
    ClearInView = MoveTemp(Confirmation);
    ClearInView->Show(KeybindingHint(*PC, TEXT("ShadowArchitect.ClearInView")));
}

void AShadowPlanSubsystem::CloseClearInView()
{
    if (auto Confirmation = MoveTemp(ClearInView)) Confirmation->Close();
}

#undef LOCTEXT_NAMESPACE
