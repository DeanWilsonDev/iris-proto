#include "Iris/CppTokenizer.h"

#include <cctype>

namespace Iris {

CppTokenizer::CppTokenizer(std::string_view Source, std::string FilePath)
    : Source_(Source), FilePath_(std::move(FilePath)) {}

bool CppTokenizer::AtEnd() const { return Pos_ >= Source_.size(); }

char CppTokenizer::CurrentChar() const { return AtEnd() ? '\0' : Source_[Pos_]; }

char CppTokenizer::PeekChar(std::size_t Offset) const {
    const std::size_t Index = Pos_ + Offset;
    return Index < Source_.size() ? Source_[Index] : '\0';
}

void CppTokenizer::Advance() {
    if (AtEnd()) {
        return;
    }
    if (Source_[Pos_] == '\n') {
        ++Line_;
        Column_ = 1;
    } else {
        ++Column_;
    }
    ++Pos_;
}

SourceLocation CppTokenizer::CurrentLocation() const {
    return SourceLocation{FilePath_, Line_, Column_};
}

Token CppTokenizer::NextToken() {
    if (AtEnd()) {
        return Token{TokenKind::EndOfFile, "", CurrentLocation()};
    }

    const char C = CurrentChar();

    if (std::isalpha(static_cast<unsigned char>(C)) || C == '_') {
        return LexIdentifier();
    }
    if (C == '"') {
        return LexStringLiteral();
    }
    if (C == '\'') {
        // Digit-separator heuristic (1'000'000): a `'` immediately preceded
        // by a digit is not a char-literal start. See CppTokenizer.h's
        // documented limitations.
        const bool PrecededByDigit = Pos_ > 0 && std::isdigit(static_cast<unsigned char>(Source_[Pos_ - 1]));
        if (!PrecededByDigit) {
            return LexCharLiteral();
        }
    }
    if (C == '/' && PeekChar() == '/') {
        return LexLineComment();
    }
    if (C == '/' && PeekChar() == '*') {
        return LexBlockComment();
    }
    if (C == '{') {
        const SourceLocation Loc = CurrentLocation();
        Advance();
        return Token{TokenKind::OpenBrace, "{", Loc};
    }
    if (C == '}') {
        const SourceLocation Loc = CurrentLocation();
        Advance();
        return Token{TokenKind::CloseBrace, "}", Loc};
    }
    return LexOther();
}

Token CppTokenizer::LexIdentifier() {
    const SourceLocation Loc = CurrentLocation();
    const std::size_t    Start = Pos_;
    while (!AtEnd() && (std::isalnum(static_cast<unsigned char>(CurrentChar())) || CurrentChar() == '_')) {
        Advance();
    }
    return Token{TokenKind::Identifier, std::string(Source_.substr(Start, Pos_ - Start)), Loc};
}

Token CppTokenizer::LexStringLiteral() {
    const SourceLocation Loc = CurrentLocation();
    const std::size_t    Start = Pos_;
    Advance(); // opening "
    while (!AtEnd() && CurrentChar() != '"') {
        if (CurrentChar() == '\\' && !AtEnd()) {
            Advance(); // skip the escaped character too, whatever it is
            if (!AtEnd()) {
                Advance();
            }
            continue;
        }
        Advance();
    }
    if (!AtEnd()) {
        Advance(); // closing " -- if absent (unterminated literal), we've
                   // simply consumed to EOF, same as an unterminated string
                   // would leave any lexer.
    }
    return Token{TokenKind::StringLiteral, std::string(Source_.substr(Start, Pos_ - Start)), Loc};
}

Token CppTokenizer::LexCharLiteral() {
    const SourceLocation Loc = CurrentLocation();
    const std::size_t    Start = Pos_;
    Advance(); // opening '
    while (!AtEnd() && CurrentChar() != '\'') {
        if (CurrentChar() == '\\' && !AtEnd()) {
            Advance();
            if (!AtEnd()) {
                Advance();
            }
            continue;
        }
        Advance();
    }
    if (!AtEnd()) {
        Advance(); // closing '
    }
    return Token{TokenKind::CharLiteral, std::string(Source_.substr(Start, Pos_ - Start)), Loc};
}

Token CppTokenizer::LexLineComment() {
    const SourceLocation Loc = CurrentLocation();
    const std::size_t    Start = Pos_;
    Advance(); // first /
    Advance(); // second /
    while (!AtEnd() && CurrentChar() != '\n') {
        Advance();
    }
    return Token{TokenKind::LineComment, std::string(Source_.substr(Start, Pos_ - Start)), Loc};
}

Token CppTokenizer::LexBlockComment() {
    const SourceLocation Loc = CurrentLocation();
    const std::size_t    Start = Pos_;
    Advance(); // /
    Advance(); // *
    while (!AtEnd() && !(CurrentChar() == '*' && PeekChar() == '/')) {
        Advance();
    }
    if (!AtEnd()) {
        Advance(); // *
        Advance(); // /
    }
    return Token{TokenKind::BlockComment, std::string(Source_.substr(Start, Pos_ - Start)), Loc};
}

Token CppTokenizer::LexOther() {
    const SourceLocation Loc = CurrentLocation();
    const std::size_t    Start = Pos_;
    while (!AtEnd()) {
        const char C = CurrentChar();
        const bool StartsIdentifier = std::isalpha(static_cast<unsigned char>(C)) || C == '_';
        const bool StartsString = C == '"';
        const bool StartsChar = C == '\'' &&
            !(Pos_ > 0 && std::isdigit(static_cast<unsigned char>(Source_[Pos_ - 1])));
        const bool StartsLineComment = C == '/' && PeekChar() == '/';
        const bool StartsBlockComment = C == '/' && PeekChar() == '*';
        const bool IsBrace = C == '{' || C == '}';
        if (StartsIdentifier || StartsString || StartsChar || StartsLineComment || StartsBlockComment || IsBrace) {
            break;
        }
        Advance();
    }
    return Token{TokenKind::Other, std::string(Source_.substr(Start, Pos_ - Start)), Loc};
}

} // namespace Iris
