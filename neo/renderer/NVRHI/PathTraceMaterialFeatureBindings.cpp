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

static const RtPathTraceMaterialFeatureBindingDesc kMaterialFeatureCanonicalBindings[] = {
    {
        RT_MATERIAL_FEATURE_RESOURCE_CURRENT_PRIMARY_SURFACE,
        30u,
        RtPathTraceMaterialFeatureBindingKind::StructuredBufferUav,
        "PrimarySurfaceHistoryCurrent"
    },
    {
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_TABLE,
        13u,
        RtPathTraceMaterialFeatureBindingKind::StructuredBufferSrv,
        "PathTraceMaterialTable"
    },
    {
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_SIDECAR,
        80u,
        RtPathTraceMaterialFeatureBindingKind::StructuredBufferSrv,
        "PathTraceMaterialFeatures"
    },
    {
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_PARAMETERS,
        81u,
        RtPathTraceMaterialFeatureBindingKind::StructuredBufferSrv,
        "PathTraceMaterialFeatureParameters"
    },
    {
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_RUNTIME_CONSTANTS,
        88u,
        RtPathTraceMaterialFeatureBindingKind::ConstantBuffer,
        "PathTraceMaterialFeatureRuntimeConstants"
    },
    {
        RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR_SOURCE,
        89u,
        RtPathTraceMaterialFeatureBindingKind::TextureSrv,
        "output-color-source"
    },
    {
        RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_SIDECAR,
        87u,
        RtPathTraceMaterialFeatureBindingKind::TextureSrv,
        "transmission-sidecar"
    },
    {
        RT_MATERIAL_FEATURE_RESOURCE_REFLECTION_SIDECAR,
        90u,
        RtPathTraceMaterialFeatureBindingKind::TextureSrv,
        "reflection-sidecar"
    }
};

static const RtPathTraceMaterialFeatureInputBindingDesc kMaterialFeatureInputs[] = {
    {
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_SIDECAR,
        80u,
        RtPathTraceMaterialFeatureInputBindingKind::StructuredBufferSrv,
        &RtPathTraceMaterialFeatureInputResources::materialFeatureBuffer
    },
    {
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_PARAMETERS,
        81u,
        RtPathTraceMaterialFeatureInputBindingKind::StructuredBufferSrv,
        &RtPathTraceMaterialFeatureInputResources::materialFeatureParameterBuffer
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

void AddOrReplaceTextureSrvBinding(nvrhi::BindingSetDesc& desc, uint32_t slot, nvrhi::TextureHandle texture)
{
    const nvrhi::BindingSetItem item = nvrhi::BindingSetItem::Texture_SRV(slot, texture);
    for (nvrhi::BindingSetItem& binding : desc.bindings)
    {
        if (binding.slot == slot && binding.type == nvrhi::ResourceType::Texture_SRV)
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

void AddOrReplaceStructuredBufferUavBinding(nvrhi::BindingSetDesc& desc, uint32_t slot, nvrhi::BufferHandle buffer)
{
    const nvrhi::BindingSetItem item = nvrhi::BindingSetItem::StructuredBuffer_UAV(slot, buffer);
    for (nvrhi::BindingSetItem& binding : desc.bindings)
    {
        if (binding.slot == slot && binding.type == nvrhi::ResourceType::StructuredBuffer_UAV)
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

nvrhi::BufferHandle PathTraceMaterialFeatureInputResourceBuffer(
    const RtPathTraceMaterialFeatureInputResources& resources,
    uint32_t resource)
{
    switch (resource)
    {
    case RT_MATERIAL_FEATURE_RESOURCE_CURRENT_PRIMARY_SURFACE:
        return resources.currentPrimarySurfaceBuffer;
    case RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_TABLE:
        return resources.materialTableBuffer;
    case RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_SIDECAR:
        return resources.materialFeatureBuffer;
    case RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_PARAMETERS:
        return resources.materialFeatureParameterBuffer;
    case RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_RUNTIME_CONSTANTS:
        return resources.runtimeConstantsBuffer;
    default:
        return nullptr;
    }
}

nvrhi::TextureHandle PathTraceMaterialFeatureInputResourceTexture(
    const RtPathTraceFrameResources& frameResources,
    uint32_t resource)
{
    switch (resource)
    {
    case RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR_SOURCE:
        return frameResources.accumulationTexture;
    case RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_SIDECAR:
        return frameResources.transmissionTexture;
    case RT_MATERIAL_FEATURE_RESOURCE_REFLECTION_SIDECAR:
        return frameResources.reflectionSidecarTexture;
    default:
        return nullptr;
    }
}

void AddPathTraceMaterialFeatureBindingLayoutItem(
    nvrhi::BindingLayoutDesc& desc,
    const RtPathTraceMaterialFeatureBindingDesc& binding)
{
    if (binding.slot == 0xffffffffu)
    {
        return;
    }

    switch (binding.kind)
    {
    case RtPathTraceMaterialFeatureBindingKind::StructuredBufferSrv:
        if (!BindingLayoutContains(desc, nvrhi::ResourceType::StructuredBuffer_SRV, binding.slot))
        {
            desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(binding.slot));
        }
        break;
    case RtPathTraceMaterialFeatureBindingKind::StructuredBufferUav:
        if (!BindingLayoutContains(desc, nvrhi::ResourceType::StructuredBuffer_UAV, binding.slot))
        {
            desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(binding.slot));
        }
        break;
    case RtPathTraceMaterialFeatureBindingKind::ConstantBuffer:
        if (!BindingLayoutContains(desc, nvrhi::ResourceType::ConstantBuffer, binding.slot))
        {
            desc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(binding.slot));
        }
        break;
    case RtPathTraceMaterialFeatureBindingKind::TextureSrv:
        if (!BindingLayoutContains(desc, nvrhi::ResourceType::Texture_SRV, binding.slot))
        {
            desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(binding.slot));
        }
        break;
    case RtPathTraceMaterialFeatureBindingKind::TextureUav:
        if (!BindingLayoutContains(desc, nvrhi::ResourceType::Texture_UAV, binding.slot))
        {
            desc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(binding.slot));
        }
        break;
    case RtPathTraceMaterialFeatureBindingKind::Unknown:
        break;
    }
}

void AddPathTraceMaterialFeatureBindingSetItem(
    nvrhi::BindingSetDesc& desc,
    const RtPathTraceMaterialFeatureInputResources& resources,
    const RtPathTraceFrameResources& frameResources,
    const RtPathTraceMaterialFeatureBindingDesc& binding)
{
    if (binding.slot == 0xffffffffu)
    {
        return;
    }

    switch (binding.kind)
    {
    case RtPathTraceMaterialFeatureBindingKind::StructuredBufferSrv:
        if (nvrhi::BufferHandle buffer = PathTraceMaterialFeatureInputResourceBuffer(resources, binding.resource))
        {
            AddOrReplaceStructuredBufferSrvBinding(desc, binding.slot, buffer);
        }
        break;
    case RtPathTraceMaterialFeatureBindingKind::StructuredBufferUav:
        if (nvrhi::BufferHandle buffer = PathTraceMaterialFeatureInputResourceBuffer(resources, binding.resource))
        {
            AddOrReplaceStructuredBufferUavBinding(desc, binding.slot, buffer);
        }
        break;
    case RtPathTraceMaterialFeatureBindingKind::ConstantBuffer:
        if (nvrhi::BufferHandle buffer = PathTraceMaterialFeatureInputResourceBuffer(resources, binding.resource))
        {
            AddOrReplaceConstantBufferBinding(desc, binding.slot, buffer);
        }
        break;
    case RtPathTraceMaterialFeatureBindingKind::TextureSrv:
        if (nvrhi::TextureHandle texture = PathTraceMaterialFeatureInputResourceTexture(frameResources, binding.resource))
        {
            AddOrReplaceTextureSrvBinding(desc, binding.slot, texture);
        }
        break;
    case RtPathTraceMaterialFeatureBindingKind::TextureUav:
        if (nvrhi::TextureHandle texture = PathTraceMaterialFeatureOutputTexture(frameResources, binding.resource))
        {
            AddOrReplaceTextureUavBinding(desc, binding.slot, texture);
        }
        break;
    case RtPathTraceMaterialFeatureBindingKind::Unknown:
        break;
    }
}

}

const RtPathTraceMaterialFeatureBindingDesc* FindPathTraceMaterialFeatureCanonicalBindingDesc(uint32_t resource)
{
    for (const RtPathTraceMaterialFeatureBindingDesc& desc : kMaterialFeatureCanonicalBindings)
    {
        if (desc.resource == resource)
        {
            return &desc;
        }
    }
    return nullptr;
}

bool BuildPathTraceMaterialFeatureCanonicalBindingDesc(uint32_t resource, RtPathTraceMaterialFeatureBindingDesc& binding)
{
    if (const RtPathTraceMaterialFeatureBindingDesc* canonicalBinding =
        FindPathTraceMaterialFeatureCanonicalBindingDesc(resource))
    {
        binding = *canonicalBinding;
        return true;
    }

    if (const RtPathTraceMaterialFeatureOutputDesc* output = FindPathTraceMaterialFeatureOutputDesc(resource))
    {
        binding.resource = output->resource;
        binding.slot = output->uavSlot;
        binding.kind = RtPathTraceMaterialFeatureBindingKind::TextureUav;
        binding.debugName = output->debugName;
        return output->uavSlot != 0xffffffffu;
    }

    binding = RtPathTraceMaterialFeatureBindingDesc();
    return false;
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

void AddPathTraceMaterialFeatureRegistrationLayoutBindings(
    nvrhi::BindingLayoutDesc& desc,
    const RtPathTraceMaterialFeaturePassRegistration& registration)
{
    if (!registration.bindingMetadata || registration.bindingMetadataCount == 0)
    {
        AddPathTraceMaterialFeatureInputLayoutBindings(desc, registration.passDesc.resourceInputs);
        AddPathTraceMaterialFeatureOutputLayoutBindings(desc, registration.passDesc.resourceOutputs);
        return;
    }

    for (size_t i = 0; i < registration.bindingMetadataCount; ++i)
    {
        const RtPathTraceMaterialFeatureBindingDesc& binding = registration.bindingMetadata[i];
        if ((registration.passDesc.resourceInputs & binding.resource) != 0u ||
            (registration.passDesc.resourceOutputs & binding.resource) != 0u)
        {
            AddPathTraceMaterialFeatureBindingLayoutItem(desc, binding);
        }
    }
}

void AddPathTraceMaterialFeatureRegistrationListLayoutBindings(
    nvrhi::BindingLayoutDesc& desc,
    const RtPathTraceMaterialFeaturePassRegistration* registrations,
    size_t registrationCount)
{
    for (size_t i = 0; registrations && i < registrationCount; ++i)
    {
        AddPathTraceMaterialFeatureRegistrationLayoutBindings(desc, registrations[i]);
    }
}

void AddPathTraceMaterialFeatureRegistrationBindings(
    nvrhi::BindingSetDesc& desc,
    const RtPathTraceMaterialFeatureInputResources& resources,
    const RtPathTraceFrameResources& frameResources,
    const RtPathTraceMaterialFeaturePassRegistration& registration)
{
    if (!registration.bindingMetadata || registration.bindingMetadataCount == 0)
    {
        AddPathTraceMaterialFeatureInputBindings(desc, resources, registration.passDesc.resourceInputs);
        AddPathTraceMaterialFeatureOutputBindings(desc, frameResources, registration.passDesc.resourceOutputs);
        return;
    }

    for (size_t i = 0; i < registration.bindingMetadataCount; ++i)
    {
        const RtPathTraceMaterialFeatureBindingDesc& binding = registration.bindingMetadata[i];
        AddPathTraceMaterialFeatureBindingSetItem(desc, resources, frameResources, binding);
    }
}

void AddPathTraceMaterialFeatureRegistrationListBindings(
    nvrhi::BindingSetDesc& desc,
    const RtPathTraceMaterialFeatureInputResources& resources,
    const RtPathTraceFrameResources& frameResources,
    const RtPathTraceMaterialFeaturePassRegistration* registrations,
    size_t registrationCount)
{
    for (size_t i = 0; registrations && i < registrationCount; ++i)
    {
        AddPathTraceMaterialFeatureRegistrationBindings(
            desc,
            resources,
            frameResources,
            registrations[i]);
    }
}
