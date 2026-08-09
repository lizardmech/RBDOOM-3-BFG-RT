foreach(required_file IN ITEMS
    "${UPT04_NATIVE_DIRECT_SPLIT_DISASSEMBLY}"
    "${UPT04_NATIVE_DIRECT_ONLY_DISASSEMBLY}"
    "${UPT04_NATIVE_INDIRECT_SPLIT_DISASSEMBLY}")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "UPT-12 missing disassembly: ${required_file}")
    endif()
endforeach()

file(READ "${UPT04_NATIVE_DIRECT_SPLIT_DISASSEMBLY}" direct_split_text)
file(READ "${UPT04_NATIVE_DIRECT_ONLY_DISASSEMBLY}" direct_only_text)
file(READ "${UPT04_NATIVE_INDIRECT_SPLIT_DISASSEMBLY}" indirect_split_text)

foreach(view_text IN ITEMS direct_split_text direct_only_text indirect_split_text)
    if("${${view_text}}" MATCHES
        "OpLoad %Upt04UnifiedLightRecord_natural")
        message(FATAL_ERROR
            "UPT-12 ${view_text} retained an eager whole current-light load")
    endif()
    if(NOT "${${view_text}}" MATCHES "OpLoad %v4uint")
        message(FATAL_ERROR
            "UPT-12 ${view_text} is missing the native uint4 current-light view")
    endif()
endforeach()

string(REGEX MATCHALL "OpRayQueryInitializeKHR" direct_split_rays
    "${direct_split_text}")
string(REGEX MATCHALL "OpRayQueryInitializeKHR" direct_only_rays
    "${direct_only_text}")
string(REGEX MATCHALL "OpRayQueryInitializeKHR" indirect_split_rays
    "${indirect_split_text}")
list(LENGTH direct_split_rays direct_split_ray_count)
list(LENGTH direct_only_rays direct_only_ray_count)
list(LENGTH indirect_split_rays indirect_split_ray_count)
if(NOT direct_split_ray_count EQUAL 1 OR
   NOT direct_only_ray_count EQUAL 1 OR
   NOT indirect_split_ray_count EQUAL 2)
    message(FATAL_ERROR
        "UPT-12 ray ceiling changed: direct split=${direct_split_ray_count} direct only=${direct_only_ray_count} indirect split=${indirect_split_ray_count}")
endif()

file(WRITE "${UPT04_NATIVE_LIGHT_VIEW_STAMP}"
    "UPT-12 native light views verified: wholeLoads=0 rays=1/1/2\n")
message(STATUS
    "UPT-12 native light views verified: wholeLoads=0 rays=1/1/2")
