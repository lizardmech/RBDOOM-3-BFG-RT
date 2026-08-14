include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/PathTracingSlang.cmake)

if(NOT PATH_TRACING_ENABLE_SLANG_UPT04 OR NOT USE_VULKAN)
    return()
endif()

set(UPT45_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/artifacts")
set(UPT45_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt45_glass_compose.slang")

path_tracing_declare_slang_spirv_module(
    NAME upt45_glass_compose
    SOURCE "${UPT45_SOURCE}"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT45_OUTPUT_DIR}"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS "${UPT45_SOURCE}"
    OUT_VAR upt45_compose_outputs)

set(UPT45_RUNTIME_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../base/renderprogs2/spirv/builtin/pathtracing/slang_upt45")
set(UPT45_RUNTIME_OUTPUT "${UPT45_RUNTIME_DIR}/upt45_glass_compose.bin")
add_custom_command(
    OUTPUT "${UPT45_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT45_RUNTIME_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT45_OUTPUT_DIR}/upt45_glass_compose.slang.spv"
        "${UPT45_RUNTIME_OUTPUT}"
    DEPENDS ${upt45_compose_outputs}
    COMMENT "UPT-45 native Vulkan glass compose artifact"
    VERBATIM)

add_custom_target(PathTraceSlangUpt45Glass
    DEPENDS ${upt45_compose_outputs} "${UPT45_RUNTIME_OUTPUT}")
set_target_properties(PathTraceSlangUpt45Glass PROPERTIES FOLDER "Shaders/UPT-45")
if(TARGET RBDoom3BFG)
    add_dependencies(RBDoom3BFG PathTraceSlangUpt45Glass)
endif()
