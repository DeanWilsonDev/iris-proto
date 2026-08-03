#pragma once

#include "Iris/SourceLocation.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Iris {

struct ElementNode;

// One piece of a `!{ }` JSX-transform escape hatch's body (docs/archive/iris_next_steps.md,
// "Resolved: JSX inside escape hatches"): either a run of ordinary host-language
// text (reconstructed token-for-token, not necessarily byte-identical to the
// original source — whitespace is normalized the same way literal element text
// is elsewhere) or a nested `<Tag>` element that was recursively parsed out of
// the escape hatch body and needs its own codegen pass spliced back in.
enum class JsxSegmentKind {
    RawText,
    Element,
};

struct JsxSegment {
    JsxSegmentKind                Kind{JsxSegmentKind::RawText};
    std::string                   Text;    // meaningful when Kind == RawText
    std::unique_ptr<ElementNode>  Element; // meaningful when Kind == Element
};

// A prop's (or `<Slot>` child's) value, per docs/iris_core_spec.md §1.4: a string
// literal, an opaque `{ }` escape hatch, or a `!{ }` JSX-transform escape hatch.
// `Text` is the raw inner text with the surrounding quotes/braces already
// stripped — for an `EscapeHatch` this is verbatim host-language source,
// captured character-for-character from the original file and never parsed or
// validated; that's the host compiler's job once the rewritten file reaches it.
// A `JsxEscapeHatch` instead carries `JsxSegments`: its body was scanned for
// `<Tag>` runs and recursively parsed, so codegen can transform those runs
// too, but `Text` itself is left empty for this kind.
enum class PropValueKind {
    StringLiteral,
    EscapeHatch,
    JsxEscapeHatch,
};

struct PropValue {
    PropValueKind            Kind{PropValueKind::StringLiteral};
    std::string               Text;
    std::vector<JsxSegment>   JsxSegments; // meaningful when Kind == JsxEscapeHatch
    SourceLocation             Location;
};

struct Prop {
    std::string    Name;
    PropValue      Value;
    SourceLocation Location; // where Name itself starts -- distinct from Value.Location
};

// A single child position inside an element (docs/iris_core_spec.md §1.4,
// §3.1): a nested element, a `{ }` escape hatch (event handlers, `<Slot>`'s
// callable, interpolated expressions), or a run of literal text (`<Text>`/
// `<Inline>` accept literal text directly, not just interpolation). Exactly
// one of Element/EscapeHatch/Text is meaningful, selected by Kind — `Element`
// goes through a unique_ptr to break the mutual recursion with ElementNode.
enum class ElementChildKind {
    Element,
    EscapeHatch,
    Text,
};

struct ElementChild {
    ElementChildKind              Kind{ElementChildKind::Text};
    std::unique_ptr<ElementNode>  Element;
    std::optional<PropValue>      EscapeHatch;
    std::string                   Text;

    static ElementChild MakeElement(ElementNode&& Node);
    static ElementChild MakeEscapeHatch(PropValue Value);
    static ElementChild MakeText(std::string Value);
};

// The parsed form of one `<Tag ...>...</Tag>` element inside a `render { }`
// block (docs/iris_core_spec.md §1.4). `Tag` is an ordinary identifier at this
// stage — this parser does not resolve it against Core primitives or
// `import`ed names, that's a semantic pass built on top of this AST, not part
// of it. `key` is pulled out of `Props` into its own field, since per §2.3 it
// is stripped by the preprocessor before codegen and never reaches the
// backend; `class` has no special structural treatment and stays an ordinary
// entry in `Props`. `ref` (docs/next-steps.md, "Named-child-handle (`ref`)
// prop") is pulled out the same way `key` is -- a plain identifying string a
// mount call surfaces back to host code, not a reconciler input, so it must
// never be confused with `Key` downstream (Reconciler.cpp's identity
// matching stays `Key`-only).
struct ElementNode {
    std::string                Tag;
    std::vector<Prop>          Props;
    std::optional<PropValue>   Key;
    std::optional<PropValue>   Ref;
    std::vector<ElementChild>  Children;
    SourceLocation              Location;

    // Where the closing tag's own name starts (`Frame` in `</Frame>`) -- nullopt for a
    // self-closing element (`<Frame />`, which has no separate closing tag at all) or one
    // whose closing tag was malformed enough that no identifier was ever found there
    // (an unterminated element, or `}`/EOF before a `</...>` was reached). Set even when
    // the closing tag's name doesn't match Tag (RenderBlockParser still reports that as
    // an error, but the identifier itself is still real source text worth a position for
    // -- e.g. iris-lsp's semantic tokens).
    std::optional<SourceLocation> ClosingTagLocation;
};

inline ElementChild ElementChild::MakeElement(ElementNode&& Node) {
    ElementChild Child;
    Child.Kind = ElementChildKind::Element;
    Child.Element = std::make_unique<ElementNode>(std::move(Node));
    return Child;
}

inline ElementChild ElementChild::MakeEscapeHatch(PropValue Value) {
    ElementChild Child;
    Child.Kind = ElementChildKind::EscapeHatch;
    Child.EscapeHatch = std::move(Value);
    return Child;
}

inline ElementChild ElementChild::MakeText(std::string Value) {
    ElementChild Child;
    Child.Kind = ElementChildKind::Text;
    Child.Text = std::move(Value);
    return Child;
}

} // namespace Iris
