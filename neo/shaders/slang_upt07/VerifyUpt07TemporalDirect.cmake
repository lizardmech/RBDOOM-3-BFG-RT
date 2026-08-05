foreach(required UPT07_FULL_REFLECTION UPT07_FULL_DISASSEMBLY
        UPT07_COMPACT_REFLECTION UPT07_COMPACT_DISASSEMBLY UPT07_STAMP)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "UPT-07 direct temporal verification missing ${required}")
    endif()
endforeach()

file(READ "${UPT07_FULL_REFLECTION}" full_reflection)
file(READ "${UPT07_FULL_DISASSEMBLY}" full_disassembly)
file(READ "${UPT07_COMPACT_REFLECTION}" compact_reflection)
file(READ "${UPT07_COMPACT_DISASSEMBLY}" compact_disassembly)

foreach(kind full compact)
    foreach(binding 0 1 2 3 4 5 6 7 8 10 11 12 14 15 16 17 18 19 20 21 23 25 26)
        if(NOT ${kind}_reflection MATCHES "\"set\"[ \t]*:[ \t]*0,[ \t\r\n]*\"binding\"[ \t]*:[ \t]*${binding}")
            message(FATAL_ERROR "UPT-07 ${kind} direct temporal lacks set 0 binding ${binding}")
        endif()
    endforeach()
    string(REGEX MATCHALL "\"binding\"[ \t]*:" bindings "${${kind}_reflection}")
    list(LENGTH bindings binding_count)
    if(NOT binding_count EQUAL 23)
        message(FATAL_ERROR "UPT-07 ${kind} direct temporal must expose exactly twenty-three narrow replay bindings")
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
       NOT ${kind}_reflection MATCHES "\"name\"[ \t]*:[ \t]*\"emissiveReplayCount\",[ \t\r\n]*\"type\"[ \t]*:[ \t]*\"uint\",[ \t\r\n]*\"offset\"[ \t]*:[ \t]*116" OR
       NOT ${kind}_reflection MATCHES "\"name\"[ \t]*:[ \t]*\"previousToCurrentLightCount\",[ \t\r\n]*\"type\"[ \t]*:[ \t]*\"uint\",[ \t\r\n]*\"offset\"[ \t]*:[ \t]*120")
        message(FATAL_ERROR "UPT-07 ${kind} direct temporal push constants lost the established 128-byte prefix")
    endif()
    string(REGEX MATCHALL "OpRayQueryInitializeKHR" rayquery_initializers "${${kind}_disassembly}")
    list(LENGTH rayquery_initializers rayquery_initializer_count)
    if(NOT rayquery_initializer_count EQUAL 1 OR ${kind}_disassembly MATCHES "OpTraceRayKHR")
        message(FATAL_ERROR "UPT-07 ${kind} direct temporal must expose exactly one winner-only RayQuery and no TraceRay site")
    endif()
    if(${kind}_disassembly MATCHES "OpImageSample|OpImageFetch|OpTypeImage")
        message(FATAL_ERROR "UPT-07 ${kind} direct temporal admitted texture/image work")
    endif()
    if(${kind}_disassembly MATCHES "OpCapability (Int16|Float16)" OR
       ${kind}_disassembly MATCHES "OpType(Int|Float) 16")
        message(FATAL_ERROR "UPT-07 ${kind} direct temporal introduced native 16-bit SPIR-V")
    endif()
endforeach()

if(NOT full_reflection MATCHES "\"name\"[ \t]*:[ \t]*\"gUpt07CurrentLights\"" OR
   NOT compact_reflection MATCHES "\"name\"[ \t]*:[ \t]*\"gUpt07CurrentLights\"")
    message(FATAL_ERROR "UPT-07 direct temporal lacks its selected full/compact light stream")
endif()

file(WRITE "${UPT07_STAMP}"
    "UPT-07 direct temporal verified: twenty-three bindings including TLAS, light remap, 2x32-byte receivers and 2x32-byte cold history sidecars, exact emissive geometry replay, full/compact light variants, bounded randomized surface-only reprojection, exactly one winner-only RayQuery, no textures/native16\n")
