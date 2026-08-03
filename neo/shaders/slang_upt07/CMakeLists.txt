include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/PathTracingSlang.cmake)

if(NOT PATH_TRACING_ENABLE_SLANG_UPT04 OR NOT USE_VULKAN)
    return()
endif()

set(UPT07_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/artifacts")
path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_ownership_probe
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_ownership_probe.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT07_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_ownership_probe.slang"
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03/upt03_reservoir_contract.slang"
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_primary_surface_live_contract.slang"
    OUT_VAR temporal_ownership_outputs)

set(UPT07_VERIFY_STAMP "${UPT07_OUTPUT_DIR}/upt07_temporal_ownership_verified.txt")
add_custom_command(
    OUTPUT "${UPT07_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT07_REFLECTION=${UPT07_OUTPUT_DIR}/upt07_temporal_ownership_probe.slang.spirv-reflection.json
        -DUPT07_DISASSEMBLY=${UPT07_OUTPUT_DIR}/upt07_temporal_ownership_probe.slang.spvasm
        -DUPT07_STAMP=${UPT07_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt07TemporalOwnership.cmake"
    DEPENDS ${temporal_ownership_outputs}
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt07TemporalOwnership.cmake"
    COMMENT "UPT-07 verifying two-page temporal ownership proof"
    VERBATIM)

add_custom_target(PathTraceSlangUpt07TemporalOwnership
    DEPENDS ${temporal_ownership_outputs} "${UPT07_VERIFY_STAMP}")
set_target_properties(PathTraceSlangUpt07TemporalOwnership PROPERTIES
    FOLDER "Shaders/UPT-07")
