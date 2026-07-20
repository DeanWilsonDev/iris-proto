# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What Iris is

Iris is a thin preprocessor and reactive UI runtime over a host language — currently C++23
(`.iris` files), with a future Nyx host language (`.irisx`) deferred. It is **not** a
standalone component/props/state/event DSL: after the Stage 1 pivot, Iris only owns the
`render { }` block grammar, `import` resolution, and the `key`/`class` reserved props.
Everything else in a `.iris` file — component declaration, props structs, `iris::Signal<T>`
state, event handlers, `if`/`for` control flow — is ordinary host-language (C++23) code that
passes through the preprocessor untouched.

`docs/iris_core_spec.md` (v2) is the single authoritative language reference — where any
other doc disagrees with it, the spec wins. The other `docs/iris_stage*` files are
chronological decision records/handoffs, kept for the reasoning trail, not current truth;
read the spec first.

## Build and test

```sh
cmake -S . -B build
cmake --build build
./build/tests/test_iris
```

One test executable, `test_iris`, built from every `tests/*.cpp` file plus `tests/TestMain.cpp`
(the entry point — just `#include <cimmerian/test-entry-point.hpp>`, Cimmerian supplies
`main()`). All tests use Cimmerian (`libs/cimmerian`, a git submodule, `CIMMERIAN_VISUAL_PLATFORM`
forced to `None` in `CMakeLists.txt` to avoid its default X11/libXtst dependency — see the
comment there): `DESCRIBE("GroupName", { IT("description", { ...; ASSERT_TRUE(cond); ... }); })`,
auto-registered — no `RunXTests()`-called-from-`main()` wiring needed, just add a new `IT(...)`
inside the relevant file's `DESCRIBE` block (or a new file, since it's auto-registered
regardless of which translation unit it's compiled in — just add it to `CMakeLists.txt`'s
`test_iris` source list). `ASSERT_TRUE`/`ASSERT_FALSE`/`ASSERT_EQUAL`/`ASSERT_NOT_EQUAL` report
and continue; `REQUIRE_TRUE`/`REQUIRE_EQUAL` halt the current test (via `return`) on failure —
use those for a guard check something later in the same test would crash on (e.g. `size() == 1`
before indexing `[0]`), matching what used to be an explicit `if (...) return;` after a failed
`Expect()`.

**Preprocessor gotcha specific to this style:** `IT`/`DESCRIBE` are function-like macros, and
the C preprocessor's macro-argument scanner balances only `(` `)`, not `{` `}` — so a `{a, b}`
brace-init-list or aggregate-init written directly inside an `IT(...)` body (not nested inside
some other call's own parens) splits into extra macro arguments and fails to compile with a
"macro 'IT' passed N arguments" error. Fix by wrapping the literal in an extra pair of parens
(`SomeType({a, b})`) or moving it into a small helper function outside the `DESCRIBE` block —
several existing tests do exactly this (e.g. `tests/ImportResolverTests.cpp`'s `OneImport`/
`SearchPaths` helpers, `tests/ComponentTests.cpp`'s parenthesized constructor call).

Previously split across a hand-rolled `iris_tests` executable (predating Cimmerian being
vendored — each `TestXxx()` called a hand-rolled `Expect(condition, description)`, no
auto-registration) and a separate `iris_cimmerian_tests`; coalesced into this one binary once
every file was migrated to Cimmerian's style (`docs/iris_stage2_decision_doc.md` §7 always
intended Cimmerian as the long-term tool).

## Architecture

- **`include/Iris/` + `src/Iris/`** — the preprocessor front end. Currently just
  `CppTokenizer`, an implementation of `IHostLanguageTokenizer` for C++23 source. Its job is
  narrow and deliberate: understand just enough C++ lexical syntax (identifiers, braces,
  string/char literals with escapes, line/block comments) to detect `render { }` blocks and
  keep `{ }` escape-hatch brace-balancing from desyncing on braces that merely *look* like
  real braces inside a string, char literal, or comment. It does not tokenize the full C++
  grammar — numbers, operators, and everything else fall through as `TokenKind::Other`;
  validating that is the host C++ compiler's job on generated output, never Iris's.
- **`IHostLanguageTokenizer`** is the abstraction point for adding a second host language
  later (Nyx/`.irisx`): one concrete tokenizer implementation per host language, selected by
  file extension at preprocessor startup. Nothing else in the preprocessor core should carry
  host-language-specific lexical rules.
- **This repo has no dependency on Penumbra, or on any other backend.** That's deliberate, not
  an oversight — Iris's core (preprocessor + IR + runtime library) is backend-agnostic by
  design (`docs/iris_core_spec.md` §2.5), and is meant to support more than one backend over
  time (Penumbra now, an Umbra Engine/Nyx backend deferred to Stage 6). Earlier, this repo
  vendored `penumbra-proto` as a git submodule directly — that was corrected: a project meant
  to stay backend-agnostic shouldn't have to pull in one specific backend's whole build just to
  compile its own preprocessor. The Stage 2 "Penumbra backend" — the code that walks
  `Component` IR and calls Penumbra's fluent `Builder` API (`Box::Builder`,
  `Label::Builder`, etc.) — lives in a separate sibling repo, `iris-penumbra-backend`
  (`../iris-penumbra-backend`), which vendors both this repo and `penumbra-proto` as
  submodules. Neither Iris nor Penumbra depends on the other, or on that bridge repo; real
  consumer projects depend on the bridge repo (and transitively get both).
- **`libs/amanuensis`** (git submodule, `github.com/DeanWilsonDev/amanuensis`) is a
  vendored dependency — a zero-dependency first-party JSON library, used by `IrisConfig` to
  parse `.iris.json`. This doesn't conflict with the backend-agnostic rule above: it's a plain
  utility library with no knowledge of Iris, Penumbra, or any backend, added via its own
  documented `add_subdirectory` integration path rather than hand-rolling a JSON parser.
- **`libs/umbra-interfaces`** (git submodule, `github.com/DeanWilsonDev/umbra-interfaces`) is
  the other vendored dependency — `Umbra::IWidget`/`IrisPropDiff` (Stage 3's reconciler-facing
  update contract) and `Umbra::IWidgetLifecycle`/`TickInfo`. Same non-conflict reasoning as
  Amanuensis: header-only, zero dependencies, names no concrete runtime (Iris) or backend
  (Penumbra) anywhere in it — a shared vocabulary a runtime and a backend adapter both build
  against without depending on each other's headers. `iris::TextureHandle` is now a plain alias
  for `Umbra::TextureHandle` from this package.
- **`.iris.json`** (project root) declares the compile target (`"target": "penumbra"`) and
  module `searchPaths` for `import` resolution. This is project-level, not per-file — a
  project is either a Penumbra tool or an Umbra Engine game UI, never both.
- **`Component`** (`docs/iris_core_spec.md` §2.5) is the backend-agnostic IR that sits
  between the parsed component tree and any backend's codegen. It carries no Penumbra (or any
  other backend) type anywhere in this repo — a backend-mapping pass in the relevant bridge
  repo (`iris-penumbra-backend` for Penumbra) is what turns it into real widgets. The runtime's
  live-widget map is keyed by element identity (`key`, or a generated position id) and stores
  `IWidget*` — a backend-agnostic interface — not a concrete Penumbra type, even at the
  runtime layer.

### Project phasing

The docs track a staged roadmap (`docs/iris_handoff.md` §6 has the full table): Stage 0
(language spec, done), Stage 1 (this preprocessor front end — the tokenizer above is part of
it), Stage 2 (Penumbra backend, static widget tree, no reactivity), Stage 3 (reactive runtime
— state, reconciler, `<Slot>`-scoped diffing, `iris::Tick()`), Stage 4 (Lustre-lite styling),
Stage 5 (first real consumer), Stage 6 (deferred: Umbra Engine/Nyx backend). Check which stage
a task belongs to before assuming a later stage's concepts (reconciler, styling, lifecycle
hooks) already exist in code — most of the architecture described in the docs is still design,
not implementation.

Resolved as of `penumbra-proto` commit `663fece`: Penumbra now has an `IWidgetLifecycle`
interface (`OnMount`/`OnUnmount`/`OnTick`) and an `Application` host dispatching `OnTick`,
matching what `docs/iris_core_spec.md` §10 / `docs/iris_stage3_decision_doc.md` §8 specified —
this was the last known Penumbra-side blocker for Stage 3 lifecycle work.

Stage 2 (the Penumbra backend itself: `IrisPenumbraBackend::BuildWidgetTree()`, walking a single
`Component` node and building the equivalent real Penumbra widget tree via each Core
primitive's own fluent `Builder`) is implemented in `iris-penumbra-backend`, not here — this
repo only ever produces the backend-agnostic `Component` IR. It's a one-shot static build —
`<Slot>` contributes nothing during it (same as `IrisElementTag::None`); `iris::ResolveSlots`
(below) splices real content in afterward.

Stage 3's core engine (`iris::Signal<T>`, ambient dependency tracking, batching, `iris::Tick()`,
the reconciler) is implemented here — see `docs/iris_stage3_implementation_decision.md`. `key`
now does reach `Component` (`Component::Key`, set via a small IIFE `Codegen.h` wraps
around any keyed element's base expression) — the reconciler's `Umbra::IWidget`-based
`key`→live-widget matching is real and tested. A real Penumbra `IWidget` adapter is also
implemented (in `iris-penumbra-backend`) and tested against actual `Penumbra::Widgets::Box`/
`Label` objects, not just a mock.

**State declaration uses `IRIS_SIGNAL(Type, Name, InitExpr)`, not a direct
`iris::Signal<T> Name = InitExpr;` declaration.** The direct form was the original spec syntax
and is unsound C++: a `<Slot>` callable capturing that local `[&]` (every spec example's own
pattern) becomes a dangling reference the instant the declaring component function returns,
which it always does immediately — confirmed with AddressSanitizer, not a corner case. Fixed
per `docs/iris_signal_lifetime_decision.md`: the macro binds `Name` to a reference into a
heap-allocated `iris::ComponentInstance` tied to that component's own mounted lifetime, so
`[&]` capture stays exactly as safe as every example assumes.

**`<Slot>` is now wired into the Stage 2 walker, for both callable shapes**
(`docs/iris_slot_stage2_wiring_decision.md`, `docs/iris_slot_list_wiring_decision.md`):
`iris::ResolveSlots()` (`include/Iris/SlotResolution.h`) walks a just-built static widget tree
and its source `Component` tree in lockstep, constructs a `SlotState` for each `<Slot>`
found, and attaches it to its exact position (`SlotState::AttachToGroup`) — every subsequent
`Reconcile()`, including ones `iris::Tick()` triggers automatically, updates that real position
in place. A `SlotSiblingGroup` shared by every `<Slot>` sibling under the same static parent
recomputes each slot's absolute index fresh on every reconcile, so a list-returning `<Slot>`'s
subsequent siblings shift correctly as its own length changes across re-renders. Verified
against real Penumbra `Box`/`Label` objects, not just a mock: a live `iris::Signal` update
reaching a real `Box::Children` vector end to end, and under AddressSanitizer (which caught and
led to a fix for a real destruction-order use-after-free among sibling `<Slot>`s).

**Nested `<Slot>` discovery is also done** (`docs/iris_nested_slot_discovery_decision.md`): a
`<Slot>` nested inside another `<Slot>`'s own dynamically-produced output — the common case of
rendering a child component whose own `render { }` body contains its own `<Slot>` — now gets
found and given its own independent `SlotState`, reacting to its own signals without the outer
`<Slot>` needing to re-render. `SlotState::NestedSlots_` is rebuilt from scratch (via the same
`ResolveSlots()` walk above) on every `Reconcile()` call of the slot that contains it, mount and
re-render alike — simpler and safer than trying to persist unchanged nested slots across a
parent re-render, at the cost of unnecessarily re-running an unaffected nested `<Slot>`'s own
callable whenever its outer parent re-renders for an unrelated reason. Still open: avoiding that
rediscovery when the underlying subtree was reused unchanged, and a `<Slot>`'s own list output
containing a *bare* `<Slot>` entry directly (rather than nested inside an ordinary wrapper
element).

**List diffing is now LIS-based and move-count-optimal**
(`docs/iris_lis_list_diff_decision.md`): the reconciler's list diff always reused the correct
widget objects, but previously always removed and reinserted every list entry regardless of
whether it needed to move — the one item every Stage 3 decision doc had flagged as deliberately
deferred. `Reconciler.cpp` now has `ReconcileChildrenAt`, a live-widget counterpart to the
existing plain-vector `ReconcileChildren`, which computes the longest increasing subsequence of
matched old positions and leaves those untouched structurally (only prop/child updates run on
them, via a new `ReconcileMatchedInPlace`) — every other position gets exactly one
`RemoveChildAt`/`InsertChildAt`, the minimum possible. Both `ReconcileWidget`'s own
child-recursion and `SlotState::Reconcile`'s attached-parent branches (`SlotRuntime.cpp`) now go
through it instead of their old hand-rolled remove-all/insert-all sequences. Verified with new
tests asserting actual mutation call counts, not just end-state correctness; full suite clean
under AddressSanitizer + UndefinedBehaviorSanitizer. This closes the last open item from Stage
3's original decision doc.
