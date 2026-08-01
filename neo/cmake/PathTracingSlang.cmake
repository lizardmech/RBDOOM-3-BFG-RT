include_guard(GLOBAL)

include(CMakeParseArguments)

function(path_tracing_find_slang_spirv_tools)
    if(NOT SLANGC_EXECUTABLE)
        find_program(SLANGC_EXECUTABLE
            NAMES slangc
            HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
            DOC "Slang compiler used by the isolated Vulkan path-tracing lane")
    endif()
    if(NOT SPIRV_VAL_EXECUTABLE)
        find_program(SPIRV_VAL_EXECUTABLE
            NAMES spirv-val
            HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
            DOC "SPIR-V validator used by the isolated Slang lane")
    endif()
    if(NOT SPIRV_DIS_EXECUTABLE)
        find_program(SPIRV_DIS_EXECUTABLE
            NAMES spirv-dis
            HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
            DOC "SPIR-V disassembler used by the isolated Slang lane")
    endif()
    if(NOT SPIRV_CROSS_EXECUTABLE)
        find_program(SPIRV_CROSS_EXECUTABLE
            NAMES spirv-cross
            HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
            DOC "SPIR-V reflection tool used by compiler comparison targets")
    endif()

    foreach(required_tool
            SLANGC_EXECUTABLE
            SPIRV_VAL_EXECUTABLE
            SPIRV_DIS_EXECUTABLE
            SPIRV_CROSS_EXECUTABLE)
        if(NOT ${required_tool})
            message(FATAL_ERROR
                "UPT-00 requires ${required_tool}; set it explicitly or install the Vulkan SDK")
        endif()
    endforeach()

    set(SLANGC_EXECUTABLE "${SLANGC_EXECUTABLE}" CACHE FILEPATH
        "Slang compiler used by the isolated Vulkan path-tracing lane" FORCE)
    set(SPIRV_VAL_EXECUTABLE "${SPIRV_VAL_EXECUTABLE}" CACHE FILEPATH
        "SPIR-V validator used by the isolated Slang lane" FORCE)
    set(SPIRV_DIS_EXECUTABLE "${SPIRV_DIS_EXECUTABLE}" CACHE FILEPATH
        "SPIR-V disassembler used by the isolated Slang lane" FORCE)
    set(SPIRV_CROSS_EXECUTABLE "${SPIRV_CROSS_EXECUTABLE}" CACHE FILEPATH
        "SPIR-V reflection tool used by compiler comparison targets" FORCE)

    set(SLANGC_EXECUTABLE "${SLANGC_EXECUTABLE}" PARENT_SCOPE)
    set(SPIRV_VAL_EXECUTABLE "${SPIRV_VAL_EXECUTABLE}" PARENT_SCOPE)
    set(SPIRV_DIS_EXECUTABLE "${SPIRV_DIS_EXECUTABLE}" PARENT_SCOPE)
    set(SPIRV_CROSS_EXECUTABLE "${SPIRV_CROSS_EXECUTABLE}" PARENT_SCOPE)
endfunction()

function(path_tracing_declare_slang_spirv_module)
    set(options RAY_QUERY RAY_TRACING)
    set(oneValueArgs NAME SOURCE ENTRY STAGE OUTPUT_DIR OUT_VAR)
    set(multiValueArgs INCLUDE_DIRS DEPENDS)
    cmake_parse_arguments(PTSLANG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    foreach(required_arg NAME SOURCE ENTRY STAGE OUTPUT_DIR OUT_VAR)
        if(NOT PTSLANG_${required_arg})
            message(FATAL_ERROR "path_tracing_declare_slang_spirv_module missing ${required_arg}")
        endif()
    endforeach()

    path_tracing_find_slang_spirv_tools()

    set(output_base "${PTSLANG_OUTPUT_DIR}/${PTSLANG_NAME}.slang")
    set(spirv_output "${output_base}.spv")
    set(reflection_output "${output_base}.reflection.json")
    set(spirv_reflection_output "${output_base}.spirv-reflection.json")
    set(disassembly_output "${output_base}.spvasm")
    set(depfile_output "${output_base}.d")

    set(include_args)
    foreach(include_dir IN LISTS PTSLANG_INCLUDE_DIRS)
        list(APPEND include_args -I "${include_dir}")
    endforeach()

    set(capability_args -capability SPIRV_1_5)
    if(PTSLANG_RAY_QUERY)
        list(APPEND capability_args -capability SPV_KHR_ray_query)
    endif()
    if(PTSLANG_RAY_TRACING)
        list(APPEND capability_args -capability SPV_KHR_ray_tracing)
    endif()

    add_custom_command(
        OUTPUT
            "${spirv_output}"
            "${reflection_output}"
            "${spirv_reflection_output}"
            "${disassembly_output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${PTSLANG_OUTPUT_DIR}"
        COMMAND "${SLANGC_EXECUTABLE}"
            "${PTSLANG_SOURCE}"
            -target spirv
            -profile sm_6_6
            -entry "${PTSLANG_ENTRY}"
            -stage "${PTSLANG_STAGE}"
            -O3
            -matrix-layout-row-major
            -fvk-use-scalar-layout
            -fvk-use-entrypoint-name
            -restrictive-capability-check
            ${capability_args}
            ${include_args}
            -depfile "${depfile_output}"
            -reflection-json "${reflection_output}"
            -o "${spirv_output}"
        COMMAND "${SPIRV_VAL_EXECUTABLE}" --target-env vulkan1.2 "${spirv_output}"
        COMMAND "${SPIRV_DIS_EXECUTABLE}" --no-color -o "${disassembly_output}" "${spirv_output}"
        COMMAND "${SPIRV_CROSS_EXECUTABLE}" "${spirv_output}" --reflect
            --output "${spirv_reflection_output}"
        DEPENDS "${PTSLANG_SOURCE}" ${PTSLANG_DEPENDS}
        DEPFILE "${depfile_output}"
        COMMENT "UPT-00 Slang -> Vulkan SPIR-V: ${PTSLANG_NAME}"
        VERBATIM)

    set(${PTSLANG_OUT_VAR}
        "${spirv_output}"
        "${reflection_output}"
        "${spirv_reflection_output}"
        "${disassembly_output}"
        PARENT_SCOPE)
endfunction()

function(path_tracing_declare_dxc_spirv_comparison_module)
    set(options RAY_QUERY)
    set(oneValueArgs NAME SOURCE ENTRY PROFILE OUTPUT_DIR OUT_VAR)
    set(multiValueArgs INCLUDE_DIRS DEPENDS)
    cmake_parse_arguments(PTDXC "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    foreach(required_arg NAME SOURCE ENTRY PROFILE OUTPUT_DIR OUT_VAR)
        if(NOT PTDXC_${required_arg})
            message(FATAL_ERROR "path_tracing_declare_dxc_spirv_comparison_module missing ${required_arg}")
        endif()
    endforeach()

    path_tracing_find_slang_spirv_tools()
    if(NOT DXC_SPIRV_PATH)
        message(FATAL_ERROR "UPT-00 requires DXC_SPIRV_PATH for its Vulkan-only compiler comparison")
    endif()

    set(output_base "${PTDXC_OUTPUT_DIR}/${PTDXC_NAME}.dxc")
    set(spirv_output "${output_base}.spv")
    set(spirv_reflection_output "${output_base}.spirv-reflection.json")
    set(disassembly_output "${output_base}.spvasm")
    set(depfile_output "${output_base}.d")

    set(include_args)
    foreach(include_dir IN LISTS PTDXC_INCLUDE_DIRS)
        list(APPEND include_args -I "${include_dir}")
    endforeach()

    set(extension_args)
    if(PTDXC_RAY_QUERY)
        list(APPEND extension_args -fspv-extension=SPV_KHR_ray_query)
    endif()

    add_custom_command(
        OUTPUT
            "${spirv_output}"
            "${spirv_reflection_output}"
            "${disassembly_output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${PTDXC_OUTPUT_DIR}"
        COMMAND "${DXC_SPIRV_PATH}"
            -T "${PTDXC_PROFILE}"
            -E "${PTDXC_ENTRY}"
            -spirv
            -fspv-target-env=vulkan1.2
            ${extension_args}
            -fvk-use-scalar-layout
            -fspv-entrypoint-name=${PTDXC_ENTRY}
            -Zpr
            -O3
            ${include_args}
            -MD
            -MF "${depfile_output}"
            -Fo "${spirv_output}"
            "${PTDXC_SOURCE}"
        # This Vulkan SDK DXC treats -MD/-MF as a dependency-only invocation,
        # even when -Fo is present. Compile separately so the depfile remains
        # real rather than replacing dependency tracking with a source glob.
        COMMAND "${DXC_SPIRV_PATH}"
            -T "${PTDXC_PROFILE}"
            -E "${PTDXC_ENTRY}"
            -spirv
            -fspv-target-env=vulkan1.2
            ${extension_args}
            -fvk-use-scalar-layout
            -fspv-entrypoint-name=${PTDXC_ENTRY}
            -Zpr
            -O3
            ${include_args}
            -Fo "${spirv_output}"
            "${PTDXC_SOURCE}"
        COMMAND "${SPIRV_VAL_EXECUTABLE}" --target-env vulkan1.2 "${spirv_output}"
        COMMAND "${SPIRV_DIS_EXECUTABLE}" --no-color -o "${disassembly_output}" "${spirv_output}"
        COMMAND "${SPIRV_CROSS_EXECUTABLE}" "${spirv_output}" --reflect
            --output "${spirv_reflection_output}"
        DEPENDS "${PTDXC_SOURCE}" ${PTDXC_DEPENDS}
        DEPFILE "${depfile_output}"
        COMMENT "UPT-00 DXC -> Vulkan SPIR-V: ${PTDXC_NAME}"
        VERBATIM)

    set(${PTDXC_OUT_VAR}
        "${spirv_output}"
        "${spirv_reflection_output}"
        "${disassembly_output}"
        PARENT_SCOPE)
endfunction()
