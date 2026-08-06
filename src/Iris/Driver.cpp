#include "Iris/Driver.h"
#include "Iris/IrisIr.h"
#include "Iris/Codegen.h"
#include "Iris/ImportResolver.h"
#include "Iris/RenderBlockParser.h"
#include "Iris/SemanticValidator.h"
#include "Iris/TokenizerFactory.h"

#include <amanuensis/io/writer.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_map>
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

// The byte offset one past the end of an `import Name` statement, given where `import`
// itself starts. Skips plain whitespace between the two identifiers, then `Name`'s own
// length — matches every `import` statement in the spec and this codebase. A comment
// between `import` and `Name` (e.g. `import /* x */ Button`) would break this, same class
// of documented, accepted limitation as CppTokenizer.h's own heuristics elsewhere in this
// preprocessor.
std::size_t ImportStatementEndOffset(std::string_view Source, std::size_t ImportKeywordOffset,
                                      std::string_view Name) {
    std::size_t Offset = ImportKeywordOffset + 6; // strlen("import")
    while (Offset < Source.size() && std::isspace(static_cast<unsigned char>(Source[Offset])) != 0) {
        ++Offset;
    }
    return Offset + Name.size();
}

// docs/iris_import_header_decision.md: every `.iris`/`.irisx` file generates one
// self-contained header (source path + `.h`, e.g. `Button.iris.h`) rather than a
// declaration/definition pair — Iris never parses struct or function signatures
// (docs/iris_core_spec.md §2.1), so it has no way to synthesize a real forward
// declaration for either `<Name>Props` or `<Name>` itself.
std::string ToHeaderPath(const std::string& SourcePath) { return SourcePath + ".h"; }

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

    // `.irisx` (nyx) bypasses Codegen.cpp entirely — there is nothing to compile a `render
    // { }` block's escape-hatch content *to* in Nyx, only IR to serialize it as (see
    // Driver.h's own doc comment, docs/iris_nyx_emission_decision.md). `.iris` (cpp) keeps
    // its existing GenerateComponentExpression + textual-splice pipeline, unchanged below.
    const bool IsNyx = DetermineHostLanguage(FilePath) == HostLanguage::Nyx;

    std::vector<std::string> GeneratedPerBlock;
    if (!IsNyx) {
        GeneratedPerBlock.reserve(ParseResult.Blocks.size());
    }
    for (const RenderBlockParser::ParsedBlock& Block : ParseResult.Blocks) {
        for (const SemanticError& Err : ValidateElementTree(Block.Root, Config.Target, ImportedNames)) {
            Result.Diagnostics.push_back(DriverDiagnostic{Err.Message, Err.Location});
        }
        if (IsNyx) {
            continue;
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

    // Every import resolved successfully by this point — an unresolved one is a
    // Diagnostics entry above, which already returned early.
    if (IsNyx) {
        const Amanuensis::Value IrisIrDoc =
            BuildIrisIr(Source, FilePath, Imports, ResolvedImports.Resolved, ParseResult);
        Result.Output = Amanuensis::Writer::WriteToString(IrisIrDoc);
        return Result;
    }

    const std::string EscapedFilePath = EscapePathForLineDirective(FilePath);

    std::unordered_map<std::string, std::string> ResolvedPathByName;
    for (const ResolvedImport& R : ResolvedImports.Resolved) {
        ResolvedPathByName[R.Name] = R.ResolvedPath;
    }

    std::vector<Edit> Edits;
    Edits.reserve(Imports.size() + ParseResult.Blocks.size());

    // `import Name` isn't valid C++23 — replaced with an `#include` of the imported
    // component's generated header (docs/iris_import_header_decision.md), resolved
    // relative to ProjectRoot so it lines up with a `-I <ProjectRoot>` build convention
    // (the same root `searchPaths` are already relative to).
    for (const ImportStatement& Import : Imports) {
        const std::size_t StartOffset = LocationToOffset(Source, Import.Location);
        const std::size_t EndOffset = ImportStatementEndOffset(Source, StartOffset, Import.Name);
        const std::string HeaderPath = ToHeaderPath(ResolvedPathByName.at(Import.Name));
        const std::filesystem::path IncludePath = std::filesystem::relative(HeaderPath, ProjectRoot);

        Edits.push_back(Edit{StartOffset, EndOffset, "#include \"" + IncludePath.generic_string() + "\""});
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

    // `#pragma once` up front — every generated file is a self-contained header now
    // (docs/iris_import_header_decision.md), included by every file that imports it.
    std::string Output;
    Output.reserve(Source.size());
    Output += "#pragma once\n#line 1 \"" + EscapedFilePath + "\"\n";
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
