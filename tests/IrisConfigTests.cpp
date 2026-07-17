#include "Iris/IrisConfig.h"

#include <cstdio>
#include <string>

extern int Failures; // defined in CppTokenizerTests.cpp

namespace {

void Expect(bool Condition, const std::string& Description) {
    if (Condition) {
        std::printf("[PASS] %s\n", Description.c_str());
    } else {
        std::printf("[FAIL] %s\n", Description.c_str());
        ++Failures;
    }
}

void TestValidConfigParses() {
    const auto Result = Iris::ParseIrisConfig(R"({
        "target": "penumbra",
        "version": "0.1.0",
        "searchPaths": ["demo", "src/ui"]
    })");
    Expect(Result.Errors.empty(), "valid .iris.json parses with no errors");
    Expect(Result.Config.has_value(), "valid .iris.json yields a config");
    if (!Result.Config) {
        return;
    }
    Expect(Result.Config->Target == Iris::IrisBuildTarget::Penumbra, "target is Penumbra");
    Expect(Result.Config->Version == "0.1.0", "version is captured verbatim");
    Expect(Result.Config->SearchPaths.size() == 2, "both search paths are captured");
    Expect(Result.Config->SearchPaths[0] == "demo" && Result.Config->SearchPaths[1] == "src/ui",
           "search paths preserve declaration order");
}

void TestUmbraEngineTarget() {
    const auto Result = Iris::ParseIrisConfig(R"({"target": "umbra-engine", "version": "1.0.0", "searchPaths": []})");
    Expect(Result.Errors.empty(), "umbra-engine target parses with no errors");
    Expect(Result.Config.has_value() && Result.Config->Target == Iris::IrisBuildTarget::UmbraEngine,
           "target is UmbraEngine");
}

void TestMissingSearchPathsIsAnError() {
    const auto Result = Iris::ParseIrisConfig(R"({"target": "penumbra", "version": "0.1.0"})");
    Expect(!Result.Config.has_value(), "missing searchPaths yields no config");
    Expect(Result.Errors.size() == 1 && Result.Errors[0].Message ==
                                             "`.iris.json` is missing required field `searchPaths`.",
           "missing searchPaths reports the spec §6 error message");
}

void TestMissingTargetIsAnError() {
    const auto Result = Iris::ParseIrisConfig(R"({"version": "0.1.0", "searchPaths": ["demo"]})");
    Expect(!Result.Config.has_value(), "missing target yields no config");
    Expect(Result.Errors.size() == 1 && Result.Errors[0].Message == "`.iris.json` is missing required field `target`.",
           "missing target reports an error");
}

void TestInvalidTargetValueIsAnError() {
    const auto Result = Iris::ParseIrisConfig(R"({"target": "unreal", "version": "0.1.0", "searchPaths": []})");
    Expect(!Result.Config.has_value(), "unrecognised target value yields no config");
    Expect(Result.Errors.size() == 1, "unrecognised target value reports exactly one error");
}

void TestMalformedJsonIsAnError() {
    const auto Result = Iris::ParseIrisConfig(R"({"target": "penumbra",)");
    Expect(!Result.Config.has_value(), "malformed JSON yields no config");
    Expect(!Result.Errors.empty(), "malformed JSON reports at least one error");
}

} // namespace

void RunIrisConfigTests() {
    TestValidConfigParses();
    TestUmbraEngineTarget();
    TestMissingSearchPathsIsAnError();
    TestMissingTargetIsAnError();
    TestInvalidTargetValueIsAnError();
    TestMalformedJsonIsAnError();
}
