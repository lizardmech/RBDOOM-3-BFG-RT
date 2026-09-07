include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/PathTracingSlang.cmake)

option(PATH_TRACING_ENABLE_SLANG_UPT00
    "Enable the isolated Vulkan-only Slang/DXC compiler comparison target"
    OFF)

if(NOT PATH_TRACING_ENABLE_SLANG_UPT00)
    return()
endif()

if(NOT USE_VULKAN)
    message(STATUS "UPT-00 Slang compiler comparison target disabled because USE_VULKAN is OFF")
    return()
endif()

path_tracing_find_slang_spirv_tools()

set(UPT00_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/artifacts")
set(UPT00_COMMON_SLANG "${CMAKE_CURRENT_SOURCE_DIR}/upt00_contracts.slang")
set(UPT00_COMMON_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/upt00_contracts.hlsli")
set(UPT00_ALL_OUTPUTS)

function(upt00_add_kernel kernel_name)
    set(options RAY_QUERY)
    cmake_parse_arguments(UPT00 "${options}" "" "" ${ARGN})

    set(extra_args)
    if(UPT00_RAY_QUERY)
        list(APPEND extra_args RAY_QUERY)
    endif()

    path_tracing_declare_slang_spirv_module(
        NAME "${kernel_name}"
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/${kernel_name}.slang"
        ENTRY main
        STAGE compute
        OUTPUT_DIR "${UPT00_OUTPUT_DIR}"
        INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        DEPENDS "${UPT00_COMMON_SLANG}"
        OUT_VAR slang_outputs
        ${extra_args})

    path_tracing_declare_dxc_spirv_comparison_module(
        NAME "${kernel_name}"
        SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/${kernel_name}.hlsl"
        ENTRY main
        PROFILE cs_6_6
        OUTPUT_DIR "${UPT00_OUTPUT_DIR}"
        INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        DEPENDS "${UPT00_COMMON_HLSL}"
        OUT_VAR dxc_outputs
        ${extra_args})

    set(UPT00_ALL_OUTPUTS
        ${UPT00_ALL_OUTPUTS}
        ${slang_outputs}
        ${dxc_outputs}
        PARENT_SCOPE)
endfunction()

upt00_add_kernel(structured_math)
upt00_add_kernel(ray_query RAY_QUERY)
upt00_add_kernel(reservoir_update)

execute_process(
    COMMAND "${SLANGC_EXECUTABLE}" -version
    OUTPUT_VARIABLE UPT00_SLANG_VERSION_STDOUT
    ERROR_VARIABLE UPT00_SLANG_VERSION_STDERR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE)
if(UPT00_SLANG_VERSION_STDOUT)
    set(UPT00_SLANG_VERSION "${UPT00_SLANG_VERSION_STDOUT}")
else()
    set(UPT00_SLANG_VERSION "${UPT00_SLANG_VERSION_STDERR}")
endif()
execute_process(
    COMMAND "${DXC_SPIRV_PATH}" --version
    OUTPUT_VARIABLE UPT00_DXC_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE)
execute_process(
    COMMAND "${SPIRV_VAL_EXECUTABLE}" --version
    OUTPUT_VARIABLE UPT00_SPIRV_TOOLS_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE)

string(REPLACE "\n" " | " UPT00_DXC_VERSION_SINGLE_LINE "${UPT00_DXC_VERSION}")
string(REPLACE "\n" " | " UPT00_SPIRV_TOOLS_VERSION_SINGLE_LINE "${UPT00_SPIRV_TOOLS_VERSION}")
file(GENERATE
    OUTPUT "${UPT00_OUTPUT_DIR}/toolchain.txt"
    CONTENT
"UPT-00 Vulkan-only compiler comparison
Slang: ${UPT00_SLANG_VERSION}
DXC: ${UPT00_DXC_VERSION_SINGLE_LINE}
SPIR-V Tools: ${UPT00_SPIRV_TOOLS_VERSION_SINGLE_LINE}
Slang common flags: -target spirv -profile sm_6_6 -stage compute -O3 -matrix-layout-row-major -fvk-use-scalar-layout -fvk-use-entrypoint-name -restrictive-capability-check -capability SPIRV_1_5
Slang ray-query addition: -capability SPV_KHR_ray_query
DXC common flags: -T cs_6_6 -E main -spirv -fspv-target-env=vulkan1.2 -fvk-use-scalar-layout -fspv-entrypoint-name=main -Zpr -O3
DXC ray-query addition: -fspv-extension=SPV_KHR_ray_query
Validation: spirv-val --target-env vulkan1.2
No DXIL output or DX12 compiler target is declared by PathTraceSlangUpt00Shaders.
")

set(UPT00_VERIFY_STAMP "${UPT00_OUTPUT_DIR}/spirv_contract_verified.txt")
set(UPT00_METRICS_OUTPUT "${UPT00_OUTPUT_DIR}/spirv_metrics.csv")
add_custom_command(
    OUTPUT "${UPT00_VERIFY_STAMP}" "${UPT00_METRICS_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT00_ARTIFACT_DIR=${UPT00_OUTPUT_DIR}
        -DUPT00_VERIFY_STAMP=${UPT00_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt00Spirv.cmake"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT00_ARTIFACT_DIR=${UPT00_OUTPUT_DIR}
        -DUPT00_METRICS_OUTPUT=${UPT00_METRICS_OUTPUT}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/GenerateUpt00Metrics.cmake"
    DEPENDS
        ${UPT00_ALL_OUTPUTS}
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt00Spirv.cmake"
        "${CMAKE_CURRENT_SOURCE_DIR}/GenerateUpt00Metrics.cmake"
    COMMENT "UPT-00 verifying ABI-facing SPIR-V and recording compiler metrics"
    VERBATIM)

add_custom_target(PathTraceSlangUpt00Shaders DEPENDS
    ${UPT00_ALL_OUTPUTS}
    "${UPT00_OUTPUT_DIR}/toolchain.txt"
    "${UPT00_VERIFY_STAMP}"
    "${UPT00_METRICS_OUTPUT}")
set_target_properties(PathTraceSlangUpt00Shaders PROPERTIES FOLDER "Shader Targets/UPT-00")

# Standalone Vulkan/NVRHI proof executable. It owns a tiny synthetic workload
# and is deliberately not linked into, or run by, the game target.
add_executable(PathTraceSlangUpt00Harness
    "${CMAKE_CURRENT_SOURCE_DIR}/upt00_gpu_harness.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt00_abi.h")
target_compile_features(PathTraceSlangUpt00Harness PRIVATE cxx_std_17)
target_link_libraries(PathTraceSlangUpt00Harness PRIVATE nvrhi nvrhi_vk ${Vulkan_LIBRARY})
add_dependencies(PathTraceSlangUpt00Harness PathTraceSlangUpt00Shaders)
set_target_properties(PathTraceSlangUpt00Harness PROPERTIES FOLDER "Shader Targets/UPT-00")

add_custom_target(RunPathTraceSlangUpt00Harness
    COMMAND "${CMAKE_COMMAND}"
        -DUPT00_HARNESS=$<TARGET_FILE:PathTraceSlangUpt00Harness>
        -DUPT00_ARTIFACT_DIR=${UPT00_OUTPUT_DIR}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/RunUpt00Trials.cmake"
    DEPENDS PathTraceSlangUpt00Harness PathTraceSlangUpt00Shaders
    COMMENT "UPT-00 running isolated Vulkan ABI/readback and compiler timing comparison"
    VERBATIM)
set_target_properties(RunPathTraceSlangUpt00Harness PROPERTIES FOLDER "Shader Targets/UPT-00")
