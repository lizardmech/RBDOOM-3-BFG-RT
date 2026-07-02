#include "precompiled.h"
#pragma hdrstop

#include "PathTraceMaterialFeatureParameters.h"

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

RtPathTraceMaterialFeatureParameterRecord BuildSmokeMaterialFeatureParameterRecord(const RtSmokeMaterialUniverseFacts& facts)
{
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
