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

    // Two distinct running counters — conflating them was the original bug, and also the
    // trap this fix's own first attempt fell into (verified by a real crash under
    // AddressSanitizer-free debug build, "two list-returning Slot siblings shift each
    // other" — see the caught regression below for why they can't be merged):
    //
    // - `StaticCount`: how many *ordinary* (non-Slot, non-None) children have been seen so
    //   far — never affected by any <Slot>'s own real-widget contribution. This is exactly
    //   `SlotSiblingGroup::AddEntry`'s own `StaticPrefixCount` contract ("the number of
    //   ordinary children directly preceding this slot"). `SlotSiblingGroup::
    //   AbsoluteIndexOf` already separately adds every *earlier* sibling-in-this-group's
    //   own live `CurrentRealChildCount()` on top of `StaticPrefixCount` — so folding a
    //   Slot's own contribution into the value passed to `AddEntry` here would double-count
    //   it there, corrupting every subsequent sibling Slot's own absolute index (confirmed:
    //   this exact mistake reproduced a real out-of-bounds `InsertChildAt` crash in
    //   "two list-returning Slot siblings shift each other").
    // - `RealIndex`: the actual current position within `Widget`'s real children — this
    //   DOES need to account for every earlier <Slot>'s live contribution, since each
    //   `Slot->Reconcile()` call below has already spliced its widget(s) into `Widget`
    //   before the loop reaches the next ordinary sibling, shifting that sibling's real
    //   index by however many widgets were just inserted.
    std::size_t StaticCount = 0;
    std::size_t RealIndex = 0;

    for (const Iris::Component& Child : Node.Children) {
        if (Child.Tag == Iris::IrisElementTag::None) {
            continue; // contributes nothing, both in the original static build and here
        }

        if (Child.Tag == Iris::IrisElementTag::Slot) {
            auto Slot = std::make_unique<SlotState>(Child.SlotCallable, Mount);
            Group->AddEntry(StaticCount, Slot.get());
            const std::size_t GroupIndex = Group->EntryCount() - 1;
            Slot->AttachToGroup(&Widget, Group, GroupIndex);
            Slot->Reconcile(); // initial mount — inserts at Group->AbsoluteIndexOf(GroupIndex)
            // Advance RealIndex (only) past however many real widgets this Slot just
            // contributed (0 for a None/empty result, 1 for a single-Component callable,
            // 0..N for a list-returning one) — otherwise the next ordinary sibling's own
            // Widget.GetChildAt(RealIndex) below reads from a stale, now-wrong index.
            // StaticCount deliberately does NOT advance here — see the comment above.
            RealIndex += Slot->CurrentRealChildCount();
            Out.push_back(std::move(Slot));
            continue;
        }

        // An ordinary static child — recurse into it for any <Slot>s nested inside its
        // own static children, at its current real position within Widget (which may have
        // shifted from its original static-build position if an earlier <Slot> sibling
        // already inserted real widgets before it).
        ResolveSlotsRecursive(*Widget.GetChildAt(RealIndex), Child, Mount, Out);
        ++RealIndex;
        ++StaticCount;
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
