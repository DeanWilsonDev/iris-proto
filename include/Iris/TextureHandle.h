#pragma once

namespace iris {

// Opaque, runtime-owned reference to a decoded backend texture
// (docs/iris_core_spec.md's `<Image>` `handle` prop, `docs/iris_stage3_decision_doc.md`
// §5's `<Image>` update path). Swapping handles during reconciliation is a pointer
// assignment — zero disk I/O, unlike `src`'s synchronous re-decode path.
//
// Deliberately in the lowercase `iris::` namespace, not `Iris::` — this is Stage 3
// reactive-runtime state (populated by a future `iris::LoadTextures()`), not a plain
// IR data shape like `Iris::IrisComponent` (docs/iris_stage1_codegen_decision.md).
// Not implemented yet: Stage 3's job, per docs/iris_next_steps.md. This stub exists only
// so `Iris::IrisPropValue` (docs/iris_props_decision.md) has a concrete type to name.
class TextureHandle {
public:
    TextureHandle() = default;
};

} // namespace iris
