if(NOT DEFINED UPT00_ARTIFACT_DIR OR NOT DEFINED UPT00_VERIFY_STAMP)
    message(FATAL_ERROR "VerifyUpt00Spirv.cmake requires UPT00_ARTIFACT_DIR and UPT00_VERIFY_STAMP")
endif()

set(kernels structured_math ray_query reservoir_update)
foreach(kernel IN LISTS kernels)
    foreach(compiler slang dxc)
        set(assembly "${UPT00_ARTIFACT_DIR}/${kernel}.${compiler}.spvasm")
        if(NOT EXISTS "${assembly}")
            message(FATAL_ERROR "UPT-00 verification missing ${assembly}")
        endif()

        file(READ "${assembly}" contents)
        if(NOT contents MATCHES "OpEntryPoint GLCompute [^\r\n]*\"main\"")
            message(FATAL_ERROR "${kernel}.${compiler} does not export compute entry point main")
        endif()
        if(NOT contents MATCHES "OpExecutionMode [^\r\n]* LocalSize 64 1 1")
            message(FATAL_ERROR "${kernel}.${compiler} does not use local size 64x1x1")
        endif()

        string(REGEX MATCHALL "ArrayStride 32" stride_matches "${contents}")
        list(LENGTH stride_matches stride_count)
        if(stride_count LESS 2)
            message(FATAL_ERROR
                "${kernel}.${compiler} has ${stride_count} 32-byte structured-buffer strides; expected at least 2")
        endif()

        string(REGEX MATCHALL "Binding [0-9]+" binding_matches "${contents}")
        list(LENGTH binding_matches binding_count)
        if(kernel STREQUAL "ray_query")
            if(binding_count LESS 3)
                message(FATAL_ERROR "${kernel}.${compiler} exposes fewer than 3 descriptor bindings")
            endif()
            if(NOT contents MATCHES "OpCapability RayQueryKHR")
                message(FATAL_ERROR "${kernel}.${compiler} is missing OpCapability RayQueryKHR")
            endif()
            if(NOT contents MATCHES "OpRayQueryInitializeKHR")
                message(FATAL_ERROR "${kernel}.${compiler} does not contain the one-ray query")
            endif()
        elseif(binding_count LESS 2)
            message(FATAL_ERROR "${kernel}.${compiler} exposes fewer than 2 descriptor bindings")
        endif()
    endforeach()
endforeach()

file(WRITE "${UPT00_VERIFY_STAMP}"
    "UPT-00 SPIR-V contract verification passed\n"
    "entry point: main\n"
    "local size: 64 1 1\n"
    "structured strides: 32 bytes\n"
    "ray query: one OpRayQueryInitializeKHR in each ray_query module\n")
