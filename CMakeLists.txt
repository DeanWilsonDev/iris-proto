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
# Iris preprocessor/runtime — static library. Nothing in here references
# demo/ or tests/, and nothing in here references Penumbra or any other
# backend — Iris's core is backend-agnostic by design (docs/iris_core_spec.md
# §2.5). The Penumbra backend (the IrisComponent-IR-to-widget-tree walker) is
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
# Tests — smoke-test executable for now. Cimmerian (the ecosystem's own test
# framework, see /Users/deanwilson/development/projects/cimmerian) is the
# intended long-term tool per docs/iris_stage2_decision_doc.md §7, but isn't
# vendored yet — this is a plain executable asserting via a handful of
# hand-rolled checks until it is.
# ---------------------------------------------------------------------------
add_executable(iris_tests
    tests/CppTokenizerTests.cpp
    tests/RenderBlockParserTests.cpp
    tests/IrisConfigTests.cpp
    tests/ImportResolverTests.cpp
    tests/CodegenTests.cpp
    tests/IrisComponentTests.cpp
    tests/SemanticValidatorTests.cpp
    tests/DriverTests.cpp
    tests/ReconcilerTests.cpp
    tests/SlotRuntimeTests.cpp
    tests/ComponentInstanceTests.cpp
    tests/SlotResolutionTests.cpp)

target_link_libraries(iris_tests PRIVATE iris)
set_target_properties(iris_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
