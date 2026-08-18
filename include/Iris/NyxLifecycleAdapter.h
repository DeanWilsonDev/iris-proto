#pragma once

#include "Umbra/IWidgetLifecycle.h"

#include "runtime/value.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Iris {

// Bridges the interpreted `.irisx` path to `iris::RegisterLifecycle`'s C++-only
// `Umbra::IWidgetLifecycle` contract -- docs/next-steps.md's "A `.irisx` component can't
// register its own per-frame OnTick" entry. `IrisNyxDriver.cpp` is the only constructor of
// this class; it decides, per mounted invocation and per authoring model, whether one of
// three reserved hook names is actually declared and builds the right `HookInvoker` for
// whichever model is in play:
//
// - Model 1 (free-function component): a top-level local lambda in the component body,
//   named to match one of `IWidgetLifecycle`'s own method names --
//   `auto OnTick = (float dt) -> { ... };` -- the same "named local lambda" idiom
//   `functions-and-control-flow.md` §10.2 already documents, called back via
//   `NyxRuntime::EvaluateInScope`'s "reconstruct call-site source text, parse, evaluate"
//   mechanism (`IrisNyxEvaluator.cpp`'s own `InvokeAsLambda` precedent for event-handler
//   props) rather than a new "call this already-bound Value" nyx-proto primitive -- none
//   exists publicly (`IrisNyxEvaluator.h`'s own doc comment on event-handler props: "nyx-proto
//   exposes no public 'call this already-evaluated Value' primitive").
// - Model 2 (class-based component): an instance method with a matching name --
//   `void OnTick(float dt) { ... }` on a `class X : Component` -- called back via
//   `Interpreter::TryCallInstanceMethod`, which already reports "no override" as
//   `std::nullopt` rather than an error, exactly the "hook not declared" case this adapter
//   needs to treat as a silent no-op.
//
// Neither model needed a new nyx-proto primitive: `Environment::FindOwn`, `NyxRuntime::
// EvaluateInScope`, and `Interpreter::TryCallInstanceMethod` are all already public --
// verified against nyx-proto's real source (`src/runtime/environment.hpp`, `src/host/
// nyx-runtime.hpp`, `src/interpreter/interpreter.hpp`), not assumed. A genuinely
// interpreter-level primitive (a `@lifecycle`-style decorator on a method/function
// declaration) was considered and rejected for this first cut: `runtime/decorator-
// descriptor.hpp`'s own doc comment confirms `DecoratorTarget::Method`/`Function` exist in
// the enum but have no dispatch wired ("MethodDecl has no decorator field at all, and
// FunctionDecl/ClassDecl's decorators vectors exist but nothing populates them yet") -- a
// real, separate nyx-proto gap, not needed for the reserved-name design implemented here.
//
// Kept alive exactly as long as the `iris::ComponentInstance` it's registered against --
// held inside that instance's own `DriverState` (`NyxDriverState::Lifecycle`,
// `IrisNyxDriver.h`), the same "opaque extension slot the backend never outlives" pattern
// `DriverState` itself already establishes (`ComponentInstance.h`'s own `Lifecycle` field
// doc comment names this as the expected ownership shape: "typically by holding the
// concrete IWidgetLifecycle inside state this same ComponentInstance already keeps alive").
class NyxLifecycleAdapter : public Umbra::IWidgetLifecycle {
public:
    // `Invoker("OnTick", {Value(dt)})` returns `std::nullopt` if the named hook wasn't
    // declared (Model 2's own `TryCallInstanceMethod` contract) -- irrelevant here since
    // `HasOnMount_`/`HasOnUnmount_`/`HasOnTick_` are already computed once at construction
    // (by whichever model built this adapter) so `OnMount`/`OnUnmount`/`OnTick` below skip
    // calling `Invoker` entirely for a hook that was never present, rather than
    // re-discovering absence on every call -- relevant for `OnTick` specifically, since a
    // real frame loop calls it every frame.
    using HookInvoker = std::function<std::optional<nyx::runtime::Value>(const std::string&,
                                                                          std::vector<nyx::runtime::Value>)>;

    NyxLifecycleAdapter(HookInvoker Invoker, bool HasOnMount, bool HasOnUnmount, bool HasOnTick)
        : Invoker_(std::move(Invoker)), HasOnMount_(HasOnMount), HasOnUnmount_(HasOnUnmount), HasOnTick_(HasOnTick) {}

    void OnMount() override {
        if (HasOnMount_) {
            Invoker_("OnMount", {});
        }
    }

    void OnUnmount() override {
        if (HasOnUnmount_) {
            Invoker_("OnUnmount", {});
        }
    }

    void OnTick(const Umbra::TickInfo& Info) override {
        if (HasOnTick_) {
            Invoker_("OnTick", {nyx::runtime::Value(Info.DeltaSeconds)});
        }
    }

private:
    HookInvoker Invoker_;
    bool        HasOnMount_;
    bool        HasOnUnmount_;
    bool        HasOnTick_;
};

} // namespace Iris
