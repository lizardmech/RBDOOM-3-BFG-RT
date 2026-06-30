#include "precompiled.h"
#pragma hdrstop

#include "PathTraceMaterialFeatureBindings.h"
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

RtPathTraceMaterialFeatureRuntimePass BuildPathTraceMaterialFeatureRuntimePass(
    const RtPathTraceMaterialFeaturePassDesc& desc,
    const RtPathTraceMaterialFeatureShaderTableState& shaderTableState)
{
    return BuildPathTraceMaterialFeatureRuntimePass(desc, shaderTableState.shaders.data(), shaderTableState.shaders.size());
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

RtPathTraceMaterialFeatureRuntimePass BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(
    bool cleanRouteRequested,
    int cleanView,
    bool producerRequested,
    bool debugOutputRequested,
    const RtPathTraceMaterialFeatureShaderTableState& shaderTableState)
{
    return BuildPathTraceCleanRtxdiDiTransmissionRuntimePass(
        cleanRouteRequested,
        cleanView,
        producerRequested,
        debugOutputRequested,
        shaderTableState.shaders.data(),
        shaderTableState.shaders.size());
}

RtPathTraceMaterialFeatureShaderState* PathTraceMaterialFeatureShaderStateForPass(
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    RtPathTraceMaterialFeatureShaderState* shaderStates,
    size_t shaderStateCount)
{
    const size_t shaderTableIndex = static_cast<size_t>(passDesc.shaderTable);
    if (!shaderStates || shaderTableIndex >= shaderStateCount)
    {
        return nullptr;
    }

    return &shaderStates[shaderTableIndex];
}

RtPathTraceMaterialFeatureShaderState* PathTraceMaterialFeatureShaderStateForPass(
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    RtPathTraceMaterialFeatureShaderTableState& shaderTableState)
{
    return PathTraceMaterialFeatureShaderStateForPass(passDesc, shaderTableState.shaders.data(), shaderTableState.shaders.size());
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

RtPathTraceMaterialFeaturePipelineRequest BuildPathTraceMaterialFeaturePipelineRequest(
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    RtPathTraceMaterialFeatureShaderState* shaderStates,
    size_t shaderStateCount,
    nvrhi::BindingLayoutHandle coreSmokeBindingLayout,
    nvrhi::BindingLayoutHandle cleanRtxdiDiBindingLayout,
    nvrhi::GraphicsAPI graphicsApi)
{
    RtPathTraceMaterialFeaturePipelineRequest request;

    request.shaderState = PathTraceMaterialFeatureShaderStateForPass(passDesc, shaderStates, shaderStateCount);
    if (!request.shaderState)
    {
        return request;
    }

    request.shaderDesc = PathTraceMaterialFeatureShaderDescForTable(passDesc.shaderTable);
    if (!request.shaderDesc.dxilShaderPath || !request.shaderDesc.spirvShaderPath)
    {
        request.shaderState = nullptr;
        return request;
    }

    request.bindingLayout = PathTraceMaterialFeatureBindingLayoutHandle(
        request.shaderDesc.bindingLayout,
        coreSmokeBindingLayout,
        cleanRtxdiDiBindingLayout);
    request.shaderPath = PathTraceMaterialFeatureShaderPathForGraphicsApi(request.shaderDesc, graphicsApi);
    return request;
}

RtPathTraceMaterialFeaturePipelineRequest BuildPathTraceMaterialFeaturePipelineRequest(
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    RtPathTraceMaterialFeatureShaderTableState& shaderTableState,
    nvrhi::BindingLayoutHandle coreSmokeBindingLayout,
    nvrhi::BindingLayoutHandle cleanRtxdiDiBindingLayout,
    nvrhi::GraphicsAPI graphicsApi)
{
    return BuildPathTraceMaterialFeaturePipelineRequest(
        passDesc,
        shaderTableState.shaders.data(),
        shaderTableState.shaders.size(),
        coreSmokeBindingLayout,
        cleanRtxdiDiBindingLayout,
        graphicsApi);
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
