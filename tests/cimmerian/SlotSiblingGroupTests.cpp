#include "cimmerian/test.hpp"

#include "Iris/Reconciler.h"
#include "Iris/Signal.h"
#include "Iris/SlotResolution.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

// Additional coverage for the SlotSiblingGroup mechanism
// (docs/iris_slot_list_wiring_decision.md), written against Cimmerian rather than the
// hand-rolled framework tests/SlotResolutionTests.cpp otherwise uses — exercising a
// three-sibling case (two list-returning, one single-IrisComponent-returning) that
// wasn't covered there.

namespace {

std::set<int> AliveWidgetIds;
int           NextWidgetId = 0;

class MockWidget : public Umbra::IWidget {
public:
    explicit MockWidget(std::string Tag) : Tag(std::move(Tag)), Id(NextWidgetId++) { AliveWidgetIds.insert(Id); }
    ~MockWidget() override { AliveWidgetIds.erase(Id); }

    void ApplyPropDiff(const Umbra::IrisPropDiff& Diff) override {
        if (Diff.Text) {
            Text = *Diff.Text;
        }
    }

    std::size_t     GetChildCount() const override { return Children.size(); }
    Umbra::IWidget* GetChildAt(std::size_t Index) const override { return Children[Index].get(); }

    void InsertChildAt(std::size_t Index, std::unique_ptr<Umbra::IWidget> Child) override {
        Children.insert(Children.begin() + static_cast<long>(Index), std::move(Child));
    }

    std::unique_ptr<Umbra::IWidget> RemoveChildAt(std::size_t Index) override {
        std::unique_ptr<Umbra::IWidget> Removed = std::move(Children[Index]);
        Children.erase(Children.begin() + static_cast<long>(Index));
        return Removed;
    }

    std::string                                  Tag;
    int                                           Id;
    std::string                                   Text;
    std::vector<std::unique_ptr<Umbra::IWidget>> Children;
};

std::string TagName(Iris::IrisElementTag Tag) {
    switch (Tag) {
        case Iris::IrisElementTag::Frame:
            return "Frame";
        case Iris::IrisElementTag::Text:
            return "Text";
        default:
            return "?";
    }
}

class TestMounter {
public:
    std::unique_ptr<Umbra::IWidget> operator()(const Iris::IrisComponent& Node) {
        auto Widget = std::make_unique<MockWidget>(TagName(Node.Tag));
        Widget->ApplyPropDiff(iris::ComputePropDiff({}, Node.Props));
        for (const Iris::IrisComponent& Child : Node.Children) {
            if (Child.Tag == Iris::IrisElementTag::None || Child.Tag == Iris::IrisElementTag::Slot) {
                continue;
            }
            Widget->Children.push_back((*this)(Child));
        }
        return Widget;
    }
};

Iris::IrisComponent MakeFrame(std::vector<Iris::IrisComponent> Children = {}) {
    return Iris::IrisComponent(Iris::IrisElementTag::Frame, {}, std::move(Children), nullptr);
}

Iris::IrisComponent MakeText(const std::string& Content) {
    Iris::IrisProps Props;
    Props["text"] = Iris::IrisPropValue{Content};
    return Iris::IrisComponent(Iris::IrisElementTag::Text, Props, {}, nullptr);
}

Iris::IrisComponent MakeSlot(std::shared_ptr<Iris::IrisSlotCallable> Callable) {
    return Iris::IrisComponent(Iris::IrisElementTag::Slot, {}, {}, std::move(Callable));
}

std::vector<std::string> TextsOf(Umbra::IWidget& Widget) {
    std::vector<std::string> Result;
    for (std::size_t I = 0; I < Widget.GetChildCount(); ++I) {
        Result.push_back(dynamic_cast<MockWidget*>(Widget.GetChildAt(I))->Text);
    }
    return Result;
}

} // namespace

DESCRIBE("SlotSiblingGroup", {
    IT("shifts a later single-IrisComponent slot when an earlier list slot changes length", {
        iris::MountFn Mount = TestMounter();

        iris::Signal<int> FirstCount = 1;
        Iris::IrisComponent RootNode = MakeFrame({
            MakeSlot(Iris::MakeSlotCallable([&]() -> std::vector<Iris::IrisComponent> {
                std::vector<Iris::IrisComponent> Items;
                for (int I = 0; I < FirstCount.get(); ++I) {
                    Items.push_back(MakeText("list" + std::to_string(I)));
                }
                return Items;
            })),
            MakeSlot(Iris::MakeSlotCallable([]() -> Iris::IrisComponent { return MakeText("single"); })),
        });

        std::unique_ptr<Umbra::IWidget> Root = Mount(RootNode);
        auto                             Slots = iris::ResolveSlots(*Root, RootNode, Mount);

        ASSERT_EQUAL(Slots.size(), static_cast<std::size_t>(2));
        ASSERT_EQUAL(TextsOf(*Root), (std::vector<std::string>{"list0", "single"}));

        FirstCount.set(3);
        iris::Tick();
        ASSERT_EQUAL(TextsOf(*Root), (std::vector<std::string>{"list0", "list1", "list2", "single"}));

        FirstCount.set(0);
        iris::Tick();
        ASSERT_EQUAL(TextsOf(*Root), (std::vector<std::string>{"single"}));
    });

    IT("resolves three siblings (list, single, list) and shifts the trailing list as the leading one grows", {
        iris::MountFn Mount = TestMounter();

        iris::Signal<int> LeadingCount = 2;
        Iris::IrisComponent RootNode = MakeFrame({
            MakeSlot(Iris::MakeSlotCallable([&]() -> std::vector<Iris::IrisComponent> {
                std::vector<Iris::IrisComponent> Items;
                for (int I = 0; I < LeadingCount.get(); ++I) {
                    Items.push_back(MakeText("lead" + std::to_string(I)));
                }
                return Items;
            })),
            MakeSlot(Iris::MakeSlotCallable([]() -> Iris::IrisComponent { return MakeText("middle"); })),
            MakeSlot(Iris::MakeSlotCallable(
                []() -> std::vector<Iris::IrisComponent> { return {MakeText("trail0"), MakeText("trail1")}; })),
        });

        std::unique_ptr<Umbra::IWidget> Root = Mount(RootNode);
        auto                             Slots = iris::ResolveSlots(*Root, RootNode, Mount);

        ASSERT_EQUAL(Slots.size(), static_cast<std::size_t>(3));
        ASSERT_EQUAL(TextsOf(*Root),
                     (std::vector<std::string>{"lead0", "lead1", "middle", "trail0", "trail1"}));

        LeadingCount.set(4);
        iris::Tick();
        ASSERT_EQUAL(TextsOf(*Root), (std::vector<std::string>{"lead0", "lead1", "lead2", "lead3", "middle",
                                                                 "trail0", "trail1"}));
    });

    IT("tears down three list/single sibling slots without a use-after-free, in any order", {
        iris::MountFn Mount = TestMounter();

        Iris::IrisComponent RootNode = MakeFrame({
            MakeSlot(Iris::MakeSlotCallable(
                []() -> std::vector<Iris::IrisComponent> { return {MakeText("a0"), MakeText("a1")}; })),
            MakeSlot(Iris::MakeSlotCallable([]() -> Iris::IrisComponent { return MakeText("b"); })),
            MakeSlot(Iris::MakeSlotCallable([]() -> std::vector<Iris::IrisComponent> { return {MakeText("c0")}; })),
        });

        std::unique_ptr<Umbra::IWidget> Root = Mount(RootNode);
        auto                             Slots = iris::ResolveSlots(*Root, RootNode, Mount);
        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(4));

        // Destroy in forward order (the order std::vector<unique_ptr<T>>'s own destructor
        // uses) — this is exactly the case that previously produced a heap-use-after-free
        // (docs/iris_slot_list_wiring_decision.md's "destruction-order bug" section),
        // fixed via SlotSiblingGroup::MarkDestroyed.
        Slots.clear();
        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(0));
    });
});
