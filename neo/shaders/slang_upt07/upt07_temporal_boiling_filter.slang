import upt03_reservoir_contract;

public struct Upt43TemporalBoilingControl
{
    public uint renderWidth;
    public uint renderHeight;
    public uint surfaceCount;
    public float strength;
};

public [[vk::binding(0, 0)]] RWStructuredBuffer<Upt03PackedReservoir>
    gUpt43TemporalReservoirs;
public [[vk::push_constant]] ConstantBuffer<Upt43TemporalBoilingControl>
    gUpt43BoilingControl;

groupshared float gUpt43BoilingWeights[64u];
groupshared uint gUpt43BoilingCounts[64u];

[numthreads(8, 8, 1)]
void main(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint groupIndex : SV_GroupIndex)
{
    const uint2 pixel = dispatchThreadId.xy;
    const bool validPixel = pixel.x < gUpt43BoilingControl.renderWidth
        && pixel.y < gUpt43BoilingControl.renderHeight;
    const uint index = validPixel
        ? pixel.y * gUpt43BoilingControl.renderWidth + pixel.x
        : 0xffffffffu;
    const bool validIndex = validPixel
        && index < gUpt43BoilingControl.surfaceCount;

    Upt03PackedReservoir packed = (Upt03PackedReservoir)0;
    float scaledWeight = 0.0f;
    if (validIndex)
    {
        packed = gUpt43TemporalReservoirs[index];
        const float candidateWeight = asfloat(packed.word11);
        if ((packed.word0 & 1u) != 0u
            && isfinite(candidateWeight) && candidateWeight > 0.0f)
            scaledWeight = candidateWeight;
    }

    gUpt43BoilingWeights[groupIndex] = scaledWeight;
    gUpt43BoilingCounts[groupIndex] = scaledWeight > 0.0f ? 1u : 0u;
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint stride = 32u; stride > 0u; stride >>= 1u)
    {
        if (groupIndex < stride)
        {
            gUpt43BoilingWeights[groupIndex] +=
                gUpt43BoilingWeights[groupIndex + stride];
            gUpt43BoilingCounts[groupIndex] +=
                gUpt43BoilingCounts[groupIndex + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (!validIndex || scaledWeight <= 0.0f)
        return;

    const uint nonzeroCount = gUpt43BoilingCounts[0];
    const float averageNonzeroWeight = nonzeroCount != 0u
        ? gUpt43BoilingWeights[0] / float(nonzeroCount) : 0.0f;
    const float strength = clamp(
        gUpt43BoilingControl.strength, 1.0e-6f, 1.0f);
    const float multiplier = 10.0f / strength - 9.0f;
    if (averageNonzeroWeight > 0.0f
        && scaledWeight > averageNonzeroWeight * multiplier)
    {
        // Match PT boiling-filter semantics: reject the complete reservoir,
        // including M and history age. Do not retain a count-only ghost.
        gUpt43TemporalReservoirs[index] = (Upt03PackedReservoir)0;
    }
}
