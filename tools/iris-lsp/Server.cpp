#include "Server.h"

#include "Iris/CorePrimitives.h"
#include "Iris/IrisConfig.h"
#include "JsonRpc.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace IrisLsp {

namespace {

std::string UriToPath(const std::string& Uri) {
    constexpr std::string_view Prefix = "file://";
    return Uri.substr(0, Prefix.size()) == Prefix ? Uri.substr(Prefix.size()) : Uri;
}

std::string PathToUri(const std::string& Path) { return "file://" + Path; }

std::optional<std::string> ReadFile(const std::filesystem::path& Path) {
    std::ifstream Stream(Path, std::ios::binary);
    if (!Stream) {
        return std::nullopt;
    }
    std::ostringstream Buffer;
    Buffer << Stream.rdbuf();
    return Buffer.str();
}

// Same "nearest ancestor directory containing .iris.json" convention tools/IrisCc.cpp
// already uses -- kept as its own small copy here rather than shared, since factoring it
// out would mean adding a new public entry point to libiris for two nine-line callers.
std::optional<std::filesystem::path> FindProjectRoot(std::filesystem::path Start) {
    std::filesystem::path Dir = std::filesystem::absolute(Start);
    for (;;) {
        std::error_code Ignored;
        if (std::filesystem::is_regular_file(Dir / ".iris.json", Ignored)) {
            return Dir;
        }
        if (!Dir.has_parent_path() || Dir.parent_path() == Dir) {
            return std::nullopt;
        }
        Dir = Dir.parent_path();
    }
}

std::string ExtensionOf(const std::string& Path) {
    const std::filesystem::path P(Path);
    return P.extension().string();
}

Amanuensis::Value MakeRange(std::uint32_t Line, std::uint32_t Column) {
    Amanuensis::Value Position = Amanuensis::Value::MakeObject();
    Position.Insert("line", Amanuensis::Value(static_cast<long long>(Line - 1)));
    Position.Insert("character", Amanuensis::Value(static_cast<long long>(Column - 1)));
    Amanuensis::Value Range = Amanuensis::Value::MakeObject();
    Range.Insert("start", Position);
    Range.Insert("end", Position);
    return Range;
}

std::pair<std::uint32_t, std::uint32_t> PositionFromParams(const Amanuensis::Value& Position) {
    return {static_cast<std::uint32_t>(Position.Get("line").AsInteger()) + 1,
            static_cast<std::uint32_t>(Position.Get("character").AsInteger()) + 1};
}

// The line's own text, 1-based -- used only for the render{}-local completion heuristic
// below, never for anything that needs to be exact down to a byte (that's what
// VirtualDocument's segment map is for).
std::string_view LineText(std::string_view Text, std::uint32_t Line) {
    std::uint32_t Current = 1;
    std::size_t   Start = 0;
    for (std::size_t Index = 0; Index <= Text.size(); ++Index) {
        if (Current == Line && Start == 0 && (Index == 0 || Text[Index - 1] == '\n')) {
            Start = Index;
        }
        if (Index == Text.size() || Text[Index] == '\n') {
            if (Current == Line) {
                return Text.substr(Start, Index - Start);
            }
            ++Current;
        }
    }
    return {};
}

enum class RenderCompletionKind { None, TagName, AttributeName };

// Backward scan from the cursor within its own line: the nearest of '<' / '>' decides
// whether the cursor sits inside an still-open start tag at all, and whether whitespace
// was crossed on the way there decides tag-name-position vs attribute-name-position.
// Text-based, not tree-based, deliberately -- RenderBlockParser's own tree is only
// guaranteed well-formed for text that already parses; a cursor mid-edit
// (`<Frame cla|`) usually doesn't, and this heuristic degrades gracefully where a real
// reparse would just fail. docs/iris_lsp_decision.md notes multi-line attribute lists as
// a known gap (this only looks at the cursor's own line).
RenderCompletionKind ClassifyRenderCompletion(std::string_view Line, std::uint32_t ColumnOneBased) {
    const std::size_t CursorOffset = ColumnOneBased > 0 ? static_cast<std::size_t>(ColumnOneBased - 1) : 0;
    bool               SawWhitespace = false;
    std::size_t        Index = std::min(CursorOffset, Line.size());
    while (Index > 0) {
        --Index;
        const char C = Line[Index];
        if (C == '>') {
            return RenderCompletionKind::None;
        }
        if (C == '<') {
            return SawWhitespace ? RenderCompletionKind::AttributeName : RenderCompletionKind::TagName;
        }
        if (std::isspace(static_cast<unsigned char>(C)) != 0) {
            SawWhitespace = true;
        }
    }
    return RenderCompletionKind::None;
}

// A best-effort search for `Name`'s own declaration line in a resolved import target:
// looks for `Name` as a whole word immediately followed (optional whitespace) by `(` --
// matches `Component Name(Props)` without needing to parse the return type, since Iris
// itself never parses component signatures either (docs/iris_core_spec.md §2.1).
std::optional<std::pair<std::uint32_t, std::uint32_t>> FindComponentDeclaration(const std::string& Text,
                                                                                  const std::string& Name) {
    std::size_t Pos = 0;
    while ((Pos = Text.find(Name, Pos)) != std::string::npos) {
        const bool WordStartOk = Pos == 0 || (std::isalnum(static_cast<unsigned char>(Text[Pos - 1])) == 0 &&
                                               Text[Pos - 1] != '_');
        std::size_t After = Pos + Name.size();
        const bool  WordEndOk = After >= Text.size() || (std::isalnum(static_cast<unsigned char>(Text[After])) == 0 &&
                                                          Text[After] != '_');
        if (WordStartOk && WordEndOk) {
            while (After < Text.size() && std::isspace(static_cast<unsigned char>(Text[After])) != 0) {
                ++After;
            }
            if (After < Text.size() && Text[After] == '(') {
                std::uint32_t Line = 1;
                std::uint32_t Column = 1;
                for (std::size_t I = 0; I < Pos; ++I) {
                    if (Text[I] == '\n') {
                        ++Line;
                        Column = 1;
                    } else {
                        ++Column;
                    }
                }
                return std::make_pair(Line, Column);
            }
        }
        Pos += Name.size();
    }
    return std::nullopt;
}

} // namespace

void Server::Run(std::FILE* In, std::FILE* Out) {
    Out_ = Out;
    for (;;) {
        const std::optional<Amanuensis::Value> Message = JsonRpc::ReadMessage(In);
        if (!Message) {
            return;
        }
        const std::string Method =
            Message->IsObject() && Message->Contains("method") ? Message->Get("method").AsString() : std::string{};
        HandleMessage(*Message);
        if (Method == "exit") {
            return;
        }
    }
}

void Server::Reply(const Amanuensis::Value& Id, Amanuensis::Value Result) {
    Amanuensis::Value Message = Amanuensis::Value::MakeObject();
    Message.Insert("jsonrpc", Amanuensis::Value("2.0"));
    Message.Insert("id", Id);
    Message.Insert("result", std::move(Result));
    JsonRpc::WriteMessage(Out_, Message);
}

void Server::ReplyError(const Amanuensis::Value& Id, int Code, const std::string& Message) {
    Amanuensis::Value Error = Amanuensis::Value::MakeObject();
    Error.Insert("code", Amanuensis::Value(Code));
    Error.Insert("message", Amanuensis::Value(Message));
    Amanuensis::Value Response = Amanuensis::Value::MakeObject();
    Response.Insert("jsonrpc", Amanuensis::Value("2.0"));
    Response.Insert("id", Id);
    Response.Insert("error", std::move(Error));
    JsonRpc::WriteMessage(Out_, Response);
}

void Server::Notify(const std::string& Method, Amanuensis::Value Params) {
    Amanuensis::Value Message = Amanuensis::Value::MakeObject();
    Message.Insert("jsonrpc", Amanuensis::Value("2.0"));
    Message.Insert("method", Amanuensis::Value(Method));
    Message.Insert("params", std::move(Params));
    JsonRpc::WriteMessage(Out_, Message);
}

void Server::HandleMessage(const Amanuensis::Value& Message) {
    if (!Message.IsObject() || !Message.Contains("method")) {
        return;
    }
    const std::string        Method = Message.Get("method").AsString();
    const Amanuensis::Value  Params = Message.Contains("params") ? Message.Get("params") : Amanuensis::Value();
    const bool                IsRequest = Message.Contains("id");
    const Amanuensis::Value   Id = IsRequest ? Message.Get("id") : Amanuensis::Value();

    if (Method == "initialize") {
        HandleInitialize(Id, Params);
    } else if (Method == "initialized" || Method == "$/setTrace" || Method == "workspace/didChangeConfiguration") {
        // Accepted, no action needed.
    } else if (Method == "shutdown") {
        ShutdownRequested_ = true;
        Reply(Id, Amanuensis::Value());
    } else if (Method == "exit") {
        // Handled by Run()'s own loop after this returns.
    } else if (Method == "textDocument/didOpen") {
        HandleDidOpen(Params);
    } else if (Method == "textDocument/didChange") {
        HandleDidChange(Params);
    } else if (Method == "textDocument/didClose") {
        HandleDidClose(Params);
    } else if (Method == "textDocument/completion") {
        HandleCompletion(Id, Params);
    } else if (Method == "textDocument/definition") {
        HandleDefinition(Id, Params);
    } else if (IsRequest) {
        ReplyError(Id, -32601, "method not found: " + Method);
    }
    // An unknown notification (no id) is silently ignored, per the LSP spec's own
    // "must not fail" guidance for messages a server doesn't recognise.
}

void Server::HandleInitialize(const Amanuensis::Value& Id, const Amanuensis::Value& /*Params*/) {
    Amanuensis::Value Completion = Amanuensis::Value::MakeObject();
    Amanuensis::Value TriggerChars = Amanuensis::Value::MakeArray();
    TriggerChars.PushBack(Amanuensis::Value("<"));
    TriggerChars.PushBack(Amanuensis::Value(" "));
    Completion.Insert("triggerCharacters", std::move(TriggerChars));

    Amanuensis::Value Capabilities = Amanuensis::Value::MakeObject();
    Capabilities.Insert("textDocumentSync", Amanuensis::Value(1)); // Full
    Capabilities.Insert("completionProvider", std::move(Completion));
    Capabilities.Insert("definitionProvider", Amanuensis::Value(true));

    Amanuensis::Value ServerInfo = Amanuensis::Value::MakeObject();
    ServerInfo.Insert("name", Amanuensis::Value("iris-lsp"));
    ServerInfo.Insert("version", Amanuensis::Value("0.1.0"));

    Amanuensis::Value Result = Amanuensis::Value::MakeObject();
    Result.Insert("capabilities", std::move(Capabilities));
    Result.Insert("serverInfo", std::move(ServerInfo));
    Reply(Id, std::move(Result));
}

void Server::HandleDidOpen(const Amanuensis::Value& Params) {
    const Amanuensis::Value& TextDocument = Params.Get("textDocument");
    RebuildDocument(TextDocument.Get("uri").AsString(), TextDocument.Get("text").AsString());
}

void Server::HandleDidChange(const Amanuensis::Value& Params) {
    const Amanuensis::Value& TextDocument = Params.Get("textDocument");
    const Amanuensis::Value& Changes = Params.Get("contentChanges");
    if (Changes.Size() == 0) {
        return;
    }
    // Full-document sync only (textDocumentSync=1 in our own capabilities) -- the last
    // entry always carries the complete new text.
    RebuildDocument(TextDocument.Get("uri").AsString(), Changes.At(Changes.Size() - 1).Get("text").AsString());
}

void Server::HandleDidClose(const Amanuensis::Value& Params) {
    Documents_.erase(Params.Get("textDocument").Get("uri").AsString());
}

void Server::RebuildDocument(const std::string& Uri, std::string Text) {
    const std::string Path = UriToPath(Uri);

    OpenDocument& Doc = Documents_[Uri];
    Doc.Text = std::move(Text);

    if (Doc.ProjectRoot.empty()) {
        const auto Root = FindProjectRoot(std::filesystem::path(Path).parent_path());
        if (Root) {
            Doc.ProjectRoot = Root->string();
            if (const auto ConfigText = ReadFile(*Root / ".iris.json")) {
                const Iris::IrisConfigParseResult ConfigResult = Iris::ParseIrisConfig(*ConfigText);
                if (ConfigResult.Config) {
                    Doc.Config = *ConfigResult.Config;
                }
            }
        } else {
            // No .iris.json found -- degrade rather than refuse: render{}-scoped
            // completion/diagnostics don't need a project config at all, only
            // import resolution does.
            Doc.ProjectRoot = std::filesystem::path(Path).parent_path().string();
        }
    }

    Doc.Virtual = std::make_unique<VirtualDocument>(Doc.Text, Path, Doc.Config, Doc.ProjectRoot);
    PublishDiagnostics(Uri, Doc);

    if (Doc.Virtual->CompileResult().Diagnostics.empty() && !Doc.Virtual->CompileResult().Output.empty()) {
        if (!ProxyStarted_) {
            Proxy_ = CreateHostLanguageServerProxy(ExtensionOf(Path));
            ProxyStarted_ = true; // only ever attempted once per server process
            if (Proxy_ && !Proxy_->Start(Doc.ProjectRoot)) {
                Proxy_.reset(); // e.g. clangd isn't on PATH -- proxy features degrade to unavailable
            }
        }
        if (Proxy_) {
            const std::string GeneratedPath = Path + ".generated.h";
            std::ofstream(GeneratedPath, std::ios::binary) << Doc.Virtual->CompileResult().Output;
            Proxy_->SyncGeneratedDocument(GeneratedPath, Doc.Virtual->CompileResult().Output);
        }
    }
}

void Server::PublishDiagnostics(const std::string& Uri, const OpenDocument& Doc) {
    Amanuensis::Value Diagnostics = Amanuensis::Value::MakeArray();
    for (const Iris::DriverDiagnostic& Diag : Doc.Virtual->CompileResult().Diagnostics) {
        Amanuensis::Value Diagnostic = Amanuensis::Value::MakeObject();
        Diagnostic.Insert("range", MakeRange(Diag.Location.Line, Diag.Location.Column));
        Diagnostic.Insert("severity", Amanuensis::Value(1)); // Error
        Diagnostic.Insert("source", Amanuensis::Value("iris"));
        Diagnostic.Insert("message", Amanuensis::Value(Diag.Message));
        Diagnostics.PushBack(std::move(Diagnostic));
    }

    Amanuensis::Value Params = Amanuensis::Value::MakeObject();
    Params.Insert("uri", Amanuensis::Value(Uri));
    Params.Insert("diagnostics", std::move(Diagnostics));
    Notify("textDocument/publishDiagnostics", std::move(Params));
}

void Server::HandleCompletion(const Amanuensis::Value& Id, const Amanuensis::Value& Params) {
    const std::string Uri = Params.Get("textDocument").Get("uri").AsString();
    const auto [Line, Column] = PositionFromParams(Params.Get("position"));

    Amanuensis::Value Items = Amanuensis::Value::MakeArray();
    const auto DocIt = Documents_.find(Uri);
    if (DocIt != Documents_.end() && DocIt->second.Virtual) {
        const OpenDocument& Doc = DocIt->second;

        if (Doc.Virtual->IsInsideRenderBlock(Line, Column)) {
            const std::string_view LineTextView = LineText(Doc.Text, Line);
            const RenderCompletionKind Kind = ClassifyRenderCompletion(LineTextView, Column);
            if (Kind == RenderCompletionKind::TagName) {
                for (const std::string& Tag : Iris::CorePrimitiveTagNames()) {
                    Amanuensis::Value Item = Amanuensis::Value::MakeObject();
                    Item.Insert("label", Amanuensis::Value(Tag));
                    Item.Insert("kind", Amanuensis::Value(7)); // Class
                    Items.PushBack(std::move(Item));
                }
                for (const Iris::ImportStatement& Import : Doc.Virtual->Imports()) {
                    Amanuensis::Value Item = Amanuensis::Value::MakeObject();
                    Item.Insert("label", Amanuensis::Value(Import.Name));
                    Item.Insert("kind", Amanuensis::Value(7)); // Class
                    Items.PushBack(std::move(Item));
                }
            } else if (Kind == RenderCompletionKind::AttributeName) {
                for (const auto& [PropName, PropType] : Iris::PrimitivePropTypeNames()) {
                    Amanuensis::Value Item = Amanuensis::Value::MakeObject();
                    Item.Insert("label", Amanuensis::Value(PropName));
                    Item.Insert("kind", Amanuensis::Value(10)); // Property
                    Item.Insert("detail", Amanuensis::Value(PropType));
                    Items.PushBack(std::move(Item));
                }
            }
        } else if (!Doc.Virtual->ImportNameAtLine(Line) && Proxy_) {
            if (const auto Generated = Doc.Virtual->ToGenerated(Line, Column)) {
                const std::string GeneratedPath = UriToPath(Uri) + ".generated.h";
                for (const ProxyCompletionItem& Item : Proxy_->Completion(GeneratedPath, Generated->first, Generated->second)) {
                    Amanuensis::Value Mapped = Amanuensis::Value::MakeObject();
                    Mapped.Insert("label", Amanuensis::Value(Item.Label));
                    if (Item.Kind) {
                        Mapped.Insert("kind", Amanuensis::Value(*Item.Kind));
                    }
                    if (Item.Detail) {
                        Mapped.Insert("detail", Amanuensis::Value(*Item.Detail));
                    }
                    Items.PushBack(std::move(Mapped));
                }
            }
        }
    }

    Amanuensis::Value Result = Amanuensis::Value::MakeObject();
    Result.Insert("isIncomplete", Amanuensis::Value(false));
    Result.Insert("items", std::move(Items));
    Reply(Id, std::move(Result));
}

void Server::HandleDefinition(const Amanuensis::Value& Id, const Amanuensis::Value& Params) {
    const std::string Uri = Params.Get("textDocument").Get("uri").AsString();
    const auto [Line, Column] = PositionFromParams(Params.Get("position"));

    const auto DocIt = Documents_.find(Uri);
    if (DocIt == Documents_.end() || !DocIt->second.Virtual) {
        Reply(Id, Amanuensis::Value());
        return;
    }
    const OpenDocument& Doc = DocIt->second;

    if (const auto ImportName = Doc.Virtual->ImportNameAtLine(Line)) {
        const Iris::ImportResolutionResult Resolved =
            Iris::ResolveImports(Doc.Virtual->Imports(), Doc.Config, Doc.ProjectRoot);
        for (const Iris::ResolvedImport& R : Resolved.Resolved) {
            if (R.Name != *ImportName) {
                continue;
            }
            const auto TargetText = ReadFile(R.ResolvedPath);
            if (!TargetText) {
                break;
            }
            const auto DeclLoc = FindComponentDeclaration(*TargetText, R.Name);
            Amanuensis::Value Location = Amanuensis::Value::MakeObject();
            Location.Insert("uri", Amanuensis::Value(PathToUri(R.ResolvedPath)));
            Location.Insert("range", MakeRange(DeclLoc ? DeclLoc->first : 1, DeclLoc ? DeclLoc->second : 1));
            Reply(Id, std::move(Location));
            return;
        }
        Reply(Id, Amanuensis::Value());
        return;
    }

    if (Doc.Virtual->IsInsideRenderBlock(Line, Column)) {
        // Tag-usage-to-declaration goto-def (jumping from `<Foo>` itself, not just
        // `import Foo`) is a known v1 gap -- see docs/iris_lsp_decision.md.
        Reply(Id, Amanuensis::Value());
        return;
    }

    if (!Proxy_) {
        Reply(Id, Amanuensis::Value());
        return;
    }
    const auto Generated = Doc.Virtual->ToGenerated(Line, Column);
    if (!Generated) {
        Reply(Id, Amanuensis::Value());
        return;
    }
    const std::string GeneratedPath = UriToPath(Uri) + ".generated.h";
    const std::optional<ProxyLocation> Result =
        Proxy_->Definition(GeneratedPath, Generated->first, Generated->second);
    if (!Result) {
        Reply(Id, Amanuensis::Value());
        return;
    }

    // A jump landing back in the same generated buffer (the common case -- most
    // definitions in a single-file component live in that file's own host-language
    // code) is translated back to a `.iris` position; a jump into a different file
    // (a standard header, another project file) is reported as-is, since it was never
    // part of any VirtualDocument's own line map.
    Amanuensis::Value Location = Amanuensis::Value::MakeObject();
    if (Result->FilePath == GeneratedPath) {
        const auto Source = Doc.Virtual->ToSource(Result->Line, Result->Column);
        Location.Insert("uri", Amanuensis::Value(Uri));
        Location.Insert("range", MakeRange(Source ? Source->first : Result->Line, Source ? Source->second : Result->Column));
    } else {
        Location.Insert("uri", Amanuensis::Value(PathToUri(Result->FilePath)));
        Location.Insert("range", MakeRange(Result->Line, Result->Column));
    }
    Reply(Id, std::move(Location));
}

} // namespace IrisLsp
