cmake_minimum_required(VERSION 3.24)

project(Iris
    VERSION 0.1.0
    DESCRIPTION "Iris — preprocessor and reactive UI runtime over a host language"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# ---------------------------------------------------------------------------
# Amanuensis — zero-dependency first-party JSON library, vendored as a git
# submodule under libs/ (its own recommended integration path) rather than
# hand-rolling `.iris.json` parsing. This is a plain utility dependency, not a
# backend — unlike the earlier penumbra-proto submodule this repo doesn't
# vendor, using it doesn't couple Iris's core to any backend.
# ---------------------------------------------------------------------------
add_subdirectory(libs/amanuensis)

# ---------------------------------------------------------------------------
# umbra-interfaces — the backend-agnostic IWidget/IrisPropDiff/IWidgetLifecycle
# contract Stage 3's reconciler needs, vendored the same way as Amanuensis.
# Zero-dependency and names no concrete runtime or backend, so depending on it
# doesn't couple Iris's core to Penumbra or any other backend either — see
# docs/iris_stage3_implementation_decision.md.
# ---------------------------------------------------------------------------
add_subdirectory(libs/umbra-interfaces)

# ---------------------------------------------------------------------------
# Cimmerian — the ecosystem's own BDD-style test framework
# (docs/iris_stage2_decision_doc.md §7), vendored the same way as Amanuensis
# and umbra-interfaces. Forced to CIMMERIAN_VISUAL_PLATFORM=None *before*
# add_subdirectory: Cimmerian's own CMakeLists only sets that cache variable
# if it isn't already set, and its default on Linux is X11, which pulls in
# find_package(X11 REQUIRED) + libXtst — a new system dependency Iris's own
# tests have no need for (no screenshot/visual-regression testing here).
# ---------------------------------------------------------------------------
set(CIMMERIAN_VISUAL_PLATFORM "None" CACHE STRING "" FORCE)
add_subdirectory(libs/cimmerian)

# ---------------------------------------------------------------------------
# nyx-lexer — just nyx-proto's Lexer/Token translation units (libs/nyx-proto,
# a git submodule), used by NyxTokenizer (below) to reuse Nyx's real,
# already-tested tokenizer rather than reimplementing its string-escape/
# template-interpolation scanning a second time
# (nyx-proto's docs/nyx-scripting-language/decision-log.md §7.1). Deliberately
# does NOT add_subdirectory(libs/nyx-proto) and build nyx-proto's own
# CMakeLists.txt: that pulls in nyx-proto's own vendored Firefly/Amanuensis/
# Cimmerian submodules, which would define `amanuensis` and `cimmerian`
# targets a second time and collide with this repo's own libs/amanuensis and
# libs/cimmerian above. Lexer.cpp/Token.cpp have zero dependency on the rest
# of nyx-proto (no Firefly, no runtime/interpreter code — verified by
# inspection), so compiling just those two translation units sidesteps the
# collision entirely while still linking Nyx's real lexer. Built at C++26
# (nyx-proto's own required standard, per its CLAUDE.md) independent of this
# project's own CMAKE_CXX_STANDARD, since nothing else here assumes it can
# use C++26 features.
# ---------------------------------------------------------------------------
add_library(nyx-lexer STATIC
    libs/nyx-proto/src/lexer/lexer.cpp
    libs/nyx-proto/src/lexer/token.cpp)
target_include_directories(nyx-lexer PUBLIC libs/nyx-proto/src)
set_target_properties(nyx-lexer PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Iris preprocessor/runtime — static library. Nothing in here references
# demo/ or tests/, and nothing in here references Penumbra or any other
# backend — Iris's core is backend-agnostic by design (docs/iris_core_spec.md
# §2.5). The Penumbra backend (the Component-IR-to-widget-tree walker) is
# a separate repo, iris-penumbra-backend, which depends on this repo and on
# penumbra-proto — never the other way around.
# ---------------------------------------------------------------------------
add_library(iris STATIC
    src/Iris/CppTokenizer.cpp
    src/Iris/NyxTokenizer.cpp
    src/Iris/RenderBlockParser.cpp
    src/Iris/IrisConfig.cpp
    src/Iris/ImportResolver.cpp
    src/Iris/CorePrimitives.cpp
    src/Iris/Codegen.cpp
    src/Iris/SemanticValidator.cpp
    src/Iris/Driver.cpp
    src/Iris/SlotRuntime.cpp
    src/Iris/Reconciler.cpp
    src/Iris/SlotResolution.cpp)

target_include_directories(iris PUBLIC include)
target_compile_features(iris PUBLIC cxx_std_23)
target_link_libraries(iris PUBLIC amanuensis umbra_interfaces)
# nyx-lexer stays PRIVATE: NyxTokenizer.h exposes no nyx:: types in its own
# public interface (only Iris::Token, via IHostLanguageTokenizer), so
# consumers of `iris` never need libs/nyx-proto/src on their include path.
target_link_libraries(iris PRIVATE nyx-lexer)

# ---------------------------------------------------------------------------
# iris_cc — the preprocessor CLI (tools/IrisCc.cpp), wrapping Iris::CompileFile
# (include/Iris/Driver.h). Ties RenderBlockParser, ImportResolver,
# SemanticValidator, and Codegen together into an actual .iris -> .cpp
# pipeline — see tools/IrisCc.cpp's header comment for invocation.
# ---------------------------------------------------------------------------
add_executable(iris_cc tools/IrisCc.cpp)
target_link_libraries(iris_cc PRIVATE iris)

# ---------------------------------------------------------------------------
# iris_compile_directory(<target> <source-dir> <generated-header-dir>) -- CMake glue for
# consumers, wrapping the per-file add_custom_command pattern every iris_cc consumer otherwise
# hand-writes once per `.iris` file (docs/next-steps.md, "CMake helper to compile every `.iris`
# file in a directory, not one hand-written block per file"). Included after iris_cc's own
# target definition above since the function references it via $<TARGET_FILE:iris_cc>.
# ---------------------------------------------------------------------------
include(cmake/IrisCompileDirectory.cmake)

# ---------------------------------------------------------------------------
# iris-lsp — the LSP server (tools/iris-lsp/), docs/iris_lsp_decision.md.
# render{}-scoped diagnostics/completion/goto-definition come straight from
# `iris`; everything outside a render{} block is proxied to a real
# host-language server (ClangdProxy for `.iris` today) through
# IHostLanguageServerProxy, kept as a portable seam for a future Nyx-targeted
# proxy over `.irisx`. POSIX-only (fork/exec) — see ClangdProxy.h.
#
# Split into a static library (everything but main.cpp) plus a thin
# executable, the same shape `iris` itself uses for `iris_cc` — so
# iris_lsp_tests below can link the real implementation without also linking
# a second `main()`.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

add_library(iris_lsp_lib STATIC
    tools/iris-lsp/Server.cpp
    tools/iris-lsp/VirtualDocument.cpp
    tools/iris-lsp/ClangdProxy.cpp
    tools/iris-lsp/JsonRpc.cpp
    tools/iris-lsp/RenderTextHeuristics.cpp
    tools/iris-lsp/SemanticTokens.cpp)
target_include_directories(iris_lsp_lib PUBLIC tools/iris-lsp)
target_link_libraries(iris_lsp_lib PUBLIC iris amanuensis Threads::Threads)

add_executable(iris_lsp tools/iris-lsp/main.cpp)
target_link_libraries(iris_lsp PRIVATE iris_lsp_lib)

# ---------------------------------------------------------------------------
# Tests — one Cimmerian-based executable. Every test uses Cimmerian's
# DESCRIBE/IT/ASSERT_* macros with auto-registration and
# tests/TestMain.cpp's single TestRunner::RunAll() entry point (see CLAUDE.md's
# "Build and test" section). Originally split across a hand-rolled `iris_tests`
# executable (predating Cimmerian being vendored) and a separate
# `iris_cimmerian_tests`; coalesced into one binary once every file was
# migrated to Cimmerian's style.
# ---------------------------------------------------------------------------
add_executable(test_iris
    tests/TestMain.cpp
    tests/CppTokenizerTests.cpp
    tests/NyxTokenizerTests.cpp
    tests/RenderBlockParserTests.cpp
    tests/IrisConfigTests.cpp
    tests/ImportResolverTests.cpp
    tests/CodegenTests.cpp
    tests/ComponentTests.cpp
    tests/SemanticValidatorTests.cpp
    tests/DriverTests.cpp
    tests/ReconcilerTests.cpp
    tests/SlotRuntimeTests.cpp
    tests/ComponentInstanceTests.cpp
    tests/SlotResolutionTests.cpp
    tests/SlotSiblingGroupTests.cpp)

target_link_libraries(test_iris PRIVATE iris cimmerian)
set_target_properties(test_iris PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

# ---------------------------------------------------------------------------
# iris-lsp's own test suite (tools/iris-lsp/tests/), same Cimmerian shape as
# test_iris above. Covers everything that doesn't need a real clangd on
# PATH: JsonRpc framing, VirtualDocument's #line-derived position mapping,
# the RenderTextHeuristics text heuristics, and Server's request dispatch
# (diagnostics, render{}-local completion, import/tag-usage goto-definition)
# driven over its real stdio JSON-RPC protocol via in-memory streams. Proxy/
# clangd behavior (docs/iris_lsp_decision.md §4) is intentionally not
# covered here — spawning a real clangd in a unit-test suite trades
# determinism for coverage of code this repo doesn't own; that path stays
# hand-verified per the decision doc's own §7.
# ---------------------------------------------------------------------------
add_executable(iris_lsp_tests
    tools/iris-lsp/tests/TestMain.cpp
    tools/iris-lsp/tests/JsonRpcTests.cpp
    tools/iris-lsp/tests/VirtualDocumentTests.cpp
    tools/iris-lsp/tests/RenderTextHeuristicsTests.cpp
    tools/iris-lsp/tests/SemanticTokensTests.cpp
    tools/iris-lsp/tests/ServerTests.cpp)
target_link_libraries(iris_lsp_tests PRIVATE iris_lsp_lib cimmerian)
set_target_properties(iris_lsp_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools/iris-lsp/tests")
