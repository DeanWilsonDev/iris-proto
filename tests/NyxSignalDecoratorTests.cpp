#include "cimmerian/test.hpp"

#include "Iris/NyxSignalDecorator.h"

#include "Iris/ComponentInstance.h"
#include "Iris/SlotRuntime.h"

#include "host/marshal.hpp"
#include "host/nyx-runtime.hpp"

#include <memory>
#include <string>

namespace {

// Same minimal Umbra::IWidget stub SlotRuntimeTests.cpp uses -- these tests exercise
// ComponentInstance/@signal wiring, not the reconciler's diff logic.
class StubWidget : public Umbra::IWidget {
public:
    void ApplyPropDiff(const Umbra::IrisPropDiff&) override {}
    std::size_t     GetChildCount() const override { return 0; }
    Umbra::IWidget* GetChildAt(std::size_t) const override { return nullptr; }
    void            InsertChildAt(std::size_t, std::unique_ptr<Umbra::IWidget>) override {}
    std::unique_ptr<Umbra::IWidget> RemoveChildAt(std::size_t) override { return nullptr; }
};

iris::MountFn StubMount() {
    return [](const Iris::Component&) -> std::unique_ptr<Umbra::IWidget> { return std::make_unique<StubWidget>(); };
}

Iris::Component MakeFrame() { return Iris::Component(Iris::IrisElementTag::Frame, {}, {}, nullptr); }

} // namespace

DESCRIBE("NyxSignalDecorator", {
    // nyx-proto's docs/nyx-scripting-language/decision-log.md §6.7: @signal registers
    // signal storage on the ambient ComponentInstance at declaration time, then installs
    // an onWrite observer so every later Nyx-script assignment reaches SetSignal.
    IT("a @signal variable's write observer notifies a dependent <Slot>, and the value round-trips through Get/SetSignal", {
        nyx::host::NyxRuntime Runtime;
        iris::RegisterSignalDecorator(Runtime);

        int                        InvocationCount = 0;
        int32_t                    LastSeenValue = -1;
        std::unique_ptr<iris::SlotState> Slot;

        Iris::Component Node = iris::MountComponentInstance([&]() -> Iris::Component {
            iris::ComponentInstance* Instance = iris::IrisRuntime::Instance().CurrentComponentInstance();
            if (Instance == nullptr) {
                return Iris::Component(nullptr); // MountComponentInstance guarantees this never happens
            }

            // TrackDependency runs from inside the Nyx script, right after @signal's
            // declaration but before the reassignment below -- letting this test build a
            // real <Slot> whose callable reads signal 0 (the only signal this component
            // declares) via ComponentInstance::GetSignal, exactly like a future
            // Nyx-generated <Slot> callable would. The Slot's own callable, and this
            // RegisterFunction callback itself, both outlive this outer lambda's own call
            // frame (the test reconciles Slot again after MountComponentInstance returns
            // below) -- so Instance must be captured *by value* here, a plain pointer copy,
            // not [&] a reference to this frame's own local. The ComponentInstance object
            // it points to stays alive regardless, via Node.Instance below; only the local
            // pointer *variable* would dangle.
            Runtime.RegisterFunction("TrackDependency", [&InvocationCount, &LastSeenValue, &Slot, Instance](
                                                              std::vector<nyx::runtime::Value>) -> nyx::runtime::Value {
                auto Callable = Iris::MakeSlotCallable([&InvocationCount, &LastSeenValue, Instance]() -> Iris::Component {
                    ++InvocationCount;
                    LastSeenValue = nyx::host::FromValue<int32_t>(Instance->GetSignal(0));
                    return MakeFrame();
                });
                Slot = std::make_unique<iris::SlotState>(Callable, StubMount());
                Slot->Reconcile(); // mount: invokes the callable once, tracking the dependency
                return nyx::runtime::Value();
            });

            Runtime.Run(
                "class SignalTestApp : Application {\n"
                "public:\n"
                "    void Run() {\n"
                "        @signal int count = 0;\n"
                "        TrackDependency();\n"
                "        count = 5;\n"
                "    }\n"
                "}\n",
                "signal_test.nyx", "SignalTestApp"
            );
            return Iris::Component(nullptr);
        });
        (void)Node;

        REQUIRE_TRUE(Slot != nullptr);
        ASSERT_EQUAL(InvocationCount, 1); // mount only -- SetSignal marks dirty, never reconciles synchronously
        ASSERT_EQUAL(LastSeenValue, 0);   // the value read at mount time, before the script's reassignment

        Slot->Reconcile();
        ASSERT_EQUAL(InvocationCount, 2); // the earlier GetSignal(0) read registered a real dependency
        ASSERT_EQUAL(LastSeenValue, 5);   // ... and the new value flowed all the way through
    });

    IT("@signal used outside a mounted component is a no-op, not a crash", {
        nyx::host::NyxRuntime Runtime;
        iris::RegisterSignalDecorator(Runtime);

        int32_t CapturedValue = -1;
        Runtime.RegisterFunction("Capture", [&](std::vector<nyx::runtime::Value> Args) -> nyx::runtime::Value {
            CapturedValue = nyx::host::FromValue<int32_t>(Args[0]);
            return nyx::runtime::Value();
        });

        // Deliberately not wrapped in iris::MountComponentInstance -- no ambient
        // ComponentInstance exists while this script runs.
        ASSERT_TRUE(iris::IrisRuntime::Instance().CurrentComponentInstance() == nullptr);
        Runtime.Run(
            "class SignalTestApp : Application {\n"
            "public:\n"
            "    void Run() {\n"
            "        @signal int count = 0;\n"
            "        count = 7;\n"
            "        Capture(count);\n"
            "    }\n"
            "}\n",
            "signal_test.nyx", "SignalTestApp"
        );
        ASSERT_EQUAL(CapturedValue, 7); // the variable still behaves like an ordinary local
    });

    IT("signal storage outlives the Nyx component function call", {
        nyx::host::NyxRuntime Runtime;
        iris::RegisterSignalDecorator(Runtime);

        iris::ComponentInstance* Instance = nullptr;
        Iris::Component Node = iris::MountComponentInstance([&]() -> Iris::Component {
            Instance = iris::IrisRuntime::Instance().CurrentComponentInstance();
            Runtime.Run(
                "class SignalTestApp : Application {\n"
                "public:\n"
                "    void Run() {\n"
                "        @signal int count = 42;\n"
                "    }\n"
                "}\n",
                "signal_test.nyx", "SignalTestApp"
            );
            return Iris::Component(nullptr);
        });
        // Run() -- and the Nyx Interpreter that owned its Environment/call stack -- has
        // long since returned by this point; only Node.Instance (a shared_ptr) keeps the
        // ComponentInstance, and the signal storage it owns, alive.
        REQUIRE_TRUE(Node.Instance != nullptr);
        ASSERT_EQUAL(nyx::host::FromValue<int32_t>(Instance->GetSignal(0)), 42);
    });
});
