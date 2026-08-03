add_executable(PathTraceUnifiedTemporalContractHarness
    "upt07_temporal_contract_test.cpp"
    "upt07_temporal_contract.h"
    "../upt02/upt02_unified_reservoir_codec.h")
target_compile_features(PathTraceUnifiedTemporalContractHarness PRIVATE cxx_std_17)
set_target_properties(PathTraceUnifiedTemporalContractHarness PROPERTIES
    FOLDER "Tests/PathTracing")

if(BUILD_TESTING)
    add_test(
        NAME PathTraceUnifiedTemporalContractHarness
        COMMAND $<TARGET_FILE:PathTraceUnifiedTemporalContractHarness>)
endif()
