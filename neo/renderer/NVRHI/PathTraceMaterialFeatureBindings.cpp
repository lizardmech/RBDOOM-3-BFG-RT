#include "precompiled.h"
#pragma hdrstop

#include "PathTraceMaterialFeatureBindings.h"
#include "PathTraceMaterialFeatureOutputs.h"
#include "PathTraceMaterialFeaturePasses.h"

nvrhi::BindingLayoutHandle PathTraceMaterialFeatureBindingLayoutHandle(
    RtPathTraceMaterialFeatureBindingLayout bindingLayout,
    nvrhi::BindingLayoutHandle coreSmokeBindingLayout,
    nvrhi::BindingLayoutHandle cleanRtxdiDiBindingLayout)
{
    switch (bindingLayout)
    {
    case RtPathTraceMaterialFeatureBindingLayout::CleanRtxdiDi:
        return cleanRtxdiDiBindingLayout;
    case RtPathTraceMaterialFeatureBindingLayout::CoreSmoke:
        return coreSmokeBindingLayout;
    default:
        return nullptr;
    }
}

void AddPathTraceMaterialFeatureOutputLayoutBinding(nvrhi::BindingLayoutDesc& desc, uint32_t resource)
{
    const uint32_t slot = PathTraceMaterialFeatureOutputUavSlot(resource);
    if (slot != UINT32_MAX)
    {
        desc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(slot));
    }
}

void AddPathTraceMaterialFeatureOutputLayoutBindings(nvrhi::BindingLayoutDesc& desc, uint32_t resources)
{
    for (uint32_t resource = 1u; resource != 0u; resource <<= 1u)
    {
        if ((resources & resource) != 0u)
        {
            AddPathTraceMaterialFeatureOutputLayoutBinding(desc, resource);
        }
    }
}

void AddPathTraceMaterialFeatureOutputLayoutBindings(nvrhi::BindingLayoutDesc& desc, RtPathTraceMaterialFeatureBindingLayout bindingLayout)
{
    AddPathTraceMaterialFeatureOutputLayoutBindings(desc, PathTraceMaterialFeatureBindingLayoutOptionalOutputs(bindingLayout));
}

void AddPathTraceCleanRtxdiDiMaterialFeatureOutputLayoutBindings(nvrhi::BindingLayoutDesc& desc)
{
    AddPathTraceMaterialFeatureOutputLayoutBindings(desc, RtPathTraceMaterialFeatureBindingLayout::CleanRtxdiDi);
}

void AddPathTraceMaterialFeatureOutputBinding(nvrhi::BindingSetDesc& desc, const RtPathTraceFrameResources& frameResources, uint32_t resource)
{
    const uint32_t slot = PathTraceMaterialFeatureOutputUavSlot(resource);
    if (slot == UINT32_MAX)
    {
        return;
    }

    desc.addItem(nvrhi::BindingSetItem::Texture_UAV(slot, PathTraceMaterialFeatureOutputTexture(frameResources, resource)));
}

void AddPathTraceMaterialFeatureOutputBindings(nvrhi::BindingSetDesc& desc, const RtPathTraceFrameResources& frameResources, uint32_t resources)
{
    for (uint32_t resource = 1u; resource != 0u; resource <<= 1u)
    {
        if ((resources & resource) != 0u)
        {
            AddPathTraceMaterialFeatureOutputBinding(desc, frameResources, resource);
        }
    }
}

void AddPathTraceMaterialFeatureOutputBindings(nvrhi::BindingSetDesc& desc, const RtPathTraceFrameResources& frameResources, RtPathTraceMaterialFeatureBindingLayout bindingLayout)
{
    AddPathTraceMaterialFeatureOutputBindings(desc, frameResources, PathTraceMaterialFeatureBindingLayoutOptionalOutputs(bindingLayout));
}

void AddPathTraceCleanRtxdiDiMaterialFeatureOutputBindings(nvrhi::BindingSetDesc& desc, const RtPathTraceFrameResources& frameResources)
{
    AddPathTraceMaterialFeatureOutputBindings(desc, frameResources, RtPathTraceMaterialFeatureBindingLayout::CleanRtxdiDi);
}
