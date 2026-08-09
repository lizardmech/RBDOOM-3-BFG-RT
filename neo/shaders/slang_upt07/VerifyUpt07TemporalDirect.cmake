foreach(required UPT07_FULL_REFLECTION UPT07_FULL_DISASSEMBLY
        UPT07_COMPACT_REFLECTION UPT07_COMPACT_DISASSEMBLY
        UPT07_UNIFIED_REFLECTION UPT07_UNIFIED_DISASSEMBLY
        UPT07_UNIFIED_COMPACT_REFLECTION UPT07_UNIFIED_COMPACT_DISASSEMBLY
        UPT07_ROUTE_DIAG_REFLECTION UPT07_ROUTE_DIAG_DISASSEMBLY
        UPT07_ROUTE_DIAG_COMPACT_REFLECTION UPT07_ROUTE_DIAG_COMPACT_DISASSEMBLY
        UPT07_RECONNECT_REFLECTION UPT07_RECONNECT_DISASSEMBLY
        UPT07_RECONNECT_COMPACT_REFLECTION UPT07_RECONNECT_COMPACT_DISASSEMBLY
        UPT07_STAMP)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "UPT-07 direct temporal verification missing ${required}")
    endif()
endforeach()

file(READ "${UPT07_FULL_REFLECTION}" full_reflection)
file(READ "${UPT07_FULL_DISASSEMBLY}" full_disassembly)
file(READ "${UPT07_COMPACT_REFLECTION}" compact_reflection)
file(READ "${UPT07_COMPACT_DISASSEMBLY}" compact_disassembly)
file(READ "${UPT07_UNIFIED_REFLECTION}" unified_reflection)
file(READ "${UPT07_UNIFIED_DISASSEMBLY}" unified_disassembly)
file(READ "${UPT07_UNIFIED_COMPACT_REFLECTION}" unified_compact_reflection)
file(READ "${UPT07_UNIFIED_COMPACT_DISASSEMBLY}" unified_compact_disassembly)
file(READ "${UPT07_ROUTE_DIAG_REFLECTION}" route_diag_reflection)
file(READ "${UPT07_ROUTE_DIAG_DISASSEMBLY}" route_diag_disassembly)
file(READ "${UPT07_ROUTE_DIAG_COMPACT_REFLECTION}" route_diag_compact_reflection)
file(READ "${UPT07_ROUTE_DIAG_COMPACT_DISASSEMBLY}" route_diag_compact_disassembly)
file(READ "${UPT07_RECONNECT_REFLECTION}" reconnect_reflection)
file(READ "${UPT07_RECONNECT_DISASSEMBLY}" reconnect_disassembly)
file(READ "${UPT07_RECONNECT_COMPACT_REFLECTION}" reconnect_compact_reflection)
file(READ "${UPT07_RECONNECT_COMPACT_DISASSEMBLY}" reconnect_compact_disassembly)

foreach(kind full compact)
    foreach(binding 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 23 25 26 27 28)
        if(NOT ${kind}_reflection MATCHES "\"set\"[ \t]*:[ \t]*0,[ \t\r\n]*\"binding\"[ \t]*:[ \t]*${binding}")
            message(FATAL_ERROR "UPT-07 ${kind} direct temporal lacks set 0 binding ${binding}")
        endif()
    endforeach()
    string(REGEX MATCHALL "\"binding\"[ \t]*:" bindings "${${kind}_reflection}")
    list(LENGTH bindings binding_count)
    if(NOT binding_count EQUAL 28)
        message(FATAL_ERROR "UPT-07 ${kind} direct temporal must retain exactly twenty-eight bindings")
    endif()
    if(NOT ${kind}_reflection MATCHES "\"set\"[ \t]*:[ \t]*1,[ \t\r\n]*\"binding\"[ \t]*:[ \t]*0")
        message(FATAL_ERROR "UPT-07 ${kind} direct temporal lacks the bindless emitter texture set")
    endif()
    if(NOT ${kind}_reflection MATCHES "\"array_stride\"[ \t]*:[ \t]*32")
        message(FATAL_ERROR "UPT-07 ${kind} direct temporal lacks the compact 32-byte receiver/history sidecar types")
    endif()
    if(NOT ${kind}_reflection MATCHES "\"workgroup_size\"[ \t\r\n]*:[ \t\r\n]*\[[ \t\r\n]*8[ \t]*,[ \t\r\n]*8")
        message(FATAL_ERROR "UPT-07 ${kind} direct temporal workgroup is not 8x8")
    endif()
    if(NOT ${kind}_reflection MATCHES "\"name\"[ \t]*:[ \t]*\"maximumHistoryAge\",[ \t\r\n]*\"type\"[ \t]*:[ \t]*\"uint\",[ \t\r\n]*\"offset\"[ \t]*:[ \t]*44" OR
       NOT ${kind}_reflection MATCHES "\"name\"[ \t]*:[ \t]*\"previousCamera\",[ \t\r\n]*\"type\"[ \t]*:[ \t]*\"_[0-9]+\",[ \t\r\n]*\"offset\"[ \t]*:[ \t]*48" OR
       NOT ${kind}_reflection MATCHES "\"name\"[ \t]*:[ \t]*\"geometryAvailabilityFlags\",[ \t\r\n]*\"type\"[ \t]*:[ \t]*\"uint\",[ \t\r\n]*\"offset\"[ \t]*:[ \t]*112" OR
       NOT ${kind}_reflection MATCHES "\"name\"[ \t]*:[ \t]*\"emissiveScale\",[ \t\r\n]*\"type\"[ \t]*:[ \t]*\"float\",[ \t\r\n]*\"offset\"[ \t]*:[ \t]*116" OR
       NOT ${kind}_reflection MATCHES "\"name\"[ \t]*:[ \t]*\"previousToCurrentLightCount\",[ \t\r\n]*\"type\"[ \t]*:[ \t]*\"uint\",[ \t\r\n]*\"offset\"[ \t]*:[ \t]*120")
        message(FATAL_ERROR "UPT-07 ${kind} direct temporal push constants lost the established 128-byte prefix")
    endif()
    string(REGEX MATCHALL "OpRayQueryInitializeKHR" rayquery_initializers "${${kind}_disassembly}")
    list(LENGTH rayquery_initializers rayquery_initializer_count)
    if(NOT rayquery_initializer_count EQUAL 1 OR ${kind}_disassembly MATCHES "OpTraceRayKHR")
        message(FATAL_ERROR "UPT-07 ${kind} direct temporal must retain exactly one winner-only RayQuery and no TraceRay site")
    endif()
    if(NOT ${kind}_disassembly MATCHES "OpImageSample" OR ${kind}_disassembly MATCHES "OpImageFetch")
        message(FATAL_ERROR "UPT-07 ${kind} direct temporal lost exact sampled emissive endpoint replay")
    endif()
    if(${kind}_disassembly MATCHES "OpCapability (Int16|Float16)" OR
       ${kind}_disassembly MATCHES "OpType(Int|Float) 16")
        message(FATAL_ERROR "UPT-07 ${kind} direct temporal introduced native 16-bit SPIR-V")
    endif()
endforeach()

foreach(kind route_diag route_diag_compact)
    if(NOT ${kind}_reflection MATCHES "\"set\"[ \t]*:[ \t]*0,[ \t\r\n]*\"binding\"[ \t]*:[ \t]*30")
        message(FATAL_ERROR "UPT temporal ${kind} lacks the route diagnostic UAV")
    endif()
    string(REGEX MATCHALL "\"binding\"[ \t]*:" bindings "${${kind}_reflection}")
    list(LENGTH bindings binding_count)
    if(NOT binding_count EQUAL 31)
        message(FATAL_ERROR "UPT temporal ${kind} must expose exactly thirty-one bindings")
    endif()
    string(REGEX MATCHALL "OpRayQueryInitializeKHR" rayquery_initializers "${${kind}_disassembly}")
    list(LENGTH rayquery_initializers rayquery_initializer_count)
    if(NOT rayquery_initializer_count EQUAL 4 OR ${kind}_disassembly MATCHES "OpTraceRayKHR")
        message(FATAL_ERROR "UPT temporal ${kind} must preserve the baseline four RayQuery sites")
    endif()
endforeach()

foreach(kind unified unified_compact)
    foreach(binding 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 25 26 27 28 29)
        if(NOT ${kind}_reflection MATCHES "\"set\"[ \t]*:[ \t]*0,[ \t\r\n]*\"binding\"[ \t]*:[ \t]*${binding}")
            message(FATAL_ERROR "UPT-07 ${kind} unified temporal lacks set 0 binding ${binding}")
        endif()
    endforeach()
    string(REGEX MATCHALL "\"binding\"[ \t]*:" bindings "${${kind}_reflection}")
    list(LENGTH bindings binding_count)
    if(NOT binding_count EQUAL 30)
        message(FATAL_ERROR "UPT-07 ${kind} unified temporal must expose exactly thirty replay bindings")
    endif()
    string(REGEX MATCHALL "OpRayQueryInitializeKHR" rayquery_initializers "${${kind}_disassembly}")
    list(LENGTH rayquery_initializers rayquery_initializer_count)
    if(NOT rayquery_initializer_count EQUAL 4 OR ${kind}_disassembly MATCHES "OpTraceRayKHR")
        message(FATAL_ERROR "UPT-07 ${kind} unified temporal must expose exactly four statically bounded RayQuery call sites and no TraceRay site")
    endif()
    if(NOT ${kind}_reflection MATCHES "\"set\"[ \t]*:[ \t]*1,[ \t\r\n]*\"binding\"[ \t]*:[ \t]*0" OR
       NOT ${kind}_reflection MATCHES "\"array_stride\"[ \t]*:[ \t]*32" OR
       NOT ${kind}_reflection MATCHES "\"workgroup_size\"[ \t\r\n]*:[ \t\r\n]*\[[ \t\r\n]*8[ \t]*,[ \t\r\n]*8" OR
       NOT ${kind}_disassembly MATCHES "OpImageSample" OR
       ${kind}_disassembly MATCHES "OpImageFetch" OR
       ${kind}_disassembly MATCHES "OpCapability (Int16|Float16)" OR
       ${kind}_disassembly MATCHES "OpType(Int|Float) 16")
        message(FATAL_ERROR "UPT-07 ${kind} unified temporal lost its compact receiver, texture, workgroup, or scalar-width contract")
    endif()
endforeach()

foreach(kind reconnect reconnect_compact)
    if(NOT ${kind}_reflection MATCHES "\"set\"[ \t]*:[ \t]*0,[ \t\r\n]*\"binding\"[ \t]*:[ \t]*30")
        message(FATAL_ERROR "UPT-17 ${kind} temporal lacks the aggregate diagnostic UAV")
    endif()
    string(REGEX MATCHALL "\"binding\"[ \t]*:" bindings "${${kind}_reflection}")
    list(LENGTH bindings binding_count)
    if(NOT binding_count EQUAL 31)
        message(FATAL_ERROR "UPT-17 ${kind} temporal must expose exactly thirty-one bindings")
    endif()
    string(REGEX MATCHALL "OpRayQueryInitializeKHR" rayquery_initializers "${${kind}_disassembly}")
    list(LENGTH rayquery_initializers rayquery_initializer_count)
    if(NOT rayquery_initializer_count EQUAL 6 OR ${kind}_disassembly MATCHES "OpTraceRayKHR")
        message(FATAL_ERROR "UPT-17 ${kind} temporal must expose six bounded branch-dependent RayQuery sites and no TraceRay site")
    endif()
endforeach()

if(NOT full_reflection MATCHES "\"name\"[ \t]*:[ \t]*\"gUpt07CurrentLights\"" OR
   NOT compact_reflection MATCHES "\"name\"[ \t]*:[ \t]*\"gUpt07CurrentLights\"")
    message(FATAL_ERROR "UPT-07 direct temporal lacks its selected full/compact light stream")
endif()

file(WRITE "${UPT07_STAMP}"
    "UPT-07 temporal verified: direct variants retain 28 bindings/one winner RayQuery; baseline unified variants retain 30 bindings/four RayQuery sites; route diagnostics expose 31 bindings while retaining four sites; UPT-17 reconnect variants expose 31 bindings/six bounded branch-dependent sites; no TraceRay or native16\n")
