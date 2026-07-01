#include "precompiled.h"
#pragma hdrstop

#include "PathTraceMaterialFeatureOutputs.h"
#include "PathTraceMaterialFeaturePasses.h"
#include "PathTraceMaterialFeatureRuntime.h"

#include <nvrhi/utils.h>

nvrhi::TextureHandle PathTraceMaterialFeatureOutputTexture(const RtPathTraceFrameResources& frameResources, uint32_t resource)
{
    switch (resource)
    {
    case RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR:
        return frameResources.outputTexture;
    case RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_OUTPUT:
        return frameResources.transmissionTexture;
    default:
        return nullptr;
    }
}

bool PathTraceMaterialFeatureOutputAvailable(const RtPathTraceMaterialFeaturePassDesc& passDesc, const RtPathTraceFrameResources& frameResources, uint32_t resource)
{
    return !PathTraceMaterialFeaturePassWritesAnyOutput(passDesc, resource) ||
        PathTraceMaterialFeatureOutputTexture(frameResources, resource);
}

bool PathTraceMaterialFeaturePrimaryOutputAvailable(const RtPathTraceMaterialFeaturePassDesc& passDesc, const RtPathTraceFrameResources& frameResources)
{
    return PathTraceMaterialFeatureOutputAvailable(passDesc, frameResources, passDesc.primaryOutputResource);
}

bool PathTraceMaterialFeaturePrimaryOutputAvailable(const RtPathTraceMaterialFeatureRuntimePass& pass, const RtPathTraceFrameResources& frameResources)
{
    return !pass.ready || PathTraceMaterialFeaturePrimaryOutputAvailable(pass.desc, frameResources);
}

void SetPathTraceMaterialFeatureOutputState(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    const RtPathTraceFrameResources& frameResources,
    uint32_t resource,
    nvrhi::ResourceStates state)
{
    if (!PathTraceMaterialFeaturePassWritesAnyOutput(passDesc, resource))
    {
        return;
    }

    const nvrhi::TextureHandle texture = PathTraceMaterialFeatureOutputTexture(frameResources, resource);
    if (commandList && texture)
    {
        commandList->setTextureState(texture, nvrhi::AllSubresources, state);
    }
}

void SetPathTraceMaterialFeaturePrimaryOutputState(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    const RtPathTraceFrameResources& frameResources,
    nvrhi::ResourceStates state)
{
    SetPathTraceMaterialFeatureOutputState(commandList, passDesc, frameResources, passDesc.primaryOutputResource, state);
}

void SetPathTraceMaterialFeaturePrimaryOutputState(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeatureRuntimePass& pass,
    const RtPathTraceFrameResources& frameResources,
    nvrhi::ResourceStates state)
{
    if (pass.ready)
    {
        SetPathTraceMaterialFeaturePrimaryOutputState(commandList, pass.desc, frameResources, state);
    }
}

void ClearPathTraceMaterialFeatureOutput(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    const RtPathTraceFrameResources& frameResources,
    uint32_t resource,
    const nvrhi::Color& color)
{
    if (!PathTraceMaterialFeaturePassWritesAnyOutput(passDesc, resource))
    {
        return;
    }

    const nvrhi::TextureHandle texture = PathTraceMaterialFeatureOutputTexture(frameResources, resource);
    if (commandList && texture)
    {
        commandList->clearTextureFloat(texture, nvrhi::AllSubresources, color);
    }
}

void ClearPathTraceMaterialFeaturePrimaryOutput(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    const RtPathTraceFrameResources& frameResources,
    const nvrhi::Color& color)
{
    ClearPathTraceMaterialFeatureOutput(commandList, passDesc, frameResources, passDesc.primaryOutputResource, color);
}

void ClearPathTraceMaterialFeaturePrimaryOutput(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeatureRuntimePass& pass,
    const RtPathTraceFrameResources& frameResources,
    const nvrhi::Color& color)
{
    if (pass.ready)
    {
        ClearPathTraceMaterialFeaturePrimaryOutput(commandList, pass.desc, frameResources, color);
    }
}

void BarrierPathTraceMaterialFeatureOutput(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    const RtPathTraceFrameResources& frameResources,
    uint32_t resource)
{
    if (!PathTraceMaterialFeaturePassWritesAnyOutput(passDesc, resource))
    {
        return;
    }

    const nvrhi::TextureHandle texture = PathTraceMaterialFeatureOutputTexture(frameResources, resource);
    if (commandList && texture)
    {
        nvrhi::utils::TextureUavBarrier(commandList, texture);
    }
}

void BarrierPathTraceMaterialFeatureOutputs(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    const RtPathTraceFrameResources& frameResources)
{
    for (uint32_t resource = 1u; resource != 0u; resource <<= 1u)
    {
        if (PathTraceMaterialFeaturePassWritesAnyOutput(passDesc, resource))
        {
            BarrierPathTraceMaterialFeatureOutput(commandList, passDesc, frameResources, resource);
        }
    }
}

void BarrierPathTraceMaterialFeatureOutputs(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeatureRuntimePass& pass,
    const RtPathTraceFrameResources& frameResources)
{
    if (pass.ready)
    {
        BarrierPathTraceMaterialFeatureOutputs(commandList, pass.desc, frameResources);
    }
}
