# Iris — Wiring `<Slot>` into the Stage 2 Walker

> **Status:** Closed and implemented for the single-`IrisComponent`-returning case. The
> list-returning case, and the "two sibling `<Slot>`s" limitation below, are now also
> closed — see `docs/iris_slot_list_wiring_decision.md`.

---

## The problem

Stage 2's `BuildWidgetTree` (`iris-penumbra-backend`) builds a widget tree from an
`IrisComponent` tree bottom-up: children are built first, then attached to their parent
via Penumbra's `Box::Builder`. `<Slot>` never fit into this — its content isn't known
until its callable is invoked, and per `docs/iris_core_spec.md` §2.5 the backend is
never supposed to see a `Slot`-tagged node at all. Until now, `BuildWidgetTree` simply
asserted if it ever encountered one.

The real tension, once Stage 3's reconciler and the Penumbra `Umbra::IWidget` adapter
both existed (`docs/iris_stage3_implementation_decision.md`,
`iris-penumbra-backend/docs/iris_penumbra_backend_adapter_decision.md`): a `<Slot>`'s
*parent* tree is static — built once, never revisited by the ordinary reconciler, per
`docs/iris_stage3_decision_doc.md` §1 ("everything outside a `<Slot>` is now provably
static once mounted"). But the `<Slot>`'s own content, sitting at a fixed position
*within* that static parent, needs to update in place whenever its captured signal
fires — and that update has to reach the real Penumbra `Box::Children` at that exact
position, not some disconnected copy.

## Decision: two-phase build, `SlotState` gets an "attached" mode

**Phase 1 (unchanged):** `BuildWidgetTree` treats `IrisElementTag::Slot` exactly like
`IrisElementTag::None` — it returns `nullptr`, contributing nothing to its parent's
built children. The static tree gets built completely, with `<Slot>` positions simply
absent.

**Phase 2 (new):** `iris::ResolveSlots(Widget, Node, Mount)` (`include/Iris/
SlotResolution.h`) walks the just-built widget tree and the original `IrisComponent`
tree in lockstep. Since `None` and `Slot` children both contributed zero widgets during
Phase 1, walking the two trees together recovers the correspondence: for each ordinary
static child, advance both cursors together (and recurse, in case a `<Slot>` sits
deeper in an ordinary child's own static children); for each `<Slot>` child, construct a
`SlotState`, tell it exactly where it lives (`SlotState::AttachAt(Parent, Index)`), and
call `Reconcile()` to perform its initial mount — which splices its first render into
`Parent`'s own real children at `Index`.

**`SlotState` gained a second mode.** Previously it always owned its widget privately
(a plain `unique_ptr<Umbra::IWidget> SingleWidget_` member) — fine for a `<Slot>` that's
the literal root of what's mounted (no static parent at all), but wrong for one embedded
inside a larger static tree, where the *real* ownership has to live in the parent's own
`Umbra::IWidget::InsertChildAt`/`RemoveChildAt`-managed children (that's what Penumbra's
own rendering, layout, and hit-testing actually walk). In attached mode, every
`Reconcile()` call — including ones triggered automatically by `iris::Tick()` — does:

```cpp
// Only if something is currently there (a previous None output means nothing to pull
// back — None never occupied a real child slot in the first place):
auto Current = PreviousSingle_.Tag != None ? AttachedParent_->RemoveChildAt(AttachedIndex_)
                                            : nullptr;
ReconcileWidget(Current, PreviousSingle_, NewOutput, Mount_); // completely unchanged
if (Current) AttachedParent_->InsertChildAt(AttachedIndex_, std::move(Current));
```

`ReconcileWidget` itself needed **zero changes** — it already operates on a
`std::unique_ptr<Umbra::IWidget>&` it's handed, indifferent to where that reference
actually lives. `SlotState`'s destructor also detaches (and drops) its content from the
attached parent if something is still there, so a torn-down `SlotState` doesn't leave
orphaned content behind.

This is the same category of "borrow real ownership temporarily, hand it back" dance
`iris-penumbra-backend`'s `PenumbraWidget` adapter already uses for its own
`InsertChildAt`/`RemoveChildAt` (owning vs. attached), applied one level up.

## Where this lives, and why it's entirely backend-agnostic

`ResolveSlots` and `SlotState::AttachAt` are pure `Umbra::IWidget` — they never name a
Penumbra type. They live in `iris`'s own runtime (`SlotRuntime.h`/`.cpp`,
`SlotResolution.h`/`.cpp`), not `iris-penumbra-backend`. The only change needed on the
Penumbra side was making `BuildWidgetTree`'s `Slot` case return `nullptr` instead of
asserting (`iris-penumbra-backend/src/IrisPenumbraBackend/Walker.cpp`) — a one-line
change, since `Umbra::IWidget`'s real child-management methods (`InsertChildAt`/
`RemoveChildAt`, added for `docs/iris_stage3_implementation_decision.md`'s reconciler
work) turned out to already be exactly what `ResolveSlots` needed.

## Verification

`tests/SlotResolutionTests.cpp` (`iris`, against a mock `Umbra::IWidget`): a `<Slot>` as
the sole child, between two static siblings (correct index), returning `None`
initially (contributes nothing), a signal-driven toggle through `iris::Tick()` (content
attaches and detaches from the real mock tree), a nested `<Slot>` found by recursing
into a static child, a list-returning `<Slot>` confirmed left untouched, and destroying
a `SlotState` detaching its content.

`tests/SlotWiringTests.cpp` (`iris-penumbra-backend`, against **real**
`Penumbra::Widgets::Box`/`Label` objects): the same static-tree-plus-slot shape, and —
the actual point of this work — a live `iris::Signal` update reaching all the way
through `IrisRuntime`/`iris::Tick()`/`SlotState`/the reconciler/`PenumbraWidget` to a
real Penumbra `Box::Children` vector, verified by inspecting that real vector directly.

## What remains deliberately deferred

- **Nested `<Slot>` discovery** (a `<Slot>` inside another `<Slot>`'s own dynamically
  produced output). ~~unchanged from `docs/iris_stage3_implementation_decision.md`.
  `ResolveSlots` only recurses into the *static* tree; it never looks inside what a
  `<Slot>`'s callable itself returns.~~ **Done** —
  `docs/iris_nested_slot_discovery_decision.md`.

List-returning `<Slot>`s, and sibling `<Slot>`s whose earlier one's real child count
changes (list growth/shrink, or toggling to/from `None`), are now resolved correctly —
see `docs/iris_slot_list_wiring_decision.md` for the `SlotSiblingGroup` mechanism that
closed both gaps.
