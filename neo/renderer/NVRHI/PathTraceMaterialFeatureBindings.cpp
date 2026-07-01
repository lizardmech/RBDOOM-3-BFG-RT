#include "precompiled.h"
#pragma hdrstop

#include "PathTraceMaterialFeatureBindings.h"
#include "PathTraceMaterialFeatureOutputs.h"
#include "PathTraceMaterialFeaturePasses.h"

namespace {

constexpr uint32_t CLEAN_RTXDI_DI_TRANSMISSION_OUTPUT_UAV_SLOT = 87u;

uint32_t PathTraceMaterialFeatureOutputUavSlot(uint32_t resource)
{
    switch (resource)
    {
    case RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR:
        return 1u;
    default:
        return UINT32_MAX;
    }
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

void AddPathTraceCleanRtxdiDiMaterialFeatureOutputLayoutBindings(nvrhi::BindingLayoutDesc& desc)
{
    desc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(CLEAN_RTXDI_DI_TRANSMISSION_OUTPUT_UAV_SLOT));
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

void AddPathTraceCleanRtxdiDiMaterialFeatureOutputBindings(nvrhi::BindingSetDesc& desc, const RtPathTraceFrameResources& frameResources)
{
    desc.addItem(nvrhi::BindingSetItem::Texture_UAV(
        CLEAN_RTXDI_DI_TRANSMISSION_OUTPUT_UAV_SLOT,
        frameResources.transmissionTexture));
}
