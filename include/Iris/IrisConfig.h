#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Iris {

// Which backend a project targets — determines which backend-gated Core
// primitives are unlocked (docs/iris_core_spec.md §3.2, §5) and which
// extension `import` resolution looks for (§1.2: `.iris` for a Penumbra/C++23
// project, `.irisx` for a future Nyx-targeted one).
enum class IrisBuildTarget {
    Penumbra,
    UmbraEngine,
};

// The parsed, validated form of a project's `.iris.json` (docs/iris_core_spec.md
// §5). All three fields are required by the spec; ParseIrisConfig() rejects a
// document missing any of them rather than defaulting silently.
struct IrisConfig {
    IrisBuildTarget          Target{IrisBuildTarget::Penumbra};
    std::string              Version;
    std::vector<std::string> SearchPaths;
};

struct IrisConfigError {
    std::string Message;
};

struct IrisConfigParseResult {
    std::optional<IrisConfig>    Config;
    std::vector<IrisConfigError> Errors;
};

// Hand-rolled, scoped only to `.iris.json`'s fixed three-field schema
// (docs/iris_core_spec.md §8 open question, resolved in favor of not vendoring
// a general-purpose JSON library for three fields). Still a real recursive-
// descent JSON parser internally so it isn't thrown by whitespace/formatting
// variation — it just doesn't expose or retain a general JSON value type.
IrisConfigParseResult ParseIrisConfig(std::string_view JsonText);

} // namespace Iris
