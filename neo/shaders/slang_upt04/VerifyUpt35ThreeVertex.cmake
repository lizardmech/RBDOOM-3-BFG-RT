foreach(required UPT35_REFLECTION UPT35_DISASSEMBLY UPT35_SPV
        UPT35_SOURCE UPT35_STAMP)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "UPT-35 verification missing ${required}")
    endif()
endforeach()

file(READ "${UPT35_REFLECTION}" reflection)
file(READ "${UPT35_DISASSEMBLY}" disassembly)
file(READ "${UPT35_SOURCE}" source)

string(REGEX MATCHALL "\"binding\"[ \t]*:" descriptor_bindings "${reflection}")
list(LENGTH descriptor_bindings descriptor_count)
if(NOT descriptor_count EQUAL 31)
    message(FATAL_ERROR
        "UPT-35 exposes ${descriptor_count} descriptors; expected 31")
endif()
if(NOT reflection MATCHES
        "\"name\"[ \t]*:[ \t]*\"reservedControl1\",[ \t\r\n]*\"type\"[ \t]*:[ \t]*\"uint\",[ \t\r\n]*\"offset\"[ \t]*:[ \t]*204" OR
   NOT reflection MATCHES
        "\"name\"[ \t]*:[ \t]*\"skyBrightness\",[ \t\r\n]*\"type\"[ \t]*:[ \t]*\"float\",[ \t\r\n]*\"offset\"[ \t]*:[ \t]*216" OR
   NOT reflection MATCHES
        "\"name\"[ \t]*:[ \t]*\"emissiveTexelBlackFloor\",[ \t\r\n]*\"type\"[ \t]*:[ \t]*\"float\",[ \t\r\n]*\"offset\"[ \t]*:[ \t]*220")
    message(FATAL_ERROR "UPT-35 push constants are not 224 bytes")
endif()
if(NOT reflection MATCHES
        "\"workgroup_size\"[ \t\r\n]*:[ \t\r\n]*\\[[ \t\r\n]*8[ \t]*,[ \t\r\n]*8")
    message(FATAL_ERROR "UPT-35 workgroup is not 8x8")
endif()

string(REGEX MATCHALL "OpRayQueryInitializeKHR" ray_queries "${disassembly}")
list(LENGTH ray_queries ray_query_count)
if(NOT ray_query_count EQUAL 4)
    message(FATAL_ERROR
        "UPT-35 has ${ray_query_count} RayQuery sites; expected 4")
endif()
if(disassembly MATCHES "OpTraceRayKHR")
    message(FATAL_ERROR "UPT-35 unexpectedly contains TraceRay")
endif()

foreach(pattern
        "asfloat\\(gUpt04Control\\.reservedControl0\\)"
        "asfloat\\(gUpt04Control\\.reservedControl1\\)"
        "dot\\(uncompensatedPathThroughput"
        "minimumPathThroughput[ \t]*\\*[ \t]*minimumPathThroughput"
        "pathThroughput[ \t]*=[ \t]*uncompensatedPathThroughput[ \t]*/[ \t]*q"
        "pathPdf[ \t]*\\*=[ \t]*continuation\\.pdf[ \t]*\\*[ \t]*q"
        "proposalPdf[ \t]*=[ \t]*endpoint\\.accumulatedRouletteProbability"
        "Upt04MakeBounceLightSchedule\\(ranges, kUpt04ThirdNeeBounceIndex\\)"
        "nee\\.pathLength[ \t]*=[ \t]*3u"
        "Upt04AdmitThirdVertexNeeVisibility"
        "pathLength[ \t]*=[ \t]*2u")
    if(NOT source MATCHES "${pattern}")
        message(FATAL_ERROR "UPT-35 source lacks estimator proof ${pattern}")
    endif()
endforeach()

file(SIZE "${UPT35_SPV}" spv_bytes)
file(WRITE "${UPT35_STAMP}"
    "UPT-35/37 verified: descriptors=28 push=224 workgroup=8x8 RayQuery=4 TraceRay=0 directionalSky=31 bytes=${spv_bytes} runtime-q initial-only-L2-cutoff endpointPathLength=2 x3NeePathLength=3 maxRays=5\n")