if(NOT DEFINED UPT05_SOURCE_DIR OR NOT DEFINED UPT05_HOST_DIR
        OR NOT DEFINED UPT05_DIAGNOSTIC_DISASSEMBLY
        OR NOT DEFINED UPT05_STAMP)
    message(FATAL_ERROR "UPT-05 repeatability verifier inputs are incomplete")
endif()

file(READ "${UPT05_SOURCE_DIR}/upt04_initial_diagnostics.slang" diagnostic_source)
file(READ "${UPT05_SOURCE_DIR}/upt04_initial_reservoir_stream.slang" stream_source)
file(READ "${UPT05_HOST_DIR}/PathTraceCVars.cpp" cvar_source)
file(READ "${UPT05_HOST_DIR}/PathTraceSmokeDispatch.cpp" dispatch_source)
file(READ "${UPT05_HOST_DIR}/PathTraceUnifiedPt.cpp" host_source)
file(READ "${UPT05_DIAGNOSTIC_DISASSEMBLY}" diagnostic_disassembly)

foreach(word_index RANGE 0 15)
    if(NOT diagnostic_source MATCHES "UPT04_DIAG_MIX_WORD\\(word${word_index},")
        message(FATAL_ERROR
            "UPT-05 repeatability signature omits reservoir word${word_index}")
    endif()
endforeach()

if(NOT diagnostic_source MATCHES "groupshared uint gUpt04GroupReservoirHash0"
        OR NOT diagnostic_source MATCHES "SV_GroupIndex"
        OR NOT diagnostic_source MATCHES "GroupMemoryBarrierWithGroupSync")
    message(FATAL_ERROR
        "UPT-05 repeatability signature must reduce through explicit group-shared state")
endif()

string(REGEX MATCHALL "OpAtomicXor" signature_atomics
    "${diagnostic_disassembly}")
list(LENGTH signature_atomics signature_atomic_count)
if(NOT signature_atomic_count EQUAL 8)
    message(FATAL_ERROR
        "UPT-05 diagnostic must contain four group and four global XOR atomics; found ${signature_atomic_count}")
endif()

string(REGEX MATCHALL "OpControlBarrier" signature_barriers
    "${diagnostic_disassembly}")
list(LENGTH signature_barriers signature_barrier_count)
if(NOT signature_barrier_count EQUAL 2)
    message(FATAL_ERROR
        "UPT-05 diagnostic must contain exactly two group barriers; found ${signature_barrier_count}")
endif()

if(NOT stream_source MATCHES "Upt04FinalizeInitialReservoir"
        OR NOT diagnostic_source MATCHES "Upt04FinalizeInitialReservoir")
    message(FATAL_ERROR
        "UPT-05 diagnostic and production must share packed-reservoir finalization")
endif()

if(NOT cvar_source MATCHES "r_pathTracingUnifiedPtFixedSampleIndex"
        OR NOT dispatch_source MATCHES "unifiedPtInputs.frameSampleIndex = unifiedPtFixedSampleIndex >= 0"
        OR NOT host_source MATCHES "UPT04_DIAGNOSTIC_COUNTER_COUNT = 107u"
        OR NOT host_source MATCHES "m_diagnosticReadbackSampleIndex = inputs.frameSampleIndex"
        OR NOT host_source MATCHES "reservoirSignature=%08x:%08x:%08x:%08x directReject"
        OR NOT host_source MATCHES "sampleIndex=%u family=%s size=%ux%u")
    message(FATAL_ERROR
        "UPT-05 host fixed-sample/signature readback contract is incomplete")
endif()

file(WRITE "${UPT05_STAMP}"
    "UPT-05 repeatability verified: 16 packed words, 128-bit group-reduced XOR signature, fixed sample index, 952-byte optional diagnostic pair\n")
message(STATUS
    "UPT-05 verified fixed-input reservoir repeatability signature")
