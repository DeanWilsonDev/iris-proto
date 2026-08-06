# Iris hot reload — a concrete design for tiered reconciliation

> **Status:** Proposed design, not yet implemented, not yet confirmed by Dean. Follow-up to
> `docs/iris_hot_reload_alignment_decision.md`, which scoped three open questions (§1
> `ComponentInstance` identity, §2 a whole-app reconcile-target registry, §4 tier
> classification) but deliberately answered none of them. This doc answers all three, at
> the same level of detail this repo's other `*_decision.md` files use before
> implementation — concrete types and signatures, not yet code.

---

## 0. The three questions, and the one idea that answers all of them

`docs/iris_hot_reload_alignment_decision.md` §1/§2/§4 read as three separate problems.
They resolve to one mechanism plus one small extension of something that already exists:

1. **§1 (state identity):** give `ComponentInstance` a *replay* mode — reusing its
   already-allocated signal storage by declaration order, instead of allocating fresh —
   so re-running a component's render body against the *same* instance preserves
   `@signal`/`IRIS_SIGNAL` values automatically.
2. **§2 (registry):** a small `ReloadTarget` type holds the one thing nothing currently
   owns — an *owning* `unique_ptr<Umbra::IWidget>` plus the exact prior `Component` tree
   — so a reload driver has something to call the existing `ReconcileWidget` against.
3. **§4 (tier classification):** falls out of (1) and (2) with **no new mechanism**.
   Whether a reload was tier 1 (render-body only) or tier 2 (`@signal` layout changed) is
   a direct, structural side effect of replaying against `ComponentInstance` — the replay
   itself can tell you, because it's the thing that discovers a mismatch. Tier 3
   (structurally irreconcilable) isn't decided by any new code at all: `ReconcileWidget`'s
   *existing* "different tag, different/missing key → unmount and mount fresh" rule
   (`Reconciler.h`) already is the tier-3 fallback — it was already correct, just never
   framed that way.

Nothing here needs nyx-proto to report which tier applies, and nothing here needs to
diff `.iris.ir` source text to guess it ahead of time — both options
`iris_hot_reload_alignment_decision.md` §4 floated turn out to be unnecessary. The tier
is discovered by actually doing the replay and the reconcile, not predicted beforehand.

---

## 1. `ComponentInstance` replay: reusing signal storage by declaration order

### The mechanism

`ComponentInstance::AllocateSignal<T>`/`RegisterSignal` (`ComponentInstance.h`) always
append to `Signals_`/`NyxSignals_` today — correct for an ordinary mount, wrong for a
reload, where a matched instance's *existing* storage should be reused rather than reset
to `InitExpr`. This is the same shape as React's hooks rules: identity is declaration
order within one component instance's single execution, not any explicit key.

```cpp
class ComponentInstance {
public:
    enum class ReloadTier { Unchanged, SignalLayoutChanged };

    // Puts this already-mounted instance into replay mode: subsequent AllocateSignal<T>/
    // RegisterSignal calls, made while this instance is ambient (via the same
    // ScopedComponentInstance an ordinary mount already uses), reuse existing storage by
    // declaration order instead of appending fresh entries. Must be paired with
    // EndReloadReplay() once the render body finishes.
    void BeginReloadReplay();

    // Ends a replay pass: discards any trailing signal storage this run's render body
    // didn't re-declare (a removed @signal field -- Nyx's own field-layout-change
    // semantics, execution-model.md §21.4, "removed fields are discarded"), and reports
    // which tier the replay turned out to be.
    ReloadTier EndReloadReplay();

private:
    bool        ReplayActive_{false};
    std::size_t ReplayIndex_{0};
    std::size_t NyxReplayIndex_{0};
    bool        ReplayLayoutChanged_{false};
    // Signals_ / NyxSignals_ as today
};
```

`AllocateSignal<T>` gains a replay branch. `Detail::SignalStorageBase` gains a
`std::type_index TypeId() const` (the manual type tag this codebase's own type-erasure
convention already implies — `SignalStorageBase` exists specifically so heterogeneous
`T`s can share one vector; a `std::type_index` comparison is one more line on the same
idea, not a new dependency on RTTI/`dynamic_cast`, which nothing in this repo uses today):

```cpp
template <typename T>
Signal<T>& AllocateSignal(T InitialValue) {
    if (ReplayActive_) {
        if (ReplayIndex_ < Signals_.size() && Signals_[ReplayIndex_]->TypeId() == typeid(T)) {
            auto& Existing = static_cast<Detail::SignalStorage<T>&>(*Signals_[ReplayIndex_++]);
            return Existing.Value;               // preserves current value; InitialValue ignored
        }
        // Either past the previously-recorded signal count (a new field), or the type at
        // this position changed (e.g. int -> std::string) -- both are the field-layout-
        // changed case. A type mismatch replaces just this one slot; new fields append.
        ReplayLayoutChanged_ = true;
        auto Storage = std::make_unique<Detail::SignalStorage<T>>(std::move(InitialValue));
        Signal<T>& Ref = Storage->Value;
        if (ReplayIndex_ < Signals_.size()) {
            Signals_[ReplayIndex_] = std::move(Storage);
        } else {
            Signals_.push_back(std::move(Storage));
        }
        ++ReplayIndex_;
        return Ref;
    }
    // ordinary mount path, unchanged from today
    ...
}

ComponentInstance::ReloadTier ComponentInstance::EndReloadReplay() {
    if (ReplayIndex_ < Signals_.size()) {
        Signals_.resize(ReplayIndex_);            // trailing entries: removed fields
        ReplayLayoutChanged_ = true;
    }
    // same truncation for NyxSignals_/NyxReplayIndex_
    ReplayActive_ = false;
    bool Changed = std::exchange(ReplayLayoutChanged_, false);
    ReplayIndex_ = NyxReplayIndex_ = 0;
    return Changed ? ReloadTier::SignalLayoutChanged : ReloadTier::Unchanged;
}
```

The Nyx-facing `RegisterSignal(const nyx::runtime::Value&)` gets the same
declaration-order reuse, minus the type-tag check: `nyx::runtime::Value` is already
dynamically typed on the Nyx side, so "did the type at this position change" is a
question nyx-proto's own `Value` comparison is better placed to answer than a blind
reuse here. **Left genuinely open, not designed here:** whether the Nyx-facing
`RegisterSignal` should itself compare `InitialValue`'s tag against the existing stored
`Value`'s tag (mirroring the C++ `typeid` check above) before deciding replay-reuse is
safe, or whether that check belongs entirely on the driver's side before it even calls
`RegisterSignal` during a replay. Small, but a real decision needs making before Nyx-side
`@signal` reload works — the mechanical scaffolding above stands regardless of which way
that gets decided.

### Why identity comes for free

`AllocateSignal<T>` returning a reference into the **same, reused** `SignalStorage<T>`
object (not a fresh one) matters beyond just preserving the value: `Signal<T>::get()`/
`set()` (`Signal.h`) track dependents keyed on `this` — the `Signal<T>` object's own
address. Reusing the storage object means any `<Slot>` that captured `Name` by reference
before the reload keeps a valid reference to the *same* object after it, with its
dependency-tracking identity intact, with zero additional bookkeeping. This wasn't a
design choice made for this doc — it's a consequence of `docs/iris_signal_lifetime_
decision.md`'s existing heap-allocation-not-stack-local scheme already being exactly the
right shape for this.

### The entry point

```cpp
// ComponentInstance.h
template <typename Callable>
Iris::Component ReloadComponentInstance(std::shared_ptr<ComponentInstance> Prior, Callable&& Fn) {
    Prior->BeginReloadReplay();
    Iris::Component Result = [&]() -> Iris::Component {
        Detail::ScopedComponentInstance Guard(Prior.get());
        return Fn();
    }();
    ComponentInstance::ReloadTier Tier = Prior->EndReloadReplay();
    Result.Instance = Prior;                     // same instance carried forward, not a fresh one
    Result.ReloadTier = Tier;                     // new Component field -- see below
    return Result;
}
```

`Iris::Component` (`Component.h`) gains an `std::optional<ComponentInstance::ReloadTier>
ReloadTier` field, set only by `ReloadComponentInstance` — `nullopt` for every ordinary
`MountComponentInstance`/`Mount` call, exactly the existing convention `Component::Key`/
`Instance` already use (present only where relevant, absent otherwise). This is how tier
classification (§4, resolved below) actually reaches whoever drives a reload.

---

## 2. `ReloadTarget`: the owning root + prior tree a reload reconciles against

`docs/iris_interpreted_host_hot_reload_gap.md` §2 named exactly what's missing:
`ReconcileWidget` needs an *owning* `unique_ptr<Umbra::IWidget>&` and the *exact* prior
`Component` tree, and nothing outside a `SlotState` (scoped to one `<Slot>`) currently
retains either at whole-application scope.

```cpp
// ReloadTarget.h (new header -- SlotRuntime.h is already large; this is a distinct,
// opt-in concern, not part of the always-present ambient runtime state)
class ReloadTarget {
public:
    ReloadTarget(std::unique_ptr<Umbra::IWidget> RootWidget, Iris::Component RootTree);

    // The retained previous root tree -- what a reload driver walks (in lockstep against
    // a freshly re-rendered tree) to find which ComponentInstances have a prior match.
    // See §3 below for why that walk itself isn't a function this header provides.
    const Iris::Component& PreviousTree() const;

    // Diffs New against PreviousTree() via the existing Reconciler.h ReconcileWidget,
    // applies the result to the retained widget, then retains New as the new
    // PreviousTree() for next time. New's own ComponentInstances may be freshly
    // allocated (untouched subtrees) or carried forward via ReloadComponentInstance
    // (§1) -- ReconcileWidget doesn't need to know or care which; it only ever diffs
    // Component values, exactly as it already does for an ordinary re-render.
    void Reconcile(Iris::Component New, const MountFn& Mount);

private:
    std::unique_ptr<Umbra::IWidget> RootWidget_;
    Iris::Component                  PreviousTree_;
};
```

### Ownership and lifetime

`IrisRuntime` (`SlotRuntime.h`) gains:

```cpp
void          RegisterReloadTarget(std::unique_ptr<ReloadTarget> Target);
ReloadTarget* GetReloadTarget();
```

Same singleton-scoped, process-lifetime storage as `Root_`/`DirtySlots_` today, but
**opt-in and nullptr by default** — an ordinary Stage 2/3 app that never hot-reloads
never populates this and pays nothing for it. The host application populates it once,
right after its own initial build, the same moment it already has both pieces on hand:

```cpp
Iris::Component Root  = iris::Mount(AppRoot);
auto            Widget = BackendBuildRoot(Root);      // e.g. penumbra-ui-backend's static builder
iris::RegisterReloadTarget(std::make_unique<iris::ReloadTarget>(std::move(Widget), Root));
```

### Relation to `RegisterRoot`/`GetRoot`: sits alongside, does not subsume

`docs/iris_hot_reload_alignment_decision.md` §2 asked this explicitly. Answer: **no**,
for the same reason `iris_interpreted_host_hot_reload_gap.md` §0 already gave —
`RegisterRoot`/`GetRoot` deliberately hand back a non-owning `Umbra::IWidget*` for
Lustre's read-only restyle walk, and that contract ("Iris does not own the widget's
lifetime, only the pointer to it") is correct for that consumer and would break if
repurposed. An app wanting both Lustre restyle *and* reload calls both, over the same
underlying widget: `RegisterRoot(Widget.get())` before `Widget` moves into
`RegisterReloadTarget`, or `RegisterRoot(Target->RootWidgetForRestyle())` if `ReloadTarget`
exposes a non-owning accessor for that — a small addition, not designed further here
since it's a two-line convenience, not a decision.

---

## 3. Tier classification: no new mechanism, and one thing deliberately left external

Combine §1 and §2:

- **Tier 1** (render-body-only): a reload driver calls `ReloadComponentInstance(Prior,
  Fn)` for a matched component; `EndReloadReplay()` returns `ReloadTier::Unchanged`.
- **Tier 2** (`@signal` field layout changed): same call, `EndReloadReplay()` returns
  `ReloadTier::SignalLayoutChanged` — detected structurally, mid-replay, exactly where a
  type or count mismatch actually shows up. No source diffing, no nyx-proto report needed.
- **Tier 3** (structurally irreconcilable): not decided by `ComponentInstance` at all.
  `ReconcileWidget`'s pre-existing "different tag, different/missing key → unmount +
  mount fresh" branch (`Reconciler.h`, already implemented, already tested) *is* the
  tier-3 fallback. `ReloadTarget::Reconcile` reaches it automatically the moment a
  freshly re-rendered root's tag/key doesn't match what's live — zero new code.

One more thing this resolves, not previously stated anywhere: nyx-proto's own tier-1
description ("the method table is patched on all live instances of that type" —
`execution-model.md` §21.4) describes *how nyx-proto itself* gets to a new render output
for a live `NyxObject`, a Nyx-internal concern about bytecode/method dispatch. iris-proto
never sees that distinction and doesn't need to — per `decision-log.md` §7.2's already-
settled boundary (the Chaos runtime owns the walk, nyx-proto stays Chaos-agnostic),
whatever nyx-proto did internally to produce a fresh render, iris-proto always just gets
"here is this render body's current output, replay it" via `WalkIrisIrDocument`/
`ConvertIrElement` (`IrisIrRuntime.h`) — same as today, unconditionally. Whether that
counted as a Nyx-side method-table patch or a full script re-parse-and-rerun is invisible
to, and irrelevant for, everything in this doc.

---

## 4. How these compose: the one thing still left to a driver, on purpose

§1 and §2 give two primitives. Something still has to decide, at every component
invocation encountered while re-rendering from the root, whether there's a matching prior
`Component` (and hence a `ComponentInstance` to replay against) or not. That's a lockstep
walk over `ReloadTarget::PreviousTree()` and the freshly-produced tree, matched the same
way `ReconcileWidget` already matches nodes — same tag, equal key.

**Deliberately not built as a core-owned utility here.** The natural hook point already
exists: `NyxEvaluator::EvaluateComponentInvocation` (`IrisIrRuntime.h`) is exactly the
callback a reload-aware driver's own implementation would consult the previous tree from,
before deciding to call `ReloadComponentInstance` vs. an ordinary fresh mount — the same
"runtime supplies a callback, this code never knows what's on the other side of it"
pattern that file's own top comment already establishes for prop/text/slot evaluation.
Duplicating `ReconcileWidget`'s tag+key matching rule as a second, separately-owned
core function is not justified yet — it would have exactly one caller (the not-yet-built
reload driver), and this project's own stated posture is not to build abstractions ahead
of a second real need. If a second caller for that matching rule shows up, factoring it
out of `ReconcileWidget` into something both call is the natural follow-up; premature
right now.

---

## 5. What this doc still leaves open

- **The reload driver itself.** Still blocked on `NyxEvaluator::EvaluateInScope` (or
  equivalent) not existing in nyx-proto yet (`decision-log.md` §7.2, `docs/next-steps.md`'s
  "Chaos runtime" entry) — nothing in this doc is reachable end to end until that exists.
- **The Nyx-side `RegisterSignal` type-compatibility check** (§1) — real, small, not
  resolved above.
- **Component-invocation lockstep matching** (§3) — left to the driver on purpose, not
  designed as a core utility, per §4's reasoning.
- **A non-owning restyle accessor on `ReloadTarget`** for apps wanting both Lustre restyle
  and reload over the same widget (§2) — a two-line convenience, not sketched.
- **Actual implementation of any of the above** — this is a design pass, matching the
  posture of every other `*_decision.md` in this repo before its own "Landed" follow-up.

## 6. Cross-references

- `docs/iris_hot_reload_alignment_decision.md` — the three questions this doc answers.
- `docs/iris_interpreted_host_hot_reload_gap.md` — the original code-level sizing pass
  for §1/§2; still accurate for what was missing before this doc.
- `docs/iris_signal_lifetime_decision.md` — why `ComponentInstance` heap-allocates signal
  storage at all; §1 above is only possible because of that existing design.
- `docs/iris_lis_list_diff_decision.md` — the key-based matching precedent §3/§4 above
  lean on (`ReconcileWidget`'s tag+key rule already existing and already being correct).
- `docs/iris_nested_slot_discovery_decision.md` — the "rebuild from scratch on every
  reconcile rather than incrementally persist" precedent this doc's `SlotState`/
  `ResolveSlots` reuse (implicit in §2 — a fresh root tree reconciled via `ReconcileWidget`
  naturally rediscovers `<Slot>`s the same way an ordinary re-render already does) follows.
- `fearless-hq/projects/nyx-scripting-language/decision-log.md` §9.1, §7.2 —
  the nyx-proto-side decision this aligns to, and the Chaos-runtime-owns-the-walk
  boundary §3 above relies on.
- `fearless-hq/projects/nyx-scripting-language/execution-model.md` §21.4 — the
  language-level tier semantics (`.nyx` gameplay scripts) this mirrors for `.chaos`/
  `.iris.ir`.
- `docs/next-steps.md`'s "Chaos runtime" entry — the still-missing `NyxEvaluator`
  prerequisite, tracked there, not restated here.
