#pragma once

// NVRHI binding helpers for material feature pass outputs.
//
// Feature pass descriptors own the logical resource contract; this module
// translates those resources into concrete binding layout/set entries.

#include <cstdint>

#include <nvrhi/nvrhi.h>

enum class RtPathTraceMaterialFeatureBindingLayout : uint8_t;
struct RtPathTraceFrameResources;

nvrhi::BindingLayoutHandle PathTraceMaterialFeatureBindingLayoutHandle(
    RtPathTraceMaterialFeatureBindingLayout bindingLayout,
    nvrhi::BindingLayoutHandle coreSmokeBindingLayout,
    nvrhi::BindingLayoutHandle cleanRtxdiDiBindingLayout);
void AddPathTraceMaterialFeatureOutputLayoutBinding(nvrhi::BindingLayoutDesc& desc, uint32_t resource);
void AddPathTraceMaterialFeatureOutputLayoutBindings(nvrhi::BindingLayoutDesc& desc, uint32_t resources);
void AddPathTraceMaterialFeatureOutputLayoutBindings(nvrhi::BindingLayoutDesc& desc, RtPathTraceMaterialFeatureBindingLayout bindingLayout);
void AddPathTraceMaterialFeatureOutputBinding(nvrhi::BindingSetDesc& desc, const RtPathTraceFrameResources& frameResources, uint32_t resource);
void AddPathTraceMaterialFeatureOutputBindings(nvrhi::BindingSetDesc& desc, const RtPathTraceFrameResources& frameResources, uint32_t resources);
void AddPathTraceMaterialFeatureOutputBindings(nvrhi::BindingSetDesc& desc, const RtPathTraceFrameResources& frameResources, RtPathTraceMaterialFeatureBindingLayout bindingLayout);
