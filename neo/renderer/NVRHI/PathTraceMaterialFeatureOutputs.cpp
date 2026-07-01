#include "precompiled.h"
#pragma hdrstop

#include "PathTraceMaterialFeatureOutputs.h"
#include "PathTraceMaterialFeaturePasses.h"
#include "PathTraceMaterialFeatureRuntime.h"

#include <nvrhi/utils.h>

namespace {

static const RtPathTraceMaterialFeatureOutputDesc kMaterialFeatureOutputs[] = {
    {
        RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR,
        1u,
        "output-color",
        &RtPathTraceFrameResources::outputTexture
    },
    {
        RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_OUTPUT,
        87u,
        "transmission-output",
        &RtPathTraceFrameResources::transmissionTexture
    }
};

}

const RtPathTraceMaterialFeatureOutputDesc* FindPathTraceMaterialFeatureOutputDesc(uint32_t resource)
{
    for (const RtPathTraceMaterialFeatureOutputDesc& desc : kMaterialFeatureOutputs)
    {
        if (desc.resource == resource)
        {
            return &desc;
        }
    }

    return nullptr;
}

nvrhi::TextureHandle PathTraceMaterialFeatureOutputTexture(const RtPathTraceFrameResources& frameResources, uint32_t resource)
{
    const RtPathTraceMaterialFeatureOutputDesc* desc = FindPathTraceMaterialFeatureOutputDesc(resource);
    return desc && desc->textureMember
        ? frameResources.*(desc->textureMember)
        : nullptr;
}

bool PathTraceMaterialFeatureOutputAvailable(const RtPathTraceMaterialFeaturePassDesc& passDesc, const RtPathTraceFrameResources& frameResources, uint32_t resource)
{
    return !PathTraceMaterialFeaturePassWritesAnyOutput(passDesc, resource) ||
        PathTraceMaterialFeatureOutputTexture(frameResources, resource);
}

bool PathTraceMaterialFeatureOutputsAvailable(const RtPathTraceMaterialFeaturePassDesc& passDesc, const RtPathTraceFrameResources& frameResources)
{
    for (uint32_t resource = 1u; resource != 0u; resource <<= 1u)
    {
        if (!PathTraceMaterialFeatureOutputAvailable(passDesc, frameResources, resource))
        {
            return false;
        }
    }
    return true;
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

void SetPathTraceMaterialFeatureOutputsState(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    const RtPathTraceFrameResources& frameResources,
    nvrhi::ResourceStates state)
{
    for (uint32_t resource = 1u; resource != 0u; resource <<= 1u)
    {
        if (PathTraceMaterialFeaturePassWritesAnyOutput(passDesc, resource))
        {
            SetPathTraceMaterialFeatureOutputState(commandList, passDesc, frameResources, resource, state);
        }
    }
}

void SetPathTraceMaterialFeatureOutputsState(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeatureRuntimePass& pass,
    const RtPathTraceFrameResources& frameResources,
    nvrhi::ResourceStates state)
{
    if (pass.ready)
    {
        SetPathTraceMaterialFeatureOutputsState(commandList, pass.desc, frameResources, state);
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
