#include "Iris/Reconciler.h"
#include "Iris/Signal.h"
#include "Iris/SlotResolution.h"

#include <cstdio>
#include <functional>
#include <set>
#include <string>

extern int Failures; // defined in CppTokenizerTests.cpp

namespace {

void Expect(bool Condition, const std::string& Description) {
    if (Condition) {
        std::printf("[PASS] %s\n", Description.c_str());
    } else {
        std::printf("[FAIL] %s\n", Description.c_str());
        ++Failures;
    }
}

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

void TestSlotAsSoleChildMountsAtIndexZero() {
    iris::MountFn Mount = TestMounter();

    Iris::IrisComponent RootNode =
        MakeFrame({MakeSlot(Iris::MakeSlotCallable([]() -> Iris::IrisComponent { return MakeText("hello"); }))});
    std::unique_ptr<Umbra::IWidget> Root = Mount(RootNode); // static build: the Slot child is skipped, 0 real children

    Expect(Root->GetChildCount() == 0, "the static build alone produces zero children (Slot skipped, like None)");

    auto Slots = iris::ResolveSlots(*Root, RootNode, Mount);
    Expect(Slots.size() == 1, "one SlotState was created");
    Expect(Root->GetChildCount() == 1, "ResolveSlots spliced the slot's content in as a real child");
    Expect(dynamic_cast<MockWidget*>(Root->GetChildAt(0))->Text == "hello", "and it's the right content");
}

void TestSlotBetweenTwoStaticSiblingsMountsAtCorrectIndex() {
    iris::MountFn Mount = TestMounter();

    Iris::IrisComponent RootNode = MakeFrame({
        MakeText("before"),
        MakeSlot(Iris::MakeSlotCallable([]() -> Iris::IrisComponent { return MakeText("slot-content"); })),
        MakeText("after"),
    });
    std::unique_ptr<Umbra::IWidget> Root = Mount(RootNode);
    Expect(Root->GetChildCount() == 2, "the static build produces the two static children only");

    auto Slots = iris::ResolveSlots(*Root, RootNode, Mount);
    Expect(Slots.size() == 1, "one SlotState created");
    Expect(Root->GetChildCount() == 3, "the slot's content was inserted between the two static siblings");
    Expect(dynamic_cast<MockWidget*>(Root->GetChildAt(0))->Text == "before", "first child unchanged");
    Expect(dynamic_cast<MockWidget*>(Root->GetChildAt(1))->Text == "slot-content", "slot content in the middle");
    Expect(dynamic_cast<MockWidget*>(Root->GetChildAt(2))->Text == "after", "third child unchanged");
}

void TestSlotReturningNoneContributesNothing() {
    iris::MountFn Mount = TestMounter();

    Iris::IrisComponent RootNode = MakeFrame({
        MakeText("before"),
        MakeSlot(Iris::MakeSlotCallable([]() -> Iris::IrisComponent { return nullptr; })),
        MakeText("after"),
    });
    std::unique_ptr<Umbra::IWidget> Root = Mount(RootNode);
    auto                             Slots = iris::ResolveSlots(*Root, RootNode, Mount);

    Expect(Root->GetChildCount() == 2, "a Slot whose first render is None contributes no child at all");
    Expect(dynamic_cast<MockWidget*>(Root->GetChildAt(0))->Text == "before" &&
               dynamic_cast<MockWidget*>(Root->GetChildAt(1))->Text == "after",
           "the two static siblings sit directly adjacent");
}

void TestSlotDrivenBySignalUpdatesRealTreeOnTick() {
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
    Expect(Root->GetChildCount() == 2, "initially hidden — only the two static siblings");

    Show.set(true);
    iris::Tick();
    Expect(Root->GetChildCount() == 3, "after the signal fires and Tick() runs, the slot's content is now attached");
    Expect(dynamic_cast<MockWidget*>(Root->GetChildAt(1))->Text == "shown", "in the correct position");

    Show.set(false);
    iris::Tick();
    Expect(Root->GetChildCount() == 2, "and it's removed again when the signal flips back");
}

void TestNestedSlotInsideStaticChildIsResolved() {
    iris::MountFn Mount = TestMounter();

    Iris::IrisComponent RootNode = MakeFrame(
        {MakeFrame({MakeSlot(Iris::MakeSlotCallable([]() -> Iris::IrisComponent { return MakeText("deep"); }))})});
    std::unique_ptr<Umbra::IWidget> Root = Mount(RootNode);
    auto                             Slots = iris::ResolveSlots(*Root, RootNode, Mount);

    Expect(Slots.size() == 1, "the nested slot was found by recursing into the static child");
    Expect(Root->GetChildCount() == 1, "outer Frame still has its one static child");
    Umbra::IWidget* Inner = Root->GetChildAt(0);
    Expect(Inner->GetChildCount() == 1, "the inner Frame now has the slot's content attached");
    Expect(dynamic_cast<MockWidget*>(Inner->GetChildAt(0))->Text == "deep", "with the right content");
}

void TestListReturningSlotMountsAllItems() {
    iris::MountFn Mount = TestMounter();

    Iris::IrisComponent RootNode = MakeFrame({MakeSlot(Iris::MakeSlotCallable(
        []() -> std::vector<Iris::IrisComponent> { return {MakeText("a"), MakeText("b"), MakeText("c")}; }))});
    std::unique_ptr<Umbra::IWidget> Root = Mount(RootNode);

    auto Slots = iris::ResolveSlots(*Root, RootNode, Mount);
    Expect(Slots.size() == 1, "one SlotState was created for the list-returning slot");
    Expect(Root->GetChildCount() == 3, "all three list items were spliced in as real children");
    Expect(dynamic_cast<MockWidget*>(Root->GetChildAt(0))->Text == "a", "first item");
    Expect(dynamic_cast<MockWidget*>(Root->GetChildAt(1))->Text == "b", "second item");
    Expect(dynamic_cast<MockWidget*>(Root->GetChildAt(2))->Text == "c", "third item");
}

void TestListReturningSlotBetweenTwoStaticSiblings() {
    iris::MountFn Mount = TestMounter();

    Iris::IrisComponent RootNode = MakeFrame({
        MakeText("before"),
        MakeSlot(Iris::MakeSlotCallable(
            []() -> std::vector<Iris::IrisComponent> { return {MakeText("a"), MakeText("b")}; })),
        MakeText("after"),
    });
    std::unique_ptr<Umbra::IWidget> Root = Mount(RootNode);
    auto                             Slots = iris::ResolveSlots(*Root, RootNode, Mount);

    Expect(Root->GetChildCount() == 4, "two static siblings plus the two list items");
    Expect(dynamic_cast<MockWidget*>(Root->GetChildAt(0))->Text == "before", "leading static sibling unchanged");
    Expect(dynamic_cast<MockWidget*>(Root->GetChildAt(1))->Text == "a", "first list item in the middle");
    Expect(dynamic_cast<MockWidget*>(Root->GetChildAt(2))->Text == "b", "second list item in the middle");
    Expect(dynamic_cast<MockWidget*>(Root->GetChildAt(3))->Text == "after", "trailing static sibling unchanged");
}

void TestListReturningSlotGrowthShiftsTrailingStaticSibling() {
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

    Expect(Root->GetChildCount() == 2, "one list item plus the trailing static sibling");
    Expect(dynamic_cast<MockWidget*>(Root->GetChildAt(1))->Text == "after", "trailing sibling right after one item");

    Count.set(4);
    iris::Tick();
    Expect(Root->GetChildCount() == 5, "the list grew to four items, still plus the trailing static sibling");
    Expect(dynamic_cast<MockWidget*>(Root->GetChildAt(4))->Text == "after",
           "the trailing static sibling shifted along with the growing list");

    Count.set(0);
    iris::Tick();
    Expect(Root->GetChildCount() == 1, "the list shrank to nothing");
    Expect(dynamic_cast<MockWidget*>(Root->GetChildAt(0))->Text == "after",
           "the trailing static sibling shifted back down to index zero");
}

void TestTwoListReturningSlotSiblingsShiftEachOther() {
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

    Expect(Slots.size() == 2, "both list-returning slots were resolved");
    Expect(Root->GetChildCount() == 3, "two items from the first slot plus one from the second");
    Expect(dynamic_cast<MockWidget*>(Root->GetChildAt(2))->Text == "second0",
           "the second slot's content sits right after the first slot's own live count");

    FirstCount.set(0);
    iris::Tick();
    Expect(Root->GetChildCount() == 1, "the first slot shrank to nothing");
    Expect(dynamic_cast<MockWidget*>(Root->GetChildAt(0))->Text == "second0",
           "the second slot's content shifted down to index zero, following the first slot's live count");
}

void TestDestroyingSlotStateDetachesItsContent() {
    iris::MountFn Mount = TestMounter();

    Iris::IrisComponent RootNode =
        MakeFrame({MakeSlot(Iris::MakeSlotCallable([]() -> Iris::IrisComponent { return MakeText("hello"); }))});
    std::unique_ptr<Umbra::IWidget> Root = Mount(RootNode);
    auto                             Slots = iris::ResolveSlots(*Root, RootNode, Mount);
    Expect(Root->GetChildCount() == 1, "content attached");

    Slots.clear(); // destroys the SlotState
    Expect(Root->GetChildCount() == 0, "destroying the SlotState detaches (and drops) its content from the real tree");
}

} // namespace

void RunSlotResolutionTests() {
    TestSlotAsSoleChildMountsAtIndexZero();
    TestSlotBetweenTwoStaticSiblingsMountsAtCorrectIndex();
    TestSlotReturningNoneContributesNothing();
    TestSlotDrivenBySignalUpdatesRealTreeOnTick();
    TestNestedSlotInsideStaticChildIsResolved();
    TestListReturningSlotMountsAllItems();
    TestListReturningSlotBetweenTwoStaticSiblings();
    TestListReturningSlotGrowthShiftsTrailingStaticSibling();
    TestTwoListReturningSlotSiblingsShiftEachOther();
    TestDestroyingSlotStateDetachesItsContent();
}
