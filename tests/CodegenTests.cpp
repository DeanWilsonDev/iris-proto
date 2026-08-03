#include "cimmerian/test.hpp"

#include "Iris/Codegen.h"
#include "Iris/RenderBlockParser.h"

#include <string>

namespace {

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

} // namespace

DESCRIBE("Codegen", {
    IT("a simple primitive with a string prop and an escape-hatch prop", {
        const auto Result = Generate(R"(render {
            <Frame class="start-menu" onPress={[&]() { x.set(true); }} />
        })");
        ASSERT_TRUE(Result.Errors.empty()); // Frame with class + onPress codegens with no errors
        ASSERT_TRUE(Contains(Result.Source, "Iris::IrisElementTag::Frame")); // tag is Iris::IrisElementTag::Frame
        ASSERT_TRUE(Contains(Result.Source, "std::in_place_type<std::string>, \"start-menu\""));
        // class is wrapped in_place_type<std::string> with the quoted literal
        ASSERT_TRUE(Contains(Result.Source, "std::in_place_type<std::function<void()>>, [&]() { x.set(true); }"));
        // onPress is wrapped in_place_type<std::function<void()>> with the escape hatch verbatim
    });

    IT("an unknown prop on a primitive is an error", {
        const auto Result = Generate(R"(render { <Frame nonsense="x" /> })");
        ASSERT_FALSE(Result.Errors.empty()); // unknown prop name on a Core primitive is a codegen error
        ASSERT_TRUE(Result.Source.empty());  // no source is emitted when there are codegen errors
    });

    IT("a nested element child recurses", {
        const auto Result = Generate(R"(render {
            <Frame class="outer"><Frame class="inner" /></Frame>
        })");
        ASSERT_TRUE(Result.Errors.empty()); // nested Frame children codegen with no errors
        const std::size_t OuterPos = Result.Source.find("\"outer\"");
        const std::size_t InnerPos = Result.Source.find("\"inner\"");
        ASSERT_TRUE(OuterPos != std::string::npos && InnerPos != std::string::npos && OuterPos < InnerPos);
        // inner Frame's expression is nested inside the outer Frame's Children list
    });

    IT("Image with a child is an error", {
        const auto Result = Generate(R"(render { <Image src="a.png"><Frame /></Image> })");
        ASSERT_FALSE(Result.Errors.empty()); // <Image> (a leaf) with a child is a codegen error
    });

    IT("Icon with an icon prop codegens with no errors", {
        const auto Result = Generate(R"(render { <Icon icon="chevron-down" /> })");
        ASSERT_TRUE(Result.Errors.empty()); // Icon with an icon prop codegens with no errors
        ASSERT_TRUE(Contains(Result.Source, "Iris::IrisElementTag::Icon")); // tag is Iris::IrisElementTag::Icon
        ASSERT_TRUE(Contains(Result.Source, "std::in_place_type<std::string>, \"chevron-down\""));
        // icon is wrapped in_place_type<std::string> with the quoted literal
    });

    IT("Icon with a size prop codegens with no errors", {
        const auto Result = Generate(R"(render { <Icon icon="chevron-down" size={14.0f} /> })");
        ASSERT_TRUE(Result.Errors.empty()); // Icon with a size prop codegens with no errors
        ASSERT_TRUE(Contains(Result.Source, "std::in_place_type<float>, 14.0f"));
        // size is wrapped in_place_type<float> with the escape-hatch expression verbatim
    });

    IT("Icon with a child is an error", {
        const auto Result = Generate(R"(render { <Icon icon="chevron-down"><Frame /></Icon> })");
        ASSERT_FALSE(Result.Errors.empty()); // <Icon> (a leaf) with a child is a codegen error
    });

    IT("Scroll with an element child and a wheelStep prop codegens with no errors", {
        const auto Result = Generate(R"(render { <Scroll wheelStep={24.0f}><Frame class="row" /></Scroll> })");
        ASSERT_TRUE(Result.Errors.empty()); // Scroll with a child and wheelStep codegens with no errors
        ASSERT_TRUE(Contains(Result.Source, "Iris::IrisElementTag::Scroll")); // tag is Iris::IrisElementTag::Scroll
        ASSERT_TRUE(Contains(Result.Source, "std::in_place_type<float>, 24.0f"));
        // wheelStep is wrapped in_place_type<float> with the escape-hatch expression verbatim
        ASSERT_TRUE(Contains(Result.Source, "\"row\"")); // the Frame child's own class made it into the Children list
    });

    IT("Scroll with a literal-text child is an error", {
        const auto Result = Generate(R"(render { <Scroll>hello</Scroll> })");
        ASSERT_FALSE(Result.Errors.empty()); // <Scroll> takes element children only, same as <Frame>
    });

    IT("Input with text and preferredWidth props codegens with no errors", {
        const auto Result = Generate(R"(render { <Input text="hello" preferredWidth={200.0f} /> })");
        ASSERT_TRUE(Result.Errors.empty()); // Input with text + preferredWidth codegens with no errors
        ASSERT_TRUE(Contains(Result.Source, "Iris::IrisElementTag::Input")); // tag is Iris::IrisElementTag::Input
        ASSERT_TRUE(Contains(Result.Source, "std::in_place_type<std::string>, \"hello\""));
        // text is wrapped in_place_type<std::string> with the quoted literal
        ASSERT_TRUE(Contains(Result.Source, "std::in_place_type<float>, 200.0f"));
        // preferredWidth is wrapped in_place_type<float> with the escape-hatch expression verbatim
    });

    IT("Input with a child is an error", {
        const auto Result = Generate(R"(render { <Input text="hello"><Frame /></Input> })");
        ASSERT_FALSE(Result.Errors.empty()); // <Input> (a leaf) with a child is a codegen error
    });

    IT("Frame with a literal-text child is an error", {
        const auto Result = Generate(R"(render { <Frame>hello</Frame> })");
        ASSERT_FALSE(Result.Errors.empty()); // <Frame> with a literal-text child is a codegen error
    });

    IT("Text concatenates mixed children", {
        const auto Result = Generate(R"(render { <Text>Hello {name}!</Text> })");
        ASSERT_TRUE(Result.Errors.empty()); // <Text> with mixed literal/escape-hatch children codegens with no errors
        ASSERT_TRUE(Contains(Result.Source, "\"text\"")); // content lands in a "text" prop
        // RenderBlockParser trims each literal-text run on flush, including the space
        // adjacent to a following escape hatch (docs/iris_stage1_codegen_decision.md notes
        // this concatenation scheme; the trimming itself is pre-existing RenderBlockParser
        // behavior, not something codegen controls) — so "Hello " arrives as "Hello".
        ASSERT_TRUE(Contains(Result.Source, "\"Hello\" + name + \"!\""));
        // literal runs and escape hatches join with '+' in source order
        ASSERT_FALSE(Contains(Result.Source, "Children"));
        // <Text> never uses a literal 'Children' identifier in its own emission
    });

    IT("Text with a nested element child is an error", {
        const auto Result = Generate(R"(render { <Text><Frame /></Text> })");
        ASSERT_FALSE(Result.Errors.empty()); // <Text> with a nested element child is a codegen error
    });

    IT("Inline wraps text children as synthetic Text nodes", {
        const auto Result = Generate(R"(render {
            <Inline>Score: {points}<Frame class="badge" /></Inline>
        })");
        ASSERT_TRUE(Result.Errors.empty()); // <Inline> with mixed children codegens with no errors
        ASSERT_TRUE(Contains(Result.Source, "Iris::IrisElementTag::Text"));
        // a synthetic Text node is emitted for Inline's literal/escape-hatch runs
        ASSERT_TRUE(Contains(Result.Source, "\"badge\"")); // the real nested Frame child is still emitted
    });

    IT("Slot with a single escape-hatch child", {
        const auto Result = Generate(R"(render {
            <Slot>{[&]() -> Component { return nullptr; }}</Slot>
        })");
        ASSERT_TRUE(Result.Errors.empty()); // <Slot> with exactly one escape-hatch child codegens with no errors
        ASSERT_TRUE(Contains(Result.Source, "Iris::MakeSlotCallable([&]() -> Component { return nullptr; })"));
        // the lambda is passed to Iris::MakeSlotCallable verbatim
    });

    IT("Slot with the wrong arity is an error", {
        const auto Result = Generate(R"(render { <Slot></Slot> })");
        ASSERT_FALSE(Result.Errors.empty()); // <Slot> with zero children is a codegen error
    });

    IT("Slot with a non-escape-hatch child is an error", {
        const auto Result = Generate(R"(render { <Slot><Frame /></Slot> })");
        ASSERT_FALSE(Result.Errors.empty());
        // <Slot> with a nested-element child instead of an escape hatch is a codegen error
    });

    IT("a component invocation emits the Name(NameProps{...}) convention", {
        const auto Result = Generate(
            R"(render { <HealthBar current={player.health} max={player.maxHealth} label="HP" /> })");
        ASSERT_TRUE(Result.Errors.empty()); // an imported-component-shaped element codegens with no errors
        ASSERT_TRUE(Contains(Result.Source, "HealthBar(HealthBarProps{"));
        // unrecognised tag becomes a call to Name(NameProps{...}) per the <Name>Props convention
        ASSERT_TRUE(Contains(Result.Source, ".current = player.health")); // escape-hatch prop values pass through verbatim
        ASSERT_TRUE(Contains(Result.Source, ".label = \"HP\"")); // string-literal prop values are quoted verbatim
    });

    IT("a component invocation is wrapped in MountComponentInstance", {
        // docs/iris_signal_lifetime_decision.md: every component invocation is wrapped so
        // any IRIS_SIGNAL declared inside <Name>'s own body allocates against a heap-owned
        // ComponentInstance instead of a stack local.
        const auto Result = Generate(R"(render { <HealthBar current={1} max={2} /> })");
        ASSERT_TRUE(Result.Errors.empty()); // codegens with no errors
        ASSERT_TRUE(Contains(Result.Source, "iris::MountComponentInstance([&]() -> Iris::Component { return "
                                            "HealthBar(HealthBarProps{"));
        // the invocation is wrapped in iris::MountComponentInstance
    });

    IT("a component invocation with children is an error", {
        const auto Result = Generate(R"(render { <HealthBar current={1} max={2}><Frame /></HealthBar> })");
        ASSERT_FALSE(Result.Errors.empty());
        // a component element with children is a codegen error (implicit children forwarding unimplemented)
    });

    IT("the PartyScreen example's outer level codegens", {
        // The spec §9 PartyScreen example, one level deep: RenderBlockParser captures a
        // <Slot>'s escape-hatch body verbatim, including any JSX-looking text inside it
        // (docs/iris_core_spec.md §1.4 — tested directly in RenderBlockParserTests.cpp's
        // "an escape hatch containing angle brackets is opaque" test). Codegen therefore
        // only transforms what's structurally outside escape hatches; this test covers
        // exactly that boundary, not the nested <Frame> inside the lambda.
        const auto Result = Generate(R"(render {
            <Frame class="party-screen">
                <Button label="Details" onPress={[&]() { detailsOpen.set(true); }} />
                <Slot>
                    {[&]() -> Component {
                        if (!detailsOpen.get()) return nullptr;
                        return <Frame class="details-panel"></Frame>;
                    }}
                </Slot>
            </Frame>
        })");
        ASSERT_TRUE(Result.Errors.empty()); // PartyScreen's outer structure codegens with no errors
        ASSERT_TRUE(Contains(Result.Source, "Button(ButtonProps{")); // Button is emitted as a component invocation
        ASSERT_TRUE(Contains(Result.Source, "Iris::MakeSlotCallable")); // the outer Slot is emitted with MakeSlotCallable
        ASSERT_TRUE(Contains(Result.Source, "return <Frame class=\"details-panel\"></Frame>;"));
        // the Slot's escape-hatch body — JSX and all — passes through verbatim, unparsed
    });

    IT("a !{ } JSX-transform escape hatch splices in the generated nested element", {
        // Same shape as the outer-level PartyScreen test above, but with `!{ }` instead
        // of `{ }` — this time the nested <Frame> JSX must actually be transformed into
        // a real Iris::Component-constructing expression and spliced back into
        // the surrounding lambda text (docs/archive/iris_next_steps.md, "Resolved: JSX inside
        // escape hatches"), not passed through verbatim.
        const auto Result = Generate(R"(render {
            <Slot>
                !{[&]() -> Component {
                    if (!detailsOpen.get()) return nullptr;
                    return <Frame class="details-panel"></Frame>;
                }}
            </Slot>
        })");
        ASSERT_TRUE(Result.Errors.empty()); // <Slot> with a !{ } JSX-transform escape hatch codegens with no errors
        ASSERT_TRUE(Contains(Result.Source, "Iris::MakeSlotCallable")); // the Slot is emitted with MakeSlotCallable
        ASSERT_FALSE(Contains(Result.Source, "<Frame")); // the raw <Frame ...> JSX text is gone from the generated source
        ASSERT_TRUE(Contains(Result.Source, "Iris::Component{Iris::IrisElementTag::Frame"));
        // the nested <Frame> was transformed into a real Iris::Component-constructing expression
        ASSERT_TRUE(Contains(Result.Source, "if (!detailsOpen.get()) return nullptr;") &&
                    Contains(Result.Source, "return Iris::Component{Iris::IrisElementTag::Frame"));
        // the surrounding lambda text is preserved around the spliced-in expression
    });

    IT("the full PartyScreen example codegens with !{ } JSX-transform escape hatches", {
        // The full spec §9 PartyScreen example, this time written with `!{ }` instead
        // of `{ }` for both <Slot>s (docs/iris_escape_hatch_decision.md) — unlike the
        // outer-level-only test above, every nested <Frame>/<HealthBar> is expected to
        // be actually transformed, not passed through as raw JSX text.
        const auto Result = Generate(R"(render {
            <Frame class="party-screen">
                <Button label="Details" onPress={[&]() { detailsOpen.set(true); }} />
                <Slot>
                    !{[&]() -> Component {
                        if (!detailsOpen.get()) return nullptr;
                        return <Frame class="details-panel">
                            <Slot>
                                !{[&]() -> std::vector<Component> {
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
        })");
        ASSERT_TRUE(Result.Errors.empty()); // the two-level !{ } PartyScreen example codegens with no errors
        ASSERT_TRUE(!Contains(Result.Source, "<Frame") && !Contains(Result.Source, "<HealthBar") &&
                    !Contains(Result.Source, "<Slot"));
        // no raw JSX text survives anywhere in the output, including the nested list-rendering Slot
        ASSERT_TRUE(Contains(Result.Source, "HealthBar(HealthBarProps{"));
        // the innermost <HealthBar> component invocation was generated

        // Every <Frame> in the source (root, details-panel, and the per-row Frame
        // inside the list) must have become a real Component-constructing
        // expression, not opaque text.
        std::size_t FrameExpressionCount = 0;
        std::size_t SearchPos = 0;
        while ((SearchPos = Result.Source.find("Iris::IrisElementTag::Frame", SearchPos)) != std::string::npos) {
            ++FrameExpressionCount;
            SearchPos += 1;
        }
        ASSERT_EQUAL(FrameExpressionCount, static_cast<std::size_t>(3));
        // all three <Frame> elements (root, details-panel, party-row) were transformed
    });

    IT("a keyed primitive wraps the base expression and sets the key", {
        const auto Result = Generate(R"(render { <Frame key={member.id} class="party-row" /> })");
        ASSERT_TRUE(Result.Errors.empty()); // a keyed primitive codegens with no errors
        ASSERT_TRUE(Contains(Result.Source, "[&]() { Iris::Component Node = Iris::Component{Iris::IrisElementTag::"
                                            "Frame,"));
        // the base primitive expression is wrapped in the key-setting IIFE
        ASSERT_TRUE(Contains(Result.Source, "Node.Key = Iris::IrisPropValue(member.id); return Node; }()"));
        // the key expression passes through verbatim into Iris::IrisPropValue's converting constructor
    });

    IT("a keyed component invocation also wraps with the key", {
        const auto Result = Generate(R"(render { <HealthBar key={member.id} current={1} max={2} /> })");
        ASSERT_TRUE(Result.Errors.empty()); // a keyed component invocation codegens with no errors
        ASSERT_TRUE(Contains(Result.Source, "[&]() { Iris::Component Node = iris::MountComponentInstance([&]() -> "
                                            "Iris::Component { return HealthBar(HealthBarProps{"));
        // the base component-invocation call (itself wrapped in iris::MountComponentInstance,
        // docs/iris_signal_lifetime_decision.md) is wrapped in the key-setting IIFE the same way
        // a primitive's is — key handling is uniform across every element kind
        ASSERT_TRUE(Contains(Result.Source, "Node.Key = Iris::IrisPropValue(member.id); return Node; }()"));
        // and the key is set on the invocation's returned Component afterward
    });

    IT("an unkeyed element gets no IIFE wrapping", {
        const auto Result = Generate(R"(render { <Frame class="a" /> })");
        ASSERT_FALSE(Contains(Result.Source, "Node.Key")); // an element with no key prop gets no IIFE wrapping at all
    });

    IT("a ref'd primitive wraps the base expression and sets the ref", {
        const auto Result = Generate(R"(render { <Icon ref="trigger-icon" class="party-row" /> })");
        ASSERT_TRUE(Result.Errors.empty()); // a ref'd primitive codegens with no errors
        ASSERT_TRUE(Contains(Result.Source, "[&]() { Iris::Component Node = Iris::Component{Iris::IrisElementTag::"
                                            "Icon,"));
        // the base primitive expression is wrapped in the ref-setting IIFE
        ASSERT_TRUE(Contains(Result.Source, "Node.Ref = Iris::IrisPropValue(\"trigger-icon\"); return Node; }()"));
        // the ref string literal is quoted the same way any other string prop value is
    });

    IT("a ref'd component invocation also wraps with the ref", {
        const auto Result = Generate(R"(render { <HealthBar ref="hb" current={1} max={2} /> })");
        ASSERT_TRUE(Result.Errors.empty()); // a ref'd component invocation codegens with no errors
        ASSERT_TRUE(Contains(Result.Source, "[&]() { Iris::Component Node = iris::MountComponentInstance([&]() -> "
                                            "Iris::Component { return HealthBar(HealthBarProps{"));
        // the base component-invocation call is wrapped in the ref-setting IIFE the same way a
        // primitive's is — ref handling is uniform across every element kind, mirroring key
        ASSERT_TRUE(Contains(Result.Source, "Node.Ref = Iris::IrisPropValue(\"hb\"); return Node; }()"));
    });

    IT("an unref'd element gets no ref IIFE wrapping", {
        const auto Result = Generate(R"(render { <Frame class="a" /> })");
        ASSERT_FALSE(Contains(Result.Source, "Node.Ref")); // an element with no ref prop gets no IIFE wrapping at all
    });

    IT("a keyed and ref'd primitive composes both IIFE wraps", {
        const auto Result = Generate(R"(render { <Frame key={member.id} ref="row" class="a" /> })");
        ASSERT_TRUE(Result.Errors.empty()); // key + ref together codegen with no errors
        ASSERT_TRUE(Contains(Result.Source, "Node.Key = Iris::IrisPropValue(member.id); return Node; }()"));
        ASSERT_TRUE(Contains(Result.Source, "Node.Ref = Iris::IrisPropValue(\"row\"); return Node; }()"));
        // both wraps are present -- key applied first, ref wrapped around it
    });

    IT("a Native with a build escape hatch codegens with no errors", {
        const auto Result =
            Generate(R"(render { <Native build={[&]() { return buildTreeRowWidget(node, app, theme); }} /> })");
        ASSERT_TRUE(Result.Errors.empty()); // <Native> with a build prop codegens with no errors
        ASSERT_TRUE(Contains(Result.Source, "Iris::IrisElementTag::Native"));
        ASSERT_TRUE(Contains(Result.Source, "Iris::MakeNativeBuilder([&]() { return buildTreeRowWidget(node, app, "
                                            "theme); })"));
        // the build prop's escape hatch is passed straight to MakeNativeBuilder, not wrapped
        // as an ordinary IrisProps entry
        ASSERT_FALSE(Contains(Result.Source, "Iris::IrisProps{{\"build\""));
    });

    IT("a Native with no build prop is an error", {
        const auto Result = Generate(R"(render { <Native /> })");
        ASSERT_FALSE(Result.Errors.empty()); // <Native> with no build prop is a codegen error
    });

    IT("a Native with a string-literal build prop is an error", {
        const auto Result = Generate(R"(render { <Native build="not-a-lambda" /> })");
        ASSERT_FALSE(Result.Errors.empty()); // build must be a { } escape hatch, not a string literal
    });

    IT("a Native with a child is an error", {
        const auto Result = Generate(R"(render { <Native build={[&]() { return x; }}><Frame /></Native> })");
        ASSERT_FALSE(Result.Errors.empty()); // <Native> is a leaf -- a child is a codegen error
    });

    IT("a Native with an unknown prop is an error", {
        const auto Result = Generate(R"(render { <Native build={[&]() { return x; }} nonsense="y" /> })");
        ASSERT_FALSE(Result.Errors.empty()); // only `build` is a known prop for <Native>
    });

    IT("a Split with exactly two children codegens with no errors", {
        const auto Result = Generate(R"(render {
            <Split axis="horizontal" ratio={0.3f}><Frame class="left" /><Frame class="right" /></Split>
        })");
        ASSERT_TRUE(Result.Errors.empty()); // <Split> with two element children codegens with no errors
        ASSERT_TRUE(Contains(Result.Source, "Iris::IrisElementTag::Split"));
        ASSERT_TRUE(Contains(Result.Source, "std::in_place_type<std::string>, \"horizontal\""));
        ASSERT_TRUE(Contains(Result.Source, "std::in_place_type<float>, 0.3f"));
        const std::size_t LeftPos  = Result.Source.find("\"left\"");
        const std::size_t RightPos = Result.Source.find("\"right\"");
        ASSERT_TRUE(LeftPos != std::string::npos && RightPos != std::string::npos && LeftPos < RightPos);
        // both panes are ordinary Children entries, leading pane first
    });

    IT("a Split with one child is an error", {
        const auto Result = Generate(R"(render { <Split><Frame class="only" /></Split> })");
        ASSERT_FALSE(Result.Errors.empty()); // <Split> requires exactly two children, not one
    });

    IT("a Split with three children is an error", {
        const auto Result = Generate(R"(render { <Split><Frame /><Frame /><Frame /></Split> })");
        ASSERT_FALSE(Result.Errors.empty()); // <Split> requires exactly two children, not three
    });

    IT("a Split with zero children is an error", {
        const auto Result = Generate(R"(render { <Split /> })");
        ASSERT_FALSE(Result.Errors.empty()); // <Split> requires exactly two children, not zero
    });
});
