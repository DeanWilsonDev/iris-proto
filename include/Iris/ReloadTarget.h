#pragma once

#include "Iris/Component.h"
#include "Iris/SlotRuntime.h"

#include "Umbra/IWidget.h"

#include <memory>

namespace iris {

// The whole-app "current tree + the IR it came from" registry
// `docs/iris_hot_reload_alignment_decision.md` §2 and `docs/
// iris_interpreted_host_hot_reload_gap.md` §2 both named as missing: `Reconciler.h`'s
// `ReconcileWidget` needs an *owning* `unique_ptr<Umbra::IWidget>&` and the exact prior
// `Component` tree it was built from, and neither existed anywhere at whole-application
// scope before this (only `SlotState::PreviousSingle_`/`PreviousList_`, scoped to one
// `<Slot>`). See `docs/iris_hot_reload_reconciliation_decision.md` §2 for the full design.
//
// Opt-in: an ordinary app that never hot-reloads has no reason to construct one of
// these, and `IrisRuntime`'s own reload-target slot (`RegisterReloadTarget`/
// `GetReloadTarget` below) defaults to null, so nothing here costs anything unless an
// app actually asks for it. Deliberately separate from `RegisterRoot`/`GetRoot`, not a
// replacement for them — those hand back a non-owning pointer specifically for Lustre's
// read-only restyle walk, a contract this type's owning `unique_ptr` would break if
// reused for it.
class ReloadTarget {
public:
    ReloadTarget(std::unique_ptr<Umbra::IWidget> RootWidget, Iris::Component RootTree);

    // The retained previous root tree — what a reload driver walks (in lockstep against
    // a freshly re-rendered tree, matched the same tag+key rule `ReconcileWidget`
    // already uses) to find which `ComponentInstance`s have a prior match to replay
    // against via `iris::ReloadComponentInstance`. This type does not perform that walk
    // itself — see `docs/iris_hot_reload_reconciliation_decision.md` §4 for why.
    const Iris::Component& PreviousTree() const;

    // The retained root widget, non-owning — for a caller that also wants to register
    // the same widget with `iris::RegisterRoot` (Lustre's own, separate, read-only
    // restyle walk) without taking ownership away from this `ReloadTarget`.
    Umbra::IWidget* RootWidget() const;

    // Diffs `New` against `PreviousTree()` via the existing `Reconciler.h`
    // `ReconcileWidget`, applies the result to the retained root widget in place, then
    // retains `New` as the new `PreviousTree()` for the next call. `New`'s own
    // `ComponentInstance`s may be freshly allocated (an untouched subtree) or carried
    // forward via `ReloadComponentInstance` (a matched, replayed one) — `ReconcileWidget`
    // doesn't need to know or care which; it only ever diffs `Component` values, exactly
    // as it already does for an ordinary re-render. A tag/key mismatch at the root falls
    // through to `ReconcileWidget`'s existing unmount-and-mount-fresh branch — tier 3,
    // requiring no code here.
    void Reconcile(Iris::Component New, const MountFn& Mount);

private:
    std::unique_ptr<Umbra::IWidget> RootWidget_;
    Iris::Component                  PreviousTree_;
};

} // namespace iris
