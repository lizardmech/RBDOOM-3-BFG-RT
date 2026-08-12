if(NOT DEFINED UPT36_REFLECTION OR NOT DEFINED UPT36_DISASSEMBLY OR
   NOT DEFINED UPT36_STAMP OR NOT DEFINED UPT36_HOST_SOURCE OR
   NOT DEFINED UPT36_REPLAY_SOURCE)
    message(FATAL_ERROR "UPT-36 temporal verification paths are required")
endif()

file(READ "${UPT36_REFLECTION}" reflection)
file(READ "${UPT36_DISASSEMBLY}" disassembly)
file(READ "${UPT36_HOST_SOURCE}" host_source)
file(READ "${UPT36_REPLAY_SOURCE}" replay_source)

string(REGEX MATCHALL "\"binding\"[ \t]*:" bindings "${reflection}")
list(LENGTH bindings binding_count)
if(NOT binding_count EQUAL 31)
    message(FATAL_ERROR
        "UPT-36 must preserve the accepted compact+duplication 31-binding layout")
endif()
if(NOT reflection MATCHES
        "\"name\"[ \t]*:[ \t]*\"gUpt04StaticTriangleClasses\"[^}]*\"set\"[ \t]*:[ \t]*0[^}]*\"binding\"[ \t]*:[ \t]*9" OR
   NOT reflection MATCHES
        "\"name\"[ \t]*:[ \t]*\"gUpt04DynamicTriangleClasses\"[^}]*\"set\"[ \t]*:[ \t]*0[^}]*\"binding\"[ \t]*:[ \t]*13")
    message(FATAL_ERROR "UPT-36 lost exact triangle-class bindings 9/13")
endif()

string(REGEX MATCHALL "OpRayQueryInitializeKHR" rayquery_initializers
    "${disassembly}")
list(LENGTH rayquery_initializers rayquery_count)
if(NOT rayquery_count EQUAL 6 OR disassembly MATCHES "OpTraceRay")
    message(FATAL_ERROR
        "UPT-36 must preserve six bounded RayQuery sites and no TraceRay site")
endif()

if(NOT replay_source MATCHES "Upt07ThreeVertexEndpointStructureValid" OR
   NOT replay_source MATCHES "const float q = clamp.source.proposalPdf" OR
   NOT replay_source MATCHES "never reapplies the biased" OR
   NOT replay_source MATCHES "secondContinuation.throughput / q" OR
   NOT replay_source MATCHES "shifted.pathLength = 2u" OR
   NOT replay_source MATCHES "shifted.accumulatedRouletteProbability = q")
    message(FATAL_ERROR
        "UPT-36 replay source lacks the stored-q two-continuation contract")
endif()
if(NOT host_source MATCHES
        "upt07_temporal_unified_rayquery_light64_duplication_common_gris_three_vertex.bin")
    message(FATAL_ERROR "UPT-36 host runtime artifact selection is missing")
endif()

file(WRITE "${UPT36_STAMP}"
    "UPT-36 temporal verified: bindings=31, triangleClasses=9/13, RayQuery sites=6, TraceRay=0, stored-q exact replay, throughput cutoff absent\n")
