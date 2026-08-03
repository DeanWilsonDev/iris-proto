# Iris — Architectural Decision Log

> **Status:** Post-planning. Records decisions made during Stage 0 and Stage 1 scoping
> conversations. Intended as a handoff to the agent maintaining `iris_handoff.md` and
> `iris_core_spec.md` — update those documents to reflect the decisions below.
>
> Decisions are recorded in the order they were made, with the reasoning behind each.
> Where a decision supersedes something in the original `iris_design.md`, that is noted
> explicitly.

---

## 1. Casing convention — all elements are PascalCase

**Decision:** All elements, both primitives and components, are PascalCase.

**Replaces:** The original `iris_design.md` used lowercase for primitives (`<frame>`,
`<inline>`, `<grid>`) to visually distinguish them from components. That convention is
dropped.

**Reasoning:** The distinction between a primitive and a component is a compiler concern,
not a casing concern. Mixed casing conventions are harder to read at a glance and the
visual distinction is better communicated by context and tooling. LSP tooling makes the
distinction obvious without encoding it in the name.

**Canonical primitive names:**
- `<Frame>` — general purpose block container
- `<Inline>` — inline element
- `<Grid>` — grid layout container
- `<Image>` — renders an image from a file path
- `<Text>` — renders a text string
- `<Model3d>` — 3D model container (umbra-engine backend only)

---

## 2. Primitive set — `<Image>` and `<Text>` are Core

**Decision:** `<Image>` and `<Text>` are Iris Core primitives, required to be implemented
by every backend.

**Reasoning:** A UI without images and text is not a UI. These are fundamental building
blocks regardless of context, not engine-specific features. The Penumbra backend will
implement a minimal asset pipeline to support them — PNG/JPG loaded from disk to SDL
texture, and text rendered via SDL_ttf. Full asset management (caching, hot reload, async
loading) is an Umbra Engine concern and does not belong in Penumbra.

**Implication for Penumbra:** Penumbra needs a minimal asset pipeline. It does not need
to be sophisticated — just sufficient to load image files and fonts from a path without
crashing. This is a Penumbra implementation concern, not an Iris language concern.

---

## 3. `<Model3d>` is umbra-engine gated

**Decision:** `<Model3d>` requires `"target": "umbra-engine"` in `.iris.json`. Using it
against any other target is a compile-time error.

**Reasoning:** 3D model rendering requires engine systems — asset pipelines, animation
rigs, Nyx-driven animation state — that do not exist in Penumbra. It is the clearest
example of a backend-gated primitive and serves as the canonical first use case for the
capability system.

---

## 4. Interactivity model — event props are valid on any element

**Decision:** Event props (`onPress`, `onRelease`, `onHover`, `onFocus`, `onChange`) are
valid on any element. A `<Frame>` with an `onPress` prop is an interactive frame. There
is no separate interactive primitive or opt-in attribute.

**Reasoning:** A button is a frame with interactivity, the same way a `<div>` with an
`onClick` is a button in HTML. Developers should be able to handroll interactive elements
from `<Frame>` upward. Lustre handles hover and press styling via state selectors — not
inline props.

**Implication for Penumbra:** `WidgetBase` needs optional input callbacks. A `Box` with
no event props set behaves exactly as today — inert. Callbacks are null by default and
only populated when the Iris backend sets them. This is a small, clean addition to
Penumbra — not a structural change.

**Note:** Event handler bodies are the host language's responsibility (see §8 below).
`onPress={[]() { settingsOpen.set(true); }}` — the lambda is C++23, passed through
verbatim by the Iris preprocessor.

---

## 5. Lustre and styling — deferred to its own handoff doc

**Decision:** Lustre is a separate project and will get its own handoff document when the
time comes. Iris Core does not define Lustre's syntax, cascade rules, or how Lustre
properties map to backend-specific style structs.

**What Iris Core must specify:**
- The `class` prop is valid on any element and accepts a string class name
- Class names are the join between an Iris element and its Lustre declarations
- Inline styles are never valid in `.iris` files

**What is explicitly deferred:** Lustre cascade rules, the mapping from Lustre class
declarations to `BoxStyle`/`ButtonStyle` etc., global vs. component-scoped precedence.
These are Lustre's design problem, not Iris's.

---

## 6. `key` prop — reserved prop name, not a keyword

**Decision:** `key` is a reserved prop name, valid on any element. It is stripped by the
Iris preprocessor before codegen — the backend never sees it. It exists exclusively for
the reconciler to establish stable element identity across re-renders.

**Rules:**
- `key` must be a string or integer literal or variable — not a dynamic expression or
  function call result. The reconciler requires a stable, predictable value at diff time.
- `key` values must be unique among siblings. Global uniqueness is not required.
- `key` cannot be used as a component prop name. The preprocessor rejects any props
  struct that declares a field named `key`.
- The identity map from `key` → live widget instance is owned by the Iris runtime.
  Penumbra requires no changes to support `key`.

---

## 7. Backend capability tagging — project-level `.iris.json`

**Decision:** The target backend is declared once, at the project level, in an `.iris.json`
file at the project root. There are no per-file pragmas, per-import annotations, or
per-element attributes.

**Reasoning:** A project is either a Penumbra tool or an Umbra Engine game UI — there is
no meaningful use case for mixing both targets in a single build. Project-level config is
the correct granularity. This matches how every modern toolchain handles target
configuration: TypeScript's `tsconfig.json`, Rust's `Cargo.toml`, Swift's build settings.

**`.iris.json` format:**

```json
{
    "target": "penumbra",
    "version": "0.1.0",
    "searchPaths": [
        "src/ui"
    ]
}
```

**Valid target values:** `"penumbra"`, `"umbra-engine"` (further backends added as they
are implemented).

**`searchPaths`** is also where module resolution lives. `import HealthBar` resolves to
`HealthBar.iris` found in one of the declared search paths, in declaration order.

---

## 8. Architectural pivot — Iris is a preprocessor, not a language

**This is the most significant decision made during planning. It supersedes large parts
of `iris_design.md` and `iris_handoff.md`.**

### What changed and why

The original Iris design assumed:
- Inside `render { }` — Iris's JSX-style declarative syntax
- Outside `render { }` — Nyx (props structs, state, event handlers, component declaration)

Nyx does not exist yet. Rather than design a placeholder language to fill the outer layer,
the decision was made to use **C++23 as the host language** for all non-render content.
This was then pushed further: if C++23 handles the outer layer, how much of what was
originally Iris's responsibility can also move to C++23?

The answer turned out to be: almost everything except the element tree itself.

### What Iris owns

- `render { }` blocks — the Iris preprocessor finds these and rewrites the element tree
  inside them into C++23 `Component` construction calls
- PascalCase element tags inside render blocks — primitives or imported components
- `{ }` escape hatches inside render blocks — balanced brace match, contents emitted
  verbatim as C++23
- Prop syntax on elements — `class="foo"`, `key="bar"`, `onPress={...}`
- `import` statements — for resolving component files at compile time

### What the host language (C++23) owns

- Component declaration — a component is a C++23 function returning `Component`
- Props — C++23 structs with standard member types and default initialisers
- State — C++23 member variables, wrapped in `iris::Signal<T>` for reconciler notification
- Event handlers — C++23 lambdas passed as props
- All control flow — conditionals, loops, data transformation
- All expressions — comparisons, boolean logic, arithmetic, function calls
- All type definitions — `string`, `int`, `f32`, `bool` are C++23 types
- Comment syntax — C++23 `//` and `/* */`

### What this means in practice

A `.iris` file is a C++23 file where `render { }` is a legal construct. The Iris
preprocessor transforms it into a valid `.cpp` file by rewriting render blocks. The C++23
compiler then compiles the output normally and validates all expressions, types, and
lambdas.

```cpp
// What you write in a .iris file
Component HealthBar(HealthBarProps props) {
    render {
        <Frame class="health-bar-container">
            <Inline class="label">{props.label}</Inline>
        </Frame>
    }
}

// What the Iris preprocessor emits as .cpp
Component HealthBar(HealthBarProps props) {
    return Frame("health-bar-container")
        .child(Inline("label").child(props.label));
}
```

Control flow, conditional rendering, and list rendering are handled by the host language
via expression blocks `{ }` that return `Component`:

```cpp
Component StartMenu() {
    iris::Signal<bool> settingsOpen = false;

    render {
        <Frame class="start-menu">
            <Button label="Settings" onPress={[&]() { settingsOpen.set(true); }} />
            { [&]() -> Component {
                if (settingsOpen.get()) {
                    return <SettingsPage onClose={[&]() { settingsOpen.set(false); }} />;
                }
                return nullptr;
            }() }
        </Frame>
    }
}
```

### Keyword list

Iris's keyword list is reduced to almost nothing as a result of this pivot:

| Token | Type | Notes |
|---|---|---|
| `render` | Keyword | Marks the block the Iris preprocessor owns |
| `import` | Keyword | Resolves component files at compile time |
| `key` | Reserved prop name | Stripped before codegen, never reaches the backend |
| `class` | Reserved prop name | Style bridge to Lustre |

Primitive tag names (`Frame`, `Inline`, `Grid`, `Image`, `Text`) are not lexer-level
reserved words — they are PascalCase identifiers the compiler resolves semantically.
This keeps the lexer simple.

---

## 9. File extensions

**Decision:**

| Extension | Host language | Status |
|---|---|---|
| `.iris` | C++23 | Current |
| `.irisx` | Nyx | When Nyx exists |

**Reasoning:** When Nyx is ready, porting a component is a mechanical translation of the
outer layer plus a file rename. The `render { }` block is copied verbatim — it is
identical in both file types. The two extensions can coexist in the same project during
a migration period.

The extension is the single source of truth for which host language the compiler expects.
No `hostLanguage` field is needed in `.iris.json`.

---

## 10. State and re-renders — `iris::Signal<T>`

**Decision:** State is C++23 member variables wrapped in `iris::Signal<T>`. Mutation
notifies the Iris runtime to re-run the component's render function and reconcile the
output against the live widget tree.

**Reasoning:** Since Iris owns only the tree, re-render triggering must come from
somewhere. Signals are the right mechanism — they keep the host language in full control
of state while giving the Iris runtime a cheap, explicit notification that something
changed. This is preferable to re-diffing every component every frame.

**Penumbra note:** Penumbra is retained mode and re-renders every frame regardless.
For the Penumbra backend, signals do not trigger a visual re-render — they trigger
reconciliation. When a signal fires, the Iris runtime re-runs the component's render
function, diffs the new `Component` tree against the previous one, and applies
minimal mutations to the live Penumbra widget tree. Penumbra then reflects those
mutations in its next frame automatically.

This mechanism works identically on the future Umbra Engine backend — the backend
difference is transparent to the host language author.

---

## 11. Questions closed from `docs/archive/iris_stage1_open_questions.md`

For the agent updating the Stage 1 open questions doc — all eight questions are now
closed. Summary:

| Question | Resolution |
|---|---|
| 1. Compiler language | C++23 — consistent with the rest of the Umbra ecosystem |
| 2. Generics vs element-start ambiguity | Resolved — C++23 handles generics outside render blocks; `<` inside render blocks is always an element start |
| 3. Expression grammar depth | Resolved — no Iris expression grammar; all expressions are C++23 inside `{ }` escape hatches |
| 4. Reserved keyword list | Resolved — `render`, `import`, `key`, `class` only |
| 5. Optional/default prop values | Resolved — C++23 struct default member initialisers handle this natively |
| 6. `if`/`else` | Resolved — host language handles all control flow |
| 7. Numeric literal syntax | Resolved — C++23 literal syntax, Iris never sees numerics |
| 8. Block comments | Resolved — C++23 `//` and `/* */` work natively |
