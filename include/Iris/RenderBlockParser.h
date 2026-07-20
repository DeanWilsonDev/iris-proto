#pragma once

#include "Iris/CppTokenizer.h"
#include "Iris/ElementNode.h"

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Iris {

// Parses every `render { }` block's element-tree grammar (docs/iris_core_spec.md
// §1.3–§1.6) out of a `.iris` source file, on top of the lexical foundation
// CppTokenizer already provides. Non-`render`-block content is scanned over
// but not retained — this produces an AST for each `render` block found, not
// a full passthrough rewrite of the file. Turning that AST into compilable
// `.cpp` (codegen) is a separate, later component: it needs the Iris runtime
// library's concrete `IrisProps` representation decided first, which
// docs/iris_core_spec.md §2.5 deliberately leaves open.
class RenderBlockParser {
public:
    // One parsed `render { }` block.
    struct ParsedBlock {
        ElementNode    Root;
        SourceLocation Location;    // where the `render` keyword itself starts
        SourceLocation EndLocation; // one character past the block's closing '}' —
                                     // a driver splicing codegen output back into the
                                     // original source (docs/iris_next_steps.md) needs
                                     // both ends to know the exact span to replace.
    };

    struct ParseError {
        std::string    Message;
        SourceLocation Location;
    };

    struct Result {
        std::vector<ParsedBlock> Blocks;
        std::vector<ParseError>  Errors;
    };

    // Source must outlive the parser — no copy is taken, matching CppTokenizer.
    RenderBlockParser(std::string_view Source, std::string FilePath);

    Result Parse();

private:
    // A normalized grammar-level token: CppTokenizer's Identifier/StringLiteral/
    // CharLiteral/OpenBrace/CloseBrace pass through unchanged; its Other kind
    // gets split into individual non-whitespace characters (Kind::Punct) so
    // the parser can recognise `<`, `>`, `/`, `=` even when the tokenizer
    // folded several of them together with no whitespace between (e.g. `></`
    // between two adjacent closing tags is a single Other lexeme). Comments
    // and whitespace are trivia at this level and never appear here; whether
    // trivia was skipped just before this token is recorded so literal text
    // runs can re-insert a single space between words instead of gluing them
    // together.
    enum class GKind {
        Identifier,
        StringLiteral,
        CharLiteral,
        OpenBrace,
        CloseBrace,
        Punct,
        EndOfFile,
    };

    struct GToken {
        GKind          Kind{GKind::EndOfFile};
        std::string    Text;
        SourceLocation Location;
        bool           PrecededByWhitespace{false};
    };

    Token   PullRawToken();
    void    Advance(); // fills Current_ with the next GToken
    GToken  PeekNext(); // the GToken after Current_, without consuming it
    bool    IsPunct(char C) const;
    bool    IsJsxEscapeHatchStart(); // Current_ == '!' immediately followed by '{'

    ElementNode              ParseElement();
    ElementNode              ParseElementAfterLAngle(SourceLocation LAngleLocation);
    std::vector<ElementChild> ParseChildren(const std::string& OpenTag,
                                             std::optional<SourceLocation>& OutClosingTagLocation);
    PropValue                ParsePropValue();
    PropValue                ParseEscapeHatch();
    PropValue                ParseJsxEscapeHatch();
    void                     ParseRenderBlock(SourceLocation BlockLocation);
    void                     RecoverToBlockEnd();

    std::string_view Source_;
    std::string       FilePath_;
    CppTokenizer      Tokenizer_;
    std::size_t       RawOffset_{0};

    GToken                 Current_;
    std::deque<GToken>     Pending_;
    bool                   PendingWhitespace_{false};
    std::optional<GToken>  Lookahead_; // one-token peek buffer, ahead of Current_

    std::vector<ParsedBlock> Blocks_;
    std::vector<ParseError>  Errors_;
};

} // namespace Iris
