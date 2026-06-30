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

RtPathTraceMaterialFeatureRuntimePass BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(
    bool cleanRouteRequested,
    int cleanView,
    bool producerRequested,
    bool debugOutputRequested,
    const RtPathTraceMaterialFeatureShaderState* shaderStates,
    size_t shaderStateCount)
{
    return BuildPathTraceMaterialFeatureRuntimePass(
        BuildPathTraceCleanRtxdiDiTransmissionFeaturePassDesc(
            cleanRouteRequested,
            cleanView,
            producerRequested,
            debugOutputRequested),
        shaderStates,
        shaderStateCount);
}

const char* PathTraceMaterialFeatureShaderPathForGraphicsApi(
    const RtPathTraceMaterialFeatureShaderDesc& shaderDesc,
    nvrhi::GraphicsAPI graphicsApi)
{
    switch (graphicsApi)
    {
    case nvrhi::GraphicsAPI::D3D12:
        return shaderDesc.dxilShaderPath;
    case nvrhi::GraphicsAPI::VULKAN:
        return shaderDesc.spirvShaderPath;
    default:
        return nullptr;
    }
}

void SetPathTraceMaterialFeatureRuntimeInfo(float runtimeInfo[4], const RtPathTraceMaterialFeaturePassDesc& desc, bool passReady)
{
    runtimeInfo[0] = PathTraceMaterialFeaturePassWritesAnyOutput(desc, RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR) ? 1.0f : 0.0f;
    runtimeInfo[1] = passReady ? 1.0f : 0.0f;
}

void SetPathTraceMaterialFeatureRuntimeInfo(float runtimeInfo[4], const RtPathTraceMaterialFeatureRuntimePass& pass)
{
    SetPathTraceMaterialFeatureRuntimeInfo(runtimeInfo, pass.desc, pass.ready);
}
