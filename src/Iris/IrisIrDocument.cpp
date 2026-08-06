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

    std::string GetString(const Amanuensis::Value& Obj, const std::string& Key, const char* NodeKind) {
        const Amanuensis::Value* Field = Amanuensis::Json::Find(Obj, Key);
        if (Field == nullptr || !Amanuensis::Json::IsString(*Field)) {
            AddError(std::string(NodeKind) + " node missing required string field '" + Key + "'");
            return {};
        }
        return Amanuensis::Json::AsString(*Field);
    }

    std::size_t GetSize(const Amanuensis::Value& Obj, const std::string& Key, const char* NodeKind) {
        const Amanuensis::Value* Field = Amanuensis::Json::Find(Obj, Key);
        if (Field == nullptr || !Amanuensis::Json::IsNumber(*Field)) {
            AddError(std::string(NodeKind) + " node missing required numeric field '" + Key + "'");
            return 0;
        }
        return static_cast<std::size_t>(Amanuensis::Json::AsInteger(*Field));
    }

    const Amanuensis::Value* GetObject(const Amanuensis::Value& Obj, const std::string& Key, const char* NodeKind) {
        const Amanuensis::Value* Field = Amanuensis::Json::Find(Obj, Key);
        if (Field == nullptr || !Amanuensis::Json::IsObject(*Field)) {
            AddError(std::string(NodeKind) + " node missing required object field '" + Key + "'");
            return nullptr;
        }
        return Field;
    }

    const Amanuensis::Value* GetArray(const Amanuensis::Value& Obj, const std::string& Key, const char* NodeKind) {
        const Amanuensis::Value* Field = Amanuensis::Json::Find(Obj, Key);
        if (Field == nullptr || !Amanuensis::Json::IsArray(*Field)) {
            AddError(std::string(NodeKind) + " node missing required array field '" + Key + "'");
            return nullptr;
        }
        return Field;
    }

    IrSourceLocation ParseLocation(const Amanuensis::Value& Obj) {
        const Amanuensis::Value* Loc = GetObject(Obj, "location", "node");
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

    std::string GetKind(const Amanuensis::Value& Obj) {
        const Amanuensis::Value* Field = Amanuensis::Json::Find(Obj, "kind");
        if (Field == nullptr || !Amanuensis::Json::IsString(*Field)) {
            AddError("node missing required string field 'kind'");
            return {};
        }
        return Amanuensis::Json::AsString(*Field);
    }

    // chaos-ir-spec.md §3.6/§3.7: a "literal" or "nyx_expression" value node -- shared shape
    // for PropNode.value, ElementNode.key, and ElementNode.ref.
    IrPropValue ParsePropValue(const Amanuensis::Value& Obj) {
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

    IrNyxExpressionNode ParseNyxExpression(const Amanuensis::Value& Obj) {
        IrNyxExpressionNode Result;
        Result.Location = ParseLocation(Obj);
        const Amanuensis::Value* Segments = GetArray(Obj, "segments", "nyx_expression");
        if (Segments != nullptr) {
            const std::size_t Count = Amanuensis::Json::Size(*Segments);
            Result.Segments.reserve(Count);
            for (std::size_t Index = 0; Index < Count; ++Index) {
                const Amanuensis::Value& SegObj = Amanuensis::Json::At(*Segments, Index);
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

    IrTextNode ParseTextNode(const Amanuensis::Value& Obj) {
        IrTextNode Result;
        Result.Value = GetString(Obj, "value", "text");
        Result.Location = ParseLocation(Obj);
        return Result;
    }

    IrPropNode ParseProp(const Amanuensis::Value& Obj) {
        IrPropNode Result;
        Result.Name = GetString(Obj, "name", "prop");
        Result.Location = ParseLocation(Obj);
        const Amanuensis::Value* Value = GetObject(Obj, "value", "prop");
        if (Value != nullptr) {
            Result.Value = ParsePropValue(*Value);
        }
        return Result;
    }

    // chaos-ir-spec.md §3.5's children union: an "element", a "nyx_expression", or a "text"
    // node, dispatched on `kind`.
    IrElementChild ParseElementChild(const Amanuensis::Value& Obj) {
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

    IrElementNode ParseElement(const Amanuensis::Value& Obj) {
        IrElementNode Result;
        Result.Tag = GetString(Obj, "tag", "element");
        Result.Location = ParseLocation(Obj);

        if (const Amanuensis::Value* Key = Amanuensis::Json::Find(Obj, "key"); Key != nullptr) {
            Result.Key = ParsePropValue(*Key);
        }
        if (const Amanuensis::Value* Ref = Amanuensis::Json::Find(Obj, "ref"); Ref != nullptr) {
            Result.Ref = ParsePropValue(*Ref);
        }

        if (const Amanuensis::Value* Props = GetArray(Obj, "props", "element"); Props != nullptr) {
            const std::size_t Count = Amanuensis::Json::Size(*Props);
            Result.Props.reserve(Count);
            for (std::size_t Index = 0; Index < Count; ++Index) {
                Result.Props.push_back(ParseProp(Amanuensis::Json::At(*Props, Index)));
            }
        }

        if (const Amanuensis::Value* Children = GetArray(Obj, "children", "element"); Children != nullptr) {
            const std::size_t Count = Amanuensis::Json::Size(*Children);
            Result.Children.reserve(Count);
            for (std::size_t Index = 0; Index < Count; ++Index) {
                Result.Children.push_back(ParseElementChild(Amanuensis::Json::At(*Children, Index)));
            }
        }

        return Result;
    }

    IrRenderBlockNode ParseRenderBlock(const Amanuensis::Value& Obj) {
        IrRenderBlockNode Result;
        Result.Location = ParseLocation(Obj);
        const Amanuensis::Value* Root = GetObject(Obj, "root", "render_block");
        if (Root != nullptr) {
            Result.Root = ParseElement(*Root);
        }
        if (const Amanuensis::Value* End = GetObject(Obj, "endLocation", "render_block"); End != nullptr) {
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

    IrNyxSourceNode ParseNyxSource(const Amanuensis::Value& Obj) {
        IrNyxSourceNode Result;
        Result.Source = GetString(Obj, "source", "nyx_source");
        Result.Location = ParseLocation(Obj);
        return Result;
    }

    IrImportNode ParseImport(const Amanuensis::Value& Obj) {
        IrImportNode Result;
        Result.Name = GetString(Obj, "name", "import");
        Result.ResolvedPath = GetString(Obj, "resolvedPath", "import");
        Result.Location = ParseLocation(Obj);
        return Result;
    }

    std::optional<IrBodyNode> ParseBodyNode(const Amanuensis::Value& Obj) {
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

    IrisIrDocument ParseDocument(const Amanuensis::Value& Obj) {
        IrisIrDocument Result;
        if (!Amanuensis::Json::IsObject(Obj)) {
            AddError("top-level IR document is not a JSON object");
            return Result;
        }
        Result.Version = GetString(Obj, "version", "document");
        Result.SourceFile = GetString(Obj, "sourceFile", "document");
        Result.HostLanguage = GetString(Obj, "hostLanguage", "document");

        if (const Amanuensis::Value* Imports = GetArray(Obj, "imports", "document"); Imports != nullptr) {
            const std::size_t Count = Amanuensis::Json::Size(*Imports);
            Result.Imports.reserve(Count);
            for (std::size_t Index = 0; Index < Count; ++Index) {
                Result.Imports.push_back(ParseImport(Amanuensis::Json::At(*Imports, Index)));
            }
        }

        if (const Amanuensis::Value* Body = GetArray(Obj, "body", "document"); Body != nullptr) {
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

IrisIrDocumentParseResult ParseIrisIrDocument(const Amanuensis::Value& Root) {
    IrisIrDocumentParseResult Result;
    DocumentParser             Parser(Result.Errors);
    Result.Document = Parser.ParseDocument(Root);
    return Result;
}

} // namespace Iris
