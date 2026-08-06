# iris_compile_directory(<target> <source-dir> <generated-header-dir>)
#
# Globs every `<source-dir>/*.iris` and `<source-dir>/*.irisx` file and emits one
# add_custom_command per file, mirroring the per-file `add_custom_command` block a consumer
# would otherwise hand-write once per file (see pharos-proto/CMakeLists.txt's own pre-existing
# pattern this generates):
#
# - `Name.iris` compiles via `iris_cc` into `<generated-header-dir>/Name.iris.h`, an
#   `#include`-able header — added to `<target>`'s dependencies *and* include path, same as
#   before `.irisx` support existed.
# - `Name.irisx` compiles via `iris_cc` into `<generated-header-dir>/Name.iris.ir` — an Iris IR
#   JSON document (docs/iris_nyx_emission_decision.md; `Driver::CompileFile`'s `.irisx` output),
#   not a header, so it's added to `<target>`'s dependencies (it still needs to be generated
#   before `<target>` builds, for whatever eventually consumes it) but not `#include`d by
#   anything — `target_include_directories` below is harmless for it (an unreferenced JSON file
#   sitting in an include path), not a claim that it's meant to be included.
#
# `<source-dir>` is expected to contain a `.iris.json` (docs/iris_core_spec.md §5) alongside its
# `.iris`/`.irisx` files, the same convention `iris_cc`'s own `--project-root` auto-resolution
# assumes (tools/IrisCc.cpp); it's listed as a dependency of every generated output so editing
# `searchPaths`/`target` triggers a recompile.
#
# Uses plain `file(GLOB ...)`, not `CONFIGURE_DEPENDS` -- like `pharos-proto/CMakeLists.txt`'s
# own `file(GLOB_RECURSE PHAROS_LIB_SOURCES ...)`, adding or removing a `.iris`/`.irisx` file
# needs a fresh `cmake -B build`, not just a rebuild. Documented, accepted limitation
# (docs/next-steps.md, "CMake helper to compile every `.iris` file in a directory").
function(iris_compile_directory Target SourceDir GeneratedHeaderDir)
    file(GLOB IrisSources "${SourceDir}/*.iris")
    file(GLOB IrisxSources "${SourceDir}/*.irisx")

    set(GeneratedOutputs "")
    foreach(IrisSource ${IrisSources})
        get_filename_component(IrisName "${IrisSource}" NAME_WE)
        set(GeneratedHeader "${GeneratedHeaderDir}/${IrisName}.iris.h")
        add_custom_command(
            OUTPUT "${GeneratedHeader}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${GeneratedHeaderDir}"
            COMMAND $<TARGET_FILE:iris_cc> "${IrisSource}" -o "${GeneratedHeader}"
            DEPENDS iris_cc "${IrisSource}" "${SourceDir}/.iris.json"
            COMMENT "iris_cc: compiling ${IrisName}.iris")
        list(APPEND GeneratedOutputs "${GeneratedHeader}")
    endforeach()

    foreach(IrisxSource ${IrisxSources})
        get_filename_component(IrisxName "${IrisxSource}" NAME_WE)
        set(GeneratedIr "${GeneratedHeaderDir}/${IrisxName}.iris.ir")
        add_custom_command(
            OUTPUT "${GeneratedIr}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${GeneratedHeaderDir}"
            COMMAND $<TARGET_FILE:iris_cc> "${IrisxSource}" -o "${GeneratedIr}"
            DEPENDS iris_cc "${IrisxSource}" "${SourceDir}/.iris.json"
            COMMENT "iris_cc: compiling ${IrisxName}.irisx")
        list(APPEND GeneratedOutputs "${GeneratedIr}")
    endforeach()

    add_custom_target(${Target}_generate_iris DEPENDS ${GeneratedOutputs})
    add_dependencies(${Target} ${Target}_generate_iris)
    target_include_directories(${Target} PUBLIC "${GeneratedHeaderDir}")
endfunction()
