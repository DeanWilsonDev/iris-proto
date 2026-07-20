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

// The closed prop-name -> `IrisPropValue` variant-member lookup table from
// docs/iris_props_decision.md — the same table Codegen.cpp uses to type-check and emit
// primitive props, exposed here (rather than kept file-local to Codegen.cpp) so tooling
// (iris-lsp's completion) has one real source of truth instead of a second hand-maintained
// copy that could drift from it.
const std::unordered_map<std::string, std::string>& PrimitivePropTypeNames();

} // namespace Iris
