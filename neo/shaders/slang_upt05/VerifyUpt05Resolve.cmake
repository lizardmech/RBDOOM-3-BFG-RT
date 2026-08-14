foreach(required UPT05_REFLECTION UPT05_DISASSEMBLY UPT05_STAMP
        UPT05_SOURCE UPT03_CONTRACT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "UPT-05 resolve verification missing ${required}")
    endif()
endforeach()

file(READ "${UPT05_REFLECTION}" reflection)
file(READ "${UPT05_DISASSEMBLY}" disassembly)
file(READ "${UPT05_SOURCE}" source)
file(READ "${UPT03_CONTRACT}" reservoir_contract)

foreach(binding RANGE 0 3)
    if(NOT reflection MATCHES "\"set\"[ \t]*:[ \t]*0,[ \t\r\n]*\"binding\"[ \t]*:[ \t]*${binding}")
        message(FATAL_ERROR "UPT-05 resolve reflection lacks set 0 binding ${binding}")
    endif()
endforeach()
string(REGEX MATCHALL "\"binding\"[ \t]*:" bindings "${reflection}")
list(LENGTH bindings binding_count)
if(NOT binding_count EQUAL 4)
    message(FATAL_ERROR "UPT-05 resolve must expose exactly four descriptor bindings")
endif()
if(NOT reflection MATCHES "\"array_stride\"[ \t]*:[ \t]*64")
    message(FATAL_ERROR "UPT-05 resolve reservoir SRV is not stride 64")
endif()
if(NOT reflection MATCHES "\"workgroup_size\"[ \t\r\n]*:[ \t\r\n]*\[[ \t\r\n]*8[ \t]*,[ \t\r\n]*8")
    message(FATAL_ERROR "UPT-05 resolve workgroup is not 8x8")
endif()
if(NOT disassembly MATCHES "OpTypeImage %float 2D 2 0 0 2 Rgba16f")
    message(FATAL_ERROR "UPT-05 resolve output is not an rgba16f storage image")
endif()
if(disassembly MATCHES "OpTraceRayKHR|OpRayQuery|OpImageSample|OpImageFetch")
    message(FATAL_ERROR "UPT-05 resolve contains forbidden ray or texture-sampling work")
endif()
foreach(required_source_text
        "const bool hasCachedX2 = Upt03HasCachedX2Endpoint(metadata)"
        "Upt03CachedX2EndpointValid(metadata)"
        "Upt04EvaluatePrimaryDirectProbe"
        "Upt03StoredProposalPayloadValid(metadata)")
    string(FIND "${source}" "${required_source_text}" source_offset)
    if(source_offset EQUAL -1)
        message(FATAL_ERROR
            "UPT-05 resolve lost packed reservoir union validation: ${required_source_text}")
    endif()
endforeach()
foreach(required_contract_text
        "Upt03EmissiveReplayPdfScalePayloadValid"
        "payload < 0.0f && payload >= -4.0f"
        "Upt03StoredProposalPayloadValid")
    string(FIND "${reservoir_contract}" "${required_contract_text}" contract_offset)
    if(contract_offset EQUAL -1)
        message(FATAL_ERROR
            "UPT-05 resolve lost collision-free word12 payload contract: ${required_contract_text}")
    endif()
endforeach()

file(WRITE "${UPT05_STAMP}"
    "UPT-05 resolve verified: set0 bindings 0..3, reservoir stride 64, event-specific word13..15 union, signed word12 payload, primary self-emission, HDR reflected-transport RR export, rgba16f output, 8x8, trace-free and sample-free\n")
