#pragma once

#include "Iris/IrisElementTag.h"
#include "Iris/IrisProps.h"

#include "Umbra/IWidget.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <variant>
#include <vector>

namespace iris {
class ComponentInstance;
} // namespace iris

namespace Iris {

struct IrisSlotCallable;
struct IrisNativeBuilder;

// The backend-agnostic IR node `render { }` blocks construct (docs/iris_core_spec.md
// §2.5). Codegen (Codegen.h) emits C++23 expressions that build values of this type;
// no backend type (Penumbra or otherwise) appears anywhere in this repo.
//
// `SlotCallable` is set only when `Tag == IrisElementTag::Slot` — see
// docs/iris_stage1_codegen_decision.md's Gap 1 for why it can't be a `Props` entry or a
// `Children` entry, and why it's held behind a pointer rather than embedded directly.
//
// Every conditional-rendering example in docs/iris_core_spec.md (§1.5, §9) writes
// `return nullptr;` inside a `<Slot>` callable declared to return `Component` — the
// `nullptr_t` converting constructor below exists so that compiles, producing an
// `IrisElementTag::None`-tagged sentinel rather than failing to compile (the gap tracked
// and closed in docs/iris_core_spec.md §8, docs/iris_escape_hatch_decision.md's
// Verification section). Declaring any constructor loses aggregate-ness, so the ordinary
// 4-field constructor below exists to keep `Component{Tag, Props, Children,
// SlotCallable}` — Codegen.h's emitted call shape — compiling unchanged.
//
// `Key` (docs/iris_core_spec.md §2.3/§2.4, docs/iris_stage3_implementation_decision.md)
// is deliberately not a constructor parameter — it's set, when present, by a small
// lambda wrapper `Codegen.h` emits around whichever expression built the rest of this
// value (a primitive's aggregate-style construction or a component invocation's
// function call), so every element kind picks up key-setting uniformly without each
// `Emit*` function needing its own key-threading logic. Every constructor here leaves
// it default-initialized to `std::nullopt` — no key — same as any other data member not
// named in an initializer list.
//
// `Ref` (docs/archive/iris_next_steps_resolved.md, "Named-child-handle (`ref`) prop") is set
// the same way `Key` is -- by a small IIFE wrapper `Codegen.h` emits around the base
// construction expression, so it
// picks up ref-setting uniformly across every element kind too. Unlike `Key`, it carries no
// reconciler meaning at all (see `Reconciler.cpp`'s `Key`-only identity matching): it exists
// purely so a mount call can hand host code back a live-widget lookup keyed by this string,
// once a backend's own tree walk (out of scope in this repo) collects `Ref`-tagged nodes.
//
// `NativeBuilder` (docs/archive/iris_next_steps_resolved.md, "No way to declare a custom
// widget/imperative-draw node as an Iris element") is set only for `Tag ==
// IrisElementTag::Native` -- the same
// "can't be a Props entry, can't be embedded directly" reasoning as `SlotCallable` above
// (its callable's return type, an already-built `Umbra::IWidget`, has no room in the closed
// `IrisPropValue` variant, and deliberately shouldn't: routing it through `IrisProps` would
// make the reconciler try to content-diff it every re-render, defeating the whole "mount
// once, opaque to Iris" point of this primitive). Unlike `SlotCallable`, its callable
// produces a live widget handle directly rather than more `Component` IR -- the one place
// in this file `Umbra::IWidget` (the same backend-agnostic vocabulary type the reconciler
// and `SlotRuntime` already depend on) appears in the IR shape itself, not just the runtime
// layer built on top of it.
//
// `Instance` (docs/iris_signal_lifetime_decision.md) is set only for a component
// invocation's result — `Codegen.h` wraps every `<Name .../>` call it emits in
// `iris::MountComponentInstance(...)`, which allocates a fresh `iris::ComponentInstance`,
// runs the invocation with it as the ambient "current instance" (so any `IRIS_SIGNAL`
// declaration inside that component function's body allocates against it, on the heap,
// rather than as a stack local), and stashes it here. Ordinary `shared_ptr` refcounting
// through wherever this `Component` value is retained (chiefly `SlotState`'s
// `PreviousSingle_`/`PreviousList_`, which already keep the last-rendered tree around
// for diffing) is what keeps a component's signals alive for exactly as long as it
// stays in the tree, and frees them the moment nothing retains it any longer — no
// separate "on unmount" hook needed anywhere else.
struct Component {
    IrisElementTag                    Tag{IrisElementTag::Frame};
    IrisProps                         Props;
    std::vector<Component>        Children;
    std::shared_ptr<IrisSlotCallable> SlotCallable;
    std::shared_ptr<IrisNativeBuilder> NativeBuilder;
    std::optional<IrisPropValue>      Key;
    std::optional<IrisPropValue>      Ref;
    std::shared_ptr<iris::ComponentInstance> Instance;

    Component() = default;
    Component(IrisElementTag Tag, IrisProps Props, std::vector<Component> Children,
                  std::shared_ptr<IrisSlotCallable> SlotCallable,
                  std::shared_ptr<IrisNativeBuilder> NativeBuilder = nullptr)
        : Tag(Tag), Props(std::move(Props)), Children(std::move(Children)),
          SlotCallable(std::move(SlotCallable)), NativeBuilder(std::move(NativeBuilder)) {}
    Component(std::nullptr_t) noexcept : Tag(IrisElementTag::None) {}
};

// Defined only once `Component` above is a complete type — unlike `std::vector`,
// `std::function` has no standard guarantee of working with an incomplete return type,
// so this can't be a direct member of `Component` (docs/iris_stage1_codegen_decision.md).
struct IrisSlotCallable {
    std::variant<std::function<Component()>, std::function<std::vector<Component>()>> Callable;
};

// Wraps a `<Slot>` child lambda for storage on `Component::SlotCallable`. The
// preprocessor never inspects escape hatch contents (docs/iris_core_spec.md §1.4), so it
// can't know whether the lambda it emitted a call to returns `Component` or
// `std::vector<Component>` — that choice is made here, by the host compiler, via
// `if constexpr` on the callable's actual return type. A lambda returning neither fails
// to compile inside this function, which is a host-compiler error (§6 tier 2), not a
// preprocessor-level one.
template <typename Callable>
std::shared_ptr<IrisSlotCallable> MakeSlotCallable(Callable&& Fn) {
    using Result = std::invoke_result_t<Callable>;
    static_assert(std::is_same_v<Result, Component> || std::is_same_v<Result, std::vector<Component>>,
                  "<Slot> callable must return Component or std::vector<Component>");

    if constexpr (std::is_same_v<Result, Component>) {
        return std::make_shared<IrisSlotCallable>(
            IrisSlotCallable{std::function<Component()>(std::forward<Callable>(Fn))});
    } else {
        return std::make_shared<IrisSlotCallable>(
            IrisSlotCallable{std::function<std::vector<Component>()>(std::forward<Callable>(Fn))});
    }
}

// Wraps a `<Native>` element's `build` prop for storage on `Component::NativeBuilder`
// (docs/archive/iris_next_steps_resolved.md, "No way to declare a custom widget/imperative-draw node as an Iris
// element"). `Umbra::IWidget` is already a complete type by this point (`Component.h`
// includes it directly, unlike `IrisSlotCallable` above which only needs `Component`
// itself), so this doesn't need `IrisSlotCallable`'s `if constexpr` return-type dispatch --
// there's exactly one valid return shape.
struct IrisNativeBuilder {
    std::function<std::unique_ptr<Umbra::IWidget>()> Build;
};

template <typename Callable>
std::shared_ptr<IrisNativeBuilder> MakeNativeBuilder(Callable&& Fn) {
    static_assert(std::is_same_v<std::invoke_result_t<Callable>, std::unique_ptr<Umbra::IWidget>>,
                  "<Native>'s build prop must return std::unique_ptr<Umbra::IWidget>");
    return std::make_shared<IrisNativeBuilder>(
        IrisNativeBuilder{std::function<std::unique_ptr<Umbra::IWidget>()>(std::forward<Callable>(Fn))});
}

} // namespace Iris
