#pragma once

#include <amanuensis.hpp>

#include <cstdio>
#include <optional>
#include <string>

namespace IrisLsp {

// LSP's wire format: `Content-Length: N\r\n\r\n<N bytes of JSON>`, one JSON-RPC 2.0
// message per frame (https://microsoft.github.io/language-server-protocol, "Base
// Protocol"). This is the only transport LSP defines for a stdio server — no
// language-specific framing beyond this header. Reading/writing are static functions
// over plain C `FILE*` (stdin/stdout in practice) rather than a class: there is exactly
// one connection for the lifetime of this process, so there is no state to hold beyond
// the streams themselves, which the caller already owns.
class JsonRpc {
public:
    // Blocks until one full frame has been read, or returns std::nullopt on EOF/a
    // malformed header (a real client never sends the latter; treated as "connection is
    // over" rather than a recoverable per-message error, same as EOF).
    static std::optional<Amanuensis::Value> ReadMessage(std::FILE* In);

    // Writes one frame and flushes — LSP has no batching/pipelining requirement, and a
    // buffered-but-unflushed stdout would hang a client waiting on a response.
    static void WriteMessage(std::FILE* Out, const Amanuensis::Value& Message);
};

} // namespace IrisLsp
