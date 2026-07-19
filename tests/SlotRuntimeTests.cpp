#include "Iris/Signal.h"
#include "Iris/SlotRuntime.h"

#include <cstdio>
#include <memory>
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

// A minimal Umbra::IWidget stub — these tests exercise Signal/SlotState/IrisRuntime's
// own bookkeeping, not the reconciler's diff logic (see ReconcilerTests.cpp for that),
// so it only needs to satisfy the interface, not do anything interesting with it.
class StubWidget : public Umbra::IWidget {
public:
    void ApplyPropDiff(const Umbra::IrisPropDiff& Diff) override {
        ++ApplyPropDiffCallCount;
        if (Diff.ClassName) {
            LastClassName = *Diff.ClassName;
        }
    }
    std::size_t     GetChildCount() const override { return 0; }
    Umbra::IWidget* GetChildAt(std::size_t) const override { return nullptr; }
    void            InsertChildAt(std::size_t, std::unique_ptr<Umbra::IWidget>) override {}
    std::unique_ptr<Umbra::IWidget> RemoveChildAt(std::size_t) override { return nullptr; }

    int         ApplyPropDiffCallCount{0};
    std::string LastClassName;
};

iris::MountFn StubMount(int* MountCount) {
    return [MountCount](const Iris::IrisComponent& Node) -> std::unique_ptr<Umbra::IWidget> {
        if (MountCount != nullptr) {
            ++*MountCount;
        }
        auto Widget = std::make_unique<StubWidget>();
        const auto It = Node.Props.find("class");
        if (It != Node.Props.end()) {
            if (const auto* Str = std::get_if<std::string>(&It->second)) {
                Widget->LastClassName = *Str;
            }
        }
        return Widget;
    };
}

Iris::IrisComponent MakeFrame(const std::string& ClassName) {
    Iris::IrisProps Props;
    Props["class"] = Iris::IrisPropValue{ClassName};
    return Iris::IrisComponent(Iris::IrisElementTag::Frame, Props, {}, nullptr);
}

void TestSignalSupportsCopyInitializationSyntax() {
    // docs/iris_core_spec.md §2.2 writes `iris::Signal<bool> settingsOpen = false;` —
    // copy-initialization, not `Signal<bool> settingsOpen(false);`. An `explicit`
    // constructor would reject this outright; caught once by an end-to-end test
    // compiling real generated .iris output using this exact syntax.
    iris::Signal<bool> Flag = false;
    Expect(!Flag.get(), "iris::Signal<T> supports the spec's own copy-initialization syntax");
}

void TestReconcileMountsOnFirstCallOnly() {
    int         MountCount = 0;
    iris::MountFn Mount = StubMount(&MountCount);

    auto Callable = Iris::MakeSlotCallable([]() -> Iris::IrisComponent { return MakeFrame("a"); });
    iris::SlotState Slot(Callable, Mount);

    Slot.Reconcile();
    Expect(MountCount == 1, "the first Reconcile() call mounts");
    Slot.Reconcile();
    Expect(MountCount == 1, "a second Reconcile() call with no dirty flag is a no-op — no remount");
}

void TestSignalGetDuringSlotInvocationRegistersDependency() {
    int           MountCount = 0;
    iris::MountFn Mount = StubMount(&MountCount);

    iris::Signal<std::string> ClassName("initial");
    int                        InvocationCount = 0;
    auto                       Callable = Iris::MakeSlotCallable([&]() -> Iris::IrisComponent {
        ++InvocationCount;
        return MakeFrame(ClassName.get());
    });
    iris::SlotState Slot(Callable, Mount);

    Slot.Reconcile();
    Expect(MountCount == 1 && InvocationCount == 1, "initial mount invoked the callable once");

    Slot.Reconcile();
    Expect(InvocationCount == 1,
           "a Reconcile() call with nothing dirty doesn't re-invoke the callable at all — the "
           "control case proving the next assertion isn't a fluke");

    // Reading ClassName.get() during the Reconcile() call above should have registered
    // Slot as a dependent, via ambient active-slot tracking — so set() here must mark
    // it dirty even though nothing explicitly subscribed to anything.
    ClassName.set("changed");
    Expect(InvocationCount == 1, "Signal::set() alone never reconciles synchronously");

    Slot.Reconcile();
    Expect(InvocationCount == 2,
           "but the next Reconcile() call DOES re-invoke the callable — proving the earlier "
           ".get() during Reconcile() registered a real dependency, not just coincidence");
}

void TestTickReconcilesDirtySlotsAutomatically() {
    int           MountCount = 0;
    iris::MountFn Mount = StubMount(&MountCount);

    iris::Signal<std::string> ClassName("initial");
    int                        InvocationCount = 0;
    auto                       Callable = Iris::MakeSlotCallable([&]() -> Iris::IrisComponent {
        ++InvocationCount;
        return MakeFrame(ClassName.get());
    });
    iris::SlotState Slot(Callable, Mount);
    Slot.Reconcile(); // mount
    Expect(InvocationCount == 1, "the initial mount invoked the callable once");

    ClassName.set("changed");
    Expect(InvocationCount == 1, "Signal::set() alone never reconciles synchronously, even though it marked "
                                  "the slot dirty (registered with IrisRuntime, not run yet)");

    iris::Tick();
    Expect(InvocationCount == 2, "iris::Tick() reconciles the dirty slot automatically — no explicit "
                                  "Slot.Reconcile() call needed, matching how a host's own frame loop drives it");

    iris::Tick();
    Expect(InvocationCount == 2, "a second Tick() with nothing newly dirty doesn't re-invoke the callable");
}

// docs/iris_stage3_decision_doc.md §6: several Signal::set() calls inside one batch
// (e.g. one event-handler invocation setting more than one signal) should only
// reconcile once, at the batch's end — not once per set() call.
void TestEventBatchCollapsesMultipleSetsIntoOneReconcile() {
    int           MountCount = 0;
    iris::MountFn Mount = StubMount(&MountCount);

    iris::Signal<int> Counter(0);
    int                InvocationCount = 0;
    auto               Callable = Iris::MakeSlotCallable([&]() -> Iris::IrisComponent {
        ++InvocationCount;
        return MakeFrame(std::to_string(Counter.get()));
    });
    iris::SlotState Slot(Callable, Mount);
    Slot.Reconcile(); // mount
    const int InvocationsAfterMount = InvocationCount;

    {
        iris::ScopedEventBatch Batch;
        Counter.set(1);
        Counter.set(2);
        Counter.set(3);
        Expect(InvocationCount == InvocationsAfterMount,
               "no reconciliation happens for any set() call while the batch is still open");
    }
    // ScopedEventBatch's destructor ran IrisRuntime::EndBatch(), which reconciles every
    // slot the runtime knows is dirty — Slot was registered dirty exactly once
    // (IrisRuntime::RegisterDirtySlot dedupes), regardless of how many times it was
    // marked dirty inside the batch.
    Expect(InvocationCount == InvocationsAfterMount + 1,
           "closing the batch reconciles exactly once, despite three set() calls inside it");
}

} // namespace

void RunSlotRuntimeTests() {
    TestSignalSupportsCopyInitializationSyntax();
    TestReconcileMountsOnFirstCallOnly();
    TestSignalGetDuringSlotInvocationRegistersDependency();
    TestTickReconcilesDirtySlotsAutomatically();
    TestEventBatchCollapsesMultipleSetsIntoOneReconcile();
}
