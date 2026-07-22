#pragma once

#include "Iris/TextureHandle.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <variant>

namespace Iris {

// Decided in docs/iris_props_decision.md: a closed, strongly-typed variant rather than
// a type-erased `unordered_map<string, any>` — every prop value a Core primitive or
// component currently needs, and nothing else. Adding a new prop *kind* means adding a
// variant member here deliberately, plus a matching `IrisPropDiff` field once Stage 3
// exists (docs/iris_props_decision.md's `IrisPropDiff` alignment rule).
using IrisPropValue = std::variant<
    std::string,
    int,
    float,
    bool,
    std::function<void()>,
    std::function<void(std::string)>,
    iris::TextureHandle
>;

// key-value prop map for one `Component` node — `key` is never an entry here, it's
// stripped by the preprocessor before codegen (docs/iris_core_spec.md §2.3,
// docs/iris_stage1_codegen_decision.md).
using IrisProps = std::unordered_map<std::string, IrisPropValue>;

} // namespace Iris
