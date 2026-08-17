#pragma once

namespace Iris {

// The Core primitive tag set an `Component` (`Component.h`) node can carry
// (docs/iris_core_spec.md §3.1). An element tag that isn't one of these is a
// component invocation (§2.6), not an `IrisElementTag` value at all — see
// `Codegen.h`.
//
// `<Model3d>` is deliberately not included yet: §3.1 calls it "illustrative
// forward-reference only; full prop set is Stage 6 work" — it has never actually
// been designed, so there is nothing correct to emit for it
// (docs/iris_stage1_codegen_decision.md).
enum class IrisElementTag {
    // Sentinel: "no component was produced here" — what a `<Slot>` callable's
    // `return nullptr;` (docs/iris_core_spec.md §1.5, §9) becomes via
    // `Component`'s `nullptr_t` converting constructor
    // (docs/iris_escape_hatch_decision.md's Verification section; the gap is
    // tracked and closed in docs/iris_core_spec.md §8). Not a real Core
    // primitive — a walker/reconciler must treat it as "unmount whatever was
    // here, mount nothing", never pass it to a backend `Builder`.
    None,
    Frame,
    Inline,
    Grid,
    Image,
    // A single vector glyph resolved by name from a backend/app-supplied icon catalog
    // via its `icon` prop (e.g. `<Icon icon="chevron-down" />`) — a leaf, same as
    // `<Image>`, but never a texture asset: no `src`/`handle` (docs/iris_core_spec.md
    // §3.1). Iris carries no icon catalog of its own — resolving the name to a real
    // drawn glyph is entirely backend-side.
    Icon,
    Text,
    // A scrolling clip container (docs/
    // penumbra_iris_lustre_componentization_gaps_requirements.md §3) — like `<Frame>`,
    // takes ordinary element children, but clips them to its own bounds and offsets
    // them by a wheel-driven scroll position instead of just stacking them. `wheelStep`
    // (float) is its one dedicated prop (logical pixels scrolled per wheel notch);
    // everything else (class, event props) is the shared set every primitive gets.
    Scroll,
    // Single-line text entry (docs/
    // penumbra_iris_lustre_componentization_gaps_requirements.md §3) — a leaf, same as
    // `<Icon>`/`<Image>`: no children. `text` (its initial value, reusing `<Text>`'s own
    // prop name) and `preferredWidth` (float, a field-width hint) are its dedicated
    // props; focus/clipboard/caret are entirely backend-side state this tag carries no
    // opinion about.
    Input,
    Slot,
    // An opaque escape-hatch node (docs/archive/iris_next_steps_resolved.md, "No way to declare a custom
    // widget/imperative-draw node as an Iris element") — its single `build` prop is a
    // `{ }` escape hatch evaluating to an already-built widget handle
    // (`Component::NativeBuilder`, not an ordinary `IrisProps` entry — see Component.h),
    // spliced directly into the built tree at this position by a backend-mapping pass.
    // Deliberately mount-once and outside the reconciler's *content*-based diffing (its
    // `NativeBuilder` is never compared, and `ReconcileMatchedInPlace`'s `ApplyPropDiff`/
    // child-recursion have nothing to act on for it either way) — the sanctioned escape
    // valve for the same hand-rolled-`Box`-subclass composition pattern every backend
    // already has, not a general imperative-draw sublanguage.
    //
    // That does not mean a `<Native>` node inside a re-rendering `<Slot>` can never
    // rebuild (docs/next-steps.md's "`<Native>` doesn't participate in `<Slot>`
    // reconciliation" entry, investigated 2026-08-17): `<Native>` gets no special-case
    // treatment anywhere in `Reconciler.cpp` — it's walked and identity-checked
    // (`Tag` + `Key`) exactly like every other Core primitive. A same-identity
    // re-render is genuinely a no-op (confirmed by `tests/SlotRuntimeTests.cpp`'s "an
    // unattached `<Native>` node's builder is NOT re-invoked..." case), but giving the
    // node a `key` that changes whenever the data driving its rebuild changes already
    // forces a fresh `NativeBuilder::Build()` call, through the exact same "different
    // key → mount fresh" path any other primitive or component invocation already
    // takes (see `tests/SlotRuntimeTests.cpp`'s two adjacent "...DOES get freshly
    // re-invoked..."/"...also rebuilds..." cases, the latter proving the freshly-built
    // widget genuinely lands in a real attached parent's child list, the shape a
    // backend's own reconciler needs) — the same `key={id}`-forces-a-remount idiom this
    // stack's own "Iris is this stack's JSX" framing (`CLAUDE.md`) already implies from
    // React. No reconciler change was needed for this; it was already load-bearing,
    // just unexercised and undocumented until now.
    Native,
    // A draggable-handle resizable split (docs/archive/iris_next_steps_resolved.md, "No layout-container
    // primitive beyond Frame's three stack modes") — exactly two element children
    // (leading/trailing panes, matching `Penumbra::Widgets::SplitPanel::SetFirst`/
    // `SetSecond`'s own two-slot shape, not a generic `Children` vector). `axis`
    // ("horizontal"|"vertical"), `ratio`, `minPaneSize`, `handleThickness` are its
    // dedicated props; everything else (class, event props) is the shared set every
    // primitive gets.
    Split,
};

} // namespace Iris
