#pragma once

#include "Iris/IHostLanguageTokenizer.h"

#include <memory>
#include <string>
#include <string_view>

namespace Iris {

// The one dispatch point IHostLanguageTokenizer.h's own doc comment promised but that didn't
// exist anywhere in code until now (docs/next-steps.md, "NyxTokenizer... PARTIALLY RESOLVED"):
// selects CppTokenizer or NyxTokenizer by FilePath's extension, per docs/iris_core_spec.md §0's
// File model ("the file extension is the sole source of truth for which host language a file
// uses"). `.irisx` -> NyxTokenizer; every other extension (`.iris`, and anything unrecognized)
// -> CppTokenizer, matching the default every caller already assumed before this factory existed.
std::unique_ptr<IHostLanguageTokenizer> CreateHostLanguageTokenizer(std::string_view Source, std::string FilePath);

} // namespace Iris
