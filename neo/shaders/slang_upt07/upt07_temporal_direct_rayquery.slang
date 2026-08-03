import upt03_reservoir_contract;
import upt04_hit_contract;
import upt04_initial_work_contract;
import upt04_light_contract;
import upt04_local_light_proposal;
import upt04_material_contract;
import upt04_primary_surface_live_contract;
import upt04_random_white_hash;
import upt07_material_openpbr;
import upt07_material_provider_contract;
import upt07_temporal_reservoir;
import upt07_temporal_surface_contract;

#ifndef UPT07_PACKED_LIGHT_RECORD
#define UPT07_PACKED_LIGHT_RECORD 0
#endif

public struct Upt07TemporalDirectControl
{
    public uint renderWidth;
    public uint renderHeight;
    public uint surfaceCount;
    public uint historyAvailable;
    public uint emissiveRangeStart;
    public uint emissiveRangeCount;
    public uint analyticRangeStart;
    public uint analyticRangeCount;
    public uint currentLightCount;
    public uint frameSampleIndex;
    public uint maximumHistoryM;
    public uint maximumHistoryAge;
    public Upt07PreviousCamera previousCamera;
};

[[vk::binding(0, 0)]] RaytracingAccelerationStructure gUpt07Scene;
[[vk::binding(1, 0)]] StructuredBuffer<Upt04PrimarySurfaceRecord> gUpt07CurrentSurfaces;
[[vk::binding(2, 0)]] StructuredBuffer<Upt04PrimarySurfaceRecord> gUpt07PreviousSurfaces;
[[vk::binding(3, 0)]] RWStructuredBuffer<Upt03PackedReservoir> gUpt07CurrentReservoirs;
[[vk::binding(4, 0)]] StructuredBuffer<Upt03PackedReservoir> gUpt07HistoryReservoirs;
#if UPT07_PACKED_LIGHT_RECORD
[[vk::binding(5, 0)]] StructuredBuffer<Upt04CompactLightRecord> gUpt07CurrentLights;
#else
[[vk::binding(5, 0)]] StructuredBuffer<Upt04UnifiedLightRecord> gUpt07CurrentLights;
#endif
[[vk::push_constant]] ConstantBuffer<Upt07TemporalDirectControl> gUpt07Control;

public Upt04UnifiedLightRecord Upt07LoadLight(uint index)
{
#if UPT07_PACKED_LIGHT_RECORD
    return Upt04UnpackCompactLight(gUpt07CurrentLights[index]);
#else
    return gUpt07CurrentLights[index];
#endif
}

public float Upt07SelectionRandom(uint2 pixel, uint historyIdentity)
{
    uint value = Upt04HashWord(pixel.x ^ (pixel.y * 0x9e3779b9u));
    value = Upt04HashWord(value ^ gUpt07Control.frameSampleIndex);
    value = Upt04HashWord(value ^ historyIdentity ^ 0x07a11ce5u);
    return float(value >> 8u) * (1.0f / 16777216.0f);
}

public bool Upt07VisibilityPassed(Upt04TraceRequest request)
{
    RayDesc ray;
    ray.Origin = request.origin;
    ray.TMin = request.tMin;
    ray.Direction = request.direction;
    ray.TMax = request.tMax;
    RayQuery<0> query = RayQuery<0>();
    query.TraceRayInline(
        gUpt07Scene,
        RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
        0xffu,
        ray);
    while (query.Proceed()) {}
    return query.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

public bool Upt07DirectHistoryEligible(Upt07DecodedReservoir history)
{
    if (history.valid == 0u || history.logical.hasSelectedSample == 0u)
        return false;
    const Upt03Candidate candidate = history.logical.selected;
    return candidate.eventKind == 1u
        && (candidate.flags & kUpt04CandidateFlagGlobal) == 0u
        && candidate.pathLength == 1u
        && candidate.reconnectionVertexLength == 0u;
}

public uint Upt07ShiftPrimaryAnalytic<
    TMaterialSet : IUpt07PrimaryMaterialSet>(
    Upt03Candidate source,
    Upt04PrimarySurfaceRecord receiver,
    out Upt03Candidate shifted)
{
    shifted = (Upt03Candidate)0;
    if (source.identity0 == 0u)
        return 0u;
    const uint denseIndex = source.identity0 - 1u;
    if (denseIndex < gUpt07Control.analyticRangeStart
        || denseIndex >= gUpt07Control.analyticRangeStart
            + gUpt07Control.analyticRangeCount
        || denseIndex >= gUpt07Control.currentLightCount)
        return 0u;
    const Upt04UnifiedLightRecord light = Upt07LoadLight(denseIndex);
    if (light.type != kUpt04LightDoomAnalytic
        || source.identity1 != light.sourceIndex + 1u)
        return 0u;

    const uint proposalCount = gUpt07Control.emissiveRangeCount
        + gUpt07Control.analyticRangeCount;
    if (proposalCount == 0u
        || proposalCount < gUpt07Control.emissiveRangeCount)
        return 0u;
    const float2 sampleCoordinates = float2(
        source.sampleCoord0, source.sampleCoord1);
    const Upt04LocalLightSample sample = Upt04SampleAnalyticSphere(
        light, receiver.worldPositionAndViewDepth.xyz, sampleCoordinates);
    if (sample.status == kUpt04SampleInvalid)
        return 0u;
    const Upt04MaterialEvaluation material =
        TMaterialSet.EvaluatePrimaryDirect(
            receiver, sample.direction, sample.radiance);
    if (material.valid == 0u)
        return 0u;
    const float proposalPdf = (1.0f / float(proposalCount))
        * sample.solidAnglePdf;
    if (!Upt04FinitePositive(proposalPdf))
        return 0u;

    shifted.status = sample.status == kUpt04SampleValidPositive
            && material.target > 0.0f
        ? kUpt04SampleValidPositive : kUpt04SampleValidZero;
    shifted.eventKind = 1u;
    shifted.lobeMask = material.lobeMask;
    shifted.flags = 0u;
    shifted.pathLength = 1u;
    shifted.reconnectionVertexLength = 0u;
    shifted.identity0 = source.identity0;
    shifted.identity1 = source.identity1;
    shifted.generationFingerprint = kUpt03ExpectedGeneration;
    shifted.sampleCoord0 = source.sampleCoord0;
    shifted.sampleCoord1 = source.sampleCoord1;
    shifted.proposalPdf = proposalPdf;
    shifted.pathPdf = proposalPdf;
    shifted.partialJacobian = 1.0f;
    shifted.accumulatedRouletteProbability = 1.0f;
    if (shifted.status == kUpt04SampleValidZero)
        return 1u;

    Upt04TraceRequest visibility;
    visibility.origin = receiver.worldPositionAndViewDepth.xyz;
    visibility.tMin = 0.01f;
    visibility.direction = sample.direction;
    visibility.tMax = max(sample.distance - 0.02f, visibility.tMin + 1.0e-4f);
    if (!Upt04TraceRequestValid(visibility))
        return 0u;
    shifted.flags = kUpt04CandidateFlagVisibilityKnown;
    if (Upt07VisibilityPassed(visibility))
    {
        shifted.flags |= kUpt04CandidateFlagVisibilityPassed;
        shifted.contributionR = material.weightedContribution.x;
        shifted.contributionG = material.weightedContribution.y;
        shifted.contributionB = material.weightedContribution.z;
        shifted.target = max(shifted.contributionR,
            max(shifted.contributionG, shifted.contributionB));
        if (!isfinite(shifted.target) || shifted.target <= 0.0f)
            return 0u;
    }
    else
    {
        shifted.status = kUpt04SampleValidZero;
    }
    return 1u;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gUpt07Control.renderWidth
        || pixel.y >= gUpt07Control.renderHeight)
        return;
    const uint index = pixel.y * gUpt07Control.renderWidth + pixel.x;
    if (index >= gUpt07Control.surfaceCount)
        return;

    const Upt03PackedReservoir currentPacked = gUpt07CurrentReservoirs[index];
    const Upt07DecodedReservoir current = Upt07DecodeReservoir(currentPacked);
    const Upt04PrimarySurfaceRecord receiver = gUpt07CurrentSurfaces[index];
    if (!Upt04PrimarySurfaceRecordValid(receiver))
    {
        gUpt07CurrentReservoirs[index] = currentPacked;
        return;
    }

    Upt07DecodedReservoir history = (Upt07DecodedReservoir)0;
    Upt03Candidate shifted = (Upt03Candidate)0;
    uint shiftedValid = 0u;
    if (gUpt07Control.historyAvailable != 0u)
    {
        const uint2 dimensions = uint2(
            gUpt07Control.renderWidth, gUpt07Control.renderHeight);
        const Upt07PreviousProjection projection = Upt07ProjectPrevious(
            receiver, gUpt07Control.previousCamera, dimensions);
        if (projection.valid != 0u)
        {
            [ForceUnroll]
            for (uint tap = 0u; tap < 5u; ++tap)
            {
                const int2 historyPixel = projection.pixel + Upt07HistoryTap(tap);
                if (historyPixel.x < 0 || historyPixel.y < 0
                    || uint(historyPixel.x) >= dimensions.x
                    || uint(historyPixel.y) >= dimensions.y)
                    continue;
                const uint historyIndex = uint(historyPixel.y) * dimensions.x
                    + uint(historyPixel.x);
                const Upt04PrimarySurfaceRecord previous =
                    gUpt07PreviousSurfaces[historyIndex];
                if (!Upt07SurfacesCompatible(
                        receiver, previous, projection.viewDepth))
                    continue;
                const Upt07DecodedReservoir candidateHistory =
                    Upt07DecodeReservoir(gUpt07HistoryReservoirs[historyIndex]);
                if (!Upt07DirectHistoryEligible(candidateHistory))
                    continue;
                history = candidateHistory;
                break;
            }
        }
    }

    // Reprojection/compatibility may inspect up to five history records, but
    // shift and visibility execute once at most, after one source is chosen.
    if (Upt07DirectHistoryEligible(history))
    {
        shiftedValid = Upt07ShiftPrimaryAnalytic<Upt07OpenPbrMaterialSet>(
            history.logical.selected,
            receiver,
            shifted);
    }

    const uint historyIdentity = shiftedValid != 0u ? shifted.identity0 : 0u;
    const Upt07TemporalMergeResult merged = Upt07MergeTemporal(
        current,
        history,
        shifted,
        shiftedValid,
        Upt07SelectionRandom(pixel, historyIdentity),
        max(gUpt07Control.maximumHistoryM, 1u),
        max(gUpt07Control.maximumHistoryAge, 1u));
    gUpt07CurrentReservoirs[index] = Upt07PackTemporalResult(merged);
}
