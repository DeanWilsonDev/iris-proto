# Iris — consuming nyx-proto's 2026-08-07 decision-log batch

> **Status:** Both gaps `docs/archive/iris_nyx_slot_loop_and_reload_gap.md` sized are now resolved
> on nyx-proto's side (`nyx-scripting-language/decision-log.md` §7.4 and §9.2, both
> 2026-08-07, both implemented and tested — 168/168 nyx-proto tests passing). Neither gap's
> resolution is a drop-in — this doc works out the concrete iris-proto-side changes each one
> needs, checked directly against this repo's current code (`IrisNyxEvaluator.cpp`,
> `IrisNyxDriver.cpp`/`.h`, `ComponentInstance.h`, `NyxTokenizer.cpp`), not assumed. Also
> covers three more items from the same nyx-proto batch that aren't part of either named gap
> but are relevant here: one (`SetErrorHandler`) turns out **not** to apply to how this repo
> actually calls into nyx-proto; two (constructors, lambdas capturing `this`) are prerequisites
> for a second, larger, not-yet-started `.irisx` authoring model this repo doesn't support at
> all yet — sized here as a distinct, lower-priority opportunity, not folded into the two
> resolved gaps above.

---

## 1. Resolved — `Array<T>.Map()`/`.Reduce()` unblocks the runtime `<Slot>` loop

**What nyx-proto now provides** (`decision-log.md` §7.4). `NyxArray` gained two instance
methods, callable through the existing `CallInstanceMethod` path — no new
`NyxRuntime::RegisterFunction` signature, no environment-snapshot machinery:

```nyx
list.Map((item) -> <Card name={item.name} />)
list.Map((item, index) -> <Card name={item.name} key={index} />)
list.Reduce((acc, item) -> acc + [<Card name={item} />], [])
```

`Map`'s callback receives `(element)` or `(element, index)` depending on the lambda's
*declared* parameter count (read from its `FunctionDecl`/`LambdaExpr`, not inspected at call
time) — a named function passed as the callback (`list.Map(BuildCard)`) gets the same arity
detection. `Reduce`'s callback receives `(accumulator, element)`; the accumulator's type is
whatever the callback returns, unconstrained by the element type.

This is the primitive gap doc §1 asked for: unlike a `for` loop (where the loop variable only
exists inside the loop's own `Environment`, unreachable from a later, separate evaluation
call), a `.Map()`/`.Reduce()` callback exposes the current element as a named **parameter** —
something `EvaluateSlot`'s reconstruction step can identify by name and inject directly into
the marker call as a plain `Value` argument.

**What `for item in list { <Card/> }` still doesn't do.** Per the same decision, a runtime
`for` loop inside a `NyxExpressionNode`/slot position remains explicitly unsupported.
`.Map()` covers every case a `for` loop would have (including index access via the second
param) — `.irisx` authors write `list.Map(...)` in a slot position, not `for`.

### Concrete change needed in `MakeNyxEvaluator::EvaluateSlot`

`src/Iris/IrisNyxEvaluator.cpp:157-191`, the exact code gap doc §1 pointed at, still only
substitutes a bare `__chaos_slot_pick(N)` for every element segment:

```cpp
// current (IrisNyxEvaluator.cpp:171-178)
for (const IrNyxExpressionSegment& Seg : Node.Segments) {
    if (Seg.Kind == IrNyxExpressionSegmentKind::Text) {
        Reconstructed += Seg.Text;
    } else {
        Reconstructed += "__chaos_slot_pick(" + std::to_string(ElementIndex) + ")";
        ++ElementIndex;
    }
}
```

This needs to recognise when a `Text` segment ends in `.Map(...)`/`.Reduce(...)` wrapping an
embedded-element segment, and inject the callback's own parameter name(s) into the marker
call instead of leaving it bare:

1. **`.Map((param) -> ...)` or `.Map((param, indexParam) -> ...)`** — any
   `__chaos_slot_pick(N)` inside that lambda body becomes `__chaos_slot_pick(N, param)` or
   `__chaos_slot_pick(N, param, indexParam)`.
2. **`.Reduce((acc, param) -> ..., initial)`** — becomes `__chaos_slot_pick(N, param)` — the
   *second* lambda parameter is the current element; the first (accumulator) carries no
   per-element meaning for slot purposes.

**This can't be pure text substitution.** To know which identifier to append, the
reconstruction step has to parse enough of the `.Map(...)`/`.Reduce(...)` call — at minimum,
find the lambda's parameter list between the outermost matching `(` `)` of `.Map(`/`.Reduce(`
and pull out the identifier name(s) before `->`. `Segments` today only carries `Text` runs and
opaque `IrNyxExpressionSegmentKind::Element` markers (`chaos-ui-authoring.md`/`chaos-ir-spec.md`
§3.7) — there's no existing structured representation of "this text segment is a `.Map()` call
wrapping the next element segment," so this parsing has to happen freshly at reconstruction
time. **`Iris::NyxTokenizer` (`include/Iris/NyxTokenizer.h`) is the wrong tool for this** —
checked directly: it collapses every keyword to a plain `Identifier`, coarsens both string
literal kinds together, discards whitespace, and its own doc comment says it isn't wired into
any compilation path yet, only built to satisfy `IHostLanguageTokenizer`'s contract in
isolation — none of that matters for `RenderBlockParser`'s brace-balancing use case, but this
job needs exact identifier boundaries, not a coarsened token stream. Better: call nyx-proto's
real `nyx::Lexer`/`nyx::TokenKind` directly (already linked — `NyxTokenizer.cpp` itself
`#include "lexer/lexer.hpp"` the same way) on just the `.Map((`/`.Reduce((...)` substring, and
walk its real token stream to the matching `RParen`/`Arrow`, the same precision
`ParseParenOrLambda`'s own lambda-param scan uses on nyx-proto's own side
(`nyx-proto/src/parser/parser.cpp:1260-1274`) — not a second hand-rolled tokenizer.

**Default-argument parameters need to still resolve to a name.** nyx-proto's same batch added
default parameter values (§5.14, e.g. `(item, index = 0) -> ...`) specifically so a `.Map()`
callback can omit the index parameter's *usage* without needing `_` as a placeholder — but the
parameter still has a declared *name* either way. The scanner above only needs the identifier
before an optional `=`/`:` — it can stop there and doesn't need to evaluate or even fully skip
the default expression, since it's only extracting names, not parsing a runnable copy of the
callback.

### Concrete change needed in `ChaosSlotMarker`

`src/Iris/IrisNyxEvaluator.cpp:120-128` currently records only the picked index:

```cpp
Runtime.RegisterFunction("__chaos_slot_pick", [Selected](std::vector<Value> Args) -> Value {
    if (!Args.empty() && Args[0].Kind() == ValueKind::Int) {
        Selected->push_back(static_cast<std::size_t>(std::get<int32_t>(Args[0].data)));
    }
    return Value();
});
```

`Selected_` (`std::vector<std::size_t>`, `include/Iris/IrisNyxEvaluator.h`) needs to become
something that also carries the extra `Value` argument(s) — e.g.
`std::vector<std::tuple<std::size_t, nyx::runtime::Value, std::optional<nyx::runtime::Value>>>`
(index, item, optional iteration-index) — and the registered callback needs to read `Args[1]`/
`Args[2]` when present, not just `Args[0]`.

### Concrete change needed in `Convert`

`EvaluateSlot`'s final loop (`IrisNyxEvaluator.cpp:184-189`) calls `Convert(Elements[Index])`
with no per-element data:

```cpp
for (std::size_t Index : Picked) {
    if (Index < Elements.size()) {
        Out.push_back(Convert(Elements[Index]));
    }
}
```

`Convert` (`IrElementConverter`, `include/Iris/IrisIrRuntime.h`) evaluates that element's own
prop expressions (`item.name`) by calling back into `EvaluateProp`/`EvaluateText`, which
evaluate against the single fixed `NyxScope` passed to `MakeNyxEvaluator`
(`Runtime.EvaluateInScope(Scope, Node.Source())`, `IrisNyxEvaluator.cpp:135,150`). For a picked
element that came from `.Map()`, its prop expressions (`item.name`) need to resolve `item`
against the *carried* per-element `Value`, not the outer scope. This needs the same
`EvalContext`-substitution shape `InvokeComponent`/`ReInvokeComponent` already use elsewhere
(`nyx-scripting-language/decision-log.md` §7.3): bind the carried item (and iteration index, if
present) into a fresh child `NyxScope`/`Environment` — most likely via
`Runtime.CreateScope`'s pattern plus a synthetic `Environment::Define` for the parameter
name(s) extracted above — before calling `Convert` for that pick specifically. Concretely,
`EvaluateSlot`'s picked-index loop needs to become something closer to: for each `(index, item,
optionalIterationIndex)` tuple, build a scope where the extracted parameter name is bound to
`item` (and the extracted index-param name, if any, bound to `optionalIterationIndex`), then
run `Convert(Elements[index])` with `EvaluateProp`/`EvaluateText` temporarily evaluating
against *that* scope instead of the outer one — `Eval.EvaluateProp`/`EvaluateText` currently
capture `&Scope` by reference (`IrisNyxEvaluator.cpp:135,153`), so this likely means threading a
scope override through `IrElementConverter` rather than mutating the shared `Scope` in place
(two picks in the same `.Map()` call must not see each other's bound item).

### Not part of this — the reconciler-integration side

None of the above changes anything about how `Iris::IrisSlotCallable`/`SlotState`/
`ReconcileChildrenAt` (`Reconciler.h`) diff repeated calls to `EvaluateSlot` against each
other — a `.Map()`-produced picked-element list should already flow through the existing
`IrisSlotCallable` "always the list-returning shape" path (`docs/next-steps.md`'s own note:
"a 0-or-1-length list behaves identically to the single-`Component` shape through the existing
`SlotState`/`ReconcileChildrenAt` machinery"), unchanged.

---

## 2. Resolved — `PatchFunction`/`ReInvokeComponent` unblocks free-function hot reload

**What nyx-proto now provides** (`decision-log.md` §9.2). Two new primitives, mirroring
§9.1's class-based tier 1/2 pair (`PatchClass`/`ReconcileInstanceFields`) but for the
free-function `Environment`-based shape `InvokeComponent` actually produces:

```cpp
// interpreter/interpreter.hpp
void PatchFunction(const std::string& name, std::shared_ptr<ast::FunctionDecl> newDecl);

// host/nyx-runtime.hpp
NyxScope ReInvokeComponent(NyxScope& oldScope, const std::string& source,
                            const std::string& functionName, std::vector<runtime::Value> args);
```

`ReInvokeComponent` re-parses `source`, patches `functionName`'s declaration on `oldScope`'s
already-live `Interpreter` (same interpreter, `Interpreter` is not rebuilt), re-invokes it
fresh via the existing `InvokeComponent` mechanism to get a new call-frame `Environment`, then
reconciles `oldScope`'s own bindings into the new one — **but only bindings backed by a write
observer** (`Environment::OwnHasOnWrite` — installed by `@signal`'s own `OnApply` via
`NyxVariable::OnWrite`, `NyxSignalDecorator.h`'s registration path already does this, nothing
else installs one). A same-name, same-`ValueKind` `@signal` binding carries its old live value
forward into the new scope; a plain (non-`@signal`) local is deliberately **not** reconciled —
it's recomputed fresh by the re-invocation every time, matching how an ordinary render-body
local should behave across a reload. Anything new, removed, or retyped between old and new
source also just keeps whatever the fresh invocation produced.

This directly answers gap doc §2: "re-invoke the patched `FunctionDecl` fresh, then reconcile
the new call's `env` bindings against the old one's" is now a real, tested primitive, not an
open question.

### What this repo still has to build — the actual reload driver

`ReInvokeComponent` gives the *evaluation* primitive; nothing about *finding which
`ComponentInstance`/`NyxScope` to reload* is nyx-proto's concern — `docs/
iris_hot_reload_reconciliation_decision.md` §4 already flagged "the component-invocation
lockstep matching a driver would use to find which `ComponentInstance` to replay at each
position" as deliberately left to a single external caller, and that's still true here; nothing
in today's nyx-proto batch resolves it. Concretely, this repo needs:

1. **A reload entry point on `IrisNyxDriver`.** `include/Iris/IrisNyxDriver.h` currently only
   exposes `MountRoot` (always a fresh mount via `Runtime_.InvokeComponent`) — there is no
   `ReloadRoot`/`Reload`-shaped method yet. A new entry point needs to walk the *previous*
   render's component tree in lockstep with a re-walk of the (possibly changed) `.iris.ir`
   document, find each surviving `ComponentInstance` (existing `key`/position identity, the
   same one `Reconciler.h`'s ordinary list diffing already uses per `docs/
   iris_hot_reload_alignment_decision.md` §1), and for a free-function component specifically,
   call `Runtime_.ReInvokeComponent(oldScope, newSource, functionName, args)` instead of
   `Runtime_.InvokeComponent(fileScope, functionName, args)` — where `oldScope` is recovered
   from that `ComponentInstance`'s existing `DriverState` (`ComponentInstance.h`'s
   `shared_ptr<void> DriverState`, already used by `InvokeComponent` today,
   `IrisNyxDriver.cpp:116`) cast back to `NyxRuntime::NyxScope`.
2. **`GetFileScope`'s cache needs invalidating for a changed file.** `GetFileScope`
   (`IrisNyxDriver.cpp:65-74`) caches a whole-file `NyxScope` keyed by resolved path and never
   rebuilds it — correct for a first mount, wrong for a reload where the file's own top-level
   declarations (anything outside the one function being patched) may also have changed. A
   reload path likely needs to decide whether the *whole file* needs re-`CreateScope`'d (tier
   3-equivalent for this file) or whether only the one function's `FunctionDecl` needs patching
   in place (tiers 1/2) — this decision isn't nyx-proto's; `PatchFunction`/`ReInvokeComponent`
   operate on an already-live scope either way, so the file-level "did anything else change"
   check has to happen here first.
3. **Tier 3 (irreconcilable) still means full remount**, exactly as `docs/
   iris_hot_reload_reconciliation_decision.md`/`decision-log.md` §9.1 already established for
   the class-based case — nothing new to design there, just apply the same fallback when a
   free-function reload can't reconcile (e.g. the function was removed, or its parameter shape
   changed incompatibly).
4. **`ComponentInstance::BeginReloadReplay`/`EndReloadReplay` are the wrong mechanism for this
   path, not a reusable piece.** Those two (`ComponentInstance.h:167,178`) replay a render body
   against an already-mounted instance by reusing `IRIS_SIGNAL` storage *by declaration order*
   — built for the compiled `.iris`/C++ path, where `IRIS_SIGNAL` macros expand to a fixed,
   ordered sequence of storage slots at compile time. A `.irisx` component's `@signal` state
   lives in a Nyx `Environment` instead, reconciled *by name* (`Environment::OwnHasOnWrite`),
   not by declaration order — the interpreted-path reload driver should call
   `Runtime_.ReInvokeComponent` directly rather than going through
   `BeginReloadReplay`/`EndReloadReplay`/`iris::ReloadComponentInstance` at all. Whatever tier
   result the driver reports back (`ComponentReloadTier::Unchanged`/`SignalLayoutChanged`) for
   an `.irisx` reload should be derived independently — e.g. by comparing the old and new
   `Environment`'s own binding sets/kinds (the same comparison `ReInvokeComponent` already does
   internally to decide what to reconcile) — not borrowed from the `IRIS_SIGNAL`-counting logic
   inside `EndReloadReplay`.

None of the above is designed here — matching this doc's own predecessor's posture, this names
and sizes the remaining work against nyx-proto's now-real primitive, it doesn't propose the
driver's exact shape.

---

## 3. Checked, does not apply — `NyxRuntime::SetErrorHandler`

nyx-proto's batch also added `NyxRuntime::SetErrorHandler(std::function<void(const Value&)>)`
(`decision-log.md` §5.6) so an unhandled Nyx error reaching the top of a script is reported to
the C++ host instead of silently discarded. **Checked directly against `host/nyx-runtime.hpp`:
this handler only fires from `Run`/`RunFile`**, when their internal `RunApplication` call
returns an error value. `IrisNyxDriver` never calls `Run`/`RunFile` — every evaluation goes
through `CreateScope`/`InvokeComponent`/`ReInvokeComponent`/`EvaluateInScope`, none of which
consult `SetErrorHandler`. Registering one on `Runtime_` in `IrisNyxDriver`'s constructor would
compile and do nothing.

**A related, pre-existing gap worth flagging separately (not something today's nyx-proto
changes require action on):** checked `IrisNyxEvaluator.cpp`/`IrisNyxDriver.cpp` directly —
neither catches `nyx::runtime::RuntimeError` around any `Runtime.EvaluateInScope(...)` call
(`EvaluateProp`, `EvaluateText`, `EvaluateSlot`, `InvokeAsLambda` all call it uncaught), and
none of them checks `Interpreter::IsErrorValue` on an evaluated `Value` either — `ValueToPropValue`'s
`default:` case silently degrades any error-Enum `Object`-kind result to an empty string
(`IrisNyxEvaluator.cpp:41-43`) rather than reporting it into `Errors_`. This is unrelated to
`SetErrorHandler` (which can't help here regardless) and predates this batch of nyx-proto
changes — noted here only because it was found while checking whether `SetErrorHandler` was
relevant, not because anything in today's batch caused or fixes it.

---

## 4. New opportunity, not urgent — class constructors enable a second `.irisx` authoring model

nyx-proto's batch also added class constructors (`decision-log.md` §5.16:
`ClassName(...) { ... }`, field defaults run first, then the constructor body, base
constructor implicitly runs before derived) and lambdas capturing `this` from their creating
context (§5.13). Together with `Interpreter::Instantiate(className, args)` now accepting
constructor arguments, these are the last nyx-proto-side pieces `decision-log.md` §9.2's
"Model 2 — Class-based components" needs:

```nyx
class Tooltip : Component {
public:
    TooltipProps props
    @signal bool isVisible = false

    Tooltip(TooltipProps props) { this.props = props }
    void Render(TooltipProps props) { ... }
}
```

**This repo implements none of Model 2 today** — confirmed by grep, zero references to
`RegisterInheritableType`/`Instantiate`/`PatchClass` anywhere in `src`/`include`/`tests`. Every
`.irisx` component this repo can mount is Model 1 (free function). Building Model 2 support is
new, separately-scoped work, not a fix to anything broken — sized here only so it's not
rediscovered from scratch later:

- A `Component` base type registered via `RegisterInheritableType<Component>` (the
  `RegisterType`/`NyxBridge<T>` pattern `decision-log.md`'s Phase 6 §6.4 already established)
  — one `NyxBridge<Component>` specialization this repo writes once, per that decision's own
  design; game/UI code authoring `.irisx` files never touches it directly.
- Detection of which model a given `.irisx` file uses (free function vs. class extending
  `Component`) — `IrisNyxDriver`/`LoadDocument` has no such check today; the compiled IR
  (`IrisIrDocument`) would need to carry (or `IrisNyxDriver` would need to derive from the
  parsed `DeclRegistry`) which shape a given entry function/class is.
- A second mount path: `Instantiate(className, args)` for initial mount, `PatchClass` +
  `Render(props)` for tier 1/2 reload — reusing §9.1's already-built
  `PatchClass`/`ReconcileInstanceFields` entirely unchanged, genuinely no new nyx-proto work
  for this half (per `decision-log.md` §9.2's own note).
- Lambdas capturing `this` (§5.13) is what makes an event handler written inside a class's
  `Render` method (`onPress={() -> isVisible = true}`) resolve the implicit `this.isVisible`
  correctly — already built and already composes with hot reload (a captured `this` surviving
  a `PatchClass` reload sees the reconciled field state, not a stale snapshot, since fields are
  looked up by name at read/write time). Nothing further needed here; noted only as the
  prerequisite that makes Model 2 event handlers behave correctly once built.

Given Model 1 already works end to end for this repo and neither of the two directly-blocking
gaps above required Model 2, this section is a lower-priority "now unblocked, not yet
requested" opportunity — not folded into §1/§2's action items.

---

## 5. Checked, no action needed

- **`ValueKind::Callable` split into `ValueKind::Function`/`ValueKind::Lambda`**
  (`decision-log.md` §4.2, reversed) — grepped `src`/`include`/`tests`: zero references to
  `ValueKind::Callable` anywhere in this repo. `ValueToPropValue`/`StringifyValue`
  (`IrisNyxEvaluator.cpp`) both switch on `Value::Kind()` with a `default:` case, so neither
  needs updating regardless of how many `ValueKind` alternatives exist.
- **`Iris::NyxTokenizer`** (`src/Iris/NyxTokenizer.cpp`) wraps nyx-proto's real `nyx::Lexer`
  directly (`#include "lexer/lexer.hpp"`) rather than reimplementing tokenization — none of
  this batch's changes (default arguments, array spread literals, constructor syntax) added a
  new lexer `TokenKind`; default arguments reuse the pre-existing `Assign` token, array spread
  reuses the pre-existing `Spread` token (already lexed for `data`-construction spread), and
  constructor detection is parser-level disambiguation over existing `Identifier`/`LParen`
  tokens. `IsIdentifierLike`/`TranslateKind` need no new cases.
- **Default arguments (§5.14) and array spread literals (§5.15) as plain `.irisx`-authoring
  syntax** — both are accepted by nyx-proto's parser/interpreter with no iris-proto C++ change
  required beyond §1's parameter-name-extraction handling (a `.Map((item, index = 0) -> ...)`
  callback's extracted parameter names are unaffected by the default value attached to one of
  them). `.irisx` authors can write `[...list.Map(item -> <Card/>), <Footer/>]` today once §1's
  driver-side `Map`/`Reduce` support lands, with no separate spread-specific driver work.

---

## Cross-references

- `docs/archive/iris_nyx_slot_loop_and_reload_gap.md` — the two gaps this doc resolves against; read
  first for the full "why" behind each, this doc only adds "and here's what's now available,
  and what to build against it."
- `docs/next-steps.md`'s "Chaos runtime" entry — current overall status; its own "Still open"
  list already named the `<Slot>` loop and named hot-reload's blocker as "the Chaos runtime
  above (no real `NyxEvaluator` yet)" — that's since been built, so the hot-reload driver work
  in §2 above is now actually startable, not still blocked on something else in this repo.
- `docs/iris_hot_reload_reconciliation_decision.md` — the compiled-`.iris` (C++) hot-reload
  design (`BeginReloadReplay`/`EndReloadReplay`/`ReloadTarget`), independent of and not reused
  by §2's interpreted-path driver per this doc's own finding above.
- `docs/iris_hot_reload_alignment_decision.md` — the `key`/position-based identity this doc's
  §2 assumes a reload driver would key `ComponentInstance` lockstep-matching on.
- `nyx-scripting-language/decision-log.md` §7.4, §9.2 — the two resolved nyx-proto decisions
  this doc responds to. §5.5, §5.6, §5.13, §5.14, §5.15, §5.16, §4.2 — the rest of the same
  batch, covered in §3/§4/§5 above.
- `include/Iris/IrisNyxEvaluator.h`/`src/Iris/IrisNyxEvaluator.cpp` — `EvaluateSlot`
  (`:157-191`) and `ChaosSlotMarker` (`:120-128`), where §1's changes land.
- `include/Iris/IrisNyxDriver.h`/`src/Iris/IrisNyxDriver.cpp` — where §2's new reload entry
  point lands; `ComponentInstance::DriverState` (`include/Iris/ComponentInstance.h`) is how a
  reload driver recovers the prior `NyxScope` to pass as `ReInvokeComponent`'s `oldScope`.
- `host/nyx-runtime.hpp` (`ReInvokeComponent`, `SetErrorHandler`, `Instantiate`) and
  `runtime/environment.hpp` (`OwnBindingNames`/`FindOwn`/`OwnHasOnWrite`) — the nyx-proto
  surface this doc was checked against directly, not assumed from the decision log's prose
  alone.
