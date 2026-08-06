#include "cimmerian/test.hpp"

#include "Iris/RenderBlockParser.h"

namespace {

Iris::RenderBlockParser::Result ParseSource(std::string_view Source) {
    Iris::RenderBlockParser Parser(Source, "test.iris");
    return Parser.Parse();
}

Iris::RenderBlockParser::Result ParseSourceAs(std::string_view Source, std::string FilePath) {
    Iris::RenderBlockParser Parser(Source, std::move(FilePath));
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

} // namespace

DESCRIBE("RenderBlockParser", {
    IT("a single self-closing root parses", {
        const auto Result = ParseSource(R"(render { <Image src="a.png" /> })");
        ASSERT_TRUE(Result.Errors.empty()); // self-closing single-root render block has no errors
        REQUIRE_EQUAL(Result.Blocks.size(), static_cast<std::size_t>(1)); // one render block found

        const auto& Root = Result.Blocks[0].Root;
        ASSERT_TRUE(Root.Tag == "Image");      // root tag is Image
        ASSERT_TRUE(Root.Children.empty());    // self-closing element has no children
        const auto* Src = FindProp(Root, "src");
        ASSERT_TRUE(Src != nullptr && Src->Value.Kind == Iris::PropValueKind::StringLiteral && Src->Value.Text == "a.png");
        // src prop is a string literal with quotes stripped
    });

    IT("a paired tag with a child element", {
        const auto Result = ParseSource(R"(render {
            <Frame class="start-menu">
                <Button label="Settings" />
            </Frame>
        })");
        ASSERT_TRUE(Result.Errors.empty()); // paired root with one child element has no errors
        REQUIRE_EQUAL(Result.Blocks.size(), static_cast<std::size_t>(1)); // one render block found

        const auto& Root = Result.Blocks[0].Root;
        ASSERT_TRUE(Root.Tag == "Frame");            // root tag is Frame
        REQUIRE_EQUAL(Root.Children.size(), static_cast<std::size_t>(1)); // root has exactly one child
        const auto& Child = Root.Children[0];
        ASSERT_TRUE(Child.Kind == Iris::ElementChildKind::Element); // child is an element
        ASSERT_TRUE(Child.Element && Child.Element->Tag == "Button"); // child tag is Button
    });

    IT("the key prop is extracted, not left in Props", {
        const auto Result = ParseSource(R"(render { <Frame key={item.id} class="row"></Frame> })");
        ASSERT_TRUE(Result.Errors.empty()); // key + class props parse with no errors
        REQUIRE_TRUE(!Result.Blocks.empty());

        const auto& Root = Result.Blocks[0].Root;
        ASSERT_TRUE(Root.Key.has_value() && Root.Key->Kind == Iris::PropValueKind::EscapeHatch &&
                    Root.Key->Text == "item.id");
        // key is captured as an escape hatch, verbatim
        ASSERT_TRUE(FindProp(Root, "key") == nullptr);   // key never appears in Props
        ASSERT_TRUE(FindProp(Root, "class") != nullptr); // class remains an ordinary entry in Props
    });

    IT("the ref prop is extracted, not left in Props", {
        const auto Result = ParseSource(R"(render { <Icon ref="trigger-icon" class="row"></Icon> })");
        ASSERT_TRUE(Result.Errors.empty()); // ref + class props parse with no errors
        REQUIRE_TRUE(!Result.Blocks.empty());

        const auto& Root = Result.Blocks[0].Root;
        ASSERT_TRUE(Root.Ref.has_value() && Root.Ref->Kind == Iris::PropValueKind::StringLiteral &&
                    Root.Ref->Text == "trigger-icon");
        // ref is captured as a string literal, quotes stripped
        ASSERT_TRUE(FindProp(Root, "ref") == nullptr);   // ref never appears in Props
        ASSERT_TRUE(FindProp(Root, "class") != nullptr); // class remains an ordinary entry in Props
    });

    IT("an escape-hatch prop is captured verbatim with nested braces", {
        const auto Result = ParseSource(
            R"(render { <Button onPress={[&]() { settingsOpen.set(true); }} /> })");
        ASSERT_TRUE(Result.Errors.empty()); // nested-brace escape hatch parses with no errors
        REQUIRE_TRUE(!Result.Blocks.empty());

        const auto* OnPress = FindProp(Result.Blocks[0].Root, "onPress");
        ASSERT_TRUE(OnPress != nullptr && OnPress->Value.Kind == Iris::PropValueKind::EscapeHatch);
        // onPress is an escape hatch
        ASSERT_TRUE(OnPress != nullptr && OnPress->Value.Text == "[&]() { settingsOpen.set(true); }");
        // escape hatch body is captured verbatim, including its own nested braces
    });

    IT("an escape hatch containing angle brackets is opaque", {
        // A tag-looking string inside an escape hatch must not be parsed as an
        // element — docs/iris_core_spec.md §1.4: escape hatch contents are
        // emitted verbatim, never parsed.
        const auto Result = ParseSource(
            R"(render {
                <Slot>
                    {[&]() -> Component {
                        if (settingsOpen.get()) {
                            return <SettingsPage onClose={[&]() { settingsOpen.set(false); }} />;
                        }
                        return nullptr;
                    }}
                </Slot>
            })");
        ASSERT_TRUE(Result.Errors.empty()); // <Slot> with a JSX-looking escape hatch body parses with no errors
        REQUIRE_TRUE(!Result.Blocks.empty());

        const auto& Root = Result.Blocks[0].Root;
        ASSERT_TRUE(Root.Tag == "Slot"); // root tag is Slot — no special-casing needed, parsed like any other tag
        ASSERT_TRUE(Root.Children.size() == 1 && Root.Children[0].Kind == Iris::ElementChildKind::EscapeHatch);
        // Slot's single child is one escape hatch
    });

    IT("a !{ } JSX-transform escape hatch recursively parses nested elements", {
        // `!{ }` — unlike the opaque `{ }` form above — recursively parses `<Tag>`
        // runs inside it (docs/archive/iris_next_steps.md, "Resolved: JSX inside escape
        // hatches"). `onClose`'s own body has no JSX, so it stays a regular,
        // opaque `{ }` escape hatch even though it's nested inside a `!{ }`.
        const auto Result = ParseSource(
            R"(render {
                <Slot>
                    !{[&]() -> Component {
                        if (settingsOpen.get()) {
                            return <SettingsPage onClose={[&]() { settingsOpen.set(false); }} />;
                        }
                        return nullptr;
                    }}
                </Slot>
            })");
        ASSERT_TRUE(Result.Errors.empty()); // <Slot> with a !{ } JSX-transform escape hatch body parses with no errors
        REQUIRE_TRUE(!Result.Blocks.empty());

        const auto& Root = Result.Blocks[0].Root;
        ASSERT_TRUE(Root.Tag == "Slot" && Root.Children.size() == 1 &&
                    Root.Children[0].Kind == Iris::ElementChildKind::EscapeHatch);
        // Slot's single child is one escape hatch
        const Iris::PropValue& Value = *Root.Children[0].EscapeHatch;
        ASSERT_TRUE(Value.Kind == Iris::PropValueKind::JsxEscapeHatch); // the escape hatch is the JSX-transform kind

        bool FoundSettingsPageElement = false;
        for (const Iris::JsxSegment& Segment : Value.JsxSegments) {
            if (Segment.Kind == Iris::JsxSegmentKind::Element) {
                ASSERT_FALSE(FoundSettingsPageElement); // exactly one nested element segment is found
                FoundSettingsPageElement = true;
                ASSERT_TRUE(Segment.Element != nullptr && Segment.Element->Tag == "SettingsPage");
                // the nested element is a parsed <SettingsPage> node, not opaque text
                const auto* OnClose = FindProp(*Segment.Element, "onClose");
                ASSERT_TRUE(OnClose != nullptr && OnClose->Value.Kind == Iris::PropValueKind::EscapeHatch);
                // SettingsPage's onClose stays a regular opaque { } escape hatch, not JSX-transformed
            }
        }
        ASSERT_TRUE(FoundSettingsPageElement); // the !{ } body's JSX run was recursively parsed into an element segment
    });

    IT("a !{ } JSX-transform escape hatch does not misread template angles as JSX", {
        // `std::vector<Component>` has exactly the same `< Identifier >` shape as
        // an attribute-less opening tag, but with no whitespace before the `<` — the
        // signal ParseJsxEscapeHatch uses to tell a template argument list apart from
        // a real JSX element start (every JSX use in the spec has a space or newline
        // before its `<`).
        const auto Result = ParseSource(
            R"(render {
                <Slot>
                    !{[&]() -> std::vector<Component> {
                        std::vector<Component> rows;
                        return rows;
                    }}
                </Slot>
            })");
        ASSERT_TRUE(Result.Errors.empty()); // a !{ } body containing std::vector<Component> parses with no errors
        REQUIRE_TRUE(!Result.Blocks.empty());

        const Iris::PropValue& Value = *Result.Blocks[0].Root.Children[0].EscapeHatch;
        for (const Iris::JsxSegment& Segment : Value.JsxSegments) {
            ASSERT_TRUE(Segment.Kind == Iris::JsxSegmentKind::RawText);
            // no segment is misread as an element — std::vector<Component> stays raw text
        }
    });

    IT("a literal text child is captured with collapsed whitespace", {
        const auto Result = ParseSource(R"(render { <Text>Hello   World</Text> })");
        ASSERT_TRUE(Result.Errors.empty()); // literal text child parses with no errors
        REQUIRE_TRUE(!Result.Blocks.empty());

        const auto& Root = Result.Blocks[0].Root;
        ASSERT_TRUE(Root.Children.size() == 1 && Root.Children[0].Kind == Iris::ElementChildKind::Text);
        // root has exactly one literal text child
        ASSERT_TRUE(Root.Children.size() == 1 && Root.Children[0].Text == "Hello World");
        // internal whitespace runs collapse to a single space
    });

    IT("mixed text and escape-hatch children", {
        const auto Result = ParseSource(R"(render { <Inline class="label">{props.label}</Inline> })");
        ASSERT_TRUE(Result.Errors.empty()); // escape-hatch-only interpolated child parses with no errors
        REQUIRE_TRUE(!Result.Blocks.empty());

        const auto& Root = Result.Blocks[0].Root;
        ASSERT_TRUE(Root.Children.size() == 1 && Root.Children[0].Kind == Iris::ElementChildKind::EscapeHatch);
        // interpolated child is one escape hatch
        ASSERT_TRUE(Root.Children.size() == 1 && Root.Children[0].EscapeHatch.has_value() &&
                    Root.Children[0].EscapeHatch->Text == "props.label");
        // interpolation body is captured verbatim
    });

    IT("comments inside a render block are stripped silently", {
        const auto Result = ParseSource(R"(render {
            <Frame class="start-menu">
                // a line comment between elements
                <Button label="Settings" />
                /* a block comment too */
            </Frame>
        })");
        ASSERT_TRUE(Result.Errors.empty()); // comments between elements parse with no errors
        REQUIRE_TRUE(!Result.Blocks.empty());

        const auto& Root = Result.Blocks[0].Root;
        ASSERT_EQUAL(Root.Children.size(), static_cast<std::size_t>(1));
        // comments contribute no children — only the real Button element does
    });

    IT("multiple root siblings is an error", {
        const auto Result = ParseSource(R"(render { <A/> <B/> })");
        ASSERT_FALSE(Result.Errors.empty()); // more than one top-level sibling is a parse error
        ASSERT_TRUE(Result.Blocks.empty());  // no block is recorded when the single-root rule is violated
    });

    IT("a mismatched closing tag is an error", {
        const auto Result = ParseSource(R"(render { <Frame></Button> })");
        ASSERT_FALSE(Result.Errors.empty()); // a closing tag that doesn't match its opening tag is a parse error
    });

    IT("the PartyScreen worked example parses end to end", {
        // The full spec §9 self-check example (props, state, an event handler, a
        // conditional, and a keyed list, all via <Slot>), minus the surrounding
        // host-language declarations that are outside any render{} block and so
        // irrelevant to this parser.
        const auto Result = ParseSource(R"(
            render {
                <Frame class="party-screen">
                    <Button label="Details" onPress={[&]() { detailsOpen.set(true); }} />
                    <Slot>
                        {[&]() -> Component {
                            if (!detailsOpen.get()) return nullptr;
                            return <Frame class="details-panel">
                                <Slot>
                                    {[&]() -> std::vector<Component> {
                                        std::vector<Component> rows;
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
        ASSERT_TRUE(Result.Errors.empty()); // the spec §9 PartyScreen example parses with no errors
        REQUIRE_EQUAL(Result.Blocks.size(), static_cast<std::size_t>(1)); // the PartyScreen example is one render block

        const auto& Root = Result.Blocks[0].Root;
        ASSERT_TRUE(Root.Tag == "Frame" && Root.Children.size() == 2);
        // root Frame has two children: the Button and the outer Slot
    });

    IT("a .irisx FilePath routes through NyxTokenizer, not CppTokenizer", {
        // A Nyx template-string literal (backtick-delimited) whose `${ }` interpolation
        // contains a stray, unbalanced '}' with no matching '{' before it. NyxTokenizer
        // (per its own doc comment) captures the whole backtick-to-backtick span as one
        // opaque StringLiteral token, so this inner '}' never reaches the escape hatch's
        // brace balancer. CppTokenizer has no notion of backtick strings at all — it sees
        // the stray '}' as a bare CloseBrace token, which closes the `content={ }` escape
        // hatch prematurely and desyncs the rest of the parse. Same source, only the
        // FilePath extension differs — this is the one place TokenizerFactory's dispatch
        // (docs/next-steps.md's "NyxTokenizer... PARTIALLY RESOLVED" entry) is actually
        // observable from RenderBlockParser's output rather than just from a unit test
        // against NyxTokenizer in isolation.
        constexpr std::string_view Source = R"(render { <Text content={`hi}there`} /> })";

        const auto CppResult = ParseSourceAs(Source, "test.iris");
        ASSERT_FALSE(CppResult.Errors.empty());
        // CppTokenizer treats the backtick as ordinary text and the stray '}' inside the
        // template string as a real CloseBrace, ending the escape hatch early and leaving
        // trailing text ("there`") where a new attribute or '/>' was expected.

        const auto NyxResult = ParseSourceAs(Source, "test.irisx");
        ASSERT_TRUE(NyxResult.Errors.empty());
        // NyxTokenizer swallows the whole `hi}there` template string as one token, so the
        // escape hatch closes correctly at the real trailing '}' and the element parses clean.
        REQUIRE_EQUAL(NyxResult.Blocks.size(), static_cast<std::size_t>(1));
        ASSERT_TRUE(NyxResult.Blocks[0].Root.Tag == "Text");
    });

    IT("a .irisx !{ } body preserves whitespace between adjacent identifiers", {
        // NyxTokenizer's underlying nyx::Lexer silently consumes whitespace and never
        // surfaces it as a token of its own (NyxTokenizer's own doc comment).
        // ParseJsxEscapeHatch reconstructs raw text token-by-token via
        // AppendText/PrecededByWhitespace, so two adjacent identifiers only keep their
        // source-level gap if Advance()'s whitespace detection works for a tokenizer that
        // never emits a whitespace token -- this is the exact `() -> { return count; }`
        // repro from docs/next-steps.md's "Codegen has no Nyx-target emission" entry,
        // which (at the time it was filed) came back as "returncount" with the gap lost.
        constexpr std::string_view Source = R"(render { <Slot> !{ () -> { return count; } } </Slot> })";

        const auto Result = ParseSourceAs(Source, "test.irisx");
        ASSERT_TRUE(Result.Errors.empty());
        REQUIRE_TRUE(!Result.Blocks.empty());

        const Iris::PropValue& Value = *Result.Blocks[0].Root.Children[0].EscapeHatch;
        REQUIRE_EQUAL(Value.JsxSegments.size(), static_cast<std::size_t>(1));
        ASSERT_TRUE(Value.JsxSegments[0].Text.find("return count") != std::string::npos);
        // adjacent identifiers keep their source-level space -- not "returncount"
    });
});
