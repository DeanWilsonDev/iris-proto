#include "cimmerian/test.hpp"

#include "Iris/ComponentInstance.h"

#include <functional>

DESCRIBE("ComponentInstance", {
    // The exact shape of the bug docs/iris_signal_lifetime_decision.md fixes: a function
    // that declares a signal, captures it by reference into a closure, and returns —
    // mirroring a component function's `render { }` producing a <Slot> callable. Before
    // the fix, calling the returned closure after this function returns was confirmed
    // dangling-reference UB via AddressSanitizer. IRIS_SIGNAL/MountComponentInstance make
    // this the same pattern every real component uses.
    IT("a signal survives the declaring function's return", {
        std::function<int()> Getter;
        Iris::IrisComponent  Node = iris::MountComponentInstance([&]() -> Iris::IrisComponent {
            IRIS_SIGNAL(int, Count, 42);
            Getter = [&]() { return Count.get(); }; // captured [&], exactly like every spec example
            return Iris::IrisComponent(nullptr);
        });
        // Node (and its Instance) are still in scope here, but the *declaring lambda* that
        // ran IRIS_SIGNAL has already returned — its own stack frame is gone. Before the
        // fix, Count's storage would have lived in that frame. Now it lives on
        // Node.Instance's heap-allocated ComponentInstance.
        ASSERT_EQUAL(Getter(), 42); // the signal is readable after the declaring lambda returns — no dangling reference
    });

    IT("the ComponentInstance is freed when its IrisComponent is dropped", {
        std::function<int()> Getter;
        auto                  DestroyedFlag = std::make_shared<bool>(false);
        {
            Iris::IrisComponent Node = iris::MountComponentInstance([&]() -> Iris::IrisComponent {
                IRIS_SIGNAL(std::shared_ptr<bool>, Flag, DestroyedFlag);
                Getter = [&]() { return static_cast<int>(Flag.get().use_count()); };
                return Iris::IrisComponent(nullptr);
            });
            REQUIRE_TRUE(Node.Instance != nullptr); // MountComponentInstance populates IrisComponent::Instance
            // DestroyedFlag is held by: the local variable here, the Signal's own copy
            // inside the ComponentInstance, and Getter's lambda closure over Flag (which is
            // a reference, not a copy) -- so refcount should be 2 (local + Signal's copy).
            ASSERT_EQUAL(Getter(), 2); // the signal's value (a copy of the shared_ptr) is alive on the heap
        } // Node goes out of scope here -- its Instance shared_ptr drops to zero, freeing
          // the ComponentInstance and every Signal it owns.
        ASSERT_EQUAL(static_cast<int>(DestroyedFlag.use_count()), 1);
        // once the IrisComponent (and its Instance) is dropped, the ComponentInstance and its
        // Signal are freed too -- refcount back down to just the local DestroyedFlag
    });

    IT("multiple signals in one component all survive independently", {
        std::function<int()> GetA;
        std::function<int()> GetB;
        Iris::IrisComponent  Node = iris::MountComponentInstance([&]() -> Iris::IrisComponent {
            IRIS_SIGNAL(int, A, 1);
            IRIS_SIGNAL(int, B, 2);
            GetA = [&]() { return A.get(); };
            GetB = [&]() { return B.get(); };
            return Iris::IrisComponent(nullptr);
        });
        (void)Node;
        ASSERT_TRUE(GetA() == 1 && GetB() == 2);
    });

    IT("set() and get() both work after the component function has returned", {
        std::function<void(int)> SetIt;
        std::function<int()>      GetIt;
        Iris::IrisComponent       Node = iris::MountComponentInstance([&]() -> Iris::IrisComponent {
            IRIS_SIGNAL(int, Count, 0);
            SetIt = [&](int V) { Count.set(V); };
            GetIt = [&]() { return Count.get(); };
            return Iris::IrisComponent(nullptr);
        });
        (void)Node;
        SetIt(99);
        ASSERT_EQUAL(GetIt(), 99);
    });
});
