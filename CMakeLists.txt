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
# Iris preprocessor/runtime — static library. Nothing in here references
# demo/ or tests/, and nothing in here references Penumbra or any other
# backend — Iris's core is backend-agnostic by design (docs/iris_core_spec.md
# §2.5). The Penumbra backend (the Component-IR-to-widget-tree walker) is
# a separate repo, iris-penumbra-backend, which depends on this repo and on
# penumbra-proto — never the other way around.
# ---------------------------------------------------------------------------
add_library(iris STATIC
    src/Iris/CppTokenizer.cpp
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

# ---------------------------------------------------------------------------
# iris_cc — the preprocessor CLI (tools/IrisCc.cpp), wrapping Iris::CompileFile
# (include/Iris/Driver.h). Ties RenderBlockParser, ImportResolver,
# SemanticValidator, and Codegen together into an actual .iris -> .cpp
# pipeline — see tools/IrisCc.cpp's header comment for invocation.
# ---------------------------------------------------------------------------
add_executable(iris_cc tools/IrisCc.cpp)
target_link_libraries(iris_cc PRIVATE iris)

# ---------------------------------------------------------------------------
# iris-lsp — the LSP server (tools/iris-lsp/), docs/iris_lsp_decision.md.
# render{}-scoped diagnostics/completion/goto-definition come straight from
# `iris`; everything outside a render{} block is proxied to a real
# host-language server (ClangdProxy for `.iris` today) through
# IHostLanguageServerProxy, kept as a portable seam for a future Nyx-targeted
# proxy over `.irisx`. POSIX-only (fork/exec) — see ClangdProxy.h.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

add_executable(iris_lsp
    tools/iris-lsp/main.cpp
    tools/iris-lsp/Server.cpp
    tools/iris-lsp/VirtualDocument.cpp
    tools/iris-lsp/ClangdProxy.cpp
    tools/iris-lsp/JsonRpc.cpp)
target_link_libraries(iris_lsp PRIVATE iris amanuensis Threads::Threads)

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
