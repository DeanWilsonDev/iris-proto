#include "Server.h"

#include "Iris/CorePrimitives.h"
#include "Iris/IrisConfig.h"
#include "JsonRpc.h"
#include "RenderTextHeuristics.h"
#include "SemanticTokens.h"

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

Amanuensis::Value MakePosition(std::uint32_t Line, std::uint32_t Column) {
    Amanuensis::Value Position = Amanuensis::Value::MakeObject();
    Position.Insert("line", Amanuensis::Value(static_cast<long long>(Line - 1)));
    Position.Insert("character", Amanuensis::Value(static_cast<long long>(Column - 1)));
    return Position;
}

Amanuensis::Value MakeRangeSpan(std::uint32_t StartLine, std::uint32_t StartColumn, std::uint32_t EndLine,
                                 std::uint32_t EndColumn) {
    Amanuensis::Value Range = Amanuensis::Value::MakeObject();
    Range.Insert("start", MakePosition(StartLine, StartColumn));
    Range.Insert("end", MakePosition(EndLine, EndColumn));
    return Range;
}

Amanuensis::Value MakeRange(std::uint32_t Line, std::uint32_t Column) { return MakeRangeSpan(Line, Column, Line, Column); }

std::pair<std::uint32_t, std::uint32_t> PositionFromParams(const Amanuensis::Value& Position) {
    return {static_cast<std::uint32_t>(Position.Get("line").AsInteger()) + 1,
            static_cast<std::uint32_t>(Position.Get("character").AsInteger()) + 1};
}

} // namespace

void Server::DisableProxyForTesting() { ProxyStarted_ = true; }

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
    std::lock_guard<std::mutex> Lock(OutMutex_);
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
    std::lock_guard<std::mutex> Lock(OutMutex_);
    JsonRpc::WriteMessage(Out_, Response);
}

void Server::Notify(const std::string& Method, Amanuensis::Value Params) {
    Amanuensis::Value Message = Amanuensis::Value::MakeObject();
    Message.Insert("jsonrpc", Amanuensis::Value("2.0"));
    Message.Insert("method", Amanuensis::Value(Method));
    Message.Insert("params", std::move(Params));
    std::lock_guard<std::mutex> Lock(OutMutex_);
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
    } else if (Method == "textDocument/semanticTokens/full") {
        HandleSemanticTokensFull(Id, Params);
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

    Amanuensis::Value TokenTypes = Amanuensis::Value::MakeArray();
    for (const char* Name : SemanticTokenTypeNames) {
        TokenTypes.PushBack(Amanuensis::Value(Name));
    }
    Amanuensis::Value Legend = Amanuensis::Value::MakeObject();
    Legend.Insert("tokenTypes", std::move(TokenTypes));
    Legend.Insert("tokenModifiers", Amanuensis::Value::MakeArray()); // none defined yet
    Amanuensis::Value SemanticTokensProvider = Amanuensis::Value::MakeObject();
    SemanticTokensProvider.Insert("legend", std::move(Legend));
    SemanticTokensProvider.Insert("full", Amanuensis::Value(true));

    Amanuensis::Value Capabilities = Amanuensis::Value::MakeObject();
    Capabilities.Insert("textDocumentSync", Amanuensis::Value(1)); // Full
    Capabilities.Insert("completionProvider", std::move(Completion));
    Capabilities.Insert("definitionProvider", Amanuensis::Value(true));
    Capabilities.Insert("semanticTokensProvider", std::move(SemanticTokensProvider));

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
    const std::string Uri = Params.Get("textDocument").Get("uri").AsString();
    std::lock_guard<std::mutex> Lock(DocumentsMutex_);
    Documents_.erase(Uri);
    GeneratedPathToUri_.erase(UriToPath(Uri) + ".generated.h");
}

void Server::RebuildDocument(const std::string& Uri, std::string Text) {
    const std::string Path = UriToPath(Uri);

    // Everything that needs Documents_/GeneratedPathToUri_ happens under the lock;
    // Proxy_ setup/sync happens after it's released, since Proxy_->Start() blocks on a
    // reply from ClangdProxy's own background reader thread -- holding DocumentsMutex_
    // across that wait would deadlock the moment that thread needs the same lock inside
    // HandleClangdDiagnostics (see this class's own header comment).
    std::string ProjectRoot;
    std::string GeneratedPath;
    std::string GeneratedText;
    bool        NeedsProxySetup = false;

    {
        std::lock_guard<std::mutex> Lock(DocumentsMutex_);
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
        GeneratedPathToUri_[Path + ".generated.h"] = Uri;
        PublishDiagnostics(Uri, Doc);

        ProjectRoot = Doc.ProjectRoot;
        if (Doc.Virtual->CompileResult().Diagnostics.empty() && !Doc.Virtual->CompileResult().Output.empty()) {
            GeneratedPath = Path + ".generated.h";
            GeneratedText = Doc.Virtual->CompileResult().Output;
            NeedsProxySetup = !ProxyStarted_;
        }
    }

    if (GeneratedPath.empty()) {
        return; // Iris-level diagnostics present, or no render{} blocks -- nothing to proxy
    }

    // Proxy_/ProxyStarted_ are touched only from this function, which only ever runs on
    // the main thread (Run()'s single-threaded read loop) -- no mutex needed for them
    // specifically, only for the Documents_/GeneratedPathToUri_ access above.
    if (NeedsProxySetup) {
        Proxy_ = CreateHostLanguageServerProxy(ExtensionOf(Path));
        ProxyStarted_ = true; // only ever attempted once per server process
        if (Proxy_) {
            Proxy_->SetDiagnosticsCallback([this](const std::string& GenPath, std::vector<ProxyDiagnostic> Diags) {
                HandleClangdDiagnostics(GenPath, std::move(Diags));
            });
            if (!Proxy_->Start(ProjectRoot)) {
                Proxy_.reset(); // e.g. clangd isn't on PATH -- proxy features degrade to unavailable
            }
        }
    }
    if (Proxy_) {
        std::ofstream(GeneratedPath, std::ios::binary) << GeneratedText;
        Proxy_->SyncGeneratedDocument(GeneratedPath, GeneratedText);
    }
}

Amanuensis::Value Server::BuildIrisDiagnosticsArray(const OpenDocument& Doc) const {
    Amanuensis::Value Diagnostics = Amanuensis::Value::MakeArray();
    for (const Iris::DriverDiagnostic& Diag : Doc.Virtual->CompileResult().Diagnostics) {
        Amanuensis::Value Diagnostic = Amanuensis::Value::MakeObject();
        Diagnostic.Insert("range", MakeRange(Diag.Location.Line, Diag.Location.Column));
        Diagnostic.Insert("severity", Amanuensis::Value(1)); // Error
        Diagnostic.Insert("source", Amanuensis::Value("iris"));
        Diagnostic.Insert("message", Amanuensis::Value(Diag.Message));
        Diagnostics.PushBack(std::move(Diagnostic));
    }
    return Diagnostics;
}

void Server::PublishDiagnostics(const std::string& Uri, const OpenDocument& Doc) {
    Amanuensis::Value Params = Amanuensis::Value::MakeObject();
    Params.Insert("uri", Amanuensis::Value(Uri));
    Params.Insert("diagnostics", BuildIrisDiagnosticsArray(Doc));
    Notify("textDocument/publishDiagnostics", std::move(Params));
}

void Server::HandleClangdDiagnostics(const std::string& GeneratedPath, std::vector<ProxyDiagnostic> Diagnostics) {
    std::string       Uri;
    Amanuensis::Value Merged;
    {
        std::lock_guard<std::mutex> Lock(DocumentsMutex_);
        const auto UriIt = GeneratedPathToUri_.find(GeneratedPath);
        if (UriIt == GeneratedPathToUri_.end()) {
            return; // e.g. a stale generated file's diagnostics after didClose
        }
        Uri = UriIt->second;
        const auto DocIt = Documents_.find(Uri);
        if (DocIt == Documents_.end() || !DocIt->second.Virtual) {
            return;
        }
        const OpenDocument& Doc = DocIt->second;

        Merged = BuildIrisDiagnosticsArray(Doc); // always empty here in practice -- clangd is only
                                                  // ever synced when Iris's own diagnostics are empty
                                                  // (RebuildDocument's own gate) -- included anyway so
                                                  // this stays correct if that gate ever loosens.
        for (const ProxyDiagnostic& D : Diagnostics) {
            const auto MappedStart = Doc.Virtual->ToSource(D.StartLine, D.StartColumn);
            const auto MappedEnd = Doc.Virtual->ToSource(D.EndLine, D.EndColumn);
            if (!MappedStart || !MappedEnd) {
                continue; // points at synthesized prologue (#pragma once/#line) -- no source position
            }
            Amanuensis::Value Diagnostic = Amanuensis::Value::MakeObject();
            Diagnostic.Insert("range",
                               MakeRangeSpan(MappedStart->first, MappedStart->second, MappedEnd->first, MappedEnd->second));
            Diagnostic.Insert("severity", Amanuensis::Value(D.Severity));
            Diagnostic.Insert("source", Amanuensis::Value("clangd"));
            Diagnostic.Insert("message", Amanuensis::Value(D.Message));
            Merged.PushBack(std::move(Diagnostic));
        }
    }

    Amanuensis::Value Params = Amanuensis::Value::MakeObject();
    Params.Insert("uri", Amanuensis::Value(Uri));
    Params.Insert("diagnostics", std::move(Merged));
    // LSP's publishDiagnostics replaces a uri's previous set wholesale, so this
    // supersedes the plain-Iris publish RebuildDocument already sent -- no duplication.
    Notify("textDocument/publishDiagnostics", std::move(Params));
}

void Server::HandleCompletion(const Amanuensis::Value& Id, const Amanuensis::Value& Params) {
    const std::string Uri = Params.Get("textDocument").Get("uri").AsString();
    const auto [Line, Column] = PositionFromParams(Params.Get("position"));

    // Everything needed from Documents_ is copied out before any Proxy_ call, which may
    // block on IPC with clangd -- see RebuildDocument's own comment on why holding
    // DocumentsMutex_ across that wait isn't safe.
    bool                        Found = false;
    bool                        InRenderBlock = false;
    RenderCompletionKind        Kind = RenderCompletionKind::None;
    std::vector<std::string>    ImportNames;
    bool                        IsImportLine = false;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> Generated;
    {
        std::lock_guard<std::mutex> Lock(DocumentsMutex_);
        const auto DocIt = Documents_.find(Uri);
        if (DocIt != Documents_.end() && DocIt->second.Virtual) {
            Found = true;
            const OpenDocument& Doc = DocIt->second;
            InRenderBlock = Doc.Virtual->IsInsideRenderBlock(Line, Column);
            if (InRenderBlock) {
                Kind = ClassifyRenderCompletion(LineText(Doc.Text, Line), Column);
                if (Kind == RenderCompletionKind::TagName) {
                    for (const Iris::ImportStatement& Import : Doc.Virtual->Imports()) {
                        ImportNames.push_back(Import.Name);
                    }
                }
            } else {
                IsImportLine = Doc.Virtual->ImportNameAtLine(Line).has_value();
                if (!IsImportLine) {
                    Generated = Doc.Virtual->ToGenerated(Line, Column);
                }
            }
        }
    }

    Amanuensis::Value Items = Amanuensis::Value::MakeArray();
    if (Found) {
        if (Kind == RenderCompletionKind::TagName) {
            for (const std::string& Tag : Iris::CorePrimitiveTagNames()) {
                Amanuensis::Value Item = Amanuensis::Value::MakeObject();
                Item.Insert("label", Amanuensis::Value(Tag));
                Item.Insert("kind", Amanuensis::Value(7)); // Class
                Items.PushBack(std::move(Item));
            }
            for (const std::string& Name : ImportNames) {
                Amanuensis::Value Item = Amanuensis::Value::MakeObject();
                Item.Insert("label", Amanuensis::Value(Name));
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
        } else if (!InRenderBlock && Generated && Proxy_) {
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

    Amanuensis::Value Result = Amanuensis::Value::MakeObject();
    Result.Insert("isIncomplete", Amanuensis::Value(false));
    Result.Insert("items", std::move(Items));
    Reply(Id, std::move(Result));
}

std::optional<ProxyLocation> Server::ResolveComponentDeclaration(const std::string& Name,
                                                                   const std::vector<Iris::ImportStatement>& Imports,
                                                                   const Iris::IrisConfig& Config,
                                                                   const std::string& ProjectRoot) const {
    const Iris::ImportResolutionResult Resolved = Iris::ResolveImports(Imports, Config, ProjectRoot);
    for (const Iris::ResolvedImport& R : Resolved.Resolved) {
        if (R.Name != Name) {
            continue;
        }
        const auto TargetText = ReadFile(R.ResolvedPath);
        if (!TargetText) {
            return std::nullopt;
        }
        const auto DeclLoc = FindComponentDeclaration(*TargetText, R.Name);
        // A resolved-but-undeclared-looking file (FindComponentDeclaration found no
        // `Name(` pattern -- e.g. the target has a syntax shape this heuristic doesn't
        // recognise) still gets a Location, just pointing at the file's own line 1 rather
        // than failing goto-def outright -- "the right file, imprecise line" beats "no
        // jump at all".
        return ProxyLocation{R.ResolvedPath, DeclLoc ? DeclLoc->first : 1, DeclLoc ? DeclLoc->second : 1};
    }
    return std::nullopt;
}

void Server::HandleDefinition(const Amanuensis::Value& Id, const Amanuensis::Value& Params) {
    const std::string Uri = Params.Get("textDocument").Get("uri").AsString();
    const auto [Line, Column] = PositionFromParams(Params.Get("position"));

    // NameToResolve covers both goto-def sources that end up at a component
    // declaration: an `import Name` statement line, and a `<Name>`/`</Name>` tag usage
    // inside render{} -- every non-Core-primitive tag *must* be imported
    // (SemanticValidator's own "is not imported and is not a Core primitive" rule), so
    // there's no separate same-file-component case; both funnel into
    // ResolveComponentDeclaration identically once a name is found.
    std::optional<std::string>          NameToResolve;
    Iris::IrisConfig                    Config;
    std::string                         ProjectRoot;
    std::vector<Iris::ImportStatement>  Imports;
    bool                                InRenderBlock = false;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> Generated;
    {
        std::lock_guard<std::mutex> Lock(DocumentsMutex_);
        const auto DocIt = Documents_.find(Uri);
        if (DocIt == Documents_.end() || !DocIt->second.Virtual) {
            Reply(Id, Amanuensis::Value());
            return;
        }
        const OpenDocument& Doc = DocIt->second;
        NameToResolve = Doc.Virtual->ImportNameAtLine(Line);
        if (!NameToResolve) {
            InRenderBlock = Doc.Virtual->IsInsideRenderBlock(Line, Column);
            if (InRenderBlock) {
                if (const auto Tag = TagNameAtPosition(LineText(Doc.Text, Line), Column)) {
                    // A Core primitive (<Frame>, <Text>, ...) has no declaration to jump
                    // to -- leave NameToResolve unset rather than trying to resolve it as
                    // an import and failing.
                    if (Iris::CorePrimitiveTagNames().count(*Tag) == 0) {
                        NameToResolve = Tag;
                    }
                }
            } else {
                Generated = Doc.Virtual->ToGenerated(Line, Column);
            }
        }
        if (NameToResolve) {
            Config = Doc.Config;
            ProjectRoot = Doc.ProjectRoot;
            Imports = Doc.Virtual->Imports();
        }
    }

    if (NameToResolve) {
        if (const auto Loc = ResolveComponentDeclaration(*NameToResolve, Imports, Config, ProjectRoot)) {
            Amanuensis::Value Location = Amanuensis::Value::MakeObject();
            Location.Insert("uri", Amanuensis::Value(PathToUri(Loc->FilePath)));
            Location.Insert("range", MakeRange(Loc->Line, Loc->Column));
            Reply(Id, std::move(Location));
        } else {
            Reply(Id, Amanuensis::Value());
        }
        return;
    }

    if (InRenderBlock || !Generated || !Proxy_) {
        Reply(Id, Amanuensis::Value());
        return;
    }

    const std::string GeneratedPath = UriToPath(Uri) + ".generated.h";
    const std::optional<ProxyLocation> Result = Proxy_->Definition(GeneratedPath, Generated->first, Generated->second);
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
        std::lock_guard<std::mutex> Lock(DocumentsMutex_);
        const auto DocIt = Documents_.find(Uri);
        const auto Source = (DocIt != Documents_.end() && DocIt->second.Virtual)
                                 ? DocIt->second.Virtual->ToSource(Result->Line, Result->Column)
                                 : std::nullopt;
        Location.Insert("uri", Amanuensis::Value(Uri));
        Location.Insert("range", MakeRange(Source ? Source->first : Result->Line, Source ? Source->second : Result->Column));
    } else {
        Location.Insert("uri", Amanuensis::Value(PathToUri(Result->FilePath)));
        Location.Insert("range", MakeRange(Result->Line, Result->Column));
    }
    Reply(Id, std::move(Location));
}

void Server::HandleSemanticTokensFull(const Amanuensis::Value& Id, const Amanuensis::Value& Params) {
    const std::string Uri = Params.Get("textDocument").Get("uri").AsString();

    std::vector<SemanticToken> Tokens;
    {
        std::lock_guard<std::mutex> Lock(DocumentsMutex_);
        const auto DocIt = Documents_.find(Uri);
        if (DocIt != Documents_.end() && DocIt->second.Virtual) {
            Tokens = CollectRenderBlockSemanticTokens(DocIt->second.Virtual->RenderBlocks());
        }
    }

    // LSP's delta encoding: each token is [deltaLine, deltaStartChar, length, tokenType,
    // tokenModifiers] relative to the *previous* token -- deltaStartChar is relative to
    // the previous token's start column only when they're on the same line, else it's
    // the token's own absolute (0-based) column. Tokens is already sorted ascending by
    // (Line, Column) (CollectRenderBlockSemanticTokens's own contract).
    Amanuensis::Value Data = Amanuensis::Value::MakeArray();
    std::uint32_t     PrevLine0 = 0;
    std::uint32_t     PrevChar0 = 0;
    for (const SemanticToken& Token : Tokens) {
        const std::uint32_t Line0 = Token.Line - 1;
        const std::uint32_t Char0 = Token.Column - 1;
        const std::uint32_t DeltaLine = Line0 - PrevLine0;
        const std::uint32_t DeltaChar = (DeltaLine == 0) ? (Char0 - PrevChar0) : Char0;

        Data.PushBack(Amanuensis::Value(static_cast<long long>(DeltaLine)));
        Data.PushBack(Amanuensis::Value(static_cast<long long>(DeltaChar)));
        Data.PushBack(Amanuensis::Value(static_cast<long long>(Token.Length)));
        Data.PushBack(Amanuensis::Value(static_cast<long long>(Token.Type)));
        Data.PushBack(Amanuensis::Value(0)); // no token modifiers defined yet

        PrevLine0 = Line0;
        PrevChar0 = Char0;
    }

    Amanuensis::Value Result = Amanuensis::Value::MakeObject();
    Result.Insert("data", std::move(Data));
    Reply(Id, std::move(Result));
}

} // namespace IrisLsp
