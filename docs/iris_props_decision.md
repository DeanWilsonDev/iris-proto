# Iris — `IrisProps` Runtime Representation Decision

> **Status:** Closed. Records the decision on `IrisProps`'s concrete runtime type.
> This was the last blocking open question for Stage 1 codegen and Stage 2's
> backend-mapping walker. Both are now unblocked.
>
> **Blocks resolved:**
> - Stage 1 codegen in `iris` — turning a parsed `ElementNode` into `.cpp` that
>   constructs real `IrisComponent` values now has a concrete target type to emit against.
> - Stage 2 walker in `iris-penumbra-backend` — reading prop values back out to call
>   `Box::Builder().className(...).onPress(...)` now has a known storage shape to read from.

---

## Decision

`IrisProps` is a closed, strongly-typed variant map:

```cpp
// include/Iris/IrisProps.h

#pragma once
#include <string>
#include <functional>
#include <unordered_map>
#include <variant>
#include "Iris/TextureHandle.h"

namespace Iris {

using IrisPropValue = std::variant<
    std::string,
    int,
    float,
    bool,
    std::function<void()>,
    iris::TextureHandle
>;

using IrisProps = std::unordered_map<std::string, IrisPropValue>;

} // namespace Iris
```

**Rejected alternatives:**
- `std::unordered_map<std::string, std::any>` — type-erased, requires `std::any_cast`
  at every read site, throws on mismatch, no compiler enforcement of what types are
  possible. A maintenance trap that grows worse as prop count increases.
- A narrower subset of the variant — functionally identical to the above since the
  variant is already scoped to what Core primitives currently need. No benefit in
  restricting further.

---

## Prop-name to variant member mapping

The preprocessor's codegen uses prop name to determine which variant member to construct.
This is a closed lookup table — adding a new prop kind requires extending both the variant
and this table deliberately.

| Prop name(s) | Variant member | Notes |
|---|---|---|
| `class` | `std::string` | Stripped to `ClassName` before reaching `IrisProps` in some paths — see §key/class handling below |
| `src` | `std::string` | File path for `<Image>` |
| Any other string literal prop | `std::string` | Default for unrecognised literal string props |
| `onPress`, `onRelease`, `onHover`, `onFocus`, `onChange` | `std::function<void()>` | All event props are zero-argument callbacks |
| `checked` | `bool` | `<Checkbox>` checked state |
| `handle` | `iris::TextureHandle` | Pre-loaded texture reference for `<Image>` |
| Numeric escape hatches | `int` or `float` | Inferred from C++23 type at the call site — the host compiler validates |

---

## `key` and `class` handling

Neither `key` nor `class` are stored in `IrisProps` in the normal sense:

- **`key`** is stripped by the preprocessor entirely before codegen. It never appears
  in an `IrisProps` map. The reconciler's identity map is built separately from the
  element's position and explicit `key` value — not from `IrisProps`.
- **`class`** maps to `std::string` and is stored in `IrisProps` under the key
  `"class"`. The backend reads it out and passes it to the widget builder's
  `.className()` method. It is never passed to Lustre directly from here — Lustre
  resolution is Stage 4's concern.

---

## `IrisPropDiff` alignment

`IrisPropDiff` (from `docs/iris_stage3_decision_doc.md` §4) uses `std::optional` fields
for each prop kind. The set of types must stay in sync with `IrisPropValue`'s variant
members. As of this decision both cover the same set:

| `IrisPropValue` variant member | Corresponding `IrisPropDiff` field |
|---|---|
| `std::string` | `std::optional<std::string> ClassName`, `std::optional<std::string> Src` |
| `std::function<void()>` | `std::optional<std::function<void()>> OnPress` etc. |
| `bool` | `std::optional<bool> Checked` |
| `iris::TextureHandle` | `std::optional<iris::TextureHandle> Handle` |
| `int`, `float` | No `IrisPropDiff` field yet — no Core primitive currently needs a numeric prop diff |

**Rule:** whenever a new variant member is added to `IrisPropValue`, a corresponding
`std::optional` field must be added to `IrisPropDiff`. These two types must be kept in
sync — a prop type that can be constructed but never diffed is a bug waiting to happen.

---

## Stage 1 codegen implications

The preprocessor emits C++23 that constructs `IrisComponent` values. For each prop on
an element, it emits an `IrisProps` entry using the lookup table above:

```cpp
// Source .iris
<Frame class="button" onPress={[&]() { props.onPress(); }}>

// Emitted .cpp
IrisComponent{
    IrisElementTag::Frame,
    IrisProps{
        { "class",   IrisPropValue{ std::string("button") } },
        { "onPress", IrisPropValue{ std::function<void()>([&]() { props.onPress(); }) } },
    },
    {}  // no children shown for brevity
}
```

Escape hatch contents (`[&]() { props.onPress(); }`) are emitted verbatim — the
preprocessor never inspects them. The variant member wrapping them is determined by prop
name from the lookup table, not by parsing the escape hatch's content.

---

## Stage 2 walker implications

The `iris-penumbra-backend` walker reads `IrisProps` entries and calls the appropriate
`Builder` method. `std::visit` or `std::get_if` against known prop names:

```cpp
// iris-penumbra-backend — reading IrisProps to drive Box::Builder
void ApplyProps(Box::Builder& Builder, const Iris::IrisProps& Props) {
    for (const auto& [Key, Value] : Props) {
        if (Key == "class") {
            Builder.className(std::get<std::string>(Value));
        } else if (Key == "onPress") {
            Builder.onPress(std::get<std::function<void()>>(Value));
        }
        // etc.
    }
}
```

`std::get` is safe here because the prop-name-to-type mapping is fixed — a `class` prop
will always be a `std::string`, an `onPress` prop will always be a
`std::function<void()>`. A mismatch would indicate a codegen bug, not a user error, so
a throwing `std::get` is the correct failure mode.

---

## What is now unblocked

Per `docs/iris_next_steps.md`'s suggested order — all items are now unblocked:

1. ~~Decide `IrisProps`~~ — **done, this document**
2. Stage 1 codegen in `iris`
3. Semantic validation pass in `iris`
4. Stage 2 walker in `iris-penumbra-backend`
5. Stage 3 reactive runtime — already fully spec'd, Penumbra-side prerequisites all landed

## What remains deliberately deferred

The following open questions from `docs/iris_core_spec.md` §8 are not resolved here and
are not expected to be resolved until implementation experience surfaces the need:

- **Event-prop vocabulary extensibility** — whether the fixed set is closed or
  extensible. Revisit when a real consumer needs a prop not in the current list.
- **Implicit children forwarding** — no mechanism specified for wrapper components.
  Revisit when a real consumer hits the need.
- **Lustre** — separate design project, separate handoff doc when the time comes.
- **`<Grid>` layout props** — deferred until a consumer needs real grid layout.
- **`umbra-engine` primitive set beyond `<Model3d>`** — Stage 6.
