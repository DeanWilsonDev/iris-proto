#include "cimmerian/test.hpp"

#include "Iris/NyxTokenizer.h"

#include <string>
#include <vector>

namespace {

std::vector<Iris::Token> TokenizeAll(std::string_view Source) {
    Iris::NyxTokenizer       Tokenizer(Source, "test.irisx");
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

DESCRIBE("NyxTokenizer", {
    IT("detects a basic render block", {
        const auto Tokens = TokenizeAll("render { <Frame class=\"x\"></Frame> }");
        ASSERT_TRUE(Contains(Tokens, Iris::TokenKind::Identifier, "render")); // finds the `render` identifier
        ASSERT_TRUE(CountKind(Tokens, Iris::TokenKind::OpenBrace) == 1);      // finds exactly one open brace
        ASSERT_TRUE(CountKind(Tokens, Iris::TokenKind::CloseBrace) == 1);   // finds exactly one close brace
        ASSERT_TRUE(Contains(Tokens, Iris::TokenKind::StringLiteral, "x")); // string literal (quotes stripped -- see NyxTokenizer.h)
    });

    IT("does not desync brace balancing on a brace inside a string literal", {
        const auto Tokens = TokenizeAll("render { auto s = \"not a { real brace }\"; }");
        ASSERT_TRUE(CountKind(Tokens, Iris::TokenKind::OpenBrace) == 1);
        ASSERT_TRUE(CountKind(Tokens, Iris::TokenKind::CloseBrace) == 1);
    });

    IT("does not desync brace balancing on a brace inside a template string interpolation", {
        // Nyx-specific: a `${ }` interpolation's braces are part of the
        // TemplateStringLiteral's own verbatim span (NyxTokenizer.h), never
        // real structural braces -- the case CppTokenizer has no equivalent
        // of, since C++ has no template-string syntax.
        const auto Tokens = TokenizeAll("render { auto s = `count: ${n}`; }");
        ASSERT_TRUE(CountKind(Tokens, Iris::TokenKind::OpenBrace) == 1);
        ASSERT_TRUE(CountKind(Tokens, Iris::TokenKind::CloseBrace) == 1);
        ASSERT_TRUE(Contains(Tokens, Iris::TokenKind::StringLiteral, "count: ${n}"));
    });

    IT("does not desync brace balancing on a brace inside a line comment", {
        const auto Tokens = TokenizeAll("render { // a { comment with a brace\n }");
        ASSERT_TRUE(CountKind(Tokens, Iris::TokenKind::OpenBrace) == 1);
        ASSERT_TRUE(CountKind(Tokens, Iris::TokenKind::CloseBrace) == 1);
        ASSERT_TRUE(Contains(Tokens, Iris::TokenKind::LineComment, "// a { comment with a brace"));
    });

    IT("does not desync brace balancing on a brace inside a block comment", {
        const auto Tokens = TokenizeAll("render { /* a { block } comment */ }");
        ASSERT_TRUE(CountKind(Tokens, Iris::TokenKind::OpenBrace) == 1);
        ASSERT_TRUE(CountKind(Tokens, Iris::TokenKind::CloseBrace) == 1);
    });

    IT("keeps a comment in its correct source-order position relative to real tokens", {
        // Comments come from nyx::Lexer's own separate Comments() side
        // channel (NyxTokenizer.h) -- this exercises the merge-by-position
        // logic that reconstructs one ordered NextToken() stream from it.
        const auto Tokens = TokenizeAll("auto /* one */ a = /* two */ 1;");
        ASSERT_TRUE(Tokens.size() >= 4);
        std::size_t FirstCommentIndex = 0;
        std::size_t SecondCommentIndex = 0;
        for (std::size_t I = 0; I < Tokens.size(); ++I) {
            if (Tokens[I].Kind == Iris::TokenKind::BlockComment && Tokens[I].Lexeme == "/* one */") {
                FirstCommentIndex = I;
            }
            if (Tokens[I].Kind == Iris::TokenKind::BlockComment && Tokens[I].Lexeme == "/* two */") {
                SecondCommentIndex = I;
            }
        }
        ASSERT_TRUE(FirstCommentIndex > 0 && SecondCommentIndex > FirstCommentIndex);
        // "one" sits between `auto` and `a`; "two" sits between `=` and `1`
        ASSERT_TRUE(Tokens[FirstCommentIndex - 1].Lexeme == "auto");
        ASSERT_TRUE(Tokens[FirstCommentIndex + 1].Lexeme == "a");
    });

    IT("collapses Nyx keywords to Identifier, matching CppTokenizer's own uniform treatment", {
        // `import`/`class` are reserved keywords in Nyx (unlike C++), but
        // RenderBlockParser/ImportResolver only ever match on lexeme text --
        // see NyxTokenizer.h's own doc comment on why this collapse matters.
        const auto Tokens = TokenizeAll("import Button");
        ASSERT_TRUE(Contains(Tokens, Iris::TokenKind::Identifier, "import"));
        ASSERT_TRUE(Contains(Tokens, Iris::TokenKind::Identifier, "Button"));
        ASSERT_TRUE(CountKind(Tokens, Iris::TokenKind::Other) == 0);
    });

    IT("keeps returning EndOfFile on repeated calls past the end", {
        Iris::NyxTokenizer Tokenizer("", "empty.irisx");
        const Iris::Token  First = Tokenizer.NextToken();
        const Iris::Token  Second = Tokenizer.NextToken();
        ASSERT_TRUE(First.Kind == Iris::TokenKind::EndOfFile && Second.Kind == Iris::TokenKind::EndOfFile);
    });

    IT("truncates cleanly at EndOfFile on an unterminated string literal, rather than throwing", {
        const auto Tokens = TokenizeAll("render { auto s = \"unterminated");
        ASSERT_TRUE(Tokens.back().Kind == Iris::TokenKind::EndOfFile);
    });
});
