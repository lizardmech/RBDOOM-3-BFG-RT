#pragma once

// NVRHI binding helpers for material feature pass outputs.
//
// Feature pass descriptors own the logical resource contract; this module
// translates those resources into concrete binding layout/set entries.

#include <cstdint>

#include <nvrhi/nvrhi.h>

struct RtPathTraceFrameResources;

void AddPathTraceMaterialFeatureOutputLayoutBinding(nvrhi::BindingLayoutDesc& desc, uint32_t resource);
void AddPathTraceMaterialFeatureOutputLayoutBindings(nvrhi::BindingLayoutDesc& desc, uint32_t resources);
void AddPathTraceCleanRtxdiDiMaterialFeatureOutputLayoutBindings(nvrhi::BindingLayoutDesc& desc);
void AddPathTraceMaterialFeatureOutputBinding(nvrhi::BindingSetDesc& desc, const RtPathTraceFrameResources& frameResources, uint32_t resource);
void AddPathTraceMaterialFeatureOutputBindings(nvrhi::BindingSetDesc& desc, const RtPathTraceFrameResources& frameResources, uint32_t resources);
void AddPathTraceCleanRtxdiDiMaterialFeatureOutputBindings(nvrhi::BindingSetDesc& desc, const RtPathTraceFrameResources& frameResources);
