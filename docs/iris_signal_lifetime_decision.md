# Iris — `Signal<T>`/Component-Lifetime Decision

> **Status:** Closed and implemented. Fixes a real, confirmed dangling-reference bug in
> Stage 3's foundational design (`docs/iris_stage3_decision_doc.md` §0), not something
> introduced by this repo's Stage 3 implementation work — `Signal`/`SlotState`/the
> reconciler faithfully implemented what was specified; the specification itself was
> unsound C++.

---

## The bug

`docs/iris_stage3_decision_doc.md` §0 asserts `iris::Signal<T>` locals are "true
long-lived locals... captured by reference in render lambdas," and every example in
`docs/iris_core_spec.md` writes this pattern:

```cpp
Component StartMenu() {
    iris::Signal<bool> settingsOpen = false;
    render {
        <Slot>{[&]() -> Component { return settingsOpen.get() ? ... : nullptr; }}</Slot>
    }
}
```

`settingsOpen` is an ordinary stack local. `StartMenu()` runs exactly once and
*returns* — per its own documented contract, and because Stage 1 codegen literally
rewrites the `render { }` block into `return <expr>;`. The instant it does,
`settingsOpen`'s stack storage is gone. The `[&]`-capturing `<Slot>` lambda now holds a
dangling reference — permanently, since nothing ever makes `StartMenu()`'s frame valid
again.

**Confirmed, not assumed:** reproduced with a minimal repro and caught by
AddressSanitizer as a textbook stack-use-after-return, then reproduced again against
real generated `.iris` output (compiled through `iris_cc`) while verifying
`iris-penumbra-backend`'s `Umbra::IWidget` adapter — the crash was real, not a fluke or
a harness bug. This affects every `[&]`-capturing `<Slot>` example in the spec; it's the
load-bearing pattern the entire reactive model depends on, not a corner case.

## Options considered

**Heap-allocate `Signal<T>`'s value behind a `shared_ptr`, require capture by value.**
Doesn't actually work as a drop-in fix: `[&]` captures a reference to the *local
variable's own storage*, not to whatever it contains — no amount of internal
indirection inside `Signal<T>` changes that a `[&]`-captured stack local dies with its
frame. The only way heap allocation helps is if the lambda captures a *copy* of a
cheap, shared-state handle (`[=]` or explicit `[settingsOpen]`) instead of a reference.
Rejected: nothing can enforce this — Iris never parses escape-hatch contents, and
ordinary C++ compilation doesn't reject `[&]` on a handle-shaped type either, since
capturing a shared_ptr-backed object by reference is exactly as syntactically legal (and
exactly as unsafe) as capturing a plain value by reference. This narrows the failure
mode without closing it, and every spec example would need rewriting from `[&]` to
`[=]` regardless.

**Make component functions C++20 coroutines**, so their local variables live in a
heap-allocated frame that persists past `co_return`. Preserves `[&]` exactly as
written. Rejected as the primary mechanism: Iris never parses component function
signatures (§2.1's explicit boundary — the same rule that ruled out real
forward-declaration headers, `docs/iris_import_header_decision.md`), so making this
transparent to component authors would mean Iris crossing that boundary for the first
time; the alternative (authors hand-write coroutine boilerplate) is a real ergonomic
regression. Also an atypical use of coroutines — `co_return` once, never resume — using
a feature designed for repeated suspension purely for its "persistent heap frame" side
effect. Meaningfully larger implementation surface (frame lifetime, `promise_type`,
allocation elision) and worse debuggability than the option below, for a problem that
doesn't need coroutines to solve.

## Decision: reference-bound heap storage tied to component-instance lifetime

`iris::Signal<T> Name = InitExpr;` is replaced by:

```cpp
IRIS_SIGNAL(T, Name, InitExpr);
```

An ordinary C++ preprocessor macro (`include/Iris/ComponentInstance.h`) — not something
Iris's own preprocessor needs to recognize; it expands during normal compilation, after
Iris has already produced its output:

```cpp
#define IRIS_SIGNAL(Type, Name, InitExpr) auto& Name = ::iris::Detail::DeclareSignal<Type>(InitExpr)
```

`Name` is now a genuine *reference*, bound to a `Signal<Type>` heap-allocated by
`DeclareSignal`. This is the key move: **when a lambda captures a reference-typed
variable by reference, it aliases the referent directly** — that's how C++ reference
capture works (references have no independent identity separate from what they're
bound to). If the referent lives on the heap, `[&Name]` is completely safe, and — this
is what makes the fix transparent — **`[&]` throughout the rest of the file keeps
meaning exactly what it already means everywhere in the spec.** No capture-list syntax
changes anywhere except the one declaration itself.

### Where the heap allocation lives, and how its lifetime is tied to the component

`iris::ComponentInstance` (`ComponentInstance.h`) owns every signal declared during one
component function's single run — a heterogeneous collection
(`Detail::SignalStorageBase`/`SignalStorage<T>`, type-erased so one `ComponentInstance`
can hold signals of different `T`).

`DeclareSignal<T>` allocates against *whichever `ComponentInstance` is currently
ambient* — the same push/pop-a-stack pattern already built and tested for `<Slot>`'s
dependency tracking (`IrisRuntime::PushActiveSlot`/`PopActiveSlot`), applied to a second
problem via `IrisRuntime::PushComponentInstance`/`PopComponentInstance`/
`CurrentComponentInstance()`.

Establishing "the current instance" happens at the one place Iris's own `Codegen`
already fully controls the shape of: every component invocation it emits. `<HealthBar
current={...} />` no longer becomes just `HealthBar(HealthBarProps{...})` — it becomes:

```cpp
iris::MountComponentInstance([&]() -> Iris::Component { return HealthBar(HealthBarProps{...}); })
```

`MountComponentInstance` pushes a fresh `ComponentInstance`, calls the lambda (any
`IRIS_SIGNAL` inside `HealthBar()`'s body registers against it), pops the stack, and
stashes the instance on the result (`Component::Instance`, a new
`std::shared_ptr<iris::ComponentInstance>` field). This is the same category of change
as the existing `key`-setting IIFE `Codegen.h` already wraps invocations in
(`docs/iris_stage3_implementation_decision.md`'s Decision 3) — not new territory.

**Lifetime needs no separate "on unmount" hook.** `SlotState` (`SlotRuntime.h`) already
retains the last-rendered `Component` tree between reconcile passes
(`PreviousSingle_`/`PreviousList_`) purely for diffing purposes — and since that tree
recursively carries every nested `Component::Instance` shared_ptr within it,
ordinary reference counting through storage that already existed is what keeps each
component's signals alive for exactly as long as it stays in the tree. The moment a
component leaves the tree (a `Reconcile()` call replaces `PreviousSingle_`/
`PreviousList_` with a new tree that no longer references it), the old value — and
every `ComponentInstance` it was the last owner of — is destroyed automatically. No
changes to `Umbra::IWidget` or the Penumbra adapter were needed at all.

### The one thing not covered by generated code: the root component

Codegen only wraps invocations *it* emits — the very first component in the tree (what
a host application calls directly, outside any `render { }` block) is written by the
application itself and never seen by Iris. `iris::Mount(RootComponentFn)` is the small,
documented entry point for that one call site — the same underlying mechanism as
`MountComponentInstance`, just named for where an app's `main()` is expected to reach
for it.

## Verification

- `tests/ComponentInstanceTests.cpp`: a signal is readable after the declaring lambda
  returns (the exact shape of the original bug); multiple signals in one component all
  survive independently; `set()`/`get()` both work long after the declaring function
  returned; the `ComponentInstance` (and its signals) are freed exactly when the owning
  `Component` is dropped (verified via `shared_ptr::use_count()`).
- The same test file, compiled standalone under AddressSanitizer + LeakSanitizer:
  clean exit, zero errors — both the original crash class and any new leak from the
  heap allocation this fix introduces.
- Full pipeline re-verified end to end: a real `.iris` file (`IRIS_SIGNAL` instead of a
  direct `Signal<T>` declaration) compiled through `iris_cc`, mounted via
  `iris::Mount()`, its `<Slot>` callable invoked directly (mirroring what `SlotState`
  does internally) from outside the declaring function's now-long-returned stack frame,
  producing a real Penumbra `Label` widget — under AddressSanitizer, zero errors.

## What changed for component authors

The one required, mechanical change: `iris::Signal<T> Name = InitExpr;` →
`IRIS_SIGNAL(T, Name, InitExpr);`. Every code sample in `docs/iris_core_spec.md` using
the old direct-declaration form has been updated. `docs/iris_stage1_decision_doc.md`,
`docs/iris_stage3_decision_doc.md`, and `docs/iris_stage3_decision_slot.md` are
historical decision records (per `CLAUDE.md`'s own framing — "kept for the reasoning
trail, not current truth") and were deliberately left showing the old, now-superseded
syntax rather than rewritten.

Consuming code (whatever assembles the final translation unit around generated
`.iris.h` output) now also needs `#include "Iris/ComponentInstance.h"` — the existing
convention already requires manually including `Iris/Component.h` before pulling in
generated headers (Driver.cpp doesn't auto-inject Iris runtime includes), so this isn't
a new category of requirement, just one more header in that same list.

## What remains deliberately deferred

- **`iris::Mount()`'s own call site isn't exercised by any generated `.iris` file** —
  by definition, since it's for the one component invocation Iris never generates.
  Verified manually and via `ComponentInstanceTests.cpp`, not via `DriverTests.cpp`.
- Everything already listed as deferred in `docs/iris_stage3_implementation_decision.md`
  (nested-`<Slot>` discovery, LIS-based minimal-move list diffing, wiring `<Slot>` into
  the Stage 2 walker) is unaffected by and orthogonal to this fix.
