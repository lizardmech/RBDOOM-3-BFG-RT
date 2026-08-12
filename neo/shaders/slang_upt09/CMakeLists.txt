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

# UPT-43.8 keeps the accepted UPT-33 full-screen estimator and adds only the
# exact rcLength-two, short-history queue publication. The compact consumer
# below performs additional simultaneous-GRIS work only for those pixels.
path_tracing_declare_slang_spirv_module(
    NAME upt09_spatial_shared_workgroup_pair_rescue_stored_target_light64_boost_classify
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
        UPT33_EMPTY_CENTER_RESCUE=1 UPT438_SPATIAL_BOOST_CLASSIFY=1
    OUT_VAR spatial_shared_boost_classify_outputs
    RAY_QUERY)

path_tracing_declare_slang_spirv_module(
    NAME upt09_spatial_shared_disocclusion_boost_compact_light64
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
        UPT438_SPATIAL_BOOST_COMPACT=1
    OUT_VAR spatial_shared_boost_compact_outputs
    RAY_QUERY)

# UPT-42 replaces the production 8x8-local pairing schedule with the paper's
# 254x254 Gaussian self-inverting reuse texture. Screen-ordered lanes recover
# their partner independently, then the lower linear endpoint publishes both
# directions. This avoids the rejected padded pair-list launch while retaining
# reciprocal ownership without cross-workgroup exchange. UPT-33 remains an
# explicit byte-distinct rollback artifact.
path_tracing_declare_slang_spirv_module(
    NAME upt09_spatial_shared_reuse_texture_pair_rescue_stored_target_light64
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
        UPT31_STORED_DIRECT_SOURCE_TARGET=1 UPT33_EMPTY_CENTER_RESCUE=1
        UPT42_REUSE_TEXTURE_PAIRING=1
    OUT_VAR spatial_shared_reuse_texture_pair_rescue_outputs
    RAY_QUERY)

# UPT-43.5 separates paper-shaped reciprocal mapping from resampling. The
# prepass writes one 16-byte scalar record per source pixel. The resampling
# pass consumes reciprocal scalars and rematerializes the shifted candidate
# only when the neighbor actually wins, so no third 64-byte reservoir page is
# introduced.
path_tracing_declare_slang_spirv_module(
    NAME upt09_spatial_shared_reuse_texture_shift_prepass_light64
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
        UPT31_STORED_DIRECT_SOURCE_TARGET=1 UPT33_EMPTY_CENTER_RESCUE=1
        UPT42_REUSE_TEXTURE_PAIRING=1 UPT43_SPATIAL_SHIFT_PREPASS=1
    OUT_VAR spatial_shared_reuse_texture_shift_prepass_outputs
    RAY_QUERY)

path_tracing_declare_slang_spirv_module(
    NAME upt09_spatial_shared_reuse_texture_resample_light64
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
        UPT31_STORED_DIRECT_SOURCE_TARGET=1 UPT33_EMPTY_CENTER_RESCUE=1
        UPT42_REUSE_TEXTURE_PAIRING=1 UPT43_SPATIAL_RESAMPLE=1
    OUT_VAR spatial_shared_reuse_texture_resample_outputs
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
    NAME upt09_spatial_shared_reuse_texture_pair_rescue_stored_target_light64_three_vertex
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
        UPT31_STORED_DIRECT_SOURCE_TARGET=1 UPT33_EMPTY_CENTER_RESCUE=1
        UPT42_REUSE_TEXTURE_PAIRING=1 UPT07_THREE_VERTEX_REPLAY=1
    OUT_VAR spatial_shared_reuse_texture_pair_rescue_three_vertex_outputs
    RAY_QUERY)

# UPT-34 preserves UPT-33 as the rollback oracle. Three distinct reciprocal
# maps are shifted into group-shared storage before one simultaneous
# canonical-plus-three-neighbor GRIS merge.
path_tracing_declare_slang_spirv_module(
    NAME upt09_spatial_shared_workgroup_multi3_rescue_stored_target_light64
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
        UPT34_MULTI_NEIGHBOR_SPATIAL=1
    OUT_VAR spatial_shared_workgroup_multi3_outputs
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
        -DUPT34_REFLECTION=${UPT09_OUTPUT_DIR}/upt09_spatial_shared_workgroup_multi3_rescue_stored_target_light64.slang.spirv-reflection.json
        -DUPT34_DISASSEMBLY=${UPT09_OUTPUT_DIR}/upt09_spatial_shared_workgroup_multi3_rescue_stored_target_light64.slang.spvasm
        -DUPT10_STAMP=${UPT10_SPATIAL_SHARED_VERIFY_STAMP}
        -DUPT10_HOST_SOURCE=${CMAKE_CURRENT_LIST_DIR}/../../renderer/NVRHI/PathTraceUnifiedPt.cpp
        -DUPT10_SHADER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/upt09_spatial_direct_rayquery.slang
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt10SpatialShared.cmake"
    DEPENDS ${spatial_shared_selected_pair_outputs}
        ${spatial_shared_stored_target_outputs}
        ${spatial_shared_workgroup_pair_outputs}
        ${spatial_shared_workgroup_pair_rescue_outputs}
        ${spatial_shared_workgroup_multi3_outputs}
        "${CMAKE_CURRENT_LIST_DIR}/../../renderer/NVRHI/PathTraceUnifiedPt.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt09_spatial_direct_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt10SpatialShared.cmake"
    COMMENT "UPT-34 verifying simultaneous three-neighbor shared spatial reuse"
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

set(UPT42_SPATIAL_VERIFY_STAMP
    "${UPT09_OUTPUT_DIR}/upt42_reuse_texture_spatial_verified.txt")
add_custom_command(
    OUTPUT "${UPT42_SPATIAL_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT42_REFLECTION=${UPT09_OUTPUT_DIR}/upt09_spatial_shared_reuse_texture_pair_rescue_stored_target_light64.slang.spirv-reflection.json
        -DUPT42_DISASSEMBLY=${UPT09_OUTPUT_DIR}/upt09_spatial_shared_reuse_texture_pair_rescue_stored_target_light64.slang.spvasm
        -DUPT42_THREE_VERTEX_REFLECTION=${UPT09_OUTPUT_DIR}/upt09_spatial_shared_reuse_texture_pair_rescue_stored_target_light64_three_vertex.slang.spirv-reflection.json
        -DUPT42_HOST_SOURCE=${CMAKE_CURRENT_LIST_DIR}/../../renderer/NVRHI/PathTraceUnifiedPt.cpp
        -DUPT42_SHADER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/upt09_spatial_direct_rayquery.slang
        -DUPT42_STAMP=${UPT42_SPATIAL_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt42ReuseTexture.cmake"
    DEPENDS ${spatial_shared_reuse_texture_pair_rescue_outputs}
        ${spatial_shared_reuse_texture_pair_rescue_three_vertex_outputs}
        "${CMAKE_CURRENT_LIST_DIR}/../../renderer/NVRHI/PathTraceUnifiedPt.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt09_spatial_direct_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt42ReuseTexture.cmake"
    COMMENT "UPT-42 verifying Gaussian self-inverting reuse-texture pairing"
    VERBATIM)

set(UPT435_SPATIAL_VERIFY_STAMP
    "${UPT09_OUTPUT_DIR}/upt435_spatial_shift_prepass_verified.txt")
add_custom_command(
    OUTPUT "${UPT435_SPATIAL_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT435_SHIFT_REFLECTION=${UPT09_OUTPUT_DIR}/upt09_spatial_shared_reuse_texture_shift_prepass_light64.slang.spirv-reflection.json
        -DUPT435_SHIFT_DISASSEMBLY=${UPT09_OUTPUT_DIR}/upt09_spatial_shared_reuse_texture_shift_prepass_light64.slang.spvasm
        -DUPT435_RESAMPLE_REFLECTION=${UPT09_OUTPUT_DIR}/upt09_spatial_shared_reuse_texture_resample_light64.slang.spirv-reflection.json
        -DUPT435_RESAMPLE_DISASSEMBLY=${UPT09_OUTPUT_DIR}/upt09_spatial_shared_reuse_texture_resample_light64.slang.spvasm
        -DUPT435_HOST_SOURCE=${CMAKE_CURRENT_LIST_DIR}/../../renderer/NVRHI/PathTraceUnifiedPt.cpp
        -DUPT435_SHADER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/upt09_spatial_direct_rayquery.slang
        -DUPT435_STAMP=${UPT435_SPATIAL_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt435SpatialShiftPrepass.cmake"
    DEPENDS ${spatial_shared_reuse_texture_shift_prepass_outputs}
        ${spatial_shared_reuse_texture_resample_outputs}
        "${CMAKE_CURRENT_LIST_DIR}/../../renderer/NVRHI/PathTraceUnifiedPt.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt09_spatial_direct_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt435SpatialShiftPrepass.cmake"
    COMMENT "UPT-43.5 verifying split Gaussian spatial shift and resampling"
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
set(UPT438_SPATIAL_BOOST_CLASSIFY_RUNTIME_OUTPUT
    "${UPT09_RUNTIME_DIR}/upt09_spatial_shared_workgroup_pair_rescue_stored_target_light64_boost_classify.bin")
set(UPT438_SPATIAL_BOOST_COMPACT_RUNTIME_OUTPUT
    "${UPT09_RUNTIME_DIR}/upt09_spatial_shared_disocclusion_boost_compact_light64.bin")
set(UPT42_SPATIAL_REUSE_TEXTURE_PAIR_RESCUE_RUNTIME_OUTPUT
    "${UPT09_RUNTIME_DIR}/upt09_spatial_shared_reuse_texture_pair_rescue_stored_target_light64.bin")
set(UPT43_SPATIAL_SHIFT_PREPASS_RUNTIME_OUTPUT
    "${UPT09_RUNTIME_DIR}/upt09_spatial_shared_reuse_texture_shift_prepass_light64.bin")
set(UPT43_SPATIAL_RESAMPLE_RUNTIME_OUTPUT
    "${UPT09_RUNTIME_DIR}/upt09_spatial_shared_reuse_texture_resample_light64.bin")
set(UPT34_SPATIAL_WORKGROUP_MULTI3_RUNTIME_OUTPUT
    "${UPT09_RUNTIME_DIR}/upt09_spatial_shared_workgroup_multi3_rescue_stored_target_light64.bin")
set(UPT38_SPATIAL_THREE_VERTEX_RUNTIME_OUTPUT
    "${UPT09_RUNTIME_DIR}/upt09_spatial_shared_workgroup_pair_rescue_stored_target_light64_three_vertex.bin")
set(UPT42_SPATIAL_REUSE_TEXTURE_THREE_VERTEX_RUNTIME_OUTPUT
    "${UPT09_RUNTIME_DIR}/upt09_spatial_shared_reuse_texture_pair_rescue_stored_target_light64_three_vertex.bin")
set(UPT09_DIRECT_RUNTIME_MODULES
    upt09_spatial_direct_rayquery
    upt09_spatial_direct_rayquery_light64
    upt09_spatial_direct_rayquery_lambert
    upt09_spatial_direct_rayquery_light64_lambert)
foreach(runtime_module IN LISTS UPT09_DIRECT_RUNTIME_MODULES)
    add_custom_command(
        OUTPUT "${UPT09_RUNTIME_DIR}/${runtime_module}.bin"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT09_RUNTIME_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${UPT09_OUTPUT_DIR}/${runtime_module}.slang.spv"
            "${UPT09_RUNTIME_DIR}/${runtime_module}.bin"
        DEPENDS "${UPT09_OUTPUT_DIR}/${runtime_module}.slang.spv"
        COMMENT "UPT-09 Vulkan direct spatial artifact: ${runtime_module}"
        VERBATIM)
endforeach()

set(UPT09_SHARED_RUNTIME_MODULES
    upt09_spatial_shared_selected_pair_light64
    upt09_spatial_shared_selected_pair_stored_target_light64
    upt09_spatial_shared_workgroup_pair_stored_target_light64
    upt09_spatial_shared_workgroup_pair_rescue_stored_target_light64
    upt09_spatial_shared_workgroup_pair_rescue_stored_target_light64_boost_classify
    upt09_spatial_shared_disocclusion_boost_compact_light64
    upt09_spatial_shared_reuse_texture_pair_rescue_stored_target_light64
    upt09_spatial_shared_reuse_texture_shift_prepass_light64
    upt09_spatial_shared_reuse_texture_resample_light64
    upt09_spatial_shared_workgroup_multi3_rescue_stored_target_light64)
foreach(runtime_module IN LISTS UPT09_SHARED_RUNTIME_MODULES)
    add_custom_command(
        OUTPUT "${UPT09_RUNTIME_DIR}/${runtime_module}.bin"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT09_RUNTIME_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${UPT09_OUTPUT_DIR}/${runtime_module}.slang.spv"
            "${UPT09_RUNTIME_DIR}/${runtime_module}.bin"
        DEPENDS "${UPT09_OUTPUT_DIR}/${runtime_module}.slang.spv"
        COMMENT "UPT Vulkan shared spatial artifact: ${runtime_module}"
        VERBATIM)
endforeach()

set(UPT438_SPATIAL_BOOST_VERIFY_STAMP
    "${UPT09_OUTPUT_DIR}/upt438_spatial_disocclusion_boost_verified.txt")
add_custom_command(
    OUTPUT "${UPT438_SPATIAL_BOOST_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT438_CLASSIFY_REFLECTION=${UPT09_OUTPUT_DIR}/upt09_spatial_shared_workgroup_pair_rescue_stored_target_light64_boost_classify.slang.spirv-reflection.json
        -DUPT438_CLASSIFY_DISASSEMBLY=${UPT09_OUTPUT_DIR}/upt09_spatial_shared_workgroup_pair_rescue_stored_target_light64_boost_classify.slang.spvasm
        -DUPT438_COMPACT_REFLECTION=${UPT09_OUTPUT_DIR}/upt09_spatial_shared_disocclusion_boost_compact_light64.slang.spirv-reflection.json
        -DUPT438_COMPACT_DISASSEMBLY=${UPT09_OUTPUT_DIR}/upt09_spatial_shared_disocclusion_boost_compact_light64.slang.spvasm
        -DUPT438_SHADER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/upt09_spatial_direct_rayquery.slang
        -DUPT438_HOST_SOURCE=${CMAKE_SOURCE_DIR}/renderer/NVRHI/PathTraceUnifiedPt.cpp
        -DUPT438_STAMP=${UPT438_SPATIAL_BOOST_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt438SpatialDisocclusionBoost.cmake"
    DEPENDS ${spatial_shared_boost_classify_outputs}
        ${spatial_shared_boost_compact_outputs}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt09_spatial_direct_rayquery.slang"
        "${CMAKE_SOURCE_DIR}/renderer/NVRHI/PathTraceUnifiedPt.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt438SpatialDisocclusionBoost.cmake"
    COMMENT "UPT-43.8 verifying sparse disocclusion boost"
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

add_custom_command(
    OUTPUT "${UPT42_SPATIAL_REUSE_TEXTURE_THREE_VERTEX_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT09_RUNTIME_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT09_OUTPUT_DIR}/upt09_spatial_shared_reuse_texture_pair_rescue_stored_target_light64_three_vertex.slang.spv"
        "${UPT42_SPATIAL_REUSE_TEXTURE_THREE_VERTEX_RUNTIME_OUTPUT}"
    DEPENDS ${spatial_shared_reuse_texture_pair_rescue_three_vertex_outputs}
    COMMENT "UPT-42 Vulkan opt-in three-vertex reuse-texture spatial artifact"
    VERBATIM)

add_custom_target(PathTraceSlangUpt09Spatial
    DEPENDS ${spatial_direct_outputs} ${spatial_direct_compact_outputs}
        ${spatial_shared_selected_pair_outputs}
        ${spatial_shared_stored_target_outputs}
        ${spatial_shared_workgroup_pair_outputs}
        ${spatial_shared_workgroup_pair_rescue_outputs}
        ${spatial_shared_boost_classify_outputs}
        ${spatial_shared_boost_compact_outputs}
        "${UPT438_SPATIAL_BOOST_VERIFY_STAMP}"
        ${spatial_shared_reuse_texture_pair_rescue_outputs}
        ${spatial_shared_reuse_texture_shift_prepass_outputs}
        ${spatial_shared_reuse_texture_resample_outputs}
        ${spatial_shared_workgroup_multi3_outputs}
        ${spatial_shared_workgroup_pair_rescue_three_vertex_outputs}
        ${spatial_shared_reuse_texture_pair_rescue_three_vertex_outputs}
        "${UPT10_SPATIAL_SHARED_VERIFY_STAMP}"
        "${UPT38_SPATIAL_VERIFY_STAMP}"
        "${UPT42_SPATIAL_VERIFY_STAMP}"
        "${UPT435_SPATIAL_VERIFY_STAMP}"
        "${UPT09_VERIFY_STAMP}" "${UPT09_RUNTIME_OUTPUT}"
        "${UPT09_COMPACT_RUNTIME_OUTPUT}" "${UPT09_LAMBERT_RUNTIME_OUTPUT}"
        "${UPT09_COMPACT_LAMBERT_RUNTIME_OUTPUT}"
        "${UPT10_SPATIAL_SHARED_RUNTIME_OUTPUT}"
        "${UPT31_SPATIAL_STORED_TARGET_RUNTIME_OUTPUT}"
        "${UPT32_SPATIAL_WORKGROUP_PAIR_RUNTIME_OUTPUT}"
        "${UPT33_SPATIAL_WORKGROUP_PAIR_RESCUE_RUNTIME_OUTPUT}"
        "${UPT438_SPATIAL_BOOST_CLASSIFY_RUNTIME_OUTPUT}"
        "${UPT438_SPATIAL_BOOST_COMPACT_RUNTIME_OUTPUT}"
        "${UPT42_SPATIAL_REUSE_TEXTURE_PAIR_RESCUE_RUNTIME_OUTPUT}"
        "${UPT43_SPATIAL_SHIFT_PREPASS_RUNTIME_OUTPUT}"
        "${UPT43_SPATIAL_RESAMPLE_RUNTIME_OUTPUT}"
        "${UPT34_SPATIAL_WORKGROUP_MULTI3_RUNTIME_OUTPUT}"
        "${UPT38_SPATIAL_THREE_VERTEX_RUNTIME_OUTPUT}"
        "${UPT42_SPATIAL_REUSE_TEXTURE_THREE_VERTEX_RUNTIME_OUTPUT}")
set_target_properties(PathTraceSlangUpt09Spatial PROPERTIES FOLDER "Shaders/UPT-09")

# UPT-41 production reachability set.  Keep the full historical UPT-09 target
# above for explicit validation, while normal engine builds retain only the
# compact direct diagnostic/fallback, accepted UPT-33 rescue, and optional
# UPT-38 third-vertex replay.
add_custom_target(PathTraceSlangUpt09ProductionRuntime
    DEPENDS
        "${UPT09_COMPACT_RUNTIME_OUTPUT}"
        "${UPT09_COMPACT_LAMBERT_RUNTIME_OUTPUT}"
        "${UPT33_SPATIAL_WORKGROUP_PAIR_RESCUE_RUNTIME_OUTPUT}"
        "${UPT438_SPATIAL_BOOST_CLASSIFY_RUNTIME_OUTPUT}"
        "${UPT438_SPATIAL_BOOST_COMPACT_RUNTIME_OUTPUT}"
        "${UPT438_SPATIAL_BOOST_VERIFY_STAMP}"
        "${UPT38_SPATIAL_THREE_VERTEX_RUNTIME_OUTPUT}"
        "${UPT42_SPATIAL_REUSE_TEXTURE_PAIR_RESCUE_RUNTIME_OUTPUT}"
        "${UPT43_SPATIAL_SHIFT_PREPASS_RUNTIME_OUTPUT}"
        "${UPT43_SPATIAL_RESAMPLE_RUNTIME_OUTPUT}"
        "${UPT42_SPATIAL_REUSE_TEXTURE_THREE_VERTEX_RUNTIME_OUTPUT}"
        "${UPT42_SPATIAL_VERIFY_STAMP}"
        "${UPT435_SPATIAL_VERIFY_STAMP}")
set_target_properties(PathTraceSlangUpt09ProductionRuntime PROPERTIES
    FOLDER "Shaders/UPT-09")
if(TARGET RBDoom3BFG)
    add_dependencies(RBDoom3BFG PathTraceSlangUpt09ProductionRuntime)
endif()

add_custom_target(PathTraceSlangUpt38ThreeVertexSpatial
    DEPENDS ${spatial_shared_workgroup_pair_rescue_three_vertex_outputs}
        "${UPT38_SPATIAL_VERIFY_STAMP}"
        "${UPT38_SPATIAL_THREE_VERTEX_RUNTIME_OUTPUT}")
set_target_properties(PathTraceSlangUpt38ThreeVertexSpatial PROPERTIES
    FOLDER "Shaders/UPT-09")
