# Iris — Stage 3 Decision: `<Slot>` Replaces Escape-Hatch IIFEs for Dynamic Regions

> **Status:** Urgent change, delivered directly (not as a separate handover file this time).
> Supersedes an earlier, unrecorded handover brief about re-invokable lambdas that this repo's
> docs never saw directly. Closes the foundational question `docs/iris_stage3_open_questions.md`
> opened: how `iris::Signal<T>` survives being declared as a "local variable" across re-renders.

---

## What changed and why

Stage 3 planning introduced `<Slot>` as a first-class Iris Core runtime primitive. It replaces
the previous plan of storing escape-hatch lambdas as named variables. **The component function
still runs once at mount** — but dynamic regions are now explicitly marked in the source by the
author using `<Slot>`, not implicitly detected by the runtime via generated variable names.

This resolves `docs/iris_stage3_open_questions.md`'s lead question in favor of the "instance-owned,
run-once component bodies" candidate: the outer function executes once, establishing
`iris::Signal<T>` locals as genuinely persistent (no call-order-indexed slot trick needed,
avoiding the React Hooks footgun that approach carried) — only `<Slot>`'s callables get
re-invoked later, closing over those same signals by reference.

## What `<Slot>` is

A Core Iris runtime primitive. Its single child is a callable C++23 lambda returning
`Component` or `std::vector<Component>`. The runtime invokes it at mount and re-invokes
it when signals it captures fire. **The backend never sees a `<Slot>` node** — the runtime
resolves it before the backend pass runs.

## What it looks like in source

```cpp
// Conditional rendering
<Slot>
    {[&]() -> Component {
        if (settingsOpen.get()) {
            return <SettingsPage onClose={[&]() { settingsOpen.set(false); }} />;
        }
        return nullptr;
    }}
</Slot>

// List rendering
<Slot>
    {[&]() -> std::vector<Component> {
        std::vector<Component> result;
        for (auto& item : props.items) {
            result.push_back(<Item key={item.id} label={item.name} />);
        }
        return result;
    }}
</Slot>
```

Note the lambda is **not** immediately invoked — no trailing `()` the way the old IIFE pattern
had. The lambda value itself is what the escape hatch evaluates to; the `<Slot>` runtime
machinery holds and calls it, rather than the generated code calling it once inline.

## Codegen implications for Stage 1

- `<Slot>` is a known Core primitive tag — the preprocessor handles it like `<Frame>` or
  `<Text>`, emitting an IR node with tag `Slot`.
- Its single child is a standard escape hatch `{ }` — balanced brace matching, contents emitted
  verbatim as C++23.
- The child must be a callable — **the runtime enforces this, not the preprocessor.**
- No stored lambda variables, no generated names, no special codegen case — the preprocessor
  treats `<Slot>` as a completely normal element.

## Constraints the runtime enforces (not the preprocessor)

- `<Slot>` must have exactly one child.
- That child must be a callable returning `Component` or `std::vector<Component>`.
- Multiple children or a non-callable child is a runtime error.

## What doesn't change

- Surface grammar is identical for everything outside `<Slot>`.
- Escape hatches inside non-`<Slot>` elements are still one-shot, as originally designed —
  evaluated once at construction, never revisited. Use them for genuinely static values (text
  interpolation, prop values, event handlers); use `<Slot>` for anything that needs to react to
  a signal.
- The `{ }` balanced-brace matching logic in the preprocessor is unchanged.
- `<Slot>` needs adding to the Core primitive set in `iris_core_spec.md` §3.1.
