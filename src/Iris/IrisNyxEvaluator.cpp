#include "Iris/IrisNyxEvaluator.h"

#include "host/marshal.hpp"

#include <sstream>
#include <variant>

namespace Iris {

namespace {

using nyx::runtime::Value;
using nyx::runtime::ValueKind;

void AddError(std::vector<IrisIrRuntimeError>* Errors, std::string Message, IrSourceLocation Location) {
    if (Errors != nullptr) {
        Errors->push_back(IrisIrRuntimeError{std::move(Message), std::move(Location)});
    }
}

// Plain-value conversion only -- never called for a Callable Value (event-handler props are
// special-cased in EvaluateProp below, before any evaluation happens, since nyx-proto has no
// public "call this already-evaluated Value" primitive to build a reusable std::function
// from one). Long/Double narrow to IrisPropValue's nearest existing alternative (IrisProps.h's
// variant has no int64/double member) rather than fail outright; Null/Array/Object/HostObject
// have no IrisPropValue alternative at all and degrade to an empty string.
Iris::IrisPropValue ValueToPropValue(const Value& V) {
    switch (V.Kind()) {
        case ValueKind::Bool:
            return Iris::IrisPropValue{std::get<bool>(V.data)};
        case ValueKind::Int:
            return Iris::IrisPropValue{static_cast<int>(std::get<int32_t>(V.data))};
        case ValueKind::Long:
            return Iris::IrisPropValue{static_cast<int>(std::get<int64_t>(V.data))};
        case ValueKind::Float:
            return Iris::IrisPropValue{std::get<float>(V.data)};
        case ValueKind::Double:
            return Iris::IrisPropValue{static_cast<float>(std::get<double>(V.data))};
        case ValueKind::String:
            return Iris::IrisPropValue{std::get<std::string>(V.data)};
        default:
            return Iris::IrisPropValue{std::string{}};
    }
}

std::string StringifyValue(const Value& V) {
    switch (V.Kind()) {
        case ValueKind::Bool:
            return std::get<bool>(V.data) ? "true" : "false";
        case ValueKind::Int:
            return std::to_string(std::get<int32_t>(V.data));
        case ValueKind::Long:
            return std::to_string(std::get<int64_t>(V.data));
        case ValueKind::Float: {
            std::ostringstream Out;
            Out << std::get<float>(V.data);
            return Out.str();
        }
        case ValueKind::Double: {
            std::ostringstream Out;
            Out << std::get<double>(V.data);
            return Out.str();
        }
        case ValueKind::String:
            return std::get<std::string>(V.data);
        default:
            return {};
    }
}

// A JSON-string-literal-style escape is overkill here (Nyx's own string-literal grammar,
// not JSON's) -- only backslash and the quote character can break the reconstructed source,
// everything else (including newlines) is legal inside a Nyx string literal verbatim, same
// escaping RenderBlockParser.cpp's own JSX-transform text reconstruction doesn't need to
// worry about since it never re-quotes text it captured.
std::string QuoteNyxStringLiteral(const std::string& Text) {
    std::string Result = "\"";
    for (char C : Text) {
        if (C == '"' || C == '\\') {
            Result += '\\';
        }
        Result += C;
    }
    Result += '"';
    return Result;
}

// Evaluates Source as an immediately-invoked lambda: every documented event-handler/`<Slot>`
// escape hatch is author-written as a `() -> ...` lambda (chaos-ir-spec.md §3.6/§4), so
// wrapping in `( )()` and parsing as one bare expression (EvaluateInScope's own contract)
// both parses the lambda and calls it in a single pass -- no separate "call this Value"
// primitive needed (nyx-proto exposes none publicly).
Value InvokeAsLambda(nyx::host::NyxRuntime& Runtime, nyx::host::NyxRuntime::NyxScope& Scope,
                      const std::string& Source, const std::vector<std::string>& ArgLiterals) {
    std::string Call = "(" + Source + ")(";
    for (std::size_t I = 0; I < ArgLiterals.size(); ++I) {
        if (I != 0) {
            Call += ", ";
        }
        Call += ArgLiterals[I];
    }
    Call += ")";
    return Runtime.EvaluateInScope(Scope, Call);
}

} // namespace

std::string ReconstructNyxSource(const IrisIrDocument& Document) {
    std::string Result;
    for (const IrBodyNode& Node : Document.Body) {
        if (const auto* Source = std::get_if<IrNyxSourceNode>(&Node)) {
            Result += Source->Source;
        } else {
            Result += "{}\n";
        }
    }
    return Result;
}

void ChaosSlotMarker::RegisterOn(nyx::host::NyxRuntime& Runtime) {
    std::shared_ptr<std::vector<std::size_t>> Selected = Selected_;
    Runtime.RegisterFunction("__chaos_slot_pick", [Selected](std::vector<Value> Args) -> Value {
        if (!Args.empty() && Args[0].Kind() == ValueKind::Int) {
            Selected->push_back(static_cast<std::size_t>(std::get<int32_t>(Args[0].data)));
        }
        return Value();
    });
}

NyxEvaluator MakeNyxEvaluator(nyx::host::NyxRuntime& Runtime, nyx::host::NyxRuntime::NyxScope& Scope,
                                 ChaosSlotMarker& Marker, ChildComponentInvoker InvokeChild,
                                 std::vector<IrisIrRuntimeError>* Errors) {
    NyxEvaluator Eval;

    Eval.EvaluateProp = [&Runtime, &Scope](const IrNyxExpressionNode& Node,
                                             const std::string& ExpectedTypeName) -> Iris::IrisPropValue {
        if (ExpectedTypeName == "std::function<void()>") {
            std::string Source = Node.Source();
            return Iris::IrisPropValue{std::function<void()>([&Runtime, &Scope, Source]() {
                InvokeAsLambda(Runtime, Scope, Source, {});
            })};
        }
        if (ExpectedTypeName == "std::function<void(std::string)>") {
            std::string Source = Node.Source();
            return Iris::IrisPropValue{
                std::function<void(std::string)>([&Runtime, &Scope, Source](std::string Arg) {
                    InvokeAsLambda(Runtime, Scope, Source, {QuoteNyxStringLiteral(Arg)});
                })};
        }
        return ValueToPropValue(Runtime.EvaluateInScope(Scope, Node.Source()));
    };

    Eval.EvaluateText = [&Runtime, &Scope](const IrNyxExpressionNode& Node) {
        return StringifyValue(Runtime.EvaluateInScope(Scope, Node.Source()));
    };

    Eval.EvaluateSlot = [&Runtime, &Scope, &Marker](const IrNyxExpressionNode& Node,
                                                       const IrElementConverter& Convert) -> IrisSlotResult {
        const std::vector<IrElementNode> Elements = Node.Elements();
        if (Elements.empty()) {
            // A plain (non-JSX) escape hatch's own callable return value has no natural
            // Component-equivalent -- no host bindings expose Component construction to Nyx
            // script (docs/iris_nyx_emission_decision.md's explicit rejection of that
            // design). Same "not yet supported" posture as <Native>: render nothing rather
            // than guess.
            return std::vector<Iris::Component>{};
        }

        std::string Reconstructed;
        std::size_t ElementIndex = 0;
        for (const IrNyxExpressionSegment& Seg : Node.Segments) {
            if (Seg.Kind == IrNyxExpressionSegmentKind::Text) {
                Reconstructed += Seg.Text;
            } else {
                Reconstructed += "__chaos_slot_pick(" + std::to_string(ElementIndex) + ")";
                ++ElementIndex;
            }
        }

        Marker.Selected_->clear();
        InvokeAsLambda(Runtime, Scope, Reconstructed, {});
        std::vector<std::size_t> Picked = *Marker.Selected_;

        std::vector<Iris::Component> Out;
        for (std::size_t Index : Picked) {
            if (Index < Elements.size()) {
                Out.push_back(Convert(Elements[Index]));
            }
        }
        return Out;
    };

    Eval.EvaluateComponentInvocation = [&Runtime, &Scope, InvokeChild, Errors](const IrElementNode& Node) -> Iris::Component {
        if (!InvokeChild) {
            AddError(Errors,
                      "<" + Node.Tag + "> is a component invocation, but no ChildComponentInvoker was supplied",
                      Node.Location);
            return Iris::Component{nullptr};
        }

        auto Props = std::make_shared<nyx::runtime::NyxObject>();
        Props->typeName = Node.Tag;
        for (const IrPropNode& P : Node.Props) {
            const Iris::IrisPropValue Evaluated =
                P.Value.IsLiteral ? Iris::IrisPropValue{P.Value.Literal.Value}
                                    : ValueToPropValue(Runtime.EvaluateInScope(Scope, P.Value.Expression.Source()));
            if (const auto* AsString = std::get_if<std::string>(&Evaluated)) {
                Props->fields[P.Name] = Value(*AsString);
            } else if (const auto* AsInt = std::get_if<int>(&Evaluated)) {
                Props->fields[P.Name] = Value(static_cast<int32_t>(*AsInt));
            } else if (const auto* AsFloat = std::get_if<float>(&Evaluated)) {
                Props->fields[P.Name] = Value(*AsFloat);
            } else if (const auto* AsBool = std::get_if<bool>(&Evaluated)) {
                Props->fields[P.Name] = Value(*AsBool);
            }
        }

        return InvokeChild(Node.Tag, Value(Props));
    };

    return Eval;
}

} // namespace Iris
