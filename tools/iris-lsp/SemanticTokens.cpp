#include "SemanticTokens.h"

#include "Iris/ElementNode.h"

#include <algorithm>

namespace IrisLsp {

namespace {

void WalkElement(const Iris::ElementNode& Node, std::vector<SemanticToken>& Out);

void WalkJsxSegments(const std::vector<Iris::JsxSegment>& Segments, std::vector<SemanticToken>& Out) {
    for (const Iris::JsxSegment& Segment : Segments) {
        if (Segment.Kind == Iris::JsxSegmentKind::Element && Segment.Element) {
            WalkElement(*Segment.Element, Out);
        }
    }
}

void WalkPropValue(const Iris::PropValue& Value, std::vector<SemanticToken>& Out) {
    if (Value.Kind == Iris::PropValueKind::StringLiteral) {
        // Text has already had its surrounding quotes stripped (RenderBlockParser's own
        // ParsePropValue) -- Location points at the opening quote, so the full span is
        // the quotes plus the stripped content.
        Out.push_back(SemanticToken{Value.Location.Line, Value.Location.Column,
                                     static_cast<std::uint32_t>(Value.Text.size() + 2), SemanticTokenType::String});
    } else if (Value.Kind == Iris::PropValueKind::JsxEscapeHatch) {
        WalkJsxSegments(Value.JsxSegments, Out);
    }
    // A plain `{ }` escape hatch's contents are ordinary host-language code -- left to
    // the editor's own C++ highlighting, not this server's business.
}

void WalkElement(const Iris::ElementNode& Node, std::vector<SemanticToken>& Out) {
    // Node.Location is the '<' itself (RenderBlockParser::ParseElementAfterLAngle's own
    // doc comment) -- the tag name starts exactly one column after it, since no
    // whitespace is allowed between '<' and a tag name.
    Iris::SourceLocation TagLocation = Node.Location;
    TagLocation.Column += 1;
    Out.push_back(
        SemanticToken{TagLocation.Line, TagLocation.Column, static_cast<std::uint32_t>(Node.Tag.size()), SemanticTokenType::Type});

    for (const Iris::Prop& P : Node.Props) {
        Out.push_back(
            SemanticToken{P.Location.Line, P.Location.Column, static_cast<std::uint32_t>(P.Name.size()), SemanticTokenType::Property});
        WalkPropValue(P.Value, Out);
    }
    if (Node.Key) {
        // `key` itself carries no token (it's a reserved prop name, stripped before
        // codegen ever sees it -- docs/iris_core_spec.md §2.3) but a `!{ }` key value can
        // still contain nested elements worth walking.
        WalkPropValue(*Node.Key, Out);
    }

    for (const Iris::ElementChild& Child : Node.Children) {
        if (Child.Kind == Iris::ElementChildKind::Element && Child.Element) {
            WalkElement(*Child.Element, Out);
        } else if (Child.Kind == Iris::ElementChildKind::EscapeHatch && Child.EscapeHatch) {
            WalkPropValue(*Child.EscapeHatch, Out);
        }
    }

    // A self-closing element (`<Frame />`) never calls ParseChildren at all, so
    // ClosingTagLocation stays nullopt for it -- correctly, since `/>` has no separate
    // tag name to highlight. Length uses Node.Tag's own size rather than the closing
    // tag's actual text (RenderBlockParser doesn't retain that once it's compared against
    // Tag) -- exactly right for a matching `</Frame>`, the overwhelming common case; a
    // mismatched or mid-edit closing tag (already a parse-error state) may get a token
    // spanning slightly more or less than its real identifier, same class of accepted
    // imprecision as this whole file's other text-based heuristics.
    if (Node.ClosingTagLocation) {
        Out.push_back(SemanticToken{Node.ClosingTagLocation->Line, Node.ClosingTagLocation->Column,
                                     static_cast<std::uint32_t>(Node.Tag.size()), SemanticTokenType::Type});
    }
}

} // namespace

std::vector<SemanticToken>
CollectRenderBlockSemanticTokens(const std::vector<Iris::RenderBlockParser::ParsedBlock>& Blocks) {
    std::vector<SemanticToken> Tokens;
    for (const Iris::RenderBlockParser::ParsedBlock& Block : Blocks) {
        WalkElement(Block.Root, Tokens);
    }
    std::sort(Tokens.begin(), Tokens.end(), [](const SemanticToken& A, const SemanticToken& B) {
        return A.Line != B.Line ? A.Line < B.Line : A.Column < B.Column;
    });
    return Tokens;
}

} // namespace IrisLsp
