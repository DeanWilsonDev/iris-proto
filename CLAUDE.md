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
./build/tests/iris_tests
```

There is no test framework wired in yet. `tests/CppTokenizerTests.cpp` is a plain executable:
each `TestXxx()` function calls a hand-rolled `Expect(condition, description)`, prints
`[PASS]`/`[FAIL]` per assertion, and the binary exits non-zero if any assertion failed. To add
a test, add a new `TestXxx()` function and call it from `main()` — there's no auto-registration.
Cimmerian (the ecosystem's own test framework) is the intended long-term tool per
`docs/iris_stage2_decision_doc.md` §7, but isn't vendored yet.

To run a single check, either comment out the other `TestXxx()` calls in `main()` temporarily,
or grep the printed `[PASS]`/`[FAIL]` lines — there's no test filtering flag.

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
  `IrisComponent` IR and calls Penumbra's fluent `Builder` API (`Box::Builder`,
  `Label::Builder`, etc.) — lives in a separate sibling repo, `iris-penumbra-backend`
  (`../iris-penumbra-backend`), which vendors both this repo and `penumbra-proto` as
  submodules. Neither Iris nor Penumbra depends on the other, or on that bridge repo; real
  consumer projects depend on the bridge repo (and transitively get both).
- **`.iris.json`** (project root) declares the compile target (`"target": "penumbra"`) and
  module `searchPaths` for `import` resolution. This is project-level, not per-file — a
  project is either a Penumbra tool or an Umbra Engine game UI, never both.
- **`IrisComponent`** (`docs/iris_core_spec.md` §2.5) is the backend-agnostic IR that sits
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

Known open gap as of Stage 3 scoping: Penumbra lacks an `IWidgetLifecycle` interface needed for
`OnMount`/`OnUnmount`/`OnTick` hooks (`docs/iris_core_spec.md` §10) — this affects
`penumbra-proto` and the `iris-penumbra-backend` bridge repo, not this repo, but blocks Stage 3
lifecycle work until resolved there.

Stage 2 (the Penumbra backend itself: tree-builder, widget mapping, the `key`→`IWidget*`
identity map) is implemented in `iris-penumbra-backend`, not here — this repo only ever
produces the backend-agnostic `IrisComponent` IR.
