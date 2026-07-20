#include "ClangdProxy.h"

#include "JsonRpc.h"

#include <unistd.h>
#include <sys/wait.h>

#include <cstdlib>
#include <string_view>
#include <utility>

namespace IrisLsp {

namespace {

std::string PathToFileUri(const std::string& Path) { return "file://" + Path; }

std::string FileUriToPath(const std::string& Uri) {
    constexpr std::string_view Prefix = "file://";
    if (Uri.substr(0, Prefix.size()) == Prefix) {
        return Uri.substr(Prefix.size());
    }
    return Uri;
}

Amanuensis::Value MakePosition(std::uint32_t Line, std::uint32_t Column) {
    // LSP positions are 0-based; every position in this codebase (Iris::SourceLocation,
    // VirtualDocument) is 1-based, so every crossing into clangd's own wire format
    // subtracts one, and every crossing back adds one -- this function and
    // PositionFromValue below are the only two places that conversion happens.
    Amanuensis::Value Position = Amanuensis::Value::MakeObject();
    Position.Insert("line", Amanuensis::Value(static_cast<long long>(Line - 1)));
    Position.Insert("character", Amanuensis::Value(static_cast<long long>(Column - 1)));
    return Position;
}

std::pair<std::uint32_t, std::uint32_t> PositionFromValue(const Amanuensis::Value& Position) {
    return {static_cast<std::uint32_t>(Position.Get("line").AsInteger()) + 1,
            static_cast<std::uint32_t>(Position.Get("character").AsInteger()) + 1};
}

} // namespace

ClangdProxy::~ClangdProxy() {
    if (Started_) {
        SendNotification("exit", Amanuensis::Value::MakeObject());
    }
    {
        std::lock_guard<std::mutex> Lock(PendingMutex_);
        Stopping_ = true;
    }
    PendingCv_.notify_all();
    // Closing the child's stdout-reading end unblocks ReaderLoop_'s blocking read (a
    // fread on an already-closed pipe returns 0/EOF rather than hanging), so the thread
    // is guaranteed to observe Stopping_ (or just EOF, equivalently) and return.
    if (ChildStdin_ != nullptr) {
        std::fclose(ChildStdin_);
    }
    if (ChildStdout_ != nullptr) {
        std::fclose(ChildStdout_);
    }
    if (ReaderThread_.joinable()) {
        ReaderThread_.join();
    }
    if (ChildPid_ > 0) {
        int Status = 0;
        waitpid(ChildPid_, &Status, 0);
    }
}

bool ClangdProxy::Start(const std::string& ProjectRoot) {
    int ToChild[2];   // parent writes ToChild[1] -> child reads ToChild[0] (child's stdin)
    int FromChild[2]; // child writes FromChild[1] -> parent reads FromChild[0] (child's stdout)
    if (pipe(ToChild) != 0 || pipe(FromChild) != 0) {
        return false;
    }

    const int Pid = fork();
    if (Pid < 0) {
        return false;
    }
    if (Pid == 0) {
        // Child: wire the pipe ends to stdin/stdout, close everything else, exec clangd.
        dup2(ToChild[0], STDIN_FILENO);
        dup2(FromChild[1], STDOUT_FILENO);
        close(ToChild[0]);
        close(ToChild[1]);
        close(FromChild[0]);
        close(FromChild[1]);
        execlp("clangd", "clangd", "--background-index=false", static_cast<char*>(nullptr));
        std::_Exit(127); // exec failed -- clangd isn't on PATH
    }

    // Parent.
    close(ToChild[0]);
    close(FromChild[1]);
    ChildStdin_ = fdopen(ToChild[1], "w");
    ChildStdout_ = fdopen(FromChild[0], "r");
    ChildPid_ = Pid;
    if (ChildStdin_ == nullptr || ChildStdout_ == nullptr) {
        return false;
    }

    // The reader thread must be running before the first SendRequest() blocks waiting on
    // it -- started here, ahead of the initialize handshake below.
    ReaderThread_ = std::thread(&ClangdProxy::ReaderLoop, this);

    Amanuensis::Value Params = Amanuensis::Value::MakeObject();
    Params.Insert("processId", Amanuensis::Value());
    Params.Insert("rootUri", Amanuensis::Value(PathToFileUri(ProjectRoot)));
    Params.Insert("capabilities", Amanuensis::Value::MakeObject());
    const Amanuensis::Value InitResult = SendRequest("initialize", std::move(Params));
    // clangd exiting/failing to start surfaces here as a read failure -- SendRequest
    // returns a null Value in that case (see ReaderLoop_'s own EOF handling), which
    // IsNull() catches without needing a separate "did the process actually start" check.
    if (InitResult.IsNull()) {
        return false;
    }
    SendNotification("initialized", Amanuensis::Value::MakeObject());
    Started_ = true;
    return true;
}

void ClangdProxy::SetDiagnosticsCallback(ProxyDiagnosticsCallback Callback) { DiagnosticsCallback_ = std::move(Callback); }

void ClangdProxy::SyncGeneratedDocument(const std::string& GeneratedPath, const std::string& GeneratedText) {
    if (!Started_) {
        return;
    }
    const std::string Uri = PathToFileUri(GeneratedPath);

    if (OpenedDocuments_.count(GeneratedPath) == 0) {
        OpenedDocuments_.insert(GeneratedPath);
        DocumentVersions_[GeneratedPath] = 1;

        Amanuensis::Value TextDocument = Amanuensis::Value::MakeObject();
        TextDocument.Insert("uri", Amanuensis::Value(Uri));
        TextDocument.Insert("languageId", Amanuensis::Value("cpp"));
        TextDocument.Insert("version", Amanuensis::Value(1));
        TextDocument.Insert("text", Amanuensis::Value(GeneratedText));

        Amanuensis::Value Params = Amanuensis::Value::MakeObject();
        Params.Insert("textDocument", std::move(TextDocument));
        SendNotification("textDocument/didOpen", std::move(Params));
        return;
    }

    const int NewVersion = ++DocumentVersions_[GeneratedPath];
    Amanuensis::Value TextDocument = Amanuensis::Value::MakeObject();
    TextDocument.Insert("uri", Amanuensis::Value(Uri));
    TextDocument.Insert("version", Amanuensis::Value(NewVersion));

    // Full-document sync (one change covering the whole text, no `range`) -- simplest
    // correct option, and cheap enough at the sizes a single `.iris` file reaches; an
    // incremental-sync optimisation is a pure performance follow-up, not a correctness
    // one, if a file ever gets large enough for it to matter.
    Amanuensis::Value Change = Amanuensis::Value::MakeObject();
    Change.Insert("text", Amanuensis::Value(GeneratedText));
    Amanuensis::Value Changes = Amanuensis::Value::MakeArray();
    Changes.PushBack(std::move(Change));

    Amanuensis::Value Params = Amanuensis::Value::MakeObject();
    Params.Insert("textDocument", std::move(TextDocument));
    Params.Insert("contentChanges", std::move(Changes));
    SendNotification("textDocument/didChange", std::move(Params));
}

std::vector<ProxyCompletionItem> ClangdProxy::Completion(const std::string& GeneratedPath, std::uint32_t Line,
                                                            std::uint32_t Column) {
    std::vector<ProxyCompletionItem> Items;
    if (!Started_) {
        return Items;
    }

    Amanuensis::Value TextDocument = Amanuensis::Value::MakeObject();
    TextDocument.Insert("uri", Amanuensis::Value(PathToFileUri(GeneratedPath)));

    Amanuensis::Value Params = Amanuensis::Value::MakeObject();
    Params.Insert("textDocument", std::move(TextDocument));
    Params.Insert("position", MakePosition(Line, Column));

    const Amanuensis::Value Result = SendRequest("textDocument/completion", std::move(Params));
    // The result is either a bare CompletionItem[] or a CompletionList {isIncomplete,
    // items} -- both branches handled since clangd's response shape depends on whether
    // the list is complete, not on anything this caller controls.
    const Amanuensis::Value* ItemArray = &Result;
    if (Result.IsObject() && Result.Contains("items")) {
        ItemArray = &Result.Get("items");
    }
    if (!ItemArray->IsArray()) {
        return Items;
    }
    for (std::size_t Index = 0; Index < ItemArray->Size(); ++Index) {
        const Amanuensis::Value& Item = ItemArray->At(Index);
        ProxyCompletionItem      Mapped;
        Mapped.Label = Item.Contains("label") ? Item.Get("label").AsString() : std::string{};
        if (Item.Contains("kind")) {
            Mapped.Kind = static_cast<int>(Item.Get("kind").AsInteger());
        }
        if (Item.Contains("detail")) {
            Mapped.Detail = Item.Get("detail").AsString();
        }
        Items.push_back(std::move(Mapped));
    }
    return Items;
}

std::optional<ProxyLocation> ClangdProxy::Definition(const std::string& GeneratedPath, std::uint32_t Line,
                                                       std::uint32_t Column) {
    if (!Started_) {
        return std::nullopt;
    }

    Amanuensis::Value TextDocument = Amanuensis::Value::MakeObject();
    TextDocument.Insert("uri", Amanuensis::Value(PathToFileUri(GeneratedPath)));

    Amanuensis::Value Params = Amanuensis::Value::MakeObject();
    Params.Insert("textDocument", std::move(TextDocument));
    Params.Insert("position", MakePosition(Line, Column));

    const Amanuensis::Value Result = SendRequest("textDocument/definition", std::move(Params));
    // Either a single Location, a Location[], or LocationLink[] -- only the first two are
    // handled for v1 (LocationLink is clangd's richer form when the client advertises
    // support for it; this proxy's own initialize capabilities don't, so clangd shouldn't
    // send it, but a future capability bump would need a third branch here).
    const Amanuensis::Value* Loc = &Result;
    if (Result.IsArray()) {
        if (Result.Size() == 0) {
            return std::nullopt;
        }
        Loc = &Result.At(0);
    }
    if (!Loc->IsObject() || !Loc->Contains("uri") || !Loc->Contains("range")) {
        return std::nullopt;
    }

    const auto [Line1, Column1] = PositionFromValue(Loc->Get("range").Get("start"));
    return ProxyLocation{FileUriToPath(Loc->Get("uri").AsString()), Line1, Column1};
}

void ClangdProxy::WriteLocked(const Amanuensis::Value& Message) {
    std::lock_guard<std::mutex> Lock(WriteMutex_);
    JsonRpc::WriteMessage(ChildStdin_, Message);
}

Amanuensis::Value ClangdProxy::SendRequest(const std::string& Method, Amanuensis::Value Params) {
    const int Id = NextId_++;

    Amanuensis::Value Message = Amanuensis::Value::MakeObject();
    Message.Insert("jsonrpc", Amanuensis::Value("2.0"));
    Message.Insert("id", Amanuensis::Value(Id));
    Message.Insert("method", Amanuensis::Value(Method));
    Message.Insert("params", std::move(Params));
    WriteLocked(Message);

    std::unique_lock<std::mutex> Lock(PendingMutex_);
    PendingCv_.wait(Lock, [&] { return Stopping_ || PendingResults_.count(Id) != 0; });
    if (PendingResults_.count(Id) == 0) {
        return Amanuensis::Value(); // Stopping_ became true (clangd exited) before a reply arrived
    }
    Amanuensis::Value Result = std::move(PendingResults_[Id]);
    PendingResults_.erase(Id);
    return Result;
}

void ClangdProxy::SendNotification(const std::string& Method, Amanuensis::Value Params) {
    Amanuensis::Value Message = Amanuensis::Value::MakeObject();
    Message.Insert("jsonrpc", Amanuensis::Value("2.0"));
    Message.Insert("method", Amanuensis::Value(Method));
    Message.Insert("params", std::move(Params));
    WriteLocked(Message);
}

void ClangdProxy::ReaderLoop() {
    for (;;) {
        const std::optional<Amanuensis::Value> Message = JsonRpc::ReadMessage(ChildStdout_);
        if (!Message) {
            std::lock_guard<std::mutex> Lock(PendingMutex_);
            Stopping_ = true;
            PendingCv_.notify_all();
            return;
        }
        if (!Message->IsObject()) {
            continue;
        }

        const bool HasId = Message->Contains("id");
        const bool HasMethod = Message->Contains("method");

        if (HasId && !HasMethod) {
            // A reply to one of our own requests.
            if (Message->Get("id").IsInteger()) {
                const int Id = static_cast<int>(Message->Get("id").AsInteger());
                Amanuensis::Value Result = Message->Contains("result") ? Message->Get("result") : Amanuensis::Value();
                {
                    std::lock_guard<std::mutex> Lock(PendingMutex_);
                    PendingResults_[Id] = std::move(Result);
                }
                PendingCv_.notify_all();
            }
            continue;
        }

        if (HasMethod) {
            const std::string MethodName = Message->Get("method").AsString();
            if (MethodName == "textDocument/publishDiagnostics" && DiagnosticsCallback_ && Message->Contains("params")) {
                const Amanuensis::Value& Params = Message->Get("params");
                const std::string        GeneratedPath = FileUriToPath(Params.Get("uri").AsString());
                std::vector<ProxyDiagnostic> Diagnostics;
                if (Params.Contains("diagnostics") && Params.Get("diagnostics").IsArray()) {
                    const Amanuensis::Value& RawList = Params.Get("diagnostics");
                    for (std::size_t Index = 0; Index < RawList.Size(); ++Index) {
                        const Amanuensis::Value& Raw = RawList.At(Index);
                        if (!Raw.IsObject() || !Raw.Contains("range") || !Raw.Contains("message")) {
                            continue;
                        }
                        const auto [SLine, SCol] = PositionFromValue(Raw.Get("range").Get("start"));
                        const auto [ELine, ECol] = PositionFromValue(Raw.Get("range").Get("end"));
                        ProxyDiagnostic Diag;
                        Diag.StartLine = SLine;
                        Diag.StartColumn = SCol;
                        Diag.EndLine = ELine;
                        Diag.EndColumn = ECol;
                        Diag.Severity = Raw.Contains("severity") ? static_cast<int>(Raw.Get("severity").AsInteger()) : 1;
                        Diag.Message = Raw.Get("message").AsString();
                        Diagnostics.push_back(std::move(Diag));
                    }
                }
                DiagnosticsCallback_(GeneratedPath, std::move(Diagnostics));
            }

            if (HasId) {
                // A server->client *request* -- clangd is waiting on a reply. Not
                // implementing e.g. workspace/configuration properly would otherwise
                // stall it indefinitely; a generic empty result unblocks it instead.
                Amanuensis::Value Reply = Amanuensis::Value::MakeObject();
                Reply.Insert("jsonrpc", Amanuensis::Value("2.0"));
                Reply.Insert("id", Message->Get("id"));
                Reply.Insert("result", Amanuensis::Value());
                WriteLocked(Reply);
            }
        }
    }
}

std::unique_ptr<IHostLanguageServerProxy> CreateHostLanguageServerProxy(const std::string& FileExtension) {
    if (FileExtension == ".iris") {
        return std::make_unique<ClangdProxy>();
    }
    // ".irisx" -> NyxLspProxy, once Nyx and its own LSP exist. Returning nullptr rather
    // than silently falling back to ClangdProxy: Nyx is not C++, so that would be a wrong
    // answer dressed up as a working one, not a harmless default.
    return nullptr;
}

} // namespace IrisLsp
