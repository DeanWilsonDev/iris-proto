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
# Iris preprocessor/runtime — static library. Nothing in here references
# demo/ or tests/, and nothing in here references Penumbra or any other
# backend — Iris's core is backend-agnostic by design (docs/iris_core_spec.md
# §2.5). The Penumbra backend (the IrisComponent-IR-to-widget-tree walker) is
# a separate repo, iris-penumbra-backend, which depends on this repo and on
# penumbra-proto — never the other way around.
# ---------------------------------------------------------------------------
add_library(iris STATIC
    src/Iris/CppTokenizer.cpp
    src/Iris/RenderBlockParser.cpp)

target_include_directories(iris PUBLIC include)
target_compile_features(iris PUBLIC cxx_std_23)

# ---------------------------------------------------------------------------
# Tests — smoke-test executable for now. Cimmerian (the ecosystem's own test
# framework, see /Users/deanwilson/development/projects/cimmerian) is the
# intended long-term tool per docs/iris_stage2_decision_doc.md §7, but isn't
# vendored yet — this is a plain executable asserting via a handful of
# hand-rolled checks until it is.
# ---------------------------------------------------------------------------
add_executable(iris_tests
    tests/CppTokenizerTests.cpp
    tests/RenderBlockParserTests.cpp)

target_link_libraries(iris_tests PRIVATE iris)
set_target_properties(iris_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
