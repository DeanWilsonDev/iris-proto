#include "cimmerian/test.hpp"

#include "Iris/IrisComponent.h"

namespace {

// docs/iris_core_spec.md §1.5/§9 write `return nullptr;` inside a `<Slot>` callable
// declared to return IrisComponent — this must actually compile and produce the
// IrisElementTag::None sentinel (docs/iris_core_spec.md §8, docs/iris_escape_hatch_
// decision.md's Verification section).
Iris::IrisComponent ReturnsNullptr(bool Condition) {
    if (!Condition) {
        return nullptr; // exercises the implicit nullptr_t -> IrisComponent conversion
    }
    return Iris::IrisComponent{Iris::IrisElementTag::Frame, Iris::IrisProps{}, {}, nullptr};
}

} // namespace

DESCRIBE("IrisComponent", {
    // The 4-field constructor exists purely so Codegen.h's emitted
    // `IrisComponent{Tag, Props, Children, SlotCallable}` call shape keeps compiling now
    // that IrisComponent has user-declared constructors (and so is no longer an
    // aggregate) — this is exactly that shape, exercised directly rather than through
    // generated source.
    IT("the 4-field constructor matches Codegen's emitted shape", {
        // Parens, not braces, around the constructor args: a top-level comma written
        // directly inside an IT(...) body would split into extra macro arguments — the
        // preprocessor's macro-argument scanner balances only `(` `)`, not `{` `}`.
        Iris::IrisComponent Node(Iris::IrisElementTag::Frame, Iris::IrisProps{}, {}, nullptr);
        ASSERT_TRUE(Node.Tag == Iris::IrisElementTag::Frame); // the 4-field constructor sets Tag
        ASSERT_TRUE(Node.Children.empty());                   // the 4-field constructor accepts an empty Children list
    });

    IT("nullptr converts to the None sentinel", {
        const Iris::IrisComponent Node = ReturnsNullptr(false);
        ASSERT_TRUE(Node.Tag == Iris::IrisElementTag::None);
        // a lambda/function returning nullptr for IrisComponent produces the None sentinel

        const Iris::IrisComponent Direct = nullptr;
        ASSERT_TRUE(Direct.Tag == Iris::IrisElementTag::None); // direct nullptr construction also yields None
    });

    IT("a Slot callable returning nullptr compiles via MakeSlotCallable", {
        // The exact shape of the spec's conditional-rendering <Slot> pattern
        // (docs/iris_core_spec.md §1.5) — a lambda that may return nullptr.
        auto Callable = Iris::MakeSlotCallable([]() -> Iris::IrisComponent { return nullptr; });
        REQUIRE_TRUE(Callable != nullptr); // MakeSlotCallable accepts a lambda that returns nullptr
        const auto* Fn = std::get_if<std::function<Iris::IrisComponent()>>(&Callable->Callable);
        REQUIRE_TRUE(Fn != nullptr); // the stored callable is the IrisComponent()-returning variant member
        ASSERT_TRUE((*Fn)().Tag == Iris::IrisElementTag::None); // invoking it produces the None sentinel
    });
});
