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
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt07/upt07_material_lambert_diagnostic.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt07/upt07_emissive_material.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt07/upt07_temporal_indirect.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt07/upt07_indirect_material.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt07/upt07_work_budget.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt10/upt10_reuse_contract.slangh"
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

# S30.1 bounded combined direct+global proof. One lane owns a disjoint
# reciprocal two-pixel pair and publishes both directions from two shifts. The
# accepted multi-neighbor direct S0 remains a separate runtime artifact.
path_tracing_declare_slang_spirv_module(
    NAME upt09_spatial_shared_selected_pair_light64
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
        UPT09_SPATIAL_INDIRECT_REPLAY=1 UPT10_SHARED_SPATIAL=1
    OUT_VAR spatial_shared_selected_pair_outputs
    RAY_QUERY)

# UPT-31 source-target A/B. This is deliberately a separate artifact: the
# accepted shared estimator remains byte-for-byte available while this variant
# removes only the redundant direct-event source-receiver emitter evaluation.
path_tracing_declare_slang_spirv_module(
    NAME upt09_spatial_shared_selected_pair_stored_target_light64
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
        UPT09_SPATIAL_INDIRECT_REPLAY=1 UPT10_SHARED_SPATIAL=1
        UPT31_STORED_DIRECT_SOURCE_TARGET=1
    OUT_VAR spatial_shared_stored_target_outputs
    RAY_QUERY)

# UPT-32 full-occupancy reciprocal pair. Every lane computes one mapping,
# publishes it through group-shared memory, then consumes the partner mapping
# and writes exactly its own output pixel. The accepted leader and UPT-31
# stored-target artifacts remain available as byte-distinct rollback oracles.
path_tracing_declare_slang_spirv_module(
    NAME upt09_spatial_shared_workgroup_pair_stored_target_light64
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
        UPT09_SPATIAL_INDIRECT_REPLAY=1 UPT10_SHARED_SPATIAL=1
        UPT31_STORED_DIRECT_SOURCE_TARGET=1
        UPT32_WORKGROUP_RECIPROCAL_PAIRING=1
    OUT_VAR spatial_shared_workgroup_pair_outputs
    RAY_QUERY)

# UPT-33 preserves UPT-32 byte-for-byte as the rollback oracle and adds only
# explicit one-frame, non-donating empty-center rescue.
path_tracing_declare_slang_spirv_module(
    NAME upt09_spatial_shared_workgroup_pair_rescue_stored_target_light64
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
        UPT09_SPATIAL_INDIRECT_REPLAY=1 UPT10_SHARED_SPATIAL=1
        UPT31_STORED_DIRECT_SOURCE_TARGET=1
        UPT32_WORKGROUP_RECIPROCAL_PAIRING=1
        UPT33_EMPTY_CENTER_RESCUE=1
    OUT_VAR spatial_shared_workgroup_pair_rescue_outputs
    RAY_QUERY)

# UPT-38 preserves the accepted UPT-33 estimator and workgroup schedule. Its
# only shader-policy change is admitting UPT-35 path-length-two endpoints
# through the exact stored-q replay already accepted by UPT-36 T0.
path_tracing_declare_slang_spirv_module(
    NAME upt09_spatial_shared_workgroup_pair_rescue_stored_target_light64_three_vertex
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
        UPT09_SPATIAL_INDIRECT_REPLAY=1 UPT10_SHARED_SPATIAL=1
        UPT31_STORED_DIRECT_SOURCE_TARGET=1
        UPT32_WORKGROUP_RECIPROCAL_PAIRING=1
        UPT33_EMPTY_CENTER_RESCUE=1
        UPT07_THREE_VERTEX_REPLAY=1
    OUT_VAR spatial_shared_workgroup_pair_rescue_three_vertex_outputs
    RAY_QUERY)

path_tracing_declare_slang_spirv_module(
    NAME upt09_spatial_direct_rayquery_lambert
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt09_spatial_direct_rayquery.slang"
    ENTRY main STAGE compute OUTPUT_DIR "${UPT09_OUTPUT_DIR}"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04"
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt07"
    DEPENDS ${UPT09_DIRECT_MODULES}
    DEFINES UPT09_SPATIAL_GEOMETRY_REPLAY=1 UPT_DIAGNOSTIC_LAMBERT=1
    OUT_VAR spatial_lambert_outputs RAY_QUERY)
path_tracing_declare_slang_spirv_module(
    NAME upt09_spatial_direct_rayquery_light64_lambert
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt09_spatial_direct_rayquery.slang"
    ENTRY main STAGE compute OUTPUT_DIR "${UPT09_OUTPUT_DIR}"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04"
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt07"
    DEPENDS ${UPT09_DIRECT_MODULES}
    DEFINES UPT09_PACKED_LIGHT_RECORD=1 UPT09_SPATIAL_GEOMETRY_REPLAY=1
        UPT_DIAGNOSTIC_LAMBERT=1
    OUT_VAR spatial_compact_lambert_outputs RAY_QUERY)

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

set(UPT10_SPATIAL_SHARED_VERIFY_STAMP
    "${UPT09_OUTPUT_DIR}/upt10_spatial_shared_selected_pair_verified.txt")
add_custom_command(
    OUTPUT "${UPT10_SPATIAL_SHARED_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT10_REFLECTION=${UPT09_OUTPUT_DIR}/upt09_spatial_shared_selected_pair_light64.slang.spirv-reflection.json
        -DUPT10_DISASSEMBLY=${UPT09_OUTPUT_DIR}/upt09_spatial_shared_selected_pair_light64.slang.spvasm
        -DUPT31_REFLECTION=${UPT09_OUTPUT_DIR}/upt09_spatial_shared_selected_pair_stored_target_light64.slang.spirv-reflection.json
        -DUPT31_DISASSEMBLY=${UPT09_OUTPUT_DIR}/upt09_spatial_shared_selected_pair_stored_target_light64.slang.spvasm
        -DUPT32_REFLECTION=${UPT09_OUTPUT_DIR}/upt09_spatial_shared_workgroup_pair_stored_target_light64.slang.spirv-reflection.json
        -DUPT32_DISASSEMBLY=${UPT09_OUTPUT_DIR}/upt09_spatial_shared_workgroup_pair_stored_target_light64.slang.spvasm
        -DUPT33_REFLECTION=${UPT09_OUTPUT_DIR}/upt09_spatial_shared_workgroup_pair_rescue_stored_target_light64.slang.spirv-reflection.json
        -DUPT33_DISASSEMBLY=${UPT09_OUTPUT_DIR}/upt09_spatial_shared_workgroup_pair_rescue_stored_target_light64.slang.spvasm
        -DUPT10_STAMP=${UPT10_SPATIAL_SHARED_VERIFY_STAMP}
        -DUPT10_HOST_SOURCE=${CMAKE_CURRENT_LIST_DIR}/../../renderer/NVRHI/PathTraceUnifiedPt.cpp
        -DUPT10_SHADER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/upt09_spatial_direct_rayquery.slang
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt10SpatialShared.cmake"
    DEPENDS ${spatial_shared_selected_pair_outputs}
        ${spatial_shared_stored_target_outputs}
        ${spatial_shared_workgroup_pair_outputs}
        ${spatial_shared_workgroup_pair_rescue_outputs}
        "${CMAKE_CURRENT_LIST_DIR}/../../renderer/NVRHI/PathTraceUnifiedPt.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt09_spatial_direct_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt10SpatialShared.cmake"
    COMMENT "UPT-33 verifying bounded non-donating shared spatial rescue"
    VERBATIM)

set(UPT38_SPATIAL_VERIFY_STAMP
    "${UPT09_OUTPUT_DIR}/upt38_three_vertex_spatial_verified.txt")
add_custom_command(
    OUTPUT "${UPT38_SPATIAL_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT38_REFLECTION=${UPT09_OUTPUT_DIR}/upt09_spatial_shared_workgroup_pair_rescue_stored_target_light64_three_vertex.slang.spirv-reflection.json
        -DUPT38_DISASSEMBLY=${UPT09_OUTPUT_DIR}/upt09_spatial_shared_workgroup_pair_rescue_stored_target_light64_three_vertex.slang.spvasm
        -DUPT38_STAMP=${UPT38_SPATIAL_VERIFY_STAMP}
        -DUPT38_HOST_SOURCE=${CMAKE_CURRENT_LIST_DIR}/../../renderer/NVRHI/PathTraceUnifiedPt.cpp
        -DUPT38_SPATIAL_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/upt09_spatial_direct_rayquery.slang
        -DUPT38_REPLAY_SOURCE=${CMAKE_CURRENT_LIST_DIR}/../slang_upt07/upt07_temporal_indirect.slang
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt38ThreeVertexSpatial.cmake"
    DEPENDS ${spatial_shared_workgroup_pair_rescue_three_vertex_outputs}
        "${CMAKE_CURRENT_LIST_DIR}/../../renderer/NVRHI/PathTraceUnifiedPt.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt09_spatial_direct_rayquery.slang"
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt07/upt07_temporal_indirect.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt38ThreeVertexSpatial.cmake"
    COMMENT "UPT-38 verifying bounded three-vertex shared spatial replay"
    VERBATIM)

set(UPT09_RUNTIME_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../base/renderprogs2/spirv/builtin/pathtracing/slang_upt09")
set(UPT09_RUNTIME_OUTPUT
    "${UPT09_RUNTIME_DIR}/upt09_spatial_direct_rayquery.bin")
set(UPT09_COMPACT_RUNTIME_OUTPUT
    "${UPT09_RUNTIME_DIR}/upt09_spatial_direct_rayquery_light64.bin")
set(UPT09_LAMBERT_RUNTIME_OUTPUT
    "${UPT09_RUNTIME_DIR}/upt09_spatial_direct_rayquery_lambert.bin")
set(UPT09_COMPACT_LAMBERT_RUNTIME_OUTPUT
    "${UPT09_RUNTIME_DIR}/upt09_spatial_direct_rayquery_light64_lambert.bin")
set(UPT10_SPATIAL_SHARED_RUNTIME_OUTPUT
    "${UPT09_RUNTIME_DIR}/upt09_spatial_shared_selected_pair_light64.bin")
set(UPT31_SPATIAL_STORED_TARGET_RUNTIME_OUTPUT
    "${UPT09_RUNTIME_DIR}/upt09_spatial_shared_selected_pair_stored_target_light64.bin")
set(UPT32_SPATIAL_WORKGROUP_PAIR_RUNTIME_OUTPUT
    "${UPT09_RUNTIME_DIR}/upt09_spatial_shared_workgroup_pair_stored_target_light64.bin")
set(UPT33_SPATIAL_WORKGROUP_PAIR_RESCUE_RUNTIME_OUTPUT
    "${UPT09_RUNTIME_DIR}/upt09_spatial_shared_workgroup_pair_rescue_stored_target_light64.bin")
set(UPT38_SPATIAL_THREE_VERTEX_RUNTIME_OUTPUT
    "${UPT09_RUNTIME_DIR}/upt09_spatial_shared_workgroup_pair_rescue_stored_target_light64_three_vertex.bin")
add_custom_command(
    OUTPUT "${UPT09_RUNTIME_OUTPUT}" "${UPT09_COMPACT_RUNTIME_OUTPUT}"
        "${UPT09_LAMBERT_RUNTIME_OUTPUT}"
        "${UPT09_COMPACT_LAMBERT_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT09_RUNTIME_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT09_OUTPUT_DIR}/upt09_spatial_direct_rayquery.slang.spv"
        "${UPT09_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT09_OUTPUT_DIR}/upt09_spatial_direct_rayquery_light64.slang.spv"
        "${UPT09_COMPACT_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT09_OUTPUT_DIR}/upt09_spatial_direct_rayquery_lambert.slang.spv"
        "${UPT09_LAMBERT_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT09_OUTPUT_DIR}/upt09_spatial_direct_rayquery_light64_lambert.slang.spv"
        "${UPT09_COMPACT_LAMBERT_RUNTIME_OUTPUT}"
    DEPENDS ${spatial_direct_outputs} ${spatial_direct_compact_outputs}
        ${spatial_lambert_outputs} ${spatial_compact_lambert_outputs}
        "${UPT09_VERIFY_STAMP}"
    COMMENT "UPT-09 verified Vulkan runtime artifacts: direct spatial rescue"
    VERBATIM)
add_custom_command(
    OUTPUT "${UPT10_SPATIAL_SHARED_RUNTIME_OUTPUT}"
        "${UPT31_SPATIAL_STORED_TARGET_RUNTIME_OUTPUT}"
        "${UPT32_SPATIAL_WORKGROUP_PAIR_RUNTIME_OUTPUT}"
        "${UPT33_SPATIAL_WORKGROUP_PAIR_RESCUE_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT09_RUNTIME_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT09_OUTPUT_DIR}/upt09_spatial_shared_selected_pair_light64.slang.spv"
        "${UPT10_SPATIAL_SHARED_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT09_OUTPUT_DIR}/upt09_spatial_shared_selected_pair_stored_target_light64.slang.spv"
        "${UPT31_SPATIAL_STORED_TARGET_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT09_OUTPUT_DIR}/upt09_spatial_shared_workgroup_pair_stored_target_light64.slang.spv"
        "${UPT32_SPATIAL_WORKGROUP_PAIR_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT09_OUTPUT_DIR}/upt09_spatial_shared_workgroup_pair_rescue_stored_target_light64.slang.spv"
        "${UPT33_SPATIAL_WORKGROUP_PAIR_RESCUE_RUNTIME_OUTPUT}"
    DEPENDS ${spatial_shared_selected_pair_outputs}
        ${spatial_shared_stored_target_outputs}
        ${spatial_shared_workgroup_pair_outputs}
        ${spatial_shared_workgroup_pair_rescue_outputs}
        "${UPT10_SPATIAL_SHARED_VERIFY_STAMP}"
    COMMENT "UPT-33 Vulkan opt-in non-donating empty-center rescue artifact"
    VERBATIM)

add_custom_command(
    OUTPUT "${UPT38_SPATIAL_THREE_VERTEX_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT09_RUNTIME_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT09_OUTPUT_DIR}/upt09_spatial_shared_workgroup_pair_rescue_stored_target_light64_three_vertex.slang.spv"
        "${UPT38_SPATIAL_THREE_VERTEX_RUNTIME_OUTPUT}"
    DEPENDS ${spatial_shared_workgroup_pair_rescue_three_vertex_outputs}
        "${UPT38_SPATIAL_VERIFY_STAMP}"
    COMMENT "UPT-38 Vulkan opt-in three-vertex shared spatial artifact"
    VERBATIM)

add_custom_target(PathTraceSlangUpt09Spatial
    DEPENDS ${spatial_direct_outputs} ${spatial_direct_compact_outputs}
        ${spatial_shared_selected_pair_outputs}
        ${spatial_shared_stored_target_outputs}
        ${spatial_shared_workgroup_pair_outputs}
        ${spatial_shared_workgroup_pair_rescue_outputs}
        ${spatial_shared_workgroup_pair_rescue_three_vertex_outputs}
        "${UPT10_SPATIAL_SHARED_VERIFY_STAMP}"
        "${UPT38_SPATIAL_VERIFY_STAMP}"
        "${UPT09_VERIFY_STAMP}" "${UPT09_RUNTIME_OUTPUT}"
        "${UPT09_COMPACT_RUNTIME_OUTPUT}" "${UPT09_LAMBERT_RUNTIME_OUTPUT}"
        "${UPT09_COMPACT_LAMBERT_RUNTIME_OUTPUT}"
        "${UPT10_SPATIAL_SHARED_RUNTIME_OUTPUT}"
        "${UPT31_SPATIAL_STORED_TARGET_RUNTIME_OUTPUT}"
        "${UPT32_SPATIAL_WORKGROUP_PAIR_RUNTIME_OUTPUT}"
        "${UPT33_SPATIAL_WORKGROUP_PAIR_RESCUE_RUNTIME_OUTPUT}"
        "${UPT38_SPATIAL_THREE_VERTEX_RUNTIME_OUTPUT}")
set_target_properties(PathTraceSlangUpt09Spatial PROPERTIES FOLDER "Shaders/UPT-09")
if(TARGET RBDoom3BFG)
    add_dependencies(RBDoom3BFG PathTraceSlangUpt09Spatial)
endif()

add_custom_target(PathTraceSlangUpt38ThreeVertexSpatial
    DEPENDS ${spatial_shared_workgroup_pair_rescue_three_vertex_outputs}
        "${UPT38_SPATIAL_VERIFY_STAMP}"
        "${UPT38_SPATIAL_THREE_VERTEX_RUNTIME_OUTPUT}")
set_target_properties(PathTraceSlangUpt38ThreeVertexSpatial PROPERTIES
    FOLDER "Shaders/UPT-09")
