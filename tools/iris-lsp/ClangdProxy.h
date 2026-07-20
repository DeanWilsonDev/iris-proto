#pragma once

#include "HostLanguageServerProxy.h"

#include <amanuensis.hpp>

#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace IrisLsp {

// Spawns a real `clangd` as a child process and speaks LSP to it over its own
// stdin/stdout -- the same wire protocol iris-lsp itself speaks to its client
// (JsonRpc::ReadMessage/WriteMessage is reused unchanged against the child's pipes
// instead of the parent's stdio). POSIX-only (fork/exec/pipe) -- this whole tool targets
// this project's own Linux dev environment first; a Windows port would need this one
// file's process-spawning replaced, nothing else.
//
// clangd's own notifications (chiefly textDocument/publishDiagnostics) arrive on its own
// schedule, not as a reply to anything this proxy sent -- a single blocking
// request/response exchange can't observe them, so a background thread owns the only
// read from ChildStdout_ for this object's whole lifetime. Completion()/Definition()
// hand their request off to it (via PendingResults_ + PendingCv_) rather than reading
// directly.
class ClangdProxy : public IHostLanguageServerProxy {
public:
    ~ClangdProxy() override;

    bool Start(const std::string& ProjectRoot) override;
    void SetDiagnosticsCallback(ProxyDiagnosticsCallback Callback) override;
    void SyncGeneratedDocument(const std::string& GeneratedPath, const std::string& GeneratedText) override;
    std::vector<ProxyCompletionItem> Completion(const std::string& GeneratedPath, std::uint32_t Line,
                                                  std::uint32_t Column) override;
    std::optional<ProxyLocation> Definition(const std::string& GeneratedPath, std::uint32_t Line,
                                             std::uint32_t Column) override;

private:
    // Sends a request over ChildStdin_ (write side is mutex-guarded -- ReaderLoop_ never
    // writes except its own auto-replies to server->client requests, which take the same
    // lock) and blocks on PendingCv_ until ReaderLoop_ has filled in PendingResults_[Id].
    Amanuensis::Value SendRequest(const std::string& Method, Amanuensis::Value Params);
    void              SendNotification(const std::string& Method, Amanuensis::Value Params);
    void              WriteLocked(const Amanuensis::Value& Message);

    // The one thread that ever calls JsonRpc::ReadMessage(ChildStdout_). Dispatches each
    // message read: a reply to one of our own requests goes into PendingResults_ (and
    // wakes whichever SendRequest() call is waiting on it); textDocument/
    // publishDiagnostics is translated and handed to DiagnosticsCallback_; a
    // server->client request (has "id" *and* "method") gets a generic empty-result reply
    // so clangd never blocks waiting on one iris-lsp doesn't implement; anything else is
    // dropped. Runs until ChildStdout_ hits EOF (clangd exited) or Stop() is requested.
    void ReaderLoop();

    std::FILE* ChildStdin_{nullptr};
    std::FILE* ChildStdout_{nullptr};
    int        ChildPid_{-1};
    int        NextId_{1};
    bool       Started_{false};

    std::thread             ReaderThread_;
    std::mutex               WriteMutex_; // guards ChildStdin_ writes from any thread
    std::mutex               PendingMutex_;
    std::condition_variable  PendingCv_;
    std::unordered_map<int, Amanuensis::Value> PendingResults_;
    bool                     Stopping_{false};

    ProxyDiagnosticsCallback DiagnosticsCallback_;

    std::unordered_set<std::string>       OpenedDocuments_;
    std::unordered_map<std::string, int> DocumentVersions_;
};

} // namespace IrisLsp
