if(NOT DEFINED UPT00_ARTIFACT_DIR OR NOT DEFINED UPT00_METRICS_OUTPUT)
    message(FATAL_ERROR "GenerateUpt00Metrics.cmake requires UPT00_ARTIFACT_DIR and UPT00_METRICS_OUTPUT")
endif()

function(count_spirv_regex contents pattern output_var)
    string(REGEX MATCHALL "${pattern}" matches "${contents}")
    list(LENGTH matches count)
    set(${output_var} "${count}" PARENT_SCOPE)
endfunction()

file(WRITE "${UPT00_METRICS_OUTPUT}"
    "kernel,compiler,spirv_bytes,spirv_sha256,id_bound,operations,functions,loads,stores,branches,conditional_branches,access_chains,ray_queries\n")

set(kernels structured_math ray_query reservoir_update)
foreach(kernel IN LISTS kernels)
    foreach(compiler slang dxc)
        set(binary "${UPT00_ARTIFACT_DIR}/${kernel}.${compiler}.spv")
        set(assembly "${UPT00_ARTIFACT_DIR}/${kernel}.${compiler}.spvasm")
        if(NOT EXISTS "${binary}" OR NOT EXISTS "${assembly}")
            message(FATAL_ERROR "UPT-00 metrics missing ${kernel}.${compiler} artifacts")
        endif()

        file(SIZE "${binary}" spirv_bytes)
        file(SHA256 "${binary}" spirv_sha256)
        file(READ "${assembly}" contents)

        string(REGEX MATCH "; Bound: ([0-9]+)" bound_match "${contents}")
        if(CMAKE_MATCH_1)
            set(id_bound "${CMAKE_MATCH_1}")
        else()
            set(id_bound 0)
        endif()

        count_spirv_regex("${contents}" "[ \t]Op[A-Za-z0-9_]+" operations)
        count_spirv_regex("${contents}" "[ \t]OpFunction[ \t\r\n]" functions)
        count_spirv_regex("${contents}" "[ \t]OpLoad[ \t\r\n]" loads)
        count_spirv_regex("${contents}" "[ \t]OpStore[ \t\r\n]" stores)
        count_spirv_regex("${contents}" "[ \t]OpBranch[ \t\r\n]" branches)
        count_spirv_regex("${contents}" "[ \t]OpBranchConditional[ \t\r\n]" conditional_branches)
        count_spirv_regex("${contents}" "[ \t]OpAccessChain[ \t\r\n]" access_chains)
        count_spirv_regex("${contents}" "[ \t]OpRayQueryInitializeKHR[ \t\r\n]" ray_queries)

        file(APPEND "${UPT00_METRICS_OUTPUT}"
            "${kernel},${compiler},${spirv_bytes},${spirv_sha256},${id_bound},${operations},${functions},${loads},${stores},${branches},${conditional_branches},${access_chains},${ray_queries}\n")
    endforeach()
endforeach()
