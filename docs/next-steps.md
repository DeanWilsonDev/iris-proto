# Iris — Next Steps

Running log of active feature requests / capability gaps for this repo, per
`~/.claude/skills/feature-request`. Each entry below is appended, never edited in place once
resolved — resolved entries get a `RESOLVED` status line and stay for the record instead of
being deleted. For the full historical status log of Stage 0–3 work (all done, effectively a
changelog), see `docs/archive/iris_next_steps.md`; for closed design decisions, see the various
`docs/iris_*_decision*.md` files — `CLAUDE.md` explains how those relate to
`docs/iris_core_spec.md`, the authoritative language reference.

This file coalesces two previously separate requirement docs
(`lustre_hotreload_iris_requirements.md`, `penumbra_iris_lustre_componentization_gaps_requirements.md`,
both since removed) plus one gap identified directly against `docs/iris_core_spec.md`.

---

## `NyxTokenizer` (IHostLanguageTokenizer for `.irisx`) — RESOLVED for the compile path; `@signal` authoring still open (2026-08-05)

> **Status:** The tokenizer adapter landed, and — as a same-day follow-up, prompted by
> Pharos-proto wanting to get ahead of a not-yet-ready Nyx integration — so did wiring it into
> the actual `.irisx` compile path (`RenderBlockParser`/`ImportResolver`/`Driver::CompileFile`
> now genuinely dispatch by file extension). What's still open is unrelated to tokenizing:
> `@signal`/`<Slot>` authoring inside a `.irisx` component body has no Nyx-side mechanism yet,
> tracked below, plus `iris-lsp`'s `NyxLspProxy` seam (LSP tooling, not the compile path) was
> not touched by this pass.
> **Trigger:** nyx-proto's `docs/nyx-scripting-language/decision-log.md` §7.1 (that repo's own
> Phase 7 — originally scoped as a full Chaos preprocessor rewrite inside nyx-proto — was moved
> here, then narrowed to "just the tokenizer adapter" once it became clear this repo's own
> `render{}`/IR/`<Slot>`/reconciler pipeline already covers everything else host-agnostically).

### What landed

- `Iris::NyxTokenizer` (`include/Iris/NyxTokenizer.h`, `src/Iris/NyxTokenizer.cpp`) — a real
  `IHostLanguageTokenizer` implementation wrapping nyx-proto's own `nyx::Lexer`, translating its
  richer `nyx::TokenKind` down into this repo's coarse 8-variant `Iris::TokenKind`, mirroring
  `CppTokenizer`'s own shape exactly. Every Nyx keyword (`Class`, `Import`, `If`, …) collapses to
  `Identifier` alongside plain identifiers — matching `CppTokenizer`'s uniform treatment of C++
  keywords, since `RenderBlockParser`/`ImportResolver` only ever match on lexeme text. Both
  `StringLiteral` and `TemplateStringLiteral` collapse to `Iris::TokenKind::StringLiteral` — Nyx's
  lexer already captures a whole backtick-to-backtick template string (including any `{`/`}`
  inside a `${ }` interpolation) as one verbatim token, so no interpolation-aware sub-tokenization
  was needed for correct brace balancing.
- `libs/nyx-proto` added as a git submodule (`github.com/DeanWilsonDev/nyx-proto`, private).
  Deliberately **not** consumed via `add_subdirectory` + nyx-proto's own `CMakeLists.txt` — that
  would pull in nyx-proto's own vendored Firefly/Amanuensis/Cimmerian submodules a second time,
  colliding with this repo's own `libs/amanuensis`/`libs/cimmerian` targets. Instead, a new
  `nyx-lexer` CMake target compiles only `src/lexer/lexer.cpp`/`token.cpp` directly (verified by
  inspection to have zero dependency on the rest of nyx-proto — no Firefly, no
  runtime/interpreter code), built at C++26 (nyx-proto's own required standard) independently of
  this project's C++23, and linked `PRIVATE` into `iris` (`NyxTokenizer.h` exposes no `nyx::`
  types publicly, so nothing about this dependency leaks to consumers of `iris`).
- Covered by 9 new Cimmerian tests (`tests/NyxTokenizerTests.cpp`), mirroring
  `CppTokenizerTests.cpp`'s cases plus a Nyx-specific one for template-string `${ }` interpolation
  brace-balancing. Full `test_iris` suite (147/147) passes.
- Two documented, deliberate behavioral differences from `CppTokenizer` (see `NyxTokenizer.h`'s
  own doc comment for the full reasoning): its `StringLiteral`/`TemplateStringLiteral` `Lexeme` is
  Nyx's *resolved* value (quotes/escapes already processed), not a raw source substring the way
  `CppTokenizer`'s is; and `nyx::Lexer::Tokenize()`'s exception on an unterminated string/template
  literal is caught and turned into a clean, immediate `EndOfFile` rather than a partial token
  stream, matching `CppTokenizer`'s own never-throws contract.

### Follow-up landed (2026-08-05): tokenizer dispatch

`IHostLanguageTokenizer.h`'s own doc comment ("selected by file extension at preprocessor
startup") now describes a dispatch point that actually exists:

- New `Iris::CreateHostLanguageTokenizer(Source, FilePath)` (`include/Iris/TokenizerFactory.h`,
  `src/Iris/TokenizerFactory.cpp`) — returns a `NyxTokenizer` when `FilePath` ends in `.irisx`,
  a `CppTokenizer` otherwise, per `docs/iris_core_spec.md` §0's File model ("the file extension
  is the sole source of truth for which host language a file uses").
- `RenderBlockParser::Tokenizer_` is now `std::unique_ptr<IHostLanguageTokenizer>`, built via the
  factory in the constructor, instead of a concrete `CppTokenizer` member.
- `ImportResolver.cpp`'s `ScanImports` builds its tokenizer through the same factory instead of
  constructing `CppTokenizer` directly.
- `Driver::CompileFile` needed no changes at all — it only ever calls `ScanImports` and
  constructs `RenderBlockParser`, both of which now dispatch correctly on their own. `iris_cc`
  (which just wraps `CompileFile`) picks this up for free too.
- New test, `RenderBlockParserTests.cpp`'s "a .irisx FilePath routes through NyxTokenizer, not
  CppTokenizer" — parses the *same* source (a `content={` `hi}there` `}` prop value, a Nyx
  template string whose unescaped `}` would desync `CppTokenizer`'s raw brace-balance counting
  in `ParseEscapeHatch` but is swallowed as one opaque token by `NyxTokenizer`) twice, once as
  `test.iris` and once as `test.irisx`, and asserts the `.iris` parse errors while the `.irisx`
  parse doesn't — proving the dispatch is real, not just "constructs without crashing." Full
  suite: 148/148 passing (was 147; one test added).

### What's still open

There is no Nyx-side equivalent yet for `@signal`/`<Slot>` authoring inside a `.irisx` component
body (`IRIS_SIGNAL`'s C++ macro form doesn't translate) — so even with the dispatch now wired, a
real `.irisx` component with reactive state still can't compile end-to-end today. Matches
`roadmap.md` §26.1's framing of that as "a Penumbra/Chaos integration concern," still open on the
nyx-proto side too; nyx-proto's own `NyxRuntime` has no `RegisterDecorator`-shaped mechanism for
this yet either. Also not touched by this pass: `iris-lsp`'s `ClangdProxy`/`NyxLspProxy` seam
(semantic completion / goto-definition for `.irisx` in the LSP, a separate concern from the
`iris_cc` compile path this entry covers).

### Explicitly not requested

- Reimplementing `render{}`/JSX parsing, `@signal` lifting, `<Slot>` resolution, or Chaos-IR
  production as new iris-proto code, even though nyx-proto's own (now-superseded) Phase 7 task
  list originally described all of that — this repo's existing `RenderBlockParser`/`Codegen`/
  `Component` IR/`SlotRuntime`/`Reconciler` already cover it host-agnostically; only the lexical
  front end differs per host language.

---

## Named-child-handle (`ref`) prop, for host code reaching one mounted child directly — RESOLVED (2026-07-23)

> **Status:** Resolved in this repo — the parser/codegen/`Component` plumbing this entry asked
> for. The registry lookup surfaced on a mount call (`iris::MountResult::GetByRef` or similar)
> is `penumbra-ui-backend`'s own follow-up, as this entry's own "Required changes elsewhere"
> section already said, and stays out of scope here.
> **Trigger:** `pharos-proto` asked to spec out what it'd take to mount a `.iris`/`.lustre`
> component with no hand-written host-side glue at all — analogous to how a React app just
> imports and renders `<App />` with no manual DOM-node bookkeeping. Investigating what
> `pharos-proto`'s own consumers (`src/ui/lens_toggle.cpp`, `src/ui/inspector_panel.cpp`'s
> `buildInspectorRow`, `src/ui/color_filter_dropdown.cpp`) actually hand-write today turned
> up this as the single largest remaining piece of irreducible boilerplate.

### What landed

- `ElementNode` (`include/Iris/ElementNode.h`) and `Component` (`include/Iris/Component.h`)
  each gained a `Ref` field paralleling `Key` exactly — `std::optional<PropValue>` and
  `std::optional<IrisPropValue>` respectively.
- `RenderBlockParser.cpp`'s reserved-prop-name check (previously `key`-only) gained a `ref`
  arm, pulling it out of `Props` into `Node.Ref` the same way `key` is pulled into `Node.Key`.
- `Codegen.cpp`'s `ComponentEmitter::Emit` gained an `EmitWithRef` IIFE wrapper mirroring
  `EmitWithKey`, applied after the key wrap so a `key` + `ref` pair on the same element compose
  (each wrap only touches its own `Component` field before returning `Node`).
- `SemanticValidator.cpp` and `iris-lsp`'s `SemanticTokens.cpp` both gained a `Node.Ref`
  counterpart to their existing `Node.Key` handling, so a `!{ }` escape-hatch `ref` value is
  validated and its nested elements get semantic tokens the same way a keyed one does.
- Deliberately **not** touched: `Reconciler.cpp`'s identity-matching (`KeysEqual`,
  `SameIdentity`) stays `Key`-only — `ref` carries no reconciler meaning, exactly as originally
  proposed.
- Covered by new tests: `RenderBlockParserTests.cpp` (ref extracted out of `Props`, mirroring
  the existing key test), and four new `CodegenTests.cpp` cases (ref'd primitive, ref'd
  component invocation, unref'd element gets no wrap, key+ref compose). Full `test_iris` suite
  (129/129) passes.

### What's actually missing

Every `pharos-proto` consumer that needs to read or drive one specific mounted child after
building a component (a `Label*` to swap `Font` on, an `IconWidget*` to retarget its
`IconName`, a `TextInput*` to focus) does it by hand-walking `GetChildAt(index)` chains that
mirror the `.iris` file's child order — e.g. `lens_toggle.cpp:108-122` goes two levels deep
(frame → label) on each of two branches, `inspector_panel.cpp`'s `buildInspectorRow`
(116-177) goes three levels deep across two branches. Every one of these carries a comment
asserting "this mirrors `<Component>.iris`'s child order exactly" — the C++ is manually kept
in sync with the `.iris` file's structure by convention, with nothing tying them together, so
reordering children in the `.iris` file silently breaks the host code with no compiler error
(wrong widget type retrieved, or a null `dynamic_cast`).

Iris's only existing identity concept is `key` (`docs/iris_core_spec.md` §2.3/§2.4,
`include/Iris/Component.h:39,64`, `include/Iris/ElementNode.h:93`) — but it's scoped entirely
to the *reconciler*, matching old-tree elements to new-tree elements across a re-render
(`include/Iris/Reconciler.h:43`). It's never surfaced to host code mounting a tree for the
first time, and there is no `GetById`/`useRef` equivalent anywhere in this repo or in
`penumbra-ui-backend`'s `PenumbraWidget`/`Umbra::IWidget` (`PenumbraWidgetAdapter.h:56-59`
exposes only positional `GetChildAt`/`GetChildCount`, the same shape `Umbra::IWidget` itself
has).

### Proposed API

A `ref` prop (name chosen to read clearly against `key`'s reconciler-only meaning — a
`class`-like plain string, not a reconciler input) on any Iris element:

```
<Icon ref="trigger-icon" ... />
```

reaching `Component`/`IrisProps` the same way `class` already does (`Component.h`'s existing
prop-threading path), plus a lookup surfaced on whatever a mount call returns — e.g. an
`iris::MountResult` (or an addition to `MountFn`'s current return shape) exposing
`Umbra::IWidget* GetByRef(std::string_view ref) const`, populated during the same tree walk
`BuildWidgetTree`/`WrapExistingTree` (in `penumbra-ui-backend`) already performs. The registry
itself would need to live wherever the mounted tree's own lifetime is tracked, so it doesn't
outlive the widgets it points at.

### Required changes elsewhere

`penumbra-ui-backend`'s `Walker.cpp` (`BuildWidgetTree`) and `PenumbraWidgetAdapter.cpp`
(`WrapExistingTree`) would need to collect `ref`-tagged nodes into that lookup during their
existing recursive walk — logged as a matching entry in `penumbra-ui-backend`'s own
`docs/next_steps.md` rather than repeated here, since it's that repo's own follow-up once
this lands.

### Explicitly not requested

- Reusing `key` for this — `key` already has a distinct, load-bearing meaning (reconciler
  list-diffing identity) that doesn't overlap with "let host code find this specific node";
  conflating them would make both harder to reason about.
- A full CSS-selector-style query API (`querySelector`-equivalent) — `ref` names one specific
  node a consumer already knows it wants a handle to, not a general tree-search facility. No
  known consumer needs the latter.

## CMake helper to compile every `.iris` file in a directory, not one hand-written block per file — RESOLVED (2026-07-23)

> **Status:** Resolved in this repo.
> **Trigger:** same `pharos-proto` "what's left for plug-and-play components" investigation as
> the `ref` entry above.

### What landed

`cmake/IrisCompileDirectory.cmake` (included from the top-level `CMakeLists.txt`, right after
the `iris_cc` target it depends on via `$<TARGET_FILE:iris_cc>`) defines exactly the proposed
`iris_compile_directory(<target> <source-dir> <generated-header-dir>)` function: a plain
`file(GLOB "${SourceDir}/*.iris")` (not `CONFIGURE_DEPENDS`, matching this entry's own accepted
`pharos-proto`-style GLOB limitation), one `add_custom_command` per file mirroring
`pharos-proto/CMakeLists.txt`'s existing hand-written pattern exactly (`iris_cc <src> -o
<generated-header-dir>/<Name>.iris.h`, depending on `iris_cc`, the source file, and the
directory's `.iris.json`), collected under a `<target>_generate_iris` custom target that
`<target>` depends on, with `<generated-header-dir>` added to `<target>`'s include path so
generated `Name.iris.h` files `#include` the same way `pharos-proto`'s hand-written blocks
already produce them for.

Verified with a throwaway smoke-test subdirectory (a `.iris.json` + one `.iris` file + a
`main.cpp` `#include`ing the generated header, wired via `iris_compile_directory` and built
through the real `iris_cc`) exercised against a temporary `add_subdirectory` in the top-level
`CMakeLists.txt`, then removed once the generated header compiled and linked successfully — not
left behind as a permanent target, since this repo's own build has no first-party `.iris`
consumer of its own yet.

### What's actually missing

Consuming `iris_cc` from CMake today means one hand-written `add_custom_command` per `.iris`
file (`pharos-proto/CMakeLists.txt:65-146`, ten near-identical copies as of this writing),
each also needing its output added to a target's `DEPENDS` list by hand
(`CMakeLists.txt:172-183`). This repo's own `CMakeLists.txt` defines only the `iris_cc`
executable target (line 72-78 as of this writing) — no CMake function/macro wraps it, and
`docs/iris_core_spec.md` §5's `.iris.json` schema (`target`/`version`/`searchPaths`) has no
CMake-integration fields; `searchPaths` only controls `import` resolution inside the
preprocessor, not what gets compiled. Every consumer re-derives the same
generate-into-a-build-dir/depend-on-`iris_cc`/depend-on-`.iris.json` wiring from scratch.

### Proposed API

A CMake function this repo installs alongside the `iris_cc` target, e.g.:

```cmake
iris_compile_directory(<target> <source-dir> <generated-header-dir>)
```

globbing `<source-dir>/*.iris`, emitting one `add_custom_command` per file (mirroring
`pharos-proto/CMakeLists.txt`'s existing per-file pattern exactly, just generated instead of
hand-written) and adding every output to `<target>`'s sources/dependencies automatically.
CMake's own `file(GLOB ...)` non-reactivity to added/removed files (needing a fresh
`cmake -B build`, not just a rebuild) is an acceptable, well-understood limitation — it's the
same tradeoff `pharos-proto/CMakeLists.txt`'s own `file(GLOB_RECURSE PHAROS_LIB_SOURCES ...)`
already accepts for `.cpp` files today.

### Required changes elsewhere

None in `penumbra-ui-backend` or `lustre` — this is CMake glue local to `iris`'s own build
integration surface.

### Explicitly not requested

- Auto-discovering matching `.lustre` sibling files and wiring their runtime-load path as a
  compile definition — that convention (`PHAROS_<NAME>_LUSTRE_PATH`) is specific to how
  `pharos-proto` currently threads `.lustre` paths into its own code
  (`pharos-proto/CMakeLists.txt:197-208`), not something `iris` itself has an opinion on. A
  consuming app could layer that on top of `iris_compile_directory` itself if useful.

## Live-widget root registry, for Lustre's hot-reload — RESOLVED (2026-07-22)

> **Status:** Resolved in this repo.
> **Trigger:** Filed during Lustre's design handoff (`../../lustre/docs/lustre_handoff.md` §3,
> "Runtime-loaded, not compiled ahead of time") — not blocking that design, but recorded since
> the underlying need is Iris's, not just Lustre's.

### What landed

Folded into the existing `iris::IrisRuntime` singleton (`include/Iris/SlotRuntime.h`) rather
than a new `IrisRuntime.h` file, since that's where every other piece of ambient runtime state
(the active-slot stack, the component-instance stack) already lives: a `Root_` field plus
`IrisRuntime::RegisterRoot(Umbra::IWidget*)`/`GetRoot() const` methods, and free functions
`iris::RegisterRoot`/`iris::GetRoot` (`SlotRuntime.cpp`) matching this entry's own proposed API
exactly. Zero backend-specific content, as proposed — `Umbra::IWidget*` in, `Umbra::IWidget*`
out, ownership left with the caller. Covered by a new `SlotRuntimeTests.cpp` case. Full
`test_iris` suite passes.

### Context

Lustre (the styling layer for Iris — `../../lustre`) loads and parses `.lustre` files at
application runtime rather than compiling them ahead of time like `iris_cc` does, so styles can
hot-reload without a rebuild. Lustre's own need is narrow — re-resolve and re-apply styling data
to already-mounted widgets when a `.lustre` file changes — and doesn't by itself require Iris to
change anything: `class` already reaches a live widget as a plain runtime string
(`WidgetBase::ClassName`, threaded through `IrisPropDiff::ClassName`), enough for a
Penumbra-backend-side restyle pass to walk the real widget tree directly.

What's actually missing: Iris's runtime has **no whole-application live-widget registry or
tree-walk entry point** anything external could use to find "every widget currently mounted,
anywhere in the app" without a full reconcile. `SlotState` tracks only its own slot's live
widget(s); `ResolveSlots` discovers slots but doesn't expose a global list; every existing
"Root" in the codebase is a compile-time `ElementNode` AST root, not a mounted widget.

**Decided:** this is a small addition to Iris itself (not `iris-penumbra-backend` or Lustre) —
the mechanism needed (hold a `Umbra::IWidget*`, hand it back out on request) has zero
backend-specific content, since `Umbra::IWidget` is already the backend-agnostic interface the
reconciler walks/mutates through. Building it per-backend would mean every backend
reimplementing the same trivial store-a-pointer/expose-a-getter logic for no backend-specific
reason.

### Proposed API

```cpp
// include/Iris/IrisRuntime.h (or similar) — folded into IrisRuntime alongside existing state
void iris::RegisterRoot(Umbra::IWidget* root);
Umbra::IWidget* iris::GetRoot();
```

Callable by any consuming app right after it builds its tree. Generic over any backend's
`IWidget` implementation by construction. Benefit beyond Lustre: the next cross-cutting concern
needing "the whole mounted tree" (a debugger, an inspector) gets this for free.

### Explicitly not requested

- Real hot-reload of *component logic* itself (structure, state, event handlers) for a future
  scripting-language host — a substantially bigger design question (state migration across a
  reload, mounted-widget identity, whether the reconciler can treat a reload as "just another
  re-render"). Revisit when that host is scoped, informed by what Lustre's narrower
  styling-only hot-reload turns out to need in practice. Sized (not designed) against the
  real current code in `docs/iris_interpreted_host_hot_reload_gap.md` — `ComponentInstance`
  has no identity across two runs of the same component (state would reset), the reconciler
  has no entry point that accepts "diff against whatever's live right now," and Nyx as
  currently specced is still a compiled host, not a genuinely interpreted one.

---

## `<Icon>` size prop and vector-glyph tag — RESOLVED (2026-07-20)

> **Status:** Resolved in this repo. Originally filed as part of the componentization-gaps
> investigation below.

`<Icon>` (`include/Iris/IrisElementTag.h`) and its `size` prop (commit `2bebf6f`, "Add
`<Icon>`'s size prop") both landed. Whether `iris-penumbra-backend`'s `Walker.cpp` has a build
case wiring it to a real `IconWidget` is that sibling repo's own concern, out of scope here.

---

## `<Scroll>` / `<Input>` Core primitive tags — RESOLVED (2026-07-20)

> **Status:** Resolved in this repo (commit `42cc09c`, "Add `<Scroll>` and `<Input>` Core
> primitives"). Originally filed as item 3 of the componentization-gaps investigation below —
> `ExplorerPanel`/`InspectorPanel`/the toolbar's path field had no representable Iris tag for
> `ScrollablePanel`/`TextInput`, the "harder ceiling" that doc called out (root content, not
> just leaf content, had no tag to compile down to).

`<Scroll>` takes element children like `<Frame>` plus `wheelStep`; `<Input>` is a leaf like
`<Image>`/`<Icon>` plus `text`/`preferredWidth`. See `docs/iris_core_spec.md` around line 540
for the full prop tables. `iris-penumbra-backend`'s `Walker.cpp` build cases targeting
`ScrollablePanel`/`TextInput`'s own `Builder`s are that sibling repo's concern, out of scope
here.

### Follow-on gap this surfaced, now RESOLVED (2026-07-22): `<Input>`'s `onChange` can't carry the new text

> **Status:** Resolved in this repo.

`docs/iris_core_spec.md` §"`<Input>`" (~line 552-561): `<Input>` shipped with no event props in
its first cut. `Penumbra::Widgets::TextInput::OnTextChanged` takes a `const std::string&`;
`IrisPropValue` (`include/Iris/IrisProps.h`)'s variant had no `function<void(std::string)>`
member to carry it, so the shared `onChange` prop (`function<void()>`, zero-argument, shared by
every primitive) couldn't tell a caller *what* changed — it existed on `<Input>` but was
effectively inert.

#### What landed

- `IrisPropValue` (`include/Iris/IrisProps.h`) gained a `std::function<void(std::string)>`
  variant member, sized as a general value-carrying-callback alternative rather than an
  `<Input>`-only special case, per this entry's own original note about future primitives (e.g.
  a slider) potentially needing the same shape.
- `<Input>` gets its own new `onTextChange` prop (`src/Iris/CorePrimitives.cpp`'s
  `PrimitivePropTypeNames()`), distinct from the shared zero-argument `onChange` so no other
  primitive's event-prop shape changed.
- `Umbra::IrisPropDiff` (`libs/umbra-interfaces/include/Umbra/IWidget.h`) gained a matching
  `OnTextChange` field; `Reconciler.cpp`'s `ComputePropDiff` populates it via a new
  `DiffTextEventField` (same "no `operator==`, always changed when present" treatment as the
  existing zero-argument event props), and `KeysEqual`'s never-a-key-match `if constexpr`
  branch was extended to include the new variant alternative.
- Covered by a new `ReconcilerTests.cpp` case asserting the captured text argument actually
  round-trips through `ComputePropDiff`, not just that the optional is populated. Full
  `test_iris` suite passes.
- Wiring `Umbra::IrisPropDiff::OnTextChange` to a real `Penumbra::Widgets::TextInput::
  OnTextChanged` is `iris-penumbra-backend`'s own concern, out of scope here (same split as
  every other Core primitive's backend wiring).

---

## Gradient-fill Lustre property — cross-reference only, not an Iris action item (2026-07-20)

> **Status:** Open, but belongs to `lustre` (property table) and `penumbra-proto`
> (`Renderer::DrawGradientRect` already exists there) — recorded here only because it was found
> during an Iris/Penumbra/Lustre componentization investigation and blocks two consumers
> (`GradientButton`, `ExplorerPanel`'s `TreeRow` selection fill) from a full `.iris`/`.lustre`
> rewrite. No Iris-side change requested.

`lustre_core_spec.md` §2's property table has `background-color` only, no
`background-image: linear-gradient(...)` or equivalent. File any follow-up against `lustre`'s
own `docs/`, not here.

---

## Popup/overlay/z-order layer — cross-reference only, not an Iris action item (2026-07-20)

> **Status:** Open, but belongs to `penumbra-proto` — already tracked there via
> `docs/penumbra_requirements.md` item 5. Recorded here only because it's the reason
> `ColorFilterDropdown`'s popover has no representable tree for an Iris/Lustre migration to
> target at all. No Iris-side change requested.

---

## `iris-lsp` goto-definition: `class="..."` string → its Lustre selector — RESOLVED (2026-07-21)

> **Status:** Resolved in this repo. Implemented the text-scan option this doc itself left as
> the leaning-towards choice, matching `FindComponentDeclaration`'s existing precedent.
> **Trigger:** Filed while building `lustre-lsp` (`../../lustre/tools/lustre-lsp`, see that
> repo's `docs/lustre_lsp_decision.md` §4) — clicking a `class="card"` string in a `.iris`
> buffer and landing on `.card { }` in the paired `Name.lustre` was an explicit ask, but the
> click happens while a `.iris` buffer is open, so `lustre-lsp` itself has no way to serve it;
> it can only live in `iris-lsp`.

### What landed

- `RenderTextHeuristics.h`/`.cpp` gained `ClassPropValueAtPosition` (the class name inside a
  `class="..."` value span the cursor sits within, mirroring `TagNameAtPosition`'s own
  boundary-inclusive convention) and `FindClassSelector` (a `.ClassName { }` /
  `.ClassName:pseudo { }` text scan over a `.lustre` file's source, mirroring
  `FindComponentDeclaration`'s word-boundary-checked search).
- `Server::HandleDefinition` checks `ClassPropValueAtPosition` before `TagNameAtPosition`
  inside the existing `InRenderBlock` branch; on a match, the new `Server::ResolveClassSelector`
  searches the paired `Name.lustre` file first, falling back to a sibling `global.lustre`,
  matching Lustre's own component-overrides-global cascade order
  (`../../lustre/docs/lustre_core_spec.md` §1.3).
- Covered by new unit tests in `tools/iris-lsp/tests/RenderTextHeuristicsTests.cpp` (both new
  heuristics in isolation) and three new end-to-end `Server.definition` cases in
  `tools/iris-lsp/tests/ServerTests.cpp` (paired-file hit, global-file fallback, and the
  neither-defines-it null case). Full `iris_lsp_tests` (51/51) and `test_iris` (122/122) suites
  pass.

### Left as-is from the original proposal

- The text-scan approach was chosen over linking `lustre`'s real `Tokenizer`/`Parser` — this
  repo still has no dependency on `lustre`, and the scan shares
  `FindComponentDeclaration`'s existing "confused by a selector-shaped string inside a
  comment" edge case, unchanged from that function's own accepted behavior.
- Goto-definition *from* Lustre back to Iris, and any change to Lustre's own resolver/parser,
  remain explicitly out of scope, as originally noted.

### Context

`iris-lsp`'s `HandleDefinition` (`tools/iris-lsp/Server.cpp:466-556`) currently resolves
exactly two goto-definition sources, both funneled through `ResolveComponentDeclaration`
(`Server.h:93-96`) once a component `Name` is found: an `import Name` statement line, and a
`<Name>`/`</Name>` tag usage inside `render{}` (`TagNameAtPosition`,
`RenderTextHeuristics.h:34`). Neither covers a prop *value* — specifically the `class="card"`
string literal on an element inside `render{}`, which is the sole join between a `.iris`
element and its Lustre styling (`../../lustre/docs/lustre_core_spec.md` §0: "the `class` prop
is the only bridge between an Iris element and its Lustre declarations").
`RenderTextHeuristics.h`'s existing heuristics (`ClassifyRenderCompletion`'s
`RenderCompletionKind::{None,TagName,AttributeName}`, `TagNameAtPosition`) have no
attribute-*value* case at all today — there's nothing to extend, a new heuristic function is
needed.

Root cause: `HandleDefinition`'s `NameToResolve` search (`Server.cpp:490-505`) only ever looks
for an import name or a tag name; it never inspects whether the cursor sits inside a
`class="..."` value span, so that case falls through to the `Generated`/clangd-proxy branch
(`Server.cpp:502-504`) and gets treated as ordinary host-language code — which it isn't, so
this can never resolve correctly as-is.

### Proposed API

A new pure heuristic, next to `TagNameAtPosition` (mirrors its own shape and doc comment
exactly, `RenderTextHeuristics.h:29-34`):

```cpp
// RenderTextHeuristics.h
// The class name inside a `class="..."` attribute value the cursor sits within, on Line, if
// any -- scans for `class="` and checks whether ColumnOneBased falls inside the quoted span.
// Distinct from TagNameAtPosition: this looks at a prop *value*, not the tag/attribute name
// itself, and only ever matches the literal `class` prop (Iris Core's own sole styling
// bridge -- ../../lustre/docs/lustre_core_spec.md §0).
std::optional<std::string> ClassPropValueAtPosition(std::string_view Line, std::uint32_t ColumnOneBased);
```

In `Server::HandleDefinition`, inside the existing `InRenderBlock` branch
(`Server.cpp:492-501`), check this before/alongside `TagNameAtPosition`. On a match, resolve
to a Location the same way `ResolveComponentDeclaration` resolves a component file — except
the target file is `Name.lustre` (same basename as the open `.iris` file, not an imported
component) — and search it for a `.class-name { }` (or `.class-name:pseudo { }`) selector
matching the clicked value, falling back to a sibling `global.lustre` if the component file
doesn't define it (mirrors `../../lustre/docs/lustre_core_spec.md` §1.3's own two-layer
cascade order: component file overrides global, so it should be searched first).

### Required changes elsewhere

Finding the selector's own location needs *some* way to locate a `.class-name {` occurrence
in the target `.lustre` file. Two options, recorded rather than decided since this repo
doesn't currently depend on `lustre` at all:

- **Lightweight text scan** (no new dependency) — search for `.class-name` followed by
  optional `:pseudo` then `{`, the same "best-effort text search, not a real parse" spirit
  `FindComponentDeclaration` (`RenderTextHeuristics.h:41`) already uses for component
  declarations. Misses nothing `FindComponentDeclaration`-style callers would notice today,
  but would (like that function) get confused by a selector text that happens to appear
  inside a Lustre comment.
- **Link `lustre`'s real `Tokenizer`/`Parser`** (`../../lustre/include/Lustre/Parser.h`) as a
  new dependency, and use its actual `Rule::Location` — exact, no edge cases, but a new
  cross-repo dependency edge (`iris` → `lustre`) that doesn't exist yet in either direction.

Leaning towards the text-scan option for a first cut, matching this file's own existing
precedent, but recorded as an open choice for whoever picks this up.

### Explicitly not requested

- Goto-definition *from* Lustre back to Iris (a `.class { }` selector jumping to every `.iris`
  element using that class) — a one-to-many relationship with no obvious single target,
  different shape of feature, not asked for.
- Any change to Lustre's own resolver/parser — this is purely an `iris-lsp`-side lookup
  against `.lustre` source text/AST, not a runtime resolution change.

## No way to declare a custom widget/imperative-draw node as an Iris element — RESOLVED (2026-07-30)

> **Status:** Resolved in this repo — the narrower, opt-in escape tag this entry's own
> "Proposed API" sketched, not a reopening of `Component`'s backend-agnostic IR guarantee for
> everything else. The backend-mapping pass that actually calls `<Native>`'s builder and
> splices the result in (`penumbra-ui-backend`'s `Walker.cpp`/`PenumbraWidgetAdapter.cpp`) is
> that repo's own follow-up, as originally scoped below.
> **Trigger:** `pharos-proto` auditing its own `src/ui/` for how much further its
> `.iris`/`.lustre` componentization could go — specifically, whether the remaining
> hand-rolled `Box` subclasses (`TreeRow`, `DropdownTrigger`/`DropdownMenuRow`,
> `ChevronSeparator`, and the whole `ViewportWidget`-hosted treemap in `atlas_panel.cpp`)
> could become `.iris` components the same way `LensToggle`/`InspectorRow`/etc. already
> did.

### What landed

- `IrisElementTag::Native` (`include/Iris/IrisElementTag.h`) — a new Core primitive.
- `Component::NativeBuilder` (`include/Iris/Component.h`): a `std::shared_ptr<IrisNativeBuilder>`
  field paralleling `SlotCallable` exactly — its own dedicated field, not an `IrisProps` entry,
  for the same reason `SlotCallable` isn't: nothing about its value (a callable producing an
  already-built `Umbra::IWidget`) belongs in the reconciler's content-diffed prop map. Unlike
  `SlotCallable`, its callable returns a live widget handle directly rather than more `Component`
  IR — the one place `Umbra::IWidget` appears in the IR shape itself (`Component.h` now includes
  `Umbra/IWidget.h`), not just the runtime layer (`Reconciler.h`/`SlotRuntime.h`) already built on
  top of it. `Iris::MakeNativeBuilder(Callable&&)` mirrors `MakeSlotCallable`'s wrapping, minus
  the `if constexpr` return-type dispatch (there's exactly one valid shape:
  `std::unique_ptr<Umbra::IWidget>()`). `Component`'s existing 4-arg constructor gained a 5th
  defaulted parameter (`NativeBuilder = nullptr`) rather than a new overload, so every existing
  4-arg call site (`EmitOrdinaryPrimitive`, `EmitTextPrimitive`, `EmitSlot`,
  `EmitSyntheticTextNode`) kept compiling unchanged.
- `Codegen.cpp`'s `ComponentEmitter::Emit` gained an `EmitNative` branch (checked alongside the
  existing `Slot`/`Text` special cases): requires exactly one `build` prop set to a `{ }` escape
  hatch (not a string literal, not a `!{ }` JSX escape hatch used as anything but a plain
  callable), no children, and no other props — each violation is its own codegen error, mirroring
  `EmitSlot`'s own arity/shape checks for `<Slot>`.
- Went with attribute syntax (`<Native build={...} />`) exactly as the entry's own speculative
  proposal sketched, rather than `<Slot>`'s single-escape-hatch-child shape — `build`'s escape
  hatch already fits the ordinary prop-value grammar (`ElementNode::Props`/`PropValue`) with no
  new parser plumbing needed; only where it's routed at codegen time (into `NativeBuilder`,
  never through `EmitPrimitiveProps`/`IrisPropValue`) differs from an ordinary prop.
- `CorePrimitiveTagNames()` (`CorePrimitives.cpp`) gained `"Native"` — `SemanticValidator`'s
  `ValidateTagIsInScope` needed no changes at all, since it already accepts any tag in that set
  generically (it has no `<Slot>`-specific handling either).
- `docs/iris_core_spec.md` §3.1 gained a `<Native>` primitive-reference entry (props, leaf-child
  shape, backend-mapping/reconciliation-exemption requirements) and §2.5 a note on
  `NativeBuilder` being the IR's one deliberate, narrow, opt-in relaxation of its
  backend-agnostic-value guarantee — explicitly not a reopening of the `Component`-as-`WidgetBase`-facade
  design §2.5's own "rejected alternative" note already rejected for the IR as a whole.
- Covered by 5 new `CodegenTests.cpp` cases: a valid `build` escape hatch, missing `build`,
  string-literal `build` (rejected — must be a real escape hatch), a child (rejected — leaf), and
  an unknown prop (rejected — `build` is the only one). Full `test_iris` suite (138/138) passes.

### What's actually missing

`docs/iris_core_spec.md` §3.1's Core primitives are a fixed, closed set: `Frame`,
`Inline`, `Grid`, `Image`, `Icon`, `Text`, `Scroll`, `Input`. Every child position inside
a `render { }` tree is either a nested `<Tag>` or a `{ }`/`!{ }` escape hatch (§1.4) whose
value is always a `Component`/`vector<Component>` IR node (§2.5) — never an
already-built, arbitrary `WidgetBase`/`Umbra::IWidget`. There is no way to hand Iris "here
is a widget I built by hand (a `Box` subclass overriding
`Measure`/`Arrange`/`Draw`/`UpdateInteractionState`), mount it at this position."

This blocks componentizing, in `pharos-proto`:
- `TreeRow` (`src/ui/explorer_panel.cpp`) — toggle-zone hit-splitting, double-click
  detection via a shared running clock, a hand-drawn selection gradient/accent bar.
- `DropdownTrigger`/`DropdownMenuRow` (`src/ui/color_filter_dropdown.cpp`) — hover/open
  border-color swap, a per-state-colored hand-drawn icon+label (needed because
  `IconWidget` can't vary color by hover state the way this row does).
- `ChevronSeparator` and the entire `ViewportWidget`-hosted treemap/particle-animation/
  breadcrumb/tooltip system (`src/ui/atlas_panel.cpp`, ~750 lines) — a custom
  `OnRenderScene`/`OnSceneInput` pair, not expressible as a tree of Core primitives at
  all.

`pharos-proto`'s own `CLAUDE.md` documents subclassing `Box` as "the intended extension
point — not a workaround," matching how Penumbra's own `Button`/`Checkbox` are built.
That framing is accurate at the composition level, but it means this class of widget has
*no* path into `.iris`/`.lustre` at all today, even in principle — not a missing
convenience the way `ref`/`GetByRef` or `LoadStylesheetFromFile` were (both landed
2026-07-23, both now adopted in `pharos-proto`), but a closed set by design. §2.5's own
"rejected alternative" note explicitly rejected a `Component` shape that would have
allowed embedding an already-built widget, for good reasons at the time (keeping
`Component` backend-agnostic IR, with nothing for Stage 3's reconciler to diff against).
Worth reopening now that Stage 3 (the reconciler, a real Penumbra `IWidget` adapter) is
implemented and tested — the tradeoff that motivated the original rejection may have
changed since.

### Proposed API (speculative — a starting point, not a firm design)

Some kind of opaque/escape-hatch primitive tag (name TBD, e.g. `<Native>`) whose child is
a `{ }` escape hatch evaluating to an already-built widget handle, e.g.:

```cpp
render {
    <Frame class="explorer-row">
        <Native build={[&]() { return buildTreeRowWidget(node, app, theme); }} />
    </Frame>
}
```

Deliberately kept **outside** the reconciler's diffing (mount-once, closer to how a
`ref`'d node is found rather than reconciled by content) so it doesn't compromise
`Component`'s backend-agnostic IR guarantee for everything else in the tree — the
opaque subtree is a black box to Iris's runtime, exactly as it already is to
`pharos-proto`'s own code today, just given a slot inside a declarative tree instead of
requiring the whole panel to be hand-built.

### Required changes elsewhere

A backend-mapping pass (`penumbra-ui-backend`'s `Walker.cpp`/`PenumbraWidgetAdapter.cpp`)
would need a case for this tag that takes the escape hatch's returned handle and splices
it directly into the built tree at that position, bypassing the usual
Core-primitive-to-`Builder`-call mapping entirely. Not scoped further here since it's
that repo's own follow-up once/if this lands, same convention the `ref`/`GetByRef` entry
above followed.

### Explicitly not requested

- A general imperative-draw/custom-layout DSL inside Iris itself — the ask is narrower:
  a documented, sanctioned escape valve for the composition pattern that's already
  idiomatic in every backend this stack has (Penumbra's own `Box` subclassing), not a new
  sublanguage for expressing custom drawing/hit-testing in `.iris` syntax.
- Any change to how reconciliation/diffing works for ordinary (non-opaque) `Component`
  trees — this tag's subtree is deliberately exempt from that machinery, not a
  generalization of it.

## No layout-container primitive beyond Frame's three stack modes — PARTIALLY RESOLVED (2026-07-30)

> **Status:** Partially resolved in this repo. Of the three concrete shapes this entry named,
> only the `SplitPanel`-equivalent (`<Split>`) is actually an Iris-grammar gap — it's a
> structurally distinct widget (two fixed slots, its own drag-interaction state), the same kind
> of gap `<Scroll>`/`<Input>` closed for `ScrollablePanel`/`TextInput`. `ThreeZoneRow`'s
> justify-content need and `FixedLeadingStrip`'s "don't shrink either child" contract are **not**
> resolved here and, per the reasoning below, were never really Iris-side gaps in the first
> place — `<Frame>`'s own spec entry (§3.1) is explicit that its only Iris-level props are
> `class`/`key`/event props; which of `VerticalStack`/`HorizontalStack`/`None` a `<Frame>` uses is
> entirely a Lustre `display`/`flex-direction` styling question (`StyleApplier` mapping onto
> `Box::Layout`), never an Iris grammar one, the same "cross-reference only, not an Iris action
> item" scoping this file already gives the "Gradient-fill Lustre property" and "Popup/overlay/
> z-order layer" entries above. Logging a justify-content-capable stack mode as a `lustre`-side
> ask is that repo's own follow-up, not repeated here (no local `lustre` checkout with a live
> backlog file was confirmed writable to during this session — see "Required changes elsewhere"
> below).
> **Trigger:** same `pharos-proto` `src/ui/` audit as the entry above.

### What landed

- `IrisElementTag::Split` (`include/Iris/IrisElementTag.h`) — a new Core primitive, grounded
  against `penumbra-proto`'s real `Penumbra::Widgets::SplitPanel`
  (`include/Penumbra/Widgets/SplitPanel.h`: `Axis`, `SplitRatio`, `HandleThicknessLogical`,
  `MinPaneSizeLogical`, and `SetFirst`/`SetSecond` in place of a generic children vector — not
  just the requirements-doc description, the same "verified against the actual shipped code"
  bar `<Scroll>`/`<Input>`'s own spec entries were held to).
- Four new ordinary props in `PrimitivePropTypeNames()` (`CorePrimitives.cpp`): `axis`
  (`std::string` — `"horizontal"`/`"vertical"`; resolving it to `SplitPanel`'s real
  `SplitAxis` enum is left to the backend-mapping pass, the same treatment `icon`'s catalog-key
  string already gets), `ratio`, `minPaneSize`, `handleThickness` (all `float`, mirroring
  `SplitPanel`'s own field names/units). Unlike `<Native>`'s `build` above, these go through the
  ordinary `IrisProps`/`EmitPrimitiveProps` path exactly like `wheelStep`/`preferredWidth` — a
  `<Split>`'s props are plain data, nothing about them needs to bypass the reconciler.
- `Codegen.cpp`'s `Emit` gained a `Split` branch enforcing **exactly two** children (leading and
  trailing panes) before delegating to the same `EmitOrdinaryPrimitive` every other
  element-children primitive already uses — zero, one, or three-or-more children is a codegen
  error; `EmitChildrenList`'s existing generic per-tag `AllowsAny`/`AllowsText` logic needed no
  changes (`<Split>` behaves like `<Frame>`: element children only, no bespoke leaf/text rules).
- `docs/iris_core_spec.md` §3.1 gained a `<Split>` primitive-reference entry.
- Covered by 4 new `CodegenTests.cpp` cases: two children (valid, both props and pane order
  checked), one child, three children, and zero children (all three arity violations rejected).
  Full `test_iris` suite (138/138) passes.

### What's still actually missing

`ThreeZoneRow` (left/center/right justify in one row) and `FixedLeadingStrip` ("fixed-height
leading child + fill-remainder child, each offered the container's own full available size,
neither shrunk") remain unaddressed — deliberately, per the scoping above. Whoever picks up a
`lustre`-side justify-content property still needs `FixedLeadingStrip`'s specific
"don't-shrink-either-child" sizing contract designed separately, exactly as this entry originally
flagged: neither a justify-content stack mode nor `<Split>` covers it (`Box::Measure` handing
every child the same full available size, rather than partitioning it per sibling the way
`FixedLeadingStrip`'s two roles need, is a `Box`-layout-algorithm question in `penumbra-proto`,
not a `Frame`-prop or Iris-grammar one either).

### What's actually missing (original, `<Split>`'s own gap — now resolved above)

`Frame`'s only layout modes are `VerticalStack`/`HorizontalStack`/`None` (mapping to
Penumbra's `Box::Layout`). `pharos-proto` has three real layout shapes today with no Core
primitive to express them:

- A `SplitPanel`-equivalent: a draggable-handle resizable split. This is
  `pharos-proto`'s actual top-level app layout (nested `SplitPanel`s, `src/main.cpp`) —
  without it, no `.iris` tree could ever represent the app's real root layout, regardless
  of how the gap above (custom widgets) resolves.
- `FixedLeadingStrip` (`pharos-proto`'s `src/ui/layout_helpers.h`): "fixed-height leading
  child + fill-remainder child, each offered the container's own full available size" —
  needed because `Box::Measure` hands every child the *same* full available size rather
  than shrinking it per sibling, so a greedy child (a `SplitPanel`/`ViewportWidget`) and a
  fixed-size sibling can't coexist in one ordinary stack without double-counting space.
- `ThreeZoneRow` (same file): "left/center/right justify in one row" —
  `HorizontalStack` only stacks sequentially, no space-between/justify concept at all.

### Proposed API

Unlike the entry above, these three are pure layout algorithms with no custom
draw/hit-testing involved — geometry only. Plausibly addressable by growing `Frame`'s own
layout-mode vocabulary rather than needing an escape-hatch mechanism:

- A justify-content-capable stack mode (e.g. `display: stack; justify-content:
  space-between;` in Lustre, alongside the existing `flex-direction`/`align-items`
  properties `StyleApplier` already maps) would cover `ThreeZoneRow`.
- A `Split`-with-handle mode (new prop(s) for ratio/min-pane-size/handle-thickness,
  mirroring `Penumbra::Widgets::SplitPanel`'s own fields) would cover the root-layout
  gap.
- `FixedLeadingStrip`'s specific "don't shrink either child" contract doesn't map cleanly
  onto either of the above — recorded as still needing its own design, not folded into
  this proposal.

### Required changes elsewhere

For `<Split>` (landed above): `penumbra-ui-backend`'s `Walker.cpp`/`PenumbraWidgetAdapter.cpp`
need a build case mapping it onto `Penumbra::Widgets::SplitPanel` (`axis`/`ratio`/
`minPaneSize`/`handleThickness` onto its matching fields, `Children[0]`/`Children[1]` via
`SetFirst`/`SetSecond`) — not scoped further here, same convention the `ref`/`GetByRef` entry
above followed. For the still-open justify-content stack mode: `lustre`'s own property table
plus `penumbra-ui-backend`'s `StyleApplier` mapping it onto `Box::Layout`/a new justify field —
that repo's own follow-up, not this one's, per the scoping note at the top of this entry.

### Explicitly not requested

- A general-purpose flexbox/grid layout engine — the three concrete shapes above are the
  ask; a full CSS-flexbox-equivalent implementation would be a much larger project than
  what any current consumer actually needs.
