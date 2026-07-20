#include "Iris/SlotResolution.h"

#include <functional>
#include <variant>

namespace iris {

namespace {

void ResolveSlotsRecursive(Umbra::IWidget& Widget, const Iris::Component& Node, const MountFn& Mount,
                            std::vector<std::unique_ptr<SlotState>>& Out) {
    // One group per parent's children list, shared by every <Slot> sibling found here —
    // so a later sibling's absolute index can account for an earlier sibling's live,
    // possibly-changing real-child-count contribution (docs/iris_slot_list_wiring_
    // decision.md). Only actually used if at least one <Slot> child turns up; harmless
    // to always allocate one.
    auto Group = std::make_shared<SlotSiblingGroup>();

    // Running count of real widgets contributed by *ordinary* children seen so far —
    // both this child's own position within Widget's already-built static children, and
    // (for a <Slot> child) its fixed StaticPrefixCount within Group.
    std::size_t RealIndex = 0;

    for (const Iris::Component& Child : Node.Children) {
        if (Child.Tag == Iris::IrisElementTag::None) {
            continue; // contributes nothing, both in the original static build and here
        }

        if (Child.Tag == Iris::IrisElementTag::Slot) {
            auto Slot = std::make_unique<SlotState>(Child.SlotCallable, Mount);
            Group->AddEntry(RealIndex, Slot.get());
            const std::size_t GroupIndex = Group->EntryCount() - 1;
            Slot->AttachToGroup(&Widget, Group, GroupIndex);
            Slot->Reconcile(); // initial mount — inserts at Group->AbsoluteIndexOf(GroupIndex)
            Out.push_back(std::move(Slot));
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

std::vector<std::unique_ptr<SlotState>> ResolveSlots(Umbra::IWidget& Widget, const Iris::Component& Node,
                                                       MountFn Mount) {
    std::vector<std::unique_ptr<SlotState>> Result;
    ResolveSlotsRecursive(Widget, Node, Mount, Result);
    return Result;
}

} // namespace iris
