#include "Iris/NyxSignalDecorator.h"

#include "Iris/ComponentInstance.h"
#include "Iris/SlotRuntime.h"

namespace iris {

void RegisterSignalDecorator(nyx::host::NyxRuntime& Runtime) {
    Runtime.RegisterDecorator("signal")
        .ValidTarget(nyx::runtime::DecoratorTarget::Variable)
        .OnApply([](nyx::runtime::NyxVariable& Variable) {
            ComponentInstance* Instance = IrisRuntime::Instance().CurrentComponentInstance();
            // `@signal` used outside a mounted component (no ambient ComponentInstance) is
            // a no-op, not an error -- the variable just behaves like an ordinary
            // undecorated local from here on.
            if (Instance == nullptr) {
                return;
            }

            SignalId Id = Instance->RegisterSignal(Variable.value);
            Variable.OnWrite([Instance, Id](const nyx::runtime::Value& NewValue) { Instance->SetSignal(Id, NewValue); });
            // Read-side half of the same contract (docs/next-steps.md's "@signal never
            // reconciles on write" gap, nyx-scripting-language/decision-log.md §6.8): every
            // subsequent script-level read of this variable now runs GetSignal's own
            // TrackSignalDependency side effect, registering whichever <Slot> is currently
            // being (re-)invoked as a dependent. The value GetSignal returns is discarded --
            // Environment::Get() already has and returns the real stored value; this callback
            // exists purely for the side effect.
            Variable.OnRead([Instance, Id](const nyx::runtime::Value&) { Instance->GetSignal(Id); });
        });
}

} // namespace iris
