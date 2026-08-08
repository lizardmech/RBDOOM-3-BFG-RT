include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/PathTracingSlang.cmake)

if(NOT PATH_TRACING_ENABLE_SLANG_UPT04 OR NOT USE_VULKAN)
    return()
endif()

set(UPT09_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/artifacts")
set(UPT09_DIRECT_MODULES
    "${CMAKE_CURRENT_SOURCE_DIR}/upt09_spatial_direct_rayquery.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt09_spatial_direct_bindings.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt07/upt07_temporal_reservoir.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt07/upt07_material_provider_contract.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt07/upt07_material_openpbr.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt07/upt07_emissive_material.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03/upt03_reservoir_contract.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_binary16.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_hit_contract.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_initial_work_contract.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_light_contract.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_local_light_proposal.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_geometry_contract.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_geometry_bindings.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_geometry_decode.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_emissive_replay.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_material_contract.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_material_live_contract.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_material_emission.slang"
      "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_material_primary_live.slang"
      "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_primary_surface_live_contract.slang"
      "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_packed_primary_surface.slang"
      "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_packed_primary_surface32.slang"
      "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_primary_history_sidecar.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_random_white_hash.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt05_material_openpbr.slang")

path_tracing_declare_slang_spirv_module(
    NAME upt09_spatial_direct_rayquery
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt09_spatial_direct_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT09_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04"
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt07"
    DEPENDS ${UPT09_DIRECT_MODULES}
    DEFINES UPT09_SPATIAL_GEOMETRY_REPLAY=1
    OUT_VAR spatial_direct_outputs
    RAY_QUERY)

path_tracing_declare_slang_spirv_module(
    NAME upt09_spatial_direct_rayquery_light64
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt09_spatial_direct_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT09_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04"
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt07"
    DEPENDS ${UPT09_DIRECT_MODULES}
    DEFINES UPT09_PACKED_LIGHT_RECORD=1 UPT09_SPATIAL_GEOMETRY_REPLAY=1
    OUT_VAR spatial_direct_compact_outputs
    RAY_QUERY)

set(UPT09_VERIFY_STAMP "${UPT09_OUTPUT_DIR}/upt09_spatial_direct_verified.txt")
add_custom_command(
    OUTPUT "${UPT09_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT09_FULL_REFLECTION=${UPT09_OUTPUT_DIR}/upt09_spatial_direct_rayquery.slang.spirv-reflection.json
        -DUPT09_FULL_DISASSEMBLY=${UPT09_OUTPUT_DIR}/upt09_spatial_direct_rayquery.slang.spvasm
        -DUPT09_COMPACT_REFLECTION=${UPT09_OUTPUT_DIR}/upt09_spatial_direct_rayquery_light64.slang.spirv-reflection.json
        -DUPT09_COMPACT_DISASSEMBLY=${UPT09_OUTPUT_DIR}/upt09_spatial_direct_rayquery_light64.slang.spvasm
        -DUPT09_STAMP=${UPT09_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt09SpatialDirect.cmake"
    DEPENDS ${spatial_direct_outputs} ${spatial_direct_compact_outputs}
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt09SpatialDirect.cmake"
    COMMENT "UPT-09 verifying direct spatial rescue provider"
    VERBATIM)

set(UPT09_RUNTIME_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../base/renderprogs2/spirv/builtin/pathtracing/slang_upt09")
set(UPT09_RUNTIME_OUTPUT
    "${UPT09_RUNTIME_DIR}/upt09_spatial_direct_rayquery.bin")
set(UPT09_COMPACT_RUNTIME_OUTPUT
    "${UPT09_RUNTIME_DIR}/upt09_spatial_direct_rayquery_light64.bin")
add_custom_command(
    OUTPUT "${UPT09_RUNTIME_OUTPUT}" "${UPT09_COMPACT_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT09_RUNTIME_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT09_OUTPUT_DIR}/upt09_spatial_direct_rayquery.slang.spv"
        "${UPT09_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT09_OUTPUT_DIR}/upt09_spatial_direct_rayquery_light64.slang.spv"
        "${UPT09_COMPACT_RUNTIME_OUTPUT}"
    DEPENDS ${spatial_direct_outputs} ${spatial_direct_compact_outputs}
        "${UPT09_VERIFY_STAMP}"
    COMMENT "UPT-09 verified Vulkan runtime artifacts: direct spatial rescue"
    VERBATIM)

add_custom_target(PathTraceSlangUpt09Spatial
    DEPENDS ${spatial_direct_outputs} ${spatial_direct_compact_outputs}
        "${UPT09_VERIFY_STAMP}" "${UPT09_RUNTIME_OUTPUT}"
        "${UPT09_COMPACT_RUNTIME_OUTPUT}")
set_target_properties(PathTraceSlangUpt09Spatial PROPERTIES FOLDER "Shaders/UPT-09")
if(TARGET RBDoom3BFG)
    add_dependencies(RBDoom3BFG PathTraceSlangUpt09Spatial)
endif()
