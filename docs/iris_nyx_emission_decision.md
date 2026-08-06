# Iris — `.irisx` Emission Target Decision

> **Status:** Decided — design only, not yet implemented. Answers the execution-model fork
> `docs/next-steps.md`'s "`Codegen` has no Nyx-target emission" entry (2026-08-06) left open.
> **Revision note (1):** this document originally (same day) proposed a "`NyxCodegen` emits
> Nyx source text, calling a new Iris host-bindings module registered on `NyxRuntime`" design.
> That was wrong — written without having read the actual Nyx/Chaos design docs, which turned
> out to already fully specify the answer. Superseded below after reading
> `fearless-hq/projects/nyx-scripting-language/chaos-ir-spec.md` and
> `chaos-ui-authoring.md` directly, per Dean's correction.
> **Revision note (2):** the Nyx/Chaos design docs' own prose ("the Nyx interpreter consumes
> the IR directly... hands element tree nodes back to the Chaos runtime") put Chaos-shaped
> concepts (elements, slots) inside the general-purpose Nyx interpreter — the wrong dependency
> direction (Nyx becoming aware of Chaos is the same inversion as a JS engine needing to know
> about React). Corrected across both repos, recorded as nyx-proto's own
> `decision-log.md` §7.2: the **Chaos runtime is iris-proto's own responsibility**, not
> nyx-proto's — it's the thing that walks `.chaos.ir`, resolves `<Slot>`, reconciles, and
> constructs widgets, calling into Nyx only through a generic (Chaos-agnostic) embedding
> primitive. This document's IR-*production* work (below) is unaffected by that correction, but
> references to "the Nyx/Chaos runtime" as an external, undetermined-location thing are updated
> to say plainly: it lives here, in iris-proto (or a sibling library this repo owns), once built.

## Recap of the problem

`Codegen.cpp`'s `EmitEscapeHatchExpression` copies escape-hatch text back out verbatim, and
`Driver::CompileFile` passes every non-render-block token through unchanged too — correct for
`.iris`, since the host language genuinely is C++, but for `.irisx` it means Nyx-flavored
source ends up inside a file handed to a C++ compiler. Full repro in `next-steps.md`'s own
entry; not repeated here.

## The actual answer: `.irisx`/`.chaos` target a Chaos IR, not source text in any language

`chaos-ir-spec.md`'s own framing note settles this outright: "`.iris`, `.irisx`, `.chaos` files
are functionally identical — the file extension determines the host language, not the IR
shape... During the prototype period, the Iris codebase (`iris_cc`) produces this IR when
targeting Nyx." This is not new design work to invent — it's an already-specified schema to
implement against.

**A `.chaos` file is a Nyx source file containing `render { }` blocks.** The Chaos preprocessor
(`iris_cc`, for `.irisx`) parses it and produces a **Chaos IR** — a structured JSON document
(`<file>.chaos.ir`, written alongside the source) with three region kinds:

- `nyx_source` — raw pass-through Nyx text outside any `render { }` block. Iris does not
  interpret this, exactly as it doesn't today.
- `render_block` — one `render { }` block, containing the `ElementNode` tree as JSON
  (`element`/`prop` nodes), not a compiled expression in any language.
- `nyx_expression` — escape-hatch (`{ }`/`!{ }`) content, raw Nyx text, exactly as opaque as it
  is to `Codegen.cpp` today (`EmitEscapeHatchExpression`'s current verbatim-copy behavior is
  already correct — it just needs to land in a JSON field instead of spliced text).

Every node carries its own `SourceLocation` (`file`/`line`/`column`/`length`) — this is what
replaces `#line` directives for this target, and it's a strictly better mechanism than the
`.iris` C++ path's `#line`-based remapping: no reconstruction from text hints, no dependency on
`NyxRuntime::Run`'s currently-unused `filename` parameter ever being wired up.

`chaos-ir-spec.md` §1 maps the schema directly onto types this repo already has — nothing here
needs inventing, only serializing:

| IR concept | Iris prototype equivalent |
| --- | --- |
| `SourceLocation` | `Iris::SourceLocation` |
| `ElementNode` | `Iris::ElementNode` |
| `ParsedBlock` | `Iris::RenderBlockParser::ParsedBlock` |
| `ImportStatement` | `Iris::ImportStatement` |

The two IR node kinds with no existing equivalent are exactly the two things `Codegen`/`Driver`
currently treat as opaque text already — `NyxSourceNode` (today: text passed through
untouched between edits) and `NyxExpressionNode` (today: `PropValue::Text`, copied verbatim).
No new capability is required, only a JSON-shaped destination for data Iris already extracts.

### Why this makes the earlier "Nyx-target Codegen" framing wrong

Per `chaos-ir-spec.md` §6 and `chaos-ui-authoring.md` §27.7: **"The backend never sees a
`<Slot>` node. The Chaos runtime resolves every `<Slot>` before any backend-mapping pass
runs."** Slot re-invocation, reconciliation, and widget construction for `.irisx` are the
**Chaos runtime's** own responsibility — and per nyx-proto's `decision-log.md` §7.2, that
runtime is this repo's (iris-proto's) to build, not nyx-proto's; it's simply unimplemented
here today, a separate, later piece of work from IR production. Either way, `iris_cc` itself
builds no host bindings for this: there is no `Iris::Component`/`MakeSlotCallable`-equivalent
to expose to Nyx script for the render tree. `iris_cc`'s job stops at producing correct IR
data. This is a materially smaller, better-bounded piece of work than either option originally
proposed (Nyx-source-text emission with a new host-bindings module, or a C++-translation
backend) — it's a serializer, not a second code generator or a new runtime-binding surface.

## Decision

`Driver::CompileFile` (or a sibling entry point — see open questions) gains a per-host-language
fork at the *output* stage, same seam previously proposed, but doing far less than before:

- `.iris` (host language `cpp`): unchanged. Existing `GenerateComponentExpression`/text-splicing
  path, verbatim.
- `.irisx` (host language `nyx`): bypasses `Codegen.cpp` entirely for the render block. Instead,
  walk `ParseResult.Blocks`, the raw source regions between them, and `ResolvedImports` (all
  already computed by `Driver::CompileFile` today, just currently used to build C++ text) and
  serialize them into the `chaos-ir-spec.md` §3 JSON shape via `libs/amanuensis`'s writer
  (`amanuensis::io::Writer::WriteToString` — already vendored, already used by `IrisConfig`; no
  new dependency). Output is `<source>.chaos.ir`, not a `.h` header.

Concretely, new code needed:

- A `ChaosIr` node-tree type (or build the `amanuensis::Value` tree directly — TBD, sized when
  implemented) mirroring §3's schema.
- A serializer walking `ElementNode`/`PropValue`/`JsxSegment` (existing types, `ElementNode.h`)
  into `element`/`prop`/`nyx_expression`/`literal` JSON nodes — the Nyx-target sibling to
  `Codegen.cpp`'s `ComponentEmitter`, but producing data, not an expression string.
- `Driver::CompileFile`'s per-language fork (still needs `IHostLanguageTokenizer`-style dispatch
  by extension, but the two branches now differ far more — one produces C++ text, the other
  produces a JSON document — than "two Codegen backends sharing one splicing algorithm" implied).

## What this decision does NOT require, correcting the previous version of this document

- **No Nyx-target `Codegen` backend emitting Nyx source text.** Escape-hatch/preamble text
  stays exactly as opaque as it is today; it's relocated into JSON fields, never translated,
  parsed, or reconstructed into a different textual form.
- **No new Iris-side Nyx host-bindings module** (`NyxComponentBindings.h`-shaped code
  registering `Component`/`IrisElementTag`/`MakeSlotCallable` equivalents on `NyxRuntime`) — the
  previous version of this document proposed this; it doesn't apply once the target is IR data
  consumed by a not-yet-built Chaos runtime, not Nyx script text Iris would need to make
  callable.
- **Iris does not set up a `NyxRuntime`, ever** — confirmed explicitly (2026-08-06): that's the
  consuming application's job, exactly as `RegisterSignalDecorator` already assumes (the caller
  passes in an already-constructed `NyxRuntime&`). Nothing in this decision changes that.

## Open sub-decisions this doc deliberately leaves unresolved

- **Whether `.irisx` compiling to `.chaos.ir` instead of a `.h` header changes `iris_cc`'s CLI
  contract** (`-o` currently assumes a header path) and `cmake/IrisCompileDirectory.cmake`
  (currently assumes every output is `#include`-able). Real, but a smaller, more mechanical
  question than before — not resolved here.
- **`ResolvedImports`' JSON shape is already fully specified** (`ImportNode`, §3.2) — no open
  question there, unlike the previous version of this doc, which incorrectly treated Nyx's
  import syntax as unknown. `file-model.md`/`execution-model.md` (now read) confirm `import
  Name` is identical syntax to `.iris`'s own, differing only in how `Name` maps to a filename
  (kebab-case convention, vs `.iris.json`'s `searchPaths`-based resolution) — a resolution-path
  detail, not a grammar question, and likely already handled correctly since `ImportResolver`
  doesn't hard-code a naming convention today.
- **What the not-yet-built Chaos runtime (this repo's own responsibility, per nyx-proto's
  `decision-log.md` §7.2 — not nyx-proto's) will eventually need from nyx-proto to actually walk
  `.chaos.ir` and drive Nyx evaluation.** §7.2 names the concrete gap: `NyxRuntime::Run`/
  `RunFile` only execute one whole script end-to-end today; there's no "evaluate this source/
  expression string against a live scope, return a `Value`" primitive at finer grain, which the
  Chaos runtime would need to call once per `nyx_source`/`nyx_expression` IR node. That's real,
  named, unbuilt nyx-proto-side work — not invented here, not this document's to design further.
  If and when the Chaos runtime itself needs something *from this repo's own C++ runtime*
  exposed back to it (e.g. `SlotRuntime`/`Reconciler`), Dean's guidance (2026-08-06) is to scope
  that as its own explicit mechanism — a `useIris`-shaped registration entry point the consuming
  app calls against its own `NyxRuntime`, mirroring `RegisterSignalDecorator`'s existing pattern
  — not something to guess at or build speculatively now. Nothing in the IR-production work
  above requires any of this.
- **IR generation trigger** (`chaos-ir-spec.md` §7's own open question — on save, on demand, or
  a build step) is upstream of this repo's own `iris_cc`/build-integration choice, not resolved
  by that spec either.

## Required changes elsewhere

None to produce IR. This (the serializer, `Driver::CompileFile`'s fork) is fully implementable
inside `iris_cc` against an already-published schema — no `nyx-proto` or `pharos-proto` code
needs to exist first. Consuming that IR later (the Chaos runtime — resolving `<Slot>`,
reconciling, building widgets) is also this repo's own responsibility, per nyx-proto's
`decision-log.md` §7.2, but is real, separate, not-yet-scoped follow-up work — and it does
depend on nyx-proto eventually exposing a finer-grained evaluation primitive (§7.2's
`EvaluateInScope`-shaped gap) than `Run`/`RunFile` provide today. Not a blocker for the IR
serializer this document actually decides on.

## Explicitly not requested

- Translating Nyx syntax into C++, or emitting Nyx source text — both rejected, per the
  correction above.
- Building the Chaos runtime itself (the `.chaos.ir` consumer — `<Slot>` resolution,
  reconciliation, widget construction) — confirmed as this repo's own future responsibility
  (nyx-proto `decision-log.md` §7.2), but a separate, larger, not-yet-scoped piece of work from
  IR *production*, which is all this decision covers.
- Implementing any of the above in this repo yet — this document records the chosen shape only.
  Building the IR serializer and `Driver::CompileFile`'s per-language fork is tracked as
  follow-up work in `docs/next-steps.md`, not done as part of this decision.
