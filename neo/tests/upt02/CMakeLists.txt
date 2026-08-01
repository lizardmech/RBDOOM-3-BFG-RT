add_executable(PathTraceUnifiedReservoirCodecHarness
	"upt02_unified_reservoir_codec_test.cpp"
	"upt02_unified_reservoir_codec.h"
	"upt02_random_dimensions.def")
target_compile_features(PathTraceUnifiedReservoirCodecHarness PRIVATE cxx_std_17)
set_target_properties(PathTraceUnifiedReservoirCodecHarness PROPERTIES FOLDER "Tests/PathTracing")

if(BUILD_TESTING)
	add_test(
		NAME PathTraceUnifiedReservoirCodecHarness
		COMMAND $<TARGET_FILE:PathTraceUnifiedReservoirCodecHarness>)
endif()
