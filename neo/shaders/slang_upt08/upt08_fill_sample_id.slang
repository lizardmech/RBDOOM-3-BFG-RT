import upt03_reservoir_contract;

public struct Upt08FillControl
{
    public uint renderWidth;
    public uint renderHeight;
    public uint surfaceCount;
    public uint reserved;
};

public [[vk::binding(0, 0)]] StructuredBuffer<Upt03PackedReservoir> gUpt08Reservoirs;
public [[vk::binding(1, 0)]] RWStructuredBuffer<uint> gUpt08SampleIds;
public [[vk::push_constant]] ConstantBuffer<Upt08FillControl> gUpt08Control;

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gUpt08Control.renderWidth
        || pixel.y >= gUpt08Control.renderHeight)
        return;
    const uint index = pixel.y * gUpt08Control.renderWidth + pixel.x;
    if (index >= gUpt08Control.surfaceCount)
        return;

    const Upt03PackedReservoir packed = gUpt08Reservoirs[index];
    // Selected records carry tag bit zero. Count-only, canonical-empty, and
    // one-frame rescue records intentionally publish ID zero.
    const uint selected = packed.word0 & kUpt03PackedSelectedTag;
    const uint rescue = packed.word4 & kUpt03ReplayKeyRescueBit;
    gUpt08SampleIds[index] = selected != 0u && rescue == 0u
        ? (packed.word4 & ~kUpt03ReplayKeyRescueBit) : 0u;
}
