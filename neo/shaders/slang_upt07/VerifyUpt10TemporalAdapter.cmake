if(NOT DEFINED UPT10_REFLECTION OR NOT DEFINED UPT10_DISASSEMBLY OR
   NOT DEFINED UPT10_STAMP OR NOT DEFINED UPT10_HOST_SOURCE)
    message(FATAL_ERROR "UPT-30 temporal adapter verification paths are required")
endif()

file(READ "${UPT10_REFLECTION}" reflection)
file(READ "${UPT10_DISASSEMBLY}" disassembly)
file(READ "${UPT10_HOST_SOURCE}" host_source)
if(NOT DEFINED UPT10_LABEL)
    set(UPT10_LABEL "shared-adapter")
endif()

string(REGEX MATCHALL "\"binding\"[ \t]*:" bindings "${reflection}")
list(LENGTH bindings binding_count)
if(NOT binding_count EQUAL 31)
    message(FATAL_ERROR
        "UPT-30 ${UPT10_LABEL} must preserve the accepted compact+duplication 31-binding layout")
endif()
if(NOT reflection MATCHES
        "\"name\"[ \t]*:[ \t]*\"gUpt04StaticTriangleClasses\"[^}]*\"set\"[ \t]*:[ \t]*0[^}]*\"binding\"[ \t]*:[ \t]*9" OR
   NOT reflection MATCHES
        "\"name\"[ \t]*:[ \t]*\"gUpt04DynamicTriangleClasses\"[^}]*\"set\"[ \t]*:[ \t]*0[^}]*\"binding\"[ \t]*:[ \t]*13")
    message(FATAL_ERROR
        "UPT-30 ${UPT10_LABEL} lost the exact triangle-class shader bindings 9/13")
endif()
if(NOT host_source MATCHES
        "StructuredBuffer_SRV.9, geometry.staticTriangleClassBuffer." OR
   NOT host_source MATCHES
        "StructuredBuffer_SRV.13, geometry.dynamicTriangleClassBuffer." OR
   NOT host_source MATCHES "Upt04AddReplayGeometryLayoutItems.layoutDesc.")
    message(FATAL_ERROR
        "UPT-30 ${UPT10_LABEL} host replay descriptor contract lacks triangle-class parity")
endif()

string(REGEX MATCHALL "OpRayQueryInitializeKHR" rayquery_initializers "${disassembly}")
list(LENGTH rayquery_initializers rayquery_count)
if(NOT rayquery_count EQUAL 4 OR disassembly MATCHES "OpTraceRay")
    message(FATAL_ERROR
        "UPT-30 ${UPT10_LABEL} must preserve four bounded RayQuery sites and no TraceRay site")
endif()

file(WRITE "${UPT10_STAMP}"
    "UPT-30 temporal ${UPT10_LABEL} verified: bindings=31, triangleClasses=9/13 host+shader, RayQuery sites=4, TraceRay=0\n")
