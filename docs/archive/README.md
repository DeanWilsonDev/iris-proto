# Archive

Docs moved here are self-superseded: each one's own status header says it's closed, resolved,
or no longer updated, and nothing outside `docs/` cites it directly (only other docs do, by
path — those references were updated to point here). They're kept for the historical trail,
not as current truth.

This does **not** include the `iris_*_decision*.md` / `iris_stage*_decision_doc.md` files —
those remain in `docs/` because `docs/iris_core_spec.md`'s own status header lists them, in
precedence order, as its sources, and because they're cited throughout `include/`, `src/`, and
`tests/` as permanent inline citations (per `CLAUDE.md`: "chronological decision records/
handoffs, kept for the reasoning trail").

| File | Superseded by |
| --- | --- |
| `iris_next_steps.md` | `docs/next-steps.md` (the active tracker) — Stage 0–3 snapshot, predates the entries in `iris_next_steps_resolved.md` below |
| `iris_next_steps_resolved.md` | `docs/next-steps.md` (the active tracker) — resolved post-Stage-3 feature-request entries, moved out wholesale as each one closed |
| `iris_stage1_open_questions.md` | Closed by `docs/iris_stage1_decision_doc.md` / `_pt2.md`; current reference is `docs/iris_core_spec.md` |
| `iris_stage2_open_questions.md` | Closed by `docs/iris_stage2_decision_doc.md`; current reference is `docs/iris_core_spec.md` |
| `iris_stage3_open_questions.md` | Closed by `docs/iris_stage3_decision_doc.md`; current reference is `docs/iris_core_spec.md` |
| `iris_nyx_slot_loop_and_reload_gap.md` | Both sized gaps resolved on nyx-proto's side (`decision-log.md` §7.4, §9.2) and consumed here — see `docs/next-steps.md`'s "Chaos runtime" entry |
| `iris_nyx_slot_loop_and_reload_gap_resolved.md` | Same as above — the iris-proto-side implementation this doc sized (Map/Reduce slot loops, free-function hot reload) is done, current reference is `docs/next-steps.md` |
