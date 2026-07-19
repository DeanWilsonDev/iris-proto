#pragma once

#include "Iris/IrisElementTag.h"
#include "Iris/IrisProps.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <variant>
#include <vector>

namespace Iris {

struct IrisSlotCallable;

// The backend-agnostic IR node `render { }` blocks construct (docs/iris_core_spec.md
// §2.5). Codegen (Codegen.h) emits C++23 expressions that build values of this type;
// no backend type (Penumbra or otherwise) appears anywhere in this repo.
//
// `SlotCallable` is set only when `Tag == IrisElementTag::Slot` — see
// docs/iris_stage1_codegen_decision.md's Gap 1 for why it can't be a `Props` entry or a
// `Children` entry, and why it's held behind a pointer rather than embedded directly.
//
// Every conditional-rendering example in docs/iris_core_spec.md (§1.5, §9) writes
// `return nullptr;` inside a `<Slot>` callable declared to return `IrisComponent` — the
// `nullptr_t` converting constructor below exists so that compiles, producing an
// `IrisElementTag::None`-tagged sentinel rather than failing to compile (the gap tracked
// and closed in docs/iris_core_spec.md §8, docs/iris_escape_hatch_decision.md's
// Verification section). Declaring any constructor loses aggregate-ness, so the ordinary
// 4-field constructor below exists to keep `IrisComponent{Tag, Props, Children,
// SlotCallable}` — Codegen.h's emitted call shape — compiling unchanged.
struct IrisComponent {
    IrisElementTag                    Tag{IrisElementTag::Frame};
    IrisProps                         Props;
    std::vector<IrisComponent>        Children;
    std::shared_ptr<IrisSlotCallable> SlotCallable;

    IrisComponent() = default;
    IrisComponent(IrisElementTag Tag, IrisProps Props, std::vector<IrisComponent> Children,
                  std::shared_ptr<IrisSlotCallable> SlotCallable)
        : Tag(Tag), Props(std::move(Props)), Children(std::move(Children)),
          SlotCallable(std::move(SlotCallable)) {}
    IrisComponent(std::nullptr_t) noexcept : Tag(IrisElementTag::None) {}
};

// Defined only once `IrisComponent` above is a complete type — unlike `std::vector`,
// `std::function` has no standard guarantee of working with an incomplete return type,
// so this can't be a direct member of `IrisComponent` (docs/iris_stage1_codegen_decision.md).
struct IrisSlotCallable {
    std::variant<std::function<IrisComponent()>, std::function<std::vector<IrisComponent>()>> Callable;
};

// Wraps a `<Slot>` child lambda for storage on `IrisComponent::SlotCallable`. The
// preprocessor never inspects escape hatch contents (docs/iris_core_spec.md §1.4), so it
// can't know whether the lambda it emitted a call to returns `IrisComponent` or
// `std::vector<IrisComponent>` — that choice is made here, by the host compiler, via
// `if constexpr` on the callable's actual return type. A lambda returning neither fails
// to compile inside this function, which is a host-compiler error (§6 tier 2), not a
// preprocessor-level one.
template <typename Callable>
std::shared_ptr<IrisSlotCallable> MakeSlotCallable(Callable&& Fn) {
    using Result = std::invoke_result_t<Callable>;
    static_assert(std::is_same_v<Result, IrisComponent> || std::is_same_v<Result, std::vector<IrisComponent>>,
                  "<Slot> callable must return IrisComponent or std::vector<IrisComponent>");

    if constexpr (std::is_same_v<Result, IrisComponent>) {
        return std::make_shared<IrisSlotCallable>(
            IrisSlotCallable{std::function<IrisComponent()>(std::forward<Callable>(Fn))});
    } else {
        return std::make_shared<IrisSlotCallable>(
            IrisSlotCallable{std::function<std::vector<IrisComponent>()>(std::forward<Callable>(Fn))});
    }
}

} // namespace Iris
