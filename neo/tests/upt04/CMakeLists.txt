add_executable(PathTraceUnifiedPrimaryScheduleHarness
    "upt04_primary_schedule_test.cpp"
    "../../renderer/NVRHI/PathTraceUnifiedPtSchedule.h")
target_compile_features(PathTraceUnifiedPrimaryScheduleHarness PRIVATE cxx_std_17)
target_include_directories(PathTraceUnifiedPrimaryScheduleHarness PRIVATE
    "../../renderer/NVRHI")
set_target_properties(PathTraceUnifiedPrimaryScheduleHarness PROPERTIES
    FOLDER "Tests/PathTracing")

if(BUILD_TESTING)
    add_test(
        NAME PathTraceUnifiedPrimaryScheduleHarness
        COMMAND $<TARGET_FILE:PathTraceUnifiedPrimaryScheduleHarness>)
endif()
