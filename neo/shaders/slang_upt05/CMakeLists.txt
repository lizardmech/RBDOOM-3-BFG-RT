include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/PathTracingSlang.cmake)

if(NOT PATH_TRACING_ENABLE_SLANG_UPT04 OR NOT USE_VULKAN)
    return()
endif()

set(UPT05_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/artifacts")
set(UPT05_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/.."
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04")
set(UPT05_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/upt05_resolve.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03/upt03_reservoir_contract.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_primary_surface_live_contract.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_material_primary_live.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_material_contract.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt05_material_openpbr.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_packed_primary_surface.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_packed_primary_surface32.slang")

path_tracing_declare_slang_spirv_module(
    NAME upt05_resolve_legacy
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt05_resolve.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT05_OUTPUT_DIR}"
    INCLUDE_DIRS ${UPT05_INCLUDE_DIRS}
    DEPENDS ${UPT05_DEPENDS}
    DEFINES UPT05_PRIMARY_RECEIVER_MODE=0
    OUT_VAR resolve_legacy_outputs)
path_tracing_declare_slang_spirv_module(
    NAME upt05_resolve_compact
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt05_resolve.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT05_OUTPUT_DIR}"
    INCLUDE_DIRS ${UPT05_INCLUDE_DIRS}
    DEPENDS ${UPT05_DEPENDS}
    DEFINES UPT05_PRIMARY_RECEIVER_MODE=1
    OUT_VAR resolve_compact_outputs)
path_tracing_declare_slang_spirv_module(
    NAME upt05_resolve_reflection_compact
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt05_resolve.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT05_OUTPUT_DIR}"
    INCLUDE_DIRS ${UPT05_INCLUDE_DIRS}
    DEPENDS ${UPT05_DEPENDS}
    DEFINES UPT05_PRIMARY_RECEIVER_MODE=1 UPT05_REFLECTION_REUSE_DOMAIN=1
    OUT_VAR resolve_reflection_compact_outputs)
path_tracing_declare_slang_spirv_module(
    NAME upt05_resolve_compact32
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt05_resolve.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT05_OUTPUT_DIR}"
    INCLUDE_DIRS ${UPT05_INCLUDE_DIRS}
    DEPENDS ${UPT05_DEPENDS}
    DEFINES UPT05_PRIMARY_RECEIVER_MODE=2
    OUT_VAR resolve_compact32_outputs)
set(resolve_outputs ${resolve_legacy_outputs} ${resolve_compact_outputs}
    ${resolve_reflection_compact_outputs} ${resolve_compact32_outputs})

set(UPT05_VERIFY_STAMP "${UPT05_OUTPUT_DIR}/upt05_resolve_verified.txt")
add_custom_command(
    OUTPUT "${UPT05_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT05_REFLECTION=${UPT05_OUTPUT_DIR}/upt05_resolve_compact32.slang.spirv-reflection.json
        -DUPT05_DISASSEMBLY=${UPT05_OUTPUT_DIR}/upt05_resolve_compact32.slang.spvasm
        -DUPT05_STAMP=${UPT05_VERIFY_STAMP}
        -DUPT05_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/upt05_resolve.slang
        -DUPT03_CONTRACT=${CMAKE_CURRENT_LIST_DIR}/../slang_upt03/upt03_reservoir_contract.slang
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt05Resolve.cmake"
    DEPENDS ${resolve_outputs}
        "${CMAKE_CURRENT_SOURCE_DIR}/upt05_resolve.slang"
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03/upt03_reservoir_contract.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt05Resolve.cmake"
    COMMENT "UPT-05 verifying trace-free RGBA16F resolve closure"
    VERBATIM)

set(UPT05_RUNTIME_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../base/renderprogs2/spirv/builtin/pathtracing/slang_upt05")
set(UPT05_RUNTIME_OUTPUTS)
foreach(resolve_variant upt05_resolve_legacy upt05_resolve_compact
        upt05_resolve_reflection_compact upt05_resolve_compact32)
    set(runtime_output "${UPT05_RUNTIME_DIR}/${resolve_variant}.bin")
    list(APPEND UPT05_RUNTIME_OUTPUTS "${runtime_output}")
    add_custom_command(
        OUTPUT "${runtime_output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT05_RUNTIME_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${UPT05_OUTPUT_DIR}/${resolve_variant}.slang.spv"
            "${runtime_output}"
        DEPENDS "${UPT05_OUTPUT_DIR}/${resolve_variant}.slang.spv"
            "${UPT05_VERIFY_STAMP}"
        COMMENT "UPT-05 verified Vulkan runtime artifact: ${resolve_variant}"
        VERBATIM)
endforeach()

add_custom_target(PathTraceSlangUpt05Resolve
    DEPENDS ${resolve_outputs} "${UPT05_VERIFY_STAMP}" ${UPT05_RUNTIME_OUTPUTS})
set_target_properties(PathTraceSlangUpt05Resolve PROPERTIES FOLDER "Shaders/UPT-05")
if(TARGET PathTraceSlangUpt04ProductionRuntime)
    add_dependencies(PathTraceSlangUpt05Resolve
        PathTraceSlangUpt04ProductionRuntime)
elseif(TARGET PathTraceSlangUpt04ModuleGraph)
    add_dependencies(PathTraceSlangUpt05Resolve PathTraceSlangUpt04ModuleGraph)
endif()
if(TARGET RBDoom3BFG)
    add_dependencies(RBDoom3BFG PathTraceSlangUpt05Resolve)
endif()
