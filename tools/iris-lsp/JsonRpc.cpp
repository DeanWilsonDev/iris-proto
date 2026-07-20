#include "JsonRpc.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace IrisLsp {

namespace {

// Reads one header line (without the trailing "\r\n"), or std::nullopt on EOF.
std::optional<std::string> ReadHeaderLine(std::FILE* In) {
    std::string Line;
    int         C;
    while ((C = std::fgetc(In)) != EOF) {
        if (C == '\n') {
            if (!Line.empty() && Line.back() == '\r') {
                Line.pop_back();
            }
            return Line;
        }
        Line.push_back(static_cast<char>(C));
    }
    return Line.empty() ? std::nullopt : std::optional<std::string>(Line);
}

} // namespace

std::optional<Amanuensis::Value> JsonRpc::ReadMessage(std::FILE* In) {
    std::size_t ContentLength = 0;
    bool        SawContentLength = false;

    for (;;) {
        const std::optional<std::string> Line = ReadHeaderLine(In);
        if (!Line) {
            return std::nullopt; // EOF before a full header — connection is over.
        }
        if (Line->empty()) {
            break; // blank line ends the header block
        }
        constexpr std::string_view Prefix = "Content-Length:";
        if (Line->size() > Prefix.size() && std::string_view(*Line).substr(0, Prefix.size()) == Prefix) {
            std::size_t ValueStart = Prefix.size();
            while (ValueStart < Line->size() && (*Line)[ValueStart] == ' ') {
                ++ValueStart;
            }
            ContentLength = static_cast<std::size_t>(std::strtoull(Line->c_str() + ValueStart, nullptr, 10));
            SawContentLength = true;
        }
        // Any other header (e.g. Content-Type) is accepted and ignored — LSP only ever
        // sends Content-Length in practice, but nothing here depends on that.
    }

    if (!SawContentLength) {
        return std::nullopt; // malformed frame — no recoverable way to resync
    }

    std::string Body(ContentLength, '\0');
    if (ContentLength > 0 && std::fread(Body.data(), 1, ContentLength, In) != ContentLength) {
        return std::nullopt; // short read — stream closed mid-frame
    }

    const Amanuensis::ParseResult Parsed = Amanuensis::Reader::ParseString(Body);
    if (!Parsed.succeeded) {
        return std::nullopt;
    }
    return Parsed.value;
}

void JsonRpc::WriteMessage(std::FILE* Out, const Amanuensis::Value& Message) {
    Amanuensis::WriterOptions Options;
    Options.pretty = false;
    Options.trailingNewline = false;
    const std::string Body = Amanuensis::Writer::WriteToString(Message, Options);

    std::fprintf(Out, "Content-Length: %zu\r\n\r\n", Body.size());
    std::fwrite(Body.data(), 1, Body.size(), Out);
    std::fflush(Out);
}

} // namespace IrisLsp
