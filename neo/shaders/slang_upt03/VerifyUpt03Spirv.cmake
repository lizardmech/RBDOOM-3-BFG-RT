if(NOT DEFINED UPT03_REFLECTION OR NOT DEFINED UPT03_DISASSEMBLY OR NOT DEFINED UPT03_STAMP)
    message(FATAL_ERROR "UPT-03 verification requires reflection, disassembly, and stamp paths")
endif()

file(READ "${UPT03_REFLECTION}" reflection)
file(READ "${UPT03_DISASSEMBLY}" disassembly)

if(NOT reflection MATCHES "\"array_stride\"[ \t]*:[ \t]*96")
    message(FATAL_ERROR "UPT-03 candidate StructuredBuffer stride is not 96 bytes")
endif()
if(NOT reflection MATCHES "\"array_stride\"[ \t]*:[ \t]*64")
    message(FATAL_ERROR "UPT-03 packed reservoir RWStructuredBuffer stride is not 64 bytes")
endif()
if(NOT reflection MATCHES "\"workgroup_size\"[ \t\r\n]*:[ \t\r\n]*\[[ \t\r\n]*64")
    message(FATAL_ERROR "UPT-03 workgroup size is not 64")
endif()
string(REGEX MATCHALL "\"binding\"[ \t]*:" descriptor_bindings "${reflection}")
list(LENGTH descriptor_bindings descriptor_binding_count)
if(NOT descriptor_binding_count EQUAL 2
        OR NOT reflection MATCHES "\"binding\"[ \t]*:[ \t]*0"
        OR NOT reflection MATCHES "\"binding\"[ \t]*:[ \t]*1")
    message(FATAL_ERROR "UPT-03 must expose exactly candidate SRV binding 0 and reservoir UAV binding 1")
endif()
if(disassembly MATCHES "OpTraceRay|OpRayQuery|RayTracingKHR|RayQueryKHR")
    message(FATAL_ERROR "UPT-03 compute-only module unexpectedly contains ray instructions/capabilities")
endif()

file(WRITE "${UPT03_STAMP}"
    "UPT-03 SPIR-V contract verified: candidate stride=96, reservoir stride=64, threads=64, rays=0\n")
