#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace IrisLsp {

struct ProxyCompletionItem {
    std::string                Label;
    std::optional<int>         Kind; // raw LSP CompletionItemKind number, passed through as-is
    std::optional<std::string> Detail;
};

struct ProxyLocation {
    std::string   FilePath; // absolute path
    std::uint32_t Line;     // 1-based, matching Iris::SourceLocation's convention
    std::uint32_t Column;
};

// One diagnostic reported against GeneratedPath, in GeneratedPath's own coordinates
// (1-based, same convention as everywhere else in this tool) -- the caller (Server.cpp,
// via VirtualDocument::ToSource) is responsible for translating both ends of the range
// back to `.iris` source positions and deciding what to do with a diagnostic whose range
// doesn't map to any source line (e.g. one pointing at the synthesized `#pragma once`
// prologue) before ever publishing it to the real client.
struct ProxyDiagnostic {
    std::uint32_t StartLine;
    std::uint32_t StartColumn;
    std::uint32_t EndLine;
    std::uint32_t EndColumn;
    int            Severity; // raw LSP DiagnosticSeverity number (1 Error .. 4 Hint)
    std::string    Message;
};

// Called whenever the underlying server pushes fresh diagnostics for GeneratedPath --
// unprompted, not in response to any one request, the same way textDocument/
// publishDiagnostics itself works. May be invoked from a background thread (ClangdProxy
// does, since clangd's notifications arrive asynchronously on its own schedule, not
// synchronously within a Completion()/Definition() call) -- implementations must be
// safe to call concurrently with any other IHostLanguageServerProxy method.
using ProxyDiagnosticsCallback =
    std::function<void(const std::string& GeneratedPath, std::vector<ProxyDiagnostic> Diagnostics)>;

// The portable seam this whole tool exists to isolate: everywhere outside a `render { }`
// block is ordinary host-language code (docs/iris_core_spec.md's own framing -- Iris only
// owns the `render { }` grammar), and Iris deliberately never parses it
// (`Iris::CppTokenizer`'s own doc comment: "does not tokenize the full C++ grammar ...
// validating is the host C++ compiler's job, never Iris's"). Real completion/definition
// for that code has to come from a real language server for whatever the host language
// is -- clangd for C++23 (`.iris`) today, and, per `Iris::IHostLanguageTokenizer`'s own
// "one concrete implementation per host language, selected by file extension" precedent
// (CLAUDE.md's Architecture section), a Nyx-targeted server for `.irisx` once Nyx and its
// own LSP exist. Server.cpp talks to exactly one `IHostLanguageServerProxy` and never to a
// concrete server directly, so adding that later is a new class plus one branch in
// CreateHostLanguageServerProxy() below, not a rewrite of Server.cpp's request dispatch.
class IHostLanguageServerProxy {
public:
    virtual ~IHostLanguageServerProxy() = default;

    // Starts the underlying server (if it's an external process) and completes its own
    // initialize handshake. Called once per iris-lsp process. Returns false if the
    // underlying server couldn't be started (e.g. clangd isn't on PATH) -- Server.cpp
    // treats that as "no host-language intelligence available this session", not a fatal
    // error, since render{}-scoped features still work without it.
    virtual bool Start(const std::string& ProjectRoot) = 0;

    // Registers the one callback this proxy delivers unprompted diagnostics through --
    // call before Start() so nothing sent immediately after the initialize handshake is
    // missed. A no-op default: a proxy that never produces diagnostics on its own (there
    // isn't one yet, but the interface shouldn't force every future implementation to
    // provide one) simply never calls it.
    virtual void SetDiagnosticsCallback(ProxyDiagnosticsCallback /*Callback*/) {}

    // Keeps the underlying server's view of GeneratedPath in sync with the latest
    // VirtualDocument rebuild -- the proxy equivalent of textDocument/didOpen (first call
    // for a given path) or didChange (every call after).
    virtual void SyncGeneratedDocument(const std::string& GeneratedPath, const std::string& GeneratedText) = 0;

    // Line/Column are 1-based positions into GeneratedText as last synced -- the caller
    // (Server.cpp, via VirtualDocument::ToGenerated) is responsible for translating from
    // the original `.iris` source position first; this interface never sees `.iris`
    // positions or knows `.iris` files exist at all, which is what keeps it portable.
    virtual std::vector<ProxyCompletionItem> Completion(const std::string& GeneratedPath, std::uint32_t Line,
                                                          std::uint32_t Column) = 0;
    virtual std::optional<ProxyLocation> Definition(const std::string& GeneratedPath, std::uint32_t Line,
                                                     std::uint32_t Column) = 0;
};

// `.iris` -> a clangd-backed proxy today. `.irisx` has no implementation yet and returns
// nullptr -- same "not yet designed, don't fake it" treatment
// `include/Iris/IrisElementTag.h`'s own `<Model3d>` comment gives Stage 6 concepts
// elsewhere in this codebase. Add a NyxLspProxy branch here once Nyx's own LSP exists.
std::unique_ptr<IHostLanguageServerProxy> CreateHostLanguageServerProxy(const std::string& FileExtension);

} // namespace IrisLsp
