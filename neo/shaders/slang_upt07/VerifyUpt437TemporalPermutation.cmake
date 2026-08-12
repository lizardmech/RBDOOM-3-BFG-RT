foreach(required_var
        UPT437_ASM
        UPT437_SHADER_SOURCE
        UPT437_HOST_SOURCE
        UPT437_DISPATCH_SOURCE)
    if(NOT DEFINED ${required_var} OR NOT EXISTS "${${required_var}}")
        message(FATAL_ERROR "UPT-43.7 missing ${required_var}: '${${required_var}}'")
    endif()
endforeach()
if(NOT DEFINED UPT437_STAMP OR UPT437_STAMP STREQUAL "")
    message(FATAL_ERROR "UPT-43.7 missing UPT437_STAMP")
endif()

file(READ "${UPT437_ASM}" disassembly)
file(READ "${UPT437_SHADER_SOURCE}" shader_source)
file(READ "${UPT437_HOST_SOURCE}" host_source)
file(READ "${UPT437_DISPATCH_SOURCE}" dispatch_source)

string(REGEX MATCHALL "OpRayQueryInitializeKHR" ray_queries
    "${disassembly}")
list(LENGTH ray_queries ray_query_count)
if(NOT ray_query_count EQUAL 2 OR disassembly MATCHES "OpTraceRay")
    message(FATAL_ERROR
        "UPT-43.7 classifier must retain two RayQuery sites and no TraceRay; found ${ray_query_count}")
endif()

foreach(shader_token
        "kUpt07TemporalPermutationFlag = 1u << 21u"
        "Upt07JenkinsHash(gUpt07Control.frameSampleIndex)"
        "previousPixel.x ^= 3"
        "previousPixel.y ^= 3"
        "if (probe == 0u"
        "Upt07HistoryProbeOffset(pixel, probe)")
    string(FIND "${shader_source}" "${shader_token}" token_position)
    if(token_position EQUAL -1)
        message(FATAL_ERROR
            "UPT-43.7 shader contract missing '${shader_token}'")
    endif()
endforeach()

foreach(host_token
        "UPT07_GEOMETRY_FLAG_TEMPORAL_PERMUTATION = 1u << 21u"
        "r_pathTracingUnifiedPtTemporalPermutationSampling.GetBool()")
    string(FIND "${host_source}" "${host_token}" token_position)
    if(token_position EQUAL -1)
        message(FATAL_ERROR
            "UPT-43.7 host contract missing '${host_token}'")
    endif()
endforeach()
if(NOT dispatch_source MATCHES
        "firstTapOnly=1 fallbackProbesUnchanged=1 extraRays=0")
    message(FATAL_ERROR "UPT-43.7 runtime diagnostic contract is missing")
endif()

file(WRITE "${UPT437_STAMP}"
    "UPT-43.7 temporal permutation verified: runtime bit21, Jenkins frame hash, shifted 4x4 XOR, first tap only, fallback probes retained, classifier RayQuery=2, TraceRay=0.\n")
