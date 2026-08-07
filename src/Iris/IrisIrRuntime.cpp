#include "Iris/IrisIrRuntime.h"

#include "Iris/CorePrimitives.h"
#include "Iris/TextureHandle.h"

#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>

namespace Iris {

namespace {

void AddError(std::vector<IrisIrRuntimeError>* Errors, std::string Message, IrSourceLocation Location) {
    if (Errors != nullptr) {
        Errors->push_back(IrisIrRuntimeError{std::move(Message), std::move(Location)});
    }
}

Iris::IrisElementTag TagNameToElementTag(const std::string& Tag) {
    static const std::unordered_map<std::string, Iris::IrisElementTag> Map = {
        {"Frame", Iris::IrisElementTag::Frame},   {"Inline", Iris::IrisElementTag::Inline},
        {"Grid", Iris::IrisElementTag::Grid},     {"Image", Iris::IrisElementTag::Image},
        {"Icon", Iris::IrisElementTag::Icon},     {"Text", Iris::IrisElementTag::Text},
        {"Scroll", Iris::IrisElementTag::Scroll}, {"Input", Iris::IrisElementTag::Input},
        {"Slot", Iris::IrisElementTag::Slot},     {"Native", Iris::IrisElementTag::Native},
        {"Split", Iris::IrisElementTag::Split},
    };
    // Only ever called for a Tag already found in CorePrimitiveTagNames() -- the Frame
    // fallback is unreachable in practice, kept only so this has a defined return on every
    // path rather than UB from an unchecked Map.at().
    const auto It = Map.find(Tag);
    return It != Map.end() ? It->second : Iris::IrisElementTag::Frame;
}

bool IrisPropValueMatchesTypeName(const Iris::IrisPropValue& Value, const std::string& TypeName) {
    if (TypeName == "std::string") return std::holds_alternative<std::string>(Value);
    if (TypeName == "int") return std::holds_alternative<int>(Value);
    if (TypeName == "float") return std::holds_alternative<float>(Value);
    if (TypeName == "bool") return std::holds_alternative<bool>(Value);
    if (TypeName == "std::function<void()>") return std::holds_alternative<std::function<void()>>(Value);
    if (TypeName == "std::function<void(std::string)>") {
        return std::holds_alternative<std::function<void(std::string)>>(Value);
    }
    if (TypeName == "iris::TextureHandle") return std::holds_alternative<iris::TextureHandle>(Value);
    return false; // unknown type name -- treat as a mismatch rather than silently accepting it
}

// `key`/`ref` accept any `IrisPropValue` alternative -- same untyped acceptance
// `Codegen.cpp`'s `EmitWithKey`/`EmitWithRef` give them (IrisIrRuntime.h's own doc comment on
// `NyxEvaluator::EvaluateProp`).
Iris::IrisPropValue ConvertKeyRefValue(const IrPropValue& Value, const NyxEvaluator& Evaluator) {
    if (Value.IsLiteral) {
        return Iris::IrisPropValue{Value.Literal.Value};
    }
    return Evaluator.EvaluateProp(Value.Expression, "");
}

// An ordinary primitive prop value: a literal is only valid where the target prop's declared
// type is `std::string` (chaos-ir-spec.md/ElementNode.h: a literal PropNode value is always a
// bare string -- there is no numeric/bool literal kind, so `wheelStep=42` isn't representable
// as a literal at all, only via a `{ }` Nyx expression); anything else must go through
// `Evaluator.EvaluateProp`, whose result is checked against `ExpectedTypeName`.
Iris::IrisPropValue ConvertTypedPropValue(const IrPropValue& Value, const std::string& ExpectedTypeName,
                                            const std::string& PropDescription, const NyxEvaluator& Evaluator,
                                            std::vector<IrisIrRuntimeError>* Errors) {
    if (Value.IsLiteral) {
        if (ExpectedTypeName != "std::string") {
            AddError(Errors,
                      PropDescription + " requires a Nyx expression -- a string literal is only valid for a "
                                          "std::string prop",
                      Value.Literal.Location);
            return Iris::IrisPropValue{std::string{}};
        }
        return Iris::IrisPropValue{Value.Literal.Value};
    }

    Iris::IrisPropValue Result = Evaluator.EvaluateProp(Value.Expression, ExpectedTypeName);
    if (!IrisPropValueMatchesTypeName(Result, ExpectedTypeName)) {
        AddError(Errors, PropDescription + " evaluated to the wrong type (expected " + ExpectedTypeName + ")",
                  Value.Expression.Location);
    }
    return Result;
}

Iris::IrisProps ConvertPrimitiveProps(const IrElementNode& Node, const NyxEvaluator& Evaluator,
                                        std::vector<IrisIrRuntimeError>* Errors) {
    Iris::IrisProps Result;
    for (const IrPropNode& P : Node.Props) {
        const auto It = PrimitivePropTypeNames().find(P.Name);
        if (It == PrimitivePropTypeNames().end()) {
            AddError(Errors, "prop '" + P.Name + "' is not a known prop for <" + Node.Tag + ">", P.Location);
            continue;
        }
        Result[P.Name] =
            ConvertTypedPropValue(P.Value, It->second, "prop '" + P.Name + "' on <" + Node.Tag + ">", Evaluator, Errors);
    }
    return Result;
}

// `<Frame>`/`<Grid>`/`<Scroll>`/`<Split>`: element children only. `<Inline>`: any mix of
// element, text, and interpolated (`nyx_expression`) children. `<Image>`/`<Icon>`/`<Input>`:
// no children at all -- mirrors `Codegen.cpp`'s `EmitChildrenList` exactly.
std::vector<Iris::Component> ConvertChildrenList(const IrElementNode& Node, const NyxEvaluator& Evaluator,
                                                    std::vector<IrisIrRuntimeError>* Errors) {
    const bool AllowsText = Node.Tag == "Inline";
    const bool AllowsAny  = Node.Tag != "Image" && Node.Tag != "Icon" && Node.Tag != "Input";

    std::vector<Iris::Component> Result;
    for (const IrElementChild& Child : Node.Children) {
        if (!AllowsAny) {
            AddError(Errors, "<" + Node.Tag + "> cannot have children", Node.Location);
            break;
        }
        if (Child.Kind == IrElementChildKind::Element) {
            Result.push_back(ConvertIrElement(*Child.Element, Evaluator, Errors));
            continue;
        }
        if (!AllowsText) {
            AddError(Errors, "<" + Node.Tag + "> cannot have text or interpolated children", Node.Location);
            continue;
        }
        const std::string Text = Child.Kind == IrElementChildKind::Text ? Child.Text.Value
                                                                          : Evaluator.EvaluateText(*Child.Expression);
        Result.push_back(Iris::Component{Iris::IrisElementTag::Text,
                                          Iris::IrisProps{{"text", Iris::IrisPropValue{Text}}}, {}, nullptr});
    }
    return Result;
}

// `<Text>`'s children concatenate into its own `"text"` prop rather than becoming `Children`
// entries, matching `Codegen.cpp`'s `EmitTextPrimitive` (it's a leaf, docs/
// iris_stage1_codegen_decision.md Gap 2).
Iris::Component ConvertTextPrimitive(const IrElementNode& Node, const NyxEvaluator& Evaluator,
                                       std::vector<IrisIrRuntimeError>* Errors) {
    std::string TextValue;
    for (const IrElementChild& Child : Node.Children) {
        if (Child.Kind == IrElementChildKind::Element) {
            AddError(Errors, "<Text> cannot contain nested elements", Node.Location);
            continue;
        }
        TextValue += Child.Kind == IrElementChildKind::Text ? Child.Text.Value
                                                              : Evaluator.EvaluateText(*Child.Expression);
    }

    Iris::IrisProps Props;
    for (const IrPropNode& P : Node.Props) {
        const auto It = PrimitivePropTypeNames().find(P.Name);
        if (It == PrimitivePropTypeNames().end()) {
            AddError(Errors, "prop '" + P.Name + "' is not a known prop for <Text>", P.Location);
            continue;
        }
        Props[P.Name] =
            ConvertTypedPropValue(P.Value, It->second, "prop '" + P.Name + "' on <Text>", Evaluator, Errors);
    }
    Props["text"] = Iris::IrisPropValue{TextValue};

    return Iris::Component{Iris::IrisElementTag::Text, std::move(Props), {}, nullptr};
}

Iris::Component ConvertOrdinaryPrimitive(const IrElementNode& Node, const NyxEvaluator& Evaluator,
                                           std::vector<IrisIrRuntimeError>* Errors) {
    return Iris::Component{TagNameToElementTag(Node.Tag), ConvertPrimitiveProps(Node, Evaluator, Errors),
                            ConvertChildrenList(Node, Evaluator, Errors), nullptr};
}

// `<Native>` (docs/archive/iris_next_steps_resolved.md, "No way to declare a custom widget/
// imperative-draw node as an Iris element") -- its `build` prop evaluates to an already-built
// `Umbra::IWidget` handle, which has no room in `IrisPropValue` and no natural "evaluate this
// Nyx expression" callback shape (unlike every other escape hatch here, its result isn't
// representable as a prop, text, or `Component`), so it goes through the dedicated
// `NyxEvaluator::EvaluateNative` hook instead of `Evaluator.EvaluateProp`. An unset callback
// (a mock evaluator with no opinion on `<Native>`, e.g. most of `IrisIrRuntimeTests.cpp`) keeps
// today's "not yet supported" error; a set callback returning `nullptr` (the real
// implementation's own failure cases -- see `IrisNyxEvaluator.cpp`) gets a specific error
// instead, still reported here rather than by the callback, since only this function knows
// `Node.Location`.
Iris::Component ConvertNative(const IrElementNode& Node, const NyxEvaluator& Evaluator,
                                std::vector<IrisIrRuntimeError>* Errors) {
    if (!Evaluator.EvaluateNative) {
        AddError(Errors,
                  "<Native> is not yet supported for .irisx -- its `build` prop's opaque widget-builder escape "
                  "hatch has no interpreted-runtime evaluator hook (only the compiled .iris/Codegen path "
                  "supports it)",
                  Node.Location);
        return Iris::Component{Iris::IrisElementTag::Native, Iris::IrisProps{}, {}, nullptr};
    }

    std::shared_ptr<Iris::IrisNativeBuilder> Builder = Evaluator.EvaluateNative(Node);
    if (Builder == nullptr) {
        AddError(Errors, "<Native>'s `build` prop did not resolve to a registered native builder", Node.Location);
        return Iris::Component{Iris::IrisElementTag::Native, Iris::IrisProps{}, {}, nullptr};
    }
    return Iris::Component{Iris::IrisElementTag::Native, Iris::IrisProps{}, {}, nullptr, std::move(Builder)};
}

// `<Slot>`'s single escape-hatch child becomes a list-returning `Iris::IrisSlotCallable`
// (IrisIrRuntime.h's own doc comment on `IrisSlotResult` explains why always list-shaped here,
// unlike `Codegen.cpp`'s compile-time `if constexpr` dispatch in `MakeSlotCallable`: there is
// no host-compiler-checked C++ return type to dispatch on at IR-consumption time, and a
// 0-or-1-length list behaves identically to the single-`Component` shape through the existing
// `SlotState`/`ReconcileChildrenAt` machinery). From here on this is an ordinary `Component`
// with a `SlotCallable` set -- `iris::ResolveSlots`/`SlotState`/`Reconciler.h` handle it
// exactly as they already do for a compiled `.iris` file's own `<Slot>`.
Iris::Component ConvertSlot(const IrElementNode& Node, const NyxEvaluator& Evaluator,
                              std::vector<IrisIrRuntimeError>* Errors) {
    if (!Node.Props.empty()) {
        AddError(Errors, "<Slot> accepts no props other than key", Node.Location);
    }
    if (Node.Children.size() != 1 || Node.Children[0].Kind != IrElementChildKind::NyxExpression) {
        AddError(Errors, "<Slot> must have exactly one { } escape-hatch child", Node.Location);
        return Iris::Component{Iris::IrisElementTag::Slot, Iris::IrisProps{}, {}, nullptr};
    }

    // Captured by the closure below for the component's whole mounted lifetime (every
    // subsequent `SlotState::Reconcile()` re-invokes it) -- shares ownership of the IR node
    // via its existing `shared_ptr` (IrisIrDocument.h's own `IrElementChild::Expression`
    // field) rather than referencing it, since the `IrisIrDocument` this node came from isn't
    // guaranteed to outlive the mounted `Component` tree built from it.
    std::shared_ptr<IrNyxExpressionNode> ExprNode = Node.Children[0].Expression;
    NyxEvaluator                          EvaluatorCopy = Evaluator;

    // `Errors` is forwarded (by pointer, not by value) into the closure -- see ConvertIrElement's
    // own doc comment in IrisIrRuntime.h for when this is actually durable across a re-invocation
    // (it is, for the one real production caller, `IrisNyxDriver`) versus not (a caller passing a
    // stack-scoped vector, e.g. a test calling `WalkIrisIrDocument` directly, is responsible for
    // keeping it alive itself if it wants re-invocation diagnostics -- same opt-in-via-non-null
    // convention `Errors` already has everywhere else in this file).
    IrElementConverter Convert = [EvaluatorCopy, Errors](const IrElementNode& Child) {
        return ConvertIrElement(Child, EvaluatorCopy, Errors);
    };

    std::function<std::vector<Iris::Component>()> Callable = [ExprNode, EvaluatorCopy, Convert]() {
        IrisSlotResult Result = EvaluatorCopy.EvaluateSlot(*ExprNode, Convert);
        if (std::holds_alternative<Iris::Component>(Result)) {
            Iris::Component Single = std::get<Iris::Component>(std::move(Result));
            if (Single.Tag == Iris::IrisElementTag::None) {
                return std::vector<Iris::Component>{};
            }
            std::vector<Iris::Component> Out;
            Out.push_back(std::move(Single));
            return Out;
        }
        return std::get<std::vector<Iris::Component>>(std::move(Result));
    };

    auto SlotCallable = std::make_shared<Iris::IrisSlotCallable>(Iris::IrisSlotCallable{std::move(Callable)});
    return Iris::Component{Iris::IrisElementTag::Slot, Iris::IrisProps{}, {}, std::move(SlotCallable)};
}

// An element tag that isn't a known Core primitive is a component invocation
// (docs/iris_core_spec.md §2.6) -- see `NyxEvaluator::EvaluateComponentInvocation`'s own doc
// comment in IrisIrRuntime.h for why this hands off the raw, unevaluated node rather than
// trying to evaluate its props itself.
Iris::Component ConvertComponentInvocation(const IrElementNode& Node, const NyxEvaluator& Evaluator,
                                             std::vector<IrisIrRuntimeError>* Errors) {
    if (!Node.Children.empty()) {
        AddError(Errors,
                  "<" + Node.Tag +
                      "> is a component and cannot have children (implicit children forwarding is not yet supported)",
                  Node.Location);
    }
    if (!Evaluator.EvaluateComponentInvocation) {
        AddError(Errors,
                  "<" + Node.Tag + "> is a component invocation, but no EvaluateComponentInvocation callback was supplied",
                  Node.Location);
        return Iris::Component{nullptr};
    }
    Iris::Component Result = Evaluator.EvaluateComponentInvocation(Node);
    // Records which tag produced this subtree -- Component.h's own `InvocationTag` doc comment
    // explains why nothing else here can answer that later. Set unconditionally (even on a
    // `Component{nullptr}`/None-tagged failure result) so a later reload-matching pass sees a
    // consistent identity for this position either way.
    Result.InvocationTag = Node.Tag;
    return Result;
}

} // namespace

Iris::Component ConvertIrElement(const IrElementNode& Node, const NyxEvaluator& Evaluator,
                                   std::vector<IrisIrRuntimeError>* Errors) {
    Iris::Component Base;
    if (CorePrimitiveTagNames().count(Node.Tag) != 0) {
        if (Node.Tag == "Slot") {
            Base = ConvertSlot(Node, Evaluator, Errors);
        } else if (Node.Tag == "Native") {
            Base = ConvertNative(Node, Evaluator, Errors);
        } else if (Node.Tag == "Text") {
            Base = ConvertTextPrimitive(Node, Evaluator, Errors);
        } else if (Node.Tag == "Split") {
            if (Node.Children.size() != 2) {
                AddError(Errors, "<Split> requires exactly two children (leading and trailing panes)", Node.Location);
            }
            Base = ConvertOrdinaryPrimitive(Node, Evaluator, Errors);
        } else {
            Base = ConvertOrdinaryPrimitive(Node, Evaluator, Errors);
        }
    } else {
        Base = ConvertComponentInvocation(Node, Evaluator, Errors);
    }

    if (Node.Key.has_value()) {
        Base.Key = ConvertKeyRefValue(*Node.Key, Evaluator);
    }
    if (Node.Ref.has_value()) {
        Base.Ref = ConvertKeyRefValue(*Node.Ref, Evaluator);
    }
    return Base;
}

IrisIrWalkResult WalkIrisIrDocument(const IrisIrDocument& Document, const NyxEvaluator& Evaluator) {
    IrisIrWalkResult Result;
    for (const IrBodyNode& Node : Document.Body) {
        if (const auto* Source = std::get_if<IrNyxSourceNode>(&Node)) {
            if (Evaluator.EvaluateSource) {
                Evaluator.EvaluateSource(*Source);
            }
        } else {
            const auto& Block = std::get<IrRenderBlockNode>(Node);
            Result.RenderRoots.push_back(ConvertIrElement(Block.Root, Evaluator, &Result.Errors));
        }
    }
    return Result;
}

} // namespace Iris
