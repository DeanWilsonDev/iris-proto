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

> **Status:** Open, narrower still. A real `NyxEvaluator` (`IrisNyxEvaluator.h`) and a real mount
> driver tying it together with import resolution (`IrisNyxDriver.h`) both exist — a `.irisx`
> file on disk can genuinely be loaded, compiled, and mounted into a live `Iris::Component` tree
> end to end, including a `<Tag .../>` invocation that names a component declared in a
> *different* `.irisx` file. As of 2026-08-07, three more items are done too, consuming
> nyx-proto's same-day decision-log batch (§7.4, §9.2) per
> `docs/archive/iris_nyx_slot_loop_and_reload_gap_resolved.md`: a genuine runtime `<Slot>` loop via
> `Array<T>.Map()`/`.Reduce()`, a real hot-reload driver (`IrisNyxDriver::ReloadRoot`) for
> free-function components, and a second, class-based component authoring model (Model 2). What's
> left is narrower still: `<Native>` for `.irisx`, and a couple of smaller open items below.
> **History:** This entry consolidates the still-open tail of two now-archived entries —
> `` `NyxTokenizer` (IHostLanguageTokenizer for `.irisx`) `` and `` `Codegen` has no Nyx-target
> emission `` — both moved to `docs/archive/iris_next_steps_resolved.md` once their own scope
> closed. Read those archived entries for the full decision trail (why `.irisx` targets an IR
> rather than transpiling to C++ or Nyx text, the IR schema itself, the three real parser bugs
> found producing it) if picking this up. `docs/archive/iris_nyx_slot_loop_and_reload_gap.md` and
> its `_resolved.md` companion (also archived) have the full trail for the three 2026-08-07 items
> above — sized against real nyx-proto code first (the gap doc), then checked against nyx-proto's
> actual resolution (the resolved doc), including the concrete iris-proto-side changes each one
> needed.

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

**The mount driver — done (2026-08-06).** `Iris::IrisNyxDriver` (`include/Iris/IrisNyxDriver.h`,
`src/Iris/IrisNyxDriver.cpp`) is the piece nothing above ever tied together: given an
`IrisConfig`/`ProjectRoot` (the same two `Driver.h::CompileFile` already takes) and an entry
file path + function name, `MountRoot` reads the `.irisx` file from disk, compiles it via the
existing `CompileFile` pipeline, parses the resulting IR (`ParseIrisIrDocument`), builds a
whole-file `NyxScope` (`ReconstructNyxSource` + `CreateScope`, cached per resolved path so a
module's own top-level declarations are only ever parsed once), and mounts the named function
via `Runtime.InvokeComponent` + `MakeNyxEvaluator` + `ConvertIrElement`, wrapped in
`iris::MountComponentInstance` so `@signal` locals register against a real, heap-owned
`ComponentInstance` (`NyxSignalDecorator.h`'s own documented precondition — a `@signal` with no
ambient `ComponentInstance` is a silent no-op, the mistake this driver exists specifically to
avoid making). The per-invocation `NyxScope` `InvokeComponent` returns is heap-allocated and
kept alive via a new `ComponentInstance::DriverState` (`ComponentInstance.h`, a narrow
`shared_ptr<void>` extension slot) — every `NyxEvaluator` closure a `<Slot>` callable captured
holds a raw reference into it, and may be re-invoked by `iris::Tick()` long after the mounting
call returns, so it has to outlive that call.

**Cross-file component invocation — done (2026-08-06), the other bundled item.**
`IrisNyxDriver`'s `ChildComponentInvoker` (built once per invocation, passed to
`MakeNyxEvaluator`) resolves a `<Tag .../>` invocation purely against the *caller's own*
already-resolved `Document.Imports` (`IrImportNode::Name`/`ResolvedPath` — populated at IR-build
time by `Driver.h`'s existing `ResolveImports` call, nothing new needed there), then recurses
into the same load/compile/invoke path for the target file — exactly the seam
`IrisNyxEvaluator.h`'s own `ChildComponentInvoker` doc comment named as "an import-resolution
question... left to whoever drives a whole application." An unimported tag is reported as an
authoring error rather than guessed at (falling back to a same-file function of that name),
matching chaos-ui-authoring.md §27.1's "each component lives in its own file" convention.
Verified end to end in `tests/IrisNyxDriverTests.cpp` against real files written to a temp
directory (not hand-built fixtures) — a single-file mount, a cross-file invocation, and two
sibling invocations of the same imported component keeping independent `@signal` state (proof
each got its own `ComponentInstance`/`NyxScope`, not a shared one) — full suite (217/217)
passing, plumbed for AddressSanitizer/UndefinedBehaviorSanitizer the same way the rest of this
suite already is.

**A genuine runtime `<Slot>` loop — done (2026-08-07).** `list.Map((item[, index]) -> <Card
.../>)`/`list.Reduce((acc, item) -> ..., initial)` are now the sanctioned way to write a dynamic
list of components inside a `<Slot>` (nyx-proto's own `decision-log.md` §7.4).
`MakeNyxEvaluator::EvaluateSlot` (`IrisNyxEvaluator.cpp`) now recognises a `.Map()`/`.Reduce()`
lambda wrapping an embedded element (scanned with nyx-proto's real `nyx::Lexer` at
reconstruction time, since the parameter name has to come from the real AST, not be guessed —
`for`/typed lambda param syntax like `(int item) -> ...` genuinely requires this, not just
`(item) -> ...`), injects the callback's bound parameter(s) into the `__chaos_slot_pick(N, item[,
index])` marker call, and binds them into a fresh per-pick `Environment`/`NyxScope` before
converting that pick — the same `EvalContext`-substitution shape `InvokeComponent` already uses,
per pick instead of per invocation. `for item in list { <Card .../> }` itself remains
unsupported, by design — `.Map()`/`.Reduce()` cover every case a `for` loop would, per
nyx-proto's own decision. Verified end to end in `tests/IrisNyxEvaluatorTests.cpp` against real
`Array` evaluation (single-param `.Map()`, two-param with iteration index, `.Reduce()`), not a
mock.

**A real hot-reload driver — done (2026-08-07), for the entry component's own state.**
`IrisNyxDriver::ReloadRoot` re-renders a free-function component via the new
`NyxRuntime::ReInvokeComponent` (patches the live `FunctionDecl` in place, re-invokes fresh,
reconciles `@signal` bindings by name into the result) and reuses the *same*
`ComponentInstance`/live `Interpreter`, reporting `ComponentReloadTier::Unchanged`/
`SignalLayoutChanged` derived independently (not via `ComponentInstance::EndReloadReplay`'s
`IRIS_SIGNAL`-counting, which doesn't apply to `Environment`-backed interpreted state at all —
`nyx-scripting-language/decision-log.md` §9.2's own item 4 guidance). **Named scope boundary:**
only the named entry component's own state survives a reload this way — a nested cross-file
child invocation reached while converting its tree always mounts fresh, since a mounted
invocation's own `Component` result carries no trace of which tag name produced it (only the
Core-primitive tree it rendered into), so matching a previous nested invocation isn't answerable
from the previous `Component` tree alone without a separate position-to-instance trace this pass
doesn't build — real, sizeable follow-up work, not attempted here (see
`IrisNyxDriver.h`'s own `ReloadRoot` doc comment). Verified in `tests/IrisNyxDriverTests.cpp`
against a file genuinely rewritten on disk between two driver calls (tier `Unchanged` with a
value preserved across a render-body-only change; tier `SignalLayoutChanged` when a `@signal` is
added; falls back to an ordinary fresh mount with no prior `Instance` to reload against).

**A second component authoring model — done (2026-08-07): Model 2, class-based components.**
`class Tooltip : Component { ... }` is now a supported alternative to the existing free-function
style, per nyx-scripting-language's own §9.2 "two supported authoring models" decision (built on
top of §5.16 constructors and the pre-existing §6.4 `RegisterInheritableType`/`NyxBridge<T>`
pattern). `Iris::NyxComponentBase`/`nyx::host::NyxBridge<NyxComponentBase>`
(`NyxComponentBridge.h`) is the one marker type + registration block this repo writes (no
`.Override(...)` calls needed — nothing here ever drives a component through a C++ vtable call,
`IrisNyxDriver::InvokeClassComponent` calls `Instantiate`/reads fields directly); model detection
(`IrisNyxDriver::InvokeComponent`, checking `Interpreter::Registry()`'s `classes`/`functions`
maps for whichever name a given invocation names) picks the free-function or class path per
invocation, so both models can coexist across a project's `.irisx` files. Mount runs
`Interpreter::Instantiate(ClassName, Args)` (field defaults, then the constructor); reload reuses
the pre-existing `PatchClass`+`ReconcileInstanceFields` machinery against the same live
`NyxObject`, unchanged from §9.1 — genuinely no new nyx-proto work for that half, as that
decision predicted. **Named simplification:** the render scope's own construction (a fresh
`Environment` binding `Render`'s own declared parameter, `EvalContext::thisObject` set to the
instance) does not execute any plain Nyx statement written before `render{}` inside `Render`'s
body — `Interpreter` exposes no public "call this instance method, capture its environment"
primitive to run them through, and every documented Model 2 example has nothing there beyond the
parameter itself. Verified in `tests/IrisNyxDriverTests.cpp`: a mount running a real constructor
and binding `Render`'s own parameter, and both reload tiers.

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
  already is it. This describes the **compiled `.iris`/C++** path specifically — the
  **interpreted `.irisx`** path's own reload driver (`IrisNyxDriver::ReloadRoot`, both
  authoring models) is done, see the "Chaos runtime" entry above; the two paths don't share a
  mechanism (`iris::ReloadComponentInstance`/`BeginReloadReplay`/`EndReloadReplay` reconcile
  `IRIS_SIGNAL` storage by declaration order, which has nothing to do with interpreted
  `Environment`/`NyxObject`-backed state). **Still not built for the compiled path:** an actual
  driver calling `ReloadComponentInstance` at all — nothing in this repo currently produces the
  "recompiled `.iris` source, please reload" trigger a driver would react to, unlike the
  interpreted path where "the file changed on disk" is `ReloadRoot`'s own explicit precondition
  — and the component-invocation lockstep matching such a driver would use to find which
  `ComponentInstance` to replay at each position (`docs/iris_hot_reload_reconciliation_decision.md`
  §4, deliberately left external, single caller) — the same open problem the interpreted path's
  own `ReloadRoot` scoped down to "the entry component only," not solved generally there either.

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
