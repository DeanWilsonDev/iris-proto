# Iris — Stage 1 Codegen Decisions

> **Status:** Closed. Records two decisions needed to turn a parsed `ElementNode` (§1.4's
> grammar, `RenderBlockParser`) into a C++23 expression constructing an `Component` value,
> per `docs/iris_props_decision.md`'s definition of `IrisProps`/`IrisPropValue`. Neither gap
> below is addressed by that document or by `docs/iris_core_spec.md` §2.5 as written — both
> assume a shape for `Component` that turns out not to be sufficient once every construct in
> the grammar (§1.4–§1.6) has to actually compile down to something.

---

## Gap 1: `<Slot>`'s callable child doesn't fit anywhere on `Component`

`docs/iris_core_spec.md` §2.5 gives `Component` as:

```cpp
struct Component {
    IrisElementTag Tag;
    IrisProps Props;
    std::vector<Component> Children;
};
```

`<Slot>`'s single child is a callable returning `Component` or `std::vector<Component>`
(`docs/iris_stage3_decision_slot.md`), re-invoked later by the runtime. That callable has
nowhere to go:

- Not `Children` — that holds already-*constructed* `Component` values, not an unevaluated
  callable.
- Not `Props` — `IrisPropValue` (`docs/iris_props_decision.md`) is a closed variant whose one
  callable member, `std::function<void()>`, is shaped for zero-argument event handlers, not for
  something returning `Component`/`vector<Component>`.

**Decision:** add one field to `Component`, meaningful only when `Tag == Slot`:

```cpp
// include/Iris/Component.h
struct Component {
    IrisElementTag                    Tag;
    IrisProps                         Props;
    std::vector<Component>        Children;
    std::shared_ptr<IrisSlotCallable> SlotCallable; // set only when Tag == Slot
};

// Defined after Component in the same header, once Component is a
// complete type — a variant of function<Component()> can't safely be a
// *direct* member of Component itself: unlike std::vector, the standard
// gives std::function no guarantee of working with an incomplete return type,
// so it's held behind a pointer instead (shared_ptr, since Component must
// stay copyable — it's stored by value in `Children` and returned by value
// throughout).
struct IrisSlotCallable {
    std::variant<std::function<Component()>, std::function<std::vector<Component>()>> Callable;
};
```

The preprocessor never inspects a `<Slot>` escape hatch's contents (`docs/iris_core_spec.md`
§1.4) — it can't tell whether the lambda the author wrote returns `Component` or
`std::vector<Component>`, so it can't pick a `IrisSlotCallable` alternative itself. That
choice is pushed to the host compiler via a small helper the generated code calls instead of
constructing the variant directly:

```cpp
// include/Iris/Component.h
template <typename Callable>
std::shared_ptr<IrisSlotCallable> MakeSlotCallable(Callable&& Fn);
```

`MakeSlotCallable` uses `if constexpr` on `std::invoke_result_t<Callable>` to build the right
alternative; a return type that's neither `Component` nor `std::vector<Component>` fails
to compile inside the helper, which is exactly the "malformed lambda → host-compiler error"
tier from §6, not a preprocessor-level check. It lives alongside `Component` itself
(`Iris::` namespace) rather than under the lowercase `iris::` runtime namespace
(`iris::Signal`, `iris::TextureHandle`, `iris::Tick`) — it constructs a plain IR value with no
runtime/reactive behavior of its own; the Stage 3 runtime that actually *reads* a mounted
`Component` tree and drives re-invocation is what's lowercase `iris::`.

Codegen for `<Slot>{ Lambda }</Slot>` is then just:

```cpp
Iris::Component{
    Iris::IrisElementTag::Slot,
    Iris::IrisProps{},
    {},
    Iris::MakeSlotCallable(Lambda) // Lambda emitted verbatim, unparsed
}
```

**Codegen-level arity/shape checks kept, despite `docs/iris_stage3_decision_slot.md` assigning
`<Slot>` constraints to "the runtime, not the preprocessor":** codegen still needs exactly one
escape-hatch child to emit the call above at all — a `<Slot>` with zero children, more than one
child, a nested-element child, or a literal-text child can't be turned into a
`MakeSlotCallable(...)` call, so it's a preprocessor-level `CodegenError`, not silently passed
through. This doesn't contradict the runtime-enforcement note — that note is about *callability
and return type*, which genuinely can't be checked without inspecting the escape hatch; arity is
checkable without inspecting it at all.

---

## Gap 2: literal text and `{ }` interpolation as element children

`docs/iris_core_spec.md` §3.1 gives `<Text>` and `<Inline>` children as "`{expr}` interpolation
or literal text" — but nothing in `Component`'s shape holds a bare string or an
unwrapped expression as a child; `Children` only holds `Component` values.

**Decision, split by primitive:**

- **`<Text>`** renders one string (leaf semantics, `Label::Builder::text(string)` on the Penumbra
  side). All of its `Text`/`EscapeHatch` children are concatenated, in source order, into a
  single `std::string`-valued `"text"` prop on the `<Text>` element itself — not into
  `Children`. A literal-text run becomes a quoted string literal; an escape hatch is emitted
  verbatim and must itself produce (or convert to) `std::string` — the host compiler enforces
  that, same as any other escape hatch. Multiple runs join with `+`:

  ```cpp
  // <Text>Hello {name}!</Text>
  IrisProps{
      { "text", IrisPropValue{std::in_place_type<std::string>,
                               std::string("Hello ") + name + std::string("!")} }
  }
  ```

  A `<Text>` with a nested-*element* child is a `CodegenError` — the spec never describes
  `<Text>` accepting one, and there's nowhere for it to go once `Children` is reserved for
  `<Inline>`-style mixed content (below).

- **Every other primitive that accepts children** (`<Inline>` for mixed inline-flow content,
  chiefly): a `Text`/`EscapeHatch` child is wrapped as a **synthetic `<Text>` node** and appended
  to `Children` alongside any real nested-element children, in source order:

  ```cpp
  Component{ IrisElementTag::Text,
                 IrisProps{ { "text", IrisPropValue{std::in_place_type<std::string>, ...} } },
                 {}, nullptr } // no SlotCallable — only Tag == Slot ever sets it
  ```

  This is what makes "`<Inline>` accepts mixed children — text runs, images, nested elements"
  (`docs/iris_stage2_decision_doc.md` §6) representable at all without inventing a second child
  type alongside `Component`: every child of every non-`<Text>`, non-`<Slot>` primitive is,
  after codegen, an ordinary `Component` — some of them just happen to be synthesized
  `<Text>` nodes rather than ones the author wrote directly.

  `<Frame>`, `<Grid>`, and `<Image>` don't document text/interpolation children at all (`<Image>`
  is a leaf); if the parser finds one there anyway, it's a `CodegenError`, not silently
  synthesized — same reasoning as `<Text>` plus a nested element.

- **Component invocation** (an element tag that isn't a known Core primitive): §2.6 says nothing
  about children — implicit children-forwarding is still an open, deferred question
  (`docs/iris_core_spec.md` §8). Any children on a component element is a `CodegenError` until
  that question is resolved, rather than guessing at a forwarding convention now.

---

## Also settled here, not worth a separate section

- **`<Model3d>` is not in codegen's Core-primitive tag set yet.** §3.1 calls it "illustrative
  forward-reference only; full prop set is Stage 6 work" — its props were never actually
  designed. Codegen treats an unrecognised tag as a component-invocation attempt (§2.6), so
  `<Model3d>` currently falls through to that path and fails at host-compile time for lack of a
  `Model3d`/`Model3dProps` pair — acceptable since nothing can use it correctly yet regardless.
- **`key` is parsed but not threaded through codegen.** `Component` has no `Key` field
  (`docs/iris_props_decision.md` confirms the reconciler's identity map is built separately, not
  from `IrisProps`) — that map is Stage 3's construction, not Stage 1's. `ElementNode::Key` is
  parsed and available; Stage 1 codegen simply doesn't consume it yet.
- **Unknown prop names on a Core primitive are a `CodegenError`**, not silently passed through —
  the closed lookup table in `docs/iris_props_decision.md` is exhaustive for what Core
  primitives currently accept (`class`, `key`, the five event props, `src`, `checked`, `handle`);
  anything else on a primitive tag has no `IrisPropValue` mapping to emit.
- **Every prop value is constructed via `IrisPropValue{std::in_place_type<T>, ...}`**, not
  `IrisPropValue{...}`'s converting constructor — `std::variant`'s converting constructor picks
  the *best-matching* alternative by overload resolution, and a `const char*` string literal
  binds to `bool` (built-in pointer-to-bool conversion) ahead of `std::string` (user-defined
  conversion) under those rules. `in_place_type` sidesteps the ambiguity entirely by naming the
  target alternative explicitly, which the lookup table already gives codegen for every prop it
  accepts.
- **Namespace qualification in generated code.** No spec example commits to one, so codegen
  fully qualifies every IR type it emits (`Iris::Component`, `Iris::IrisElementTag::Frame`,
  `Iris::IrisProps`, `Iris::IrisPropValue`, `Iris::MakeSlotCallable`) — matching where this repo
  already puts everything it owns (`namespace Iris`, `include/Iris/*.h`), same as
  `Iris::RenderBlockParser`/`Iris::ElementNode` today. `iris::TextureHandle` is the one
  deliberate exception — see Gap 1.
