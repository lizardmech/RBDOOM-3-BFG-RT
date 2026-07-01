#include "precompiled.h"
#pragma hdrstop

#include "PathTraceMaterialFeatureBindings.h"
#include "PathTraceMaterialFeatureOutputs.h"
#include "PathTraceMaterialFeaturePasses.h"

void AddPathTraceMaterialFeatureOutputLayoutBinding(nvrhi::BindingLayoutDesc& desc, uint32_t resource)
{
    const RtPathTraceMaterialFeatureOutputDesc* output = FindPathTraceMaterialFeatureOutputDesc(resource);
    if (output && output->uavSlot != 0xffffffffu)
    {
        desc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(output->uavSlot));
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
    const RtPathTraceMaterialFeatureOutputDesc* output = FindPathTraceMaterialFeatureOutputDesc(resource);
    if (!output || output->uavSlot == 0xffffffffu)
    {
        return;
    }

    desc.addItem(nvrhi::BindingSetItem::Texture_UAV(
        output->uavSlot,
        PathTraceMaterialFeatureOutputTexture(frameResources, resource)));
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
