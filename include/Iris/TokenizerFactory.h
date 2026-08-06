#pragma once

#include "Iris/IHostLanguageTokenizer.h"

#include <memory>
#include <string>
#include <string_view>

namespace Iris {

// Which host language a `.iris`/`.irisx` file's non-`render{}` content is written in --
// the same fact CreateHostLanguageTokenizer below uses to pick a tokenizer, but also needed
// by Driver::CompileFile to pick an *output* format (spliced C++23 text for Cpp, an Iris IR
// JSON document for Nyx -- docs/iris_nyx_emission_decision.md). Exposed separately from
// CreateHostLanguageTokenizer so callers that only need the language fact, not a tokenizer
// instance, don't have to construct one just to inspect it.
enum class HostLanguage {
    Cpp,
    Nyx,
};

// `.irisx` -> Nyx; every other extension (`.iris`, and anything unrecognized) -> Cpp, matching
// the default every caller already assumed before this factory existed.
HostLanguage DetermineHostLanguage(std::string_view FilePath);

// The one dispatch point IHostLanguageTokenizer.h's own doc comment promised but that didn't
// exist anywhere in code until now (docs/archive/iris_next_steps_resolved.md, "NyxTokenizer"):
// selects CppTokenizer or NyxTokenizer by FilePath's extension, per docs/iris_core_spec.md §0's
// File model ("the file extension is the sole source of truth for which host language a file
// uses"). `.irisx` -> NyxTokenizer; every other extension (`.iris`, and anything unrecognized)
// -> CppTokenizer, matching the default every caller already assumed before this factory existed.
std::unique_ptr<IHostLanguageTokenizer> CreateHostLanguageTokenizer(std::string_view Source, std::string FilePath);

} // namespace Iris
