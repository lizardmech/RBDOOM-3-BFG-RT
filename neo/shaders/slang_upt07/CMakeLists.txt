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

# UPT-43.6 is a bounded post-temporal pass. It deliberately owns only the
# finalized reservoir page and therefore has no scene, light or RayQuery ABI.
path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_boiling_filter
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_boiling_filter.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT07_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_boiling_filter.slang"
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03/upt03_reservoir_contract.slang"
    OUT_VAR temporal_boiling_filter_outputs)

set(UPT07_DIRECT_MODULES
    "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_direct_rayquery.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_direct_bindings.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_surface_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_reservoir.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt07_material_provider_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt07_material_openpbr.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt07_material_lambert_diagnostic.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt07_emissive_material.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt07_indirect_material.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_indirect.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt07_work_budget.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt10/upt10_reuse_contract.slangh"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03/upt03_reservoir_contract.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_binary16.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_hit_contract.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_initial_work_contract.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_light_contract.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_local_light_proposal.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_local_light_evaluation_direct.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_geometry_contract.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_geometry_bindings.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_geometry_decode.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_emissive_replay.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_initial_bindings.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_initial_control.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_material_contract.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_material_live_contract.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_material_emission.slang"
      "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_material_primary_live.slang"
      "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_primary_surface_live_contract.slang"
      "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_packed_primary_surface.slang"
      "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_packed_primary_surface32.slang"
      "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_primary_history_sidecar.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_random_white_hash.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_replay_random.slang"
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
    DEFINES UPT07_TEMPORAL_GEOMETRY_REPLAY=1
    OUT_VAR temporal_direct_outputs)

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
    DEFINES UPT07_PACKED_LIGHT_RECORD=1 UPT07_TEMPORAL_GEOMETRY_REPLAY=1
    OUT_VAR temporal_direct_compact_outputs)

path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_direct_rayquery_duplication
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
    DEFINES UPT07_TEMPORAL_GEOMETRY_REPLAY=1 UPT07_DUPLICATION_MAP=1
    OUT_VAR temporal_direct_duplication_outputs)

path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_direct_rayquery_light64_duplication
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
    DEFINES UPT07_PACKED_LIGHT_RECORD=1 UPT07_TEMPORAL_GEOMETRY_REPLAY=1 UPT07_DUPLICATION_MAP=1
    OUT_VAR temporal_direct_compact_duplication_outputs)

path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_unified_rayquery
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
    DEFINES UPT07_TEMPORAL_GEOMETRY_REPLAY=1 UPT07_INDIRECT_REPLAY=1
    OUT_VAR temporal_unified_outputs)

path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_unified_rayquery_light64
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
    DEFINES UPT07_PACKED_LIGHT_RECORD=1 UPT07_TEMPORAL_GEOMETRY_REPLAY=1 UPT07_INDIRECT_REPLAY=1
    OUT_VAR temporal_unified_compact_outputs)

path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_unified_rayquery_duplication
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
    DEFINES UPT07_TEMPORAL_GEOMETRY_REPLAY=1 UPT07_DUPLICATION_MAP=1 UPT07_INDIRECT_REPLAY=1
    OUT_VAR temporal_unified_duplication_outputs)

path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_unified_rayquery_light64_duplication
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
    DEFINES UPT07_PACKED_LIGHT_RECORD=1 UPT07_TEMPORAL_GEOMETRY_REPLAY=1 UPT07_DUPLICATION_MAP=1 UPT07_INDIRECT_REPLAY=1
    OUT_VAR temporal_unified_compact_duplication_outputs)

# T30.2 compile-only parity specialization. It is intentionally not copied to
# the runtime shader directory until the shared adapter passes fixed-input
# temporal parity against the accepted artifact.
path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_unified_rayquery_light64_duplication_shared_adapter
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
    DEFINES UPT07_PACKED_LIGHT_RECORD=1 UPT07_TEMPORAL_GEOMETRY_REPLAY=1
        UPT07_DUPLICATION_MAP=1 UPT07_INDIRECT_REPLAY=1
        UPT10_SHARED_REUSE_ADAPTER=1
    OUT_VAR temporal_unified_shared_adapter_outputs)

# T30.3 opt-in estimator specialization. Keep the accepted T30.2 adapter as a
# separate runtime A/B while the common GRIS merge is validated in game.
path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_unified_rayquery_light64_duplication_common_gris
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
    DEFINES UPT07_PACKED_LIGHT_RECORD=1 UPT07_TEMPORAL_GEOMETRY_REPLAY=1
        UPT07_DUPLICATION_MAP=1 UPT07_INDIRECT_REPLAY=1
        UPT10_SHARED_REUSE_ADAPTER=1 UPT10_COMMON_GRIS_MERGE=1
    OUT_VAR temporal_unified_common_gris_outputs)

# UPT-36 keeps the accepted common-GRIS temporal artifact unchanged and adds
# exact replay only for UPT-35 eventKind=4,pathLength=2 endpoints.
path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_unified_rayquery_light64_duplication_common_gris_three_vertex
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
    DEFINES UPT07_PACKED_LIGHT_RECORD=1 UPT07_TEMPORAL_GEOMETRY_REPLAY=1
        UPT07_DUPLICATION_MAP=1 UPT07_INDIRECT_REPLAY=1
        UPT10_SHARED_REUSE_ADAPTER=1 UPT10_COMMON_GRIS_MERGE=1
        UPT07_THREE_VERTEX_REPLAY=1
    OUT_VAR temporal_unified_common_gris_three_vertex_outputs)

# UPT-43.4 compiles exact replay out of the full-screen classifier and retains
# it only in the compact 64x1 consumer. Both variants use the accepted common
# GRIS estimator and reservoir ABI.
path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_unified_rayquery_light64_duplication_common_gris_replay_classify
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
    DEFINES UPT07_PACKED_LIGHT_RECORD=1 UPT07_TEMPORAL_GEOMETRY_REPLAY=1
        UPT07_DUPLICATION_MAP=1 UPT07_INDIRECT_REPLAY=1
        UPT10_SHARED_REUSE_ADAPTER=1 UPT10_COMMON_GRIS_MERGE=1
        UPT43_TEMPORAL_REPLAY_CLASSIFY=1
    OUT_VAR temporal_common_gris_replay_classify_outputs)

path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_unified_rayquery_light64_duplication_common_gris_replay_compact
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
    DEFINES UPT07_PACKED_LIGHT_RECORD=1 UPT07_TEMPORAL_GEOMETRY_REPLAY=1
        UPT07_DUPLICATION_MAP=1 UPT07_INDIRECT_REPLAY=1
        UPT10_SHARED_REUSE_ADAPTER=1 UPT10_COMMON_GRIS_MERGE=1
        UPT43_TEMPORAL_REPLAY_COMPACT=1
    OUT_VAR temporal_common_gris_replay_compact_outputs)

path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_unified_rayquery_light64_duplication_common_gris_three_vertex_replay_classify
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
    DEFINES UPT07_PACKED_LIGHT_RECORD=1 UPT07_TEMPORAL_GEOMETRY_REPLAY=1
        UPT07_DUPLICATION_MAP=1 UPT07_INDIRECT_REPLAY=1
        UPT10_SHARED_REUSE_ADAPTER=1 UPT10_COMMON_GRIS_MERGE=1
        UPT07_THREE_VERTEX_REPLAY=1 UPT43_TEMPORAL_REPLAY_CLASSIFY=1
    OUT_VAR temporal_three_vertex_replay_classify_outputs)

path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_unified_rayquery_light64_duplication_common_gris_three_vertex_replay_compact
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
    DEFINES UPT07_PACKED_LIGHT_RECORD=1 UPT07_TEMPORAL_GEOMETRY_REPLAY=1
        UPT07_DUPLICATION_MAP=1 UPT07_INDIRECT_REPLAY=1
        UPT10_SHARED_REUSE_ADAPTER=1 UPT10_COMMON_GRIS_MERGE=1
        UPT07_THREE_VERTEX_REPLAY=1 UPT43_TEMPORAL_REPLAY_COMPACT=1
    OUT_VAR temporal_three_vertex_replay_compact_outputs)

set(UPT43_REPLAY_COMPACTION_VERIFY_STAMP
    "${UPT07_OUTPUT_DIR}/upt43_temporal_replay_compaction_verified.txt")
add_custom_command(
    OUTPUT "${UPT43_REPLAY_COMPACTION_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT43_CLASSIFY_ASM=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_duplication_common_gris_replay_classify.slang.spvasm
        -DUPT43_COMPACT_ASM=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_duplication_common_gris_replay_compact.slang.spvasm
        -DUPT43_THREE_CLASSIFY_ASM=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_duplication_common_gris_three_vertex_replay_classify.slang.spvasm
        -DUPT43_THREE_COMPACT_ASM=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_duplication_common_gris_three_vertex_replay_compact.slang.spvasm
        -DUPT43_CLASSIFY_REFLECTION=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_duplication_common_gris_replay_classify.slang.spirv-reflection.json
        -DUPT43_COMPACT_REFLECTION=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_duplication_common_gris_replay_compact.slang.spirv-reflection.json
        -DUPT43_TEMPORAL_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_indirect.slang
        -DUPT43_HOST_SOURCE=${CMAKE_CURRENT_LIST_DIR}/../../renderer/NVRHI/PathTraceUnifiedPt.cpp
        -DUPT43_STAMP=${UPT43_REPLAY_COMPACTION_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt43ReplayCompaction.cmake"
    DEPENDS ${temporal_common_gris_replay_classify_outputs}
        ${temporal_common_gris_replay_compact_outputs}
        ${temporal_three_vertex_replay_classify_outputs}
        ${temporal_three_vertex_replay_compact_outputs}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_indirect.slang"
        "${CMAKE_CURRENT_LIST_DIR}/../../renderer/NVRHI/PathTraceUnifiedPt.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt43ReplayCompaction.cmake"
    COMMENT "UPT-43.4 verifying bounded temporal replay compaction"
    VERBATIM)

set(UPT436_TEMPORAL_BOILING_VERIFY_STAMP
    "${UPT07_OUTPUT_DIR}/upt436_temporal_boiling_filter_verified.txt")
add_custom_command(
    OUTPUT "${UPT436_TEMPORAL_BOILING_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT436_ASM=${UPT07_OUTPUT_DIR}/upt07_temporal_boiling_filter.slang.spvasm
        -DUPT436_REFLECTION=${UPT07_OUTPUT_DIR}/upt07_temporal_boiling_filter.slang.spirv-reflection.json
        -DUPT436_SHADER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_boiling_filter.slang
        -DUPT436_HOST_SOURCE=${CMAKE_CURRENT_LIST_DIR}/../../renderer/NVRHI/PathTraceUnifiedPt.cpp
        -DUPT436_STAMP=${UPT436_TEMPORAL_BOILING_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt436TemporalBoilingFilter.cmake"
    DEPENDS ${temporal_boiling_filter_outputs}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_boiling_filter.slang"
        "${CMAKE_CURRENT_LIST_DIR}/../../renderer/NVRHI/PathTraceUnifiedPt.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt436TemporalBoilingFilter.cmake"
    COMMENT "UPT-43.6 verifying bounded post-temporal boiling filter"
    VERBATIM)

set(UPT437_TEMPORAL_PERMUTATION_VERIFY_STAMP
    "${UPT07_OUTPUT_DIR}/upt437_temporal_permutation_verified.txt")
add_custom_command(
    OUTPUT "${UPT437_TEMPORAL_PERMUTATION_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT437_ASM=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_duplication_common_gris_replay_classify.slang.spvasm
        -DUPT437_SHADER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_direct_rayquery.slang
        -DUPT437_HOST_SOURCE=${CMAKE_CURRENT_LIST_DIR}/../../renderer/NVRHI/PathTraceUnifiedPt.cpp
        -DUPT437_DISPATCH_SOURCE=${CMAKE_CURRENT_LIST_DIR}/../../renderer/NVRHI/PathTraceSmokeDispatch.cpp
        -DUPT437_STAMP=${UPT437_TEMPORAL_PERMUTATION_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt437TemporalPermutation.cmake"
    DEPENDS ${temporal_common_gris_replay_classify_outputs}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_direct_rayquery.slang"
        "${CMAKE_CURRENT_LIST_DIR}/../../renderer/NVRHI/PathTraceUnifiedPt.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../renderer/NVRHI/PathTraceSmokeDispatch.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt437TemporalPermutation.cmake"
    COMMENT "UPT-43.7 verifying first-tap temporal permutation"
    VERBATIM)

set(UPT10_TEMPORAL_ADAPTER_VERIFY_STAMP
    "${UPT07_OUTPUT_DIR}/upt10_temporal_shared_adapter_verified.txt")
add_custom_command(
    OUTPUT "${UPT10_TEMPORAL_ADAPTER_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT10_REFLECTION=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_duplication_shared_adapter.slang.spirv-reflection.json
        -DUPT10_DISASSEMBLY=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_duplication_shared_adapter.slang.spvasm
        -DUPT10_STAMP=${UPT10_TEMPORAL_ADAPTER_VERIFY_STAMP}
        -DUPT10_HOST_SOURCE=${CMAKE_CURRENT_LIST_DIR}/../../renderer/NVRHI/PathTraceUnifiedPt.cpp
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt10TemporalAdapter.cmake"
    DEPENDS ${temporal_unified_shared_adapter_outputs}
        "${CMAKE_CURRENT_LIST_DIR}/../../renderer/NVRHI/PathTraceUnifiedPt.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt10TemporalAdapter.cmake"
    COMMENT "UPT-30 verifying compile-only temporal shared adapter ceiling"
    VERBATIM)

set(UPT10_TEMPORAL_COMMON_GRIS_VERIFY_STAMP
    "${UPT07_OUTPUT_DIR}/upt10_temporal_common_gris_verified.txt")
add_custom_command(
    OUTPUT "${UPT10_TEMPORAL_COMMON_GRIS_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT10_REFLECTION=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_duplication_common_gris.slang.spirv-reflection.json
        -DUPT10_DISASSEMBLY=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_duplication_common_gris.slang.spvasm
        -DUPT10_STAMP=${UPT10_TEMPORAL_COMMON_GRIS_VERIFY_STAMP}
        -DUPT10_LABEL=common-GRIS
        -DUPT10_HOST_SOURCE=${CMAKE_CURRENT_LIST_DIR}/../../renderer/NVRHI/PathTraceUnifiedPt.cpp
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt10TemporalAdapter.cmake"
    DEPENDS ${temporal_unified_common_gris_outputs}
        "${CMAKE_CURRENT_LIST_DIR}/../../renderer/NVRHI/PathTraceUnifiedPt.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt10TemporalAdapter.cmake"
    COMMENT "UPT-30 verifying common GRIS temporal ceiling"
    VERBATIM)

set(UPT36_TEMPORAL_VERIFY_STAMP
    "${UPT07_OUTPUT_DIR}/upt36_three_vertex_temporal_verified.txt")
add_custom_command(
    OUTPUT "${UPT36_TEMPORAL_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT36_REFLECTION=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_duplication_common_gris_three_vertex.slang.spirv-reflection.json
        -DUPT36_DISASSEMBLY=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_duplication_common_gris_three_vertex.slang.spvasm
        -DUPT36_STAMP=${UPT36_TEMPORAL_VERIFY_STAMP}
        -DUPT36_HOST_SOURCE=${CMAKE_CURRENT_LIST_DIR}/../../renderer/NVRHI/PathTraceUnifiedPt.cpp
        -DUPT36_REPLAY_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_indirect.slang
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt36ThreeVertexTemporal.cmake"
    DEPENDS ${temporal_unified_common_gris_three_vertex_outputs}
        "${CMAKE_CURRENT_LIST_DIR}/../../renderer/NVRHI/PathTraceUnifiedPt.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_indirect.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt36ThreeVertexTemporal.cmake"
    COMMENT "UPT-36 verifying bounded three-vertex temporal replay"
    VERBATIM)

function(upt07_declare_frozen_factor_variant variant_name)
    path_tracing_declare_slang_spirv_module(
        NAME "${variant_name}"
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_direct_rayquery.slang"
        ENTRY main STAGE compute OUTPUT_DIR "${UPT07_OUTPUT_DIR}"
        INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_LIST_DIR}/.."
            "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
            "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04"
        DEPENDS ${UPT07_DIRECT_MODULES}
        DEFINES UPT07_TEMPORAL_GEOMETRY_REPLAY=1
            UPT07_INDIRECT_REPLAY=1 ${ARGN}
        OUT_VAR frozen_outputs)
    set(UPT07_FROZEN_OUTPUTS ${UPT07_FROZEN_OUTPUTS} ${frozen_outputs}
        PARENT_SCOPE)
endfunction()
upt07_declare_frozen_factor_variant(
    upt07_temporal_unified_rayquery_frozen_static
    UPT_FROZEN_STATIC_SCENE=1)
upt07_declare_frozen_factor_variant(
    upt07_temporal_unified_rayquery_frozen_static_duplication
    UPT_FROZEN_STATIC_SCENE=1 UPT07_DUPLICATION_MAP=1)
upt07_declare_frozen_factor_variant(
    upt07_temporal_unified_rayquery_frozen_lights
    UPT_FROZEN_FLAT_LIGHTS=1)
upt07_declare_frozen_factor_variant(
    upt07_temporal_unified_rayquery_frozen_lights_duplication
    UPT_FROZEN_FLAT_LIGHTS=1 UPT07_DUPLICATION_MAP=1)
upt07_declare_frozen_factor_variant(
    upt07_temporal_unified_rayquery_frozen_geometry
    UPT_FROZEN_STATIC_GEOMETRY=1)
upt07_declare_frozen_factor_variant(
    upt07_temporal_unified_rayquery_frozen_geometry_duplication
    UPT_FROZEN_STATIC_GEOMETRY=1 UPT07_DUPLICATION_MAP=1)

function(upt07_declare_lambert_variant variant_name)
    path_tracing_declare_slang_spirv_module(
        NAME "${variant_name}"
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_direct_rayquery.slang"
        ENTRY main STAGE compute OUTPUT_DIR "${UPT07_OUTPUT_DIR}"
        INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_LIST_DIR}/.."
            "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
            "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04"
        DEPENDS ${UPT07_DIRECT_MODULES}
        DEFINES UPT07_TEMPORAL_GEOMETRY_REPLAY=1 UPT07_INDIRECT_REPLAY=1
            UPT_DIAGNOSTIC_LAMBERT=1 ${ARGN}
        OUT_VAR lambert_outputs)
    set(UPT07_LAMBERT_OUTPUTS ${UPT07_LAMBERT_OUTPUTS} ${lambert_outputs}
        PARENT_SCOPE)
endfunction()
upt07_declare_lambert_variant(upt07_temporal_unified_rayquery_lambert)
upt07_declare_lambert_variant(
    upt07_temporal_unified_rayquery_frozen_static_lambert
    UPT_FROZEN_STATIC_SCENE=1)
upt07_declare_lambert_variant(
    upt07_temporal_unified_rayquery_frozen_static_duplication_lambert
    UPT_FROZEN_STATIC_SCENE=1 UPT07_DUPLICATION_MAP=1)
upt07_declare_lambert_variant(
    upt07_temporal_unified_rayquery_frozen_lights_lambert
    UPT_FROZEN_FLAT_LIGHTS=1)
upt07_declare_lambert_variant(
    upt07_temporal_unified_rayquery_frozen_lights_duplication_lambert
    UPT_FROZEN_FLAT_LIGHTS=1 UPT07_DUPLICATION_MAP=1)
upt07_declare_lambert_variant(
    upt07_temporal_unified_rayquery_frozen_geometry_lambert
    UPT_FROZEN_STATIC_GEOMETRY=1)
upt07_declare_lambert_variant(
    upt07_temporal_unified_rayquery_frozen_geometry_duplication_lambert
    UPT_FROZEN_STATIC_GEOMETRY=1 UPT07_DUPLICATION_MAP=1)
upt07_declare_lambert_variant(upt07_temporal_unified_rayquery_light64_lambert
    UPT07_PACKED_LIGHT_RECORD=1)
upt07_declare_lambert_variant(upt07_temporal_unified_rayquery_duplication_lambert
    UPT07_DUPLICATION_MAP=1)
upt07_declare_lambert_variant(
    upt07_temporal_unified_rayquery_light64_duplication_lambert
    UPT07_PACKED_LIGHT_RECORD=1 UPT07_DUPLICATION_MAP=1)

set(UPT07_BOTTLENECK_OUTPUTS)
foreach(bottleneck_stage RANGE 1 11)
    set(bottleneck_module
        "upt07_temporal_unified_rayquery_light64_duplication_probe${bottleneck_stage}")
    path_tracing_declare_slang_spirv_module(
        NAME "${bottleneck_module}"
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_direct_rayquery.slang"
        ENTRY main STAGE compute OUTPUT_DIR "${UPT07_OUTPUT_DIR}"
        INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_LIST_DIR}/.."
            "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
            "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04"
        DEPENDS ${UPT07_DIRECT_MODULES}
        DEFINES UPT07_TEMPORAL_GEOMETRY_REPLAY=1 UPT07_INDIRECT_REPLAY=1
            UPT07_PACKED_LIGHT_RECORD=1 UPT07_DUPLICATION_MAP=1
            UPT07_BOTTLENECK_STAGE=${bottleneck_stage}
        OUT_VAR bottleneck_outputs)
    list(APPEND UPT07_BOTTLENECK_OUTPUTS ${bottleneck_outputs})
endforeach()

path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_unified_rayquery_light64_duplication_probe12
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_direct_rayquery.slang"
    ENTRY main STAGE compute OUTPUT_DIR "${UPT07_OUTPUT_DIR}"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04"
    DEPENDS ${UPT07_DIRECT_MODULES}
    DEFINES UPT07_TEMPORAL_GEOMETRY_REPLAY=1 UPT07_INDIRECT_REPLAY=1
        UPT07_PACKED_LIGHT_RECORD=1 UPT07_DUPLICATION_MAP=1
        UPT10_SHARED_REUSE_ADAPTER=1 UPT10_COMMON_GRIS_MERGE=1
        UPT07_BOTTLENECK_STAGE=12
    OUT_VAR upt07_work_budget_outputs)
list(APPEND UPT07_BOTTLENECK_OUTPUTS ${upt07_work_budget_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_unified_rayquery_route_diag
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_direct_rayquery.slang"
    ENTRY main STAGE compute OUTPUT_DIR "${UPT07_OUTPUT_DIR}"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03" "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04"
    DEPENDS ${UPT07_DIRECT_MODULES}
    DEFINES UPT07_TEMPORAL_GEOMETRY_REPLAY=1 UPT07_INDIRECT_REPLAY=1 UPT07_ROUTE_DIAGNOSTICS=1
    OUT_VAR temporal_unified_route_diag_outputs)

path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_unified_rayquery_light64_route_diag
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_direct_rayquery.slang"
    ENTRY main STAGE compute OUTPUT_DIR "${UPT07_OUTPUT_DIR}"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03" "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04"
    DEPENDS ${UPT07_DIRECT_MODULES}
    DEFINES UPT07_PACKED_LIGHT_RECORD=1 UPT07_TEMPORAL_GEOMETRY_REPLAY=1 UPT07_INDIRECT_REPLAY=1 UPT07_ROUTE_DIAGNOSTICS=1
    OUT_VAR temporal_unified_compact_route_diag_outputs)

path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_unified_rayquery_duplication_route_diag
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_direct_rayquery.slang"
    ENTRY main STAGE compute OUTPUT_DIR "${UPT07_OUTPUT_DIR}"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03" "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04"
    DEPENDS ${UPT07_DIRECT_MODULES}
    DEFINES UPT07_TEMPORAL_GEOMETRY_REPLAY=1 UPT07_DUPLICATION_MAP=1 UPT07_INDIRECT_REPLAY=1 UPT07_ROUTE_DIAGNOSTICS=1
    OUT_VAR temporal_unified_duplication_route_diag_outputs)

path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_unified_rayquery_light64_duplication_route_diag
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_direct_rayquery.slang"
    ENTRY main STAGE compute OUTPUT_DIR "${UPT07_OUTPUT_DIR}"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03" "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04"
    DEPENDS ${UPT07_DIRECT_MODULES}
    DEFINES UPT07_PACKED_LIGHT_RECORD=1 UPT07_TEMPORAL_GEOMETRY_REPLAY=1 UPT07_DUPLICATION_MAP=1 UPT07_INDIRECT_REPLAY=1 UPT07_ROUTE_DIAGNOSTICS=1
    OUT_VAR temporal_unified_compact_duplication_route_diag_outputs)

path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_unified_rayquery_reconnect
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_direct_rayquery.slang"
    ENTRY main STAGE compute OUTPUT_DIR "${UPT07_OUTPUT_DIR}"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03" "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04"
    DEPENDS ${UPT07_DIRECT_MODULES}
    DEFINES UPT07_TEMPORAL_GEOMETRY_REPLAY=1 UPT07_INDIRECT_REPLAY=1 UPT07_EARLY_RECONNECT=1
    OUT_VAR temporal_unified_reconnect_outputs)

path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_unified_rayquery_light64_reconnect
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_direct_rayquery.slang"
    ENTRY main STAGE compute OUTPUT_DIR "${UPT07_OUTPUT_DIR}"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03" "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04"
    DEPENDS ${UPT07_DIRECT_MODULES}
    DEFINES UPT07_PACKED_LIGHT_RECORD=1 UPT07_TEMPORAL_GEOMETRY_REPLAY=1 UPT07_INDIRECT_REPLAY=1 UPT07_EARLY_RECONNECT=1
    OUT_VAR temporal_unified_compact_reconnect_outputs)

path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_unified_rayquery_duplication_reconnect
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_direct_rayquery.slang"
    ENTRY main STAGE compute OUTPUT_DIR "${UPT07_OUTPUT_DIR}"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03" "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04"
    DEPENDS ${UPT07_DIRECT_MODULES}
    DEFINES UPT07_TEMPORAL_GEOMETRY_REPLAY=1 UPT07_DUPLICATION_MAP=1 UPT07_INDIRECT_REPLAY=1 UPT07_EARLY_RECONNECT=1
    OUT_VAR temporal_unified_duplication_reconnect_outputs)

path_tracing_declare_slang_spirv_module(
    NAME upt07_temporal_unified_rayquery_light64_duplication_reconnect
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_direct_rayquery.slang"
    ENTRY main STAGE compute OUTPUT_DIR "${UPT07_OUTPUT_DIR}"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03" "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04"
    DEPENDS ${UPT07_DIRECT_MODULES}
    DEFINES UPT07_PACKED_LIGHT_RECORD=1 UPT07_TEMPORAL_GEOMETRY_REPLAY=1 UPT07_DUPLICATION_MAP=1 UPT07_INDIRECT_REPLAY=1 UPT07_EARLY_RECONNECT=1
    OUT_VAR temporal_unified_compact_duplication_reconnect_outputs)

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
        -DUPT07_UNIFIED_REFLECTION=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery.slang.spirv-reflection.json
        -DUPT07_UNIFIED_DISASSEMBLY=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery.slang.spvasm
        -DUPT07_UNIFIED_COMPACT_REFLECTION=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64.slang.spirv-reflection.json
        -DUPT07_UNIFIED_COMPACT_DISASSEMBLY=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64.slang.spvasm
        -DUPT07_ROUTE_DIAG_REFLECTION=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_route_diag.slang.spirv-reflection.json
        -DUPT07_ROUTE_DIAG_DISASSEMBLY=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_route_diag.slang.spvasm
        -DUPT07_ROUTE_DIAG_COMPACT_REFLECTION=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_route_diag.slang.spirv-reflection.json
        -DUPT07_ROUTE_DIAG_COMPACT_DISASSEMBLY=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_route_diag.slang.spvasm
        -DUPT07_RECONNECT_REFLECTION=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_reconnect.slang.spirv-reflection.json
        -DUPT07_RECONNECT_DISASSEMBLY=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_reconnect.slang.spvasm
        -DUPT07_RECONNECT_COMPACT_REFLECTION=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_reconnect.slang.spirv-reflection.json
        -DUPT07_RECONNECT_COMPACT_DISASSEMBLY=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_reconnect.slang.spvasm
        -DUPT07_WORK_BUDGET_REFLECTION=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_duplication_probe12.slang.spirv-reflection.json
        -DUPT07_WORK_BUDGET_DISASSEMBLY=${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_duplication_probe12.slang.spvasm
        -DUPT07_TEMPORAL_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_indirect.slang
        -DUPT07_WORK_BUDGET_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/upt07_work_budget.slang
        -DUPT07_STAMP=${UPT07_DIRECT_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt07TemporalDirect.cmake"
    DEPENDS ${temporal_direct_outputs} ${temporal_direct_compact_outputs}
        ${temporal_unified_outputs} ${temporal_unified_compact_outputs}
        ${temporal_unified_route_diag_outputs}
        ${temporal_unified_compact_route_diag_outputs}
        ${temporal_unified_reconnect_outputs}
        ${temporal_unified_compact_reconnect_outputs}
        ${upt07_work_budget_outputs}
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt07TemporalDirect.cmake"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt07_work_budget.slang"
    COMMENT "UPT-07 verifying complete primary-direct temporal shift provider"
    VERBATIM)

set(UPT07_RUNTIME_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../base/renderprogs2/spirv/builtin/pathtracing/slang_upt07")
set(UPT07_DIRECT_RUNTIME_OUTPUT
    "${UPT07_RUNTIME_DIR}/upt07_temporal_direct_rayquery.bin")
set(UPT07_DIRECT_COMPACT_RUNTIME_OUTPUT
    "${UPT07_RUNTIME_DIR}/upt07_temporal_direct_rayquery_light64.bin")
set(UPT07_DIRECT_DUPLICATION_RUNTIME_OUTPUT
    "${UPT07_RUNTIME_DIR}/upt07_temporal_direct_rayquery_duplication.bin")
set(UPT07_DIRECT_COMPACT_DUPLICATION_RUNTIME_OUTPUT
    "${UPT07_RUNTIME_DIR}/upt07_temporal_direct_rayquery_light64_duplication.bin")
set(UPT07_UNIFIED_RUNTIME_OUTPUT
    "${UPT07_RUNTIME_DIR}/upt07_temporal_unified_rayquery.bin")
set(UPT07_UNIFIED_COMPACT_RUNTIME_OUTPUT
    "${UPT07_RUNTIME_DIR}/upt07_temporal_unified_rayquery_light64.bin")
set(UPT07_UNIFIED_DUPLICATION_RUNTIME_OUTPUT
    "${UPT07_RUNTIME_DIR}/upt07_temporal_unified_rayquery_duplication.bin")
set(UPT07_UNIFIED_COMPACT_DUPLICATION_RUNTIME_OUTPUT
    "${UPT07_RUNTIME_DIR}/upt07_temporal_unified_rayquery_light64_duplication.bin")
set(UPT10_TEMPORAL_SHARED_ADAPTER_RUNTIME_OUTPUT
    "${UPT07_RUNTIME_DIR}/upt07_temporal_unified_rayquery_light64_duplication_shared_adapter.bin")
add_custom_command(
    OUTPUT "${UPT10_TEMPORAL_SHARED_ADAPTER_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT07_RUNTIME_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_duplication_shared_adapter.slang.spv"
        "${UPT10_TEMPORAL_SHARED_ADAPTER_RUNTIME_OUTPUT}"
    DEPENDS ${temporal_unified_shared_adapter_outputs}
        "${UPT10_TEMPORAL_ADAPTER_VERIFY_STAMP}"
    COMMENT "UPT-30 Vulkan opt-in temporal shared-adapter artifact"
    VERBATIM)
set(UPT10_TEMPORAL_COMMON_GRIS_RUNTIME_OUTPUT
    "${UPT07_RUNTIME_DIR}/upt07_temporal_unified_rayquery_light64_duplication_common_gris.bin")
add_custom_command(
    OUTPUT "${UPT10_TEMPORAL_COMMON_GRIS_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT07_RUNTIME_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_duplication_common_gris.slang.spv"
        "${UPT10_TEMPORAL_COMMON_GRIS_RUNTIME_OUTPUT}"
    DEPENDS ${temporal_unified_common_gris_outputs}
        "${UPT10_TEMPORAL_COMMON_GRIS_VERIFY_STAMP}"
    COMMENT "UPT-30 Vulkan opt-in temporal common-GRIS artifact"
    VERBATIM)
set(UPT36_TEMPORAL_RUNTIME_OUTPUT
    "${UPT07_RUNTIME_DIR}/upt07_temporal_unified_rayquery_light64_duplication_common_gris_three_vertex.bin")
add_custom_command(
    OUTPUT "${UPT36_TEMPORAL_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT07_RUNTIME_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_duplication_common_gris_three_vertex.slang.spv"
        "${UPT36_TEMPORAL_RUNTIME_OUTPUT}"
    DEPENDS ${temporal_unified_common_gris_three_vertex_outputs}
        "${UPT36_TEMPORAL_VERIFY_STAMP}"
    COMMENT "UPT-36 Vulkan opt-in three-vertex temporal artifact"
    VERBATIM)
set(UPT43_TEMPORAL_RUNTIME_OUTPUTS)
foreach(upt43_module
        upt07_temporal_unified_rayquery_light64_duplication_common_gris_replay_classify
        upt07_temporal_unified_rayquery_light64_duplication_common_gris_replay_compact
        upt07_temporal_unified_rayquery_light64_duplication_common_gris_three_vertex_replay_classify
        upt07_temporal_unified_rayquery_light64_duplication_common_gris_three_vertex_replay_compact)
    set(upt43_runtime "${UPT07_RUNTIME_DIR}/${upt43_module}.bin")
    add_custom_command(
        OUTPUT "${upt43_runtime}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT07_RUNTIME_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${UPT07_OUTPUT_DIR}/${upt43_module}.slang.spv"
            "${upt43_runtime}"
        DEPENDS "${UPT07_OUTPUT_DIR}/${upt43_module}.slang.spv"
        COMMENT "UPT-43.4 Vulkan temporal replay compaction artifact: ${upt43_module}"
        VERBATIM)
    list(APPEND UPT43_TEMPORAL_RUNTIME_OUTPUTS "${upt43_runtime}")
endforeach()
set(UPT436_TEMPORAL_BOILING_RUNTIME_OUTPUT
    "${UPT07_RUNTIME_DIR}/upt07_temporal_boiling_filter.bin")
add_custom_command(
    OUTPUT "${UPT436_TEMPORAL_BOILING_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT07_RUNTIME_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT07_OUTPUT_DIR}/upt07_temporal_boiling_filter.slang.spv"
        "${UPT436_TEMPORAL_BOILING_RUNTIME_OUTPUT}"
    DEPENDS ${temporal_boiling_filter_outputs}
        "${UPT436_TEMPORAL_BOILING_VERIFY_STAMP}"
    COMMENT "UPT-43.6 Vulkan temporal boiling-filter artifact"
    VERBATIM)
set(UPT07_LAMBERT_RUNTIME_OUTPUTS)
foreach(lambert_module
        upt07_temporal_unified_rayquery_lambert
        upt07_temporal_unified_rayquery_light64_lambert
        upt07_temporal_unified_rayquery_duplication_lambert
        upt07_temporal_unified_rayquery_light64_duplication_lambert
        upt07_temporal_unified_rayquery_frozen_static_lambert
        upt07_temporal_unified_rayquery_frozen_static_duplication_lambert
        upt07_temporal_unified_rayquery_frozen_lights_lambert
        upt07_temporal_unified_rayquery_frozen_lights_duplication_lambert
        upt07_temporal_unified_rayquery_frozen_geometry_lambert
        upt07_temporal_unified_rayquery_frozen_geometry_duplication_lambert)
    set(lambert_runtime "${UPT07_RUNTIME_DIR}/${lambert_module}.bin")
    add_custom_command(
        OUTPUT "${lambert_runtime}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT07_RUNTIME_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${UPT07_OUTPUT_DIR}/${lambert_module}.slang.spv"
            "${lambert_runtime}"
        DEPENDS "${UPT07_OUTPUT_DIR}/${lambert_module}.slang.spv"
        COMMENT "UPT-07 Vulkan Lambert diagnostic artifact: ${lambert_module}"
        VERBATIM)
    list(APPEND UPT07_LAMBERT_RUNTIME_OUTPUTS "${lambert_runtime}")
endforeach()
set(UPT07_FROZEN_RUNTIME_OUTPUTS)
foreach(frozen_module
        upt07_temporal_unified_rayquery_frozen_static
        upt07_temporal_unified_rayquery_frozen_static_duplication
        upt07_temporal_unified_rayquery_frozen_lights
        upt07_temporal_unified_rayquery_frozen_lights_duplication
        upt07_temporal_unified_rayquery_frozen_geometry
        upt07_temporal_unified_rayquery_frozen_geometry_duplication)
    set(frozen_runtime "${UPT07_RUNTIME_DIR}/${frozen_module}.bin")
    add_custom_command(
        OUTPUT "${frozen_runtime}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT07_RUNTIME_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${UPT07_OUTPUT_DIR}/${frozen_module}.slang.spv"
            "${frozen_runtime}"
        DEPENDS ${UPT07_FROZEN_OUTPUTS}
        COMMENT "UPT frozen-static temporal artifact: ${frozen_module}"
        VERBATIM)
    list(APPEND UPT07_FROZEN_RUNTIME_OUTPUTS "${frozen_runtime}")
endforeach()
set(UPT07_BOTTLENECK_RUNTIME_OUTPUTS)
foreach(bottleneck_stage RANGE 1 12)
    set(bottleneck_module
        "upt07_temporal_unified_rayquery_light64_duplication_probe${bottleneck_stage}")
    set(bottleneck_runtime "${UPT07_RUNTIME_DIR}/${bottleneck_module}.bin")
    add_custom_command(
        OUTPUT "${bottleneck_runtime}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT07_RUNTIME_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${UPT07_OUTPUT_DIR}/${bottleneck_module}.slang.spv"
            "${bottleneck_runtime}"
        DEPENDS ${UPT07_BOTTLENECK_OUTPUTS}
        COMMENT "UPT-22 Vulkan T0 bottleneck probe artifact: stage ${bottleneck_stage}"
        VERBATIM)
    list(APPEND UPT07_BOTTLENECK_RUNTIME_OUTPUTS "${bottleneck_runtime}")
endforeach()
set(UPT07_UNIFIED_ROUTE_DIAG_RUNTIME_OUTPUT
    "${UPT07_RUNTIME_DIR}/upt07_temporal_unified_rayquery_route_diag.bin")
set(UPT07_UNIFIED_COMPACT_ROUTE_DIAG_RUNTIME_OUTPUT
    "${UPT07_RUNTIME_DIR}/upt07_temporal_unified_rayquery_light64_route_diag.bin")
set(UPT07_UNIFIED_DUPLICATION_ROUTE_DIAG_RUNTIME_OUTPUT
    "${UPT07_RUNTIME_DIR}/upt07_temporal_unified_rayquery_duplication_route_diag.bin")
set(UPT07_UNIFIED_COMPACT_DUPLICATION_ROUTE_DIAG_RUNTIME_OUTPUT
    "${UPT07_RUNTIME_DIR}/upt07_temporal_unified_rayquery_light64_duplication_route_diag.bin")
set(UPT07_UNIFIED_RECONNECT_RUNTIME_OUTPUT
    "${UPT07_RUNTIME_DIR}/upt07_temporal_unified_rayquery_reconnect.bin")
set(UPT07_UNIFIED_COMPACT_RECONNECT_RUNTIME_OUTPUT
    "${UPT07_RUNTIME_DIR}/upt07_temporal_unified_rayquery_light64_reconnect.bin")
set(UPT07_UNIFIED_DUPLICATION_RECONNECT_RUNTIME_OUTPUT
    "${UPT07_RUNTIME_DIR}/upt07_temporal_unified_rayquery_duplication_reconnect.bin")
set(UPT07_UNIFIED_COMPACT_DUPLICATION_RECONNECT_RUNTIME_OUTPUT
    "${UPT07_RUNTIME_DIR}/upt07_temporal_unified_rayquery_light64_duplication_reconnect.bin")
set(UPT07_CORE_RUNTIME_MODULES
    upt07_temporal_direct_rayquery
    upt07_temporal_direct_rayquery_light64
    upt07_temporal_direct_rayquery_duplication
    upt07_temporal_direct_rayquery_light64_duplication
    upt07_temporal_unified_rayquery
    upt07_temporal_unified_rayquery_light64
    upt07_temporal_unified_rayquery_duplication
    upt07_temporal_unified_rayquery_light64_duplication
    upt07_temporal_unified_rayquery_route_diag
    upt07_temporal_unified_rayquery_light64_route_diag
    upt07_temporal_unified_rayquery_duplication_route_diag
    upt07_temporal_unified_rayquery_light64_duplication_route_diag
    upt07_temporal_unified_rayquery_reconnect
    upt07_temporal_unified_rayquery_light64_reconnect
    upt07_temporal_unified_rayquery_duplication_reconnect
    upt07_temporal_unified_rayquery_light64_duplication_reconnect)
foreach(runtime_module IN LISTS UPT07_CORE_RUNTIME_MODULES)
    add_custom_command(
        OUTPUT "${UPT07_RUNTIME_DIR}/${runtime_module}.bin"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT07_RUNTIME_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${UPT07_OUTPUT_DIR}/${runtime_module}.slang.spv"
            "${UPT07_RUNTIME_DIR}/${runtime_module}.bin"
        DEPENDS "${UPT07_OUTPUT_DIR}/${runtime_module}.slang.spv"
        COMMENT "UPT-07 Vulkan temporal artifact: ${runtime_module}"
        VERBATIM)
endforeach()

add_custom_target(PathTraceSlangUpt07TemporalOwnership
    DEPENDS ${temporal_ownership_outputs} "${UPT07_VERIFY_STAMP}"
        ${temporal_direct_outputs} ${temporal_direct_compact_outputs}
        "${UPT07_DIRECT_VERIFY_STAMP}"
        "${UPT07_DIRECT_RUNTIME_OUTPUT}"
        "${UPT07_DIRECT_COMPACT_RUNTIME_OUTPUT}"
        "${UPT07_DIRECT_DUPLICATION_RUNTIME_OUTPUT}"
        "${UPT07_DIRECT_COMPACT_DUPLICATION_RUNTIME_OUTPUT}"
        "${UPT07_UNIFIED_RUNTIME_OUTPUT}"
        "${UPT07_UNIFIED_COMPACT_RUNTIME_OUTPUT}"
        "${UPT07_UNIFIED_DUPLICATION_RUNTIME_OUTPUT}"
        "${UPT07_UNIFIED_COMPACT_DUPLICATION_RUNTIME_OUTPUT}"
        "${UPT07_UNIFIED_ROUTE_DIAG_RUNTIME_OUTPUT}"
        "${UPT07_UNIFIED_COMPACT_ROUTE_DIAG_RUNTIME_OUTPUT}"
        "${UPT07_UNIFIED_DUPLICATION_ROUTE_DIAG_RUNTIME_OUTPUT}"
        "${UPT07_UNIFIED_COMPACT_DUPLICATION_ROUTE_DIAG_RUNTIME_OUTPUT}"
        ${UPT07_LAMBERT_RUNTIME_OUTPUTS}
        ${UPT07_FROZEN_RUNTIME_OUTPUTS}
        ${UPT07_BOTTLENECK_RUNTIME_OUTPUTS}
        ${temporal_unified_shared_adapter_outputs}
        "${UPT10_TEMPORAL_ADAPTER_VERIFY_STAMP}"
        "${UPT10_TEMPORAL_SHARED_ADAPTER_RUNTIME_OUTPUT}"
        ${temporal_unified_common_gris_outputs}
        "${UPT10_TEMPORAL_COMMON_GRIS_VERIFY_STAMP}"
        "${UPT10_TEMPORAL_COMMON_GRIS_RUNTIME_OUTPUT}"
        ${temporal_unified_common_gris_three_vertex_outputs}
        "${UPT36_TEMPORAL_VERIFY_STAMP}"
        "${UPT36_TEMPORAL_RUNTIME_OUTPUT}"
        ${temporal_common_gris_replay_classify_outputs}
        ${temporal_common_gris_replay_compact_outputs}
        ${temporal_three_vertex_replay_classify_outputs}
        ${temporal_three_vertex_replay_compact_outputs}
        "${UPT43_REPLAY_COMPACTION_VERIFY_STAMP}"
        ${UPT43_TEMPORAL_RUNTIME_OUTPUTS}
        "${UPT436_TEMPORAL_BOILING_RUNTIME_OUTPUT}")
set_target_properties(PathTraceSlangUpt07TemporalOwnership PROPERTIES
    FOLDER "Shaders/UPT-07")

# UPT-41 production reachability set.  The full ownership target above keeps
# every historical route/reconnect/frozen/probe specialization explicitly
# buildable.  The engine only needs the accepted common-GRIS path, its shared
# adapter rollback, the bounded third-vertex extension, and the compact-light
# Lambert diagnostic during normal iteration.
add_custom_target(PathTraceSlangUpt07ProductionRuntime
    DEPENDS
        "${UPT07_DIRECT_COMPACT_DUPLICATION_RUNTIME_OUTPUT}"
        "${UPT07_UNIFIED_COMPACT_DUPLICATION_RUNTIME_OUTPUT}"
        "${UPT10_TEMPORAL_SHARED_ADAPTER_RUNTIME_OUTPUT}"
        "${UPT10_TEMPORAL_COMMON_GRIS_RUNTIME_OUTPUT}"
        "${UPT36_TEMPORAL_RUNTIME_OUTPUT}"
        "${UPT43_REPLAY_COMPACTION_VERIFY_STAMP}"
        ${UPT43_TEMPORAL_RUNTIME_OUTPUTS}
        "${UPT436_TEMPORAL_BOILING_RUNTIME_OUTPUT}"
        "${UPT437_TEMPORAL_PERMUTATION_VERIFY_STAMP}"
        "${UPT07_RUNTIME_DIR}/upt07_temporal_unified_rayquery_light64_duplication_lambert.bin")
set_target_properties(PathTraceSlangUpt07ProductionRuntime PROPERTIES
    FOLDER "Shaders/UPT-07")
if(TARGET RBDoom3BFG)
    add_dependencies(RBDoom3BFG PathTraceSlangUpt07ProductionRuntime)
endif()

add_custom_target(PathTraceSlangUpt36ThreeVertexTemporal
    DEPENDS ${temporal_unified_common_gris_three_vertex_outputs}
        "${UPT36_TEMPORAL_VERIFY_STAMP}"
        "${UPT36_TEMPORAL_RUNTIME_OUTPUT}")
set_target_properties(PathTraceSlangUpt36ThreeVertexTemporal PROPERTIES
    FOLDER "Shaders/UPT-07")
