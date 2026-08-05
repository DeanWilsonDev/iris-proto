#include "Iris/TokenizerFactory.h"

#include "Iris/CppTokenizer.h"
#include "Iris/NyxTokenizer.h"

#include <utility>

namespace Iris {

namespace {

bool HasIrisxExtension(std::string_view FilePath) {
    constexpr std::string_view Extension = ".irisx";
    return FilePath.size() >= Extension.size() &&
           FilePath.compare(FilePath.size() - Extension.size(), Extension.size(), Extension) == 0;
}

} // namespace

std::unique_ptr<IHostLanguageTokenizer> CreateHostLanguageTokenizer(std::string_view Source, std::string FilePath) {
    if (HasIrisxExtension(FilePath)) {
        return std::make_unique<NyxTokenizer>(Source, std::move(FilePath));
    }
    return std::make_unique<CppTokenizer>(Source, std::move(FilePath));
}

} // namespace Iris
