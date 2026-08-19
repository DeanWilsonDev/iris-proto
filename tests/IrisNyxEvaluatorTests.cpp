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

// A minimal Umbra::IWidget stub -- same shape as SlotRuntimeTests.cpp's own StubWidget, used
// here only as <Native>'s registered builder's own return value; these tests exercise
// EvaluateNative's own name-resolution logic, not what a real backend would do with the widget.
class StubWidget : public Umbra::IWidget {
public:
    void            ApplyPropDiff(const Umbra::IrisPropDiff&) override {}
    std::size_t     GetChildCount() const override { return 0; }
    Umbra::IWidget* GetChildAt(std::size_t) const override { return nullptr; }
    void            InsertChildAt(std::size_t, std::unique_ptr<Umbra::IWidget>) override {}
    std::unique_ptr<Umbra::IWidget> RemoveChildAt(std::size_t) override { return nullptr; }
};

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

    // nyx-scripting-language/decision-log.md §7.4 / docs/archive/iris_nyx_slot_loop_and_reload_gap_resolved.md
    // §1: Array<T>.Map()/.Reduce() are the sanctioned way to write a dynamic list of components
    // inside a <Slot> -- exercised end to end against real nyx-proto Array evaluation, not a mock.
    IT("a <Slot> .Map() with a single-parameter lambda renders one Card per element", {
        const std::string_view Source =
            "void CardList() {\n"
            "    render {\n"
            "        <Slot>\n"
            "            !{() -> {\n"
            "                Array<string> names = [\"Ann\", \"Bo\", \"Cy\"];\n"
            "                return names.Map((string item) -> <Frame class={item} />);\n"
            "            }}\n"
            "        </Slot>\n"
            "    }\n"
            "}\n";
        const IrisIrDocument Document = BuildRealDocument(Source);

        nyx::host::NyxRuntime Runtime;
        iris::RegisterSignalDecorator(Runtime);
        ChaosSlotMarker Marker;
        Marker.RegisterOn(Runtime);
        auto FileScope = Runtime.CreateScope(ReconstructNyxSource(Document), "test.irisx");
        auto Invocation = Runtime.InvokeComponent(FileScope, "CardList", {});

        std::vector<IrisIrRuntimeError> Errors;
        NyxEvaluator Eval = MakeNyxEvaluator(Runtime, Invocation, Marker, nullptr, &Errors);
        const Component Result = ConvertIrElement(OnlyRenderBlock(Document).Root, Eval, &Errors);
        REQUIRE_TRUE(Errors.empty());
        REQUIRE_TRUE(Result.SlotCallable != nullptr);

        const std::vector<Component> Output =
            std::get<std::function<std::vector<Component>()>>(Result.SlotCallable->Callable)();
        REQUIRE_EQUAL(Output.size(), static_cast<std::size_t>(3));
        ASSERT_TRUE(std::get<std::string>(Output[0].Props.at("class")) == "Ann");
        ASSERT_TRUE(std::get<std::string>(Output[1].Props.at("class")) == "Bo");
        ASSERT_TRUE(std::get<std::string>(Output[2].Props.at("class")) == "Cy");
    });

    IT("a <Slot> .Map() with a two-parameter lambda also binds the iteration index", {
        const std::string_view Source =
            "void CardList() {\n"
            "    render {\n"
            "        <Slot>\n"
            "            !{() -> {\n"
            "                Array<string> names = [\"Ann\", \"Bo\"];\n"
            "                return names.Map((string item, int index) -> <Frame class={item} key={index} />);\n"
            "            }}\n"
            "        </Slot>\n"
            "    }\n"
            "}\n";
        const IrisIrDocument Document = BuildRealDocument(Source);

        nyx::host::NyxRuntime Runtime;
        iris::RegisterSignalDecorator(Runtime);
        ChaosSlotMarker Marker;
        Marker.RegisterOn(Runtime);
        auto FileScope = Runtime.CreateScope(ReconstructNyxSource(Document), "test.irisx");
        auto Invocation = Runtime.InvokeComponent(FileScope, "CardList", {});

        std::vector<IrisIrRuntimeError> Errors;
        NyxEvaluator Eval = MakeNyxEvaluator(Runtime, Invocation, Marker, nullptr, &Errors);
        const Component Result = ConvertIrElement(OnlyRenderBlock(Document).Root, Eval, &Errors);
        REQUIRE_TRUE(Errors.empty());
        REQUIRE_TRUE(Result.SlotCallable != nullptr);

        const std::vector<Component> Output =
            std::get<std::function<std::vector<Component>()>>(Result.SlotCallable->Callable)();
        REQUIRE_EQUAL(Output.size(), static_cast<std::size_t>(2));
        ASSERT_TRUE(std::get<std::string>(Output[0].Props.at("class")) == "Ann");
        REQUIRE_TRUE(Output[0].Key.has_value());
        ASSERT_TRUE(std::get<int>(*Output[0].Key) == 0);
        ASSERT_TRUE(std::get<std::string>(Output[1].Props.at("class")) == "Bo");
        REQUIRE_TRUE(Output[1].Key.has_value());
        ASSERT_TRUE(std::get<int>(*Output[1].Key) == 1);
    });

    IT("a <Slot> .Reduce() binds only the element (second) lambda parameter", {
        // Reduce's own accumulated value has no bearing on which Components iris-proto
        // produces (EvaluateSlot reads that back from ChaosSlotMarker's own recorded picks,
        // never from Nyx's own Reduce return value) -- the accumulator here is a plain int
        // solely so the lambda body stays trivially well-typed; `<Frame .../>` is a bare
        // expression-statement purely for its __chaos_slot_pick(N, item) side effect.
        const std::string_view Source =
            "void CardList() {\n"
            "    render {\n"
            "        <Slot>\n"
            "            !{() -> {\n"
            "                Array<string> names = [\"Ann\", \"Bo\"];\n"
            "                return names.Reduce((int acc, string item) -> { <Frame class={item} />; return acc; }, 0);\n"
            "            }}\n"
            "        </Slot>\n"
            "    }\n"
            "}\n";
        const IrisIrDocument Document = BuildRealDocument(Source);

        nyx::host::NyxRuntime Runtime;
        iris::RegisterSignalDecorator(Runtime);
        ChaosSlotMarker Marker;
        Marker.RegisterOn(Runtime);
        auto FileScope = Runtime.CreateScope(ReconstructNyxSource(Document), "test.irisx");
        auto Invocation = Runtime.InvokeComponent(FileScope, "CardList", {});

        std::vector<IrisIrRuntimeError> Errors;
        NyxEvaluator Eval = MakeNyxEvaluator(Runtime, Invocation, Marker, nullptr, &Errors);
        const Component Result = ConvertIrElement(OnlyRenderBlock(Document).Root, Eval, &Errors);
        REQUIRE_TRUE(Errors.empty());
        REQUIRE_TRUE(Result.SlotCallable != nullptr);

        const std::vector<Component> Output =
            std::get<std::function<std::vector<Component>()>>(Result.SlotCallable->Callable)();
        REQUIRE_EQUAL(Output.size(), static_cast<std::size_t>(2));
        ASSERT_TRUE(std::get<std::string>(Output[0].Props.at("class")) == "Ann");
        ASSERT_TRUE(std::get<std::string>(Output[1].Props.at("class")) == "Bo");
    });

    IT("<Native> resolves its build prop's name against a registered builder", {
        const std::string_view Source =
            "void App() {\n"
            "    render {\n"
            "        <Native build={() -> \"stub\"} />\n"
            "    }\n"
            "}\n";
        const IrisIrDocument Document = BuildRealDocument(Source);

        nyx::host::NyxRuntime Runtime;
        ChaosSlotMarker Marker;
        auto FileScope = Runtime.CreateScope(ReconstructNyxSource(Document), "test.irisx");
        auto Invocation = Runtime.InvokeComponent(FileScope, "App", {});

        std::vector<IrisIrRuntimeError> Errors;
        NativeBuilderLookup             Lookup =
            [](const std::string& Name) -> std::function<std::unique_ptr<Umbra::IWidget>(const nyx::runtime::Value&)> {
            if (Name != "stub") {
                return nullptr;
            }
            return [](const nyx::runtime::Value&) -> std::unique_ptr<Umbra::IWidget> {
                return std::make_unique<StubWidget>();
            };
        };
        NyxEvaluator Eval = MakeNyxEvaluator(Runtime, Invocation, Marker, nullptr, &Errors, Lookup);

        const Component Result = ConvertIrElement(OnlyRenderBlock(Document).Root, Eval, &Errors);
        REQUIRE_TRUE(Errors.empty());
        ASSERT_TRUE(Result.Tag == IrisElementTag::Native);
        REQUIRE_TRUE(Result.NativeBuilder != nullptr);
        std::unique_ptr<Umbra::IWidget> Built = Result.NativeBuilder->Build();
        ASSERT_TRUE(Built != nullptr);
    });

    // docs/next-steps.md's "<Native> builders can't receive the invoking element's own props"
    // entry -- the concrete gap pharos-proto's Explorer panel hit: a <Native> element's own
    // non-`build` props (here `label`/`depth`, mirroring that entry's `node`/`depth` example)
    // must reach the registered factory, packed the same way a component invocation's props
    // already are (an ad hoc NyxObject, readable via FindFieldByName), not silently dropped.
    IT("<Native>'s own non-build props reach the registered builder via NativeBuilderLookup's "
       "props-aware factory",
       {
        const std::string_view Source =
            "void App() {\n"
            "    render {\n"
            "        <Native build={() -> \"stub\"} label=\"hello\" depth={1 + 1} />\n"
            "    }\n"
            "}\n";
        const IrisIrDocument Document = BuildRealDocument(Source);

        nyx::host::NyxRuntime Runtime;
        ChaosSlotMarker Marker;
        auto FileScope = Runtime.CreateScope(ReconstructNyxSource(Document), "test.irisx");
        auto Invocation = Runtime.InvokeComponent(FileScope, "App", {});

        std::vector<IrisIrRuntimeError> Errors;
        bool                             FactoryCalled = false;
        NativeBuilderLookup              Lookup =
            [&FactoryCalled](
                const std::string& Name) -> std::function<std::unique_ptr<Umbra::IWidget>(const nyx::runtime::Value&)> {
            if (Name != "stub") {
                return nullptr;
            }
            return [&FactoryCalled](const nyx::runtime::Value& Props) -> std::unique_ptr<Umbra::IWidget> {
                FactoryCalled = true;
                ASSERT_TRUE(Props.Kind() == nyx::runtime::ValueKind::Object);
                const auto* Obj = std::get<std::shared_ptr<nyx::runtime::NyxObject>>(Props.data).get();
                const nyx::runtime::Value* Label = Obj->FindFieldByName("label");
                const nyx::runtime::Value* Depth = Obj->FindFieldByName("depth");
                ASSERT_TRUE(Label != nullptr);
                ASSERT_TRUE(Depth != nullptr);
                if (Label != nullptr) {
                    ASSERT_TRUE(std::get<std::string>(Label->data) == "hello");
                }
                if (Depth != nullptr) {
                    ASSERT_TRUE(std::get<int32_t>(Depth->data) == 2);
                }
                // `build` itself must not leak into the props object handed to the factory --
                // it's consumed by EvaluateNative to resolve the factory, not forwarded again.
                ASSERT_TRUE(Obj->FindFieldByName("build") == nullptr);
                return std::make_unique<StubWidget>();
            };
        };
        NyxEvaluator Eval = MakeNyxEvaluator(Runtime, Invocation, Marker, nullptr, &Errors, Lookup);

        const Component Result = ConvertIrElement(OnlyRenderBlock(Document).Root, Eval, &Errors);
        REQUIRE_TRUE(Errors.empty());
        REQUIRE_TRUE(Result.NativeBuilder != nullptr);

        // The factory is only invoked lazily, on Build() -- same "not yet built until Build()
        // is called" contract the zero-argument form already had (Component.h's own
        // IrisNativeBuilder doc comment).
        ASSERT_FALSE(FactoryCalled);
        std::unique_ptr<Umbra::IWidget> Built = Result.NativeBuilder->Build();
        ASSERT_TRUE(FactoryCalled);
        ASSERT_TRUE(Built != nullptr);
    });

    IT("<Native> reports an error when the build prop names an unregistered builder", {
        const std::string_view Source =
            "void App() {\n"
            "    render {\n"
            "        <Native build={() -> \"missing\"} />\n"
            "    }\n"
            "}\n";
        const IrisIrDocument Document = BuildRealDocument(Source);

        nyx::host::NyxRuntime Runtime;
        ChaosSlotMarker Marker;
        auto FileScope = Runtime.CreateScope(ReconstructNyxSource(Document), "test.irisx");
        auto Invocation = Runtime.InvokeComponent(FileScope, "App", {});

        std::vector<IrisIrRuntimeError> Errors;
        NativeBuilderLookup             Lookup =
            [](const std::string&) -> std::function<std::unique_ptr<Umbra::IWidget>(const nyx::runtime::Value&)> {
            return nullptr;
        };
        NyxEvaluator Eval = MakeNyxEvaluator(Runtime, Invocation, Marker, nullptr, &Errors, Lookup);

        const Component Result = ConvertIrElement(OnlyRenderBlock(Document).Root, Eval, &Errors);
        ASSERT_FALSE(Errors.empty());
        ASSERT_TRUE(Result.NativeBuilder == nullptr);
    });

    IT("<Native> with no NativeBuilderLookup supplied keeps the existing not-yet-supported error", {
        const std::string_view Source =
            "void App() {\n"
            "    render {\n"
            "        <Native build={() -> \"stub\"} />\n"
            "    }\n"
            "}\n";
        const IrisIrDocument Document = BuildRealDocument(Source);

        nyx::host::NyxRuntime Runtime;
        ChaosSlotMarker Marker;
        auto FileScope = Runtime.CreateScope(ReconstructNyxSource(Document), "test.irisx");
        auto Invocation = Runtime.InvokeComponent(FileScope, "App", {});

        std::vector<IrisIrRuntimeError> Errors;
        NyxEvaluator Eval = MakeNyxEvaluator(Runtime, Invocation, Marker, nullptr, &Errors); // no lookup

        const Component Result = ConvertIrElement(OnlyRenderBlock(Document).Root, Eval, &Errors);
        ASSERT_FALSE(Errors.empty());
        ASSERT_TRUE(Result.NativeBuilder == nullptr);
    });
});
