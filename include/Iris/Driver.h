#pragma once

#include "Iris/IrisConfig.h"
#include "Iris/SourceLocation.h"

#include <string>
#include <string_view>
#include <vector>

namespace Iris {

struct DriverDiagnostic {
    std::string    Message;
    SourceLocation Location;
};

struct DriverResult {
    // For `.iris` (host language cpp): valid host-language (C++23) source. For `.irisx`
    // (host language nyx): an Iris IR JSON document -- this repo's own name (CLAUDE.md's
    // "Chaos"/"Cosmos" terminology rule) for what docs/iris_nyx_emission_decision.md and
    // chaos-ir-spec.md §3 both specify the schema for -- not C++ -- `.irisx` bypasses
    // Codegen.cpp entirely rather than producing text a C++ compiler could ever accept
    // (docs/next-steps.md, "`Codegen` has no Nyx-target emission"). Empty whenever
    // Diagnostics is non-empty, same convention as CodegenResult, in both cases.
    std::string                    Output;
    std::vector<DriverDiagnostic>  Diagnostics;
};

// The full `.iris`/`.irisx` -> generated-output pipeline for one source file: `ScanImports`,
// `RenderBlockParser`, `ValidateElementTree` (against `Config.Target` and the scanned import
// names), then a fork by host language (`TokenizerFactory.h`'s `DetermineHostLanguage`, the
// same dispatch fact `CreateHostLanguageTokenizer` already uses):
//
// - `.iris` (cpp): `GenerateComponentExpression` for every `render { }` block found, then
//   each block's generated `Iris::Component`-constructing expression is spliced back into
//   the original source as `return <expr>;` in place of the `render { ... }` block it
//   replaces (docs/iris_core_spec.md §0's "rewrites the file into valid host-language
//   source"; Codegen.h's own doc comment specifies this exact wrapping). `#line` directives
//   resync line numbers after every splice (docs/iris_core_spec.md §6), since collapsing a
//   (usually multi-line) `render { }` block into a single-line `return` statement shifts
//   every line after it.
// - `.irisx` (nyx): `Codegen.cpp` is never invoked. `BuildIrisIr` (`IrisIr.h`) walks the
//   same `ScanImports`/`RenderBlockParser` output directly into an Iris IR JSON document,
//   serialized via `Amanuensis::Writer::WriteToString`. No C++ text is ever produced or
//   spliced for this path.
//
// Per docs/iris_import_header_decision.md: every `.iris` file compiles to one self-contained
// header (conventionally `<original-path>.h`, e.g. `Button.iris.h`) rather than a
// declaration/definition pair — Iris never parses struct or function signatures
// (docs/iris_core_spec.md §2.1), so it has no way to synthesize a real forward declaration
// for either `<Name>Props` or `<Name>` itself; only a full, header-only definition is
// achievable without crossing that boundary. `Output` therefore always starts with
// `#pragma once`, and each `import Name` line is replaced with an `#include` of that
// resolved import's own generated header, path computed relative to `ProjectRoot` (the
// same root `Config.SearchPaths` are already relative to — a consuming build is expected
// to add `-I <ProjectRoot>`). Because definitions are header-only, a component's function
// needs `inline` to stay ODR-safe when the header is included by more than one
// translation unit — Iris doesn't inject this (it would mean parsing the function
// signature it's committed not to touch); it's a convention the component's author
// applies themselves, same as any other host-language detail Iris passes through
// untouched.
//
// `.irisx` doesn't go through any of the above -- its output is Iris IR JSON data, not a
// header, so nothing about the `#pragma once`/`#include`-splicing/`inline` conventions
// applies to it. Its own naming convention (`<original-path>.iris.ir`, e.g.
// `Button.irisx` -> `Button.iris.ir`) is established at the `iris_cc` CLI/
// `cmake/IrisCompileDirectory.cmake` layer, not here -- `Driver::CompileFile` itself is
// agnostic to what path `Output` eventually gets written to.
//
// `ImportResolver`'s own unresolved-import errors, `RenderBlockParser`'s parse errors,
// `ValidateElementTree`'s semantic errors, and `GenerateComponentExpression`'s codegen
// errors are all collected into one `Diagnostics` list — if any are present, `Output` is
// empty; nothing partially-generated is ever returned.
DriverResult CompileFile(std::string_view Source, std::string FilePath, const IrisConfig& Config,
                          std::string_view ProjectRoot);

} // namespace Iris
