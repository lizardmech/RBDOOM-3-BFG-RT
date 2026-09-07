include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/PathTracingSlang.cmake)

option(PATH_TRACING_ENABLE_SLANG_UPT04
    "Enable the isolated UPT-04 module-ownership compile gate"
    OFF)

if(NOT PATH_TRACING_ENABLE_SLANG_UPT04)
    return()
endif()
if(NOT USE_VULKAN)
    message(STATUS "UPT-04 Slang module gate disabled because USE_VULKAN is OFF")
    return()
endif()

set(UPT04_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/artifacts")
set(UPT04_RANDOM_SLOT_LEDGER
    "${CMAKE_CURRENT_LIST_DIR}/../PathTraceUnifiedPtRandomDimensions.inc")
set(UPT04_MODULES
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_surface_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_path_event_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_set_baseline_probe.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_set_deterministic_probe.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_random_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_random_white_hash.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_replay_random.slang"
    "${UPT04_RANDOM_SLOT_LEDGER}"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03/upt03_reservoir_contract.slang")

set(UPT04_BACKEND_MODULES
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_binary16.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_primary_surface_live_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_packed_primary_surface.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_packed_primary_surface32.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_primary_history_sidecar.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_hit_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_light_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_light_tile_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_control.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_live_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_classifier.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_alpha_clip.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_emission.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_provider_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_bindings.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_local_light_proposal.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_transport_initial_bounded.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_set_live_probe.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_primary_live.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt05_material_openpbr.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt05_material_set_openpbr.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt05_material_lambert_diagnostic.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt05_material_set_lambert_diagnostic.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_local_light_evaluation_direct.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_reservoir_stream.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_direct_only_shared.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_shared.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_backend_rayquery.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_random_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_random_white_hash.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_path_event_contract.slang"
    "${UPT04_RANDOM_SLOT_LEDGER}"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03/upt03_reservoir_contract.slang")

set(UPT04_UNIFIED_BACKEND_MODULES
    ${UPT04_BACKEND_MODULES}
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_work_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_random.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_replay_random.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_light_selection.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_previous_best.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_reservoir_image.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_primary_image.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_nee_cache.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_direct.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_multibounce.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_local_light_evaluation.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_bindings.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_decode.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_emissive_replay.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_emissive_lookup.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_emissive_compact.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_resolved_emissive.slang")

set(UPT04_ALL_OUTPUTS)
foreach(material_set baseline deterministic)
    path_tracing_declare_slang_spirv_module(
        NAME "upt04_contract_${material_set}"
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_contract_${material_set}.slang"
        ENTRY main
        STAGE compute
        OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
        INCLUDE_DIRS
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_LIST_DIR}/.."
            "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
        DEPENDS ${UPT04_MODULES}
        OUT_VAR material_outputs)
    list(APPEND UPT04_ALL_OUTPUTS ${material_outputs})
endforeach()

set(UPT04_VERIFY_STAMP "${UPT04_OUTPUT_DIR}/upt04_module_graph_verified.txt")
add_custom_command(
    OUTPUT "${UPT04_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT04_BASELINE_REFLECTION=${UPT04_OUTPUT_DIR}/upt04_contract_baseline.slang.spirv-reflection.json
        -DUPT04_BASELINE_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_contract_baseline.slang.spvasm
        -DUPT04_TEST_REFLECTION=${UPT04_OUTPUT_DIR}/upt04_contract_deterministic.slang.spirv-reflection.json
        -DUPT04_TEST_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_contract_deterministic.slang.spvasm
        -DUPT04_STAMP=${UPT04_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt04ModuleGraph.cmake"
    DEPENDS ${UPT04_ALL_OUTPUTS} "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt04ModuleGraph.cmake"
    COMMENT "UPT-04 verifying replaceable material/random ownership graph"
    VERBATIM)

add_custom_target(PathTraceSlangUpt04ModuleGraph
    DEPENDS ${UPT04_ALL_OUTPUTS} "${UPT04_VERIFY_STAMP}")
set_target_properties(PathTraceSlangUpt04ModuleGraph PROPERTIES FOLDER "Shader Targets/UPT-04")

set(UPT04_BACKEND_OUTPUTS)
path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_rayquery
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
    OUT_VAR rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS ${rayquery_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_rayquery_compact
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=1
    OUT_VAR compact_rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS ${compact_rayquery_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_rayquery_compact32
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2
    OUT_VAR compact32_rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS ${compact32_rayquery_outputs})

# Native 112-byte geometry with the compact 64-byte light sidecar.  This is a
# distinct ABI from geometry48_light64: the vertex bindings remain the scene's
# native SmokeVertex records while only the current-light buffer is packed.
path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_rayquery_compact32_light64
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1
    OUT_VAR compact32_light64_rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS ${compact32_light64_rayquery_outputs})

# Production family-0 parity specialization for the currently profiled
# compact32 / 112-byte light path.  The generic artifact remains the parity-0
# route and the fallback for every other packing/continuation permutation.
path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_rayquery_compact32_unified_parity1
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
    DEFINES
        UPT04_PACKED_PRIMARY_RECEIVER=2
        UPT04_STATIC_ENABLED_FAMILY_MASK=3
        UPT04_STATIC_DIRECT_PROPOSAL_PARITY=1
    OUT_VAR compact32_unified_parity1_rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${compact32_unified_parity1_rayquery_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_rayquery_compact32_geometry48
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_GEOMETRY_VERTEX=1
    OUT_VAR compact32_geometry48_rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS ${compact32_geometry48_rayquery_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_primary_image_copy
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_primary_image_copy.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/upt04_primary_image_copy.slang"
    OUT_VAR primary_image_copy_outputs)
list(APPEND UPT04_BACKEND_OUTPUTS ${primary_image_copy_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_compact_geometry_pack
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_compact_geometry_pack.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_compact_geometry_pack.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_binary16.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_contract.slang"
    OUT_VAR compact_geometry_pack_outputs)
list(APPEND UPT04_BACKEND_OUTPUTS ${compact_geometry_pack_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_rayquery_compact32_geometry48_light64
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_GEOMETRY_VERTEX=1 UPT04_PACKED_LIGHT_RECORD=1
    OUT_VAR compact32_geometry48_light64_rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${compact32_geometry48_light64_rayquery_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_compact_light_pack
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_compact_light_pack.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_compact_light_pack.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_binary16.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_light_contract.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_contract.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_hit_contract.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_contract.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_live_contract.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_emission.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_resolved_emissive.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_random_white_hash.slang"
    OUT_VAR compact_light_pack_outputs)
list(APPEND UPT04_BACKEND_OUTPUTS ${compact_light_pack_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_rayquery_compact32_geometry48_light64_material48
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_GEOMETRY_VERTEX=1 UPT04_PACKED_LIGHT_RECORD=1 UPT04_PACKED_MATERIAL_RECORD=1
    OUT_VAR compact32_geometry48_light64_material48_rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${compact32_geometry48_light64_material48_rayquery_outputs})

# Experimental exact shared-reservoir split of unified D0. The direct pass
# publishes an unnormalized intermediate reservoir into page 0; the indirect
# pass loads, extends, and performs the sole final normalization.
path_tracing_declare_slang_spirv_module(
    NAME upt04_light_tile_presample
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_light_tile_presample.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_light_tile_presample.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_light_tile_contract.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_light_contract.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_random_white_hash.slang"
    OUT_VAR light_tile_presample_outputs)
list(APPEND UPT04_BACKEND_OUTPUTS ${light_tile_presample_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_direct_rayquery_compact32
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_direct_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_direct_only_rayquery.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_SPLIT_INITIAL_DIRECT=1 UPT04_NATIVE_LIGHT_UINT4_VIEW=1
    OUT_VAR split_direct_native_rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS ${split_direct_native_rayquery_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_NATIVE_LIGHT_UINT4_VIEW=1
    OUT_VAR split_indirect_native_rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS ${split_indirect_native_rayquery_outputs})

# Accepted fastest split: retain the scene's native 112-byte geometry while
# packing only current-light records to compact64.
path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_direct_rayquery_compact32_light64
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_direct_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_direct_only_rayquery.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_DIRECT=1
    OUT_VAR split_direct_light64_rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS ${split_direct_light64_rayquery_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_direct_rayquery_compact32_light64_resolved
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_direct_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_direct_only_rayquery.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_DIRECT=1 UPT04_REPLAY_FREE_SECONDARY_NEE=1
    OUT_VAR split_direct_light64_resolved_rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS ${split_direct_light64_resolved_rayquery_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_direct_rayquery_compact32_light64_lambertbsdf
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_direct_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_direct_only_rayquery.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_DIRECT=1 UPT05_LAMBERT_BSDF=1
    OUT_VAR split_direct_light64_lambertbsdf_rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS ${split_direct_light64_lambertbsdf_rayquery_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_direct_rayquery_compact32_light64_resolved_lambertbsdf
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_direct_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_direct_only_rayquery.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_DIRECT=1 UPT04_REPLAY_FREE_SECONDARY_NEE=1 UPT05_LAMBERT_BSDF=1
    OUT_VAR split_direct_light64_resolved_lambertbsdf_rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS ${split_direct_light64_resolved_lambertbsdf_rayquery_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1
    OUT_VAR split_indirect_light64_rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS ${split_indirect_light64_rayquery_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_emissive_classify
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_EMISSIVE_COMPACT_CLASSIFY=1
    OUT_VAR split_indirect_light64_emissive_classify_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_emissive_classify_outputs})


path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1
    OUT_VAR split_indirect_light64_x3_produce_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_x3_produce_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_emissive_classify_noskinned
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_EMISSIVE_COMPACT_CLASSIFY=1 UPT04_NO_SKINNED_GEOMETRY=1
    OUT_VAR split_indirect_light64_emissive_classify_noskinned_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_emissive_classify_noskinned_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_noskinned
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_NO_SKINNED_GEOMETRY=1
    OUT_VAR split_indirect_light64_x3_produce_noskinned_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_x3_produce_noskinned_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_MULTI_BOUNCE_INITIAL=1 UPT04_THREE_VERTEX_SPLIT_CONSUME=1
    OUT_VAR split_indirect_light64_x3_consume_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_x3_consume_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_nee1
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_SECONDARY_NEE_ONE=1
    OUT_VAR split_indirect_light64_x3_produce_nee1_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_x3_produce_nee1_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_nee1
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_MULTI_BOUNCE_INITIAL=1 UPT04_THREE_VERTEX_SPLIT_CONSUME=1 UPT04_SECONDARY_NEE_ONE=1
    OUT_VAR split_indirect_light64_x3_consume_nee1_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_x3_consume_nee1_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_noexactmis
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_EXACT_SECONDARY_MIS=0
    OUT_VAR split_indirect_light64_x3_produce_noexactmis_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_x3_produce_noexactmis_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_noexactmis
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_MULTI_BOUNCE_INITIAL=1 UPT04_THREE_VERTEX_SPLIT_CONSUME=1 UPT04_EXACT_SECONDARY_MIS=0
    OUT_VAR split_indirect_light64_x3_consume_noexactmis_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_x3_consume_noexactmis_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_nee1_noexactmis
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0
    OUT_VAR split_indirect_light64_x3_produce_nee1_noexactmis_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_x3_produce_nee1_noexactmis_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_nee1_noexactmis
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_MULTI_BOUNCE_INITIAL=1 UPT04_THREE_VERTEX_SPLIT_CONSUME=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0
    OUT_VAR split_indirect_light64_x3_consume_nee1_noexactmis_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_x3_consume_nee1_noexactmis_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_onepath.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_ONE_PATH_GEOMETRY=1
    OUT_VAR split_indirect_light64_onepath_x3_produce_nee1_noexactmis_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_onepath_x3_produce_nee1_noexactmis_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_body_ray
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_onepath.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_ONE_PATH_GEOMETRY=1 UPT04_PRODUCE_BODY_PROBE=1
    OUT_VAR split_indirect_light64_onepath_x3_produce_nee1_noexactmis_body_ray_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_onepath_x3_produce_nee1_noexactmis_body_ray_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_body_hit
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_onepath.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_ONE_PATH_GEOMETRY=1 UPT04_PRODUCE_BODY_PROBE=2
    OUT_VAR split_indirect_light64_onepath_x3_produce_nee1_noexactmis_body_hit_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_onepath_x3_produce_nee1_noexactmis_body_hit_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_body_nee
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_onepath.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_ONE_PATH_GEOMETRY=1 UPT04_PRODUCE_BODY_PROBE=3
    OUT_VAR split_indirect_light64_onepath_x3_produce_nee1_noexactmis_body_nee_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_onepath_x3_produce_nee1_noexactmis_body_nee_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_body_decode
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_onepath.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_ONE_PATH_GEOMETRY=1 UPT04_PRODUCE_BODY_PROBE=4
    OUT_VAR split_indirect_light64_onepath_x3_produce_nee1_noexactmis_body_decode_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_onepath_x3_produce_nee1_noexactmis_body_decode_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_body_shade
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_onepath.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_ONE_PATH_GEOMETRY=1 UPT04_PRODUCE_BODY_PROBE=5
    OUT_VAR split_indirect_light64_onepath_x3_produce_nee1_noexactmis_body_shade_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_onepath_x3_produce_nee1_noexactmis_body_shade_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_body_brdf
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_onepath.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_ONE_PATH_GEOMETRY=1 UPT04_PRODUCE_BODY_PROBE=6
    OUT_VAR split_indirect_light64_onepath_x3_produce_nee1_noexactmis_body_brdf_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_onepath_x3_produce_nee1_noexactmis_body_brdf_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_body_pick
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_onepath.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_ONE_PATH_GEOMETRY=1 UPT04_PRODUCE_BODY_PROBE=7
    OUT_VAR split_indirect_light64_onepath_x3_produce_nee1_noexactmis_body_pick_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_onepath_x3_produce_nee1_noexactmis_body_pick_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_body_eval
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_onepath.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_ONE_PATH_GEOMETRY=1 UPT04_PRODUCE_BODY_PROBE=8
    OUT_VAR split_indirect_light64_onepath_x3_produce_nee1_noexactmis_body_eval_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_onepath_x3_produce_nee1_noexactmis_body_eval_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_q1sib
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_onepath.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_q1_split.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_ONE_PATH_GEOMETRY=1 UPT04_Q1_SIBLING=1
    OUT_VAR split_indirect_light64_onepath_x3_produce_nee1_noexactmis_q1sib_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_onepath_x3_produce_nee1_noexactmis_q1sib_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_q1sib_consume
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_q1_sibling.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_q1_sibling.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_q1_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_onepath.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_multibounce.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_ONE_PATH_GEOMETRY=1 UPT04_Q1_SIBLING_CONSUME=1
    OUT_VAR split_indirect_light64_onepath_x3_q1sib_consume_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_onepath_x3_q1sib_consume_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_q1sib_consume_resolved
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_q1_sibling.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_q1_sibling.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_q1_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_onepath.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_multibounce.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_ONE_PATH_GEOMETRY=1 UPT04_Q1_SIBLING_CONSUME=1 UPT04_REPLAY_FREE_SECONDARY_NEE=1
    OUT_VAR split_indirect_light64_onepath_x3_q1sib_consume_resolved_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_onepath_x3_q1sib_consume_resolved_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_q1sib_lambertbsdf
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_onepath.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_q1_split.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_ONE_PATH_GEOMETRY=1 UPT04_Q1_SIBLING=1 UPT05_LAMBERT_BSDF=1
    OUT_VAR split_indirect_light64_onepath_x3_produce_nee1_noexactmis_q1sib_lambertbsdf_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_onepath_x3_produce_nee1_noexactmis_q1sib_lambertbsdf_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_q1sib_consume_lambertbsdf
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_q1_sibling.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_q1_sibling.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_q1_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_onepath.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_multibounce.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_ONE_PATH_GEOMETRY=1 UPT04_Q1_SIBLING_CONSUME=1 UPT05_LAMBERT_BSDF=1
    OUT_VAR split_indirect_light64_onepath_x3_q1sib_consume_lambertbsdf_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_onepath_x3_q1sib_consume_lambertbsdf_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_q1sib_consume_resolved_lambertbsdf
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_q1_sibling.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_q1_sibling.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_q1_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_onepath.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_multibounce.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_ONE_PATH_GEOMETRY=1 UPT04_Q1_SIBLING_CONSUME=1 UPT04_REPLAY_FREE_SECONDARY_NEE=1 UPT05_LAMBERT_BSDF=1
    OUT_VAR split_indirect_light64_onepath_x3_q1sib_consume_resolved_lambertbsdf_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_onepath_x3_q1sib_consume_resolved_lambertbsdf_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_consume_nee1_noexactmis_lambertbsdf
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_onepath.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_MULTI_BOUNCE_INITIAL=1 UPT04_THREE_VERTEX_SPLIT_CONSUME=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_ONE_PATH_GEOMETRY=1 UPT05_LAMBERT_BSDF=1
    OUT_VAR split_indirect_light64_onepath_x3_consume_nee1_noexactmis_lambertbsdf_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_onepath_x3_consume_nee1_noexactmis_lambertbsdf_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_consume_nee1_noexactmis_resolved_lambertbsdf
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_onepath.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_MULTI_BOUNCE_INITIAL=1 UPT04_THREE_VERTEX_SPLIT_CONSUME=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_ONE_PATH_GEOMETRY=1 UPT04_REPLAY_FREE_SECONDARY_NEE=1 UPT05_LAMBERT_BSDF=1
    OUT_VAR split_indirect_light64_onepath_x3_consume_nee1_noexactmis_resolved_lambertbsdf_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_onepath_x3_consume_nee1_noexactmis_resolved_lambertbsdf_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_consume_nee1_noexactmis
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_onepath.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_MULTI_BOUNCE_INITIAL=1 UPT04_THREE_VERTEX_SPLIT_CONSUME=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_ONE_PATH_GEOMETRY=1
    OUT_VAR split_indirect_light64_onepath_x3_consume_nee1_noexactmis_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_onepath_x3_consume_nee1_noexactmis_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_resolved
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_onepath.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_ONE_PATH_GEOMETRY=1 UPT04_REPLAY_FREE_SECONDARY_NEE=1
    OUT_VAR split_indirect_light64_onepath_x3_produce_nee1_noexactmis_resolved_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_onepath_x3_produce_nee1_noexactmis_resolved_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_consume_nee1_noexactmis_resolved
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_onepath.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_MULTI_BOUNCE_INITIAL=1 UPT04_THREE_VERTEX_SPLIT_CONSUME=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_ONE_PATH_GEOMETRY=1 UPT04_REPLAY_FREE_SECONDARY_NEE=1
    OUT_VAR split_indirect_light64_onepath_x3_consume_nee1_noexactmis_resolved_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_onepath_x3_consume_nee1_noexactmis_resolved_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_noskinned
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_onepath.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_ONE_PATH_GEOMETRY=1 UPT04_NO_SKINNED_GEOMETRY=1
    OUT_VAR split_indirect_light64_onepath_x3_produce_noskinned_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_onepath_x3_produce_noskinned_outputs})


path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_neecache
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_onepath.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_nee_cache.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_ONE_PATH_GEOMETRY=1 UPT04_NEE_CACHE=1
    OUT_VAR split_indirect_light64_onepath_x3_produce_nee1_noexactmis_neecache_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_onepath_x3_produce_nee1_noexactmis_neecache_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_consume_nee1_noexactmis_neecache
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_onepath.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_nee_cache.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_MULTI_BOUNCE_INITIAL=1 UPT04_THREE_VERTEX_SPLIT_CONSUME=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_ONE_PATH_GEOMETRY=1 UPT04_NEE_CACHE=1
    OUT_VAR split_indirect_light64_onepath_x3_consume_nee1_noexactmis_neecache_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_onepath_x3_consume_nee1_noexactmis_neecache_outputs})


path_tracing_declare_slang_spirv_module(
    NAME upt04_onepath_instance_pack
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_onepath_instance_pack.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_onepath_instance_pack.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_contract.slang"
    OUT_VAR onepath_instance_pack_outputs)
list(APPEND UPT04_BACKEND_OUTPUTS ${onepath_instance_pack_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_nee1_noexactmis_resolved
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_REPLAY_FREE_SECONDARY_NEE=1
    OUT_VAR split_indirect_light64_x3_produce_nee1_noexactmis_resolved_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_x3_produce_nee1_noexactmis_resolved_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_nee1_noexactmis_resolved
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_MULTI_BOUNCE_INITIAL=1 UPT04_THREE_VERTEX_SPLIT_CONSUME=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_REPLAY_FREE_SECONDARY_NEE=1
    OUT_VAR split_indirect_light64_x3_consume_nee1_noexactmis_resolved_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_x3_consume_nee1_noexactmis_resolved_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_material48
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_PACKED_MATERIAL_RECORD=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1
    OUT_VAR split_indirect_light64_x3_produce_material48_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_x3_produce_material48_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_material48
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_PACKED_MATERIAL_RECORD=1 UPT04_MULTI_BOUNCE_INITIAL=1 UPT04_THREE_VERTEX_SPLIT_CONSUME=1
    OUT_VAR split_indirect_light64_x3_consume_material48_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_x3_consume_material48_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_nee1_noexactmis_material48
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_PACKED_MATERIAL_RECORD=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0
    OUT_VAR split_indirect_light64_x3_produce_nee1_noexactmis_material48_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_x3_produce_nee1_noexactmis_material48_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_nee1_noexactmis_material48
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_PACKED_MATERIAL_RECORD=1 UPT04_MULTI_BOUNCE_INITIAL=1 UPT04_THREE_VERTEX_SPLIT_CONSUME=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0
    OUT_VAR split_indirect_light64_x3_consume_nee1_noexactmis_material48_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_x3_consume_nee1_noexactmis_material48_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_nee1_noexactmis_resolved_material48
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_PACKED_MATERIAL_RECORD=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_REPLAY_FREE_SECONDARY_NEE=1
    OUT_VAR split_indirect_light64_x3_produce_nee1_noexactmis_resolved_material48_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_x3_produce_nee1_noexactmis_resolved_material48_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_nee1_noexactmis_resolved_material48
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_PACKED_MATERIAL_RECORD=1 UPT04_MULTI_BOUNCE_INITIAL=1 UPT04_THREE_VERTEX_SPLIT_CONSUME=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0 UPT04_REPLAY_FREE_SECONDARY_NEE=1
    OUT_VAR split_indirect_light64_x3_consume_nee1_noexactmis_resolved_material48_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_x3_consume_nee1_noexactmis_resolved_material48_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_lambert
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt05_material_lambert_diagnostic.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt05_material_set_lambert_diagnostic.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT_DIAGNOSTIC_LAMBERT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1
    OUT_VAR split_indirect_light64_x3_produce_lambert_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_x3_produce_lambert_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_lambert
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt05_material_lambert_diagnostic.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt05_material_set_lambert_diagnostic.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT_DIAGNOSTIC_LAMBERT=1 UPT04_MULTI_BOUNCE_INITIAL=1 UPT04_THREE_VERTEX_SPLIT_CONSUME=1
    OUT_VAR split_indirect_light64_x3_consume_lambert_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_x3_consume_lambert_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_nee1_noexactmis_lambert
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt05_material_lambert_diagnostic.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt05_material_set_lambert_diagnostic.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT_DIAGNOSTIC_LAMBERT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0
    OUT_VAR split_indirect_light64_x3_produce_nee1_noexactmis_lambert_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_x3_produce_nee1_noexactmis_lambert_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_nee1_noexactmis_lambert
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt05_material_lambert_diagnostic.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt05_material_set_lambert_diagnostic.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT_DIAGNOSTIC_LAMBERT=1 UPT04_MULTI_BOUNCE_INITIAL=1 UPT04_THREE_VERTEX_SPLIT_CONSUME=1 UPT04_SECONDARY_NEE_ONE=1 UPT04_EXACT_SECONDARY_MIS=0
    OUT_VAR split_indirect_light64_x3_consume_nee1_noexactmis_lambert_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_x3_consume_nee1_noexactmis_lambert_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_debug_thin
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_split_indirect_debug_thin.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_split_indirect_debug_thin.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1
    OUT_VAR split_indirect_light64_x3_produce_debug_thin_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_x3_produce_debug_thin_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_debug_thin_ray
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_split_indirect_debug_thin.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_split_indirect_debug_thin.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_DEBUG_THIN_RAY=1
    OUT_VAR split_indirect_light64_x3_produce_debug_thin_ray_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_x3_produce_debug_thin_ray_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_geometry48_light64_x3_produce
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_GEOMETRY_VERTEX=1 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1
    OUT_VAR split_indirect_geometry48_light64_x3_produce_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_geometry48_light64_x3_produce_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_geometry48_light64_x3_produce_noskinned
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_GEOMETRY_VERTEX=1 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_THREE_VERTEX_SPLIT_PRODUCE=1 UPT04_NO_SKINNED_GEOMETRY=1
    OUT_VAR split_indirect_geometry48_light64_x3_produce_noskinned_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_geometry48_light64_x3_produce_noskinned_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_geometry48_light64_x3_consume
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_x3_split.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_GEOMETRY_VERTEX=1 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_MULTI_BOUNCE_INITIAL=1 UPT04_THREE_VERTEX_SPLIT_CONSUME=1
    OUT_VAR split_indirect_geometry48_light64_x3_consume_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_geometry48_light64_x3_consume_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_three_vertex
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_MULTI_BOUNCE_INITIAL=1
    OUT_VAR split_indirect_light64_three_vertex_rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_three_vertex_rayquery_outputs})

# Static closure-excision proof for probes 5/6. This is intentionally a
# separate artifact: setting emissiveCount=0 in the production shader leaves
# the emissive replay/material closure resident and cannot test footprint.
path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_three_vertex_analytic_only
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_MULTI_BOUNCE_INITIAL=1 UPT04_STATIC_ANALYTIC_INDIRECT_ONLY=1
    OUT_VAR split_indirect_light64_three_vertex_analytic_only_rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_three_vertex_analytic_only_rayquery_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_three_vertex_emissive_classify
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_MULTI_BOUNCE_INITIAL=1 UPT04_EMISSIVE_COMPACT_CLASSIFY=1
    OUT_VAR split_indirect_light64_three_vertex_emissive_classify_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_three_vertex_emissive_classify_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light64_three_vertex_emissive_consume
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_MULTI_BOUNCE_INITIAL=1 UPT04_EMISSIVE_COMPACT_CONSUME=1
    OUT_VAR split_indirect_light64_three_vertex_emissive_consume_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light64_three_vertex_emissive_consume_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_direct_rayquery_compact32_light_tiles
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_direct_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_direct_only_rayquery.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_SPLIT_INITIAL_DIRECT=1 UPT04_NATIVE_LIGHT_UINT4_VIEW=1 UPT04_LIGHT_TILES=1
    OUT_VAR split_direct_light_tiles_rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_direct_light_tiles_rayquery_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_light_tiles
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_SPLIT_INITIAL_INDIRECT=1 UPT04_NATIVE_LIGHT_UINT4_VIEW=1 UPT04_LIGHT_TILES=1
    OUT_VAR split_indirect_light_tiles_rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_light_tiles_rayquery_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_direct_rayquery_compact32_geometry48_light64
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_direct_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_direct_only_rayquery.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_GEOMETRY_VERTEX=1 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_DIRECT=1
    OUT_VAR split_direct_rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS ${split_direct_rayquery_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_geometry48_light64
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_GEOMETRY_VERTEX=1 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1
    OUT_VAR split_indirect_rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS ${split_indirect_rayquery_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_split_indirect_rayquery_compact32_geometry48_light64_material48
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect_only_rayquery.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_GEOMETRY_VERTEX=1 UPT04_PACKED_LIGHT_RECORD=1 UPT04_PACKED_MATERIAL_RECORD=1 UPT04_SPLIT_INITIAL_INDIRECT=1
    OUT_VAR split_indirect_material48_rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${split_indirect_material48_rayquery_outputs})

# Continuation-hit boundary experiment. C0 writes only the frozen 32-byte hit
# facts; D0 deterministically reconstructs path state and performs one final
# reservoir write after direct/endpoint/secondary streaming.
path_tracing_declare_slang_spirv_module(
    NAME upt04_continuation_trace_rayquery_compact32
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_continuation_trace_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_continuation_trace_rayquery.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_continuation_hit_bindings.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_CONTINUATION_HIT_WRITE=1
    OUT_VAR continuation_trace_rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS ${continuation_trace_rayquery_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_rayquery_compact32_geometry48_light64_continuation32
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_continuation_hit_bindings.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_GEOMETRY_VERTEX=1 UPT04_PACKED_LIGHT_RECORD=1 UPT04_SPLIT_CONTINUATION_SHADE=1
    OUT_VAR continuation32_shade_rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS ${continuation32_shade_rayquery_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_rayquery_compact32_geometry48_light64_material48_continuation32
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_continuation_hit_bindings.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_GEOMETRY_VERTEX=1 UPT04_PACKED_LIGHT_RECORD=1 UPT04_PACKED_MATERIAL_RECORD=1 UPT04_SPLIT_CONTINUATION_SHADE=1
    OUT_VAR continuation32_material48_shade_rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS
    ${continuation32_material48_shade_rayquery_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_rayquery_compact32_frozen_static
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_rayquery.slang"
    ENTRY main STAGE compute OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2
        UPT_FROZEN_STATIC_SCENE=1
    OUT_VAR frozen_static_rayquery_outputs RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS ${frozen_static_rayquery_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_rayquery_compact32_frozen_lights
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_rayquery.slang"
    ENTRY main STAGE compute OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT_FROZEN_FLAT_LIGHTS=1
    OUT_VAR frozen_lights_rayquery_outputs RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS ${frozen_lights_rayquery_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_rayquery_compact32_frozen_geometry
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_rayquery.slang"
    ENTRY main STAGE compute OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT_FROZEN_STATIC_GEOMETRY=1
    OUT_VAR frozen_geometry_rayquery_outputs RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS ${frozen_geometry_rayquery_outputs})

# Static Lambert-only profiling closure. These variants deliberately omit the
# OpenPBR implementation from the selected entry-point graph.
function(upt04_declare_lambert_variant variant_name source_file)
    path_tracing_declare_slang_spirv_module(
        NAME "${variant_name}"
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}"
        ENTRY main STAGE compute OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
        INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_LIST_DIR}/.."
            "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
        DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
            "${CMAKE_CURRENT_SOURCE_DIR}/upt05_material_lambert_diagnostic.slang"
            "${CMAKE_CURRENT_SOURCE_DIR}/upt05_material_set_lambert_diagnostic.slang"
            "${CMAKE_CURRENT_SOURCE_DIR}/upt04_continuation_hit_bindings.slang"
        DEFINES ${ARGN} UPT_DIAGNOSTIC_LAMBERT=1
        OUT_VAR lambert_outputs RAY_QUERY)
    set(UPT04_LAMBERT_OUTPUTS ${UPT04_LAMBERT_OUTPUTS} ${lambert_outputs}
        PARENT_SCOPE)
endfunction()

upt04_declare_lambert_variant(upt04_initial_rayquery_compact32_lambert
    upt04_initial_rayquery.slang UPT04_PACKED_PRIMARY_RECEIVER=2)
upt04_declare_lambert_variant(upt04_initial_rayquery_compact32_light64_lambert
    upt04_initial_rayquery.slang UPT04_PACKED_PRIMARY_RECEIVER=2
    UPT04_PACKED_LIGHT_RECORD=1)
upt04_declare_lambert_variant(
    upt04_initial_rayquery_compact32_frozen_static_lambert
    upt04_initial_rayquery.slang UPT04_PACKED_PRIMARY_RECEIVER=2
    UPT_FROZEN_STATIC_SCENE=1)
upt04_declare_lambert_variant(
    upt04_initial_rayquery_compact32_frozen_lights_lambert
    upt04_initial_rayquery.slang UPT04_PACKED_PRIMARY_RECEIVER=2
    UPT_FROZEN_FLAT_LIGHTS=1)
upt04_declare_lambert_variant(
    upt04_initial_rayquery_compact32_frozen_geometry_lambert
    upt04_initial_rayquery.slang UPT04_PACKED_PRIMARY_RECEIVER=2
    UPT_FROZEN_STATIC_GEOMETRY=1)
upt04_declare_lambert_variant(upt04_initial_rayquery_compact32_geometry48_lambert
    upt04_initial_rayquery.slang UPT04_PACKED_PRIMARY_RECEIVER=2
    UPT04_PACKED_GEOMETRY_VERTEX=1)
upt04_declare_lambert_variant(upt04_initial_rayquery_compact32_geometry48_light64_lambert
    upt04_initial_rayquery.slang UPT04_PACKED_PRIMARY_RECEIVER=2
    UPT04_PACKED_GEOMETRY_VERTEX=1 UPT04_PACKED_LIGHT_RECORD=1)
upt04_declare_lambert_variant(upt04_initial_rayquery_compact32_geometry48_light64_material48_lambert
    upt04_initial_rayquery.slang UPT04_PACKED_PRIMARY_RECEIVER=2
    UPT04_PACKED_GEOMETRY_VERTEX=1 UPT04_PACKED_LIGHT_RECORD=1
    UPT04_PACKED_MATERIAL_RECORD=1)
upt04_declare_lambert_variant(upt04_initial_rayquery_compact32_geometry48_light64_continuation32_lambert
    upt04_initial_rayquery.slang UPT04_PACKED_PRIMARY_RECEIVER=2
    UPT04_PACKED_GEOMETRY_VERTEX=1 UPT04_PACKED_LIGHT_RECORD=1
    UPT04_SPLIT_CONTINUATION_SHADE=1)
upt04_declare_lambert_variant(upt04_initial_rayquery_compact32_geometry48_light64_material48_continuation32_lambert
    upt04_initial_rayquery.slang UPT04_PACKED_PRIMARY_RECEIVER=2
    UPT04_PACKED_GEOMETRY_VERTEX=1 UPT04_PACKED_LIGHT_RECORD=1
    UPT04_PACKED_MATERIAL_RECORD=1 UPT04_SPLIT_CONTINUATION_SHADE=1)
upt04_declare_lambert_variant(upt04_continuation_trace_rayquery_compact32_lambert
    upt04_continuation_trace_rayquery.slang UPT04_PACKED_PRIMARY_RECEIVER=2
    UPT04_CONTINUATION_HIT_WRITE=1)
list(APPEND UPT04_BACKEND_OUTPUTS ${UPT04_LAMBERT_OUTPUTS})

path_tracing_declare_slang_spirv_module(
    NAME upt04_compact_material_pack
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_compact_material_pack.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_compact_material_pack.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_live_contract.slang"
    OUT_VAR compact_material_pack_outputs)
list(APPEND UPT04_BACKEND_OUTPUTS ${compact_material_pack_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_diagnostics
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_diagnostics.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_diagnostics.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_multibounce.slang"
    OUT_VAR diagnostic_outputs
    DEFINES UPT04_MULTI_BOUNCE_INITIAL=1
    RAY_QUERY)
list(APPEND UPT04_BACKEND_OUTPUTS ${diagnostic_outputs})

foreach(ray_stage raygen miss closest_hit)
    if(ray_stage STREQUAL "raygen")
        set(ray_source upt04_initial_raygen.slang)
        set(ray_stage_name raygeneration)
    elseif(ray_stage STREQUAL "miss")
        set(ray_source upt04_initial_raygen_miss.slang)
        set(ray_stage_name miss)
    else()
        set(ray_source upt04_initial_raygen_closest_hit.slang)
        set(ray_stage_name closesthit)
    endif()
    path_tracing_declare_slang_spirv_module(
        NAME "upt04_initial_${ray_stage}"
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/${ray_source}"
        ENTRY main
        STAGE "${ray_stage_name}"
        OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
        INCLUDE_DIRS
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_LIST_DIR}/.."
            "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
        DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        OUT_VAR ray_stage_outputs
        RAY_TRACING)
    list(APPEND UPT04_BACKEND_OUTPUTS ${ray_stage_outputs})
endforeach()

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_raygen_compact
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_raygen.slang"
    ENTRY main
    STAGE raygeneration
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=1
    OUT_VAR compact_raygen_outputs
    RAY_TRACING)
list(APPEND UPT04_BACKEND_OUTPUTS ${compact_raygen_outputs})

path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_raygen_compact32
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_raygen.slang"
    ENTRY main
    STAGE raygeneration
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2
    OUT_VAR compact32_raygen_outputs
    RAY_TRACING)
list(APPEND UPT04_BACKEND_OUTPUTS ${compact32_raygen_outputs})

set(UPT04_BACKEND_VERIFY_STAMP "${UPT04_OUTPUT_DIR}/upt04_backend_closure_verified.txt")
add_custom_command(
    OUTPUT "${UPT04_BACKEND_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT04_RAYQUERY_REFLECTION=${UPT04_OUTPUT_DIR}/upt04_initial_rayquery.slang.spirv-reflection.json
        -DUPT04_RAYQUERY_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_initial_rayquery.slang.spvasm
        -DUPT04_DIAGNOSTIC_REFLECTION=${UPT04_OUTPUT_DIR}/upt04_initial_diagnostics.slang.spirv-reflection.json
        -DUPT04_DIAGNOSTIC_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_initial_diagnostics.slang.spvasm
        -DUPT04_RAYGEN_REFLECTION=${UPT04_OUTPUT_DIR}/upt04_initial_raygen.slang.spirv-reflection.json
        -DUPT04_RAYGEN_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_initial_raygen.slang.spvasm
        -DUPT04_MISS_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_initial_miss.slang.spvasm
        -DUPT04_CLOSEST_HIT_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_initial_closest_hit.slang.spvasm
        -DUPT04_COMPACT_PRODUCTION_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_initial_rayquery_compact32_geometry48_light64.slang.spvasm
        -DUPT04_COMPACT_GEOMETRY_PACK_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_compact_geometry_pack.slang.spvasm
        -DUPT04_COMPACT_LIGHT_PACK_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_compact_light_pack.slang.spvasm
        -DUPT04_STAMP=${UPT04_BACKEND_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt04BackendClosure.cmake"
    DEPENDS ${UPT04_BACKEND_OUTPUTS}
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt04BackendClosure.cmake"
    COMMENT "UPT-04 verifying RayQuery/raygen bounded-transport closure"
    VERBATIM)

add_custom_target(PathTraceSlangUpt04BackendClosure
    DEPENDS ${UPT04_BACKEND_OUTPUTS} "${UPT04_BACKEND_VERIFY_STAMP}")
set_target_properties(PathTraceSlangUpt04BackendClosure PROPERTIES FOLDER "Shader Targets/UPT-04")
add_dependencies(PathTraceSlangUpt04ModuleGraph PathTraceSlangUpt04BackendClosure)

set(UPT04_GEOMETRY_MODULES
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_binary16.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_bindings.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_control.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_primary_surface_live_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_hit_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_light_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_live_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_provider_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_set_live_probe.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_primary_live.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt05_material_openpbr.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt05_material_set_openpbr.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_path_event_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_bindings.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_decode.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_emissive_replay.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_emissive_lookup.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03/upt03_reservoir_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_probe.slang")
path_tracing_declare_slang_spirv_module(
    NAME upt04_geometry_probe
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_probe.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_GEOMETRY_MODULES}
    OUT_VAR geometry_outputs)

set(UPT04_GEOMETRY_VERIFY_STAMP "${UPT04_OUTPUT_DIR}/upt04_geometry_closure_verified.txt")
add_custom_command(
    OUTPUT "${UPT04_GEOMETRY_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT04_GEOMETRY_REFLECTION=${UPT04_OUTPUT_DIR}/upt04_geometry_probe.slang.spirv-reflection.json
        -DUPT04_GEOMETRY_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_geometry_probe.slang.spvasm
        -DUPT04_STAMP=${UPT04_GEOMETRY_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt04GeometryClosure.cmake"
    DEPENDS ${geometry_outputs}
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt04GeometryClosure.cmake"
    COMMENT "UPT-04 verifying bounded O(1) geometry/replay closure"
    VERBATIM)

add_custom_target(PathTraceSlangUpt04GeometryClosure
    DEPENDS ${geometry_outputs} "${UPT04_GEOMETRY_VERIFY_STAMP}")
set_target_properties(PathTraceSlangUpt04GeometryClosure PROPERTIES FOLDER "Shader Targets/UPT-04")
add_dependencies(PathTraceSlangUpt04ModuleGraph PathTraceSlangUpt04GeometryClosure)

set(UPT04_STATIC_SPECIALIZATION_OUTPUTS)
foreach(family direct indirect)
    set(static_native_light_view_define)
    if(family STREQUAL "direct")
        set(static_native_light_view_define UPT04_NATIVE_LIGHT_UINT4_VIEW=1)
    endif()
    path_tracing_declare_slang_spirv_module(
        NAME "upt04_initial_${family}_only_rayquery"
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_${family}_only_rayquery.slang"
        ENTRY main
        STAGE compute
        OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
        INCLUDE_DIRS
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_LIST_DIR}/.."
            "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
        DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
            "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_${family}_only_rayquery.slang"
        OUT_VAR static_rayquery_outputs
        RAY_QUERY)
    list(APPEND UPT04_STATIC_SPECIALIZATION_OUTPUTS ${static_rayquery_outputs})

    path_tracing_declare_slang_spirv_module(
        NAME "upt04_initial_${family}_only_rayquery_compact"
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_${family}_only_rayquery.slang"
        ENTRY main
        STAGE compute
        OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
        INCLUDE_DIRS
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_LIST_DIR}/.."
            "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
        DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
            "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_${family}_only_rayquery.slang"
        DEFINES UPT04_PACKED_PRIMARY_RECEIVER=1
        OUT_VAR static_compact_rayquery_outputs
        RAY_QUERY)
    list(APPEND UPT04_STATIC_SPECIALIZATION_OUTPUTS
        ${static_compact_rayquery_outputs})

    path_tracing_declare_slang_spirv_module(
        NAME "upt04_initial_${family}_only_rayquery_compact32"
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_${family}_only_rayquery.slang"
        ENTRY main
        STAGE compute
        OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
        INCLUDE_DIRS
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_LIST_DIR}/.."
            "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
        DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
            "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_${family}_only_rayquery.slang"
        DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2
            ${static_native_light_view_define}
        OUT_VAR static_compact32_rayquery_outputs
        RAY_QUERY)
    list(APPEND UPT04_STATIC_SPECIALIZATION_OUTPUTS
        ${static_compact32_rayquery_outputs})

    path_tracing_declare_slang_spirv_module(
        NAME "upt04_initial_${family}_only_rayquery_compact32_geometry48"
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_${family}_only_rayquery.slang"
        ENTRY main
        STAGE compute
        OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
        INCLUDE_DIRS
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_LIST_DIR}/.."
            "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
        DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
            "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_${family}_only_rayquery.slang"
        DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_GEOMETRY_VERTEX=1
        OUT_VAR static_compact32_geometry48_rayquery_outputs
        RAY_QUERY)
    list(APPEND UPT04_STATIC_SPECIALIZATION_OUTPUTS
        ${static_compact32_geometry48_rayquery_outputs})

    path_tracing_declare_slang_spirv_module(
        NAME "upt04_initial_${family}_only_rayquery_compact32_geometry48_light64"
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_${family}_only_rayquery.slang"
        ENTRY main
        STAGE compute
        OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
        INCLUDE_DIRS
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_LIST_DIR}/.."
            "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
        DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
            "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_${family}_only_rayquery.slang"
        DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_GEOMETRY_VERTEX=1 UPT04_PACKED_LIGHT_RECORD=1
        OUT_VAR static_compact32_geometry48_light64_rayquery_outputs
        RAY_QUERY)
    list(APPEND UPT04_STATIC_SPECIALIZATION_OUTPUTS
        ${static_compact32_geometry48_light64_rayquery_outputs})

    path_tracing_declare_slang_spirv_module(
        NAME "upt04_initial_${family}_only_rayquery_compact32_geometry48_light64_material48"
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_${family}_only_rayquery.slang"
        ENTRY main
        STAGE compute
        OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
        INCLUDE_DIRS
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_LIST_DIR}/.."
            "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
        DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
            "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_${family}_only_rayquery.slang"
        DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2 UPT04_PACKED_GEOMETRY_VERTEX=1 UPT04_PACKED_LIGHT_RECORD=1 UPT04_PACKED_MATERIAL_RECORD=1
        OUT_VAR static_compact32_geometry48_light64_material48_rayquery_outputs
        RAY_QUERY)
    list(APPEND UPT04_STATIC_SPECIALIZATION_OUTPUTS
        ${static_compact32_geometry48_light64_material48_rayquery_outputs})

    path_tracing_declare_slang_spirv_module(
        NAME "upt04_initial_${family}_only_raygen"
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_${family}_only_raygen.slang"
        ENTRY main
        STAGE raygeneration
        OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
        INCLUDE_DIRS
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_LIST_DIR}/.."
            "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
        DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
            "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_${family}_only_raygen.slang"
        OUT_VAR static_raygen_outputs
        RAY_TRACING)
    list(APPEND UPT04_STATIC_SPECIALIZATION_OUTPUTS ${static_raygen_outputs})

    path_tracing_declare_slang_spirv_module(
        NAME "upt04_initial_${family}_only_raygen_compact"
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_${family}_only_raygen.slang"
        ENTRY main
        STAGE raygeneration
        OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
        INCLUDE_DIRS
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_LIST_DIR}/.."
            "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
        DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
            "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_${family}_only_raygen.slang"
        DEFINES UPT04_PACKED_PRIMARY_RECEIVER=1
        OUT_VAR static_compact_raygen_outputs
        RAY_TRACING)
    list(APPEND UPT04_STATIC_SPECIALIZATION_OUTPUTS
        ${static_compact_raygen_outputs})

    path_tracing_declare_slang_spirv_module(
        NAME "upt04_initial_${family}_only_raygen_compact32"
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_${family}_only_raygen.slang"
        ENTRY main
        STAGE raygeneration
        OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
        INCLUDE_DIRS
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_LIST_DIR}/.."
            "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
        DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
            "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_${family}_only_raygen.slang"
        DEFINES UPT04_PACKED_PRIMARY_RECEIVER=2
        OUT_VAR static_compact32_raygen_outputs
        RAY_TRACING)
    list(APPEND UPT04_STATIC_SPECIALIZATION_OUTPUTS
        ${static_compact32_raygen_outputs})
endforeach()

# UPT-47 glass-reflection child.  Keep the biased positive-only estimator in
# its own artifact so the main direct/indirect reservoir contract remains the
# accepted fixed-schedule estimator.
path_tracing_declare_slang_spirv_module(
    NAME upt04_initial_reflection_direct_rayquery
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_direct_only_rayquery.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_UNIFIED_BACKEND_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_direct_only_rayquery.slang"
    DEFINES UPT04_PACKED_PRIMARY_RECEIVER=1
        UPT47_REFLECTION_POSITIVE_ONLY=1
    OUT_VAR reflection_direct_rayquery_outputs
    RAY_QUERY)
list(APPEND UPT04_STATIC_SPECIALIZATION_OUTPUTS
    ${reflection_direct_rayquery_outputs})

set(UPT04_STATIC_VERIFY_STAMP "${UPT04_OUTPUT_DIR}/upt04_static_specializations_verified.txt")
add_custom_command(
    OUTPUT "${UPT04_STATIC_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT04_DIRECT_RQ_REFLECTION=${UPT04_OUTPUT_DIR}/upt04_initial_direct_only_rayquery.slang.spirv-reflection.json
        -DUPT04_DIRECT_RQ_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_initial_direct_only_rayquery.slang.spvasm
        -DUPT04_DIRECT_RQ_SPV=${UPT04_OUTPUT_DIR}/upt04_initial_direct_only_rayquery.slang.spv
        -DUPT04_DIRECT_RG_REFLECTION=${UPT04_OUTPUT_DIR}/upt04_initial_direct_only_raygen.slang.spirv-reflection.json
        -DUPT04_DIRECT_RG_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_initial_direct_only_raygen.slang.spvasm
        -DUPT04_DIRECT_RG_SPV=${UPT04_OUTPUT_DIR}/upt04_initial_direct_only_raygen.slang.spv
        -DUPT04_INDIRECT_RQ_REFLECTION=${UPT04_OUTPUT_DIR}/upt04_initial_indirect_only_rayquery.slang.spirv-reflection.json
        -DUPT04_INDIRECT_RQ_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_initial_indirect_only_rayquery.slang.spvasm
        -DUPT04_INDIRECT_RQ_SPV=${UPT04_OUTPUT_DIR}/upt04_initial_indirect_only_rayquery.slang.spv
        -DUPT04_INDIRECT_RG_REFLECTION=${UPT04_OUTPUT_DIR}/upt04_initial_indirect_only_raygen.slang.spirv-reflection.json
        -DUPT04_INDIRECT_RG_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_initial_indirect_only_raygen.slang.spvasm
        -DUPT04_INDIRECT_RG_SPV=${UPT04_OUTPUT_DIR}/upt04_initial_indirect_only_raygen.slang.spv
        -DUPT04_UNIFIED_RQ_SPV=${UPT04_OUTPUT_DIR}/upt04_initial_rayquery.slang.spv
        -DUPT04_UNIFIED_RG_SPV=${UPT04_OUTPUT_DIR}/upt04_initial_raygen.slang.spv
        -DUPT04_INDIRECT_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect.slang
        -DUPT04_STAMP=${UPT04_STATIC_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt04StaticSpecializations.cmake"
    DEPENDS ${UPT04_STATIC_SPECIALIZATION_OUTPUTS} ${UPT04_BACKEND_OUTPUTS}
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt04StaticSpecializations.cmake"
    COMMENT "UPT-04 verifying static direct/indirect family closures"
    VERBATIM)

add_custom_target(PathTraceSlangUpt04StaticSpecializations
    DEPENDS ${UPT04_STATIC_SPECIALIZATION_OUTPUTS} "${UPT04_STATIC_VERIFY_STAMP}")
set_target_properties(PathTraceSlangUpt04StaticSpecializations PROPERTIES FOLDER "Shader Targets/UPT-04")
add_dependencies(PathTraceSlangUpt04ModuleGraph PathTraceSlangUpt04StaticSpecializations)

set(UPT05_MATERIAL_BOUNDARY_STAMP
    "${UPT04_OUTPUT_DIR}/upt05_material_boundary_verified.txt")
add_custom_command(
    OUTPUT "${UPT05_MATERIAL_BOUNDARY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT05_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DUPT05_STAMP=${UPT05_MATERIAL_BOUNDARY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt05MaterialBoundary.cmake"
    DEPENDS ${UPT04_BACKEND_OUTPUTS} ${UPT04_STATIC_SPECIALIZATION_OUTPUTS}
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt05MaterialBoundary.cmake"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_provider_contract.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt05_material_openpbr.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt05_material_set_openpbr.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_direct.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_shared.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_direct_only_shared.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_local_light_evaluation.slang"
    COMMENT "UPT-05 verifying generic material boundary and OpenPBR composition"
    VERBATIM)
add_custom_target(PathTraceSlangUpt05MaterialBoundary
    DEPENDS "${UPT05_MATERIAL_BOUNDARY_STAMP}")
set_target_properties(PathTraceSlangUpt05MaterialBoundary
    PROPERTIES FOLDER "Shader Targets/UPT-05")
add_dependencies(PathTraceSlangUpt04ModuleGraph
    PathTraceSlangUpt05MaterialBoundary)

set(UPT05_REPEATABILITY_STAMP
    "${UPT04_OUTPUT_DIR}/upt05_repeatability_verified.txt")
add_custom_command(
    OUTPUT "${UPT05_REPEATABILITY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT05_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DUPT05_HOST_DIR=${CMAKE_SOURCE_DIR}/renderer/NVRHI
        -DUPT05_DIAGNOSTIC_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_initial_diagnostics.slang.spvasm
        -DUPT05_STAMP=${UPT05_REPEATABILITY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt05Repeatability.cmake"
    DEPENDS ${UPT04_BACKEND_OUTPUTS}
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt05Repeatability.cmake"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_diagnostics.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_reservoir_stream.slang"
        "${CMAKE_SOURCE_DIR}/renderer/NVRHI/PathTraceCVars.cpp"
        "${CMAKE_SOURCE_DIR}/renderer/NVRHI/PathTraceSmokeDispatch.cpp"
        "${CMAKE_SOURCE_DIR}/renderer/NVRHI/PathTraceUnifiedPt.cpp"
    COMMENT "UPT-05 verifying fixed-input reservoir repeatability signature"
    VERBATIM)
add_custom_target(PathTraceSlangUpt05Repeatability
    DEPENDS "${UPT05_REPEATABILITY_STAMP}")
set_target_properties(PathTraceSlangUpt05Repeatability
    PROPERTIES FOLDER "Shader Targets/UPT-05")
add_dependencies(PathTraceSlangUpt04ModuleGraph
    PathTraceSlangUpt05Repeatability)

# Minimal live-game TLAS compiler A/B. These modules intentionally share only
# a TLAS and one uint UAV; they do not import any UPT estimator modules.
path_tracing_declare_slang_spirv_module(
    NAME upt04_live_tlas_probe_slang
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_live_tlas_probe.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    OUT_VAR live_tlas_probe_slang_outputs
    RAY_QUERY)
path_tracing_declare_dxc_spirv_comparison_module(
    NAME upt04_live_tlas_probe_dxc
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_live_tlas_probe.hlsl"
    ENTRY main
    PROFILE cs_6_6
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    OUT_VAR live_tlas_probe_dxc_outputs
    RAY_QUERY)
path_tracing_declare_slang_spirv_module(
    NAME upt04_live_tlas_full_layout_probe
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_live_tlas_full_layout_probe.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS ${UPT04_GEOMETRY_MODULES}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_live_tlas_full_layout_probe.slang"
    OUT_VAR live_tlas_full_layout_probe_outputs
    RAY_QUERY)
path_tracing_declare_slang_spirv_module(
    NAME upt04_live_tlas_full_host_probe
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_live_tlas_full_host_probe.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/upt04_live_tlas_full_host_probe.slang"
    OUT_VAR live_tlas_full_host_probe_outputs
    RAY_QUERY)
path_tracing_declare_slang_spirv_module(
    NAME upt04_full_frame_traversal_probe
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt04_full_frame_traversal_probe.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT04_OUTPUT_DIR}"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_full_frame_traversal_probe.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_control.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_primary_surface_live_contract.slang"
    OUT_VAR full_frame_traversal_probe_outputs
    RAY_QUERY)
set(UPT04_LIVE_TLAS_PROBE_VERIFY_STAMP
    "${UPT04_OUTPUT_DIR}/upt04_live_tlas_probe_verified.txt")
add_custom_command(
    OUTPUT "${UPT04_LIVE_TLAS_PROBE_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT04_SLANG_PROBE_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_live_tlas_probe_slang.slang.spvasm
        -DUPT04_DXC_PROBE_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_live_tlas_probe_dxc.dxc.spvasm
        -DUPT04_FULL_LAYOUT_PROBE_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_live_tlas_full_layout_probe.slang.spvasm
        -DUPT04_FULL_HOST_PROBE_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_live_tlas_full_host_probe.slang.spvasm
        -DUPT04_TRAVERSAL_PROBE_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_full_frame_traversal_probe.slang.spvasm
        -DUPT04_PROBE_STAMP=${UPT04_LIVE_TLAS_PROBE_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt04LiveTlasProbe.cmake"
    DEPENDS ${live_tlas_probe_slang_outputs} ${live_tlas_probe_dxc_outputs}
        ${live_tlas_full_layout_probe_outputs}
        ${live_tlas_full_host_probe_outputs}
        ${full_frame_traversal_probe_outputs}
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt04LiveTlasProbe.cmake"
    COMMENT "UPT-04 verifying compact live-TLAS Slang/DXC probes"
    VERBATIM)

# Runtime admission is explicit: copy only the legacy and compact selectable initial modules
# and the two shared raygen hit modules after every offline verifier passes.
# Reflection/disassembly artifacts remain build-only evidence.
set(UPT04_RUNTIME_OUTPUT_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../base/renderprogs2/spirv/builtin/pathtracing/slang_upt04")
set(UPT35_VERIFY_STAMP
    "${UPT04_OUTPUT_DIR}/upt35_three_vertex_verified.txt")
add_custom_command(
    OUTPUT "${UPT35_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT35_REFLECTION=${UPT04_OUTPUT_DIR}/upt04_initial_split_indirect_rayquery_compact32_light64_three_vertex.slang.spirv-reflection.json
        -DUPT35_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_initial_split_indirect_rayquery_compact32_light64_three_vertex.slang.spvasm
        -DUPT35_SPV=${UPT04_OUTPUT_DIR}/upt04_initial_split_indirect_rayquery_compact32_light64_three_vertex.slang.spv
        -DUPT35_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_multibounce.slang
        -DUPT35_STAMP=${UPT35_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt35ThreeVertex.cmake"
    DEPENDS ${split_indirect_light64_three_vertex_rayquery_outputs}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_multibounce.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt35ThreeVertex.cmake"
    COMMENT "UPT-35 verifying bounded three-vertex initial transport"
    VERBATIM)
add_custom_target(PathTraceSlangUpt35ThreeVertex
    DEPENDS "${UPT35_VERIFY_STAMP}")
set_target_properties(PathTraceSlangUpt35ThreeVertex
    PROPERTIES FOLDER "Shader Targets/UPT-04")
add_dependencies(PathTraceSlangUpt04ModuleGraph
    PathTraceSlangUpt35ThreeVertex)
set(UPT56_VERIFY_STAMP
    "${UPT04_OUTPUT_DIR}/upt56_emissive_closure_verified.txt")
add_custom_command(
    OUTPUT "${UPT56_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT56_PRODUCTION_REFLECTION=${UPT04_OUTPUT_DIR}/upt04_initial_split_indirect_rayquery_compact32_light64_three_vertex.slang.spirv-reflection.json
        -DUPT56_PRODUCTION_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_initial_split_indirect_rayquery_compact32_light64_three_vertex.slang.spvasm
        -DUPT56_CLASSIFY_REFLECTION=${UPT04_OUTPUT_DIR}/upt04_initial_split_indirect_rayquery_compact32_light64_three_vertex_emissive_classify.slang.spirv-reflection.json
        -DUPT56_CLASSIFY_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_initial_split_indirect_rayquery_compact32_light64_three_vertex_emissive_classify.slang.spvasm
        -DUPT56_CONSUME_REFLECTION=${UPT04_OUTPUT_DIR}/upt04_initial_split_indirect_rayquery_compact32_light64_three_vertex_emissive_consume.slang.spirv-reflection.json
        -DUPT56_CONSUME_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_initial_split_indirect_rayquery_compact32_light64_three_vertex_emissive_consume.slang.spvasm
        -DUPT56_W_CLASSIFY_REFLECTION=${UPT04_OUTPUT_DIR}/upt04_initial_split_indirect_rayquery_compact32_light64_emissive_classify.slang.spirv-reflection.json
        -DUPT56_W_CLASSIFY_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_initial_split_indirect_rayquery_compact32_light64_emissive_classify.slang.spvasm
        -DUPT56_WG_CLASSIFY_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_initial_split_indirect_rayquery_compact32_light64_emissive_classify_noskinned.slang.spvasm
        -DUPT56_XG_PRODUCE_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_noskinned.slang.spvasm
        -DUPT56_STAMP=${UPT56_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt56EmissiveClosure.cmake"
    DEPENDS ${split_indirect_light64_three_vertex_rayquery_outputs}
        ${split_indirect_light64_three_vertex_emissive_classify_outputs}
        ${split_indirect_light64_three_vertex_emissive_consume_outputs}
        ${split_indirect_light64_emissive_classify_outputs}
        ${split_indirect_light64_emissive_classify_noskinned_outputs}
        ${split_indirect_light64_x3_produce_noskinned_outputs}
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt56EmissiveClosure.cmake"
    COMMENT "UPT-56 verifying emissive-compact classify/consume closure split"
    VERBATIM)
add_custom_target(PathTraceSlangUpt56EmissiveClosure
    DEPENDS "${UPT56_VERIFY_STAMP}")
set_target_properties(PathTraceSlangUpt56EmissiveClosure
    PROPERTIES FOLDER "Shader Targets/UPT-04")
add_dependencies(PathTraceSlangUpt04ModuleGraph
    PathTraceSlangUpt56EmissiveClosure)
set(UPT04_LIGHT_TILE_VERIFY_STAMP
    "${UPT04_OUTPUT_DIR}/upt04_light_tiles_verified.txt")
add_custom_command(
    OUTPUT "${UPT04_LIGHT_TILE_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT04_TILE_PRESAMPLE_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_light_tile_presample.slang.spvasm
        -DUPT04_TILE_DIRECT_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_initial_split_direct_rayquery_compact32_light_tiles.slang.spvasm
        -DUPT04_TILE_INDIRECT_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_initial_split_indirect_rayquery_compact32_light_tiles.slang.spvasm
        -DUPT04_TILE_STAMP=${UPT04_LIGHT_TILE_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt04LightTiles.cmake"
    DEPENDS ${light_tile_presample_outputs}
        ${split_direct_light_tiles_rayquery_outputs}
        ${split_indirect_light_tiles_rayquery_outputs}
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt04LightTiles.cmake"
    COMMENT "UPT-11 verifying light-tile bindings and split ray ceiling"
    VERBATIM)
add_custom_target(PathTraceSlangUpt04LightTiles
    DEPENDS "${UPT04_LIGHT_TILE_VERIFY_STAMP}")
set_target_properties(PathTraceSlangUpt04LightTiles
    PROPERTIES FOLDER "Shader Targets/UPT-04")
add_dependencies(PathTraceSlangUpt04ModuleGraph PathTraceSlangUpt04LightTiles)

set(UPT04_NATIVE_LIGHT_VIEW_VERIFY_STAMP
    "${UPT04_OUTPUT_DIR}/upt04_native_light_view_verified.txt")
add_custom_command(
    OUTPUT "${UPT04_NATIVE_LIGHT_VIEW_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT04_NATIVE_DIRECT_SPLIT_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_initial_split_direct_rayquery_compact32.slang.spvasm
        -DUPT04_NATIVE_DIRECT_ONLY_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_initial_direct_only_rayquery_compact32.slang.spvasm
        -DUPT04_NATIVE_INDIRECT_SPLIT_DISASSEMBLY=${UPT04_OUTPUT_DIR}/upt04_initial_split_indirect_rayquery_compact32.slang.spvasm
        -DUPT04_NATIVE_LIGHT_VIEW_STAMP=${UPT04_NATIVE_LIGHT_VIEW_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt04NativeLightView.cmake"
    DEPENDS ${split_direct_native_rayquery_outputs}
        ${split_indirect_native_rayquery_outputs}
        ${UPT04_STATIC_SPECIALIZATION_OUTPUTS}
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt04NativeLightView.cmake"
    COMMENT "UPT-12 verifying native current-light uint4 views"
    VERBATIM)
add_custom_target(PathTraceSlangUpt04NativeLightView
    DEPENDS "${UPT04_NATIVE_LIGHT_VIEW_VERIFY_STAMP}")
set_target_properties(PathTraceSlangUpt04NativeLightView
    PROPERTIES FOLDER "Shader Targets/UPT-04")
add_dependencies(PathTraceSlangUpt04ModuleGraph
    PathTraceSlangUpt04NativeLightView)

set(UPT04_RUNTIME_MODULES
    upt04_initial_rayquery
    upt04_initial_rayquery_compact
    upt04_initial_rayquery_compact32
    upt04_initial_rayquery_compact32_light64
    upt04_initial_rayquery_compact32_unified_parity1
    upt04_initial_rayquery_compact32_geometry48
    upt04_primary_image_copy
    upt04_compact_geometry_pack
    upt04_initial_rayquery_compact32_geometry48_light64
    upt04_compact_light_pack
    upt04_initial_rayquery_compact32_geometry48_light64_material48
    upt04_light_tile_presample
    upt04_initial_split_direct_rayquery_compact32
    upt04_initial_split_indirect_rayquery_compact32
    upt04_initial_split_direct_rayquery_compact32_light64
    upt04_initial_split_direct_rayquery_compact32_light64_resolved
    upt04_initial_split_direct_rayquery_compact32_light64_lambertbsdf
    upt04_initial_split_direct_rayquery_compact32_light64_resolved_lambertbsdf
    upt04_initial_split_indirect_rayquery_compact32_light64
    upt04_initial_split_indirect_rayquery_compact32_light64_emissive_classify
    upt04_initial_split_indirect_rayquery_compact32_light64_emissive_classify_noskinned
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_noskinned
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_nee1
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_nee1
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_noexactmis
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_noexactmis
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_nee1_noexactmis
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_nee1_noexactmis
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_body_ray
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_body_hit
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_body_nee
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_body_decode
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_body_shade
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_body_brdf
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_body_pick
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_body_eval
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_q1sib
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_q1sib_consume
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_q1sib_consume_resolved
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_q1sib_lambertbsdf
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_q1sib_consume_lambertbsdf
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_q1sib_consume_resolved_lambertbsdf
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_consume_nee1_noexactmis_lambertbsdf
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_consume_nee1_noexactmis_resolved_lambertbsdf
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_consume_nee1_noexactmis
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_resolved
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_consume_nee1_noexactmis_resolved
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_neecache
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_consume_nee1_noexactmis_neecache
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_noskinned
    upt04_onepath_instance_pack
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_nee1_noexactmis_resolved
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_nee1_noexactmis_resolved
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_material48
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_material48
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_nee1_noexactmis_material48
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_nee1_noexactmis_material48
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_nee1_noexactmis_resolved_material48
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_nee1_noexactmis_resolved_material48
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_lambert
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_lambert
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_nee1_noexactmis_lambert
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_nee1_noexactmis_lambert
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_debug_thin
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_debug_thin_ray
    upt04_initial_split_indirect_rayquery_compact32_geometry48_light64_x3_produce
    upt04_initial_split_indirect_rayquery_compact32_geometry48_light64_x3_produce_noskinned
    upt04_initial_split_indirect_rayquery_compact32_geometry48_light64_x3_consume
    upt04_initial_split_indirect_rayquery_compact32_light64_three_vertex
    upt04_initial_split_indirect_rayquery_compact32_light64_three_vertex_analytic_only
    upt04_initial_split_indirect_rayquery_compact32_light64_three_vertex_emissive_classify
    upt04_initial_split_indirect_rayquery_compact32_light64_three_vertex_emissive_consume
    upt04_initial_split_direct_rayquery_compact32_light_tiles
    upt04_initial_split_indirect_rayquery_compact32_light_tiles
    upt04_initial_split_direct_rayquery_compact32_geometry48_light64
    upt04_initial_split_indirect_rayquery_compact32_geometry48_light64
    upt04_initial_split_indirect_rayquery_compact32_geometry48_light64_material48
    upt04_continuation_trace_rayquery_compact32
    upt04_initial_rayquery_compact32_geometry48_light64_continuation32
    upt04_initial_rayquery_compact32_geometry48_light64_material48_continuation32
    upt04_initial_rayquery_compact32_lambert
    upt04_initial_rayquery_compact32_light64_lambert
    upt04_initial_rayquery_compact32_frozen_static
    upt04_initial_rayquery_compact32_frozen_static_lambert
    upt04_initial_rayquery_compact32_frozen_lights
    upt04_initial_rayquery_compact32_frozen_lights_lambert
    upt04_initial_rayquery_compact32_frozen_geometry
    upt04_initial_rayquery_compact32_frozen_geometry_lambert
    upt04_initial_rayquery_compact32_geometry48_lambert
    upt04_initial_rayquery_compact32_geometry48_light64_lambert
    upt04_initial_rayquery_compact32_geometry48_light64_material48_lambert
    upt04_continuation_trace_rayquery_compact32_lambert
    upt04_initial_rayquery_compact32_geometry48_light64_continuation32_lambert
    upt04_initial_rayquery_compact32_geometry48_light64_material48_continuation32_lambert
    upt04_compact_material_pack
    upt04_initial_diagnostics
    upt04_initial_direct_only_rayquery
    upt04_initial_direct_only_rayquery_compact
    upt04_initial_reflection_direct_rayquery
    upt04_initial_direct_only_rayquery_compact32
    upt04_initial_direct_only_rayquery_compact32_geometry48
    upt04_initial_direct_only_rayquery_compact32_geometry48_light64
    upt04_initial_direct_only_rayquery_compact32_geometry48_light64_material48
    upt04_initial_indirect_only_rayquery
    upt04_initial_indirect_only_rayquery_compact
    upt04_initial_indirect_only_rayquery_compact32
    upt04_initial_indirect_only_rayquery_compact32_geometry48
    upt04_initial_indirect_only_rayquery_compact32_geometry48_light64
    upt04_initial_indirect_only_rayquery_compact32_geometry48_light64_material48
    upt04_initial_raygen
    upt04_initial_raygen_compact
    upt04_initial_raygen_compact32
    upt04_initial_direct_only_raygen
    upt04_initial_direct_only_raygen_compact
    upt04_initial_direct_only_raygen_compact32
    upt04_initial_indirect_only_raygen
    upt04_initial_indirect_only_raygen_compact
    upt04_initial_indirect_only_raygen_compact32
    upt04_initial_miss
    upt04_initial_closest_hit)

# UPT-41 production reachability set.  Keep the full runtime matrix above as
# an explicitly buildable audit/rollback target, but do not make the engine
# rebuild every obsolete packing, frozen-scene, backend and family cross
# product.  These are the accepted D0 route, its compact32/parity fallback,
# the Lambert diagnostic, and the optional bounded third vertex.
set(UPT04_PRODUCTION_RUNTIME_MODULES
    upt04_initial_rayquery_compact32
    upt04_initial_rayquery_compact32_light64
    upt04_initial_rayquery_compact32_unified_parity1
    upt04_compact_light_pack
    upt04_primary_image_copy
    upt04_initial_split_direct_rayquery_compact32_light64
    upt04_initial_split_direct_rayquery_compact32_light64_resolved
    upt04_initial_split_direct_rayquery_compact32_light64_lambertbsdf
    upt04_initial_split_direct_rayquery_compact32_light64_resolved_lambertbsdf
    upt04_initial_split_indirect_rayquery_compact32_light64
    upt04_initial_split_indirect_rayquery_compact32_light64_emissive_classify
    upt04_initial_split_indirect_rayquery_compact32_light64_emissive_classify_noskinned
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_noskinned
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_nee1
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_nee1
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_noexactmis
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_noexactmis
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_nee1_noexactmis
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_nee1_noexactmis
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_body_ray
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_body_hit
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_body_nee
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_body_decode
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_body_shade
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_body_brdf
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_body_pick
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_body_eval
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_q1sib
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_q1sib_consume
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_q1sib_consume_resolved
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_q1sib_lambertbsdf
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_q1sib_consume_lambertbsdf
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_q1sib_consume_resolved_lambertbsdf
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_consume_nee1_noexactmis_lambertbsdf
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_consume_nee1_noexactmis_resolved_lambertbsdf
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_consume_nee1_noexactmis
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_resolved
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_consume_nee1_noexactmis_resolved
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_nee1_noexactmis_neecache
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_consume_nee1_noexactmis_neecache
    upt04_initial_split_indirect_rayquery_compact32_light64_onepath_x3_produce_noskinned
    upt04_onepath_instance_pack
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_nee1_noexactmis_resolved
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_nee1_noexactmis_resolved
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_material48
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_material48
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_nee1_noexactmis_material48
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_nee1_noexactmis_material48
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_nee1_noexactmis_resolved_material48
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_nee1_noexactmis_resolved_material48
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_lambert
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_lambert
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_nee1_noexactmis_lambert
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_consume_nee1_noexactmis_lambert
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_debug_thin
    upt04_initial_split_indirect_rayquery_compact32_light64_x3_produce_debug_thin_ray
    upt04_initial_split_indirect_rayquery_compact32_geometry48_light64_x3_produce
    upt04_initial_split_indirect_rayquery_compact32_geometry48_light64_x3_produce_noskinned
    upt04_initial_split_indirect_rayquery_compact32_geometry48_light64_x3_consume
    upt04_initial_split_indirect_rayquery_compact32_light64_three_vertex
    upt04_initial_split_indirect_rayquery_compact32_light64_three_vertex_analytic_only
    upt04_initial_split_indirect_rayquery_compact32_light64_three_vertex_emissive_classify
    upt04_initial_split_indirect_rayquery_compact32_light64_three_vertex_emissive_consume
    upt04_initial_rayquery_compact32_lambert
    upt04_initial_rayquery_compact32_light64_lambert
    upt04_initial_reflection_direct_rayquery)
set(UPT04_RUNTIME_OUTPUTS)
set(UPT04_PRODUCTION_RUNTIME_OUTPUTS)
foreach(runtime_module IN LISTS UPT04_RUNTIME_MODULES)
    set(runtime_source "${UPT04_OUTPUT_DIR}/${runtime_module}.slang.spv")
    set(runtime_output "${UPT04_RUNTIME_OUTPUT_DIR}/${runtime_module}.bin")
    if(runtime_module IN_LIST UPT04_PRODUCTION_RUNTIME_MODULES)
        set(runtime_dependencies "${runtime_source}")
        if(runtime_module STREQUAL
                "upt04_initial_split_indirect_rayquery_compact32_light64_three_vertex")
            list(APPEND runtime_dependencies "${UPT35_VERIFY_STAMP}")
        endif()
        if(runtime_module STREQUAL
                "upt04_initial_split_indirect_rayquery_compact32_light64_three_vertex_emissive_classify"
           OR runtime_module STREQUAL
                "upt04_initial_split_indirect_rayquery_compact32_light64_three_vertex_emissive_consume"
           OR runtime_module STREQUAL
                "upt04_initial_split_indirect_rayquery_compact32_light64_emissive_classify")
            list(APPEND runtime_dependencies "${UPT56_VERIFY_STAMP}")
        endif()
    else()
        set(runtime_dependencies "${runtime_source}"
            "${UPT04_BACKEND_VERIFY_STAMP}"
            "${UPT04_STATIC_VERIFY_STAMP}"
            "${UPT05_MATERIAL_BOUNDARY_STAMP}"
            "${UPT04_LIGHT_TILE_VERIFY_STAMP}"
            "${UPT04_NATIVE_LIGHT_VIEW_VERIFY_STAMP}"
            "${UPT35_VERIFY_STAMP}")
    endif()
    add_custom_command(
        OUTPUT "${runtime_output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT04_RUNTIME_OUTPUT_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${runtime_source}" "${runtime_output}"
        DEPENDS ${runtime_dependencies}
        COMMENT "UPT-04 verified Vulkan runtime artifact: ${runtime_module}"
        VERBATIM)
    list(APPEND UPT04_RUNTIME_OUTPUTS "${runtime_output}")
    if(runtime_module IN_LIST UPT04_PRODUCTION_RUNTIME_MODULES)
        list(APPEND UPT04_PRODUCTION_RUNTIME_OUTPUTS "${runtime_output}")
    endif()
endforeach()

set(UPT04_LIVE_TLAS_PROBE_SLANG_RUNTIME_OUTPUT
    "${UPT04_RUNTIME_OUTPUT_DIR}/upt04_live_tlas_probe_slang.bin")
add_custom_command(
    OUTPUT "${UPT04_LIVE_TLAS_PROBE_SLANG_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT04_RUNTIME_OUTPUT_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT04_OUTPUT_DIR}/upt04_live_tlas_probe_slang.slang.spv"
        "${UPT04_LIVE_TLAS_PROBE_SLANG_RUNTIME_OUTPUT}"
    DEPENDS "${UPT04_OUTPUT_DIR}/upt04_live_tlas_probe_slang.slang.spv"
        "${UPT04_LIVE_TLAS_PROBE_VERIFY_STAMP}"
    COMMENT "UPT-04 verified live-TLAS Slang runtime probe"
    VERBATIM)
set(UPT04_LIVE_TLAS_PROBE_DXC_RUNTIME_OUTPUT
    "${UPT04_RUNTIME_OUTPUT_DIR}/upt04_live_tlas_probe_dxc.bin")
add_custom_command(
    OUTPUT "${UPT04_LIVE_TLAS_PROBE_DXC_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT04_RUNTIME_OUTPUT_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT04_OUTPUT_DIR}/upt04_live_tlas_probe_dxc.dxc.spv"
        "${UPT04_LIVE_TLAS_PROBE_DXC_RUNTIME_OUTPUT}"
    DEPENDS "${UPT04_OUTPUT_DIR}/upt04_live_tlas_probe_dxc.dxc.spv"
        "${UPT04_LIVE_TLAS_PROBE_VERIFY_STAMP}"
    COMMENT "UPT-04 verified live-TLAS DXC runtime probe"
    VERBATIM)
list(APPEND UPT04_RUNTIME_OUTPUTS
    "${UPT04_LIVE_TLAS_PROBE_SLANG_RUNTIME_OUTPUT}"
    "${UPT04_LIVE_TLAS_PROBE_DXC_RUNTIME_OUTPUT}")

set(UPT04_LIVE_TLAS_FULL_LAYOUT_PROBE_RUNTIME_OUTPUT
    "${UPT04_RUNTIME_OUTPUT_DIR}/upt04_live_tlas_full_layout_probe.bin")
add_custom_command(
    OUTPUT "${UPT04_LIVE_TLAS_FULL_LAYOUT_PROBE_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT04_RUNTIME_OUTPUT_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT04_OUTPUT_DIR}/upt04_live_tlas_full_layout_probe.slang.spv"
        "${UPT04_LIVE_TLAS_FULL_LAYOUT_PROBE_RUNTIME_OUTPUT}"
    DEPENDS "${UPT04_OUTPUT_DIR}/upt04_live_tlas_full_layout_probe.slang.spv"
        "${UPT04_LIVE_TLAS_PROBE_VERIFY_STAMP}"
    COMMENT "UPT-04 verified full-layout live-TLAS Slang runtime probe"
    VERBATIM)
list(APPEND UPT04_RUNTIME_OUTPUTS
    "${UPT04_LIVE_TLAS_FULL_LAYOUT_PROBE_RUNTIME_OUTPUT}")

set(UPT04_LIVE_TLAS_FULL_HOST_PROBE_RUNTIME_OUTPUT
    "${UPT04_RUNTIME_OUTPUT_DIR}/upt04_live_tlas_full_host_probe.bin")
add_custom_command(
    OUTPUT "${UPT04_LIVE_TLAS_FULL_HOST_PROBE_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT04_RUNTIME_OUTPUT_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT04_OUTPUT_DIR}/upt04_live_tlas_full_host_probe.slang.spv"
        "${UPT04_LIVE_TLAS_FULL_HOST_PROBE_RUNTIME_OUTPUT}"
    DEPENDS "${UPT04_OUTPUT_DIR}/upt04_live_tlas_full_host_probe.slang.spv"
        "${UPT04_LIVE_TLAS_PROBE_VERIFY_STAMP}"
    COMMENT "UPT-04 verified full-host live-TLAS Slang runtime probe"
    VERBATIM)
list(APPEND UPT04_RUNTIME_OUTPUTS
    "${UPT04_LIVE_TLAS_FULL_HOST_PROBE_RUNTIME_OUTPUT}")

set(UPT04_FULL_FRAME_TRAVERSAL_PROBE_RUNTIME_OUTPUT
    "${UPT04_RUNTIME_OUTPUT_DIR}/upt04_full_frame_traversal_probe.bin")
add_custom_command(
    OUTPUT "${UPT04_FULL_FRAME_TRAVERSAL_PROBE_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT04_RUNTIME_OUTPUT_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT04_OUTPUT_DIR}/upt04_full_frame_traversal_probe.slang.spv"
        "${UPT04_FULL_FRAME_TRAVERSAL_PROBE_RUNTIME_OUTPUT}"
    DEPENDS "${UPT04_OUTPUT_DIR}/upt04_full_frame_traversal_probe.slang.spv"
        "${UPT04_LIVE_TLAS_PROBE_VERIFY_STAMP}"
    COMMENT "UPT-04 verified full-frame traversal-isolation runtime probe"
    VERBATIM)
list(APPEND UPT04_RUNTIME_OUTPUTS
    "${UPT04_FULL_FRAME_TRAVERSAL_PROBE_RUNTIME_OUTPUT}")

add_custom_target(PathTraceSlangUpt04RuntimeArtifacts
    DEPENDS ${UPT04_RUNTIME_OUTPUTS})
set_target_properties(PathTraceSlangUpt04RuntimeArtifacts PROPERTIES FOLDER "Shader Targets/UPT-04")
add_dependencies(PathTraceSlangUpt04ModuleGraph PathTraceSlangUpt04RuntimeArtifacts)
add_custom_target(PathTraceSlangUpt04ProductionRuntime
    DEPENDS ${UPT04_PRODUCTION_RUNTIME_OUTPUTS})
set_target_properties(PathTraceSlangUpt04ProductionRuntime
    PROPERTIES FOLDER "Shader Targets/UPT-04")
if(TARGET RBDoom3BFG)
    add_dependencies(RBDoom3BFG PathTraceSlangUpt04ProductionRuntime)
endif()
