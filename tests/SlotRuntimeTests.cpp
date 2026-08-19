#include "cimmerian/test.hpp"

#include "Iris/Signal.h"
#include "Iris/SlotRuntime.h"

#include <csignal>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// fork()/waitpid()/pipe() -- POSIX-only, needed only by the death test below (see its own
// comment for why: cimmerian/test.hpp has no death-test primitive of its own, and this
// repo's own build has no Windows target/CI configured, so a POSIX-only test here matches
// the rest of this repo's own platform assumptions rather than introducing a new one).
#include <sys/wait.h>
#include <unistd.h>

namespace {

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

// A distinct widget object's raw address is not a reliable "was this actually a
// different object" signal on its own -- freeing one heap allocation and immediately
// making another can (and, observed empirically while writing the Native reconciliation
// tests below, does) land the new allocation at the exact same address as the one just
// freed. NativeMarkerWidget carries its own monotonically increasing Id instead, so
// "was the live widget actually swapped" can be asserted unambiguously.
int NextNativeMarkerId = 0;

class NativeMarkerWidget : public Umbra::IWidget {
public:
    NativeMarkerWidget() : Id(NextNativeMarkerId++) {}
    void            ApplyPropDiff(const Umbra::IrisPropDiff&) override {}
    std::size_t     GetChildCount() const override { return 0; }
    Umbra::IWidget* GetChildAt(std::size_t) const override { return nullptr; }
    void            InsertChildAt(std::size_t, std::unique_ptr<Umbra::IWidget>) override {}
    std::unique_ptr<Umbra::IWidget> RemoveChildAt(std::size_t) override { return nullptr; }

    int Id;
};

iris::MountFn StubMount(int* MountCount) {
    return [MountCount](const Iris::Component& Node) -> std::unique_ptr<Umbra::IWidget> {
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

Iris::Component MakeFrame(const std::string& ClassName) {
    Iris::IrisProps Props;
    Props["class"] = Iris::IrisPropValue{ClassName};
    return Iris::Component(Iris::IrisElementTag::Frame, Props, {}, nullptr);
}

// docs/next-steps.md's "<Native> doesn't participate in <Slot> reconciliation" entry
// (2026-08-17): the helpers below investigate whether a <Native> node sitting inside a
// re-rendering <Slot>'s subtree actually gets its Component::NativeBuilder re-invoked.
// A minimal Umbra::IWidget that also supports real InsertChildAt/RemoveChildAt/
// GetChildAt bookkeeping, needed for the "attached to a real parent" case (the actual
// pharos-proto splice shape: a <Native> spliced via a <Slot> attached under a static
// parent widget, not a standalone/detached SlotState) — StubWidget above deliberately
// doesn't track children at all, so it can't stand in for that case.
class ContainerWidget : public Umbra::IWidget {
public:
    void ApplyPropDiff(const Umbra::IrisPropDiff&) override {}
    std::size_t     GetChildCount() const override { return Children.size(); }
    Umbra::IWidget* GetChildAt(std::size_t Index) const override { return Children[Index].get(); }
    void            InsertChildAt(std::size_t Index, std::unique_ptr<Umbra::IWidget> Child) override {
        Children.insert(Children.begin() + static_cast<long>(Index), std::move(Child));
    }
    std::unique_ptr<Umbra::IWidget> RemoveChildAt(std::size_t Index) override {
        std::unique_ptr<Umbra::IWidget> Removed = std::move(Children[Index]);
        Children.erase(Children.begin() + static_cast<long>(Index));
        return Removed;
    }

    std::vector<std::unique_ptr<Umbra::IWidget>> Children;
};

// A real backend's MountFn (SlotRuntime.h's own doc comment: "Supplied by whoever
// embeds Iris, e.g. iris-penumbra-backend") is the only place Component::NativeBuilder
// is ever meant to be invoked from — mirrors that contract: whenever asked to mount a
// Native-tagged node, call its NativeBuilder->Build() directly (Native is always a
// leaf, so there's no further subtree to recurse into), counting how many times that
// genuinely happens. Anything else falls back to a plain container, unused by the
// Native-specific tests below.
iris::MountFn NativeAwareMount(int* BuildCount) {
    return [BuildCount](const Iris::Component& Node) -> std::unique_ptr<Umbra::IWidget> {
        if (Node.Tag == Iris::IrisElementTag::Native && Node.NativeBuilder) {
            if (BuildCount != nullptr) {
                ++*BuildCount;
            }
            return Node.NativeBuilder->Build();
        }
        return std::make_unique<ContainerWidget>();
    };
}

Iris::Component MakeNativeNode(std::optional<Iris::IrisPropValue> Key = std::nullopt) {
    std::shared_ptr<Iris::IrisNativeBuilder> Builder = Iris::MakeNativeBuilder(
        []() -> std::unique_ptr<Umbra::IWidget> { return std::make_unique<NativeMarkerWidget>(); });
    Iris::Component Node(Iris::IrisElementTag::Native, {}, {}, nullptr, Builder);
    Node.Key = std::move(Key);
    return Node;
}

} // namespace

DESCRIBE("SlotRuntime", {
    // docs/iris_core_spec.md §2.2 writes `iris::Signal<bool> settingsOpen = false;` —
    // copy-initialization, not `Signal<bool> settingsOpen(false);`. An `explicit`
    // constructor would reject this outright; caught once by an end-to-end test
    // compiling real generated .iris output using this exact syntax.
    IT("Signal supports copy-initialization syntax", {
        iris::Signal<bool> Flag = false;
        ASSERT_FALSE(Flag.get()); // iris::Signal<T> supports the spec's own copy-initialization syntax
    });

    IT("Reconcile mounts on the first call only", {
        int         MountCount = 0;
        iris::MountFn Mount = StubMount(&MountCount);

        auto Callable = Iris::MakeSlotCallable([]() -> Iris::Component { return MakeFrame("a"); });
        iris::SlotState Slot(Callable, Mount);

        Slot.Reconcile();
        ASSERT_EQUAL(MountCount, 1); // the first Reconcile() call mounts
        Slot.Reconcile();
        ASSERT_EQUAL(MountCount, 1); // a second Reconcile() call with no dirty flag is a no-op — no remount
    });

    IT("a Signal::get() during a slot's own invocation registers a dependency", {
        int           MountCount = 0;
        iris::MountFn Mount = StubMount(&MountCount);

        iris::Signal<std::string> ClassName("initial");
        int                        InvocationCount = 0;
        auto                       Callable = Iris::MakeSlotCallable([&]() -> Iris::Component {
            ++InvocationCount;
            return MakeFrame(ClassName.get());
        });
        iris::SlotState Slot(Callable, Mount);

        Slot.Reconcile();
        ASSERT_TRUE(MountCount == 1 && InvocationCount == 1); // initial mount invoked the callable once

        Slot.Reconcile();
        ASSERT_EQUAL(InvocationCount, 1);
        // a Reconcile() call with nothing dirty doesn't re-invoke the callable at all — the
        // control case proving the next assertion isn't a fluke

        // Reading ClassName.get() during the Reconcile() call above should have registered
        // Slot as a dependent, via ambient active-slot tracking — so set() here must mark
        // it dirty even though nothing explicitly subscribed to anything.
        ClassName.set("changed");
        ASSERT_EQUAL(InvocationCount, 1); // Signal::set() alone never reconciles synchronously

        Slot.Reconcile();
        ASSERT_EQUAL(InvocationCount, 2);
        // but the next Reconcile() call DOES re-invoke the callable — proving the earlier
        // .get() during Reconcile() registered a real dependency, not just coincidence
    });

    IT("Tick() reconciles dirty slots automatically", {
        int           MountCount = 0;
        iris::MountFn Mount = StubMount(&MountCount);

        iris::Signal<std::string> ClassName("initial");
        int                        InvocationCount = 0;
        auto                       Callable = Iris::MakeSlotCallable([&]() -> Iris::Component {
            ++InvocationCount;
            return MakeFrame(ClassName.get());
        });
        iris::SlotState Slot(Callable, Mount);
        Slot.Reconcile(); // mount
        ASSERT_EQUAL(InvocationCount, 1); // the initial mount invoked the callable once

        ClassName.set("changed");
        ASSERT_EQUAL(InvocationCount, 1);
        // Signal::set() alone never reconciles synchronously, even though it marked
        // the slot dirty (registered with IrisRuntime, not run yet)

        iris::Tick();
        ASSERT_EQUAL(InvocationCount, 2);
        // iris::Tick() reconciles the dirty slot automatically — no explicit
        // Slot.Reconcile() call needed, matching how a host's own frame loop drives it

        iris::Tick();
        ASSERT_EQUAL(InvocationCount, 2); // a second Tick() with nothing newly dirty doesn't re-invoke the callable
    });

    // docs/iris_stage3_decision_doc.md §6: several Signal::set() calls inside one batch
    // (e.g. one event-handler invocation setting more than one signal) should only
    // reconcile once, at the batch's end — not once per set() call.
    IT("an event batch collapses multiple sets into one reconcile", {
        int           MountCount = 0;
        iris::MountFn Mount = StubMount(&MountCount);

        iris::Signal<int> Counter(0);
        int                InvocationCount = 0;
        auto               Callable = Iris::MakeSlotCallable([&]() -> Iris::Component {
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
            ASSERT_EQUAL(InvocationCount, InvocationsAfterMount);
            // no reconciliation happens for any set() call while the batch is still open
        }
        // ScopedEventBatch's destructor ran IrisRuntime::EndBatch(), which reconciles every
        // slot the runtime knows is dirty — Slot was registered dirty exactly once
        // (IrisRuntime::RegisterDirtySlot dedupes), regardless of how many times it was
        // marked dirty inside the batch.
        ASSERT_EQUAL(InvocationCount, InvocationsAfterMount + 1);
        // closing the batch reconciles exactly once, despite three set() calls inside it
    });

    // docs/next-steps.md's "<Native> doesn't participate in <Slot> reconciliation" entry
    // (2026-08-17). This is the negative case: today, a same-identity re-render (same
    // Tag, same-or-absent Key) never re-invokes Component::NativeBuilder, even though the
    // Slot genuinely re-rendered because a Signal it read changed. Traced into
    // Reconciler.cpp: ReconcileWidget's SameIdentity branch calls ReconcileMatchedInPlace,
    // which only calls ApplyPropDiff (a no-op for <Native> — its build closure isn't an
    // IrisProps entry at all) and recurses into Children (always empty for a leaf) —
    // Component::NativeBuilder is never read there. This matches IrisElementTag.h's own
    // documented intent for Native ("deliberately mount-once... outside the reconciler's
    // content-based diffing"), confirmed here as actual runtime behavior, not just a
    // comment.
    IT("an unattached <Native> node's builder is NOT re-invoked across a re-render with an unchanged key", {
        int           BuildCount = 0;
        iris::MountFn Mount = NativeAwareMount(&BuildCount);

        iris::Signal<int> Version(0);
        auto               Callable = Iris::MakeSlotCallable([&]() -> Iris::Component {
            Version.get(); // read so Version.set() below marks this Slot dirty
            return MakeNativeNode(); // no key, on every render
        });
        iris::SlotState Slot(Callable, Mount);

        Slot.Reconcile();
        ASSERT_EQUAL(BuildCount, 1); // initial mount builds once

        Version.set(1);
        iris::Tick(); // re-renders the Slot -- but Tag+Key (both unset) match the prior render
        ASSERT_EQUAL(BuildCount, 1); // still 1: the "gap" as filed -- same-identity re-render is a no-op
    });

    // The positive case: identity (Tag + Key) is the *only* thing ReconcileWidget/
    // ReconcileChildrenAt ever use to decide "same widget, update in place" vs. "mount
    // fresh" -- nothing about that logic is Native-specific or skips Native nodes. So
    // giving a <Native> node a Key derived from whatever data should drive its rebuild
    // (exactly React's own "key={id} forces a remount" idiom, matching this stack's own
    // documented "Iris is this stack's JSX" framing) already makes NativeBuilder
    // genuinely re-invoke on a Signal-driven re-render, with zero reconciler changes.
    IT("an unattached <Native> node's builder DOES get freshly re-invoked when its key changes across a re-render", {
        int           BuildCount = 0;
        iris::MountFn Mount = NativeAwareMount(&BuildCount);

        iris::Signal<int> Version(0);
        auto               Callable = Iris::MakeSlotCallable([&]() -> Iris::Component {
            const int V = Version.get();
            return MakeNativeNode(Iris::IrisPropValue{std::to_string(V)}); // key tracks the signal
        });
        iris::SlotState Slot(Callable, Mount);

        Slot.Reconcile();
        ASSERT_EQUAL(BuildCount, 1);

        Version.set(1);
        iris::Tick();
        ASSERT_EQUAL(BuildCount, 2);
        // key changed ("0" -> "1") => ReconcileWidget's SameIdentity check fails => the
        // "mount fresh" branch runs => NativeBuilder::Build() genuinely re-invoked.
    });

    // Same as above, but attached to a real parent via AttachToGroup -- the actual
    // pharos-proto splice shape (a <Native> panel spliced into a static App.irisx tree
    // via a <Slot>), not a standalone/detached SlotState. Also proves the handoff shape a
    // consuming backend needs: the freshly-built widget genuinely lands in the real
    // parent's own child list (via ReconcileChildrenAt's InsertChildAt), replacing the
    // old one -- a backend's own MountFn just needs to check Tag == Native and call
    // NativeBuilder->Build(), the same contract every other fresh-mount case already uses.
    IT("a <Native> node spliced via an attached Slot also rebuilds -- and the live widget in the real "
       "parent is genuinely swapped -- only when its key changes",
       {
           int           BuildCount = 0;
           iris::MountFn Mount = NativeAwareMount(&BuildCount);

           ContainerWidget    Parent;
           iris::Signal<int> Version(0);
           auto               Callable = Iris::MakeSlotCallable([&]() -> Iris::Component {
               const int V = Version.get();
               return MakeNativeNode(Iris::IrisPropValue{std::to_string(V)});
           });
           iris::SlotState Slot(Callable, Mount);
           auto             Group = std::make_shared<iris::SlotSiblingGroup>();
           Group->AddEntry(0, &Slot);
           Slot.AttachToGroup(&Parent, Group, 0);

           Slot.Reconcile();
           ASSERT_TRUE(BuildCount == 1 && Parent.GetChildCount() == 1);

           // Identify the mounted widget by its own monotonic Id, not its address -- a
           // freed allocation's address can be reused by the very next allocation, which
           // would make a raw-pointer inequality check a false negative (confirmed while
           // writing this test: RemoveChildAt destroying the old widget followed
           // immediately by Mount() allocating the new one did land at the same address).
           const int FirstId = static_cast<NativeMarkerWidget*>(Parent.GetChildAt(0))->Id;

           Version.set(1);
           iris::Tick();
           ASSERT_TRUE(BuildCount == 2 && Parent.GetChildCount() == 1);
           const int SecondId = static_cast<NativeMarkerWidget*>(Parent.GetChildAt(0))->Id;
           ASSERT_TRUE(SecondId != FirstId);
           // the real parent's own child at this position is a genuinely different widget
           // object -- not just prop-updated -- exactly what a backend's own reconciler
           // needs to pick up and treat as "the live real widget was replaced".
       });

    // docs/next-steps.md's "SlotState::AttachedParent_ dangles past its own widget tree's
    // destruction" entry: reproduces the exact destruction-order mistake that used to crash
    // via a dangling dynamic_cast inside PenumbraWidget::RemoveChildAt -- AttachedParent_'s
    // own widget destroyed while this SlotState is still alive, then the SlotState itself
    // destroyed. Umbra::LivenessGuard (umbra-interfaces, wired into SlotState via
    // AttachedParentWatch_ -- see that field's own doc comment) should make ~SlotState()
    // abort with a clear, attributable message right at the AssertAlive() call, instead of
    // dereferencing the freed parent inside RemoveChildAt.
    //
    // Grepped both libs/cimmerian and this repo's own tests/ for any existing death/abort-
    // expectation primitive (fork, waitpid, SIGABRT, "death") and found none -- cimmerian/
    // test.hpp has no EXPECT_DEATH-style macro to match. Rather than fabricate a check that
    // only restates LivenessGuard's own precondition (e.g. just asserting Watch.IsAlive() ==
    // false, which proves nothing about whether AssertAlive() actually aborts), this uses a
    // real fork()+waitpid() subprocess -- the same technique gtest's own death tests use
    // internally -- so the assertions below prove the actual abort happens, not just that
    // its precondition holds.
    IT("SlotState::~SlotState() aborts via LivenessGuard, with an attributable message, "
       "rather than dereferencing an AttachedParent_ destroyed out from under it",
       {
           int Pipe[2];
           REQUIRE_TRUE(pipe(Pipe) == 0);

           const pid_t Pid = fork();
           if (Pid < 0) {
               REQUIRE_TRUE(false); // fork() itself failed -- an environment problem, not what this test covers
           }

           if (Pid == 0) {
               // ---- Child process only from here down. Must never touch cimmerian's own
               // shared test-reporting state (a REQUIRE_TRUE/ASSERT_TRUE call here would run
               // in a second, forked copy of the whole test binary's process image and
               // corrupt the parent's own test output) -- exits via abort() (the expected,
               // guarded outcome) or _exit() (the bug regressed and nothing caught it).
               close(Pipe[0]);
               dup2(Pipe[1], STDERR_FILENO); // capture AssertAlive()'s own stderr message
               close(Pipe[1]);

               auto Parent   = std::make_unique<ContainerWidget>();
               auto Callable = Iris::MakeSlotCallable([]() -> Iris::Component { return MakeFrame("a"); });
               auto Slot     = std::make_unique<iris::SlotState>(Callable, StubMount(nullptr));
               auto Group    = std::make_shared<iris::SlotSiblingGroup>();
               Group->AddEntry(0, Slot.get());
               Slot->AttachToGroup(Parent.get(), Group, 0);
               Slot->Reconcile(); // gives AttachedParent_ a real child to (attempt to) remove

               Parent.reset(); // the exact destruction-order mistake LivenessGuard exists for
               Slot.reset();   // ~SlotState() should AssertAlive()+abort() here, not dereference Parent

               _exit(0); // only reached if the guard failed to catch the dangling pointer
           }

           // ---- Parent process only from here down (the child above always exits inside
           // its own branch and never falls through to this point).
           close(Pipe[1]);
           std::string Output;
           char        Buf[256];
           ssize_t     BytesRead = 0;
           while ((BytesRead = read(Pipe[0], Buf, sizeof(Buf))) > 0) {
               Output.append(Buf, static_cast<std::size_t>(BytesRead));
           }
           close(Pipe[0]);

           int Status = 0;
           REQUIRE_TRUE(waitpid(Pid, &Status, 0) == Pid);
           // Aborted via SIGABRT specifically -- not just "crashed somehow" (a SIGSEGV here
           // would mean the guard failed to catch it and the old dangling-dereference crash
           // regressed instead).
           ASSERT_TRUE(WIFSIGNALED(Status) != 0 && WTERMSIG(Status) == SIGABRT);
           ASSERT_TRUE(Output.find("AttachedParent_") != std::string::npos);
       });

    IT("RegisterRoot/GetRoot hold the whole application's live-widget root", {
        StubWidget Root;
        iris::RegisterRoot(&Root);
        ASSERT_TRUE(iris::GetRoot() == &Root);

        StubWidget Replacement;
        iris::RegisterRoot(&Replacement); // a later call simply replaces the previous one
        ASSERT_TRUE(iris::GetRoot() == &Replacement);

        iris::RegisterRoot(nullptr); // leave global runtime state clean for later tests
    });
});
