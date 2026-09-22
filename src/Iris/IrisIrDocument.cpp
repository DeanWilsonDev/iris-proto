#include "Iris/IrisIrDocument.h"

#include <amanuensis/json.hpp>

namespace Iris {

namespace {

class DocumentParser {
public:
    explicit DocumentParser(std::vector<IrisIrDocumentError>& Errors) : Errors_(Errors) {}

    // --- primitive field accessors -- each records an error and returns a default rather
    // than throwing: a malformed IR file (stale schema version, hand-edited, truncated) is
    // an ordinary reportable condition here, not a programmer error to assert on. ---

    std::string GetString(const Amanuensis::JsonValue& Obj, const std::string& Key, const char* NodeKind) {
        const Amanuensis::JsonValue* Field = Amanuensis::Json::Find(Obj, Key);
        if (Field == nullptr || !Amanuensis::Json::IsString(*Field)) {
            AddError(std::string(NodeKind) + " node missing required string field '" + Key + "'");
            return {};
        }
        return Amanuensis::Json::AsString(*Field);
    }

    std::size_t GetSize(const Amanuensis::JsonValue& Obj, const std::string& Key, const char* NodeKind) {
        const Amanuensis::JsonValue* Field = Amanuensis::Json::Find(Obj, Key);
        if (Field == nullptr || !Amanuensis::Json::IsNumber(*Field)) {
            AddError(std::string(NodeKind) + " node missing required numeric field '" + Key + "'");
            return 0;
        }
        return static_cast<std::size_t>(Amanuensis::Json::AsInteger(*Field));
    }

    const Amanuensis::JsonValue* GetObject(const Amanuensis::JsonValue& Obj, const std::string& Key, const char* NodeKind) {
        const Amanuensis::JsonValue* Field = Amanuensis::Json::Find(Obj, Key);
        if (Field == nullptr || !Amanuensis::Json::IsObject(*Field)) {
            AddError(std::string(NodeKind) + " node missing required object field '" + Key + "'");
            return nullptr;
        }
        return Field;
    }

    const Amanuensis::JsonValue* GetArray(const Amanuensis::JsonValue& Obj, const std::string& Key, const char* NodeKind) {
        const Amanuensis::JsonValue* Field = Amanuensis::Json::Find(Obj, Key);
        if (Field == nullptr || !Amanuensis::Json::IsArray(*Field)) {
            AddError(std::string(NodeKind) + " node missing required array field '" + Key + "'");
            return nullptr;
        }
        return Field;
    }

    IrSourceLocation ParseLocation(const Amanuensis::JsonValue& Obj) {
        const Amanuensis::JsonValue* Loc = GetObject(Obj, "location", "node");
        IrSourceLocation         Result;
        if (Loc == nullptr) {
            return Result;
        }
        Result.FilePath = GetString(*Loc, "file", "location");
        Result.Line = static_cast<std::uint32_t>(GetSize(*Loc, "line", "location"));
        Result.Column = static_cast<std::uint32_t>(GetSize(*Loc, "column", "location"));
        Result.Length = GetSize(*Loc, "length", "location");
        return Result;
    }

    std::string GetKind(const Amanuensis::JsonValue& Obj) {
        const Amanuensis::JsonValue* Field = Amanuensis::Json::Find(Obj, "kind");
        if (Field == nullptr || !Amanuensis::Json::IsString(*Field)) {
            AddError("node missing required string field 'kind'");
            return {};
        }
        return Amanuensis::Json::AsString(*Field);
    }

    // chaos-ir-spec.md §3.6/§3.7: a "literal" or "nyx_expression" value node -- shared shape
    // for PropNode.value, ElementNode.key, and ElementNode.ref.
    IrPropValue ParsePropValue(const Amanuensis::JsonValue& Obj) {
        const std::string Kind = GetKind(Obj);
        IrPropValue        Result;
        if (Kind == "literal") {
            Result.IsLiteral = true;
            Result.Literal.Value = GetString(Obj, "value", "literal");
            Result.Literal.Location = ParseLocation(Obj);
        } else if (Kind == "nyx_expression") {
            Result.IsLiteral = false;
            Result.Expression = ParseNyxExpression(Obj);
        } else {
            AddError("expected a 'literal' or 'nyx_expression' value node, found kind '" + Kind + "'");
        }
        return Result;
    }

    IrNyxExpressionNode ParseNyxExpression(const Amanuensis::JsonValue& Obj) {
        IrNyxExpressionNode Result;
        Result.Location = ParseLocation(Obj);
        const Amanuensis::JsonValue* Segments = GetArray(Obj, "segments", "nyx_expression");
        if (Segments != nullptr) {
            const std::size_t Count = Amanuensis::Json::Size(*Segments);
            Result.Segments.reserve(Count);
            for (std::size_t Index = 0; Index < Count; ++Index) {
                const Amanuensis::JsonValue& SegObj = Amanuensis::Json::At(*Segments, Index);
                const std::string        SegKind = GetKind(SegObj);
                IrNyxExpressionSegment    Seg;
                if (SegKind == "text") {
                    Seg.Kind = IrNyxExpressionSegmentKind::Text;
                    Seg.Text = GetString(SegObj, "value", "nyx_expression segment");
                } else if (SegKind == "element") {
                    Seg.Kind = IrNyxExpressionSegmentKind::Element;
                    Seg.Element = std::make_shared<IrElementNode>(ParseElement(SegObj));
                } else {
                    AddError("expected a 'text' or 'element' nyx_expression segment, found kind '" + SegKind + "'");
                    continue;
                }
                Result.Segments.push_back(std::move(Seg));
            }
        }
        return Result;
    }

    IrTextNode ParseTextNode(const Amanuensis::JsonValue& Obj) {
        IrTextNode Result;
        Result.Value = GetString(Obj, "value", "text");
        Result.Location = ParseLocation(Obj);
        return Result;
    }

    IrPropNode ParseProp(const Amanuensis::JsonValue& Obj) {
        IrPropNode Result;
        Result.Name = GetString(Obj, "name", "prop");
        Result.Location = ParseLocation(Obj);
        const Amanuensis::JsonValue* Value = GetObject(Obj, "value", "prop");
        if (Value != nullptr) {
            Result.Value = ParsePropValue(*Value);
        }
        return Result;
    }

    // chaos-ir-spec.md §3.5's children union: an "element", a "nyx_expression", or a "text"
    // node, dispatched on `kind`.
    IrElementChild ParseElementChild(const Amanuensis::JsonValue& Obj) {
        const std::string Kind = GetKind(Obj);
        IrElementChild     Result;
        if (Kind == "element") {
            Result.Kind = IrElementChildKind::Element;
            Result.Element = std::make_shared<IrElementNode>(ParseElement(Obj));
        } else if (Kind == "nyx_expression") {
            Result.Kind = IrElementChildKind::NyxExpression;
            Result.Expression = std::make_shared<IrNyxExpressionNode>(ParseNyxExpression(Obj));
        } else if (Kind == "text") {
            Result.Kind = IrElementChildKind::Text;
            Result.Text = ParseTextNode(Obj);
        } else {
            AddError("expected an 'element', 'nyx_expression', or 'text' child node, found kind '" + Kind + "'");
        }
        return Result;
    }

    IrElementNode ParseElement(const Amanuensis::JsonValue& Obj) {
        IrElementNode Result;
        Result.Tag = GetString(Obj, "tag", "element");
        Result.Location = ParseLocation(Obj);

        if (const Amanuensis::JsonValue* Key = Amanuensis::Json::Find(Obj, "key"); Key != nullptr) {
            Result.Key = ParsePropValue(*Key);
        }
        if (const Amanuensis::JsonValue* Ref = Amanuensis::Json::Find(Obj, "ref"); Ref != nullptr) {
            Result.Ref = ParsePropValue(*Ref);
        }

        if (const Amanuensis::JsonValue* Props = GetArray(Obj, "props", "element"); Props != nullptr) {
            const std::size_t Count = Amanuensis::Json::Size(*Props);
            Result.Props.reserve(Count);
            for (std::size_t Index = 0; Index < Count; ++Index) {
                Result.Props.push_back(ParseProp(Amanuensis::Json::At(*Props, Index)));
            }
        }

        if (const Amanuensis::JsonValue* Children = GetArray(Obj, "children", "element"); Children != nullptr) {
            const std::size_t Count = Amanuensis::Json::Size(*Children);
            Result.Children.reserve(Count);
            for (std::size_t Index = 0; Index < Count; ++Index) {
                Result.Children.push_back(ParseElementChild(Amanuensis::Json::At(*Children, Index)));
            }
        }

        return Result;
    }

    IrRenderBlockNode ParseRenderBlock(const Amanuensis::JsonValue& Obj) {
        IrRenderBlockNode Result;
        Result.Location = ParseLocation(Obj);
        const Amanuensis::JsonValue* Root = GetObject(Obj, "root", "render_block");
        if (Root != nullptr) {
            Result.Root = ParseElement(*Root);
        }
        if (const Amanuensis::JsonValue* End = GetObject(Obj, "endLocation", "render_block"); End != nullptr) {
            // endLocation is itself a SourceLocation object, not a nested "location" field --
            // reuse the same {file, line, column, length} extraction ParseLocation does for the
            // ordinary "location" field, on this object directly instead of a ".location" child.
            IrSourceLocation EndLoc;
            EndLoc.FilePath = GetString(*End, "file", "render_block.endLocation");
            EndLoc.Line = static_cast<std::uint32_t>(GetSize(*End, "line", "render_block.endLocation"));
            EndLoc.Column = static_cast<std::uint32_t>(GetSize(*End, "column", "render_block.endLocation"));
            EndLoc.Length = GetSize(*End, "length", "render_block.endLocation");
            Result.EndLocation = EndLoc;
        }
        return Result;
    }

    IrNyxSourceNode ParseNyxSource(const Amanuensis::JsonValue& Obj) {
        IrNyxSourceNode Result;
        Result.Source = GetString(Obj, "source", "nyx_source");
        Result.Location = ParseLocation(Obj);
        return Result;
    }

    IrImportNode ParseImport(const Amanuensis::JsonValue& Obj) {
        IrImportNode Result;
        Result.Name = GetString(Obj, "name", "import");
        Result.ResolvedPath = GetString(Obj, "resolvedPath", "import");
        Result.Location = ParseLocation(Obj);
        return Result;
    }

    std::optional<IrBodyNode> ParseBodyNode(const Amanuensis::JsonValue& Obj) {
        const std::string Kind = GetKind(Obj);
        if (Kind == "nyx_source") {
            return IrBodyNode{ParseNyxSource(Obj)};
        }
        if (Kind == "render_block") {
            return IrBodyNode{ParseRenderBlock(Obj)};
        }
        AddError("expected a 'nyx_source' or 'render_block' body node, found kind '" + Kind + "'");
        return std::nullopt;
    }

    IrisIrDocument ParseDocument(const Amanuensis::JsonValue& Obj) {
        IrisIrDocument Result;
        if (!Amanuensis::Json::IsObject(Obj)) {
            AddError("top-level IR document is not a JSON object");
            return Result;
        }
        Result.Version = GetString(Obj, "version", "document");
        Result.SourceFile = GetString(Obj, "sourceFile", "document");
        Result.HostLanguage = GetString(Obj, "hostLanguage", "document");

        if (const Amanuensis::JsonValue* Imports = GetArray(Obj, "imports", "document"); Imports != nullptr) {
            const std::size_t Count = Amanuensis::Json::Size(*Imports);
            Result.Imports.reserve(Count);
            for (std::size_t Index = 0; Index < Count; ++Index) {
                Result.Imports.push_back(ParseImport(Amanuensis::Json::At(*Imports, Index)));
            }
        }

        if (const Amanuensis::JsonValue* Body = GetArray(Obj, "body", "document"); Body != nullptr) {
            const std::size_t Count = Amanuensis::Json::Size(*Body);
            Result.Body.reserve(Count);
            for (std::size_t Index = 0; Index < Count; ++Index) {
                if (std::optional<IrBodyNode> Node = ParseBodyNode(Amanuensis::Json::At(*Body, Index))) {
                    Result.Body.push_back(std::move(*Node));
                }
            }
        }

        return Result;
    }

private:
    void AddError(std::string Message) { Errors_.push_back(IrisIrDocumentError{std::move(Message)}); }

    std::vector<IrisIrDocumentError>& Errors_;
};

} // namespace

std::vector<IrElementNode> IrNyxExpressionNode::Elements() const {
    std::vector<IrElementNode> Result;
    for (const IrNyxExpressionSegment& Seg : Segments) {
        if (Seg.Kind == IrNyxExpressionSegmentKind::Element) {
            Result.push_back(*Seg.Element);
        }
    }
    return Result;
}

IrisIrDocumentParseResult ParseIrisIrDocument(const Amanuensis::JsonValue& Root) {
    IrisIrDocumentParseResult Result;
    DocumentParser             Parser(Result.Errors);
    Result.Document = Parser.ParseDocument(Root);
    return Result;
}

} // namespace Iris
