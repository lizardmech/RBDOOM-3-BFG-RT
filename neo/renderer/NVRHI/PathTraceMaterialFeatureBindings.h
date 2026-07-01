#pragma once

// NVRHI binding helpers for material feature pass outputs.
//
// Feature pass descriptors own the logical resource contract; this module
// translates those resources into concrete binding layout/set entries.

#include <cstdint>

#include <nvrhi/nvrhi.h>

struct RtPathTraceFrameResources;

struct RtPathTraceMaterialFeatureInputResources
{
    nvrhi::BufferHandle materialFeatureBuffer;
    nvrhi::BufferHandle runtimeConstantsBuffer;
};

void AddPathTraceMaterialFeatureInputLayoutBinding(nvrhi::BindingLayoutDesc& desc, uint32_t resource);
void AddPathTraceMaterialFeatureInputLayoutBindings(nvrhi::BindingLayoutDesc& desc, uint32_t resources);
void AddPathTraceMaterialFeatureInputBinding(nvrhi::BindingSetDesc& desc, const RtPathTraceMaterialFeatureInputResources& resources, uint32_t resource);
void AddPathTraceMaterialFeatureInputBindings(nvrhi::BindingSetDesc& desc, const RtPathTraceMaterialFeatureInputResources& resources, uint32_t resourceMask);
void AddPathTraceMaterialFeatureOutputLayoutBinding(nvrhi::BindingLayoutDesc& desc, uint32_t resource);
void AddPathTraceMaterialFeatureOutputLayoutBindings(nvrhi::BindingLayoutDesc& desc, uint32_t resources);
void AddPathTraceMaterialFeatureOutputBinding(nvrhi::BindingSetDesc& desc, const RtPathTraceFrameResources& frameResources, uint32_t resource);
void AddPathTraceMaterialFeatureOutputBindings(nvrhi::BindingSetDesc& desc, const RtPathTraceFrameResources& frameResources, uint32_t resources);
