#pragma once

#include "Iris/IrisComponent.h"
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
void ReconcileWidget(std::unique_ptr<Umbra::IWidget>& Widget, const Iris::IrisComponent& Old,
                      const Iris::IrisComponent& New, const MountFn& Mount);

// Diffs a `<Slot>`'s list-returning case, or any widget's own children list: `OldList`/
// `NewList` matched primarily by `Key` (docs/iris_stage3_decision_doc.md §3), any
// remaining unmatched entries paired up by their relative order among what's left. A
// matched pair keeps its existing widget object (reconciled via `ReconcileWidget`,
// never rebuilt) — the property that actually matters for preserving widget identity
// (scroll position, focus, animation state all live on the widget object, not on its
// position in a list). `Widgets` ends in the same order as `NewList`.
//
// **Known limitation** (docs/iris_stage3_implementation_decision.md): this always
// removes and reinserts every entry, matched or not — it reuses the right widget
// objects (correctness), but doesn't compute or apply the *minimum* move set the spec
// describes (an LIS-based algorithm, not yet implemented). A real backend adapter
// would see more `RemoveChildAt`/`InsertChildAt` traffic than strictly necessary; it
// would not see incorrect results or lost widget identity.
void ReconcileChildren(std::vector<std::unique_ptr<Umbra::IWidget>>& Widgets,
                        const std::vector<Iris::IrisComponent>& OldList,
                        const std::vector<Iris::IrisComponent>& NewList, const MountFn& Mount);

} // namespace iris
