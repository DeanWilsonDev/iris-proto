# Iris — aligning with nyx-proto's tiered `.chaos` hot-reload decision

> **Status:** Open — an alignment/scoping pass, like
> `docs/iris_interpreted_host_hot_reload_gap.md` before it, not a proposed API or an
> implementation. Triggered by `fearless-hq/projects/nyx-scripting-language/decision-log.md`
> §9.1 (nyx-proto side, 2026-08-06): `.chaos` hot reload was redesigned from "always full
> remount" to a three-tier model — method/render-body patch, `@signal` field-layout
> reconciliation, full remount only as the fallback for a genuinely irreconcilable change —
> matching what `.nyx` gameplay scripts already do one subsection earlier in the same doc.
> This file works out what that decision implies for this repo specifically, using the gap
> doc above as the starting inventory of what's actually missing.

---

## 0. Why this repo's own gap doc gets revisited

`docs/iris_interpreted_host_hot_reload_gap.md` already named and sized three unsolved
problems, checked directly against this repo's runtime code. Nothing about the code has
changed since that doc was written — it's still all correct. What's changed is the
premise it was written against: its own §4 concluded Nyx "isn't the interpreted host this
conversation meant," because at the time `docs/iris_core_spec.md` described `.irisx` as
just a second `IHostLanguageTokenizer` over the same compile-ahead-of-time pipeline
`CppTokenizer` already drives.

That's no longer an accurate description of nyx-proto. It's a genuine tree-walking
interpreter, and per its own decision-log §6.7, `nyx::runtime::Environment` is
`shared_ptr`-managed independent of whatever AST produced it — a captured environment
already outlives its declaring call frame today (a `NyxCallable`'s closure keeps it
alive), the same "re-executed live, in-process, without a rebuild" property §4 said
didn't exist. `nyx::interpreter::Interpreter::EvalExpr`/`MakeGlobalContext` are already
linked into this repo via the `nyx-runtime` CMake target (`docs/next-steps.md`'s "Chaos
runtime" entry). §4's conclusion — that plugging a real interpreted host into Iris needs
Iris's own execution model reconsidered, not just a new tokenizer — still holds; it's
just that the host in question is nyx-proto as it exists today, not a hypothetical one.

Problems §1–§3 of the gap doc are unaffected by any of this — they're about this repo's
own `ComponentInstance`/`Reconciler` code, not about what drives it. What nyx-proto's
§9.1 decision adds is a concrete target shape for what "solved" needs to look like on
this side, instead of an open-ended "some day, someone reconciles a reload."

---

## 1. What nyx-proto's decision requires from `ComponentInstance` (gap doc §1)

`.chaos` tier 1 (render-body/method-only change) requires the *same* component instance
to survive a reload — its `@signal` state carried forward, only its behaviour replaced.
Today `MountComponentInstance` (`include/Iris/ComponentInstance.h`) allocates a fresh
`ComponentInstance` on every invocation with no identity-based lookup against a prior
run, which is exactly gap-doc §1's finding.

**What this means concretely:** a hot-reload-driven re-walk of a `.iris.ir` document (or,
downstream, a re-run of a live `.irisx` component via the still-unbuilt `NyxEvaluator`)
needs a way to say "this component invocation is logically the same one as last time —
reuse its `ComponentInstance` instead of calling `MountComponentInstance` fresh." That
requires some identity a reload driver can key on across two separate walks of the IR
document — the existing `key`/position-based identity the reconciler already uses for
ordinary list diffing (§3 below) is the natural candidate, but wiring it into
`MountComponentInstance`'s call path for the *reload* case specifically is new work, not
already covered by anything Stage 3 built for ordinary re-renders (those never re-mount
an existing instance — only `<Slot>` re-invocation touches an existing one, and it always
reuses the instance a `SlotState` already holds, never re-derives identity from scratch).

Whether this identity match happens on the Nyx side (nyx-proto reports "same
`NyxObject`, patched method table" per its own §9.1 registry-patch primitive, not yet
designed) or the Iris side (this repo diffs `.iris.ir` documents and infers "same
component" from position/`key`) is exactly the open question nyx-proto's entry left
unresolved ("which side decides which tier applies"). Not settled here either.

## 2. What nyx-proto's decision requires from the reconciler (gap doc §2)

Unaffected in substance — gap-doc §2's finding stands exactly as written:
`ReconcileWidget` (`include/Iris/Reconciler.h`) needs an owning `unique_ptr<IWidget>&`
and the exact prior `Component` tree, and neither exists anywhere at whole-application
scope today — only `SlotState::PreviousSingle_`/`PreviousList_`, scoped to one `<Slot>`.

What nyx-proto's decision *does* add here is a concrete reason this can no longer stay
indefinitely deferred behind "revisit once the Chaos runtime is scoped"
(`docs/next-steps.md`'s current framing): tiers 1 and 2 both end with "re-run `render{}`,
get a new element tree, reconcile it against what's live" — the exact operation this gap
blocks. A whole-app "current tree + the IR it came from" registry (`docs/
iris_interpreted_host_hot_reload_gap.md` §5's own naming) is now a concrete prerequisite
for nyx-proto's tier 1/2, not just a nice-to-have for a hypothetical future reload
feature. Still not designed here — who owns it, its lifetime relative to
`IrisRuntime`'s other ambient state, and whether it subsumes or sits alongside
`RegisterRoot`/`GetRoot` (`SlotRuntime.h`) are the same open questions the gap doc already
listed as deliberately unsolved.

## 3. What nyx-proto's decision requires from list/widget identity (gap doc §3)

Unaffected — already the closest of the three to solved. The existing `key`-based LIS
list diffing (`docs/iris_lis_list_diff_decision.md`) should extend to a reload-triggered
reconcile once §1 and §2 above exist; this remains an exercise-it-and-verify item, not
new design, same as the gap doc already concluded.

## 4. Tier classification isn't designed here

Nyx-proto's tiers assume something can tell "render-body-only" apart from
"`@signal` field layout changed" apart from "structurally irreconcilable." Two ways this
could work, both viable, neither chosen:

- **nyx-proto reports it** — its own not-yet-designed registry-patch primitive
  (decision-log §9.1) could return which tier applied when patching a class.
- **This repo infers it** — a hot-reload driver diffs successive `.iris.ir` documents
  (`Iris::ParseIrisIrDocument`, `include/Iris/IrisIrDocument.h`) itself: a `nyx_source`
  region's field declarations changing is answerable directly from that node's own
  `source` text, without nyx-proto needing to report anything.

Left to whoever actually implements this, matching nyx-proto's own §9.1 entry leaving
the same question open on its side.

---

## 5. What's not requested here

- No API changes to `ComponentInstance.h`, `Reconciler.h`, `SlotRuntime.h`, or
  `IrisIrRuntime.h` — this is a scoping pass, not a design, same posture as
  `docs/iris_interpreted_host_hot_reload_gap.md` itself.
- No change to the Stage 2/3 static-mount or ordinary-re-render paths — everything above
  is additive, specific to the reload case.
- Implementing the still-missing real `NyxEvaluator` (`docs/next-steps.md`'s "Chaos
  runtime" entry) is a separate, larger, already-tracked prerequisite this doesn't
  restate.

## 6. Cross-references

- `fearless-hq/projects/nyx-scripting-language/decision-log.md` §9.1 — the nyx-proto
  decision this doc aligns to.
- `fearless-hq/projects/nyx-scripting-language/execution-model.md` §21.4 — the
  language-level spec this implements against.
- `docs/iris_interpreted_host_hot_reload_gap.md` — the original three-problem sizing
  pass; still the authoritative code-level detail for §1–§3 above.
- `docs/next-steps.md`'s "Chaos runtime" entry — tracks the still-missing real
  `NyxEvaluator`, the actual prerequisite before any of this is reachable end to end.
