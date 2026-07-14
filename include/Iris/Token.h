#pragma once

#include "Iris/SourceLocation.h"

#include <string>

namespace Iris {

// Coarser than a full host-language lexical grammar — an IHostLanguageTokenizer
// only needs to distinguish enough to detect `render {` and balance `{ }`
// escape-hatch braces without being fooled by host-language text that merely
// looks like Iris syntax inside a string, char literal, or comment
// (docs/iris_core_spec.md §1.3). Everything else — numbers, operators,
// whitespace, punctuation — collapses into Other; validating any of that is
// the host compiler's job on the generated output, never Iris's.
enum class TokenKind {
    Identifier,
    OpenBrace,
    CloseBrace,
    StringLiteral,
    CharLiteral,
    LineComment,
    BlockComment,
    Other,
    EndOfFile,
};

struct Token {
    TokenKind      Kind{TokenKind::EndOfFile};
    std::string    Lexeme;
    SourceLocation Location;
};

} // namespace Iris
