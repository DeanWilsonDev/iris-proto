#include "Iris/Codegen.h"
#include "Iris/RenderBlockParser.h"

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

// Parses one render block and hands its root straight to codegen — Codegen only
// consumes RenderBlockParser's output, never re-parses source itself.
Iris::CodegenResult Generate(std::string_view Source) {
    Iris::RenderBlockParser Parser(Source, "test.iris");
    const auto              Parsed = Parser.Parse();
    if (!Parsed.Errors.empty() || Parsed.Blocks.empty()) {
        return Iris::CodegenResult{"", {{"render block failed to parse", {}}}};
    }
    return Iris::GenerateComponentExpression(Parsed.Blocks[0].Root);
}

bool Contains(const std::string& Haystack, std::string_view Needle) {
    return Haystack.find(Needle) != std::string::npos;
}

void TestSimplePrimitiveWithStringAndEscapeHatchProps() {
    const auto Result = Generate(R"(render {
        <Frame class="start-menu" onPress={[&]() { x.set(true); }} />
    })");
    Expect(Result.Errors.empty(), "Frame with class + onPress codegens with no errors");
    Expect(Contains(Result.Source, "Iris::IrisElementTag::Frame"), "tag is Iris::IrisElementTag::Frame");
    Expect(Contains(Result.Source, "std::in_place_type<std::string>, \"start-menu\""),
           "class is wrapped in_place_type<std::string> with the quoted literal");
    Expect(Contains(Result.Source, "std::in_place_type<std::function<void()>>, [&]() { x.set(true); }"),
           "onPress is wrapped in_place_type<std::function<void()>> with the escape hatch verbatim");
}

void TestUnknownPropOnPrimitiveIsAnError() {
    const auto Result = Generate(R"(render { <Frame nonsense="x" /> })");
    Expect(!Result.Errors.empty(), "unknown prop name on a Core primitive is a codegen error");
    Expect(Result.Source.empty(), "no source is emitted when there are codegen errors");
}

void TestNestedElementChildRecurses() {
    const auto Result = Generate(R"(render {
        <Frame class="outer"><Frame class="inner" /></Frame>
    })");
    Expect(Result.Errors.empty(), "nested Frame children codegen with no errors");
    const std::size_t OuterPos = Result.Source.find("\"outer\"");
    const std::size_t InnerPos = Result.Source.find("\"inner\"");
    Expect(OuterPos != std::string::npos && InnerPos != std::string::npos && OuterPos < InnerPos,
           "inner Frame's expression is nested inside the outer Frame's Children list");
}

void TestImageWithChildIsAnError() {
    const auto Result = Generate(R"(render { <Image src="a.png"><Frame /></Image> })");
    Expect(!Result.Errors.empty(), "<Image> (a leaf) with a child is a codegen error");
}

void TestFrameWithTextChildIsAnError() {
    const auto Result = Generate(R"(render { <Frame>hello</Frame> })");
    Expect(!Result.Errors.empty(), "<Frame> with a literal-text child is a codegen error");
}

void TestTextPrimitiveConcatenatesMixedChildren() {
    const auto Result = Generate(R"(render { <Text>Hello {name}!</Text> })");
    Expect(Result.Errors.empty(), "<Text> with mixed literal/escape-hatch children codegens with no errors");
    Expect(Contains(Result.Source, "\"text\""), "content lands in a \"text\" prop");
    // RenderBlockParser trims each literal-text run on flush, including the space
    // adjacent to a following escape hatch (docs/iris_stage1_codegen_decision.md notes
    // this concatenation scheme; the trimming itself is pre-existing RenderBlockParser
    // behavior, not something codegen controls) — so "Hello " arrives as "Hello".
    Expect(Contains(Result.Source, "\"Hello\" + name + \"!\""),
           "literal runs and escape hatches join with '+' in source order");
    Expect(!Contains(Result.Source, "Children"), "<Text> never uses a literal 'Children' identifier in its own emission");
}

void TestTextWithNestedElementIsAnError() {
    const auto Result = Generate(R"(render { <Text><Frame /></Text> })");
    Expect(!Result.Errors.empty(), "<Text> with a nested element child is a codegen error");
}

void TestInlineWrapsTextChildrenAsSyntheticTextNodes() {
    const auto Result = Generate(R"(render {
        <Inline>Score: {points}<Frame class="badge" /></Inline>
    })");
    Expect(Result.Errors.empty(), "<Inline> with mixed children codegens with no errors");
    Expect(Contains(Result.Source, "Iris::IrisElementTag::Text"),
           "a synthetic Text node is emitted for Inline's literal/escape-hatch runs");
    Expect(Contains(Result.Source, "\"badge\""), "the real nested Frame child is still emitted");
}

void TestSlotWithSingleEscapeHatchChild() {
    const auto Result = Generate(R"(render {
        <Slot>{[&]() -> IrisComponent { return nullptr; }}</Slot>
    })");
    Expect(Result.Errors.empty(), "<Slot> with exactly one escape-hatch child codegens with no errors");
    Expect(Contains(Result.Source, "Iris::MakeSlotCallable([&]() -> IrisComponent { return nullptr; })"),
           "the lambda is passed to Iris::MakeSlotCallable verbatim");
}

void TestSlotWithWrongArityIsAnError() {
    const auto Result = Generate(R"(render { <Slot></Slot> })");
    Expect(!Result.Errors.empty(), "<Slot> with zero children is a codegen error");
}

void TestSlotWithNonEscapeHatchChildIsAnError() {
    const auto Result = Generate(R"(render { <Slot><Frame /></Slot> })");
    Expect(!Result.Errors.empty(), "<Slot> with a nested-element child instead of an escape hatch is a codegen error");
}

void TestComponentInvocationEmitsNamePropsConvention() {
    const auto Result = Generate(
        R"(render { <HealthBar current={player.health} max={player.maxHealth} label="HP" /> })");
    Expect(Result.Errors.empty(), "an imported-component-shaped element codegens with no errors");
    Expect(Contains(Result.Source, "HealthBar(HealthBarProps{"),
           "unrecognised tag becomes a call to Name(NameProps{...}) per the <Name>Props convention");
    Expect(Contains(Result.Source, ".current = player.health"), "escape-hatch prop values pass through verbatim");
    Expect(Contains(Result.Source, ".label = \"HP\""), "string-literal prop values are quoted verbatim");
}

void TestComponentInvocationWithChildrenIsAnError() {
    const auto Result = Generate(R"(render { <HealthBar current={1} max={2}><Frame /></HealthBar> })");
    Expect(!Result.Errors.empty(),
           "a component element with children is a codegen error (implicit children forwarding unimplemented)");
}

void TestPartyScreenOuterLevelCodegens() {
    // The spec §9 PartyScreen example, one level deep: RenderBlockParser captures a
    // <Slot>'s escape-hatch body verbatim, including any JSX-looking text inside it
    // (docs/iris_core_spec.md §1.4 — tested directly in
    // TestEscapeHatchContainingAngleBracketsIsOpaque, tests/RenderBlockParserTests.cpp).
    // Codegen therefore only transforms what's structurally outside escape hatches;
    // this test covers exactly that boundary, not the nested <Frame> inside the lambda.
    const auto Result = Generate(R"(render {
        <Frame class="party-screen">
            <Button label="Details" onPress={[&]() { detailsOpen.set(true); }} />
            <Slot>
                {[&]() -> IrisComponent {
                    if (!detailsOpen.get()) return nullptr;
                    return <Frame class="details-panel"></Frame>;
                }}
            </Slot>
        </Frame>
    })");
    Expect(Result.Errors.empty(), "PartyScreen's outer structure codegens with no errors");
    Expect(Contains(Result.Source, "Button(ButtonProps{"), "Button is emitted as a component invocation");
    Expect(Contains(Result.Source, "Iris::MakeSlotCallable"), "the outer Slot is emitted with MakeSlotCallable");
    Expect(Contains(Result.Source, "return <Frame class=\"details-panel\"></Frame>;"),
           "the Slot's escape-hatch body — JSX and all — passes through verbatim, unparsed");
}

} // namespace

void RunCodegenTests() {
    TestSimplePrimitiveWithStringAndEscapeHatchProps();
    TestUnknownPropOnPrimitiveIsAnError();
    TestNestedElementChildRecurses();
    TestImageWithChildIsAnError();
    TestFrameWithTextChildIsAnError();
    TestTextPrimitiveConcatenatesMixedChildren();
    TestTextWithNestedElementIsAnError();
    TestInlineWrapsTextChildrenAsSyntheticTextNodes();
    TestSlotWithSingleEscapeHatchChild();
    TestSlotWithWrongArityIsAnError();
    TestSlotWithNonEscapeHatchChildIsAnError();
    TestComponentInvocationEmitsNamePropsConvention();
    TestComponentInvocationWithChildrenIsAnError();
    TestPartyScreenOuterLevelCodegens();
}
