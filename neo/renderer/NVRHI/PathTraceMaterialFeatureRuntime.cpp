#include "precompiled.h"
#pragma hdrstop

#include "PathTraceMaterialFeatureRuntime.h"
#include "PathTracePrimaryPass.h"

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
