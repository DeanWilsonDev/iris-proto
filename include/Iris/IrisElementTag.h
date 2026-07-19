#pragma once

namespace Iris {

// The Core primitive tag set an `IrisComponent` (`IrisComponent.h`) node can carry
// (docs/iris_core_spec.md §3.1). An element tag that isn't one of these is a
// component invocation (§2.6), not an `IrisElementTag` value at all — see
// `Codegen.h`.
//
// `<Model3d>` is deliberately not included yet: §3.1 calls it "illustrative
// forward-reference only; full prop set is Stage 6 work" — it has never actually
// been designed, so there is nothing correct to emit for it
// (docs/iris_stage1_codegen_decision.md).
enum class IrisElementTag {
    Frame,
    Inline,
    Grid,
    Image,
    Text,
    Slot,
};

} // namespace Iris
