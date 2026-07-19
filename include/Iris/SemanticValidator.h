#pragma once

#include "Iris/ElementNode.h"
#include "Iris/IrisConfig.h"
#include "Iris/SourceLocation.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace Iris {

struct SemanticError {
    std::string    Message;
    SourceLocation Location;
};

// Validates a parsed `render { }` tree (`RenderBlockParser`'s output) against the
// preprocessor-level checks from docs/iris_core_spec.md §6's error catalogue that
// `Codegen.h` doesn't already cover on its own:
//
//   - Backend-gated primitive used against the wrong `.iris.json` target (`<Model3d>`
//     without `"target": "umbra-engine"`).
//   - An inline `style` prop on any element — Core primitive or component invocation
//     alike; Iris has no inline styling at all (§4).
//   - `<Text font=...>` — font is a Lustre concern, never an Iris prop.
//   - An element tag that's neither a Core primitive nor a name resolved by `import`
//     in scope.
//
// `ImportedNames` is the set of names `import`ed by the file the tree came from
// (`ScanImports`'s output, by name — resolution success/failure is `ImportResolver`'s
// own separate error, not re-checked here). Recurses into every child position,
// including nested elements found inside a `!{ }` JSX-transform escape hatch
// (docs/iris_escape_hatch_decision.md) — those are real parsed elements too, and get
// the same validation as anything written at the top level.
std::vector<SemanticError> ValidateElementTree(const ElementNode& Root, IrisBuildTarget Target,
                                                const std::unordered_set<std::string>& ImportedNames);

} // namespace Iris
