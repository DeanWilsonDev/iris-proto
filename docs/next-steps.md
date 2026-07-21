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

## Live-widget root registry, for Lustre's hot-reload (2026-07-19)

> **Status:** Open — not yet scoped or implemented.
> **Trigger:** Filed during Lustre's design handoff (`../../lustre/docs/lustre_handoff.md` §3,
> "Runtime-loaded, not compiled ahead of time") — not blocking that design, but recorded since
> the underlying need is Iris's, not just Lustre's.

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

### Follow-on gap this surfaced, still open: `<Input>`'s `onChange` can't carry the new text

> **Status:** Open.

`docs/iris_core_spec.md` §"`<Input>`" (~line 552-561): `<Input>` ships with no event props in
this first cut. `Penumbra::Widgets::TextInput::OnTextChanged` takes a `const std::string&`;
`IrisPropValue` (`include/Iris/IrisProps.h`)'s variant has no `function<void(std::string)>`
member to carry it, so the shared `onChange` prop (`function<void()>`, zero-argument, shared by
every primitive) can't tell a caller *what* changed — it exists on `<Input>` but is effectively
inert.

#### Proposed fix

Add a `function<void(std::string)>` (or similarly-shaped) alternative to `IrisPropValue`'s
variant, and give `<Input>` its own text-carrying event prop (distinct from the shared
zero-argument `onChange`, to avoid changing every other primitive's event-prop shape). Exact
naming/shape not decided — whoever picks this up should check whether other future primitives
(e.g. a slider) will also need a value-carrying callback, to size the variant addition once
rather than per-primitive.

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

## `iris-lsp` goto-definition: `class="..."` string → its Lustre selector (2026-07-21)

> **Status:** Open — not yet scoped or implemented.
> **Trigger:** Filed while building `lustre-lsp` (`../../lustre/tools/lustre-lsp`, see that
> repo's `docs/lustre_lsp_decision.md` §4) — clicking a `class="card"` string in a `.iris`
> buffer and landing on `.card { }` in the paired `Name.lustre` was an explicit ask, but the
> click happens while a `.iris` buffer is open, so `lustre-lsp` itself has no way to serve it;
> it can only live in `iris-lsp`.

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
