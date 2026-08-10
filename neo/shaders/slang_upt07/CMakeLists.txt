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
    "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_direct_bindings.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_surface_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_reservoir.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt07_material_provider_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt07_material_openpbr.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt07_material_lambert_diagnostic.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt07_emissive_material.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt07_indirect_material.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt07_temporal_indirect.slang"
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
upt07_declare_lambert_variant(upt07_temporal_unified_rayquery_light64_lambert
    UPT07_PACKED_LIGHT_RECORD=1)
upt07_declare_lambert_variant(upt07_temporal_unified_rayquery_duplication_lambert
    UPT07_DUPLICATION_MAP=1)
upt07_declare_lambert_variant(
    upt07_temporal_unified_rayquery_light64_duplication_lambert
    UPT07_PACKED_LIGHT_RECORD=1 UPT07_DUPLICATION_MAP=1)

set(UPT07_BOTTLENECK_OUTPUTS)
foreach(bottleneck_stage RANGE 1 5)
    set(bottleneck_module
        "upt07_temporal_unified_rayquery_light64_duplication_lambert_probe${bottleneck_stage}")
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
            UPT_DIAGNOSTIC_LAMBERT=1 UPT07_PACKED_LIGHT_RECORD=1
            UPT07_DUPLICATION_MAP=1
            UPT07_BOTTLENECK_STAGE=${bottleneck_stage}
        OUT_VAR bottleneck_outputs)
    list(APPEND UPT07_BOTTLENECK_OUTPUTS ${bottleneck_outputs})
endforeach()

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
        -DUPT07_STAMP=${UPT07_DIRECT_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt07TemporalDirect.cmake"
    DEPENDS ${temporal_direct_outputs} ${temporal_direct_compact_outputs}
        ${temporal_unified_outputs} ${temporal_unified_compact_outputs}
        ${temporal_unified_route_diag_outputs}
        ${temporal_unified_compact_route_diag_outputs}
        ${temporal_unified_reconnect_outputs}
        ${temporal_unified_compact_reconnect_outputs}
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt07TemporalDirect.cmake"
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
set(UPT07_LAMBERT_RUNTIME_OUTPUTS)
foreach(lambert_module
        upt07_temporal_unified_rayquery_lambert
        upt07_temporal_unified_rayquery_light64_lambert
        upt07_temporal_unified_rayquery_duplication_lambert
        upt07_temporal_unified_rayquery_light64_duplication_lambert)
    set(lambert_runtime "${UPT07_RUNTIME_DIR}/${lambert_module}.bin")
    add_custom_command(
        OUTPUT "${lambert_runtime}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT07_RUNTIME_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${UPT07_OUTPUT_DIR}/${lambert_module}.slang.spv"
            "${lambert_runtime}"
        DEPENDS ${UPT07_LAMBERT_OUTPUTS}
        COMMENT "UPT-07 Vulkan Lambert diagnostic artifact: ${lambert_module}"
        VERBATIM)
    list(APPEND UPT07_LAMBERT_RUNTIME_OUTPUTS "${lambert_runtime}")
endforeach()
set(UPT07_BOTTLENECK_RUNTIME_OUTPUTS)
foreach(bottleneck_stage RANGE 1 5)
    set(bottleneck_module
        "upt07_temporal_unified_rayquery_light64_duplication_lambert_probe${bottleneck_stage}")
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
add_custom_command(
    OUTPUT "${UPT07_DIRECT_RUNTIME_OUTPUT}" "${UPT07_DIRECT_COMPACT_RUNTIME_OUTPUT}"
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
        "${UPT07_UNIFIED_RECONNECT_RUNTIME_OUTPUT}"
        "${UPT07_UNIFIED_COMPACT_RECONNECT_RUNTIME_OUTPUT}"
        "${UPT07_UNIFIED_DUPLICATION_RECONNECT_RUNTIME_OUTPUT}"
        "${UPT07_UNIFIED_COMPACT_DUPLICATION_RECONNECT_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT07_RUNTIME_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT07_OUTPUT_DIR}/upt07_temporal_direct_rayquery.slang.spv"
        "${UPT07_DIRECT_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT07_OUTPUT_DIR}/upt07_temporal_direct_rayquery_light64.slang.spv"
        "${UPT07_DIRECT_COMPACT_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT07_OUTPUT_DIR}/upt07_temporal_direct_rayquery_duplication.slang.spv"
        "${UPT07_DIRECT_DUPLICATION_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT07_OUTPUT_DIR}/upt07_temporal_direct_rayquery_light64_duplication.slang.spv"
        "${UPT07_DIRECT_COMPACT_DUPLICATION_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery.slang.spv"
        "${UPT07_UNIFIED_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64.slang.spv"
        "${UPT07_UNIFIED_COMPACT_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_duplication.slang.spv"
        "${UPT07_UNIFIED_DUPLICATION_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_duplication.slang.spv"
        "${UPT07_UNIFIED_COMPACT_DUPLICATION_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_route_diag.slang.spv"
        "${UPT07_UNIFIED_ROUTE_DIAG_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_route_diag.slang.spv"
        "${UPT07_UNIFIED_COMPACT_ROUTE_DIAG_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_duplication_route_diag.slang.spv"
        "${UPT07_UNIFIED_DUPLICATION_ROUTE_DIAG_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_duplication_route_diag.slang.spv"
        "${UPT07_UNIFIED_COMPACT_DUPLICATION_ROUTE_DIAG_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_reconnect.slang.spv"
        "${UPT07_UNIFIED_RECONNECT_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_reconnect.slang.spv"
        "${UPT07_UNIFIED_COMPACT_RECONNECT_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_duplication_reconnect.slang.spv"
        "${UPT07_UNIFIED_DUPLICATION_RECONNECT_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT07_OUTPUT_DIR}/upt07_temporal_unified_rayquery_light64_duplication_reconnect.slang.spv"
        "${UPT07_UNIFIED_COMPACT_DUPLICATION_RECONNECT_RUNTIME_OUTPUT}"
    DEPENDS ${temporal_direct_outputs} ${temporal_direct_compact_outputs}
        ${temporal_direct_duplication_outputs}
        ${temporal_direct_compact_duplication_outputs}
        ${temporal_unified_outputs} ${temporal_unified_compact_outputs}
        ${temporal_unified_duplication_outputs}
        ${temporal_unified_compact_duplication_outputs}
        ${temporal_unified_route_diag_outputs}
        ${temporal_unified_compact_route_diag_outputs}
        ${temporal_unified_duplication_route_diag_outputs}
        ${temporal_unified_compact_duplication_route_diag_outputs}
        ${temporal_unified_reconnect_outputs}
        ${temporal_unified_compact_reconnect_outputs}
        ${temporal_unified_duplication_reconnect_outputs}
        ${temporal_unified_compact_duplication_reconnect_outputs}
        "${UPT07_DIRECT_VERIFY_STAMP}"
    COMMENT "UPT-07 verified Vulkan runtime artifacts: primary-direct temporal shift"
    VERBATIM)

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
        ${UPT07_BOTTLENECK_RUNTIME_OUTPUTS})
set_target_properties(PathTraceSlangUpt07TemporalOwnership PROPERTIES
    FOLDER "Shaders/UPT-07")
if(TARGET RBDoom3BFG)
    add_dependencies(RBDoom3BFG PathTraceSlangUpt07TemporalOwnership)
endif()
