// The Iris preprocessor CLI: `.iris`/`.irisx` -> compilable `.cpp`, wrapping
// Iris::CompileFile (include/Iris/Driver.h). Usage:
//
//   iris_cc <input.iris|input.irisx> [-o <output.cpp>] [--project-root <dir>]
//
// `--project-root` defaults to the nearest ancestor directory (starting from the input
// file's own directory) containing a `.iris.json` — the same "nearest ancestor config"
// resolution convention tools like tsconfig.json use, since docs/iris_core_spec.md §5
// doesn't specify one itself (the project root is simply "the directory `.iris.json`
// lives in", with searchPaths relative to it).
//
// Without `-o`, generated source goes to stdout; diagnostics always go to stderr, one per
// line as `<file>:<line>:<col>: error: <message>`. Exit code is 0 on success, 1 on any
// diagnostic (including a missing/malformed `.iris.json`) or usage/I/O error.

#include "Iris/Driver.h"
#include "Iris/IrisConfig.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

namespace {

std::optional<std::string> ReadFile(const std::filesystem::path& Path) {
    std::ifstream Stream(Path, std::ios::binary);
    if (!Stream) {
        return std::nullopt;
    }
    std::ostringstream Buffer;
    Buffer << Stream.rdbuf();
    return Buffer.str();
}

std::optional<std::filesystem::path> FindProjectRoot(std::filesystem::path Start) {
    std::filesystem::path Dir = std::filesystem::absolute(Start);
    for (;;) {
        std::error_code Ignored;
        if (std::filesystem::is_regular_file(Dir / ".iris.json", Ignored)) {
            return Dir;
        }
        if (!Dir.has_parent_path() || Dir.parent_path() == Dir) {
            return std::nullopt;
        }
        Dir = Dir.parent_path();
    }
}

void PrintDiagnostic(const Iris::DriverDiagnostic& Diag) {
    std::cerr << Diag.Location.FilePath << ':' << Diag.Location.Line << ':' << Diag.Location.Column
              << ": error: " << Diag.Message << '\n';
}

} // namespace

int main(int Argc, char** Argv) {
    std::string InputPath;
    std::string OutputPath;
    std::string ProjectRootOverride;

    for (int Index = 1; Index < Argc; ++Index) {
        const std::string Arg = Argv[Index];
        if (Arg == "-o" && Index + 1 < Argc) {
            OutputPath = Argv[++Index];
        } else if (Arg == "--project-root" && Index + 1 < Argc) {
            ProjectRootOverride = Argv[++Index];
        } else if (InputPath.empty()) {
            InputPath = Arg;
        } else {
            std::cerr << "iris_cc: unexpected argument '" << Arg << "'\n";
            return 1;
        }
    }

    if (InputPath.empty()) {
        std::cerr << "usage: iris_cc <input.iris|input.irisx> [-o <output.cpp>] [--project-root <dir>]\n";
        return 1;
    }

    const auto Source = ReadFile(InputPath);
    if (!Source) {
        std::cerr << "iris_cc: cannot read '" << InputPath << "'\n";
        return 1;
    }

    std::filesystem::path ProjectRoot;
    if (!ProjectRootOverride.empty()) {
        ProjectRoot = ProjectRootOverride;
    } else {
        const auto Found = FindProjectRoot(std::filesystem::path(InputPath).parent_path());
        if (!Found) {
            std::cerr << "iris_cc: no .iris.json found in '" << InputPath
                      << "'s directory or any parent (pass --project-root to override)\n";
            return 1;
        }
        ProjectRoot = *Found;
    }

    const auto ConfigText = ReadFile(ProjectRoot / ".iris.json");
    if (!ConfigText) {
        std::cerr << "iris_cc: cannot read '" << (ProjectRoot / ".iris.json").string() << "'\n";
        return 1;
    }

    const Iris::IrisConfigParseResult ConfigResult = Iris::ParseIrisConfig(*ConfigText);
    if (!ConfigResult.Config.has_value()) {
        for (const Iris::IrisConfigError& Err : ConfigResult.Errors) {
            std::cerr << "iris_cc: " << (ProjectRoot / ".iris.json").string() << ": " << Err.Message << '\n';
        }
        return 1;
    }

    const Iris::DriverResult Result =
        Iris::CompileFile(*Source, InputPath, *ConfigResult.Config, ProjectRoot.string());

    if (!Result.Diagnostics.empty()) {
        for (const Iris::DriverDiagnostic& Diag : Result.Diagnostics) {
            PrintDiagnostic(Diag);
        }
        return 1;
    }

    if (OutputPath.empty()) {
        std::cout << Result.Output;
        return 0;
    }

    std::ofstream Out(OutputPath, std::ios::binary);
    if (!Out) {
        std::cerr << "iris_cc: cannot write '" << OutputPath << "'\n";
        return 1;
    }
    Out << Result.Output;
    return 0;
}
