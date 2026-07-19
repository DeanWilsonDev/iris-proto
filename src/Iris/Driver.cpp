#include "Iris/Driver.h"
#include "Iris/Codegen.h"
#include "Iris/ImportResolver.h"
#include "Iris/RenderBlockParser.h"
#include "Iris/SemanticValidator.h"

#include <algorithm>
#include <unordered_set>

namespace Iris {

namespace {

// Converts a 1-based (Line, Column) — the only form SourceLocation carries — into a byte
// offset into Source. O(n) per call, which is fine here: a driver run touches each
// location a handful of times, not in a hot loop.
std::size_t LocationToOffset(std::string_view Source, const SourceLocation& Loc) {
    std::size_t   Offset = 0;
    std::uint32_t Line = 1;
    std::uint32_t Column = 1;
    while (Offset < Source.size() && (Line < Loc.Line || (Line == Loc.Line && Column < Loc.Column))) {
        if (Source[Offset] == '\n') {
            ++Line;
            Column = 1;
        } else {
            ++Column;
        }
        ++Offset;
    }
    return Offset;
}

std::string EscapePathForLineDirective(std::string_view Path) {
    std::string Result;
    Result.reserve(Path.size());
    for (const char C : Path) {
        if (C == '\\' || C == '"') {
            Result += '\\';
        }
        Result += C;
    }
    return Result;
}

// One byte-range replacement (or, when StartOffset == EndOffset, a pure insertion) to
// apply to the original source. Edits never overlap: `import` statements never appear
// inside a `render { }` block (docs/iris_core_spec.md §1.2), so the two edit kinds this
// driver produces can't collide.
struct Edit {
    std::size_t StartOffset;
    std::size_t EndOffset;
    std::string Replacement;
};

} // namespace

DriverResult CompileFile(std::string_view Source, std::string FilePath, const IrisConfig& Config,
                          std::string_view ProjectRoot) {
    DriverResult Result;

    const std::vector<ImportStatement> Imports = ScanImports(Source, FilePath);
    const ImportResolutionResult       ResolvedImports = ResolveImports(Imports, Config, ProjectRoot);
    for (const ImportError& Err : ResolvedImports.Errors) {
        Result.Diagnostics.push_back(DriverDiagnostic{Err.Message, Err.Location});
    }

    // Semantic validation only cares which names were *declared* imported, independent of
    // whether ResolveImports could find a file for them — that's the already-reported
    // ImportError above, not re-checked here (SemanticValidator.h's own documented
    // contract).
    std::unordered_set<std::string> ImportedNames;
    for (const ImportStatement& Import : Imports) {
        ImportedNames.insert(Import.Name);
    }

    RenderBlockParser               Parser(Source, FilePath);
    const RenderBlockParser::Result ParseResult = Parser.Parse();
    for (const RenderBlockParser::ParseError& Err : ParseResult.Errors) {
        Result.Diagnostics.push_back(DriverDiagnostic{Err.Message, Err.Location});
    }

    std::vector<std::string> GeneratedPerBlock;
    GeneratedPerBlock.reserve(ParseResult.Blocks.size());
    for (const RenderBlockParser::ParsedBlock& Block : ParseResult.Blocks) {
        for (const SemanticError& Err : ValidateElementTree(Block.Root, Config.Target, ImportedNames)) {
            Result.Diagnostics.push_back(DriverDiagnostic{Err.Message, Err.Location});
        }
        const CodegenResult Codegen = GenerateComponentExpression(Block.Root);
        for (const CodegenError& Err : Codegen.Errors) {
            Result.Diagnostics.push_back(DriverDiagnostic{Err.Message, Err.Location});
        }
        GeneratedPerBlock.push_back(Codegen.Source);
    }

    if (!Result.Diagnostics.empty()) {
        return Result; // Output stays empty — nothing partially-generated is ever returned.
    }

    const std::string EscapedFilePath = EscapePathForLineDirective(FilePath);

    std::vector<Edit> Edits;
    Edits.reserve(Imports.size() + ParseResult.Blocks.size());

    // `import Name` isn't valid C++23 — commented out in place rather than passed through
    // or guessed at (see Driver.h's doc comment for why). A pure insertion right before
    // the `import` keyword turns the rest of its line into a `//` comment without
    // disturbing line count or leading indentation.
    for (const ImportStatement& Import : Imports) {
        const std::size_t Offset = LocationToOffset(Source, Import.Location);
        Edits.push_back(Edit{Offset, Offset, "// "});
    }

    // Each render{ } block becomes `return <expr>;` (Codegen.h's documented wrapping
    // convention), followed by a `#line` directive resyncing the line count for whatever
    // follows — collapsing a block onto one line shifts every subsequent line otherwise.
    for (std::size_t Index = 0; Index < ParseResult.Blocks.size(); ++Index) {
        const RenderBlockParser::ParsedBlock& Block = ParseResult.Blocks[Index];
        Edit                                   E;
        E.StartOffset = LocationToOffset(Source, Block.Location);
        E.EndOffset = LocationToOffset(Source, Block.EndLocation);
        E.Replacement = "return " + GeneratedPerBlock[Index] + ";\n#line " +
                         std::to_string(Block.EndLocation.Line) + " \"" + EscapedFilePath + "\"\n";
        Edits.push_back(std::move(E));
    }

    std::sort(Edits.begin(), Edits.end(),
              [](const Edit& A, const Edit& B) { return A.StartOffset < B.StartOffset; });

    std::string Output;
    Output.reserve(Source.size());
    Output += "#line 1 \"" + EscapedFilePath + "\"\n";
    std::size_t Cursor = 0;
    for (const Edit& E : Edits) {
        Output.append(Source.substr(Cursor, E.StartOffset - Cursor));
        Output += E.Replacement;
        Cursor = E.EndOffset;
    }
    Output.append(Source.substr(Cursor));

    Result.Output = std::move(Output);
    return Result;
}

} // namespace Iris
