# Iris — a real `NyxEvaluator` needs two nyx-proto primitives that don't exist yet

> **Status: RESOLVED (2026-08-06), fully implemented.** Both nyx-proto-side problems below
> are answered and implemented — see `nyx-scripting-language/decision-log.md` §7.3 (the
> nyx-proto-side record) and the "Resolution" notes inline in §1/§2/§3 below. **Problem 1
> needed no new nyx-proto primitive at all** — it turned out to be a pure iris-proto-side
> reconstruction step using ordinary Nyx syntax. **Problem 2 got one new, small primitive**
> — `Interpreter::CallFunctionCapturingEnvironment` / `NyxRuntime::InvokeComponent` — built
> and tested in `libs/nyx-proto` (`tests/host_test.cpp`, "Host —
> CallFunctionCapturingEnvironment / InvokeComponent" group, 3 new tests, full suite
> 130/130 passing). **Problem 3's finding is confirmed, not resolved** — still open, see
> its own section, unaffected by the rest of this update. **§3.5 (new): a fourth,
> iris-proto-only defect** was found and fixed while actually implementing the real
> evaluator against these two primitives — see that section.
>
> **The real evaluator is now built and tested**: `Iris::MakeNyxEvaluator`/
> `ReconstructNyxSource`/`ChaosSlotMarker` (`include/Iris/IrisNyxEvaluator.h`,
> `src/Iris/IrisNyxEvaluator.cpp`), verified end to end against real nyx-proto execution —
> not a mock — in `tests/IrisNyxEvaluatorTests.cpp`, including this doc's own `Button`/
> `isHovered` example. Full `test_iris` suite (212/212) clean under AddressSanitizer +
> UndefinedBehaviorSanitizer. See `docs/next-steps.md`'s "Chaos runtime" entry for the
> summary and what's still left (the mount/reload driver, cross-file component invocation,
> and dynamic/loop-shaped `<Slot>` content are all still open, not this doc's scope).
> The original problem statement below is left intact as the record of what was found and
> why; resolution notes are layered on top, not a rewrite.
>
> **Original status (superseded):** Open gap, not designed. This doc only names and sizes
> the problem, with concrete evidence from the published IR spec — no proposed API, no
> implementation. Per this ecosystem's own working convention (nyx-proto's decision-log,
> "stop on undocumented design decisions" — don't guess a cross-repo protocol
> unilaterally), this is deliberately deferred rather than invented on the spot.
> **Trigger:** nyx-proto pushed `5c45f71` ("Add EvaluateInScope embedding primitive and
> class hot-reload patch primitives"), landing the `NyxRuntime::CreateScope`/
> `EvaluateInScope` primitive `docs/iris_nyx_emission_decision.md` and `decision-log.md`
> §7.2 both named as the blocker for a real (non-mock) `NyxEvaluator`
> (`IrisIrRuntime.h`) — asked to build that evaluator now that the blocker looked
> resolved. It isn't fully resolved: `EvaluateInScope` covers one real case (evaluating a
> self-contained expression against an already-known, already-built scope) but not the
> two problems below, both required for `WalkIrisIrDocument`'s existing, already-tested
> `NyxEvaluator` contract to work against real Nyx source.

---

## 0. What's already true, and what isn't

`NyxRuntime::CreateScope(source, filename)` parses `source` as one complete program and
returns a `NyxScope` (a heap-owned `Interpreter` + a single shared `EvalContext`).
`EvaluateInScope(scope, source)` parses `source` as one bare expression and evaluates it
against that scope's stored context, repeatable any number of times. Both are real,
tested (`libs/nyx-proto/tests/host_test.cpp`), and exactly the right shape for **one**
piece of what `IrisIrRuntime.h`'s `NyxEvaluator::EvaluateProp`/`EvaluateText` need: a
self-contained prop/text expression, evaluated against a scope that already exists and
is already correctly positioned. That part is genuinely unblocked.

What's still missing is upstream of that — getting a correctly-populated scope to
evaluate *against* in the first place, for a real (not hand-built-in-a-test) `.chaos.ir`
document. Two separate problems, checked directly against `chaos-ir-spec.md`'s own §4
worked example, not assumed.

## 1. A `nyx_source` region is not, in general, an independently parseable program

`chaos-ir-spec.md` §4's own example compiles this source:

```chaos
Penumbra::Component Button(ButtonProps props) {
    @signal bool isHovered = false;

    render {
        <Frame class="button"> ... </Frame>
    }
}
```

into **two separate** `nyx_source` body nodes, split around the `render_block` in
between them:

```json
{ "kind": "nyx_source",
  "source": "Penumbra::Component Button(ButtonProps props) {\n    @signal bool isHovered = false;\n" }
```
```json
{ "kind": "nyx_source", "source": "}\n" }
```

Neither string is valid Nyx on its own — the first has an unbalanced `{` and half a
function body with no closing brace or return; the second is a bare `}` with nothing to
close. `WalkIrisIrDocument` (`IrisIrRuntime.cpp`, already implemented and tested) calls
`NyxEvaluator::EvaluateSource` once per `nyx_source` node it encounters, in document
order — exactly matching the IR's own per-region granularity. But `CreateScope` requires
"a full program" (its own doc comment). Calling `CreateScope` on either fragment above
fails to parse.

**What this means concretely:** a real `NyxEvaluator::EvaluateSource` implementation
can't just forward each node's `source` to `CreateScope` as `WalkIrisIrDocument`'s
current per-node call shape would suggest. Something has to reconstruct one complete,
syntactically valid Nyx program for the whole file — splicing a placeholder in for each
`render_block`/`nyx_expression` position the real source contained — before anything can
be parsed at all. No such splicing convention exists in `chaos-ir-spec.md`, and nothing
in nyx-proto's new primitives addresses it; `CreateScope`'s own doc comment assumes its
caller already has a complete program string in hand.

> **Resolution (2026-08-06, nyx-scripting-language/decision-log.md §7.3):** no new
> nyx-proto primitive needed. The IR only ever splits `nyx_source` around a `render_block`
> boundary — never inside one; the whole element tree, including nested `!{}` escape
> hatches, travels as `ElementNode`/`NyxExpressionNode`, never more `nyx_source` text. And
> nyx-proto confirmed `Parser::ParseStatement` treats a bare `{}` as unconditionally valid
> wherever a statement is expected (`if (Check(TokenKind::LBrace)) return ParseBlock();`,
> no dependency on a preceding `if`/`while`/`for`). So reconstruction is purely mechanical
> and entirely **this repo's** own responsibility (only iris-proto's walk over the IR body
> ever sees where a `render_block` falls, consistent with the established "Chaos depends
> on Nyx" direction — §7.1/§7.2): concatenate `body[]`'s `nyx_source` fragments in document
> order, splicing an empty block `{}` in place of each `render_block` entry, then hand the
> result to `CreateScope` unchanged. Applied to this section's own worked example, the
> reconstructed program is:
> ```nyx
> Penumbra::Component Button(ButtonProps props) {
>     @signal bool isHovered = false;
>     {}
> }
> ```
> **One precondition this — and Problem 2's resolution below — both rely on:** every
> documented `.chaos` example has `render { }` as the *last* statement in the component
> body, nothing after it. Confirm this still holds before relying on either resolution if
> a future documented example ever puts a statement after `render { }`.

## 2. No mechanism reaches a specific component invocation's live scope

Even granting problem 1 solved (a whole-file scope exists), a `render_block`'s prop
expressions (`current={player.hp}`, `!{() -> isHovered ? ... : ...}`) need to evaluate
against *that specific call's* environment — parameters bound to whatever this
invocation's actual prop values are, `@signal` locals declared with this call's own
`ComponentInstance` ambient (exactly like the compiled `.iris` path's `IRIS_SIGNAL`
already requires — see `ComponentInstance.h`). A component can be invoked many times
(a list of `<HealthBar>`s, one per player) with independent state each time; each needs
its own call frame, not the one shared scope `CreateScope` builds once.

Neither of nyx-proto's two new primitives reaches this:

- `CreateScope` builds one scope for a whole file/session, evaluated at most once per
  region — it has no notion of "the environment live during this one function call."
- `EvaluateInScope` evaluates an expression against an *already-known, already-built*
  scope — it doesn't create a scope from a live call in progress, and has no way to
  pause a function call at the point `render{}` used to sit and hand back that call's
  own environment.

**What this means concretely:** invoking a component (`EvaluateComponentInvocation`,
`IrisIrRuntime.h`) for real needs some way to run up to (or through) the point in a Nyx
function's execution where `render{}` was extracted from, and get back that specific
call's live `Environment` to evaluate the corresponding `render_block`'s expressions
against — a mid-call hook, or an explicit "call this function, get back {return value,
environment-at-marker}" primitive. Nothing in nyx-proto exposes this today; `EvalExpr`
and `MakeGlobalContext` (which `CreateScope`/`EvaluateInScope` are themselves built from)
don't either.

> **Resolution (2026-08-06, nyx-scripting-language/decision-log.md §7.3):** the second
> option — and no mid-call pause/resume mechanism was needed after all, once checked
> against `Interpreter::CallNyxFunction`'s actual implementation. Every call already
> builds a *fresh* `env` (a child `Environment` of the closure) for that call's params, and
> executes the body's top-level statements directly in `env` — not a further-nested scope
> (that only happens for `if`/`while`/nested-block bodies). Given Problem 1's precondition
> (`render{}`, spliced to `{}`, is always the body's last statement), the environment live
> when the call *returns* already **is** "the environment at the render{} marker" —
> `CallNyxFunction` was simply discarding it before. And since every call already gets its
> own fresh `env`, independent state across multiple invocations (a list of
> `<HealthBar>`s) falls out for free, with no additional machinery — nothing about this
> needed a coroutine or a genuine pause/resume.
>
> **What was built, in nyx-proto:**
> ```cpp
> // interpreter/interpreter.hpp
> struct CallResult {
>     runtime::Value value;
>     std::shared_ptr<runtime::Environment> callEnvironment;
> };
> CallResult CallFunctionCapturingEnvironment(const std::string& name, std::vector<runtime::Value> args);
> ```
> ```cpp
> // host/nyx-runtime.hpp — reuses NyxScope/EvaluateInScope entirely unchanged, just a new
> // way to construct a NyxScope: from a captured call-frame Environment instead of
> // MakeGlobalContext(). fileScope is whatever CreateScope already built over the
> // reconstructed nyx_source region (Problem 1) — call this once per mount/list-item
> // against that same fileScope.
> NyxScope InvokeComponent(NyxScope& fileScope, const std::string& functionName, std::vector<runtime::Value> args);
> ```
> **How this repo would use it, once `EvaluateComponentInvocation` is written for real:**
> ```cpp
> NyxRuntime::NyxScope invocation = nyxRuntime.InvokeComponent(fileScope, "Button", {startHoveredValue});
> // then, per prop/text node inside this mount's render_block:
> Value propValue = nyxRuntime.EvaluateInScope(invocation, propSourceText);
> ```
> `invocation` is what a `ComponentInstance` (this repo's own concept — nyx-proto has no
> awareness of it) keeps alive for that mount's lifetime, re-used across repeated
> `EvaluateInScope` calls exactly like `EvaluateInScope`'s existing contract already
> promises for a `CreateScope`-built scope.

## 3. A related, smaller finding: `PatchClass`/`ReconcileInstanceFields` don't fit `.chaos`'s free-function model

Not this doc's main gap, but found while investigating the above and worth recording so
it isn't rediscovered independently: the same nyx-proto commit (`5c45f71`) also added
`Interpreter::PatchClass`/`ReconcileInstanceFields`/`Instantiate`, described in its own
commit message as backing "the tiered `.chaos` hot-reload model." Those three are built
around Nyx **classes** — patching a class's registry entry, reconciling fields on a live
class **instance**. But `chaos-ui-authoring.md` states directly (line 51): *"A Chaos
component is a free function whose body contains a `render { }` block"* — not a class,
no instance for `ReconcileInstanceFields` to operate on. This repo's own
`docs/iris_hot_reload_reconciliation_decision.md` §1 (already implemented — see
`ComponentInstance::BeginReloadReplay`/`EndReloadReplay`) solves tier-1/tier-2 state
preservation a different way, entirely on the iris-proto side, without needing
`PatchClass`/`ReconcileInstanceFields` at all. These three nyx-proto primitives look
built for `.nyx` gameplay-script entity reload (genuinely class-based there) and
mislabeled in the commit message as also backing `.chaos` — not verified against
`chaos-ui-authoring.md`'s own free-function description before that claim was made.

> **Confirmed, not resolved (2026-08-06, nyx-scripting-language/decision-log.md §7.3):**
> nyx-proto's own design pass for §2 makes this finding concrete rather than papering over
> it: `InvokeComponent`'s captured state is a plain `Environment`, not a `NyxObject` — there
> is nothing for `PatchClass`/`ReconcileInstanceFields` to operate on for a `.chaos`
> component, full stop. Those two primitives are confirmed `.nyx` gameplay-entity-reload
> machinery only. Hot-reloading a component invocation whose state lives in a captured
> `Environment` needs its own analog of nyx-proto's §9.1 tiers — something like "re-invoke
> against a patched `FunctionDecl`, reconcile the new call's `env` bindings against the old
> one's" — genuinely different shape from field reconciliation on an instance, since
> there's no persistent object identity to reconcile *onto*, only two independent
> `Environment`s to diff. **Still not designed anywhere** — this repo's own
> `docs/iris_hot_reload_reconciliation_decision.md` §1 already solves tier-1/2 state
> preservation independently of this (via `ComponentInstance::BeginReloadReplay`/
> `EndReloadReplay`), so this may turn out to be moot for the actual reload path rather
> than a real blocker — worth checking against that mechanism before treating it as
> something that needs its own nyx-proto-side primitive.

## 3.5 A fourth finding, iris-proto-only: `JsxSegment` serialization lost interleaving order

Found and fixed while actually implementing `EvaluateSlot` against the two resolved
problems above — not a nyx-proto gap at all, a pre-existing, already-shipped defect in this
repo's own `IrisIr.cpp`.

`RenderBlockParser::ParseJsxEscapeHatch` scans a `!{ }` body into an ordered
`std::vector<JsxSegment>` — text and embedded-element runs, correctly interleaved (e.g. a
ternary's `cond ?` / element / `:` / element, in that order). But `IrisIr.cpp`'s
`SerializePropValue` took that already-ordered vector and flushed it into **two separate**
JSON fields — every `RawText` segment concatenated into `source`, every `Element` segment
into `children` — discarding which text ran before/between/after which element. Traced
directly against the scanner (`RenderBlockParser.cpp:378-472`): for chaos-ir-spec.md §4's
own ternary example, this produced `source = "() -> isHovered ?" + ":"` (both text
fragments concatenated) with the actual `? :` branches now unrecoverable from `source`
alone — the two `<Frame>` elements were correctly captured in `children`, but nothing said
which one was the true-branch and which the false-branch. `chaos-ir-spec.md` §3.7's own
worked example (`"source": "() -> isHovered"`, no trailing `?:`) turned out to be an
abbreviated illustration, not literal output — confirmed by actually running the real
serializer, not by reading the spec prose alone.

**Fix:** replaced `source`/`children` with one ordered `segments` array
(`IrNyxExpressionNode::Segments` in `IrisIrDocument.h`, mirroring `JsxSegment` 1:1) —
`{"kind": "text", "value": ...}` or a full element node, in original order. `Source()`
(concatenated text-only) and `Elements()` (element-only, in order) are convenience views for
callers that don't need interleaving (`EvaluateProp`/`EvaluateText` only ever need
`Source()`). `chaos-ir-spec.md` §3.7 and its §4 worked example were both updated to match.
Every existing test touching this shape was updated to the new fixture builders; two new
tests (`IrisIrDocumentTests.cpp`, `IrisIrTests.cpp`) assert the interleaving order directly
against the real parser/serializer, not just a hand-built fixture.

This is what actually made a real, non-mock `EvaluateSlot` possible at all — without it,
there was no way to know which embedded element corresponded to which branch of a
conditional, for the single most common `<Slot>` use case in every documented example.

## 4. What's not designed here

- ~~**The `nyx_source` reconstruction/splicing convention** (§1)~~ — **Resolved**, see §1's
  inline resolution note above. No placeholder token in the IR schema itself; the
  convention lives entirely in whatever reconstructs `nyx_source` before calling
  `CreateScope` (this repo's own responsibility).
- ~~**The per-invocation live-scope primitive** (§2)~~ — **Resolved**, see §2's inline
  resolution note above. `CallFunctionCapturingEnvironment`/`InvokeComponent`, built and
  tested in nyx-proto.
- **Whether `PatchClass`/`ReconcileInstanceFields`/`Instantiate` (§3) have any actual use
  for `.chaos` reload** — **Confirmed they don't** (see §3's inline note), but the
  replacement mechanism for captured-environment reload is still undesigned. Still open.
- ~~**The `JsxSegment` interleaving-loss defect** (§3.5)~~ — **Resolved**, see §3.5's own
  fix description. iris-proto-only; no nyx-proto involvement.
- ~~**Any implementation in this repo**~~ — **Done.** `Iris::MakeNyxEvaluator`/
  `ReconstructNyxSource`/`ChaosSlotMarker` (`include/Iris/IrisNyxEvaluator.h`,
  `src/Iris/IrisNyxEvaluator.cpp`), tested end to end against real nyx-proto execution in
  `tests/IrisNyxEvaluatorTests.cpp`. **Still not done, genuinely out of this doc's scope**:
  the mount/reload driver that ties `WalkIrisIrDocument` + this evaluator + import
  resolution + `iris::MountComponentInstance`/`ReloadComponentInstance` into a running
  application (`ChildComponentInvoker`, this evaluator's own cross-file seam, is left
  unimplemented for exactly that driver to supply); and dynamic/loop-shaped `<Slot>`
  content (only a statically-bounded conditional — a fixed set of embedded elements
  selected by a boolean/index expression — is handled, not a runtime loop producing a
  variable number of items with per-iteration prop bindings). Tracked by
  `docs/next-steps.md`'s "Chaos runtime" entry.

## 5. Cross-references

- `libs/nyx-proto` commits `5c45f71`/`bf81574` — the primitives this doc checked against and
  resolved.
- `libs/nyx-proto` `interpreter/interpreter.hpp`'s `CallFunctionCapturingEnvironment` and
  `host/nyx-runtime.hpp`'s `InvokeComponent` — the primitives this doc's Problem 2
  resolution is built on; tested in `tests/host_test.cpp`'s "Host —
  CallFunctionCapturingEnvironment / InvokeComponent" group.
- `include/Iris/IrisNyxEvaluator.h`/`src/Iris/IrisNyxEvaluator.cpp` — the real
  `NyxEvaluator` implementation built on all of the above; `tests/IrisNyxEvaluatorTests.cpp`
  is its end-to-end test coverage.
- `include/Iris/IrisIrDocument.h`'s `IrNyxExpressionSegment`/`IrNyxExpressionNode::Segments`
  and `src/Iris/IrisIr.cpp`'s `SerializePropValue` — §3.5's interleaving-order fix.
- `fearless-hq/projects/nyx-scripting-language/chaos-ir-spec.md` §3.3, §3.7, §4 — the IR
  schema and worked examples; §3.7/§4 were updated in place for §3.5's `segments` fix.
- `fearless-hq/projects/nyx-scripting-language/chaos-ui-authoring.md` line 51 — "a Chaos
  component is a free function," the fact problem 3 turns on.
- `fearless-hq/projects/nyx-scripting-language/decision-log.md` §7.2, §7.3, §9.1 — §7.2 is
  the original `EvaluateInScope` ask this doc found incomplete; §7.3 is the resolution of
  both problems this doc raised; §9.1 is the tiered-reload decision §3's finding relates
  to.
- `docs/iris_nyx_emission_decision.md` — named `EvaluateInScope` as the concrete
  nyx-proto-side gap before it existed; this doc was the follow-up once it landed but
  turned out not to fully close the gap; §7.3 closes the remainder.
- `docs/iris_hot_reload_reconciliation_decision.md` — this repo's own state-preservation
  design (§1), unaffected by anything in this doc; already implemented independent of a
  real `NyxEvaluator` existing; possibly relevant to §3's still-open reload question.
- `docs/next-steps.md`'s "Chaos runtime" entry — tracks the still-missing real
  `NyxEvaluator` implementation; this doc no longer explains a nyx-proto-side blocker for
  it, only the remaining iris-proto-side integration work (§4).
