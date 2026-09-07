include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/PathTracingSlang.cmake)

if(NOT PATH_TRACING_ENABLE_SLANG_UPT04 OR NOT USE_VULKAN)
    return()
endif()

set(UPT10_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/artifacts")
set(UPT10_CONTRACT "${CMAKE_CURRENT_SOURCE_DIR}/upt10_reuse_contract.slang")
set(UPT10_CONTRACT_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/upt10_reuse_contract.slangh")
path_tracing_declare_slang_spirv_module(
    NAME upt10_reuse_contract_probe
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt10_reuse_contract_probe.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT10_OUTPUT_DIR}"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS "${UPT10_CONTRACT}" "${UPT10_CONTRACT_HEADER}"
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03/upt03_reservoir_contract.slang"
    OUT_VAR upt10_outputs)

set(UPT10_VERIFY_STAMP "${UPT10_OUTPUT_DIR}/upt10_contract_verified.txt")
add_custom_command(
    OUTPUT "${UPT10_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT10_REFLECTION=${UPT10_OUTPUT_DIR}/upt10_reuse_contract_probe.slang.spirv-reflection.json
        -DUPT10_DISASSEMBLY=${UPT10_OUTPUT_DIR}/upt10_reuse_contract_probe.slang.spvasm
        -DUPT10_STAMP=${UPT10_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt10Contract.cmake"
    DEPENDS ${upt10_outputs} "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt10Contract.cmake"
    COMMENT "UPT-30 verifying resource-free shared reuse contract"
    VERBATIM)

add_executable(PathTraceSlangUpt10Oracle
    "${CMAKE_CURRENT_SOURCE_DIR}/upt10_contract_oracle.cpp")
target_compile_features(PathTraceSlangUpt10Oracle PRIVATE cxx_std_17)
set_target_properties(PathTraceSlangUpt10Oracle PROPERTIES FOLDER "Shader Targets/UPT-30")

add_executable(PathTraceSlangUpt10GpuHarness
    "${CMAKE_CURRENT_SOURCE_DIR}/upt10_gpu_harness.cpp")
target_compile_features(PathTraceSlangUpt10GpuHarness PRIVATE cxx_std_17)
target_link_libraries(PathTraceSlangUpt10GpuHarness PRIVATE nvrhi nvrhi_vk ${Vulkan_LIBRARY})
add_dependencies(PathTraceSlangUpt10GpuHarness PathTraceSlangUpt10Oracle)
set_target_properties(PathTraceSlangUpt10GpuHarness PROPERTIES FOLDER "Shader Targets/UPT-30")

set(UPT10_ORACLE_STAMP "${UPT10_OUTPUT_DIR}/upt10_cpu_oracle_passed.txt")
add_custom_command(
    OUTPUT "${UPT10_ORACLE_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT10_OUTPUT_DIR}"
    COMMAND $<TARGET_FILE:PathTraceSlangUpt10Oracle> "${UPT10_ORACLE_STAMP}"
    DEPENDS PathTraceSlangUpt10Oracle
    COMMENT "UPT-30 running deterministic CPU reuse oracle"
    VERBATIM)

set(UPT10_GPU_STAMP "${UPT10_OUTPUT_DIR}/upt10_cpu_gpu_bytes_passed.txt")
add_custom_command(
    OUTPUT "${UPT10_GPU_STAMP}"
    COMMAND $<TARGET_FILE:PathTraceSlangUpt10GpuHarness>
        "${UPT10_OUTPUT_DIR}" "${UPT10_GPU_STAMP}"
    DEPENDS PathTraceSlangUpt10GpuHarness ${upt10_outputs}
    COMMENT "UPT-30 comparing deterministic CPU and Vulkan shader bytes"
    VERBATIM)

add_custom_target(PathTraceSlangUpt10Contract
    DEPENDS ${upt10_outputs} "${UPT10_VERIFY_STAMP}" "${UPT10_ORACLE_STAMP}"
        "${UPT10_GPU_STAMP}")
set_target_properties(PathTraceSlangUpt10Contract PROPERTIES FOLDER "Shader Targets/UPT-30")
if(TARGET RBDoom3BFG)
    add_dependencies(RBDoom3BFG PathTraceSlangUpt10Contract)
endif()
