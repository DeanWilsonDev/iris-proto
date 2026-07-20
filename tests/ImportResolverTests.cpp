#include "cimmerian/test.hpp"

#include "Iris/ImportResolver.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

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

// A `{...}` list literal with a top-level comma, written directly inside an IT(...)
// body, would split into extra macro arguments — the preprocessor's macro-argument
// scanner balances only `(` `)`, not `{` `}`. These helpers keep such literals inside
// an ordinary function call instead.
std::vector<Iris::ImportStatement> OneImport(const std::string& Name) {
    return {{Name, {}}};
}

std::vector<std::string> SearchPaths(std::vector<std::string> Paths) { return Paths; }

} // namespace

DESCRIBE("ImportResolver", {
    IT("ScanImports finds leading import statements", {
        const auto Imports = Iris::ScanImports(R"(
import Button
import SettingsPage

Component StartMenu() {
    render { <Button /> }
}
)",
                                                "test.iris");
        REQUIRE_EQUAL(Imports.size(), static_cast<std::size_t>(2)); // two import statements found
        ASSERT_TRUE(Imports[0].Name == "Button");       // first import name is Button
        ASSERT_TRUE(Imports[1].Name == "SettingsPage"); // second import name is SettingsPage
    });

    IT("ScanImports ignores the word inside a string or comment", {
        const auto Imports = Iris::ScanImports(R"(
// import Button
const char* Note = "import Button";
)",
                                                "test.iris");
        ASSERT_TRUE(Imports.empty()); // 'import' inside a comment or string literal is not treated as the keyword
    });

    IT("ScanImports with no imports is empty", {
        const auto Imports = Iris::ScanImports("Component Foo() { render { <Frame /> } }", "test.iris");
        ASSERT_TRUE(Imports.empty()); // a file with no import statements yields no results
    });

    IT("ResolveImports finds a file in the search path", {
        TempProject Project;
        Project.WriteComponent("Button", ".iris");

        Iris::IrisConfig Config;
        Config.Target      = Iris::IrisBuildTarget::Penumbra;
        Config.SearchPaths  = SearchPaths({"demo"});

        const std::vector<Iris::ImportStatement> Imports = OneImport("Button");
        const auto                               Result  = Iris::ResolveImports(Imports, Config, Project.RootPath());

        ASSERT_TRUE(Result.Errors.empty()); // resolving an import whose file exists produces no errors
        ASSERT_TRUE(Result.Resolved.size() == 1 && Result.Resolved[0].Name == "Button");
    });

    IT("ResolveImports reports a missing file", {
        TempProject Project;

        Iris::IrisConfig Config;
        Config.Target     = Iris::IrisBuildTarget::Penumbra;
        Config.SearchPaths = SearchPaths({"demo"});

        const std::vector<Iris::ImportStatement> Imports = OneImport("Missing");
        const auto                               Result  = Iris::ResolveImports(Imports, Config, Project.RootPath());

        ASSERT_TRUE(Result.Resolved.empty()); // an import with no matching file resolves to nothing
        ASSERT_EQUAL(Result.Errors.size(), static_cast<std::size_t>(1));
    });

    IT("ResolveImports uses the .irisx extension for the umbra-engine target", {
        TempProject Project;
        Project.WriteComponent("HudPanel", ".irisx");

        Iris::IrisConfig Config;
        Config.Target      = Iris::IrisBuildTarget::UmbraEngine;
        Config.SearchPaths  = SearchPaths({"demo"});

        const std::vector<Iris::ImportStatement> Imports = OneImport("HudPanel");
        const auto                               Result  = Iris::ResolveImports(Imports, Config, Project.RootPath());

        ASSERT_TRUE(Result.Errors.empty()); // umbra-engine target resolves against .irisx files with no errors
        ASSERT_EQUAL(Result.Resolved.size(), static_cast<std::size_t>(1));
    });

    IT("ResolveImports searches paths in declaration order", {
        TempProject Project;
        std::filesystem::create_directories(std::filesystem::path(Project.RootPath()) / "other");
        std::ofstream(std::filesystem::path(Project.RootPath()) / "other" / "Button.iris") << "// wrong one\n";
        Project.WriteComponent("Button", ".iris"); // demo/Button.iris — should win, "demo" declared first

        Iris::IrisConfig Config;
        Config.Target      = Iris::IrisBuildTarget::Penumbra;
        Config.SearchPaths  = SearchPaths({"demo", "other"});

        const std::vector<Iris::ImportStatement> Imports = OneImport("Button");
        const auto                               Result  = Iris::ResolveImports(Imports, Config, Project.RootPath());

        REQUIRE_EQUAL(Result.Resolved.size(), static_cast<std::size_t>(1)); // Button resolves to exactly one file
        const std::string& Resolved = Result.Resolved[0].ResolvedPath;
        ASSERT_TRUE(Resolved.find("demo") != std::string::npos && Resolved.find("other") == std::string::npos);
        // first-declared search path wins over a later one containing the same filename
    });
});
