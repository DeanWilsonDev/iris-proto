# Iris — Next Steps

Running log of active feature requests / capability gaps for this repo, per
`~/.claude/skills/feature-request`. Each entry below is appended, never edited in place once
resolved — resolved entries get a `RESOLVED` status line and stay for the record instead of
being deleted. For the full historical status log of Stage 0–3 work (all done, effectively a
changelog), see `docs/iris_next_steps.md`; for closed design decisions, see the various
`docs/iris_*_decision*.md` files — `CLAUDE.md` explains how those relate to
`docs/iris_core_spec.md`, the authoritative language reference.

This file coalesces two previously separate requirement docs
(`lustre_hotreload_iris_requirements.md`, `penumbra_iris_lustre_componentization_gaps_requirements.md`,
both since removed) plus one gap identified directly against `docs/iris_core_spec.md`.

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
  styling-only hot-reload turns out to need in practice.

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
