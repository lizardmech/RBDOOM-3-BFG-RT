#include "precompiled.h"
#pragma hdrstop

#include "PathTraceMaterialFeatureBindings.h"
#include "PathTraceFrameResources.h"
#include "PathTraceMaterialFeaturePasses.h"

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
