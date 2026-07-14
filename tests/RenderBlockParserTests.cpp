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

Iris::RenderBlockParser::Result ParseSource(std::string_view Source) {
    Iris::RenderBlockParser Parser(Source, "test.iris");
    return Parser.Parse();
}

const Iris::Prop* FindProp(const Iris::ElementNode& Node, std::string_view Name) {
    for (const auto& P : Node.Props) {
        if (P.Name == Name) {
            return &P;
        }
    }
    return nullptr;
}

void TestSingleSelfClosingRoot() {
    const auto Result = ParseSource(R"(render { <Image src="a.png" /> })");
    Expect(Result.Errors.empty(), "self-closing single-root render block has no errors");
    Expect(Result.Blocks.size() == 1, "one render block found");
    if (Result.Blocks.empty()) {
        return;
    }
    const auto& Root = Result.Blocks[0].Root;
    Expect(Root.Tag == "Image", "root tag is Image");
    Expect(Root.Children.empty(), "self-closing element has no children");
    const auto* Src = FindProp(Root, "src");
    Expect(Src != nullptr && Src->Value.Kind == Iris::PropValueKind::StringLiteral && Src->Value.Text == "a.png",
           "src prop is a string literal with quotes stripped");
}

void TestPairedTagWithChildElement() {
    const auto Result = ParseSource(R"(render {
        <Frame class="start-menu">
            <Button label="Settings" />
        </Frame>
    })");
    Expect(Result.Errors.empty(), "paired root with one child element has no errors");
    Expect(Result.Blocks.size() == 1, "one render block found");
    if (Result.Blocks.empty()) {
        return;
    }
    const auto& Root = Result.Blocks[0].Root;
    Expect(Root.Tag == "Frame", "root tag is Frame");
    Expect(Root.Children.size() == 1, "root has exactly one child");
    if (Root.Children.size() == 1) {
        const auto& Child = Root.Children[0];
        Expect(Child.Kind == Iris::ElementChildKind::Element, "child is an element");
        Expect(Child.Element && Child.Element->Tag == "Button", "child tag is Button");
    }
}

void TestKeyPropIsExtractedNotLeftInProps() {
    const auto Result = ParseSource(R"(render { <Frame key={item.id} class="row"></Frame> })");
    Expect(Result.Errors.empty(), "key + class props parse with no errors");
    if (Result.Blocks.empty()) {
        return;
    }
    const auto& Root = Result.Blocks[0].Root;
    Expect(Root.Key.has_value() && Root.Key->Kind == Iris::PropValueKind::EscapeHatch &&
               Root.Key->Text == "item.id",
           "key is captured as an escape hatch, verbatim");
    Expect(FindProp(Root, "key") == nullptr, "key never appears in Props");
    Expect(FindProp(Root, "class") != nullptr, "class remains an ordinary entry in Props");
}

void TestEscapeHatchPropIsCapturedVerbatimWithNestedBraces() {
    const auto Result = ParseSource(
        R"(render { <Button onPress={[&]() { settingsOpen.set(true); }} /> })");
    Expect(Result.Errors.empty(), "nested-brace escape hatch parses with no errors");
    if (Result.Blocks.empty()) {
        return;
    }
    const auto* OnPress = FindProp(Result.Blocks[0].Root, "onPress");
    Expect(OnPress != nullptr && OnPress->Value.Kind == Iris::PropValueKind::EscapeHatch,
           "onPress is an escape hatch");
    Expect(OnPress != nullptr && OnPress->Value.Text == "[&]() { settingsOpen.set(true); }",
           "escape hatch body is captured verbatim, including its own nested braces");
}

void TestEscapeHatchContainingAngleBracketsIsOpaque() {
    // A tag-looking string inside an escape hatch must not be parsed as an
    // element — docs/iris_core_spec.md §1.4: escape hatch contents are
    // emitted verbatim, never parsed.
    const auto Result = ParseSource(
        R"(render {
            <Slot>
                {[&]() -> IrisComponent {
                    if (settingsOpen.get()) {
                        return <SettingsPage onClose={[&]() { settingsOpen.set(false); }} />;
                    }
                    return nullptr;
                }}
            </Slot>
        })");
    Expect(Result.Errors.empty(), "<Slot> with a JSX-looking escape hatch body parses with no errors");
    if (Result.Blocks.empty()) {
        return;
    }
    const auto& Root = Result.Blocks[0].Root;
    Expect(Root.Tag == "Slot", "root tag is Slot — no special-casing needed, parsed like any other tag");
    Expect(Root.Children.size() == 1 && Root.Children[0].Kind == Iris::ElementChildKind::EscapeHatch,
           "Slot's single child is one escape hatch");
}

void TestLiteralTextChildIsCapturedWithCollapsedWhitespace() {
    const auto Result = ParseSource(R"(render { <Text>Hello   World</Text> })");
    Expect(Result.Errors.empty(), "literal text child parses with no errors");
    if (Result.Blocks.empty()) {
        return;
    }
    const auto& Root = Result.Blocks[0].Root;
    Expect(Root.Children.size() == 1 && Root.Children[0].Kind == Iris::ElementChildKind::Text,
           "root has exactly one literal text child");
    Expect(Root.Children.size() == 1 && Root.Children[0].Text == "Hello World",
           "internal whitespace runs collapse to a single space");
}

void TestMixedTextAndEscapeHatchChildren() {
    const auto Result = ParseSource(R"(render { <Inline class="label">{props.label}</Inline> })");
    Expect(Result.Errors.empty(), "escape-hatch-only interpolated child parses with no errors");
    if (Result.Blocks.empty()) {
        return;
    }
    const auto& Root = Result.Blocks[0].Root;
    Expect(Root.Children.size() == 1 && Root.Children[0].Kind == Iris::ElementChildKind::EscapeHatch,
           "interpolated child is one escape hatch");
    Expect(Root.Children.size() == 1 && Root.Children[0].EscapeHatch.has_value() &&
               Root.Children[0].EscapeHatch->Text == "props.label",
           "interpolation body is captured verbatim");
}

void TestCommentsInsideRenderBlockAreStrippedSilently() {
    const auto Result = ParseSource(R"(render {
        <Frame class="start-menu">
            // a line comment between elements
            <Button label="Settings" />
            /* a block comment too */
        </Frame>
    })");
    Expect(Result.Errors.empty(), "comments between elements parse with no errors");
    if (Result.Blocks.empty()) {
        return;
    }
    const auto& Root = Result.Blocks[0].Root;
    Expect(Root.Children.size() == 1, "comments contribute no children — only the real Button element does");
}

void TestMultipleRootSiblingsIsAnError() {
    const auto Result = ParseSource(R"(render { <A/> <B/> })");
    Expect(!Result.Errors.empty(), "more than one top-level sibling is a parse error");
    Expect(Result.Blocks.empty(), "no block is recorded when the single-root rule is violated");
}

void TestMismatchedClosingTagIsAnError() {
    const auto Result = ParseSource(R"(render { <Frame></Button> })");
    Expect(!Result.Errors.empty(), "a closing tag that doesn't match its opening tag is a parse error");
}

void TestPartyScreenWorkedExampleParsesEndToEnd() {
    // The full spec §9 self-check example (props, state, an event handler, a
    // conditional, and a keyed list, all via <Slot>), minus the surrounding
    // host-language declarations that are outside any render{} block and so
    // irrelevant to this parser.
    const auto Result = ParseSource(R"(
        render {
            <Frame class="party-screen">
                <Button label="Details" onPress={[&]() { detailsOpen.set(true); }} />
                <Slot>
                    {[&]() -> IrisComponent {
                        if (!detailsOpen.get()) return nullptr;
                        return <Frame class="details-panel">
                            <Slot>
                                {[&]() -> std::vector<IrisComponent> {
                                    std::vector<IrisComponent> rows;
                                    for (auto& member : props.members) {
                                        rows.push_back(
                                            <Frame key={member.id} class="party-row">
                                                <HealthBar current={member.hp} max={member.maxHp} label={member.name} />
                                            </Frame>
                                        );
                                    }
                                    return rows;
                                }}
                            </Slot>
                        </Frame>;
                    }}
                </Slot>
            </Frame>
        }
    )");
    Expect(Result.Errors.empty(), "the spec §9 PartyScreen example parses with no errors");
    Expect(Result.Blocks.size() == 1, "the PartyScreen example is one render block");
    if (Result.Blocks.empty()) {
        return;
    }
    const auto& Root = Result.Blocks[0].Root;
    Expect(Root.Tag == "Frame" && Root.Children.size() == 2,
           "root Frame has two children: the Button and the outer Slot");
}

} // namespace

void RunRenderBlockParserTests() {
    TestSingleSelfClosingRoot();
    TestPairedTagWithChildElement();
    TestKeyPropIsExtractedNotLeftInProps();
    TestEscapeHatchPropIsCapturedVerbatimWithNestedBraces();
    TestEscapeHatchContainingAngleBracketsIsOpaque();
    TestLiteralTextChildIsCapturedWithCollapsedWhitespace();
    TestMixedTextAndEscapeHatchChildren();
    TestCommentsInsideRenderBlockAreStrippedSilently();
    TestMultipleRootSiblingsIsAnError();
    TestMismatchedClosingTagIsAnError();
    TestPartyScreenWorkedExampleParsesEndToEnd();
}
