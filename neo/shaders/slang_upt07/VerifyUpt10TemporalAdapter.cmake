if(NOT DEFINED UPT10_REFLECTION OR NOT DEFINED UPT10_DISASSEMBLY OR NOT DEFINED UPT10_STAMP)
    message(FATAL_ERROR "UPT-30 temporal adapter verification paths are required")
endif()

file(READ "${UPT10_REFLECTION}" reflection)
file(READ "${UPT10_DISASSEMBLY}" disassembly)
if(NOT DEFINED UPT10_LABEL)
    set(UPT10_LABEL "shared-adapter")
endif()

string(REGEX MATCHALL "\"binding\"[ \t]*:" bindings "${reflection}")
list(LENGTH bindings binding_count)
if(NOT binding_count EQUAL 30)
    message(FATAL_ERROR
        "UPT-30 ${UPT10_LABEL} must preserve the accepted compact+duplication 30-binding layout")
endif()

string(REGEX MATCHALL "OpRayQueryInitializeKHR" rayquery_initializers "${disassembly}")
list(LENGTH rayquery_initializers rayquery_count)
if(NOT rayquery_count EQUAL 4 OR disassembly MATCHES "OpTraceRay")
    message(FATAL_ERROR
        "UPT-30 ${UPT10_LABEL} must preserve four bounded RayQuery sites and no TraceRay site")
endif()

file(WRITE "${UPT10_STAMP}"
    "UPT-30 temporal ${UPT10_LABEL} verified: bindings=30, RayQuery sites=4, TraceRay=0\n")
