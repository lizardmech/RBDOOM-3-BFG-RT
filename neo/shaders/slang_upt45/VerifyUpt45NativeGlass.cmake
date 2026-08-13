foreach(required INPUT_PRODUCER_ASM INPUT_PRODUCER_REFLECTION INPUT_COMPOSE_REFLECTION INPUT_PRODUCER_SOURCE INPUT_GEOMETRY_SOURCE)
    if(NOT DEFINED ${required} OR NOT EXISTS "${${required}}")
        message(FATAL_ERROR "UPT-45 verifier missing ${required}: ${${required}}")
    endif()
endforeach()

file(READ "${INPUT_PRODUCER_ASM}" producer_asm)
string(REGEX MATCHALL "OpRayQueryInitializeKHR" ray_query_initializers "${producer_asm}")
list(LENGTH ray_query_initializers ray_query_initializer_count)
string(REGEX MATCHALL "OpRayQueryProceedKHR" ray_query_proceeds "${producer_asm}")
list(LENGTH ray_query_proceeds ray_query_proceed_count)
if(NOT ray_query_initializer_count EQUAL 1 OR NOT ray_query_proceed_count EQUAL 1)
    message(FATAL_ERROR
        "UPT-45 producer RayQuery ceiling violated: initialize=${ray_query_initializer_count} proceed=${ray_query_proceed_count}, expected 1/1")
endif()

file(READ "${INPUT_PRODUCER_SOURCE}" producer_source)
file(READ "${INPUT_GEOMETRY_SOURCE}" geometry_source)
set(native_source "${producer_source}\n${geometry_source}")
foreach(forbidden
    Upt04ResolvedTriangle
    Upt04GeometryHitSurface
    gUpt04UnifiedLight
    gUpt04EmissiveDistribution
    PreviousToCurrentLights
    CleanRtxdi
    ReGIR)
    if(native_source MATCHES "${forbidden}")
        message(FATAL_ERROR "UPT-45 native source imported forbidden dependency: ${forbidden}")
    endif()
endforeach()

function(verify_binding_whitelist reflection_path allowed label)
    file(READ "${reflection_path}" reflection)
    string(REGEX MATCHALL "\"binding\"[ \t\r\n]*:[ \t]*[0-9]+" binding_entries "${reflection}")
    foreach(entry IN LISTS binding_entries)
        string(REGEX REPLACE ".*:[ \t]*([0-9]+)" "\\1" binding "${entry}")
        if(NOT binding IN_LIST allowed)
            message(FATAL_ERROR "UPT-45 ${label} reflected forbidden descriptor binding ${binding}")
        endif()
    endforeach()
endfunction()

set(producer_allowed 0 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35)
set(compose_allowed 0 1 2)
verify_binding_whitelist("${INPUT_PRODUCER_REFLECTION}" "${producer_allowed}" "producer")
verify_binding_whitelist("${INPUT_COMPOSE_REFLECTION}" "${compose_allowed}" "composer")

message(STATUS
    "UPT-45 native glass verified: RayQuery=1 producerBindings=0,3..35 composerBindings=0..2 forbiddenDependencies=0")
