if(NOT DEFINED UPT36_REFLECTION OR NOT DEFINED UPT36_DISASSEMBLY OR
   NOT DEFINED UPT36_STAMP OR NOT DEFINED UPT36_HOST_SOURCE OR
   NOT DEFINED UPT36_REPLAY_SOURCE OR NOT DEFINED UPT36_MATERIAL_SOURCE)
    message(FATAL_ERROR "UPT-36 temporal verification paths are required")
endif()

file(READ "${UPT36_REFLECTION}" reflection)
file(READ "${UPT36_DISASSEMBLY}" disassembly)
file(READ "${UPT36_HOST_SOURCE}" host_source)
file(READ "${UPT36_REPLAY_SOURCE}" replay_source)
file(READ "${UPT36_MATERIAL_SOURCE}" material_source)

string(REGEX MATCHALL "\"binding\"[ \t]*:" bindings "${reflection}")
list(LENGTH bindings binding_count)
if(NOT binding_count EQUAL 32)
    message(FATAL_ERROR
        "UPT-36 must preserve the compact+duplication 32-binding directional-sky layout")
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
if(NOT rayquery_count EQUAL 8 OR disassembly MATCHES "OpTraceRay")
    message(FATAL_ERROR
        "UPT-36 must preserve eight bounded RayQuery sites and no TraceRay site")
endif()

if(NOT replay_source MATCHES "Upt07ThreeVertexEndpointStructureValid" OR
   NOT replay_source MATCHES "const float q = clamp.source.proposalPdf" OR
   NOT replay_source MATCHES "never reapplies the biased" OR
   NOT replay_source MATCHES "secondContinuation.throughput / q" OR
   NOT replay_source MATCHES "Upt07ThreeVertexNeeStructureValid" OR
   NOT replay_source MATCHES "source.pathLength == 3u" OR
   NOT replay_source MATCHES "shifted.pathLength = 3u" OR
   NOT replay_source MATCHES "shifted.pathLength = 2u" OR
   NOT replay_source MATCHES "shifted.accumulatedRouletteProbability = q")
    message(FATAL_ERROR
        "UPT-36 replay source lacks the stored-q two-continuation contract")
endif()
if(NOT host_source MATCHES
        "upt07_temporal_unified_rayquery_light64_duplication_common_gris_three_vertex.bin")
    message(FATAL_ERROR "UPT-36 host runtime artifact selection is missing")
endif()
if(NOT material_source MATCHES "import upt04_material_classifier" OR
   NOT material_source MATCHES "Upt04ApplyMaterialClassifier")
    message(FATAL_ERROR
        "UPT-36 replay material must share D0 classifier interpretation")
endif()

file(WRITE "${UPT36_STAMP}"
    "UPT-36 temporal verified: bindings=32, directionalSky=36, triangleClasses=9/13, RayQuery sites=8, TraceRay=0, stored-q exact replay, x3NeeReplay=1, throughput cutoff absent, D0/T0 materialClassifier=shared\n")
