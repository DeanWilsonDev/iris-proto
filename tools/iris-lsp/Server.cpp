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

Amanuensis::JsonValue MakePosition(std::uint32_t Line, std::uint32_t Column) {
    Amanuensis::JsonValue Position = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Position, "line", Amanuensis::JsonValue(static_cast<long long>(Line - 1)));
    Amanuensis::Json::Insert(Position, "character", Amanuensis::JsonValue(static_cast<long long>(Column - 1)));
    return Position;
}

Amanuensis::JsonValue MakeRangeSpan(std::uint32_t StartLine, std::uint32_t StartColumn, std::uint32_t EndLine,
                                 std::uint32_t EndColumn) {
    Amanuensis::JsonValue Range = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Range, "start", MakePosition(StartLine, StartColumn));
    Amanuensis::Json::Insert(Range, "end", MakePosition(EndLine, EndColumn));
    return Range;
}

Amanuensis::JsonValue MakeRange(std::uint32_t Line, std::uint32_t Column) { return MakeRangeSpan(Line, Column, Line, Column); }

std::pair<std::uint32_t, std::uint32_t> PositionFromParams(const Amanuensis::JsonValue& Position) {
    return {static_cast<std::uint32_t>(Amanuensis::Json::AsInteger(Amanuensis::Json::Get(Position, "line"))) + 1,
            static_cast<std::uint32_t>(Amanuensis::Json::AsInteger(Amanuensis::Json::Get(Position, "character"))) + 1};
}

} // namespace

void Server::DisableProxyForTesting() { ProxyStarted_ = true; }

void Server::Run(std::FILE* In, std::FILE* Out) {
    Out_ = Out;
    for (;;) {
        const std::optional<Amanuensis::JsonValue> Message = JsonRpc::ReadMessage(In);
        if (!Message) {
            return;
        }
        const std::string Method = Amanuensis::Json::IsObject(*Message) && Amanuensis::Json::Contains(*Message, "method")
                                        ? Amanuensis::Json::AsString(Amanuensis::Json::Get(*Message, "method"))
                                        : std::string{};
        HandleMessage(*Message);
        if (Method == "exit") {
            return;
        }
    }
}

void Server::Reply(const Amanuensis::JsonValue& Id, Amanuensis::JsonValue Result) {
    Amanuensis::JsonValue Message = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Message, "jsonrpc", Amanuensis::JsonValue("2.0"));
    Amanuensis::Json::Insert(Message, "id", Id);
    Amanuensis::Json::Insert(Message, "result", std::move(Result));
    std::lock_guard<std::mutex> Lock(OutMutex_);
    JsonRpc::WriteMessage(Out_, Message);
}

void Server::ReplyError(const Amanuensis::JsonValue& Id, int Code, const std::string& Message) {
    Amanuensis::JsonValue Error = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Error, "code", Amanuensis::JsonValue(static_cast<long long>(Code)));
    Amanuensis::Json::Insert(Error, "message", Amanuensis::JsonValue(Message));
    Amanuensis::JsonValue Response = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Response, "jsonrpc", Amanuensis::JsonValue("2.0"));
    Amanuensis::Json::Insert(Response, "id", Id);
    Amanuensis::Json::Insert(Response, "error", std::move(Error));
    std::lock_guard<std::mutex> Lock(OutMutex_);
    JsonRpc::WriteMessage(Out_, Response);
}

void Server::Notify(const std::string& Method, Amanuensis::JsonValue Params) {
    Amanuensis::JsonValue Message = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Message, "jsonrpc", Amanuensis::JsonValue("2.0"));
    Amanuensis::Json::Insert(Message, "method", Amanuensis::JsonValue(Method));
    Amanuensis::Json::Insert(Message, "params", std::move(Params));
    std::lock_guard<std::mutex> Lock(OutMutex_);
    JsonRpc::WriteMessage(Out_, Message);
}

void Server::HandleMessage(const Amanuensis::JsonValue& Message) {
    if (!Amanuensis::Json::IsObject(Message) || !Amanuensis::Json::Contains(Message, "method")) {
        return;
    }
    const std::string       Method = Amanuensis::Json::AsString(Amanuensis::Json::Get(Message, "method"));
    const Amanuensis::JsonValue Params =
        Amanuensis::Json::Contains(Message, "params") ? Amanuensis::Json::Get(Message, "params") : Amanuensis::JsonValue();
    const bool               IsRequest = Amanuensis::Json::Contains(Message, "id");
    const Amanuensis::JsonValue  Id = IsRequest ? Amanuensis::Json::Get(Message, "id") : Amanuensis::JsonValue();

    if (Method == "initialize") {
        HandleInitialize(Id, Params);
    } else if (Method == "initialized" || Method == "$/setTrace" || Method == "workspace/didChangeConfiguration") {
        // Accepted, no action needed.
    } else if (Method == "shutdown") {
        ShutdownRequested_ = true;
        Reply(Id, Amanuensis::JsonValue());
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

void Server::HandleInitialize(const Amanuensis::JsonValue& Id, const Amanuensis::JsonValue& /*Params*/) {
    Amanuensis::JsonValue Completion = Amanuensis::Json::MakeObject();
    Amanuensis::JsonValue TriggerChars = Amanuensis::Json::MakeArray();
    Amanuensis::Json::PushBack(TriggerChars, Amanuensis::JsonValue("<"));
    Amanuensis::Json::PushBack(TriggerChars, Amanuensis::JsonValue(" "));
    Amanuensis::Json::Insert(Completion, "triggerCharacters", std::move(TriggerChars));

    Amanuensis::JsonValue TokenTypes = Amanuensis::Json::MakeArray();
    for (const char* Name : SemanticTokenTypeNames) {
        Amanuensis::Json::PushBack(TokenTypes, Amanuensis::JsonValue(Name));
    }
    Amanuensis::JsonValue Legend = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Legend, "tokenTypes", std::move(TokenTypes));
    Amanuensis::Json::Insert(Legend, "tokenModifiers", Amanuensis::Json::MakeArray()); // none defined yet
    Amanuensis::JsonValue SemanticTokensProvider = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(SemanticTokensProvider, "legend", std::move(Legend));
    Amanuensis::Json::Insert(SemanticTokensProvider, "full", Amanuensis::JsonValue(true));

    Amanuensis::JsonValue Capabilities = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Capabilities, "textDocumentSync", Amanuensis::JsonValue(static_cast<long long>(1))); // Full
    Amanuensis::Json::Insert(Capabilities, "completionProvider", std::move(Completion));
    Amanuensis::Json::Insert(Capabilities, "definitionProvider", Amanuensis::JsonValue(true));
    Amanuensis::Json::Insert(Capabilities, "semanticTokensProvider", std::move(SemanticTokensProvider));

    Amanuensis::JsonValue ServerInfo = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(ServerInfo, "name", Amanuensis::JsonValue("iris-lsp"));
    Amanuensis::Json::Insert(ServerInfo, "version", Amanuensis::JsonValue("0.1.0"));

    Amanuensis::JsonValue Result = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Result, "capabilities", std::move(Capabilities));
    Amanuensis::Json::Insert(Result, "serverInfo", std::move(ServerInfo));
    Reply(Id, std::move(Result));
}

void Server::HandleDidOpen(const Amanuensis::JsonValue& Params) {
    const Amanuensis::JsonValue& TextDocument = Amanuensis::Json::Get(Params, "textDocument");
    RebuildDocument(Amanuensis::Json::AsString(Amanuensis::Json::Get(TextDocument, "uri")),
                     Amanuensis::Json::AsString(Amanuensis::Json::Get(TextDocument, "text")));
}

void Server::HandleDidChange(const Amanuensis::JsonValue& Params) {
    const Amanuensis::JsonValue& TextDocument = Amanuensis::Json::Get(Params, "textDocument");
    const Amanuensis::JsonValue& Changes = Amanuensis::Json::Get(Params, "contentChanges");
    if (Amanuensis::Json::Size(Changes) == 0) {
        return;
    }
    // Full-document sync only (textDocumentSync=1 in our own capabilities) -- the last
    // entry always carries the complete new text.
    RebuildDocument(Amanuensis::Json::AsString(Amanuensis::Json::Get(TextDocument, "uri")),
                     Amanuensis::Json::AsString(Amanuensis::Json::Get(
                         Amanuensis::Json::At(Changes, Amanuensis::Json::Size(Changes) - 1), "text")));
}

void Server::HandleDidClose(const Amanuensis::JsonValue& Params) {
    const std::string Uri =
        Amanuensis::Json::AsString(Amanuensis::Json::Get(Amanuensis::Json::Get(Params, "textDocument"), "uri"));
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

Amanuensis::JsonValue Server::BuildIrisDiagnosticsArray(const OpenDocument& Doc) const {
    Amanuensis::JsonValue Diagnostics = Amanuensis::Json::MakeArray();
    for (const Iris::DriverDiagnostic& Diag : Doc.Virtual->CompileResult().Diagnostics) {
        Amanuensis::JsonValue Diagnostic = Amanuensis::Json::MakeObject();
        Amanuensis::Json::Insert(Diagnostic, "range", MakeRange(Diag.Location.Line, Diag.Location.Column));
        Amanuensis::Json::Insert(Diagnostic, "severity", Amanuensis::JsonValue(static_cast<long long>(1))); // Error
        Amanuensis::Json::Insert(Diagnostic, "source", Amanuensis::JsonValue("iris"));
        Amanuensis::Json::Insert(Diagnostic, "message", Amanuensis::JsonValue(Diag.Message));
        Amanuensis::Json::PushBack(Diagnostics, std::move(Diagnostic));
    }
    return Diagnostics;
}

void Server::PublishDiagnostics(const std::string& Uri, const OpenDocument& Doc) {
    Amanuensis::JsonValue Params = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Params, "uri", Amanuensis::JsonValue(Uri));
    Amanuensis::Json::Insert(Params, "diagnostics", BuildIrisDiagnosticsArray(Doc));
    Notify("textDocument/publishDiagnostics", std::move(Params));
}

void Server::HandleClangdDiagnostics(const std::string& GeneratedPath, std::vector<ProxyDiagnostic> Diagnostics) {
    std::string       Uri;
    Amanuensis::JsonValue Merged;
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
            Amanuensis::JsonValue Diagnostic = Amanuensis::Json::MakeObject();
            Amanuensis::Json::Insert(
                Diagnostic, "range",
                MakeRangeSpan(MappedStart->first, MappedStart->second, MappedEnd->first, MappedEnd->second));
            Amanuensis::Json::Insert(Diagnostic, "severity", Amanuensis::JsonValue(static_cast<long long>(D.Severity)));
            Amanuensis::Json::Insert(Diagnostic, "source", Amanuensis::JsonValue("clangd"));
            Amanuensis::Json::Insert(Diagnostic, "message", Amanuensis::JsonValue(D.Message));
            Amanuensis::Json::PushBack(Merged, std::move(Diagnostic));
        }
    }

    Amanuensis::JsonValue Params = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Params, "uri", Amanuensis::JsonValue(Uri));
    Amanuensis::Json::Insert(Params, "diagnostics", std::move(Merged));
    // LSP's publishDiagnostics replaces a uri's previous set wholesale, so this
    // supersedes the plain-Iris publish RebuildDocument already sent -- no duplication.
    Notify("textDocument/publishDiagnostics", std::move(Params));
}

void Server::HandleCompletion(const Amanuensis::JsonValue& Id, const Amanuensis::JsonValue& Params) {
    const std::string Uri =
        Amanuensis::Json::AsString(Amanuensis::Json::Get(Amanuensis::Json::Get(Params, "textDocument"), "uri"));
    const auto [Line, Column] = PositionFromParams(Amanuensis::Json::Get(Params, "position"));

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

    Amanuensis::JsonValue Items = Amanuensis::Json::MakeArray();
    if (Found) {
        if (Kind == RenderCompletionKind::TagName) {
            for (const std::string& Tag : Iris::CorePrimitiveTagNames()) {
                Amanuensis::JsonValue Item = Amanuensis::Json::MakeObject();
                Amanuensis::Json::Insert(Item, "label", Amanuensis::JsonValue(Tag));
                Amanuensis::Json::Insert(Item, "kind", Amanuensis::JsonValue(static_cast<long long>(7))); // Class
                Amanuensis::Json::PushBack(Items, std::move(Item));
            }
            for (const std::string& Name : ImportNames) {
                Amanuensis::JsonValue Item = Amanuensis::Json::MakeObject();
                Amanuensis::Json::Insert(Item, "label", Amanuensis::JsonValue(Name));
                Amanuensis::Json::Insert(Item, "kind", Amanuensis::JsonValue(static_cast<long long>(7))); // Class
                Amanuensis::Json::PushBack(Items, std::move(Item));
            }
        } else if (Kind == RenderCompletionKind::AttributeName) {
            for (const auto& [PropName, PropType] : Iris::PrimitivePropTypeNames()) {
                Amanuensis::JsonValue Item = Amanuensis::Json::MakeObject();
                Amanuensis::Json::Insert(Item, "label", Amanuensis::JsonValue(PropName));
                Amanuensis::Json::Insert(Item, "kind", Amanuensis::JsonValue(static_cast<long long>(10))); // Property
                Amanuensis::Json::Insert(Item, "detail", Amanuensis::JsonValue(PropType));
                Amanuensis::Json::PushBack(Items, std::move(Item));
            }
        } else if (!InRenderBlock && Generated && Proxy_) {
            const std::string GeneratedPath = UriToPath(Uri) + ".generated.h";
            for (const ProxyCompletionItem& Item : Proxy_->Completion(GeneratedPath, Generated->first, Generated->second)) {
                Amanuensis::JsonValue Mapped = Amanuensis::Json::MakeObject();
                Amanuensis::Json::Insert(Mapped, "label", Amanuensis::JsonValue(Item.Label));
                if (Item.Kind) {
                    Amanuensis::Json::Insert(Mapped, "kind", Amanuensis::JsonValue(static_cast<long long>(*Item.Kind)));
                }
                if (Item.Detail) {
                    Amanuensis::Json::Insert(Mapped, "detail", Amanuensis::JsonValue(*Item.Detail));
                }
                Amanuensis::Json::PushBack(Items, std::move(Mapped));
            }
        }
    }

    Amanuensis::JsonValue Result = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Result, "isIncomplete", Amanuensis::JsonValue(false));
    Amanuensis::Json::Insert(Result, "items", std::move(Items));
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

std::optional<ProxyLocation> Server::ResolveClassSelector(const std::string& ClassName,
                                                            const std::string& IrisFilePath) const {
    const std::filesystem::path Iris(IrisFilePath);
    const std::filesystem::path ComponentLustre = std::filesystem::path(Iris).replace_extension(".lustre");
    const std::filesystem::path GlobalLustre = Iris.parent_path() / "global.lustre";

    for (const std::filesystem::path& Candidate : {ComponentLustre, GlobalLustre}) {
        const auto Text = ReadFile(Candidate);
        if (!Text) {
            continue;
        }
        if (const auto Loc = FindClassSelector(*Text, ClassName)) {
            return ProxyLocation{Candidate.string(), Loc->first, Loc->second};
        }
    }
    return std::nullopt;
}

void Server::HandleDefinition(const Amanuensis::JsonValue& Id, const Amanuensis::JsonValue& Params) {
    const std::string Uri =
        Amanuensis::Json::AsString(Amanuensis::Json::Get(Amanuensis::Json::Get(Params, "textDocument"), "uri"));
    const auto [Line, Column] = PositionFromParams(Amanuensis::Json::Get(Params, "position"));

    // NameToResolve covers both goto-def sources that end up at a component
    // declaration: an `import Name` statement line, and a `<Name>`/`</Name>` tag usage
    // inside render{} -- every non-Core-primitive tag *must* be imported
    // (SemanticValidator's own "is not imported and is not a Core primitive" rule), so
    // there's no separate same-file-component case; both funnel into
    // ResolveComponentDeclaration identically once a name is found.
    std::optional<std::string>          NameToResolve;
    std::optional<std::string>          ClassNameToResolve;
    Iris::IrisConfig                    Config;
    std::string                         ProjectRoot;
    std::vector<Iris::ImportStatement>  Imports;
    bool                                InRenderBlock = false;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> Generated;
    {
        std::lock_guard<std::mutex> Lock(DocumentsMutex_);
        const auto DocIt = Documents_.find(Uri);
        if (DocIt == Documents_.end() || !DocIt->second.Virtual) {
            Reply(Id, Amanuensis::JsonValue());
            return;
        }
        const OpenDocument& Doc = DocIt->second;
        NameToResolve = Doc.Virtual->ImportNameAtLine(Line);
        if (!NameToResolve) {
            InRenderBlock = Doc.Virtual->IsInsideRenderBlock(Line, Column);
            if (InRenderBlock) {
                const std::string_view CurrentLine = LineText(Doc.Text, Line);
                // Checked before TagNameAtPosition: a `class="card"` value span and a tag
                // name never overlap on the same line (the tag name always precedes any
                // attribute), so this ordering is only ever a matter of which heuristic
                // actually matches, not a real precedence conflict.
                ClassNameToResolve = ClassPropValueAtPosition(CurrentLine, Column);
                if (!ClassNameToResolve) {
                    if (const auto Tag = TagNameAtPosition(CurrentLine, Column)) {
                        // A Core primitive (<Frame>, <Text>, ...) has no declaration to jump
                        // to -- leave NameToResolve unset rather than trying to resolve it as
                        // an import and failing.
                        if (Iris::CorePrimitiveTagNames().count(*Tag) == 0) {
                            NameToResolve = Tag;
                        }
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
            Amanuensis::JsonValue Location = Amanuensis::Json::MakeObject();
            Amanuensis::Json::Insert(Location, "uri", Amanuensis::JsonValue(PathToUri(Loc->FilePath)));
            Amanuensis::Json::Insert(Location, "range", MakeRange(Loc->Line, Loc->Column));
            Reply(Id, std::move(Location));
        } else {
            Reply(Id, Amanuensis::JsonValue());
        }
        return;
    }

    if (ClassNameToResolve) {
        if (const auto Loc = ResolveClassSelector(*ClassNameToResolve, UriToPath(Uri))) {
            Amanuensis::JsonValue Location = Amanuensis::Json::MakeObject();
            Amanuensis::Json::Insert(Location, "uri", Amanuensis::JsonValue(PathToUri(Loc->FilePath)));
            Amanuensis::Json::Insert(Location, "range", MakeRange(Loc->Line, Loc->Column));
            Reply(Id, std::move(Location));
        } else {
            Reply(Id, Amanuensis::JsonValue());
        }
        return;
    }

    if (InRenderBlock || !Generated || !Proxy_) {
        Reply(Id, Amanuensis::JsonValue());
        return;
    }

    const std::string GeneratedPath = UriToPath(Uri) + ".generated.h";
    const std::optional<ProxyLocation> Result = Proxy_->Definition(GeneratedPath, Generated->first, Generated->second);
    if (!Result) {
        Reply(Id, Amanuensis::JsonValue());
        return;
    }

    // A jump landing back in the same generated buffer (the common case -- most
    // definitions in a single-file component live in that file's own host-language
    // code) is translated back to a `.iris` position; a jump into a different file
    // (a standard header, another project file) is reported as-is, since it was never
    // part of any VirtualDocument's own line map.
    Amanuensis::JsonValue Location = Amanuensis::Json::MakeObject();
    if (Result->FilePath == GeneratedPath) {
        std::lock_guard<std::mutex> Lock(DocumentsMutex_);
        const auto DocIt = Documents_.find(Uri);
        const auto Source = (DocIt != Documents_.end() && DocIt->second.Virtual)
                                 ? DocIt->second.Virtual->ToSource(Result->Line, Result->Column)
                                 : std::nullopt;
        Amanuensis::Json::Insert(Location, "uri", Amanuensis::JsonValue(Uri));
        Amanuensis::Json::Insert(
            Location, "range", MakeRange(Source ? Source->first : Result->Line, Source ? Source->second : Result->Column));
    } else {
        Amanuensis::Json::Insert(Location, "uri", Amanuensis::JsonValue(PathToUri(Result->FilePath)));
        Amanuensis::Json::Insert(Location, "range", MakeRange(Result->Line, Result->Column));
    }
    Reply(Id, std::move(Location));
}

void Server::HandleSemanticTokensFull(const Amanuensis::JsonValue& Id, const Amanuensis::JsonValue& Params) {
    const std::string Uri =
        Amanuensis::Json::AsString(Amanuensis::Json::Get(Amanuensis::Json::Get(Params, "textDocument"), "uri"));

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
    Amanuensis::JsonValue Data = Amanuensis::Json::MakeArray();
    std::uint32_t     PrevLine0 = 0;
    std::uint32_t     PrevChar0 = 0;
    for (const SemanticToken& Token : Tokens) {
        const std::uint32_t Line0 = Token.Line - 1;
        const std::uint32_t Char0 = Token.Column - 1;
        const std::uint32_t DeltaLine = Line0 - PrevLine0;
        const std::uint32_t DeltaChar = (DeltaLine == 0) ? (Char0 - PrevChar0) : Char0;

        Amanuensis::Json::PushBack(Data, Amanuensis::JsonValue(static_cast<long long>(DeltaLine)));
        Amanuensis::Json::PushBack(Data, Amanuensis::JsonValue(static_cast<long long>(DeltaChar)));
        Amanuensis::Json::PushBack(Data, Amanuensis::JsonValue(static_cast<long long>(Token.Length)));
        Amanuensis::Json::PushBack(Data, Amanuensis::JsonValue(static_cast<long long>(Token.Type)));
        Amanuensis::Json::PushBack(Data, Amanuensis::JsonValue(static_cast<long long>(0))); // no token modifiers defined yet

        PrevLine0 = Line0;
        PrevChar0 = Char0;
    }

    Amanuensis::JsonValue Result = Amanuensis::Json::MakeObject();
    Amanuensis::Json::Insert(Result, "data", std::move(Data));
    Reply(Id, std::move(Result));
}

} // namespace IrisLsp
