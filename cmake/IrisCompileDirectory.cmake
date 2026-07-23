# iris_compile_directory(<target> <source-dir> <generated-header-dir>)
#
# Globs every `<source-dir>/*.iris` file and emits one add_custom_command per file, mirroring
# the per-file `add_custom_command` block a consumer would otherwise hand-write once per `.iris`
# file (see pharos-proto/CMakeLists.txt's own pre-existing pattern this generates) — each
# `Name.iris` compiles via `iris_cc` into `<generated-header-dir>/Name.iris.h`, and every
# generated header is added to `<target>`'s dependencies and include path automatically.
#
# `<source-dir>` is expected to contain a `.iris.json` (docs/iris_core_spec.md §5) alongside its
# `.iris` files, the same convention `iris_cc`'s own `--project-root` auto-resolution assumes
# (tools/IrisCc.cpp); it's listed as a dependency of every generated header so editing
# `searchPaths`/`target` triggers a recompile.
#
# Uses plain `file(GLOB ...)`, not `CONFIGURE_DEPENDS` -- like `pharos-proto/CMakeLists.txt`'s
# own `file(GLOB_RECURSE PHAROS_LIB_SOURCES ...)`, adding or removing a `.iris` file needs a
# fresh `cmake -B build`, not just a rebuild. Documented, accepted limitation
# (docs/next-steps.md, "CMake helper to compile every `.iris` file in a directory").
function(iris_compile_directory Target SourceDir GeneratedHeaderDir)
    file(GLOB IrisSources "${SourceDir}/*.iris")

    set(GeneratedHeaders "")
    foreach(IrisSource ${IrisSources})
        get_filename_component(IrisName "${IrisSource}" NAME_WE)
        set(GeneratedHeader "${GeneratedHeaderDir}/${IrisName}.iris.h")
        add_custom_command(
            OUTPUT "${GeneratedHeader}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${GeneratedHeaderDir}"
            COMMAND $<TARGET_FILE:iris_cc> "${IrisSource}" -o "${GeneratedHeader}"
            DEPENDS iris_cc "${IrisSource}" "${SourceDir}/.iris.json"
            COMMENT "iris_cc: compiling ${IrisName}.iris")
        list(APPEND GeneratedHeaders "${GeneratedHeader}")
    endforeach()

    add_custom_target(${Target}_generate_iris DEPENDS ${GeneratedHeaders})
    add_dependencies(${Target} ${Target}_generate_iris)
    target_include_directories(${Target} PUBLIC "${GeneratedHeaderDir}")
endfunction()
