foreach(required_var
        UPT42_REFLECTION
        UPT42_DISASSEMBLY
        UPT42_THREE_VERTEX_REFLECTION
        UPT42_HOST_SOURCE
        UPT42_SHADER_SOURCE)
    if(NOT DEFINED ${required_var} OR NOT EXISTS "${${required_var}}")
        message(FATAL_ERROR "UPT-42 missing ${required_var}: '${${required_var}}'")
    endif()
endforeach()
if(NOT DEFINED UPT42_STAMP OR UPT42_STAMP STREQUAL "")
    message(FATAL_ERROR "UPT-42 missing UPT42_STAMP")
endif()

file(READ "${UPT42_REFLECTION}" reflection)
file(READ "${UPT42_THREE_VERTEX_REFLECTION}" three_vertex_reflection)
file(READ "${UPT42_DISASSEMBLY}" disassembly)
file(READ "${UPT42_HOST_SOURCE}" host_source)
file(READ "${UPT42_SHADER_SOURCE}" shader_source)

foreach(reflection_text reflection three_vertex_reflection)
    if(NOT "${${reflection_text}}" MATCHES
            "gUpt09ReuseTextureDeltas[\n\r\t ]*\"[,\n\r\t ]+\"readonly\"[\n\r\t ]*:[\n\r\t ]*true")
        message(FATAL_ERROR "UPT-42 reuse map is not reflected as a read-only resource")
    endif()
    if(NOT "${${reflection_text}}" MATCHES
            "\"binding\"[\n\r\t ]*:[\n\r\t ]*29")
        message(FATAL_ERROR "UPT-42 reuse map binding 29 missing")
    endif()
endforeach()

if(NOT disassembly MATCHES "OpExecutionMode[^\n\r]*LocalSize 8 8 1")
    message(FATAL_ERROR "UPT-42 must use screen-ordered 8x8 dispatch")
endif()
string(FIND "${host_source}" "StructuredBuffer_SRV(29)" host_binding_pos)
string(FIND "${host_source}" "8x8-screen-leaders" host_group_pos)
string(FIND "${host_source}" "32258ull" stale_host_pair_count_pos)
if(host_binding_pos EQUAL -1 OR host_group_pos EQUAL -1)
    message(FATAL_ERROR "UPT-42 host binding or screen dispatch contract missing")
endif()
if(NOT stale_host_pair_count_pos EQUAL -1)
    message(FATAL_ERROR "UPT-42 stale host pair-list dispatch remains")
endif()
if(NOT shader_source MATCHES "UPT42_REUSE_TEXTURE_PAIRING"
        OR NOT shader_source MATCHES "if[ ]*\\(index > partnerIndex\\)"
        OR NOT shader_source MATCHES "Upt42ReuseTextureDelta\\(pixel, 0u\\)")
    message(FATAL_ERROR "UPT-42 shader screen-leader contract missing")
endif()
if(shader_source MATCHES "Upt42ExecuteReuseTexturePair"
        OR shader_source MATCHES "Upt42InverseTransformCoordinate"
        OR shader_source MATCHES "pairBase = 161516u")
    message(FATAL_ERROR "UPT-42 stale shader pair-list path remains")
endif()

file(WRITE "${UPT42_STAMP}"
    "UPT-42 reuse-texture pairing verified: binding29, 8x8 screen leaders, production and three-vertex artifacts.\n")
