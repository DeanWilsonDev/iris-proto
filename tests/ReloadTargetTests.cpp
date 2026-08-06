#include "cimmerian/test.hpp"

#include "Iris/ReloadTarget.h"

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <variant>

namespace {

// Minimal Umbra::IWidget, same shape as ReconcilerTests.cpp's own MockWidget (kept
// separate rather than shared: each is internal-linkage to its own translation unit,
// and this file only needs a fraction of what ReconcilerTests.cpp's version tracks).
std::set<int> AliveWidgetIds;
int           NextWidgetId = 0;

class MockWidget : public Umbra::IWidget {
public:
    explicit MockWidget(std::string Tag) : Tag(std::move(Tag)), Id(NextWidgetId++) { AliveWidgetIds.insert(Id); }
    ~MockWidget() override { AliveWidgetIds.erase(Id); }

    void ApplyPropDiff(const Umbra::IrisPropDiff&) override {}

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
    int                                            Id;
    std::vector<std::unique_ptr<Umbra::IWidget>> Children;
};

Iris::Component MakeNode(Iris::IrisElementTag Tag, std::optional<Iris::IrisPropValue> Key = std::nullopt) {
    Iris::Component Node(Tag, {}, {}, nullptr);
    Node.Key = std::move(Key);
    return Node;
}

class TestMounter {
public:
    explicit TestMounter(int* MountCount) : MountCount_(MountCount) {}

    std::unique_ptr<Umbra::IWidget> operator()(const Iris::Component& Node) {
        if (MountCount_ != nullptr) {
            ++*MountCount_;
        }
        auto Widget = std::make_unique<MockWidget>("Widget");
        for (const Iris::Component& Child : Node.Children) {
            Widget->Children.push_back((*this)(Child));
        }
        return Widget;
    }

private:
    int* MountCount_;
};

} // namespace

// docs/iris_hot_reload_reconciliation_decision.md §2: ReloadTarget owns the one thing
// ReconcileWidget needs but nothing at whole-app scope previously retained -- an owning
// root widget plus the exact prior Component tree it was built from.
DESCRIBE("ReloadTarget", {
    IT("Reconcile updates the retained root widget in place on a matched tag+key", {
        int         MountCount = 0;
        TestMounter Mount(&MountCount);

        Iris::Component Root      = MakeNode(Iris::IrisElementTag::Frame, Iris::IrisPropValue(1));
        auto             Widget    = Mount(Root);
        auto*            RawWidget = Widget.get();

        iris::ReloadTarget Target(std::move(Widget), Root);
        const int OriginalMountCount = MountCount;

        Iris::Component NewRoot = MakeNode(Iris::IrisElementTag::Frame, Iris::IrisPropValue(1));
        Target.Reconcile(NewRoot, Mount);

        ASSERT_EQUAL(MountCount, OriginalMountCount); // same tag+key: no remount, Mount not called again
        ASSERT_TRUE(Target.RootWidget() == RawWidget); // same widget object, updated in place
    });

    IT("Reconcile falls through to a fresh mount on a tag mismatch -- tier 3, no new code needed", {
        int         MountCount = 0;
        TestMounter Mount(&MountCount);

        Iris::Component Root   = MakeNode(Iris::IrisElementTag::Frame);
        auto             Widget = Mount(Root);

        iris::ReloadTarget Target(std::move(Widget), Root);
        const int OriginalMountCount = MountCount;

        Iris::Component NewRoot = MakeNode(Iris::IrisElementTag::Text); // structurally irreconcilable
        Target.Reconcile(NewRoot, Mount);

        ASSERT_EQUAL(MountCount, OriginalMountCount + 1); // exactly one fresh mount
        const auto* AsMock = dynamic_cast<MockWidget*>(Target.RootWidget());
        REQUIRE_TRUE(AsMock != nullptr);
        ASSERT_EQUAL(AsMock->Tag, "Widget"); // the freshly mounted widget, not the old one reused
    });

    IT("Reconcile retains New as PreviousTree() for the next call", {
        int         MountCount = 0;
        TestMounter Mount(&MountCount);

        Iris::Component Root   = MakeNode(Iris::IrisElementTag::Frame, Iris::IrisPropValue(1));
        auto             Widget = Mount(Root);

        iris::ReloadTarget Target(std::move(Widget), Root);

        Iris::Component Second = MakeNode(Iris::IrisElementTag::Frame, Iris::IrisPropValue(2));
        Target.Reconcile(Second, Mount);

        ASSERT_TRUE(Target.PreviousTree().Tag == Iris::IrisElementTag::Frame);
        REQUIRE_TRUE(Target.PreviousTree().Key.has_value());
        ASSERT_EQUAL(std::get<int>(*Target.PreviousTree().Key), 2); // Second's key, not Root's
    });

    IT("IrisRuntime::RegisterReloadTarget/GetReloadTarget round-trip", {
        int         MountCount = 0;
        TestMounter Mount(&MountCount);
        Iris::Component Root      = MakeNode(Iris::IrisElementTag::Frame);
        auto             Widget    = Mount(Root);
        auto*            RawWidget = Widget.get();

        iris::RegisterReloadTarget(std::make_unique<iris::ReloadTarget>(std::move(Widget), Root));
        REQUIRE_TRUE(iris::GetReloadTarget() != nullptr);
        ASSERT_TRUE(iris::GetReloadTarget()->RootWidget() == RawWidget);
    });
});
