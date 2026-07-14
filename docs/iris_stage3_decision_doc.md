# Iris — Stage 3 Decision Log

> **Status:** Post-planning. Records decisions made during Stage 3 scoping conversations.
> Intended as a handoff to the agent maintaining `iris_handoff.md`, `iris_core_spec.md`,
> and `iris_stage3_open_questions.md` — update those documents to reflect the decisions
> below.
>
> All ten questions from `iris_stage3_open_questions.md` are closed here. The foundational
> signal-persistence question and Item 1 (reconciliation model) are the two decisions
> everything else depends on — read those first.
>
> **Important:** This doc also contains decisions that affect Stage 1 and Stage 2
> implementations currently in progress. See the flagged notes in the foundational
> question and §4 — pass these to the relevant implementation agents immediately.

---

## Summary

| # | Question | Resolution |
|---|---|---|
| Foundational | Signal persistence across re-renders | Instance-owned, run-once component bodies. `<Slot>` for reactive regions. See §0. |
| 1 | Diff-based vs fine-grained reconciliation | Slot-scoped diffing. See §1. |
| 2 | Same-position matching rule | Standard rule, slot-scoped. See §2. |
| 3 | Keyed list diffing algorithm | Minimal-move (LIS-based). See §3. |
| 4 | Prop-level update mechanism | Strongly-typed `IrisPropDiff`. See §4. |
| 5 | `<Image>` update path | Synchronous for `src`, zero-cost for `handle`. See §5. |
| 6 | Batching | Batched within handler invocation. See §6. |
| 7 | Frame loop integration | Explicit `iris::Tick()` per frame. See §7. |
| 8 | Lifecycle hooks | `IWidgetLifecycle` — `OnMount`, `OnUnmount`, `OnTick`. See §8. |
| 9 | Cross-component state sharing | No formal mechanism — composition preferred. See §9. |
| 10 | Penumbra API verification | Verification task for Stage 3 kickoff. See §10. |

---

## 0. Foundational — signal persistence and `<Slot>`

**Decision:** Component functions run once at mount. `iris::Signal<T>` instances are
true long-lived locals established at mount time and captured by reference in render
lambdas. Only the reactive regions of `render { }` re-execute when a signal fires.
The component function body itself never re-runs.

**Reactive regions are marked explicitly by the author using `<Slot>`** — a new Core
Iris runtime primitive. `<Slot>`'s single child is a callable C++23 lambda returning
`IrisComponent` or `std::vector<IrisComponent>`. The runtime invokes it at mount and
re-invokes it when signals it captures fire. The backend never sees a `<Slot>` node —
the runtime resolves it before the backend pass runs.

```cpp
IrisComponent StartMenu() {
    iris::Signal<bool> settingsOpen = false;

    render {
        <Frame class="start-menu">
            <Button
                label="Settings"
                onPress={[&]() { settingsOpen.set(true); }}
            />
            <Slot>
                {[&]() -> IrisComponent {
                    if (settingsOpen.get()) {
                        return <SettingsPage
                            onClose={[&]() { settingsOpen.set(false); }}
                        />;
                    }
                    return nullptr;
                }}
            </Slot>
        </Frame>
    }
}
```

List rendering via `<Slot>`:

```cpp
<Slot>
    {[&]() -> std::vector<IrisComponent> {
        std::vector<IrisComponent> result;
        for (auto& item : props.items) {
            result.push_back(<Item key={item.id} label={item.name} />);
        }
        return result;
    }}
</Slot>
```

**Rejected alternatives:**
- Call-order-indexed slots (React Hooks style) — inherits the well-known footgun where
  signals declared inside conditionals corrupt later slot indices silently.
- Keyed identity piggybacking on the Stage 2 position map — plausible but the `<Slot>`
  primitive is cleaner, more explicit, and self-documenting.

**`<Slot>` runtime constraints (enforced by the runtime, not the preprocessor):**
- Must have exactly one child
- That child must be a callable returning `IrisComponent` or `std::vector<IrisComponent>`
- Multiple children or a non-callable child is a runtime error

**`<Slot>` must be added to the Core primitive set in `docs/iris_core_spec.md` §3.1.**

---

### ⚠️ Stage 1 implementation impact — act on this now

Escape hatches inside `<Slot>` children are re-invokable — the runtime calls them again
when signals fire. Escape hatches outside `<Slot>` remain one-shot, as originally
designed. The preprocessor treats `<Slot>` as a normal Core primitive element — no
special codegen case. The re-invokability is a runtime concern, not a preprocessor
concern. Pass this to the Stage 1 implementation agent.

---

### ⚠️ Stage 2 implementation impact — act on this now

The `key → IWidget*` identity map built in Stage 2 must use `IWidget*`, not
`WidgetBase*`. The Iris runtime is backend-agnostic and must never reference Penumbra
types directly. See §4 for the full `IWidget` interface. Pass this to the Stage 2
implementation agent.

---

## 1. Reconciliation model — slot-scoped diffing

**Decision:** The Iris runtime uses slot-scoped diffing. On a signal trigger, only the
`<Slot>` instances that captured that signal are re-invoked. The runtime diffs only the
new output of those slots against their previous output and patches the live backend
widget tree. Everything outside a `<Slot>` is static mount-time structure and is never
touched after mount.

**Rejected alternatives:**
- Full tree diff (React-style) — diffs entire subtrees on every trigger, no benefit from
  `<Slot>`'s explicit reactive boundaries.
- Fine-grained direct updates (SolidJS-style) — requires automatic dependency tracking,
  which needs either compiler support or an explicit subscription API. Neither is
  available in the current architecture.

**Slot-scoped diffing gets most of fine-grained's performance benefit — no whole-tree
diff — with none of the dependency tracking complexity**, because `<Slot>` lambdas
capture signals explicitly via C++23 `[&]`. The runtime re-invokes a slot when any
signal it captures fires; the lambda handles the rest.

**Per-slot runtime state:**

```cpp
struct SlotState {
    std::function<IrisComponent()> Lambda;       // or vector<IrisComponent> variant
    IrisComponent PreviousOutput;                // last rendered output, diffed on re-invoke
    std::unique_ptr<IWidget> RootWidget;         // live backend widget this slot owns
};
```

`IWidget` is a backend-agnostic interface — see §4. `WidgetBase*` must never appear
in `SlotState` or anywhere else in the Iris runtime.

---

## 2. Same-position matching rule

**Decision:** Within a `<Slot>`'s output, the reconciler applies the following rule:

> Same tag + same `key` at the same tree position → update in place, recurse into
> children. Different tag at the same position → unmount old, mount new.

**On remount:** When a position remounts — old widget discarded, new one created — any
signals tied to that position are also discarded. A fresh component instance with fresh
signals is created. This is consistent with the foundational decision that signals are
long-lived locals on the component instance established at mount.

---

## 3. Keyed list diffing — minimal-move algorithm

**Decision:** When a `<Slot>` returns a `std::vector<IrisComponent>`, the runtime diffs
the new list against the previous one by `key` using a minimal-move algorithm based on
longest increasing subsequence (LIS). The minimum set of `MoveChild`/`InsertChildAt`/
`RemoveChild` operations is computed and applied to reach the new order. Existing widget
instances are preserved where possible.

**Rejected alternative — naive (remove all, re-add in new order):** correct but discards
all live widget instances on every reorder, losing transient state like scroll position
and focus. Produces subtle, hard-to-reproduce bugs in real UIs.

**Penumbra already supports this:** `MoveChild`/`InsertChildAt`/`RemoveChild` all landed
in the Penumbra implementation pass (`docs/penumbra_iris_backend_requirements.md`).

---

## 4. Prop-level update mechanism — `IrisPropDiff`

**Decision:** For matched old/new widget pairs (same tag + key, update in place), the
reconciler computes a strongly-typed `IrisPropDiff` containing only changed props and
applies it via `IWidget::ApplyPropDiff`. No string matching at runtime.

**`IrisPropDiff`** — lives in the Iris runtime, not in any backend:

```cpp
struct IrisPropDiff {
    std::optional<std::string> ClassName;
    std::optional<std::string> Text;
    std::optional<std::string> Src;
    std::optional<iris::TextureHandle> Handle;
    std::optional<bool> Checked;
    std::optional<std::function<void()>> OnPress;
    std::optional<std::function<void()>> OnRelease;
    std::optional<std::function<void()>> OnHover;
    std::optional<std::function<void()>> OnFocus;
    std::optional<std::function<void()>> OnChange;
};
```

**`IWidget`** — backend-agnostic interface, lives in Penumbra temporarily, to be
extracted to `umbra-interfaces` when Umbra Engine needs it:

```cpp
class IWidget {
public:
    virtual void ApplyPropDiff(const IrisPropDiff& Diff) = 0;
    virtual ~IWidget() = default;
};
```

**Penumbra backend implementation:**

```cpp
// Penumbra/PenumbraWidget.cpp
void PenumbraWidget::ApplyPropDiff(const IrisPropDiff& Diff) {
    if (Diff.ClassName) Widget->ClassName = *Diff.ClassName;
    if (Diff.Text)      Label()->Text = *Diff.Text;
    if (Diff.Checked)   Checkbox()->Checked = *Diff.Checked;
    if (Diff.OnPress)   Widget->OnPressed = *Diff.OnPress;
    // etc.
}
```

**Rejected alternative — `SetProp(std::string_view, IrisPropValue)`:** a stringly-typed
switch statement that grows forever with no type safety. Rejected in favour of the
strongly-typed diff approach.

---

## 5. `<Image>` update path

**Decision:** `<Image>` supports two props with different update costs:

- **`src`** — path-based, synchronous load from disk on every `src` change during
  reconciliation. Acceptable for static images in tool UI. Not suitable for animated
  use cases.
- **`handle`** — pre-loaded `iris::TextureHandle`, an opaque runtime type wrapping a
  backend texture. Swapping handles during reconciliation is a pointer assignment — zero
  disk I/O, suitable for dynamic and animated use cases.

**For animated use cases** (e.g. 2D animation preview in Dawn), the consumer pre-loads
frames via `iris::LoadTextures()` and drives the active frame via a signal:

```cpp
std::vector<iris::TextureHandle> frames = iris::LoadTextures({
    "assets/anim/frame_0.png",
    "assets/anim/frame_1.png",
    "assets/anim/frame_2.png"
});

iris::Signal<int> currentFrame = 0;

render {
    <Slot>
        {[&]() -> IrisComponent {
            return <Image handle={frames[currentFrame.get()]} />;
        }}
    </Slot>
}
```

`iris::LoadTextures()` lives in the Iris runtime and delegates to the backend's
`IImageBackend`. The consumer never touches `IImageBackend` directly.

**Synchronous re-decode on `src` change is accepted for Stage 3.** Revisit if a real
consumer hits the limitation with static `src` props.

---

## 6. Batching — within handler invocation

**Decision:** Signal mutations are batched within a single handler invocation. All
`.set()` calls within one handler are queued. A single reconciliation pass runs after
the handler returns. One diff, one mutation pass, no intermediate states.

The runtime maintains a dirty set of `<Slot>` instances whose signals fired during the
handler. Only those slots are re-invoked in the reconciliation pass.

```cpp
class IrisRuntime {
    std::vector<SlotState*> DirtySlots;
    bool BatchingActive = false;

    void BeginBatch()  { BatchingActive = true; }
    void EndBatch() {
        BatchingActive = false;
        ReconcileDirtySlots();
        DirtySlots.clear();
    }
};
```

The runtime wraps every event handler invocation in `BeginBatch`/`EndBatch`
automatically. The component author never thinks about batching.

---

## 7. Frame loop integration — explicit `iris::Tick()`

**Decision:** The Iris runtime exposes an explicit `iris::Tick()` entry point. The host
calls it once per frame in the main loop. Reconciliation only ever happens inside
`iris::Tick()`. Signals set from any context mark slots dirty but never trigger
reconciliation directly.

**Host main loop integration:**

```cpp
while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        penumbra.HandleEvent(event);  // fires handlers, marks slots dirty
    }

    iris::Tick();                     // reconciles dirty slots, mutates widget tree

    penumbra.Measure();
    penumbra.Arrange();
    penumbra.Draw();
}
```

**Thread safety:** `iris::Tick()` is always called on the main thread. The widget tree
is only ever mutated on the main thread. Signals may be set from any thread — they mark
slots dirty but never touch the widget tree directly.

---

## 8. Lifecycle hooks — `IWidgetLifecycle`

**Decision:** Stage 3 includes minimal lifecycle hooks: `OnMount`, `OnUnmount`, and
`OnTick`. These are defined on a shared `IWidgetLifecycle` interface that lives in
Penumbra temporarily and will be extracted to `umbra-interfaces` when Umbra Engine needs
it.

**Neither Penumbra nor Umbra Engine reference each other directly — ever.** They are
mirrors of each other. When `umbra-interfaces` is extracted, both depend on it
independently with no cross-dependency.

**Interface — lives in Penumbra temporarily:**

```cpp
// include/Penumbra/IWidgetLifecycle.h
namespace Penumbra {
    struct TickInfo {
        float DeltaSeconds = 0.0f;
    };

    class IWidgetLifecycle {
    public:
        virtual void OnMount() {}
        virtual void OnUnmount() {}
        virtual void OnTick(const TickInfo& Info) {}
        virtual ~IWidgetLifecycle() = default;
    };
}
```

**Penumbra application loop calls the interface with no Iris dependency:**

```cpp
void PenumbraApp::Tick(float DeltaSeconds) {
    TickInfo Info { .DeltaSeconds = DeltaSeconds };
    for (auto& Lifecycle : RegisteredLifecycles) {
        Lifecycle->OnTick(Info);
    }
    iris::Tick();
    Measure();
    Arrange();
    Draw();
}
```

**Umbra Engine superset rule:** When Umbra Engine implements `IWidgetLifecycle`, it must
be a strict superset of Penumbra's base interface — every method on
`Penumbra::IWidgetLifecycle` must exist with the same signature. Umbra Engine may add
methods Penumbra doesn't have but may never remove or change ones Penumbra defined.
Iris's implementation of the base interface works against both backends without
modification.

**Component author registration:**

```cpp
IrisComponent AnimationPreview() {
    iris::Signal<int> currentFrame = 0;

    iris::RegisterLifecycle(new class : public Penumbra::IWidgetLifecycle {
        void OnMount() override { /* setup */ }
        void OnUnmount() override { /* teardown */ }
        void OnTick(const Penumbra::TickInfo& Info) override {
            if (Info.DeltaSeconds > frameInterval) {
                currentFrame.set((currentFrame.get() + 1) % frameCount);
            }
        }
    });

    render {
        <Image handle={frames[currentFrame.get()]} />
    }
}
```

**Note:** The registration ergonomics are verbose in C++23. Nyx will clean this up in
`.irisx` files. For now it is functional and the interface is sound.

---

## 9. Cross-component state sharing

**Decision:** No formal shared-state or context mechanism exists in Iris. Props drilling
is the recommended pattern — state is lifted to the appropriate level in the component
tree and passed down via props. Composition is preferred over shared state in all cases.

Cross-component state sharing via C++23 scope — a signal declared at a wider scope
captured by multiple components — is possible but undocumented and left to the
developer's discretion. Iris has no way to prevent it and no reason to formalise it.

---

## 10. Penumbra API verification — Stage 3 kickoff task

This is a verification task, not a design decision. The Stage 3 implementation agent
must grep the actual Penumbra source against the following checklist before writing
reconciler code. Do not trust decision doc prose alone — verify against shipped code,
the same way Stage 2's `<Image>` gap was caught.

**Structural mutation — required for minimal-move list differ:**
- `RemoveChild`
- `ReplaceChild`
- `ClearChildren`
- `MoveChild`
- `InsertChildAt`

**Prop-level mutation — required for matched-pair updates:**
- Every prop in `IrisPropDiff` (§4) has a corresponding mutable public field on every
  relevant widget. Verified for `ClassName`, `Text`, `Checked` — not yet verified for
  every widget/prop combination Stage 3 will need. Check all.

**Lifecycle — required for `OnMount`/`OnUnmount`/`OnTick`:**
- [x] `IWidgetLifecycle` interface exists at `include/Penumbra/IWidgetLifecycle.h` — landed in
      `penumbra-proto` commit `663fece`, matching this shape exactly.
- [x] Penumbra has an application loop that calls `OnTick` — `Application::Tick(float)`
      dispatches to every registered `IWidgetLifecycle`, meant to be called before
      `iris::Tick()` (same commit). `iris::Tick()` itself doesn't exist yet (Stage 3
      implementation work, not a Penumbra-side prerequisite).

**Tree walking — required for the reconciler:**
- `GetChildCount` on `WidgetBase`
- `GetChildAt` on `WidgetBase`
- Correct implementations on `Box` and `SplitPanel`

---

## Appendix — interfaces to extract to `umbra-interfaces` when Umbra Engine needs them

The following currently live in Penumbra. When Umbra Engine needs them, extract to a
standalone `umbra-interfaces` header library. No changes to the interfaces themselves —
just a change of home.

- `IWidgetLifecycle` + `TickInfo`
- `IWidget`
- `IrisPropDiff`

Neither Penumbra nor Umbra Engine should reference each other at that point or ever.
