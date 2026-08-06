# Iris — real hot-reload of component logic for a future interpreted host

> **Status:** Open gap, not designed. This doc only names and sizes the problem, with
> concrete code-level citations for why each part is genuinely unsolved — no proposed
> API, no implementation. Per this ecosystem's own convention ("ask the dependency, then
> stop" / "log the gap, don't guess a fix" for something this size), a real design is
> deliberately deferred to whoever scopes an actual interpreted host.
> **Trigger:** a conversation about eventually plugging Lustre and Iris into an
> interpreted (not compiled) host language, with Penumbra updating the on-screen drawing
> live as that language's source changes — surfaced that this repo's own docs had already
> flagged the underlying need, twice, without ever sizing it against the real code:
> `docs/archive/iris_next_steps_resolved.md`'s "Live-widget root registry, for Lustre's
> hot-reload" entry (its "Explicitly not requested" section) and
> `../../lustre/docs/lustre_handoff.md` (§3,
> "Runtime-loaded, not compiled ahead of time"). This doc is that sizing pass.

---

## 0. What's already true, and what isn't

`iris::RegisterRoot(Umbra::IWidget*)`/`iris::GetRoot()` (`include/Iris/SlotRuntime.h`,
`src/Iris/SlotRuntime.cpp`) were built specifically to unblock Lustre's own, much narrower
hot-reload need: when a `.lustre` file changes, walk the live mounted tree and re-apply
resolved styling. That's a **read-only, structure-preserving** walk — it never rebuilds or
replaces any widget, only mutates style fields on ones that already exist. Real
component-logic hot-reload — re-executing a component's actual `render{}` body (because its
source changed) and reconciling the result into the running app — is a different, harder
problem this registry does nothing to solve. Checked directly against the current runtime
code (not assumed from the docs), it breaks into three separate, currently-unsolved pieces.

## 1. State has no identity across two separate runs of the same component

`ComponentInstance` (`include/Iris/ComponentInstance.h`) is allocated fresh —
`std::make_shared<ComponentInstance>()` inside `MountComponentInstance` — on **every**
invocation of a component function, with no key/position-based lookup against a prior
instance anywhere in that path. `IRIS_SIGNAL(Type, Name, InitExpr)` (`ComponentInstance.h`,
expanding through `Detail::DeclareSignal<Type>`) allocates its storage against whichever
`ComponentInstance` is currently active on `IrisRuntime`'s ambient stack — there is no
mechanism that says "this is logically the same component instance as before this reload,
reuse its existing signal values instead of re-running `InitExpr`."

This works today only because a component's `render{}` body runs exactly once, at mount
(`Signal.h`'s own doc comment) — every subsequent update goes through `<Slot>`
re-invocation, which reuses the *existing* `ComponentInstance` a `SlotState` already holds,
never re-mounts one. Re-executing a component's top-level render function from scratch (the
premise of "the interpreted source changed, re-run it") has no path to inherit prior state:
every `Signal` would silently reset to its `InitExpr`, discarding anything a user did at
runtime (a dragged slider, a typed value, scroll position) the instant a hot-reload fires.

## 2. The reconciler has no entry point that accepts "diff against whatever's live right now"

`ReconcileWidget(std::unique_ptr<Umbra::IWidget>& Widget, const Component& Old, const
Component& New, const MountFn& Mount)` (`include/Iris/Reconciler.h`,
`src/Iris/Reconciler.cpp`) is the only real diff entry point, and it needs two things
nothing outside a `SlotState` currently has:

- An **owning** `unique_ptr<IWidget>&` — `iris::GetRoot()` deliberately hands back a raw,
  non-owning `Umbra::IWidget*` (right call for Lustre's read-only restyle walk; wrong shape
  for anything that needs to replace nodes).
- The **exact prior `Component` IR tree** the live widget was built from — this only exists
  as private state inside a `SlotState` (`SlotState::PreviousSingle_`/`PreviousList_`,
  `include/Iris/SlotRuntime.h`), scoped to one specific `<Slot>`, never stored anywhere at
  the whole-application level.

So the mechanically obvious approach — re-run the component's `render{}`, get a fresh
`Component` tree, hand it to `Reconcile()` alongside `iris::GetRoot()` — doesn't work as
written today. There is no whole-app "the tree currently mounted, and the IR it came from"
record for a hot-reload driver to hand to the reconciler; `docs/archive/iris_next_steps_resolved.md`'s own framing of
the original gap ("no whole-application live-widget registry or tree-walk entry point... 
`SlotState` tracks only its own slot's live widget(s)") undersold how much of this piece
specifically blocks logic hot-reload, since it was written to justify the read-only
restyle case, not this one.

## 3. Widget identity across a reload is *closer* to solved, but never exercised for this

The reconciler's `key`-based, LIS-optimal list diffing (`docs/iris_lis_list_diff_decision.md`)
already preserves widget identity correctly across an ordinary re-render — the same
machinery a hot-reload's resulting diff would presumably want to lean on, once problems #1
and #2 have an answer. This piece is not itself unsolved in the way the other two are; it's
simply never been exercised against "the entire render function's source changed," only
against normal prop/state-driven re-renders within a stable component identity. Flagging it
as the one part of this gap that a future design likely gets close to for free, rather than
needing new machinery.

## 4. Nyx, as currently specced, isn't the interpreted host this conversation meant

`docs/iris_core_spec.md` describes Nyx (`.irisx`), Iris's own planned future host language,
purely as a second `IHostLanguageTokenizer` implementation selected by file extension — "the
`render{}` block itself is copied verbatim... only the surrounding host code differs." That
is still a **preprocessed-then-compiled** path, structurally identical to the current C++23
one, just with different host syntax. It carries none of the "re-executed live, in-process,
without a rebuild" semantics a genuinely interpreted host implies. Plugging Iris/Lustre into
an actually-interpreted language (embedding a scripting VM whose `render{}`-equivalent gets
re-run live) is a different, bigger idea than what Nyx's own spec currently covers — it
would need Iris's core execution model reconsidered (how/when a component "mounts," whether
mounting is even the right mental model for something re-run on every edit), not just a new
tokenizer alongside `CppTokenizer`.

## 5. Explicitly not designed here

- **A concrete state-migration scheme** (§1) — e.g. matching `ComponentInstance`s by some
  stable identity across a reload, or a snapshot/restore convention for `Signal` values.
  Real design work, not sketched.
- **A whole-app "current tree + the IR it came from" registry** (§2) to give a hot-reload
  driver something to call `Reconcile()` against — noted as the concrete missing piece, not
  designed. Would need to decide who owns it, its lifetime relative to `IrisRuntime`'s
  other ambient state, and whether it subsumes or sits alongside `RegisterRoot`/`GetRoot`.
- **What execution model a genuinely interpreted host needs** (§4) — the biggest open
  question of the four, and the one most likely to reshape the other three once answered,
  since "mount once, then only `<Slot>`-driven updates" may not even be the right frame for
  something re-executed on every source edit.
- **Any implementation** — this is a sizing pass, triggered by a conversation about the
  idea, not a commitment to build it. Revisit once an actual interpreted host is scoped,
  per `lustre_handoff.md`'s own existing "revisit when that host is scoped" note.
