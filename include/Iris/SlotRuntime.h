#pragma once

#include "Iris/Component.h"

#include "Umbra/IWidget.h"

#include <functional>
#include <memory>
#include <vector>

namespace iris {

// Supplied by whoever embeds Iris (e.g. `iris-penumbra-backend`) to build a fresh
// `Umbra::IWidget`-conforming widget for an `Component` subtree that has no
// existing widget to diff against — a fresh mount, or a remount because the tag/key at
// some position changed. Builds the *whole* subtree, recursively — the reconciler
// (`Reconciler.h`) never recurses into `New.Children` itself on this path, it hands the
// entire node to `Mount` and trusts the result back. Iris's own runtime has no backend
// knowledge (docs/iris_core_spec.md §2.5) — it never constructs a widget itself, only
// asks this callback to (mirroring `Codegen`/`SemanticValidator`'s own "never inspect
// escape hatch contents" posture: the runtime doesn't know what a widget *is*, only
// what `Umbra::IWidget` says it can do).
using MountFn = std::function<std::unique_ptr<Umbra::IWidget>(const Iris::Component&)>;

class SlotState;
class ComponentInstance;
class ReloadTarget;

// Coordinates a set of `<Slot>` siblings attached under the *same* static parent
// (`SlotResolution.h`'s `ResolveSlots` builds one per parent's children list), so a
// later sibling's absolute child index can account for how many real widgets earlier
// siblings *currently* contribute — which, for a list-returning `<Slot>`, can change on
// every re-render (docs/iris_slot_list_wiring_decision.md). A single-`Component`-
// returning `<Slot>` with no siblings under its parent just uses a group with one entry
// and nothing earlier to sum — the same mechanism handles both shapes uniformly.
class SlotSiblingGroup {
public:
    // `StaticPrefixCount` is fixed: the number of *ordinary* (non-`Slot`, non-`None`)
    // children directly preceding this slot — never changes after `ResolveSlots`
    // finishes walking one parent's children. `Slot` is non-owning; whoever calls
    // `ResolveSlots` owns every `SlotState` for the tree's whole mounted lifetime.
    void AddEntry(std::size_t StaticPrefixCount, SlotState* Slot);

    std::size_t EntryCount() const;

    // The absolute index, within the shared parent's real children, that the slot at
    // `GroupIndex` currently occupies: its own fixed static prefix, plus the *live*
    // contribution of every earlier sibling in the group (`SlotState::
    // CurrentRealChildCount()`) — recomputed fresh on every call, since an earlier
    // sibling's contribution may have changed since the last time this was asked.
    std::size_t AbsoluteIndexOf(std::size_t GroupIndex) const;

    // Called by `SlotState::~SlotState()` once it has removed its own widgets from the
    // shared parent, so a sibling destroyed afterward (in either direction — nothing
    // requires siblings to tear down in group order) never dereferences this slot's now-
    // freed `SlotState*`: from this point on, `AbsoluteIndexOf` treats this entry as
    // contributing zero, which is correct since its widgets are already gone.
    void MarkDestroyed(std::size_t GroupIndex);

private:
    struct Entry {
        std::size_t StaticPrefixCount{0};
        SlotState*   Slot{nullptr};
    };
    std::vector<Entry> Entries_;
};

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
// Owns the callable, the last `Component` tree(s) it produced (diffed against on
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

    // Attaches this slot's output to a position within `Parent`'s own real children —
    // coordinated through `Group` (`GroupIndex` is this slot's own position within it) —
    // rather than a standalone widget this `SlotState` owns privately. Used when a
    // `<Slot>` is found nested inside an otherwise-static tree (`SlotResolution.h`'s
    // `ResolveSlots`, which constructs one `Group` per parent and registers every
    // `<Slot>` sibling found under it). Every subsequent `Reconcile()` call — including
    // ones triggered automatically by `IrisRuntime::ReconcileDirtySlots()` (e.g. via
    // `iris::Tick()`) — asks `Group->AbsoluteIndexOf(GroupIndex)` fresh (an earlier
    // sibling's own contribution may have changed since the last call) and updates
    // `Parent`'s real children at that position in place: for the single-
    // `Component`-returning shape, `ReconcileWidget` as before; for the
    // list-returning shape, an equivalent extract/`ReconcileChildren`/reinsert dance
    // over as many consecutive positions as this slot currently occupies. Must be
    // called before the first `Reconcile()`.
    void AttachToGroup(Umbra::IWidget* Parent, std::shared_ptr<SlotSiblingGroup> Group, std::size_t GroupIndex);

    // How many real widgets this slot currently contributes to its attached parent (or
    // would, if standalone) — 0 or 1 for a single-`Component`-returning callable,
    // 0..N for a list-returning one. `SlotSiblingGroup::AbsoluteIndexOf` calls this on
    // every earlier sibling to compute a later one's current absolute position.
    std::size_t CurrentRealChildCount() const;

private:
    // Walks `Widget` (this slot's own just-reconciled real widget, for one position of
    // its current output — the single widget, or one list item) alongside `Node` (the
    // matching `Component`) via `ResolveSlots` to find every `<Slot>` nested inside
    // it, and appends the resulting `SlotState`s to `NestedSlots_`
    // (docs/iris_nested_slot_discovery_decision.md). Called once per position, per
    // `Reconcile()` call, after `NestedSlots_` has already been cleared for this render.
    void DiscoverNestedSlots(Umbra::IWidget& Widget, const Iris::Component& Node);

    std::shared_ptr<Iris::IrisSlotCallable> Callable_;
    MountFn                                  Mount_;
    bool                                      Mounted_{false};
    bool                                      Dirty_{false};

    Umbra::IWidget*                    AttachedParent_{nullptr};
    std::shared_ptr<SlotSiblingGroup>  AttachedGroup_;
    std::size_t                         AttachedGroupIndex_{0};
    std::size_t                         AttachedCount_{0}; // how many real widgets this slot currently owns, when attached

    // Exactly one of these two pairs is meaningful, matching which alternative
    // `Callable_->Callable` holds — mirrors `IrisSlotCallable`'s own variant shape
    // rather than introducing a parallel one. In attached mode (`AttachedParent_ !=
    // nullptr`), `SingleWidget_`/`ListWidgets_` stay unused — the live widget(s) live
    // inside `AttachedParent_`'s own children between `Reconcile()` calls, never here.
    Iris::Component                          PreviousSingle_{nullptr}; // starts at IrisElementTag::None — "nothing was here"
    std::unique_ptr<Umbra::IWidget>              SingleWidget_;
    std::vector<Iris::Component>             PreviousList_;
    std::vector<std::unique_ptr<Umbra::IWidget>> ListWidgets_;

    // Every `<Slot>` found nested inside this slot's own *current* rendered output —
    // discovered fresh on every `Reconcile()` call, mount and re-render alike
    // (docs/iris_nested_slot_discovery_decision.md). Declared last: `~SlotState()`
    // explicitly clears this before touching `SingleWidget_`/`ListWidgets_`/
    // `AttachedParent_` below, since a nested entry's own `AttachedParent_` is a widget
    // living inside one of those — order matters, this comment is not a substitute for
    // that explicit clear.
    std::vector<std::unique_ptr<SlotState>> NestedSlots_;
};

// The process-wide batching/dirty-tracking/active-slot-stack owner
// (docs/iris_stage3_decision_doc.md §6's `IrisRuntime`). A singleton: `iris::Signal<T>`
// and `<Slot>` callables have no explicit handle to thread one through, matching how
// the spec itself never shows a runtime instance being passed around.
class IrisRuntime {
public:
    static IrisRuntime& Instance();

    // Declared (not defaulted here) so it's defined out-of-line, in SlotRuntime.cpp,
    // where ReloadTarget_ below is a complete type -- the usual fix for a unique_ptr
    // member of a forward-declared type (ReloadTarget.h itself includes this header for
    // MountFn, so the reverse include would be circular; forward-declaring it here and
    // completing the type only where it's actually destroyed avoids that).
    ~IrisRuntime();

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

    // The whole-application live-widget registry
    // (docs/archive/iris_next_steps_resolved.md, "Live-widget root registry, for Lustre's
    // hot-reload") — holds whatever `Umbra::IWidget*` the consuming
    // app last registered as its mounted tree's root, with zero backend-specific content
    // (`Umbra::IWidget` is already the backend-agnostic interface the reconciler itself
    // walks/mutates through). A later `RegisterRoot` call simply replaces the previous
    // one; Iris does not own the widget's lifetime, only the pointer to it, mirroring
    // `MountFn`'s own "the caller owns what it builds" convention.
    void            RegisterRoot(Umbra::IWidget* Root);
    Umbra::IWidget* GetRoot() const;

    // The whole-app reconcile-target registry (docs/
    // iris_hot_reload_reconciliation_decision.md §2) — separate from RegisterRoot/GetRoot
    // above, not a replacement for them (see ReloadTarget.h's own doc comment for why).
    // Null until an app that actually wants reload calls RegisterReloadTarget; ordinary
    // Stage 2/3 apps never touch this and pay nothing for it.
    void          RegisterReloadTarget(std::unique_ptr<ReloadTarget> Target);
    ReloadTarget* GetReloadTarget() const;

private:
    IrisRuntime() = default;

    std::vector<SlotState*>         ActiveSlotStack_;
    std::vector<SlotState*>         DirtySlots_;
    int                              BatchDepth_{0};
    std::vector<ComponentInstance*> ComponentInstanceStack_;
    std::unique_ptr<ReloadTarget>    ReloadTarget_;
    Umbra::IWidget*                 Root_{nullptr};
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

// Registers `Root` as the whole application's mounted-widget-tree entry point
// (`IrisRuntime::RegisterRoot`) — callable by any consuming app right after it builds its
// tree. Generic over any backend's `IWidget` implementation by construction; benefit
// beyond the Lustre hot-reload need that prompted this is that the next cross-cutting
// concern needing "the whole mounted tree" (a debugger, an inspector) gets this for free.
void RegisterRoot(Umbra::IWidget* Root);

// The most recently registered root, or nullptr if none has been
// (docs/archive/iris_next_steps_resolved.md, "Live-widget root registry, for Lustre's
// hot-reload").
Umbra::IWidget* GetRoot();

// Registers `Target` as the whole application's reload registry
// (`IrisRuntime::RegisterReloadTarget`, docs/iris_hot_reload_reconciliation_decision.md
// §2) — callable by any consuming app that wants hot reload, right after it builds its
// initial tree. A later call replaces the previous target, same convention as
// `RegisterRoot`.
void RegisterReloadTarget(std::unique_ptr<ReloadTarget> Target);

// The most recently registered reload target, or nullptr if none has been.
ReloadTarget* GetReloadTarget();

} // namespace iris
