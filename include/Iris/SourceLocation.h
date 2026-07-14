#pragma once

#include <cstdint>
#include <string>

namespace Iris {

// Where a Token came from in the original .iris/.irisx source — carried by
// every Token so the preprocessor can emit #line directives into generated
// output (docs/iris_core_spec.md §6) without any separate position-tracking
// pass.
struct SourceLocation {
    std::string   FilePath;
    std::uint32_t Line{1};
    std::uint32_t Column{1};
};

} // namespace Iris
