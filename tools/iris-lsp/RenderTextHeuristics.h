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

// The class name inside a `class="..."` attribute value the cursor sits within, on Line, if
// any -- scans for `class="` and checks whether ColumnOneBased falls inside the quoted span.
// Distinct from TagNameAtPosition: this looks at a prop *value*, not the tag/attribute name
// itself, and only ever matches the literal `class` prop (Iris Core's own sole styling
// bridge -- ../../lustre/docs/lustre_core_spec.md §0).
std::optional<std::string> ClassPropValueAtPosition(std::string_view Line, std::uint32_t ColumnOneBased);

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

// A best-effort search for a `.ClassName { }` (or `.ClassName:pseudo { }`) selector's own
// declaration in some `.lustre` file's source text -- the same "text scan, not a real parse"
// spirit FindComponentDeclaration already uses, per docs/archive/iris_next_steps_resolved.md's
// own recorded choice (lustre_core_spec.md §1: three selector kinds, never combined, so
// `.ClassName` is always
// immediately followed by optional whitespace, then either `{` or a `:pseudo` block before
// `{`). Returns the (1-based line, column) of ClassName's own first character, or nullopt if
// no matching selector block is found (a plain text search would also be confused by a
// selector-shaped string inside a Lustre comment, the same edge case FindComponentDeclaration
// already accepts).
std::optional<std::pair<std::uint32_t, std::uint32_t>> FindClassSelector(const std::string& Text,
                                                                           const std::string& ClassName);

} // namespace IrisLsp
