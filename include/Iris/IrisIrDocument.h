#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace Amanuensis {
struct Value;
} // namespace Amanuensis

namespace Iris {

// The read-side counterpart of `IrisIr.cpp`'s `BuildIrisIr` -- deserializes an already-parsed
// `.iris.ir` JSON document (`Amanuensis::Value`, e.g. from `Amanuensis::Reader::ParseFile`)
// back into a typed C++ tree matching chaos-ir-spec.md §3's schema exactly, field for field.
// This is the Iris IR runtime's own parser AST, analogous to `ElementNode.h`'s role for the
// `.iris`/`.irisx` *source* grammar -- but built from JSON, not lexed from source text, since
// by the time this runs the original `.iris`/`.irisx` file may not even be present (the whole
// point of shipping `.iris.ir` as a build artifact). `IrisIrRuntime.h` is what walks this tree
// into a live `Iris::Component`; this file only deserializes it.

// chaos-ir-spec.md §2's `SourceLocation` node, plus its `length` field (which
// `Iris::SourceLocation`, used elsewhere for `#line`-directive purposes only, never needed to
// track -- so it isn't reused here as a base; every field below is a direct 1:1 mirror of the
// JSON schema instead).
struct IrSourceLocation {
    std::string   FilePath;
    std::uint32_t Line{1};
    std::uint32_t Column{1};
    std::size_t   Length{0};
};

struct IrElementNode;

// One ordered piece of a nyx_expression's body (chaos-ir-spec.md §3.7) -- either a raw text
// run or a nested element, in original interleaved order. A `!{ }` JSX-transform escape
// hatch's text and embedded `<Tag>` runs can alternate arbitrarily (e.g. a ternary's
// `cond ?` / element / `:` / element) -- an earlier version of this schema flushed text and
// element segments into two separate fields (`source`/`children`), silently discarding which
// text ran before/between/after which element. That broke real `<Slot>` evaluation for
// exactly the conditional-rendering pattern chaos-ir-spec.md's own worked example uses (see
// docs/iris_nyx_evaluator_scope_gap.md's finding). `Segments` preserves the original order
// directly instead.
enum class IrNyxExpressionSegmentKind { Text, Element };

struct IrNyxExpressionSegment {
    IrNyxExpressionSegmentKind     Kind{IrNyxExpressionSegmentKind::Text};
    std::string                    Text;    // meaningful when Kind == Text
    std::shared_ptr<IrElementNode> Element; // meaningful when Kind == Element
};

// chaos-ir-spec.md §3.7's "nyx_expression" node -- an escape hatch's raw, unparsed Nyx source
// text, plus (only when a `!{ }` JSX-transform escape hatch's body contained embedded `<Tag>`
// runs) the element tree nodes recursively parsed out of it, in original interleaved order.
struct IrNyxExpressionNode {
    std::vector<IrNyxExpressionSegment> Segments;
    IrSourceLocation                     Location;

    // The concatenated text-only view: every Text segment's own text, in order, Element
    // segments contributing nothing. Exact for the common case -- a plain `{ }` escape hatch
    // (never containing embedded JSX) always produces exactly one Text segment, so this
    // equals that segment's own text precisely. `EvaluateProp`/`EvaluateText`
    // (`IrisIrRuntime.h`) only ever need this view. For a JsxEscapeHatch with embedded
    // elements, this is a lossy approximation (real branch-selection logic needs `Segments`
    // directly, not this) -- the same shape the old `Source` field always was, kept only for
    // convenience/debugging.
    std::string Source() const {
        std::string Result;
        for (const IrNyxExpressionSegment& Seg : Segments) {
            if (Seg.Kind == IrNyxExpressionSegmentKind::Text) {
                Result += Seg.Text;
            }
        }
        return Result;
    }

    // The element-only view, in order -- what every pre-fix caller (`ConvertSlot`'s mock
    // `Convert` callback, tests) used before `Segments` existed. Order among elements
    // themselves is preserved even though the interleaved text positions are dropped here.
    std::vector<IrElementNode> Elements() const;
};

// chaos-ir-spec.md §3.6's "literal" value node.
struct IrLiteralValue {
    std::string       Value;
    IrSourceLocation  Location;
};

// A `PropNode`/`key`/`ref` value: either a `literal` or a `nyx_expression` (chaos-ir-spec.md
// §3.5, §3.6) -- mirrors `Iris::PropValue`'s own Kind-selected-payload shape from
// `ElementNode.h`, one level removed (JSON-sourced, not parsed from source text).
struct IrPropValue {
    bool                  IsLiteral{true};
    IrLiteralValue        Literal;    // meaningful when IsLiteral
    IrNyxExpressionNode   Expression; // meaningful when !IsLiteral
};

// chaos-ir-spec.md §3.6's "prop" node.
struct IrPropNode {
    std::string      Name;
    IrPropValue      Value;
    IrSourceLocation Location;
};

// chaos-ir-spec.md §3.5a's "text" node -- a literal-text child *position*, distinct from
// `IrLiteralValue` (a prop's *value*, never a child slot).
struct IrTextNode {
    std::string      Value;
    IrSourceLocation Location;
};

// A child position inside an `element` node -- chaos-ir-spec.md §3.5's
// `(ElementNode | NyxExpressionNode | TextNode)[]` children union.
enum class IrElementChildKind { Element, NyxExpression, Text };

struct IrElementChild {
    IrElementChildKind                    Kind{IrElementChildKind::Text};
    // `shared_ptr`, not `unique_ptr`: an IR tree is immutable once parsed, and
    // `IrisIrRuntime.cpp`'s `<Slot>` wiring needs to retain a node (its own escape-hatch
    // child) for the whole mounted lifetime of the `Component` built from it -- far past
    // when the `IrisIrDocument` this tree came from might itself go out of scope. Sharing
    // ownership this way is a cheap refcount bump; `unique_ptr` would force a deep copy
    // instead (and one `IrElementNode`/`IrNyxExpressionNode`'s own recursive children make
    // that copy non-trivial), for no benefit since nothing here ever mutates a node in place.
    std::shared_ptr<IrElementNode>        Element;    // Kind == Element
    std::shared_ptr<IrNyxExpressionNode>  Expression; // Kind == NyxExpression
    IrTextNode                            Text;       // Kind == Text
};

// chaos-ir-spec.md §3.5's "element" node.
struct IrElementNode {
    std::string                    Tag;
    std::optional<IrPropValue>     Key;
    std::optional<IrPropValue>     Ref;
    std::vector<IrPropNode>        Props;
    std::vector<IrElementChild>    Children;
    IrSourceLocation                Location;
};

// chaos-ir-spec.md §3.4's "render_block" node.
struct IrRenderBlockNode {
    IrElementNode    Root;
    IrSourceLocation Location;
    IrSourceLocation EndLocation;
};

// chaos-ir-spec.md §3.3's "nyx_source" node.
struct IrNyxSourceNode {
    std::string      Source;
    IrSourceLocation Location;
};

// chaos-ir-spec.md §3.1's `body` entry union: alternating `nyx_source` and `render_block`.
using IrBodyNode = std::variant<IrNyxSourceNode, IrRenderBlockNode>;

// chaos-ir-spec.md §3.2's "import" node.
struct IrImportNode {
    std::string      Name;
    std::string      ResolvedPath;
    IrSourceLocation Location;
};

// chaos-ir-spec.md §3.1's top-level document.
struct IrisIrDocument {
    std::string                 Version;
    std::string                 SourceFile;
    std::string                 HostLanguage;
    std::vector<IrImportNode>   Imports;
    std::vector<IrBodyNode>     Body;
};

// A deserialization-level problem -- a required field missing, or of the wrong JSON type. Not
// expected in practice (the IR is a trusted, self-produced build artifact, `IrisIr.cpp`'s own
// output), but reported rather than asserted since this function's whole job is reading a file
// off disk, which can always be stale, hand-edited, or from a mismatched schema version.
struct IrisIrDocumentError {
    std::string Message;
};

struct IrisIrDocumentParseResult {
    std::optional<IrisIrDocument>       Document;
    std::vector<IrisIrDocumentError>    Errors;
};

// Deserializes `Root` (an already-parsed `.iris.ir` JSON document, e.g.
// `Amanuensis::Reader::ParseFile(path).value`) into an `IrisIrDocument`. `Document` is
// populated even when `Errors` is non-empty, on a best-effort basis (a malformed sub-node is
// skipped/defaulted rather than aborting the whole parse) -- matching `RenderBlockParser::
// Result`'s own "partial result plus errors" shape.
IrisIrDocumentParseResult ParseIrisIrDocument(const Amanuensis::Value& Root);

} // namespace Iris
