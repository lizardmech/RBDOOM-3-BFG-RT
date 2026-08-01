include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/PathTracingSlang.cmake)

option(PATH_TRACING_ENABLE_SLANG_UPT03
    "Enable the isolated UPT-03 deterministic Slang reservoir codec target"
    OFF)

if(NOT PATH_TRACING_ENABLE_SLANG_UPT03)
    return()
endif()
if(NOT USE_VULKAN)
    message(STATUS "UPT-03 Slang reservoir codec target disabled because USE_VULKAN is OFF")
    return()
endif()

set(UPT03_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/artifacts")
set(UPT03_CONTRACT "${CMAKE_CURRENT_SOURCE_DIR}/upt03_reservoir_contract.slang")
set(UPT03_PROVIDER "${CMAKE_CURRENT_SOURCE_DIR}/upt03_test_random_provider.slang")
path_tracing_declare_slang_spirv_module(
    NAME upt03_codec_test
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt03_codec_test.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT03_OUTPUT_DIR}"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS "${UPT03_CONTRACT}" "${UPT03_PROVIDER}"
    OUT_VAR upt03_outputs)

set(UPT03_VERIFY_STAMP "${UPT03_OUTPUT_DIR}/upt03_spirv_verified.txt")
add_custom_command(
    OUTPUT "${UPT03_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT03_REFLECTION=${UPT03_OUTPUT_DIR}/upt03_codec_test.slang.spirv-reflection.json
        -DUPT03_DISASSEMBLY=${UPT03_OUTPUT_DIR}/upt03_codec_test.slang.spvasm
        -DUPT03_STAMP=${UPT03_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt03Spirv.cmake"
    DEPENDS ${upt03_outputs} "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt03Spirv.cmake"
    COMMENT "UPT-03 verifying deterministic codec SPIR-V layout and zero-ray contract"
    VERBATIM)

add_custom_target(PathTraceSlangUpt03Shaders
    DEPENDS ${upt03_outputs} "${UPT03_VERIFY_STAMP}")
set_target_properties(PathTraceSlangUpt03Shaders PROPERTIES FOLDER "Shaders/UPT-03")

add_executable(PathTraceSlangUpt03Harness
    "${CMAKE_CURRENT_SOURCE_DIR}/upt03_gpu_harness.cpp")
target_compile_features(PathTraceSlangUpt03Harness PRIVATE cxx_std_17)
target_include_directories(PathTraceSlangUpt03Harness PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../tests/upt02")
target_link_libraries(PathTraceSlangUpt03Harness PRIVATE nvrhi nvrhi_vk ${Vulkan_LIBRARY})
add_dependencies(PathTraceSlangUpt03Harness PathTraceSlangUpt03Shaders)
set_target_properties(PathTraceSlangUpt03Harness PROPERTIES FOLDER "Shaders/UPT-03")

add_custom_target(RunPathTraceSlangUpt03Harness
    COMMAND $<TARGET_FILE:PathTraceSlangUpt03Harness> "${UPT03_OUTPUT_DIR}"
    DEPENDS PathTraceSlangUpt03Harness PathTraceSlangUpt03Shaders
    COMMENT "UPT-03 running bounded Vulkan CPU/GPU codec byte comparison"
    VERBATIM)
set_target_properties(RunPathTraceSlangUpt03Harness PROPERTIES FOLDER "Shaders/UPT-03")
