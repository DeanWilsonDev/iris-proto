#include "Iris/SlotResolution.h"

#include <functional>
#include <variant>

namespace iris {

namespace {

void ResolveSlotsRecursive(Umbra::IWidget& Widget, const Iris::IrisComponent& Node, const MountFn& Mount,
                            std::vector<std::unique_ptr<SlotState>>& Out) {
    std::size_t RealIndex = 0;
    for (const Iris::IrisComponent& Child : Node.Children) {
        if (Child.Tag == Iris::IrisElementTag::None) {
            continue; // contributes nothing, both in the original static build and here
        }

        if (Child.Tag == Iris::IrisElementTag::Slot) {
            // Only the single-IrisComponent-returning shape is resolved here — see
            // SlotResolution.h's own doc comment for why the list-returning shape isn't.
            if (!std::holds_alternative<std::function<Iris::IrisComponent()>>(Child.SlotCallable->Callable)) {
                continue;
            }
            auto Slot = std::make_unique<SlotState>(Child.SlotCallable, Mount);
            Slot->AttachAt(&Widget, RealIndex);
            Slot->Reconcile(); // initial mount — inserts at RealIndex unless the first render is None
            const bool Mounted = Slot->HasMountedContent();
            Out.push_back(std::move(Slot));
            if (Mounted) {
                ++RealIndex;
            }
            continue;
        }

        // An ordinary static child, already present in Widget at RealIndex (built by
        // BuildWidgetTree before ResolveSlots ever runs) — recurse into it for any
        // <Slot>s nested inside its own static children.
        ResolveSlotsRecursive(*Widget.GetChildAt(RealIndex), Child, Mount, Out);
        ++RealIndex;
    }
}

} // namespace

std::vector<std::unique_ptr<SlotState>> ResolveSlots(Umbra::IWidget& Widget, const Iris::IrisComponent& Node,
                                                       MountFn Mount) {
    std::vector<std::unique_ptr<SlotState>> Result;
    ResolveSlotsRecursive(Widget, Node, Mount, Result);
    return Result;
}

} // namespace iris
