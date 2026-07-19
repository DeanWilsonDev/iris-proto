# Iris — JSX-Inside-Escape-Hatch Decision (`!{ }`)

> **Status:** Closed and implemented. Records the decision on how a `<Slot>` (or any
> escape hatch)'s body gets its nested JSX transformed, and closes the gap
> `docs/iris_next_steps.md` and `docs/iris_core_spec.md` §8 flagged after Stage 1
> codegen was written against `docs/iris_props_decision.md` and
> `docs/iris_stage1_codegen_decision.md`.
>
> **Blocks resolved:** Stage 1 codegen's output now compiles for any component using
> `<Slot>` for conditional/list rendering — every such example in `docs/iris_core_spec.md`
> (§1.1, §1.5, §9).

---

## The problem

`RenderBlockParser` captures every `{ }` escape hatch as fully opaque verbatim text
(§1.4, directly tested by `TestEscapeHatchContainingAngleBracketsIsOpaque`). That's
correct for event handlers and simple interpolation, but `<Slot>`'s entire purpose —
conditional and list rendering — is written as JSX *inside* that escape hatch:

```cpp
<Slot>
    {[&]() -> IrisComponent {
        if (settingsOpen.get()) {
            return <SettingsPage onClose={[&]() { settingsOpen.set(false); }} />;
        }
        return nullptr;
    }}
</Slot>
```

Since the escape hatch is opaque, `<SettingsPage ... />` is never re-parsed — it reaches
generated `.cpp` as literal `<`/`>` tokens, which isn't valid C++ and won't compile. Every
conditional/list-rendering example in the spec uses exactly this pattern, so this isn't an
edge case.

## Decision

Introduce a second escape-hatch sigil, `!{ }`, meaning "host-language code that may
contain JSX — recursively transform it." `{ }` is unchanged and stays fully opaque.

- `{ }` — regular escape hatch. Opaque, verbatim, unchanged from before this decision.
- `!{ }` — JSX-transform escape hatch. The parser scans its body for `<Tag>` runs and
  recursively parses each one back into the ordinary element-tree grammar (§1.4); codegen
  then transforms each parsed run into an `Iris::IrisComponent`-constructing expression and
  splices it back into the surrounding verbatim text. Closes on the matching `}`. Valid
  anywhere a regular `{ }` is valid — prop values, child positions, lambda bodies.
- Nesting composes normally: once inside a `!{ }`, a nested `{ }` (e.g. an event-handler
  prop on a JSX-transformed element) is opaque as usual; a nested `!{ }` recursively
  transforms again. The spec's §9 `PartyScreen` example exercises exactly this — the outer
  `<Slot>`'s `!{ }` body contains a `<Frame>` whose own child `<Slot>` also uses `!{ }`.
- `!{` is not valid C++ in a child or prop-value position, so the token is unambiguous at
  the lexer level — no heuristic needed to tell `!{ }` apart from `{ }`.

## Implementation

`RenderBlockParser::ParseJsxEscapeHatch` (`src/Iris/RenderBlockParser.cpp`) is the new
entry point, wired in wherever `ParseEscapeHatch` was already reachable (`ParsePropValue`,
`ParseChildren`) via `IsJsxEscapeHatchStart()` (`Current_ == '!'` immediately followed by
`'{'`, checked with a one-token lookahead — `PeekNext()`, backed by a new `Lookahead_`
buffer on the parser).

Unlike `ParseEscapeHatch` (which scans raw bytes and never leaves the tokenizer's raw
token stream), `ParseJsxEscapeHatch` walks the parser's structured `GToken` stream so it
can recognize `<Identifier` runs and hand them to the existing `ParseElementAfterLAngle` —
the same function that parses any other element — recursing naturally through nested
`!{ }`/`{ }` and nested elements. Its output, `PropValue::JsxSegments`
(`include/Iris/ElementNode.h`'s new `JsxSegment`/`JsxSegmentKind`), is a sequence of
`RawText` and `Element` pieces in source order — `RawText` reconstructed token-for-token
(whitespace-normalized, like literal element text elsewhere in this parser; not
byte-identical to source, which doesn't matter since only the JSX runs need to compile as
written) and `Element` holding a fully parsed nested `ElementNode`.

### Disambiguating a JSX element start from a template argument list

`std::vector<IrisComponent>` — a real pattern in the spec's own `PartyScreen` example,
where `<Slot>`'s list-rendering lambda returns `std::vector<IrisComponent>` — has exactly
the same `< Identifier >` shape as an attribute-less opening tag like `<IrisComponent>`.
Naively treating every whitespace-agnostic `<Identifier` as a JSX start misparses the
template argument list as an element, which then fails to find its (nonexistent) matching
close tag.

The fix: require whitespace immediately before the `<` for it to count as a JSX start.
Every JSX use in the spec is written with a space or newline before it (`return <Frame
...`, `push_back(\n    <Frame ...`), while a template argument list never has one
(`vector<IrisComponent>`, `map<int, T>`). This is a heuristic, not a proof — a
whitespace-free JSX element (`push_back(<Frame/>)`) would be misread as a template — but it
matches every example in the spec and codebase, and is cheap to revisit if a real case
needs it. Covered by `TestJsxTransformEscapeHatchDoesNotMisreadTemplateAnglesAsJsx`
(`tests/RenderBlockParserTests.cpp`).

### Codegen

`Codegen.cpp`'s `ComponentEmitter::EmitEscapeHatchExpression` replaces the old free
function `EmitBarePropValue` (now a method, `EmitPropValueExpression`, so it can recurse
back into `Emit()`): for a plain `EscapeHatch`, behavior is unchanged — `Value.Text`
verbatim. For a `JsxEscapeHatch`, it walks `JsxSegments`, emitting `RawText` pieces
verbatim and recursively calling `Emit()` on each `Element` piece, concatenating the
result. Every call site that used to read `PropValue::Text`/`ElementChild::EscapeHatch->Text`
directly for an escape hatch (`<Slot>`'s lambda, synthetic `<Text>` children, ordinary
prop values, component-invocation prop values) now goes through this method instead, so
`!{ }` works in every position `{ }` did.

## Verification

`tests/CodegenTests.cpp`'s `TestPartyScreenFullyCodegensWithJsxTransformEscapeHatches`
covers the full two-level §9 example (both `<Slot>`s using `!{ }`, including the
`std::vector<IrisComponent>` return type on the inner one) and asserts no raw `<Tag>` text
survives anywhere in the output. Beyond the string-shape assertions the test suite already
does for every other codegen case, this example's generated output was manually verified
to actually compile as real C++23 against a stub `Button`/`HealthBar`/`IrisComponent`
harness — the first time a `<Slot>`-using component's full generated `.cpp` has been
confirmed compilable, not just structurally plausible. (That manual compile surfaced one
separate, pre-existing gap *not* part of this decision — `IrisComponent` had no `nullptr_t`
constructor, so the spec's own `return nullptr;` inside an `-> IrisComponent` lambda didn't
compile as written — closed separately, see `docs/iris_core_spec.md` §8 and
`tests/IrisComponentTests.cpp`. Re-running the same manual compile after that fix now
succeeds with no workarounds.)

## What is now unblocked

Per `docs/iris_next_steps.md`'s suggested order:

1. ~~Write this decision doc~~ — **done**
2. ~~Implement `!{ }` in `RenderBlockParser`~~ — **done**, this document
3. Semantic validation pass in `iris`
4. Stage 2 walker in `iris-penumbra-backend`, consuming codegen's
   `Iris::IrisComponent`-constructing output
5. Stage 3 reactive runtime

## What remains deliberately deferred

- **A whitespace-free JSX element inside `!{ }`** (e.g. `push_back(<Frame/>)`) is
  misparsed as a template argument list under the current heuristic. No example in the
  spec needs this; revisit if a real consumer does.
