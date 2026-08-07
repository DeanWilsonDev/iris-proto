#pragma once

#include "host/nyx-bridge.hpp"

namespace Iris {

// The C++ marker type Model 2 (class-based) `.irisx` components extend on Nyx's own side --
// `class Tooltip : Component { ... }` (nyx-scripting-language/decision-log.md §9.2). Carries no
// state and overrides no virtual method for a Nyx override to hook into: unlike every other
// `RegisterInheritableType` consumer in this ecosystem (`Engine::Application`, per
// decision-log.md §6.4), nothing in this repo ever *drives* a component instance through a C++
// vtable call the way a real engine frame loop drives an `Application` bridge -- iris-proto
// itself calls `Instantiate`/reads fields and calls `Render` directly
// (`IrisNyxDriver::InvokeClassComponent`), never through this bridge's own methods at all. It
// exists purely so `class X : Component` has a real, registered base type to extend --
// `Interpreter::InstantiateClass` requires the base of any class chain to resolve to either a
// declared Nyx class or a `RegisterInheritableType`-registered C++ type, and `Component` is
// neither declared in Nyx nor meant to be (`chaos-ui-authoring.md`'s own Model 2 examples never
// give it Nyx-declared fields/methods of its own -- it's a pure marker, like an empty interface).
class NyxComponentBase {};

} // namespace Iris

namespace nyx::host {

// No `.Override(...)` calls needed for `RegisterInheritableType<NyxComponentBase>` either --
// see `NyxComponentBase`'s own doc comment above for why this bridge is never reached through a
// C++ vtable call.
template <>
class NyxBridge<Iris::NyxComponentBase> : public Iris::NyxComponentBase, public NyxBridgeBase {};

} // namespace nyx::host
