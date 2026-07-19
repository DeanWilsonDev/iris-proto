# Iris — Next Steps

> Snapshot as of 2026-07-19. Reflects the corrected three-repo architecture
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
  - Codegen (`ElementNode` AST → compilable `.cpp`) — **done, for a single (non-nested) `render`
    block**. `docs/iris_props_decision.md` (the `IrisProps`/`IrisPropValue` runtime shape) and
    `docs/iris_stage1_codegen_decision.md` (two follow-on gaps that decision left open — see
    below) both closed. `Codegen.h`/`GenerateComponentExpression()` walks an `ElementNode` and
    emits a C++23 expression constructing `Iris::IrisComponent` — Core primitives (including
    `<Slot>` via `Iris::MakeSlotCallable`), the `<Name>Props` component-invocation convention,
    text/interpolation-child concatenation, and the closed prop-name lookup table are all
    tested (`tests/CodegenTests.cpp`). The JSX-inside-escape-hatch gap is now resolved by
    decision — see below.
  - `import` / `.iris.json` resolution — **done**. `IrisConfig` (parses `target`/`version`/
    `searchPaths` via the newly-vendored `libs/amanuensis` — a zero-dependency first-party JSON
    library, git-submoduled rather than hand-rolled, see below) and `ImportResolver`
    (`ScanImports` + `ResolveImports`, `.iris`/`.irisx` extension chosen by `target`) both landed
    with tests (`tests/IrisConfigTests.cpp`, `tests/ImportResolverTests.cpp`). Not yet wired into
    an actual preprocessor driver/CLI — there isn't one yet — and the semantic pass that uses
    resolved imports to validate element tags is still separate, blocked (see below).
  - Semantic validation (Core-primitive vs. imported-component resolution, backend-gated
    primitive checks, the `<Text font=...>` and inline-style errors) — **not started**, depends
    on import resolution to know what's in scope. Now unblocked.
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

## Done: IrisProps runtime representation and Stage 1 codegen's two follow-on gaps

`docs/iris_props_decision.md` closed `IrisProps`/`IrisPropValue`'s shape (a closed, strongly-typed
`std::variant`, not a type-erased `unordered_map<string, any>`). Writing codegen against it then
surfaced two more gaps neither that document nor `docs/iris_core_spec.md` §2.5 actually covered —
both closed in `docs/iris_stage1_codegen_decision.md`:

1. `<Slot>`'s callable child doesn't fit in `Props` (whose one callable variant member is
   zero-argument, shaped for event handlers) or in `Children` (which holds already-constructed
   `IrisComponent` values, not an unevaluated callable) — resolved by adding a `SlotCallable`
   field to `IrisComponent`, populated via a `Iris::MakeSlotCallable()` helper that defers the
   `IrisComponent` vs. `vector<IrisComponent>` return-type choice to the host compiler.
2. Literal text and `{ }` interpolation as element children have nowhere to go in a shape where
   `Children` only holds `IrisComponent` values — resolved per-primitive: `<Text>` concatenates
   its children into its own `"text"` prop; every other children-accepting primitive (chiefly
   `<Inline>`) wraps a text/escape-hatch child as a synthetic `<Text>` `IrisComponent` node
   appended to `Children` instead.

Both `IrisComponent`'s revised shape (`include/Iris/IrisComponent.h`) and `Codegen.h` are
implemented and tested.

## Resolved: JSX inside escape hatches (`!{}` transform escape hatch)

The gap surfaced during codegen testing: `RenderBlockParser` treats `{ }` escape hatch contents
as fully opaque verbatim text (§1.4), but `<Slot>` is used throughout the spec with JSX inside
its escape hatch body — conditional and list rendering both rely on this pattern. That JSX was
never being transformed, producing uncompilable output.

**Decision:** introduce a second escape hatch sigil, `!{}`, meaning "C++ that may contain JSX —
recursively transform it." The existing `{ }` form is unchanged and stays fully opaque.

Rules:
- `{ }` — regular escape hatch. Opaque. Contents pass through verbatim. No change to existing
  behaviour.
- `!{ }` — JSX-transform escape hatch. Parser enters recursive transform mode. Any `<Tag>`
  expressions inside are transformed to `Iris::IrisComponent`-constructing expressions. Closes
  on the matching `}`. Valid anywhere a regular `{ }` is valid — prop values, child positions,
  lambda bodies.
- Once inside `!{}`, nested `{ }` props and children follow normal rules (opaque unless also
  marked `!{}`). A nested `!{}` is valid if a second level of JSX-transform is genuinely needed,
  but the spec has no examples requiring this.
- `!{` is not valid C++ in a child or prop-value position, so the token is unambiguous at the
  lexer level. No heuristic detection required.

The `<Slot>` pattern from the spec becomes:

```cpp
<Slot>
    !{[&]() -> IrisComponent {
        if (settingsOpen.get()) {
            return <SettingsPage onClose={[&]() { settingsOpen.set(false); }} />;
        }
        return nullptr;
    }}
</Slot>
```

`onClose` uses a regular `{ }` — its lambda body has no JSX — and stays opaque. Only the outer
block needs `!{}`. A formal decision doc (`docs/iris_escape_hatch_decision.md`) should be written
before implementation, following the same format as `iris_props_decision.md`.

## Suggested order

Starting from what's actually left:

1. **Write `docs/iris_escape_hatch_decision.md`** — capture the `!{}` decision formally before
   implementing. Same format as `iris_props_decision.md`.
2. **Implement `!{}` in `RenderBlockParser`** — the one gap that stops Stage 1 codegen's output
   from compiling for any component that uses `<Slot>`.
3. **Semantic validation pass in `iris`** — element-tag resolution against Core primitives and
   the now-implemented import resolution. Backend-gated primitive checks and prop-level
   validation (`<Text font=...>`, inline-style errors) per the spec.
4. **Stage 2 walker in `iris-penumbra-backend`** — consuming codegen's `Iris::IrisComponent`-
   constructing output.
5. **Stage 3 reactive runtime** — already fully spec'd, largest remaining implementation chunk.
   Unblocked on the Penumbra side now that `IWidgetLifecycle` has landed.
6. **Stage 4 (Lustre)** — needs its own design pass first, nothing to implement yet.
7. **Stage 5** — validate against one of the real consuming projects once (1)–(5) produce
   something an actual `.iris` file can round-trip through.
