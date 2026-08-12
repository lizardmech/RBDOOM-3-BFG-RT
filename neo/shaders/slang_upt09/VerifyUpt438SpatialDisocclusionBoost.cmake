foreach(required UPT438_CLASSIFY_REFLECTION UPT438_CLASSIFY_DISASSEMBLY
        UPT438_COMPACT_REFLECTION UPT438_COMPACT_DISASSEMBLY
        UPT438_SHADER_SOURCE UPT438_HOST_SOURCE UPT438_STAMP)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "UPT-43.8 verification missing ${required}")
    endif()
endforeach()

file(READ "${UPT438_CLASSIFY_REFLECTION}" classify_reflection)
file(READ "${UPT438_CLASSIFY_DISASSEMBLY}" classify_disassembly)
file(READ "${UPT438_COMPACT_REFLECTION}" compact_reflection)
file(READ "${UPT438_COMPACT_DISASSEMBLY}" compact_disassembly)
file(READ "${UPT438_SHADER_SOURCE}" shader_source)
file(READ "${UPT438_HOST_SOURCE}" host_source)

foreach(binding 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 27 28 32 33 34)
    if(NOT classify_reflection MATCHES "\"set\"[ \t]*:[ \t]*0,[ \t\r\n]*\"binding\"[ \t]*:[ \t]*${binding}")
        message(FATAL_ERROR "UPT-43.8 classifier lacks set 0 binding ${binding}")
    endif()
    if(NOT compact_reflection MATCHES "\"set\"[ \t]*:[ \t]*0,[ \t\r\n]*\"binding\"[ \t]*:[ \t]*${binding}")
        message(FATAL_ERROR "UPT-43.8 compact pass lacks set 0 binding ${binding}")
    endif()
endforeach()
if(NOT classify_reflection MATCHES "\"set\"[ \t]*:[ \t]*0,[ \t\r\n]*\"binding\"[ \t]*:[ \t]*35")
    message(FATAL_ERROR "UPT-43.8 classifier lacks indirect dispatch arguments")
endif()
foreach(kind classify compact)
    if(NOT ${kind}_reflection MATCHES "\"set\"[ \t]*:[ \t]*1,[ \t\r\n]*\"binding\"[ \t]*:[ \t]*0")
        message(FATAL_ERROR "UPT-43.8 ${kind} lacks bindless emitter textures")
    endif()
    if(${kind}_disassembly MATCHES "OpTraceRayKHR" OR
       ${kind}_disassembly MATCHES "OpCapability (Int16|Float16)" OR
       ${kind}_disassembly MATCHES "OpType(Int|Float) 16")
        message(FATAL_ERROR "UPT-43.8 ${kind} introduced TraceRay or native16")
    endif()
endforeach()
if(NOT classify_reflection MATCHES "\"workgroup_size\"[ \t\r\n]*:[ \t\r\n]*\[[ \t\r\n]*8[ \t]*,[ \t\r\n]*8" OR
   NOT compact_reflection MATCHES "\"workgroup_size\"[ \t\r\n]*:[ \t\r\n]*\[[ \t\r\n]*64[ \t]*,[ \t\r\n]*1")
    message(FATAL_ERROR "UPT-43.8 lost its 8x8 classifier or 64x1 compact workgroup")
endif()
string(REGEX MATCHALL "OpRayQueryInitializeKHR" classify_ray_queries "${classify_disassembly}")
string(REGEX MATCHALL "OpRayQueryInitializeKHR" compact_ray_queries "${compact_disassembly}")
list(LENGTH classify_ray_queries classify_ray_query_count)
list(LENGTH compact_ray_queries compact_ray_query_count)
if(NOT classify_ray_query_count EQUAL 5 OR NOT compact_ray_query_count EQUAL 4)
    message(FATAL_ERROR "UPT-43.8 RayQuery site ceilings changed")
endif()
foreach(token
        "reconnectionVertexLength == 2u"
        "effectiveM < gUpt09Control.rescueNeighborCount"
        "neighborSampleId == centerSampleId"
        "Upt09SharedWinnerVisibilityPassed"
        "gUpt438BoostQueue[slot] = index"
        "InterlockedMax")
    string(FIND "${shader_source}" "${token}" token_position)
    if(token_position EQUAL -1)
        message(FATAL_ERROR "UPT-43.8 shader lost contract token: ${token}")
    endif()
endforeach()
foreach(token
        "const uint32_t boostArgs[3] = { 0u, 1u, 1u }"
        "m_temporalReplayCapacity"
        "CurrentPage(), nvrhi::ResourceStates::ShaderResource"
        "HistoryPage(), nvrhi::ResourceStates::UnorderedAccess"
        "inputs.commandList->dispatchIndirect(0u)")
    string(FIND "${host_source}" "${token}" token_position)
    if(token_position EQUAL -1)
        message(FATAL_ERROR "UPT-43.8 host lost bounded-dispatch token: ${token}")
    endif()
endforeach()

file(WRITE "${UPT438_STAMP}"
    "UPT-43.8 sparse disocclusion boost verified: exact x2 short-history classifier, bounded reused queue, 64x1 indirect compact dispatch, canonical duplicate exclusion, winner-only visibility, classifier/compact RayQuery sites 5/4, no TraceRay/native16\n")
