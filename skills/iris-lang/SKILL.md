---
description: Read, write, and debug Iris (.iris/.irisx) — a JSX-flavored reactive UI preprocessor over C++23. Use whenever a project has .iris files, an .iris.json config, or renders UI through Iris components.
---

Iris is **not a standalone language** — it's a preprocessor and reactive UI
runtime layered over a host language (currently C++23). A `.iris` file is
100% valid host-language source except that it may contain `render { }`
blocks; inside those, Iris parses a JSX-style element tree and rewrites the
whole file into ordinary compilable C++. Everything outside `render { }` is
untouched host code — Iris deliberately never parses it. The authoritative
reference is `docs/iris_core_spec.md` in this repo; treat everything below as
a summary of it, and go there for anything not covered here.

## Toolchain

```sh
cmake -S . -B build && cmake --build build   # builds iris, iris_cc, iris_lsp, test_iris
./build/tests/test_iris                       # run tests

./build/iris_cc MyComponent.iris -o MyComponent.iris.cpp --project-root .
# --project-root defaults to the nearest ancestor dir containing .iris.json (tsconfig-style walk-up)
# without -o, generated source goes to stdout; diagnostics go to stderr as
#   <file>:<line>:<col>: error: <message>
# each .iris file compiles to ONE self-contained header (#pragma once, no decl/def split) —
# mark component functions `inline` yourself for ODR-safety across includes; Iris doesn't inject this.
```

Every consuming project needs an `.iris.json` at its root:

```json
{
    "target": "penumbra",
    "version": "0.1.0",
    "searchPaths": ["src/ui"]
}
```

`target` is `"penumbra"` or `"umbra-engine"` — it gates which Core primitives
are legal (e.g. `<Model3d>` requires `"umbra-engine"`, a compile error under
`"penumbra"`). `searchPaths` is where `import Name` looks for
`Name.iris`/`Name.irisx`.

## Core syntax

Only two real keywords: `render`, `import`.

```cpp
import Button
import SettingsPage

Component StartMenu() {
    IRIS_SIGNAL(bool, settingsOpen, false);   // state — see "State" below

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

- `Component ComponentName()` runs **exactly once, at mount** — it is never
  re-invoked on re-render.
- `render { }` allows exactly one root element; multiple top-level siblings
  is a compile error.
- Tags are PascalCase, resolved semantically against Core primitives or
  `import`ed component names — not lexer keywords.
- `class` and `key` are the only two reserved prop names; both accept a
  string literal or a `{ }` escape hatch.
- Comments (`//`, `/* */`) work both outside and inside `render { }`.

### `<ComponentName>Props` — load-bearing, unchecked naming convention

**Critical gotcha**: a component's props struct must be named
`<ComponentName>Props`. Iris never parses struct declarations — it relies on
this naming convention alone to know what type to construct
(`<HealthBar current={...} />` → `HealthBar(HealthBarProps{...})`). Getting
this wrong produces **silently wrong generated code with no compiler
error.**

```cpp
struct PartyScreenProps {
    std::vector<Character> members;
};

Component PartyScreen(PartyScreenProps props) { ... }
```

### State — `IRIS_SIGNAL`, never a direct `iris::Signal<T>` declaration

```cpp
IRIS_SIGNAL(bool, settingsOpen, false);        // correct
// iris::Signal<bool> settingsOpen = false;    // WRONG — dangling-reference UB
```

`IRIS_SIGNAL(Type, Name, InitExpr)` expands to `Name` being a reference into
heap-allocated storage owned by the mounted component's lifetime. This is a
hard, mechanical rule — always use the macro, never the direct form (some
older docs show the direct form; it's unsound and was fixed, see
`docs/iris_signal_lifetime_decision.md`). Accessors are `.get()`/`.set()`.

### `<Slot>` — the only dynamic/re-invokable region

Every plain `{ }` escape hatch runs once at mount and is never revisited.
`<Slot>` is the sole construct for conditional/list rendering that reacts to
signals later:

```cpp
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

Rules: `<Slot>`'s single child must be a `!{ }` escape hatch containing a
**callable** (lambda, no trailing `()`) returning `Component` or
`std::vector<Component>` — never an immediately-invoked one. The runtime
invokes it at mount and re-invokes it whenever a signal it captured (via
`.get()`) fires. `return nullptr;` inside a `<Slot>` lambda means "unmount,
mount nothing." Anything else (multiple children, non-callable) is a
**runtime** error, not caught at compile time.

### `{ }` vs `!{ }`

- `{ }` — opaque. Balanced-brace matching, emitted verbatim as host code,
  never parsed by Iris. Use for event handlers, `{props.label}`
  interpolation, ordinary prop values.
- `!{ }` — identical, except any whitespace-preceded `<Tag>` run inside it is
  recursively re-parsed as nested JSX. **Required inside `<Slot>`** since its
  body always contains nested JSX; using plain `{ }` there leaves the nested
  JSX unparsed. Disambiguation: `<Tag>` only counts as JSX-start if
  whitespace-preceded, so `std::vector<Component>` in a return type is never
  misread as JSX.

### Core primitives

`<Frame>` (block container), `<Inline>` (inline-flow container, distinct
from `<Text>`), `<Grid>` (**stubbed** — maps to a plain `Box`, no real
grid layout yet), `<Image src="...">`, `<Icon icon="..." size={...}>`,
`<Scroll wheelStep={...}>`, `<Input text="..." preferredWidth={...}
onTextChange={...}>`, `<Text>` (**no `font` prop** — compile error, styling
comes from a paired Lustre stylesheet), `<Slot>`. Shared event props:
`onPress`, `onRelease`, `onHover`, `onFocus`, `onChange` — all zero-argument
`std::function<void()>` (except `<Input>`'s `onTextChange`, which carries the
new string). `<Model3d>` is backend-gated to `"target": "umbra-engine"`.

### No inline styles — ever

```cpp
<Frame class="health-bar-container">...</Frame>   // correct
<Frame style="background: red;">                   // compile error
```

`class` is the sole bridge to a paired `.lustre` stylesheet (a separate
project — if this repo also has the `lustre` plugin installed, see that
skill for the styling side).

## Diagnostics model (three tiers)

1. Preprocessor-level errors (render-block structure, tag resolution,
   `import`, `.iris.json`) — reported with real `.iris` file/line.
2. Host-compiler errors — anything inside `{ }`/`!{ }` is opaque to Iris;
   type errors surface as ordinary C++23 errors, but `#line` directives keep
   them pointed at the original `.iris` line.
3. Runtime reconciler warnings — `key` uniqueness/presence, checked only at
   runtime, not compile time.

Common compile errors: backend-gated primitive on the wrong `target`, inline
`style` prop, `<Text font=...>`, unresolved/unimported component reference,
malformed `.iris.json`, multiple render roots, unterminated render
block/escape hatch.

## Tooling

- **`iris_lsp`** — real LSP server (stdio). Everything inside `render{}` or
  on an `import` line is handled by Iris itself; everything else is proxied
  to `clangd` (must be on `PATH` for host-language completion/goto-def —
  Iris-only features still work without it). Hover, rename, and
  find-references are **not implemented**. POSIX-only.
- **No formatter, no linter** — only `iris_cc` (compiler) and `iris_lsp`
  exist.
- Neovim setup: `editors/nvim/iris-lsp.lua` (LSP) and
  `editors/nvim/iris-treesitter.lua` (the `cpp` tree-sitter grammar handles
  everything outside `render{}` via injection; `iris-lsp`'s own semantic
  tokens cover the JSX bits inside it — requires `:TSInstall cpp`).

## Things that will bite an agent generating Iris code

- **Staged roadmap** — Stages 0–3 (spec, preprocessor, Penumbra backend,
  reactive runtime) are done. **Stage 4 (Lustre-lite styling integration)
  isn't scoped yet, Stage 5 (first real consumer) hasn't started, Stage 6
  (Umbra Engine/Nyx backend, `.irisx`) is deliberately deferred** — don't
  assume `.irisx`/Nyx-specific behavior exists.
- `<Grid>` has no real grid layout — don't expect columns/rows/gap props to
  do anything yet.
- `IRIS_SIGNAL` is mandatory; the direct-declaration form is unsound even
  though it appears in some older docs.
- `<ComponentName>Props` naming is unchecked — get it wrong and nothing
  tells you.
- `!{ }` is required (not `{ }`) inside `<Slot>` whenever the body contains
  nested JSX.
- No cross-component state-sharing mechanism exists — props drilling only,
  by design.
- Nested `<Slot>`s are fully rediscovered on every outer re-render even when
  unchanged — a known, accepted perf cost, not a bug.
- `docs/archive/iris_next_steps.md` is a frozen historical snapshot — use
  `docs/next-steps.md` for the live list of open gaps.

## Project structure reference

```
iris/
├── CLAUDE.md              — primary onboarding doc (no README.md exists)
├── .iris.json             — this repo's own project config
├── include/Iris/          — public headers (Driver, Component, ComponentInstance, Signal, ...)
├── src/Iris/               — implementations mirroring include/Iris/
├── tools/IrisCc.cpp        — iris_cc CLI entry point
├── tools/iris-lsp/          — LSP server
├── tests/                    — test_iris (worked PartyScreen example lives in DriverTests.cpp)
├── docs/                      — iris_core_spec.md is authoritative; rest are decision-record history
└── editors/nvim/                — iris-lsp.lua, iris-treesitter.lua
```
