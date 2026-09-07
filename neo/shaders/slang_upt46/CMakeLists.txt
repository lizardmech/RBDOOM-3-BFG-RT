include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/PathTracingSlang.cmake)

if(NOT PATH_TRACING_ENABLE_SLANG_UPT04 OR NOT USE_VULKAN)
    return()
endif()

set(UPT46_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/artifacts")
set(UPT46_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/upt46_glass_optical_transport.slang")
set(UPT46_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt04"
    "${CMAKE_CURRENT_LIST_DIR}/../slang_upt03"
    "${CMAKE_CURRENT_LIST_DIR}/..")

foreach(light_mode natural compact64)
    set(upt46_defines UPT04_PACKED_PRIMARY_RECEIVER=0)
    if(light_mode STREQUAL "compact64")
        list(APPEND upt46_defines UPT04_PACKED_LIGHT_RECORD=1)
    endif()
    path_tracing_declare_slang_spirv_module(
        NAME "upt46_glass_optical_transport_${light_mode}"
        SOURCE "${UPT46_SOURCE}"
        ENTRY main
        STAGE compute
        OUTPUT_DIR "${UPT46_OUTPUT_DIR}"
        INCLUDE_DIRS ${UPT46_INCLUDE_DIRS}
        DEPENDS "${UPT46_SOURCE}"
        DEFINES ${upt46_defines}
        OUT_VAR upt46_outputs_${light_mode}
        RAY_QUERY)
    list(APPEND UPT46_ALL_OUTPUTS ${upt46_outputs_${light_mode}})
endforeach()

set(UPT46_RUNTIME_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../base/renderprogs2/spirv/builtin/pathtracing/slang_upt46")
set(UPT46_RUNTIME_OUTPUTS)
foreach(light_mode natural compact64)
    set(runtime_output
        "${UPT46_RUNTIME_DIR}/upt46_glass_optical_transport_${light_mode}.bin")
    add_custom_command(
        OUTPUT "${runtime_output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${UPT46_RUNTIME_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${UPT46_OUTPUT_DIR}/upt46_glass_optical_transport_${light_mode}.slang.spv"
            "${runtime_output}"
        DEPENDS ${upt46_outputs_${light_mode}}
        COMMENT "UPT-46 native Vulkan glass optical transport (${light_mode})"
        VERBATIM)
    list(APPEND UPT46_RUNTIME_OUTPUTS "${runtime_output}")
endforeach()

add_custom_target(PathTraceSlangUpt46Glass
    DEPENDS ${UPT46_ALL_OUTPUTS} ${UPT46_RUNTIME_OUTPUTS})
set_target_properties(PathTraceSlangUpt46Glass PROPERTIES FOLDER "Shader Targets/UPT-46")
if(TARGET RBDoom3BFG)
    add_dependencies(RBDoom3BFG PathTraceSlangUpt46Glass)
endif()
