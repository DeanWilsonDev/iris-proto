# Iris — Next Steps

> Living backlog, not a session-changelog: add an entry for anything new
> and open; remove or mark an entry done the moment it's actually
> implemented, rather than letting finished work linger. First file of
> this kind in this repo (previously tracked via the individual
> `iris_*_decision.md`/`iris_*_gap.md` files under `docs/` — those stay
> as-is, not migrated into here retroactively).
> Last updated: 2026-08-17.

## Open items

_None open right now._

## Status note — 2026-08-17 (fleet retirement)

This repo's agent (iris) was briefed and on standby for the multi-repo
coordination experiment (pharos-proto / penumbra-proto / lustre /
penumbra-ui-backend / nyx-proto / iris-proto). No ask was ever queued for
this repo during the run, so **no implementation work was started or is
in-flight** — nothing to hand off, nothing half-finished. The peer roster
received (penumbra, lustre, backend, nyx agent addresses) is not recorded
here since it's session-specific coordination state, not durable repo
backlog.

The coordinator ("main") sent a stop-all instruction because this test
fleet is being retired in favor of a fresh one. This note exists per this
repo's own stop protocol, so a fresh session picking this repo back up
knows the true state: clean, idle, nothing pending.
