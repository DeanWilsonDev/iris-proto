#pragma once

#include "Iris/SourceLocation.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Iris {

// A prop's (or `<Slot>` child's) value, per docs/iris_core_spec.md §1.4: either
// a string literal or a `{ }` escape hatch. `Text` is the raw inner text with
// the surrounding quotes/braces already stripped — for an escape hatch this is
// verbatim host-language source, captured character-for-character from the
// original file and never parsed or validated; that's the host compiler's job
// once the rewritten file reaches it.
enum class PropValueKind {
    StringLiteral,
    EscapeHatch,
};

struct PropValue {
    PropValueKind  Kind{PropValueKind::StringLiteral};
    std::string    Text;
    SourceLocation Location;
};

struct Prop {
    std::string Name;
    PropValue   Value;
};

struct ElementNode;

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
// entry in `Props`.
struct ElementNode {
    std::string                Tag;
    std::vector<Prop>          Props;
    std::optional<PropValue>   Key;
    std::vector<ElementChild>  Children;
    SourceLocation              Location;
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
