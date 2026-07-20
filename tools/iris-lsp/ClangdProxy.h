#pragma once

#include "HostLanguageServerProxy.h"

#include <amanuensis.hpp>

#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace IrisLsp {

// Spawns a real `clangd` as a child process and speaks LSP to it over its own
// stdin/stdout -- the same wire protocol iris-lsp itself speaks to its client
// (JsonRpc::ReadMessage/WriteMessage is reused unchanged against the child's pipes
// instead of the parent's stdio). POSIX-only (fork/exec/pipe) -- this whole tool targets
// this project's own Linux dev environment first; a Windows port would need this one
// file's process-spawning replaced, nothing else.
class ClangdProxy : public IHostLanguageServerProxy {
public:
    ~ClangdProxy() override;

    bool Start(const std::string& ProjectRoot) override;
    void SyncGeneratedDocument(const std::string& GeneratedPath, const std::string& GeneratedText) override;
    std::vector<ProxyCompletionItem> Completion(const std::string& GeneratedPath, std::uint32_t Line,
                                                  std::uint32_t Column) override;
    std::optional<ProxyLocation> Definition(const std::string& GeneratedPath, std::uint32_t Line,
                                             std::uint32_t Column) override;

private:
    // Sends a request and blocks until the response with a matching id arrives, dropping
    // any interleaved server->client notifications (clangd sends e.g.
    // textDocument/publishDiagnostics unprompted, which this proxy has no client to
    // forward them to yet -- a known v1 gap, not a correctness bug: iris-lsp's own
    // diagnostics already come from Iris::CompileFile directly, so losing clangd's
    // diagnostics here doesn't regress anything this tool currently promises). A
    // server->client *request* (needing a reply) is answered with a generic empty-result
    // response rather than left to hang clangd indefinitely.
    Amanuensis::Value SendRequest(const std::string& Method, Amanuensis::Value Params);
    void              SendNotification(const std::string& Method, Amanuensis::Value Params);

    std::FILE* ChildStdin_{nullptr};
    std::FILE* ChildStdout_{nullptr};
    int        ChildPid_{-1};
    int        NextId_{1};
    bool       Started_{false};

    std::unordered_set<std::string> OpenedDocuments_;
    std::unordered_map<std::string, int> DocumentVersions_;
};

} // namespace IrisLsp
