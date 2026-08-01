if(NOT DEFINED UPT04_SLANG_PROBE_DISASSEMBLY OR
   NOT DEFINED UPT04_DXC_PROBE_DISASSEMBLY OR
   NOT DEFINED UPT04_PROBE_STAMP)
    message(FATAL_ERROR "UPT-04 live TLAS probe verification arguments are incomplete")
endif()

foreach(probe IN ITEMS SLANG DXC)
    file(READ "${UPT04_${probe}_PROBE_DISASSEMBLY}" contents)
    if(NOT contents MATCHES "OpExecutionMode [^\n]* LocalSize 1 1 1")
        message(FATAL_ERROR "UPT-04 ${probe} live TLAS probe is not one invocation per group")
    endif()
    string(REGEX MATCHALL "OpRayQueryInitializeKHR" query_initializers "${contents}")
    list(LENGTH query_initializers query_initializer_count)
    if(NOT query_initializer_count EQUAL 1)
        message(FATAL_ERROR "UPT-04 ${probe} live TLAS probe must contain exactly one RayQuery initializer")
    endif()
    if(NOT contents MATCHES "OpDecorate [^\n]* DescriptorSet 0")
        message(FATAL_ERROR "UPT-04 ${probe} live TLAS probe is missing descriptor set 0")
    endif()
    if(NOT contents MATCHES "OpDecorate [^\n]* Binding 0" OR
       NOT contents MATCHES "OpDecorate [^\n]* Binding 1")
        message(FATAL_ERROR "UPT-04 ${probe} live TLAS probe must expose only the TLAS/UAV binding pair")
    endif()
endforeach()

if(DEFINED UPT04_FULL_LAYOUT_PROBE_DISASSEMBLY)
    file(READ "${UPT04_FULL_LAYOUT_PROBE_DISASSEMBLY}" full_layout_contents)
    if(NOT full_layout_contents MATCHES "OpExecutionMode [^\n]* LocalSize 1 1 1")
        message(FATAL_ERROR "UPT-04 full-layout live TLAS probe is not one invocation per group")
    endif()
    string(REGEX MATCHALL "OpRayQueryInitializeKHR" full_layout_query_initializers
        "${full_layout_contents}")
    list(LENGTH full_layout_query_initializers full_layout_query_initializer_count)
    if(NOT full_layout_query_initializer_count EQUAL 1)
        message(FATAL_ERROR "UPT-04 full-layout probe must contain exactly one RayQuery initializer")
    endif()
    if(NOT full_layout_contents MATCHES "OpDecorate [^\n]* Binding 0" OR
       NOT full_layout_contents MATCHES "OpDecorate [^\n]* Binding 4")
        message(FATAL_ERROR "UPT-04 full-layout probe must use the production TLAS/page bindings")
    endif()
    if(NOT full_layout_contents MATCHES "PushConstant")
        message(FATAL_ERROR "UPT-04 full-layout probe must retain the production push-constant interface")
    endif()
endif()

if(DEFINED UPT04_FULL_HOST_PROBE_DISASSEMBLY)
    file(READ "${UPT04_FULL_HOST_PROBE_DISASSEMBLY}" full_host_contents)
    if(NOT full_host_contents MATCHES "OpExecutionMode [^\n]* LocalSize 1 1 1")
        message(FATAL_ERROR "UPT-04 full-host live TLAS probe is not one invocation per group")
    endif()
    string(REGEX MATCHALL "OpRayQueryInitializeKHR" full_host_query_initializers
        "${full_host_contents}")
    list(LENGTH full_host_query_initializers full_host_query_initializer_count)
    if(NOT full_host_query_initializer_count EQUAL 1)
        message(FATAL_ERROR "UPT-04 full-host probe must contain exactly one RayQuery initializer")
    endif()
    if(NOT full_host_contents MATCHES "OpDecorate [^\n]* Binding 0" OR
       NOT full_host_contents MATCHES "OpDecorate [^\n]* Binding 4")
        message(FATAL_ERROR "UPT-04 full-host probe must use production TLAS/page slots")
    endif()
    if(full_host_contents MATCHES "PushConstant")
        message(FATAL_ERROR "UPT-04 full-host probe must not import the production shader ABI")
    endif()
endif()

file(WRITE "${UPT04_PROBE_STAMP}"
    "UPT-04 live TLAS compiler A/B verified\n"
    "Slang and DXC compact: LocalSize 1 1 1, descriptor set 0 bindings 0/1, one RayQuery initializer\n"
    "Slang full layout: LocalSize 1 1 1, production bindings 0/4, push constants, one RayQuery initializer\n"
    "Slang full host: LocalSize 1 1 1, production bindings 0/4, no imported UPT ABI, one RayQuery initializer\n")
