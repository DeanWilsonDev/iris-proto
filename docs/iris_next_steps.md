# Iris — Next Steps

> Snapshot as of 2026-07-19. Reflects the corrected three-repo architecture
> (`docs/iris_stage2_decision_doc.md`'s correction note): `iris` (this repo, standalone,
> backend-agnostic), `penumbra-proto` (standalone, no Iris knowledge), and
> `iris-penumbra-backend` (vendors both, owns the Stage 2/3 backend-mapping code).

## Where things stand

- **Stage 0 (spec)** — done. `docs/iris_core_spec.md` v2 is authoritative.
- **Stage 1 (preprocessor front end)** — done, end to end (tokenizer through a working CLI):
  - `CppTokenizer` (`IHostLanguageTokenizer` for C++23) — done.
  - `RenderBlockParser` (`render{ }` → `ElementNode` AST: tags, props, `key`/`class`, nested
    elements, `{ }` escape hatches, literal text, comment stripping, single-root enforcement) —
    done, tested against the spec §9 worked example end-to-end.
  - Codegen (`ElementNode` AST → compilable `.cpp`) — **done, including nested JSX inside
    `<Slot>`/escape hatches**. `docs/iris_props_decision.md` (the `IrisProps`/`IrisPropValue`
    runtime shape), `docs/iris_stage1_codegen_decision.md` (two follow-on gaps that decision
    left open), and `docs/iris_escape_hatch_decision.md` (the `!{ }` JSX-transform escape
    hatch — see below) are all closed and implemented. `Codegen.h`/`GenerateComponentExpression()`
    walks an `ElementNode` and emits a C++23 expression constructing `Iris::IrisComponent` —
    Core primitives (including `<Slot>` via `Iris::MakeSlotCallable`), the `<Name>Props`
    component-invocation convention, text/interpolation-child concatenation, the closed
    prop-name lookup table, and `!{ }`-transformed nested JSX are all tested
    (`tests/CodegenTests.cpp`). The full spec §9 `PartyScreen` example, written with `!{ }` on
    both `<Slot>`s, was manually verified to generate output that actually host-compiles as
    real C++23, including the `return nullptr;` line — see below, the one gap it surfaced is
    now closed too.
  - `import` / `.iris.json` resolution — **done**. `IrisConfig` (parses `target`/`version`/
    `searchPaths` via the newly-vendored `libs/amanuensis` — a zero-dependency first-party JSON
    library, git-submoduled rather than hand-rolled, see below) and `ImportResolver`
    (`ScanImports` + `ResolveImports`, `.iris`/`.irisx` extension chosen by `target`) both landed
    with tests (`tests/IrisConfigTests.cpp`, `tests/ImportResolverTests.cpp`).
  - Semantic validation (Core-primitive vs. imported-component resolution, backend-gated
    primitive checks, the `<Text font=...>` and inline-style errors) — **done**. See below.
  - Preprocessor driver/CLI — **done**. See below.
- **Stage 2 (Penumbra backend)** — **done**, in the sibling `iris-penumbra-backend` repo (not
  this one). `IrisPenumbraBackend::BuildWidgetTree()` walks a single `IrisComponent` node and
  recursively builds the equivalent real Penumbra widget tree via each Core primitive's own
  fluent `Builder` (`Frame`→`Box`, `Inline`→`InlineContainer`, `Grid`→`Box` stub, `Image`→
  `ImageWidget`, `Text`→`Label`; `Slot` never reaches it, `None` builds to `nullptr` and is
  skipped as a child). One-shot tree build only — no diffing, no identity tracking; `key`
  reaching `IrisComponent` at all (needed for the reconciler's matching rule) landed as part
  of Stage 3, below — this walker still doesn't use it, since it never diffs anything. Verified
  against the full pipeline: a real `.iris` component compiled through this repo's own `iris_cc`,
  `#include`d, called, and the resulting `IrisComponent` fed through `BuildWidgetTree` produced a
  real `Box`/`Label` tree with correct class name, child count, and interpolated text — first
  time output has been traced from `.iris` source all the way to a real Penumbra widget.
  `iris-penumbra-backend`'s vendored `iris` submodule was also bumped from this repo's very
  first commit (which predates `IrisComponent` having its current shape) to current `main`.
- **Stage 3 (reactive runtime)** — **core engine done**; backend integration not yet. `Signal<T>`,
  ambient dependency tracking, batching, `iris::Tick()`, and the reconciler (prop diffing,
  same-tag-key matching, keyed list diffing) are implemented and tested against a mock
  `Umbra::IWidget` — see `docs/iris_stage3_implementation_decision.md`. Three real gaps the
  decision docs left open got resolved along the way: `key` never actually reached
  `IrisComponent` (fixed — see below), no mechanism was ever specified for how a signal knows
  which `<Slot>`s to mark dirty (ambient "active slot" tracking, the user's explicit choice), and
  `IWidget`/`IrisPropDiff` were said to belong in a not-yet-existing `umbra-interfaces` package
  that conflicted with this repo's zero-Penumbra-dependency rule (that package now exists for
  real — see below). Still deliberately deferred: a real Penumbra `IWidget` adapter, wiring
  `SlotState` into the Stage 2 walker (which still asserts on encountering `<Slot>`), and
  discovering/resolving nested `<Slot>`s within an arbitrary tree.
- **Stage 4 (Lustre-lite styling)** — not scoped yet.
- **Stage 5 (first real consumer)** — not started. You mentioned real consuming projects already
  exist, which is why the repo-dependency direction got fixed now rather than later.
- **Stage 6 (Umbra Engine/Nyx backend)** — deferred by design.

## Done: IWidgetLifecycle docs synced

`penumbra-proto` commit `663fece` ("Add IWidgetLifecycle interface and Application lifecycle
host") landed `include/Penumbra/IWidgetLifecycle.h` — `OnMount`/`OnUnmount`/`OnTick(TickInfo)` —
plus an `Application` host that dispatches `OnTick`, exactly matching what
`docs/iris_stage3_decision_doc.md` §8 specified. This closed the one real, verified
Penumbra-side gap that was blocking Stage 3 lifecycle hooks.

`docs/iris_core_spec.md` §10, `docs/iris_handoff.md` §5, `docs/iris_stage3_open_questions.md`,
`docs/iris_stage3_decision_doc.md` §10's checklist, and `CLAUDE.md` have all been updated to
mark this resolved (mirroring how the `<Image>` gap's resolution was documented). Stage 3 now
has every known Penumbra-side prerequisite it needs.

## Done: IrisProps runtime representation and Stage 1 codegen's two follow-on gaps

`docs/iris_props_decision.md` closed `IrisProps`/`IrisPropValue`'s shape (a closed, strongly-typed
`std::variant`, not a type-erased `unordered_map<string, any>`). Writing codegen against it then
surfaced two more gaps neither that document nor `docs/iris_core_spec.md` §2.5 actually covered —
both closed in `docs/iris_stage1_codegen_decision.md`:

1. `<Slot>`'s callable child doesn't fit in `Props` (whose one callable variant member is
   zero-argument, shaped for event handlers) or in `Children` (which holds already-constructed
   `IrisComponent` values, not an unevaluated callable) — resolved by adding a `SlotCallable`
   field to `IrisComponent`, populated via a `Iris::MakeSlotCallable()` helper that defers the
   `IrisComponent` vs. `vector<IrisComponent>` return-type choice to the host compiler.
2. Literal text and `{ }` interpolation as element children have nowhere to go in a shape where
   `Children` only holds `IrisComponent` values — resolved per-primitive: `<Text>` concatenates
   its children into its own `"text"` prop; every other children-accepting primitive (chiefly
   `<Inline>`) wraps a text/escape-hatch child as a synthetic `<Text>` `IrisComponent` node
   appended to `Children` instead.

Both `IrisComponent`'s revised shape (`include/Iris/IrisComponent.h`) and `Codegen.h` are
implemented and tested.

## Resolved and implemented: JSX inside escape hatches (`!{ }` transform escape hatch)

The gap surfaced during codegen testing: `RenderBlockParser` treats `{ }` escape hatch contents
as fully opaque verbatim text (§1.4), but `<Slot>` is used throughout the spec with JSX inside
its escape hatch body — conditional and list rendering both rely on this pattern. That JSX was
never being transformed, producing uncompilable output.

Full decision, implementation notes, and verification writeup are in
`docs/iris_escape_hatch_decision.md`. Summary: a second escape-hatch sigil, `!{ }`, means
"host-language code that may contain JSX — recursively transform it"; the existing `{ }` form is
unchanged and stays fully opaque; nesting (`!{ }` inside `!{ }`, `{ }` inside `!{ }`) composes
normally. Implemented in `RenderBlockParser::ParseJsxEscapeHatch`
(`src/Iris/RenderBlockParser.cpp`) and `Codegen.cpp`'s `EmitEscapeHatchExpression`, tested in
both `tests/RenderBlockParserTests.cpp` and `tests/CodegenTests.cpp` — including an end-to-end
test against the full spec §9 `PartyScreen` example (both `<Slot>`s, two levels of nesting,
`std::vector<IrisComponent>` correctly *not* misread as a JSX element) whose generated output
was manually confirmed to host-compile as real C++23.

One implementation wrinkle worth knowing: `std::vector<IrisComponent>` (a real return type used
in the spec's own list-rendering `<Slot>`) has the exact same `< Identifier >` shape as an
attribute-less JSX opening tag. Disambiguated by requiring whitespace immediately before the
`<` for it to count as a JSX start — true of every JSX use in the spec, never true of a template
argument list. See the decision doc for the one known edge case this doesn't cover
(whitespace-free JSX like `push_back(<Frame/>)`), deliberately deferred since nothing in the
spec needs it.

## Resolved: `IrisComponent` had no `nullptr_t` constructor

Surfaced by manually host-compiling the §9 `PartyScreen` example while verifying `!{ }` above —
`IrisComponent` had no `nullptr_t` constructor, so the spec's own `return nullptr;` inside a
`<Slot>` lambda declared to return `IrisComponent` (§1.5, §9 — every conditional-rendering
example) didn't actually compile as written. An `IrisComponent`-shape gap, not an escape-hatch
one; unrelated to the `!{ }` decision itself.

Fixed with a new `IrisElementTag::None` sentinel (`include/Iris/IrisElementTag.h`) plus an
implicit `IrisComponent(std::nullptr_t)` converting constructor
(`include/Iris/IrisComponent.h`) that produces it — a walker/reconciler must treat a
`None`-tagged node as "unmount whatever was here, mount nothing" and never hand it to a backend
`Builder`. Adding that constructor loses `IrisComponent`'s aggregate-ness, so an explicit
4-field constructor was added alongside it to keep Codegen.h's emitted `IrisComponent{Tag,
Props, Children, SlotCallable}` call shape compiling unchanged.

Also added: `tests/IrisComponentTests.cpp`, a first-of-its-kind test file that host-compiles
`IrisComponent.h` directly — previously nothing did, since Codegen's own tests only ever check
the shape of generated *text*, never compile it, which is exactly how this gap went unnoticed
until a manual compile check found it. Re-running that same manual compile against the full
`PartyScreen` example after this fix now succeeds with no workarounds, `nullptr` line included.
Documented in `docs/iris_core_spec.md` §8 and `docs/iris_escape_hatch_decision.md`'s
Verification section.

## Done: semantic validation pass

`include/Iris/SemanticValidator.h` / `src/Iris/SemanticValidator.cpp` add
`ValidateElementTree(Root, Target, ImportedNames)`, covering the four preprocessor-level
`docs/iris_core_spec.md` §6 error-catalogue entries `Codegen.h` doesn't already handle (or
doesn't handle with the spec's exact wording):

1. **Backend-gated primitive on the wrong target** — `<Model3d>` without
   `"target": "umbra-engine"`.
2. **Inline `style` prop** — on *any* element, Core primitive or component invocation alike.
   Codegen's per-primitive prop tables happen to reject `style` on a primitive too, but with a
   generic "unknown prop" message, and never check a component invocation's props at all (they
   pass straight through to `<Name>Props`'s designated initializers) — this pass is the only
   place `<HealthBar style="...">` gets caught.
3. **`<Text font=...>`** — same gap as `style`, `<Text>`-specific.
4. **Unresolved/unimported component reference** — a tag that's neither a Core primitive nor a
   name the caller says was `import`ed (`ImportedNames`, by name — `ImportResolver`'s own
   file-resolution success/failure is a separate, already-reported error, not re-checked here).

Recurses into every child position, including nested elements found inside a `!{ }`
JSX-transform escape hatch (`docs/iris_escape_hatch_decision.md`) — those are real parsed
elements and get the same checks as anything written at the top level; a plain `{ }` escape
hatch stays untouched, same as everywhere else in the preprocessor.

Pulled the Core-primitive tag-name set out of `Codegen.cpp`'s private anonymous namespace into
a new shared `include/Iris/CorePrimitives.h`/`src/Iris/CorePrimitives.cpp`
(`CorePrimitiveTagNames()`, `BackendGatedPrimitiveTagNames()`) so Codegen and the semantic
validator can't drift out of sync on what counts as a primitive — Codegen.cpp now calls the
shared function instead of keeping its own copy.

Tested end-to-end in `tests/SemanticValidatorTests.cpp`, including the full spec §9
`PartyScreen` example (with `Button`/`HealthBar` correctly `import`ed) validating cleanly, and
a case confirming an unimported tag nested inside a `!{ }` body is still caught.

## Done: preprocessor driver/CLI, and the header-generation decision it surfaced

`include/Iris/Driver.h`'s `Iris::CompileFile(Source, FilePath, Config, ProjectRoot)` is the full
pipeline connecting every Stage 1 piece into an actual `.iris`/`.irisx` → generated-header
transform, and `tools/IrisCc.cpp` wraps it as an `iris_cc` CLI binary
(`iris_cc <input.iris> [-o <output.h>] [--project-root <dir>]`, project root auto-resolved by
walking up from the input file for the nearest `.iris.json`, tsconfig-style).

Per file: `ScanImports`, `RenderBlockParser::Parse()`, then for every parsed block
`ValidateElementTree()` (against `Config.Target` and the scanned import names) and
`GenerateComponentExpression()`. All diagnostics — `ImportResolver`'s unresolved-import errors,
parse errors, semantic errors, codegen errors — are collected into one list; if any are present,
no output is produced at all (matches `CodegenResult`'s existing "empty `Source` whenever
`Errors` is non-empty" convention). On success, each `render { }` block is spliced out of the
original source and replaced with `return <expr>;` (`Codegen.h`'s own documented wrapping
convention), with a `#line` directive inserted after every splice to resync line numbers — a
render block usually spans multiple lines and always collapses to one, so everything after it
would otherwise report at the wrong line to a host-compiler error.

**The `import Name` gap this first version left open is now closed** —
`docs/iris_import_header_decision.md`. Short version: the obvious answer (generate a `.h`/`.cpp`
pair per component, forward-declaring `NameProps`/`Name()`) turns out to be unreachable without
Iris parsing struct/function signatures, which `docs/iris_core_spec.md` §2.1 explicitly rules
out. The actual decision: every `.iris`/`.irisx` file compiles to **one self-contained header**
(`<original-path>.h`, e.g. `Button.iris.h`) rather than a declaration/definition pair —
`#pragma once`, full definition inline in the header, nothing split. `import Name` becomes
`#include "<path-relative-to-ProjectRoot>"`. The tradeoff: a component's function needs `inline`
to stay ODR-safe once included by more than one translation unit — Iris doesn't inject this
(would mean parsing the signature it's committed not to touch); it's a convention the author
applies themselves, documented in the decision doc.

`RenderBlockParser::ParsedBlock` gained an `EndLocation` field (one character past the block's
closing `}`) to make the render-block splicing possible — previously it only exposed where
`render` itself starts, not where the block ends.

Verified three ways: `tests/DriverTests.cpp` (including the full spec §9 `PartyScreen` example
compiling with no diagnostics, `#include`/`#pragma once` shape asserted directly), running
`iris_cc` three times against an on-disk `StartMenu.iris` + `Button.iris`/`SettingsPage.iris`
fixture (each marked `inline` per the convention) to produce three real `.iris.h` files, and
host-compiling a `main.cpp` that only `#include`s the top-level generated header — no manual
glue, no hand-written declarations, confirmed to compile with `g++ -std=c++23`. First time a
*multi-file* Iris project has been shown to actually build end-to-end.

## Done (core engine): Stage 3 reactive runtime

`docs/iris_stage3_implementation_decision.md` has the full writeup. Summary:

- **`iris::Signal<T>`** (`include/Iris/Signal.h`) — `.get()`/`.set()`, ambient "active slot"
  dependency tracking (the user's explicit choice over blanket re-invocation).
- **`iris::SlotState`/`iris::IrisRuntime`** (`include/Iris/SlotRuntime.h`,
  `src/Iris/SlotRuntime.cpp`) — one `<Slot>`'s live reactive state, batching
  (`BeginBatch`/`EndBatch`/`ScopedEventBatch`), dirty-slot tracking, and `iris::Tick()`.
- **The reconciler** (`include/Iris/Reconciler.h`, `src/Iris/Reconciler.cpp`) —
  `ComputePropDiff`, same-tag-key matching (`ReconcileWidget`), keyed list diffing
  (`ReconcileChildren`/`ReconcileList`), all working against `Umbra::IWidget` (a new,
  backend-agnostic interface — see below) rather than any concrete backend type.
- **`umbra-interfaces`** — a real new repo
  ([`github.com/DeanWilsonDev/umbra-interfaces`](https://github.com/DeanWilsonDev/umbra-interfaces)),
  created per explicit direction rather than working around the conflict between "IWidget
  should live in Penumbra temporarily" and this repo's hard zero-backend-dependency rule.
  Vendored into `iris` as `libs/umbra-interfaces`. `IWidget` also gained child-management
  methods (`GetChildCount`/`GetChildAt`/`InsertChildAt`/`RemoveChildAt`) it didn't have before —
  needed for the reconciler's "recurse into children" rule, mirrored against Penumbra's own
  `Box` methods.
- **`key` now actually reaches `IrisComponent`** — it didn't before (`Codegen` dropped it
  entirely). `Emit()` wraps any keyed element's base expression in a small IIFE that sets
  `.Key` afterward, uniformly across primitives and component invocations alike. Verified
  end-to-end: a real `.iris` file with `key={props.id}` compiled through `iris_cc` and
  host-compiled, confirming the runtime value round-trips correctly.

Tested thoroughly against a mock `Umbra::IWidget` (`tests/ReconcilerTests.cpp`,
`tests/SlotRuntimeTests.cpp`) — 27 new tests, including keyed-list reordering preserving
widget identity, tag-mismatch remounts actually destroying the old widget, nested-children
recursion through a matched parent, and batching collapsing multiple `set()` calls into one
reconcile.

**Not part of this pass** (see the decision doc's "What remains deliberately deferred"): a
real Penumbra `IWidget` adapter (nothing implements `Umbra::IWidget` for a real
`Penumbra::Widgets::WidgetBase` yet), wiring `SlotState` into the Stage 2 walker (which still
asserts on encountering `<Slot>` — it was built and tested before `<Slot>` resolution
existed), nested-`<Slot>` discovery within an arbitrary tree, and LIS-based minimal-move list
diffing (the current list diff is correct — matched widgets are always reused — but not
move-count-optimal).

## Done: the Penumbra `Umbra::IWidget` adapter, in `iris-penumbra-backend`

`PenumbraWidget` wraps a real `Penumbra::Widgets::WidgetBase` to satisfy `Umbra::IWidget`,
verified against real `Box`/`Label` objects (not a mock) — a same-tag-same-key update reuses
the literal same `Box*` address, a structural add actually grows the real `Box::Children`
vector. Full writeup: `iris-penumbra-backend/docs/iris_penumbra_backend_adapter_decision.md`.
Also fixed along the way: `iris::Signal<T>`'s constructor was `explicit`, silently rejecting
the exact copy-initialization syntax (`iris::Signal<bool> settingsOpen = false;`) every example
in `docs/iris_core_spec.md` §2.2 uses — caught only once a real generated `.iris` file was
compiled against it, now fixed with a regression test.

## Critical, newly discovered: `<Slot>` capturing a `Signal` by reference is dangling-reference UB

Found while verifying the adapter against a genuine `.iris` → `iris_cc` → real-`Signal`-driven
pipeline (not just hand-constructed `IrisComponent` trees) — confirmed with AddressSanitizer,
not a fluke:

```cpp
IrisComponent Counter() {
    iris::Signal<int> Count = 0;
    render {
        <Slot>!{[&]() -> IrisComponent { return <Text>{std::to_string(Count.get())}</Text>; }}</Slot>
    }
}
```

`docs/iris_stage3_decision_doc.md` §0 asserts `iris::Signal<T>` locals are "true long-lived
locals... captured by reference in render lambdas" — but as currently specified *and*
implemented, `Count` is an ordinary stack-local `int`-holding object. The component function
runs exactly once and **returns** (per its own documented contract, and per how Stage 1 codegen
actually rewrites `render { }` into `return <expr>;`) — the instant it does, `Count`'s stack
storage is gone. Every `[&]`-capturing `<Slot>` lambda in every example in the spec holds a
dangling reference from that point on. This isn't a corner case; it's the load-bearing pattern
the entire reactive model is built around.

**This is a Stage 3 foundational-design gap, not an implementation bug in this session's
`Signal`/`SlotState`/reconciler work** — those faithfully implement what
`docs/iris_stage3_decision_doc.md` §0 specifies; the spec's own claim of "genuinely persistent
for free" doesn't hold for an ordinary C++ function returning by value. Fixing it needs a real
design decision — candidates, none picked yet:

- **Heap-allocate `Signal<T>`'s storage** so a `Signal<T>` local is really just a thin
  handle/proxy over independently-owned heap state — the `[&]` capture would then be safe
  because it doesn't actually reference stack memory, even after the declaring function returns.
- **Keep the component function's activation frame alive** via C++20 coroutines (`co_return` at
  the `render { }` boundary instead of an ordinary `return`) — a much larger change touching
  Codegen's `return <expr>;` wrapping convention (`docs/iris_import_header_decision.md`'s
  neighbor, Codegen.h's own documented convention) and every component function's declared
  signature.
- Something else not yet considered.

Every `.iris` example that's been host-compiled and run *with a live signal update* so far in
this project's history has actually hit this — the `HealthBar`/`StartMenu` end-to-end checks
earlier in this doc never exercised a signal `.set()` after the declaring function returned, so
they never triggered it. This blocks Stage 3 from being usable for real, ahead of any of the
items below.

## Suggested order

Starting from what's actually left:

1. **Decide and fix the `Signal<T>` lifetime gap above** — blocks Stage 3 from being usable for
   real; needs a real design decision, not a quick patch.
2. **Wiring `<Slot>` into the Stage 2 walker** — nothing yet constructs a `SlotState` when
   `BuildWidgetTree` encounters a `<Slot>` tag; it still asserts on one. The `Umbra::IWidget`
   adapter that would receive that wiring is now ready; the wiring itself isn't done.
3. **Nested `<Slot>` discovery** — finding and giving each nested `<Slot>` its own `SlotState`
   within an arbitrary tree, rather than assuming a slot's own output is always already fully
   resolved.
4. **LIS-based minimal-move list diffing** — an optimization on top of the current
   correct-but-not-optimal list diff, once real-world move patterns make the extra
   `RemoveChildAt`/`InsertChildAt` traffic worth avoiding.
5. **Stage 4 (Lustre)** — needs its own design pass first, nothing to implement yet.
6. **Stage 5** — validate against one of the real consuming projects once (1)–(4) produce
   something an actual `.iris` file can round-trip through, mount, and reconcile for real.
