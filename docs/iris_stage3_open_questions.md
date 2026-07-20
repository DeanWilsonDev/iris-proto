# Stage 3 — Planning Notes and Open Questions

> **Status:** Fully closed. `docs/iris_stage3_decision_doc.md` closed all ten questions below
> plus the foundational one. One real, unmet Penumbra-side gap was found while verifying the
> decision against actual shipped code (`IWidgetLifecycle` didn't exist at the time) — flagged
> at the bottom; since closed by `penumbra-proto` commit `663fece`. Kept as a historical
> record — `docs/iris_core_spec.md` §10 is the current index into the Stage 3 architecture, and
> `docs/iris_stage3_decision_doc.md` is the full reference with exact interfaces, not this doc.
>
> The `vendor/penumbra` path referenced below no longer exists in this repo — Iris's
> repo/build integration was corrected after this was written (see
> `docs/iris_stage2_decision_doc.md`'s correction note); Penumbra now lives in its own
> submodule inside the separate `iris-penumbra-backend` repo. The verification facts below are
> still accurate as a record of what was checked and when.

---

## Closed: the foundational question

Resolved by `docs/iris_stage3_decision_slot.md`, reaffirmed by `docs/iris_stage3_decision_doc.md`
§0: component functions run once at mount; `<Slot>` is the sole re-invocation mechanism. See
`docs/iris_core_spec.md` §1.5, §2.2, §3.1, §9.5.

**Important correction this same decision doc introduced, affecting Stage 2 work already
documented:** the Iris runtime's `key → live widget` identity map (built in Stage 2) must be
typed `IWidget*` — a new backend-agnostic interface — never a concrete Penumbra type like
`WidgetBase*`. The Iris runtime must not reference Penumbra types anywhere, not just at the
`Component` IR level. Propagated to `docs/iris_core_spec.md` §2.3, §10 and
`docs/iris_handoff.md` §5.

## Resolution table

Per `docs/iris_stage3_decision_doc.md`:

| # | Question | Resolution |
| --- | --- | --- |
| 1 | Diff-based vs. fine-grained reconciliation | Slot-scoped diffing — diff only within what a re-invoked `<Slot>` produces, never the whole tree. |
| 2 | Same-position matching rule | Standard rule (same tag+key → update; different tag → remount), scoped to a `<Slot>`'s output. Remount discards signals tied to that position. |
| 3 | Keyed list diffing algorithm | Minimal-move, LIS-based — not naive remove-and-readd. |
| 4 | Prop-level update mechanism | Strongly-typed `IrisPropDiff` applied via a new `IWidget::ApplyPropDiff` interface — not direct backend-type field mutation from the reconciler itself. |
| 5 | `<Image>` update path | Two props: `src` (synchronous re-decode, accepted cost) and new `handle` (`iris::TextureHandle`, zero-cost swap, for animation). |
| 6 | Batching | Every event handler invocation auto-wrapped in begin/end batch; one reconciliation pass per handler, not per `.set()`. |
| 7 | Frame-loop integration | Explicit `iris::Tick()`, called once per frame by the host. Reconciliation only happens inside it. |
| 8 | Lifecycle hooks | `IWidgetLifecycle` (`OnMount`/`OnUnmount`/`OnTick`) — **verification gap below is now closed, buildable.** |
| 9 | Cross-component state sharing | No formal mechanism. Props drilling only, deliberately. |
| 10 | Penumbra API verification | Verification task — performed while incorporating this decision, see below. |

## Verification performed (per the decision doc's own §10 instruction not to trust prose alone)

Checked directly against `vendor/penumbra` (git submodule, pinned at commit `f008666`):

- ✅ **Structural mutation** — `RemoveChild`/`ReplaceChild`/`ClearChildren`/`MoveChild`/
  `InsertChildAt` all present on `Box`.
- ✅ **Tree walking** — `GetChildCount`/`GetChildAt` present on `WidgetBase`, correctly overridden
  on both `Box` and `SplitPanel`.
- ✅ **Prop-level mutable fields** — `ClassName`, `Text`, `Checked`, `OnPressed`/`OnReleased`/
  `OnHovered`/`OnFocused`/`OnChanged` all confirmed public and mutable (verified during Stage 2
  grounding, reconfirmed still valid at this commit). Not exhaustively checked for every
  widget/prop combination Stage 3 might eventually need — only what `IrisPropDiff` names today.
- ✅ **Lifecycle — resolved.** Was a genuine unmet Penumbra-side prerequisite (no
  `include/Penumbra/IWidgetLifecycle.h`, no matching file anywhere in the tree, at the
  `f008666` commit this was originally checked against) — the same category as Stage 2's
  `<Image>` gap was before it got fixed. `penumbra-proto` commit `663fece` landed
  `IWidgetLifecycle` (`OnMount`/`OnUnmount`/`OnTick(TickInfo)`) plus an `Application` host
  dispatching `OnTick`. See `docs/iris_handoff.md` §5 and `docs/iris_core_spec.md` §10.

---

## Deferred — unrelated to Stage 3

Lustre (Stage 4), the `umbra-engine` primitive set beyond `Model3d` (Stage 6), `<Grid>` real
layout (deferred by decision), event-prop vocabulary extensibility, implicit children-forwarding.
None of these block Stage 3 implementation.
