#pragma once

// CPU-side defaults for shader-owned material feature parameter records.
// The dynamic material table stores and uploads these rows, but shader feature
// policy belongs here so new material shaders do not edit the table builder.

#include "PathTraceMaterialFeaturePasses.h"
#include "PathTraceMaterialUniverse.h"
#include "PathTracePrimarySurface.h"

enum RtPathTraceObjectGlassMaterialFeatureParam0Lane : uint8_t
{
    RT_PATH_TRACE_OBJECT_GLASS_PARAM0_TRANSMITTANCE_R = 0,
    RT_PATH_TRACE_OBJECT_GLASS_PARAM0_TRANSMITTANCE_G = 1,
    RT_PATH_TRACE_OBJECT_GLASS_PARAM0_TRANSMITTANCE_B = 2,
    RT_PATH_TRACE_OBJECT_GLASS_PARAM0_THICKNESS = 3
};

enum RtPathTraceObjectGlassMaterialFeatureParam1Lane : uint8_t
{
    RT_PATH_TRACE_OBJECT_GLASS_PARAM1_IOR = 0,
    RT_PATH_TRACE_OBJECT_GLASS_PARAM1_STRENGTH = 1,
    RT_PATH_TRACE_OBJECT_GLASS_PARAM1_REFLECTION_BOOST = 2,
    RT_PATH_TRACE_OBJECT_GLASS_PARAM1_TRANSMISSION_FLOOR = 3
};

enum RtPathTraceLiquidPoolMaterialFeatureParam0Lane : uint8_t
{
    RT_PATH_TRACE_LIQUID_POOL_PARAM0_REFERENCE_TRANSMITTANCE_R = 0,
    RT_PATH_TRACE_LIQUID_POOL_PARAM0_REFERENCE_TRANSMITTANCE_G = 1,
    RT_PATH_TRACE_LIQUID_POOL_PARAM0_REFERENCE_TRANSMITTANCE_B = 2,
    RT_PATH_TRACE_LIQUID_POOL_PARAM0_OPTICAL_DEPTH_SCALE = 3
};

enum RtPathTraceLiquidPoolMaterialFeatureParam1Lane : uint8_t
{
    RT_PATH_TRACE_LIQUID_POOL_PARAM1_COAT_ROUGHNESS = 0,
    RT_PATH_TRACE_LIQUID_POOL_PARAM1_DIELECTRIC_IOR = 1,
    RT_PATH_TRACE_LIQUID_POOL_PARAM1_AUTHORED_NORMAL_STRENGTH = 2,
    RT_PATH_TRACE_LIQUID_POOL_PARAM1_RESERVED_ZERO = 3
};

static constexpr uint32_t RT_PATH_TRACE_LIQUID_POOL_PARAMETER_ABI_VERSION = 1;
static constexpr float RT_PATH_TRACE_LIQUID_POOL_TRANSMITTANCE_MIN = 1.0f / 1024.0f;
static constexpr float RT_PATH_TRACE_LIQUID_POOL_OPTICAL_DEPTH_MAX = 8.0f;
static constexpr float RT_PATH_TRACE_LIQUID_POOL_COAT_ROUGHNESS_MIN = 0.0f;
static constexpr float RT_PATH_TRACE_LIQUID_POOL_COAT_ROUGHNESS_MAX = 1.0f;
static constexpr float RT_PATH_TRACE_LIQUID_POOL_DIELECTRIC_IOR_MIN = 1.0f;
static constexpr float RT_PATH_TRACE_LIQUID_POOL_DIELECTRIC_IOR_MAX = 2.5f;
static constexpr float RT_PATH_TRACE_LIQUID_POOL_AUTHORED_NORMAL_STRENGTH_MIN = 0.0f;
static constexpr float RT_PATH_TRACE_LIQUID_POOL_AUTHORED_NORMAL_STRENGTH_MAX = 1.0f;

RtPathTraceMaterialFeatureParameterRecord BuildPathTraceObjectGlassMaterialFeatureParameters();
RtPathTraceMaterialFeatureParameterRecord BuildPathTracePortalWindowMaterialFeatureParameters();
RtPathTraceMaterialFeatureParameterRecord BuildPathTraceLiquidPoolMaterialFeatureParameters();
RtPathTraceMaterialFeatureParameterRecord SanitizePathTraceLiquidPoolMaterialFeatureParameters(
    const RtPathTraceMaterialFeatureParameterRecord& params);
bool PathTraceLiquidPoolMaterialFeatureParametersAreValid(
    const RtPathTraceMaterialFeatureParameterRecord& params);
bool ValidatePathTraceLiquidPoolMaterialFeatureParameterContract();
RtPathTraceMaterialFeatureParameterRecord BuildSmokeMaterialFeatureParameterRecord(const RtSmokeMaterialUniverseFacts& facts);
RtPathTraceMaterialFeatureParameterLayoutDesc PathTraceObjectGlassMaterialFeatureParameterLayout();
RtPathTraceMaterialFeatureParameterLayoutDesc PathTraceLiquidPoolMaterialFeatureParameterLayout();
void CopyPathTraceMaterialFeatureParametersToRuntimeInfo(
    RtPathTraceMaterialFeatureRuntimeInfo& runtimeInfo,
    const RtPathTraceMaterialFeatureParameterRecord& params);
