# Nested `<Slot>` discovery — decision doc

## The gap

`SlotState`/`Reconciler.cpp` have always assumed the tree a `<Slot>` callable produces
contains no further `<Slot>` tags of its own (`docs/iris_stage3_implementation_decision.md`,
`docs/iris_slot_stage2_wiring_decision.md`, `docs/iris_slot_list_wiring_decision.md` all flag
this as the one item on their "deferred" lists not yet closed). In practice this isn't a
corner case: a `<Slot>` callable very commonly renders another component's own output, and
that component's own `render { }` body can itself contain a `<Slot>` for its own conditional
rendering — a `<Slot>` nested inside another `<Slot>`'s dynamically-produced output. This doc
closes that gap.

## Two separate problems, easy to conflate

1. **Widget-tree correctness.** `Reconciler.cpp`'s `ReconcileWidget`/`ReconcileList` assume a
   1:1 index correspondence between an `IrisComponent`'s `Children` and its matched widget's
   real children. That's true for a tree with no `<Slot>` children (today's precondition) but
   false the moment one appears: a `<Slot>` child contributes **zero** real widgets (same
   convention as `None`, and as `BuildWidgetTree`/`ResolveSlotsRecursive` already implement for
   the static tree), so a naive index-aligned diff would try to `ApplyPropDiff`/recurse/`Mount`
   a real widget for a position that shouldn't have one at all, corrupting the real tree.
2. **`SlotState` lifecycle for the nested `<Slot>` itself.** Once (1) is fixed and a nested
   `<Slot>` position is safely skipped rather than corrupted, *something* still has to notice
   it's there, construct a `SlotState` for it, mount its initial output, and keep reconciling
   it as its own signals fire — independently of whatever caused the *outer* `<Slot>` to
   re-render.

## Fix for (1): ordinary-children filtering in `ReconcileWidget`

`ReconcileWidget` now filters `Old.Children`/`New.Children` down to their non-`Slot` entries
(`FilterOrdinary`, `Reconciler.cpp`) before handing them to `ReconcileList` — `ReconcileList`
itself is untouched, and keeps assuming (correctly, now) 1:1 alignment with the widget's real
children, since a `Slot` entry never reaches it. This mirrors the skip `BuildWidgetTree` and
`ResolveSlotsRecursive` already apply, just one level later in the pipeline (post-initial-mount
reconciliation, not only the one-shot static build). `ReconcileChildren`'s own public contract
(used directly by `SlotState::Reconcile` for a `<Slot>`'s own list-shape *top-level* output) is
**not** changed — a list-returning `<Slot>` callable whose own list contains a bare
`Slot`-tagged entry directly (rather than nested inside an ordinary wrapper) remains
unsupported, same posture as `ResolveSlots`'s existing "`Node.Tag` must not itself be `Slot`"
precondition. This is a deliberately narrower gap than the common case this doc closes (a
nested `<Slot>` living inside an ordinary element somewhere in the output), and is left for
later, consistent with this project's habit of documenting a real, separate remaining gap
rather than blocking on it.

## Fix for (2): rebuild-on-every-reconcile, not incremental matching

The design space here has a much more "efficient" point: recognize that `Reconciler.cpp`
already decides, at every position, whether a widget object is *reused* (same tag+key,
update in place) or *replaced* (fresh `Mount()`) — and a nested `SlotState` attached
somewhere under a reused widget could, in principle, simply be left alone, untouched, across
the outer `<Slot>`'s own re-renders, with no rediscovery needed at all.

That's attractive but unsafe to implement quickly: it requires threading "the nested slots
that live under this specific widget" through `Reconciler.cpp`'s own recursive diff so that a
widget replaced at any depth (not just at the outer `<Slot>`'s own root) reliably tears down
whatever nested `SlotState`s lived under it *before* the widget itself is destroyed — otherwise
a nested `SlotState`'s destructor (which calls `RemoveChildAt` on its own `AttachedParent_` to
detach cleanly) runs against a dangling pointer. A first attempt at this used a global,
pointer-keyed side table (`std::unordered_map<const Umbra::IWidget*, ...>`, the same shape as
`SlotRuntime.cpp`'s existing `SignalRegistry`) to give nested slots a lifetime tied to "this
exact widget object stays alive" — but that requires *every* code path that can destroy a
widget (not just the two or three call sites in `Reconciler.cpp` we control) to notify the
registry first, including a real backend widget destroying its own children recursively, or an
outer `SlotState`'s own destructor dropping `SingleWidget_`/`ListWidgets_`. Missing even one
such path leaves a dangling `SlotState*` in the registry, which is exactly the kind of
destruction-order use-after-free `docs/iris_slot_list_wiring_decision.md` already hit and fixed
once (`SlotSiblingGroup::MarkDestroyed`) — not a mistake worth repeating for a bigger win we
don't strictly need yet.

**What's implemented instead:** `SlotState` owns `NestedSlots_`
(`std::vector<std::unique_ptr<SlotState>>`), rebuilt from scratch on **every** `Reconcile()`
call — mount and re-render alike, regardless of whether this slot's own top-level output ended
up reusing or replacing its widget:

1. At the very top of `Reconcile()`, `NestedSlots_.clear()` — destroying whatever nested
   `<Slot>`s the *previous* render discovered. Safe by construction: this always runs before
   this slot's own `ReconcileWidget`/`ReconcileChildren` call below, so every widget a nested
   `SlotState` might try to detach from is still fully valid at this point, never yet
   replaced or destroyed.
2. This slot's own output is reconciled as before (`ReconcileWidget` for the single-shape
   case, `ReconcileChildren` for the list-shape case) — unchanged.
3. For whatever real widget(s) now hold this slot's current output, walk them via the
   **existing, unmodified** `ResolveSlots()`/`ResolveSlotsRecursive()` (`SlotResolution.h`) —
   exactly the function that already finds `<Slot>` tags nested inside a *static* tree, now
   just pointed at a *dynamically produced* one — and append every `SlotState` it constructs
   into the freshly-cleared `NestedSlots_`. `ResolveSlots` already recurses arbitrarily deep and
   already handles both callable shapes, so nesting more than one level deep (a nested `<Slot>`
   whose own output contains a further-nested `<Slot>`) falls out for free — that nested
   `SlotState`'s own first `Reconcile()` call (triggered by `ResolveSlots`'s initial-mount
   `Slot->Reconcile()`) recurses into this exact same mechanism again, one level down.

Because rediscovery always happens, a nested `<Slot>`'s callable re-runs every time the
*outer* `<Slot>` re-renders, even when the widget it's attached under was reused unchanged.
This is strictly a performance cost, not a correctness one: the callable is expected to be a
pure function of whatever signals it reads, so re-running it and re-reconciling its (typically
identical) output is idempotent — the newly-constructed `SlotState` re-establishes its own
signal dependencies immediately on that first `Reconcile()` call, same as any other slot's
first mount. Avoiding this unnecessary rebuild when the underlying subtree was reused
unchanged is real, separate follow-up work — recorded below, in the same spirit as the
already-open "LIS-based minimal-move list diffing" item.

## Destructor ordering fix this exposed

`~SlotState()`'s existing body detaches this slot's own widget(s) from `AttachedParent_`
*before* any member's own destructor runs (ordinary C++ destruction order: body first, then
members in reverse declaration order) — but `NestedSlots_`'s entries are attached to widgets
*inside* those very widgets. Left as an implicit member, `NestedSlots_` would be destroyed
strictly after the explicit body ran, meaning a nested `SlotState` trying to detach from a
widget its own outer `SlotState` had already ripped out and dropped — a use-after-free.
Fixed by explicitly clearing `NestedSlots_` as the very first line of `~SlotState()`'s body,
before the existing detach logic.

## Verification

Extended `tests/SlotResolutionTests.cpp`: a `<Slot>` whose callable renders an ordinary
wrapper element containing a further `<Slot>` — checks the nested slot mounts on the outer
slot's first render, reacts to its own independent signal via `iris::Tick()` without the outer
slot re-rendering, and gets cleanly torn down (no crash, no dangling widget) when the outer
slot's own output changes to `None` or to a differently-tagged/keyed output entirely.

## What remains deliberately deferred

- **Rediscovery-avoidance for unchanged subtrees** — nested `<Slot>`s currently get torn down
  and reconstructed on every outer re-render rather than persisted when the widget they live
  under was reused unchanged. Real, separate, purely a performance concern.
- **A bare `<Slot>` as a raw list item** (a list-returning `<Slot>` callable whose own list
  contains a `Slot`-tagged entry directly, not wrapped in an ordinary element) — still
  unsupported, same posture as `ResolveSlots`'s pre-existing "`Node.Tag` must not itself be
  `Slot`" precondition.
- **LIS-based minimal-move list diffing** (`docs/iris_stage3_implementation_decision.md`) —
  unchanged, still open.
