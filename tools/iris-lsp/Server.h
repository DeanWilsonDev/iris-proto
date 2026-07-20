#pragma once

#include "HostLanguageServerProxy.h"
#include "VirtualDocument.h"

#include <amanuensis.hpp>

#include <memory>
#include <string>
#include <unordered_map>

namespace IrisLsp {

// The whole server: one process, one client connection over stdio, request dispatch for
// the handful of LSP methods this tool implements (docs/iris_lsp_decision.md has the
// full feature scope and rationale). Every open document rebuilds its VirtualDocument on
// each change -- Iris compiles small files fast enough (CLAUDE.md's own test-suite timing
// notes sub-millisecond per-file numbers) that debouncing wasn't worth the complexity for
// v1.
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
    // diagnostics -- the one path both didOpen and didChange funnel through.
    void RebuildDocument(const std::string& Uri, std::string Text);
    void PublishDiagnostics(const std::string& Uri, const OpenDocument& Doc);

    std::FILE* Out_{nullptr};
    bool       ShutdownRequested_{false};

    std::unordered_map<std::string, OpenDocument>     Documents_;
    std::unique_ptr<IHostLanguageServerProxy>          Proxy_;
    bool                                                ProxyStarted_{false};
};

} // namespace IrisLsp
