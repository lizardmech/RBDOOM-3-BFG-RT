include(../compileshaders.cmake)

file(GLOB globalshaders "BRDF.inc.hlsl" "global_inc.hlsl" "vulkan.hlsli" "renderParmSet0.inc.hlsl" "renderParmSet1.inc.hlsl" "renderParmSet2.inc.hlsl" "renderParmSet3.inc.hlsl" "renderParmSet4.inc.hlsl" "renderParmSet5.inc.hlsl" "renderParmSet6.inc.hlsl" "renderParmSet7.inc.hlsl" "renderParmSet8.inc.hlsl" "renderParmSet9.inc.hlsl" "renderParmSet10.inc.hlsl" "renderParmSet11.inc.hlsl" "renderParmSet12.inc.hlsl" "renderParmSet13.inc.hlsl" "renderParmSet14.inc.hlsl")
file(GLOB builtins "builtin/*.hlsl")
file(GLOB debug "builtin/debug/*hlsl")
file(GLOB fog "builtin/fog/*hlsl")
file(GLOB legacy "builtin/legacy/*hlsl")
file(GLOB lighting "builtin/lighting/*hlsl")
file(GLOB post "builtin/post/*hlsl")
file(GLOB pathtracing "builtin/pathtracing/*hlsl")
file(GLOB_RECURSE cleanroom_rtxdi "builtin/pathtracing/cleanroom_rtxdi/*hlsl")
file(GLOB_RECURSE remix_restir_gi "builtin/pathtracing/remix_restir_gi/*hlsl")
file(GLOB ssao "builtin/SSAO/*hlsl")
file(GLOB ssgi "builtin/SSGI/*hlsl")
file(GLOB video "builtin/video/*hlsl")
file(GLOB vr "builtin/VR/*hlsl")
file(GLOB usershaders "*.hlsl")

list(REMOVE_ITEM usershaders ${globalshaders})

set(shaders ${globalshaders} ${builtins} ${debug} ${fog} ${legacy} ${lighting} ${post} ${pathtracing} ${cleanroom_rtxdi} ${remix_restir_gi} ${ssao} ${ssgi} ${video} ${vr} ${usershaders})

source_group("builtin" FILES ${builtins})
source_group("builtin/debug" FILES ${debug})
source_group("builtin/fog" FILES ${fog})
source_group("builtin/legacy" FILES ${legacy})
source_group("builtin/lighting" FILES ${lighting})
source_group("builtin/post" FILES ${post})
source_group("builtin/pathtracing" FILES ${pathtracing})
source_group("builtin/pathtracing/cleanroom_rtxdi" FILES ${cleanroom_rtxdi})
source_group("builtin/pathtracing/remix_restir_gi" FILES ${remix_restir_gi})
source_group("builtin/SSAO" FILES ${ssao})
source_group("builtin/SSGI" FILES ${ssgi})
source_group("builtin/video" FILES ${video})
source_group("builtin/VR" FILES ${vr})
source_group("global" FILES ${globalshaders})

source_group("user" FILES ${usershaders})

if(STANDALONE)
    compile_shaders_all_platforms(
        TARGET Shaders
        CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/shaders.cfg
        OUTPUT_BASE ${CMAKE_CURRENT_SOURCE_DIR}/../../content/renderprogs2
        SHADER_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
        SOURCES ${shaders}
    )
else()
    compile_shaders_all_platforms(
        TARGET Shaders
        CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/shaders.cfg
        OUTPUT_BASE ${CMAKE_CURRENT_SOURCE_DIR}/../../base/renderprogs2 # This path scheme is a bit odd.
        SHADER_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
        SOURCES ${shaders}
    )
endif()

set(PATH_TRACING_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/pathtrace_smoke.rt.hlsl)
set(PATH_TRACING_MODE18_RESTIR_HYBRID_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/pathtrace_smoke_mode18_restir_hybrid.rt.hlsl)
set(PATH_TRACING_RESTIR_INITIAL_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/pathtrace_smoke_restir_initial.rt.hlsl)
set(PATH_TRACING_RESTIR_TEMPORAL_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/pathtrace_smoke_restir_temporal.rt.hlsl)
set(PATH_TRACING_RESTIR_TEMPORAL_SHADING_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/pathtrace_smoke_restir_temporal_shading.rt.hlsl)
set(PATH_TRACING_RESTIR_ATTRIBUTION_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/pathtrace_smoke_restir_attribution.rt.hlsl)
set(PATH_TRACING_RESTIR_SPATIAL_RESERVOIR_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/pathtrace_smoke_restir_spatial_reservoir.rt.hlsl)
set(PATH_TRACING_RESTIR_SPATIAL_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/pathtrace_smoke_restir_spatial.rt.hlsl)
set(PATH_TRACING_RESTIR_SPATIAL_ATTRIBUTION_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/pathtrace_smoke_restir_spatial_attribution.rt.hlsl)
set(PATH_TRACING_RESTIR_COMBINED_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/pathtrace_smoke_restir_combined.rt.hlsl)
set(PATH_TRACING_PRIMARY_SURFACE_PRODUCER_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/pathtrace_primary_surface_producer.rt.hlsl)
set(PATH_TRACING_RESTIR_INDIRECT_INITIAL_PRODUCER_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/pathtrace_restir_indirect_initial_producer.rt.hlsl)
set(PATH_TRACING_RESTIR_DIRECT_TEMPORAL_PRODUCER_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/pathtrace_restir_direct_temporal_producer.rt.hlsl)
set(PATH_TRACING_RESTIR_DIRECT_SPATIAL_RESERVOIR_PRODUCER_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/pathtrace_restir_direct_spatial_reservoir_producer.rt.hlsl)
set(PATH_TRACING_RESTIR_REFLECTION_PRODUCER_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/pathtrace_restir_reflection_producer.rt.hlsl)
set(PATH_TRACING_RESTIR_COMBINED_RESOLVE_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/pathtrace_restir_combined_resolve.rt.hlsl)
set(PATH_TRACING_RESTIR_PDF_NEE_RLU_CURRENT_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/pathtrace_restir_pdf_nee_rlu_current.rt.hlsl)
set(PATH_TRACING_PDF_NEE_VERIFIER_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/pathtrace_pdf_nee_verifier.rt.hlsl)
set(PATH_TRACING_REGIR_DEBUG_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/pathtrace_regir_debug.rt.hlsl)
set(PATH_TRACING_NEE_CACHE_DEBUG_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/pathtrace_nee_cache_debug.rt.hlsl)
set(PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_sentinel.rt.hlsl)
set(PATH_TRACING_CLEAN_RTXDI_DI_INITIAL_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_initial.rt.hlsl)
set(PATH_TRACING_CLEAN_RTXDI_DI_TEMPORAL_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_temporal.rt.hlsl)
set(PATH_TRACING_CLEAN_RTXDI_DI_SPATIAL_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_spatial.rt.hlsl)
set(PATH_TRACING_CLEAN_RTXDI_DI_TRANSMISSION_PRODUCER_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_transmission_producer.rt.hlsl)
set(PATH_TRACING_CLEAN_RESTIR_GI_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/remix_restir_gi/pathtrace_clean_restir_gi.rt.hlsl)
set(PATH_TRACING_CLEAN_RESTIR_GI_PRODUCER_RAYQUERY_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/remix_restir_gi/pathtrace_clean_restir_gi_producer_rayquery.cs.hlsl)
set(PATH_TRACING_CLEAN_RESTIR_GI_TEMPORAL_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/remix_restir_gi/pathtrace_clean_restir_gi_temporal.cs.hlsl)
set(PATH_TRACING_CLEAN_RESTIR_GI_BOILING_FILTER_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/remix_restir_gi/pathtrace_clean_restir_gi_boiling_filter.cs.hlsl)
file(GLOB_RECURSE PATH_TRACING_SHADER_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/*.hlsli")
file(GLOB_RECURSE PATH_TRACING_CLEAN_RTXDI_DI_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_rtxdi/*.hlsli")
set(PATH_TRACING_RANDOM_SAMPLER_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/Rtxdi/Utils/RandomSamplerState.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_common/restir_random_sampler_state.hlsli)
set(PATH_TRACING_SHARED_RTXDI_UTILITY_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/Rtxdi/RtxdiParameters.h
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/Rtxdi/DI/Reservoir.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/Rtxdi/DI/ReservoirStorage.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/Rtxdi/DI/ReSTIRDIParameters.h
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/Rtxdi/DI/InitialSampling.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/Rtxdi/DI/TemporalResampling.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/Rtxdi/DI/SpatialResampling.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/Rtxdi/Utils/Color.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/Rtxdi/Utils/SampledLightData.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/Rtxdi/Utils/BrdfRaySample.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/Rtxdi/Utils/ReservoirAddressing.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/Rtxdi/Utils/Math.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_common/restir_shared_parameters.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_common/restir_shared_color.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_common/restir_shared_sampled_light_data.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_common/restir_shared_brdf_ray_sample.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_common/restir_di_parameters.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_common/restir_di_reservoir.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_common/restir_di_reservoir_storage.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_common/restir_di_initial_sampling.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_common/restir_di_temporal_resampling.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_common/restir_di_spatial_resampling.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_common/restir_shared_spatial_neighbors.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_common/restir_shared_reservoir_addressing.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_common/restir_shared_math.hlsli)
set(PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/PathTracePrimarySurface.hlsli
    ${PATH_TRACING_RANDOM_SAMPLER_INCLUDES}
    ${PATH_TRACING_SHARED_RTXDI_UTILITY_INCLUDES}
    ${PATH_TRACING_CLEAN_RTXDI_DI_INCLUDES})
file(GLOB_RECURSE PATH_TRACING_CLEAN_RESTIR_GI_LOCAL_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/remix_restir_gi/*.hlsli"
    "${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/remix_bridge/*.hlsli")
set(PATH_TRACING_CLEAN_RESTIR_GI_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/PathTracePrimarySurface.hlsli
    ${PATH_TRACING_RANDOM_SAMPLER_INCLUDES}
    ${PATH_TRACING_SHARED_RTXDI_UTILITY_INCLUDES}
    ${PATH_TRACING_CLEAN_RTXDI_DI_INCLUDES}
    ${PATH_TRACING_CLEAN_RESTIR_GI_LOCAL_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/Rtxdi/GI/Reservoir.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/Rtxdi/GI/ReSTIRGIParameters.h
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_common/pathtrace_first_indirect_candidate.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_common/restir_gi_reservoir.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_common/restir_gi_reservoir_storage.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_common/restir_gi_parameters.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/Rtxdi/GI/TemporalResampling.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_common/restir_gi_temporal_resampling.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/Rtxdi/GI/SpatialResampling.hlsli
    ${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing/cleanroom_common/restir_gi_spatial_resampling.hlsli)
option(PATH_TRACING_BUILD_RESTIR_LEGACY_DIAGNOSTICS "Build legacy monolithic ReSTIR PT diagnostic RT shader libraries that are not used by the mode 56 primary-prepass path." OFF)
option(PATH_TRACING_BUILD_DXIL_RT_SHADERS "Build DirectX DXIL path tracing RT shader libraries. Disable for Vulkan-only PT debug iteration." ON)
set(PATH_TRACING_SHADER_OUTPUT_BASE ${CMAKE_CURRENT_SOURCE_DIR}/../../base/renderprogs2)
set(PATH_TRACING_SHADER_OUTPUTS)
set(PATH_TRACING_RTXDI_INCLUDE_ARGS)
list(APPEND PATH_TRACING_RTXDI_INCLUDE_ARGS -I "${CMAKE_CURRENT_SOURCE_DIR}/builtin/pathtracing")

function(path_tracing_declare_spirv_rt_library)
    set(options)
    set(oneValueArgs OUTPUT SOURCE OUTPUT_DIR COMMENT)
    set(multiValueArgs DEPENDS DEFINES)
    cmake_parse_arguments(PT_RT "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT PT_RT_OUTPUT OR NOT PT_RT_SOURCE OR NOT PT_RT_OUTPUT_DIR OR NOT PT_RT_COMMENT)
        message(FATAL_ERROR "path_tracing_declare_spirv_rt_library requires OUTPUT, SOURCE, OUTPUT_DIR, and COMMENT")
    endif()

    add_custom_command(
        OUTPUT ${PT_RT_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PT_RT_OUTPUT_DIR}
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PT_RT_DEFINES} ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PT_RT_OUTPUT} ${PT_RT_SOURCE}
        DEPENDS ${PT_RT_SOURCE} ${PT_RT_DEPENDS}
        COMMENT ${PT_RT_COMMENT}
        VERBATIM)
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PT_RT_OUTPUT})
    set(PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_SHADER_OUTPUTS} PARENT_SCOPE)
endfunction()

if(USE_DX12 AND USE_DXIL_ON_DX12 AND PATH_TRACING_BUILD_DXIL_RT_SHADERS)
    set(PATH_TRACING_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/pathtrace_smoke.rt.bin)
    set(PATH_TRACING_MODE18_RESTIR_HYBRID_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/pathtrace_smoke_mode18_restir_hybrid.rt.bin)
    set(PATH_TRACING_RESTIR_INITIAL_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/pathtrace_smoke_restir_initial.rt.bin)
    set(PATH_TRACING_RESTIR_TEMPORAL_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/pathtrace_smoke_restir_temporal.rt.bin)
    set(PATH_TRACING_RESTIR_TEMPORAL_SHADING_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/pathtrace_smoke_restir_temporal_shading.rt.bin)
    set(PATH_TRACING_RESTIR_ATTRIBUTION_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/pathtrace_smoke_restir_attribution.rt.bin)
    set(PATH_TRACING_RESTIR_SPATIAL_RESERVOIR_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/pathtrace_smoke_restir_spatial_reservoir.rt.bin)
    set(PATH_TRACING_RESTIR_SPATIAL_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/pathtrace_smoke_restir_spatial.rt.bin)
    set(PATH_TRACING_RESTIR_SPATIAL_ATTRIBUTION_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/pathtrace_smoke_restir_spatial_attribution.rt.bin)
    set(PATH_TRACING_RESTIR_COMBINED_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/pathtrace_smoke_restir_combined.rt.bin)
    set(PATH_TRACING_PRIMARY_SURFACE_PRODUCER_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/pathtrace_primary_surface_producer.rt.bin)
    set(PATH_TRACING_RESTIR_INDIRECT_INITIAL_PRODUCER_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/pathtrace_restir_indirect_initial_producer.rt.bin)
    set(PATH_TRACING_RESTIR_DIRECT_TEMPORAL_PRODUCER_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/pathtrace_restir_direct_temporal_producer.rt.bin)
    set(PATH_TRACING_RESTIR_DIRECT_SPATIAL_RESERVOIR_PRODUCER_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/pathtrace_restir_direct_spatial_reservoir_producer.rt.bin)
    set(PATH_TRACING_RESTIR_REFLECTION_PRODUCER_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/pathtrace_restir_reflection_producer.rt.bin)
    set(PATH_TRACING_RESTIR_COMBINED_RESOLVE_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/pathtrace_restir_combined_resolve.rt.bin)
    set(PATH_TRACING_RESTIR_PDF_NEE_RLU_CURRENT_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/pathtrace_restir_pdf_nee_rlu_current.rt.bin)
    set(PATH_TRACING_PDF_NEE_VERIFIER_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/pathtrace_pdf_nee_verifier.rt.bin)
    set(PATH_TRACING_REGIR_DEBUG_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/pathtrace_regir_debug.rt.bin)
    set(PATH_TRACING_NEE_CACHE_DEBUG_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/pathtrace_nee_cache_debug.rt.bin)
    set(PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_sentinel.rt.bin)
    set(PATH_TRACING_CLEAN_RTXDI_DI_INITIAL_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_initial.rt.bin)
    set(PATH_TRACING_CLEAN_RTXDI_DI_TEMPORAL_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_temporal.rt.bin)
    set(PATH_TRACING_CLEAN_RTXDI_DI_SPATIAL_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_spatial.rt.bin)
    set(PATH_TRACING_CLEAN_RTXDI_DI_TRANSMISSION_PRODUCER_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_transmission_producer.rt.bin)
    set(PATH_TRACING_CLEAN_RESTIR_GI_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/remix_restir_gi/pathtrace_clean_restir_gi.rt.bin)
    set(PATH_TRACING_CLEAN_RESTIR_GI_PRODUCER_RAYQUERY_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/remix_restir_gi/pathtrace_clean_restir_gi_producer_rayquery.cs.bin)
    set(PATH_TRACING_CLEAN_RESTIR_GI_TEMPORAL_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/remix_restir_gi/pathtrace_clean_restir_gi_temporal.cs.bin)
    set(PATH_TRACING_CLEAN_RESTIR_GI_BOILING_FILTER_DXIL_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/remix_restir_gi/pathtrace_clean_restir_gi_boiling_filter.cs.bin)
    add_custom_command(
        OUTPUT ${PATH_TRACING_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_DXIL_OUTPUT} ${PATH_TRACING_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing smoke core RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_MODE18_RESTIR_HYBRID_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_MODE18_RESTIR_HYBRID_DXIL_OUTPUT} ${PATH_TRACING_MODE18_RESTIR_HYBRID_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_MODE18_RESTIR_HYBRID_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing smoke mode18 ReSTIR hybrid RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_INITIAL_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_INITIAL_DXIL_OUTPUT} ${PATH_TRACING_RESTIR_INITIAL_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_INITIAL_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing smoke ReSTIR initial RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_TEMPORAL_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_TEMPORAL_DXIL_OUTPUT} ${PATH_TRACING_RESTIR_TEMPORAL_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_TEMPORAL_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing smoke ReSTIR temporal RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_TEMPORAL_SHADING_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_TEMPORAL_SHADING_DXIL_OUTPUT} ${PATH_TRACING_RESTIR_TEMPORAL_SHADING_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_TEMPORAL_SHADING_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing smoke ReSTIR temporal shading RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_ATTRIBUTION_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_ATTRIBUTION_DXIL_OUTPUT} ${PATH_TRACING_RESTIR_ATTRIBUTION_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_ATTRIBUTION_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing smoke ReSTIR attribution RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_SPATIAL_RESERVOIR_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_SPATIAL_RESERVOIR_DXIL_OUTPUT} ${PATH_TRACING_RESTIR_SPATIAL_RESERVOIR_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_SPATIAL_RESERVOIR_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing smoke ReSTIR spatial reservoir RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_SPATIAL_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_SPATIAL_DXIL_OUTPUT} ${PATH_TRACING_RESTIR_SPATIAL_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_SPATIAL_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing smoke ReSTIR spatial RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_SPATIAL_ATTRIBUTION_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_SPATIAL_ATTRIBUTION_DXIL_OUTPUT} ${PATH_TRACING_RESTIR_SPATIAL_ATTRIBUTION_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_SPATIAL_ATTRIBUTION_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing smoke ReSTIR spatial attribution RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_COMBINED_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_COMBINED_DXIL_OUTPUT} ${PATH_TRACING_RESTIR_COMBINED_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_COMBINED_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing smoke ReSTIR combined direct+GI RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_PRIMARY_SURFACE_PRODUCER_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_PRIMARY_SURFACE_PRODUCER_DXIL_OUTPUT} ${PATH_TRACING_PRIMARY_SURFACE_PRODUCER_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_PRIMARY_SURFACE_PRODUCER_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing primary-surface producer RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_INDIRECT_INITIAL_PRODUCER_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_INDIRECT_INITIAL_PRODUCER_DXIL_OUTPUT} ${PATH_TRACING_RESTIR_INDIRECT_INITIAL_PRODUCER_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_INDIRECT_INITIAL_PRODUCER_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing ReSTIR indirect initial producer RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_DIRECT_TEMPORAL_PRODUCER_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_DIRECT_TEMPORAL_PRODUCER_DXIL_OUTPUT} ${PATH_TRACING_RESTIR_DIRECT_TEMPORAL_PRODUCER_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_DIRECT_TEMPORAL_PRODUCER_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing ReSTIR direct temporal producer RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_DIRECT_SPATIAL_RESERVOIR_PRODUCER_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_DIRECT_SPATIAL_RESERVOIR_PRODUCER_DXIL_OUTPUT} ${PATH_TRACING_RESTIR_DIRECT_SPATIAL_RESERVOIR_PRODUCER_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_DIRECT_SPATIAL_RESERVOIR_PRODUCER_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing ReSTIR direct spatial-reservoir producer RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_REFLECTION_PRODUCER_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_REFLECTION_PRODUCER_DXIL_OUTPUT} ${PATH_TRACING_RESTIR_REFLECTION_PRODUCER_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_REFLECTION_PRODUCER_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing ReSTIR reflection producer RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_CLEAN_RTXDI_DI_TRANSMISSION_PRODUCER_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/cleanroom_rtxdi
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_CLEAN_RTXDI_DI_TRANSMISSION_PRODUCER_DXIL_OUTPUT} ${PATH_TRACING_CLEAN_RTXDI_DI_TRANSMISSION_PRODUCER_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_CLEAN_RTXDI_DI_TRANSMISSION_PRODUCER_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_INCLUDES}
        COMMENT "Compiling clean-room RTXDI DI transmission producer RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_COMBINED_RESOLVE_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_COMBINED_RESOLVE_DXIL_OUTPUT} ${PATH_TRACING_RESTIR_COMBINED_RESOLVE_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_COMBINED_RESOLVE_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing ReSTIR combined resolve RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_PDF_NEE_RLU_CURRENT_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_PDF_NEE_RLU_CURRENT_DXIL_OUTPUT} ${PATH_TRACING_RESTIR_PDF_NEE_RLU_CURRENT_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_PDF_NEE_RLU_CURRENT_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing ReSTIR PDF+NEE RLU current producer RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_PDF_NEE_VERIFIER_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_PDF_NEE_VERIFIER_DXIL_OUTPUT} ${PATH_TRACING_PDF_NEE_VERIFIER_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_PDF_NEE_VERIFIER_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing PDF+NEE verifier RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_REGIR_DEBUG_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_REGIR_DEBUG_DXIL_OUTPUT} ${PATH_TRACING_REGIR_DEBUG_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_REGIR_DEBUG_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing ReGIR debug RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_NEE_CACHE_DEBUG_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_NEE_CACHE_DEBUG_DXIL_OUTPUT} ${PATH_TRACING_NEE_CACHE_DEBUG_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_NEE_CACHE_DEBUG_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing NEE cache debug RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/cleanroom_rtxdi
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_DXIL_OUTPUT} ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_INCLUDES}
        COMMENT "Compiling clean-room RTXDI DI sentinel RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_CLEAN_RTXDI_DI_INITIAL_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/cleanroom_rtxdi
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_CLEAN_RTXDI_DI_INITIAL_DXIL_OUTPUT} ${PATH_TRACING_CLEAN_RTXDI_DI_INITIAL_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_CLEAN_RTXDI_DI_INITIAL_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_INCLUDES}
        COMMENT "Compiling clean-room RTXDI DI initial RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_CLEAN_RTXDI_DI_TEMPORAL_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/cleanroom_rtxdi
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_CLEAN_RTXDI_DI_TEMPORAL_DXIL_OUTPUT} ${PATH_TRACING_CLEAN_RTXDI_DI_TEMPORAL_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_CLEAN_RTXDI_DI_TEMPORAL_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_INCLUDES}
        COMMENT "Compiling clean-room RTXDI DI temporal RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_CLEAN_RTXDI_DI_SPATIAL_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/cleanroom_rtxdi
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_CLEAN_RTXDI_DI_SPATIAL_DXIL_OUTPUT} ${PATH_TRACING_CLEAN_RTXDI_DI_SPATIAL_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_CLEAN_RTXDI_DI_SPATIAL_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_INCLUDES}
        COMMENT "Compiling clean-room RTXDI DI spatial RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_CLEAN_RESTIR_GI_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/remix_restir_gi
        COMMAND ${DXC_PATH} -T lib_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_CLEAN_RESTIR_GI_DXIL_OUTPUT} ${PATH_TRACING_CLEAN_RESTIR_GI_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_CLEAN_RESTIR_GI_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RESTIR_GI_INCLUDES}
        COMMENT "Compiling clean-room ReSTIR GI RT shader library (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_CLEAN_RESTIR_GI_PRODUCER_RAYQUERY_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/remix_restir_gi
        COMMAND ${DXC_PATH} -T cs_6_6 -D RBPT_ENABLE_BLUE_NOISE ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_CLEAN_RESTIR_GI_PRODUCER_RAYQUERY_DXIL_OUTPUT} ${PATH_TRACING_CLEAN_RESTIR_GI_PRODUCER_RAYQUERY_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_CLEAN_RESTIR_GI_PRODUCER_RAYQUERY_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RESTIR_GI_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RESTIR_GI_INCLUDES}
        COMMENT "Compiling clean-room ReSTIR GI producer ray-query CS (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_CLEAN_RESTIR_GI_TEMPORAL_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/remix_restir_gi
        COMMAND ${DXC_PATH} -T cs_6_6 -D RBPT_ENABLE_BLUE_NOISE ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_CLEAN_RESTIR_GI_TEMPORAL_DXIL_OUTPUT} ${PATH_TRACING_CLEAN_RESTIR_GI_TEMPORAL_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_CLEAN_RESTIR_GI_TEMPORAL_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RESTIR_GI_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RESTIR_GI_INCLUDES}
        COMMENT "Compiling clean-room ReSTIR GI temporal CS (DXIL)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_CLEAN_RESTIR_GI_BOILING_FILTER_DXIL_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/dxil/builtin/pathtracing/remix_restir_gi
        COMMAND ${DXC_PATH} -T cs_6_6 ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_CLEAN_RESTIR_GI_BOILING_FILTER_DXIL_OUTPUT} ${PATH_TRACING_CLEAN_RESTIR_GI_BOILING_FILTER_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_CLEAN_RESTIR_GI_BOILING_FILTER_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RESTIR_GI_INCLUDES}
        COMMENT "Compiling clean-room ReSTIR GI boiling filter CS (DXIL)"
        VERBATIM)
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_DXIL_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_PRIMARY_SURFACE_PRODUCER_DXIL_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_INDIRECT_INITIAL_PRODUCER_DXIL_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_DIRECT_TEMPORAL_PRODUCER_DXIL_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_DIRECT_SPATIAL_RESERVOIR_PRODUCER_DXIL_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_REFLECTION_PRODUCER_DXIL_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_CLEAN_RTXDI_DI_TRANSMISSION_PRODUCER_DXIL_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_COMBINED_RESOLVE_DXIL_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_PDF_NEE_RLU_CURRENT_DXIL_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_PDF_NEE_VERIFIER_DXIL_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_REGIR_DEBUG_DXIL_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_NEE_CACHE_DEBUG_DXIL_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_DXIL_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_CLEAN_RTXDI_DI_INITIAL_DXIL_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_CLEAN_RTXDI_DI_TEMPORAL_DXIL_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_CLEAN_RTXDI_DI_SPATIAL_DXIL_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_CLEAN_RESTIR_GI_DXIL_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_CLEAN_RESTIR_GI_PRODUCER_RAYQUERY_DXIL_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_CLEAN_RESTIR_GI_TEMPORAL_DXIL_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_CLEAN_RESTIR_GI_BOILING_FILTER_DXIL_OUTPUT})
    if(PATH_TRACING_BUILD_RESTIR_LEGACY_DIAGNOSTICS)
        list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_MODE18_RESTIR_HYBRID_DXIL_OUTPUT})
        list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_INITIAL_DXIL_OUTPUT})
        list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_TEMPORAL_DXIL_OUTPUT})
        list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_TEMPORAL_SHADING_DXIL_OUTPUT})
        list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_ATTRIBUTION_DXIL_OUTPUT})
        list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_SPATIAL_RESERVOIR_DXIL_OUTPUT})
        list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_SPATIAL_DXIL_OUTPUT})
        list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_SPATIAL_ATTRIBUTION_DXIL_OUTPUT})
        list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_COMBINED_DXIL_OUTPUT})
    endif()
endif()

if(USE_VULKAN)
    set(PATH_TRACING_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/pathtrace_smoke.rt.bin)
    set(PATH_TRACING_MODE18_RESTIR_HYBRID_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/pathtrace_smoke_mode18_restir_hybrid.rt.bin)
    set(PATH_TRACING_RESTIR_INITIAL_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/pathtrace_smoke_restir_initial.rt.bin)
    set(PATH_TRACING_RESTIR_TEMPORAL_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/pathtrace_smoke_restir_temporal.rt.bin)
    set(PATH_TRACING_RESTIR_TEMPORAL_SHADING_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/pathtrace_smoke_restir_temporal_shading.rt.bin)
    set(PATH_TRACING_RESTIR_ATTRIBUTION_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/pathtrace_smoke_restir_attribution.rt.bin)
    set(PATH_TRACING_RESTIR_SPATIAL_RESERVOIR_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/pathtrace_smoke_restir_spatial_reservoir.rt.bin)
    set(PATH_TRACING_RESTIR_SPATIAL_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/pathtrace_smoke_restir_spatial.rt.bin)
    set(PATH_TRACING_RESTIR_SPATIAL_ATTRIBUTION_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/pathtrace_smoke_restir_spatial_attribution.rt.bin)
    set(PATH_TRACING_RESTIR_COMBINED_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/pathtrace_smoke_restir_combined.rt.bin)
    set(PATH_TRACING_PRIMARY_SURFACE_PRODUCER_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/pathtrace_primary_surface_producer.rt.bin)
    set(PATH_TRACING_RESTIR_INDIRECT_INITIAL_PRODUCER_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/pathtrace_restir_indirect_initial_producer.rt.bin)
    set(PATH_TRACING_RESTIR_DIRECT_TEMPORAL_PRODUCER_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/pathtrace_restir_direct_temporal_producer.rt.bin)
    set(PATH_TRACING_RESTIR_DIRECT_SPATIAL_RESERVOIR_PRODUCER_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/pathtrace_restir_direct_spatial_reservoir_producer.rt.bin)
    set(PATH_TRACING_RESTIR_REFLECTION_PRODUCER_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/pathtrace_restir_reflection_producer.rt.bin)
    set(PATH_TRACING_RESTIR_COMBINED_RESOLVE_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/pathtrace_restir_combined_resolve.rt.bin)
    set(PATH_TRACING_RESTIR_PDF_NEE_RLU_CURRENT_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/pathtrace_restir_pdf_nee_rlu_current.rt.bin)
    set(PATH_TRACING_PDF_NEE_VERIFIER_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/pathtrace_pdf_nee_verifier.rt.bin)
    set(PATH_TRACING_REGIR_DEBUG_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/pathtrace_regir_debug.rt.bin)
    set(PATH_TRACING_NEE_CACHE_DEBUG_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/pathtrace_nee_cache_debug.rt.bin)
    set(PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_sentinel.rt.bin)
    set(PATH_TRACING_CLEAN_RTXDI_DI_INITIAL_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_initial.rt.bin)
    set(PATH_TRACING_CLEAN_RTXDI_DI_TEMPORAL_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_temporal.rt.bin)
    set(PATH_TRACING_CLEAN_RTXDI_DI_SPATIAL_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_spatial.rt.bin)
    set(PATH_TRACING_CLEAN_RESTIR_GI_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/remix_restir_gi/pathtrace_clean_restir_gi.rt.bin)
    set(PATH_TRACING_CLEAN_RESTIR_GI_PRODUCER_RAYQUERY_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/remix_restir_gi/pathtrace_clean_restir_gi_producer_rayquery.cs.bin)
    set(PATH_TRACING_CLEAN_RESTIR_GI_TEMPORAL_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/remix_restir_gi/pathtrace_clean_restir_gi_temporal.cs.bin)
    set(PATH_TRACING_CLEAN_RESTIR_GI_BOILING_FILTER_SPIRV_OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/remix_restir_gi/pathtrace_clean_restir_gi_boiling_filter.cs.bin)
    add_custom_command(
        OUTPUT ${PATH_TRACING_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_SPIRV_OUTPUT} ${PATH_TRACING_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing smoke core RT shader library (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_MODE18_RESTIR_HYBRID_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_MODE18_RESTIR_HYBRID_SPIRV_OUTPUT} ${PATH_TRACING_MODE18_RESTIR_HYBRID_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_MODE18_RESTIR_HYBRID_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing smoke mode18 ReSTIR hybrid RT shader library (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_INITIAL_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_INITIAL_SPIRV_OUTPUT} ${PATH_TRACING_RESTIR_INITIAL_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_INITIAL_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing smoke ReSTIR initial RT shader library (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_TEMPORAL_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_TEMPORAL_SPIRV_OUTPUT} ${PATH_TRACING_RESTIR_TEMPORAL_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_TEMPORAL_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing smoke ReSTIR temporal RT shader library (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_TEMPORAL_SHADING_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_TEMPORAL_SHADING_SPIRV_OUTPUT} ${PATH_TRACING_RESTIR_TEMPORAL_SHADING_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_TEMPORAL_SHADING_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing smoke ReSTIR temporal shading RT shader library (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_ATTRIBUTION_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_ATTRIBUTION_SPIRV_OUTPUT} ${PATH_TRACING_RESTIR_ATTRIBUTION_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_ATTRIBUTION_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing smoke ReSTIR attribution RT shader library (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_SPATIAL_RESERVOIR_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_SPATIAL_RESERVOIR_SPIRV_OUTPUT} ${PATH_TRACING_RESTIR_SPATIAL_RESERVOIR_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_SPATIAL_RESERVOIR_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing smoke ReSTIR spatial reservoir RT shader library (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_SPATIAL_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_SPATIAL_SPIRV_OUTPUT} ${PATH_TRACING_RESTIR_SPATIAL_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_SPATIAL_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing smoke ReSTIR spatial RT shader library (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_SPATIAL_ATTRIBUTION_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_SPATIAL_ATTRIBUTION_SPIRV_OUTPUT} ${PATH_TRACING_RESTIR_SPATIAL_ATTRIBUTION_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_SPATIAL_ATTRIBUTION_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing smoke ReSTIR spatial attribution RT shader library (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_COMBINED_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_COMBINED_SPIRV_OUTPUT} ${PATH_TRACING_RESTIR_COMBINED_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_COMBINED_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing smoke ReSTIR combined direct+GI RT shader library (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_PRIMARY_SURFACE_PRODUCER_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_PRIMARY_SURFACE_PRODUCER_SPIRV_OUTPUT} ${PATH_TRACING_PRIMARY_SURFACE_PRODUCER_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_PRIMARY_SURFACE_PRODUCER_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing primary-surface producer RT shader library (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_INDIRECT_INITIAL_PRODUCER_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_INDIRECT_INITIAL_PRODUCER_SPIRV_OUTPUT} ${PATH_TRACING_RESTIR_INDIRECT_INITIAL_PRODUCER_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_INDIRECT_INITIAL_PRODUCER_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing ReSTIR indirect initial producer RT shader library (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_DIRECT_TEMPORAL_PRODUCER_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_DIRECT_TEMPORAL_PRODUCER_SPIRV_OUTPUT} ${PATH_TRACING_RESTIR_DIRECT_TEMPORAL_PRODUCER_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_DIRECT_TEMPORAL_PRODUCER_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing ReSTIR direct temporal producer RT shader library (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_DIRECT_SPATIAL_RESERVOIR_PRODUCER_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_DIRECT_SPATIAL_RESERVOIR_PRODUCER_SPIRV_OUTPUT} ${PATH_TRACING_RESTIR_DIRECT_SPATIAL_RESERVOIR_PRODUCER_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_DIRECT_SPATIAL_RESERVOIR_PRODUCER_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing ReSTIR direct spatial-reservoir producer RT shader library (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_REFLECTION_PRODUCER_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_REFLECTION_PRODUCER_SPIRV_OUTPUT} ${PATH_TRACING_RESTIR_REFLECTION_PRODUCER_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_REFLECTION_PRODUCER_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing ReSTIR reflection producer RT shader library (SPIR-V)"
        VERBATIM)
    path_tracing_declare_spirv_rt_library(
        OUTPUT ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_transmission_producer.rt.bin
        SOURCE ${PATH_TRACING_CLEAN_RTXDI_DI_TRANSMISSION_PRODUCER_SHADER_SOURCE}
        OUTPUT_DIR ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/cleanroom_rtxdi
        DEPENDS ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_INCLUDES}
        COMMENT "Compiling clean-room RTXDI DI transmission producer RT shader library (SPIR-V)")
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_COMBINED_RESOLVE_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_COMBINED_RESOLVE_SPIRV_OUTPUT} ${PATH_TRACING_RESTIR_COMBINED_RESOLVE_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_COMBINED_RESOLVE_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing ReSTIR combined resolve RT shader library (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_RESTIR_PDF_NEE_RLU_CURRENT_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_RESTIR_PDF_NEE_RLU_CURRENT_SPIRV_OUTPUT} ${PATH_TRACING_RESTIR_PDF_NEE_RLU_CURRENT_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_RESTIR_PDF_NEE_RLU_CURRENT_SHADER_SOURCE} ${PATH_TRACING_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing ReSTIR PDF+NEE RLU current producer RT shader library (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_PDF_NEE_VERIFIER_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_PDF_NEE_VERIFIER_SPIRV_OUTPUT} ${PATH_TRACING_PDF_NEE_VERIFIER_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_PDF_NEE_VERIFIER_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing PDF+NEE verifier RT shader library (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_REGIR_DEBUG_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_REGIR_DEBUG_SPIRV_OUTPUT} ${PATH_TRACING_REGIR_DEBUG_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_REGIR_DEBUG_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing ReGIR debug RT shader library (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_NEE_CACHE_DEBUG_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_NEE_CACHE_DEBUG_SPIRV_OUTPUT} ${PATH_TRACING_NEE_CACHE_DEBUG_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_NEE_CACHE_DEBUG_SHADER_SOURCE} ${PATH_TRACING_SHADER_INCLUDES}
        COMMENT "Compiling path tracing NEE cache debug RT shader library (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/cleanroom_rtxdi
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_SPIRV_OUTPUT} ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_INCLUDES}
        COMMENT "Compiling clean-room RTXDI DI sentinel RT shader library (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_CLEAN_RTXDI_DI_INITIAL_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/cleanroom_rtxdi
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_CLEAN_RTXDI_DI_INITIAL_SPIRV_OUTPUT} ${PATH_TRACING_CLEAN_RTXDI_DI_INITIAL_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_CLEAN_RTXDI_DI_INITIAL_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_INCLUDES}
        COMMENT "Compiling clean-room RTXDI DI initial RT shader library (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_CLEAN_RTXDI_DI_TEMPORAL_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/cleanroom_rtxdi
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_CLEAN_RTXDI_DI_TEMPORAL_SPIRV_OUTPUT} ${PATH_TRACING_CLEAN_RTXDI_DI_TEMPORAL_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_CLEAN_RTXDI_DI_TEMPORAL_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_INCLUDES}
        COMMENT "Compiling clean-room RTXDI DI temporal RT shader library (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_CLEAN_RTXDI_DI_SPATIAL_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/cleanroom_rtxdi
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_CLEAN_RTXDI_DI_SPATIAL_SPIRV_OUTPUT} ${PATH_TRACING_CLEAN_RTXDI_DI_SPATIAL_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_CLEAN_RTXDI_DI_SPATIAL_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_INCLUDES}
        COMMENT "Compiling clean-room RTXDI DI spatial RT shader library (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_CLEAN_RESTIR_GI_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/remix_restir_gi
        COMMAND ${DXC_SPIRV_PATH} -T lib_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV -D RBPT_ENABLE_BLUE_NOISE ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_CLEAN_RESTIR_GI_SPIRV_OUTPUT} ${PATH_TRACING_CLEAN_RESTIR_GI_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_CLEAN_RESTIR_GI_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RESTIR_GI_INCLUDES}
        COMMENT "Compiling clean-room ReSTIR GI RT shader library (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_CLEAN_RESTIR_GI_PRODUCER_RAYQUERY_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/remix_restir_gi
        COMMAND ${DXC_SPIRV_PATH} -T cs_6_6 -spirv -fspv-target-env=vulkan1.2 -fspv-extension=SPV_KHR_ray_query -fspv-extension=SPV_EXT_descriptor_indexing -D SPIRV -D RBPT_ENABLE_BLUE_NOISE ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_CLEAN_RESTIR_GI_PRODUCER_RAYQUERY_SPIRV_OUTPUT} ${PATH_TRACING_CLEAN_RESTIR_GI_PRODUCER_RAYQUERY_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_CLEAN_RESTIR_GI_PRODUCER_RAYQUERY_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RESTIR_GI_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RESTIR_GI_INCLUDES}
        COMMENT "Compiling clean-room ReSTIR GI producer ray-query CS (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_CLEAN_RESTIR_GI_TEMPORAL_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/remix_restir_gi
        COMMAND ${DXC_SPIRV_PATH} -T cs_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV -D RBPT_ENABLE_BLUE_NOISE ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_CLEAN_RESTIR_GI_TEMPORAL_SPIRV_OUTPUT} ${PATH_TRACING_CLEAN_RESTIR_GI_TEMPORAL_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_CLEAN_RESTIR_GI_TEMPORAL_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RESTIR_GI_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RESTIR_GI_INCLUDES}
        COMMENT "Compiling clean-room ReSTIR GI temporal CS (SPIR-V)"
        VERBATIM)
    add_custom_command(
        OUTPUT ${PATH_TRACING_CLEAN_RESTIR_GI_BOILING_FILTER_SPIRV_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PATH_TRACING_SHADER_OUTPUT_BASE}/spirv/builtin/pathtracing/remix_restir_gi
        COMMAND ${DXC_SPIRV_PATH} -T cs_6_6 -spirv -fspv-target-env=vulkan1.2 -D SPIRV ${PATH_TRACING_RTXDI_INCLUDE_ARGS} -Fo ${PATH_TRACING_CLEAN_RESTIR_GI_BOILING_FILTER_SPIRV_OUTPUT} ${PATH_TRACING_CLEAN_RESTIR_GI_BOILING_FILTER_SHADER_SOURCE}
        DEPENDS ${PATH_TRACING_CLEAN_RESTIR_GI_BOILING_FILTER_SHADER_SOURCE} ${PATH_TRACING_CLEAN_RESTIR_GI_INCLUDES}
        COMMENT "Compiling clean-room ReSTIR GI boiling filter CS (SPIR-V)"
        VERBATIM)
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_SPIRV_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_PRIMARY_SURFACE_PRODUCER_SPIRV_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_INDIRECT_INITIAL_PRODUCER_SPIRV_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_DIRECT_TEMPORAL_PRODUCER_SPIRV_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_DIRECT_SPATIAL_RESERVOIR_PRODUCER_SPIRV_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_REFLECTION_PRODUCER_SPIRV_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_COMBINED_RESOLVE_SPIRV_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_PDF_NEE_RLU_CURRENT_SPIRV_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_PDF_NEE_VERIFIER_SPIRV_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_REGIR_DEBUG_SPIRV_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_NEE_CACHE_DEBUG_SPIRV_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_CLEAN_RTXDI_DI_SENTINEL_SPIRV_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_CLEAN_RTXDI_DI_INITIAL_SPIRV_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_CLEAN_RTXDI_DI_TEMPORAL_SPIRV_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_CLEAN_RTXDI_DI_SPATIAL_SPIRV_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_CLEAN_RESTIR_GI_SPIRV_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_CLEAN_RESTIR_GI_PRODUCER_RAYQUERY_SPIRV_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_CLEAN_RESTIR_GI_TEMPORAL_SPIRV_OUTPUT})
    list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_CLEAN_RESTIR_GI_BOILING_FILTER_SPIRV_OUTPUT})
    if(PATH_TRACING_BUILD_RESTIR_LEGACY_DIAGNOSTICS)
        list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_MODE18_RESTIR_HYBRID_SPIRV_OUTPUT})
        list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_INITIAL_SPIRV_OUTPUT})
        list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_TEMPORAL_SPIRV_OUTPUT})
        list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_TEMPORAL_SHADING_SPIRV_OUTPUT})
        list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_ATTRIBUTION_SPIRV_OUTPUT})
        list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_SPATIAL_RESERVOIR_SPIRV_OUTPUT})
        list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_SPATIAL_SPIRV_OUTPUT})
        list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_SPATIAL_ATTRIBUTION_SPIRV_OUTPUT})
        list(APPEND PATH_TRACING_SHADER_OUTPUTS ${PATH_TRACING_RESTIR_COMBINED_SPIRV_OUTPUT})
    endif()
endif()

if(PATH_TRACING_SHADER_OUTPUTS)
    add_custom_target(PathTracingShaders DEPENDS ${PATH_TRACING_SHADER_OUTPUTS})
    add_dependencies(Shaders PathTracingShaders)
endif()
