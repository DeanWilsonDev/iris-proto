#pragma once

#include "Iris/Component.h"
#include "Iris/SlotRuntime.h"

#include "Umbra/IWidget.h"

#include <memory>
#include <vector>

namespace iris {

// Computes the strongly-typed diff between two prop maps
// (docs/iris_props_decision.md, `Umbra::IrisPropDiff`'s own doc comment) — a field is
// populated only when `New` actually has that prop and it differs from `Old`'s value
// (or `Old` doesn't have it at all). There is deliberately no way to express "this prop
// was removed" — `IrisPropDiff`'s shape has no room for it, and no Core primitive's
// prop set is dynamic in a way that would need it (every prop a primitive can carry is
// fixed and known, docs/iris_core_spec.md §3.1). `std::function<void()>` and
// `Umbra::TextureHandle` have no meaningful equality (the former structurally, the
// latter because it currently carries no data to compare) — both are treated as
// "changed" whenever `New` has one at all, which is harmless: reapplying an identical
// closure/handle is a no-op from the widget's perspective.
Umbra::IrisPropDiff ComputePropDiff(const Iris::IrisProps& Old, const Iris::IrisProps& New);

// Diffs `Old` against `New` at a single tree position and applies the result to
// `Widget`:
//   - `New.Tag == IrisElementTag::None` → unmount whatever `Widget` currently holds
//     (if anything) and mount nothing. A no-op if `Widget` was already null (still
//     nothing here — docs/iris_core_spec.md §8's `None` sentinel).
//   - Same tag *and* equal key → update in place: compute and apply an
//     `Umbra::IrisPropDiff`, then reconcile `Old.Children` against `New.Children`
//     through `Widget`'s own `Umbra::IWidget` child-management methods (§2's "recurse
//     into children").
//   - Anything else (different tag, different/missing key, nothing was here before) →
//     unmount whatever `Widget` currently holds (if anything) and mount a fresh widget
//     for the whole of `New` via `Mount` — there's no "old" to diff against, so no
//     partial update is attempted.
void ReconcileWidget(std::unique_ptr<Umbra::IWidget>& Widget, const Iris::Component& Old,
                      const Iris::Component& New, const MountFn& Mount);

// Diffs a `<Slot>`'s list-returning case, or any widget's own children list: `OldList`/
// `NewList` matched primarily by `Key` (docs/iris_stage3_decision_doc.md §3), any
// remaining unmatched entries paired up by their relative order among what's left. A
// matched pair keeps its existing widget object (reconciled in place, never rebuilt) —
// the property that actually matters for preserving widget identity (scroll position,
// focus, animation state all live on the widget object, not on its position in a
// list). `Widgets` ends in the same order as `NewList`.
//
// This is the plain-vector form: there's no live `Umbra::IWidget` parent backing
// `Widgets` (a detached `<Slot>` not currently attached to a real tree, or a test
// working against a bare vector), so there's no real "move" cost to optimize —
// reordering `unique_ptr`s in memory is free. It always rebuilds the vector; matched
// entries reuse their widget object (correctness), but this is not the LIS-based
// minimal-move algorithm — see `ReconcileChildrenAt` below for the version that
// matters, which operates against a real widget's children and does compute the
// minimum `RemoveChildAt`/`InsertChildAt` set.
void ReconcileChildren(std::vector<std::unique_ptr<Umbra::IWidget>>& Widgets,
                        const std::vector<Iris::Component>& OldList,
                        const std::vector<Iris::Component>& NewList, const MountFn& Mount);

// The live-widget counterpart to `ReconcileChildren`: `Parent`'s real children at
// `[Base, Base + OldList.size())` correspond 1:1 to `OldList` (same precondition
// `ReconcileWidget`'s own child-recursion already relied on) — reconciles them to
// `NewList`, leaving `Parent`'s children at `[Base, Base + NewList.size())`
// corresponding 1:1 to `NewList`.
//
// Matches `OldList`/`NewList` the same way `ReconcileChildren` does (key first, then
// relative order among the rest), but then computes the longest increasing
// subsequence of matched old positions (LIS, `docs/iris_stage3_implementation_
// decision.md`'s deferred item) — entries in that subsequence are already in the
// right relative order and are left completely untouched structurally (only
// `ApplyPropDiff`/child-recursion runs on them, via a live in-place reference, never a
// `RemoveChildAt`/`InsertChildAt` pair). Every other position gets exactly one
// `InsertChildAt` (a moved existing widget, extracted with exactly one
// `RemoveChildAt`, or a freshly `Mount`ed one) or is removed once and dropped
// (an old entry unmatched in `NewList`). This is the *minimum* number of structural
// operations needed, not merely a correct one — the gap `ReconcileList`'s
// remove-everything/reinsert-everything approach left open.
void ReconcileChildrenAt(Umbra::IWidget& Parent, std::size_t Base,
                          const std::vector<Iris::Component>& OldList,
                          const std::vector<Iris::Component>& NewList, const MountFn& Mount);

} // namespace iris
