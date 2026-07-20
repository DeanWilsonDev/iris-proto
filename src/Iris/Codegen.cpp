#include "Iris/Codegen.h"
#include "Iris/CorePrimitives.h"

#include <unordered_map>

namespace Iris {

namespace {

// The closed prop-name → `IrisPropValue` variant-member lookup table from
// docs/iris_props_decision.md. The mapped string is the `std::in_place_type<T>`
// argument codegen emits — not a type it introspects at runtime.
const std::unordered_map<std::string, std::string>& PrimitivePropTypes() {
    static const std::unordered_map<std::string, std::string> Types = {
        {"class", "std::string"},
        {"src", "std::string"},
        {"checked", "bool"},
        {"handle", "iris::TextureHandle"},
        {"onPress", "std::function<void()>"},
        {"onRelease", "std::function<void()>"},
        {"onHover", "std::function<void()>"},
        {"onFocus", "std::function<void()>"},
        {"onChange", "std::function<void()>"},
    };
    return Types;
}

std::string EscapeStringLiteral(std::string_view Text) {
    std::string Result;
    Result.reserve(Text.size() + 2);
    for (const char C : Text) {
        switch (C) {
            case '"':  Result += "\\\""; break;
            case '\\': Result += "\\\\"; break;
            case '\n': Result += "\\n"; break;
            default:   Result += C; break;
        }
    }
    return Result;
}

std::string QuoteLiteral(std::string_view Text) { return "\"" + EscapeStringLiteral(Text) + "\""; }

std::string TagToIrisElementTag(const std::string& Tag) { return "Iris::IrisElementTag::" + Tag; }

class ComponentEmitter {
public:
    explicit ComponentEmitter(std::vector<CodegenError>& Errors) : Errors_(Errors) {}

    std::string Emit(const ElementNode& Node) {
        std::string Base;
        if (CorePrimitiveTagNames().count(Node.Tag) != 0) {
            if (Node.Tag == "Slot") {
                Base = EmitSlot(Node);
            } else if (Node.Tag == "Text") {
                Base = EmitTextPrimitive(Node);
            } else {
                Base = EmitOrdinaryPrimitive(Node);
            }
        } else {
            Base = EmitComponentInvocation(Node);
        }
        return Node.Key.has_value() ? EmitWithKey(Base, *Node.Key) : Base;
    }

private:
    void AddError(std::string Message, SourceLocation Location) {
        Errors_.push_back(CodegenError{std::move(Message), std::move(Location)});
    }

    // An escape hatch's contents as a bare C++ expression: an opaque `{ }` passes
    // through verbatim and unparsed, same as everywhere else in the preprocessor;
    // a `!{ }` JSX-transform escape hatch splices each nested element's own
    // generated expression back into the surrounding verbatim text
    // (docs/iris_next_steps.md, "Resolved: JSX inside escape hatches").
    std::string EmitEscapeHatchExpression(const PropValue& Value) {
        if (Value.Kind != PropValueKind::JsxEscapeHatch) {
            return Value.Text; // EscapeHatch — verbatim, unparsed
        }
        std::string Result;
        for (const JsxSegment& Segment : Value.JsxSegments) {
            if (Segment.Kind == JsxSegmentKind::RawText) {
                Result += Segment.Text;
            } else {
                Result += Emit(*Segment.Element);
            }
        }
        return Result;
    }

    // One prop value, whichever form the parser captured, as a bare C++ expression —
    // no `IrisPropValue` wrapping. Used both for primitive props (wrapped by the
    // caller via `in_place_type`) and component-invocation props (used as-is against
    // whatever type the real `<Name>Props` field declares — §2.6, host-compiler-
    // checked).
    std::string EmitPropValueExpression(const PropValue& Value) {
        if (Value.Kind == PropValueKind::StringLiteral) {
            return QuoteLiteral(Value.Text);
        }
        return EmitEscapeHatchExpression(Value);
    }

    // `key` (docs/iris_core_spec.md §2.3/§2.4) applies uniformly to every element kind —
    // a primitive's own aggregate-style construction and a component invocation's
    // function call alike — so rather than threading key-emission through every
    // `Emit*` function individually, `Emit()` always computes the base expression first
    // and, only when a key is present, wraps it in an immediately-invoked lambda that
    // sets `Component::Key` on the already-built value before returning it.
    // `IrisPropValue`'s ordinary converting constructor (no `in_place_type` needed) picks
    // the right variant alternative from the key expression's own type, exactly the way
    // `key={item.id}` (an `int`) and `key="bar"` (a `std::string`) both need to work
    // without codegen ever knowing which one it's looking at.
    std::string EmitWithKey(const std::string& BaseExpression, const PropValue& KeyValue) {
        return "[&]() { Iris::Component Node = " + BaseExpression + "; Node.Key = Iris::IrisPropValue(" +
               EmitPropValueExpression(KeyValue) + "); return Node; }()";
    }

    // Builds `Iris::IrisProps{ {"name", Iris::IrisPropValue{std::in_place_type<T>, expr}}, ... }`
    // for a primitive's own declared props (never called for component invocations,
    // which pass prop values straight through to `<Name>Props` instead).
    std::string EmitPrimitiveProps(const ElementNode& Node) {
        std::string Result = "Iris::IrisProps{";
        bool         First = true;
        for (const Prop& P : Node.Props) {
            const auto It = PrimitivePropTypes().find(P.Name);
            if (It == PrimitivePropTypes().end()) {
                AddError("prop '" + P.Name + "' is not a known prop for <" + Node.Tag + ">", P.Value.Location);
                continue;
            }
            if (!First) {
                Result += ", ";
            }
            First = false;
            Result += "{\"" + P.Name + "\", Iris::IrisPropValue{std::in_place_type<" + It->second + ">, " +
                      EmitPropValueExpression(P.Value) + "}}";
        }
        Result += "}";
        return Result;
    }

    // A single child position's contribution as a synthetic `<Text>` `Component`
    // node — for a `Text`/`EscapeHatch` child of a primitive other than `<Text>` itself
    // (docs/iris_stage1_codegen_decision.md, Gap 2).
    std::string EmitSyntheticTextNode(const ElementChild& Child) {
        const std::string Expr =
            Child.Kind == ElementChildKind::Text ? QuoteLiteral(Child.Text) : EmitEscapeHatchExpression(*Child.EscapeHatch);
        return "Iris::Component{Iris::IrisElementTag::Text, "
               "Iris::IrisProps{{\"text\", Iris::IrisPropValue{std::in_place_type<std::string>, " +
               Expr + "}}}, {}, nullptr}";
    }

    // `<Frame>`/`<Grid>`: element children only. `<Inline>`: any mix of element,
    // text, and escape-hatch children (docs/iris_stage2_decision_doc.md §6). `<Image>`:
    // no children at all.
    std::string EmitChildrenList(const ElementNode& Node) {
        const bool AllowsText = Node.Tag == "Inline";
        const bool AllowsAny  = Node.Tag != "Image";

        std::string Result = "{";
        bool         First = true;
        for (const ElementChild& Child : Node.Children) {
            if (!AllowsAny) {
                AddError("<" + Node.Tag + "> cannot have children", Node.Location);
                break;
            }
            std::string ChildExpr;
            if (Child.Kind == ElementChildKind::Element) {
                ChildExpr = Emit(*Child.Element);
            } else if (AllowsText) {
                ChildExpr = EmitSyntheticTextNode(Child);
            } else {
                AddError("<" + Node.Tag + "> cannot have text or interpolated children", Node.Location);
                continue;
            }
            if (!First) {
                Result += ", ";
            }
            First = false;
            Result += ChildExpr;
        }
        Result += "}";
        return Result;
    }

    std::string EmitOrdinaryPrimitive(const ElementNode& Node) {
        return "Iris::Component{" + TagToIrisElementTag(Node.Tag) + ", " + EmitPrimitiveProps(Node) + ", " +
               EmitChildrenList(Node) + ", nullptr}";
    }

    // `<Text>`'s children concatenate into its own `"text"` prop rather than becoming
    // `Children` entries — it's a leaf (docs/iris_stage1_codegen_decision.md, Gap 2).
    std::string EmitTextPrimitive(const ElementNode& Node) {
        std::vector<std::string> Pieces;
        for (const ElementChild& Child : Node.Children) {
            if (Child.Kind == ElementChildKind::Element) {
                AddError("<Text> cannot contain nested elements", Node.Location);
                continue;
            }
            Pieces.push_back(Child.Kind == ElementChildKind::Text ? QuoteLiteral(Child.Text)
                                                    : EmitEscapeHatchExpression(*Child.EscapeHatch));
        }

        std::string TextExpr;
        if (Pieces.empty()) {
            TextExpr = QuoteLiteral("");
        } else {
            TextExpr = Pieces[0];
            for (std::size_t Index = 1; Index < Pieces.size(); ++Index) {
                TextExpr += " + " + Pieces[Index];
            }
        }

        std::string Props = "Iris::IrisProps{";
        bool         First = true;
        for (const Prop& P : Node.Props) {
            const auto It = PrimitivePropTypes().find(P.Name);
            if (It == PrimitivePropTypes().end()) {
                AddError("prop '" + P.Name + "' is not a known prop for <Text>", P.Value.Location);
                continue;
            }
            Props += "{\"" + P.Name + "\", Iris::IrisPropValue{std::in_place_type<" + It->second + ">, " +
                     EmitPropValueExpression(P.Value) + "}}, ";
            First = false;
        }
        (void)First;
        Props += "{\"text\", Iris::IrisPropValue{std::in_place_type<std::string>, " + TextExpr + "}}}";

        return "Iris::Component{Iris::IrisElementTag::Text, " + Props + ", {}, nullptr}";
    }

    // `<Slot>`'s single escape-hatch child becomes a `MakeSlotCallable(...)` call
    // (docs/iris_stage1_codegen_decision.md, Gap 1). Arity/shape are checked here since
    // codegen can't emit the call without them; callability/return type are left to the
    // host compiler inside `MakeSlotCallable` itself.
    std::string EmitSlot(const ElementNode& Node) {
        if (!Node.Props.empty()) {
            AddError("<Slot> accepts no props other than key", Node.Location);
        }
        if (Node.Children.size() != 1 || Node.Children[0].Kind != ElementChildKind::EscapeHatch) {
            AddError("<Slot> must have exactly one { } escape-hatch child", Node.Location);
            return "Iris::Component{Iris::IrisElementTag::Slot, Iris::IrisProps{}, {}, nullptr}";
        }
        const std::string Lambda = EmitEscapeHatchExpression(*Node.Children[0].EscapeHatch);
        return "Iris::Component{Iris::IrisElementTag::Slot, Iris::IrisProps{}, {}, Iris::MakeSlotCallable(" +
               Lambda + ")}";
    }

    // An element tag that isn't a known Core primitive is a component invocation
    // (docs/iris_core_spec.md §2.6): `Name(NameProps{ .prop = expr, ... })`. Children are
    // unsupported — implicit children-forwarding is still an open question (§8).
    std::string EmitComponentInvocation(const ElementNode& Node) {
        if (!Node.Children.empty()) {
            AddError("<" + Node.Tag + "> is a component and cannot have children (implicit children "
                                       "forwarding is not yet supported)",
                      Node.Location);
        }

        std::string Initializer = Node.Tag + "Props{";
        bool         First = true;
        for (const Prop& P : Node.Props) {
            if (!First) {
                Initializer += ", ";
            }
            First = false;
            Initializer += "." + P.Name + " = " + EmitPropValueExpression(P.Value);
        }
        Initializer += "}";

        // Wrapped in iris::MountComponentInstance (docs/iris_signal_lifetime_decision.md)
        // so any IRIS_SIGNAL declaration inside <Name>'s own body allocates against a
        // fresh, heap-owned iris::ComponentInstance rather than living as a stack local
        // that dangles the moment <Name>() returns. Every component invocation gets this,
        // unconditionally — Codegen has no visibility into whether <Name>'s body actually
        // declares any signals, and wrapping a signal-free component is harmless (an
        // empty ComponentInstance, freed immediately on unmount).
        return "iris::MountComponentInstance([&]() -> Iris::Component { return " + Node.Tag + "(" +
               Initializer + "); })";
    }

    std::vector<CodegenError>& Errors_;
};

} // namespace

CodegenResult GenerateComponentExpression(const ElementNode& Root) {
    CodegenResult Result;
    ComponentEmitter Emitter(Result.Errors);
    std::string       Source = Emitter.Emit(Root);
    if (Result.Errors.empty()) {
        Result.Source = std::move(Source);
    }
    return Result;
}

} // namespace Iris
