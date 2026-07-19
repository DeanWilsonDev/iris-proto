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
  - Codegen (`ElementNode` AST → compilable `.cpp`) — **done, including nested JSX inside
    `<Slot>`/escape hatches**. `docs/iris_props_decision.md` (the `IrisProps`/`IrisPropValue`
    runtime shape), `docs/iris_stage1_codegen_decision.md` (two follow-on gaps that decision
    left open), and `docs/iris_escape_hatch_decision.md` (the `!{ }` JSX-transform escape
    hatch — see below) are all closed and implemented. `Codegen.h`/`GenerateComponentExpression()`
    walks an `ElementNode` and emits a C++23 expression constructing `Iris::IrisComponent` —
    Core primitives (including `<Slot>` via `Iris::MakeSlotCallable`), the `<Name>Props`
    component-invocation convention, text/interpolation-child concatenation, the closed
    prop-name lookup table, and `!{ }`-transformed nested JSX are all tested
    (`tests/CodegenTests.cpp`). The full spec §9 `PartyScreen` example, written with `!{ }` on
    both `<Slot>`s, was manually verified to generate output that actually host-compiles as
    real C++23 — the one thing it surfaced that's still open is unrelated to escape hatches:
    `IrisComponent` has no `nullptr_t` constructor, so the spec's own `return nullptr;` inside
    an `-> IrisComponent` lambda doesn't compile as written (now tracked in
    `docs/iris_core_spec.md` §8).
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

## Resolved and implemented: JSX inside escape hatches (`!{ }` transform escape hatch)

The gap surfaced during codegen testing: `RenderBlockParser` treats `{ }` escape hatch contents
as fully opaque verbatim text (§1.4), but `<Slot>` is used throughout the spec with JSX inside
its escape hatch body — conditional and list rendering both rely on this pattern. That JSX was
never being transformed, producing uncompilable output.

Full decision, implementation notes, and verification writeup are in
`docs/iris_escape_hatch_decision.md`. Summary: a second escape-hatch sigil, `!{ }`, means
"host-language code that may contain JSX — recursively transform it"; the existing `{ }` form is
unchanged and stays fully opaque; nesting (`!{ }` inside `!{ }`, `{ }` inside `!{ }`) composes
normally. Implemented in `RenderBlockParser::ParseJsxEscapeHatch`
(`src/Iris/RenderBlockParser.cpp`) and `Codegen.cpp`'s `EmitEscapeHatchExpression`, tested in
both `tests/RenderBlockParserTests.cpp` and `tests/CodegenTests.cpp` — including an end-to-end
test against the full spec §9 `PartyScreen` example (both `<Slot>`s, two levels of nesting,
`std::vector<IrisComponent>` correctly *not* misread as a JSX element) whose generated output
was manually confirmed to host-compile as real C++23.

One implementation wrinkle worth knowing: `std::vector<IrisComponent>` (a real return type used
in the spec's own list-rendering `<Slot>`) has the exact same `< Identifier >` shape as an
attribute-less JSX opening tag. Disambiguated by requiring whitespace immediately before the
`<` for it to count as a JSX start — true of every JSX use in the spec, never true of a template
argument list. See the decision doc for the one known edge case this doesn't cover
(whitespace-free JSX like `push_back(<Frame/>)`), deliberately deferred since nothing in the
spec needs it.

## Suggested order

Starting from what's actually left:

1. **Semantic validation pass in `iris`** — element-tag resolution against Core primitives and
   the now-implemented import resolution. Backend-gated primitive checks and prop-level
   validation (`<Text font=...>`, inline-style errors) per the spec.
2. **Stage 2 walker in `iris-penumbra-backend`** — consuming codegen's `Iris::IrisComponent`-
   constructing output, now confirmed to include correctly-transformed `<Slot>` bodies.
3. **Stage 3 reactive runtime** — already fully spec'd, largest remaining implementation chunk.
   Unblocked on the Penumbra side now that `IWidgetLifecycle` has landed.
4. **Stage 4 (Lustre)** — needs its own design pass first, nothing to implement yet.
5. **Stage 5** — validate against one of the real consuming projects once (1)–(4) produce
   something an actual `.iris` file can round-trip through.

Separately, not blocking the order above: `IrisComponent` needs a `nullptr_t` constructor (or
the spec's "render nothing" convention needs to change) — see `docs/iris_core_spec.md` §8 and
`docs/iris_escape_hatch_decision.md`'s Verification section for how this surfaced.
