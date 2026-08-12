if(NOT DEFINED UPT43_CLASSIFY_ASM OR
   NOT DEFINED UPT43_COMPACT_ASM OR
   NOT DEFINED UPT43_THREE_CLASSIFY_ASM OR
   NOT DEFINED UPT43_THREE_COMPACT_ASM OR
   NOT DEFINED UPT43_CLASSIFY_REFLECTION OR
   NOT DEFINED UPT43_COMPACT_REFLECTION OR
   NOT DEFINED UPT43_TEMPORAL_SOURCE OR
   NOT DEFINED UPT43_HOST_SOURCE OR
   NOT DEFINED UPT43_STAMP)
    message(FATAL_ERROR "UPT-43.4 replay compaction verification paths are required")
endif()

function(upt43_require_rayquery_count path expected label)
    file(READ "${path}" text)
    string(REGEX MATCHALL "OpRayQueryInitializeKHR" sites "${text}")
    list(LENGTH sites count)
    if(NOT count EQUAL expected OR text MATCHES "OpTraceRay")
        message(FATAL_ERROR
            "UPT-43.4 ${label} expected ${expected} RayQuery sites and no TraceRay; found ${count}")
    endif()
endfunction()

upt43_require_rayquery_count("${UPT43_CLASSIFY_ASM}" 2 "classifier")
upt43_require_rayquery_count("${UPT43_COMPACT_ASM}" 4 "compact consumer")
upt43_require_rayquery_count("${UPT43_THREE_CLASSIFY_ASM}" 2 "three-vertex classifier")
upt43_require_rayquery_count("${UPT43_THREE_COMPACT_ASM}" 6 "three-vertex compact consumer")

file(READ "${UPT43_CLASSIFY_REFLECTION}" classify_reflection)
file(READ "${UPT43_COMPACT_REFLECTION}" compact_reflection)
foreach(binding 33 34)
    if(NOT classify_reflection MATCHES "\"binding\"[ \t]*:[ \t]*${binding}" OR
       NOT compact_reflection MATCHES "\"binding\"[ \t]*:[ \t]*${binding}")
        message(FATAL_ERROR
            "UPT-43.4 classifier/compact reflection lost queue binding ${binding}")
    endif()
endforeach()
if(NOT classify_reflection MATCHES "\"binding\"[ \t]*:[ \t]*35")
    message(FATAL_ERROR
        "UPT-43.4 classifier reflection lost indirect-argument UAV binding 35")
endif()

file(READ "${UPT43_TEMPORAL_SOURCE}" temporal_source)
file(READ "${UPT43_HOST_SOURCE}" host_source)
if(NOT temporal_source MATCHES "failureReason = kUpt07NeedsCompactReplay" OR
   NOT host_source MATCHES "resetBytes=20" OR
   host_source MATCHES "clearBufferUInt[^;]*m_temporalReplayQueue")
    message(FATAL_ERROR
        "UPT-43.4 lost the classify boundary or no-full-clear host contract")
endif()

file(WRITE "${UPT43_STAMP}"
    "UPT-43.4 replay compaction verified: classifier/compact RayQuery=2/4, threeVertex=2/6, queue=uint, bindings=33/34/35, fullQueueClear=0\n")
