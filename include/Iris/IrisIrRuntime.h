#pragma once

#include "Iris/Component.h"
#include "Iris/IrisIrDocument.h"

#include <functional>
#include <string>
#include <variant>
#include <vector>

namespace Iris {

// The `.iris.ir` consumer -- what docs/next-steps.md calls "the Chaos runtime": walks an
// already-deserialized `IrisIrDocument` (IrisIrDocument.h) and produces `Iris::Component`
// values from it, the same backend-agnostic IR `Codegen.h` produces for the compiled `.iris`
// path. This is that same emission step, operating at IR-consumption time over JSON-sourced
// nodes instead of at preprocessor time over parsed source text -- `ConvertIrElement` mirrors
// `Codegen.cpp`'s `ComponentEmitter::Emit` node-for-node (same tag dispatch, same
// CorePrimitives.h prop/child rules), except it *evaluates* to a live `Component` value via
// `NyxEvaluator` rather than emitting C++ source text for a host compiler to build later.
//
// Once a tree comes out of here, it is functionally identical to one `Codegen.h`-generated
// C++ would have produced for the same file -- everything downstream (`iris::
// MountComponentInstance`, `iris::ResolveSlots`, `Reconciler.h`, `iris::Tick()`) is the
// existing Stage 2/3 runtime, completely unchanged. This file's only job is the ElementNode
// (IR) -> Component conversion and the `<Slot>` wiring into `Iris::IrisSlotCallable`; it does
// not reimplement reconciliation, mounting, or signal tracking.
//
// **What this deliberately does not do**, per docs/next-steps.md and
// `chaos-ir-spec.md` §6/`decision-log.md` §7.2: it does not itself evaluate Nyx source text.
// nyx-proto has no "evaluate this source against a live scope" embedding primitive yet
// (`EvaluateInScope`, decision-log.md §7.2 -- "genuinely new Phase 6 work, not yet started").
// Every place this walker would need one, it instead calls a small injected `NyxEvaluator`
// callback -- the same "runtime supplies a callback, this code never knows what's on the
// other side of it" pattern `SlotRuntime.h`'s `MountFn` already uses for widget construction.
// A test (or, later, a real nyx-proto-backed implementation) supplies the callbacks; this file
// only defines the contract and drives it.

struct IrisIrRuntimeError {
    std::string      Message;
    IrSourceLocation Location;
};

// A `<Slot>` callable's (or, per below, a one-shot mount's) current output -- mirrors
// `IrisSlotCallable::Callable`'s own two-shape variant (Component.h) at the evaluator-
// interface level, since a Nyx `<Slot>` callable's declared return type
// (`(): Penumbra::Component` vs `(): Array<Penumbra::Component>`, chaos-ui-authoring.md
// §27.7) is exactly that same choice, just written in Nyx rather than C++.
using IrisSlotResult = std::variant<Iris::Component, std::vector<Iris::Component>>;

struct IrElementNode;

// Recursively converts one already-parsed `IrElementNode` subtree into a live `Iris::
// Component`, evaluating any prop/key/ref/text `nyx_expression` values inside it via whatever
// `NyxEvaluator` produced this callback (`ConvertIrElement` below, partially applied). Handed
// to `NyxEvaluator::EvaluateSlot` so a `<Slot>`'s own control flow -- a conditional, a loop
// over `props.members` -- can call this as many times as *it* decides to, once per branch or
// iteration, with whatever live Nyx scope is active at that call: the one piece of real
// per-invocation logic only the Nyx interpreter itself can drive, which is exactly why this is
// a callback the walker hands outward rather than something the walker computes once itself.
using IrElementConverter = std::function<Iris::Component(const IrElementNode&)>;

// The seam between this walker and whatever actually evaluates Nyx source -- every callback
// here corresponds to one `chaos-ir-spec.md` node kind that carries raw Nyx text
// (`NyxExpressionNode`/`NyxSourceNode`). None of these are implemented in this repo yet (see
// this file's own top comment); a test supplies a mock, a real embedding calls into nyx-proto
// once `EvaluateInScope` (or equivalent) exists there.
struct NyxEvaluator {
    // A `nyx_expression` used as a prop, `key`, or `ref` value. `ExpectedTypeName` is the
    // target primitive prop's declared C++ type name (`CorePrimitives.h`'s
    // `PrimitivePropTypeNames()`, e.g. `"float"`, `"std::function<void()>"`) for an ordinary
    // primitive prop, or empty for a `key`/`ref` value or a component-invocation prop, where
    // any `IrisPropValue` alternative is accepted (mirrors `Codegen.cpp`'s own untyped
    // acceptance there -- `IrisPropValue`'s converting constructor picks whichever alternative
    // the expression's own value turns out to be).
    std::function<Iris::IrisPropValue(const IrNyxExpressionNode&, const std::string& ExpectedTypeName)> EvaluateProp;

    // A bare `{ expr }` used as element/text-interpolation content (chaos-ui-authoring.md
    // §27.6) -- returns the text to render, the same role `EmitSyntheticTextNode`/
    // `EmitTextPrimitive` give an escape-hatch child in the compiled `.iris` path.
    std::function<std::string(const IrNyxExpressionNode&)> EvaluateText;

    // A `<Slot>`'s single callable child -- builds (or, on re-invocation once a captured
    // signal fires, rebuilds) its current output. `Convert` lets the evaluator turn whichever
    // of `Node.Children`'s pre-parsed candidate `ElementNode`s its own Nyx-side control flow
    // selects into a real `Component`, as many times as it needs to.
    std::function<IrisSlotResult(const IrNyxExpressionNode& Node, const IrElementConverter& Convert)> EvaluateSlot;

    // A non-primitive tag -- a component invocation (docs/iris_core_spec.md §2.6, e.g.
    // `<HealthBar current={...} />`). Handed the *unevaluated* `IrElementNode` (props still
    // carry their own `IrPropValue`) rather than a pre-evaluated prop map, since only the
    // evaluator knows the target `<Tag>Props` struct's field types -- exactly the boundary
    // `Codegen.cpp`'s `EmitComponentInvocation` leaves to the host C++ compiler (it emits
    // `Name(NameProps{.prop = expr, ...})` textually and never itself knows `NameProps`'s
    // shape); this is that same boundary at IR-consumption time. Expected to invoke the named
    // component function and return the result already wrapped in `iris::
    // MountComponentInstance`, matching what generated `.iris` code does for every `<Name
    // .../>` invocation.
    std::function<Iris::Component(const IrElementNode& Node)> EvaluateComponentInvocation;

    // One `nyx_source` region -- ordinary Nyx code outside any `render { }` block (component
    // declaration, `@signal` locals, free functions). Invoked once per region encountered,
    // in document order, before whichever `render_block` (if any) follows it.
    std::function<void(const IrNyxSourceNode&)> EvaluateSource;

    // A `<Native>` element's `build` prop (docs/next-steps.md's "Chaos runtime" entry). Nyx
    // script has no binding for `Umbra::IWidget` construction (no such binding should exist --
    // it would leak a backend type into the interpreter), so unlike every other callback here
    // this isn't "evaluate this expression and narrow the result" -- it's handed the *whole*
    // node so it can read+evaluate the `build` prop's own expression however its own
    // implementation decides is meaningful (the real implementation, `MakeNyxEvaluator`,
    // requires it to name a host-registered builder; a mock is free to do something simpler).
    // Expected to return the already-wrapped builder (`Iris::MakeNativeBuilder`) on success, or
    // `nullptr` on any failure (unset callback, missing/wrong-typed `build` prop, unresolved
    // name) -- `ConvertNative` reports an error in every `nullptr` case, same as when this
    // callback is left unset entirely.
    std::function<std::shared_ptr<Iris::IrisNativeBuilder>(const IrElementNode&)> EvaluateNative;
};

// Converts one `IrElementNode` into a live `Component`, per this file's own top comment.
// Exposed directly (not just via `WalkIrisIrDocument`) since `NyxEvaluator::EvaluateSlot`
// implementations need an `IrElementConverter` bound to this, and tests exercising element
// conversion in isolation don't need a whole document.
//
// `Errors` may be `nullptr`. A `<Slot>` callable's own `IrElementConverter` closure
// (`ConvertSlot`, IrisIrRuntime.cpp) forwards whichever `Errors` pointer was live at the moment
// the `<Slot>` was first converted, since a re-invocation can happen an arbitrary time later (via
// `iris::Tick()`), long after the original `WalkIrisIrDocument`/`ConvertIrElement` call that
// produced it already returned. Whether that makes a re-invocation's own errors durable depends
// entirely on what the caller passed in here: `IrisNyxDriver` (IrisNyxDriver.cpp) passes its own
// `Errors_` member, which lives for the driver's whole lifetime, so a re-invocation error reaches
// a real, durable sink on that path. A caller passing a stack-scoped vector instead (e.g. a test
// calling `WalkIrisIrDocument` directly with `&Result.Errors`) gets no such guarantee -- that
// vector is gone the moment the call returns, before any later re-invocation could run -- so such
// a caller should supply its own durable vector if it wants re-invocation diagnostics, same
// opt-in-via-non-null convention this parameter already has for every other use.
Iris::Component ConvertIrElement(const IrElementNode& Node, const NyxEvaluator& Evaluator,
                                   std::vector<IrisIrRuntimeError>* Errors);

struct IrisIrWalkResult {
    // One entry per `render_block` found in `Document.Body`, in document order -- almost
    // always exactly one (chaos-ui-authoring.md §27.1: "each component lives in its own
    // `.chaos` file"), but the schema itself doesn't forbid more.
    std::vector<Iris::Component>       RenderRoots;
    std::vector<IrisIrRuntimeError>    Errors;
};

// Walks `Document.Body` in order: a `nyx_source` node invokes `Evaluator.EvaluateSource` once;
// a `render_block` node converts its `Root` via `ConvertIrElement` and appends the result to
// `RenderRoots`. `Document.Imports` is not otherwise acted on here -- import resolution
// already happened at IR-*production* time (`ResolvedImportsList`, `IrisIr.h`'s own
// precondition); the runtime has nothing further to do with it besides what a future
// component-invocation evaluator implementation might read to resolve a `<Tag>` name to a
// callable.
IrisIrWalkResult WalkIrisIrDocument(const IrisIrDocument& Document, const NyxEvaluator& Evaluator);

} // namespace Iris
