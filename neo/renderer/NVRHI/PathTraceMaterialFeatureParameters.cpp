#include "precompiled.h"
#pragma hdrstop

#include "PathTraceMaterialFeatureParameters.h"

namespace {

RtPathTraceMaterialFeatureParameterRecord BuildDefaultMaterialFeatureParameters()
{
    RtPathTraceMaterialFeatureParameterRecord params = {};
    return params;
}

}

RtPathTraceMaterialFeatureParameterRecord BuildPathTracePortalWindowMaterialFeatureParameters()
{
    RtPathTraceMaterialFeatureParameterRecord params = {};
    params.params0[0] = 0.78f;
    params.params0[1] = 0.92f;
    params.params0[2] = 1.0f;
    params.params0[3] = 0.05f;
    params.params1[0] = 1.45f;
    params.params1[1] = 1.0f;
    params.params1[2] = 1.15f;
    params.params1[3] = 0.015f;
    return params;
}

RtPathTraceMaterialFeatureParameterRecord BuildPathTraceObjectGlassMaterialFeatureParameters()
{
    RtPathTraceMaterialFeatureParameterRecord params = {};
    params.params0[0] = 0.82f;
    params.params0[1] = 0.93f;
    params.params0[2] = 1.0f;
    params.params0[3] = 0.08f;
    params.params1[0] = 1.5f;
    params.params1[1] = 1.0f;
    params.params1[2] = 1.5f;
    params.params1[3] = 0.02f;
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

void CopyPathTraceMaterialFeatureParametersToRuntimeInfo(
    RtPathTraceMaterialFeatureRuntimeInfo& runtimeInfo,
    const RtPathTraceMaterialFeatureParameterRecord& params)
{
    for (size_t i = 0; i < 4; ++i)
    {
        runtimeInfo.featureParams0[i] = params.params0[i];
        runtimeInfo.featureParams1[i] = params.params1[i];
    }
}
