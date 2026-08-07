#include "cimmerian/test.hpp"

#include "Iris/IrisIrRuntime.h"

#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace Iris;

// --- IrElementNode/IrPropNode/IrElementChild fixture builders -- hand-constructed rather than
// round-tripped through BuildIrisIr/ParseIrisIrDocument (IrisIrDocumentTests.cpp already covers
// that path): these tests are about ConvertIrElement/WalkIrisIrDocument's own behavior, which
// only needs a well-formed IR tree, not a real .irisx source string to produce one from. ---

IrPropNode MakeLiteralProp(std::string Name, std::string Value) {
    IrPropNode P;
    P.Name = std::move(Name);
    P.Value.IsLiteral = true;
    P.Value.Literal.Value = std::move(Value);
    return P;
}

IrNyxExpressionSegment TextSegment(std::string Text) {
    IrNyxExpressionSegment Seg;
    Seg.Kind = IrNyxExpressionSegmentKind::Text;
    Seg.Text = std::move(Text);
    return Seg;
}

IrNyxExpressionSegment ElementSegment(IrElementNode Node) {
    IrNyxExpressionSegment Seg;
    Seg.Kind = IrNyxExpressionSegmentKind::Element;
    Seg.Element = std::make_shared<IrElementNode>(std::move(Node));
    return Seg;
}

IrPropNode MakeExprProp(std::string Name, std::string Source) {
    IrPropNode P;
    P.Name = std::move(Name);
    P.Value.IsLiteral = false;
    P.Value.Expression.Segments = {TextSegment(std::move(Source))};
    return P;
}

IrElementChild MakeElementChild(IrElementNode Node) {
    IrElementChild Child;
    Child.Kind = IrElementChildKind::Element;
    Child.Element = std::make_unique<IrElementNode>(std::move(Node));
    return Child;
}

// Builds a fixture whose Segments is a single text segment followed by every given element,
// in that order -- sufficient for existing tests here, none of which depend on more complex
// text/element interleaving (that's covered directly in IrisIrDocumentTests.cpp against the
// real parser/serializer round-trip).
IrElementChild MakeExprChild(std::string Source, std::vector<IrElementNode> NestedChildren = {}) {
    IrElementChild Child;
    Child.Kind = IrElementChildKind::NyxExpression;
    auto Expr = std::make_unique<IrNyxExpressionNode>();
    Expr->Segments.push_back(TextSegment(std::move(Source)));
    for (IrElementNode& Node : NestedChildren) {
        Expr->Segments.push_back(ElementSegment(std::move(Node)));
    }
    Child.Expression = std::move(Expr);
    return Child;
}

IrElementChild MakeTextChild(std::string Value) {
    IrElementChild Child;
    Child.Kind = IrElementChildKind::Text;
    Child.Text.Value = std::move(Value);
    return Child;
}

IrElementNode MakeElement(std::string Tag, std::vector<IrPropNode> Props = {},
                            std::vector<IrElementChild> Children = {}) {
    IrElementNode Node;
    Node.Tag = std::move(Tag);
    Node.Props = std::move(Props);
    Node.Children = std::move(Children);
    return Node;
}

// A mock NyxEvaluator: EvaluateProp returns a typed value based on ExpectedTypeName (echoing
// the expression's own source text for std::string), EvaluateText brackets its source,
// EvaluateSlot calls Convert once per Node.Children entry and returns the list.
NyxEvaluator MakeEvaluator() {
    NyxEvaluator Eval;
    Eval.EvaluateProp = [](const IrNyxExpressionNode& Node, const std::string& ExpectedType) -> Iris::IrisPropValue {
        if (ExpectedType == "float") return Iris::IrisPropValue{1.5f};
        if (ExpectedType == "bool") return Iris::IrisPropValue{true};
        if (ExpectedType == "int") return Iris::IrisPropValue{7};
        if (ExpectedType == "std::function<void()>") {
            return Iris::IrisPropValue{std::function<void()>([]() {})};
        }
        return Iris::IrisPropValue{Node.Source()};
    };
    Eval.EvaluateText = [](const IrNyxExpressionNode& Node) { return "[" + Node.Source() + "]"; };
    Eval.EvaluateSlot = [](const IrNyxExpressionNode& Node, const IrElementConverter& Convert) -> IrisSlotResult {
        std::vector<Iris::Component> Out;
        for (const IrElementNode& Child : Node.Elements()) {
            Out.push_back(Convert(Child));
        }
        return Out;
    };
    return Eval;
}

} // namespace

DESCRIBE("IrisIrRuntime", {
    IT("converts an ordinary primitive's literal string prop and element children", {
        IrElementNode      Node = MakeElement("Frame", {MakeLiteralProp("class", "button")},
                                                {MakeElementChild(MakeElement("Grid"))});
        std::vector<IrisIrRuntimeError> Errors;
        Component                        Result = ConvertIrElement(Node, MakeEvaluator(), &Errors);

        ASSERT_TRUE(Errors.empty());
        ASSERT_TRUE(Result.Tag == IrisElementTag::Frame);
        REQUIRE_TRUE(Result.Props.count("class") == 1);
        ASSERT_TRUE(std::get<std::string>(Result.Props.at("class")) == "button");
        REQUIRE_EQUAL(Result.Children.size(), static_cast<std::size_t>(1));
        ASSERT_TRUE(Result.Children[0].Tag == IrisElementTag::Grid);
    });

    IT("evaluates a nyx_expression prop through the evaluator, typed by the target prop", {
        IrElementNode Node = MakeElement("Scroll", {MakeExprProp("wheelStep", "settings.wheelStep")});
        std::vector<IrisIrRuntimeError> Errors;
        Component                        Result = ConvertIrElement(Node, MakeEvaluator(), &Errors);

        ASSERT_TRUE(Errors.empty());
        ASSERT_TRUE(std::get<float>(Result.Props.at("wheelStep")) == 1.5f);
    });

    IT("reports an unknown prop name rather than silently dropping it", {
        IrElementNode Node = MakeElement("Frame", {MakeLiteralProp("bogus", "x")});
        std::vector<IrisIrRuntimeError> Errors;
        ConvertIrElement(Node, MakeEvaluator(), &Errors);
        REQUIRE_EQUAL(Errors.size(), static_cast<std::size_t>(1));
        ASSERT_TRUE(Errors[0].Message.find("bogus") != std::string::npos);
    });

    IT("reports a literal value used for a non-string prop as a type mismatch", {
        IrElementNode Node = MakeElement("Scroll", {MakeLiteralProp("wheelStep", "3")});
        std::vector<IrisIrRuntimeError> Errors;
        ConvertIrElement(Node, MakeEvaluator(), &Errors);
        REQUIRE_EQUAL(Errors.size(), static_cast<std::size_t>(1));
        ASSERT_TRUE(Errors[0].Message.find("wheelStep") != std::string::npos);
    });

    IT("reports an evaluator result whose type doesn't match the target prop", {
        NyxEvaluator Eval = MakeEvaluator();
        Eval.EvaluateProp = [](const IrNyxExpressionNode&, const std::string&) {
            return Iris::IrisPropValue{std::string("wrong type")}; // always a string, even for "float"
        };
        IrElementNode Node = MakeElement("Scroll", {MakeExprProp("wheelStep", "x")});
        std::vector<IrisIrRuntimeError> Errors;
        ConvertIrElement(Node, Eval, &Errors);
        REQUIRE_EQUAL(Errors.size(), static_cast<std::size_t>(1));
    });

    IT("rejects a non-Element child on a Frame/Grid/Scroll-family primitive", {
        IrElementNode Node = MakeElement("Frame", {}, {MakeTextChild("oops")});
        std::vector<IrisIrRuntimeError> Errors;
        ConvertIrElement(Node, MakeEvaluator(), &Errors);
        ASSERT_FALSE(Errors.empty());
    });

    IT("rejects any children at all on a leaf primitive (Image/Icon/Input)", {
        IrElementNode Node = MakeElement("Image", {}, {MakeElementChild(MakeElement("Frame"))});
        std::vector<IrisIrRuntimeError> Errors;
        ConvertIrElement(Node, MakeEvaluator(), &Errors);
        ASSERT_FALSE(Errors.empty());
    });

    IT("<Split> requires exactly two children", {
        IrElementNode Node = MakeElement("Split", {}, {MakeElementChild(MakeElement("Frame"))});
        std::vector<IrisIrRuntimeError> Errors;
        ConvertIrElement(Node, MakeEvaluator(), &Errors);
        ASSERT_FALSE(Errors.empty());
    });

    IT("<Inline> mixes element, literal-text, and interpolated children -- both text kinds "
       "become synthetic Text components",
       {
           IrElementNode Node =
               MakeElement("Inline", {}, {MakeElementChild(MakeElement("Grid")), MakeTextChild("Hello "),
                                           MakeExprChild("player.score")});
           std::vector<IrisIrRuntimeError> Errors;
           Component                        Result = ConvertIrElement(Node, MakeEvaluator(), &Errors);

           ASSERT_TRUE(Errors.empty());
           REQUIRE_EQUAL(Result.Children.size(), static_cast<std::size_t>(3));
           ASSERT_TRUE(Result.Children[0].Tag == IrisElementTag::Grid);
           ASSERT_TRUE(Result.Children[1].Tag == IrisElementTag::Text);
           ASSERT_TRUE(std::get<std::string>(Result.Children[1].Props.at("text")) == "Hello ");
           ASSERT_TRUE(Result.Children[2].Tag == IrisElementTag::Text);
           ASSERT_TRUE(std::get<std::string>(Result.Children[2].Props.at("text")) == "[player.score]");
       });

    IT("<Text> concatenates its literal and interpolated children into a single text prop", {
        IrElementNode Node = MakeElement("Text", {}, {MakeTextChild("Score: "), MakeExprChild("player.score")});
        std::vector<IrisIrRuntimeError> Errors;
        Component                        Result = ConvertIrElement(Node, MakeEvaluator(), &Errors);

        ASSERT_TRUE(Errors.empty());
        ASSERT_TRUE(Result.Tag == IrisElementTag::Text);
        ASSERT_TRUE(Result.Children.empty());
        ASSERT_TRUE(std::get<std::string>(Result.Props.at("text")) == "Score: [player.score]");
    });

    IT("<Text> rejects a nested element child", {
        IrElementNode Node = MakeElement("Text", {}, {MakeElementChild(MakeElement("Frame"))});
        std::vector<IrisIrRuntimeError> Errors;
        ConvertIrElement(Node, MakeEvaluator(), &Errors);
        ASSERT_FALSE(Errors.empty());
    });

    IT("a literal key/ref round-trips into Component::Key/Ref", {
        IrElementNode Node = MakeElement("Frame");
        Node.Key = IrPropValue{};
        Node.Key->IsLiteral = true;
        Node.Key->Literal.Value = "row-1";
        Node.Ref = IrPropValue{};
        Node.Ref->IsLiteral = true;
        Node.Ref->Literal.Value = "trigger";

        std::vector<IrisIrRuntimeError> Errors;
        Component                        Result = ConvertIrElement(Node, MakeEvaluator(), &Errors);

        REQUIRE_TRUE(Result.Key.has_value());
        ASSERT_TRUE(std::get<std::string>(*Result.Key) == "row-1");
        REQUIRE_TRUE(Result.Ref.has_value());
        ASSERT_TRUE(std::get<std::string>(*Result.Ref) == "trigger");
    });

    IT("<Slot> wraps its callable into a list-returning IrisSlotCallable that Convert drives", {
        IrElementNode SlotNode =
            MakeElement("Slot", {},
                        {MakeExprChild("cond", {MakeElement("Frame"), MakeElement("Grid")})});
        std::vector<IrisIrRuntimeError> Errors;
        Component                        Result = ConvertIrElement(SlotNode, MakeEvaluator(), &Errors);

        ASSERT_TRUE(Errors.empty());
        ASSERT_TRUE(Result.Tag == IrisElementTag::Slot);
        REQUIRE_TRUE(Result.SlotCallable != nullptr);
        REQUIRE_TRUE(std::holds_alternative<std::function<std::vector<Component>()>>(Result.SlotCallable->Callable));

        std::vector<Component> Output = std::get<std::function<std::vector<Component>()>>(Result.SlotCallable->Callable)();
        REQUIRE_EQUAL(Output.size(), static_cast<std::size_t>(2));
        ASSERT_TRUE(Output[0].Tag == IrisElementTag::Frame);
        ASSERT_TRUE(Output[1].Tag == IrisElementTag::Grid);
    });

    IT("<Slot> normalizes a single-Component evaluator result into a one-element list", {
        NyxEvaluator Eval = MakeEvaluator();
        Eval.EvaluateSlot = [](const IrNyxExpressionNode& Node, const IrElementConverter& Convert) -> IrisSlotResult {
            return Convert(Node.Elements().front());
        };
        IrElementNode SlotNode = MakeElement("Slot", {}, {MakeExprChild("cond", {MakeElement("Frame")})});
        std::vector<IrisIrRuntimeError> Errors;
        Component                        Result = ConvertIrElement(SlotNode, Eval, &Errors);

        std::vector<Component> Output = std::get<std::function<std::vector<Component>()>>(Result.SlotCallable->Callable)();
        REQUIRE_EQUAL(Output.size(), static_cast<std::size_t>(1));
        ASSERT_TRUE(Output[0].Tag == IrisElementTag::Frame);
    });

    IT("<Slot> normalizes a None-sentinel single-Component result into an empty list", {
        NyxEvaluator Eval = MakeEvaluator();
        Eval.EvaluateSlot = [](const IrNyxExpressionNode&, const IrElementConverter&) -> IrisSlotResult {
            return Component{nullptr};
        };
        IrElementNode SlotNode = MakeElement("Slot", {}, {MakeExprChild("cond")});
        std::vector<IrisIrRuntimeError> Errors;
        Component                        Result = ConvertIrElement(SlotNode, Eval, &Errors);

        std::vector<Component> Output = std::get<std::function<std::vector<Component>()>>(Result.SlotCallable->Callable)();
        ASSERT_TRUE(Output.empty());
    });

    IT("<Slot> with the wrong child shape reports an error and still returns a Slot-tagged Component", {
        IrElementNode SlotNode = MakeElement("Slot", {}, {MakeTextChild("not an escape hatch")});
        std::vector<IrisIrRuntimeError> Errors;
        Component                        Result = ConvertIrElement(SlotNode, MakeEvaluator(), &Errors);
        ASSERT_FALSE(Errors.empty());
        ASSERT_TRUE(Result.Tag == IrisElementTag::Slot);
        ASSERT_TRUE(Result.SlotCallable == nullptr);
    });

    IT("a <Slot> re-invocation's own conversion error reaches a caller-owned durable Errors vector", {
        NyxEvaluator Eval = MakeEvaluator();
        auto          CallCount = std::make_shared<int>(0);
        Eval.EvaluateSlot = [CallCount](const IrNyxExpressionNode& Node, const IrElementConverter& Convert) -> IrisSlotResult {
            ++*CallCount;
            return Convert(Node.Elements()[*CallCount == 1 ? 0 : 1]);
        };
        IrElementNode SlotNode = MakeElement(
            "Slot", {},
            {MakeExprChild("cond", {MakeElement("Frame"),
                                     MakeElement("Text", {}, {MakeElementChild(MakeElement("Frame"))})})});

        // Owned by the test itself, outliving the initial ConvertIrElement call below -- models
        // IrisNyxDriver::Errors_, the one real production caller this now actually helps.
        std::vector<IrisIrRuntimeError> Errors;
        Component                        Result = ConvertIrElement(SlotNode, Eval, &Errors);
        ASSERT_TRUE(Errors.empty());
        REQUIRE_TRUE(Result.SlotCallable != nullptr);

        auto& Callable = std::get<std::function<std::vector<Component>()>>(Result.SlotCallable->Callable);
        Callable(); // first invocation picks <Frame> -- no error
        ASSERT_TRUE(Errors.empty());
        Callable(); // second invocation picks <Text> containing a nested element -- an error
        ASSERT_FALSE(Errors.empty());
    });

    IT("<Native> is reported as not yet supported", {
        IrElementNode Node = MakeElement("Native", {MakeExprProp("build", "buildIt()")});
        std::vector<IrisIrRuntimeError> Errors;
        Component                        Result = ConvertIrElement(Node, MakeEvaluator(), &Errors);
        ASSERT_FALSE(Errors.empty());
        ASSERT_TRUE(Result.Tag == IrisElementTag::Native);
        ASSERT_TRUE(Result.NativeBuilder == nullptr);
    });

    IT("a non-primitive tag is dispatched to EvaluateComponentInvocation, keyed on top", {
        IrElementNode Node = MakeElement("HealthBar", {MakeExprProp("current", "player.hp")});
        Node.Key = IrPropValue{};
        Node.Key->IsLiteral = true;
        Node.Key->Literal.Value = "hp-bar";

        std::string CalledWithTag;
        NyxEvaluator Eval = MakeEvaluator();
        Eval.EvaluateComponentInvocation = [&](const IrElementNode& N) {
            CalledWithTag = N.Tag;
            return Component({IrisElementTag::Frame, IrisProps{}, {}, nullptr});
        };

        std::vector<IrisIrRuntimeError> Errors;
        Component                        Result = ConvertIrElement(Node, Eval, &Errors);

        ASSERT_TRUE(Errors.empty());
        ASSERT_TRUE(CalledWithTag == "HealthBar");
        REQUIRE_TRUE(Result.Key.has_value());
        ASSERT_TRUE(std::get<std::string>(*Result.Key) == "hp-bar");
    });

    IT("a component invocation with children reports an error", {
        IrElementNode Node = MakeElement("HealthBar", {}, {MakeElementChild(MakeElement("Frame"))});
        NyxEvaluator   Eval = MakeEvaluator();
        Eval.EvaluateComponentInvocation = [](const IrElementNode&) {
            return Component({IrisElementTag::Frame, IrisProps{}, {}, nullptr});
        };
        std::vector<IrisIrRuntimeError> Errors;
        ConvertIrElement(Node, Eval, &Errors);
        ASSERT_FALSE(Errors.empty());
    });

    IT("WalkIrisIrDocument evaluates nyx_source regions and converts every render_block root, in order", {
        std::vector<std::string> SourceLog;
        NyxEvaluator               Eval = MakeEvaluator();
        Eval.EvaluateSource = [&](const IrNyxSourceNode& Node) { SourceLog.push_back(Node.Source); };

        IrisIrDocument Doc;
        IrNyxSourceNode Pre;
        Pre.Source = "PRE";
        IrRenderBlockNode Block;
        Block.Root = MakeElement("Frame");
        Doc.Body.push_back(IrBodyNode{Pre});
        Doc.Body.push_back(IrBodyNode{Block});

        IrisIrWalkResult Result = WalkIrisIrDocument(Doc, Eval);

        REQUIRE_EQUAL(SourceLog.size(), static_cast<std::size_t>(1));
        ASSERT_TRUE(SourceLog[0] == "PRE");
        REQUIRE_EQUAL(Result.RenderRoots.size(), static_cast<std::size_t>(1));
        ASSERT_TRUE(Result.RenderRoots[0].Tag == IrisElementTag::Frame);
        ASSERT_TRUE(Result.Errors.empty());
    });
});
