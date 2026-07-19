#pragma once

#include "Iris/IrisComponent.h"
#include "Iris/SlotRuntime.h"

#include "Umbra/IWidget.h"

#include <memory>
#include <vector>

namespace iris {

// Walks `Node`'s tree in lockstep with `Widget` — the real widget tree already built
// from it (e.g. via a `MountFn` combining Stage 2's `BuildWidgetTree` with a
// `Umbra::IWidget` wrapper) — and for every `<Slot>` found as a direct child of an
// otherwise-static position, constructs a `SlotState`, attaches it to that position
// (`SlotState::AttachAt`), and performs its initial mount, splicing its own rendered
// content into `Widget`'s real children at exactly the position that `<Slot>` occupies
// among its siblings.
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
// (docs/iris_stage3_implementation_decision.md). Does **not** resolve a list-returning
// `<Slot>` (a callable returning `std::vector<IrisComponent>`) at all — attaching one at
// a stable index doesn't hold once the list's own length can change; it's left exactly
// as `BuildWidgetTree` already leaves it (contributing nothing).
//
// **Known limitation, not solved here:** if two `<Slot>`s are direct siblings under the
// same static parent, and the *earlier* one (by position) ever transitions between
// producing a widget and producing `IrisElementTag::None`, the *later* one's own
// attached index goes stale — nothing currently renumbers a sibling `SlotState` when an
// earlier one's real child count changes. Correct as long as either only one `<Slot>`
// sits under a given static parent, or any earlier ones never toggle to/from `None`.
// See docs/iris_slot_stage2_wiring_decision.md for the reasoning and what a full fix
// would need.
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
std::vector<std::unique_ptr<SlotState>> ResolveSlots(Umbra::IWidget& Widget, const Iris::IrisComponent& Node,
                                                       MountFn Mount);

} // namespace iris
