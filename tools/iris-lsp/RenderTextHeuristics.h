#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace IrisLsp {

// Pure, no-I/O text heuristics over a `.iris` source buffer, split out of Server.cpp so
// they're directly unit-testable without spinning up a Server or feeding it JSON-RPC
// frames. None of these re-lex/re-parse the whole file the way RenderBlockParser does --
// deliberately text-based (see ClassifyRenderCompletion's own comment), since the calling
// code the whole point of them is for a cursor mid-edit, where a real reparse usually
// fails.

// Line's own text, 1-based -- Text may span multiple lines; this extracts just Line's own
// content, no trailing newline.
std::string_view LineText(std::string_view Text, std::uint32_t Line);

enum class RenderCompletionKind { None, TagName, AttributeName };

// Backward scan from the cursor within its own line: the nearest of '<' / '>' decides
// whether the cursor sits inside a still-open start tag at all, and whether whitespace
// was crossed on the way there decides tag-name-position vs attribute-name-position.
RenderCompletionKind ClassifyRenderCompletion(std::string_view Line, std::uint32_t ColumnOneBased);

// The tag name of the `<Name` (or `</Name`) the cursor sits on or inside, if any --
// scans the whole line for every `<`/`</` start, and returns the first whose identifier
// span the cursor falls within (inclusive of the `<` itself, so clicking right on it
// still counts). Used for goto-definition on a component *usage*, as opposed to
// ClassifyRenderCompletion's narrower "cursor is still typing this tag name" case.
std::optional<std::string> TagNameAtPosition(std::string_view Line, std::uint32_t ColumnOneBased);

// A best-effort search for `Name`'s own declaration line in some component's source
// text: looks for `Name` as a whole word immediately followed (optional whitespace) by
// `(` -- matches `Component Name(Props)` without needing to parse the return type, since
// Iris itself never parses component signatures either (docs/iris_core_spec.md §2.1).
// Returns the (1-based line, column) of `Name`'s own first character.
std::optional<std::pair<std::uint32_t, std::uint32_t>> FindComponentDeclaration(const std::string& Text,
                                                                                  const std::string& Name);

} // namespace IrisLsp
