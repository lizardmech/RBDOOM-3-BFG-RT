include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/PathTracingSlang.cmake)

if(NOT PATH_TRACING_ENABLE_SLANG_UPT04 OR NOT USE_VULKAN)
    return()
endif()

set(UPT45_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/artifacts")
set(UPT45_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/.."
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04")
set(UPT45_PRODUCER_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/upt45_clear_window_psr.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt45_glass_contract.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/upt45_narrow_geometry.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_binary16.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_packed_primary_surface.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_packed_primary_surface32.slang"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04/upt04_primary_history_sidecar.slang")

path_tracing_declare_slang_spirv_module(
    NAME upt45_clear_window_psr
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt45_clear_window_psr.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT45_OUTPUT_DIR}"
    INCLUDE_DIRS ${UPT45_INCLUDE_DIRS}
    DEPENDS ${UPT45_PRODUCER_DEPENDS}
    OUT_VAR producer_outputs
    RAY_QUERY)

path_tracing_declare_slang_spirv_module(
    NAME upt45_glass_compose
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt45_glass_compose.slang"
    ENTRY main
    STAGE compute
    OUTPUT_DIR "${UPT45_OUTPUT_DIR}"
    INCLUDE_DIRS ${UPT45_INCLUDE_DIRS}
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/upt45_glass_compose.slang"
    OUT_VAR compose_outputs)

set(UPT45_RUNTIME_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../base/renderprogs2/spirv/builtin/pathtracing/slang_upt45")
set(UPT45_RUNTIME_OUTPUTS)
foreach(module upt45_clear_window_psr upt45_glass_compose)
    set(runtime_output "${UPT45_RUNTIME_DIR}/${module}.bin")
    list(APPEND UPT45_RUNTIME_OUTPUTS "${runtime_output}")
    add_custom_command(
        OUTPUT "${runtime_output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT45_RUNTIME_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${UPT45_OUTPUT_DIR}/${module}.slang.spv"
            "${runtime_output}"
        DEPENDS "${UPT45_OUTPUT_DIR}/${module}.slang.spv"
        COMMENT "UPT-45 native Vulkan artifact: ${module}"
        VERBATIM)
endforeach()

set(UPT45_VERIFY_STAMP "${UPT45_OUTPUT_DIR}/upt45_native_glass_verified.stamp")
add_custom_command(
    OUTPUT "${UPT45_VERIFY_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DINPUT_PRODUCER_ASM=${UPT45_OUTPUT_DIR}/upt45_clear_window_psr.slang.spvasm
        -DINPUT_PRODUCER_REFLECTION=${UPT45_OUTPUT_DIR}/upt45_clear_window_psr.slang.spirv-reflection.json
        -DINPUT_COMPOSE_REFLECTION=${UPT45_OUTPUT_DIR}/upt45_glass_compose.slang.spirv-reflection.json
        -DINPUT_PRODUCER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/upt45_clear_window_psr.slang
        -DINPUT_GEOMETRY_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/upt45_narrow_geometry.slang
        -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt45NativeGlass.cmake"
    COMMAND "${CMAKE_COMMAND}" -E touch "${UPT45_VERIFY_STAMP}"
    DEPENDS
        ${producer_outputs}
        ${compose_outputs}
        "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUpt45NativeGlass.cmake"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt45_clear_window_psr.slang"
        "${CMAKE_CURRENT_SOURCE_DIR}/upt45_narrow_geometry.slang"
    COMMENT "Verifying UPT-45 native glass descriptor and ray ceiling"
    VERBATIM)

add_custom_target(PathTraceSlangUpt45Glass
    DEPENDS ${producer_outputs} ${compose_outputs} ${UPT45_RUNTIME_OUTPUTS} "${UPT45_VERIFY_STAMP}")
set_target_properties(PathTraceSlangUpt45Glass PROPERTIES FOLDER "Shaders/UPT-45")
if(TARGET RBDoom3BFG)
    add_dependencies(RBDoom3BFG PathTraceSlangUpt45Glass)
endif()
