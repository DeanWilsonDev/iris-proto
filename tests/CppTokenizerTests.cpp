#include "Iris/CppTokenizer.h"

#include <cstdio>
#include <string>
#include <vector>

int Failures = 0;

namespace {

void Expect(bool Condition, const std::string& Description) {
    if (Condition) {
        std::printf("[PASS] %s\n", Description.c_str());
    } else {
        std::printf("[FAIL] %s\n", Description.c_str());
        ++Failures;
    }
}

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

void TestBasicRenderBlockDetection() {
    const auto Tokens = TokenizeAll("render { <Frame class=\"x\"></Frame> }");
    Expect(Contains(Tokens, Iris::TokenKind::Identifier, "render"),
           "finds the `render` identifier");
    Expect(CountKind(Tokens, Iris::TokenKind::OpenBrace) == 1, "finds exactly one open brace");
    Expect(CountKind(Tokens, Iris::TokenKind::CloseBrace) == 1, "finds exactly one close brace");
    Expect(Contains(Tokens, Iris::TokenKind::StringLiteral, "\"x\""),
           "lexes the class string literal as one token");
}

void TestBraceInsideStringLiteralDoesNotDesyncBalancing() {
    // The whole point of IHostLanguageTokenizer per docs/iris_core_spec.md
    // §1.3: a `{` inside a string literal must not be seen as a real brace.
    const auto Tokens = TokenizeAll("render { auto s = \"not a { real brace }\"; }");
    Expect(CountKind(Tokens, Iris::TokenKind::OpenBrace) == 1,
           "brace inside a string literal is not counted as a real open brace");
    Expect(CountKind(Tokens, Iris::TokenKind::CloseBrace) == 1,
           "brace inside a string literal is not counted as a real close brace");
}

void TestBraceInsideLineCommentDoesNotDesyncBalancing() {
    const auto Tokens = TokenizeAll("render { // a { comment with a brace\n }");
    Expect(CountKind(Tokens, Iris::TokenKind::OpenBrace) == 1,
           "brace inside a line comment is not counted as a real open brace");
    Expect(CountKind(Tokens, Iris::TokenKind::CloseBrace) == 1,
           "brace inside a line comment is not counted as a real close brace");
    Expect(Contains(Tokens, Iris::TokenKind::LineComment, "// a { comment with a brace"),
           "the line comment is lexed as a single token, brace and all");
}

void TestBraceInsideBlockCommentDoesNotDesyncBalancing() {
    const auto Tokens = TokenizeAll("render { /* a { block } comment */ }");
    Expect(CountKind(Tokens, Iris::TokenKind::OpenBrace) == 1,
           "brace inside a block comment is not counted as a real open brace");
    Expect(CountKind(Tokens, Iris::TokenKind::CloseBrace) == 1,
           "brace inside a block comment is not counted as a real close brace");
}

void TestEscapedQuoteInsideStringLiteral() {
    const auto Tokens = TokenizeAll(R"(render { auto s = "a \" with an escaped quote and a { brace"; })");
    Expect(CountKind(Tokens, Iris::TokenKind::OpenBrace) == 1,
           "escaped quote doesn't terminate the string literal early, so the brace after it stays inside it");
}

void TestCharLiteralVsDigitSeparator() {
    const auto Tokens = TokenizeAll("auto n = 1'000'000; auto c = '{';");
    Expect(CountKind(Tokens, Iris::TokenKind::CharLiteral) == 1,
           "digit separators aren't mistaken for char-literal starts, but a real char literal still is");
    Expect(Contains(Tokens, Iris::TokenKind::CharLiteral, "'{'"),
           "the real char literal is lexed correctly, brace and all, and doesn't affect brace balancing");
}

void TestSourceLocationTracking() {
    // "a\nb" tokenizes as three tokens: identifier "a", an Other token for
    // the newline itself (starting on line 1, since a token's location is
    // where it starts, before the newline it contains gets consumed), then
    // identifier "b" on line 2.
    Iris::CppTokenizer Tokenizer("a\nb", "loc_test.iris");
    const Iris::Token  First = Tokenizer.NextToken();
    const Iris::Token  Whitespace = Tokenizer.NextToken();
    const Iris::Token  Third = Tokenizer.NextToken();
    Expect(First.Location.Line == 1 && First.Location.FilePath == "loc_test.iris",
           "first token is on line 1 with the right file path");
    Expect(Whitespace.Kind == Iris::TokenKind::Other && Whitespace.Lexeme == "\n",
           "the newline between identifiers is its own Other token");
    Expect(Third.Location.Line == 2, "the identifier after the newline is reported on line 2");
}

void TestEndOfFileIsStableOnRepeatedCalls() {
    Iris::CppTokenizer Tokenizer("", "empty.iris");
    const Iris::Token  First = Tokenizer.NextToken();
    const Iris::Token  Second = Tokenizer.NextToken();
    Expect(First.Kind == Iris::TokenKind::EndOfFile && Second.Kind == Iris::TokenKind::EndOfFile,
           "repeated NextToken() calls past the end keep returning EndOfFile rather than misbehaving");
}

} // namespace

void RunRenderBlockParserTests(); // tests/RenderBlockParserTests.cpp — no auto-registration, see CLAUDE.md
void RunIrisConfigTests();        // tests/IrisConfigTests.cpp — no auto-registration, see CLAUDE.md
void RunImportResolverTests();    // tests/ImportResolverTests.cpp — no auto-registration, see CLAUDE.md
void RunCodegenTests();           // tests/CodegenTests.cpp — no auto-registration, see CLAUDE.md
void RunIrisComponentTests();     // tests/IrisComponentTests.cpp — no auto-registration, see CLAUDE.md
void RunSemanticValidatorTests(); // tests/SemanticValidatorTests.cpp — no auto-registration, see CLAUDE.md
void RunDriverTests();            // tests/DriverTests.cpp — no auto-registration, see CLAUDE.md

int main() {
    TestBasicRenderBlockDetection();
    TestBraceInsideStringLiteralDoesNotDesyncBalancing();
    TestBraceInsideLineCommentDoesNotDesyncBalancing();
    TestBraceInsideBlockCommentDoesNotDesyncBalancing();
    TestEscapedQuoteInsideStringLiteral();
    TestCharLiteralVsDigitSeparator();
    TestSourceLocationTracking();
    TestEndOfFileIsStableOnRepeatedCalls();

    RunRenderBlockParserTests();
    RunIrisConfigTests();
    RunImportResolverTests();
    RunCodegenTests();
    RunIrisComponentTests();
    RunSemanticValidatorTests();
    RunDriverTests();

    std::printf("\n%d failure(s)\n", Failures);
    return Failures == 0 ? 0 : 1;
}
