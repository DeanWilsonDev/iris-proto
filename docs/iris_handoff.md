# Iris — Handoff: Scoping Iris Core Against Penumbra as a First Backend

> **Status:** Design handoff — no Iris code exists yet.
> **Scope:** How Iris's language/runtime should be split so it can target both Penumbra
> (2D desktop tool UI, available now) and Umbra Engine/Nyx (in-game UI, later) without the
> two diverging into separate DSLs.
>
> **Superseding note (post-Stage-1-scoping):** `docs/iris_stage1_decision_doc.md` records a
> later architectural pivot — Iris is now a thin preprocessor over a host language (C++23 via
> `.iris` today, Nyx via `.irisx` later), not a standalone component/props/state/event DSL.
> Most of what this document and its embedded Stage 0 spec (below) describe as "Iris Core
> language" — component declaration, props structs, `state` blocks, event-handler grammar,
> `if`/`for` — is now ordinary host-language code, not Iris syntax. This document is kept as
> the historical record of *why* the multi-backend split exists; `docs/iris_core_spec.md`
> (v2) is the current authoritative language reference. Where the two disagree, the spec wins.

## 1. Why this document exists

Iris was originally designed against Umbra Engine + Nyx as its only backend — Nyx decides
which component is active, Nyx drives 3D animation state, Umbra's state machine owns
routing. But Umbra Engine doesn't exist yet, and Penumbra — a small, real, working C++23/SDL3
retained-mode widget library — already does, with two real consumers: **Pharos** (an existing
external tool) and **Dawn** (Umbra's 2D level editor, upcoming). Building Iris against
Penumbra first proves the reactive-runtime and styling machinery against a real target years
before Umbra Engine can host it, and gives Pharos/Dawn a shared authoring experience with the
in-game UI that will eventually use the same language.

## 2. The core decision: one Iris Core, capability-gated backend extensions

Not two DSLs, and not silent unavailability. Concretely:

- **Iris Core** — backend-agnostic — is everything already in the original design that
  doesn't depend on a specific engine: primitives (`frame`, `inline`, `grid`), the
  component model (props, state, events, composition, conditionals/loops in `render`),
  imports, and Lustre-lite styling. *(Superseded — see the note above. Post-pivot, Iris Core
  is only the `render` block grammar, `import`, and backend-capability tagging; props, state,
  events, and control flow are host-language, not Iris-defined. See
  `docs/iris_core_spec.md` v2.)*
- **Backend capabilities** — features that only one backend can support are tagged, and
  referencing a capability the compile target doesn't have is a **compile-time error**, not
  a runtime no-op. Example: `<model3d>` requires the `umbra-engine` backend; compiling an
  Iris file that uses it with `--target penumbra` fails at compile time with a clear message.
- This is a well-trodden pattern, not a novel risk: React Native vs React DOM host
  components, Kotlin Multiplatform `expect`/`actual`, SwiftUI `#if os()`.
- Two backends to design for: `penumbra` (now) and `umbra-engine` (later, deferred — see
  §6 Stage 6). Nothing in Stages 0–5 below should depend on `umbra-engine` existing.

## 3. Why Penumbra first

- It's real and working today, not speculative.
- Two consumers are already lined up: Pharos (existing), Dawn (upcoming, Umbra's own level
  editor — eating Iris's own dog food inside the Umbra ecosystem before the game-facing
  backend exists).
- Penumbra's own spec already anticipated a themed middle tier it calls
  `UmbraComponentLibrary`, sitting between raw Penumbra widgets and a real consuming tool —
  Iris-on-Penumbra is a natural fit for that reserved slot.
- Penumbra's recent styling work (gradients, radial gradients, drop shadows, blend modes —
  see `docs/penumbra_theming_requirements.md`, `docs/penumbra_glow_gradient_requirements.md`)
  already gives Lustre-lite a real, modern token surface to target, not just flat colors.

## 4. What Iris needs to build regardless of backend

These are hard parts of Iris itself — no backend choice avoids them:

- **Reactive state + reconciliation.** State (now a host-language `iris::Signal<T>` per the
  post-pivot model — see superseding note above) and conditional rendering imply diffing a new
  render against the previous one and applying minimal mutations — a real reconciler, not just
  a tree builder.
- **Stable element identity (`key`).** Not present in the original Iris design doc. A
  reconciler needs a way to match new elements to existing instances across re-renders
  (lists, conditionals) — this needs to be added to Iris Core's prop model at the language
  design stage (Stage 0), not bolted on later.
- **Lustre-lite style resolution.** Global + component-scoped precedence, class selectors,
  resolving down to concrete values per backend. This doc doesn't attempt to design Lustre
  itself — flagged as a dependency, not solved here.
- **Module/import resolution.** `import HealthBar` → file search path → component graph.
  Backend-independent; needed before any backend can compile anything.

## 5. What the Penumbra backend specifically needs

> **Updated post-Stage-2-scoping, verified against the real `penumbra-proto` source** (this
> section originally described what Penumbra *would need*; several items below have since
> actually landed there — see `docs/iris_stage2_decision_doc.md` and
> `docs/iris_stage2_open_questions.md` for the full trail). One claim below was wrong even at
> the time it was written — see the event-props bullet.

- A codegen pass that walks a parsed, props-resolved Iris component tree — represented as a
  backend-agnostic `IrisComponent` IR, not Penumbra-specific types
  (`docs/iris_core_spec.md` §2.5) — and constructs a real Penumbra widget tree via each
  widget's fluent `Builder` API (`Box::Builder`, `Label::Builder`, `Image::Builder`,
  `InlineContainer::Builder`), which now exists in `penumbra-proto`. The original hand-built
  imperative pattern in `demo/main.cpp` (`make_unique` → assign fields → `AddChild`) is what
  proved this mapping was viable before the `Builder` API existed to make it declarative.
- Because re-renders must eventually produce mutations, not full rebuilds, the Penumbra backend
  runtime retains a map from Iris element identity (`key`, or a generated position-based id) →
  live widget, matching Penumbra's own model of a persistent, mutate-in-place tree that re-runs
  layout every frame. Stage 2 already builds this map (`docs/iris_stage2_decision_doc.md` §8)
  even though nothing reads it until Stage 3. **Correction, per Stage 3 scoping:** the map's
  value type is `IWidget*` — a new backend-agnostic interface — never a concrete Penumbra type
  like `WidgetBase*`. The Iris runtime must not reference Penumbra types directly anywhere, not
  just at the `IrisComponent` IR level. See `docs/iris_core_spec.md` §10,
  `docs/iris_stage3_decision_doc.md` §4.
- **Correction:** this section previously claimed event props "map directly onto Penumbra's
  existing `std::function` callback members (`Button::OnClicked`, `Checkbox::OnChanged`) — no
  new binding mechanism needed... this part already lines up cleanly." That was wrong — verified
  by reading the code as of Stage 2 planning, `WidgetBase`/`Box` had **no** generic callback
  mechanism at that time; `Button`/`Checkbox`'s callbacks were one-off additions to those two
  widgets specifically, insufficient for "any element can have `onPress`." This has since been
  built: `WidgetBase` now carries five generic, null-by-default `std::function<void()>` members
  (`OnPressed`/`OnReleased`/`OnHovered`/`OnFocused`/`OnChanged`), reachable via every primitive's
  `Builder::onPress()` etc. See `docs/iris_core_spec.md` §4 for the current, verified state.
- **Resolved, ahead of schedule:** the child-mutation gap below landed alongside the Stage 2
  Penumbra work, even though it was correctly scoped as Stage-3-relevant, not a Stage 2
  blocker — `docs/penumbra_iris_backend_requirements.md` now reports all required items
  implemented (`Box::RemoveChild`/`ReplaceChild`/`ClearChildren`/`MoveChild`/`InsertChildAt`,
  `WidgetBase::GetChildCount`/`GetChildAt`).
- **Resolved:** the `<Image>` gap flagged during initial Stage 2 grounding (Penumbra's first
  `Image` widget only drew a pre-decoded `SDL_Texture*`, no decode-from-path pipeline) has since
  been fixed by a follow-up Penumbra change — `Backends::IImageBackend`/`SdlImageBackend` now
  decode PNG/JPG via real `SDL_image`, and the widget was rebuilt as `ImageWidget` with a
  confirmed `src` content prop, documented in `penumbra-proto`'s own
  `docs/penumbra_image_widget_requirements.md`. See `docs/iris_core_spec.md` §3.1, §9.4 for
  full detail, including two implementation divergences from other primitives worth knowing
  (narrower `Builder`, explicit `LoadFrom` step).
- **Gap found during Stage 3 scoping, since resolved:** Stage 3's lifecycle hooks
  (`OnMount`/`OnUnmount`/`OnTick`) depend on an `IWidgetLifecycle` interface at
  `include/Penumbra/IWidgetLifecycle.h` — verified at the time directly against the pinned
  `vendor/penumbra` submodule commit (a path that existed in the `iris` repo then; the
  Penumbra checkout this now refers to lives in `iris-penumbra-backend`'s own submodule —
  repo/build integration was corrected after this was written, see
  `docs/iris_stage2_decision_doc.md`'s correction note) and confirmed absent at that commit
  (`f008666`). Unlike the child-mutation API (which landed ahead of schedule), this was a
  genuine unmet prerequisite for Stage 3's lifecycle feature specifically. `penumbra-proto`
  commit `663fece` ("Add IWidgetLifecycle interface and Application lifecycle host") has since
  landed it, matching the shape `docs/iris_stage3_decision_doc.md` §8 specified
  (`OnMount`/`OnUnmount`/`OnTick(TickInfo)`, plus an `Application` host dispatching `OnTick`).
  Everything Stage 3 needs from Penumbra (structural mutation, tree walking, prop-level mutable
  fields, lifecycle) is now there. See `docs/iris_core_spec.md` §10 and
  `docs/iris_stage3_decision_doc.md` §8, §10 for the exact interface shape.

## 6. Proposed phased roadmap

| Stage | Scope |
| --- | --- |
| 0 | Formalize the Iris Core language spec. *(Done — see `docs/iris_core_spec.md`. Post-pivot, scope narrowed to: `render`-block element-tree grammar, `import` resolution, the `key`/`class` reserved props, and backend-capability tagging; component/props/state/event model is host-language, not Iris-defined.)* |
| 1 | Front end: a preprocessor that detects `render { }` blocks in host-language (C++23) source, parses the element-tree grammar and `{ }` escape hatches inside them, and rewrites the file to valid host-language output — passthrough for everything outside `render { }`. Open questions being scoped in `docs/iris_stage1_open_questions.md`. |
| 2 | Penumbra backend, static slice first: build a real Penumbra widget tree once from a parsed, props-resolved `IrisComponent` IR tree via Penumbra's `Builder` API, no state/re-render yet. Implemented in the separate `iris-penumbra-backend` repo (depends on both `iris` and `penumbra-proto`), not in this repo — `iris` itself only ever produces the backend-agnostic IR. *(Scoped — see `docs/iris_stage2_decision_doc.md` for all ten planning decisions and `docs/iris_core_spec.md` §2.5–§2.6, §3 for what they mean for the language/primitive reference. Two Penumbra-side prerequisites — generic `WidgetBase` callbacks, `InlineContainer` — already landed; `<Image>`'s decode-from-path pipeline has not, see §8 there.)* |
| 3 | Reactive runtime: state, re-render triggers, the reconciler (diff + minimal mutation), consuming Penumbra's child-mutation API. *(Fully scoped — see `docs/iris_stage3_decision_doc.md` for the complete architecture: `<Slot>`-scoped diffing, `IWidget`/`IrisPropDiff` as the backend-agnostic update boundary, minimal-move keyed list diffing, batched `iris::Tick()` frame-loop integration, `IWidgetLifecycle` hooks. All Penumbra-side prerequisites — structural mutation, tree-walking, and (as of `penumbra-proto` commit `663fece`) `IWidgetLifecycle` — have landed; see `docs/iris_core_spec.md` §10 and §5 above. Nothing known is blocking Stage 3 implementation from the Penumbra side.)* |
| 4 | Lustre-lite: global + component-scoped style resolution mapped onto Penumbra's `BoxStyle`/`ButtonStyle`/etc. and its gradient/shadow/blend-mode primitives. |
| 5 | First real consumer: port a real slice of Pharos (or a new Dawn panel) to Iris — validates the pipeline against real UI, not a toy demo. |
| 6 (deferred) | Umbra Engine/Nyx backend, `model3d`, engine-driven routing — gated behind the Stage 0 capability system from day one so nothing in Stages 0–5 accidentally depends on it. |


# Stage 0 — Iris Core Language Specification

> **This section is historical.** It's kept as the record of Stage 0 scoping discussion and the
> reasoning behind decisions like PascalCase casing, the `key` prop, and `.iris.json`'s shape —
> all still accurate. But its component/props/state/event grammar (e.g. `component Button(props:
> ButtonProps) { render { ... } }`, `state { ... }`, arrow-function event handlers) predates the
> preprocessor pivot in `docs/iris_stage1_decision_doc.md` §8 and no longer reflects current
> Iris syntax. For the current language reference, use `docs/iris_core_spec.md` (v2).

---

## What this stage is

Pure design work. No compiler, no runtime, no code. The output is a written language
specification that every subsequent stage builds against. A wrong decision here costs a
rewrite of the front end, the reconciler, and the backend mapping simultaneously — getting
the language shape right is the highest-leverage work in the entire project.

**Done when:** a reader who has never seen Iris can write a correct, non-trivial component
from the spec alone, and a reader implementing the Stage 1 parser knows exactly what grammar
to target.

---

## Decisions resolved during Stage 0 scoping

The following were open questions. They are recorded here as closed so they do not re-open
during spec writing or implementation.

### Casing convention

All elements — primitives and components alike — are **PascalCase**. The original design
used lowercase for primitives (`<frame>`, `<inline>`, `<grid>`) to distinguish them from
components visually. That distinction is better communicated by context and tooling than by
casing, and mixed conventions are harder to read at a glance.

The rule is: if it appears between angle brackets, it is PascalCase. Whether it is a
built-in primitive or an imported component is a compiler concern, not a casing concern.

```iris
// Primitives
<Frame>
<Inline>
<Grid>
<Image>
<Text>

// Components (imported)
<HealthBar />
<Button />
<SettingsPage />
```

---

### Primitive set

The Iris Core primitive set is the minimum that every backend is required to implement.
A primitive is Core if it is a fundamental building block of any UI regardless of context.
If it requires engine systems — 3D rendering, asset management pipelines, animation rigs,
game state — it is backend-gated, not Core.

**Core primitives (all backends):**

| Primitive | Purpose |
| --- | --- |
| `<Frame>` | General purpose block container. The primary layout primitive. Equivalent to `<div>` in HTML. |
| `<Inline>` | Inline element. Equivalent to `<span>` in HTML. |
| `<Grid>` | Grid-based layout container. |
| `<Image>` | Renders an image from a file path. |
| `<Text>` | Renders a text string. Font specified via Lustre. |

`<Image>` and `<Text>` are Core because every UI needs images and text regardless of
backend. The Penumbra backend is responsible for implementing a minimal asset pipeline
sufficient to load image files from disk (PNG/JPG → SDL texture) and render text via
SDL_ttf. Full asset management — caching, hot reload, async loading — is an Umbra Engine
concern and does not belong in Penumbra.

**Backend-gated primitives (declared in `.iris.json`):**

| Primitive | Required backend |
| --- | --- |
| `<Model3d>` | `umbra-engine` |

Using a backend-gated primitive when the project's `.iris.json` declares a different target
is a **compile-time error**, not a runtime no-op.

---

### Interactivity model

Event props (`onPress`, `onRelease`, `onHover`, `onFocus`, `onChange`) are valid on **any
element**. A `<Frame>` with an `onPress` prop is an interactive frame. There is no separate
interactive primitive or opt-in attribute.

```iris
component Button(props: ButtonProps) {
    render {
        <Frame onPress={props.onPress}>
            <Inline>{props.label}</Inline>
        </Frame>
    }
}
```

This is the intentional model: a button is a frame with interactivity, the same way a
`<div>` with an `onClick` handler is a button in HTML. The developer handrolls interactive
elements from `<Frame>` upward. Lustre handles hover and press styling via state selectors
— not inline props.

The Penumbra backend implements this by adding optional input callbacks to `WidgetBase`.
A `Box` with no event props set behaves exactly as today — inert. The callbacks are null
by default and only populated when the Iris backend sets them.

---

### `key` prop

`key` is a **reserved prop name**, valid on any element. It is stripped by the compiler
before codegen — the backend never sees it. It exists exclusively for the reconciler to
establish stable element identity across re-renders.

```iris
for (item in props.items) {
    <Frame key={item.id}>
        <Inline>{item.name}</Inline>
    </Frame>
}
```

Rules:
- `key` must be a string or integer **literal or variable** — not a dynamic expression or
  function call result. The reconciler requires a stable, predictable value.
- `key` values must be **unique among siblings**. Global uniqueness is not required.
- `key` cannot be used as a component prop name. The compiler rejects any `props` struct
  that declares a field named `key`.
- The identity map from `key` → live widget instance is owned by the Iris runtime, not
  Penumbra. Penumbra requires no changes to support `key`.

---

### Backend capability tagging — project-level config

The target backend is declared once, at the **project level**, in an `.iris.json` file at
the project root. There are no per-file pragmas, per-import annotations, or per-element
attributes. The compiler reads `.iris.json` on startup and enforces capability rules across
the entire build.

```json
{
    "target": "penumbra",
    "version": "0.1.0",
    "searchPaths": [
        "src/ui"
    ]
}
```

Switching to `"target": "umbra-engine"` unlocks all `umbra-engine`-gated primitives
project-wide. This is the correct granularity: a project is either a Penumbra tool or an
Umbra Engine game UI — there is no meaningful use case for mixing both targets in a single
build.

`searchPaths` is also where module resolution lives. The import statement `import HealthBar`
resolves to `HealthBar.iris` found in one of the declared search paths, in declaration
order. This closes the module resolution question for Stage 1.

---

### Lustre and styling

Style is never inline in `.iris` files. `.lustre` files are the sole authoring surface for
visual properties. Iris Core does not define Lustre's syntax, cascade rules, or how Lustre
properties map to backend-specific style structs — that is Lustre's own design problem,
to be specified in a separate Lustre handoff doc when the time comes.

What Iris Core must specify:

- The `class` prop is valid on any element and accepts a string class name
- Class names are the join between an Iris element and its Lustre declarations
- No other style information appears in `.iris` files

```iris
// Correct — style lives in the paired .lustre file
<Frame class="health-bar-container">
    <Inline class="label">{props.label}</Inline>
</Frame>

// Never valid in Iris — no inline styles
<Frame style="background: red;">
```

---

## What the Stage 0 spec document must contain

Stage 0 produces a single written spec document. Its structure:

**1. Grammar** — prose description with short examples for every syntactic construct.
Formal BNF/EBNF is a Stage 1 concern when a parser author needs it; Stage 0 prose is
sufficient to design against.

**2. Component model** — component declaration, props structs, state blocks, event props,
composition, and the `render` block in full. The original Iris design doc
(`docs/iris_design.md`) already covers most of this and should be treated as the starting
draft, updated to reflect the decisions above (PascalCase, interactivity model, `key`).

**3. Primitive reference** — one entry per Core primitive defining accepted props,
accepted children, and backend implementation requirements. Backend-gated primitives are
listed separately with their required target declared.

**4. `.iris.json` reference** — all recognised fields, their types, and their defaults.

**5. Compiler error catalogue (starter)** — the errors Stage 0 decisions imply:
using a backend-gated primitive against the wrong target, using `key` as a prop name,
using inline style props, referencing an unimported component.

**6. Open questions** — anything Stage 0 deliberately does not resolve. Lustre cascade
rules, the full Penumbra asset pipeline spec, and the `umbra-engine` backend primitive set
beyond `<Model3d>` are all deferred. Record them here rather than blocking Stage 0 on
resolving them.

---

## What Stage 0 does not design

- Lustre syntax, cascade rules, or the mapping from Lustre to backend style structs
- The Penumbra asset pipeline beyond confirming it must exist and what it must support
- The `umbra-engine` backend beyond confirming `<Model3d>` as its first gated primitive
- Formal grammar notation (BNF/EBNF) — prose + examples is sufficient at this stage
- The reconciler algorithm — that is Stage 3 work
- Anything requiring `"target": "umbra-engine"` to compile

---

## A note on scope discipline

The temptation in Stage 0 is to keep designing rather than ship the spec and move to Stage
1. The right exit condition is: the spec is complete enough that Stage 1 can begin, not
complete enough that no questions remain. Lustre, the full asset pipeline, and the
`umbra-engine` backend are explicitly out of scope — record them as open questions in the
spec document and move on.
