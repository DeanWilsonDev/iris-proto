#include "Iris/RenderBlockParser.h"

#include <cctype>

namespace Iris {

namespace {

std::string Trim(std::string_view Text) {
    std::size_t Start = 0;
    while (Start < Text.size() && std::isspace(static_cast<unsigned char>(Text[Start]))) {
        ++Start;
    }
    std::size_t End = Text.size();
    while (End > Start && std::isspace(static_cast<unsigned char>(Text[End - 1]))) {
        --End;
    }
    return std::string(Text.substr(Start, End - Start));
}

} // namespace

RenderBlockParser::RenderBlockParser(std::string_view Source, std::string FilePath)
    : Source_(Source), FilePath_(std::move(FilePath)), Tokenizer_(Source_, FilePath_) {}

Token RenderBlockParser::PullRawToken() {
    Token Tok = Tokenizer_.NextToken();
    RawOffset_ += Tok.Lexeme.size();
    return Tok;
}

bool RenderBlockParser::IsPunct(char C) const {
    return Current_.Kind == GKind::Punct && Current_.Text.size() == 1 && Current_.Text[0] == C;
}

bool RenderBlockParser::IsJsxEscapeHatchStart() {
    return IsPunct('!') && PeekNext().Kind == GKind::OpenBrace;
}

RenderBlockParser::GToken RenderBlockParser::PeekNext() {
    if (!Lookahead_.has_value()) {
        const GToken Saved = Current_;
        Advance();
        Lookahead_ = Current_;
        Current_ = Saved;
    }
    return *Lookahead_;
}

void RenderBlockParser::Advance() {
    if (Lookahead_.has_value()) {
        Current_ = *Lookahead_;
        Lookahead_.reset();
        return;
    }
    if (!Pending_.empty()) {
        Current_ = Pending_.front();
        Pending_.pop_front();
        return;
    }

    for (;;) {
        const Token Tok = PullRawToken();

        if (Tok.Kind == TokenKind::EndOfFile) {
            Current_ = GToken{GKind::EndOfFile, "", Tok.Location, PendingWhitespace_};
            PendingWhitespace_ = false;
            return;
        }

        if (Tok.Kind == TokenKind::LineComment || Tok.Kind == TokenKind::BlockComment) {
            // Comments are trivia everywhere inside a render block, per
            // docs/iris_core_spec.md §1.6 — stripped silently, and treated
            // like whitespace for literal-text word-spacing purposes.
            PendingWhitespace_ = true;
            continue;
        }

        if (Tok.Kind == TokenKind::Other) {
            // Split into individual non-whitespace characters: a single
            // Other lexeme can bundle several punctuation marks together
            // with no whitespace between them (e.g. `></` between two
            // adjacent closing tags), since CppTokenizer only breaks a run
            // on identifier/string/char/comment/brace boundaries.
            std::uint32_t Line = Tok.Location.Line;
            std::uint32_t Column = Tok.Location.Column;
            bool           SawWhitespace = PendingWhitespace_;

            for (const char C : Tok.Lexeme) {
                if (C == '\n') {
                    ++Line;
                    Column = 1;
                    SawWhitespace = true;
                    continue;
                }
                if (std::isspace(static_cast<unsigned char>(C))) {
                    ++Column;
                    SawWhitespace = true;
                    continue;
                }
                Pending_.push_back(GToken{GKind::Punct, std::string(1, C),
                                           SourceLocation{Tok.Location.FilePath, Line, Column}, SawWhitespace});
                SawWhitespace = false;
                ++Column;
            }

            PendingWhitespace_ = SawWhitespace; // trailing whitespace carries forward
            if (Pending_.empty()) {
                continue; // the whole Other lexeme was whitespace; keep pulling
            }
            Current_ = Pending_.front();
            Pending_.pop_front();
            return;
        }

        GKind Kind = GKind::EndOfFile;
        switch (Tok.Kind) {
            case TokenKind::Identifier:    Kind = GKind::Identifier; break;
            case TokenKind::StringLiteral: Kind = GKind::StringLiteral; break;
            case TokenKind::CharLiteral:   Kind = GKind::CharLiteral; break;
            case TokenKind::OpenBrace:     Kind = GKind::OpenBrace; break;
            case TokenKind::CloseBrace:    Kind = GKind::CloseBrace; break;
            default:                       Kind = GKind::EndOfFile; break; // unreachable
        }
        Current_ = GToken{Kind, Tok.Lexeme, Tok.Location, PendingWhitespace_};
        PendingWhitespace_ = false;
        return;
    }
}

RenderBlockParser::Result RenderBlockParser::Parse() {
    Advance(); // prime Current_

    while (Current_.Kind != GKind::EndOfFile) {
        if (Current_.Kind == GKind::Identifier && Current_.Text == "render") {
            const SourceLocation BlockLocation = Current_.Location;
            Advance();
            if (Current_.Kind == GKind::OpenBrace) {
                Advance(); // consume the render block's opening '{'
                ParseRenderBlock(BlockLocation);
            }
            // Otherwise `render` wasn't followed by `{` — not actually a
            // render block (e.g. an unrelated identifier) — keep scanning
            // from wherever Current_ already landed.
        } else {
            Advance();
        }
    }

    return Result{std::move(Blocks_), std::move(Errors_)};
}

void RenderBlockParser::ParseRenderBlock(SourceLocation BlockLocation) {
    if (!IsPunct('<')) {
        Errors_.push_back({"`render` must have exactly one root element", Current_.Location});
        RecoverToBlockEnd();
        return;
    }

    ElementNode Root = ParseElement();

    if (Current_.Kind == GKind::CloseBrace) {
        // '}' is always a single character, so one column past its own start is one
        // character past the block's end — no need to peek the next token for this.
        SourceLocation EndLocation = Current_.Location;
        EndLocation.Column += 1;
        Advance(); // consume the render block's own closing '}'
        Blocks_.push_back(ParsedBlock{std::move(Root), BlockLocation, EndLocation});
        return;
    }

    Errors_.push_back({"`render` must have exactly one root element", Current_.Location});
    RecoverToBlockEnd();
}

void RenderBlockParser::RecoverToBlockEnd() {
    int Depth = 1; // already inside the render block's own '{'
    while (Depth > 0 && Current_.Kind != GKind::EndOfFile) {
        if (Current_.Kind == GKind::OpenBrace) {
            ++Depth;
        } else if (Current_.Kind == GKind::CloseBrace) {
            --Depth;
            if (Depth == 0) {
                break;
            }
        }
        Advance();
    }
    if (Current_.Kind == GKind::CloseBrace) {
        Advance(); // consume the render block's own closing '}'
    }
}

ElementNode RenderBlockParser::ParseElement() {
    const SourceLocation LAngleLocation = Current_.Location;
    Advance(); // consume '<'
    return ParseElementAfterLAngle(LAngleLocation);
}

ElementNode RenderBlockParser::ParseElementAfterLAngle(SourceLocation LAngleLocation) {
    ElementNode Node;
    Node.Location = LAngleLocation;

    if (Current_.Kind != GKind::Identifier) {
        Errors_.push_back({"expected a tag name after '<'", Current_.Location});
        return Node;
    }
    Node.Tag = Current_.Text;
    Advance();

    for (;;) {
        if (IsPunct('/')) {
            Advance();
            if (IsPunct('>')) {
                Advance(); // self-closing: no children
                return Node;
            }
            Errors_.push_back({"expected '>' after '/' in self-closing <" + Node.Tag + ">", Current_.Location});
            return Node;
        }
        if (IsPunct('>')) {
            Advance();
            break;
        }
        if (Current_.Kind != GKind::Identifier) {
            Errors_.push_back({"expected a prop name or '>' inside <" + Node.Tag + ">", Current_.Location});
            return Node;
        }

        const std::string    PropName = Current_.Text;
        const SourceLocation PropNameLocation = Current_.Location;
        Advance();
        if (!IsPunct('=')) {
            Errors_.push_back({"expected '=' after prop name '" + PropName + "'", Current_.Location});
            return Node;
        }
        Advance();

        PropValue Value = ParsePropValue();
        if (PropName == "key") {
            Node.Key = std::move(Value);
        } else if (PropName == "ref") {
            Node.Ref = std::move(Value);
        } else {
            Node.Props.push_back(Prop{PropName, std::move(Value), PropNameLocation});
        }
    }

    Node.Children = ParseChildren(Node.Tag, Node.ClosingTagLocation);
    return Node;
}

PropValue RenderBlockParser::ParsePropValue() {
    if (Current_.Kind == GKind::StringLiteral) {
        const SourceLocation Location = Current_.Location;
        const std::string&   Raw = Current_.Text; // includes surrounding quotes
        const std::string    Inner = Raw.size() >= 2 ? Raw.substr(1, Raw.size() - 2) : "";
        Advance();
        PropValue Value;
        Value.Kind = PropValueKind::StringLiteral;
        Value.Text = Inner;
        Value.Location = Location;
        return Value;
    }
    if (IsJsxEscapeHatchStart()) {
        return ParseJsxEscapeHatch();
    }
    if (Current_.Kind == GKind::OpenBrace) {
        return ParseEscapeHatch();
    }

    Errors_.push_back({"expected a string literal or '{' for a prop value", Current_.Location});
    PropValue Value;
    Value.Kind = PropValueKind::EscapeHatch;
    Value.Location = Current_.Location;
    return Value;
}

PropValue RenderBlockParser::ParseEscapeHatch() {
    const SourceLocation Location = Current_.Location;
    const std::size_t    BodyStart = RawOffset_; // Current_ == '{', already consumed by PullRawToken
    int                  Depth = 1;
    std::size_t          BodyEndExclusive = BodyStart;

    for (;;) {
        const Token Tok = PullRawToken();
        if (Tok.Kind == TokenKind::EndOfFile) {
            Errors_.push_back({"unterminated '{' escape hatch", Location});
            BodyEndExclusive = RawOffset_;
            break;
        }
        if (Tok.Kind == TokenKind::OpenBrace) {
            ++Depth;
            continue;
        }
        if (Tok.Kind == TokenKind::CloseBrace) {
            --Depth;
            if (Depth == 0) {
                BodyEndExclusive = RawOffset_ - Tok.Lexeme.size();
                break;
            }
        }
    }

    const std::string Text = Trim(Source_.substr(BodyStart, BodyEndExclusive - BodyStart));
    Advance(); // refill Current_ with whatever follows the escape hatch
    PropValue Value;
    Value.Kind = PropValueKind::EscapeHatch;
    Value.Text = Text;
    Value.Location = Location;
    return Value;
}

// `!{ }` — the JSX-transform escape hatch (docs/archive/iris_next_steps.md, "Resolved: JSX
// inside escape hatches"). Unlike the opaque `{ }` form, this one is walked at the
// structured GToken level rather than scanned as a raw byte span: a `<Identifier`
// run is recursively parsed as a nested element via the ordinary
// ParseElementAfterLAngle path (so its own props/children keep following normal
// rules — `{ }` stays opaque, a nested `!{ }` recurses again), and everything else
// is reconstructed as verbatim host-language text between those element runs. The
// reconstructed text is token-for-token, not byte-for-byte — whitespace is
// normalized the same way literal element text is elsewhere in this parser — which
// is fine because only the JSX runs need transforming; ordinary C++ doesn't care
// about incidental whitespace.
PropValue RenderBlockParser::ParseJsxEscapeHatch() {
    const SourceLocation Location = Current_.Location; // Current_ == '!'
    Advance();                                          // consume '!'
    Advance();                                          // consume '{'

    std::vector<JsxSegment> Segments;
    std::string              TextBuffer;

    auto AppendText = [&](const std::string& Text, bool PrecededByWhitespace) {
        if (!TextBuffer.empty() && PrecededByWhitespace) {
            TextBuffer += ' ';
        }
        TextBuffer += Text;
    };
    auto FlushText = [&]() {
        if (!TextBuffer.empty()) {
            JsxSegment Segment;
            Segment.Kind = JsxSegmentKind::RawText;
            Segment.Text = TextBuffer;
            Segments.push_back(std::move(Segment));
            TextBuffer.clear();
        }
    };

    int Depth = 1; // already inside the '!{' itself
    for (;;) {
        if (Current_.Kind == GKind::EndOfFile) {
            Errors_.push_back({"unterminated '!{' JSX-transform escape hatch", Location});
            FlushText();
            break;
        }

        if (Current_.Kind == GKind::OpenBrace) {
            ++Depth;
            AppendText("{", Current_.PrecededByWhitespace);
            Advance();
            continue;
        }
        if (Current_.Kind == GKind::CloseBrace) {
            --Depth;
            if (Depth == 0) {
                FlushText();
                Advance(); // consume the closing '}'
                break;
            }
            AppendText("}", Current_.PrecededByWhitespace);
            Advance();
            continue;
        }

        // A JSX element start is disambiguated from a template angle bracket
        // (`std::vector<Component>` has exactly the same `< Identifier >`
        // shape as an attribute-less opening tag) by requiring whitespace
        // before the `<` — every JSX use in the spec is written with a space
        // or newline before it (`return <Frame ...`, `push_back(\n <Frame
        // ...`), while a template argument list never has one.
        if (IsPunct('<') && Current_.PrecededByWhitespace && PeekNext().Kind == GKind::Identifier) {
            TextBuffer += ' ';
            FlushText();
            const SourceLocation LAngleLocation = Current_.Location;
            Advance(); // consume '<'
            JsxSegment Segment;
            Segment.Kind = JsxSegmentKind::Element;
            Segment.Element = std::make_unique<ElementNode>(ParseElementAfterLAngle(LAngleLocation));
            Segments.push_back(std::move(Segment));
            continue;
        }

        if (Current_.Kind == GKind::StringLiteral || Current_.Kind == GKind::CharLiteral) {
            AppendText(Current_.Text, Current_.PrecededByWhitespace); // already includes quotes
            Advance();
            continue;
        }

        AppendText(Current_.Text, Current_.PrecededByWhitespace);
        Advance();
    }

    PropValue Value;
    Value.Kind = PropValueKind::JsxEscapeHatch;
    Value.JsxSegments = std::move(Segments);
    Value.Location = Location;
    return Value;
}

std::vector<ElementChild> RenderBlockParser::ParseChildren(const std::string& OpenTag,
                                                             std::optional<SourceLocation>& OutClosingTagLocation) {
    std::vector<ElementChild> Children;
    std::string                TextBuffer;

    auto FlushText = [&]() {
        const std::string Trimmed = Trim(TextBuffer);
        if (!Trimmed.empty()) {
            Children.push_back(ElementChild::MakeText(Trimmed));
        }
        TextBuffer.clear();
    };

    for (;;) {
        if (Current_.Kind == GKind::EndOfFile) {
            Errors_.push_back(
                {"unterminated <" + OpenTag + ">, expected a matching </" + OpenTag + ">", Current_.Location});
            FlushText();
            return Children;
        }

        if (IsPunct('<')) {
            const SourceLocation LAngleLocation = Current_.Location;
            Advance(); // consume '<'
            if (IsPunct('/')) {
                Advance(); // consume '/'
                std::string CloseTag;
                if (Current_.Kind == GKind::Identifier) {
                    CloseTag = Current_.Text;
                    // Captured even when CloseTag ends up not matching OpenTag below --
                    // it's still real source text a caller (e.g. iris-lsp's semantic
                    // tokens) may want a position for, independent of whether the tags
                    // actually match.
                    OutClosingTagLocation = Current_.Location;
                    Advance();
                } else {
                    Errors_.push_back({"expected a closing tag name after '</'", Current_.Location});
                }
                if (IsPunct('>')) {
                    Advance();
                } else {
                    Errors_.push_back({"expected '>' to close </" + CloseTag + ">", Current_.Location});
                }
                if (CloseTag != OpenTag) {
                    Errors_.push_back({"closing tag </" + CloseTag + "> does not match opening tag <" + OpenTag +
                                            ">",
                                        LAngleLocation});
                }
                FlushText();
                return Children;
            }

            FlushText();
            Children.push_back(ElementChild::MakeElement(ParseElementAfterLAngle(LAngleLocation)));
            continue;
        }

        if (IsJsxEscapeHatchStart()) {
            FlushText();
            Children.push_back(ElementChild::MakeEscapeHatch(ParseJsxEscapeHatch()));
            continue;
        }

        if (Current_.Kind == GKind::OpenBrace) {
            FlushText();
            Children.push_back(ElementChild::MakeEscapeHatch(ParseEscapeHatch()));
            continue;
        }

        if (Current_.Kind == GKind::CloseBrace) {
            Errors_.push_back(
                {"unexpected '}' inside <" + OpenTag + ">, expected a matching </" + OpenTag + ">", Current_.Location});
            FlushText();
            return Children;
        }

        // Literal text: an identifier, string/char literal, or a stray
        // punctuation character that isn't `<` or `{`.
        if (!TextBuffer.empty() && Current_.PrecededByWhitespace) {
            TextBuffer += ' ';
        }
        TextBuffer += Current_.Text;
        Advance();
    }
}

} // namespace Iris
