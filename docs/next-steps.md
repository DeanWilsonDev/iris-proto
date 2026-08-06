# Iris — Next Steps

Running log of active feature requests / capability gaps for this repo, per
`~/.claude/skills/feature-request`. This file shows **only currently-open work** — once an
entry is fully resolved (or resolved to the point that nothing remaining is this repo's own
action item), it's moved wholesale to `docs/archive/iris_next_steps_resolved.md` rather than
kept here with a `RESOLVED` marker, so this doc stays a fast read of what's actually left. For
the full historical status log of Stage 0–3 work (all done, effectively a changelog), see
`docs/archive/iris_next_steps.md`; for closed design decisions, see the various
`docs/iris_*_decision*.md` files — `CLAUDE.md` explains how those relate to
`docs/iris_core_spec.md`, the authoritative language reference.

This file coalesces two previously separate requirement docs
(`lustre_hotreload_iris_requirements.md`, `penumbra_iris_lustre_componentization_gaps_requirements.md`,
both since removed) plus one gap identified directly against `docs/iris_core_spec.md`.

---

## Chaos runtime — the `.iris.ir` consumer — IR-to-Component walk is built; real Nyx evaluation is not (2026-08-06)

> **Status:** Open, narrower than before. `IrisIrRuntime.h`/`.cpp` now does everything the
> *iris-proto side* of the Chaos runtime can do without a working Nyx embedding primitive: read
> a `.iris.ir` document, walk `render_block`/`ElementNode`/`<Slot>` nodes, and produce a live
> `Iris::Component` tree — the exact same backend-agnostic IR `Codegen.h` produces for the
> compiled `.iris` path, so `iris::MountComponentInstance`/`iris::ResolveSlots`/
> `Reconciler.h`/`iris::Tick()` (Stage 3, already built and tested) consume it unchanged. What
> remains is entirely on the "make Nyx actually evaluate real script text" side, most of which
> is a nyx-proto blocker, not an iris-proto one — see below.
> **History:** This entry consolidates the still-open tail of two now-archived entries —
> `` `NyxTokenizer` (IHostLanguageTokenizer for `.irisx`) `` and `` `Codegen` has no Nyx-target
> emission `` — both moved to `docs/archive/iris_next_steps_resolved.md` once their own scope
> closed. Read those archived entries for the full decision trail (why `.irisx` targets an IR
> rather than transpiling to C++ or Nyx text, the IR schema itself, the three real parser bugs
> found producing it) if picking this up.

### What's already done, upstream of this

- `Iris::NyxTokenizer`/`Iris::CreateHostLanguageTokenizer` — `.irisx` dispatches through a real
  Nyx-aware tokenizer, not `CppTokenizer`.
- `Iris::RegisterSignalDecorator`/`ComponentInstance` — `@signal` reactive state authoring
  works for a `.irisx` component body.
- `Iris::BuildIrisIr` (`include/Iris/IrisIr.h`, `src/Iris/IrisIr.cpp`) — `.irisx` compiles to a
  fully-populated `.iris.ir` JSON document (`chaos-ir-spec.md`'s schema, including this repo's
  own `key`/`ref`/`TextNode` schema additions), not C++. `iris_cc`/`cmake/IrisCompileDirectory.cmake`
  both know the `.irisx` → `.iris.ir` naming convention.

### What's now done (this entry)

- `Iris::ParseIrisIrDocument` (`include/Iris/IrisIrDocument.h`, `src/Iris/IrisIrDocument.cpp`)
  — deserializes a `.iris.ir` JSON document (`Amanuensis::Value`) back into a typed C++ tree,
  the read-side counterpart of `BuildIrisIr`. Reports malformed/missing fields rather than
  asserting, matching `RenderBlockParser::Result`'s own "partial result plus errors" shape.
- `Iris::ConvertIrElement`/`Iris::WalkIrisIrDocument` (`include/Iris/IrisIrRuntime.h`,
  `src/Iris/IrisIrRuntime.cpp`) — walks that tree and produces live `Iris::Component` values,
  mirroring `Codegen.cpp`'s `ComponentEmitter` node-for-node (same `CorePrimitives.h` tag/prop/
  child rules, same `<Text>`/`<Inline>` text-child handling, same `key`/`ref` IIFE-equivalent
  wrapping) but *evaluating* to a value instead of emitting C++ source text. `<Slot>` becomes a
  real `Iris::IrisSlotCallable` (always the list-returning shape — a 0-or-1-length list behaves
  identically to the single-`Component` shape through the existing `SlotState`/
  `ReconcileChildrenAt` machinery, so there's no need for `MakeSlotCallable`'s compile-time
  `if constexpr` dispatch here), so it plugs directly into `iris::ResolveSlots`/`SlotState`/
  `Reconciler.h`/`iris::Tick()` completely unchanged from the compiled `.iris` path.
- The seam where real Nyx evaluation would plug in is a `NyxEvaluator` struct of injected
  callbacks (`EvaluateProp`/`EvaluateText`/`EvaluateSlot`/`EvaluateComponentInvocation`/
  `EvaluateSource`) — the same "runtime supplies a callback, this code never knows what's on
  the other side of it" pattern `SlotRuntime.h`'s `MountFn` already uses for widget
  construction. Covered by `tests/IrisIrRuntimeTests.cpp` against a mock evaluator (31 new
  tests total, alongside `tests/IrisIrDocumentTests.cpp`'s deserializer round-trip coverage).

### What's still actually missing

**A real `NyxEvaluator` implementation backed by nyx-proto — done (2026-08-06).**
`docs/iris_nyx_evaluator_scope_gap.md` sized two real gaps against `chaos-ir-spec.md` §4's
own worked example (a `NyxSourceNode`'s raw text being an unparseable *fragment*; no
primitive for a specific component invocation's own live scope) — nyx-proto closed both,
first with `NyxRuntime::CreateScope`/`EvaluateInScope` (commit `5c45f71`) and then
`Interpreter::CallFunctionCapturingEnvironment`/`NyxRuntime::InvokeComponent` (commit
`bf81574`), recorded in nyx-scripting-language's own `decision-log.md` §7.3. On top of
those, this repo found and fixed a third, unrelated defect while building the real
evaluator: `IrisIr.cpp`'s `JsxSegment` serializer flushed a `!{ }` escape hatch's text and
embedded-element runs into two separate JSON fields (`source`/`children`), silently
discarding their relative order — broke exactly the conditional-rendering pattern
(`cond ? <A/> : <B/>`) chaos-ir-spec.md's own example uses. Fixed by replacing that shape
with one ordered `segments` array (`IrNyxExpressionNode::Segments`,
`chaos-ir-spec.md` §3.7 updated to match) — see `docs/iris_nyx_evaluator_scope_gap.md`'s
own added section for the full finding.

`Iris::MakeNyxEvaluator`/`ReconstructNyxSource`/`ChaosSlotMarker`
(`include/Iris/IrisNyxEvaluator.h`, `src/Iris/IrisNyxEvaluator.cpp`) are the real, non-mock
implementation: `EvaluateProp`/`EvaluateText` evaluate directly against a component
invocation's own `NyxScope`; `EvaluateSlot` resolves a JSX-transform conditional by
substituting a `__chaos_slot_pick(N)` marker call for each embedded element (per the fixed,
order-preserving `Segments`) and reading back which index(es) actually got invoked;
event-handler props (`onPress`, etc.) re-evaluate their source as an immediately-invoked
lambda on every C++-side firing, since nyx-proto exposes no public "call this
already-evaluated Value" primitive. `EvaluateComponentInvocation` packs evaluated props into
a `NyxObject` and forwards to a caller-supplied `ChildComponentInvoker` — cross-file
import/component resolution is deliberately left to that callback, not attempted here (a
driver's job, still not built — see below). Verified end to end against real nyx-proto
execution (not a mock) in `tests/IrisNyxEvaluatorTests.cpp`, including chaos-ir-spec.md §4's
own `Button`/`isHovered` ternary example, independent per-invocation `@signal` state across
two mounts of the same component, and event-handler re-evaluation — full suite (212/212)
clean under AddressSanitizer + UndefinedBehaviorSanitizer.

**Still open:** the actual reload/mount driver that ties `WalkIrisIrDocument` +
`MakeNyxEvaluator` + import resolution + `iris::MountComponentInstance`/
`ReloadComponentInstance` into something that runs a whole `.irisx` application — not built,
not this entry's scope. `<Slot>` support is also narrower than the full language: only a
statically-bounded conditional (a fixed, finite set of embedded elements selected by
evaluating a boolean/index expression) is handled — a genuine runtime loop producing a
dynamic number of items from one embedded element, with per-iteration prop bindings, needs
the marker call to carry per-iteration data into a correspondingly-scoped `Convert()` call,
which `MakeNyxEvaluator` doesn't attempt.

**`<Native>` is unsupported for `.irisx`.** `ConvertIrElement` reports an error for it — its
`build` prop evaluates to an opaque `Umbra::IWidget` handle, which has no natural
`NyxEvaluator` callback shape (unlike every other escape hatch here, its result isn't
representable as a prop, text, or `Component`). Only the compiled `.iris`/`Codegen` path
supports `<Native>` today.

**A `<Slot>` re-invocation's own conversion errors have no durable sink.** `ConvertIrElement`
takes an `IrisIrRuntimeError*` that may be `nullptr`; the `IrElementConverter` closure a
`<Slot>`'s `EvaluateSlot` callback receives passes `nullptr`, since a re-invocation (driven by
`iris::Tick()`, long after the original `WalkIrisIrDocument` call that produced the `<Slot>`
already returned its own one-shot, by-value error list) has nowhere to report into yet. Minor
compared to the above, but real — a diagnostics story for this is future work.

### Also open, smaller

- **IR generation trigger** (`chaos-ir-spec.md` §7) — on save, on demand, or a build step?
  `iris_compile_directory` answers "as a build step, via CMake" for a compiled pipeline, but an
  editor/LSP-triggered regeneration for a real hot-reload workflow remains an open question.
- **Real hot-reload of component logic itself** (state, structure, handlers, not just Lustre's
  narrower style-only reload) — sized against the real code in `docs/
  iris_interpreted_host_hot_reload_gap.md`, then given a concrete design in `docs/
  iris_hot_reload_reconciliation_decision.md` (2026-08-06) following nyx-proto's own tiered
  `decision-log.md` §9.1. **The two core primitives that design named are now implemented and
  tested** (`test_iris`, `ComponentInstance`/`ReloadTarget` groups): `iris::
  ReloadComponentInstance`/`ComponentInstance::BeginReloadReplay`/`EndReloadReplay`
  (`ComponentInstance.h`) replay a render body against an already-mounted instance, reusing
  `@signal`/`IRIS_SIGNAL` storage by declaration order and reporting tier 1
  (`ComponentReloadTier::Unchanged`) vs. tier 2 (`SignalLayoutChanged`) as a structural side
  effect; `iris::ReloadTarget` (`ReloadTarget.h`/`.cpp`, registered via `IrisRuntime::
  RegisterReloadTarget`/`GetReloadTarget`, alongside not instead of `RegisterRoot`/`GetRoot`)
  is the owning root-widget-plus-prior-tree registry `ReconcileWidget` needs but nothing held
  before. Tier 3 needed no new code — `ReconcileWidget`'s existing tag/key-mismatch fallback
  already is it. **Still not built:** the actual reload driver — blocked on the Chaos runtime
  above (no real `NyxEvaluator` yet) — and the component-invocation lockstep matching a driver
  would use to find which `ComponentInstance` to replay at each position (`docs/
  iris_hot_reload_reconciliation_decision.md` §4, deliberately left external, single caller).

### Explicitly not requested

- Reimplementing `render{}`/JSX parsing, `@signal` lifting, or IR production as new work here —
  all already done (see "What's already done" above).
- An implementation from the `pharos-proto` side — this is `iris-proto`'s own architecture
  decision to make, not something to hack around in a consuming application repo.

---

## Gradient-fill Lustre property — cross-reference only, not an Iris action item (2026-07-20)

> **Status:** Open, but belongs to `lustre` (property table) and `penumbra-proto`
> (`Renderer::DrawGradientRect` already exists there) — recorded here only because it was found
> during an Iris/Penumbra/Lustre componentization investigation and blocks two consumers
> (`GradientButton`, `ExplorerPanel`'s `TreeRow` selection fill) from a full `.iris`/`.lustre`
> rewrite. No Iris-side change requested.

`lustre_core_spec.md` §2's property table has `background-color` only, no
`background-image: linear-gradient(...)` or equivalent. File any follow-up against `lustre`'s
own `docs/`, not here.

---

## Popup/overlay/z-order layer — cross-reference only, not an Iris action item (2026-07-20)

> **Status:** Open, but belongs to `penumbra-proto` — already tracked there via
> `docs/penumbra_requirements.md` item 5. Recorded here only because it's the reason
> `ColorFilterDropdown`'s popover has no representable tree for an Iris/Lustre migration to
> target at all. No Iris-side change requested.

---
