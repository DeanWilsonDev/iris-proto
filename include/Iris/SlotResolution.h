#pragma once

#include "Iris/Component.h"
#include "Iris/SlotRuntime.h"

#include "Umbra/IWidget.h"

#include <memory>
#include <vector>

namespace iris {

// Walks `Node`'s tree in lockstep with `Widget` — the real widget tree already built
// from it (e.g. via a `MountFn` combining Stage 2's `BuildWidgetTree` with a
// `Umbra::IWidget` wrapper) — and for every `<Slot>` found as a direct child of an
// otherwise-static position, constructs a `SlotState`, attaches it to that position via
// a `SlotSiblingGroup` shared with every other `<Slot>` under the same static parent
// (`SlotState::AttachToGroup`), and performs its initial mount, splicing its own
// rendered content into `Widget`'s real children at exactly the position that `<Slot>`
// currently occupies among its siblings. Both callable shapes are resolved here — a
// single-`Component`-returning `<Slot>` contributes 0 or 1 real widgets, a
// list-returning one contributes 0..N; either way, `SlotSiblingGroup::AbsoluteIndexOf`
// recomputes each slot's absolute position fresh on every reconcile by summing every
// earlier sibling's *current* real child count, so later siblings shift correctly as an
// earlier list-returning slot's own length changes across re-renders
// (docs/iris_slot_list_wiring_decision.md).
//
// `None` and `<Slot>` children both contribute zero widgets during the initial static
// build (matching how `BuildWidgetTree` already treats `None`, and now treats `<Slot>`
// the same way — see its own doc comment), so this recomputes the correspondence
// between `Node.Children`'s indices and `Widget`'s already-built ones by walking both
// in step, skipping `None`/`<Slot>` entries on one side while `Widget`'s own children
// stay put on the other.
//
// Recurses into ordinary (non-`Slot`, non-`None`) children to find further `<Slot>`s
// nested deeper in the static tree. Does **not** recurse into a `<Slot>`'s own
// dynamically-produced output looking for further nested `<Slot>`s — that's the
// documented, deferred "nested `<Slot>` discovery" gap
// (docs/iris_stage3_implementation_decision.md).
//
// `Node.Tag` must not itself be `IrisElementTag::Slot` — a `<Slot>` at the very root of
// what's being resolved (e.g. `render { <Slot>...</Slot> }`, no static wrapper at all)
// has no "parent widget position" for this function's model; construct a plain,
// unattached `SlotState` for that case instead (`SlotState`'s own constructor,
// documented in `SlotRuntime.h`).
//
// Returns every `SlotState` created, in tree order. The caller owns them for exactly as
// long as the tree they were resolved against stays mounted — destroying one detaches
// and drops whatever it last rendered (`SlotState`'s own destructor).
std::vector<std::unique_ptr<SlotState>> ResolveSlots(Umbra::IWidget& Widget, const Iris::Component& Node,
                                                       MountFn Mount);

} // namespace iris
