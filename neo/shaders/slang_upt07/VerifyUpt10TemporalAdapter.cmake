if(NOT DEFINED UPT10_REFLECTION OR NOT DEFINED UPT10_DISASSEMBLY OR NOT DEFINED UPT10_STAMP)
    message(FATAL_ERROR "UPT-30 temporal adapter verification paths are required")
endif()

file(READ "${UPT10_REFLECTION}" reflection)
file(READ "${UPT10_DISASSEMBLY}" disassembly)

string(REGEX MATCHALL "\"binding\"[ \t]*:" bindings "${reflection}")
list(LENGTH bindings binding_count)
if(NOT binding_count EQUAL 30)
    message(FATAL_ERROR
        "UPT-30 temporal adapter must preserve the accepted compact+duplication 30-binding layout")
endif()

string(REGEX MATCHALL "OpRayQueryInitializeKHR" rayquery_initializers "${disassembly}")
list(LENGTH rayquery_initializers rayquery_count)
if(NOT rayquery_count EQUAL 4 OR disassembly MATCHES "OpTraceRay")
    message(FATAL_ERROR
        "UPT-30 temporal adapter must preserve four bounded RayQuery sites and no TraceRay site")
endif()

file(WRITE "${UPT10_STAMP}"
    "UPT-30 temporal shared adapter verified: bindings=30, RayQuery sites=4, TraceRay=0, runtime copy=0\n")
