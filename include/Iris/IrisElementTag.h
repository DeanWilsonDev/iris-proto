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
    Text,
    Slot,
};

} // namespace Iris
