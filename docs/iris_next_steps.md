# Iris — Next Steps

> Snapshot as of 2026-07-14. Reflects the corrected three-repo architecture
> (`docs/iris_stage2_decision_doc.md`'s correction note): `iris` (this repo, standalone,
> backend-agnostic), `penumbra-proto` (standalone, no Iris knowledge), and
> `iris-penumbra-backend` (vendors both, owns the Stage 2/3 backend-mapping code).

## Where things stand

- **Stage 0 (spec)** — done. `docs/iris_core_spec.md` v2 is authoritative.
- **Stage 1 (preprocessor front end)** — partially done:
  - `CppTokenizer` (`IHostLanguageTokenizer` for C++23) — done.
  - `RenderBlockParser` (`render{ }` → `ElementNode` AST: tags, props, `key`/`class`, nested
    elements, `{ }` escape hatches, literal text, comment stripping, single-root enforcement) —
    done, tested against the spec §9 worked example end-to-end.
  - Codegen (`ElementNode` AST → compilable `.cpp`) — **not started**, blocked (see below).
  - `import` / `.iris.json` resolution — **not started**, blocked (see below).
  - Semantic validation (Core-primitive vs. imported-component resolution, backend-gated
    primitive checks, the `<Text font=...>` and inline-style errors) — **not started**, depends
    on import resolution to know what's in scope.
- **Stage 2 (Penumbra backend)** — repo/build wiring only. `iris-penumbra-backend` vendors both
  `iris` and `penumbra-proto` and links an `iris_penumbra_backend` interface target against
  both; the actual `IrisComponent`-IR-to-widget-tree walker has no sources yet.
- **Stage 3 (reactive runtime)** — fully spec'd (`docs/iris_stage3_decision_doc.md`), not
  implemented. Its last known real blocker just closed — see below.
- **Stage 4 (Lustre-lite styling)** — not scoped yet.
- **Stage 5 (first real consumer)** — not started. You mentioned real consuming projects already
  exist, which is why the repo-dependency direction got fixed now rather than later.
- **Stage 6 (Umbra Engine/Nyx backend)** — deferred by design.

## Done: IWidgetLifecycle docs synced

`penumbra-proto` commit `663fece` ("Add IWidgetLifecycle interface and Application lifecycle
host") landed `include/Penumbra/IWidgetLifecycle.h` — `OnMount`/`OnUnmount`/`OnTick(TickInfo)` —
plus an `Application` host that dispatches `OnTick`, exactly matching what
`docs/iris_stage3_decision_doc.md` §8 specified. This closed the one real, verified
Penumbra-side gap that was blocking Stage 3 lifecycle hooks.

`docs/iris_core_spec.md` §10, `docs/iris_handoff.md` §5, `docs/iris_stage3_open_questions.md`,
`docs/iris_stage3_decision_doc.md` §10's checklist, and `CLAUDE.md` have all been updated to
mark this resolved (mirroring how the `<Image>` gap's resolution was documented). Stage 3 now
has every known Penumbra-side prerequisite it needs.

## The one decision blocking the most downstream work: `IrisProps`'s runtime representation

Two separate pieces of real work are both stalled on the same open question:
`docs/iris_core_spec.md` §2.5 gives `IrisComponent`'s struct shape —

```cpp
struct IrisComponent {
    IrisElementTag Tag;
    IrisProps      Props;
    std::vector<IrisComponent> Children;
};
```

— but never pins down what `IrisProps` concretely *is*. Prop values are heterogeneous by nature
(a string for `class`, an int for `<HealthBar current={...}>`, a `std::function<void()>` for
`onPress`), so it can't be a simple `map<string, string>`. This blocks:

1. **Stage 1 codegen** in `iris` — turning a parsed `ElementNode` into `.cpp` that constructs
   real `IrisComponent` values needs a concrete target type to emit against.
2. **Stage 2's walker** in `iris-penumbra-backend` — reading prop values back out to call
   `Box::Builder().className(...).onPress(...)` needs to know how they're actually stored.

Recommend treating this the way every other Stage decision in this project has been made: a
short decision doc (candidates worth weighing — a type-erased map like
`unordered_map<string, any>`, a closed variant type covering the known prop value kinds, or
something narrower scoped to just what Core primitives currently need) before either downstream
piece gets implemented, rather than deciding it as a side effect of writing codegen. Now
formally tracked in `docs/iris_core_spec.md` §8's open-questions list, alongside the
`.iris.json` JSON-parsing question below, so neither gets lost.

## Suggested order

Starting from what's actually left (docs sync is done — see above):

1. `import` / `.iris.json` resolution in `iris` — independent of the `IrisProps` decision, can
   happen in parallel with it. Needs its own small call: hand-roll a minimal parser scoped to
   `.iris.json`'s fixed three-field schema, or vendor a real JSON library (e.g. nlohmann/json).
2. Decide `IrisProps`'s runtime representation (decision doc).
3. Stage 1 codegen in `iris`, targeting the type decided in (2).
4. Semantic validation pass in `iris` (needs (1) done to know what's in scope).
5. Stage 2 walker in `iris-penumbra-backend`, targeting the type decided in (2) and consuming
   codegen'd output from (3).
6. Stage 3 reactive runtime — already fully spec'd, largest remaining implementation chunk.
   Unblocked on the Penumbra side now that `IWidgetLifecycle` has landed.
7. Stage 4 (Lustre) — needs its own design pass first, nothing to implement yet.
8. Stage 5 — validate against one of the real consuming projects once (2)–(6) produce something
   an actual `.iris` file can round-trip through.
