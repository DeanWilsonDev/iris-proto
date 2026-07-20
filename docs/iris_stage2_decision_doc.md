# Iris — Stage 2 Decision Log

> **Status:** Post-planning. Records decisions made during Stage 2 scoping conversations.
> Intended as a handoff to the agent maintaining `iris_handoff.md` and
> `iris_core_spec.md` — update those documents to reflect the decisions below.
>
> All ten questions from `iris_stage2_open_questions.md` are closed here. Item 1
> (generic interactive-element mechanism) was already closed by `iris_stage1_decision_log.md`
> and `penumbra_iris_changes.md` — noted here for completeness only.
>
> **Correction to item 10 (repo and build integration):** §10's original call — Iris vendors
> Penumbra as a git submodule — has been reversed. Iris's core is meant to be backend-agnostic
> and shouldn't have to pull in one specific backend's build to compile; Penumbra is a
> general-purpose widget library with no inherent reason to know Iris exists either. The Stage
> 2 Penumbra-backend code (the `Component`-IR-to-widget-tree walker) now lives in its own
> repo, `iris-penumbra-backend`, which vendors both Iris and `penumbra-proto` as submodules —
> neither depends on the other directly. This repo (`iris`) has no Penumbra reference anywhere
> in its build. See §10 below for the original (superseded) reasoning, kept for the record.

---

## Summary

| # | Question | Resolution |
|---|---|---|
| 1 | Generic interactive-element mechanism | Closed by `penumbra_iris_changes.md` §4 — `WidgetBase` input callbacks |
| 2 | `<Image>` backend | Build now — minimal `IImageBackend`/`Image` widget. See §2. |
| 3 | `<Grid>` layout | Deferred — stubbed as plain `Box` in Stage 2. See §3. |
| 4 | What is `Component` concretely | Lightweight backend-agnostic IR node. See §4. |
| 5 | Component invocation codegen convention | `<Name>Props` required naming rule. See §5. |
| 6 | `<Inline>` vs `<Text>` mapping | Distinct — `<Inline>` maps to new `InlineContainer` widget. See §6. |
| 7 | Styling stub strategy | Both: Cimmerian tests + visible demo with hardcoded placeholder styles. See §7. |
| 8 | `key` bookkeeping | Build the identity map now in Stage 2. See §8. |
| 9 | What does Stage 2 done look like | Defined concretely. See §9. |
| 10 | Repo and build integration | Standalone repo, CMake. **Superseded**: originally decided Penumbra as a submodule of Iris; reversed — the Penumbra backend lives in a separate `iris-penumbra-backend` repo that depends on both. See §10. |

---

## 1. Generic interactive-element mechanism

Closed by `penumbra_iris_changes.md` §4. `WidgetBase` gains five null-by-default
`std::function` input callback members. Stage 2 waits for this Penumbra change to land
before starting — no need to scope around the gap.

---

## 2. `<Image>` — build now

**Decision:** Build a minimal `IImageBackend`/`Image` widget in Penumbra now, mirroring
`IFontBackend`'s shape.

**Reasoning:** `<Image>` is a Core primitive — every backend is required to implement it
per `docs/iris_core_spec.md` §3.1. Deferring it means Stage 2 cannot claim to cover Core
primitives, which weakens what Stage 2 actually proves. Since a Penumbra implementation
pass is already underway, the marginal cost of adding it now is low.

**Scope:** Minimal asset pipeline only — load PNG/JPG from a file path to an SDL texture.
No caching, no hot reload, no async loading. Those are Umbra Engine concerns. Penumbra
just needs to not crash when given a path.

**See also:** `penumbra_image_handoff.md` for the full Penumbra implementation spec.

---

## 3. `<Grid>` — deferred

**Decision:** `<Grid>` is deferred. Stage 2 stubs it as a plain `Box`.

**Reasoning:** There is currently nothing in Penumbra a `<Grid>` could map onto — no grid
layout mode exists even partially. A real grid layout mode is non-trivial Penumbra work,
and no current consumer needs it. The spec already flagged Grid's props as an open
question and noted it may end up a Lustre concern.

**Stage 2 behaviour:** A `<Grid>` element in an Iris component tree maps to a plain `Box`
with `LayoutMode::HorizontalStack`. This is explicitly documented as not meeting the Core
primitive requirement. Revisit when a real consumer needs it.

---

## 4. `Component` — lightweight IR node

**Decision:** `Component` is a lightweight, backend-agnostic intermediate
representation node. It carries a tag, a props map, and an ordered list of child
`Component` nodes. It has no knowledge of Penumbra, `WidgetBase`, or any concrete
widget type.

```cpp
struct Component {
    IrisElementTag Tag;             // Frame, Inline, Text, Image, or a component name
    IrisProps Props;                // key-value prop map, key already stripped
    std::vector<Component> Children;
};
```

The Penumbra backend walks the IR tree in a separate pass and constructs the live widget
tree from it. The IR is pure Iris — when the Umbra Engine backend arrives at Stage 6, it
walks the same IR and maps it to whatever Nyx needs. The IR never changes between
backends.

**Rejected alternative:** `Component` as a thin facade around `unique_ptr<WidgetBase>`
that calls `AddChild` immediately as `.child()` is invoked. This ties Iris's core type
to Penumbra's ownership model and produces nothing to diff against in Stage 3's
reconciler. Building Stage 2 on that model means throwing it away when Stage 3 starts.

---

## 5. Component invocation codegen convention — `<Name>Props`

**Decision:** Props structs must be named `<ComponentName>Props`. This is a required
naming rule, not a convention — the preprocessor depends on it and will emit incorrect
code if it is violated.

**Reasoning:** The preprocessor never parses struct declarations — they are host code
outside `render { }`. It needs a rule to follow when emitting a call for
`<HealthBar current={player.health} max={player.maxHealth} />`. The `<Name>Props`
convention is already what every example in the codebase uses naturally, making it a
formalisation of existing practice rather than a new constraint.

**Codegen behaviour:**
- `<HealthBar />` → emits `HealthBarProps{...}` and calls `HealthBar(HealthBarProps{...})`
- `<Frame />` → known primitive tag, emits an IR node directly, no props struct lookup

**Documentation requirement:** This rule must be prominently documented in the Iris spec.
It is the one place where violating a naming convention produces incorrect codegen rather
than a compiler error.

---

## 6. `<Inline>` vs `<Text>` — distinct widgets

**Decision:** `<Inline>` and `<Text>` are distinct and must never be used
interchangeably.

- `<Text>` — renders a single string. Maps to Penumbra's existing `Label` widget.
- `<Inline>` — an inline-flow container. Accepts mixed children — text runs, images,
  other inline elements. Maps to a new Penumbra `InlineContainer` widget.

**Reasoning:** `<Inline>` was designed as a container, not a text renderer. Mapping it
to `Label` (same as `<Text>`) or to a plain `Box` would silently misrepresent its
semantics and create a confusing divergence between Penumbra and future backends.
Building `InlineContainer` now establishes the correct mapping from day one.

**See also:** Inline container handoff note already delivered in this conversation.

---

## 7. Styling stub strategy — both tests and visible demo

**Decision:** Stage 2 ships two proofs of correctness:

1. **Cimmerian test suite** — asserts the built Penumbra widget tree's shape, child
   counts, and prop values against the source Iris component tree. This is the
   correctness proof.

2. **Runnable demo window** — a minimal hardcoded style pass makes widgets visible.
   Solid background colours, borders, padding — just enough to confirm the full pipeline
   including SDL rendering and the builder API works end to end. This is the integration
   proof.

**The hardcoded style pass is explicitly temporary.** It lives in a single isolated file
— `Stage2StyleStub.cpp` — and is deleted entirely when Lustre lands at Stage 4. No
placeholder styling logic should bleed into the real pipeline.

---

## 8. `key` bookkeeping — build now

**Decision:** Stage 2's tree builder maintains a `key → WidgetBase*` identity map from
day one, even though nothing reads it until Stage 3's reconciler needs it.

**Reasoning:** The map is built naturally as `AddChild`/`InsertChildAt` return pointers
during tree construction. Capturing those pointers at Stage 2 costs almost nothing.
Retrofitting the bookkeeping at Stage 3 onto a builder that wasn't designed for it is
disproportionately more expensive — exactly the problem `demo/main.cpp`'s hand-rolled
raw pointer captures were working around.

**Implementation:**
- Elements with an explicit `key` prop are keyed by that value
- Elements without an explicit `key` are assigned a stable generated id based on
  depth-first tree position — e.g. `"0"`, `"0.1"`, `"0.1.2"`
- Position-based ids are stable across re-renders as long as the tree structure doesn't
  change — which is exactly the case where the reconciler doesn't need `key` anyway
- The map is owned by the Iris runtime and lives alongside the IR tree

---

## 9. Stage 2 done — concrete definition

Stage 2 is complete when all of the following are true:

- A static Iris component tree (no state, no re-render) parses and maps correctly to a
  live Penumbra widget tree via the IR
- The `key → WidgetBase*` identity map is populated and accessible to Stage 3
- Cimmerian tests pass, asserting tree shape, child counts, and prop values
- A demo window renders a visible component with placeholder styles from `Stage2StyleStub.cpp`
- All Core primitives are exercised: `<Frame>`, `<Text>`, `<Inline>`, `<Image>`
- `<Grid>` is stubbed as `Box` with a documented note that it does not yet meet the
  Core primitive requirement
- The full pipeline runs: `.iris` file → Iris preprocessor → `.cpp` → C++23 compiler →
  Penumbra widget tree → SDL render window

---

## 10. Repo and build integration

> **Superseded — see the correction note at the top of this document.** Kept below verbatim as
> the historical record of the original reasoning; §10's "Penumbra as a submodule of Iris" call
> is no longer what's implemented.

**Original decision:**
- Iris lives in a new standalone repo — consistent with the rest of the Umbra ecosystem
  where every project (Penumbra, Dawn, first-party libs) is a separate repo
- Penumbra is referenced as a **git submodule**
- Build system is **CMake**, matching the rest of the ecosystem

**Reasoning on dependency management:** Git submodules were chosen over CPM.cmake and
Conan. CPM is built on `FetchContent` and adds thin conveniences (shared cache,
deduplication) that are marginal for a solo ecosystem with a handful of repos. Conan
with a private remote is the most powerful option but carries meaningful infrastructure
overhead. Git submodules give pinned, committed version references checked into the
consuming repo — you always know exactly what version of Penumbra Iris was built against
because it's in the git history. The workflow friction (`git submodule update --init
--recursive`) is manageable solo. Revisit a proper package manager if the ecosystem
grows or other developers join.

**Repo structure (initial, as originally decided):**
```
iris/
    docs/
    include/
        Iris/
    src/
        Iris/
    tests/
    demo/
    vendor/
        penumbra/          ← git submodule
    CMakeLists.txt
    .iris.json             ← project config for the demo and tests
```

**What's actually implemented instead:** `iris/` has no `vendor/` directory and no Penumbra
reference at all. A third repo, `iris-penumbra-backend`, vendors both `iris` and
`penumbra-proto` as submodules and owns the Stage 2 backend-mapping code — see the correction
note at the top of this document.
