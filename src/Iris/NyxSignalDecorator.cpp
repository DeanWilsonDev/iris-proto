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
        });
}

} // namespace iris
