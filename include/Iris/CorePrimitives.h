#pragma once

#include "Iris/IrisConfig.h"

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Iris {

// The Core primitive tag set (docs/iris_core_spec.md §3.1) — available on every backend,
// no `.iris.json` `target` gating. `<Model3d>` is deliberately not included here: it's a
// backend-gated primitive (§3.2, `BackendGatedPrimitiveTagNames()` below), not a Core one.
const std::unordered_set<std::string>& CorePrimitiveTagNames();

// Backend-gated primitives (docs/iris_core_spec.md §3.2): a tag name valid only when
// `.iris.json`'s `target` matches the required backend. Using one against the wrong
// target is a preprocessor-level compile error (§6's error catalogue), not a runtime
// no-op.
const std::unordered_map<std::string, IrisBuildTarget>& BackendGatedPrimitiveTagNames();

} // namespace Iris
