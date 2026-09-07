include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/PathTracingSlang.cmake)

if(NOT PATH_TRACING_ENABLE_SLANG_UPT04 OR NOT USE_VULKAN)
    return()
endif()

set(UPT08_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/artifacts")
path_tracing_declare_slang_spirv_module(
    NAME upt08_fill_sample_id
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt08_fill_sample_id.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT08_OUTPUT_DIR}"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/upt08_fill_sample_id.slang"
        "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03/upt03_reservoir_contract.slang"
    OUT_VAR upt08_fill_outputs)

path_tracing_declare_slang_spirv_module(
    NAME upt08_compute_duplication
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt08_compute_duplication.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT08_OUTPUT_DIR}"
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/upt08_compute_duplication.slang"
    OUT_VAR upt08_compute_outputs)

set(UPT08_VERIFY_STAMP "${UPT08_OUTPUT_DIR}/upt08_duplication_verified.txt")
add_custom_command(
    OUTPUT "${UPT08_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DUPT08_FILL_REFLECTION=${UPT08_OUTPUT_DIR}/upt08_fill_sample_id.slang.spirv-reflection.json
        -DUPT08_FILL_DISASSEMBLY=${UPT08_OUTPUT_DIR}/upt08_fill_sample_id.slang.spvasm
        -DUPT08_COMPUTE_REFLECTION=${UPT08_OUTPUT_DIR}/upt08_compute_duplication.slang.spirv-reflection.json
        -DUPT08_COMPUTE_DISASSEMBLY=${UPT08_OUTPUT_DIR}/upt08_compute_duplication.slang.spvasm
        -DUPT08_STAMP=${UPT08_VERIFY_STAMP}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt08Duplication.cmake"
    DEPENDS ${upt08_fill_outputs} ${upt08_compute_outputs}
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt08Duplication.cmake"
    COMMENT "UPT-08 verifying sample identity and 17x17 duplication map"
    VERBATIM)

set(UPT08_RUNTIME_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../base/renderprogs2/spirv/builtin/pathtracing/slang_upt08")
set(UPT08_FILL_RUNTIME_OUTPUT "${UPT08_RUNTIME_DIR}/upt08_fill_sample_id.bin")
set(UPT08_COMPUTE_RUNTIME_OUTPUT "${UPT08_RUNTIME_DIR}/upt08_compute_duplication.bin")
add_custom_command(
    OUTPUT "${UPT08_FILL_RUNTIME_OUTPUT}" "${UPT08_COMPUTE_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT08_RUNTIME_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT08_OUTPUT_DIR}/upt08_fill_sample_id.slang.spv"
        "${UPT08_FILL_RUNTIME_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${UPT08_OUTPUT_DIR}/upt08_compute_duplication.slang.spv"
        "${UPT08_COMPUTE_RUNTIME_OUTPUT}"
    DEPENDS ${upt08_fill_outputs} ${upt08_compute_outputs}
        "${UPT08_VERIFY_STAMP}"
    COMMENT "UPT-08 Vulkan runtime artifacts: sample identity and duplication map"
    VERBATIM)

add_custom_target(PathTraceSlangUpt08Duplication
    DEPENDS ${upt08_fill_outputs} ${upt08_compute_outputs}
        "${UPT08_VERIFY_STAMP}"
        "${UPT08_FILL_RUNTIME_OUTPUT}" "${UPT08_COMPUTE_RUNTIME_OUTPUT}")
set_target_properties(PathTraceSlangUpt08Duplication PROPERTIES FOLDER "Shader Targets/UPT-08")
if(TARGET RBDoom3BFG)
    add_dependencies(RBDoom3BFG PathTraceSlangUpt08Duplication)
endif()
