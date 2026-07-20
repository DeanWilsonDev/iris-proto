# Penumbra/Iris/Lustre — Componentization Gaps Found Extending the LensToggle Migration

> **Scope:** Investigated while attempting to extend
> `docs/pharos_iris_lustre_migration_feature_request.md`'s proof of concept
> (`LensTogglePanel`, `src/ui/iris/LensToggle.{iris,lustre}`) to
> `ExplorerPanel`, `InspectorPanel`, `ColorFilterDropdown`, and
> `GradientButton`, against `penumbra-ui-backend@5935a18` /
> `penumbra-proto@663fece` / this repo's current `iris`/`lustre` pins.
> **Status:** Not blocking — every panel below already works today via
> composition (subclassing `Box`, same pattern `LensToggle`'s own migration
> doc calls "the intended extension point"), and continues to. This doc
> records what stops a *full* declarative `.iris`/`.lustre` rewrite of each
> panel specifically, not a functional gap in Pharos itself.
> **Not in scope:** `GradientButton`'s gradient/shadow gap is already
> partially recorded in `pharos_iris_lustre_migration_feature_request.md`
> §3; this doc adds a second, independent consumer (`ExplorerPanel`'s
> selected-row fill) and a concrete proposed fix, rather than re-litigating
> whether `GradientButton` itself should migrate.

---

## 0. Context

Four panels were evaluated for the same kind of migration `LensTogglePanel`
went through (hand-rolled `Box::Builder` calls → a compiled `.iris` file +
a `.lustre` stylesheet, walked by `penumbra-ui-backend`'s
`BuildWidgetTree`/`WrapExistingTree`). Per this repo's own
`.claude/skills/feature-request` rule ("is this buildable via composition
before concluding it's a framework gap") each one was checked against
Pharos's actual source, not assumed. All four already *are* built via
composition — custom `Box` subclasses overriding `DrawContent`/
`UpdateInteractionState`, exactly `explorer_panel.cpp:39-43`'s own comment
("Composition, not a framework gap ... a tree row's ... hit-splitting is an
app-specific interaction Penumbra has no reason to know about"). That
composition keeps working with or without this doc. What it rules out is
re-expressing that same content as a real `.iris`/`.lustre` file pair, for
four independent reasons below, each traced to a specific missing piece of
`Iris`/`Lustre`/`penumbra-ui-backend`, not a Pharos-side limitation.

---

## 1. No icon-drawing primitive in Lustre or a `<Icon>` Iris tag

Three panels draw a vector glyph from Pharos's own icon catalog
(`icons::drawIcon`, `src/icons/icon_renderer.h`) inside a custom
`DrawContent` override:

- `TreeRow::DrawContent` draws an expand/collapse chevron via
  `drawChevron` (`src/ui/explorer_panel.cpp:116-118`).
- `InspectorRow::DrawContent` draws a per-row catalog icon via
  `icons::drawIcon` (`src/ui/inspector_panel.cpp:60-64`).
- `DropdownTrigger::DrawContent` draws the active overlay's icon
  (`src/ui/color_filter_dropdown.cpp:71-74`), and the open popover draws one
  icon per list row plus the selected row's own icon
  (`src/ui/color_filter_dropdown.cpp:179-180, 206`).

`Iris::IrisElementTag` (`penumbra-ui-backend`'s vendored `iris`,
`include/Iris/IrisElementTag.h:22-28`) only has `Frame`/`Inline`/`Grid`/
`Image`/`Text`/`Slot` — no icon/vector-glyph tag — and Lustre's real
property table (`lustre_core_spec.md` §2) has no `background-image`/
`content`/glyph-reference property either, only `background-color`. An
`<Image>` tag exists but maps to `ImageWidget`, which loads a raster/texture
`src` (`Walker.h`'s own comment on `ImageBackend`/`LoadFrom`) — Pharos's
icon catalog is drawn procedurally (stroke paths via `Renderer`, not a
texture asset), so `<Image>` isn't a fit even where a tag exists.

### What would unblock this

A `Renderer`-level vector-icon draw primitive that Lustre could reference
by name (e.g. `icon: "chevron-down"` resolving through the same catalog
`icons::IconKind` already enumerates) plus a corresponding `<Icon>` Iris
tag `penumbra-ui-backend`'s `Walker.cpp` could build. No proposed exact
signature here — Pharos's icon catalog is itself Pharos-side
(`src/icons/icon_renderer.h`), so the shape of a shared primitive is a
design question for whoever owns the catalog going forward, not something
this doc should guess at.

### What stays hand-rolled until this lands

`TreeRow`, `InspectorRow`, and `DropdownTrigger`/the color-filter popover
all stay native `Box` subclasses — this already works today and needs no
further action.

---

## 2. No gradient-fill primitive in Lustre (second consumer beyond `GradientButton`)

`pharos_iris_lustre_migration_feature_request.md` §3 already flagged
`GradientButton::Draw`'s two-stop gradient (`src/ui/gradient_button.cpp:32`,
`renderer.DrawGradientRect(...)`) as blocking its own migration. The same
gap independently blocks `ExplorerPanel`: a selected `TreeRow` paints a
top-to-bottom gradient fill plus a solid accent bar instead of a flat
background —

```cpp
// src/ui/explorer_panel.cpp:108-113
renderer.DrawGradientRect(ArrangedRect, theme->ColorRowSelectedGradientTop, theme->ColorRowSelected,
                          Style.BorderRadius);
renderer.DrawFilledRect({ArrangedRect.X, ArrangedRect.Y + 2.0f, theme->SelectionAccentBarWidth,
                         ArrangedRect.H - 4.0f},
                        theme->ColorAccent);
```

`lustre_core_spec.md` §2's real property table has `background-color` only
— no `background-image: linear-gradient(...)` or equivalent, confirmed
against the same table `penumbra_iris_lustre_migration...`'s §3 already
cited for `GradientButton`.

### What would unblock this

Same ask as the existing `GradientButton` gap — a `background-gradient`
(or CSS-shaped `linear-gradient(...)`) Lustre property resolving to
`Renderer::DrawGradientRect`, plus `BoxStyle` gaining the two-color field(s)
to carry it. Not re-specifying the exact grammar here; whoever picks up
`GradientButton`'s existing ask should size it to cover both consumers.

### What stays hand-rolled until this lands

`TreeRow`'s selection fill and `GradientButton` both stay native — already
working, no action needed.

---

## 3. No `<Scroll>` or `<Input>` Iris core tag

`ExplorerPanel`, `InspectorPanel`, and the toolbar's path field each root
themselves in a Penumbra widget type `Iris::IrisElementTag` has no
equivalent for:

- `ScrollablePanel` — `src/ui/explorer_panel.cpp:220`,
  `src/ui/inspector_panel.cpp:112`. Needed for `WheelStepLogical`-driven
  scroll offset, not reproducible by a plain `Frame`/`Box`.
- `TextInput` — `src/main.cpp:97` (the toolbar's JSON-path field). Needed
  for caret/selection/focus/clipboard handling, not reproducible by
  `Text`/`Label`.

`IrisElementTag`'s full enum (`IrisElementTag.h:22-28`) is `None`, `Frame`,
`Inline`, `Grid`, `Image`, `Text`, `Slot` — there is no tag `iris_cc` could
compile a `<Scroll>` or `<Input>` element down to, and no case in
`penumbra-ui-backend`'s `Walker.cpp` that could build one even if the
grammar accepted it. This is a harder ceiling than the two items above:
those block full-tree componentization, but *root* content can still nest
an `.iris`-built subtree inside a hand-rolled `ScrollablePanel`/`TextInput`
(see §5 below, and this session's own `InspectorPanel` chrome migration,
which does exactly that). A panel whose *only* content is scrollable or
editable (nothing static to hoist into a nested subtree) has no such
option.

### What would unblock this

Two new `IrisElementTag` values (`Scroll`, `Input`) plus matching
`Walker.cpp` build cases targeting `ScrollablePanel`/`TextInput`'s own
`Builder`s. Each is a separably-sized piece of work — `Scroll` only needs
to expose `WheelStepLogical` as a prop; `Input` needs to expose
`Text`/`PreferredWidthLogical`/focus and clipboard wiring, a larger prop
surface. Sizing not attempted here.

### What stays hand-rolled until this lands

`ScrollablePanel`/`TextInput` stay hand-built native widgets, with
`.iris`-built content nested inside them where there's static content
worth declaring (§5) — this already works for `InspectorPanel`'s chrome as
of this investigation.

---

## 4. No popup/overlay/z-order layer (pre-existing, cross-referenced here)

`ColorFilterDropdown`'s open popover is drawn and hit-tested entirely
outside the normal widget tree — `panel.drawOverlay`/`panel.updateOverlay`
(`src/ui/color_filter_dropdown.h:29-39`, implementations at
`src/ui/color_filter_dropdown.cpp:158-214` and `216-258`) call `Renderer`
directly after the tree's own `Draw()` pass, mirroring `main.cpp`'s
existing panel-drop-shadow "elevation pass." This is already documented as
a deliberate Penumbra limitation, not newly discovered here:
`color_filter_dropdown.h:20` ("Penumbra has no popup/z-order layer"),
`main.cpp:82` (the toolbar's own File-menu-replacement comment, same
wording), and `docs/penumbra_requirements.md` item 5's own framing.
Recorded here only because it's the reason `ColorFilterDropdown` has no
representable *tree* for `penumbra-ui-backend` to walk in the first place —
§§1-3 above at least leave a tree, just with gaps in what can style/tag it;
this one has no tree to target at all for the popover itself (the closed
trigger square is a separate, small `Box` and inherits §1's icon gap only).

### What would unblock this

Already asked for in `docs/penumbra_requirements.md` — not re-requesting
here, just cross-referencing so a future Iris/Lustre migration pass knows
this is the blocking dependency for `ColorFilterDropdown` specifically.

### What stays hand-rolled until this lands

The entire `ColorFilterDropdown` popover — already working, no action
needed.

---

## 5. Not a gap — confirmed working: nesting a componentized subtree inside a hand-rolled root

While investigating §3, this session confirmed (and used, in
`InspectorPanel`) that an `.iris`-built subtree can be mounted as an
ordinary child of a hand-rolled `ScrollablePanel` via the same
`WrapExistingTree`/`DetachOwnership` pair `lens_toggle.cpp:100-125` already
uses, then have further native children (`InspectorRow`s) `AddChild`'d onto
the built subtree's own root after the fact — no `penumbra-ui-backend`
change needed. This is why §§1-4 are framed as "full-tree" gaps rather than
"Pharos is blocked": any panel with *some* static, non-data-driven chrome
can still declare that part in `.iris`/`.lustre` today, nesting it inside
whatever hand-rolled root or hand-rolled rows the panel still needs.
`ExplorerPanel` was evaluated and found to have no such static chrome to
hoist (every element under its `ScrollablePanel` is a data-driven `TreeRow`
recursion — see `src/ui/explorer_panel.cpp:217-256`), so it was left
entirely hand-rolled rather than introducing a componentized wrapper with
no declarative content in it.

---

## 6. What unblocks when this lands

Landing §1 (icon primitive) would let `InspectorRow`'s icon+label content
move into `.iris`/`.lustre` (its two-band text layout would still need a
plain nested `Frame`/`Text` pair, which is already expressible). Landing §2
(gradient) would let `GradientButton` and `TreeRow`'s selection fill do the
same. Landing §3 (`<Scroll>`/`<Input>`) would remove the last reason
`ExplorerPanel`/`InspectorPanel`/the toolbar need a hand-rolled root at
all. None of these are required for Pharos to keep working — every panel
above renders correctly today, this doc only scopes what a *further* Iris/
Lustre migration pass would need.
