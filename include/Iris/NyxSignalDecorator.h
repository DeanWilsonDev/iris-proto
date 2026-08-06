#pragma once

#include "host/nyx-runtime.hpp"

namespace iris {

// Registers the `@signal` Nyx decorator (`decorators.md §19.5`) against `Runtime` —
// the Nyx-script counterpart to a C++ `IRIS_SIGNAL(Type, Name, InitExpr)` declaration
// (nyx-proto's docs/nyx-scripting-language/decision-log.md §6.7). Call once per
// `nyx::host::NyxRuntime` instance, before running any script that uses `@signal`,
// the same way a consumer registers any other host type/decorator.
//
// A `@signal`-decorated local variable's `OnApply` fires once, at declaration time:
// it registers signal storage on whichever `iris::ComponentInstance` is currently
// ambient (`IrisRuntime::CurrentComponentInstance()` — set up by
// `iris::MountComponentInstance`/`iris::Mount`, exactly like `IRIS_SIGNAL`'s own
// `DeclareSignal` requires), then installs an `onWrite` observer
// (`nyx::runtime::NyxVariable::OnWrite`) so every later Nyx-script assignment to that
// variable calls `ComponentInstance::SetSignal`, marking dependent `<Slot>` callables
// dirty. `@signal` used outside any mounted component (no ambient `ComponentInstance`)
// is a silent no-op — the variable behaves like an ordinary undecorated local, never a
// crash — since a bare script invocation (e.g. this repo's own tests, or Nyx code that
// never runs inside a `render {}`-produced component) has nothing to attach signal
// storage to.
void RegisterSignalDecorator(nyx::host::NyxRuntime& Runtime);

} // namespace iris
