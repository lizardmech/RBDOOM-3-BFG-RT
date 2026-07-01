#include "precompiled.h"
#pragma hdrstop

#include "PathTraceMaterialFeatureBindings.h"
#include "PathTraceMaterialFeatureOutputs.h"
#include "PathTraceMaterialFeaturePasses.h"

namespace {

enum class RtPathTraceMaterialFeatureInputBindingKind
{
    StructuredBufferSrv,
    ConstantBuffer
};

struct RtPathTraceMaterialFeatureInputBindingDesc
{
    uint32_t resource = 0;
    uint32_t slot = 0xffffffffu;
    RtPathTraceMaterialFeatureInputBindingKind kind = RtPathTraceMaterialFeatureInputBindingKind::StructuredBufferSrv;
    nvrhi::BufferHandle RtPathTraceMaterialFeatureInputResources::* bufferMember = nullptr;
};

static const RtPathTraceMaterialFeatureInputBindingDesc kMaterialFeatureInputs[] = {
    {
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_SIDECAR,
        80u,
        RtPathTraceMaterialFeatureInputBindingKind::StructuredBufferSrv,
        &RtPathTraceMaterialFeatureInputResources::materialFeatureBuffer
    },
    {
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_RUNTIME_CONSTANTS,
        88u,
        RtPathTraceMaterialFeatureInputBindingKind::ConstantBuffer,
        &RtPathTraceMaterialFeatureInputResources::runtimeConstantsBuffer
    }
};

const RtPathTraceMaterialFeatureInputBindingDesc* FindPathTraceMaterialFeatureInputBindingDesc(uint32_t resource)
{
    for (const RtPathTraceMaterialFeatureInputBindingDesc& desc : kMaterialFeatureInputs)
    {
        if (desc.resource == resource)
        {
            return &desc;
        }
    }
    return nullptr;
}

bool BindingLayoutContains(const nvrhi::BindingLayoutDesc& desc, nvrhi::ResourceType type, uint32_t slot)
{
    for (const nvrhi::BindingLayoutItem& binding : desc.bindings)
    {
        if (binding.type == type && binding.slot == slot)
        {
            return true;
        }
    }
    return false;
}

void AddOrReplaceTextureUavBinding(nvrhi::BindingSetDesc& desc, uint32_t slot, nvrhi::TextureHandle texture)
{
    const nvrhi::BindingSetItem item = nvrhi::BindingSetItem::Texture_UAV(slot, texture);
    for (nvrhi::BindingSetItem& binding : desc.bindings)
    {
        if (binding.slot == slot && binding.type == nvrhi::ResourceType::Texture_UAV)
        {
            binding = item;
            return;
        }
    }
    desc.addItem(item);
}

void AddOrReplaceStructuredBufferSrvBinding(nvrhi::BindingSetDesc& desc, uint32_t slot, nvrhi::BufferHandle buffer)
{
    const nvrhi::BindingSetItem item = nvrhi::BindingSetItem::StructuredBuffer_SRV(slot, buffer);
    for (nvrhi::BindingSetItem& binding : desc.bindings)
    {
        if (binding.slot == slot && binding.type == nvrhi::ResourceType::StructuredBuffer_SRV)
        {
            binding = item;
            return;
        }
    }
    desc.addItem(item);
}

void AddOrReplaceConstantBufferBinding(nvrhi::BindingSetDesc& desc, uint32_t slot, nvrhi::BufferHandle buffer)
{
    const nvrhi::BindingSetItem item = nvrhi::BindingSetItem::ConstantBuffer(slot, buffer);
    for (nvrhi::BindingSetItem& binding : desc.bindings)
    {
        if (binding.slot == slot && binding.type == nvrhi::ResourceType::ConstantBuffer)
        {
            binding = item;
            return;
        }
    }
    desc.addItem(item);
}

}

void AddPathTraceMaterialFeatureInputLayoutBinding(nvrhi::BindingLayoutDesc& desc, uint32_t resource)
{
    const RtPathTraceMaterialFeatureInputBindingDesc* binding = FindPathTraceMaterialFeatureInputBindingDesc(resource);
    if (!binding || binding->slot == 0xffffffffu)
    {
        return;
    }

    switch (binding->kind)
    {
    case RtPathTraceMaterialFeatureInputBindingKind::StructuredBufferSrv:
        if (!BindingLayoutContains(desc, nvrhi::ResourceType::StructuredBuffer_SRV, binding->slot))
        {
            desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(binding->slot));
        }
        break;
    case RtPathTraceMaterialFeatureInputBindingKind::ConstantBuffer:
        if (!BindingLayoutContains(desc, nvrhi::ResourceType::ConstantBuffer, binding->slot))
        {
            desc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(binding->slot));
        }
        break;
    }
}

void AddPathTraceMaterialFeatureInputLayoutBindings(nvrhi::BindingLayoutDesc& desc, uint32_t resources)
{
    for (uint32_t resource = 1u; resource != 0u; resource <<= 1u)
    {
        if ((resources & resource) != 0u)
        {
            AddPathTraceMaterialFeatureInputLayoutBinding(desc, resource);
        }
    }
}

void AddPathTraceMaterialFeatureInputBinding(nvrhi::BindingSetDesc& desc, const RtPathTraceMaterialFeatureInputResources& resources, uint32_t resource)
{
    const RtPathTraceMaterialFeatureInputBindingDesc* binding = FindPathTraceMaterialFeatureInputBindingDesc(resource);
    if (!binding || binding->slot == 0xffffffffu || !binding->bufferMember)
    {
        return;
    }

    const nvrhi::BufferHandle buffer = resources.*(binding->bufferMember);
    if (!buffer)
    {
        return;
    }

    switch (binding->kind)
    {
    case RtPathTraceMaterialFeatureInputBindingKind::StructuredBufferSrv:
        AddOrReplaceStructuredBufferSrvBinding(desc, binding->slot, buffer);
        break;
    case RtPathTraceMaterialFeatureInputBindingKind::ConstantBuffer:
        AddOrReplaceConstantBufferBinding(desc, binding->slot, buffer);
        break;
    }
}

void AddPathTraceMaterialFeatureInputBindings(nvrhi::BindingSetDesc& desc, const RtPathTraceMaterialFeatureInputResources& resources, uint32_t resourceMask)
{
    for (uint32_t resource = 1u; resource != 0u; resource <<= 1u)
    {
        if ((resourceMask & resource) != 0u)
        {
            AddPathTraceMaterialFeatureInputBinding(desc, resources, resource);
        }
    }
}

void AddPathTraceMaterialFeatureOutputLayoutBinding(nvrhi::BindingLayoutDesc& desc, uint32_t resource)
{
    const RtPathTraceMaterialFeatureOutputDesc* output = FindPathTraceMaterialFeatureOutputDesc(resource);
    if (output && output->uavSlot != 0xffffffffu &&
        !BindingLayoutContains(desc, nvrhi::ResourceType::Texture_UAV, output->uavSlot))
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

    AddOrReplaceTextureUavBinding(
        desc,
        output->uavSlot,
        PathTraceMaterialFeatureOutputTexture(frameResources, resource));
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
