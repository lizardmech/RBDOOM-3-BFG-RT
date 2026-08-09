foreach(required_file IN ITEMS
    "${UPT04_TILE_PRESAMPLE_DISASSEMBLY}"
    "${UPT04_TILE_DIRECT_DISASSEMBLY}"
    "${UPT04_TILE_INDIRECT_DISASSEMBLY}")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "UPT-11 missing disassembly: ${required_file}")
    endif()
endforeach()

file(READ "${UPT04_TILE_PRESAMPLE_DISASSEMBLY}" presample_text)
file(READ "${UPT04_TILE_DIRECT_DISASSEMBLY}" direct_text)
file(READ "${UPT04_TILE_INDIRECT_DISASSEMBLY}" indirect_text)

foreach(binding IN ITEMS 0 1)
    if(NOT presample_text MATCHES "OpDecorate %[^\r\n]+ Binding ${binding}")
        message(FATAL_ERROR
            "UPT-11 presample is missing descriptor binding ${binding}")
    endif()
endforeach()
foreach(split_text IN ITEMS direct_text indirect_text)
    if(NOT "${${split_text}}" MATCHES "OpDecorate %[^\r\n]+ Binding 30")
        message(FATAL_ERROR
            "UPT-11 ${split_text} is missing the light-tile SRV at binding 30")
    endif()
endforeach()

string(REGEX MATCHALL "OpRayQueryInitializeKHR" presample_rays
    "${presample_text}")
string(REGEX MATCHALL "OpRayQueryInitializeKHR" direct_rays
    "${direct_text}")
string(REGEX MATCHALL "OpRayQueryInitializeKHR" indirect_rays
    "${indirect_text}")
list(LENGTH presample_rays presample_ray_count)
list(LENGTH direct_rays direct_ray_count)
list(LENGTH indirect_rays indirect_ray_count)
if(NOT presample_ray_count EQUAL 0)
    message(FATAL_ERROR "UPT-11 presample must issue zero ray queries")
endif()
if(NOT direct_ray_count EQUAL 1)
    message(FATAL_ERROR
        "UPT-11 split direct must retain exactly one ray query; found ${direct_ray_count}")
endif()
if(NOT indirect_ray_count EQUAL 2)
    message(FATAL_ERROR
        "UPT-11 split indirect must retain exactly two ray queries; found ${indirect_ray_count}")
endif()

file(WRITE "${UPT04_TILE_STAMP}"
    "UPT-11 light tiles verified: bindings=0/1/30 rays=0+1+2\n")
message(STATUS
    "UPT-11 light tiles verified: bindings=0/1/30 rays=0+1+2")
