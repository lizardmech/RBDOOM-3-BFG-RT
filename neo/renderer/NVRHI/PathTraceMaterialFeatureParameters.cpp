#include "precompiled.h"
#pragma hdrstop

#include "PathTraceMaterialFeatureParameters.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace {

RtPathTraceMaterialFeatureParameterRecord BuildDefaultMaterialFeatureParameters()
{
    RtPathTraceMaterialFeatureParameterRecord params = {};
    return params;
}

static const RtPathTraceMaterialFeatureParameterLaneDesc kObjectGlassParameterLanes[] = {
    { "transmittance.r", RtPathTraceMaterialFeatureParameterVector::Params0, RT_PATH_TRACE_OBJECT_GLASS_PARAM0_TRANSMITTANCE_R },
    { "transmittance.g", RtPathTraceMaterialFeatureParameterVector::Params0, RT_PATH_TRACE_OBJECT_GLASS_PARAM0_TRANSMITTANCE_G },
    { "transmittance.b", RtPathTraceMaterialFeatureParameterVector::Params0, RT_PATH_TRACE_OBJECT_GLASS_PARAM0_TRANSMITTANCE_B },
    { "thickness", RtPathTraceMaterialFeatureParameterVector::Params0, RT_PATH_TRACE_OBJECT_GLASS_PARAM0_THICKNESS },
    { "ior", RtPathTraceMaterialFeatureParameterVector::Params1, RT_PATH_TRACE_OBJECT_GLASS_PARAM1_IOR },
    { "strength", RtPathTraceMaterialFeatureParameterVector::Params1, RT_PATH_TRACE_OBJECT_GLASS_PARAM1_STRENGTH },
    { "reflection-boost", RtPathTraceMaterialFeatureParameterVector::Params1, RT_PATH_TRACE_OBJECT_GLASS_PARAM1_REFLECTION_BOOST },
    { "transmission-floor", RtPathTraceMaterialFeatureParameterVector::Params1, RT_PATH_TRACE_OBJECT_GLASS_PARAM1_TRANSMISSION_FLOOR }
};

static const RtPathTraceMaterialFeatureParameterLaneDesc kLiquidPoolParameterLanes[] = {
    { "reference-transmittance.r", RtPathTraceMaterialFeatureParameterVector::Params0, RT_PATH_TRACE_LIQUID_POOL_PARAM0_REFERENCE_TRANSMITTANCE_R },
    { "reference-transmittance.g", RtPathTraceMaterialFeatureParameterVector::Params0, RT_PATH_TRACE_LIQUID_POOL_PARAM0_REFERENCE_TRANSMITTANCE_G },
    { "reference-transmittance.b", RtPathTraceMaterialFeatureParameterVector::Params0, RT_PATH_TRACE_LIQUID_POOL_PARAM0_REFERENCE_TRANSMITTANCE_B },
    { "optical-depth-scale", RtPathTraceMaterialFeatureParameterVector::Params0, RT_PATH_TRACE_LIQUID_POOL_PARAM0_OPTICAL_DEPTH_SCALE },
    { "coat-roughness", RtPathTraceMaterialFeatureParameterVector::Params1, RT_PATH_TRACE_LIQUID_POOL_PARAM1_COAT_ROUGHNESS },
    { "dielectric-ior", RtPathTraceMaterialFeatureParameterVector::Params1, RT_PATH_TRACE_LIQUID_POOL_PARAM1_DIELECTRIC_IOR },
    { "authored-normal-strength", RtPathTraceMaterialFeatureParameterVector::Params1, RT_PATH_TRACE_LIQUID_POOL_PARAM1_AUTHORED_NORMAL_STRENGTH },
    { "reserved-zero", RtPathTraceMaterialFeatureParameterVector::Params1, RT_PATH_TRACE_LIQUID_POOL_PARAM1_RESERVED_ZERO }
};

float SanitizeLiquidPoolParameter(float value, float defaultValue, float minimumValue, float maximumValue)
{
    return std::isfinite(value)
        ? idMath::ClampFloat(minimumValue, maximumValue, value)
        : defaultValue;
}

}

RtPathTraceMaterialFeatureParameterRecord BuildPathTracePortalWindowMaterialFeatureParameters()
{
    RtPathTraceMaterialFeatureParameterRecord params = {};
    params.params0[RT_PATH_TRACE_OBJECT_GLASS_PARAM0_TRANSMITTANCE_R] = 0.78f;
    params.params0[RT_PATH_TRACE_OBJECT_GLASS_PARAM0_TRANSMITTANCE_G] = 0.92f;
    params.params0[RT_PATH_TRACE_OBJECT_GLASS_PARAM0_TRANSMITTANCE_B] = 1.0f;
    params.params0[RT_PATH_TRACE_OBJECT_GLASS_PARAM0_THICKNESS] = 0.05f;
    params.params1[RT_PATH_TRACE_OBJECT_GLASS_PARAM1_IOR] = 1.45f;
    params.params1[RT_PATH_TRACE_OBJECT_GLASS_PARAM1_STRENGTH] = 1.0f;
    params.params1[RT_PATH_TRACE_OBJECT_GLASS_PARAM1_REFLECTION_BOOST] = 1.15f;
    params.params1[RT_PATH_TRACE_OBJECT_GLASS_PARAM1_TRANSMISSION_FLOOR] = 0.015f;
    return params;
}

RtPathTraceMaterialFeatureParameterRecord BuildPathTraceObjectGlassMaterialFeatureParameters()
{
    RtPathTraceMaterialFeatureParameterRecord params = {};
    params.params0[RT_PATH_TRACE_OBJECT_GLASS_PARAM0_TRANSMITTANCE_R] = 0.82f;
    params.params0[RT_PATH_TRACE_OBJECT_GLASS_PARAM0_TRANSMITTANCE_G] = 0.93f;
    params.params0[RT_PATH_TRACE_OBJECT_GLASS_PARAM0_TRANSMITTANCE_B] = 1.0f;
    params.params0[RT_PATH_TRACE_OBJECT_GLASS_PARAM0_THICKNESS] = 0.08f;
    params.params1[RT_PATH_TRACE_OBJECT_GLASS_PARAM1_IOR] = 1.5f;
    params.params1[RT_PATH_TRACE_OBJECT_GLASS_PARAM1_STRENGTH] = 1.0f;
    params.params1[RT_PATH_TRACE_OBJECT_GLASS_PARAM1_REFLECTION_BOOST] = 1.5f;
    params.params1[RT_PATH_TRACE_OBJECT_GLASS_PARAM1_TRANSMISSION_FLOOR] = 0.02f;
    return params;
}

RtPathTraceMaterialFeatureParameterRecord BuildPathTraceLiquidPoolMaterialFeatureParameters()
{
    RtPathTraceMaterialFeatureParameterRecord params = {};
    params.params0[RT_PATH_TRACE_LIQUID_POOL_PARAM0_REFERENCE_TRANSMITTANCE_R] = 1.0f;
    params.params0[RT_PATH_TRACE_LIQUID_POOL_PARAM0_REFERENCE_TRANSMITTANCE_G] = 1.0f;
    params.params0[RT_PATH_TRACE_LIQUID_POOL_PARAM0_REFERENCE_TRANSMITTANCE_B] = 1.0f;
    params.params0[RT_PATH_TRACE_LIQUID_POOL_PARAM0_OPTICAL_DEPTH_SCALE] = 1.0f;
    params.params1[RT_PATH_TRACE_LIQUID_POOL_PARAM1_COAT_ROUGHNESS] = 0.0f;
    params.params1[RT_PATH_TRACE_LIQUID_POOL_PARAM1_DIELECTRIC_IOR] = 1.5f;
    params.params1[RT_PATH_TRACE_LIQUID_POOL_PARAM1_AUTHORED_NORMAL_STRENGTH] = 0.0f;
    params.params1[RT_PATH_TRACE_LIQUID_POOL_PARAM1_RESERVED_ZERO] = 0.0f;
    return params;
}

RtPathTraceMaterialFeatureParameterRecord SanitizePathTraceLiquidPoolMaterialFeatureParameters(
    const RtPathTraceMaterialFeatureParameterRecord& params)
{
    const RtPathTraceMaterialFeatureParameterRecord defaults = BuildPathTraceLiquidPoolMaterialFeatureParameters();
    RtPathTraceMaterialFeatureParameterRecord result = params;
    for (int component = RT_PATH_TRACE_LIQUID_POOL_PARAM0_REFERENCE_TRANSMITTANCE_R;
         component <= RT_PATH_TRACE_LIQUID_POOL_PARAM0_REFERENCE_TRANSMITTANCE_B;
         ++component)
    {
        result.params0[component] = SanitizeLiquidPoolParameter(
            params.params0[component],
            defaults.params0[component],
            RT_PATH_TRACE_LIQUID_POOL_TRANSMITTANCE_MIN,
            1.0f);
    }
    result.params0[RT_PATH_TRACE_LIQUID_POOL_PARAM0_OPTICAL_DEPTH_SCALE] = SanitizeLiquidPoolParameter(
        params.params0[RT_PATH_TRACE_LIQUID_POOL_PARAM0_OPTICAL_DEPTH_SCALE],
        defaults.params0[RT_PATH_TRACE_LIQUID_POOL_PARAM0_OPTICAL_DEPTH_SCALE],
        0.0f,
        RT_PATH_TRACE_LIQUID_POOL_OPTICAL_DEPTH_MAX);
    result.params1[RT_PATH_TRACE_LIQUID_POOL_PARAM1_COAT_ROUGHNESS] = SanitizeLiquidPoolParameter(
        params.params1[RT_PATH_TRACE_LIQUID_POOL_PARAM1_COAT_ROUGHNESS],
        defaults.params1[RT_PATH_TRACE_LIQUID_POOL_PARAM1_COAT_ROUGHNESS],
        RT_PATH_TRACE_LIQUID_POOL_COAT_ROUGHNESS_MIN,
        RT_PATH_TRACE_LIQUID_POOL_COAT_ROUGHNESS_MAX);
    result.params1[RT_PATH_TRACE_LIQUID_POOL_PARAM1_DIELECTRIC_IOR] = SanitizeLiquidPoolParameter(
        params.params1[RT_PATH_TRACE_LIQUID_POOL_PARAM1_DIELECTRIC_IOR],
        defaults.params1[RT_PATH_TRACE_LIQUID_POOL_PARAM1_DIELECTRIC_IOR],
        RT_PATH_TRACE_LIQUID_POOL_DIELECTRIC_IOR_MIN,
        RT_PATH_TRACE_LIQUID_POOL_DIELECTRIC_IOR_MAX);
    result.params1[RT_PATH_TRACE_LIQUID_POOL_PARAM1_AUTHORED_NORMAL_STRENGTH] = SanitizeLiquidPoolParameter(
        params.params1[RT_PATH_TRACE_LIQUID_POOL_PARAM1_AUTHORED_NORMAL_STRENGTH],
        defaults.params1[RT_PATH_TRACE_LIQUID_POOL_PARAM1_AUTHORED_NORMAL_STRENGTH],
        RT_PATH_TRACE_LIQUID_POOL_AUTHORED_NORMAL_STRENGTH_MIN,
        RT_PATH_TRACE_LIQUID_POOL_AUTHORED_NORMAL_STRENGTH_MAX);
    result.params1[RT_PATH_TRACE_LIQUID_POOL_PARAM1_RESERVED_ZERO] = 0.0f;
    return result;
}

bool PathTraceLiquidPoolMaterialFeatureParametersAreValid(
    const RtPathTraceMaterialFeatureParameterRecord& params)
{
    const RtPathTraceMaterialFeatureParameterRecord sanitized =
        SanitizePathTraceLiquidPoolMaterialFeatureParameters(params);
    return std::memcmp(params.params0, sanitized.params0, sizeof(params.params0)) == 0 &&
        std::memcmp(params.params1, sanitized.params1, sizeof(params.params1)) == 0;
}

bool ValidatePathTraceLiquidPoolMaterialFeatureParameterContract()
{
    const RtPathTraceMaterialFeatureParameterRecord defaults = BuildPathTraceLiquidPoolMaterialFeatureParameters();
    if (!PathTraceLiquidPoolMaterialFeatureParametersAreValid(defaults))
    {
        return false;
    }

    RtPathTraceMaterialFeatureParameterRecord invalid = defaults;
    invalid.params0[0] = std::numeric_limits<float>::quiet_NaN();
    invalid.params0[1] = std::numeric_limits<float>::infinity();
    invalid.params0[2] = -1.0f;
    invalid.params0[3] = -1.0f;
    invalid.params1[0] = 2.0f;
    invalid.params1[1] = 0.5f;
    invalid.params1[2] = std::numeric_limits<float>::quiet_NaN();
    invalid.params1[3] = 7.0f;
    const RtPathTraceMaterialFeatureParameterRecord sanitized =
        SanitizePathTraceLiquidPoolMaterialFeatureParameters(invalid);
    return sanitized.params0[0] == defaults.params0[0] &&
        sanitized.params0[1] == defaults.params0[1] &&
        sanitized.params0[2] == RT_PATH_TRACE_LIQUID_POOL_TRANSMITTANCE_MIN &&
        sanitized.params0[3] == 0.0f &&
        sanitized.params1[0] == RT_PATH_TRACE_LIQUID_POOL_COAT_ROUGHNESS_MAX &&
        sanitized.params1[1] == RT_PATH_TRACE_LIQUID_POOL_DIELECTRIC_IOR_MIN &&
        sanitized.params1[2] == defaults.params1[2] &&
        sanitized.params1[3] == 0.0f;
}

RtPathTraceMaterialFeatureParameterRecord BuildSmokeMaterialFeatureParameterRecord(const RtSmokeMaterialUniverseFacts& facts)
{
    if (facts.liquidFilmCandidate)
    {
        return BuildPathTraceLiquidPoolMaterialFeatureParameters();
    }
    if (facts.portalWindowFallback)
    {
        return BuildPathTracePortalWindowMaterialFeatureParameters();
    }
    if (facts.objectGlassFallback)
    {
        return BuildPathTraceObjectGlassMaterialFeatureParameters();
    }
    return BuildDefaultMaterialFeatureParameters();
}

RtPathTraceMaterialFeatureParameterLayoutDesc PathTraceObjectGlassMaterialFeatureParameterLayout()
{
    return {
        "object-glass-thin-parameters",
        kObjectGlassParameterLanes,
        sizeof(kObjectGlassParameterLanes) / sizeof(kObjectGlassParameterLanes[0])
    };
}

RtPathTraceMaterialFeatureParameterLayoutDesc PathTraceLiquidPoolMaterialFeatureParameterLayout()
{
    return {
        "liquid-pool-v1-parameters",
        kLiquidPoolParameterLanes,
        sizeof(kLiquidPoolParameterLanes) / sizeof(kLiquidPoolParameterLanes[0])
    };
}

void CopyPathTraceMaterialFeatureParametersToRuntimeInfo(
    RtPathTraceMaterialFeatureRuntimeInfo& runtimeInfo,
    const RtPathTraceMaterialFeatureParameterRecord& params)
{
    for (size_t i = 0; i < RT_PATH_TRACE_MATERIAL_FEATURE_PARAMETER_LANE_COUNT; ++i)
    {
        runtimeInfo.featureParams0[i] = params.params0[i];
        runtimeInfo.featureParams1[i] = params.params1[i];
    }
}
