foreach(required UPT08_FILL_REFLECTION UPT08_FILL_DISASSEMBLY
        UPT08_COMPUTE_REFLECTION UPT08_COMPUTE_DISASSEMBLY UPT08_STAMP)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "UPT-08 duplication verification missing ${required}")
    endif()
endforeach()

file(READ "${UPT08_FILL_REFLECTION}" fill_reflection)
file(READ "${UPT08_FILL_DISASSEMBLY}" fill_disassembly)
file(READ "${UPT08_COMPUTE_REFLECTION}" compute_reflection)
file(READ "${UPT08_COMPUTE_DISASSEMBLY}" compute_disassembly)

foreach(kind fill compute)
    foreach(binding 0 1)
        if(NOT ${kind}_reflection MATCHES
                "\"set\"[ \t]*:[ \t]*0,[ \t\r\n]*\"binding\"[ \t]*:[ \t]*${binding}")
            message(FATAL_ERROR "UPT-08 ${kind} pass lacks set 0 binding ${binding}")
        endif()
    endforeach()
    string(REGEX MATCHALL "\"binding\"[ \t]*:" bindings "${${kind}_reflection}")
    list(LENGTH bindings binding_count)
    if(NOT binding_count EQUAL 2)
        message(FATAL_ERROR "UPT-08 ${kind} pass must expose exactly two buffers")
    endif()
    if(NOT ${kind}_reflection MATCHES
            "\"workgroup_size\"[ \t\r\n]*:[ \t\r\n]*\[[ \t\r\n]*8[ \t]*,[ \t\r\n]*8")
        message(FATAL_ERROR "UPT-08 ${kind} pass workgroup is not 8x8")
    endif()
    if(${kind}_disassembly MATCHES "OpRayQuery|OpTraceRayKHR|OpTypeImage|OpImage")
        message(FATAL_ERROR "UPT-08 ${kind} pass admitted ray or image work")
    endif()
    if(${kind}_disassembly MATCHES "OpCapability (Int16|Float16)" OR
       ${kind}_disassembly MATCHES "OpType(Int|Float) 16")
        message(FATAL_ERROR "UPT-08 ${kind} pass introduced native 16-bit SPIR-V")
    endif()
endforeach()

if(NOT fill_reflection MATCHES "\"array_stride\"[ \t]*:[ \t]*64")
    message(FATAL_ERROR "UPT-08 fill pass lost the 64-byte reservoir input ABI")
endif()
if(NOT compute_disassembly MATCHES "OpTypeArray[^\r\n]*%int_576" AND
   NOT compute_disassembly MATCHES "OpTypeArray[^\r\n]*%uint_576")
    message(FATAL_ERROR "UPT-08 compute pass lost its 24x24 shared sample-ID tile")
endif()

file(WRITE "${UPT08_STAMP}"
    "UPT-08 duplication verified: two-buffer 8x8 passes, 64-byte reservoir input, 24x24 shared tile for a 17x17 neighborhood, no ray/image/native16 work\n")
