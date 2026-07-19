#include "Iris/Reconciler.h"

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

// A minimal Umbra::IWidget for testing the reconciler purely against the interface —
// no real backend needed. Tracks its own construction/destruction in a shared set so
// tests can distinguish "the same widget object was reused" (identity preserved) from
// "a new widget was mounted and the old one destroyed" (remount).
std::set<int> AliveWidgetIds;
int           NextWidgetId = 0;

class MockWidget : public Umbra::IWidget {
public:
    explicit MockWidget(std::string Tag) : Tag(std::move(Tag)), Id(NextWidgetId++) { AliveWidgetIds.insert(Id); }
    ~MockWidget() override { AliveWidgetIds.erase(Id); }

    void ApplyPropDiff(const Umbra::IrisPropDiff& Diff) override {
        ++ApplyPropDiffCallCount;
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
    int                                             ApplyPropDiffCallCount{0};
    std::vector<std::unique_ptr<Umbra::IWidget>>  Children;
};

std::string TagName(Iris::IrisElementTag Tag) {
    switch (Tag) {
        case Iris::IrisElementTag::Frame:
            return "Frame";
        case Iris::IrisElementTag::Inline:
            return "Inline";
        case Iris::IrisElementTag::Grid:
            return "Grid";
        case Iris::IrisElementTag::Image:
            return "Image";
        case Iris::IrisElementTag::Text:
            return "Text";
        case Iris::IrisElementTag::Slot:
            return "Slot";
        case Iris::IrisElementTag::None:
            return "None";
    }
    return "?";
}

Iris::IrisComponent MakeNode(Iris::IrisElementTag Tag, Iris::IrisProps Props = {},
                              std::vector<Iris::IrisComponent> Children = {},
                              std::optional<Iris::IrisPropValue> Key = std::nullopt) {
    Iris::IrisComponent Node(Tag, std::move(Props), std::move(Children), nullptr);
    Node.Key = std::move(Key);
    return Node;
}

// Builds a whole subtree recursively, mirroring what a real Stage 2 walker's MountFn
// contract requires: given ANY IrisComponent node, build its entire subtree, since
// ReconcileWidget never recurses into New.Children on the "no old to diff against" path.
class TestMounter {
public:
    explicit TestMounter(int* MountCount) : MountCount_(MountCount) {}

    std::unique_ptr<Umbra::IWidget> operator()(const Iris::IrisComponent& Node) {
        if (MountCount_ != nullptr) {
            ++*MountCount_;
        }
        auto Widget = std::make_unique<MockWidget>(TagName(Node.Tag));
        Umbra::IrisPropDiff Diff = iris::ComputePropDiff({}, Node.Props);
        Widget->ApplyPropDiff(Diff);
        for (const Iris::IrisComponent& Child : Node.Children) {
            Widget->Children.push_back((*this)(Child));
        }
        return Widget;
    }

private:
    int* MountCount_;
};

void TestComputePropDiffOnlyIncludesChangedFields() {
    Iris::IrisProps Old;
    Old["class"] = Iris::IrisPropValue{std::string("a")};
    Iris::IrisProps New;
    New["class"] = Iris::IrisPropValue{std::string("a")};    // unchanged
    New["text"] = Iris::IrisPropValue{std::string("hello")}; // new

    const auto Diff = iris::ComputePropDiff(Old, New);
    Expect(!Diff.ClassName.has_value(), "an unchanged prop value is omitted from the diff");
    Expect(Diff.Text.has_value() && *Diff.Text == "hello", "a new/changed prop value is included");
}

void TestComputePropDiffEventPropsAlwaysIncludedWhenPresent() {
    Iris::IrisProps New;
    New["onPress"] = Iris::IrisPropValue{std::function<void()>([]() {})};
    const auto Diff = iris::ComputePropDiff({}, New);
    Expect(Diff.OnPress.has_value(), "an event prop present in New is always included (no operator== to compare)");
}

void TestReconcileWidgetMountsFreshWhenNoWidgetExists() {
    int                              MountCount = 0;
    TestMounter                       Mount(&MountCount);
    std::unique_ptr<Umbra::IWidget>  Widget;
    const auto                        Old = Iris::IrisComponent(nullptr);
    const auto                        New = MakeNode(Iris::IrisElementTag::Frame);

    iris::ReconcileWidget(Widget, Old, New, Mount);
    Expect(Widget != nullptr, "a fresh mount produces a widget");
    Expect(MountCount == 1, "Mount was called exactly once");
}

void TestReconcileWidgetUpdatesInPlaceOnSameTagAndKey() {
    int         MountCount = 0;
    TestMounter Mount(&MountCount);

    Iris::IrisProps InitialProps;
    InitialProps["class"] = Iris::IrisPropValue{std::string("a")};
    const auto Old = MakeNode(Iris::IrisElementTag::Frame, InitialProps, {}, Iris::IrisPropValue(1));

    std::unique_ptr<Umbra::IWidget> Widget;
    iris::ReconcileWidget(Widget, Iris::IrisComponent(nullptr), Old, Mount);
    const int OriginalMountCount = MountCount;
    const auto* AsMock = dynamic_cast<MockWidget*>(Widget.get());
    const int   OriginalId = AsMock->Id;

    Iris::IrisProps UpdatedProps;
    UpdatedProps["class"] = Iris::IrisPropValue{std::string("b")};
    const auto New = MakeNode(Iris::IrisElementTag::Frame, UpdatedProps, {}, Iris::IrisPropValue(1));

    iris::ReconcileWidget(Widget, Old, New, Mount);
    AsMock = dynamic_cast<MockWidget*>(Widget.get());
    Expect(MountCount == OriginalMountCount, "same tag + same key: no remount, Mount is not called again");
    Expect(AsMock->Id == OriginalId, "the same widget object is reused — identity preserved");
    Expect(AsMock->ClassName == "b", "the prop diff was applied to the reused widget");
}

void TestReconcileWidgetRemountsOnTagMismatch() {
    int         MountCount = 0;
    TestMounter Mount(&MountCount);

    const auto Old = MakeNode(Iris::IrisElementTag::Frame);
    std::unique_ptr<Umbra::IWidget> Widget;
    iris::ReconcileWidget(Widget, Iris::IrisComponent(nullptr), Old, Mount);
    const int OldId = dynamic_cast<MockWidget*>(Widget.get())->Id;
    Expect(AliveWidgetIds.count(OldId) == 1, "the original widget is alive before the remount");

    const auto New = MakeNode(Iris::IrisElementTag::Text);
    iris::ReconcileWidget(Widget, Old, New, Mount);

    Expect(AliveWidgetIds.count(OldId) == 0, "a tag mismatch unmounts (destroys) the old widget");
    Expect(dynamic_cast<MockWidget*>(Widget.get())->Tag == "Text", "and mounts a fresh widget for the new tag");
}

void TestReconcileWidgetNoneUnmountsAndMountsNothing() {
    int         MountCount = 0;
    TestMounter Mount(&MountCount);

    const auto Old = MakeNode(Iris::IrisElementTag::Frame);
    std::unique_ptr<Umbra::IWidget> Widget;
    iris::ReconcileWidget(Widget, Iris::IrisComponent(nullptr), Old, Mount);
    const int OldId = dynamic_cast<MockWidget*>(Widget.get())->Id;

    const Iris::IrisComponent NewNone(nullptr);
    iris::ReconcileWidget(Widget, Old, NewNone, Mount);

    Expect(AliveWidgetIds.count(OldId) == 0, "transitioning to None unmounts the old widget");
    Expect(Widget == nullptr, "and Widget ends up null — no widget for None");
}

void TestReconcileWidgetRecursesIntoChildren() {
    int         MountCount = 0;
    TestMounter Mount(&MountCount);

    std::vector<Iris::IrisComponent> OldChildren;
    OldChildren.push_back(MakeNode(Iris::IrisElementTag::Text));
    const auto Old = MakeNode(Iris::IrisElementTag::Frame, {}, std::move(OldChildren));

    std::unique_ptr<Umbra::IWidget> Widget;
    iris::ReconcileWidget(Widget, Iris::IrisComponent(nullptr), Old, Mount);
    const int ChildId = dynamic_cast<MockWidget*>(Widget->GetChildAt(0))->Id;

    Iris::IrisProps ChildProps;
    ChildProps["text"] = Iris::IrisPropValue{std::string("updated")};
    std::vector<Iris::IrisComponent> NewChildren;
    NewChildren.push_back(MakeNode(Iris::IrisElementTag::Text, ChildProps));
    const auto New = MakeNode(Iris::IrisElementTag::Frame, {}, std::move(NewChildren));

    iris::ReconcileWidget(Widget, Old, New, Mount);
    Expect(Widget->GetChildCount() == 1, "still exactly one child");
    const auto* AsMockChild = dynamic_cast<MockWidget*>(Widget->GetChildAt(0));
    Expect(AsMockChild->Id == ChildId, "the child widget's identity is preserved across the parent's own reconcile");
    Expect(AsMockChild->Text == "updated", "and the child's own prop diff was applied");
}

void TestReconcileChildrenPreservesIdentityAcrossReorderByKey() {
    int         MountCount = 0;
    TestMounter Mount(&MountCount);

    std::vector<Iris::IrisComponent> OldList;
    OldList.push_back(MakeNode(Iris::IrisElementTag::Frame, {}, {}, Iris::IrisPropValue(1)));
    OldList.push_back(MakeNode(Iris::IrisElementTag::Frame, {}, {}, Iris::IrisPropValue(2)));

    std::vector<std::unique_ptr<Umbra::IWidget>> Widgets;
    iris::ReconcileChildren(Widgets, {}, OldList, Mount);
    Expect(Widgets.size() == 2, "both list items mounted");
    const int Id1 = dynamic_cast<MockWidget*>(Widgets[0].get())->Id; // key 1
    const int Id2 = dynamic_cast<MockWidget*>(Widgets[1].get())->Id; // key 2

    // Reordered: key 2 now comes first.
    std::vector<Iris::IrisComponent> NewList;
    NewList.push_back(MakeNode(Iris::IrisElementTag::Frame, {}, {}, Iris::IrisPropValue(2)));
    NewList.push_back(MakeNode(Iris::IrisElementTag::Frame, {}, {}, Iris::IrisPropValue(1)));

    iris::ReconcileChildren(Widgets, OldList, NewList, Mount);
    Expect(Widgets.size() == 2, "still two widgets after reordering");
    Expect(dynamic_cast<MockWidget*>(Widgets[0].get())->Id == Id2,
           "the key-2 widget object is reused in its new (first) position");
    Expect(dynamic_cast<MockWidget*>(Widgets[1].get())->Id == Id1,
           "the key-1 widget object is reused in its new (second) position");
}

void TestReconcileChildrenUnmountsRemovedAndMountsAdded() {
    int         MountCount = 0;
    TestMounter Mount(&MountCount);

    std::vector<Iris::IrisComponent> OldList;
    OldList.push_back(MakeNode(Iris::IrisElementTag::Frame, {}, {}, Iris::IrisPropValue(1)));
    OldList.push_back(MakeNode(Iris::IrisElementTag::Frame, {}, {}, Iris::IrisPropValue(2)));

    std::vector<std::unique_ptr<Umbra::IWidget>> Widgets;
    iris::ReconcileChildren(Widgets, {}, OldList, Mount);
    const int RemovedId = dynamic_cast<MockWidget*>(Widgets[1].get())->Id; // key 2 will be removed

    std::vector<Iris::IrisComponent> NewList;
    NewList.push_back(MakeNode(Iris::IrisElementTag::Frame, {}, {}, Iris::IrisPropValue(1)));
    NewList.push_back(MakeNode(Iris::IrisElementTag::Frame, {}, {}, Iris::IrisPropValue(3))); // new key

    iris::ReconcileChildren(Widgets, OldList, NewList, Mount);
    Expect(Widgets.size() == 2, "still two widgets — one removed, one added");
    Expect(AliveWidgetIds.count(RemovedId) == 0, "the widget for the removed key was actually unmounted");
}

} // namespace

void RunReconcilerTests() {
    TestComputePropDiffOnlyIncludesChangedFields();
    TestComputePropDiffEventPropsAlwaysIncludedWhenPresent();
    TestReconcileWidgetMountsFreshWhenNoWidgetExists();
    TestReconcileWidgetUpdatesInPlaceOnSameTagAndKey();
    TestReconcileWidgetRemountsOnTagMismatch();
    TestReconcileWidgetNoneUnmountsAndMountsNothing();
    TestReconcileWidgetRecursesIntoChildren();
    TestReconcileChildrenPreservesIdentityAcrossReorderByKey();
    TestReconcileChildrenUnmountsRemovedAndMountsAdded();
}
