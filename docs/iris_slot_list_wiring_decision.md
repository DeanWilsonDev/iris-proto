# Iris — Wiring List-Returning `<Slot>`s Into the Stage 2 Walker

> **Status:** Closed and implemented. Extends
> `docs/iris_slot_stage2_wiring_decision.md`'s single-`IrisComponent`-returning case to
> the list-returning one (`std::function<std::vector<IrisComponent>()>`), and fixes the
> "two sibling `<Slot>`s" limitation that doc's own single-callable case had already
> flagged as deferred.

---

## The problem

`docs/iris_slot_stage2_wiring_decision.md`'s original `SlotState::AttachAt(Parent,
Index)` assumed a fixed `Index`, set once and never revisited. That holds for a
single-`IrisComponent`-returning `<Slot>` with no siblings under its parent, but breaks
in two related ways once either condition is relaxed:

- **A list-returning `<Slot>`'s own length can change across re-renders** (it's a
  `std::vector<IrisComponent>()>`, not a fixed 0-or-1 output) — so even its *own*
  attachment point can't be a single fixed index once it produces more than one item.
- **Any static sibling positioned after a `<Slot>`, or another `<Slot>`, has to shift**
  whenever an earlier `<Slot>` (list-returning or not) changes how many real widgets it
  currently contributes.

A wrapper `Box` around each `<Slot>`'s output was considered and rejected: reading
Penumbra's real `Box::Measure`/`Box::Arrange` source confirmed `LayoutMode::None` simply
doesn't measure or arrange children at all ("footgun accepted" — a literal source
comment), and the stack layout modes impose an unwanted nested layout scope. Neither is
semantically transparent, so a `<Slot>`'s content has to live as direct children of its
real static parent, not inside an intermediate container.

## Decision: `SlotSiblingGroup` — live, shared index coordination

`SlotSiblingGroup` (`include/Iris/SlotRuntime.h`) is constructed once per static parent
by `ResolveSlots` and shared (via `shared_ptr`) by every `<Slot>` child found directly
under that parent, in the order they're encountered. Each entry records:

- `StaticPrefixCount` — the number of *ordinary* (non-`Slot`, non-`None`) children
  immediately preceding this `<Slot>` in the static tree. Fixed forever once
  `ResolveSlots` finishes walking that parent's children.
- A non-owning `SlotState*`.

`SlotSiblingGroup::AbsoluteIndexOf(GroupIndex)` computes a slot's *current* absolute
position within the shared parent's real children as `StaticPrefixCount + Σ` of every
earlier sibling's `SlotState::CurrentRealChildCount()` — recomputed fresh on every call,
since an earlier sibling's own contribution may have changed since the last one.
`CurrentRealChildCount()` is 0 or 1 for a single-`IrisComponent`-returning `<Slot>`, 0..N
for a list-returning one — the same mechanism handles both shapes uniformly, and a
`<Slot>` with no siblings just gets a group with one entry and nothing earlier to sum
(no behavior change from the single-callable case).

`SlotState::AttachAt(Parent, Index)` is replaced by `SlotState::AttachToGroup(Parent,
Group, GroupIndex)`. Every `Reconcile()` call now asks `Group->AbsoluteIndexOf
(GroupIndex)` for its base position immediately before touching `Parent`'s children,
rather than trusting a value cached at attach time:

- **Single-`IrisComponent`-returning shape:** same extract/`ReconcileWidget`/reinsert
  dance as before, just at a freshly computed `Base` instead of a fixed `AttachedIndex_`.
- **List-returning shape (new):** extracts however many widgets this slot currently owns
  (`AttachedCount_`, tracked per-slot) via repeated `RemoveChildAt(Base)`, runs the
  existing `ReconcileChildren` unchanged against that extracted vector, then reinserts
  the (possibly different-length) result via `InsertChildAt(Base + I, ...)` for each
  item, and updates `AttachedCount_` to the new length.

`ReconcileWidget`/`ReconcileChildren` themselves needed **zero changes** — both already
operate on a widget/vector reference they're handed, indifferent to where it lives or
how many entries a list has going in vs. coming out.

## The destruction-order bug ASan caught

The first implementation had `~SlotState()` compute its own removal position via
`AttachedGroup_->AbsoluteIndexOf(AttachedGroupIndex_)`, same as `Reconcile()`. That's
correct while every earlier sibling in the group is still alive — but when an owning
container (e.g. `ResolveSlots`'s returned `std::vector<std::unique_ptr<SlotState>>`) is
torn down, `std::vector`'s destructor destroys elements in forward order: element 0
first. A later sibling's own destructor then calls `AbsoluteIndexOf`, which sums element
0's `CurrentRealChildCount()` — a virtual-dispatch-free but still very real
use-after-free, since element 0's `SlotState` had already been deleted. Confirmed with
AddressSanitizer, not a theoretical concern (`heap-use-after-free ... in
iris::SlotState::CurrentRealChildCount()`).

Fixed by making a destroyed entry self-report as "gone" rather than relying on
destruction order at all: `SlotSiblingGroup::MarkDestroyed(GroupIndex)` nulls that
entry's `SlotState*` once `~SlotState()` has finished removing its own widgets from the
shared parent. `AbsoluteIndexOf` skips null entries (contributing 0 — correct, since a
destroyed slot's widgets are already gone). This makes teardown order-independent: an
entry's contribution to a still-alive sibling's position is always either "its current
live count" (still alive) or "zero" (already destroyed and already removed) — never a
dereference of freed memory, regardless of which sibling in the group gets destroyed
first.

## Where this lives, and why it's entirely backend-agnostic

`SlotSiblingGroup`, `SlotState::AttachToGroup`, and `ResolveSlots` are pure
`Umbra::IWidget` — none of them ever name a Penumbra type. All of it lives in `iris`'s
own runtime (`SlotRuntime.h`/`.cpp`, `SlotResolution.h`/`.cpp`); no changes were needed
on the `iris-penumbra-backend` side at all for this piece.

## Verification

`tests/SlotResolutionTests.cpp` (against a mock `Umbra::IWidget`): a list-returning
`<Slot>` mounting all its items, one between two static siblings, a static sibling
shifting as a list-returning `<Slot>`'s own length grows and shrinks across
`iris::Tick()`-driven re-renders, two list-returning `<Slot>` siblings shifting each
other as the earlier one's length changes, and destroying a `SlotState` still correctly
detaching its content. The full suite (`iris_tests`, all 272 assertions) also passes
clean under AddressSanitizer + UndefinedBehaviorSanitizer — including the destruction-
order case above, which reproduced the bug reliably before the `MarkDestroyed` fix and is
clean after it.

## What remains deliberately deferred

- **Nested `<Slot>` discovery** (a `<Slot>` inside another `<Slot>`'s own dynamically
  produced output) — unchanged from `docs/iris_stage3_implementation_decision.md` and
  `docs/iris_slot_stage2_wiring_decision.md`. `ResolveSlots` only recurses into the
  *static* tree; it never looks inside what a `<Slot>`'s callable itself returns.
