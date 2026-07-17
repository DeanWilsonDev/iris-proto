#include "Iris/ImportResolver.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
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

void TestScanImportsFindsLeadingImports() {
    const auto Imports = Iris::ScanImports(R"(
import Button
import SettingsPage

IrisComponent StartMenu() {
    render { <Button /> }
}
)",
                                            "test.iris");
    Expect(Imports.size() == 2, "two import statements found");
    if (Imports.size() != 2) {
        return;
    }
    Expect(Imports[0].Name == "Button", "first import name is Button");
    Expect(Imports[1].Name == "SettingsPage", "second import name is SettingsPage");
}

void TestScanImportsIgnoresTheWordInsideAStringOrComment() {
    const auto Imports = Iris::ScanImports(R"(
// import Button
const char* Note = "import Button";
)",
                                            "test.iris");
    Expect(Imports.empty(), "'import' inside a comment or string literal is not treated as the keyword");
}

void TestScanImportsWithNoImportsIsEmpty() {
    const auto Imports = Iris::ScanImports("IrisComponent Foo() { render { <Frame /> } }", "test.iris");
    Expect(Imports.empty(), "a file with no import statements yields no results");
}

// Minimal on-disk fixture for ResolveImports() — real filesystem, cleaned up
// after each test that uses it, since resolution genuinely stats files.
class TempProject {
public:
    TempProject() {
        Root_ = std::filesystem::temp_directory_path() / "iris_import_resolver_test";
        std::filesystem::remove_all(Root_);
        std::filesystem::create_directories(Root_ / "demo");
    }

    ~TempProject() { std::filesystem::remove_all(Root_); }

    void WriteComponent(const std::string& Name, std::string_view Extension) {
        std::ofstream(Root_ / "demo" / (Name + std::string(Extension))) << "// stub component\n";
    }

    std::string RootPath() const { return Root_.string(); }

private:
    std::filesystem::path Root_;
};

void TestResolveImportsFindsFileInSearchPath() {
    TempProject Project;
    Project.WriteComponent("Button", ".iris");

    Iris::IrisConfig Config;
    Config.Target      = Iris::IrisBuildTarget::Penumbra;
    Config.SearchPaths  = {"demo"};

    const std::vector<Iris::ImportStatement> Imports = {{"Button", {}}};
    const auto                               Result  = Iris::ResolveImports(Imports, Config, Project.RootPath());

    Expect(Result.Errors.empty(), "resolving an import whose file exists produces no errors");
    Expect(Result.Resolved.size() == 1 && Result.Resolved[0].Name == "Button",
           "Button resolves successfully");
}

void TestResolveImportsReportsMissingFile() {
    TempProject Project;

    Iris::IrisConfig Config;
    Config.Target     = Iris::IrisBuildTarget::Penumbra;
    Config.SearchPaths = {"demo"};

    const std::vector<Iris::ImportStatement> Imports = {{"Missing", {}}};
    const auto                               Result  = Iris::ResolveImports(Imports, Config, Project.RootPath());

    Expect(Result.Resolved.empty(), "an import with no matching file resolves to nothing");
    Expect(Result.Errors.size() == 1, "an import with no matching file reports exactly one error");
}

void TestResolveImportsUsesIrisxExtensionForUmbraEngine() {
    TempProject Project;
    Project.WriteComponent("HudPanel", ".irisx");

    Iris::IrisConfig Config;
    Config.Target      = Iris::IrisBuildTarget::UmbraEngine;
    Config.SearchPaths  = {"demo"};

    const std::vector<Iris::ImportStatement> Imports = {{"HudPanel", {}}};
    const auto                               Result  = Iris::ResolveImports(Imports, Config, Project.RootPath());

    Expect(Result.Errors.empty(), "umbra-engine target resolves against .irisx files with no errors");
    Expect(Result.Resolved.size() == 1, "HudPanel.irisx resolves successfully");
}

void TestResolveImportsSearchesPathsInDeclarationOrder() {
    TempProject Project;
    std::filesystem::create_directories(std::filesystem::path(Project.RootPath()) / "other");
    std::ofstream(std::filesystem::path(Project.RootPath()) / "other" / "Button.iris") << "// wrong one\n";
    Project.WriteComponent("Button", ".iris"); // demo/Button.iris — should win, "demo" declared first

    Iris::IrisConfig Config;
    Config.Target      = Iris::IrisBuildTarget::Penumbra;
    Config.SearchPaths  = {"demo", "other"};

    const std::vector<Iris::ImportStatement> Imports = {{"Button", {}}};
    const auto                               Result  = Iris::ResolveImports(Imports, Config, Project.RootPath());

    Expect(Result.Resolved.size() == 1, "Button resolves to exactly one file");
    if (Result.Resolved.empty()) {
        return;
    }
    const std::string& Resolved = Result.Resolved[0].ResolvedPath;
    Expect(Resolved.find("demo") != std::string::npos && Resolved.find("other") == std::string::npos,
           "first-declared search path wins over a later one containing the same filename");
}

} // namespace

void RunImportResolverTests() {
    TestScanImportsFindsLeadingImports();
    TestScanImportsIgnoresTheWordInsideAStringOrComment();
    TestScanImportsWithNoImportsIsEmpty();
    TestResolveImportsFindsFileInSearchPath();
    TestResolveImportsReportsMissingFile();
    TestResolveImportsUsesIrisxExtensionForUmbraEngine();
    TestResolveImportsSearchesPathsInDeclarationOrder();
}
