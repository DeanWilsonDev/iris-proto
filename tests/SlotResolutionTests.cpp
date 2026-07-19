#include "cimmerian/test.hpp"

#include "Iris/Reconciler.h"
#include "Iris/Signal.h"
#include "Iris/SlotResolution.h"

#include <set>
#include <string>

namespace {

// Mirrors ReconcilerTests.cpp's MockWidget — a minimal Umbra::IWidget for testing
// purely against the interface, no real backend needed.
std::set<int> AliveWidgetIds;
int           NextWidgetId = 0;

class MockWidget : public Umbra::IWidget {
public:
    explicit MockWidget(std::string Tag) : Tag(std::move(Tag)), Id(NextWidgetId++) { AliveWidgetIds.insert(Id); }
    ~MockWidget() override { AliveWidgetIds.erase(Id); }

    void ApplyPropDiff(const Umbra::IrisPropDiff& Diff) override {
        if (Diff.ClassName) {
            ClassName = *Diff.ClassName;
        }
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

    std::string                                   Tag;
    int                                            Id;
    std::string                                    ClassName;
    std::string                                    Text;
    std::vector<std::unique_ptr<Umbra::IWidget>>  Children;
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
                continue; // matches BuildWidgetTree's own real behavior for these two
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

} // namespace

DESCRIBE("SlotResolution", {
    IT("a Slot as the sole child mounts at index zero", {
        iris::MountFn Mount = TestMounter();

        Iris::IrisComponent RootNode =
            MakeFrame({MakeSlot(Iris::MakeSlotCallable([]() -> Iris::IrisComponent { return MakeText("hello"); }))});
        std::unique_ptr<Umbra::IWidget> Root = Mount(RootNode); // static build: the Slot child is skipped, 0 real children

        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(0));
        // the static build alone produces zero children (Slot skipped, like None)

        auto Slots = iris::ResolveSlots(*Root, RootNode, Mount);
        ASSERT_EQUAL(Slots.size(), static_cast<std::size_t>(1)); // one SlotState was created
        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(1));
        // ResolveSlots spliced the slot's content in as a real child
        ASSERT_TRUE(dynamic_cast<MockWidget*>(Root->GetChildAt(0))->Text == "hello");
    });

    IT("a Slot between two static siblings mounts at the correct index", {
        iris::MountFn Mount = TestMounter();

        Iris::IrisComponent RootNode = MakeFrame({
            MakeText("before"),
            MakeSlot(Iris::MakeSlotCallable([]() -> Iris::IrisComponent { return MakeText("slot-content"); })),
            MakeText("after"),
        });
        std::unique_ptr<Umbra::IWidget> Root = Mount(RootNode);
        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(2));
        // the static build produces the two static children only

        auto Slots = iris::ResolveSlots(*Root, RootNode, Mount);
        ASSERT_EQUAL(Slots.size(), static_cast<std::size_t>(1)); // one SlotState created
        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(3));
        // the slot's content was inserted between the two static siblings
        ASSERT_TRUE(dynamic_cast<MockWidget*>(Root->GetChildAt(0))->Text == "before");
        ASSERT_TRUE(dynamic_cast<MockWidget*>(Root->GetChildAt(1))->Text == "slot-content");
        ASSERT_TRUE(dynamic_cast<MockWidget*>(Root->GetChildAt(2))->Text == "after");
    });

    IT("a Slot returning None contributes nothing", {
        iris::MountFn Mount = TestMounter();

        Iris::IrisComponent RootNode = MakeFrame({
            MakeText("before"),
            MakeSlot(Iris::MakeSlotCallable([]() -> Iris::IrisComponent { return nullptr; })),
            MakeText("after"),
        });
        std::unique_ptr<Umbra::IWidget> Root = Mount(RootNode);
        auto                             Slots = iris::ResolveSlots(*Root, RootNode, Mount);

        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(2));
        // a Slot whose first render is None contributes no child at all
        ASSERT_TRUE(dynamic_cast<MockWidget*>(Root->GetChildAt(0))->Text == "before" &&
                    dynamic_cast<MockWidget*>(Root->GetChildAt(1))->Text == "after");
        // the two static siblings sit directly adjacent
    });

    IT("a Slot driven by a signal updates the real tree on Tick()", {
        iris::MountFn Mount = TestMounter();

        iris::Signal<bool> Show = false;
        Iris::IrisComponent RootNode = MakeFrame({
            MakeText("before"),
            MakeSlot(Iris::MakeSlotCallable(
                [&]() -> Iris::IrisComponent { return Show.get() ? MakeText("shown") : Iris::IrisComponent(nullptr); })),
            MakeText("after"),
        });
        std::unique_ptr<Umbra::IWidget> Root = Mount(RootNode);
        auto                             Slots = iris::ResolveSlots(*Root, RootNode, Mount);
        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(2)); // initially hidden — only the two static siblings

        Show.set(true);
        iris::Tick();
        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(3));
        // after the signal fires and Tick() runs, the slot's content is now attached
        ASSERT_TRUE(dynamic_cast<MockWidget*>(Root->GetChildAt(1))->Text == "shown"); // in the correct position

        Show.set(false);
        iris::Tick();
        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(2)); // and it's removed again when the signal flips back
    });

    IT("a Slot nested inside a static child is resolved", {
        iris::MountFn Mount = TestMounter();

        Iris::IrisComponent RootNode = MakeFrame(
            {MakeFrame({MakeSlot(Iris::MakeSlotCallable([]() -> Iris::IrisComponent { return MakeText("deep"); }))})});
        std::unique_ptr<Umbra::IWidget> Root = Mount(RootNode);
        auto                             Slots = iris::ResolveSlots(*Root, RootNode, Mount);

        ASSERT_EQUAL(Slots.size(), static_cast<std::size_t>(1)); // the nested slot was found by recursing into the static child
        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(1)); // outer Frame still has its one static child
        Umbra::IWidget* Inner = Root->GetChildAt(0);
        ASSERT_EQUAL(Inner->GetChildCount(), static_cast<std::size_t>(1)); // the inner Frame now has the slot's content attached
        ASSERT_TRUE(dynamic_cast<MockWidget*>(Inner->GetChildAt(0))->Text == "deep");
    });

    IT("a list-returning Slot mounts all its items", {
        iris::MountFn Mount = TestMounter();

        Iris::IrisComponent RootNode = MakeFrame({MakeSlot(Iris::MakeSlotCallable(
            []() -> std::vector<Iris::IrisComponent> { return {MakeText("a"), MakeText("b"), MakeText("c")}; }))});
        std::unique_ptr<Umbra::IWidget> Root = Mount(RootNode);

        auto Slots = iris::ResolveSlots(*Root, RootNode, Mount);
        ASSERT_EQUAL(Slots.size(), static_cast<std::size_t>(1)); // one SlotState was created for the list-returning slot
        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(3)); // all three list items were spliced in as real children
        ASSERT_TRUE(dynamic_cast<MockWidget*>(Root->GetChildAt(0))->Text == "a");
        ASSERT_TRUE(dynamic_cast<MockWidget*>(Root->GetChildAt(1))->Text == "b");
        ASSERT_TRUE(dynamic_cast<MockWidget*>(Root->GetChildAt(2))->Text == "c");
    });

    IT("a list-returning Slot between two static siblings", {
        iris::MountFn Mount = TestMounter();

        Iris::IrisComponent RootNode = MakeFrame({
            MakeText("before"),
            MakeSlot(Iris::MakeSlotCallable(
                []() -> std::vector<Iris::IrisComponent> { return {MakeText("a"), MakeText("b")}; })),
            MakeText("after"),
        });
        std::unique_ptr<Umbra::IWidget> Root = Mount(RootNode);
        auto                             Slots = iris::ResolveSlots(*Root, RootNode, Mount);

        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(4)); // two static siblings plus the two list items
        ASSERT_TRUE(dynamic_cast<MockWidget*>(Root->GetChildAt(0))->Text == "before");
        ASSERT_TRUE(dynamic_cast<MockWidget*>(Root->GetChildAt(1))->Text == "a");
        ASSERT_TRUE(dynamic_cast<MockWidget*>(Root->GetChildAt(2))->Text == "b");
        ASSERT_TRUE(dynamic_cast<MockWidget*>(Root->GetChildAt(3))->Text == "after");
    });

    IT("a list-returning Slot's growth shifts the trailing static sibling", {
        iris::MountFn Mount = TestMounter();

        iris::Signal<int> Count = 1;
        Iris::IrisComponent RootNode = MakeFrame({
            MakeSlot(Iris::MakeSlotCallable([&]() -> std::vector<Iris::IrisComponent> {
                std::vector<Iris::IrisComponent> Items;
                for (int I = 0; I < Count.get(); ++I) {
                    Items.push_back(MakeText("item" + std::to_string(I)));
                }
                return Items;
            })),
            MakeText("after"),
        });
        std::unique_ptr<Umbra::IWidget> Root = Mount(RootNode);
        auto                             Slots = iris::ResolveSlots(*Root, RootNode, Mount);

        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(2)); // one list item plus the trailing static sibling
        ASSERT_TRUE(dynamic_cast<MockWidget*>(Root->GetChildAt(1))->Text == "after");

        Count.set(4);
        iris::Tick();
        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(5));
        // the list grew to four items, still plus the trailing static sibling
        ASSERT_TRUE(dynamic_cast<MockWidget*>(Root->GetChildAt(4))->Text == "after");
        // the trailing static sibling shifted along with the growing list

        Count.set(0);
        iris::Tick();
        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(1)); // the list shrank to nothing
        ASSERT_TRUE(dynamic_cast<MockWidget*>(Root->GetChildAt(0))->Text == "after");
        // the trailing static sibling shifted back down to index zero
    });

    IT("two list-returning Slot siblings shift each other", {
        iris::MountFn Mount = TestMounter();

        iris::Signal<int> FirstCount = 2;
        Iris::IrisComponent RootNode = MakeFrame({
            MakeSlot(Iris::MakeSlotCallable([&]() -> std::vector<Iris::IrisComponent> {
                std::vector<Iris::IrisComponent> Items;
                for (int I = 0; I < FirstCount.get(); ++I) {
                    Items.push_back(MakeText("first" + std::to_string(I)));
                }
                return Items;
            })),
            MakeSlot(Iris::MakeSlotCallable(
                []() -> std::vector<Iris::IrisComponent> { return {MakeText("second0")}; })),
        });
        std::unique_ptr<Umbra::IWidget> Root = Mount(RootNode);
        auto                             Slots = iris::ResolveSlots(*Root, RootNode, Mount);

        ASSERT_EQUAL(Slots.size(), static_cast<std::size_t>(2)); // both list-returning slots were resolved
        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(3)); // two items from the first slot plus one from the second
        ASSERT_TRUE(dynamic_cast<MockWidget*>(Root->GetChildAt(2))->Text == "second0");
        // the second slot's content sits right after the first slot's own live count

        FirstCount.set(0);
        iris::Tick();
        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(1)); // the first slot shrank to nothing
        ASSERT_TRUE(dynamic_cast<MockWidget*>(Root->GetChildAt(0))->Text == "second0");
        // the second slot's content shifted down to index zero, following the first slot's live count
    });

    IT("a Slot's output containing a nested Slot is discovered", {
        iris::MountFn Mount = TestMounter();

        // The outer slot's callable returns an ordinary wrapper Frame whose own child is a
        // <Slot> — the case docs/iris_nested_slot_discovery_decision.md closes: a <Slot>
        // nested inside another <Slot>'s own dynamically-produced output, not present
        // anywhere in the original static tree.
        Iris::IrisComponent RootNode = MakeFrame({MakeSlot(Iris::MakeSlotCallable([]() -> Iris::IrisComponent {
            return MakeFrame(
                {MakeSlot(Iris::MakeSlotCallable([]() -> Iris::IrisComponent { return MakeText("nested"); }))});
        }))});
        std::unique_ptr<Umbra::IWidget> Root = Mount(RootNode);
        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(0));
        // static build alone: the outer Slot is skipped, zero real children

        auto Slots = iris::ResolveSlots(*Root, RootNode, Mount);
        ASSERT_EQUAL(Slots.size(), static_cast<std::size_t>(1)); // ResolveSlots only ever discovers the OUTER slot directly
        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(1)); // the outer slot's own wrapper Frame is now attached

        Umbra::IWidget* Wrapper = Root->GetChildAt(0);
        ASSERT_EQUAL(Wrapper->GetChildCount(), static_cast<std::size_t>(1));
        // the nested slot's own content was discovered and attached inside the wrapper
        ASSERT_TRUE(dynamic_cast<MockWidget*>(Wrapper->GetChildAt(0))->Text == "nested");
        // with the right content, even though it never appeared in the original static tree
    });

    IT("a nested Slot reacts to its own signal independently", {
        iris::MountFn Mount = TestMounter();

        iris::Signal<bool> ShowNested = false;
        Iris::IrisComponent RootNode = MakeFrame({MakeSlot(Iris::MakeSlotCallable([&]() -> Iris::IrisComponent {
            return MakeFrame({MakeSlot(Iris::MakeSlotCallable([&]() -> Iris::IrisComponent {
                return ShowNested.get() ? MakeText("shown") : Iris::IrisComponent(nullptr);
            }))});
        }))});
        std::unique_ptr<Umbra::IWidget> Root = Mount(RootNode);
        auto                             Slots = iris::ResolveSlots(*Root, RootNode, Mount);

        Umbra::IWidget* Wrapper = Root->GetChildAt(0);
        ASSERT_EQUAL(Wrapper->GetChildCount(), static_cast<std::size_t>(0));
        // the nested slot's first render is None — nothing attached yet

        ShowNested.set(true);
        iris::Tick();
        ASSERT_EQUAL(Wrapper->GetChildCount(), static_cast<std::size_t>(1));
        // the nested slot's own signal fired and Tick() reconciled it
        ASSERT_TRUE(dynamic_cast<MockWidget*>(Wrapper->GetChildAt(0))->Text == "shown");

        ShowNested.set(false);
        iris::Tick();
        ASSERT_EQUAL(Wrapper->GetChildCount(), static_cast<std::size_t>(0));
        // and it's removed again when the nested slot's own signal flips back
    });

    IT("a nested Slot is torn down when the outer Slot's output becomes None", {
        iris::MountFn Mount = TestMounter();

        iris::Signal<bool> ShowOuter = true;
        Iris::IrisComponent RootNode = MakeFrame({MakeSlot(Iris::MakeSlotCallable([&]() -> Iris::IrisComponent {
            if (!ShowOuter.get()) {
                return nullptr;
            }
            return MakeFrame(
                {MakeSlot(Iris::MakeSlotCallable([]() -> Iris::IrisComponent { return MakeText("nested"); }))});
        }))});
        std::unique_ptr<Umbra::IWidget> Root = Mount(RootNode);
        auto                             Slots = iris::ResolveSlots(*Root, RootNode, Mount);
        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(1)); // wrapper attached, with the nested slot's content inside it

        ShowOuter.set(false);
        iris::Tick(); // must not crash or leave a dangling nested SlotState behind
        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(0));
        // the outer slot's own output became None, tearing down the wrapper and the nested
        // slot living inside it along with it

        ShowOuter.set(true);
        iris::Tick();
        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(1));
        // and it can remount cleanly afterward, rediscovering the nested slot fresh
        ASSERT_EQUAL(dynamic_cast<MockWidget*>(Root->GetChildAt(0))->GetChildCount(), static_cast<std::size_t>(1));
        // with its own nested slot's content attached again
    });

    IT("a Slot nested inside a list-returning Slot's item is discovered", {
        iris::MountFn Mount = TestMounter();

        Iris::IrisComponent RootNode = MakeFrame({MakeSlot(Iris::MakeSlotCallable([]() -> std::vector<Iris::IrisComponent> {
            return {MakeFrame(
                {MakeSlot(Iris::MakeSlotCallable([]() -> Iris::IrisComponent { return MakeText("item-nested"); }))})};
        }))});
        std::unique_ptr<Umbra::IWidget> Root = Mount(RootNode);
        auto                             Slots = iris::ResolveSlots(*Root, RootNode, Mount);

        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(1)); // the one list item was attached
        Umbra::IWidget* Item = Root->GetChildAt(0);
        ASSERT_EQUAL(Item->GetChildCount(), static_cast<std::size_t>(1));
        // and the slot nested inside that list item was discovered too
        ASSERT_TRUE(dynamic_cast<MockWidget*>(Item->GetChildAt(0))->Text == "item-nested");
    });

    IT("destroying a SlotState detaches its content", {
        iris::MountFn Mount = TestMounter();

        Iris::IrisComponent RootNode =
            MakeFrame({MakeSlot(Iris::MakeSlotCallable([]() -> Iris::IrisComponent { return MakeText("hello"); }))});
        std::unique_ptr<Umbra::IWidget> Root = Mount(RootNode);
        auto                             Slots = iris::ResolveSlots(*Root, RootNode, Mount);
        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(1)); // content attached

        Slots.clear(); // destroys the SlotState
        ASSERT_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(0));
        // destroying the SlotState detaches (and drops) its content from the real tree
    });
});
