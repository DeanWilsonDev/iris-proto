# Iris Core — Stage 0 Language Specification (v2 — preprocessor architecture)

> **Status:** Stage 0 deliverable per `docs/iris_handoff.md`, revised to reflect the
> architectural pivot recorded in `docs/iris_stage1_decision_doc.md` §8: **Iris is now a thin
> preprocessor over a host language, not a standalone DSL.** This is a substantial rewrite of
> v1 of this document, not an incremental edit — most of v1's grammar (component declaration,
> props structs, `state` blocks, `if`/`for`, arrow-function event handlers) no longer exists as
> Iris syntax. It is now ordinary host-language (C++23) code. Iris Core, as of this revision, is
> just: `render { }` block detection, the JSX-style element-tree grammar inside it, `{ }` escape
> hatches, `import` resolution, and backend-capability tagging.
> **Sources, in precedence order:** `docs/iris_stage3_decision_doc.md` (newest — full Stage 3
> reconciler/runtime architecture, closes all ten Stage 3 questions plus a correction to Stage 2)
> > `docs/iris_stage3_decision_slot.md` (introduces `<Slot>`, closing Stage 3's foundational
> component-lifetime question) > `docs/iris_stage2_decision_doc.md` (Stage 2/Penumbra backend
> decisions, cross-checked against the real Penumbra source, not just its own prose) >
> `docs/iris_stage1_decision_doc_pt2.md` (closes every remaining Stage 1 implementation
> question) > `docs/iris_stage1_decision_doc.md` (architectural pivot) > `docs/iris_handoff.md`
> (multi-backend pivot) > `docs/iris_design.md` (original draft, mostly superseded now). Every
> point where this revision overrides v1 of this spec is in §9.1; Stage 2 changes are in §9.3;
> `<Slot>` is in §9.5; the Stage 3 runtime architecture is summarized in §10.

---

## 0. Overview

A `.iris` file is a valid host-language source file with one Iris-specific extension: it may
contain `render { }` blocks. The Iris preprocessor finds those blocks, parses the JSX-style
element tree inside them, and rewrites the file into valid host-language source (e.g. `.cpp`),
which the host compiler then compiles completely normally — types, expressions, and lambdas are
validated by that compiler, not by Iris.

Today the host language is **C++23** (matching Penumbra). When Nyx exists, the same mechanism
works against a `.irisx` file with Nyx as the host language (§6) — the `render { }` block itself
is copied verbatim between the two; only the surrounding host code differs.

The three-layer model, updated for the pivot:

| Layer | Owner | Responsibility |
| --- | --- | --- |
| Element tree (inside `render { }`) | Iris preprocessor | JSX-style UI structure |
| Everything else — component declaration, props, state, events, control flow, expressions | Host language (C++23 now; Nyx via `.irisx` later) | Behavior and logic |
| Style + transitions | Lustre (`.lustre`) | Appearance and animation |
| Widget instantiation / rendering | Penumbra (now) or Umbra Engine (later) | What's actually on screen |

Backend-specific features are still capability-gated, not silently unavailable — see §3.2 and
§4.

### Design goals

1. **Composition first.** UI is built from small components composed into larger ones — still
   true; composition happens inside `render { }` exactly as before (§2.4).
2. **Plain text.** No editor, no drag-and-drop, no XML scene files.
3. **Separation of concerns.** Structure lives in `.iris`/`.irisx`; style lives in `.lustre`,
   never inline (§4).
4. **Dumb components, smart pages.** Unchanged in spirit — now expressed as an ordinary
   host-language convention (which functions hold `iris::Signal` state vs. which are pure) rather
   than an Iris-enforced rule, since Iris no longer parses component bodies outside `render { }`.
5. **Minimal own surface area.** New goal, direct consequence of the pivot: Iris should own as
   little syntax as possible. If something can be expressed in the host language without Iris
   needing to understand it, it is — see §8 of the decision doc for the full reasoning.

### File model

| Extension | Host language | Purpose |
| --- | --- | --- |
| `.iris` | C++23 | Current. UI component: host code plus `render { }` blocks. |
| `.irisx` | Nyx | Future, once Nyx exists. Same `render { }` grammar, different host. |
| `.lustre` | — | Component-scoped or global stylesheet. |
| `.iris.json` | — | Project-level config: target backend, version, module search paths (§5). |

The **file extension is the sole source of truth for which host language a file uses** — there
is no `hostLanguage` field in `.iris.json`. The two extensions can coexist in the same project
during an eventual Nyx migration. Each component's file name matches its function name
(`HealthBar.iris` defines a function `HealthBar`).

### Routing and host integration — still not an Iris concern

Unchanged from v1: Iris components have no knowledge of routing or which component is active.
On Penumbra, the host application decides what to mount; on Umbra Engine (deferred —
`docs/iris_handoff.md` §6), Nyx's state machine decides. Out of scope for Iris regardless of
backend or host language.

---

## 1. Grammar

Iris's own grammar is now small enough to describe completely in this one section. Formal
BNF/EBNF is still deferred to Stage 1 implementation, but there's markedly less to formalize
than v1 of this spec assumed.

### 1.1 File structure

A `.iris` file is host-language (C++23) source. The preprocessor makes exactly two passes over
it conceptually: find `import` statements (§1.2), and find `render { }` blocks (§1.3) — every
other token is passed through to the emitted output unchanged.

```cpp
import Button
import SettingsPage

Component StartMenu() {
    IRIS_SIGNAL(bool, settingsOpen, false);

    render {
        <Frame class="start-menu">
            <Button label="Settings" onPress={[&]() { settingsOpen.set(true); }} />
            <Slot>
                !{[&]() -> Component {
                    if (settingsOpen.get()) {
                        return <SettingsPage onClose={[&]() { settingsOpen.set(false); }} />;
                    }
                    return nullptr;
                }}
            </Slot>
        </Frame>
    }
}
```

Component declaration, the `Component` return type, and `iris::Signal<T>` are all ordinary
C++23 — nothing here is Iris grammar except the `import` line and the `render { }` block's
contents (which now includes `<Slot>`, a Core primitive like any other — §3.1, §1.5).
`StartMenu()` runs exactly once, at mount — the lambda inside `<Slot>` is what re-runs later,
not this whole function (§2.2).

### 1.2 Imports

```
import HealthBar
import Button
import SettingsPage
```

Unchanged from v1: `import Name` resolves to `Name.iris` (or `Name.irisx` in a Nyx-targeted
project) searched across `searchPaths` declared in `.iris.json` (§5), in declaration order. This
is one of only two real Iris keywords (the other is `render`, §1.3).

### 1.3 The `render` block

```cpp
render {
    <Frame class="hud-row">
        <Frame class="icon-container">
            <Image src="assets/icons/health.png" />
        </Frame>
        <HealthBar current={player.health} max={player.maxHealth} />
    </Frame>
}
```

`render { ... }` is the marker the preprocessor scans for. On a match, it switches from
passthrough mode into Iris's element-tree grammar (§1.4) until the block's closing brace, then
resumes passthrough.

Detection and brace balancing (§1.4's escape hatches) both run through a single abstraction: an
`IHostLanguageTokenizer` interface that tokenizes the outer host layer with awareness of that
language's string/char/comment syntax, so a stray `{`/`render`-looking-text inside a string
literal or comment can't cause a false match. The concrete implementation is selected by file
extension at startup — `CppTokenizer` for `.iris`, and eventually `NyxTokenizer` for `.irisx`.
The preprocessor core itself contains no host-language-specific lexical rules, which is what
keeps `render { }`'s grammar identical across host languages when Nyx arrives. Every `Token` the
tokenizer produces carries a `SourceLocation`, which is also what makes the source-mapping in §6
possible without extra infrastructure.

**Rule, unchanged from v1: a `render` block has exactly one root element**, matching Penumbra's
single-root `Box` tree model. A `render` block yielding more than one top-level sibling is a
preprocessor-level compile error (§7).

### 1.4 Elements, props, and the `{ }` / `!{ }` escape hatches

```cpp
<Frame class="button" onPress={[&]() { props.onPress(); }}>
    <Inline class="button-label">{props.label}</Inline>
</Frame>
```

- Element tags are PascalCase. Per the decision doc §8, tag names are **not lexer-level
  keywords** — `Frame`, `Inline`, `Grid`, `Image`, `Icon`, `Text` (§3) and any imported component name
  are ordinary PascalCase identifiers the preprocessor resolves semantically against Core
  primitives and `import`ed names.
- `class` and `key` are the two Iris-reserved prop names (§2.3, §4). Both accept either a string
  literal (`class="button"`, `key="bar"`) or a `{ }` escape hatch (`class={isActive ?
  "button-active" : "button-inactive"}`, `key={item.id}`) — there is no restriction limiting
  either to literals; both are treated identically to any other prop value.
- Every other prop value, and every child position that isn't a nested element, is a **`{ }`
  escape hatch**: the preprocessor does a balanced-brace match from the opening `{` to its
  closing `}` (via the tokenizer in §1.3, so string/char literals and comments inside the escape
  hatch can't desync the brace count) and emits everything inside **verbatim** as host-language
  code. It does not parse or validate the contents — that's the host compiler's job, once the
  rewritten file reaches it.
- This is how event handlers (`onPress={[&]() { ... }}`), interpolated text (`{props.label}`),
  and — via `<Slot>` (§1.5) — conditional/list rendering all work: they are all just escape
  hatches containing C++23 expressions, whether that expression is evaluated once (everything
  outside `<Slot>`) or held as a callable the runtime re-invokes (`<Slot>`'s child specifically).
  There is no longer a separate "expression interpolation" construct or a restricted
  event-handler grammar — v1's
  single-assignment/single-call limitation on event handlers is gone; any valid C++23 lambda
  works.
- **`!{ }` — the JSX-transform escape hatch** (`docs/iris_escape_hatch_decision.md`): identical
  to `{ }` except that its contents are *not* opaque — the preprocessor recursively parses any
  `<Tag>` run inside it (whitespace-preceded, to tell a real element start apart from a template
  argument list like `std::vector<Component>`) back into the same element-tree grammar this
  section describes, and codegen transforms it the same way it would a top-level element. This
  is required inside `<Slot>` (§1.5), whose whole purpose is conditional/list rendering via
  nested JSX — a plain `{ }` there would leave that nested JSX as unparsed, uncompilable text. A
  nested `{ }` or `!{ }` inside a `!{ }` body (e.g. an event handler prop on a JSX-transformed
  element) follows the normal rule for whichever sigil it uses.

### 1.5 Dynamic regions — `<Slot>`

**Superseded by `docs/iris_stage3_decision_slot.md`** — a bare child-position escape hatch
returning `Component`/`std::vector<Component>` directly, as earlier drafts of this spec
showed, is no longer the mechanism for conditional or list rendering. `<Slot>` is.

Every ordinary escape hatch (§1.4) is **one-shot**: evaluated once, at construction, its value
used immediately and never revisited. That's fine for values that don't need to change on their
own — text interpolation, a prop, an event handler set once on a widget. It is not fine for
content that needs to change in response to a signal firing later, since nothing would ever
re-run it. `<Slot>` is the one construct where re-invocation happens:

```cpp
// Conditional rendering
<Slot>
    !{[&]() -> Component {
        if (settingsOpen.get()) {
            return <SettingsPage onClose={[&]() { settingsOpen.set(false); }} />;
        }
        return nullptr;
    }}
</Slot>

// List rendering
<Slot>
    !{[&]() -> std::vector<Component> {
        std::vector<Component> result;
        for (auto& item : props.items) {
            result.push_back(<Item key={item.id} label={item.name} />);
        }
        return result;
    }}
</Slot>
```

**`<Slot>`'s single child is a `!{ }` JSX-transform escape hatch** — the callable's body always
contains the nested `<Tag>` JSX that conditional/list rendering exists to produce, so `{ }`
(opaque) would leave that JSX unparsed and uncompilable; `!{ }` (§1.4,
`docs/iris_escape_hatch_decision.md`) recursively transforms it. Structurally it's still parsed
exactly like any other child position on a Core primitive tag (§3.1) parsed like `<Frame>` or
`<Text>` — `<Slot>` gets no special-cased grammar. What's different is what the escape hatch's
content *is*: not an evaluated value, but a **callable** — note there's no trailing `()` after
the lambda, unlike the old IIFE pattern this replaces. The lambda itself is what the escape
hatch evaluates to; the Iris runtime holds onto it and invokes it — once at mount, and again
every time a signal it captures fires. A `<Slot>` whose body needs no nested JSX (rare, but not
disallowed) may still use plain `{ }`.

The runtime, not the preprocessor, enforces: `<Slot>` must have exactly one child; that child
must be a callable returning `Component` or `std::vector<Component>`; anything else
(multiple children, a non-callable child) is a runtime error, not a compile-time one — consistent
with the preprocessor's stated boundary of never inspecting escape-hatch contents (§1.4).

**The backend never sees a `<Slot>` node.** The runtime resolves each `<Slot>` — invoking its
callable and substituting the result — before any backend-mapping pass (Stage 2's Penumbra
walker, or a future Nyx equivalent) walks the tree. A backend implementer never has to know
`<Slot>` exists.

### 1.6 Comments

Outside `render { }`, comments are whatever the host language defines — C++23 `//` and `/* */`
both work natively, since that code is passthrough. **Inside a `render { }` block, between
elements**, `//` and `/* */` are also valid and are stripped silently by the preprocessor before
codegen — this comes for free from the `IHostLanguageTokenizer` (§1.3), which already has to
understand comment syntax to detect `render {` and balance escape-hatch braces correctly.

```cpp
render {
    <Frame class="start-menu">
        // Renders the settings button
        <Button label="Settings" onPress={[&]() { settingsOpen.set(true); }} />

        /* Conditionally render the settings page */
        { [&]() -> Component {
            if (settingsOpen.get()) return <SettingsPage />;
            return nullptr;
        }() }
    </Frame>
}
```

---

## 2. Component model

Almost all of this section moved to the host language. What remains Iris's responsibility:

### 2.1 What the host language now owns

Per the decision doc §8, verbatim: component declaration (a component is a host-language
function returning `Component`), props (host-language structs, default member initializers
handle what v1 called "optional/default prop values"), state (host-language variables wrapped in
`iris::Signal<T>`, §2.2), event handlers (host-language lambdas), all control flow, all
expressions, all type definitions, and comment syntax. Iris does not define grammar for any of
these — they're whatever C++23 (or later, Nyx) already defines.

### 2.2 State and re-renders — `IRIS_SIGNAL`

```cpp
IRIS_SIGNAL(bool, settingsOpen, false);
```

State is a host-language local declared via the `IRIS_SIGNAL(Type, Name, InitExpr)` macro
(`include/Iris/ComponentInstance.h`), which expands to `Name` being a reference bound to a
heap-allocated `iris::Signal<Type>` — **not** `iris::Signal<Type> settingsOpen = false;`
declared directly, which was the original (and, per `docs/iris_signal_lifetime_decision.md`,
unsound) spelling. See that decision doc for why: a plain `Signal<T>` local's storage lives in
the declaring function's stack frame, and every use of it here happens through a `<Slot>`
callable that outlives that frame — capturing it `[&]`, exactly as every example in this
section does, is dangling-reference UB once the component function returns, which every
component function does immediately (§0's `return <expr>;` codegen convention). `IRIS_SIGNAL`
closes that gap structurally: `[&]` capture keeps working exactly as written everywhere in this
spec, because what's being captured is a reference to heap-stable storage, not a stack slot.

**Component functions run exactly once, at mount** — per
`docs/iris_stage3_decision_slot.md`, closing what was previously an open question about how a
plain local variable could survive being "re-run." A component function is not re-invoked on
every re-render the way earlier drafts of this spec implied; it executes a single time, and
every `IRIS_SIGNAL` declared as a local inside it is genuinely persistent for the component's
whole lifetime — owned by a heap-allocated `iris::ComponentInstance`
(`docs/iris_signal_lifetime_decision.md`) tied to that specific mounted component, not to the
declaring function's own stack frame, and freed automatically the moment that component leaves
the tree.

What *does* re-run is narrower and explicit: only the callables inside `<Slot>` (§1.5).
Mutating a signal (`settingsOpen.set(true)`) notifies the Iris runtime to re-invoke whichever
`<Slot>` callables captured it, and to reconcile each one's new result against what it produced
last time. On Penumbra specifically — which is retained-mode and redraws every frame regardless
— this reconciliation patches the live widget tree; Penumbra reflects the resulting mutations on
its next frame automatically. The reconciler's own diffing algorithm remains Stage 3 work still
being planned (`docs/iris_stage3_open_questions.md`); this section only fixes what triggers it
and what scope it runs over — `<Slot>`-produced regions, not the whole tree from the root.

### 2.3 The `key` prop

Unchanged in spirit from v1: `key` is a reserved prop name, valid on any element inside a
`render { }` block, stripped by the preprocessor before it emits host code — the backend never
sees it. It exists exclusively so the reconciler can establish stable element identity across
re-renders.

```cpp
<Frame key={item.id}>
    <Inline>{item.name}</Inline>
</Frame>
```

**Compile-time enforcement of `key` rules is dropped entirely — `key` is a runtime reconciler
responsibility** (per `docs/iris_stage1_decision_doc_pt2.md` §4, superseding the stricter v1/
decision-doc-pt1 rules):
- `key`'s value is just a `{ }` escape hatch like any other prop (§1.4) — the preprocessor does
  not restrict it to literals or variables; any C++23 expression is accepted, since the
  preprocessor never inspects escape-hatch contents beyond balancing braces.
- Uniqueness among siblings is not statically checked. The reconciler warns at runtime when two
  elements land at the same tree position without distinct keys — the same posture React takes:
  a missing/duplicate `key` in a list is a runtime warning, not a compile error.
- A props struct declaring a field literally named `key` is not a compiler error — Iris doesn't
  parse struct bodies (§8's stated boundary) so it has no way to see one. `key={...}` on an
  element is always intercepted and stripped by the preprocessor before that component's props
  are constructed, so such a field would simply be permanently unreachable via that path, not
  rejected.
- The identity map from `key` → live widget instance is owned by the Iris runtime, not the
  backend. Penumbra requires no changes to support `key`. **The map's value type is
  `IWidget*`, never a concrete backend type like `WidgetBase*`** — per
  `docs/iris_stage3_decision_doc.md` §0's Stage 2 implementation note, the Iris runtime must stay
  backend-agnostic throughout, not just at the `Component` IR level (§2.5). Penumbra's
  backend-mapping pass (§3) wraps each concrete widget it builds in a `PenumbraWidget : IWidget`
  adapter (§10) before it's stored anywhere in the runtime.

### 2.4 Composition

Unchanged mechanically: a `render { }` block nests other components (and primitives) as
children, passing props at each call site — exactly as in v1's examples. What changed is only
what's on either side of the tag: the props being passed in are constructed however C++23
constructs a struct, not via Iris-defined prop syntax.

```cpp
<Frame class="hud-row">
    <Frame class="icon-container">
        <Image src="assets/icons/health.png" />
    </Frame>
    <HealthBar current={player.health} max={player.maxHealth} />
</Frame>
```

Implicit children-forwarding for a generic wrapper component remains unspecified, same as v1 —
carried forward in §8.

### 2.5 `Component` — the backend-agnostic IR

Per `docs/iris_stage2_decision_doc.md` §4: `Component` (§1.1, §1.5) is a lightweight,
backend-agnostic **intermediate representation node**, not a live-widget facade. It carries a
tag, a resolved prop map, and an ordered list of child `Component` nodes — nothing about
Penumbra, `WidgetBase`, or any concrete widget type.

```cpp
struct Component {
    IrisElementTag Tag;             // Frame, Inline, Text, Image, Icon, or a component name
    IrisProps Props;                // key-value prop map, `key` already stripped
    std::vector<Component> Children;
};
```

(Illustrative only — the real, current shape in `include/Iris/Component.h` also carries
`SlotCallable` per §1.5/`docs/iris_stage1_codegen_decision.md`, and an `IrisElementTag::None`
sentinel plus `nullptr_t` converting constructor per §8's resolved `nullptr`-conversion gap.)

`render { }` blocks construct values of this IR type — they do **not** call directly into a
backend's widget/builder API. Between construction and backend mapping there is now a middle
step: the Iris runtime resolves every `Slot`-tagged node (§1.5, §3.1), invoking its callable and
substituting the result, so the tree a backend pass ever sees is fully concrete — no `Slot`
nodes, only Core primitives and components. Only *then* does a backend-specific pass walk the
resolved IR tree to build the real widget tree: the Penumbra backend (Stage 2) walks it using
Penumbra's Builder API (§3); when Umbra Engine/Nyx (Stage 6) exists, it walks the identical,
unchanged IR to build whatever Nyx needs. The IR shape itself never varies by backend, and
neither does the Slot-resolution step in front of it.

**Rejected alternative**, recorded for completeness: `Component` as a thin move-only facade
directly around `unique_ptr<WidgetBase>`, calling `AddChild` immediately as `.child()` is
invoked. Rejected because it ties Iris's core type to one backend's ownership model and leaves
nothing for Stage 3's reconciler to diff against — building Stage 2 on that model would mean
throwing it away once Stage 3 starts.

### 2.6 Component invocation and the `<Name>Props` convention

`<HealthBar current={player.health} max={player.maxHealth} />` must become a call to a
`HealthBar` function taking a `HealthBarProps`-shaped argument — but the preprocessor never
parses struct declarations (§2.1's stated boundary), so it has no way to discover a props
struct's real name from its fields. Per `docs/iris_stage2_decision_doc.md` §5:

**A component's props struct must be named `<ComponentName>Props`. This is a required naming
rule the preprocessor depends on, not a stylistic convention.** Violating it does not produce a
compiler error — the preprocessor has no way to detect the mismatch — it produces **incorrect
generated code**. This is the one place in Iris where breaking a naming convention silently
miscompiles rather than failing loudly, which is why it's called out here prominently rather
than left as an implied pattern.

Codegen branches on how a tag resolves (§1.4):
- **Core primitive** (`Frame`, `Inline`, `Grid`, `Image`, `Icon`, `Text`) → the preprocessor emits an
  `Component` IR node (§2.5) directly; no props-struct lookup happens.
- **Imported component name** → the preprocessor emits `Name(NameProps{ ...prop initializers...
  })` and wraps the result as this element's `Component` — relying on the `<Name>Props`
  rule above to know what type to construct.

---

## 3. Primitive reference

Unchanged from v1 except for the note in §1.4 that tag names are not lexer keywords.

### 3.1 Core primitives (all backends)

Every Core primitive below maps onto a Penumbra widget via that widget's fluent `Builder` class
(`Box::Builder`, `Label::Builder`, `ImageWidget::Builder`, `InlineContainer::Builder` — all in
`penumbra-proto`'s `include/Penumbra/Widgets/`). Most share the same method-naming convention,
verified against the actual shipped code: `className(string)` (for the `class` prop — named
`className` because `class` is a C++ reserved word, the one deliberate divergence from
mechanical prop-name matching), `child(unique_ptr<WidgetBase>)` /
`children(vector<unique_ptr<WidgetBase>>)`, and `onPress()`/`onRelease()`/`onHover()`/
`onFocus()`/`onChange()`, each taking a `std::function<void()>` — **zero-argument**. A generic
event prop never receives a payload (e.g. a toggle's new value); a handler that needs one reads
current state itself rather than accepting it as a callback argument. `key` has no `Builder`
method anywhere — confirming it truly never reaches the backend (§2.3). `ImageWidget::Builder` is
the one exception to the shared method set — see its entry below.

**`<Frame>`** — General-purpose block container; the primary layout primitive. Equivalent to
`<div>` in HTML.
- Props: `class`, `key`, any event prop. No primitive-specific props.
- Children: any elements, any number.
- Backend requirement: maps onto Penumbra's `Box` via `Box::Builder`.

**`<Inline>`** — Inline-flow container. Equivalent to `<span>` in HTML.
- Props: `class`, `key`, any event prop.
- Children: text interpolation (`{expr}`), nested elements — real inline-flow content, not just
  a single text run.
- Backend requirement: maps onto Penumbra's `InlineContainer` widget via
  `InlineContainer::Builder` — a real wrapping inline-flow layout (left-to-right, line-wraps by
  available width), distinct from `<Text>`/`Label` below. Per
  `docs/iris_stage2_decision_doc.md` §6, `<Inline>` and `<Text>` must never be treated as
  interchangeable: `<Inline>` is a container, `<Text>` is a leaf.

**`<Grid>`** — Grid-based layout container.
- Props: `class`, `key`, any event prop. Grid-layout-specific props (columns/rows/gap) remain
  undesigned — may end up a Lustre concern (§8).
- Children: any elements, any number.
- Backend requirement, per `docs/iris_stage2_decision_doc.md` §3: **deferred**. Penumbra has no
  grid layout mode at all today (`Box::Layout` is `None`/`VerticalStack`/`HorizontalStack`
  only). Stage 2 maps `<Grid>` onto a plain `Box` with `LayoutMode::HorizontalStack` as a stub —
  this explicitly does **not** meet the Core-primitive requirement yet; revisit when a real
  consumer needs actual grid layout.

**`<Image>`** — Renders an image from a file path.
- Props: `src` (string, required file path — confirmed as the actual prop name by Penumbra's own
  `ImageWidget::Builder::src()`, closing what was an open naming question), `class`, `key`. No
  event props — see below.
- Children: none (leaf).
- Backend requirement: maps onto Penumbra's `ImageWidget` (`include/Penumbra/Widgets/
  ImageWidget.h`) — **resolved**, verified against the landed code, not just the requirements
  doc. `ImageWidget` is a direct `WidgetBase` subclass (not a `Box`, unlike every other Core
  primitive here — no background/border/padding of its own, just the `class` hook via
  `WidgetBase::ClassName`). Image decoding goes through a new `Backends::IImageBackend`
  interface (mirroring `Render::IFontBackend`'s shape), concretely implemented by
  `SdlImageBackend` (`IMG_Load` + `SDL_CreateTextureFromSurface`, `SDL3_image` now a real
  `CMakeLists.txt` dependency) — the previously-missing decode-from-path pipeline now exists.
  Two implementation details Stage 2's tree-builder needs to account for, since they diverge
  from every other primitive: (1) `ImageWidget::Builder` deliberately exposes only `.src(string)`
  and `.className(string)` — no `child()`/`children()` (it's a leaf) and no event-prop methods,
  even though the underlying `WidgetBase::OnPressed`/etc. fields still exist and are dispatched
  by `ImageWidget::UpdateInteractionState` the same way `Box` does, so `<Image onPress={...}>`
  would need to set that field directly rather than through the builder chain if ever needed; (2)
  loading is **not** part of `build()` — the tree-builder must call `.LoadFrom(imageBackend,
  sdlRenderer)` on the built widget as a separate explicit step, since `Builder` has no access to
  a renderer/backend to load through.

**`<Icon>`** — Renders a single vector glyph resolved by name from a backend/app-supplied icon
catalog (`docs/penumbra_iris_lustre_componentization_gaps_requirements.md` §1).
- Props: `icon` (string, required — the catalog key, e.g. `icon="chevron-down"`), `class`, `key`.
  No event props, no `src`/`handle` — unlike `<Image>`, there is never a texture asset backing
  it.
- Children: none (leaf).
- Backend requirement: Iris carries no icon catalog itself — resolving `icon` to an actual drawn
  glyph is entirely backend-side (Penumbra's `IconWidget` + an app-supplied `IconBackend`
  resolving the name, mirroring `<Image>`'s own `IImageBackend` shape). A backend with no icon
  catalog wired up may leave the glyph undrawn, the same "build succeeds, nothing loaded"
  tolerance `<Image>` already has when `ImageBackend`/`SdlRenderer` are null.

**`<Text>`** — Renders a text string.
- Props: `class`, `key`, any event prop. **No `font` prop** — font is specified via Lustre.
- Children: `{expr}` interpolation or literal text.
- Backend requirement: maps onto Penumbra's `Label` widget via `Label::Builder`, which adds a
  `text(string)` method for the content on top of the shared set above. Penumbra renders text via
  SDL_ttf (`IFontBackend`/`SdlTtfFontBackend`).

`<Image>` and `<Text>` are Core because every UI needs images and text regardless of backend.

**`<Slot>`** — Marks a dynamic region; not a widget. Per
`docs/iris_stage3_decision_slot.md`, added to the Core primitive set alongside the others above.
- Props: `key` (per the general rule that `key` is valid on any element, §2.4), though rarely
  needed in practice since each `<Slot>` usage already has a distinct static source position.
  `class` is not applicable — see below.
- Children: exactly one — a `{ }` escape hatch whose content is a callable returning
  `Component` or `std::vector<Component>` (§1.5). Enforced by the runtime, not the
  preprocessor; anything else is a runtime error.
- Backend requirement: **none.** The Iris runtime resolves every `<Slot>` — invoking its
  callable and substituting the result — before any backend-mapping pass runs. No backend
  (Penumbra today, a future Nyx backend) ever sees a `Slot`-tagged `Component` node, which is
  also why `class` isn't meaningful on it: there's nothing for Lustre to select against on a node
  that never reaches a widget tree.

### 3.2 Backend-gated primitives

| Primitive | Required backend | Notes |
| --- | --- | --- |
| `<Model3d>` | `umbra-engine` | Illustrative forward-reference only; full prop set is Stage 6 work. |

Using `<Model3d>` when `.iris.json` (§5) declares `"target": "penumbra"` is a compile-time
error, not a runtime no-op (§7).

---

## 4. Styling: no inline styles

Unchanged from v1. Style is never inline in `.iris` files. `.lustre` files are the sole
authoring surface for visual properties — Iris Core does not define Lustre's syntax or cascade
rules (§8).

What Iris Core specifies:
- `class` is a reserved prop, valid on any element, accepting a string class name — the join
  between an Iris element and its Lustre declarations.
- No other style information appears in `.iris` files, including `<Text>`'s font.

```cpp
// Correct — style lives in the paired .lustre file
<Frame class="health-bar-container">
    <Inline class="label">{props.label}</Inline>
</Frame>

// Never valid in Iris — no inline styles
<Frame style="background: red;">
```

Interactivity (§2.3 of the decision doc): event props are valid on any element, with no separate
"interactive" primitive. **Verified against the landed Penumbra code** (superseding this
section's earlier description, which cited `Button::OnClicked`/`Checkbox::OnChanged` as the
mechanism — those are older, widget-specific, differently-typed callbacks that still exist but
are not what a generic Iris event prop maps onto): `WidgetBase` now carries five generic,
null-by-default `std::function<void()>` members — `OnPressed`, `OnReleased`, `OnHovered`,
`OnFocused`, `OnChanged` — dispatched by `Box::UpdateInteractionState` when set, and reachable
from Iris codegen via every primitive's `Builder::onPress()`/`onRelease()`/`onHover()`/
`onFocus()`/`onChange()` (§3.1). A widget with none set is exactly as inert as before this
existed. `WidgetBase` also now carries a plain `std::string ClassName` field as inert storage
for Iris's `class` prop — Penumbra holds it and does nothing with it, consistent with "no
defaults, no opinions"; Lustre-lite class-selector resolution (§8) is what will eventually read
it.

---

## 5. `.iris.json` reference

Unchanged from v1.

```json
{
    "target": "penumbra",
    "version": "0.1.0",
    "searchPaths": [
        "src/ui"
    ]
}
```

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `target` | enum: `"penumbra"` \| `"umbra-engine"` | Yes | Determines which backend-gated primitives (§3.2) are unlocked project-wide. Further backends added as implemented. |
| `version` | string (semver) | Yes | Iris language/spec version the project targets. |
| `searchPaths` | list of strings | Yes | Directories searched, in declaration order, to resolve `import Name` (§1.2). |

No `hostLanguage` field — that's carried by the file extension instead (§0 File model).
Switching `target` to `"umbra-engine"` unlocks all `umbra-engine`-gated primitives
project-wide; there is no mixed-target build.

---

## 6. Error and warning model

A direct consequence of the pivot: Iris now produces diagnostics in **three tiers**, and it
matters which tier a given mistake surfaces in.

1. **Preprocessor-level errors** — things Iris itself can detect because they're within its
   parsing domain (`render { }` block structure, element tags, `class` usage, `import`
   resolution, `.iris.json`). These are Iris compile errors, reported before host compilation.
2. **Host-compiler errors** — everything inside a `{ }` escape hatch (§1.4) is opaque to Iris; it
   only balances braces, it doesn't validate the C++23 inside them. A malformed lambda, a type
   error in a prop value, an undeclared variable — none of these are caught by Iris. They surface
   as ordinary C++23 compiler errors, but **not** against generated code the developer never
   wrote: per `docs/iris_stage1_decision_doc_pt2.md` §3, source-mapping ships day one. Every
   `Token` from the `IHostLanguageTokenizer` (§1.3) carries a `SourceLocation`, and the
   preprocessor emits `#line` directives into the generated `.cpp` throughout, so host-compiler
   errors point at the original `.iris` file and line — the same source-location plumbing also
   gives Iris's own preprocessor-level errors accurate locations for free.
3. **Runtime reconciler warnings** — `key` uniqueness/presence (§2.3) is checked only at
   runtime, not statically, since the preprocessor can't see inside escape hatches to know
   whether a `key`-bearing element sits inside a loop. The reconciler warns when two elements
   land at the same tree position without distinct keys — the same posture React takes.

### Compiler error catalogue (starter, preprocessor-level only)

| Error | Trigger | Minimal repro | Message intent |
| --- | --- | --- | --- |
| Backend-gated primitive on wrong target | `.iris.json` `target` doesn't include the primitive's required backend | `<Model3d/>` used with `"target": "penumbra"` | "`<Model3d>` requires backend `umbra-engine`; project target is `penumbra`." |
| Inline style prop | Any `style="..."` prop on an element | `<Frame style="background: red;">` | "Inline styles are not permitted. Use `class` and define styling in a `.lustre` file." |
| `<Text>` `font` prop | `font` prop set on a `<Text>` element | `<Text font="...">Hello</Text>` | "`<Text>` has no `font` prop; specify font via a Lustre class." |
| Unresolved/unimported component reference | Element tag not a Core primitive and not covered by an `import` in scope | `<HealthBar/>` used without `import HealthBar` | "`HealthBar` is not imported and is not a Core primitive." |
| Malformed/missing `.iris.json` field | `target`, `version`, or `searchPaths` absent or wrong type | `.iris.json` missing `searchPaths` | "`.iris.json` is missing required field `searchPaths`." |
| Multiple render roots | `render { ... }` block yields more than one top-level sibling element | `render { <A/> <B/> }` | "`render` must have exactly one root element." |
| Unterminated `render` block or escape hatch | Brace count never returns to zero before EOF | Unclosed `{` inside `render { }` | "Unterminated `render` block (or escape hatch) starting at line N." |

Dropped from v1's catalogue and not replaced: "non-literal/dynamic `key`," "`key` used as a
props field name," and "missing `key` in a loop" — all three assumed compile-time enforcement
that `docs/iris_stage1_decision_doc_pt2.md` §4 explicitly drops in favor of the runtime warning
described above (tier 3).

---

## 7. Reserved keywords and prop names

Per decision doc §8, the entire Iris-owned vocabulary:

| Token | Type | Notes |
| --- | --- | --- |
| `render` | Keyword | Marks the block the Iris preprocessor owns. |
| `import` | Keyword | Resolves component files at compile time. |
| `key` | Reserved prop name | Stripped before codegen; never reaches the backend (§2.3). |
| `class` | Reserved prop name | Style bridge to Lustre (§4). |

That's the whole list. Primitive tag names (`Frame`, `Inline`, `Grid`, `Image`, `Icon`, `Text`,
`Model3d`) are ordinary identifiers, not keywords (§1.4).

---

## 8. Open questions

All Stage 1 front-end/preprocessor-mechanics questions are now closed —
`docs/iris_stage1_decision_doc.md` §11 closed the original eight, and
`docs/iris_stage1_decision_doc_pt2.md` closed the eight that the pivot itself surfaced (list/loop
rendering → §1.5; `render {` detection and brace balancing → §1.3; error source-mapping → §6;
`key` enforcement → §2.3; dynamic `class` → §1.4; comments in the element tree → §1.6). What
remains open is unrelated to Stage 1 and was never expected to block it:

- **Lustre.** Cascade rules, selector syntax, and the mapping from Lustre properties to
  backend-specific style structs — deferred to a separate Lustre handoff doc.
- **`umbra-engine` primitive set beyond `<Model3d>`.** Stage 6 work (deferred).
- **`Grid` layout props and real grid layout.** Deferred by decision
  (`docs/iris_stage2_decision_doc.md` §3); Stage 2 stubs `<Grid>` as a plain `Box` and
  explicitly does not meet the Core-primitive requirement for it (§3.1).
- **Event-prop vocabulary extensibility.** Is the fixed set (`onPress`, `onRelease`, `onHover`,
  `onFocus`, `onChange`) extensible by component authors, or a closed list?
- **Implicit children forwarding.** No mechanism specified for a generic wrapper component to
  forward arbitrary children it wasn't handed as a named prop.
**Resolved:** `iris::Signal<T>` locals captured by `<Slot>` were dangling-reference UB as
originally specified — §2.2/§9.5 and `docs/iris_stage3_decision_doc.md` §0 asserted a
`Signal<T>` local was "genuinely persistent" once captured `[&]` into a `<Slot>` lambda, but a
component function runs once and *returns* (Stage 1 codegen literally emits `return <expr>;`
for the `render { }` block), at which point a plain local's stack storage is gone. Confirmed
with AddressSanitizer against real generated `.iris` output while verifying
`iris-penumbra-backend`'s `Umbra::IWidget` adapter — every `[&]`-capturing `<Slot>` example in
this spec was affected; this was the load-bearing pattern the whole reactive model depended on,
not a corner case. Fixed per `docs/iris_signal_lifetime_decision.md`: state is now declared via
`IRIS_SIGNAL(Type, Name, InitExpr)` (§2.2), which binds `Name` to a reference into a
heap-allocated `iris::ComponentInstance` rather than a stack local — `[&]` capture keeps working
exactly as every example already writes it, since what's captured is now a reference to
heap-stable storage. Re-verified end to end (real `.iris` → `iris_cc` → `IRIS_SIGNAL` →
`iris::Mount` → real Penumbra widget) under AddressSanitizer with zero errors.

**Resolved:** `Component` had no `nullptr_t` constructor, so every conditional-rendering
example in this spec (§1.5, §9) writing `return nullptr;` inside a `<Slot>` lambda declared to
return `Component` didn't actually compile — surfaced by manually host-compiling the §9
`PartyScreen` example's generated output while verifying `docs/iris_escape_hatch_decision.md`
(an `Component`-shape gap, not an escape-hatch one). Fixed with a new `IrisElementTag::None`
sentinel (`include/Iris/IrisElementTag.h`) plus an implicit `Component(std::nullptr_t)`
converting constructor (`include/Iris/Component.h`) that produces it — `return nullptr;`
now yields an `IrisElementTag::None`-tagged `Component`, which a walker/reconciler must
treat as "unmount whatever was here, mount nothing" and never hand to a backend `Builder`.
Adding that constructor loses `Component`'s aggregate-ness, so an explicit 4-field
constructor was added alongside it to keep Codegen.h's emitted `Component{Tag, Props,
Children, SlotCallable}` call shape compiling unchanged. `tests/ComponentTests.cpp` is a
new test file that host-compiles `Component.h` directly (previously nothing did — Codegen's
own tests only ever checked the shape of generated *text*, never compiled it) and covers the
`nullptr` conversion, including through `MakeSlotCallable`.

`<Image>`'s asset-pipeline gap and content-prop-name question — both flagged in the previous
revision of this document as real, unresolved gaps despite `docs/iris_stage2_decision_doc.md`
assuming they were closed — are now genuinely resolved: `Backends::IImageBackend` /
`SdlImageBackend` (real `SDL_image`-backed PNG/JPG decoding) and `ImageWidget` with a confirmed
`src` content prop landed in `penumbra-proto`, documented in its own
`docs/penumbra_image_widget_requirements.md`. See §3.1 for the two implementation-detail
divergences (narrower `Builder`, explicit `LoadFrom` step) worth knowing before Stage 2's
tree-builder targets it.

None of the remaining carried-forward items block starting Stage 1 implementation, and Stage 2
now has a real target for every Core primitive except `<Grid>` (deferred by decision, §3.1).

**Resolved:** `IrisProps`'s concrete runtime representation — `docs/iris_props_decision.md`: a
closed, strongly-typed `std::variant` (`Iris::IrisPropValue`), not a type-erased
`unordered_map<string, any>`. This unblocked Stage 1 codegen, which in turn surfaced two more
gaps neither that document nor §2.5's `Component` shape actually covered — both closed in
`docs/iris_stage1_codegen_decision.md`: where `<Slot>`'s callable child lives on `Component`
(not `Props`, not `Children` — a new `SlotCallable` field, populated via
`Iris::MakeSlotCallable()`), and how literal text/`{ }` interpolation children are represented
(`<Text>` concatenates into its own `"text"` prop; other primitives synthesize a `<Text>` child
node). `Codegen.h`/`GenerateComponentExpression()` implements and tests all of this
(`tests/CodegenTests.cpp`).

**Resolved:** Nested JSX inside an escape hatch is now transformed via a second escape-hatch
sigil, `!{ }` (§1.4, `docs/iris_escape_hatch_decision.md`). The opaque `{ }` form is unchanged;
`!{ }` recursively re-parses any whitespace-preceded `<Tag>` run inside it back into the element
grammar this document describes, and codegen transforms each such run the same way it would a
top-level element, splicing the generated expression back into the surrounding verbatim text.
`<Slot>`'s examples throughout this spec (§1.1, §1.5, §9) now use `!{ }`. Implemented in
`RenderBlockParser::ParseJsxEscapeHatch` and `Codegen.h`'s `EmitEscapeHatchExpression`, tested
end-to-end against the full §9 `PartyScreen` example (`tests/CodegenTests.cpp`'s
`TestPartyScreenFullyCodegensWithJsxTransformEscapeHatches`), including verifying the generated
output actually compiles as real C++23.

**Resolved:** How `.iris.json` gets parsed and how `import` resolves to a file. Decided in
favor of vendoring Amanuensis (`libs/amanuensis`, a zero-dependency first-party JSON library,
git-submoduled per its own recommended `add_subdirectory` integration path) rather than
hand-rolling a parser scoped to the three-field schema — a real (if minimal) JSON library was
already available and free of transitive dependencies, so there was no reason to duplicate one.
`IrisConfig` (`include/Iris/IrisConfig.h`) uses `Amanuensis::Reader`/`Amanuensis::Value` to parse
`target`/`version`/`searchPaths` with the exact error messages §6's catalogue specifies.
`ImportResolver` (`include/Iris/ImportResolver.h`) scans a source file for `import Name`
statements via `CppTokenizer` and resolves each to `Name.iris` (`target: "penumbra"`) or
`Name.irisx` (`target: "umbra-engine"`), searched across `searchPaths` in declaration order.
**Resolved:** the semantic validation pass that uses `import`s to validate element tags.
`include/Iris/SemanticValidator.h`'s `ValidateElementTree()` covers all four preprocessor-level
checks from §6's catalogue that `Codegen.h` doesn't already handle with the spec's exact
wording: backend-gated primitive on the wrong target (`<Model3d>`), inline `style` prop (on any
element, not just Core primitives), `<Text font=...>`, and unresolved/unimported component
reference. Recurses into elements nested inside a `!{ }` JSX-transform escape hatch
(`docs/iris_escape_hatch_decision.md`) as well as the top level. The Core-primitive tag-name set
now lives in a shared `include/Iris/CorePrimitives.h` so `Codegen.cpp` and the validator can't
drift apart on what counts as a primitive. Tested in `tests/SemanticValidatorTests.cpp`.

**Resolved:** the preprocessor driver/CLI wiring `RenderBlockParser`, `ImportResolver`,
`SemanticValidator`, and `Codegen` together. `include/Iris/Driver.h`'s `CompileFile()` runs the
full per-file pipeline and splices each `render { }` block's generated expression back into the
original source as `return <expr>;`, with `#line` directives resyncing line numbers after every
splice (this section's requirement, above). `tools/IrisCc.cpp` wraps it as the `iris_cc` CLI
binary.

**Resolved:** what `import Name` (not valid C++23) becomes in generated output —
`docs/iris_import_header_decision.md`. Real forward-declaration headers turned out to be
unreachable without Iris parsing struct/function signatures, which §2.1 explicitly rules out, so
every `.iris`/`.irisx` file instead compiles to one self-contained, `#pragma once` header
(`<original-path>.h`) with its full definition inline — nothing split into declaration vs.
definition. `import Name` becomes `#include` of that resolved import's own generated header. A
component's function needs `inline` to stay ODR-safe once its header is included by more than
one translation unit; Iris doesn't inject this itself (same reasoning — it would mean parsing
the signature), so it's an author-applied convention. Verified against a real multi-file, three-
component fixture that host-compiles end to end via nothing but generated `#include`s.

---

## 9. Ported-example self-check (Stage 0 exit criterion)

Same worked scenario as v1 (props, state, an event handler, a conditional, and a keyed list),
rewritten using `<Slot>` (§1.5), the current mechanism for both:

```cpp
import HealthBar
import Button

struct PartyScreenProps {
    std::vector<Character> members;
};

Component PartyScreen(PartyScreenProps props) {
    IRIS_SIGNAL(bool, detailsOpen, false);

    render {
        <Frame class="party-screen">
            <Button label="Details" onPress={[&]() { detailsOpen.set(true); }} />
            <Slot>
                !{[&]() -> Component {
                    if (!detailsOpen.get()) return nullptr;
                    return <Frame class="details-panel">
                        <Slot>
                            !{[&]() -> std::vector<Component> {
                                std::vector<Component> rows;
                                for (auto& member : props.members) {
                                    rows.push_back(
                                        <Frame key={member.id} class="party-row">
                                            <HealthBar current={member.hp} max={member.maxHp} label={member.name} />
                                        </Frame>
                                    );
                                }
                                return rows;
                            }}
                        </Slot>
                    </Frame>;
                }}
            </Slot>
        </Frame>
    }
}
```

`PartyScreen` runs once, at mount (§2.2). Only the two `<Slot>` callables re-invoke later: the
outer one when `detailsOpen` changes, the inner one whenever whatever drives `props.members`
changes (that signal isn't shown here — it would live in whatever parent owns the list). Every
construct here is fully specified by §1–§7 with no outside knowledge required — this confirms the
spec is sufficient for Stage 1 to target and for a new component author to write against.

---

## 9.1 Summary of overrides — v2 (this revision) vs. v1

For a reader diffing this revision against the previous version of this spec:

1. **Architecture.** Iris is no longer a standalone component/props/state/event language. It's a
   preprocessor that owns only `render { }` block contents, `import`, and `.iris.json`. Every
   v1 construct for component declaration, props structs, `state` blocks, event-handler arrow
   functions, `if`, and `for` is now plain host-language (C++23) code (decision doc §8).
2. **Event-handler expression limits removed.** v1 restricted event handlers to a single
   assignment or call; that restriction is gone — any valid C++23 lambda is legal, since Iris
   doesn't parse escape-hatch contents at all now.
3. **File extensions.** New `.iris`/`.irisx` split by host language (§0), not present in v1.
4. **Error model.** New two-phase (preprocessor vs. host-compiler) error model (§6); two of v1's
   error-catalogue entries no longer apply as compile-time checks (§6, §8).
5. **New open questions surfaced by the pivot** that v1 didn't have because the relevant
   constructs didn't yet involve opaque escape hatches: list/loop rendering mechanism, `key`
   enforcement inside now-opaque struct/loop code, `render {` detection robustness, brace
   balancing around string literals, error source-mapping (§8).

For the earlier override wave — v1 vs. the original `docs/iris_design.md` draft (casing,
`<Text>` font, `<Model3d>` gating, backend framing) — see git history of this file or
`docs/iris_stage1_decision_doc.md` §1–§3, which restates those decisions.

## 9.2 Changes from `docs/iris_stage1_decision_doc_pt2.md`

This revision folds in the closure of every question §8 previously listed as open post-pivot,
all resolved without contradiction or reinterpretation needed:

1. List/loop rendering settled as `std::vector<Component>` escape hatches (§1.5) — new
   section, new runtime-API implication (children accept a vector, not just a single component).
2. `render {` detection and escape-hatch brace balancing both resolved by a single
   `IHostLanguageTokenizer` abstraction, pluggable per file extension (§1.3).
3. Error source-mapping resolved: ships day one via `SourceLocation` on every token and `#line`
   directives in generated output (§6) — upgraded the error model from two tiers to three
   (preprocessor / host-compiler / runtime reconciler warnings).
4. `key` compile-time enforcement dropped entirely in favor of runtime-only checks (§2.3) — this
   also resolved the "open tension" this document previously flagged around the
   key-as-struct-field-name rule; no reinterpretation needed anymore, the rule itself is gone.
5. `class={expr}` confirmed valid, symmetric with `key` (§1.4).
6. Comments between elements inside `render { }` confirmed valid, stripped silently (§1.6).

## 9.3 Changes from `docs/iris_stage2_decision_doc.md`, grounded against real Penumbra code

Unlike the previous two revisions, this pass was checked directly against
`penumbra-proto`'s actual source (`/Users/deanwilson/development/projects/penumbra-proto`), not
just the decision doc's prose — two of the ten Stage 2 decisions turned out not to match what
actually shipped (see below).

1. `Component` confirmed as a backend-agnostic IR node (tag/props/children), not a
   live-widget facade (§2.5) — closes the representation question `docs/iris_stage2_open_questions.md`
   §4 raised.
2. New `<Name>Props` required naming rule for component-invocation codegen, and the
   primitive-vs-component codegen branch (§2.6).
3. `<Inline>` and `<Text>` confirmed distinct — `<Inline>` maps to a real new Penumbra
   `InlineContainer` widget, not `Label` (§3.1).
4. `<Grid>` deferred and stubbed as a plain `Box` (§3.1, §8).
5. Primitive reference and the interactivity section (§3.1, §4) rewritten with the actual
   shipped `Builder` API (`className`/`child`/`children`/`onPress`/`onRelease`/`onHover`/
   `onFocus`/`onChange`, all verified zero-argument) and the real generic
   `WidgetBase::OnPressed`/etc. callback mechanism — replacing this spec's earlier, now
   outdated, `Button::OnClicked`/`Checkbox::OnChanged` description.
6. **Discrepancy found, not assumed:** `docs/iris_stage2_decision_doc.md` §2 states a minimal
   `IImageBackend`/PNG-JPG-from-path pipeline was to be built for `<Image>`. The actual landed
   `Image` widget only draws a pre-supplied `SDL_Texture*` — no decode pipeline exists. Recorded
   as a new, real open question (§8) rather than silently treated as resolved, since the decision
   doc's own text says otherwise.
7. Similarly surfaced, not previously tracked anywhere: `<Image>`'s content prop name (`src`)
   was never explicitly decided in any Iris doc — Penumbra's `Image::Builder` omitted a
   content-setting method specifically because of this, which is what surfaced the gap.

## 9.4 `<Image>` gap closed

Both items §9.3 flagged as genuinely open (not just documentation lag) are now resolved, per a
follow-up Penumbra change verified directly against the landed code:

1. `Backends::IImageBackend`/`SdlImageBackend` now provide real PNG/JPG-from-path decoding via
   `SDL_image` (a real `CMakeLists.txt` dependency now, confirmed), mirroring `IFontBackend`'s
   shape as originally intended.
2. The widget was renamed `Image` → `ImageWidget` and rebuilt as a direct `WidgetBase` subclass
   (not `Box`-derived, unlike every other Core primitive) with a confirmed `src` content prop on
   its `Builder` — closing the naming question from the outside in, by Penumbra shipping the
   answer rather than Iris deciding it first.
3. Two divergences from every other primitive's `Builder` worth remembering, documented in
   §3.1: `ImageWidget::Builder` has no event-prop or `child()`/`children()` methods (a leaf,
   deliberately narrow), and loading happens via a separate explicit `.LoadFrom(backend,
   renderer)` call, not inside `build()`.

Stage 2 now has a real backend target for every Core primitive except `<Grid>` (deferred by
decision, not by gap).

## 9.5 `<Slot>` — the component-lifetime question closed

Per `docs/iris_stage3_decision_slot.md`, delivered directly rather than as a separate handover
file this repo saw. Closes the single most important open question this spec had left
unresolved (previously tracked only in `docs/iris_stage3_open_questions.md`'s lead item, never
in this document's own §8, since it only surfaced once Stage 3 planning began):

1. **Component functions run exactly once, at mount** — not re-invoked on every render as
   earlier phrasing in this spec implied. `IRIS_SIGNAL` locals are therefore genuinely
   persistent — not "for free" as originally assumed here (see §8's resolved note and
   `docs/iris_signal_lifetime_decision.md`: a plain stack local doesn't survive the declaring
   function returning, so persistence needed the heap-backed `iris::ComponentInstance`
   mechanism `IRIS_SIGNAL` provides) — and no call-order-indexed slot table (and its associated
   conditional-hooks footgun) is needed (§2.2).
2. **New Core primitive `<Slot>`** (§3.1) is the sole mechanism for re-invocation: its one
   escape-hatch child must be a callable — not an immediately-invoked one, unlike the IIFE
   pattern it replaces — that the runtime invokes at mount and re-invokes when a captured signal
   fires. §1.5 rewritten around it; every other escape hatch in the language remains one-shot,
   unchanged.
3. **The backend never sees `<Slot>`.** The runtime resolves it before any backend-mapping pass
   runs (§2.5) — Stage 2's Penumbra tree-walker, already decided before this change, needs no
   revision as a result.
4. No preprocessor codegen changes at all — `<Slot>` parses via the exact same element +
   escape-hatch grammar as any other primitive (§1.4). The constraints on its child (exactly one,
   must be callable) are runtime-enforced, not compile-time.
5. Narrows Stage 3's remaining scope favorably, worth noting for
   `docs/iris_stage3_open_questions.md`: reconciliation only ever needs to happen *within* what a
   `<Slot>` callable produces, comparing its new result against its previous one — never across
   the whole component tree from the root, since everything outside a `<Slot>` is now provably
   static once mounted.

---

## 10. Stage 3 runtime architecture (index, not full detail)

Full interfaces and reasoning live in `docs/iris_stage3_decision_doc.md` — this section is a
map to that document for whoever implements Stage 3, not a restatement. All ten questions
`docs/iris_stage3_open_questions.md` raised are closed there.

> Note on the `vendor/penumbra` references below: at the time this verification was performed,
> `iris` vendored Penumbra directly at that path. That repo/build decision was later corrected
> (`docs/iris_stage2_decision_doc.md`'s correction note) — Penumbra is no longer referenced
> anywhere in this repo, and the Penumbra backend (where these verifications actually matter)
> now lives in `iris-penumbra-backend`, which vendors both `iris` and `penumbra-proto`. The
> verification facts below are still accurate as a record of what was checked and when.

- **Reconciliation model: slot-scoped diffing.** On a signal trigger, only the `<Slot>`
  instances that captured it are re-invoked; only their new output is diffed against their
  previous output. Nothing outside a `<Slot>` is ever revisited after mount (decision doc §1).
- **Same-position matching, scoped to a `<Slot>`'s output:** same tag + same `key`/position →
  update in place; different tag → unmount old (discarding any signals tied to that position)
  and mount new (decision doc §2).
- **Keyed list diffing: minimal-move, LIS-based** — not naive remove-and-readd. Uses Penumbra's
  `MoveChild`/`InsertChildAt`/`RemoveChild`, verified present (§10 of the decision doc; also
  reconfirmed directly against `vendor/penumbra` while incorporating this decision — see the
  verification note below) (decision doc §3).
- **Prop updates via a strongly-typed `IrisPropDiff`**, applied through a new **`IWidget`**
  interface (`ApplyPropDiff`) — **not** by touching a concrete backend widget type directly. This
  is why §2.3's identity map is `IWidget*`, not `WidgetBase*`: the Penumbra backend implements
  `PenumbraWidget : IWidget` as an adapter, so the Iris runtime itself never names a Penumbra
  type (decision doc §4).
- **`<Image>` gets two update paths:** `src` (path, synchronous re-decode on change — accepted
  cost for Stage 3) and a new `handle` prop (`iris::TextureHandle`, a runtime-owned opaque
  texture reference — zero-cost swap, meant for animation). `iris::TextureHandle` and
  `iris::LoadTextures()` are new Iris-runtime-level concepts; they don't exist in Penumbra and
  aren't Penumbra's concern (decision doc §5).
- **Batching:** every event handler invocation is automatically wrapped in begin/end batch calls
  by the runtime; all signal writes within one handler produce exactly one reconciliation pass,
  not one per `.set()` call (decision doc §6).
- **Frame-loop integration: explicit `iris::Tick()`**, called once per frame by the host's main
  loop. Reconciliation happens only inside `Tick()` — signals set from any thread mark `<Slot>`s
  dirty but never mutate the widget tree directly; only the main thread, only inside `Tick()`,
  ever does (decision doc §7).
- **Lifecycle hooks: `IWidgetLifecycle`** (`OnMount`/`OnUnmount`/`OnTick`) — **resolved.** Was a
  real, unmet Penumbra-side prerequisite (verified absent against the then-pinned submodule
  commit while incorporating this decision), the same category as Stage 2's `<Image>` gap was
  before it got fixed. `penumbra-proto` commit `663fece` ("Add IWidgetLifecycle interface and
  Application lifecycle host") landed `include/Penumbra/IWidgetLifecycle.h`
  (`OnMount`/`OnUnmount`/`OnTick(TickInfo)`) plus an `Application` host dispatching `OnTick`,
  matching this decision's shape exactly. Everything Stage 3 needs from Penumbra (structural
  mutation, tree walking, lifecycle) is now there (decision doc §8, §10).
- **No cross-component state-sharing mechanism** — props drilling only, deliberately, forever
  (decision doc §9).
- **Resolved:** `IWidget`, `IrisPropDiff`, and `IWidgetLifecycle`/`TickInfo` were said to "live
  in Penumbra temporarily, pending extraction to `umbra-interfaces`" — but this repo has a hard
  rule against depending on Penumbra at all (§2.5, CLAUDE.md), so that framing couldn't actually
  be implemented as written. `umbra-interfaces` now exists for real
  (`github.com/DeanWilsonDev/umbra-interfaces`, vendored here as `libs/umbra-interfaces`) rather
  than working around the conflict — header-only, zero dependencies, names no concrete runtime
  or backend. `IWidget` also gained child-management methods it didn't have before
  (`GetChildCount`/`GetChildAt`/`InsertChildAt`/`RemoveChildAt`, mirroring Penumbra's own `Box`)
  — needed for this section's own "recurse into children" matching rule, which a bare
  `ApplyPropDiff` alone can't do. See `docs/iris_stage3_implementation_decision.md`.

**Verification performed while incorporating this decision** (per the decision doc §10's own
instruction not to trust decision-doc prose alone): re-confirmed directly against
`vendor/penumbra` (pinned at `f008666`) that `Box`'s five structural-mutation methods and
`SplitPanel`'s `GetChildCount`/`GetChildAt` overrides are all present as claimed. `Box`'s own
`GetChildCount`/`GetChildAt`, and the prop-mutation fields (`ClassName`, `Text`, `Checked`,
`OnPressed`/etc.) were already verified in earlier Stage 2 grounding and remain valid at this
commit. `IWidgetLifecycle` was checked and confirmed absent at commit `f008666`; as of
`penumbra-proto` commit `663fece` it exists and matches this decision's shape — see above.
Stage 3 now has every documented Penumbra-side prerequisite it needs; nothing left blocking
implementation from the Penumbra side.

**Implementation status:** the core reactive engine described above — `IRIS_SIGNAL`/
`iris::Signal<T>`, ambient dependency tracking, `IrisRuntime` batching, `iris::Tick()`, and the
reconciler (`ComputePropDiff`, same-tag-key matching, keyed list diffing) — is implemented,
closing several gaps this section's prose left genuinely open (how a signal knows which slots
to notify was never specified; `key` never actually reached `Component`; `Signal<T>` locals
were dangling-reference UB as originally specified — see §8's resolved notes). A real Penumbra
`Umbra::IWidget` adapter is also implemented and tested against actual `Penumbra::Widgets::Box`/
`Label` objects, not just a mock (`iris-penumbra-backend`'s
`docs/iris_penumbra_backend_adapter_decision.md`). Full writeups:
`docs/iris_stage3_implementation_decision.md` (the reconciler/runtime core),
`docs/iris_signal_lifetime_decision.md` (the signal-lifetime fix), and
`docs/iris_slot_stage2_wiring_decision.md`/`docs/iris_slot_list_wiring_decision.md` (wiring
`<Slot>` into the Stage 2 walker, for both the single-`Component`- and list-returning
callable shapes — verified against real Penumbra `Box`/`Label` objects, a live `iris::Signal`
update reaching a real `Box::Children` vector end to end, clean under AddressSanitizer +
UndefinedBehaviorSanitizer). Nested-`<Slot>` discovery (a `<Slot>` inside another `<Slot>`'s
own dynamically-produced output) is now done too — `docs/iris_nested_slot_discovery_decision.md`.
Not yet done: LIS-based minimal-move list diffing, and avoiding nested-`<Slot>` rediscovery on
every outer re-render when the underlying subtree was reused unchanged.
