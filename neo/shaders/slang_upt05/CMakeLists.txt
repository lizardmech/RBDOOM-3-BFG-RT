include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/PathTracingSlang.cmake)

if(NOT PATH_TRACING_ENABLE_SLANG_UPT04 OR NOT USE_VULKAN)
    return()
endif()

set(UPT05_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/artifacts")
path_tracing_declare_slang_spirv_module(
    NAME upt05_resolve
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt05_resolve.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT05_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/upt05_resolve.slang"
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03/upt03_reservoir_contract.slang"
    OUT_VAR resolve_outputs)

set(UPT05_VERIFY_STAMP "${UPT05_OUTPUT_DIR}/upt05_resolve_verified.txt")
add_custom_command(
    OUTPUT "${UPT05_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT05_REFLECTION=${UPT05_OUTPUT_DIR}/upt05_resolve.slang.spirv-reflection.json
        -DUPT05_DISASSEMBLY=${UPT05_OUTPUT_DIR}/upt05_resolve.slang.spvasm
        -DUPT05_STAMP=${UPT05_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt05Resolve.cmake"
    DEPENDS ${resolve_outputs}
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt05Resolve.cmake"
    COMMENT "UPT-05 verifying trace-free RGBA16F resolve closure"
    VERBATIM)

set(UPT05_RUNTIME_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../base/renderprogs2/spirv/builtin/pathtracing/slang_upt05")
set(UPT05_RUNTIME_OUTPUT "${UPT05_RUNTIME_DIR}/upt05_resolve.bin")
add_custom_command(
    OUTPUT "${UPT05_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT05_RUNTIME_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT05_OUTPUT_DIR}/upt05_resolve.slang.spv"
        "${UPT05_RUNTIME_OUTPUT}"
    DEPENDS "${UPT05_OUTPUT_DIR}/upt05_resolve.slang.spv"
        "${UPT05_VERIFY_STAMP}"
    COMMENT "UPT-05 verified Vulkan runtime artifact: upt05_resolve"
    VERBATIM)

add_custom_target(PathTraceSlangUpt05Resolve
    DEPENDS ${resolve_outputs} "${UPT05_VERIFY_STAMP}" "${UPT05_RUNTIME_OUTPUT}")
set_target_properties(PathTraceSlangUpt05Resolve PROPERTIES FOLDER "Shaders/UPT-05")
if(TARGET PathTraceSlangUpt04ModuleGraph)
    add_dependencies(PathTraceSlangUpt05Resolve PathTraceSlangUpt04ModuleGraph)
endif()
if(TARGET RBDoom3BFG)
    add_dependencies(RBDoom3BFG PathTraceSlangUpt05Resolve)
endif()
