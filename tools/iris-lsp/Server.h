#pragma once

#include "HostLanguageServerProxy.h"
#include "VirtualDocument.h"

#include <amanuensis.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace IrisLsp {

// The whole server: one process, one client connection over stdio, request dispatch for
// the handful of LSP methods this tool implements (docs/iris_lsp_decision.md has the
// full feature scope and rationale). Every open document rebuilds its VirtualDocument on
// each change -- Iris compiles small files fast enough (CLAUDE.md's own test-suite timing
// notes sub-millisecond per-file numbers) that debouncing wasn't worth the complexity for
// v1.
//
// Two threads touch this class: the main thread, driving Run()'s own read loop over
// stdin one message at a time, and (once a proxy with its own background reader exists --
// ClangdProxy does) whatever thread the proxy calls DiagnosticsCallback_ from. Two
// mutexes keep them from corrupting shared state: DocumentsMutex_ around every
// Documents_/GeneratedPathToUri_ access, and OutMutex_ around every write to Out_ (inside
// Reply/ReplyError/Notify) -- kept separate, not combined, so a diagnostics callback
// holding DocumentsMutex_ can still call Notify() (which only needs OutMutex_) without
// any risk of one function reacquiring a mutex it already holds.
class Server {
public:
    // Blocks reading JsonRpc messages from In until "exit" or EOF.
    void Run(std::FILE* In, std::FILE* Out);

private:
    struct OpenDocument {
        std::string                      Text;
        std::string                      ProjectRoot;
        Iris::IrisConfig                 Config;
        std::unique_ptr<VirtualDocument> Virtual;
    };

    void HandleMessage(const Amanuensis::Value& Message);
    void Reply(const Amanuensis::Value& Id, Amanuensis::Value Result);
    void ReplyError(const Amanuensis::Value& Id, int Code, const std::string& Message);
    void Notify(const std::string& Method, Amanuensis::Value Params);

    void HandleInitialize(const Amanuensis::Value& Id, const Amanuensis::Value& Params);
    void HandleDidOpen(const Amanuensis::Value& Params);
    void HandleDidChange(const Amanuensis::Value& Params);
    void HandleDidClose(const Amanuensis::Value& Params);
    void HandleCompletion(const Amanuensis::Value& Id, const Amanuensis::Value& Params);
    void HandleDefinition(const Amanuensis::Value& Id, const Amanuensis::Value& Params);

    // Rebuilds Documents_[Uri]'s VirtualDocument from Text and publishes fresh
    // diagnostics -- the one path both didOpen and didChange funnel through. Locks
    // DocumentsMutex_ itself; callers must not already hold it.
    void RebuildDocument(const std::string& Uri, std::string Text);

    // Builds the Iris::CompileFile-sourced half of a publish -- shared by the
    // immediately-after-rebuild publish (clangd hasn't replied yet) and the merged
    // republish HandleClangdDiagnostics does once it has. Caller must hold
    // DocumentsMutex_ already (Doc is a reference into Documents_).
    Amanuensis::Value BuildIrisDiagnosticsArray(const OpenDocument& Doc) const;
    void              PublishDiagnostics(const std::string& Uri, const OpenDocument& Doc);

    // ProxyDiagnosticsCallback target -- see Server.h's own class comment on the
    // threading this implies. Looks GeneratedPath back up to its owning Uri/OpenDocument,
    // position-translates every diagnostic via VirtualDocument::ToSource, merges with
    // that document's own Iris-level diagnostics, and republishes the combined set
    // (LSP's publishDiagnostics replaces a uri's previous set wholesale, so this
    // supersedes the plain-Iris publish RebuildDocument already sent -- no duplication).
    void HandleClangdDiagnostics(const std::string& GeneratedPath, std::vector<ProxyDiagnostic> Diagnostics);

    std::FILE* Out_{nullptr};
    bool       ShutdownRequested_{false};

    std::mutex DocumentsMutex_;
    std::unordered_map<std::string, OpenDocument> Documents_;
    std::unordered_map<std::string, std::string>  GeneratedPathToUri_; // guarded by DocumentsMutex_ too

    std::mutex                                OutMutex_;
    std::unique_ptr<IHostLanguageServerProxy> Proxy_;
    bool                                       ProxyStarted_{false};
};

} // namespace IrisLsp
