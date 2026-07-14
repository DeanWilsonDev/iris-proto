#pragma once

#include "Iris/IHostLanguageTokenizer.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace Iris {

// IHostLanguageTokenizer for C++23 source (.iris files, docs/iris_core_spec.md
// §0 File model). Understands just enough C++ lexical syntax — identifiers,
// braces, string/char literals (with backslash escapes), line and block
// comments — to detect `render {` blocks and balance escape-hatch braces
// correctly. Does not tokenize the full C++ grammar; numbers, operators, and
// everything else fall through as TokenKind::Other — validating any of that
// is the host compiler's job on the generated output, never Iris's.
//
// Known limitations of this first cut, not yet handled: raw string literals
// (R"(...)")  are lexed as ordinary Other text rather than as a single
// string-literal span, so a `{`/`}` inside one would desync brace balancing
// the same way an unhandled string would. Digit separators (`1'000'000`) are
// distinguished from char-literal starts by a simple heuristic — a `'`
// immediately preceded by a digit is treated as Other, not a char-literal
// start — which covers the common case but not every malformed edge case.
class CppTokenizer : public IHostLanguageTokenizer {
public:
    // Source must outlive the tokenizer — no copy is taken. FilePath is
    // carried on every emitted Token's SourceLocation (docs/iris_core_spec.md
    // §6's source-mapping).
    CppTokenizer(std::string_view Source, std::string FilePath);

    Token NextToken() override;

private:
    bool AtEnd() const;
    char CurrentChar() const;
    char PeekChar(std::size_t Offset = 1) const;
    void Advance();

    Token LexIdentifier();
    Token LexStringLiteral();
    Token LexCharLiteral();
    Token LexLineComment();
    Token LexBlockComment();
    Token LexOther();

    SourceLocation CurrentLocation() const;

    std::string_view Source_;
    std::string       FilePath_;
    std::size_t       Pos_{0};
    std::uint32_t     Line_{1};
    std::uint32_t     Column_{1};
};

} // namespace Iris
