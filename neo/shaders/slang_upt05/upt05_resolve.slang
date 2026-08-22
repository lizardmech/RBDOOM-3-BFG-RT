import upt03_reservoir_contract;
import upt04_primary_surface_live_contract;
import upt04_packed_primary_surface;
import upt04_packed_primary_surface32;
import upt04_material_contract;
import upt04_material_primary_live;

#ifndef UPT05_PRIMARY_RECEIVER_MODE
#define UPT05_PRIMARY_RECEIVER_MODE 0
#endif
#ifndef UPT05_REFLECTION_REUSE_DOMAIN
#define UPT05_REFLECTION_REUSE_DOMAIN 0
#endif

public static const uint kUpt05CandidateFlagGlobal = 1u << 1u;
public static const uint kUpt05LobeDiffuse = 1u << 0u;
public static const uint kUpt05LobeSpecular = 1u << 1u;

public struct Upt05ResolveControl
{
    public uint renderWidth;
    public uint renderHeight;
    public uint surfaceCount;
    public uint view;
    public float3 primaryCameraOrigin;
    public uint reserved0;
};

[[vk::binding(0, 0)]] StructuredBuffer<Upt03PackedReservoir> gUpt05Reservoirs;
[[vk::binding(1, 0)]]
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> gUpt05Output;
#if UPT05_PRIMARY_RECEIVER_MODE == 1
[[vk::binding(2, 0)]] StructuredBuffer<Upt04PackedPrimarySurfaceRecord> gUpt05PrimarySurfaces;
#elif UPT05_PRIMARY_RECEIVER_MODE == 2
[[vk::binding(2, 0)]] StructuredBuffer<Upt04PackedPrimarySurfaceRecord32> gUpt05PrimarySurfaces;
#else
[[vk::binding(2, 0)]] StructuredBuffer<Upt04PrimarySurfaceRecord> gUpt05PrimarySurfaces;
#endif
[[vk::binding(3, 0)]]
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> gUpt05RrSpecularAlbedo;
[[vk::push_constant]] ConstantBuffer<Upt05ResolveControl> gUpt05Control;

[[vk::binding(4, 0)]]
[[vk::image_format("rgba32ui")]]
Texture2DArray<uint4> gUpt05ReservoirTex;

[[vk::binding(5, 0)]]
[[vk::image_format("rgba32ui")]]
Texture2DArray<uint4> gUpt05PrimaryTex;

public uint Upt05SurfaceBufferIndex(uint surfaceIndex)
{
    if ((gUpt05Control.reserved0 & 1u) == 0u || gUpt05Control.renderWidth == 0u)
        return surfaceIndex;
    const uint width = gUpt05Control.renderWidth;
    const uint x = surfaceIndex % width;
    const uint y = surfaceIndex / width;
    const uint tilesX = (width + 7u) / 8u;
    return (((y >> 3u) * tilesX) + (x >> 3u)) * 64u
        + ((y & 7u) * 8u) + (x & 7u);
}

public Upt03PackedReservoir Upt05LoadPackedReservoir(uint surfaceIndex)
{
    if ((gUpt05Control.reserved0 & 2u) != 0u && gUpt05Control.renderWidth != 0u)
    {
        const uint width = gUpt05Control.renderWidth;
        const uint2 pixel = uint2(surfaceIndex % width, surfaceIndex / width);
        const uint4 a = gUpt05ReservoirTex[uint3(pixel, 0)];
        const uint4 b = gUpt05ReservoirTex[uint3(pixel, 1)];
        const uint4 c = gUpt05ReservoirTex[uint3(pixel, 2)];
        const uint4 d = gUpt05ReservoirTex[uint3(pixel, 3)];
        Upt03PackedReservoir packed;
        packed.word0 = a.x; packed.word1 = a.y; packed.word2 = a.z; packed.word3 = a.w;
        packed.word4 = b.x; packed.word5 = b.y; packed.word6 = b.z; packed.word7 = b.w;
        packed.word8 = c.x; packed.word9 = c.y; packed.word10 = c.z; packed.word11 = c.w;
        packed.word12 = d.x; packed.word13 = d.y; packed.word14 = d.z; packed.word15 = d.w;
        return packed;
    }
    return gUpt05Reservoirs[Upt05SurfaceBufferIndex(surfaceIndex)];
}

public bool Upt05FiniteNonNegative3(float3 value)
{
    return all(isfinite(value)) && all(value >= 0.0f);
}

public bool Upt05PrimaryTextureEnabled()
{
    return (gUpt05Control.reserved0 & 4u) != 0u
        && gUpt05Control.renderWidth != 0u;
}

public uint2 Upt05PrimaryTexel(uint surfaceIndex)
{
    const uint width = max(gUpt05Control.renderWidth, 1u);
    return uint2(surfaceIndex % width, surfaceIndex / width);
}

#if UPT05_PRIMARY_RECEIVER_MODE == 2
public Upt04PackedPrimarySurfaceRecord32 Upt05LoadPacked32Primary(uint surfaceIndex)
{
    if (Upt05PrimaryTextureEnabled())
    {
        const uint2 pixel = Upt05PrimaryTexel(surfaceIndex);
        Upt04PackedPrimarySurfaceRecord32 packed;
        packed.geometry = gUpt05PrimaryTex[uint3(pixel, 0)];
        packed.material = gUpt05PrimaryTex[uint3(pixel, 1)];
        return packed;
    }
    return gUpt05PrimarySurfaces[surfaceIndex];
}
#endif

public float3 Upt05LoadPrimaryEmission(uint surfaceIndex)
{
#if UPT05_PRIMARY_RECEIVER_MODE == 1
    const Upt04PackedPrimarySurfaceRecord packed = gUpt05PrimarySurfaces[surfaceIndex];
    if ((packed.positionAndHeader.w & 0xffffu) != kUpt04PackedPrimaryVersion ||
        (packed.positionAndHeader.w & kUpt04PackedPrimaryValid) == 0u)
        return float3(0.0f);
    return max(Upt04UnpackSharedExponentEmission(packed.material.w), float3(0.0f));
#elif UPT05_PRIMARY_RECEIVER_MODE == 2
    const Upt04PackedPrimarySurfaceRecord32 packed =
        Upt05LoadPacked32Primary(surfaceIndex);
    const uint header = packed.material.w >> 16u;
    if ((header & 0x7fffu) != kUpt04PackedPrimary32Version ||
        (header & kUpt04PackedPrimary32Valid) == 0u)
        return float3(0.0f);
    return max(Upt04UnpackSharedExponentEmission(packed.material.z), float3(0.0f));
#else
    const Upt04PrimarySurfaceRecord surface = gUpt05PrimarySurfaces[surfaceIndex];
    if (!Upt04PrimarySurfaceRecordValid(surface) ||
        !all(isfinite(surface.emissiveAndHeight.xyz)))
        return float3(0.0f);
    return max(surface.emissiveAndHeight.xyz, float3(0.0f));
#endif
}

public Upt04PrimarySurfaceRecord Upt05LoadPrimarySurface(uint surfaceIndex)
{
#if UPT05_PRIMARY_RECEIVER_MODE == 1
    return Upt04UnpackPrimarySurface(gUpt05PrimarySurfaces[surfaceIndex]);
#elif UPT05_PRIMARY_RECEIVER_MODE == 2
    return Upt04UnpackPrimarySurface32(
        Upt05LoadPacked32Primary(surfaceIndex),
        gUpt05Control.primaryCameraOrigin);
#else
    return gUpt05PrimarySurfaces[surfaceIndex];
#endif
}

public bool Upt05DecodeEstimate(
    Upt03PackedReservoir packed,
    uint surfaceIndex,
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

    // Words 13..15 are an event-specific union. Cached secondary NEE carries
    // its exact x2 position there; all other events carry the path-PDF,
    // Jacobian and roulette tuple. Resolve must validate the active arm only.
    Upt03Candidate metadata = (Upt03Candidate)0;
    metadata.status = 2u;
    metadata.eventKind = eventKind;
    metadata.flags = flags;
    metadata.pathLength = (packed.word0 >> 24u) & 0x0fu;
    metadata.reconnectionVertexLength = (packed.word0 >> 28u) & 0x0fu;
    metadata.proposalPdf = proposalPdf;
    const bool hasCachedX2 = Upt03HasCachedX2Endpoint(metadata);
    bool metadataValid = false;
    if (hasCachedX2)
    {
        metadata.reconnectionNormalPacked = packed.word3;
        metadata.reconnectionPosition = asfloat(uint3(
            packed.word13, packed.word14, packed.word15));
        metadata.contributionR = selectedContribution.x;
        metadata.contributionG = selectedContribution.y;
        metadata.contributionB = selectedContribution.z;
        metadataValid = Upt03CachedX2EndpointValid(metadata);
    }
    else
    {
        const float pathPdf = asfloat(packed.word13);
        const float partialJacobian = asfloat(packed.word14);
        const float rouletteProbability = asfloat(packed.word15);
        metadataValid = isfinite(pathPdf) && pathPdf > 0.0f
            && isfinite(partialJacobian) && partialJacobian > 0.0f
            && isfinite(rouletteProbability) && rouletteProbability > 0.0f
            && rouletteProbability <= 1.0f;
    }
    if (!Upt05FiniteNonNegative3(selectedContribution)
        || !isfinite(selectedTarget) || selectedTarget <= 0.0f
        || !isfinite(meanWeight) || meanWeight <= 0.0f
        || !Upt03StoredProposalPayloadValid(metadata)
        || !metadataValid)
        return false;

    normalization = meanWeight / selectedTarget;
    if (hasCachedX2)
    {
        const Upt04PrimarySurfaceRecord receiver =
            Upt05LoadPrimarySurface(surfaceIndex);
        const float3 delta = metadata.reconnectionPosition
            - receiver.worldPositionAndViewDepth.xyz;
        const float distanceSquared = dot(delta, delta);
        if (!Upt04PrimarySurfaceRecordValid(receiver)
            || !isfinite(distanceSquared) || distanceSquared <= 1.0e-8f)
            return false;
        const Upt04MaterialEvaluation primary =
            Upt04EvaluatePrimaryDirectProbe(
                receiver,
                delta * rsqrt(distanceSquared),
                selectedContribution);
        if (primary.valid == 0u
            || !Upt03FinitePositive(primary.directionalPdf)
            || !Upt05FiniteNonNegative3(primary.weightedContribution))
            return false;
        selectedContribution = primary.weightedContribution
            / primary.directionalPdf;
    }
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
    if (gUpt05Control.view == 6u)
        return eventKind == 2u ? estimate : float3(0.0f);
    if (gUpt05Control.view == 7u)
        return eventKind == 1u ? estimate : float3(0.0f);
    if (gUpt05Control.view == 8u)
        return eventKind == 2u ? estimate : float3(0.0f);
    if (gUpt05Control.view == 9u)
        return float3(0.0f);
    if (gUpt05Control.view == 10u)
        return (flags & kUpt05CandidateFlagGlobal) != 0u
            ? estimate
            : float3(0.0f);
    if (gUpt05Control.view == 11u)
        return (flags & kUpt05CandidateFlagGlobal) == 0u
            ? estimate
            : float3(0.0f);
    if (gUpt05Control.view == 12u)
        return float3(0.0f);
    return estimate;
}

public float3 Upt05PackedStateView(Upt03PackedReservoir packed)
{
    const uint tag = packed.word0 & 0x0fu;
    if ((tag & 1u) != 0u)
    {
        const float age = float((packed.word0 >> 18u) & 0x3fu) / 63.0f;
        return float3(0.05f, 0.20f + 0.80f * age, 1.0f - 0.75f * age);
    }
    const uint reason = (packed.word0 >> 8u) & 0x0fu;
    if (reason == 1u)
        return float3(1.0f, 0.8f, 0.0f);
    if (reason == 2u)
        return float3(1.0f, 0.0f, 0.0f);
    if (reason == 3u)
        return float3(1.0f, 0.0f, 1.0f);
    if (reason == 4u)
        return float3(0.0f, 1.0f, 1.0f);
    if (reason == 5u)
        return float3(1.0f);
    if (reason == 6u)
        return float3(0.0f, 1.0f, 0.0f);
    if (reason == 7u)
        return float3(1.0f, 0.35f, 0.0f);
    if (reason == 8u)
        return float3(1.0f, 0.25f, 0.75f);
    if (reason == 9u)
        return float3(0.45f, 0.0f, 0.85f);
    if (reason == 10u)
        return float3(0.15f, 0.2f, 1.0f);
    if (reason == 11u)
        return float3(0.0f, 0.55f, 1.0f);
    if (reason == 12u)
        return float3(1.0f, 0.35f, 0.1f);
    if (reason == 13u)
        return float3(0.75f, 0.15f, 0.0f);
    if (reason == 14u)
        return float3(0.2f, 0.9f, 0.45f);
    if (reason == 15u)
        return float3(0.65f, 0.65f, 0.65f);
    if (tag == kUpt03PackedCountOnlyTag)
        return float3(1.0f, 0.0f, 0.0f);
    if (tag == kUpt03PackedNeedsRescueBit)
        return float3(1.0f, 0.0f, 1.0f);
    if (tag == (kUpt03PackedCountOnlyTag | kUpt03PackedNeedsRescueBit))
        return float3(1.0f, 0.8f, 0.0f);
    if (packed.word0 == 0u && packed.word1 == 0u)
        return float3(0.0f);
    return float3(0.0f, 1.0f, 1.0f);
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

    if (gUpt05Control.view == 5u)
    {
        gUpt05Output[pixel] = float4(
            Upt05PackedStateView(Upt05LoadPackedReservoir(surfaceIndex)), 1.0f);
        return;
    }

    float3 estimate;
    float3 selectedContribution;
    float normalization;
    uint eventKind;
    uint lobeMask;
    uint flags;
    const bool valid = Upt05DecodeEstimate(
        Upt05LoadPackedReservoir(surfaceIndex),
        surfaceIndex,
        estimate,
        selectedContribution,
        normalization,
        eventKind,
        lobeMask,
        flags);
    float3 color = valid
        ? Upt05ResolveView(
            estimate,
            selectedContribution,
            normalization,
            eventKind,
            lobeMask,
            flags)
        : float3(0.0f);
    // View 6 intentionally excludes self-emission so it measures only light
    // transported from an emissive triangle to another surface.  View 8 adds
    // the primary emitter glow back without admitting Doom-analytic lighting;
    // view 9 is the complementary primary-emission-only reference.
    if (gUpt05Control.view == 0u ||
        gUpt05Control.view == 8u ||
        gUpt05Control.view == 9u ||
        gUpt05Control.view == 10u ||
        gUpt05Control.view == 11u)
        color += Upt05LoadPrimaryEmission(surfaceIndex);
#if UPT05_REFLECTION_REUSE_DOMAIN
    const float outputAlpha = Upt04PrimarySurfaceRecordValid(
        Upt05LoadPrimarySurface(surfaceIndex)) ? 1.0f : 0.0f;
#else
    const float outputAlpha = 1.0f;
#endif
    gUpt05Output[pixel] = float4(color, outputAlpha);

    // Export the actual noisy reflected transport carried by the combined
    // reservoir. This reconstruction signal is deliberately HDR radiance,
    // not F0. Direct BRDF mixtures remain on the primary material guide
    // because their diffuse and specular terms are not stored separately.
    const bool reflectedTransport = valid
        && (flags & kUpt05CandidateFlagGlobal) != 0u
        && (lobeMask & kUpt05LobeSpecular) != 0u;
    if (gUpt05Control.view == 0u && reflectedTransport)
        gUpt05RrSpecularAlbedo[pixel] = float4(
            max(color, float3(0.0f)), 1.0f);
}
