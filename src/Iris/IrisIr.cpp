#include "Iris/IrisIr.h"

#include <amanuensis/json.hpp>

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace Iris {

namespace {

// Converts a 1-based (Line, Column) into a byte offset into Source -- same technique
// Driver.cpp's own (separate, anonymous-namespace-private) LocationToOffset uses for the
// `.iris` splicing path. Duplicated rather than shared: both are tiny, self-contained, and
// each file already keeps its own small positional helpers this way (CppTokenizer.cpp/
// NyxTokenizer.cpp do the same for their own lexical scanning).
std::size_t LocationToOffset(std::string_view Source, const SourceLocation& Loc) {
    std::size_t   Offset = 0;
    std::uint32_t Line = 1;
    std::uint32_t Column = 1;
    while (Offset < Source.size() && (Line < Loc.Line || (Line == Loc.Line && Column < Loc.Column))) {
        if (Source[Offset] == '\n') {
            ++Line;
            Column = 1;
        } else {
            ++Column;
        }
        ++Offset;
    }
    return Offset;
}

// The byte offset one past the end of an `import Name` statement, given where `import`
// itself starts -- identical logic to Driver.cpp's own ImportStatementEndOffset (see that
// function's own comment for the accepted "comment between import and Name" limitation).
std::size_t ImportStatementEndOffset(std::string_view Source, std::size_t ImportKeywordOffset,
                                      std::string_view Name) {
    std::size_t Offset = ImportKeywordOffset + 6; // strlen("import")
    while (Offset < Source.size() && std::isspace(static_cast<unsigned char>(Source[Offset])) != 0) {
        ++Offset;
    }
    return Offset + Name.size();
}

// Walks forward from (Start, StartOffset) to EndOffset, tracking line/column as it goes --
// used only for the (typically short) span of one `import Name` statement, whose end
// line/column isn't already known the way a ParsedBlock's own EndLocation is.
SourceLocation AdvanceLocation(std::string_view Source, const SourceLocation& Start, std::size_t StartOffset,
                                std::size_t EndOffset) {
    std::uint32_t Line = Start.Line;
    std::uint32_t Column = Start.Column;
    for (std::size_t Offset = StartOffset; Offset < EndOffset; ++Offset) {
        if (Source[Offset] == '\n') {
            ++Line;
            Column = 1;
        } else {
            ++Column;
        }
    }
    return SourceLocation{Start.FilePath, Line, Column};
}

Amanuensis::Value StringValue(std::string Text) { return Amanuensis::Value{std::move(Text)}; }

Amanuensis::Value IntValue(std::size_t N) { return Amanuensis::Value{static_cast<long long>(N)}; }

// chaos-ir-spec.md §2: every IR node carries a SourceLocation with an explicit byte
// `length`, which `Iris::SourceLocation` itself doesn't track (it only ever needed
// Line/Column, for `#line` directives -- docs/iris_core_spec.md §6). Where the parser
// tracks a node's own text directly (a Tag, a Name, literal/expression Text), Length is
// exact; where it only tracks a *start* position for a wider span the spec describes (a
// PropNode's full `name=value`, a literal's surrounding quotes, an escape hatch's
// surrounding braces), Length is a documented best-effort approximation, not a byte-exact
// scan of the original source -- see each call site below.
Amanuensis::Value LocationValue(const SourceLocation& Loc, std::size_t Length) {
    Amanuensis::Value Node = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Node, "file", StringValue(Loc.FilePath));
    Amanuensis::Json::Insert(Node, "line", IntValue(Loc.Line));
    Amanuensis::Json::Insert(Node, "column", IntValue(Loc.Column));
    Amanuensis::Json::Insert(Node, "length", IntValue(Length));
    return Node;
}

// chaos-ir-spec.md §3.6's "literal" value node, for a PropValue (including a `key`/`ref`
// value, §3.5) whose Kind is StringLiteral. Its Text has its surrounding quotes already
// stripped (ElementNode.h's own doc comment), so Length pads them back in as a documented
// approximation, not a byte-exact scan.
Amanuensis::Value LiteralValue(const std::string& Text, const SourceLocation& Location) {
    Amanuensis::Value Node = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Node, "kind", StringValue("literal"));
    Amanuensis::Json::Insert(Node, "value", StringValue(Text));
    Amanuensis::Json::Insert(Node, "location", LocationValue(Location, Text.size() + 2));
    return Node;
}

// chaos-ir-spec.md §3.5a's "text" node, for a literal-text element child (`<Text>Hello
// </Text>`'s `Hello`) -- a child *position*, distinct from a PropNode's own "literal" value
// (LiteralValue above). `Location` is that text run's own real start position, with no
// surrounding-quote padding (a text child has none in source).
Amanuensis::Value TextNodeValue(const std::string& Text, const SourceLocation& Location) {
    Amanuensis::Value Node = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Node, "kind", StringValue("text"));
    Amanuensis::Json::Insert(Node, "value", StringValue(Text));
    Amanuensis::Json::Insert(Node, "location", LocationValue(Location, Text.size()));
    return Node;
}

Amanuensis::Value SerializeElement(const ElementNode& Node);

// chaos-ir-spec.md §3.7's "nyx_expression" node, for a PropValue whose Kind is EscapeHatch
// or JsxEscapeHatch. A plain EscapeHatch's Text is copied verbatim into `source` (already
// opaque, unparsed Nyx text -- same treatment Codegen.cpp's EmitEscapeHatchExpression gives
// it for `.iris`). A JsxEscapeHatch's JsxSegments (ElementNode.h) map directly onto the
// spec's own source/children split: RawText segments concatenate into `source`, Element
// segments become `children` -- the same reconstruction ComponentEmitter::
// EmitEscapeHatchExpression already does when splicing C++ text, just building JSON instead
// of a string. Interleaving order between text and element segments is lost either way,
// matching the spec's own two-separate-fields shape (chaos-ir-spec.md §3.7's own worked
// example: `source: "() -> isHovered"`, `children: [...]`, not one ordered stream).
Amanuensis::Value SerializePropValue(const PropValue& Value) {
    if (Value.Kind == PropValueKind::StringLiteral) {
        return LiteralValue(Value.Text, Value.Location);
    }

    std::string        Source;
    Amanuensis::Value Children = Amanuensis::Json::MakeArray();
    if (Value.Kind == PropValueKind::JsxEscapeHatch) {
        for (const JsxSegment& Segment : Value.JsxSegments) {
            if (Segment.Kind == JsxSegmentKind::RawText) {
                Source += Segment.Text;
            } else {
                Amanuensis::Json::PushBack(Children, SerializeElement(*Segment.Element));
            }
        }
    } else {
        Source = Value.Text;
    }

    Amanuensis::Value Node = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Node, "kind", StringValue("nyx_expression"));
    Amanuensis::Json::Insert(Node, "source", StringValue(Source));
    Amanuensis::Json::Insert(Node, "children", std::move(Children));
    // +2: the surrounding `{ }` (or `!{ }`, undercounted by one for the JSX-transform
    // marker) ElementNode.h's own doc comment says are already stripped from Text/
    // JsxSegments -- an approximation, not a scan of the original source.
    Amanuensis::Json::Insert(Node, "location", LocationValue(Value.Location, Source.size() + 2));
    return Node;
}

// chaos-ir-spec.md §3.6's "prop" node. `P.Location` is where the prop *name* starts, not
// the full `name=value` span §3.6's own worked example measures (`length: 15` for
// `class="button"`) -- RenderBlockParser doesn't track that wider span today (only where
// the name and the value each individually start), so Length here is Name's own length
// only, a documented approximation rather than a byte-exact source scan.
Amanuensis::Value SerializeProp(const Prop& P) {
    Amanuensis::Value Node = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Node, "kind", StringValue("prop"));
    Amanuensis::Json::Insert(Node, "name", StringValue(P.Name));
    Amanuensis::Json::Insert(Node, "value", SerializePropValue(P.Value));
    Amanuensis::Json::Insert(Node, "location", LocationValue(P.Location, P.Name.size()));
    return Node;
}

// A child position inside an element -- chaos-ir-spec.md §3.5's
// `(ElementNode | NyxExpressionNode | TextNode)[]` children union, one arm per
// ElementChildKind. `Child.Location` is that text run's own real start position
// (ElementNode.h's `ElementChild::Location`, threaded through by
// `RenderBlockParser::ParseChildren`).
Amanuensis::Value SerializeElementChild(const ElementChild& Child) {
    switch (Child.Kind) {
        case ElementChildKind::Element:
            return SerializeElement(*Child.Element);
        case ElementChildKind::EscapeHatch:
            return SerializePropValue(*Child.EscapeHatch);
        case ElementChildKind::Text:
        default:
            return TextNodeValue(Child.Text, Child.Location);
    }
}

// chaos-ir-spec.md §3.5's "element" node. `Node.Location` is where the opening tag starts
// (RenderBlockParser's own convention); Length = Tag's own length matches the spec's own
// worked example exactly (`"Frame"` -> `length: 5`). `ClosingTagLocation` carries no IR
// meaning (not part of §3.5's schema) and is deliberately not serialized. `key`/`ref`
// (§3.5) are each a dedicated field sharing PropNode's own value shape (SerializePropValue,
// since either can be a dynamic Nyx expression, not just a string literal) -- omitted
// entirely, not written as `null`, when the element carries no key/ref.
Amanuensis::Value SerializeElement(const ElementNode& Node) {
    Amanuensis::Value Obj = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Obj, "kind", StringValue("element"));
    Amanuensis::Json::Insert(Obj, "tag", StringValue(Node.Tag));
    if (Node.Key.has_value()) {
        Amanuensis::Json::Insert(Obj, "key", SerializePropValue(*Node.Key));
    }
    if (Node.Ref.has_value()) {
        Amanuensis::Json::Insert(Obj, "ref", SerializePropValue(*Node.Ref));
    }

    Amanuensis::Value Props = Amanuensis::Json::MakeArray();
    for (const Prop& P : Node.Props) {
        Amanuensis::Json::PushBack(Props, SerializeProp(P));
    }
    Amanuensis::Json::Insert(Obj, "props", std::move(Props));

    Amanuensis::Value Children = Amanuensis::Json::MakeArray();
    for (const ElementChild& Child : Node.Children) {
        Amanuensis::Json::PushBack(Children, SerializeElementChild(Child));
    }
    Amanuensis::Json::Insert(Obj, "children", std::move(Children));

    Amanuensis::Json::Insert(Obj, "location", LocationValue(Node.Location, Node.Tag.size()));
    return Obj;
}

// chaos-ir-spec.md §3.4's "render_block" node. `render` and the closing `}` are each a
// fixed, known-length token -- 6 and 1 bytes respectively -- so Length needs no
// approximation here, unlike most other node kinds above.
Amanuensis::Value SerializeRenderBlock(const RenderBlockParser::ParsedBlock& Block) {
    Amanuensis::Value Node = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Node, "kind", StringValue("render_block"));
    Amanuensis::Json::Insert(Node, "root", SerializeElement(Block.Root));
    Amanuensis::Json::Insert(Node, "location", LocationValue(Block.Location, 6));    // strlen("render")
    Amanuensis::Json::Insert(Node, "endLocation", LocationValue(Block.EndLocation, 1)); // the closing '}'
    return Node;
}

// chaos-ir-spec.md §3.2's "import" node. Both offsets (and therefore Length) are exact --
// the same ImportStatementEndOffset logic Driver.cpp's own `.iris` splicing path already
// uses to know precisely where an `import Name` statement ends.
Amanuensis::Value SerializeImport(const ImportStatement& Import, const std::string& ResolvedPath,
                                   std::string_view Source) {
    const std::size_t StartOffset = LocationToOffset(Source, Import.Location);
    const std::size_t EndOffset = ImportStatementEndOffset(Source, StartOffset, Import.Name);

    Amanuensis::Value Node = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Node, "kind", StringValue("import"));
    Amanuensis::Json::Insert(Node, "name", StringValue(Import.Name));
    Amanuensis::Json::Insert(Node, "resolvedPath", StringValue(ResolvedPath));
    Amanuensis::Json::Insert(Node, "location", LocationValue(Import.Location, EndOffset - StartOffset));
    return Node;
}

// One region of the file that becomes a body entry: either an `import` statement (excluded
// from `body` entirely -- it's reported separately in the top-level `imports` array, per
// chaos-ir-spec.md §3.1/§4's own worked example, where the import line never reappears
// inside `body`'s nyx_source text) or a `render { }` block. Sorted by start offset so
// `BuildIrisIr` can walk the file once, left to right, alternating "gap of nyx_source" with
// "cut" the same way Driver.cpp's own Edit-sorting does for its splice-based `.iris` output.
struct Cut {
    std::size_t    StartOffset;
    std::size_t    EndOffset;
    SourceLocation EndLocation;
    bool           IsImport;
    std::size_t    Index; // into Imports or ParseResult.Blocks, matching IsImport
};

} // namespace

Amanuensis::Value BuildIrisIr(std::string_view Source, const std::string& FilePath,
                                const std::vector<ImportStatement>& Imports,
                                const std::vector<ResolvedImport>& ResolvedImportsList,
                                const RenderBlockParser::Result& ParseResult) {
    std::unordered_map<std::string, std::string> ResolvedPathByName;
    for (const ResolvedImport& R : ResolvedImportsList) {
        ResolvedPathByName[R.Name] = R.ResolvedPath;
    }

    std::vector<Cut> Cuts;
    Cuts.reserve(Imports.size() + ParseResult.Blocks.size());
    for (std::size_t Index = 0; Index < Imports.size(); ++Index) {
        const ImportStatement& Import = Imports[Index];
        const std::size_t      StartOffset = LocationToOffset(Source, Import.Location);
        const std::size_t      EndOffset = ImportStatementEndOffset(Source, StartOffset, Import.Name);
        Cuts.push_back(Cut{StartOffset, EndOffset, AdvanceLocation(Source, Import.Location, StartOffset, EndOffset),
                            true, Index});
    }
    for (std::size_t Index = 0; Index < ParseResult.Blocks.size(); ++Index) {
        const RenderBlockParser::ParsedBlock& Block = ParseResult.Blocks[Index];
        const std::size_t                      StartOffset = LocationToOffset(Source, Block.Location);
        const std::size_t                      EndOffset = LocationToOffset(Source, Block.EndLocation);
        Cuts.push_back(Cut{StartOffset, EndOffset, Block.EndLocation, false, Index});
    }
    std::sort(Cuts.begin(), Cuts.end(), [](const Cut& A, const Cut& B) { return A.StartOffset < B.StartOffset; });

    Amanuensis::Value Body = Amanuensis::Json::MakeArray();
    std::size_t         CursorOffset = 0;
    SourceLocation       CursorLocation{FilePath, 1, 1};

    auto EmitGapUpTo = [&](std::size_t UpToOffset) {
        if (UpToOffset <= CursorOffset) {
            return;
        }
        std::string GapText(Source.substr(CursorOffset, UpToOffset - CursorOffset));
        Amanuensis::Value Node = Amanuensis::Json::MakeObject();
        Amanuensis::Json::Insert(Node, "kind", StringValue("nyx_source"));
        Amanuensis::Json::Insert(Node, "source", StringValue(GapText));
        Amanuensis::Json::Insert(Node, "location", LocationValue(CursorLocation, GapText.size()));
        Amanuensis::Json::PushBack(Body, std::move(Node));
    };

    for (const Cut& C : Cuts) {
        EmitGapUpTo(C.StartOffset);
        if (!C.IsImport) {
            Amanuensis::Json::PushBack(Body, SerializeRenderBlock(ParseResult.Blocks[C.Index]));
        }
        // Imports are never added to `body` -- they're reported once, in the top-level
        // `imports` array below, per chaos-ir-spec.md §3.1's own worked example.
        CursorOffset = C.EndOffset;
        CursorLocation = C.EndLocation;
    }
    EmitGapUpTo(Source.size());

    Amanuensis::Value ImportNodes = Amanuensis::Json::MakeArray();
    for (const ImportStatement& Import : Imports) {
        Amanuensis::Json::PushBack(ImportNodes, SerializeImport(Import, ResolvedPathByName.at(Import.Name), Source));
    }

    Amanuensis::Value Doc = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Doc, "version", StringValue("1.0"));
    Amanuensis::Json::Insert(Doc, "sourceFile", StringValue(FilePath));
    Amanuensis::Json::Insert(Doc, "hostLanguage", StringValue("nyx"));
    Amanuensis::Json::Insert(Doc, "imports", std::move(ImportNodes));
    Amanuensis::Json::Insert(Doc, "body", std::move(Body));
    return Doc;
}

} // namespace Iris
