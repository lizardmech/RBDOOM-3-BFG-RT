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
    "${UPT04_RANDOM_SLOT_LEDGER}"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03/upt03_reservoir_contract.slang")

set(UPT04_BACKEND_MODULES
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_primary_surface_live_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_packed_primary_surface.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_hit_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_light_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_control.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_live_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_provider_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_bindings.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_local_light_proposal.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_transport_initial_bounded.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_set_live_probe.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_primary_live.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt05_material_openpbr.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt05_material_set_openpbr.slang"
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
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_direct.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_initial_indirect.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_local_light_evaluation.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_bindings.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_geometry_decode.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_emissive_replay.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_emissive_lookup.slang")

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
set_target_properties(PathTraceSlangUpt04ModuleGraph PROPERTIES FOLDER "Shaders/UPT-04")

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
    OUT_VAR diagnostic_outputs
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
        -DUPT04_STAMP=${UPT04_BACKEND_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt04BackendClosure.cmake"
    DEPENDS ${UPT04_BACKEND_OUTPUTS}
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt04BackendClosure.cmake"
    COMMENT "UPT-04 verifying RayQuery/raygen bounded-transport closure"
    VERBATIM)

add_custom_target(PathTraceSlangUpt04BackendClosure
    DEPENDS ${UPT04_BACKEND_OUTPUTS} "${UPT04_BACKEND_VERIFY_STAMP}")
set_target_properties(PathTraceSlangUpt04BackendClosure PROPERTIES FOLDER "Shaders/UPT-04")
add_dependencies(PathTraceSlangUpt04ModuleGraph PathTraceSlangUpt04BackendClosure)

set(UPT04_GEOMETRY_MODULES
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
set_target_properties(PathTraceSlangUpt04GeometryClosure PROPERTIES FOLDER "Shaders/UPT-04")
add_dependencies(PathTraceSlangUpt04ModuleGraph PathTraceSlangUpt04GeometryClosure)

set(UPT04_STATIC_SPECIALIZATION_OUTPUTS)
foreach(family direct indirect)
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
endforeach()

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
        -DUPT04_STAMP=${UPT04_STATIC_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt04StaticSpecializations.cmake"
    DEPENDS ${UPT04_STATIC_SPECIALIZATION_OUTPUTS} ${UPT04_BACKEND_OUTPUTS}
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt04StaticSpecializations.cmake"
    COMMENT "UPT-04 verifying static direct/indirect family closures"
    VERBATIM)

add_custom_target(PathTraceSlangUpt04StaticSpecializations
    DEPENDS ${UPT04_STATIC_SPECIALIZATION_OUTPUTS} "${UPT04_STATIC_VERIFY_STAMP}")
set_target_properties(PathTraceSlangUpt04StaticSpecializations PROPERTIES FOLDER "Shaders/UPT-04")
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
    PROPERTIES FOLDER "Shaders/UPT-05")
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
    PROPERTIES FOLDER "Shaders/UPT-05")
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
set(UPT04_RUNTIME_MODULES
    upt04_initial_rayquery
    upt04_initial_rayquery_compact
    upt04_initial_diagnostics
    upt04_initial_direct_only_rayquery
    upt04_initial_direct_only_rayquery_compact
    upt04_initial_indirect_only_rayquery
    upt04_initial_indirect_only_rayquery_compact
    upt04_initial_raygen
    upt04_initial_raygen_compact
    upt04_initial_direct_only_raygen
    upt04_initial_direct_only_raygen_compact
    upt04_initial_indirect_only_raygen
    upt04_initial_indirect_only_raygen_compact
    upt04_initial_miss
    upt04_initial_closest_hit)
set(UPT04_RUNTIME_OUTPUTS)
foreach(runtime_module IN LISTS UPT04_RUNTIME_MODULES)
    set(runtime_source "${UPT04_OUTPUT_DIR}/${runtime_module}.slang.spv")
    set(runtime_output "${UPT04_RUNTIME_OUTPUT_DIR}/${runtime_module}.bin")
    add_custom_command(
        OUTPUT "${runtime_output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT04_RUNTIME_OUTPUT_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${runtime_source}" "${runtime_output}"
        DEPENDS "${runtime_source}"
            "${UPT04_BACKEND_VERIFY_STAMP}"
            "${UPT04_STATIC_VERIFY_STAMP}"
            "${UPT05_MATERIAL_BOUNDARY_STAMP}"
        COMMENT "UPT-04 verified Vulkan runtime artifact: ${runtime_module}"
        VERBATIM)
    list(APPEND UPT04_RUNTIME_OUTPUTS "${runtime_output}")
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
set_target_properties(PathTraceSlangUpt04RuntimeArtifacts PROPERTIES FOLDER "Shaders/UPT-04")
add_dependencies(PathTraceSlangUpt04ModuleGraph PathTraceSlangUpt04RuntimeArtifacts)
if(TARGET RBDoom3BFG)
    add_dependencies(RBDoom3BFG PathTraceSlangUpt04RuntimeArtifacts)
endif()
