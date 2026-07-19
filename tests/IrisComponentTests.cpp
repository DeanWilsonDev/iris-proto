#include "Iris/IrisComponent.h"

#include <cstdio>
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

// The 4-field constructor exists purely so Codegen.h's emitted
// `IrisComponent{Tag, Props, Children, SlotCallable}` call shape keeps compiling now
// that IrisComponent has user-declared constructors (and so is no longer an
// aggregate) — this is exactly that shape, exercised directly rather than through
// generated source.
void TestFourFieldConstructorMatchesCodegenShape() {
    Iris::IrisComponent Node{Iris::IrisElementTag::Frame, Iris::IrisProps{}, {}, nullptr};
    Expect(Node.Tag == Iris::IrisElementTag::Frame, "the 4-field constructor sets Tag");
    Expect(Node.Children.empty(), "the 4-field constructor accepts an empty Children list");
}

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

void TestNullptrConvertsToNoneSentinel() {
    const Iris::IrisComponent Node = ReturnsNullptr(false);
    Expect(Node.Tag == Iris::IrisElementTag::None,
           "a lambda/function returning nullptr for IrisComponent produces the None sentinel");

    const Iris::IrisComponent Direct = nullptr;
    Expect(Direct.Tag == Iris::IrisElementTag::None, "direct nullptr construction also yields None");
}

void TestSlotCallableReturningNullptrCompilesViaMakeSlotCallable() {
    // The exact shape of the spec's conditional-rendering <Slot> pattern
    // (docs/iris_core_spec.md §1.5) — a lambda that may return nullptr.
    auto Callable = Iris::MakeSlotCallable([]() -> Iris::IrisComponent { return nullptr; });
    Expect(Callable != nullptr, "MakeSlotCallable accepts a lambda that returns nullptr");
    const auto* Fn = std::get_if<std::function<Iris::IrisComponent()>>(&Callable->Callable);
    Expect(Fn != nullptr, "the stored callable is the IrisComponent()-returning variant member");
    if (Fn != nullptr) {
        Expect((*Fn)().Tag == Iris::IrisElementTag::None, "invoking it produces the None sentinel");
    }
}

} // namespace

void RunIrisComponentTests() {
    TestFourFieldConstructorMatchesCodegenShape();
    TestNullptrConvertsToNoneSentinel();
    TestSlotCallableReturningNullptrCompilesViaMakeSlotCallable();
}
