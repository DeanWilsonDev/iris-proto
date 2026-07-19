# LIS-based minimal-move list diffing — decision doc

## The gap

`Reconciler.cpp`'s list diff (`ReconcileList`, used by the public `ReconcileChildren`) has
always matched `OldList`/`NewList` correctly (key first, then relative order for the rest —
docs/iris_stage3_decision_doc.md §3) but never computed a *minimal* move set: every call site
that owned a real `Umbra::IWidget` parent (`Reconciler.cpp`'s own `ReconcileWidget`, recursing
into a matched widget's children; `SlotRuntime.cpp`'s two `AttachedParent_` branches) removed
every one of that parent's real children unconditionally, then reinserted the full reconciled
list unconditionally — correct (the right widget objects were always reused for props/identity
purposes), but not the *minimum* number of `RemoveChildAt`/`InsertChildAt` calls, exactly the
gap `docs/iris_stage3_implementation_decision.md` flagged as deliberately deferred and every
Stage 3 `<Slot>`-wiring doc since has repeated as still open (`docs/iris_next_steps.md`'s
"Suggested order" §2).

## What "minimal" means here

Given a keyed match between `OldList` and `NewList` (unchanged matching logic), some matched
entries are already in the right *relative* order across the reorder — the standard trick
(React/Vue/Inferno all use it) is to find the longest increasing subsequence (LIS) of matched
old-positions, in new-list order. LIS members never need to move at all: leaving every other
widget untouched, an LIS member is already exactly where it needs to be. Every entry *not* in
the LIS needs exactly one `RemoveChildAt` + `InsertChildAt` pair (a real move) or, for a
newly-mounted entry, exactly one `InsertChildAt`. Entries dropped entirely need exactly one
`RemoveChildAt`. This is asymptotically optimal for a "no direct move op, only remove/insert"
container API (`Umbra::IWidget` deliberately has no `MoveChildAt` — see its own doc comment,
"expressed as `RemoveChildAt` then `InsertChildAt`").

## Implementation

Two forms now exist side by side in `Reconciler.cpp`/`Reconciler.h`:

- **`ReconcileChildren`** (unchanged in spirit) — the plain-`std::vector<unique_ptr<IWidget>>`
  form, used only where there's no live widget backing the vector (a detached `<Slot>`,
  standalone tests). Reordering `unique_ptr`s in a bare vector has no real "move" cost to
  optimize, so this form still just rebuilds the vector wholesale — correct, simple, and
  genuinely the right tradeoff here (optimizing it would add complexity for zero real saving).
- **`ReconcileChildrenAt(Umbra::IWidget& Parent, std::size_t Base, OldList, NewList, Mount)`**
  (new) — the live-widget form: `Parent`'s real children at `[Base, Base + OldList.size())`
  correspond 1:1 to `OldList` (the same precondition the old remove-all/insert-all code
  already relied on); reconciles them to `NewList`, applying only the minimal
  `RemoveChildAt`/`InsertChildAt` set computed via `ComputeKeepInPlace`'s LIS (O(n log n),
  patience-sorting with predecessor links for reconstruction — `MatchLists`, factored out of
  the old inline matching code, is shared by both forms so they can't drift apart on what
  counts as a match).

  A "kept" (LIS) entry is updated in place through a new `ReconcileMatchedInPlace(Umbra::IWidget&,
  Old, New, Mount)` — a variant of `ReconcileWidget`'s matched-pair logic (`ApplyPropDiff` +
  recurse into children) that takes a non-owning `IWidget&` instead of a `unique_ptr<IWidget>&`,
  since a kept entry's identity/ownership is never in question (same tag, same key, guaranteed
  by the caller). `ReconcileWidget` itself is now expressed in terms of it for the matched-pair
  branch, and its own child-recursion now calls `ReconcileChildrenAt` instead of the old
  manual remove-all-then-insert-all loop — so the same-tag/same-key "reconcile the children of
  a matched widget" path gets the optimization too, not just top-level `<Slot>` lists.

  `SlotRuntime.cpp`'s two `AttachedParent_` branches (`SlotState::Reconcile`'s single- and
  list-output cases) were rewritten to call `ReconcileChildrenAt` directly against
  `AttachedParent_`, replacing their own hand-rolled remove-loop/reconcile-detached-vector/
  insert-loop sequences. The single-output case is expressed as a 0-or-1-element list
  reconciliation (`OldAsList`/`NewAsList`, built from `AttachedCount_`/`NewOutput.Tag`) rather
  than kept as bespoke logic — `ReconcileChildrenAt` already handles 0/1-element lists
  correctly (mount, unmount, update-in-place, tag-mismatch-remount all fall out of the same
  general matching logic), so this also deletes some code rather than adding a parallel path.

## Verification

Extended `tests/ReconcilerTests.cpp` with `ReconcileChildrenAt`-specific tests that assert on
actual `RemoveChildAt`/`InsertChildAt` call counts (a new instrumented counter on the existing
`MockWidget`), not just end-state correctness — appending one item touches zero old entries;
moving one item out of an otherwise-stable four-item list costs exactly one remove + one
insert; a no-op reconcile touches nothing at all; and a non-zero `Base` (simulating a `<Slot>`
sitting after a static sibling) leaves everything before `Base` untouched. All pre-existing
Reconciler/SlotRuntime/SlotResolution/SlotSiblingGroup tests continue to pass unchanged — the
matching semantics (what counts as "the same item") are bit-for-bit identical to before, only
the resulting sequence of widget-tree mutations is smaller. Full suite (115 tests, up from 111)
verified clean under AddressSanitizer + UndefinedBehaviorSanitizer.

## What remains

Nothing further deferred from `docs/iris_stage3_implementation_decision.md`'s original list —
this closes its last open item. The only remaining item in `docs/iris_next_steps.md`'s
"Suggested order" is Stage 4 (Lustre-lite styling), which needs its own design pass before
there's anything to implement.
