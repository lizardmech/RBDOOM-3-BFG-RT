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
set(UPT04_MODULES
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_surface_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_path_event_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_set_baseline_probe.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_material_set_deterministic_probe.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_random_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt04_random_white_hash.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03/upt03_reservoir_contract.slang")

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
