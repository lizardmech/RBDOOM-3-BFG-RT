#include "precompiled.h"
#pragma hdrstop

#include "PathTraceMaterialFeatureRuntime.h"

RtPathTraceMaterialFeatureRuntimePass BuildPathTraceMaterialFeatureRuntimePass(
    const RtPathTraceMaterialFeaturePassDesc& desc,
    const RtPathTraceMaterialFeatureShaderState* shaderStates,
    size_t shaderStateCount)
{
    RtPathTraceMaterialFeatureRuntimePass pass;
    pass.desc = desc;

    const size_t shaderTableIndex = static_cast<size_t>(desc.shaderTable);
    if (shaderStates && shaderTableIndex < shaderStateCount)
    {
        pass.shader = &shaderStates[shaderTableIndex];
    }

    pass.ready = pass.shader && PathTraceMaterialFeaturePassIsReady(pass.desc);
    return pass;
}

void SetPathTraceMaterialFeatureRuntimeInfo(float runtimeInfo[4], const RtPathTraceMaterialFeaturePassDesc& desc, bool passReady)
{
    runtimeInfo[0] = PathTraceMaterialFeaturePassWritesAnyOutput(desc, RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR) ? 1.0f : 0.0f;
    runtimeInfo[1] = passReady ? 1.0f : 0.0f;
}
