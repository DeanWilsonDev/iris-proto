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

// The full `.iris`/`.irisx` -> `.cpp` pipeline for one source file: `ScanImports`,
// `RenderBlockParser`, `ValidateElementTree` (against `Config.Target` and the scanned
// import names), and `GenerateComponentExpression` for every `render { }` block found.
// On success, each block's generated `Iris::IrisComponent`-constructing expression is
// spliced back into the original source as `return <expr>;` in place of the
// `render { ... }` block it replaces (docs/iris_core_spec.md §0's "rewrites the file into
// valid host-language source"; Codegen.h's own doc comment specifies this exact wrapping).
// `#line` directives resync line numbers after every splice (docs/iris_core_spec.md §6),
// since collapsing a (usually multi-line) `render { }` block into a single-line `return`
// statement shifts every line after it.
//
// `import Name` is not valid C++23 and can't pass through unchanged. What it *should*
// become in generated output (a `#include`, a forward declaration, something else)
// depends on a header-generation strategy this repo hasn't decided yet — see
// docs/iris_next_steps.md. Until that's decided, each `import Name` line is commented out
// in place (`// import Name`) rather than either left as invalid syntax or guessed at with
// unfounded `#include` behavior; making `Name` visible to the generated `.cpp` remains the
// caller's problem for now. This does not affect semantic validation, which uses the
// *names* `ScanImports` found regardless of whether `ResolveImports` could locate a file
// for them (a separate, already-reported diagnostic here) or what becomes of the line.
//
// `ImportResolver`'s own unresolved-import errors, `RenderBlockParser`'s parse errors,
// `ValidateElementTree`'s semantic errors, and `GenerateComponentExpression`'s codegen
// errors are all collected into one `Diagnostics` list — if any are present, `Output` is
// empty; nothing partially-generated is ever returned.
DriverResult CompileFile(std::string_view Source, std::string FilePath, const IrisConfig& Config,
                          std::string_view ProjectRoot);

} // namespace Iris
