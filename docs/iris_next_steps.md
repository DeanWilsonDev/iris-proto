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
- **Stage 3 (reactive runtime)** — **done, including both items this doc used to list as
  remaining.** `Signal<T>`, ambient dependency tracking, batching, `iris::Tick()`, and the
  reconciler (prop diffing, same-tag-key matching, keyed list diffing, now LIS-based
  minimal-move — see below) are implemented and tested against a mock `Umbra::IWidget` — see
  `docs/iris_stage3_implementation_decision.md`. A real Penumbra `IWidget` adapter
  (`iris-penumbra-backend`'s `PenumbraWidget`) and `<Slot>` wiring into the Stage 2 walker
  (`iris::ResolveSlots`, both the single-`IrisComponent`- and list-returning callable shapes,
  plus nested `<Slot>` discovery within a `<Slot>`'s own dynamically-produced output) are also
  done and verified against real `Penumbra::Widgets::Box`/`Label` objects under
  AddressSanitizer — see the "Done" sections below. Three real gaps the decision docs left open
  got resolved along the way: `key` never actually reached `IrisComponent` (fixed — see below),
  no mechanism was ever specified for how a signal knows which `<Slot>`s to mark dirty (ambient
  "active slot" tracking, the user's explicit choice), and `IWidget`/`IrisPropDiff` were said to
  belong in a not-yet-existing `umbra-interfaces` package that conflicted with this repo's
  zero-Penumbra-dependency rule (that package now exists for real — see below).
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
real Penumbra `IWidget` adapter (nothing implemented `Umbra::IWidget` for a real
`Penumbra::Widgets::WidgetBase` yet), wiring `SlotState` into the Stage 2 walker (which still
asserted on encountering `<Slot>` — it was built and tested before `<Slot>` resolution
existed), nested-`<Slot>` discovery within an arbitrary tree, and LIS-based minimal-move list
diffing (the current list diff is correct — matched widgets are always reused — but not
move-count-optimal). **The first two are now done — see below.**

## Done: the Penumbra `Umbra::IWidget` adapter, in `iris-penumbra-backend`

`PenumbraWidget` wraps a real `Penumbra::Widgets::WidgetBase` to satisfy `Umbra::IWidget`,
verified against real `Box`/`Label` objects (not a mock) — a same-tag-same-key update reuses
the literal same `Box*` address, a structural add actually grows the real `Box::Children`
vector. Full writeup: `iris-penumbra-backend/docs/iris_penumbra_backend_adapter_decision.md`.
Also fixed along the way: `iris::Signal<T>`'s constructor was `explicit`, silently rejecting
the exact copy-initialization syntax (`iris::Signal<bool> settingsOpen = false;`) every example
in `docs/iris_core_spec.md` §2.2 uses — caught only once a real generated `.iris` file was
compiled against it, now fixed with a regression test.

## Done: fixed the `<Slot>`/`Signal` dangling-reference bug

Full writeup: `docs/iris_signal_lifetime_decision.md`. Summary: `iris::Signal<T> Name = Init;`
declared directly (every spec example's original syntax) put `Name` on the stack — `[&]`
capturing it into a `<Slot>` lambda was confirmed (AddressSanitizer) dangling-reference UB the
instant the declaring component function returned, which it always does immediately
(`return <expr>;`, Stage 1's own codegen convention). Not a bug in this session's `Signal`/
`SlotState`/reconciler work — that faithfully implemented what `docs/iris_stage3_decision_doc.md`
§0 specified; the spec's own claim of "genuinely persistent for free" didn't hold for an
ordinary C++ function returning by value.

Fixed by `IRIS_SIGNAL(Type, Name, InitExpr)` (`include/Iris/ComponentInstance.h`) — an ordinary
C++ macro expanding to a *reference* bound to heap-allocated storage, owned by a new
`iris::ComponentInstance` tied to that specific mounted component's lifetime (freed
automatically via the `shared_ptr` refcounting `SlotState` already does for diffing purposes —
no new "on unmount" hook needed anywhere). `[&]` capture keeps meaning exactly what every spec
example already writes, because a reference-typed variable captured by reference aliases its
heap-stable referent directly — no coroutines, no capture-list syntax changes anywhere except
the one declaration itself. `Codegen.h` now wraps every component invocation it emits in
`iris::MountComponentInstance(...)`, the same category of change as the existing `key`-setting
IIFE wrapping. Re-verified end to end under AddressSanitizer with zero errors: real `.iris` →
`iris_cc` → `IRIS_SIGNAL` → `iris::Mount` → real Penumbra widget.

One required, mechanical syntax change for every component author: `iris::Signal<T> Name = v;`
→ `IRIS_SIGNAL(T, Name, v)`. Every code sample in `docs/iris_core_spec.md` has been updated;
historical decision-record docs were deliberately left showing the old syntax.

## Done: wired `<Slot>` into the Stage 2 walker

Full writeup: `docs/iris_slot_stage2_wiring_decision.md`. Summary: `BuildWidgetTree` now
treats `IrisElementTag::Slot` exactly like `None` during the static build (contributes
nothing, no more assert) — a new `iris::ResolveSlots(Widget, Node, Mount)`
(`include/Iris/SlotResolution.h`) then walks the just-built widget tree and the original
`IrisComponent` tree in lockstep, and for each `<Slot>` found, constructs a `SlotState`,
tells it exactly where it lives (originally `SlotState::AttachAt(Parent, Index)`, a new
"attached" mode alongside the existing standalone one — since generalized to
`AttachToGroup`, see the list-wiring paragraph below), and performs its initial mount —
splicing its render into the parent's real children at that position. Every subsequent
`Reconcile()` (including ones `iris::Tick()` triggers automatically) updates that same
real position in place, via `ReconcileWidget` completely unchanged — it never knew or
cared where its `unique_ptr<Umbra::IWidget>&` actually lived. `ResolveSlots`/`SlotState`'s
attachment API are pure `Umbra::IWidget` — entirely backend-agnostic, living in `iris`'s
own runtime; the only change needed in `iris-penumbra-backend` was a one-line change to
`BuildWidgetTree`'s own `Slot` case.

Verified against a mock (`tests/SlotResolutionTests.cpp`) and, more importantly, against
**real** `Penumbra::Widgets::Box`/`Label` objects
(`iris-penumbra-backend/tests/SlotWiringTests.cpp`) — a live `iris::Signal` update
reaching all the way through `IrisRuntime`/`iris::Tick()`/`SlotState`/the reconciler/
`PenumbraWidget` to a real Penumbra `Box::Children` vector, confirmed by inspecting that
vector directly.

**List-returning `<Slot>` wiring is now also closed** (`docs/iris_slot_list_wiring_
decision.md`): a `SlotSiblingGroup`, shared by every `<Slot>` sibling under the same
static parent, recomputes each slot's absolute position fresh on every reconcile by
summing every earlier sibling's *current* real child count — so both a list-returning
`<Slot>`'s own growth/shrinkage and a sibling `<Slot>` toggling to/from `None` correctly
shift whatever comes after them. Verified against real Penumbra widgets and clean under
AddressSanitizer + UndefinedBehaviorSanitizer, including a real destruction-order
use-after-free ASan caught and that decision doc's fix for it (`SlotSiblingGroup::
MarkDestroyed`).

**Deliberately deferred, not solved here:** nested-`<Slot>` discovery (unchanged from
before — `ResolveSlots` only walks the *static* tree, never a `<Slot>`'s own
dynamically-produced output).

## Done: vendored Cimmerian, the ecosystem's own test framework

`libs/cimmerian` (git submodule, `github.com/DeanWilsonDev/cimmerian`), pinned at its latest
upstream commit. `CMakeLists.txt` forces `CIMMERIAN_VISUAL_PLATFORM=None` (a cache variable
set *before* `add_subdirectory(libs/cimmerian)`, since Cimmerian's own `CMakeLists.txt` only
sets it if it isn't already set) to avoid the default `X11`/`libXtst` dependency its
screenshot/visual-regression testing needs — nothing Iris's own tests need.

A new `iris_cimmerian_tests` executable (`tests/cimmerian/`) uses Cimmerian's `DESCRIBE`/`IT`/
`ASSERT_EQUAL` BDD-style macros with auto-registration and its own `TestRunner::RunAll()` entry
point (`tests/cimmerian/CimmerianTestMain.cpp`, just `#include <cimmerian/test-entry-point.hpp>`).
~~Kept as a *separate* binary from `iris_tests` rather than merged in: Cimmerian's
auto-registration model doesn't mesh with `iris_tests`' existing hand-rolled
`RunXTests()`-called-from-one-`main()` pattern, and migrating ~13 existing test files wasn't
in scope.~~ **Later coalesced**: every hand-rolled `iris_tests` file was migrated to Cimmerian's
`DESCRIBE`/`IT` style and merged into one `test_iris` executable (`tests/TestMain.cpp` is the
renamed, relocated entry point; `tests/cimmerian/` no longer exists as a separate directory —
see CLAUDE.md's "Build and test" section). This is the intended tool for new tests going
forward (`docs/iris_stage2_decision_doc.md` §7).

First test file, `tests/cimmerian/SlotSiblingGroupTests.cpp` (now `tests/SlotSiblingGroupTests.cpp`):
three tests adding coverage for the list-`<Slot>` wiring work not present in
`tests/SlotResolutionTests.cpp` — a three-sibling mix of list- and single-returning `<Slot>`s,
and an explicit regression test for the forward-order sibling-teardown use-after-free
AddressSanitizer caught during that work. Clean under AddressSanitizer + UndefinedBehaviorSanitizer.

## Done: nested `<Slot>` discovery

Full writeup: `docs/iris_nested_slot_discovery_decision.md`. Summary: a `<Slot>` nested inside
another `<Slot>`'s own dynamically-produced output (the common case of rendering a child
component whose own `render { }` body contains its own `<Slot>`) now gets found and given its
own independent `SlotState`, reacting to its own signals without the outer `<Slot>` needing to
re-render. `Reconciler.cpp`'s `ReconcileWidget`/`ReconcileList` were fixed to filter `Slot`-tagged
children out of ordinary index-aligned child diffing first (`FilterOrdinary`) — a `<Slot>` child
contributes zero real widgets, same convention the static-tree walker already used, and a naive
diff would otherwise corrupt the real tree at that position. `SlotState::NestedSlots_` is then
rebuilt from scratch (re-running `ResolveSlots` against the just-reconciled dynamic output) on
every `Reconcile()` call, mount and re-render alike — simpler and safer than persisting a nested
slot unchanged across a parent re-render, at the cost of an unnecessary rediscovery/re-render
whenever the outer `<Slot>` re-renders for an unrelated reason. Verified against real Penumbra
widgets and clean under AddressSanitizer + UndefinedBehaviorSanitizer, including a real
destruction-order use-after-free ASan caught (`~SlotState()` now clears `NestedSlots_` before its
existing detach logic runs) and a regression test for it.

**Deliberately still deferred, not solved here:** a bare `<Slot>` as a raw list item (a
list-returning `<Slot>` callable whose own list contains a `Slot`-tagged entry directly, not
wrapped in an ordinary element) — real, separate, narrower gap; and rediscovery-avoidance for a
nested `<Slot>` living under a widget that was reused unchanged (purely a performance concern).

## Done: LIS-based minimal-move list diffing

Full writeup: `docs/iris_lis_list_diff_decision.md`. Summary: closes the one item every Stage 3
decision doc since `docs/iris_stage3_implementation_decision.md` had flagged as deliberately
deferred — the list diff reused the right widget objects (correctness) but always removed and
reinserted every list entry, matched or not. `Reconciler.cpp` gained `ReconcileChildrenAt`
(`Reconciler.h`), the live-widget counterpart to the existing plain-vector `ReconcileChildren`:
it computes the longest increasing subsequence of matched old positions (`ComputeKeepInPlace`,
O(n log n) patience sorting) and leaves those entries completely untouched structurally — only
`ApplyPropDiff`/child-recursion runs on them, via a new `ReconcileMatchedInPlace(Umbra::IWidget&,
...)` that updates a matched pair in place without needing ownership. Every other position gets
exactly one `RemoveChildAt`/`InsertChildAt` (a real move, or a fresh mount) — the minimum
possible given `Umbra::IWidget`'s deliberately move-less API. `ReconcileWidget`'s own
child-recursion and both of `SlotState::Reconcile`'s `AttachedParent_` branches
(`SlotRuntime.cpp`) now call this instead of their old hand-rolled remove-all/insert-all
sequences — the single-output attached case collapsed into a 0-or-1-element list reconciliation
against the same function, deleting bespoke code rather than adding a parallel path. Verified
with new `tests/ReconcilerTests.cpp` cases asserting actual `RemoveChildAt`/`InsertChildAt` call
counts (not just end-state correctness) — appending touches nothing old, a single out-of-order
move among four stable siblings costs exactly one remove + one insert, a no-op reconcile costs
zero, and a non-zero base index (a `<Slot>` after a static sibling) leaves everything before it
alone. Full suite (115 tests) clean under AddressSanitizer + UndefinedBehaviorSanitizer.

## Suggested order

Both items this section used to list are now done (see above) — Stage 3 has no known open gaps
left. What's actually left:

1. **Stage 4 (Lustre)** — needs its own design pass first, nothing to implement yet.
2. **Stage 5** — validate against one of the real consuming projects now that Stage 3's
   reconciler is both correct and move-count-optimal, and an actual `.iris` file can round-trip
   through, mount, and reconcile for real, arbitrarily-nested `<Slot>`s included.
