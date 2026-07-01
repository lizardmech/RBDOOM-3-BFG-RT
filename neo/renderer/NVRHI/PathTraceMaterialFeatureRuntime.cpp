#include "precompiled.h"
#pragma hdrstop

#include "PathTraceMaterialFeatureRuntime.h"

RtPathTraceMaterialFeatureRuntimePass BuildPathTraceMaterialFeatureRuntimePass(
    const RtPathTraceMaterialFeaturePassDesc& desc,
    const RtPathTraceMaterialFeatureShaderState* shaderState)
{
    RtPathTraceMaterialFeatureRuntimePass pass;
    pass.desc = desc;
    pass.shader = shaderState;
    pass.ready = pass.shader && PathTraceMaterialFeaturePassIsReady(pass.desc);
    return pass;
}

RtPathTraceMaterialFeatureRuntimePass BuildPathTraceMaterialFeatureRuntimePass(
    const RtPathTraceMaterialFeaturePassDesc& desc,
    const RtPathTraceMaterialFeatureShaderState* shaderStates,
    size_t shaderStateCount)
{
    const size_t shaderTableIndex = static_cast<size_t>(desc.shaderTable);
    return BuildPathTraceMaterialFeatureRuntimePass(
        desc,
        shaderStates && shaderTableIndex < shaderStateCount
            ? &shaderStates[shaderTableIndex]
            : nullptr);
}

RtPathTraceMaterialFeatureRuntimePass BuildPathTraceMaterialFeatureRuntimePass(
    const RtPathTraceMaterialFeaturePassDesc& desc,
    const RtPathTraceMaterialFeatureShaderTableState& shaderTableState)
{
    return BuildPathTraceMaterialFeatureRuntimePass(desc, shaderTableState.shaders.data(), shaderTableState.shaders.size());
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

std::string PathTraceMaterialFeatureShaderPathForGraphicsApi(
    const RtPathTraceMaterialFeatureShaderDesc& shaderDesc,
    nvrhi::GraphicsAPI graphicsApi)
{
    if (!shaderDesc.shaderBlobPath)
    {
        return std::string();
    }

    switch (graphicsApi)
    {
    case nvrhi::GraphicsAPI::D3D12:
        return std::string("renderprogs2/dxil/") + shaderDesc.shaderBlobPath;
    case nvrhi::GraphicsAPI::VULKAN:
        return std::string("renderprogs2/spirv/") + shaderDesc.shaderBlobPath;
    default:
        return std::string();
    }
}

RtPathTraceMaterialFeaturePipelineRequest BuildPathTraceMaterialFeaturePipelineRequest(
    const RtPathTraceMaterialFeatureShaderDesc& shaderDesc,
    RtPathTraceMaterialFeatureShaderState* shaderState,
    nvrhi::BindingLayoutHandle bindingLayout,
    nvrhi::GraphicsAPI graphicsApi)
{
    RtPathTraceMaterialFeaturePipelineRequest request;

    request.shaderState = shaderState;
    if (!request.shaderState)
    {
        return request;
    }

    request.shaderDesc = shaderDesc;
    if (!request.shaderDesc.shaderBlobPath)
    {
        request.shaderState = nullptr;
        return request;
    }

    request.bindingLayout = bindingLayout;
    request.shaderPath = PathTraceMaterialFeatureShaderPathForGraphicsApi(request.shaderDesc, graphicsApi);
    if (request.shaderPath.empty())
    {
        request.shaderState = nullptr;
    }
    return request;
}

RtPathTraceMaterialFeatureRuntimeInfo BuildPathTraceMaterialFeatureRuntimeInfo(const RtPathTraceMaterialFeaturePassDesc& desc, bool passReady)
{
    RtPathTraceMaterialFeatureRuntimeInfo typedInfo;
    typedInfo.writesOutputColor = PathTraceMaterialFeaturePassWritesAnyOutput(desc, RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR) ? 1.0f : 0.0f;
    typedInfo.ready = passReady ? 1.0f : 0.0f;
    return typedInfo;
}

RtPathTraceMaterialFeatureRuntimeInfo BuildPathTraceMaterialFeatureRuntimeInfo(const RtPathTraceMaterialFeatureRuntimePass& pass)
{
    return BuildPathTraceMaterialFeatureRuntimeInfo(pass.desc, pass.ready);
}

void PackPathTraceMaterialFeatureRuntimeInfo(float runtimeInfo[4], const RtPathTraceMaterialFeatureRuntimeInfo& typedInfo)
{
    runtimeInfo[0] = typedInfo.writesOutputColor;
    runtimeInfo[1] = typedInfo.ready;
    runtimeInfo[2] = typedInfo.debugMode;
    runtimeInfo[3] = typedInfo.frameIndex;
}

void SetPathTraceMaterialFeatureRuntimeInfo(float runtimeInfo[4], const RtPathTraceMaterialFeaturePassDesc& desc, bool passReady)
{
    PackPathTraceMaterialFeatureRuntimeInfo(
        runtimeInfo,
        BuildPathTraceMaterialFeatureRuntimeInfo(desc, passReady));
}

void SetPathTraceMaterialFeatureRuntimeInfo(float runtimeInfo[4], const RtPathTraceMaterialFeatureRuntimePass& pass)
{
    PackPathTraceMaterialFeatureRuntimeInfo(runtimeInfo, BuildPathTraceMaterialFeatureRuntimeInfo(pass));
}
