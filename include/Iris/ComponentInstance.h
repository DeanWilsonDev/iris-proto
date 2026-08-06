#pragma once

#include "Iris/Signal.h"
#include "Iris/SlotRuntime.h"

#include "runtime/value.hpp"

#include <cassert>
#include <cstddef>
#include <memory>
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
// needing to know every T.
class SignalStorageBase {
public:
    virtual ~SignalStorageBase() = default;
};

template <typename T>
class SignalStorage : public SignalStorageBase {
public:
    explicit SignalStorage(T InitialValue) : Value(std::move(InitialValue)) {}
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

private:
    std::vector<std::unique_ptr<Detail::SignalStorageBase>> Signals_;
    std::vector<std::unique_ptr<nyx::runtime::Value>>       NyxSignals_;
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

} // namespace iris

// A host-language local that behaves like `iris::Signal<T> Name = InitExpr;` used to
// (before docs/iris_signal_lifetime_decision.md was implemented — see there for why
// that literal spelling is unsafe) — but expands to a genuine reference bound to
// heap-owned storage via `iris::Detail::DeclareSignal`, so `[&]`-capturing `Name`
// anywhere (an event handler, a `<Slot>` callable) is safe for the component's whole
// lifetime. The one visible syntax change component authors need to make: this macro
// in place of a direct `iris::Signal<T>` declaration.
#define IRIS_SIGNAL(Type, Name, InitExpr) auto& Name = ::iris::Detail::DeclareSignal<Type>(InitExpr)
