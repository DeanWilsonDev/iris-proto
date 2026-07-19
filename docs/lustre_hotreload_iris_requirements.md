# Feature request: hot-reload support, surfaced by Lustre's design

> **Status:** Request, not yet scoped or implemented. Filed during Lustre's design handoff
> (`../../lustre/docs/lustre_handoff.md` §3, "Runtime-loaded, not compiled ahead of time") —
> not blocking that design, but recorded here since the underlying need is Iris's, not just
> Lustre's.

## Why this exists

Lustre (the styling layer for Iris — see `../../lustre` repo) is being designed to load and
parse `.lustre` files at application runtime rather than compile them ahead of time like
`iris_cc` does, specifically so styles can be hot-reloaded without a rebuild. Lustre's own
hot-reload need is narrow: re-resolve and re-apply styling data to already-mounted widgets
when a `.lustre` file changes. It does not, by itself, require Iris to change anything —
`class` already reaches a live widget as a plain runtime string (`WidgetBase::ClassName`,
threaded through `IrisPropDiff::ClassName`), which is enough for a Penumbra-backend-side
restyle pass to walk the real widget tree directly using types it already knows about.

The broader reason to flag this in `iris` itself rather than only in `lustre`: Iris is
designed to support more than one host language over time — C++23 (`.iris`) now, a future
first-party scripting language (`.irisx`'s eventual real successor, or whatever that language
turns out to be) later. That future host language will need real hot-reload of *component
code itself* (structure, state, event handlers — not just styling), which is a much larger
question than Lustre's. This doc exists so that need is on record now, before it's needed,
rather than rediscovered later the way several Penumbra-side gaps were during Stage 2/3
(see `docs/iris_handoff.md` §5's history of the `<Image>` and `IWidgetLifecycle` gaps).

## What Lustre's own hot-reload does *not* need from Iris

- No change to `IrisComponent`, `Codegen`, or generated header shape.
- No change to `key`/`class` prop handling — both already reach the runtime as plain values.
- No change to the reconciler — Lustre's restyle pass is not a reconcile, it doesn't diff or
  mutate the component tree, only re-applies style structs to widgets that already exist.

## What's actually missing, narrowly, for Lustre's case

Iris's runtime currently has **no whole-application live-widget registry or tree-walk entry
point** that anything external could use to find "every widget currently mounted, anywhere in
the app" without going through a full reconcile. `SlotState` tracks only its own slot's live
widget(s); `ResolveSlots` discovers slots but doesn't expose a global list. A Penumbra-side
Lustre restyle pass triggered by a changed `.lustre` file needs to reach every live widget to
re-resolve its class's style — today that would require the *application* to already hold a
reference to its own root widget and walk it manually (possible, since `Umbra::IWidget`
already has `GetChildCount`/`GetChildAt` for exactly this kind of traversal), so this may not
need a new Iris-side API at all, just a documented pattern for how a consuming app wires a
restyle pass up to its own root. Recorded here as a "check this holds" item, not a confirmed
gap, since it hasn't been tried against a real app yet.

## The larger, deliberately out-of-scope question

Real hot-reload of *component logic* for a future scripting-language host — re-running a
component's `state`/`render` body live, preserving `Signal` state across the reload, without
restarting the process — is a substantially bigger design question (state migration across a
reload, what happens to already-mounted widget identity, whether the reconciler can treat a
reload as "just another re-render") that this doc deliberately does not attempt to answer.
Recorded here only so it isn't forgotten: **when the scripting-language host is scoped, revisit
this doc and give hot-reload its own design pass**, informed by whatever Lustre's narrower
styling-only hot-reload turns out to need in practice.
