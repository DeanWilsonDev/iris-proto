#include "cimmerian/test.hpp"

#include "Iris/ComponentInstance.h"

#include <functional>
#include <string>

namespace {

// A minimal concrete Umbra::IWidgetLifecycle for exercising iris::RegisterLifecycle --
// docs/next-steps.md's "iris::RegisterLifecycle" entry / docs/iris_stage3_decision_doc.md
// §8. Real registration against a backend's own frame loop (e.g. Penumbra's
// Application::RegisterLifecycle) is a consuming backend's job, not this repo's -- these
// tests only cover that the pointer reaches ComponentInstance::Lifecycle correctly.
struct RecordingLifecycle : Umbra::IWidgetLifecycle {
    int  MountCount{0};
    int  UnmountCount{0};
    int  TickCount{0};
    void OnMount() override { ++MountCount; }
    void OnUnmount() override { ++UnmountCount; }
    void OnTick(const Umbra::TickInfo&) override { ++TickCount; }
};

} // namespace

DESCRIBE("ComponentInstance", {
    // The exact shape of the bug docs/iris_signal_lifetime_decision.md fixes: a function
    // that declares a signal, captures it by reference into a closure, and returns —
    // mirroring a component function's `render { }` producing a <Slot> callable. Before
    // the fix, calling the returned closure after this function returns was confirmed
    // dangling-reference UB via AddressSanitizer. IRIS_SIGNAL/MountComponentInstance make
    // this the same pattern every real component uses.
    IT("a signal survives the declaring function's return", {
        std::function<int()> Getter;
        Iris::Component  Node = iris::MountComponentInstance([&]() -> Iris::Component {
            IRIS_SIGNAL(int, Count, 42);
            Getter = [&]() { return Count.get(); }; // captured [&], exactly like every spec example
            return Iris::Component(nullptr);
        });
        // Node (and its Instance) are still in scope here, but the *declaring lambda* that
        // ran IRIS_SIGNAL has already returned — its own stack frame is gone. Before the
        // fix, Count's storage would have lived in that frame. Now it lives on
        // Node.Instance's heap-allocated ComponentInstance.
        ASSERT_EQUAL(Getter(), 42); // the signal is readable after the declaring lambda returns — no dangling reference
    });

    IT("the ComponentInstance is freed when its Component is dropped", {
        std::function<int()> Getter;
        auto                  DestroyedFlag = std::make_shared<bool>(false);
        {
            Iris::Component Node = iris::MountComponentInstance([&]() -> Iris::Component {
                IRIS_SIGNAL(std::shared_ptr<bool>, Flag, DestroyedFlag);
                Getter = [&]() { return static_cast<int>(Flag.get().use_count()); };
                return Iris::Component(nullptr);
            });
            REQUIRE_TRUE(Node.Instance != nullptr); // MountComponentInstance populates Component::Instance
            // DestroyedFlag is held by: the local variable here, the Signal's own copy
            // inside the ComponentInstance, and Getter's lambda closure over Flag (which is
            // a reference, not a copy) -- so refcount should be 2 (local + Signal's copy).
            ASSERT_EQUAL(Getter(), 2); // the signal's value (a copy of the shared_ptr) is alive on the heap
        } // Node goes out of scope here -- its Instance shared_ptr drops to zero, freeing
          // the ComponentInstance and every Signal it owns.
        ASSERT_EQUAL(static_cast<int>(DestroyedFlag.use_count()), 1);
        // once the Component (and its Instance) is dropped, the ComponentInstance and its
        // Signal are freed too -- refcount back down to just the local DestroyedFlag
    });

    IT("multiple signals in one component all survive independently", {
        std::function<int()> GetA;
        std::function<int()> GetB;
        Iris::Component  Node = iris::MountComponentInstance([&]() -> Iris::Component {
            IRIS_SIGNAL(int, A, 1);
            IRIS_SIGNAL(int, B, 2);
            GetA = [&]() { return A.get(); };
            GetB = [&]() { return B.get(); };
            return Iris::Component(nullptr);
        });
        (void)Node;
        ASSERT_TRUE(GetA() == 1 && GetB() == 2);
    });

    IT("set() and get() both work after the component function has returned", {
        std::function<void(int)> SetIt;
        std::function<int()>      GetIt;
        Iris::Component       Node = iris::MountComponentInstance([&]() -> Iris::Component {
            IRIS_SIGNAL(int, Count, 0);
            SetIt = [&](int V) { Count.set(V); };
            GetIt = [&]() { return Count.get(); };
            return Iris::Component(nullptr);
        });
        (void)Node;
        SetIt(99);
        ASSERT_EQUAL(GetIt(), 99);
    });

    // docs/iris_hot_reload_reconciliation_decision.md §1: ReloadComponentInstance
    // replays a render body against an already-mounted ComponentInstance instead of
    // allocating a fresh one -- these tests cover the tier-1/tier-2 classification that
    // falls out of that replay.
    IT("a tier-1 replay preserves the signal's current value, not its InitExpr", {
        std::function<void(int)> SetIt;
        std::function<int()>      GetIt;
        Iris::Component Node = iris::MountComponentInstance([&]() -> Iris::Component {
            IRIS_SIGNAL(int, Count, 1);
            SetIt = [&](int V) { Count.set(V); };
            return Iris::Component(nullptr);
        });
        SetIt(99); // simulates runtime state accrued before the reload fires

        Iris::Component Reloaded = iris::ReloadComponentInstance(Node.Instance, [&]() -> Iris::Component {
            IRIS_SIGNAL(int, Count, 1); // same declaration -- InitExpr is ignored on replay
            GetIt = [&]() { return Count.get(); };
            return Iris::Component(nullptr);
        });

        REQUIRE_TRUE(Reloaded.ReloadTier.has_value());
        ASSERT_TRUE(*Reloaded.ReloadTier == iris::ComponentReloadTier::Unchanged);
        ASSERT_EQUAL(GetIt(), 99);                    // not reset to InitExpr's 1
        ASSERT_TRUE(Reloaded.Instance == Node.Instance); // same ComponentInstance carried forward
    });

    IT("a tier-1 replay keeps the same Signal object identity -- pre-reload closures stay valid", {
        std::function<int()> OldGetter;
        Iris::Component Node = iris::MountComponentInstance([&]() -> Iris::Component {
            IRIS_SIGNAL(int, Count, 1);
            OldGetter = [&]() { return Count.get(); }; // captured before any reload
            return Iris::Component(nullptr);
        });

        std::function<void(int)> NewSetter;
        Iris::Component Reloaded = iris::ReloadComponentInstance(Node.Instance, [&]() -> Iris::Component {
            IRIS_SIGNAL(int, Count, 1);
            NewSetter = [&](int V) { Count.set(V); };
            return Iris::Component(nullptr);
        });
        (void)Reloaded;

        NewSetter(7); // set via the NEW closure's reference
        ASSERT_EQUAL(OldGetter(), 7); // the OLD closure reads the same, updated storage
    });

    IT("a replay that declares a new signal is tier-2 (SignalLayoutChanged)", {
        Iris::Component Node = iris::MountComponentInstance([&]() -> Iris::Component {
            IRIS_SIGNAL(int, A, 1);
            return Iris::Component(nullptr);
        });

        std::function<int()> GetB;
        Iris::Component Reloaded = iris::ReloadComponentInstance(Node.Instance, [&]() -> Iris::Component {
            IRIS_SIGNAL(int, A, 1);
            IRIS_SIGNAL(int, B, 2); // new field this run
            GetB = [&]() { return B.get(); };
            return Iris::Component(nullptr);
        });

        REQUIRE_TRUE(Reloaded.ReloadTier.has_value());
        ASSERT_TRUE(*Reloaded.ReloadTier == iris::ComponentReloadTier::SignalLayoutChanged);
        ASSERT_EQUAL(GetB(), 2); // a new field initializes to its declared default
    });

    IT("a replay that declares fewer signals than before is tier-2 (SignalLayoutChanged)", {
        Iris::Component Node = iris::MountComponentInstance([&]() -> Iris::Component {
            IRIS_SIGNAL(int, A, 1);
            IRIS_SIGNAL(int, B, 2);
            return Iris::Component(nullptr);
        });

        Iris::Component Reloaded = iris::ReloadComponentInstance(Node.Instance, [&]() -> Iris::Component {
            IRIS_SIGNAL(int, A, 1); // B no longer declared this run -- a removed field
            return Iris::Component(nullptr);
        });

        REQUIRE_TRUE(Reloaded.ReloadTier.has_value());
        ASSERT_TRUE(*Reloaded.ReloadTier == iris::ComponentReloadTier::SignalLayoutChanged);
    });

    IT("a replay whose signal changed type at the same position is tier-2, with a fresh value", {
        Iris::Component Node = iris::MountComponentInstance([&]() -> Iris::Component {
            IRIS_SIGNAL(int, Count, 1);
            return Iris::Component(nullptr);
        });

        std::function<std::string()> GetIt;
        Iris::Component Reloaded = iris::ReloadComponentInstance(Node.Instance, [&]() -> Iris::Component {
            IRIS_SIGNAL(std::string, Count, "hello"); // same position, different T
            GetIt = [&]() { return Count.get(); };
            return Iris::Component(nullptr);
        });

        REQUIRE_TRUE(Reloaded.ReloadTier.has_value());
        ASSERT_TRUE(*Reloaded.ReloadTier == iris::ComponentReloadTier::SignalLayoutChanged);
        ASSERT_EQUAL(GetIt(), "hello"); // no compatible old value to carry forward -- uses InitExpr
    });

    // docs/next-steps.md's "iris::RegisterLifecycle" entry / docs/iris_stage3_decision_doc.md
    // §8: a component calls iris::RegisterLifecycle(...) with a single Umbra::IWidgetLifecycle*
    // argument, mirroring IRIS_SIGNAL's ambient-current-instance pattern -- these tests cover
    // that the pointer actually reaches ComponentInstance::Lifecycle, the passive/non-owning
    // "extension slot" a backend's own reconciler is expected to read.
    IT("ComponentInstance::Lifecycle is nullptr when RegisterLifecycle is never called", {
        Iris::Component Node = iris::MountComponentInstance([&]() -> Iris::Component {
            IRIS_SIGNAL(int, Count, 0); // an ordinary component that never registers a lifecycle
            return Iris::Component(nullptr);
        });
        REQUIRE_TRUE(Node.Instance != nullptr);
        ASSERT_TRUE(Node.Instance->Lifecycle == nullptr);
    });

    IT("iris::RegisterLifecycle stashes the pointer on the ambient ComponentInstance", {
        RecordingLifecycle Lifecycle;
        Iris::Component    Node = iris::MountComponentInstance([&]() -> Iris::Component {
            iris::RegisterLifecycle(&Lifecycle);
            return Iris::Component(nullptr);
        });
        REQUIRE_TRUE(Node.Instance != nullptr);
        ASSERT_TRUE(Node.Instance->Lifecycle == &Lifecycle);
    });

    IT("RegisterLifecycle is passive -- it never calls OnMount/OnUnmount/OnTick itself", {
        RecordingLifecycle Lifecycle;
        Iris::Component    Node = iris::MountComponentInstance([&]() -> Iris::Component {
            iris::RegisterLifecycle(&Lifecycle);
            return Iris::Component(nullptr);
        });
        (void)Node;
        // Registration alone must not drive the interface -- that's a backend's own frame
        // loop's job (e.g. Penumbra's Application::RegisterLifecycle fanning out OnTick),
        // which this repo deliberately never calls into (Iris stays backend-agnostic).
        ASSERT_EQUAL(Lifecycle.MountCount, 0);
        ASSERT_EQUAL(Lifecycle.UnmountCount, 0);
        ASSERT_EQUAL(Lifecycle.TickCount, 0);
    });

    IT("two sibling components each register their own independent Lifecycle pointer", {
        RecordingLifecycle LifecycleA;
        RecordingLifecycle LifecycleB;
        Iris::Component    NodeA = iris::MountComponentInstance([&]() -> Iris::Component {
            iris::RegisterLifecycle(&LifecycleA);
            return Iris::Component(nullptr);
        });
        Iris::Component NodeB = iris::MountComponentInstance([&]() -> Iris::Component {
            iris::RegisterLifecycle(&LifecycleB);
            return Iris::Component(nullptr);
        });
        REQUIRE_TRUE(NodeA.Instance != nullptr);
        REQUIRE_TRUE(NodeB.Instance != nullptr);
        ASSERT_TRUE(NodeA.Instance->Lifecycle == &LifecycleA);
        ASSERT_TRUE(NodeB.Instance->Lifecycle == &LifecycleB);
        ASSERT_TRUE(NodeA.Instance->Lifecycle != NodeB.Instance->Lifecycle);
    });

    IT("a reload replay re-registering a lifecycle overwrites the prior pointer on the same instance", {
        RecordingLifecycle Original;
        Iris::Component    Node = iris::MountComponentInstance([&]() -> Iris::Component {
            iris::RegisterLifecycle(&Original);
            return Iris::Component(nullptr);
        });
        REQUIRE_TRUE(Node.Instance->Lifecycle == &Original);

        RecordingLifecycle Replacement;
        Iris::Component    Reloaded = iris::ReloadComponentInstance(Node.Instance, [&]() -> Iris::Component {
            iris::RegisterLifecycle(&Replacement); // same render body re-running on reload
            return Iris::Component(nullptr);
        });

        ASSERT_TRUE(Reloaded.Instance == Node.Instance); // same ComponentInstance carried forward
        ASSERT_TRUE(Reloaded.Instance->Lifecycle == &Replacement); // the latest registration wins
    });

    IT("Lifecycle stays a raw, non-owning pointer -- ComponentInstance destruction never touches the pointee", {
        auto DestroyedFlag = std::make_shared<bool>(false);
        struct FlaggingLifecycle : Umbra::IWidgetLifecycle {
            std::shared_ptr<bool> Flag;
            ~FlaggingLifecycle() override { *Flag = true; }
        };
        FlaggingLifecycle Lifecycle;
        Lifecycle.Flag = DestroyedFlag;
        {
            Iris::Component Node = iris::MountComponentInstance([&]() -> Iris::Component {
                iris::RegisterLifecycle(&Lifecycle);
                return Iris::Component(nullptr);
            });
            REQUIRE_TRUE(Node.Instance->Lifecycle == &Lifecycle);
        } // Node's ComponentInstance is destroyed here -- Lifecycle is stack-owned by this
          // test, not the ComponentInstance, so its own destructor must not fire.
        ASSERT_TRUE(*DestroyedFlag == false);
    });
});
