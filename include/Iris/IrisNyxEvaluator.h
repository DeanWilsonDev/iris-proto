#pragma once

#include "Iris/IrisIrRuntime.h"

#include "host/nyx-runtime.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Iris {

// Real, non-mock `NyxEvaluator` implementation backed by nyx-proto -- docs/
// iris_nyx_evaluator_scope_gap.md's resolution (nyx-scripting-language/decision-log.md
// §7.3), the still-missing piece `docs/next-steps.md`'s "Chaos runtime" entry tracked.
// This file's job is narrow: bridge `IrisIrRuntime.h`'s `NyxEvaluator` callbacks to real
// Nyx evaluation. It does not resolve which file a `<Tag>` component invocation refers to,
// and does not drive a whole application's mount/reload lifecycle -- both are a driver's
// job (not yet built; see docs/next-steps.md), left here as explicit callback seams, the
// same "runtime supplies a callback, this code never knows what's on the other side of it"
// pattern `SlotRuntime.h`'s `MountFn` already uses for widget construction.

// Reconstructs `Document`'s `nyx_source` fragments into one complete, parseable Nyx
// program, splicing an empty `{}` block in place of each `render_block` position --
// docs/iris_nyx_evaluator_scope_gap.md's Problem 1 resolution. Verified there against
// nyx-proto's own `Parser::ParseStatement`, which accepts a bare `{}` unconditionally
// wherever a statement is expected, so no new nyx-proto primitive was needed for this, just
// this splicing convention. Import nodes are already excluded from `Document.Body`
// (chaos-ir-spec.md §3.1 -- reported separately in `Document.Imports`), so nothing here
// needs to strip them again.
//
// One precondition this relies on (decision-log.md §7.3's own note, shared with
// `NyxRuntime::InvokeComponent`'s own captured-environment trick): `render{}` is always the
// *last* top-level statement in a component's body, nothing after it -- true for every
// documented `.chaos`/`.irisx` example.
std::string ReconstructNyxSource(const IrisIrDocument& Document);

// The seam a component invocation (`<Tag .../>`, a non-primitive element -- docs/
// iris_core_spec.md §2.6) needs to mount a *different* component: given the tag name and
// this invocation's own evaluated props (already packed into a `NyxObject` -- see
// `MakeNyxEvaluator`'s own doc comment), returns a live `Component` already wrapped in
// `iris::MountComponentInstance` (`ConvertComponentInvocation`'s existing contract,
// `IrisIrRuntime.h`). Finding which file/document a tag name refers to is an
// import-resolution question (`ImportResolver`'s job) this file deliberately doesn't
// attempt -- left to whoever drives a whole application.
//
// `Node` is the same `IrElementNode` `EvaluateComponentInvocation` was itself handed --
// exposed here purely so a reload-aware invoker (`IrisNyxDriver::InvokeChildComponent`) can
// read this invocation's own `Key`/position to find a matching previous invocation to reload
// against; an ordinary mount-only invoker (`MountRoot`'s own) is free to ignore it.
using ChildComponentInvoker =
    std::function<Iris::Component(const std::string& TagName, const nyx::runtime::Value& Props,
                                    const IrElementNode& Node)>;

// Registers the host function a real `EvaluateSlot` implementation (`MakeNyxEvaluator`
// below) needs to resolve a JSX-transform escape hatch's conditional content --
// docs/iris_nyx_evaluator_scope_gap.md's Problem 3. Not a decision-log.md §7.3 item --
// resolved entirely on iris-proto's own side, since "the Chaos runtime resolves every
// `<Slot>`" per decision-log.md §7.2 (nyx-proto has no concept of `<Slot>` at all, and
// never gains one here either -- `__chaos_slot_pick` is just an opaque integer-recording
// function from its own point of view). Call once per `nyx::host::NyxRuntime`, alongside
// `RegisterSignalDecorator`, before evaluating anything `MakeNyxEvaluator` produces against
// that runtime.
//
// Mechanism: a JsxEscapeHatch's `Segments` (`IrisIrDocument.h`) preserve the original
// interleaved text/element order (e.g. a ternary's `cond ?` / element / `:` / element), but
// Nyx has no syntax for an embedded element -- `MakeNyxEvaluator`'s `EvaluateSlot`
// reconstructs a parseable expression by substituting `__chaos_slot_pick(N)` (N = that
// element's index among `Node.Elements()`) for each Element segment, then evaluates the
// whole thing as an immediately-invoked lambda (every documented `<Slot>` escape hatch is
// itself author-written as a `() -> ...` lambda already -- chaos-ir-spec.md §4). Each call
// to this marker function records its own index into a buffer `EvaluateSlot` reads back
// afterward -- exactly the element(s) the escape hatch's own Nyx-side conditional logic
// selected, in call order.
// A single `__chaos_slot_pick` call's own recorded data -- which element index was picked,
// plus (only when the picked element came from a `.Map()`/`.Reduce()` callback -- see
// `MakeNyxEvaluator`'s own `EvaluateSlot` doc comment and
// docs/archive/iris_nyx_slot_loop_and_reload_gap_resolved.md §1) the current element/iteration-index
// `Value` that callback's lambda parameter(s) were bound to for this call, carried out here so
// a later `Convert` call can bind them into a fresh per-pick scope -- `item`/`index` only ever
// existed inside the `.Map()`/`.Reduce()` callback's own call-frame `Environment`, gone by the
// time this marker call returns.
struct ChaosSlotPick {
    std::size_t                          ElementIndex{0};
    std::optional<nyx::runtime::Value>   Item;
    std::optional<nyx::runtime::Value>   IterationIndex;
};

class ChaosSlotMarker {
public:
    void RegisterOn(nyx::host::NyxRuntime& Runtime);

private:
    friend NyxEvaluator MakeNyxEvaluator(nyx::host::NyxRuntime&, nyx::host::NyxRuntime::NyxScope&, ChaosSlotMarker&,
                                            ChildComponentInvoker, std::vector<IrisIrRuntimeError>*);

    std::shared_ptr<std::vector<ChaosSlotPick>> Selected_ = std::make_shared<std::vector<ChaosSlotPick>>();
};

// Builds a real, non-mock `NyxEvaluator` (`IrisIrRuntime.h`) whose `EvaluateProp`/
// `EvaluateText`/`EvaluateSlot` evaluate against `Scope` -- typically one component
// invocation's own live state, e.g. from `Runtime.InvokeComponent`
// (docs/iris_nyx_evaluator_scope_gap.md's Problem 2 resolution). `EvaluateComponentInvocation`
// packs the node's own evaluated props into a `NyxObject` (`typeName` set to the tag name --
// an assumed convention, not verified against any multi-file `.chaos` example; harmless
// unless Nyx script itself later `as`-casts a props value against a *different* declared
// type name) and forwards to `InvokeChild`. `EvaluateSource` is deliberately left null:
// `ReconstructNyxSource` (above) already handles every `nyx_source` region up front, before
// `WalkIrisIrDocument`/`ConvertIrElement` ever runs, so there is nothing left for a
// per-node callback to do.
//
// Event-handler props (`onPress`, etc. -- `std::function<void()>`/
// `std::function<void(std::string)>`) are not evaluated eagerly: the underlying Nyx source
// (always author-written as a `() -> ...` lambda, chaos-ir-spec.md §3.6) is re-parsed and
// re-evaluated as an immediately-invoked expression against `Scope` on every C++-side
// invocation, rather than evaluated once into a `Value` and reused -- nyx-proto exposes no
// public "call this already-evaluated Value" primitive (`Interpreter::CallCallable` is
// private), only `EvaluateInScope`'s "parse and evaluate this source text" entry point, so
// re-evaluation is the only mechanism available. This is exactly the eventual-consistency
// cost that pattern implies (each firing re-parses), acceptable for a UI event handler
// (rare compared to a render pass), not attempted for a hot path.
//
// `EvaluateSlot` also resolves `list.Map((item[, index]) -> <Card .../>)` and
// `list.Reduce((acc, item) -> ..., initial)` -- the sanctioned way to write a dynamic list of
// components inside a `<Slot>` (`nyx-scripting-language/decision-log.md` §7.4,
// docs/archive/iris_nyx_slot_loop_and_reload_gap_resolved.md §1). Each picked element carried a
// `.Map()`/`.Reduce()` callback's own bound parameter(s), extracted at reconstruction time
// (`ChaosSlotMarker`'s own doc comment) and bound into a fresh per-pick `NyxScope` before that
// element's own prop expressions (`item.name`) are converted -- two picks from the same
// `.Map()` call never see each other's bound item.
NyxEvaluator MakeNyxEvaluator(nyx::host::NyxRuntime& Runtime, nyx::host::NyxRuntime::NyxScope& Scope,
                                 ChaosSlotMarker& Marker, ChildComponentInvoker InvokeChild,
                                 std::vector<IrisIrRuntimeError>* Errors);

} // namespace Iris
