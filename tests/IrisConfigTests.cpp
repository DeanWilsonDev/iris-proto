#include "cimmerian/test.hpp"

#include "Iris/IrisConfig.h"

#include <string>

DESCRIBE("IrisConfig", {
    IT("parses a valid config", {
        const auto Result = Iris::ParseIrisConfig(R"({
            "target": "penumbra",
            "version": "0.1.0",
            "searchPaths": ["demo", "src/ui"]
        })");
        ASSERT_TRUE(Result.Errors.empty());       // valid .iris.json parses with no errors
        REQUIRE_TRUE(Result.Config.has_value());  // valid .iris.json yields a config

        ASSERT_TRUE(Result.Config->Target == Iris::IrisBuildTarget::Penumbra); // target is Penumbra
        ASSERT_TRUE(Result.Config->Version == "0.1.0");                       // version is captured verbatim
        ASSERT_EQUAL(Result.Config->SearchPaths.size(), static_cast<std::size_t>(2)); // both search paths captured
        ASSERT_TRUE(Result.Config->SearchPaths[0] == "demo" && Result.Config->SearchPaths[1] == "src/ui");
        // search paths preserve declaration order
    });

    IT("parses an umbra-engine target", {
        const auto Result = Iris::ParseIrisConfig(R"({"target": "umbra-engine", "version": "1.0.0", "searchPaths": []})");
        ASSERT_TRUE(Result.Errors.empty()); // umbra-engine target parses with no errors
        ASSERT_TRUE(Result.Config.has_value() && Result.Config->Target == Iris::IrisBuildTarget::UmbraEngine);
    });

    IT("reports missing searchPaths as an error", {
        const auto Result = Iris::ParseIrisConfig(R"({"target": "penumbra", "version": "0.1.0"})");
        ASSERT_FALSE(Result.Config.has_value()); // missing searchPaths yields no config
        ASSERT_TRUE(Result.Errors.size() == 1 &&
                    Result.Errors[0].Message == "`.iris.json` is missing required field `searchPaths`.");
        // missing searchPaths reports the spec §6 error message
    });

    IT("reports missing target as an error", {
        const auto Result = Iris::ParseIrisConfig(R"({"version": "0.1.0", "searchPaths": ["demo"]})");
        ASSERT_FALSE(Result.Config.has_value()); // missing target yields no config
        ASSERT_TRUE(Result.Errors.size() == 1 &&
                    Result.Errors[0].Message == "`.iris.json` is missing required field `target`.");
    });

    IT("reports an invalid target value as an error", {
        const auto Result = Iris::ParseIrisConfig(R"({"target": "unreal", "version": "0.1.0", "searchPaths": []})");
        ASSERT_FALSE(Result.Config.has_value());          // unrecognised target value yields no config
        ASSERT_EQUAL(Result.Errors.size(), static_cast<std::size_t>(1));
        // unrecognised target value reports exactly one error
    });

    IT("reports malformed JSON as an error", {
        const auto Result = Iris::ParseIrisConfig(R"({"target": "penumbra",)");
        ASSERT_FALSE(Result.Config.has_value());  // malformed JSON yields no config
        ASSERT_FALSE(Result.Errors.empty());      // malformed JSON reports at least one error
    });
});
