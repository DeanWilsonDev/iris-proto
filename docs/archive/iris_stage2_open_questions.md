# Stage 2 — Planning Notes and Open Questions

> **Status:** Fully closed. `docs/iris_stage2_decision_doc.md` closed all ten questions below.
> Two of them (`<Image>`'s decode pipeline and content-prop name) were checked directly against
> the real `penumbra-proto` source and initially found *not* to match what the decision doc
> claimed — a follow-up Penumbra change (`IImageBackend`/`SdlImageBackend`/`ImageWidget`,
> documented in `penumbra-proto`'s own `docs/penumbra_image_widget_requirements.md`) has since
> closed both for real, verified against the landed code. Kept as a historical record —
> `docs/iris_core_spec.md` §2.5, §2.6, §3, §4, §8, §9.3–§9.4 are the current language/primitive
> reference, not this doc.
>
> Item 10 (repo/build integration) has since been corrected: Penumbra is not a submodule of
> Iris. The Penumbra backend lives in a separate `iris-penumbra-backend` repo that vendors both
> `iris` and `penumbra-proto` — see `docs/iris_stage2_decision_doc.md`'s correction note.

---

## Resolution table

Per `docs/iris_stage2_decision_doc.md`:

| # | Question | Resolution | Verified against real code? |
| --- | --- | --- | --- |
| 1 | Generic interactive-element mechanism | `WidgetBase` gains five null-by-default `std::function<void()>` callbacks (`OnPressed`/`OnReleased`/`OnHovered`/`OnFocused`/`OnChanged`) | **Yes** — landed exactly as decided |
| 2 | `<Image>` backend | Decided: build a minimal `IImageBackend`/`Image` widget with PNG/JPG-from-path loading | **No** — see "New gaps" below |
| 3 | `<Grid>` layout | Deferred; stubbed as plain `Box` with `LayoutMode::HorizontalStack` | Yes — nothing to build, decision itself is the resolution |
| 4 | What is `Component` concretely | Lightweight backend-agnostic IR node (tag/props/children) | N/A — Iris-side design decision, not Penumbra code |
| 5 | Component invocation codegen convention | `<Name>Props` required naming rule | N/A — Iris-side convention |
| 6 | `<Inline>` vs `<Text>` mapping | Distinct — `<Inline>` maps to a new `InlineContainer` widget | **Yes** — `InlineContainer` landed as a real wrapping inline-flow layout, not a stub |
| 7 | Styling stub strategy | Both: Cimmerian tests + visible demo with hardcoded placeholder styles | Not re-verified — implementation-phase concern, not blocking further doc updates |
| 8 | `key` bookkeeping | Build the `key → WidgetBase*` identity map now in Stage 2 | **Superseded** — Stage 3 scoping corrected the value type to `IWidget*` (backend-agnostic), never a concrete Penumbra type. See `docs/iris_core_spec.md` §2.3, §10. |
| 9 | What does Stage 2 done look like | Defined concretely (see decision doc §9) | N/A |
| 10 | Repo and build integration | Standalone repo, git submodule for Penumbra, CMake | **Corrected** — Penumbra is not a submodule of Iris; the backend-mapping code lives in `iris-penumbra-backend`, which depends on both. |

---

## `<Image>` gaps — found, then closed

### `<Image>`'s decode-from-path pipeline was decided but initially did not ship — now fixed

`docs/iris_stage2_decision_doc.md` §2 said: *"Build a minimal `IImageBackend`/`Image` widget in
Penumbra now... Minimal asset pipeline only — load PNG/JPG from a file path to an SDL texture."*
The first pass only shipped the widget half — `Image` drew a pre-existing `SDL_Texture*`, no
decode pipeline anywhere. A follow-up Penumbra commit (`f008666`, "Replace Image widget with
spec-conformant ImageWidget + IImageBackend") fixed this for real: `Backends::IImageBackend`/
`SdlImageBackend` now decode PNG/JPG via `SDL_image` (`IMG_Load` + `SDL_CreateTextureFromSurface`,
`SDL3_image` now linked in `CMakeLists.txt`), documented in
`penumbra-proto/docs/penumbra_image_widget_requirements.md`. Verified against the actual header/
source, not just that requirements doc's prose.

### `<Image>`'s content prop name — now confirmed `src`

The widget was renamed `Image` → `ImageWidget` and its `Builder` now has a `.src(std::string)`
method, explicitly mapped to Iris's `src` prop in the requirements doc's own table — closing the
naming question every existing Iris example already assumed but had never formally settled.

**Two implementation-detail divergences from every other primitive**, worth knowing before
Stage 2's tree-builder targets `ImageWidget` (documented in `docs/iris_core_spec.md` §3.1, §9.4):
`ImageWidget::Builder` has no `child()`/`children()` or event-prop methods (a leaf, deliberately
narrower than `Box`/`Label`/`InlineContainer`'s shared builder shape — though the underlying
`WidgetBase::OnPressed`/etc. fields still exist and are dispatched the same way, so `<Image
onPress={...}>` would need to set that field directly rather than go through the builder chain);
and loading is a separate explicit `.LoadFrom(backend, renderer)` call, not part of `build()`.

---

## Original ten questions — full detail

Kept for reference; see `docs/iris_stage2_decision_doc.md` for complete reasoning on each.

1. Generic interactive-element mechanism → `docs/iris_core_spec.md` §4.
2. `<Image>` backend → §3.1 (partially — see "New gaps" above).
3. `<Grid>` layout → §3.1.
4. `Component` representation → §2.5.
5. Component invocation codegen convention → §2.6.
6. `<Inline>` vs `<Text>` mapping → §3.1.
7. Styling stub strategy → decision doc §7 (not yet reflected in the language spec — an
   implementation-phase detail, not a language-surface concern).
8. `key` bookkeeping → decision doc §8 (same — implementation detail).
9. Stage 2 done definition → decision doc §9.
10. Repo/build integration → decision doc §10.

---

## Deferred — unrelated to Stage 2

Unchanged: Lustre cascade rules, the `umbra-engine` primitive set beyond `Model3d`,
event-prop vocabulary extensibility, implicit children-forwarding. See
`docs/iris_core_spec.md` §8 for the live list.
