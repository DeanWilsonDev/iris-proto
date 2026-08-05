#include "Iris/NyxTokenizer.h"

#include "lexer/lexer.hpp"

namespace Iris {

namespace {

bool IsIdentifierLike(nyx::TokenKind Kind) {
    switch (Kind) {
        case nyx::TokenKind::Identifier:
        case nyx::TokenKind::Class:
        case nyx::TokenKind::Data:
        case nyx::TokenKind::Enum:
        case nyx::TokenKind::Interface:
        case nyx::TokenKind::Trait:
        case nyx::TokenKind::Import:
        case nyx::TokenKind::Return:
        case nyx::TokenKind::Throw:
        case nyx::TokenKind::Guard:
        case nyx::TokenKind::If:
        case nyx::TokenKind::Else:
        case nyx::TokenKind::While:
        case nyx::TokenKind::For:
        case nyx::TokenKind::Break:
        case nyx::TokenKind::Continue:
        case nyx::TokenKind::When:
        case nyx::TokenKind::Auto:
        case nyx::TokenKind::Const:
        case nyx::TokenKind::Static:
        case nyx::TokenKind::Virtual:
        case nyx::TokenKind::Override:
        case nyx::TokenKind::Public:
        case nyx::TokenKind::Private:
        case nyx::TokenKind::Protected:
        case nyx::TokenKind::Null:
        case nyx::TokenKind::True:
        case nyx::TokenKind::False:
        case nyx::TokenKind::Void:
        case nyx::TokenKind::As:
        case nyx::TokenKind::Namespace:
            return true;
        default:
            return false;
    }
}

TokenKind TranslateKind(nyx::TokenKind Kind) {
    if (IsIdentifierLike(Kind)) {
        return TokenKind::Identifier;
    }
    switch (Kind) {
        case nyx::TokenKind::LBrace:
            return TokenKind::OpenBrace;
        case nyx::TokenKind::RBrace:
            return TokenKind::CloseBrace;
        case nyx::TokenKind::StringLiteral:
        case nyx::TokenKind::TemplateStringLiteral:
            return TokenKind::StringLiteral;
        case nyx::TokenKind::LineComment:
            return TokenKind::LineComment;
        case nyx::TokenKind::BlockComment:
            return TokenKind::BlockComment;
        case nyx::TokenKind::Eof:
            return TokenKind::EndOfFile;
        default:
            // Operators, delimiters other than braces, numeric literals, and
            // decorators all fall through as Other — CppTokenizer's own
            // numbers/operators do the same; validating any of it is the
            // Nyx compiler's job on the generated output, never this
            // tokenizer's (IHostLanguageTokenizer.h's own doc comment).
            return TokenKind::Other;
    }
}

Token Translate(const nyx::Token& Src, const std::string& FilePath) {
    return Token{TranslateKind(Src.kind), Src.lexeme,
                 SourceLocation{FilePath, static_cast<std::uint32_t>(Src.line),
                                static_cast<std::uint32_t>(Src.column)}};
}

// Strict source-position ordering shared by both the main token stream and
// the side-channel comment stream (nyx::Lexer::Comments()), so they can be
// merged back into one ordered NextToken() sequence. Every comment is
// scanned strictly before whichever real token follows it
// (Lexer::SkipWhitespaceAndComments() always runs immediately before
// Lexer::ScanToken() inside Tokenize()'s own loop), so no two entries from
// the two streams ever share a position.
bool StartsBefore(const nyx::Token& A, const nyx::Token& B) {
    return A.line != B.line ? A.line < B.line : A.column < B.column;
}

} // namespace

NyxTokenizer::NyxTokenizer(std::string_view Source, std::string FilePath) {
    nyx::Lexer NyxLexer{std::string(Source)};

    std::vector<nyx::Token> Main;
    try {
        Main = NyxLexer.Tokenize();
    } catch (const nyx::LexError&) {
        // Tokenize() builds its result on the stack and only returns it at
        // the very end, so throwing mid-scan (an unterminated string/
        // template-string literal) leaves Main empty — there is nothing
        // partial to recover here. See this class's own header comment for
        // why a clean, immediate EndOfFile is the documented behavior
        // rather than letting the exception escape.
        Tokens_.push_back(Token{TokenKind::EndOfFile, "", SourceLocation{FilePath, 1, 1}});
        return;
    }

    const std::vector<nyx::Token>& Comments = NyxLexer.Comments();

    Tokens_.reserve(Main.size() + Comments.size());
    std::size_t MainIndex = 0;
    std::size_t CommentIndex = 0;
    while (MainIndex < Main.size()) {
        if (CommentIndex < Comments.size() && StartsBefore(Comments[CommentIndex], Main[MainIndex])) {
            Tokens_.push_back(Translate(Comments[CommentIndex], FilePath));
            ++CommentIndex;
        } else {
            Tokens_.push_back(Translate(Main[MainIndex], FilePath));
            ++MainIndex;
        }
    }
    // Nyx's scanning order never produces a comment positioned after the
    // final (Eof) token, so this loop never actually runs — a defensive
    // drain rather than a silent drop, in case that invariant ever changes.
    for (; CommentIndex < Comments.size(); ++CommentIndex) {
        Tokens_.push_back(Translate(Comments[CommentIndex], FilePath));
    }
}

Token NyxTokenizer::NextToken() {
    if (Index_ >= Tokens_.size()) {
        return Tokens_.back(); // always at least the Eof/EndOfFile token
    }
    return Tokens_[Index_++];
}

} // namespace Iris
