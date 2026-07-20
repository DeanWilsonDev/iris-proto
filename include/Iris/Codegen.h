#pragma once

#include "Iris/ElementNode.h"
#include "Iris/SourceLocation.h"

#include <string>
#include <vector>

namespace Iris {

struct CodegenError {
    std::string    Message;
    SourceLocation Location;
};

struct CodegenResult {
    // A single C++23 expression constructing an `Iris::Component` value — empty
    // whenever Errors is non-empty. Callers wrap it themselves (e.g. `return <Source>;`
    // to replace a whole `render { }` block) — this only emits the expression.
    std::string              Source;
    std::vector<CodegenError> Errors;
};

// Turns a parsed `ElementNode` (`RenderBlockParser`'s output) into the C++23 expression
// that constructs it as an `Iris::Component` value, per
// docs/iris_props_decision.md and docs/iris_stage1_codegen_decision.md. Escape hatch
// contents are never parsed or validated here — copied verbatim into the generated
// expression, same as everywhere else in the preprocessor (docs/iris_core_spec.md §1.4);
// the host compiler validates them once the generated file reaches it.
CodegenResult GenerateComponentExpression(const ElementNode& Root);

} // namespace Iris
