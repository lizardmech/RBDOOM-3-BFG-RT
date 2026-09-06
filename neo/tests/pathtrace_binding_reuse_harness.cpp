#include "PathTraceBindingReuse.h"
#include <cstdio>
#include <utility>

namespace
{
int failures = 0;
void Check(bool ok, const char* name)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
struct Resource : nvrhi::RefCounter<nvrhi::IResource>
{
    int* destroyed;
    explicit Resource(int* count) : destroyed(count) {}
    ~Resource() override { if (destroyed) ++*destroyed; }
};
struct Layout : nvrhi::RefCounter<nvrhi::IBindingLayout>
{
    nvrhi::BindingLayoutDesc desc;
    const nvrhi::BindingLayoutDesc* getDesc() const override { return &desc; }
    const nvrhi::BindlessLayoutDesc* getBindlessDesc() const override { return nullptr; }
};
struct Binding : nvrhi::RefCounter<nvrhi::IBindingSet>
{
    nvrhi::BindingSetDesc desc;
    nvrhi::BindingLayoutHandle layout;
    const nvrhi::BindingSetDesc* getDesc() const override { return &desc; }
    nvrhi::IBindingLayout* getLayout() const override { return layout; }
};
}

int main()
{
    using Reason = RtPathTraceBindingMismatch;
    int deviceA = 0, deviceB = 0, wrapperDestroyed = 0;
    nvrhi::RefCountPtr<nvrhi::IResource> wrapper, underlying;
    wrapper.Attach(new Resource(&wrapperDestroyed));
    underlying.Attach(new Resource(nullptr));
    nvrhi::BindingLayoutHandle layout, otherLayout;
    layout.Attach(new Layout); otherLayout.Attach(new Layout);
    nvrhi::BindingSetDesc input;
    auto item = nvrhi::BindingSetItem::RayTracingAccelStruct(0, nullptr);
    item.resourceHandle = wrapper;
    input.addItem(item);
    input.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, nullptr));
    nvrhi::RefCountPtr<Binding> binding;
    binding.Attach(new Binding);
    binding->layout = layout;
    binding->desc = input;
    binding->desc.bindings[0].resourceHandle = underlying;
    Check(binding->desc != input, "validation-unwrapped descriptor reproduces old false mismatch");
    RtPathTraceBindingReuseReceipt committed;
    Check(committed.Compare(&deviceA, layout, input).reason == Reason::MissingReceipt, "empty receipt rejects");
    committed.Capture(&deviceA, layout, input, binding);
    Check(committed.Compare(&deviceA, layout, input).reason == Reason::Equal,
        "caller-level receipt reuses unchanged wrapped TLAS without backend descriptor equality");
    Check(committed.Compare(&deviceB, layout, input).reason == Reason::Device, "device change rejects");
    Check(committed.Compare(&deviceA, otherLayout, input).reason == Reason::Layout, "layout change rejects");
    auto changed = input;
    changed.trackLiveness = !changed.trackLiveness;
    Check(committed.Compare(&deviceA, layout, changed).reason == Reason::Liveness, "liveness change rejects");
    changed = input; changed.bindings.pop_back();
    Check(committed.Compare(&deviceA, layout, changed).reason == Reason::Count, "binding count change rejects");
    for (int field = 0; field < 7; ++field)
    {
        changed = input;
        auto& v = changed.bindings[0];
        switch (field)
        {
        case 0: v.resourceHandle = underlying; break;
        case 1: v.slot = 99; break;
        case 2: v.type = nvrhi::ResourceType::StructuredBuffer_SRV; break;
        case 3: v.dimension = nvrhi::TextureDimension::TextureCube; break;
        case 4: v.format = nvrhi::Format::R32_UINT; break;
        case 5: v.rawData[0] ^= 1; break;
        case 6: v.rawData[1] ^= 1; break;
        }
        const auto diff = committed.Compare(&deviceA, layout, changed);
        Check(diff.reason == Reason::Item && diff.item == 0 && diff.slot == v.slot,
            "each descriptor field rejects with exact item and slot");
    }
    changed = input; std::swap(changed.bindings[0], changed.bindings[1]);
    Check(committed.Compare(&deviceA, layout, changed).reason == Reason::Item, "ordered binding changes reject");
    wrapper = nullptr;
    Check(wrapperDestroyed == 0 && committed.Compare(&deviceA, layout, input).reason == Reason::Equal,
        "receipt strongly retains caller wrapper after original owner releases it");
    {
        RtPathTraceBindingReuseReceipt candidate = committed;
        candidate.input.trackLiveness = false;
    }
    Check(committed.Compare(&deviceA, layout, input).reason == Reason::Equal && wrapperDestroyed == 0,
        "discarding local candidate preserves committed receipt and owner");
    RtPathTraceBindingReuseReceipt slots[3];
    for (auto& slot : slots) slot = committed;
    committed = {};
    for (auto& slot : slots)
        Check(slot.Compare(&deviceA, layout, input).reason == Reason::Equal, "paired slot owns independent receipt");
    slots[0] = {}; slots[1] = {};
    Check(wrapperDestroyed == 0, "partial slot clear preserves remaining receipt ownership");
    slots[2] = {};
    Check(wrapperDestroyed == 1, "final receipt clear releases wrapper exactly once");
    return failures ? 1 : 0;
}
