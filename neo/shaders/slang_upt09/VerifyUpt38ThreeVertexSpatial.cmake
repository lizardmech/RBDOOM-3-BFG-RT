foreach(required UPT38_REFLECTION UPT38_DISASSEMBLY UPT38_STAMP
        UPT38_HOST_SOURCE UPT38_SPATIAL_SOURCE UPT38_REPLAY_SOURCE)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR
            "UPT-38 three-vertex spatial verification missing ${required}")
    endif()
endforeach()

file(READ "${UPT38_REFLECTION}" reflection)
file(READ "${UPT38_DISASSEMBLY}" disassembly)
file(READ "${UPT38_HOST_SOURCE}" host_source)
file(READ "${UPT38_SPATIAL_SOURCE}" spatial_source)
file(READ "${UPT38_REPLAY_SOURCE}" replay_source)

string(REGEX MATCHALL "\"binding\"[ \t]*:" bindings "${reflection}")
list(LENGTH bindings binding_count)
if(NOT binding_count EQUAL 26)
    message(FATAL_ERROR
        "UPT-38 must preserve the accepted 26-binding shared spatial ABI")
endif()
if(NOT reflection MATCHES
        "\"name\"[ \t]*:[ \t]*\"gUpt04StaticTriangleClasses\"[^}]*\"binding\"[ \t]*:[ \t]*9" OR
   NOT reflection MATCHES
        "\"name\"[ \t]*:[ \t]*\"gUpt04DynamicTriangleClasses\"[^}]*\"binding\"[ \t]*:[ \t]*13" OR
   NOT reflection MATCHES
        "\"name\"[ \t]*:[ \t]*\"emissiveDistributionCountAndValid\"[^}]*\"offset\"[ \t]*:[ \t]*88" OR
   NOT reflection MATCHES
        "\"name\"[ \t]*:[ \t]*\"emissiveLookupCapacityAndValid\"[^}]*\"offset\"[ \t]*:[ \t]*92")
    message(FATAL_ERROR
        "UPT-38 lost triangle-class bindings 9/13 or the 96-byte replay control ABI")
endif()

string(REGEX MATCHALL "OpRayQueryInitializeKHR" ray_queries
    "${disassembly}")
list(LENGTH ray_queries ray_query_count)
string(REGEX MATCHALL "OpControlBarrier" barriers "${disassembly}")
list(LENGTH barriers barrier_count)
if(NOT ray_query_count EQUAL 6
        OR NOT barrier_count EQUAL 1
        OR disassembly MATCHES "OpTraceRay")
    message(FATAL_ERROR
        "UPT-38 must retain exactly six static RayQuery sites, one barrier and no TraceRay")
endif()

foreach(required_spatial_pattern
        "return Upt07IndirectHistoryEligible(decoded)"
        "shiftedValid = Upt07ReplayIndirectSample("
        "result.sourceTarget = source.target"
        "GroupMemoryBarrierWithGroupSync()"
        "Upt09PublishSharedEmptyCenterRescue")
    string(FIND "${spatial_source}" "${required_spatial_pattern}" offset)
    if(offset EQUAL -1)
        message(FATAL_ERROR
            "UPT-38 lost accepted UPT-33 spatial contract: ${required_spatial_pattern}")
    endif()
endforeach()

foreach(required_replay_pattern
        "public bool Upt07ThreeVertexEndpointStructureValid"
        "const float q = clamp(source.proposalPdf"
        "const Upt04PathEvent secondContinuation"
        "const Upt04HitFacts secondHit = Upt07TraceIndirect("
        "shifted.pathLength = 2u"
        "shifted.reconnectionVertexLength = 1u")
    string(FIND "${replay_source}" "${required_replay_pattern}" offset)
    if(offset EQUAL -1)
        message(FATAL_ERROR
            "UPT-38 lost exact stored-q replay contract: ${required_replay_pattern}")
    endif()
endforeach()
if(replay_source MATCHES
        "minimumPathThroughput[^\n]*Upt07ReplayIndirectSample")
    message(FATAL_ERROR
        "UPT-38 must not reapply the initial-only throughput cutoff")
endif()

if(NOT host_source MATCHES
        "upt09_spatial_shared_workgroup_pair_rescue_stored_target_light64_three_vertex.bin" OR
   NOT host_source MATCHES "threeVertexReplay=.u" OR
   NOT host_source MATCHES "continuationRaysMaxPerMapping=.u" OR
   NOT host_source MATCHES "mappingRaysMaxPerPair=.u")
    message(FATAL_ERROR
        "UPT-38 host specialization or bounded ray diagnostic is missing")
endif()

file(SIZE "${UPT38_DISASSEMBLY}" disassembly_bytes)
file(WRITE "${UPT38_STAMP}"
    "UPT-38 spatial verified: bindings=26 triangleClasses=9/13 push=96 RayQuery=${ray_query_count} barrier=${barrier_count} TraceRay=0 exactStoredQReplay=1 initialCutoffReplay=0 disassemblyBytes=${disassembly_bytes}\n")
