#pragma once

#include "Iris/RenderBlockParser.h"
#include "Iris/SourceLocation.h"

#include <array>
#include <cstdint>
#include <vector>

namespace IrisLsp {

// The token types this server actually emits, in legend order -- the index into this
// array *is* the wire-format tokenType each SemanticToken below carries, and the same
// array (as strings) is what Server advertises in its `semanticTokensProvider.legend`
// during initialize. Kept deliberately small: just enough to make render{}'s
// JSX-flavored syntax (invisible to the `cpp` tree-sitter grammar an editor highlights
// the rest of the file with -- docs/iris_lsp_decision.md §6) read the way JSX reads
// elsewhere, not a general-purpose C++ semantic highlighter.
enum class SemanticTokenType { Type, Property, String };
inline constexpr std::array<const char*, 3> SemanticTokenTypeNames = {"type", "property", "string"};

struct SemanticToken {
    std::uint32_t      Line;   // 1-based, matching Iris::SourceLocation's convention
    std::uint32_t      Column; // 1-based
    std::uint32_t      Length;
    SemanticTokenType Type;
};

// Walks every render{} block's already-parsed ElementNode tree (recursing into nested
// elements, including ones reached through a `!{ }` JSX-transform escape hatch's own
// JsxSegments) and returns one SemanticToken per:
//   - tag name (`Frame` in `<Frame ...>`) -- Type
//   - prop name (`class` in `class="a"`) -- Property
//   - a string-literal prop value (`"a"` in `class="a"`, quotes included) -- String
// Returned in ascending (Line, Column) order, matching LSP's own delta-encoding
// requirement for semanticTokens/full -- Server.cpp still sorts defensively rather than
// relying on that, since this is a property of the current DFS walk order, not a
// contract this function documents or promises to keep as an implementation detail.
std::vector<SemanticToken> CollectRenderBlockSemanticTokens(const std::vector<Iris::RenderBlockParser::ParsedBlock>& Blocks);

} // namespace IrisLsp
