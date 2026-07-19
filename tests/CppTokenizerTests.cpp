#include "cimmerian/test.hpp"

#include "Iris/CppTokenizer.h"

#include <string>
#include <vector>

namespace {

std::vector<Iris::Token> TokenizeAll(std::string_view Source) {
    Iris::CppTokenizer     Tokenizer(Source, "test.iris");
    std::vector<Iris::Token> Tokens;
    for (;;) {
        Iris::Token Tok = Tokenizer.NextToken();
        Tokens.push_back(Tok);
        if (Tok.Kind == Iris::TokenKind::EndOfFile) {
            break;
        }
    }
    return Tokens;
}

bool Contains(const std::vector<Iris::Token>& Tokens, Iris::TokenKind Kind, std::string_view Lexeme) {
    for (const auto& Tok : Tokens) {
        if (Tok.Kind == Kind && Tok.Lexeme == Lexeme) {
            return true;
        }
    }
    return false;
}

std::size_t CountKind(const std::vector<Iris::Token>& Tokens, Iris::TokenKind Kind) {
    std::size_t Count = 0;
    for (const auto& Tok : Tokens) {
        Count += (Tok.Kind == Kind) ? 1 : 0;
    }
    return Count;
}

} // namespace

DESCRIBE("CppTokenizer", {
    IT("detects a basic render block", {
        const auto Tokens = TokenizeAll("render { <Frame class=\"x\"></Frame> }");
        ASSERT_TRUE(Contains(Tokens, Iris::TokenKind::Identifier, "render")); // finds the `render` identifier
        ASSERT_TRUE(CountKind(Tokens, Iris::TokenKind::OpenBrace) == 1);      // finds exactly one open brace
        ASSERT_TRUE(CountKind(Tokens, Iris::TokenKind::CloseBrace) == 1);    // finds exactly one close brace
        ASSERT_TRUE(Contains(Tokens, Iris::TokenKind::StringLiteral, "\"x\"")); // lexes the class string literal as one token
    });

    IT("does not desync brace balancing on a brace inside a string literal", {
        // The whole point of IHostLanguageTokenizer per docs/iris_core_spec.md
        // §1.3: a `{` inside a string literal must not be seen as a real brace.
        const auto Tokens = TokenizeAll("render { auto s = \"not a { real brace }\"; }");
        ASSERT_TRUE(CountKind(Tokens, Iris::TokenKind::OpenBrace) == 1);
        ASSERT_TRUE(CountKind(Tokens, Iris::TokenKind::CloseBrace) == 1);
    });

    IT("does not desync brace balancing on a brace inside a line comment", {
        const auto Tokens = TokenizeAll("render { // a { comment with a brace\n }");
        ASSERT_TRUE(CountKind(Tokens, Iris::TokenKind::OpenBrace) == 1);
        ASSERT_TRUE(CountKind(Tokens, Iris::TokenKind::CloseBrace) == 1);
        ASSERT_TRUE(Contains(Tokens, Iris::TokenKind::LineComment, "// a { comment with a brace"));
        // the line comment is lexed as a single token, brace and all
    });

    IT("does not desync brace balancing on a brace inside a block comment", {
        const auto Tokens = TokenizeAll("render { /* a { block } comment */ }");
        ASSERT_TRUE(CountKind(Tokens, Iris::TokenKind::OpenBrace) == 1);
        ASSERT_TRUE(CountKind(Tokens, Iris::TokenKind::CloseBrace) == 1);
    });

    IT("does not end a string literal early on an escaped quote", {
        const auto Tokens = TokenizeAll(R"(render { auto s = "a \" with an escaped quote and a { brace"; })");
        ASSERT_TRUE(CountKind(Tokens, Iris::TokenKind::OpenBrace) == 1);
        // escaped quote doesn't terminate the string literal early, so the brace after it stays inside it
    });

    IT("distinguishes a char literal from a digit separator", {
        const auto Tokens = TokenizeAll("auto n = 1'000'000; auto c = '{';");
        ASSERT_TRUE(CountKind(Tokens, Iris::TokenKind::CharLiteral) == 1);
        // digit separators aren't mistaken for char-literal starts, but a real char literal still is
        ASSERT_TRUE(Contains(Tokens, Iris::TokenKind::CharLiteral, "'{'"));
        // the real char literal is lexed correctly, brace and all, and doesn't affect brace balancing
    });

    IT("tracks source locations across a newline", {
        // "a\nb" tokenizes as three tokens: identifier "a", an Other token for
        // the newline itself (starting on line 1, since a token's location is
        // where it starts, before the newline it contains gets consumed), then
        // identifier "b" on line 2.
        Iris::CppTokenizer Tokenizer("a\nb", "loc_test.iris");
        const Iris::Token  First = Tokenizer.NextToken();
        const Iris::Token  Whitespace = Tokenizer.NextToken();
        const Iris::Token  Third = Tokenizer.NextToken();
        ASSERT_TRUE(First.Location.Line == 1 && First.Location.FilePath == "loc_test.iris");
        ASSERT_TRUE(Whitespace.Kind == Iris::TokenKind::Other && Whitespace.Lexeme == "\n");
        // the newline between identifiers is its own Other token
        ASSERT_TRUE(Third.Location.Line == 2); // the identifier after the newline is reported on line 2
    });

    IT("keeps returning EndOfFile on repeated calls past the end", {
        Iris::CppTokenizer Tokenizer("", "empty.iris");
        const Iris::Token  First = Tokenizer.NextToken();
        const Iris::Token  Second = Tokenizer.NextToken();
        ASSERT_TRUE(First.Kind == Iris::TokenKind::EndOfFile && Second.Kind == Iris::TokenKind::EndOfFile);
    });
});
