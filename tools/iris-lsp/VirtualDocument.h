#pragma once

#include "Iris/Driver.h"
#include "Iris/ImportResolver.h"
#include "Iris/IrisConfig.h"
#include "Iris/RenderBlockParser.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace IrisLsp {

// One open `.iris`/`.irisx` buffer's compiled view, rebuilt on every edit. Wraps
// `Iris::CompileFile` — Iris's own compiler already produces a real, self-contained C++
// header with `#line` directives resyncing line numbers after every `render { }` splice
// (`Driver.cpp`'s own comment) — rather than iris-lsp inventing a second, parallel
// virtual-document/position-mapping scheme. `#line` directives are parsed back out of
// `Output` (not threaded through from `Driver` internals) so this stays decoupled from
// how `Driver` is implemented, and so the same approach ports unchanged to a future
// Nyx-targeted `.irisx` document if Nyx's own codegen resyncs line numbers the same way.
//
// A cursor position is exactly one of three things here, and each has a different owner:
//   - inside a `render { }` block -> Iris's own business (tag/prop completion); never
//     proxied to a host-language server, since it isn't host-language syntax.
//   - on an `import Name` line -> also Iris's own business (goto-definition via
//     ImportResolver); the line's *content* differs between source and generated text
//     (`import Name` vs `#include "..."`), so it has no meaningful generated-position
//     counterpart either.
//   - anywhere else -> ordinary host-language code, copied verbatim into the generated
///    text (only whole-line shifts happen around it, never same-line edits), so
//     ToGenerated/ToSource is well-defined and exact down to the column.
class VirtualDocument {
public:
    VirtualDocument(std::string Source, std::string FilePath, Iris::IrisConfig Config, std::string ProjectRoot);

    const Iris::DriverResult& CompileResult() const { return Result_; }
    const std::string&        Source() const { return Source_; }
    const std::string&        FilePath() const { return FilePath_; }
    const Iris::IrisConfig&   Config() const { return Config_; }
    const std::string&        ProjectRoot() const { return ProjectRoot_; }

    const std::vector<Iris::RenderBlockParser::ParsedBlock>& RenderBlocks() const { return RenderBlocks_; }
    const std::vector<Iris::ImportStatement>&                  Imports() const { return Imports_; }

    bool IsInsideRenderBlock(std::uint32_t Line, std::uint32_t Column) const;

    // The imported component's name if (Line, *) is an `import Name` statement line —
    // column-insensitive since the whole line is that one statement (docs/iris_core_spec.md
    // §1.2: `import` is always a whole top-level line).
    std::optional<std::string> ImportNameAtLine(std::uint32_t Line) const;

    // 1-based (line, column), same convention as Iris::SourceLocation throughout this
    // codebase (LSP's own positions are 0-based -- Server.cpp converts at the boundary,
    // not here, so this class stays consistent with the rest of Iris).
    std::optional<std::pair<std::uint32_t, std::uint32_t>> ToGenerated(std::uint32_t Line, std::uint32_t Column) const;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> ToSource(std::uint32_t Line, std::uint32_t Column) const;

private:
    struct LineSegment {
        std::uint32_t GeneratedStartLine;
        std::uint32_t SourceStartLine;
    };

    void BuildLineSegments();

    std::string       Source_;
    std::string       FilePath_;
    Iris::IrisConfig  Config_;
    std::string       ProjectRoot_;

    Iris::DriverResult                                  Result_;
    std::vector<Iris::RenderBlockParser::ParsedBlock> RenderBlocks_;
    std::vector<Iris::ImportStatement>                  Imports_;
    std::vector<LineSegment>                            Segments_; // ascending by both fields
};

} // namespace IrisLsp
