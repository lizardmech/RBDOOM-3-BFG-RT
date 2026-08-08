add_executable(PathTraceUnifiedDuplicationContractHarness
    "upt08_duplication_contract_test.cpp")
target_compile_features(PathTraceUnifiedDuplicationContractHarness PRIVATE cxx_std_17)
set_target_properties(PathTraceUnifiedDuplicationContractHarness PROPERTIES
    FOLDER "Tests/PathTracing")

if(BUILD_TESTING)
    add_test(
        NAME PathTraceUnifiedDuplicationContractHarness
        COMMAND $<TARGET_FILE:PathTraceUnifiedDuplicationContractHarness>)
endif()
