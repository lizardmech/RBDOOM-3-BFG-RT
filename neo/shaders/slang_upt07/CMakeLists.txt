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
        "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_surface_contract.slang"
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03/upt03_reservoir_contract.slang"
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_primary_surface_live_contract.slang"
    OUT_VAR temporal_ownership_outputs)

set(UPT07_DIRECT_MODULES
    "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_direct_rayquery.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_surface_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_reservoir.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt07_material_provider_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt07_material_openpbr.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03/upt03_reservoir_contract.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_binary16.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_hit_contract.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_initial_work_contract.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_light_contract.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_local_light_proposal.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_material_contract.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_material_primary_live.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_primary_surface_live_contract.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_random_white_hash.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_transport_initial_bounded.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt05_material_openpbr.slang")

path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_direct_rayquery
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_direct_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT07_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04"
    DEPENDS ${UPT07_DIRECT_MODULES}
    OUT_VAR temporal_direct_outputs
    RAY_QUERY)

path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_direct_rayquery_light64
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_direct_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT07_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04"
    DEPENDS ${UPT07_DIRECT_MODULES}
    DEFINES UPT07_PACKED_LIGHT_RECORD=1
    OUT_VAR temporal_direct_compact_outputs
    RAY_QUERY)

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

set(UPT07_DIRECT_VERIFY_STAMP "${UPT07_OUTPUT_DIR}/upt07_temporal_direct_verified.txt")
add_custom_command(
    OUTPUT "${UPT07_DIRECT_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT07_FULL_REFLECTION=${UPT07_OUTPUT_DIR}/upt07_temporal_direct_rayquery.slang.spirv-reflection.json
        -DUPT07_FULL_DISASSEMBLY=${UPT07_OUTPUT_DIR}/upt07_temporal_direct_rayquery.slang.spvasm
        -DUPT07_COMPACT_REFLECTION=${UPT07_OUTPUT_DIR}/upt07_temporal_direct_rayquery_light64.slang.spirv-reflection.json
        -DUPT07_COMPACT_DISASSEMBLY=${UPT07_OUTPUT_DIR}/upt07_temporal_direct_rayquery_light64.slang.spvasm
        -DUPT07_STAMP=${UPT07_DIRECT_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt07TemporalDirect.cmake"
    DEPENDS ${temporal_direct_outputs} ${temporal_direct_compact_outputs}
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt07TemporalDirect.cmake"
    COMMENT "UPT-07 verifying primary-analytic temporal shift provider"
    VERBATIM)

add_custom_target(PathTraceSlangUpt07TemporalOwnership
    DEPENDS ${temporal_ownership_outputs} "${UPT07_VERIFY_STAMP}"
        ${temporal_direct_outputs} ${temporal_direct_compact_outputs}
        "${UPT07_DIRECT_VERIFY_STAMP}")
set_target_properties(PathTraceSlangUpt07TemporalOwnership PROPERTIES
    FOLDER "Shaders/UPT-07")
