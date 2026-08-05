#pragma once

#include "Iris/IHostLanguageTokenizer.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Iris {

// IHostLanguageTokenizer for Nyx source (.irisx files, once Nyx is the
// active host language — docs/iris_core_spec.md §0 File model,
// nyx-proto's docs/nyx-scripting-language/decision-log.md §7.1). Wraps
// nyx-proto's own real Lexer (linked in as `nyx-lexer`, this repo's
// CMakeLists.txt) and translates its richer nyx::TokenKind down into
// IHostLanguageTokenizer's coarse view, exactly the direction and
// reasoning decision-log.md §7.1 lays out — this reuses Nyx's already-
// tested string-escape/template-interpolation scanning rather than
// reimplementing it a second time, the same way CppTokenizer hand-rolls
// its own instead of linking a full C++ compiler frontend (that tradeoff
// doesn't apply here: nyx-proto is designed to be embedded, so linking its
// real Lexer costs nothing like linking clang would).
//
// Every nyx::TokenKind keyword (Class, Import, If, …) collapses to
// Iris::TokenKind::Identifier alongside nyx::TokenKind::Identifier itself —
// matching CppTokenizer's own uniform treatment of C++ keywords as plain
// identifiers, since IHostLanguageTokenizer's consumers (RenderBlockParser,
// ImportResolver) only ever match on lexeme text ("render", "import",
// "class"), never on whether the host language happens to reserve that
// word. Both nyx::TokenKind::StringLiteral and ::TemplateStringLiteral
// collapse to Iris::TokenKind::StringLiteral — Nyx's own Lexer already
// captures a template string's opening-to-closing-backtick span (including
// any `{`/`}` inside a `${ }` interpolation) as one verbatim token
// (docs/nyx-scripting-language/prototype-implementation-plan.md Task 3.4),
// so no interpolation-aware sub-tokenization is needed here for correct
// brace balancing. Nyx has no char-literal syntax, so
// Iris::TokenKind::CharLiteral is never emitted.
//
// Known limitations of this first cut:
// - nyx::Lexer resolves string/template-string escapes and strips the
//   surrounding quote/backtick characters before a Token's Lexeme is ever
//   built (ScanString/ScanTemplateString) — unlike CppTokenizer, whose
//   StringLiteral Lexeme is the exact raw source substring, quotes and all.
//   Fine for brace-balance detection (the only thing IHostLanguageTokenizer
//   promises), but a future consumer that splices Lexeme text back into
//   generated output verbatim (the way RenderBlockParser's prop-value
//   handling does for CppTokenizer today) would need to re-derive the raw
//   source span itself rather than trust this Lexeme.
// - nyx::Lexer discards whitespace outright rather than emitting it as its
//   own token the way CppTokenizer's LexOther does, so a consumer relying
//   on an explicit whitespace-shaped Other token between two Identifier
//   tokens (RenderBlockParser's PrecededByWhitespace bookkeeping) will not
//   see one from this tokenizer. Not exercised by anything yet, since this
//   adapter isn't wired into RenderBlockParser/ImportResolver/Driver's
//   tokenizer selection — only into IHostLanguageTokenizer's contract in
//   isolation.
// - nyx::Lexer::Tokenize() throws nyx::LexError on an unterminated string/
//   template-string literal rather than degrading gracefully the way
//   CppTokenizer does (consume to EOF). NyxTokenizer's constructor catches
//   that and truncates the token stream at the point of failure — the
//   caller sees a clean, immediate EndOfFile rather than a partially-lexed
//   stream — rather than letting the exception escape
//   IHostLanguageTokenizer's (implicitly non-throwing) contract.
class NyxTokenizer : public IHostLanguageTokenizer {
public:
    // Source must outlive the tokenizer only for the duration of the
    // constructor call — unlike CppTokenizer, which lexes lazily and keeps
    // a Source_ std::string_view alive for the tokenizer's whole lifetime,
    // this tokenizes eagerly up front (nyx::Lexer::Tokenize() has no
    // incremental mode) and stores the translated Token stream, so no
    // reference to Source survives construction.
    NyxTokenizer(std::string_view Source, std::string FilePath);

    Token NextToken() override;

private:
    std::vector<Token> Tokens_;
    std::size_t         Index_{0};
};

} // namespace Iris
