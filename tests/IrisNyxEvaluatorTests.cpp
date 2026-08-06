#include "cimmerian/test.hpp"

#include "Iris/IrisNyxEvaluator.h"

#include "Iris/IrisIr.h"
#include "Iris/NyxSignalDecorator.h"
#include "Iris/RenderBlockParser.h"

#include <amanuensis/json.hpp>

#include <string>
#include <variant>

namespace {

using namespace Iris;

// Builds a real IrisIrDocument the same way iris_cc would for a real .irisx file --
// through the actual NyxTokenizer/RenderBlockParser/BuildIrisIr/ParseIrisIrDocument
// pipeline, not a hand-built fixture. `.irisx` (the real extension RenderBlockParser
// dispatches on to pick NyxTokenizer) is used as the fake FilePath throughout.
IrisIrDocument BuildRealDocument(std::string_view Source) {
    RenderBlockParser::Result ParseResult = RenderBlockParser(Source, "test.irisx").Parse();
    Amanuensis::Value          Json = BuildIrisIr(Source, "test.irisx", {}, {}, ParseResult);
    IrisIrDocumentParseResult   Parsed = ParseIrisIrDocument(Json);
    return std::move(*Parsed.Document);
}

const IrRenderBlockNode& OnlyRenderBlock(const IrisIrDocument& Document) {
    for (const IrBodyNode& Node : Document.Body) {
        if (const auto* Block = std::get_if<IrRenderBlockNode>(&Node)) {
            return *Block;
        }
    }
    throw std::runtime_error("no render_block in document");
}

} // namespace

DESCRIBE("IrisNyxEvaluator", {
    IT("ReconstructNyxSource splices an empty block for a render_block, preserving the rest verbatim", {
        const std::string_view Source =
            "void Button(bool startHovered) {\n"
            "    @signal bool isHovered = startHovered;\n"
            "\n"
            "    render {\n"
            "        <Frame class=\"button\" />\n"
            "    }\n"
            "}\n";
        const IrisIrDocument Document = BuildRealDocument(Source);
        const std::string      Reconstructed = ReconstructNyxSource(Document);

        ASSERT_TRUE(Reconstructed.find("void Button(bool startHovered)") != std::string::npos);
        ASSERT_TRUE(Reconstructed.find("@signal bool isHovered = startHovered;") != std::string::npos);
        ASSERT_TRUE(Reconstructed.find("render") == std::string::npos); // the render block itself is gone
        ASSERT_TRUE(Reconstructed.find("{}") != std::string::npos);     // ... replaced with an empty block
    });

    IT("a plain (no-JSX) prop expression evaluates for real, typed by the target prop", {
        const std::string_view Source =
            "void Frame1(float score) {\n"
            "    render { <Scroll wheelStep={score} /> }\n"
            "}\n";
        const IrisIrDocument Document = BuildRealDocument(Source);

        nyx::host::NyxRuntime Runtime;
        auto FileScope = Runtime.CreateScope(ReconstructNyxSource(Document), "test.irisx");
        auto Invocation = Runtime.InvokeComponent(FileScope, "Frame1", {nyx::runtime::Value(42.0f)});

        ChaosSlotMarker Marker;
        std::vector<IrisIrRuntimeError> Errors;
        NyxEvaluator Eval = MakeNyxEvaluator(Runtime, Invocation, Marker, nullptr, &Errors);

        const Component Result = ConvertIrElement(OnlyRenderBlock(Document).Root, Eval, &Errors);
        REQUIRE_TRUE(Errors.empty());
        ASSERT_TRUE(std::get<float>(Result.Props.at("wheelStep")) == 42.0f);
    });

    // chaos-ir-spec.md §4's own worked example, end to end: a JSX-transform ternary inside
    // a <Slot>, evaluated against real per-invocation state via nyx-proto's
    // CreateScope/InvokeComponent/EvaluateInScope -- docs/iris_nyx_evaluator_scope_gap.md's
    // resolution, exercised for real rather than through a mock evaluator.
    IT("a <Slot> ternary picks the true branch when the @signal is true", {
        const std::string_view Source =
            "void Button(bool startHovered) {\n"
            "    @signal bool isHovered = startHovered;\n"
            "\n"
            "    render {\n"
            "        <Frame class=\"button\">\n"
            "            <Slot>\n"
            "                !{() -> isHovered ? <Frame class=\"hovered\" /> : <Frame class=\"normal\" />}\n"
            "            </Slot>\n"
            "        </Frame>\n"
            "    }\n"
            "}\n";
        const IrisIrDocument Document = BuildRealDocument(Source);

        nyx::host::NyxRuntime Runtime;
        iris::RegisterSignalDecorator(Runtime);
        ChaosSlotMarker Marker;
        Marker.RegisterOn(Runtime);

        auto FileScope = Runtime.CreateScope(ReconstructNyxSource(Document), "test.irisx");
        auto Invocation = Runtime.InvokeComponent(FileScope, "Button", {nyx::runtime::Value(true)});

        std::vector<IrisIrRuntimeError> Errors;
        NyxEvaluator Eval = MakeNyxEvaluator(Runtime, Invocation, Marker, nullptr, &Errors);
        const Component Result = ConvertIrElement(OnlyRenderBlock(Document).Root, Eval, &Errors);
        REQUIRE_TRUE(Errors.empty());

        // Result is the outer <Frame class="button">; its sole child is the <Slot>.
        REQUIRE_EQUAL(Result.Children.size(), static_cast<std::size_t>(1));
        const Component& Slot = Result.Children[0];
        REQUIRE_TRUE(Slot.SlotCallable != nullptr);
        REQUIRE_TRUE(std::holds_alternative<std::function<std::vector<Component>()>>(Slot.SlotCallable->Callable));
        const std::vector<Component> Output =
            std::get<std::function<std::vector<Component>()>>(Slot.SlotCallable->Callable)();
        REQUIRE_EQUAL(Output.size(), static_cast<std::size_t>(1));
        ASSERT_TRUE(std::get<std::string>(Output[0].Props.at("class")) == "hovered");
    });

    IT("the same <Slot> ternary picks the false branch when the @signal is false", {
        const std::string_view Source =
            "void Button(bool startHovered) {\n"
            "    @signal bool isHovered = startHovered;\n"
            "\n"
            "    render {\n"
            "        <Frame class=\"button\">\n"
            "            <Slot>\n"
            "                !{() -> isHovered ? <Frame class=\"hovered\" /> : <Frame class=\"normal\" />}\n"
            "            </Slot>\n"
            "        </Frame>\n"
            "    }\n"
            "}\n";
        const IrisIrDocument Document = BuildRealDocument(Source);

        nyx::host::NyxRuntime Runtime;
        iris::RegisterSignalDecorator(Runtime);
        ChaosSlotMarker Marker;
        Marker.RegisterOn(Runtime);

        auto FileScope = Runtime.CreateScope(ReconstructNyxSource(Document), "test.irisx");
        auto Invocation = Runtime.InvokeComponent(FileScope, "Button", {nyx::runtime::Value(false)});

        std::vector<IrisIrRuntimeError> Errors;
        NyxEvaluator Eval = MakeNyxEvaluator(Runtime, Invocation, Marker, nullptr, &Errors);
        const Component Result = ConvertIrElement(OnlyRenderBlock(Document).Root, Eval, &Errors);
        REQUIRE_TRUE(Errors.empty());

        REQUIRE_EQUAL(Result.Children.size(), static_cast<std::size_t>(1));
        const Component& Slot = Result.Children[0];
        REQUIRE_TRUE(Slot.SlotCallable != nullptr);
        const std::vector<Component> Output =
            std::get<std::function<std::vector<Component>()>>(Slot.SlotCallable->Callable)();
        REQUIRE_EQUAL(Output.size(), static_cast<std::size_t>(1));
        ASSERT_TRUE(std::get<std::string>(Output[0].Props.at("class")) == "normal");
    });

    IT("two invocations of the same component keep independent @signal state", {
        const std::string_view Source =
            "void Button(bool startHovered) {\n"
            "    @signal bool isHovered = startHovered;\n"
            "\n"
            "    render {\n"
            "        <Slot>\n"
            "            !{() -> isHovered ? <Frame class=\"hovered\" /> : <Frame class=\"normal\" />}\n"
            "        </Slot>\n"
            "    }\n"
            "}\n";
        const IrisIrDocument Document = BuildRealDocument(Source);

        nyx::host::NyxRuntime Runtime;
        iris::RegisterSignalDecorator(Runtime);
        ChaosSlotMarker Marker;
        Marker.RegisterOn(Runtime);
        auto FileScope = Runtime.CreateScope(ReconstructNyxSource(Document), "test.irisx");

        auto First = Runtime.InvokeComponent(FileScope, "Button", {nyx::runtime::Value(true)});
        auto Second = Runtime.InvokeComponent(FileScope, "Button", {nyx::runtime::Value(false)});

        std::vector<IrisIrRuntimeError> Errors;
        NyxEvaluator FirstEval = MakeNyxEvaluator(Runtime, First, Marker, nullptr, &Errors);
        NyxEvaluator SecondEval = MakeNyxEvaluator(Runtime, Second, Marker, nullptr, &Errors);

        const Component FirstResult = ConvertIrElement(OnlyRenderBlock(Document).Root, FirstEval, &Errors);
        const Component SecondResult = ConvertIrElement(OnlyRenderBlock(Document).Root, SecondEval, &Errors);
        REQUIRE_TRUE(Errors.empty());
        REQUIRE_TRUE(FirstResult.SlotCallable != nullptr);
        REQUIRE_TRUE(SecondResult.SlotCallable != nullptr);

        const auto FirstOutput =
            std::get<std::function<std::vector<Component>()>>(FirstResult.SlotCallable->Callable)();
        const auto SecondOutput =
            std::get<std::function<std::vector<Component>()>>(SecondResult.SlotCallable->Callable)();
        ASSERT_TRUE(std::get<std::string>(FirstOutput[0].Props.at("class")) == "hovered");
        ASSERT_TRUE(std::get<std::string>(SecondOutput[0].Props.at("class")) == "normal");
    });

    IT("an event-handler prop re-evaluates against the persistent invocation scope on every call", {
        const std::string_view Source =
            "void Button(bool startHovered) {\n"
            "    @signal bool isHovered = startHovered;\n"
            "\n"
            "    render {\n"
            // Nyx's lambda expression-body (no LBrace after `->`) parses via
            // Parser::ParseExpression(), which does not include assignment -- an
            // assignment lambda body needs the block form (functions-and-control-flow.md
            // §10.2's own distinction, confirmed directly against Parser::ParseParenOrLambda).
            // chaos-ir-spec.md §3.6's own worked example ("() -> isHovered = true") is not
            // actually parseable as written for this reason -- a spec/implementation gap
            // outside this test's own scope, worked around here with the valid block form.
            "        <Frame onPress={() -> { isHovered = true; }}>\n"
            "            <Slot>\n"
            "                !{() -> isHovered ? <Frame class=\"hovered\" /> : <Frame class=\"normal\" />}\n"
            "            </Slot>\n"
            "        </Frame>\n"
            "    }\n"
            "}\n";
        const IrisIrDocument Document = BuildRealDocument(Source);

        nyx::host::NyxRuntime Runtime;
        iris::RegisterSignalDecorator(Runtime);
        ChaosSlotMarker Marker;
        Marker.RegisterOn(Runtime);
        auto FileScope = Runtime.CreateScope(ReconstructNyxSource(Document), "test.irisx");
        auto Invocation = Runtime.InvokeComponent(FileScope, "Button", {nyx::runtime::Value(false)});

        std::vector<IrisIrRuntimeError> Errors;
        NyxEvaluator Eval = MakeNyxEvaluator(Runtime, Invocation, Marker, nullptr, &Errors);
        const Component Result = ConvertIrElement(OnlyRenderBlock(Document).Root, Eval, &Errors);
        REQUIRE_TRUE(Errors.empty());
        REQUIRE_EQUAL(Result.Children.size(), static_cast<std::size_t>(1));
        const Component& Slot = Result.Children[0];
        REQUIRE_TRUE(Slot.SlotCallable != nullptr);

        // Before the handler fires: isHovered is still false.
        auto SlotOutput = [&]() {
            return std::get<std::function<std::vector<Component>()>>(Slot.SlotCallable->Callable)();
        };
        ASSERT_TRUE(std::get<std::string>(SlotOutput()[0].Props.at("class")) == "normal");

        REQUIRE_TRUE(Result.Props.count("onPress") == 1);
        std::get<std::function<void()>>(Result.Props.at("onPress"))();

        // After firing onPress once: isHovered is now true, and a fresh Slot evaluation
        // (re-invoking EvaluateSlot against the same persistent scope) sees it.
        ASSERT_TRUE(std::get<std::string>(SlotOutput()[0].Props.at("class")) == "hovered");
    });
});
