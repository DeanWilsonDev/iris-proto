#pragma once

#include "Iris/ComponentReloadTier.h"
#include "Iris/Signal.h"
#include "Iris/SlotRuntime.h"

#include "Umbra/IWidgetLifecycle.h"

#include "runtime/value.hpp"

#include <cassert>
#include <cstddef>
#include <memory>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

namespace iris {

// Opaque handle to one `nyx::runtime::Value`-typed signal registered via
// `ComponentInstance::RegisterSignal` — the Nyx `@signal` decorator's
// counterpart to a C++ `IRIS_SIGNAL`'s `Signal<T>&` reference
// (nyx-proto's docs/nyx-scripting-language/decision-log.md §6.7). Just an
// index into `ComponentInstance::NyxSignals_`; meaningless outside the
// `ComponentInstance` that produced it.
using SignalId = std::size_t;

namespace Detail {

// Type-erased base so ComponentInstance can hold a heterogeneous collection of
// Signal<T> instances (one per IRIS_SIGNAL declaration in a component's body) without
// needing to know every T. `TypeId()` is a manual type tag on top of that same
// type-erasure idea — used only during a reload replay (docs/
// iris_hot_reload_reconciliation_decision.md §1) to check "is the signal previously
// declared at this position still the same T" before reusing its storage. Deliberately
// not `dynamic_cast`/RTTI: this codebase has never depended on RTTI, and a manual tag
// fits the existing base+derived type-erasure shape here better than reaching for it.
class SignalStorageBase {
public:
    virtual ~SignalStorageBase() = default;
    virtual std::type_index TypeId() const = 0;
};

template <typename T>
class SignalStorage : public SignalStorageBase {
public:
    explicit SignalStorage(T InitialValue) : Value(std::move(InitialValue)) {}
    std::type_index TypeId() const override { return std::type_index(typeid(T)); }
    Signal<T> Value;
};

} // namespace Detail

// docs/iris_signal_lifetime_decision.md: owns every `iris::Signal<T>` declared (via
// `IRIS_SIGNAL`) during one component function's single run, heap-allocating each on
// this object rather than letting it live as a stack local in the declaring function's
// frame — which is what made `[&]`-captured signals dangling-reference UB once that
// function returned (confirmed with AddressSanitizer against real generated `.iris`
// output). `AllocateSignal` returns a genuine `Signal<T>&` bound to that heap storage,
// so `[&]` capture throughout the rest of the component (event handlers, `<Slot>`
// callables) works exactly as every spec example already writes it — no change to
// capture semantics, no coroutines.
//
// Instances are never explicitly "freed on unmount" — there's no separate hook for
// that anywhere. `Component::Instance` (a `shared_ptr<ComponentInstance>`, set by
// `MountComponentInstance` below) is what keeps one alive, via perfectly ordinary
// `shared_ptr` refcounting through wherever that `Component` value is retained —
// chiefly `SlotState::PreviousSingle_`/`PreviousList_`, which already keep the
// last-rendered tree around for diffing. When a component leaves the tree (a
// different tag wins at its position, or its parent scope is torn down), the
// `Component` value holding its `Instance` stops being retained and this object —
// and every `Signal` it owns — is destroyed automatically, exactly when appropriate.
class ComponentInstance {
public:
    template <typename T>
    Signal<T>& AllocateSignal(T InitialValue) {
        if (ReplayActive_) {
            // Reload replay (docs/iris_hot_reload_reconciliation_decision.md §1):
            // declaration order is this signal's identity across two separate runs of
            // the same instance, the same rule React's own hooks use. A match at this
            // position, of the same T, reuses the existing storage untouched --
            // InitialValue is discarded, exactly like tier 1's "state preserved"
            // requirement. Anything else (past the previously-recorded count, or a type
            // change at this position) is the tier-2 case: replace/append fresh storage
            // and flag that this replay's layout changed.
            if (ReplayIndex_ < Signals_.size() &&
                Signals_[ReplayIndex_]->TypeId() == std::type_index(typeid(T))) {
                auto& Existing = static_cast<Detail::SignalStorage<T>&>(*Signals_[ReplayIndex_]);
                ++ReplayIndex_;
                return Existing.Value;
            }
            ReplayLayoutChanged_ = true;
            auto        Storage = std::make_unique<Detail::SignalStorage<T>>(std::move(InitialValue));
            Signal<T>& Ref = Storage->Value;
            if (ReplayIndex_ < Signals_.size()) {
                Signals_[ReplayIndex_] = std::move(Storage); // type-changed field: replace this slot
            } else {
                Signals_.push_back(std::move(Storage)); // new field: append
            }
            ++ReplayIndex_;
            return Ref;
        }
        auto Storage = std::make_unique<Detail::SignalStorage<T>>(std::move(InitialValue));
        Signal<T>& Ref = Storage->Value;
        Signals_.push_back(std::move(Storage));
        return Ref;
    }

    // The `@signal` Nyx decorator's counterpart to `AllocateSignal<T>` above —
    // type-erased on the `nyx::runtime::Value` side rather than templated,
    // since the Nyx interpreter only ever works in `Value`, never a concrete
    // C++ `T` (nyx-proto's docs/nyx-scripting-language/decision-log.md §6.7).
    // Storage is heap-stable (one `unique_ptr<Value>` per signal, same
    // reasoning as `Signals_` above) so the `SignalId` returned here, and the
    // `const void*` identity `GetSignal`/`SetSignal` hand to
    // `TrackSignalDependency`/`NotifySignalDependents`, stay valid for this
    // instance's whole lifetime regardless of how many further signals get
    // registered afterward and reallocate `NyxSignals_` itself.
    SignalId RegisterSignal(const nyx::runtime::Value& InitialValue) {
        if (ReplayActive_) {
            // Same declaration-order reuse as AllocateSignal<T> above, minus the type-
            // tag check: nyx::runtime::Value is already dynamically typed on the Nyx
            // side, so whether the type at this position is compatible with what's
            // stored is a question nyx-proto's own Value comparison is better placed to
            // answer than a blind reuse here (docs/iris_hot_reload_reconciliation_
            // decision.md §1 -- left open, not resolved by this scaffolding).
            if (NyxReplayIndex_ < NyxSignals_.size()) {
                return NyxReplayIndex_++;
            }
            ReplayLayoutChanged_ = true;
            NyxSignals_.push_back(std::make_unique<nyx::runtime::Value>(InitialValue));
            ++NyxReplayIndex_;
            return NyxSignals_.size() - 1;
        }
        NyxSignals_.push_back(std::make_unique<nyx::runtime::Value>(InitialValue));
        return NyxSignals_.size() - 1;
    }

    // The Nyx-side counterpart to `Signal<T>::get()`: registers whichever
    // `<Slot>` is currently being (re-)invoked as a dependent of this signal
    // (same ambient active-slot tracking, `TrackSignalDependency` in
    // `SlotRuntime.h`), then returns the current value.
    const nyx::runtime::Value& GetSignal(SignalId Id) const {
        nyx::runtime::Value* Storage = NyxSignals_.at(Id).get();
        TrackSignalDependency(Storage);
        return *Storage;
    }

    // The Nyx-side counterpart to `Signal<T>::set()`: updates storage, then
    // marks every dependent `<Slot>` dirty (`NotifySignalDependents`) — never
    // reconciles synchronously, exactly like `Signal<T>::set()` (reconciling
    // only ever happens inside `iris::Tick()`). Installed as the `onWrite`
    // callback the `@signal` decorator attaches via `NyxVariable::OnWrite`,
    // so this fires on every subsequent Nyx-script assignment to the
    // decorated variable.
    void SetSignal(SignalId Id, const nyx::runtime::Value& NewValue) {
        nyx::runtime::Value* Storage = NyxSignals_.at(Id).get();
        *Storage = NewValue;
        NotifySignalDependents(Storage);
    }

    // Puts this already-mounted instance into replay mode (docs/
    // iris_hot_reload_reconciliation_decision.md §1): the next run of AllocateSignal<T>/
    // RegisterSignal calls made while this instance is ambient reuse existing signal
    // storage by declaration order instead of appending fresh entries. Must be paired
    // with a later EndReloadReplay() call once the render body finishes -- see
    // iris::ReloadComponentInstance below, the only intended caller.
    void BeginReloadReplay() {
        ReplayActive_ = true;
        ReplayIndex_ = 0;
        NyxReplayIndex_ = 0;
        ReplayLayoutChanged_ = false;
    }

    // Ends a replay pass: discards any trailing signal storage this run's render body
    // didn't re-declare (a removed @signal/IRIS_SIGNAL -- Nyx's own field-layout-change
    // semantics, execution-model.md §21.4, "removed fields are discarded"), and reports
    // which tier the just-finished replay turned out to be.
    ComponentReloadTier EndReloadReplay() {
        if (ReplayIndex_ < Signals_.size()) {
            Signals_.resize(ReplayIndex_);
            ReplayLayoutChanged_ = true;
        }
        if (NyxReplayIndex_ < NyxSignals_.size()) {
            NyxSignals_.resize(NyxReplayIndex_);
            ReplayLayoutChanged_ = true;
        }
        ReplayActive_ = false;
        const bool Changed = ReplayLayoutChanged_;
        ReplayLayoutChanged_ = false;
        return Changed ? ComponentReloadTier::SignalLayoutChanged : ComponentReloadTier::Unchanged;
    }

    // Opaque per-invocation state an interpreted-Nyx driver (`IrisNyxDriver.h`) needs kept
    // alive for exactly as long as this instance is -- concretely, the `nyx::host::
    // NyxRuntime::NyxScope` `InvokeComponent` captured for this one call, which every
    // `NyxEvaluator` closure this instance's render produced (including a `<Slot>`
    // callable that may be re-invoked long after the mounting call returns, via
    // `iris::Tick()`) holds a raw reference into. `ComponentInstance` doesn't know or care
    // what's actually stored here -- `shared_ptr<void>` rather than a concrete
    // `host/nyx-runtime.hpp` type so this header (already included by the compiled `.iris`
    // codegen path, which has nothing to do with Nyx interpretation) doesn't gain a
    // dependency on it. Untouched (`nullptr`) for every ordinary compiled-`.iris`
    // `IRIS_SIGNAL`-only component.
    std::shared_ptr<void> DriverState;

    // The producer side of `docs/iris_stage3_decision_doc.md` §8's `IWidgetLifecycle`
    // design (`docs/next-steps.md`'s "`iris::RegisterLifecycle`" entry) -- set via the
    // free function `iris::RegisterLifecycle` below, read by a backend's own reconciler
    // (e.g. `penumbra-ui-backend`) to actually register/unregister against a real frame
    // loop's lifecycle registry (e.g. Penumbra's `Application::RegisterLifecycle`/
    // `UnregisterLifecycle`) at mount/unmount time. Passive and non-owning -- the same
    // "opaque extension slot the backend reads and manages" shape `DriverState` above
    // already establishes, deliberately not a `ComponentInstance`-owned registry
    // reference with self-unregister-at-destruction: `ComponentInstance` has no custom
    // destructor anywhere today (see this class's own doc comment above, "no separate
    // on-unmount hook needed anywhere"), and adding one just to call a registry would
    // introduce new, invasive machinery with a real registry/component-lifetime-ordering
    // hazard that doesn't exist today. Whoever calls `iris::RegisterLifecycle` retains
    // ownership of the pointee and must keep it alive for at least as long as this
    // instance stays mounted -- typically by holding the concrete `IWidgetLifecycle`
    // inside state this same `ComponentInstance` already keeps alive (a `Signal<T>`, or
    // `DriverState`), not as a bare stack local. `nullptr` for every component that never
    // calls `iris::RegisterLifecycle`.
    Umbra::IWidgetLifecycle* Lifecycle{nullptr};

private:
    std::vector<std::unique_ptr<Detail::SignalStorageBase>> Signals_;
    std::vector<std::unique_ptr<nyx::runtime::Value>>       NyxSignals_;

    bool        ReplayActive_{false};
    std::size_t ReplayIndex_{0};
    std::size_t NyxReplayIndex_{0};
    bool        ReplayLayoutChanged_{false};
};

namespace Detail {

// What the `IRIS_SIGNAL` macro expands to: allocates against whichever
// `ComponentInstance` is currently ambient (`IrisRuntime::CurrentComponentInstance()`),
// set up by `MountComponentInstance`/`Mount` below. Asserts if called outside one —
// every `IRIS_SIGNAL` declaration must live inside a component function reached via
// generated `<Name .../>` codegen or `iris::Mount()`, never called directly by hand.
template <typename T>
Signal<T>& DeclareSignal(T InitialValue) {
    ComponentInstance* Instance = IrisRuntime::Instance().CurrentComponentInstance();
    assert(Instance != nullptr &&
           "IRIS_SIGNAL used outside any component invocation wrapped with "
           "MountComponentInstance -- every component function must be reached via "
           "generated <Name .../> codegen or iris::Mount() for signal lifetime to be "
           "tracked correctly (docs/iris_signal_lifetime_decision.md).");
    return Instance->AllocateSignal<T>(std::move(InitialValue));
}

// RAII: makes Instance the ambient "current component instance" for its lifetime.
class ScopedComponentInstance {
public:
    explicit ScopedComponentInstance(ComponentInstance* Instance) {
        IrisRuntime::Instance().PushComponentInstance(Instance);
    }
    ~ScopedComponentInstance() { IrisRuntime::Instance().PopComponentInstance(); }

    ScopedComponentInstance(const ScopedComponentInstance&) = delete;
    ScopedComponentInstance& operator=(const ScopedComponentInstance&) = delete;
};

} // namespace Detail

// Registers `Lifecycle` against whichever `ComponentInstance` is currently ambient
// (`IrisRuntime::CurrentComponentInstance()`) — the same ambient-current-instance
// pattern `IRIS_SIGNAL`'s `Detail::DeclareSignal` above uses, and the one-argument
// shape `docs/iris_stage3_decision_doc.md` §8's own worked example calls this with
// (`iris::RegisterLifecycle(new class : public Umbra::IWidgetLifecycle { ... })`) —
// not a two-argument `RegisterLifecycle(ComponentInstance&, ...)` form. Stashes the
// pointer on `ComponentInstance::Lifecycle` above and returns; does not itself talk to
// any backend's real lifecycle registry (Iris stays backend-agnostic per this repo's
// own design) — a consuming backend's reconciler is expected to read that field back
// and drive the actual registration. Asserts if called outside any component
// invocation, the same precondition `IRIS_SIGNAL` already has.
inline void RegisterLifecycle(Umbra::IWidgetLifecycle* Lifecycle) {
    ComponentInstance* Instance = IrisRuntime::Instance().CurrentComponentInstance();
    assert(Instance != nullptr &&
           "iris::RegisterLifecycle used outside any component invocation wrapped with "
           "MountComponentInstance -- every component function must be reached via "
           "generated <Name .../> codegen or iris::Mount() for the ambient instance to "
           "exist (mirrors IRIS_SIGNAL's own precondition, docs/iris_signal_lifetime_"
           "decision.md).");
    Instance->Lifecycle = Lifecycle;
}

// Wraps a component invocation — what `Codegen.h` emits for every `<Name .../>` — so
// any `IRIS_SIGNAL` declarations inside the component function `Fn` calls register
// against a fresh `ComponentInstance`, then ties that instance's lifetime to the
// returned `Iris::Component` (`Component::Instance`) rather than to `Fn`'s own
// stack frame. `Fn` must return `Iris::Component` — the same contract every
// component function already has.
template <typename Callable>
Iris::Component MountComponentInstance(Callable&& Fn) {
    auto Instance = std::make_shared<ComponentInstance>();
    Iris::Component Result = [&]() -> Iris::Component {
        Detail::ScopedComponentInstance Guard(Instance.get());
        return Fn();
    }();
    Result.Instance = std::move(Instance);
    return Result;
}

// The entry point for the one component invocation `Codegen.h` never generates: the
// root of the whole tree, called directly by the host application. Same mechanism as
// `MountComponentInstance` above — a distinct name only so an app's own `main()` has an
// obvious, documented thing to call rather than reaching for internal machinery.
template <typename Callable>
Iris::Component Mount(Callable&& RootComponentFn) {
    return MountComponentInstance(std::forward<Callable>(RootComponentFn));
}

// The reload counterpart to `MountComponentInstance` (docs/
// iris_hot_reload_reconciliation_decision.md §1): re-runs `Fn` against `Prior` — an
// already-mounted instance from a previous run, matched by whatever identity a reload
// driver uses (position/key, external to this function) — instead of allocating a fresh
// `ComponentInstance`. Every `IRIS_SIGNAL`/`RegisterSignal` call `Fn` makes reuses
// `Prior`'s existing signal storage by declaration order (`BeginReloadReplay`'s doc
// comment), so a matched signal's value, and its `Signal<T>`/`Value` object identity
// (dependency tracking is keyed on that object's own address — `Signal.h`), survive the
// reload untouched. `Result.ReloadTier` reports whether every signal matched (tier 1) or
// at least one was added, removed, or changed type (tier 2) — see `Component.h`'s own
// `ReloadTier` field doc comment.
template <typename Callable>
Iris::Component ReloadComponentInstance(std::shared_ptr<ComponentInstance> Prior, Callable&& Fn) {
    assert(Prior != nullptr &&
           "ReloadComponentInstance requires a prior ComponentInstance to replay against -- "
           "use MountComponentInstance for a subtree with no prior match.");
    Prior->BeginReloadReplay();
    Iris::Component Result = [&]() -> Iris::Component {
        Detail::ScopedComponentInstance Guard(Prior.get());
        return Fn();
    }();
    const ComponentReloadTier Tier = Prior->EndReloadReplay();
    Result.Instance = Prior;
    Result.ReloadTier = Tier;
    return Result;
}

} // namespace iris

// A host-language local that behaves like `iris::Signal<T> Name = InitExpr;` used to
// (before docs/iris_signal_lifetime_decision.md was implemented — see there for why
// that literal spelling is unsafe) — but expands to a genuine reference bound to
// heap-owned storage via `iris::Detail::DeclareSignal`, so `[&]`-capturing `Name`
// anywhere (an event handler, a `<Slot>` callable) is safe for the component's whole
// lifetime. The one visible syntax change component authors need to make: this macro
// in place of a direct `iris::Signal<T>` declaration.
#define IRIS_SIGNAL(Type, Name, InitExpr) auto& Name = ::iris::Detail::DeclareSignal<Type>(InitExpr)
