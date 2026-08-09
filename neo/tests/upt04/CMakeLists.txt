add_executable(PathTraceUnifiedPrimaryScheduleHarness
    "upt04_primary_schedule_test.cpp"
    "../../renderer/NVRHI/PathTraceUnifiedPtSchedule.h"
    "../../renderer/NVRHI/PathTraceUnifiedPtRandomDimensions.h"
    "../../shaders/PathTraceUnifiedPtRandomDimensions.inc")
target_compile_features(PathTraceUnifiedPrimaryScheduleHarness PRIVATE cxx_std_17)
target_include_directories(PathTraceUnifiedPrimaryScheduleHarness PRIVATE
    "../../renderer/NVRHI")
set_target_properties(PathTraceUnifiedPrimaryScheduleHarness PROPERTIES
    FOLDER "Tests/PathTracing")

add_executable(PathTraceUnifiedLightTileHarness
    "upt04_light_tile_test.cpp")
target_compile_features(PathTraceUnifiedLightTileHarness PRIVATE cxx_std_17)
set_target_properties(PathTraceUnifiedLightTileHarness PROPERTIES
    FOLDER "Tests/PathTracing")

if(BUILD_TESTING)
    add_test(
        NAME PathTraceUnifiedPrimaryScheduleHarness
        COMMAND $<TARGET_FILE:PathTraceUnifiedPrimaryScheduleHarness>)
    add_test(
        NAME PathTraceUnifiedLightTileHarness
        COMMAND $<TARGET_FILE:PathTraceUnifiedLightTileHarness>)
endif()
