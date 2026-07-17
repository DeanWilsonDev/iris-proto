#pragma once

#include "Iris/IrisConfig.h"
#include "Iris/SourceLocation.h"

#include <string>
#include <string_view>
#include <vector>

namespace Iris {

// One `import Name` statement found in a `.iris`/`.irisx` source file
// (docs/iris_core_spec.md §1.2). Scanning is purely lexical — it doesn't
// resolve `Name` against any search path, just finds the statements.
struct ImportStatement {
    std::string    Name;
    SourceLocation Location;
};

// Scans the whole file for top-level `import Name` statements: an Identifier
// token spelled "import" immediately followed by another Identifier token.
// Uses the same CppTokenizer front end as RenderBlockParser, so string/char
// literals and comments that merely contain the text "import" can't be
// mistaken for the keyword. `import` never appears inside a `render { }`
// block (§1.2), but this scanner doesn't need to know where render blocks
// are — the token sequence "Identifier(import) Identifier(Name)" doesn't
// occur validly anywhere else in host-language code.
std::vector<ImportStatement> ScanImports(std::string_view Source, std::string FilePath);

// One `import` successfully resolved to a file on disk.
struct ResolvedImport {
    std::string Name;
    std::string ResolvedPath;
};

struct ImportError {
    std::string    Message;
    SourceLocation Location;
};

struct ImportResolutionResult {
    std::vector<ResolvedImport> Resolved;
    std::vector<ImportError>    Errors;
};

// Resolves each ScanImports() result to `Name.iris` (Config.Target ==
// Penumbra) or `Name.irisx` (Config.Target == UmbraEngine), searched across
// Config.SearchPaths in declaration order, each taken relative to
// ProjectRoot (docs/iris_core_spec.md §1.2, §5). The first search path
// containing the file wins; an import satisfied by no search path is an
// error, not a fatal failure — resolution continues for the rest.
ImportResolutionResult ResolveImports(const std::vector<ImportStatement>& Imports, const IrisConfig& Config,
                                       std::string_view ProjectRoot);

} // namespace Iris
