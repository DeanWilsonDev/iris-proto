#pragma once

#include "Iris/ElementNode.h"
#include "Iris/ImportResolver.h"
#include "Iris/RenderBlockParser.h"

#include <amanuensis/value.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace Iris {

// Builds the Chaos IR (docs/iris_nyx_emission_decision.md; chaos-ir-spec.md §3) JSON
// document for one already-parsed `.irisx` file, as an Amanuensis::Value ready for
// Amanuensis::Writer::WriteToString -- the Nyx-target sibling to
// GenerateComponentExpression (Codegen.h), producing data instead of a spliced C++23
// expression string. Every value this function reads (Imports, ResolvedImportsList,
// ParseResult) is something Driver::CompileFile already computes for the `.iris` path
// today; this is a serializer over existing types, not a second parser.
//
// `Source` is the whole original file -- needed to slice the raw Nyx-source text between
// `import` statements and `render { }` blocks into `nyx_source` IR nodes (chaos-ir-spec.md
// §3.3): everything Codegen.cpp already treats as opaque pass-through text, just relocated
// into a JSON field instead of spliced output. `ResolvedImportsList` must already have every
// entry of `Imports` resolved -- same precondition Driver::CompileFile's own `.iris` path
// has (an unresolved import is a diagnostic that returns before either path runs).
Amanuensis::Value BuildChaosIr(std::string_view Source, const std::string& FilePath,
                                const std::vector<ImportStatement>& Imports,
                                const std::vector<ResolvedImport>& ResolvedImportsList,
                                const RenderBlockParser::Result& ParseResult);

} // namespace Iris
