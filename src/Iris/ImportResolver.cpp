#include "Iris/ImportResolver.h"

#include "Iris/CppTokenizer.h"

#include <cctype>
#include <filesystem>
#include <optional>

namespace Iris {

namespace {

// True for an Other token that is pure whitespace — CppTokenizer bundles
// whitespace runs (and other non-identifier text) into a single Other token,
// so the gap between "import" and its name is exactly one such token when
// nothing else separates them.
bool IsWhitespaceOnly(const Token& Tok) {
    if (Tok.Kind != TokenKind::Other) {
        return false;
    }
    for (const char C : Tok.Lexeme) {
        if (!std::isspace(static_cast<unsigned char>(C))) {
            return false;
        }
    }
    return true;
}

} // namespace

std::vector<ImportStatement> ScanImports(std::string_view Source, std::string FilePath) {
    std::vector<ImportStatement> Imports;
    CppTokenizer                 Tokenizer(Source, FilePath);

    std::optional<Token> PendingImport;
    Token                Tok = Tokenizer.NextToken();
    while (Tok.Kind != TokenKind::EndOfFile) {
        if (PendingImport) {
            if (Tok.Kind == TokenKind::Identifier) {
                Imports.push_back(ImportStatement{Tok.Lexeme, PendingImport->Location});
                PendingImport.reset();
            } else if (!IsWhitespaceOnly(Tok) && Tok.Kind != TokenKind::LineComment &&
                       Tok.Kind != TokenKind::BlockComment) {
                PendingImport.reset();
            }
        }

        if (Tok.Kind == TokenKind::Identifier && Tok.Lexeme == "import") {
            PendingImport = Tok;
        }

        Tok = Tokenizer.NextToken();
    }

    return Imports;
}

ImportResolutionResult ResolveImports(const std::vector<ImportStatement>& Imports, const IrisConfig& Config,
                                       std::string_view ProjectRoot) {
    ImportResolutionResult Result;

    const std::string_view Extension = Config.Target == IrisBuildTarget::Penumbra ? ".iris" : ".irisx";
    const std::filesystem::path Root(ProjectRoot);

    for (const ImportStatement& Import : Imports) {
        bool Found = false;
        for (const std::string& SearchPath : Config.SearchPaths) {
            std::filesystem::path Candidate = Root / SearchPath / (Import.Name + std::string(Extension));
            std::error_code       Ignored;
            if (std::filesystem::is_regular_file(Candidate, Ignored)) {
                Result.Resolved.push_back(ResolvedImport{Import.Name, Candidate.string()});
                Found = true;
                break;
            }
        }
        if (!Found) {
            Result.Errors.push_back(ImportError{"Cannot resolve `import " + Import.Name + "`: no `" + Import.Name +
                                                      std::string(Extension) + "` found in any search path.",
                                                  Import.Location});
        }
    }

    return Result;
}

} // namespace Iris
