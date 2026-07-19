#pragma once

#include "Iris/SlotRuntime.h"

#include <utility>

namespace iris {

// docs/iris_core_spec.md §2.2: a host-language local wrapped in this runtime-library
// type. Declared once, inside a component function that runs exactly once at mount —
// every `iris::Signal<T>` local is therefore a genuinely persistent value for the
// component's whole lifetime (§2.2). Captured by reference into `<Slot>` lambdas is how
// a component wires state to the one thing that ever re-runs.
//
// `.get()` registers whichever `<Slot>` is currently being (re-)invoked as a dependent,
// via ambient "active slot" tracking (`TrackSignalDependency`, `SlotRuntime.h`) — there
// is no compiler-assisted or explicit subscription API; reading a signal while a slot's
// callable runs *is* the entire subscription mechanism
// (docs/iris_stage3_implementation_decision.md — the Stage 3 decision doc asserts this
// happens "automatically" via `[&]` capture but never specifies how; this is the
// mechanism chosen to make that true).
//
// `.set()` marks every dependent slot dirty; reconciliation itself only happens later,
// inside `iris::Tick()` (docs/iris_stage3_decision_doc.md §6, §7) — never synchronously
// inside `set()`. There is deliberately no equality check skipping a same-value `set()`:
// requiring `T` to support `operator==` would narrow what `iris::Signal<T>` can hold
// beyond what the spec's own examples need (plain `bool`/`int` locals), and "no
// defaults, no opinions" is this ecosystem's established posture elsewhere
// (docs/iris_core_spec.md's styling section) — a component author who wants that
// optimization can add the check themselves before calling `set()`.
template <typename T>
class Signal {
public:
    explicit Signal(T InitialValue) : Value_(std::move(InitialValue)) {}

    const T& get() const {
        TrackSignalDependency(this);
        return Value_;
    }

    void set(T NewValue) {
        Value_ = std::move(NewValue);
        NotifySignalDependents(this);
    }

private:
    T Value_;
};

} // namespace iris
