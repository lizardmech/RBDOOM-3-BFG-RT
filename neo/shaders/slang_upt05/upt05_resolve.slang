import upt03_reservoir_contract;

public static const uint kUpt05CandidateFlagGlobal = 1u << 1u;
public static const uint kUpt05LobeDiffuse = 1u << 0u;
public static const uint kUpt05LobeSpecular = 1u << 1u;

public struct Upt05ResolveControl
{
    public uint renderWidth;
    public uint renderHeight;
    public uint surfaceCount;
    public uint view;
};

[[vk::binding(0, 0)]] StructuredBuffer<Upt03PackedReservoir> gUpt05Reservoirs;
[[vk::binding(1, 0)]]
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> gUpt05Output;
[[vk::push_constant]] ConstantBuffer<Upt05ResolveControl> gUpt05Control;

public bool Upt05FiniteNonNegative3(float3 value)
{
    return all(isfinite(value)) && all(value >= 0.0f);
}

public bool Upt05DecodeEstimate(
    Upt03PackedReservoir packed,
    out float3 estimate,
    out float3 selectedContribution,
    out float normalization,
    out uint eventKind,
    out uint lobeMask,
    out uint flags)
{
    estimate = float3(0.0f);
    selectedContribution = float3(0.0f);
    normalization = 0.0f;
    eventKind = 0u;
    lobeMask = 0u;
    flags = 0u;
    if ((packed.word0 & 1u) == 0u || packed.word1 == 0u
        || packed.word2 == 0u)
        return false;

    eventKind = (packed.word0 >> 4u) & 0x7u;
    lobeMask = (packed.word0 >> 7u) & 0x1fu;
    flags = (packed.word0 >> 12u) & 0x3fu;
    if (eventKind < 1u || eventKind > 4u || lobeMask == 0u)
        return false;

    selectedContribution = float3(
        asfloat(packed.word7),
        asfloat(packed.word8),
        asfloat(packed.word9));
    const float selectedTarget = asfloat(packed.word10);
    const float meanWeight = asfloat(packed.word11);
    const float proposalPdf = asfloat(packed.word12);
    const float pathPdf = asfloat(packed.word13);
    const float partialJacobian = asfloat(packed.word14);
    const float rouletteProbability = asfloat(packed.word15);
    if (!Upt05FiniteNonNegative3(selectedContribution)
        || !isfinite(selectedTarget) || selectedTarget <= 0.0f
        || !isfinite(meanWeight) || meanWeight <= 0.0f
        || !isfinite(proposalPdf) || proposalPdf <= 0.0f
        || !isfinite(pathPdf) || pathPdf <= 0.0f
        || !isfinite(partialJacobian) || partialJacobian <= 0.0f
        || !isfinite(rouletteProbability) || rouletteProbability <= 0.0f
        || rouletteProbability > 1.0f)
        return false;

    normalization = meanWeight / selectedTarget;
    estimate = selectedContribution * normalization;
    return isfinite(normalization) && normalization > 0.0f
        && Upt05FiniteNonNegative3(estimate);
}

public float3 Upt05ResolveView(
    float3 estimate,
    float3 selectedContribution,
    float normalization,
    uint eventKind,
    uint lobeMask,
    uint flags)
{
    if (gUpt05Control.view == 1u)
    {
        // Direct/global categorical classification. Intensity retains a small
        // estimate cue without requiring another output image.
        const float cue = saturate(max(estimate.x, max(estimate.y, estimate.z)));
        return (flags & kUpt05CandidateFlagGlobal) != 0u
            ? float3(0.1f, 0.35f, 1.0f) * (0.25f + 0.75f * cue)
            : float3(0.1f, 1.0f, 0.25f) * (0.25f + 0.75f * cue);
    }
    if (gUpt05Control.view == 2u)
    {
        const bool diffuse = (lobeMask & kUpt05LobeDiffuse) != 0u;
        const bool specular = (lobeMask & kUpt05LobeSpecular) != 0u;
        if (diffuse && specular)
            return float3(1.0f, 0.8f, 0.1f);
        return specular
            ? float3(1.0f, 0.1f, 0.8f)
            : float3(0.1f, 0.8f, 1.0f);
    }
    if (gUpt05Control.view == 3u)
        return selectedContribution;
    if (gUpt05Control.view == 4u)
        return float3(saturate(normalization / (1.0f + normalization)));
    return estimate;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gUpt05Control.renderWidth
        || pixel.y >= gUpt05Control.renderHeight)
        return;
    const uint surfaceIndex = pixel.y * gUpt05Control.renderWidth + pixel.x;
    if (surfaceIndex >= gUpt05Control.surfaceCount)
        return;

    float3 estimate;
    float3 selectedContribution;
    float normalization;
    uint eventKind;
    uint lobeMask;
    uint flags;
    const bool valid = Upt05DecodeEstimate(
        gUpt05Reservoirs[surfaceIndex],
        estimate,
        selectedContribution,
        normalization,
        eventKind,
        lobeMask,
        flags);
    const float3 color = valid
        ? Upt05ResolveView(
            estimate,
            selectedContribution,
            normalization,
            eventKind,
            lobeMask,
            flags)
        : float3(0.0f);
    gUpt05Output[pixel] = float4(color, 1.0f);
}
