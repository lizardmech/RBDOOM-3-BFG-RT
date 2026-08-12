foreach(required_var
        UPT436_ASM
        UPT436_REFLECTION
        UPT436_SHADER_SOURCE
        UPT436_HOST_SOURCE)
    if(NOT DEFINED ${required_var} OR NOT EXISTS "${${required_var}}")
        message(FATAL_ERROR "UPT-43.6 missing ${required_var}: '${${required_var}}'")
    endif()
endforeach()
if(NOT DEFINED UPT436_STAMP OR UPT436_STAMP STREQUAL "")
    message(FATAL_ERROR "UPT-43.6 missing UPT436_STAMP")
endif()

file(READ "${UPT436_ASM}" disassembly)
file(READ "${UPT436_REFLECTION}" reflection)
file(READ "${UPT436_SHADER_SOURCE}" shader_source)
file(READ "${UPT436_HOST_SOURCE}" host_source)

if(NOT disassembly MATCHES "OpExecutionMode[^\n\r]*LocalSize 8 8 1")
    message(FATAL_ERROR "UPT-43.6 must use one 8x8 full-screen workgroup")
endif()
if(disassembly MATCHES "OpRayQueryInitializeKHR|OpTraceRay")
    message(FATAL_ERROR "UPT-43.6 must remain ray-free")
endif()
if(NOT reflection MATCHES "\"ssbos\"" OR
   NOT reflection MATCHES "\"binding\"[\n\r\t ]*:[\n\r\t ]*0" OR
   NOT reflection MATCHES "\"array_stride\"[\n\r\t ]*:[\n\r\t ]*64")
    message(FATAL_ERROR "UPT-43.6 lost its sole reservoir UAV binding 0")
endif()

foreach(shader_token
        "asfloat(packed.word11)"
        "10.0f / strength - 9.0f"
        "gUpt43BoilingCounts"
        "gUpt43TemporalReservoirs[index] = (Upt03PackedReservoir)0")
    string(FIND "${shader_source}" "${shader_token}" token_position)
    if(token_position EQUAL -1)
        message(FATAL_ERROR "UPT-43.6 shader contract missing '${shader_token}'")
    endif()
endforeach()
if(shader_source MATCHES "selected[.]target|candidate[.]target")
    message(FATAL_ERROR
        "UPT-43.6 must consume target-scaled UCW word11 directly, not multiply target again")
endif()

foreach(host_token
        "UPT.T0b Temporal Compact Exact Replay"
        "UPT.T0c Temporal Boiling Filter"
        "BufferUavBarrier"
        "UPT43_TEMPORAL_BOILING_PUSH_CONSTANT_BYTES")
    string(FIND "${host_source}" "${host_token}" token_position)
    if(token_position EQUAL -1)
        message(FATAL_ERROR "UPT-43.6 host contract missing '${host_token}'")
    endif()
endforeach()

file(WRITE "${UPT436_STAMP}"
    "UPT-43.6 temporal boiling filter verified: 8x8, binding0 only, RayQuery=0, targetScaledUCW=word11, complete-reservoir rejection, post-T0b barrier.\n")
