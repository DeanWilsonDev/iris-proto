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

**A real `NyxEvaluator` implementation backed by nyx-proto.** Every callback above is
implemented only by tests' mock evaluator today — nothing yet drives them from actual Nyx
source text, so a `.irisx` component still can't run end-to-end. This is now *mostly* a
nyx-proto blocker, not an iris-proto design gap: per `chaos-ir-spec.md` §6 / `decision-log.md`
§7.2, nyx-proto has no "evaluate this source against a live scope, repeatable across a
component's lifetime" embedding primitive yet (`NyxRuntime::Run`/`RunFile` only execute a whole
script end-to-end) — "genuinely new Phase 6 work, not yet started" there. Note `nyx-proto`'s
`nyx::interpreter::Interpreter::EvalExpr`/`MakeGlobalContext` (already linked into `iris` via
this repo's `nyx-runtime` CMake target) evaluate a bare expression against a scope, but that
alone doesn't close the gap: a `NyxSourceNode`/`NyxExpressionNode`'s raw text is a *fragment* of
a larger Nyx program (the file's own `render { }` block already cut out of it) and, for a `!{ }`
JSX-transform escape hatch, still contains literal embedded `<Tag>` runs that don't lex as
ordinary Nyx at all — reassembling/re-lexing that correctly, and deciding how a `NyxEvaluator`
implementation substitutes each embedded element's spot in the source for the already-converted
`Iris::Component` `IrElementConverter` hands it, is real design work belonging to whoever
implements the real evaluator (this repo, once nyx-proto's own primitive exists), not solved by
this entry.

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
  narrower style-only reload) is a separate, larger, deliberately-deferred design question —
  sized against the real code (not designed) in `docs/iris_interpreted_host_hot_reload_gap.md`.
  nyx-proto's `decision-log.md` §9.1 (2026-08-06) has since settled the target shape on its
  side (a three-tier model, not unconditional full remount) — `docs/
  iris_hot_reload_alignment_decision.md` works out what that implies here. Still blocked on
  the Chaos runtime above (no real `NyxEvaluator` yet), and on the whole-app reconcile-target
  registry §2 of that new doc names as a concrete prerequisite, not just a someday item.

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
