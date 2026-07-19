# Iris — Stage 3 Reactive Runtime: Implementation Decisions

> **Status:** Core reactive engine closed and implemented (Signal, ambient dependency
> tracking, batching, the reconciler). Backend integration (a real Penumbra `IWidget`
> adapter, wiring `SlotState` into the Stage 2 walker, nested-`<Slot>` discovery) is
> explicitly **not** part of this pass — see "What remains deliberately deferred".
>
> Records several implementation decisions `docs/iris_stage3_decision_doc.md` and
> `docs/iris_stage3_decision_slot.md` left genuinely open — they establish data shapes
> and matching *rules* but not, in a few load-bearing places, the actual mechanism.

---

## Why this document exists

Reading the Stage 3 decision docs closely (rather than assuming their code samples were
implementation-ready) surfaced several real gaps between "the shape is specified" and
"there's a working mechanism":

1. **`key` never reached `IrisComponent`.** `docs/iris_props_decision.md` states `key`
   is stripped before codegen, and `include/Iris/IrisComponent.h` had no `Key` field at
   all. The reconciler's entire matching rule ("same tag + same key") had nothing to
   match on.
2. **How a `Signal` knows which `<Slot>`s to mark dirty was never specified.** The docs
   assert `.set()` marks dependent slots dirty via `[&]` capture, but capture alone
   doesn't tell a signal *who's* reading it — some registration mechanism has to exist,
   and none is described.
3. **`IWidget`/`IWidgetLifecycle`/`IrisPropDiff` were said to "currently live in
   Penumbra, pending extraction to `umbra-interfaces`"** — but `CLAUDE.md` has a hard,
   explicit rule that this repo has zero dependency on Penumbra or any backend. Those
   two statements can't both be true at once.
4. **`IWidget`, as specified, had no way to attach/detach/reorder children** — only
   `ApplyPropDiff`. The documented matching rule explicitly says "recurse into
   children," which needs more than prop-diffing alone can do.

Each is addressed below, then implemented.

## Decision 1: `umbra-interfaces` is now a real repo

Per the user's explicit direction (asked directly rather than picked from a menu):
create `umbra-interfaces` now, matching what the decision doc actually describes, rather
than working around the conflict with a compatibility shim.

[`github.com/DeanWilsonDev/umbra-interfaces`](https://github.com/DeanWilsonDev/umbra-interfaces) —
header-only, zero dependencies, names no concrete runtime or backend. Contains:

- `Umbra::IWidget` / `Umbra::IrisPropDiff` (`include/Umbra/IWidget.h`)
- `Umbra::IWidgetLifecycle` / `Umbra::TickInfo` (`include/Umbra/IWidgetLifecycle.h`) —
  mirrors `penumbra-proto`'s own `include/Penumbra/IWidgetLifecycle.h` verbatim, whose
  own doc comment already said it "should lift out into a standalone umbra-interfaces
  library unchanged" once one existed.
- `Umbra::TextureHandle` (`include/Umbra/TextureHandle.h`) — moved here from being
  Iris-specific; a texture handle is a rendering-backend concept, not specific to any
  one runtime depending on this package. `iris::TextureHandle`
  (`include/Iris/TextureHandle.h`) is now a plain alias:
  `using TextureHandle = Umbra::TextureHandle;`.

Vendored into `iris` as a git submodule (`libs/umbra-interfaces`), the same pattern as
Amanuensis — `iris` still has zero dependency on Penumbra or any concrete backend;
`umbra-interfaces` is neither.

## Decision 2: `IWidget` gained child-management methods

Added directly to `umbra-interfaces` (safe to extend immediately — nothing depended on
it yet when this was found):

```cpp
virtual std::size_t GetChildCount() const = 0;
virtual IWidget*    GetChildAt(std::size_t Index) const = 0;
virtual void         InsertChildAt(std::size_t Index, std::unique_ptr<IWidget> Child) = 0;
virtual std::unique_ptr<IWidget> RemoveChildAt(std::size_t Index) = 0;
```

Mirrors Penumbra's own `Box` (`AddChild`/`InsertChildAt`/`RemoveChild`/`MoveChild`,
verified against the real shipped widget, not just a requirements doc) closely enough
that a Penumbra-side adapter should be straightforward. `MoveChild` is expressed as
`RemoveChildAt` + `InsertChildAt` rather than its own primitive, keeping the interface
smaller.

## Decision 3: `key` representation on `IrisComponent`

`IrisComponent` gained a `std::optional<IrisPropValue> Key;` field — reusing the
already-closed `IrisPropValue` variant (`string`, `int`, `float`, `bool`,
`function<void()>`, `TextureHandle`) rather than inventing a new type, since every real
example in the spec uses a plain `string`- or `int`-shaped key
(`key={item.id}`, `key="bar"`) and `IrisPropValue`'s ordinary converting constructor
already picks the right alternative from the expression's own type with no
`in_place_type` hinting needed.

**Emission is uniform across every element kind**, not threaded through each
`Emit*` function individually: `ComponentEmitter::Emit()` always computes the base
expression first (a primitive's aggregate-style construction, or a component
invocation's function call — unchanged), and only when `Node.Key` is present wraps it:

```cpp
[&]() { Iris::IrisComponent Node = <base expression>; Node.Key = Iris::IrisPropValue(<key expression>); return Node; }()
```

This means a component invocation's `key` (`<HealthBar key={member.id} .../>`) works
identically to a primitive's — the key is set on whatever `IrisComponent` came back,
regardless of how it was built. Tested in `tests/CodegenTests.cpp`
(`TestKeyedPrimitiveWrapsBaseExpressionAndSetsKey`,
`TestKeyedComponentInvocationAlsoWrapsWithKey`), and verified end-to-end: a real `.iris`
file with `key={props.id}` compiled through `iris_cc` and host-compiled, confirming
`Node.Key` actually holds the right runtime value.

## Decision 4: ambient "active slot" signal tracking

Per the user's explicit choice between two options presented. `Signal<T>::get()`
(`include/Iris/Signal.h`) calls `TrackSignalDependency(this)`
(`include/Iris/SlotRuntime.h`), which registers whichever `SlotState` is on top of
`IrisRuntime`'s active-slot stack (`IrisRuntime::PushActiveSlot`/`PopActiveSlot`,
pushed/popped by `SlotState::Reconcile()` around invoking its callable) as a dependent
of that signal, keyed by the signal's own `this` pointer (type-erased — the tracking
mechanism never needs to know `T`). `Signal<T>::set()` calls
`NotifySignalDependents(this)`, marking every registered dependent dirty
(`SlotState::MarkDirty()`) — never reconciling synchronously.

This is the standard "getter-based" dependency-tracking pattern (the same shape as
React/Vue/Solid-style automatic reactivity without compiler support): a `<Slot>`'s
dependencies are re-collected every time it's invoked (cleared via
`SignalRegistry::ClearSlot` right before each invocation, since which signals get read
can vary between calls — e.g. a conditional branch), so a slot never keeps a stale
subscription to a signal it stopped reading.

Verified in `tests/SlotRuntimeTests.cpp`: a slot reading a signal genuinely gets marked
dirty and re-invoked on the next `Reconcile()`/`Tick()` call; a slot that never reads a
signal doesn't.

## Decision 5: batching and `iris::Tick()`

`IrisRuntime` (`include/Iris/SlotRuntime.h`, `src/Iris/SlotRuntime.cpp`) owns:

- `BeginBatch()`/`EndBatch()` — nestable; only the outermost `EndBatch()` (depth back to
  zero) actually reconciles. `ScopedEventBatch` is the RAII convenience a backend
  adapter's own event-dispatch code is expected to construct around invoking a handler
  (docs/iris_stage3_decision_doc.md §6's "every event handler invocation is
  auto-wrapped" — Iris's preprocessor never sees or rewrites event-handler lambdas
  itself, so this wrapping has to happen at the dispatch call site, not inside Iris).
- `ReconcileDirtySlots()` — takes a snapshot of the dirty set (so a slot reconciling
  can safely mark other slots dirty without invalidating the iteration), reconciles
  each. What both `EndBatch()` and `iris::Tick()` call.
- `iris::Tick()` (free function) — calls `ReconcileDirtySlots()` directly, unconditional
  and idempotent if nothing's dirty. Matches `docs/iris_stage3_decision_doc.md` §7's
  `PenumbraApp::Tick` pseudocode calling it directly before `Measure()`/`Arrange()`/
  `Draw()` — `iris::Tick()` has zero Penumbra dependency; a backend's own frame loop
  calls it as an opaque function.

Verified: multiple `Signal::set()` calls inside one batch collapse into exactly one
reconcile at `EndBatch()`, not one per `set()` call
(`TestEventBatchCollapsesMultipleSetsIntoOneReconcile`).

## Decision 6: the reconciler algorithm

`include/Iris/Reconciler.h` / `src/Iris/Reconciler.cpp`:

- **`ComputePropDiff(Old, New)`** — per-field comparison (not blanket `IrisPropValue`
  equality, since `std::function`/`TextureHandle` have no meaningful `operator==`); a
  field is populated only when `New` has that prop and it differs from `Old`'s value (or
  `Old` doesn't have it). There is deliberately no way to express "this prop was
  removed" — no Core primitive's prop set is dynamic in a way that needs it.
- **`ReconcileWidget(Widget, Old, New, Mount)`** — the single-position matching rule:
  `New.Tag == None` unmounts and mounts nothing; same tag + equal key updates in place
  (`ApplyPropDiff` + recurse into children); anything else unmounts whatever was there
  (if anything) and mounts a fresh widget via `Mount` for the whole of `New` (no partial
  update attempted when there's no compatible "old" to diff against).
- **`ReconcileChildren`** — a keyed list diff: explicit keys matched first
  (position-independent — an item that moved is still recognised), remaining unkeyed
  entries matched by relative order among what's left. A matched pair keeps its
  existing widget object, reconciled in place rather than rebuilt — the property that
  actually matters (scroll position, focus, animation state live on the widget object,
  not its position in a list).

Both nested-children recursion (through a matched widget's own `GetChildCount`/
`RemoveChildAt`/`InsertChildAt`) and a `<Slot>`'s own top-level list output go through
the same `ReconcileList` core, avoiding two divergent implementations of the same
matching logic.

**Known limitation, documented rather than silently accepted as optimal:** the list
diff always removes and reinserts every entry, matched or not — it reuses the right
widget *objects* (correctness, identity preservation), but doesn't compute the
*minimum* move set `docs/iris_stage3_decision_doc.md` §3 describes (an LIS-based
algorithm). A real backend adapter would see more `RemoveChildAt`/`InsertChildAt`
traffic than strictly necessary, never incorrect results or lost widget identity.
Verified thoroughly in `tests/ReconcilerTests.cpp` against a mock `IWidget` — including
that a reordered keyed list reuses the same widget objects in their new positions, that
a tag mismatch actually destroys the old widget (not just replaces the pointer), and
that nested children reconcile correctly through a parent that's itself being updated in
place.

## Decision 7: `IrisElementTag::None`'s reconciler contract

`ReconcileWidget`'s `None` handling: transitioning *to* `None` unmounts whatever was
there and mounts nothing (`Widget.reset()`) — a no-op if nothing was there already
(resetting an already-null `unique_ptr` is safe), which is exactly the "still nothing,
no re-render needed" case for a `<Slot>` that keeps returning `nullptr`. Transitioning
*from* `None` to something real is just an ordinary "no old to diff against" mount,
since `None` never matches any real tag. This was deliberately deferred to this pass
(per an earlier explicit decision, when the `nullptr_t` constructor gap was fixed) —
now decided by implementation, not left further open.

## What remains deliberately deferred

Scoped out of this pass, each a real, separate piece of work:

- **A real Penumbra `IWidget` adapter.** ~~Nothing in `iris-penumbra-backend` yet
  implements `Umbra::IWidget` by wrapping `Penumbra::Widgets::WidgetBase` — this
  runtime is tested exclusively against a mock `IWidget` (`tests/ReconcilerTests.cpp`,
  `tests/SlotRuntimeTests.cpp`). The mock proves the algorithm is correct; it doesn't
  prove a real Penumbra widget tree reconciles correctly yet.~~ **Done** —
  `iris-penumbra-backend`'s `PenumbraWidget`
  (`docs/iris_penumbra_backend_adapter_decision.md` in that repo), verified against
  real `Box`/`Label` objects.
- **Wiring `SlotState` into Stage 2's walker.** ~~`IrisPenumbraBackend::BuildWidgetTree`
  (the Stage 2 walker) still explicitly asserts on encountering an `IrisElementTag::Slot`
  node rather than resolving it — it was built and tested before `<Slot>` resolution
  existed. Making a `<Slot>`-containing tree actually mountable means teaching that
  walker (or something layered in front of it) to construct a `SlotState` at each
  `<Slot>` position and splice its own widget(s) in.~~ **Done**, for both the
  single-`IrisComponent`- and list-returning callable shapes —
  `docs/iris_slot_stage2_wiring_decision.md` and `docs/iris_slot_list_wiring_decision.md`.
- **Nested `<Slot>` discovery.** `SlotState`/`Reconciler` assume the trees a slot's
  callable produces contain no further `<Slot>` tags — matching Stage 2's own
  documented precondition ("the tree a backend pass ever sees is fully concrete — no
  Slot nodes"). Recursively finding nested `<Slot>` tags within an arbitrary
  `IrisComponent` tree and giving each its own independent `SlotState` (per the spec's
  own `<Slot>`-scoped-diffing design) is real, separate work this pass doesn't attempt.
  **Still open** — the one item on this original list not yet closed.
- **LIS-based minimal-move list diffing** (Decision 6's known limitation). **Still open.**
- **Numeric (`int`/`float`) `IrisPropDiff` fields** — still absent, per
  `docs/iris_props_decision.md`'s original note; add both together, deliberately, if a
  Core primitive ever needs one.
