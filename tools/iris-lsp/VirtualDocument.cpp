#include "VirtualDocument.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>

namespace IrisLsp {

namespace {

bool Before(std::uint32_t Line, std::uint32_t Column, const Iris::SourceLocation& Loc) {
    return Line < Loc.Line || (Line == Loc.Line && Column < Loc.Column);
}

} // namespace

VirtualDocument::VirtualDocument(std::string Source, std::string FilePath, Iris::IrisConfig Config,
                                  std::string ProjectRoot)
    : Source_(std::move(Source)), FilePath_(std::move(FilePath)), Config_(std::move(Config)),
      ProjectRoot_(std::move(ProjectRoot)) {
    Imports_ = Iris::ScanImports(Source_, FilePath_);

    Iris::RenderBlockParser Parser(Source_, FilePath_);
    RenderBlocks_ = Parser.Parse().Blocks;

    Result_ = Iris::CompileFile(Source_, FilePath_, Config_, ProjectRoot_);
    BuildLineSegments();
}

void VirtualDocument::BuildLineSegments() {
    Segments_.clear();
    const std::string_view Output = Result_.Output;
    std::uint32_t          GeneratedLine = 0;
    std::size_t            Pos = 0;

    while (Pos <= Output.size()) {
        const std::size_t      NewlinePos = Output.find('\n', Pos);
        const std::string_view Line =
            (NewlinePos == std::string_view::npos) ? Output.substr(Pos) : Output.substr(Pos, NewlinePos - Pos);
        ++GeneratedLine;

        constexpr std::string_view Directive = "#line ";
        if (Line.size() > Directive.size() && Line.substr(0, Directive.size()) == Directive) {
            const std::uint32_t SourceLine =
                static_cast<std::uint32_t>(std::strtoul(std::string(Line.substr(Directive.size())).c_str(), nullptr, 10));
            // The directive resyncs the line *after* itself, not its own line — standard
            // `#line` semantics, matching how Driver.cpp emits it (right after each
            // splice's replacement text, before the content that follows).
            Segments_.push_back(LineSegment{GeneratedLine + 1, SourceLine});
        }

        if (NewlinePos == std::string_view::npos) {
            break;
        }
        Pos = NewlinePos + 1;
    }
}

bool VirtualDocument::IsInsideRenderBlock(std::uint32_t Line, std::uint32_t Column) const {
    for (const Iris::RenderBlockParser::ParsedBlock& Block : RenderBlocks_) {
        if (!Before(Line, Column, Block.Location) && Before(Line, Column, Block.EndLocation)) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> VirtualDocument::ImportNameAtLine(std::uint32_t Line) const {
    for (const Iris::ImportStatement& Import : Imports_) {
        if (Import.Location.Line == Line) {
            return Import.Name;
        }
    }
    return std::nullopt;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> VirtualDocument::ToGenerated(std::uint32_t Line,
                                                                                      std::uint32_t Column) const {
    if (Segments_.empty()) {
        return std::nullopt;
    }
    // Find the last segment whose SourceStartLine is <= Line -- segments are ascending in
    // both fields (each one covers every source line up to the next splice), so this is
    // the segment Line falls in.
    const LineSegment* Owner = nullptr;
    for (const LineSegment& Seg : Segments_) {
        if (Seg.SourceStartLine <= Line) {
            Owner = &Seg;
        } else {
            break;
        }
    }
    if (Owner == nullptr) {
        return std::nullopt;
    }
    const std::uint32_t GeneratedLine = Owner->GeneratedStartLine + (Line - Owner->SourceStartLine);
    return std::make_pair(GeneratedLine, Column);
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> VirtualDocument::ToSource(std::uint32_t Line,
                                                                                    std::uint32_t Column) const {
    if (Segments_.empty()) {
        return std::nullopt;
    }
    const LineSegment* Owner = nullptr;
    for (const LineSegment& Seg : Segments_) {
        if (Seg.GeneratedStartLine <= Line) {
            Owner = &Seg;
        } else {
            break;
        }
    }
    if (Owner == nullptr) {
        return std::nullopt;
    }
    const std::uint32_t SourceLine = Owner->SourceStartLine + (Line - Owner->GeneratedStartLine);
    return std::make_pair(SourceLine, Column);
}

} // namespace IrisLsp
