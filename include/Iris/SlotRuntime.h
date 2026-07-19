#pragma once

#include "Iris/IrisComponent.h"

#include "Umbra/IWidget.h"

#include <functional>
#include <memory>
#include <vector>

namespace iris {

// Supplied by whoever embeds Iris (e.g. `iris-penumbra-backend`) to build a fresh
// `Umbra::IWidget`-conforming widget for an `IrisComponent` subtree that has no
// existing widget to diff against — a fresh mount, or a remount because the tag/key at
// some position changed. Builds the *whole* subtree, recursively — the reconciler
// (`Reconciler.h`) never recurses into `New.Children` itself on this path, it hands the
// entire node to `Mount` and trusts the result back. Iris's own runtime has no backend
// knowledge (docs/iris_core_spec.md §2.5) — it never constructs a widget itself, only
// asks this callback to (mirroring `Codegen`/`SemanticValidator`'s own "never inspect
// escape hatch contents" posture: the runtime doesn't know what a widget *is*, only
// what `Umbra::IWidget` says it can do).
using MountFn = std::function<std::unique_ptr<Umbra::IWidget>(const Iris::IrisComponent&)>;

class SlotState;
class ComponentInstance;

// The ambient "active slot" tracking `iris::Signal<T>` (`Signal.h`) needs and nothing
// else should call directly: `TrackSignalDependency` registers whichever `SlotState` is
// currently invoking its callable (`IrisRuntime::ActiveSlot()`) as a dependent of the
// signal identified by `SignalIdentity` (its own `this` pointer, type-erased — the
// tracking mechanism never needs to know `T`). `NotifySignalDependents` marks every
// registered dependent dirty. Declared here, not as a private implementation detail,
// only because `Signal.h` (a header, instantiated per `T` at every call site) needs to
// call across the same translation-unit boundary as the actual bookkeeping
// (`SlotRuntime.cpp`) without becoming a template itself.
void TrackSignalDependency(const void* SignalIdentity);
void NotifySignalDependents(const void* SignalIdentity);

// One `<Slot>`'s live reactive state (docs/iris_stage3_decision_doc.md's `SlotState`).
// Owns the callable, the last `IrisComponent` tree(s) it produced (diffed against on
// re-invocation — "`<Slot>`-scoped diffing": never mixed with a sibling slot's own
// state or the static tree around it), and the live widget(s) built/reconciled from
// that output.
//
// Assumes the trees its callable produces contain no further `<Slot>` tags of their
// own — matching Stage 2's own documented precondition ("the tree a backend pass ever
// sees is fully concrete — no Slot nodes"). Discovering and giving a nested `<Slot>`
// its own `SlotState` is deliberately not this class's job; see
// docs/iris_stage3_implementation_decision.md's "What remains deliberately deferred".
class SlotState {
public:
    SlotState(std::shared_ptr<Iris::IrisSlotCallable> Callable, MountFn Mount);
    ~SlotState();

    SlotState(const SlotState&) = delete;
    SlotState& operator=(const SlotState&) = delete;

    // Invokes the callable if this `SlotState` has never been reconciled before (the
    // bootstrap "mount" case — the owner is expected to call this once after
    // constructing a `SlotState`) or `MarkDirty()` was called since the last
    // `Reconcile()` (a dependent signal fired). Diffs the new output against whatever
    // this slot produced last time and applies the result to its own live widget(s) —
    // mounting, unmounting, updating in place, or reordering as needed
    // (`Reconciler.h`). A no-op otherwise.
    void Reconcile();

    // Called by `NotifySignalDependents` when a signal this slot's last invocation read
    // fires. Does not reconcile immediately — only records that this slot needs
    // reconciling next time `Reconcile()` runs (docs/iris_stage3_decision_doc.md §6/§7:
    // reconciliation only ever happens inside `iris::Tick()`, never synchronously
    // inside a signal's `set()`).
    void MarkDirty();

    // Attaches this slot's output to a specific position within `Parent`'s own real
    // children, rather than a standalone widget this `SlotState` owns privately — used
    // when a `<Slot>` is found nested inside an otherwise-static tree
    // (`SlotResolution.h`'s `ResolveSlots`). Every subsequent `Reconcile()` call —
    // including ones triggered automatically by `IrisRuntime::ReconcileDirtySlots()`
    // (e.g. via `iris::Tick()`) — updates `Parent`'s child at `Index` in place instead
    // of this `SlotState`'s own private storage: `Parent->RemoveChildAt(Index)` right
    // before invoking the callable (only if something is currently there — see below),
    // `ReconcileWidget` as usual, `Parent->InsertChildAt(Index, ...)` right after (only
    // if the new output isn't `None`). Must be called before the first `Reconcile()`,
    // and only supports the single-`IrisComponent`-returning callable shape — a
    // list-returning `<Slot>` attached this way would need `Index` (and every static
    // sibling after it) to shift as the list grows/shrinks, which `ResolveSlots`
    // deliberately doesn't attempt (see its own doc comment).
    void AttachAt(Umbra::IWidget* Parent, std::size_t Index);

    // True if this slot's most recent render produced a real widget (its callable
    // didn't return `nullptr`/`IrisElementTag::None`) — meaningful only for the
    // single-`IrisComponent`-returning callable shape (`SlotResolution.h`'s
    // `ResolveSlots` uses this to know whether a subsequent static sibling's real
    // child index needs to account for this slot's own contribution).
    bool HasMountedContent() const;

private:
    std::shared_ptr<Iris::IrisSlotCallable> Callable_;
    MountFn                                  Mount_;
    bool                                      Mounted_{false};
    bool                                      Dirty_{false};

    Umbra::IWidget* AttachedParent_{nullptr};
    std::size_t      AttachedIndex_{0};

    // Exactly one of these two pairs is meaningful, matching which alternative
    // `Callable_->Callable` holds — mirrors `IrisSlotCallable`'s own variant shape
    // rather than introducing a parallel one. In attached mode (`AttachedParent_ !=
    // nullptr`), `SingleWidget_` stays unused — the live widget lives inside
    // `AttachedParent_`'s own children between `Reconcile()` calls, never here.
    Iris::IrisComponent                          PreviousSingle_{nullptr}; // starts at IrisElementTag::None — "nothing was here"
    std::unique_ptr<Umbra::IWidget>              SingleWidget_;
    std::vector<Iris::IrisComponent>             PreviousList_;
    std::vector<std::unique_ptr<Umbra::IWidget>> ListWidgets_;
};

// The process-wide batching/dirty-tracking/active-slot-stack owner
// (docs/iris_stage3_decision_doc.md §6's `IrisRuntime`). A singleton: `iris::Signal<T>`
// and `<Slot>` callables have no explicit handle to thread one through, matching how
// the spec itself never shows a runtime instance being passed around.
class IrisRuntime {
public:
    static IrisRuntime& Instance();

    // Wraps a batch of signal writes — typically one event-handler invocation
    // (docs/iris_stage3_decision_doc.md §6) — so several `Signal<T>::set()` calls in a
    // row only reconcile once, at the matching `EndBatch()`, rather than once per
    // `set()`. Nestable: reconciliation only actually runs once the outermost
    // `EndBatch()` brings the depth back to zero. Wiring this around real event
    // dispatch (e.g. Penumbra's `WidgetBase::OnPressed` invocation) is a backend
    // adapter's job, not this runtime's — see `ScopedEventBatch` below for the RAII
    // shape such an adapter is expected to use.
    void BeginBatch();
    void EndBatch();

    // Reconciles every slot currently marked dirty, then clears that set. What
    // `iris::Tick()` (`SlotRuntime.h`) calls every frame, and what a balanced
    // `BeginBatch()`/`EndBatch()` pair calls once the depth reaches zero — the same
    // underlying pass either way; `Tick()` exists to also catch anything marked dirty
    // from outside any batch (e.g. a signal set from a background thread,
    // docs/iris_stage3_decision_doc.md §7).
    void ReconcileDirtySlots();

    void RegisterDirtySlot(SlotState* Slot);
    void UnregisterSlot(SlotState* Slot);

    void       PushActiveSlot(SlotState* Slot);
    void       PopActiveSlot();
    SlotState* ActiveSlot() const;

    // The ambient "current component instance" `ComponentInstance.h`'s
    // `MountComponentInstance`/`IRIS_SIGNAL` machinery needs (docs/iris_signal_lifetime_
    // decision.md) — the same push/pop-a-stack pattern as `PushActiveSlot` above, applied
    // to a second problem: while a component function's body is running, any
    // `IRIS_SIGNAL` declaration inside it allocates against whichever
    // `ComponentInstance` is on top of this stack, rather than living as a stack local
    // that dangles once the function returns. Not part of the public component-author
    // API — set up by generated code (`Codegen.h`'s wrapping of every component
    // invocation) or `iris::Mount()`, never called directly.
    void               PushComponentInstance(ComponentInstance* Instance);
    void               PopComponentInstance();
    ComponentInstance* CurrentComponentInstance() const;

private:
    IrisRuntime() = default;

    std::vector<SlotState*>         ActiveSlotStack_;
    std::vector<SlotState*>         DirtySlots_;
    int                              BatchDepth_{0};
    std::vector<ComponentInstance*> ComponentInstanceStack_;
};

// RAII convenience for a backend adapter's own event-dispatch code to wrap a handler
// invocation in `IrisRuntime::BeginBatch()`/`EndBatch()` without matching them by hand.
class ScopedEventBatch {
public:
    ScopedEventBatch() { IrisRuntime::Instance().BeginBatch(); }
    ~ScopedEventBatch() { IrisRuntime::Instance().EndBatch(); }

    ScopedEventBatch(const ScopedEventBatch&) = delete;
    ScopedEventBatch& operator=(const ScopedEventBatch&) = delete;
};

// Called once per frame by the host's own frame loop, main-thread only
// (docs/iris_stage3_decision_doc.md §7's `PenumbraApp::Tick` pseudocode calls this
// directly and unconditionally, before `Measure()`/`Arrange()`/`Draw()`). Reconciles
// every slot marked dirty since the last `Tick()` or batch flush.
void Tick();

} // namespace iris
