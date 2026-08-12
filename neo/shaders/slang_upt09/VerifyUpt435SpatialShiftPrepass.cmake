foreach(required_var
        UPT435_SHIFT_REFLECTION
        UPT435_SHIFT_DISASSEMBLY
        UPT435_RESAMPLE_REFLECTION
        UPT435_RESAMPLE_DISASSEMBLY
        UPT435_HOST_SOURCE
        UPT435_SHADER_SOURCE)
    if(NOT DEFINED ${required_var} OR NOT EXISTS "${${required_var}}")
        message(FATAL_ERROR "UPT-43.5 missing ${required_var}: '${${required_var}}'")
    endif()
endforeach()
if(NOT DEFINED UPT435_STAMP OR UPT435_STAMP STREQUAL "")
    message(FATAL_ERROR "UPT-43.5 missing UPT435_STAMP")
endif()

file(READ "${UPT435_SHIFT_REFLECTION}" shift_reflection)
file(READ "${UPT435_SHIFT_DISASSEMBLY}" shift_disassembly)
file(READ "${UPT435_RESAMPLE_REFLECTION}" resample_reflection)
file(READ "${UPT435_RESAMPLE_DISASSEMBLY}" resample_disassembly)
file(READ "${UPT435_HOST_SOURCE}" host_source)
file(READ "${UPT435_SHADER_SOURCE}" shader_source)

foreach(reflection_text shift_reflection resample_reflection)
    if(NOT "${${reflection_text}}" MATCHES "gUpt43SpatialShifts")
        message(FATAL_ERROR "UPT-43.5 shift record resource is missing")
    endif()
    if(NOT "${${reflection_text}}" MATCHES
            "\"binding\"[\n\r\t ]*:[\n\r\t ]*30")
        message(FATAL_ERROR "UPT-43.5 shift record binding 30 missing")
    endif()
endforeach()

foreach(disassembly_text shift_disassembly resample_disassembly)
    if(NOT "${${disassembly_text}}" MATCHES
            "OpExecutionMode[^\n\r]*LocalSize 8 8 1")
        message(FATAL_ERROR "UPT-43.5 stages must use full-screen 8x8 dispatch")
    endif()
endforeach()

string(REGEX MATCHALL "OpRayQueryInitializeKHR" shift_ray_queries
    "${shift_disassembly}")
list(LENGTH shift_ray_queries shift_ray_query_count)
string(REGEX MATCHALL "OpRayQueryInitializeKHR" resample_ray_queries
    "${resample_disassembly}")
list(LENGTH resample_ray_queries resample_ray_query_count)
if(NOT shift_ray_query_count EQUAL 1)
    message(FATAL_ERROR
        "UPT-43.5 shift prepass RayQuery site count changed: ${shift_ray_query_count}, expected 1")
endif()
if(NOT resample_ray_query_count EQUAL 6)
    message(FATAL_ERROR
        "UPT-43.5 resample RayQuery site count changed: ${resample_ray_query_count}, expected 6")
endif()

foreach(host_token
        "UPT.S0a Spatial Gaussian Shift Prepass"
        "BufferUavBarrier"
        "UPT43_SPATIAL_SHIFT_STRIDE"
        "StructuredBuffer_UAV(30)")
    string(FIND "${host_source}" "${host_token}" token_position)
    if(token_position EQUAL -1)
        message(FATAL_ERROR "UPT-43.5 host contract missing '${host_token}'")
    endif()
endforeach()
foreach(shader_token
        "UPT43_SPATIAL_SHIFT_PREPASS"
        "UPT43_SPATIAL_RESAMPLE"
        "gUpt43SpatialShifts[index] = float4(0.0f)"
        "Upt43ExecuteSpatialResample")
    string(FIND "${shader_source}" "${shader_token}" token_position)
    if(token_position EQUAL -1)
        message(FATAL_ERROR "UPT-43.5 shader contract missing '${shader_token}'")
    endif()
endforeach()

file(WRITE "${UPT435_STAMP}"
    "UPT-43.5 spatial split verified: binding30, 16-byte overwrite record, 8x8 stages, RayQuery sites shift/resample=1/6.\n")
