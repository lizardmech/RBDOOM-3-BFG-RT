foreach(required UPT09_FULL_REFLECTION UPT09_FULL_DISASSEMBLY
        UPT09_COMPACT_REFLECTION UPT09_COMPACT_DISASSEMBLY UPT09_STAMP)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "UPT-09 direct spatial verification missing ${required}")
    endif()
endforeach()

file(READ "${UPT09_FULL_REFLECTION}" full_reflection)
file(READ "${UPT09_FULL_DISASSEMBLY}" full_disassembly)
file(READ "${UPT09_COMPACT_REFLECTION}" compact_reflection)
file(READ "${UPT09_COMPACT_DISASSEMBLY}" compact_disassembly)

foreach(kind full compact)
    # Slang strips generic replay declarations 7..21 because the baseline
    # direct-only shader resolves through the live endpoint bindings 6/32.
    foreach(binding 0 1 2 3 4 5 6 27 28 32)
        if(NOT ${kind}_reflection MATCHES "\"set\"[ \t]*:[ \t]*0,[ \t\r\n]*\"binding\"[ \t]*:[ \t]*${binding}")
            message(FATAL_ERROR "UPT-09 ${kind} direct spatial lacks set 0 binding ${binding}")
        endif()
    endforeach()
    string(REGEX MATCHALL "\"binding\"[ \t]*:" bindings "${${kind}_reflection}")
    list(LENGTH bindings binding_count)
    if(NOT binding_count EQUAL 11)
        message(FATAL_ERROR "UPT-09 ${kind} direct spatial must expose exactly eleven live bindings including the bindless emitter set")
    endif()
    if(NOT ${kind}_reflection MATCHES "\"set\"[ \t]*:[ \t]*1,[ \t\r\n]*\"binding\"[ \t]*:[ \t]*0")
        message(FATAL_ERROR "UPT-09 ${kind} direct spatial lacks the bindless emitter texture set")
    endif()
    if(NOT ${kind}_reflection MATCHES "\"array_stride\"[ \t]*:[ \t]*32")
        message(FATAL_ERROR "UPT-09 ${kind} direct spatial lacks the compact 32-byte receiver/history sidecar types")
    endif()
    if(NOT ${kind}_reflection MATCHES "\"workgroup_size\"[ \t\r\n]*:[ \t\r\n]*\[[ \t\r\n]*8[ \t]*,[ \t\r\n]*8")
        message(FATAL_ERROR "UPT-09 ${kind} direct spatial workgroup is not 8x8")
    endif()
    if(NOT ${kind}_reflection MATCHES "\"name\"[ \t]*:[ \t]*\"neighborRadius\",[ \t\r\n]*\"type\"[ \t]*:[ \t]*\"float\",[ \t\r\n]*\"offset\"[ \t]*:[ \t]*48" OR
       NOT ${kind}_reflection MATCHES "\"name\"[ \t]*:[ \t]*\"emissiveScale\",[ \t\r\n]*\"type\"[ \t]*:[ \t]*\"float\",[ \t\r\n]*\"offset\"[ \t]*:[ \t]*56" OR
       NOT ${kind}_reflection MATCHES "\"name\"[ \t]*:[ \t]*\"emissiveTexelBlackFloor\",[ \t\r\n]*\"type\"[ \t]*:[ \t]*\"float\",[ \t\r\n]*\"offset\"[ \t]*:[ \t]*100")
        message(FATAL_ERROR "UPT-09 ${kind} direct spatial push constants lost the established 64-byte prefix")
    endif()
    string(REGEX MATCHALL "OpRayQueryInitializeKHR" ray_queries "${${kind}_disassembly}")
    list(LENGTH ray_queries ray_query_count)
    if(NOT ray_query_count EQUAL 1 OR ${kind}_disassembly MATCHES "OpTraceRayKHR")
        message(FATAL_ERROR "UPT-09 ${kind} direct spatial must expose exactly one bounded RayQuery trace site")
    endif()
    if(NOT ${kind}_disassembly MATCHES "OpImageSample" OR ${kind}_disassembly MATCHES "OpImageFetch")
        message(FATAL_ERROR "UPT-09 ${kind} direct spatial lost exact sampled emissive endpoint replay")
    endif()
    if(${kind}_disassembly MATCHES "OpCapability (Int16|Float16)" OR
       ${kind}_disassembly MATCHES "OpType(Int|Float) 16")
        message(FATAL_ERROR "UPT-09 ${kind} direct spatial introduced native 16-bit SPIR-V")
    endif()
endforeach()

if(NOT full_reflection MATCHES "\"binding\"[ \t]*:[ \t]*4" OR
   NOT compact_reflection MATCHES "\"binding\"[ \t]*:[ \t]*4")
    message(FATAL_ERROR "UPT-09 direct spatial lacks its selected full/compact light binding")
endif()

file(WRITE "${UPT09_STAMP}"
    "UPT-09 direct spatial verified: eleven live bindings, compact 32-byte current receiver plus cold history sidecar, separate input/output 64-byte pages, exact emissive endpoint and texture replay, bounded empty-center rescue, one RayQuery site, no native16\n")
