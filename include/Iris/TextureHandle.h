#pragma once

#include "Umbra/TextureHandle.h"

namespace iris {

// Opaque, runtime-owned reference to a decoded backend texture
// (docs/iris_core_spec.md's `<Image>` `handle` prop, `docs/iris_stage3_decision_doc.md`
// §5's `<Image>` update path). Swapping handles during reconciliation is a pointer
// assignment — zero disk I/O, unlike `src`'s synchronous re-decode path.
//
// An alias, not a distinct type: a texture handle is a rendering-backend concept, not
// specific to Iris, so the real definition lives in `umbra-interfaces`
// (`docs/iris_stage3_implementation_decision.md`) — `Umbra::IrisPropDiff::Handle`
// names the same type. Kept as `iris::TextureHandle` here so `Iris::IrisPropValue`
// (docs/iris_props_decision.md) and existing code naming it don't need to change.
using TextureHandle = Umbra::TextureHandle;

} // namespace iris
