#include "cimmerian/test.hpp"

#include "Iris/IrisNyxDriver.h"
#include "Iris/Reconciler.h"
#include "Iris/SlotResolution.h"

#include "host/marshal.hpp"
#include "host/type-builder.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>

namespace {

using namespace Iris;

// Real on-disk fixture -- IrisNyxDriver::LoadDocument genuinely reads files and compiles
// them via Driver.h's CompileFile, so this exercises the whole `.irisx` -> `.iris.ir` ->
// Component pipeline end to end, not a hand-built IrisIrDocument.
class TempProject {
public:
    TempProject() {
        Root_ = std::filesystem::temp_directory_path() / "iris_nyx_driver_test";
        std::filesystem::remove_all(Root_);
        std::filesystem::create_directories(Root_ / "demo");
    }

    ~TempProject() { std::filesystem::remove_all(Root_); }

    std::string Write(const std::string& Name, std::string_view Source) {
        const std::filesystem::path Path = Root_ / "demo" / Name;
        std::ofstream(Path) << Source;
        return Path.string();
    }

    std::string RootPath() const { return Root_.string(); }

private:
    std::filesystem::path Root_;
};

IrisConfig UmbraConfig() {
    IrisConfig Config;
    Config.Target      = IrisBuildTarget::UmbraEngine;
    Config.SearchPaths  = {"demo"};
    return Config;
}

// A minimal Umbra::IWidget stub -- same shape as SlotRuntimeTests.cpp's/
// IrisNyxEvaluatorTests.cpp's own StubWidget, used here only as a registered <Native>
// builder's return value.
class StubWidget : public Umbra::IWidget {
public:
    void            ApplyPropDiff(const Umbra::IrisPropDiff&) override {}
    std::size_t     GetChildCount() const override { return 0; }
    Umbra::IWidget* GetChildAt(std::size_t) const override { return nullptr; }
    void            InsertChildAt(std::size_t, std::unique_ptr<Umbra::IWidget>) override {}
    std::unique_ptr<Umbra::IWidget> RemoveChildAt(std::size_t) override { return nullptr; }
};

// A real (non-stub) widget tree, mirroring SlotResolutionTests.cpp's own MockWidget/
// TestMounter -- needed only by the reconcile-path @signal/<Slot> regression test below,
// which (unlike every other test in this file) must go through iris::ResolveSlots/
// SlotState::Reconcile()/iris::Tick() instead of calling a <Slot>'s raw Callable() by
// hand, to actually exercise the read-tracking path a real click goes through
// (docs/next-steps.md's "@signal never reconciles on write" gap).
class MockWidget : public Umbra::IWidget {
public:
    explicit MockWidget(std::string Tag) : Tag(std::move(Tag)) {}

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

    std::string                                  Tag;
    std::string                                  ClassName;
    std::string                                  Text;
    std::vector<std::unique_ptr<Umbra::IWidget>> Children;
};

// A stand-in host engine type -- not a real Pharos/Umbra type, just something to prove a
// HostObject Value survives being passed as a component-invocation prop
// (`<Card node={...} />`), the exact shape Pharos's own Explorer migration needs
// (docs/next-steps.md's "component-invocation props silently drop HostObject/Array/
// Object values" entry) for `<ExplorerRowNode node={item} />` inside a `.Map()` callback.
struct TestNode {
    std::string name;
    std::string Name() const { return name; }
};

class TestMounter {
public:
    std::unique_ptr<Umbra::IWidget> operator()(const Component& Node) {
        auto Widget = std::make_unique<MockWidget>(TagName(Node.Tag));
        Widget->ApplyPropDiff(iris::ComputePropDiff({}, Node.Props));
        for (const Component& Child : Node.Children) {
            if (Child.Tag == IrisElementTag::None || Child.Tag == IrisElementTag::Slot) {
                continue; // matches BuildWidgetTree's own real behavior for these two
            }
            Widget->Children.push_back((*this)(Child));
        }
        return Widget;
    }

private:
    static std::string TagName(IrisElementTag Tag) {
        switch (Tag) {
            case IrisElementTag::Frame:
                return "Frame";
            case IrisElementTag::Text:
                return "Text";
            default:
                return "?";
        }
    }
};

} // namespace

DESCRIBE("IrisNyxDriver", {
    IT("mounts a single-file component with no imports", {
        TempProject Project;
        const std::string AppPath = Project.Write("App.irisx",
                                                    "void App() {\n"
                                                    "    render {\n"
                                                    "        <Frame class=\"root\">\n"
                                                    "            <Text>Hello</Text>\n"
                                                    "        </Frame>\n"
                                                    "    }\n"
                                                    "}\n");

        IrisNyxDriver Driver(UmbraConfig(), Project.RootPath());
        const Component Root = Driver.MountRoot(AppPath, "App");

        REQUIRE_TRUE(Driver.Errors().empty());
        ASSERT_TRUE(std::get<std::string>(Root.Props.at("class")) == "root");
        REQUIRE_EQUAL(Root.Children.size(), static_cast<std::size_t>(1));
        ASSERT_TRUE(std::get<std::string>(Root.Children[0].Props.at("text")) == "Hello");
    });

    // docs/next-steps.md's "GetFileScope caches a stale snapshot of Runtime_.Globals() forever"
    // gap: a host's RegisterFunction call made *after* a file's whole-file NyxScope is already
    // cached (i.e. after a first MountRoot for that path) was silently invisible to any
    // subsequent MountRoot for that same path -- GetFileScope's cache-hit branch just returned
    // the already-cached Interpreter untouched, which only ever saw the globals that existed at
    // CreateScope time. This is exactly pharos-proto's own "register additional things up front"
    // convention (switchLens() rebuilding its NativeNodeRegistry and re-registering
    // "ExplorerTopLevelNodes" with a closure over the new registry before every MountRoot call on
    // the same, shared, process-lifetime driver) -- reproduced here with a plain string-returning
    // function instead of a node registry, since the underlying mechanism (a second
    // RegisterFunction call under the same name, expected to be visible on the next MountRoot) is
    // identical either way.
    IT("a RegisterFunction call made after a file's scope is already cached becomes visible on a "
       "subsequent MountRoot for the same path, replacing an earlier-registered function of the "
       "same name",
       {
           TempProject Project;
           const std::string AppPath = Project.Write("App.irisx",
                                                       "void App() {\n"
                                                       "    render {\n"
                                                       "        <Frame class={GetLabel()} />\n"
                                                       "    }\n"
                                                       "}\n");

           IrisNyxDriver Driver(UmbraConfig(), Project.RootPath());
           Driver.Runtime().RegisterFunction(
               "GetLabel", [](std::vector<nyx::runtime::Value>) -> nyx::runtime::Value {
                   return nyx::runtime::Value(std::string("first"));
               });

           const Component First = Driver.MountRoot(AppPath, "App");
           REQUIRE_TRUE(Driver.Errors().empty());
           ASSERT_TRUE(std::get<std::string>(First.Props.at("class")) == "first");

           // Re-register "GetLabel" with fresh data before a second MountRoot call for the same
           // path -- this is the exact moment GetFileScope's cache-hit branch used to hand back
           // an Interpreter that had never seen this new registration.
           Driver.Runtime().RegisterFunction(
               "GetLabel", [](std::vector<nyx::runtime::Value>) -> nyx::runtime::Value {
                   return nyx::runtime::Value(std::string("second"));
               });

           const Component Second = Driver.MountRoot(AppPath, "App");
           REQUIRE_TRUE(Driver.Errors().empty());
           ASSERT_TRUE(std::get<std::string>(Second.Props.at("class")) == "second");
       });

    IT("reports an error rather than crashing when the entry file doesn't exist", {
        TempProject Project;
        IrisNyxDriver Driver(UmbraConfig(), Project.RootPath());
        const Component Root = Driver.MountRoot(Project.RootPath() + "/demo/Missing.irisx", "Missing");

        ASSERT_TRUE(Root.Tag == Iris::IrisElementTag::None);
        ASSERT_FALSE(Driver.Errors().empty());
    });

    IT("resolves a cross-file component invocation via the caller's own import list", {
        TempProject Project;
        Project.Write("Card.irisx",
                       "void Card(CardProps props) {\n"
                       "    render {\n"
                       "        <Frame class={props.label} />\n"
                       "    }\n"
                       "}\n");
        const std::string ParentPath = Project.Write("Parent.irisx",
                                                       "import Card\n"
                                                       "\n"
                                                       "void Parent() {\n"
                                                       "    render {\n"
                                                       "        <Frame class=\"parent\">\n"
                                                       "            <Card label=\"hi\" />\n"
                                                       "        </Frame>\n"
                                                       "    }\n"
                                                       "}\n");

        IrisNyxDriver Driver(UmbraConfig(), Project.RootPath());
        const Component Root = Driver.MountRoot(ParentPath, "Parent");

        REQUIRE_TRUE(Driver.Errors().empty());
        ASSERT_TRUE(std::get<std::string>(Root.Props.at("class")) == "parent");
        REQUIRE_EQUAL(Root.Children.size(), static_cast<std::size_t>(1));
        const Component& CardResult = Root.Children[0];
        ASSERT_TRUE(std::get<std::string>(CardResult.Props.at("class")) == "hi");
        ASSERT_TRUE(CardResult.Instance != nullptr); // mounted via iris::MountComponentInstance, not a bare Component
    });

    IT("a HostObject prop passed to a child component invocation survives intact, not just "
       "primitive props (docs/next-steps.md's component-invocation-prop-marshaling gap)", {
        TempProject Project;
        Project.Write("Card.irisx",
                       "void Card(CardProps props) {\n"
                       "    render {\n"
                       "        <Frame class={props.node.Name()} />\n"
                       "    }\n"
                       "}\n");
        const std::string ParentPath = Project.Write("Parent.irisx",
                                                       "import Card\n"
                                                       "\n"
                                                       "void Parent(TestNode node) {\n"
                                                       "    render {\n"
                                                       "        <Frame class=\"parent\">\n"
                                                       "            <Card node={node} />\n"
                                                       "        </Frame>\n"
                                                       "    }\n"
                                                       "}\n");

        IrisNyxDriver Driver(UmbraConfig(), Project.RootPath());
        Driver.Runtime().RegisterType<TestNode>("TestNode").Method("Name", &TestNode::Name);
        const auto* Descriptor =
            std::get<std::shared_ptr<nyx::runtime::HostObject>>(Driver.Runtime().Globals().at("TestNode").data)
                ->descriptor;

        TestNode Node;
        Node.name = "vulkan_context.cpp";
        const Component Root =
            Driver.MountRoot(ParentPath, "Parent", {nyx::host::ToValue(&Node, Descriptor)});

        REQUIRE_TRUE(Driver.Errors().empty());
        ASSERT_TRUE(std::get<std::string>(Root.Props.at("class")) == "parent");
        REQUIRE_EQUAL(Root.Children.size(), static_cast<std::size_t>(1));
        // Before this fix, EvaluateComponentInvocation's prop loop routed every prop through
        // ValueToPropValue (bool/int/float/string only), so `node={node}` silently dropped the
        // HostObject -- Card.irisx's own `props.node` would have been Null, and
        // `props.node.Name()` would have thrown rather than resolving to "vulkan_context.cpp".
        ASSERT_TRUE(std::get<std::string>(Root.Children[0].Props.at("class")) == "vulkan_context.cpp");
    });

    IT("two invocations of the same imported component keep independent @signal state", {
        TempProject Project;
        Project.Write("Toggle.irisx",
                       "void Toggle(ToggleProps props) {\n"
                       "    @signal bool isOn = props.startOn;\n"
                       "\n"
                       "    render {\n"
                       "        <Frame onPress={() -> { isOn = true; }}>\n"
                       "            <Slot>\n"
                       "                !{() -> isOn ? <Frame class=\"on\" /> : <Frame class=\"off\" />}\n"
                       "            </Slot>\n"
                       "        </Frame>\n"
                       "    }\n"
                       "}\n");
        const std::string ParentPath = Project.Write("TwoToggles.irisx",
                                                       "import Toggle\n"
                                                       "\n"
                                                       "void TwoToggles() {\n"
                                                       "    render {\n"
                                                       "        <Frame>\n"
                                                       "            <Toggle key=\"a\" startOn={false} />\n"
                                                       "            <Toggle key=\"b\" startOn={false} />\n"
                                                       "        </Frame>\n"
                                                       "    }\n"
                                                       "}\n");

        IrisNyxDriver Driver(UmbraConfig(), Project.RootPath());
        const Component Root = Driver.MountRoot(ParentPath, "TwoToggles");

        REQUIRE_TRUE(Driver.Errors().empty());
        REQUIRE_EQUAL(Root.Children.size(), static_cast<std::size_t>(2));

        // Fire the first Toggle's onPress; the second must stay untouched -- proof each
        // <Toggle/> invocation got its own independent NyxScope/ComponentInstance, not a
        // shared one.
        std::get<std::function<void()>>(Root.Children[0].Props.at("onPress"))();

        auto SlotOutput = [](const Component& Toggle) -> std::vector<Component> {
            const Component& Slot = Toggle.Children.at(0);
            return std::get<std::function<std::vector<Component>()>>(Slot.SlotCallable->Callable)();
        };

        ASSERT_TRUE(std::get<std::string>(SlotOutput(Root.Children[0])[0].Props.at("class")) == "on");
        ASSERT_TRUE(std::get<std::string>(SlotOutput(Root.Children[1])[0].Props.at("class")) == "off");
    });

    IT("a real .irisx <Slot> reconciles on a @signal write reached through iris::Tick() "
       "(docs/next-steps.md's read-tracking gap, nyx-scripting-language/decision-log.md §6.8) -- "
       "not by calling the Slot's raw Callable() directly, the blind spot that let the original "
       "bug through every existing test here",
       {
        TempProject Project;
        const std::string AppPath = Project.Write("Counter.irisx",
                                                    "void Counter() {\n"
                                                    "    @signal int count = 0;\n"
                                                    "\n"
                                                    "    render {\n"
                                                    "        <Frame onPress={() -> { count = count + 1; }}>\n"
                                                    "            <Slot>\n"
                                                    "                !{() -> count == 0\n"
                                                    "                    ? <Frame class=\"zero\" />\n"
                                                    "                    : <Frame class=\"nonzero\" />}\n"
                                                    "            </Slot>\n"
                                                    "        </Frame>\n"
                                                    "    }\n"
                                                    "}\n");

        IrisNyxDriver Driver(UmbraConfig(), Project.RootPath());
        const Component RootNode = Driver.MountRoot(AppPath, "Counter");
        REQUIRE_TRUE(Driver.Errors().empty());

        iris::MountFn                   Mount = TestMounter();
        std::unique_ptr<Umbra::IWidget> Root  = Mount(RootNode);
        auto                             Slots = iris::ResolveSlots(*Root, RootNode, Mount);
        REQUIRE_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(1));
        ASSERT_TRUE(dynamic_cast<MockWidget*>(Root->GetChildAt(0))->ClassName == "zero");

        // The exact same onPress firing the "independent @signal state" test above already
        // exercises -- proven to reach SetSignal/NotifySignalDependents. What's new here is
        // observing the result through the real Reconcile()/Tick() path a live app actually
        // uses, instead of re-invoking the Slot's Callable() by hand: that's what the original
        // bug report (pharos-proto's real click test) needed and no prior test here provided.
        std::get<std::function<void()>>(RootNode.Props.at("onPress"))();
        iris::Tick();

        REQUIRE_EQUAL(Root->GetChildCount(), static_cast<std::size_t>(1));
        ASSERT_TRUE(dynamic_cast<MockWidget*>(Root->GetChildAt(0))->ClassName == "nonzero");
    });

    IT("reports an error for a component invocation with no matching import", {
        TempProject Project;
        const std::string ParentPath = Project.Write("Orphan.irisx",
                                                       "void Orphan() {\n"
                                                       "    render {\n"
                                                       "        <Mystery />\n"
                                                       "    }\n"
                                                       "}\n");

        IrisNyxDriver Driver(UmbraConfig(), Project.RootPath());
        Driver.MountRoot(ParentPath, "Orphan");

        ASSERT_FALSE(Driver.Errors().empty());
    });

    // nyx-scripting-language/decision-log.md §9.2 / docs/archive/iris_nyx_slot_loop_and_reload_gap_resolved.md
    // §2: ReloadRoot re-renders a free-function component via Runtime_.ReInvokeComponent,
    // preserving @signal state by name and reusing the same ComponentInstance -- exercised end
    // to end against a real file rewritten on disk between the two calls, not a mock.
    IT("ReloadRoot preserves @signal state across a render-body-only change (tier Unchanged)", {
        TempProject Project;
        auto Source = [](const std::string& ZeroClass, const std::string& NonZeroClass) {
            return "void Counter() {\n"
                   "    @signal int count = 0;\n"
                   "\n"
                   "    render {\n"
                   "        <Frame onPress={() -> { count = count + 1; }}>\n"
                   "            <Slot>\n"
                   "                !{() -> count == 0 ? <Frame class=\"" +
                   ZeroClass + "\" /> : <Frame class=\"" + NonZeroClass + "\" />}\n"
                   "            </Slot>\n"
                   "        </Frame>\n"
                   "    }\n"
                   "}\n";
        };
        const std::string EntryPath = Project.Write("Counter.irisx", Source("zero", "nonzero"));

        IrisNyxDriver Driver(UmbraConfig(), Project.RootPath());
        const Component Root = Driver.MountRoot(EntryPath, "Counter");
        REQUIRE_TRUE(Driver.Errors().empty());

        // Bump count to 1 before reloading -- proves the reload preserves this, not just the
        // InitExpr's own default.
        std::get<std::function<void()>>(Root.Props.at("onPress"))();

        // Render-body-only change: swap which class name each branch uses, @signal untouched.
        Project.Write("Counter.irisx", Source("empty", "has-count"));

        const IrisNyxReloadResult Reloaded = Driver.ReloadRoot(EntryPath, "Counter", {}, Root);
        REQUIRE_TRUE(Driver.Errors().empty());
        ASSERT_TRUE(Reloaded.Tier == iris::ComponentReloadTier::Unchanged);
        ASSERT_TRUE(Reloaded.Root.Instance == Root.Instance); // same ComponentInstance reused, not a fresh mount

        const Component& Slot = Reloaded.Root.Children.at(0);
        const std::vector<Component> SlotOutput =
            std::get<std::function<std::vector<Component>()>>(Slot.SlotCallable->Callable)();
        REQUIRE_EQUAL(SlotOutput.size(), static_cast<std::size_t>(1));
        // count is still 1 (not reset to 0) *and* the new render body's own class name is used.
        ASSERT_TRUE(std::get<std::string>(SlotOutput[0].Props.at("class")) == "has-count");
    });

    IT("ReloadRoot reports tier SignalLayoutChanged when a @signal is added", {
        TempProject Project;
        const std::string EntryPath = Project.Write("Counter.irisx",
                                                      "void Counter() {\n"
                                                      "    @signal int count = 0;\n"
                                                      "    render { <Frame class=\"v1\" /> }\n"
                                                      "}\n");

        IrisNyxDriver Driver(UmbraConfig(), Project.RootPath());
        const Component Root = Driver.MountRoot(EntryPath, "Counter");
        REQUIRE_TRUE(Driver.Errors().empty());

        Project.Write("Counter.irisx",
                       "void Counter() {\n"
                       "    @signal int count = 0;\n"
                       "    @signal bool extra = false;\n"
                       "    render { <Frame class=\"v2\" /> }\n"
                       "}\n");

        const IrisNyxReloadResult Reloaded = Driver.ReloadRoot(EntryPath, "Counter", {}, Root);
        REQUIRE_TRUE(Driver.Errors().empty());
        ASSERT_TRUE(Reloaded.Tier == iris::ComponentReloadTier::SignalLayoutChanged);
        ASSERT_TRUE(std::get<std::string>(Reloaded.Root.Props.at("class")) == "v2");
    });

    IT("ReloadRoot falls back to a fresh mount when PreviousRoot has no reusable Instance", {
        TempProject Project;
        const std::string EntryPath =
            Project.Write("Counter.irisx", "void Counter() {\n    render { <Frame class=\"v1\" /> }\n}\n");

        IrisNyxDriver Driver(UmbraConfig(), Project.RootPath());
        const Component Unmounted{nullptr}; // no Instance at all -- nothing to reload against

        const IrisNyxReloadResult Reloaded = Driver.ReloadRoot(EntryPath, "Counter", {}, Unmounted);
        REQUIRE_TRUE(Driver.Errors().empty());
        ASSERT_TRUE(std::get<std::string>(Reloaded.Root.Props.at("class")) == "v1");
        ASSERT_TRUE(Reloaded.Root.Instance != nullptr); // still a real mount, just a fresh one
    });

    // Model 2 -- class-based components (nyx-scripting-language/decision-log.md §9.2,
    // §5.16 constructors): a class extending Component is instantiated via
    // Interpreter::Instantiate (running its constructor), and its render{} block's expressions
    // are evaluated against a scope binding both the Render method's own declared parameter and
    // implicit `this.field` reads -- exercised end to end against real nyx-proto execution.
    IT("mounts a class-based (Model 2) component, running its constructor and binding Render's own parameter", {
        TempProject Project;
        const std::string EntryPath = Project.Write("Toggle.irisx",
                                                      "class Toggle : Component {\n"
                                                      "public:\n"
                                                      "    @signal bool isOn = false;\n"
                                                      "\n"
                                                      "    Toggle(bool startOn) {\n"
                                                      "        isOn = startOn;\n"
                                                      "    }\n"
                                                      "\n"
                                                      "    void Render(bool label) {\n"
                                                      "        render {\n"
                                                      "            <Frame class={label ? \"true-label\" : \"false-label\"}>\n"
                                                      "                <Slot>\n"
                                                      "                    !{() -> isOn ? <Frame class=\"on\" /> : <Frame class=\"off\" />}\n"
                                                      "                </Slot>\n"
                                                      "            </Frame>\n"
                                                      "        }\n"
                                                      "    }\n"
                                                      "}\n");

        IrisNyxDriver Driver(UmbraConfig(), Project.RootPath());
        const Component Root = Driver.MountRoot(EntryPath, "Toggle", {nyx::runtime::Value(true)});

        REQUIRE_TRUE(Driver.Errors().empty());
        REQUIRE_TRUE(Root.Instance != nullptr); // mounted via iris::MountComponentInstance
        ASSERT_TRUE(std::get<std::string>(Root.Props.at("class")) == "true-label"); // Render's own param

        REQUIRE_EQUAL(Root.Children.size(), static_cast<std::size_t>(1));
        const std::vector<Component> SlotOutput =
            std::get<std::function<std::vector<Component>()>>(Root.Children[0].SlotCallable->Callable)();
        REQUIRE_EQUAL(SlotOutput.size(), static_cast<std::size_t>(1));
        ASSERT_TRUE(std::get<std::string>(SlotOutput[0].Props.at("class")) == "on"); // constructor-set @signal field
    });

    IT("ReloadRoot preserves a class component's @signal field across a render-body-only change (tier Unchanged)", {
        TempProject Project;
        auto Source = [](const std::string& OnClass, const std::string& OffClass) {
            return "class Toggle : Component {\n"
                   "public:\n"
                   "    @signal bool isOn = false;\n"
                   "\n"
                   "    Toggle(bool startOn) {\n"
                   "        isOn = startOn;\n"
                   "    }\n"
                   "\n"
                   "    void Render(bool label) {\n"
                   "        render {\n"
                   "            <Slot>\n"
                   "                !{() -> isOn ? <Frame class=\"" +
                   OnClass + "\" /> : <Frame class=\"" + OffClass +
                   "\" />}\n"
                   "            </Slot>\n"
                   "        }\n"
                   "    }\n"
                   "}\n";
        };
        const std::string EntryPath = Project.Write("Toggle.irisx", Source("on", "off"));

        IrisNyxDriver Driver(UmbraConfig(), Project.RootPath());
        const Component Root = Driver.MountRoot(EntryPath, "Toggle", {nyx::runtime::Value(true)});
        REQUIRE_TRUE(Driver.Errors().empty());

        // Render-body-only change: swap which class name each branch uses -- no field change.
        Project.Write("Toggle.irisx", Source("still-on", "still-off"));

        const IrisNyxReloadResult Reloaded = Driver.ReloadRoot(EntryPath, "Toggle", {nyx::runtime::Value(false)}, Root);
        REQUIRE_TRUE(Driver.Errors().empty());
        ASSERT_TRUE(Reloaded.Tier == iris::ComponentReloadTier::Unchanged);
        ASSERT_TRUE(Reloaded.Root.Instance == Root.Instance); // same ComponentInstance reused

        const std::vector<Component> SlotOutput =
            std::get<std::function<std::vector<Component>()>>(Reloaded.Root.SlotCallable->Callable)();
        REQUIRE_EQUAL(SlotOutput.size(), static_cast<std::size_t>(1));
        // isOn is still true (the constructor never re-runs on reload, and the field was never
        // written to false) even though this ReloadRoot call's own Render arg is false -- proof
        // the @signal field, not the Render parameter, drove this branch.
        ASSERT_TRUE(std::get<std::string>(SlotOutput[0].Props.at("class")) == "still-on");
    });

    IT("ReloadRoot reports tier SignalLayoutChanged for a class component when a @signal field is added", {
        TempProject Project;
        const std::string EntryPath = Project.Write("Toggle.irisx",
                                                      "class Toggle : Component {\n"
                                                      "public:\n"
                                                      "    @signal bool isOn = false;\n"
                                                      "\n"
                                                      "    Toggle(bool startOn) {\n"
                                                      "        isOn = startOn;\n"
                                                      "    }\n"
                                                      "\n"
                                                      "    void Render(bool label) {\n"
                                                      "        render { <Frame class=\"v1\" /> }\n"
                                                      "    }\n"
                                                      "}\n");

        IrisNyxDriver Driver(UmbraConfig(), Project.RootPath());
        const Component Root = Driver.MountRoot(EntryPath, "Toggle", {nyx::runtime::Value(true)});
        REQUIRE_TRUE(Driver.Errors().empty());

        Project.Write("Toggle.irisx",
                       "class Toggle : Component {\n"
                       "public:\n"
                       "    @signal bool isOn = false;\n"
                       "    @signal int extra;\n"
                       "\n"
                       "    Toggle(bool startOn) {\n"
                       "        isOn = startOn;\n"
                       "    }\n"
                       "\n"
                       "    void Render(bool label) {\n"
                       "        render { <Frame class=\"v2\" /> }\n"
                       "    }\n"
                       "}\n");

        const IrisNyxReloadResult Reloaded = Driver.ReloadRoot(EntryPath, "Toggle", {nyx::runtime::Value(true)}, Root);
        REQUIRE_TRUE(Driver.Errors().empty());
        ASSERT_TRUE(Reloaded.Tier == iris::ComponentReloadTier::SignalLayoutChanged);
        ASSERT_TRUE(std::get<std::string>(Reloaded.Root.Props.at("class")) == "v2");
    });

    IT("mounts a <Native> element via a builder registered with RegisterNativeBuilder", {
        TempProject Project;
        const std::string AppPath = Project.Write("App.irisx",
                                                    "void App() {\n"
                                                    "    render {\n"
                                                    "        <Native build={() -> \"stub\"} />\n"
                                                    "    }\n"
                                                    "}\n");

        IrisNyxDriver Driver(UmbraConfig(), Project.RootPath());
        Driver.RegisterNativeBuilder("stub", []() -> std::unique_ptr<Umbra::IWidget> {
            return std::make_unique<StubWidget>();
        });

        const Component Root = Driver.MountRoot(AppPath, "App");
        REQUIRE_TRUE(Driver.Errors().empty());
        ASSERT_TRUE(Root.Tag == IrisElementTag::Native);
        REQUIRE_TRUE(Root.NativeBuilder != nullptr);
        ASSERT_TRUE(Root.NativeBuilder->Build() != nullptr);
    });

    // docs/next-steps.md's "<Native> builders can't receive the invoking element's own props"
    // entry, resolved: pharos-proto's own Explorer panel motivating case -- a single registered
    // `"BuildTreeRow"` factory receiving the invoking <Native>'s own `node`/`depth` props
    // (a HostObject included, not just primitive props, the exact reason pharos-proto's
    // `registerRowBuilders()` previously had to pre-register one uniquely-named builder per
    // tree node instead) via the new props-aware `RegisterNativeBuilder` overload.
    IT("<Native>'s own props, HostObject included, reach a builder registered with "
       "RegisterNativeBuilder's props-aware overload",
       {
        TempProject Project;
        const std::string AppPath = Project.Write(
            "App.irisx", "void App(TestNode node) {\n"
                          "    render {\n"
                          "        <Native build={() -> \"BuildTreeRow\"} node={node} depth={2} />\n"
                          "    }\n"
                          "}\n");

        IrisNyxDriver Driver(UmbraConfig(), Project.RootPath());
        Driver.Runtime().RegisterType<TestNode>("TestNode").Method("Name", &TestNode::Name);
        const auto* Descriptor =
            std::get<std::shared_ptr<nyx::runtime::HostObject>>(Driver.Runtime().Globals().at("TestNode").data)
                ->descriptor;

        bool FactoryCalled = false;
        Driver.RegisterNativeBuilder(
            "BuildTreeRow", [&FactoryCalled](const nyx::runtime::Value& Props) -> std::unique_ptr<Umbra::IWidget> {
                FactoryCalled = true;
                ASSERT_TRUE(Props.Kind() == nyx::runtime::ValueKind::Object);
                const auto*                 Obj        = std::get<std::shared_ptr<nyx::runtime::NyxObject>>(Props.data).get();
                const nyx::runtime::Value*  NodeField  = Obj->FindFieldByName("node");
                const nyx::runtime::Value*  DepthField = Obj->FindFieldByName("depth");
                ASSERT_TRUE(NodeField != nullptr);
                ASSERT_TRUE(DepthField != nullptr);
                if (NodeField == nullptr || DepthField == nullptr) {
                    return std::make_unique<StubWidget>();
                }
                ASSERT_TRUE(NodeField->Kind() == nyx::runtime::ValueKind::HostObject);
                // The same marshaled-pointer shape EvaluateComponentInvocation's own HostObject
                // regression test above proves for a component-invocation prop -- here read back
                // by hand (no Nyx-side `.Name()` method call), the way a real C++ native-builder
                // factory (e.g. pharos-proto's BuildTreeRow) would.
                auto* HostNode = static_cast<TestNode*>(
                    std::get<std::shared_ptr<nyx::runtime::HostObject>>(NodeField->data)->instance);
                ASSERT_TRUE(HostNode != nullptr);
                if (HostNode == nullptr) {
                    return std::make_unique<StubWidget>();
                }
                const std::string Label = HostNode->name + ":" + std::to_string(std::get<int32_t>(DepthField->data));
                return std::make_unique<MockWidget>(Label);
            });

        TestNode Node;
        Node.name = "vulkan_context.cpp";
        const Component Root = Driver.MountRoot(AppPath, "App", {nyx::host::ToValue(&Node, Descriptor)});

        REQUIRE_TRUE(Driver.Errors().empty());
        REQUIRE_TRUE(Root.NativeBuilder != nullptr);
        // Lazy, exactly like the zero-argument overload above -- not called until Build() runs.
        ASSERT_FALSE(FactoryCalled);
        std::unique_ptr<Umbra::IWidget> Built = Root.NativeBuilder->Build();
        ASSERT_TRUE(FactoryCalled);
        REQUIRE_TRUE(Built != nullptr);
        ASSERT_TRUE(dynamic_cast<MockWidget*>(Built.get())->Tag == "vulkan_context.cpp:2");
    });

    // docs/next-steps.md's narrowed nested-reload gap: a statically-nested (non-<Slot>) cross-
    // file component invocation now reloads too, matched via Component::InvocationTag, instead
    // of always mounting fresh -- proven here by firing Header's own onPress before reloading
    // App with an unrelated render-body-only change, then checking Header's own @signal-derived
    // class survived (would reset to "zero" on a fresh mount) and its ComponentInstance identity
    // is unchanged (would be a different Instance on a fresh mount).
    IT("ReloadRoot reloads a statically-nested cross-file component invocation, preserving its own @signal state", {
        TempProject Project;
        Project.Write("Header.irisx",
                       "void Header(HeaderProps props) {\n"
                       "    @signal int count = 0;\n"
                       "\n"
                       "    render {\n"
                       "        <Frame onPress={() -> { count = count + 1; }}"
                       " class={count == 0 ? \"zero\" : \"nonzero\"} />\n"
                       "    }\n"
                       "}\n");
        auto AppSource = [](const std::string& RootClass) {
            return "import Header\n"
                   "\n"
                   "void App() {\n"
                   "    render {\n"
                   "        <Frame class=\"" +
                   RootClass +
                   "\">\n"
                   "            <Header />\n"
                   "        </Frame>\n"
                   "    }\n"
                   "}\n";
        };
        const std::string AppPath = Project.Write("App.irisx", AppSource("app"));

        IrisNyxDriver Driver(UmbraConfig(), Project.RootPath());
        const Component Root = Driver.MountRoot(AppPath, "App");
        REQUIRE_TRUE(Driver.Errors().empty());
        REQUIRE_EQUAL(Root.Children.size(), static_cast<std::size_t>(1));
        REQUIRE_TRUE(Root.Children[0].Instance != nullptr);

        // Bump Header's own count to 1 before reloading.
        std::get<std::function<void()>>(Root.Children[0].Props.at("onPress"))();

        // Render-body-only change to App's own root class -- Header.irisx itself is untouched.
        Project.Write("App.irisx", AppSource("app-v2"));

        const IrisNyxReloadResult Reloaded = Driver.ReloadRoot(AppPath, "App", {}, Root);
        REQUIRE_TRUE(Driver.Errors().empty());
        ASSERT_TRUE(std::get<std::string>(Reloaded.Root.Props.at("class")) == "app-v2");
        REQUIRE_EQUAL(Reloaded.Root.Children.size(), static_cast<std::size_t>(1));
        ASSERT_TRUE(Reloaded.Root.Children[0].Instance == Root.Children[0].Instance);
        ASSERT_TRUE(std::get<std::string>(Reloaded.Root.Children[0].Props.at("class")) == "nonzero");
    });

    // The remaining, documented boundary (IrisNyxDriver.h's own ReloadRoot doc comment): a
    // component invocation reached only through a <Slot> is invisible to the nested-invocation
    // collector (a <Slot>'s own dynamic output is never stored in Component::Children), so it
    // still mounts fresh on every reload -- this test documents that this is unchanged, not a
    // regression this feature was expected to close.
    IT("a <Slot>-mediated nested component invocation still mounts fresh on reload", {
        TempProject Project;
        Project.Write("Header.irisx",
                       "void Header(HeaderProps props) {\n"
                       "    @signal int count = 0;\n"
                       "\n"
                       "    render {\n"
                       "        <Frame onPress={() -> { count = count + 1; }}"
                       " class={count == 0 ? \"zero\" : \"nonzero\"} />\n"
                       "    }\n"
                       "}\n");
        const std::string AppPath = Project.Write("App.irisx",
                                                    "import Header\n"
                                                    "\n"
                                                    "void App() {\n"
                                                    "    render {\n"
                                                    "        <Frame class=\"app\">\n"
                                                    "            <Slot>\n"
                                                    "                !{() -> <Header />}\n"
                                                    "            </Slot>\n"
                                                    "        </Frame>\n"
                                                    "    }\n"
                                                    "}\n");

        IrisNyxDriver Driver(UmbraConfig(), Project.RootPath());
        const Component Root = Driver.MountRoot(AppPath, "App");
        REQUIRE_TRUE(Driver.Errors().empty());

        auto SlotOutput = [](const Component& Root) -> Component {
            const Component& Slot = Root.Children.at(0);
            std::vector<Component> Out =
                std::get<std::function<std::vector<Component>()>>(Slot.SlotCallable->Callable)();
            return std::move(Out.at(0));
        };

        const Component Header = SlotOutput(Root);
        std::get<std::function<void()>>(Header.Props.at("onPress"))();

        const IrisNyxReloadResult Reloaded = Driver.ReloadRoot(AppPath, "App", {}, Root);
        REQUIRE_TRUE(Driver.Errors().empty());

        const Component ReloadedHeader = SlotOutput(Reloaded.Root);
        ASSERT_TRUE(ReloadedHeader.Instance != Header.Instance); // fresh mount, not the same instance
        ASSERT_TRUE(std::get<std::string>(ReloadedHeader.Props.at("class")) == "zero"); // count reset
    });

    // docs/next-steps.md's "A `.irisx` component can't register its own per-frame OnTick" entry:
    // a Model 1 (free-function) component declaring a reserved-name local lambda (`auto OnTick =
    // (float dt) -> { ... };`) gets a real `iris::ComponentInstance::Lifecycle` wired
    // automatically, callable by whoever drives a real frame loop (a `penumbra-ui-backend`
    // `UmbraLifecycleBridge`, in the real consuming pipeline -- called directly here, since this
    // driver is deliberately backend-agnostic and never drives one itself).
    IT("a Model 1 component's `auto OnTick = ...` local is wired to a real Lifecycle, called via "
       "Instance->Lifecycle->OnTick",
       {
        TempProject Project;
        const std::string AppPath = Project.Write("Ticker.irisx",
                                                    "void Ticker() {\n"
                                                    "    @signal float elapsed = 0;\n"
                                                    "\n"
                                                    "    auto OnTick = (float dt) -> { elapsed = elapsed + dt; };\n"
                                                    "\n"
                                                    "    render {\n"
                                                    "        <Frame>\n"
                                                    "            <Slot>\n"
                                                    "                !{() -> elapsed > 0.4\n"
                                                    "                    ? <Frame class=\"ticked\" />\n"
                                                    "                    : <Frame class=\"idle\" />}\n"
                                                    "            </Slot>\n"
                                                    "        </Frame>\n"
                                                    "    }\n"
                                                    "}\n");

        IrisNyxDriver Driver(UmbraConfig(), Project.RootPath());
        const Component Root = Driver.MountRoot(AppPath, "Ticker");
        REQUIRE_TRUE(Driver.Errors().empty());
        REQUIRE_TRUE(Root.Instance != nullptr);
        REQUIRE_TRUE(Root.Instance->Lifecycle != nullptr);

        auto SlotClass = [&]() {
            std::vector<Component> Out =
                std::get<std::function<std::vector<Component>()>>(Root.Children.at(0).SlotCallable->Callable)();
            return std::get<std::string>(Out.at(0).Props.at("class"));
        };
        ASSERT_TRUE(SlotClass() == "idle");

        Root.Instance->Lifecycle->OnTick(Umbra::TickInfo{0.5f});
        ASSERT_TRUE(SlotClass() == "ticked");
    });

    IT("a Model 1 component's `auto OnMount`/`auto OnUnmount` locals are wired to the same "
       "Lifecycle, called via Instance->Lifecycle->OnMount/OnUnmount",
       {
        TempProject Project;
        const std::string AppPath = Project.Write("Watcher.irisx",
                                                    "void Watcher() {\n"
                                                    "    @signal bool mounted = false;\n"
                                                    "    @signal bool unmounted = false;\n"
                                                    "\n"
                                                    "    auto OnMount = () -> { mounted = true; };\n"
                                                    "    auto OnUnmount = () -> { unmounted = true; };\n"
                                                    "\n"
                                                    "    render {\n"
                                                    "        <Frame>\n"
                                                    "            <Slot>\n"
                                                    "                !{() -> !mounted\n"
                                                    "                    ? <Frame class=\"neither\" />\n"
                                                    "                    : (!unmounted ? <Frame class=\"mounted-only\" /> : <Frame class=\"both\" />)}\n"
                                                    "            </Slot>\n"
                                                    "        </Frame>\n"
                                                    "    }\n"
                                                    "}\n");

        IrisNyxDriver Driver(UmbraConfig(), Project.RootPath());
        const Component Root = Driver.MountRoot(AppPath, "Watcher");
        REQUIRE_TRUE(Driver.Errors().empty());
        REQUIRE_TRUE(Root.Instance != nullptr);
        REQUIRE_TRUE(Root.Instance->Lifecycle != nullptr);

        auto SlotClass = [&]() {
            std::vector<Component> Out =
                std::get<std::function<std::vector<Component>()>>(Root.Children.at(0).SlotCallable->Callable)();
            return std::get<std::string>(Out.at(0).Props.at("class"));
        };
        ASSERT_TRUE(SlotClass() == "neither");

        Root.Instance->Lifecycle->OnMount();
        ASSERT_TRUE(SlotClass() == "mounted-only");

        Root.Instance->Lifecycle->OnUnmount();
        ASSERT_TRUE(SlotClass() == "both");
    });

    IT("a component declaring none of OnMount/OnUnmount/OnTick leaves Instance->Lifecycle null "
       "(no per-frame cost for the common case)",
       {
        TempProject Project;
        const std::string AppPath = Project.Write("Plain.irisx",
                                                    "void Plain() {\n"
                                                    "    render {\n"
                                                    "        <Frame class=\"plain\" />\n"
                                                    "    }\n"
                                                    "}\n");

        IrisNyxDriver Driver(UmbraConfig(), Project.RootPath());
        const Component Root = Driver.MountRoot(AppPath, "Plain");
        REQUIRE_TRUE(Driver.Errors().empty());
        REQUIRE_TRUE(Root.Instance != nullptr);
        ASSERT_TRUE(Root.Instance->Lifecycle == nullptr);
    });

    // Model 2 (class-based) counterpart of the Model 1 OnTick test above -- `void OnTick(float
    // dt) { ... }` declared as an ordinary instance method, dispatched via
    // `Interpreter::TryCallInstanceMethod`.
    IT("a Model 2 (class-based) component's `OnTick(float)` method is wired to a real Lifecycle",
       {
        TempProject Project;
        const std::string AppPath = Project.Write("Ticker.irisx",
                                                    "class Ticker : Component {\n"
                                                    "public:\n"
                                                    "    @signal float elapsed = 0;\n"
                                                    "\n"
                                                    "    void OnTick(float dt) {\n"
                                                    "        elapsed = elapsed + dt;\n"
                                                    "    }\n"
                                                    "\n"
                                                    "    void Render() {\n"
                                                    "        render {\n"
                                                    "            <Frame>\n"
                                                    "                <Slot>\n"
                                                    "                    !{() -> elapsed > 0.4\n"
                                                    "                        ? <Frame class=\"ticked\" />\n"
                                                    "                        : <Frame class=\"idle\" />}\n"
                                                    "                </Slot>\n"
                                                    "            </Frame>\n"
                                                    "        }\n"
                                                    "    }\n"
                                                    "}\n");

        IrisNyxDriver Driver(UmbraConfig(), Project.RootPath());
        const Component Root = Driver.MountRoot(AppPath, "Ticker");
        REQUIRE_TRUE(Driver.Errors().empty());
        REQUIRE_TRUE(Root.Instance != nullptr);
        REQUIRE_TRUE(Root.Instance->Lifecycle != nullptr);

        auto SlotClass = [&]() {
            std::vector<Component> Out =
                std::get<std::function<std::vector<Component>()>>(Root.Children.at(0).SlotCallable->Callable)();
            return std::get<std::string>(Out.at(0).Props.at("class"));
        };
        ASSERT_TRUE(SlotClass() == "idle");

        Root.Instance->Lifecycle->OnTick(Umbra::TickInfo{0.5f});
        ASSERT_TRUE(SlotClass() == "ticked");
    });

    // A class declaring no OnMount/OnUnmount/OnTick method gets the same null-Lifecycle
    // backward-compatibility guarantee as the Model 1 case above.
    IT("a Model 2 component declaring no lifecycle methods leaves Instance->Lifecycle null", {
        TempProject Project;
        const std::string AppPath = Project.Write("Plain.irisx",
                                                    "class Plain : Component {\n"
                                                    "public:\n"
                                                    "    void Render() {\n"
                                                    "        render {\n"
                                                    "            <Frame class=\"plain\" />\n"
                                                    "        }\n"
                                                    "    }\n"
                                                    "}\n");

        IrisNyxDriver Driver(UmbraConfig(), Project.RootPath());
        const Component Root = Driver.MountRoot(AppPath, "Plain");
        REQUIRE_TRUE(Driver.Errors().empty());
        REQUIRE_TRUE(Root.Instance != nullptr);
        ASSERT_TRUE(Root.Instance->Lifecycle == nullptr);
    });

    // Regression coverage for the "rebuilt fresh every reload" requirement
    // (BuildFreeFunctionLifecycleAdapter's own doc comment, IrisNyxDriver.cpp): a Model 1
    // reload's new RenderScope is a genuinely different NyxScope than the pre-reload one, so a
    // Lifecycle adapter left pointing at the old one would evaluate `OnTick(...)` against a
    // stale/dangling scope after a reload -- exactly the kind of bug AddressSanitizer is run
    // against this suite to catch (docs/next-steps.md's own verification convention).
    IT("Instance->Lifecycle keeps working, against the reloaded scope, after ReloadRoot", {
        TempProject Project;
        auto Source = [](const std::string& InitialClass) {
            return "void Ticker() {\n"
                   "    @signal float elapsed = 0;\n"
                   "\n"
                   "    auto OnTick = (float dt) -> { elapsed = elapsed + dt; };\n"
                   "\n"
                   "    render {\n"
                   "        <Frame>\n"
                   "            <Slot>\n"
                   "                !{() -> elapsed > 0.4\n"
                   "                    ? <Frame class=\"" + InitialClass + "-ticked\" />\n"
                   "                    : <Frame class=\"" + InitialClass + "-idle\" />}\n"
                   "            </Slot>\n"
                   "        </Frame>\n"
                   "    }\n"
                   "}\n";
        };
        const std::string AppPath = Project.Write("Ticker.irisx", Source("v1"));

        IrisNyxDriver Driver(UmbraConfig(), Project.RootPath());
        const Component Root = Driver.MountRoot(AppPath, "Ticker");
        REQUIRE_TRUE(Driver.Errors().empty());
        REQUIRE_TRUE(Root.Instance != nullptr);
        REQUIRE_TRUE(Root.Instance->Lifecycle != nullptr);

        Project.Write("Ticker.irisx", Source("v2")); // render-body-only change -- tier Unchanged
        const IrisNyxReloadResult Reloaded = Driver.ReloadRoot(AppPath, "Ticker", {}, Root);
        REQUIRE_TRUE(Driver.Errors().empty());
        REQUIRE_EQUAL(static_cast<int>(Reloaded.Tier), static_cast<int>(iris::ComponentReloadTier::Unchanged));
        REQUIRE_TRUE(Reloaded.Root.Instance == Root.Instance); // same ComponentInstance, reused
        REQUIRE_TRUE(Reloaded.Root.Instance->Lifecycle != nullptr);

        auto SlotClass = [&](const Component& RootNode) {
            std::vector<Component> Out =
                std::get<std::function<std::vector<Component>()>>(RootNode.Children.at(0).SlotCallable->Callable)();
            return std::get<std::string>(Out.at(0).Props.at("class"));
        };
        ASSERT_TRUE(SlotClass(Reloaded.Root) == "v2-idle");

        Reloaded.Root.Instance->Lifecycle->OnTick(Umbra::TickInfo{0.5f});
        ASSERT_TRUE(SlotClass(Reloaded.Root) == "v2-ticked");
    });
});
