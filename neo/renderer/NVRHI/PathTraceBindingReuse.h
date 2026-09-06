#pragma once

#include <nvrhi/nvrhi.h>
#include <array>
#include <cstdint>

enum class RtPathTraceBindingMismatch : uint32_t
{
    Equal, MissingReceipt, Device, Layout, Liveness, Count, Item
};

struct RtPathTraceBindingComparison
{
    RtPathTraceBindingMismatch reason = RtPathTraceBindingMismatch::MissingReceipt;
    uint32_t item = UINT32_MAX;
    uint32_t slot = UINT32_MAX;
};

// MainThread-owned receipt of the exact caller-level createBindingSet arguments.
// Validation may unwrap handles in IBindingSet::getDesc(); that backend descriptor
// is not the caller's identity authority. Strong refs keep wrapper identities alive.
struct RtPathTraceBindingReuseReceipt
{
    const void* deviceIdentity = nullptr;
    nvrhi::BindingLayoutHandle layout;
    nvrhi::BindingSetHandle bindingSet;
    nvrhi::BindingSetDesc input;
    std::array<nvrhi::RefCountPtr<nvrhi::IResource>, nvrhi::c_MaxBindingsPerLayout> resources;

    RtPathTraceBindingComparison Compare(const void* device, nvrhi::IBindingLayout* candidateLayout,
        const nvrhi::BindingSetDesc& candidate) const
    {
        if (!bindingSet) return {};
        if (deviceIdentity != device) return { RtPathTraceBindingMismatch::Device };
        if (layout != candidateLayout || bindingSet->getLayout() != candidateLayout)
            return { RtPathTraceBindingMismatch::Layout };
        if (input.trackLiveness != candidate.trackLiveness)
            return { RtPathTraceBindingMismatch::Liveness };
        if (input.bindings.size() != candidate.bindings.size())
            return { RtPathTraceBindingMismatch::Count };
        for (uint32_t i = 0; i < input.bindings.size(); ++i)
            if (input.bindings[i] != candidate.bindings[i])
                return { RtPathTraceBindingMismatch::Item, i, candidate.bindings[i].slot };
        return { RtPathTraceBindingMismatch::Equal };
    }

    void Capture(const void* device, nvrhi::IBindingLayout* createdLayout,
        const nvrhi::BindingSetDesc& createdInput, nvrhi::IBindingSet* createdSet)
    {
        // Used only with a local candidate after successful creation; no publication.
        deviceIdentity = device;
        layout = createdLayout;
        bindingSet = createdSet;
        input = createdInput;
        for (size_t i = 0; i < resources.size(); ++i)
            resources[i] = i < input.bindings.size() ? input.bindings[i].resourceHandle : nullptr;
    }
};
static_assert(sizeof(RtPathTraceBindingReuseReceipt) <= 16 * 1024,
    "three retained receipts plus one candidate must fit the reviewed 64 KiB metadata bound");
