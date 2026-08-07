# Iris → Nyx: two open primitives needed for real `.irisx` apps

> **Status:** Open, not designed. Per this ecosystem's own working convention
> (nyx-proto's `CLAUDE.md`, "stop on undocumented design decisions — don't guess and move
> on"), this doc only names and sizes two remaining nyx-proto-side gaps, with concrete
> evidence from the real, already-implemented iris-proto code that hits them. No proposed
> API for either — that's a nyx-proto design decision, same posture
> `docs/iris_nyx_evaluator_scope_gap.md` took for the two problems that preceded these
> (both since resolved — see `nyx-scripting-language/decision-log.md` §7.3).
>
> **Context these build on:** `Iris::MakeNyxEvaluator`/`Iris::IrisNyxDriver`
> (`include/Iris/IrisNyxEvaluator.h`, `include/Iris/IrisNyxDriver.h`) are real, non-mock,
> and tested end to end against real nyx-proto execution (not a mock) — a `.irisx` file on
> disk can be loaded, compiled, mounted into a live `Iris::Component` tree, and cross-file
> component invocations resolve correctly, all working today. Both gaps below are the
> narrower, self-contained remainder — see `docs/next-steps.md`'s "Chaos runtime" entry for
> the full picture of what's done vs. open.

---

## 1. No way to bind per-iteration data for a runtime `<Slot>` loop

**The gap, concretely:** `for item in list { <Card name={item.name} /> }` — probably the
single most common dynamic-UI pattern — cannot be evaluated for real today. Only a
statically-bounded conditional (a fixed, finite set of embedded elements selected by a
boolean/index expression, e.g. `isHovered ? <A/> : <B/>`) works.

### Why: how the existing conditional case works, and where it stops

`MakeNyxEvaluator::EvaluateSlot` (`src/Iris/IrisNyxEvaluator.cpp:157-191`) resolves a JSX
escape hatch (`chaos-ir-spec.md`'s `NyxExpressionNode`) by reconstructing its ordered
`Segments` into one expression, substituting a marker call,
`__chaos_slot_pick(N)`, for each embedded element position:

```cpp
for (const IrNyxExpressionSegment& Seg : Node.Segments) {
    if (Seg.Kind == IrNyxExpressionSegmentKind::Text) {
        Reconstructed += Seg.Text;
    } else {
        Reconstructed += "__chaos_slot_pick(" + std::to_string(ElementIndex) + ")";
        ++ElementIndex;
    }
}
```

`__chaos_slot_pick` itself is a plain host function registered via
`NyxRuntime::RegisterFunction` (`ChaosSlotMarker::RegisterOn`,
`src/Iris/IrisNyxEvaluator.cpp:120-128`) that just records which index(es) got called:

```cpp
Runtime.RegisterFunction("__chaos_slot_pick", [Selected](std::vector<Value> Args) -> Value {
    if (!Args.empty() && Args[0].Kind() == ValueKind::Int) {
        Selected->push_back(static_cast<std::size_t>(std::get<int32_t>(Args[0].data)));
    }
    return Value();
});
```

The whole reconstructed expression is evaluated once
(`InvokeAsLambda` → `Runtime.EvaluateInScope`), and afterward `Convert(Elements[Index])` is
called for every index the marker recorded, in the order recorded. This already tolerates
the *same* index being recorded more than once — e.g. a `for` loop calling the marker once
per iteration produces `[0, 0, 0]` for a 3-item list evaluating a single embedded
`<Card/>` — so the marker-call mechanism itself is not the blocker.

**What's actually missing is per-iteration *data*.** `Convert(Elements[Index])`
(`IrElementConverter`, `include/Iris/IrisIrRuntime.h`) evaluates that element's own prop
expressions (`item.name`) by calling back into the *same* `EvaluateProp`/`EvaluateText`
closures `MakeNyxEvaluator` built for the single outer invocation — which evaluate against
one fixed `NyxRuntime::NyxScope` (`src/Iris/IrisNyxEvaluator.cpp:135,150`:
`Runtime.EvaluateInScope(Scope, Node.Source())`). There is no way for a loop variable
(`item`) bound only inside the Nyx-side `for` loop's own environment — alive only during
that one native-function call, one loop iteration at a time — to reach that later, separate
`Convert` call, which runs after the whole reconstructed expression has already returned.

### Checked directly against nyx-proto's current public API, not assumed

- `NyxRuntime::RegisterFunction` (`host/nyx-runtime.hpp:96`) binds
  `std::function<runtime::Value(std::vector<runtime::Value>)>` — the native callback
  receives only its call arguments, nothing about the interpreter's live environment at
  the call site (no access to `item`, or to any other local the surrounding `for` loop
  bound).
- `NyxRuntime::InvokeComponent`/`Interpreter::CallFunctionCapturingEnvironment`
  (`host/nyx-runtime.hpp:281-284`, `interpreter/interpreter.hpp:53-69` — the primitive
  built for `nyx-scripting-language/decision-log.md` §7.3 Problem 2) captures the
  environment live *when a whole function call returns* — exactly the shape that unblocked
  per-invocation `@signal`/prop scope, but it has no mid-call, mid-loop analog. It captures
  once, at return; a `for` loop runs many iterations inside a single call.
- `EvaluateInScope` (`host/nyx-runtime.hpp:246-251`) evaluates a bare expression against an
  already-existing, already-fixed `EvalContext` — it has no notion of "the specific
  environment that happened to be live for iteration 3 of a loop that already finished
  running."

None of nyx-proto's public surface today exposes "the environment active right now, inside
this native function call" — the thing needed to read back `item`'s value for a specific
marker-call site, or to carry it back out to `Convert`.

### What this means concretely — sized, not designed

Closing this needs a nyx-proto-side decision, likely one of:

- A native-function-callback variant that receives the calling `Environment` (or every
  named local visible at the call site) alongside its `Value` args, so
  `__chaos_slot_pick`'s replacement could read `item` directly at each call and stash it
  next to the recorded index.
- A way for a marker call to carry an arbitrary `Value` (the loop item) back out through its
  return value or a side channel, which `IrisNyxDriver`/`MakeNyxEvaluator` could then bind
  into a fresh per-pick `NyxScope` (the same `EvalContext`-substitution shape
  `InvokeComponent` already uses) before calling `Convert`.

Either shape is a real design decision for nyx-proto to make (interacts with `Environment`
ownership/lifetime, and whatever `for`-loop desugaring the interpreter already does) — not
invented here.

---

## 2. Hot-reloading a `.chaos`/`.irisx` component's *captured-environment* state has no tiered model

**The gap, concretely:** `nyx-scripting-language/decision-log.md` §9.1 designed a tiered
hot-reload model for Nyx generally — (1) render-body/method-only change: patch the live
instance's method table, state untouched; (2) field-layout change: best-effort respawn
preserving compatible fields; (3) irreconcilable change: full restart. But that design (and
the `Interpreter::PatchClass`/`ReconcileInstanceFields`/`Instantiate` primitives built for
it) is built entirely around Nyx **classes** — patching a class's registry entry,
reconciling fields on a live class **instance**.

`chaos-ui-authoring.md` line 51 states directly: *"A Chaos component is a free function
whose body contains a `render { }` block"* — not a class, no `NyxObject` instance for
`ReconcileInstanceFields` to operate on. Confirmed concretely once
`NyxRuntime::InvokeComponent` was actually built (§7.3): its captured state is a plain
`std::shared_ptr<runtime::Environment>` (`host/nyx-runtime.hpp:281-284`), never a
`NyxObject`. There is nothing for `PatchClass`/`ReconcileInstanceFields` to operate on for
a `.chaos`/`.irisx` component, full stop —
`nyx-scripting-language/decision-log.md` §9.3 (recorded while designing `InvokeComponent`)
already reaches and records this same conclusion.

### What this means concretely — sized, not designed

`docs/iris_hot_reload_reconciliation_decision.md` already solves this on the iris-proto
side for the **compiled `.iris`/C++ path** — `iris::ReloadComponentInstance`/
`ComponentInstance::BeginReloadReplay`/`EndReloadReplay` replay a render body against an
already-mounted instance, reusing `IRIS_SIGNAL` storage by declaration order — and needed
no nyx-proto involvement, because that path's state lives in a C++
`iris::ComponentInstance`, not a Nyx `Environment`.

For a `.irisx` component mounted via `IrisNyxDriver`/`InvokeComponent`, the equivalent state
lives in a captured `Environment` instead, and reloading it needs its own analog of §9.1's
tiers — something like "re-invoke the patched `FunctionDecl` fresh, then reconcile the new
call's `env` bindings against the old one's" — a genuinely different shape from field
reconciliation onto a persistent object identity, since two independent `Environment`s (old
call, new call) have no shared identity to reconcile *onto*, only a diff to compute between
them. `nyx-scripting-language/decision-log.md` §9.3 already flags this as open and
deliberately undesigned; this doc's contribution is confirming it's still the live blocker
now that the rest of the driver (`IrisNyxDriver`, cross-file invocation) is built and this is
one of the only two remaining gaps standing between today's code and a real, reloadable
multi-component `.irisx` application.

Not sized further here — real design work belongs in nyx-proto, matching how §9.1's
original class-based tiers were designed there rather than in iris-proto.

---

## 3. Cross-references

- `docs/next-steps.md`'s "Chaos runtime" entry — the full status of what's done (real
  `NyxEvaluator`, mount driver, cross-file invocation) vs. open (both gaps above, plus two
  smaller iris-proto-only items: `<Native>` unsupported for `.irisx`, and a `<Slot>`
  re-invocation's conversion errors having no durable sink).
- `docs/iris_nyx_evaluator_scope_gap.md` — the two prior nyx-proto gaps this doc's problems
  build on top of (`nyx_source` reconstruction, per-invocation live scope), both resolved;
  same doc also recorded the `PatchClass`/`ReconcileInstanceFields` class-vs-free-function
  mismatch this doc's §2 confirms is still unaddressed.
- `docs/iris_hot_reload_reconciliation_decision.md` — the compiled-`.iris` (C++) side of
  hot-reload state preservation, already implemented, independent of this doc's §2.
- `include/Iris/IrisNyxEvaluator.h`/`src/Iris/IrisNyxEvaluator.cpp` — `EvaluateSlot`
  (`:157-191`) and `ChaosSlotMarker` (`:120-128`), where §1's gap is hit today.
- `include/Iris/IrisNyxDriver.h`/`src/Iris/IrisNyxDriver.cpp` — the mount/reload driver both
  gaps ultimately block from reaching feature-complete.
- `nyx-scripting-language/decision-log.md` §7.3 — `CallFunctionCapturingEnvironment`/
  `InvokeComponent`'s design and the precedent for how a prior nyx-proto primitive request
  from iris-proto was scoped and resolved.
- `nyx-scripting-language/decision-log.md` §9.1, §9.3 — the class-based tiered hot-reload
  design, and §9.3's own note (independent of this doc) already flagging the
  free-function/`Environment` case as open.
- `host/nyx-runtime.hpp` (`RegisterFunction:96`, `NyxScope:187-190`,
  `InvokeComponent:281-284`) and `interpreter/interpreter.hpp` (`CallResult:53-56`,
  `CallFunctionCapturingEnvironment:69`) — the current public API both gaps were checked
  against directly, not assumed from memory.
