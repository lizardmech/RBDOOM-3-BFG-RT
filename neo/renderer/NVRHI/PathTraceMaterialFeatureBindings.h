#pragma once

// NVRHI binding helpers for material feature pass outputs.
//
// Feature pass descriptors own the logical resource contract; this module
// translates those resources into concrete binding layout/set entries.

#include <cstdint>

#include <nvrhi/nvrhi.h>

struct RtPathTraceFrameResources;
struct RtPathTraceMaterialFeaturePassRegistration;

struct RtPathTraceMaterialFeatureInputResources
{
    nvrhi::BufferHandle currentPrimarySurfaceBuffer;
    nvrhi::BufferHandle materialTableBuffer;
    nvrhi::BufferHandle materialFeatureBuffer;
    nvrhi::BufferHandle materialFeatureParameterBuffer;
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
void AddPathTraceMaterialFeatureRegistrationLayoutBindings(
    nvrhi::BindingLayoutDesc& desc,
    const RtPathTraceMaterialFeaturePassRegistration& registration);
void AddPathTraceMaterialFeatureRegistrationListLayoutBindings(
    nvrhi::BindingLayoutDesc& desc,
    const RtPathTraceMaterialFeaturePassRegistration* registrations,
    size_t registrationCount);
void AddPathTraceMaterialFeatureRegistrationBindings(
    nvrhi::BindingSetDesc& desc,
    const RtPathTraceMaterialFeatureInputResources& resources,
    const RtPathTraceFrameResources& frameResources,
    const RtPathTraceMaterialFeaturePassRegistration& registration);
void AddPathTraceMaterialFeatureRegistrationListBindings(
    nvrhi::BindingSetDesc& desc,
    const RtPathTraceMaterialFeatureInputResources& resources,
    const RtPathTraceFrameResources& frameResources,
    const RtPathTraceMaterialFeaturePassRegistration* registrations,
    size_t registrationCount);
