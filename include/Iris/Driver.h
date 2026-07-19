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
    // Valid host-language (C++23) source — empty whenever Diagnostics is non-empty, same
    // convention as CodegenResult.
    std::string                    Output;
    std::vector<DriverDiagnostic>  Diagnostics;
};

// The full `.iris`/`.irisx` -> generated-header pipeline for one source file:
// `ScanImports`, `RenderBlockParser`, `ValidateElementTree` (against `Config.Target` and
// the scanned import names), and `GenerateComponentExpression` for every `render { }`
// block found. On success, each block's generated `Iris::IrisComponent`-constructing
// expression is spliced back into the original source as `return <expr>;` in place of the
// `render { ... }` block it replaces (docs/iris_core_spec.md §0's "rewrites the file into
// valid host-language source"; Codegen.h's own doc comment specifies this exact wrapping).
// `#line` directives resync line numbers after every splice (docs/iris_core_spec.md §6),
// since collapsing a (usually multi-line) `render { }` block into a single-line `return`
// statement shifts every line after it.
//
// Per docs/iris_import_header_decision.md: every `.iris`/`.irisx` file compiles to one
// self-contained header (conventionally `<original-path>.h`, e.g. `Button.iris.h`) rather
// than a declaration/definition pair — Iris never parses struct or function signatures
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
// `ImportResolver`'s own unresolved-import errors, `RenderBlockParser`'s parse errors,
// `ValidateElementTree`'s semantic errors, and `GenerateComponentExpression`'s codegen
// errors are all collected into one `Diagnostics` list — if any are present, `Output` is
// empty; nothing partially-generated is ever returned.
DriverResult CompileFile(std::string_view Source, std::string FilePath, const IrisConfig& Config,
                          std::string_view ProjectRoot);

} // namespace Iris
