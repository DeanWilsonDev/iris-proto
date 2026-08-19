#include "Iris/IrisNyxEvaluator.h"

#include "host/marshal.hpp"
#include "lexer/lexer.hpp"
#include "runtime/environment.hpp"

#include <sstream>
#include <unordered_map>
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

// Which identifier(s) a `.Map()`/`.Reduce()` callback's own lambda parameters bind an embedded
// element's current item/iteration-index to -- extracted once at reconstruction time so
// `EvaluateSlot` can inject them into the `__chaos_slot_pick` marker call, and again to know
// what to `Define` in a per-pick scope afterward.
struct MapReduceBinding {
    std::string                Item;
    std::optional<std::string> Index; // Map's optional 2nd lambda param only; never set for Reduce
};

// Scans one `Text` segment for a `.Map((param[, indexParam]) -> ` or
// `.Reduce((acc, param) -> ` lambda that is still *open* at the end of the segment -- i.e. the
// segment immediately following it (an embedded element) sits inside that lambda's own body.
// docs/archive/iris_nyx_slot_loop_and_reload_gap_resolved.md §1: this can't be pure text substitution,
// and `Iris::NyxTokenizer` is the wrong tool (it coarsens keywords/strings, losing exact
// identifier boundaries) -- this tokenizes `Text` directly with nyx-proto's own real
// `nyx::Lexer`, the same tool `NyxTokenizer.cpp` itself wraps, and walks its token stream the
// same way `Parser::ParseParenOrLambda`'s own lambda-param scan does on nyx-proto's own side.
std::optional<MapReduceBinding> DetectOpenMapReduceLambda(const std::string& Text) {
    nyx::Lexer               Lexer{Text};
    std::vector<nyx::Token> Tokens;
    try {
        Tokens = Lexer.Tokenize();
    } catch (const nyx::LexError&) {
        // `Text` is one interleaved fragment of a larger expression, not necessarily
        // self-terminated (e.g. a string literal split across a `.Map()` boundary is not a
        // pattern this scanner needs to diagnose) -- no binding found is the safe fallback.
        return std::nullopt;
    }

    std::optional<MapReduceBinding> Result;
    for (std::size_t I = 0; I < Tokens.size(); ++I) {
        if (Tokens[I].kind != nyx::TokenKind::Dot || I + 1 >= Tokens.size() ||
            Tokens[I + 1].kind != nyx::TokenKind::Identifier) {
            continue;
        }
        const bool IsMap    = Tokens[I + 1].lexeme == "Map";
        const bool IsReduce = Tokens[I + 1].lexeme == "Reduce";
        if (!IsMap && !IsReduce) {
            continue;
        }

        std::size_t J = I + 2;
        if (J >= Tokens.size() || Tokens[J].kind != nyx::TokenKind::LParen) {
            continue;
        }
        ++J; // the outer .Map( / .Reduce( call's own paren
        if (J >= Tokens.size() || Tokens[J].kind != nyx::TokenKind::LParen) {
            continue;
        }
        ++J; // the lambda's own parameter-list paren

        // A `LambdaParam` is always `Type Name [= default]` (parser.cpp's own
        // `ParseParenOrLambda`: `p.type = ParseType(); p.name = Expect(Identifier, ...)`) --
        // untyped `(item) -> ...` isn't actually valid Nyx syntax, only `(int item) -> ...`
        // is. `Type` may itself be generic (`Array<int> item`), so the parameter's own name
        // is the *last* Identifier seen at bracket/paren depth 0 within this parameter's own
        // span, not the first -- Type always precedes Name, never the reverse. Once `=` is
        // seen at depth 0 the name is already known; further identifiers belong to the
        // default-value expression and must not overwrite it (decision-log.md §5.14's default
        // parameters still carry a declared name either way, so this only needs to stop
        // updating, not fully evaluate that expression).
        std::vector<std::string> Params;
        while (J < Tokens.size() && Tokens[J].kind != nyx::TokenKind::RParen) {
            std::string LastIdentifier;
            int         AngleDepth = 0;
            int         ParenDepth = 0;
            bool        SawDefault = false;
            while (J < Tokens.size() &&
                   !(ParenDepth == 0 && AngleDepth == 0 &&
                     (Tokens[J].kind == nyx::TokenKind::Comma || Tokens[J].kind == nyx::TokenKind::RParen))) {
                switch (Tokens[J].kind) {
                    case nyx::TokenKind::Less:
                        ++AngleDepth;
                        break;
                    case nyx::TokenKind::Greater:
                        if (AngleDepth > 0) {
                            --AngleDepth;
                        }
                        break;
                    case nyx::TokenKind::LParen:
                        ++ParenDepth;
                        break;
                    case nyx::TokenKind::RParen:
                        --ParenDepth;
                        break;
                    case nyx::TokenKind::Assign:
                        if (ParenDepth == 0 && AngleDepth == 0) {
                            SawDefault = true;
                        }
                        break;
                    case nyx::TokenKind::Identifier:
                        if (ParenDepth == 0 && AngleDepth == 0 && !SawDefault) {
                            LastIdentifier = Tokens[J].lexeme;
                        }
                        break;
                    default:
                        break;
                }
                ++J;
            }
            if (!LastIdentifier.empty()) {
                Params.push_back(LastIdentifier);
            }
            if (J < Tokens.size() && Tokens[J].kind == nyx::TokenKind::Comma) {
                ++J;
            }
        }
        if (J >= Tokens.size() || Tokens[J].kind != nyx::TokenKind::RParen) {
            continue; // malformed -- no matching close found
        }
        ++J; // the lambda param list's own closing paren
        if (J >= Tokens.size() || Tokens[J].kind != nyx::TokenKind::Arrow || Params.empty()) {
            continue;
        }
        ++J; // `->`

        MapReduceBinding Binding;
        if (IsMap) {
            Binding.Item = Params[0];
            if (Params.size() > 1) {
                Binding.Index = Params[1];
            }
        } else {
            if (Params.size() < 2) {
                continue; // Reduce always takes (acc, item) -- malformed otherwise
            }
            Binding.Item = Params[1]; // second lambda param is the element; first is the accumulator
        }

        // From here on this is the lambda body: track paren depth starting at 1 (the outer
        // .Map(/.Reduce( call's own still-open paren) -- if depth never returns to 0 by the end
        // of this Text segment, the call is still open, and the element segment immediately
        // following belongs to this lambda's body.
        int  Depth     = 1;
        bool StillOpen = true;
        for (std::size_t K = J; K < Tokens.size(); ++K) {
            if (Tokens[K].kind == nyx::TokenKind::LParen) {
                ++Depth;
            } else if (Tokens[K].kind == nyx::TokenKind::RParen) {
                if (--Depth == 0) {
                    StillOpen = false;
                    break;
                }
            }
        }
        if (StillOpen) {
            Result = Binding; // the closest-to-the-end open call wins
        }
    }
    return Result;
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
    std::shared_ptr<std::vector<ChaosSlotPick>> Selected = Selected_;
    Runtime.RegisterFunction("__chaos_slot_pick", [Selected](std::vector<Value> Args) -> Value {
        if (!Args.empty() && Args[0].Kind() == ValueKind::Int) {
            ChaosSlotPick Pick;
            Pick.ElementIndex = static_cast<std::size_t>(std::get<int32_t>(Args[0].data));
            if (Args.size() > 1) {
                Pick.Item = Args[1];
            }
            if (Args.size() > 2) {
                Pick.IterationIndex = Args[2];
            }
            Selected->push_back(std::move(Pick));
        }
        return Value();
    });
}

NyxEvaluator MakeNyxEvaluator(nyx::host::NyxRuntime& Runtime, nyx::host::NyxRuntime::NyxScope& Scope,
                                 ChaosSlotMarker& Marker, ChildComponentInvoker InvokeChild,
                                 std::vector<IrisIrRuntimeError>* Errors,
                                 NativeBuilderLookup FindNativeBuilder) {
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

    if (FindNativeBuilder) {
        Eval.EvaluateNative = [&Runtime, &Scope, FindNativeBuilder,
                                 Errors](const IrElementNode& Node) -> std::shared_ptr<Iris::IrisNativeBuilder> {
            const IrPropNode* BuildProp = nullptr;
            for (const IrPropNode& P : Node.Props) {
                if (P.Name == "build") {
                    BuildProp = &P;
                    break;
                }
            }
            if (BuildProp == nullptr || BuildProp->Value.IsLiteral) {
                AddError(Errors,
                          "<Native> requires a `build` prop with a Nyx expression naming a registered builder",
                          Node.Location);
                return nullptr;
            }

            // `build` is author-written as a `() -> ...` lambda, same as every other escape
            // hatch here (chaos-ir-spec.md §3.6) -- evaluating its raw source alone (as opposed
            // to InvokeAsLambda's own "parse and immediately call" contract) would yield the
            // *Callable* itself, never the string it returns.
            const Value Result = InvokeAsLambda(Runtime, Scope, BuildProp->Value.Expression.Source(), {});
            if (Result.Kind() != ValueKind::String) {
                AddError(Errors, "<Native>'s `build` prop must evaluate to a string naming a registered builder",
                          BuildProp->Value.Expression.Location);
                return nullptr;
            }

            const std::string& Name = std::get<std::string>(Result.data);
            std::function<std::unique_ptr<Umbra::IWidget>(const Value&)> Factory = FindNativeBuilder(Name);
            if (!Factory) {
                AddError(Errors, "no native builder registered for name '" + Name + "'",
                          BuildProp->Value.Expression.Location);
                return nullptr;
            }

            // Every prop on this <Native> other than `build` is the invoking element's own
            // data for the factory -- `<Native build={() -> "BuildTreeRow"} node={props.node}
            // depth={props.depth} />` (docs/next-steps.md's "<Native> builders can't receive
            // the invoking element's own props" entry). Packed exactly the way
            // EvaluateComponentInvocation below packs a component invocation's own props (ad
            // hoc NyxObject, raw Value preserved -- not narrowed through ValueToPropValue) so a
            // HostObject/Array/Object prop (e.g. a marshaled tree-node pointer) survives intact,
            // not just bool/int/float/string.
            auto Props = std::make_shared<nyx::runtime::NyxObject>();
            Props->typeName = Name;
            for (const IrPropNode& P : Node.Props) {
                if (P.Name == "build") {
                    continue;
                }
                Value PropValue = P.Value.IsLiteral ? Value(P.Value.Literal.Value)
                                                     : Runtime.EvaluateInScope(Scope, P.Value.Expression.Source());
                Props->adHocFields.emplace_back(P.Name, std::move(PropValue));
            }
            Value PropsValue(Props);

            return Iris::MakeNativeBuilder(
                [Factory = std::move(Factory), PropsValue = std::move(PropsValue)]() { return Factory(PropsValue); });
        };
    }

    Eval.EvaluateSlot =
        [&Runtime, &Scope, &Marker, InvokeChild, Errors, FindNativeBuilder,
         PickScopes = std::make_shared<std::vector<std::shared_ptr<nyx::host::NyxRuntime::NyxScope>>>()](
            const IrNyxExpressionNode& Node, const IrElementConverter& Convert) -> IrisSlotResult {
        const std::vector<IrElementNode> Elements = Node.Elements();
        if (Elements.empty()) {
            // A plain (non-JSX) escape hatch's own callable return value has no natural
            // Component-equivalent -- no host bindings expose Component construction to Nyx
            // script (docs/iris_nyx_emission_decision.md's explicit rejection of that
            // design). Same "not yet supported" posture as <Native>: render nothing rather
            // than guess.
            return std::vector<Iris::Component>{};
        }

        std::string                              Reconstructed;
        std::size_t                              ElementIndex = 0;
        std::unordered_map<std::size_t, MapReduceBinding> Bindings;
        std::optional<MapReduceBinding>          Pending;
        for (const IrNyxExpressionSegment& Seg : Node.Segments) {
            if (Seg.Kind == IrNyxExpressionSegmentKind::Text) {
                Reconstructed += Seg.Text;
                Pending = DetectOpenMapReduceLambda(Seg.Text);
            } else {
                if (Pending.has_value()) {
                    Bindings[ElementIndex] = *Pending;
                }
                Reconstructed += "__chaos_slot_pick(" + std::to_string(ElementIndex);
                if (Pending.has_value()) {
                    Reconstructed += ", " + Pending->Item;
                    if (Pending->Index.has_value()) {
                        Reconstructed += ", " + *Pending->Index;
                    }
                }
                Reconstructed += ")";
                ++ElementIndex;
                Pending.reset();
            }
        }

        Marker.Selected_->clear();
        InvokeAsLambda(Runtime, Scope, Reconstructed, {});
        std::vector<ChaosSlotPick> Picked = *Marker.Selected_;

        // Discards the previous call's per-pick scopes -- safe once this call's own picks are
        // about to (re)build the tree: any prop closure capturing the old scopes belonged to
        // Components this same re-render is about to replace, same "freshly rebuilt, old
        // discarded" lifetime every other prop closure in this file already has.
        PickScopes->clear();

        std::vector<Iris::Component> Out;
        for (const ChaosSlotPick& Pick : Picked) {
            if (Pick.ElementIndex >= Elements.size()) {
                continue;
            }
            if (!Pick.Item.has_value()) {
                Out.push_back(Convert(Elements[Pick.ElementIndex]));
                continue;
            }
            const auto BindingIt = Bindings.find(Pick.ElementIndex);
            if (BindingIt == Bindings.end()) {
                Out.push_back(Convert(Elements[Pick.ElementIndex])); // shouldn't happen; fall back rather than drop
                continue;
            }

            // The same EvalContext-substitution shape InvokeComponent already uses
            // (decision-log.md §7.3), applied per pick instead of per invocation: a fresh
            // child Environment of the outer invocation's own scope, with the .Map()/.Reduce()
            // callback's bound parameter(s) Defined directly -- two picks from the same call
            // never share an Environment, so never see each other's bound item. Heap-allocated
            // and kept alive via PickScopes (this closure's own persistent state, not a local)
            // since a produced element's event-handler prop may re-evaluate against it long
            // after this call returns -- MakeNyxEvaluator's closures hold a raw reference into
            // whatever NyxScope they're given, same contract IrisNyxDriver's own DriverState
            // already satisfies for the outer per-invocation scope.
            auto ChildEnv = std::make_shared<nyx::runtime::Environment>(Scope.context.env);
            ChildEnv->Define(BindingIt->second.Item, *Pick.Item);
            if (BindingIt->second.Index.has_value() && Pick.IterationIndex.has_value()) {
                ChildEnv->Define(*BindingIt->second.Index, *Pick.IterationIndex);
            }
            auto PerPickScope = std::make_shared<nyx::host::NyxRuntime::NyxScope>(
                nyx::host::NyxRuntime::NyxScope{Scope.interpreter, nyx::interpreter::EvalContext{ChildEnv, Scope.context.thisObject}});
            PickScopes->push_back(PerPickScope);

            NyxEvaluator PerPickEval =
                MakeNyxEvaluator(Runtime, *PerPickScope, Marker, InvokeChild, Errors, FindNativeBuilder);
            Out.push_back(ConvertIrElement(Elements[Pick.ElementIndex], PerPickEval, Errors));
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

        // Stores the raw evaluated Value directly -- deliberately *not* routed through
        // ValueToPropValue (this file's own helper for a leaf Core primitive's own
        // Penumbra-facing props, e.g. <Icon size={...}>), which only has cases for
        // bool/int/float/string and silently drops everything else (Array/Object/
        // HostObject) from Props->fields entirely. That's the right narrowing for a leaf
        // widget prop -- Penumbra has no concept of a HostObject -- but wrong here: a
        // component-invocation prop is a pure Nyx-side value the callee's own render body
        // reads back via ordinary `props.name` field access (NyxObject::fields already
        // holds `Value`, no kind restriction), so passing a HostObject (e.g. a marshaled
        // tree node) or an Array through a recursive/child component invocation --
        // `<ExplorerRowNode node={item} />` inside a `.Map()` callback, in particular --
        // was previously impossible: the prop silently vanished rather than erroring,
        // discovered while wiring a real recursive .irisx component tree.
        //
        // Props->adHocFields, not Props->fields (nyx-proto's e28c957, decision-log.md
        // §8.7/§8.8, replaced the single field map with schema/slots for real class/data
        // instances plus an adHocFields fallback for host-constructed objects like this
        // one -- Props is never registered against a schema, so it's always ad hoc mode).
        auto Props = std::make_shared<nyx::runtime::NyxObject>();
        Props->typeName = Node.Tag;
        for (const IrPropNode& P : Node.Props) {
            Value PropValue = P.Value.IsLiteral ? Value(P.Value.Literal.Value)
                                                 : Runtime.EvaluateInScope(Scope, P.Value.Expression.Source());
            Props->adHocFields.emplace_back(P.Name, std::move(PropValue));
        }

        return InvokeChild(Node.Tag, Value(Props), Node);
    };

    return Eval;
}

} // namespace Iris
