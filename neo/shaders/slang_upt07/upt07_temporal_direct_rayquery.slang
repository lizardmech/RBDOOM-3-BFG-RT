import upt03_reservoir_contract;
import upt04_hit_contract;
import upt04_initial_work_contract;
import upt04_initial_control;
import upt04_geometry_contract;
import upt04_light_contract;
import upt04_local_light_proposal;
import upt04_emissive_replay;
import upt04_material_contract;
import upt04_primary_surface_live_contract;
import upt04_random_white_hash;
import upt07_material_openpbr;
import upt07_material_provider_contract;
import upt07_temporal_direct_bindings;
import upt07_temporal_reservoir;
import upt07_temporal_surface_contract;
import upt07_emissive_material;

#ifndef UPT07_DUPLICATION_MAP
#define UPT07_DUPLICATION_MAP 0
#endif

public float Upt07SelectionRandom(uint2 pixel, uint historyIdentity)
{
    uint value = Upt04HashWord(pixel.x ^ (pixel.y * 0x9e3779b9u));
    value = Upt04HashWord(value ^ gUpt07Control.frameSampleIndex);
    value = Upt04HashWord(value ^ historyIdentity ^ 0x07a11ce5u);
    return float(value >> 8u) * (1.0f / 16777216.0f);
}

public float Upt07PreviousBestRandom(uint2 pixel, uint historyIdentity)
{
    uint value = Upt04HashWord(pixel.x ^ (pixel.y * 0xc2b2ae35u));
    value = Upt04HashWord(value ^ gUpt07Control.frameSampleIndex);
    value = Upt04HashWord(value ^ historyIdentity ^ 0x51ed270bu);
    return float(value >> 8u) * (1.0f / 16777216.0f);
}

public float Upt07HistorySearchRandom(uint2 pixel, uint stream)
{
    uint value = Upt04HashWord(pixel.x ^ (pixel.y * 0x85ebca6bu));
    value = Upt04HashWord(value ^ gUpt07Control.frameSampleIndex
        ^ 0x7f4a7c15u);
    value = Upt04HashWord(value ^ (stream * 0x9e3779b9u));
    return float(value >> 8u) * (1.0f / 16777216.0f);
}

public int2 Upt07HistoryProbeOffset(uint2 pixel, uint probeIndex)
{
    if (probeIndex == 0u)
        return int2(0, 0);
    // Match the accepted custom DI convention: a named radius of four spans
    // [-2, 2) pixels on each axis. Random probes decorrelate neighboring
    // receivers while keeping the exact reprojected center authoritative.
    const float radius = 4.0f;
    const float x = Upt07HistorySearchRandom(pixel, 2u + probeIndex * 2u);
    const float y = Upt07HistorySearchRandom(pixel, 3u + probeIndex * 2u);
    return int2(int((x - 0.5f) * radius), int((y - 0.5f) * radius));
}

public bool Upt07DirectHistoryEligible(Upt07DecodedReservoir history)
{
    if (history.valid == 0u || history.logical.hasSelectedSample == 0u)
        return false;
    const Upt03Candidate candidate = history.logical.selected;
    return (candidate.eventKind == 1u || candidate.eventKind == 2u)
        && (candidate.flags & kUpt04CandidateFlagGlobal) == 0u
        && !Upt03IsDirectSpatialRescue(candidate)
        && candidate.pathLength == 1u
        && candidate.reconnectionVertexLength == 0u;
}

// T0 has a trace-free resolve downstream, so a history winner must establish
// visibility against the current TLAS here. Fresh D0 winners already carry a
// current-frame VisibilityKnown|VisibilityPassed proof and pay no second ray.
public bool Upt07VisibilityPassed(
    Upt04TraceRequest request,
    Upt04VisibilityIgnore ignore)
{
    RayDesc ray;
    ray.Origin = request.origin;
    ray.TMin = request.tMin;
    ray.Direction = request.direction;
    ray.TMax = request.tMax;
    RayQuery<0> query = RayQuery<0>();
    query.TraceRayInline(
        gUpt07Scene,
        RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
        0xffu,
        ray);
    while (query.Proceed())
    {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
            continue;
        if (Upt04VisibilityIgnoresCandidate(
                ignore,
                query.CandidateInstanceID(),
                query.CandidatePrimitiveIndex()))
            continue;
        query.CommitNonOpaqueTriangleHit();
    }
    return query.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

// IMPORTANT REGRESSION GUARD:
// Adjacent-frame DI reuse is not a persistent cache.  The previous-to-current
// remap plus the stable identity fingerprint prove analytic continuity, and
// replay deliberately evaluates the current payload so animation and portal
// publication changes remain valid.  Never require kUpt04LightStableCacheable
// for Doom analytic lights here: that flag is removed when an analytic payload
// changes, and using it as a T0 gate caused widespread reservoir loss, portal
// flashes, and rigid/skinned temporal flicker in real maps.  STABLE_CACHEABLE
// is a stronger multi-frame cache promise, not the one-frame temporal contract.
// Dynamic emissive geometry remains quarantined until its route/motion replay
// contract is complete.
public bool Upt07DirectLightTemporalReplayable(Upt04UnifiedLightRecord light)
{
    if (light.type == kUpt04LightDoomAnalytic)
        return true;
    if (light.type == kUpt04LightEmissiveTriangle)
        return (light.flags & kUpt04EmissiveHistoryDynamic) == 0u;
    return false;
}

public uint Upt07ResolveDirectLight(
    Upt03Candidate source,
    uint sourceIsHistory,
    out uint emissive,
    out uint currentDenseIndex,
    out Upt04UnifiedLightRecord light)
{
    emissive = source.eventKind == 2u ? 1u : 0u;
    currentDenseIndex = 0xffffffffu;
    light = (Upt04UnifiedLightRecord)0;
    if ((source.eventKind != 1u && source.eventKind != 2u)
        || source.identity0 == 0u)
        return 0u;
    const uint sourceDenseIndex = source.identity0 - 1u;
    if (sourceIsHistory != 0u)
    {
        if (sourceDenseIndex >= gUpt07Control.previousToCurrentLightCount)
            return 0u;
        currentDenseIndex = gUpt07PreviousToCurrentLights[sourceDenseIndex];
        if (currentDenseIndex == 0xffffffffu)
            return 0u;
    }
    else
    {
        currentDenseIndex = sourceDenseIndex;
    }
    const bool inEmissiveRange = currentDenseIndex >= gUpt07Control.emissiveRangeStart
        && currentDenseIndex - gUpt07Control.emissiveRangeStart
            < gUpt07Control.emissiveRangeCount;
    const bool inAnalyticRange = currentDenseIndex >= gUpt07Control.analyticRangeStart
        && currentDenseIndex - gUpt07Control.analyticRangeStart
            < gUpt07Control.analyticRangeCount;
    if (currentDenseIndex >= gUpt07Control.currentLightCount
        || (emissive != 0u ? !inEmissiveRange : !inAnalyticRange))
        return 0u;
    light = Upt07LoadLight(currentDenseIndex);
    const uint expectedType = emissive != 0u
        ? kUpt04LightEmissiveTriangle : kUpt04LightDoomAnalytic;
    return light.type == expectedType
            && source.identity1 == Upt04StableLightIdentityFingerprint(light)
        ? 1u : 0u;
}

public uint Upt07EvaluateDirectSample(
    Upt03Candidate source,
    Upt04PrimarySurfaceRecord receiver,
    uint sourceIsHistory,
    out Upt04LocalLightSample sample,
    out uint emissive,
    out uint currentDenseIndex)
{
    sample = (Upt04LocalLightSample)0;
    Upt04UnifiedLightRecord light;
    if (Upt07ResolveDirectLight(
            source, sourceIsHistory, emissive, currentDenseIndex, light) == 0u)
        return 0u;
    if (sourceIsHistory != 0u && !Upt07DirectLightTemporalReplayable(light))
        return 0u;

    if (emissive != 0u)
    {
        const Upt04EmissiveGeometrySample replay =
            Upt04ReplayEmissiveTriangleBarycentrics(
                light,
                receiver.worldPositionAndViewDepth.xyz,
                float2(source.sampleCoord0, source.sampleCoord1),
                (gUpt07Control.geometryAvailabilityFlags
                    & kUpt04EmissiveReplayTwoSided) != 0u);
        if (replay.status == kUpt04SampleInvalid)
            return 0u;
        const float4 regionWeights = UptDirectEmissiveRegionWeights(
            replay,
            gUpt07Control.materialCount,
            gUpt07Control.logicalTextureCount,
            gUpt07Control.emissiveScale);
        const uint region = Upt04EmissiveRegionFromBarycentrics(
            replay.barycentrics);
        const Upt04MaterialEmissionEvaluation emission =
            UptDirectEvaluateEmissiveEndpoint(
                replay.materialIndex,
                replay.triangleClassAndFlags,
                replay.emissionTexCoord,
                float2(0.0f),
                float2(0.0f),
                gUpt07Control.materialCount,
                gUpt07Control.logicalTextureCount,
                gUpt07Control.emissiveScale);
        if (emission.valid == 0u)
            return 0u;
        // Preserve valid black mask texels. The manager record describes the
        // triangle as a whole and cannot replace endpoint texture evaluation.
        const float3 replayRadiance = emission.radiance;
        sample.status = replay.status == kUpt04SampleValidPositive
                && any(replayRadiance > 0.0f)
            ? kUpt04SampleValidPositive : kUpt04SampleValidZero;
        sample.position = replay.position;
        sample.normal = replay.normal;
        sample.distance = replay.distance;
        sample.direction = replay.direction;
        sample.solidAnglePdf = replay.solidAnglePdf
            * (4.0f * Upt04EmissiveRegionSelectionPdf(
                regionWeights, region));
        sample.radiance = replayRadiance;
    }
    else
    {
        if (!Upt04AnalyticLightCurrentPortalEligible(
                light, gUpt07Control.geometryAvailabilityFlags))
            return 0u;
        sample = Upt04SampleAnalyticSphere(
            light,
            receiver.worldPositionAndViewDepth.xyz,
            float2(source.sampleCoord0, source.sampleCoord1));
        if (sample.status == kUpt04SampleInvalid)
            return 0u;
    }
    return 1u;
}

public float Upt07PrimaryDirectTargetAtReceiver<
    TMaterialSet : IUpt07PrimaryMaterialSet>(
    Upt03Candidate source,
    Upt04PrimarySurfaceRecord receiver)
{
    if ((source.flags & kUpt04CandidateFlagGlobal) != 0u
        || source.pathLength != 1u
        || source.reconnectionVertexLength != 0u)
        return 0.0f;
    Upt04LocalLightSample sample;
    uint emissive;
    uint currentDenseIndex;
    if (Upt07EvaluateDirectSample(
            source, receiver, 0u, sample, emissive, currentDenseIndex) == 0u
        || sample.status != kUpt04SampleValidPositive)
        return 0.0f;
    const Upt04MaterialEvaluation material =
        TMaterialSet.EvaluatePrimaryDirect(
            receiver, sample.direction, sample.radiance);
    if (material.valid == 0u)
        return 0.0f;
    Upt04LocalLightRanges ranges;
    ranges.emissiveStart = gUpt07Control.emissiveRangeStart;
    ranges.emissiveCount = gUpt07Control.emissiveRangeCount;
    ranges.analyticStart = gUpt07Control.analyticRangeStart;
    ranges.analyticCount = gUpt07Control.analyticRangeCount;
    const float proposalPdf = Upt04DirectRisIdentityPdf(
        ranges,
        emissive != 0u
            ? kUpt04LightEmissiveTriangle : kUpt04LightDoomAnalytic,
        (source.flags & kUpt04CandidateFlagDirectProposalParity) != 0u
            ? 1u : 0u,
        Upt04ConfiguredEmissiveTrialCount(
            gUpt07Control.geometryAvailabilityFlags))
        * sample.solidAnglePdf;
    const float techniqueMis = emissive != 0u
        ? Upt04BalanceHeuristic(proposalPdf, material.directionalPdf) : 1.0f;
    const float3 contribution = Upt04DirectReservoirContribution(
        material.weightedContribution,
        sample.solidAnglePdf,
        techniqueMis,
        gUpt07Control.geometryAvailabilityFlags);
    const float target = Upt04DirectReservoirTarget(
        contribution,
        gUpt07Control.geometryAvailabilityFlags);
    return isfinite(target) && target > 0.0f ? target : 0.0f;
}

public uint Upt07ShiftPrimaryDirect<
    TMaterialSet : IUpt07PrimaryMaterialSet>(
    Upt03Candidate source,
    Upt04PrimarySurfaceRecord receiver,
    out Upt03Candidate shifted)
{
    shifted = (Upt03Candidate)0;
    Upt04LocalLightRanges ranges;
    ranges.emissiveStart = gUpt07Control.emissiveRangeStart;
    ranges.emissiveCount = gUpt07Control.emissiveRangeCount;
    ranges.analyticStart = gUpt07Control.analyticRangeStart;
    ranges.analyticCount = gUpt07Control.analyticRangeCount;
    if (Upt04LocalLightRangeCount(ranges) == 0u)
        return 0u;
    Upt04LocalLightSample sample;
    uint emissive;
    uint currentDenseIndex;
    if (Upt07EvaluateDirectSample(
            source, receiver, 1u, sample, emissive, currentDenseIndex) == 0u)
        return 0u;
    const Upt04MaterialEvaluation material =
        TMaterialSet.EvaluatePrimaryDirect(
            receiver, sample.direction, sample.radiance);
    if (material.valid == 0u)
        return 0u;
    const float proposalPdf = Upt04DirectRisIdentityPdf(
        ranges,
        emissive != 0u
            ? kUpt04LightEmissiveTriangle : kUpt04LightDoomAnalytic,
        (source.flags & kUpt04CandidateFlagDirectProposalParity) != 0u
            ? 1u : 0u,
        Upt04ConfiguredEmissiveTrialCount(
            gUpt07Control.geometryAvailabilityFlags))
        * sample.solidAnglePdf;
    if (!Upt04FinitePositive(proposalPdf))
        return 0u;

    shifted.status = sample.status == kUpt04SampleValidPositive
            && material.target > 0.0f
        ? kUpt04SampleValidPositive : kUpt04SampleValidZero;
    const float techniqueMis = emissive != 0u
        ? Upt04BalanceHeuristic(proposalPdf, material.directionalPdf) : 1.0f;
    const float3 contribution = Upt04DirectReservoirContribution(
        material.weightedContribution,
        sample.solidAnglePdf,
        techniqueMis,
        gUpt07Control.geometryAvailabilityFlags);
    shifted.eventKind = emissive != 0u ? 2u : 1u;
    shifted.lobeMask = material.lobeMask;
    shifted.flags = source.flags
        & kUpt04CandidateFlagDirectProposalParity;
    shifted.pathLength = 1u;
    shifted.reconnectionVertexLength = 0u;
    shifted.identity0 = currentDenseIndex + 1u;
    shifted.identity1 = Upt04StableLightIdentityFingerprint(
        Upt07LoadLight(currentDenseIndex));
    shifted.replayKey = source.replayKey;
    shifted.replayIndex = source.replayIndex;
    shifted.generationFingerprint = kUpt03ExpectedGeneration;
    shifted.sampleCoord0 = source.sampleCoord0;
    shifted.sampleCoord1 = source.sampleCoord1;
    shifted.proposalPdf = proposalPdf;
    shifted.pathPdf = proposalPdf;
    shifted.partialJacobian = 1.0f;
    shifted.accumulatedRouletteProbability = 1.0f;
    if (shifted.status == kUpt04SampleValidZero)
        return 1u;

    const float3 geometricNormal = normalize(
        receiver.geometricNormalAndRoughness.xyz);
    const float3 shadingNormal = normalize(
        receiver.shadingNormalAndOpacity.xyz);
    if (!all(isfinite(geometricNormal)) || !all(isfinite(shadingNormal))
        || dot(geometricNormal, sample.direction) <= 0.0f
        || dot(shadingNormal, sample.direction) <= 0.0f)
        return 0u;
    shifted.contributionR = contribution.x;
    shifted.contributionG = contribution.y;
    shifted.contributionB = contribution.z;
    shifted.target = Upt04DirectReservoirTarget(
        contribution,
        gUpt07Control.geometryAvailabilityFlags);
    if (!isfinite(shifted.target) || shifted.target <= 0.0f)
        return 0u;
    return 1u;
}

public uint Upt07BuildSelectedVisibility(
    Upt03Candidate selected,
    Upt04PrimarySurfaceRecord receiver,
    out Upt04TraceRequest visibility,
    out Upt04VisibilityIgnore visibilityIgnore)
{
    visibility = (Upt04TraceRequest)0;
    visibilityIgnore = Upt04NoVisibilityIgnore();
    Upt04LocalLightSample sample;
    uint emissive;
    uint currentDenseIndex;
    if (Upt07EvaluateDirectSample(
            selected,
            receiver,
            0u,
            sample,
            emissive,
            currentDenseIndex) == 0u
        || sample.status != kUpt04SampleValidPositive
        || !Upt04BuildPrimaryVisibilityRequest(
            receiver.worldPositionAndViewDepth.xyz,
            receiver.geometricNormalAndRoughness.xyz,
            receiver.shadingNormalAndOpacity.xyz,
            sample.direction,
            sample.distance,
            visibility))
        return 0u;
    if (emissive != 0u)
    {
        const Upt04UnifiedLightRecord visibilityLight =
            Upt07LoadLight(currentDenseIndex);
        visibilityIgnore = Upt04MakeVisibilityIgnore(
            visibilityLight.instanceId, visibilityLight.primitiveIndex);
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
    const Upt04PrimarySurfaceRecord receiver = Upt07LoadCurrentSurface(index);
    if (!Upt04PrimarySurfaceRecordValid(receiver))
    {
        gUpt07CurrentReservoirs[index] = currentPacked;
        return;
    }

    // Only a dynamic emissive selection bypasses temporal merge.  Analytic
    // lights remain replayable when their current payload changes; identity
    // remapping, not the persistent-cache flag, owns one-frame continuity.
    if (current.valid != 0u && current.logical.hasSelectedSample != 0u)
    {
        uint currentEmissive;
        uint currentDenseIndex;
        Upt04UnifiedLightRecord currentLight;
        if (Upt07ResolveDirectLight(
                current.logical.selected,
                0u,
                currentEmissive,
                currentDenseIndex,
                currentLight) != 0u
            && !Upt07DirectLightTemporalReplayable(currentLight))
        {
            gUpt07CurrentReservoirs[index] = currentPacked;
            return;
        }
    }

    Upt07DecodedReservoir history = (Upt07DecodedReservoir)0;
    uint historyIndex = 0u;
    Upt04PrimarySurfaceRecord historyReceiver =
        (Upt04PrimarySurfaceRecord)0;
    Upt03Candidate shifted = (Upt03Candidate)0;
    uint shiftedValid = 0u;
    uint historyFailureReason = kUpt07RejectNoCompatibleHistory;
    if (gUpt07Control.historyAvailable != 0u)
    {
        const uint2 dimensions = uint2(
            gUpt07Control.renderWidth, gUpt07Control.renderHeight);
        const uint searchMode = min(
            gUpt07Control.previousCamera.historySearchMode, 2u);
        const float2 pixelJitter = searchMode >= 1u
            ? float2(
                Upt07HistorySearchRandom(pixel, 0u),
                Upt07HistorySearchRandom(pixel, 1u)) - 0.5f
            : float2(0.0f);
        const uint probeCount = searchMode >= 2u ? 9u : 1u;
        const Upt07PreviousProjection projection =
            Upt07ProjectPreviousForSearch(
                receiver,
                gUpt07Control.previousCamera,
                dimensions,
                pixelJitter,
                searchMode >= 2u ? 2u : 0u);
        if (projection.valid != 0u)
        {
            // Search surfaces only. The first geometrically compatible
            // receiver wins; reservoir state is loaded exactly once afterward
            // and can never steer the chosen history pixel.
            int2 historyPixel = int2(-1, -1);
            [loop]
            for (uint probe = 0u; probe < probeCount; ++probe)
            {
                const int2 candidatePixel = projection.pixel
                    + Upt07HistoryProbeOffset(pixel, probe);
                // Keep this signed test. Casting a negative coordinate to uint
                // before rejection recreated a leading-edge history leak in
                // the FullSample integration.
                if (candidatePixel.x < 0 || candidatePixel.y < 0
                    || uint(candidatePixel.x) >= dimensions.x
                    || uint(candidatePixel.y) >= dimensions.y)
                    continue;
                const uint candidateIndex = uint(candidatePixel.y) * dimensions.x
                    + uint(candidatePixel.x);
                const Upt04PrimarySurfaceRecord previous =
                    Upt07LoadPreviousSurface(candidateIndex);
                if (Upt07SurfacesCompatible(
                        receiver, previous, projection.linearDepth))
                {
                    historyPixel = candidatePixel;
                    historyIndex = candidateIndex;
                    historyReceiver = previous;
                    break;
                }
            }
            if (historyPixel.x >= 0)
            {
                history = Upt07DecodeReservoir(
                    gUpt07HistoryReservoirs[historyIndex]);
                historyFailureReason =
                    history.logical.hasSelectedSample != 0u
                    ? kUpt07RejectUnsupportedHistoryEvent
                    : kUpt07RejectHistoryNoSelection;
            }
        }
    }

    // DI temporal owns the first compatible reprojected receiver and arithmetic
    // replay. It never searches based on reservoir contents. Visibility is
    // deferred until after the final winner is known so a current D0 winner
    // does not pay a duplicate ray. Future indirect/reconnection events require
    // a separate traced provider and an explicit drift-accounting contract.
    if (Upt07DirectHistoryEligible(history))
    {
        historyFailureReason = kUpt07RejectShiftInvalid;
        shiftedValid = Upt07ShiftPrimaryDirect<Upt07OpenPbrMaterialSet>(
            history.logical.selected,
            receiver,
            shifted);
        if (shiftedValid != 0u)
            historyFailureReason = shifted.status == kUpt04SampleValidPositive
                ? 0u : kUpt07RejectShiftedValidZero;
    }

    const uint historyIdentity = shiftedValid != 0u ? shifted.identity0 : 0u;
    Upt07DecodedReservoir seededCurrent = current;
    uint previousBestAdmitted = 0u;
    if ((gUpt07Control.geometryAvailabilityFlags
            & kUpt07PreviousBestSeedFlag) != 0u
        && shiftedValid != 0u
        && shifted.status == kUpt04SampleValidPositive)
    {
        // Replayed history is a new current-domain proposal. A visibility bit
        // from the old frame is not proof for this receiver, so a selected
        // seed must reach the winner-only query below.
        Upt03Candidate previousBestCandidate = shifted;
        previousBestCandidate.flags &= ~(
            kUpt04CandidateFlagVisibilityKnown
            | kUpt04CandidateFlagVisibilityPassed);
        seededCurrent = Upt07InjectPreviousBestSeed(
            current,
            previousBestCandidate,
            1u,
            Upt07PreviousBestRandom(pixel, historyIdentity),
            previousBestAdmitted);
    }

    // Previous-best and the confidence merge consume the same history
    // reservoir. Once admitted as an M=1 current-domain proposal, that source
    // must not be admitted again with its accumulated M. The old double
    // admission caused the overexposure and max-age-shaped persistence.
    Upt07DecodedReservoir confidenceHistory = history;
    uint confidenceShiftedValid = shiftedValid;
    if (previousBestAdmitted != 0u)
    {
        confidenceHistory = (Upt07DecodedReservoir)0;
        confidenceShiftedValid = 0u;
    }

    const float currentTargetAtHistory = confidenceShiftedValid != 0u
            && seededCurrent.valid != 0u
            && seededCurrent.logical.hasSelectedSample != 0u
            && Upt04PrimarySurfaceRecordValid(historyReceiver)
        ? Upt07PrimaryDirectTargetAtReceiver<Upt07OpenPbrMaterialSet>(
            seededCurrent.logical.selected,
            historyReceiver)
        : 0.0f;
    const float currentTargetAtCurrent = seededCurrent.valid != 0u
            && seededCurrent.logical.hasSelectedSample != 0u
        ? Upt07PrimaryDirectTargetAtReceiver<Upt07OpenPbrMaterialSet>(
            seededCurrent.logical.selected,
            receiver)
        : 0.0f;
    // The history winner still carries a previous-frame dense light index.
    // Never replay that index through the current light buffer here: light
    // ordering churn can alias it to an unrelated current payload (the
    // observed normal-light-to-red-light poisoning). The target stored with
    // the history reservoir was published in this source domain and is the
    // authoritative p_i(x_i) required by the pairwise term. The shifted
    // replay above separately remaps the identity and supplies p_i(x_current).
    const float historyTargetAtHistory = confidenceShiftedValid != 0u
            && confidenceHistory.valid != 0u
            && confidenceHistory.logical.hasSelectedSample != 0u
            && Upt04PrimarySurfaceRecordValid(historyReceiver)
            && isfinite(confidenceHistory.logical.selected.target)
            && confidenceHistory.logical.selected.target > 0.0f
        ? confidenceHistory.logical.selected.target
        : 0.0f;

    uint adaptiveMaximumHistoryM = max(gUpt07Control.maximumHistoryM, 1u);
#if UPT07_DUPLICATION_MAP
    if ((gUpt07Control.geometryAvailabilityFlags & (1u << 26u)) != 0u
        && shiftedValid != 0u)
    {
        const uint historyY = historyIndex / gUpt07Control.renderWidth;
        const uint historyX = historyIndex - historyY * gUpt07Control.renderWidth;
        const uint packed = gUpt07HistoryDuplication[
            historyY * ((gUpt07Control.renderWidth + 3u) / 4u)
                + historyX / 4u];
        const uint score = (packed >> ((historyX & 3u) * 8u)) & 0xffu;
        const float correlation = float(score) / 255.0f;
        // The enhanced estimator's adaptive range starts at 20 even when the
        // user permits a larger global history M. Using maximumHistoryM here
        // weakened the correlation control precisely in the 32/63 stress case
        // it is meant to contain. A lower user cap remains authoritative.
        const uint duplicationDefaultCap = min(adaptiveMaximumHistoryM, 20u);
        const float adaptiveCap = lerp(
            float(duplicationDefaultCap),
            1.0f,
            pow(correlation, 0.1f));
        adaptiveMaximumHistoryM = max(uint(round(adaptiveCap)), 1u);
    }
#endif
    Upt07TemporalMergeResult merged = Upt07MergeTemporal(
        seededCurrent,
        confidenceHistory,
        shifted,
        confidenceShiftedValid,
        currentTargetAtCurrent,
        currentTargetAtHistory,
        historyTargetAtHistory,
        historyFailureReason,
        Upt07SelectionRandom(pixel, historyIdentity),
        adaptiveMaximumHistoryM,
        max(gUpt07Control.maximumHistoryAge, 1u),
        float(gUpt07Control.maximumHistoryContributionRatio),
        (gUpt07Control.geometryAvailabilityFlags
            & kUpt07PairwiseMisFlag) != 0u ? 1u : 0u);

    // Regression guard: both RTXDI FullSample and the accepted legacy DI lane
    // re-establish final visibility after reuse. UPT-05 deliberately cannot
    // trace, so deleting the old T0 ray left history-selected radiance fully
    // unshadowed and allowed it to overwrite a correct D0 count-only result.
    // Trace only a final winner without a current-frame visibility proof. If
    // it is occluded (or cannot be reconstructed), publish the original D0
    // reservoir rather than retaining any mass from the rejected history.
    const uint visibilityProof = kUpt04CandidateFlagVisibilityKnown
        | kUpt04CandidateFlagVisibilityPassed;
    // A visibility bit carried by a history record is only proof for the old
    // frame. selectedHistory must therefore force a new query even when that
    // stale bit is still present. The flag-only test is retained for any
    // current candidate route that reaches T0 without D0 visibility proof.
    if (merged.logical.hasSelectedSample != 0u
        && (merged.selectedHistory != 0u
            || (merged.logical.selected.flags & visibilityProof) != visibilityProof))
    {
        Upt04TraceRequest visibility;
        Upt04VisibilityIgnore visibilityIgnore;
        if (Upt07BuildSelectedVisibility(
                merged.logical.selected,
                receiver,
                visibility,
                visibilityIgnore) == 0u
            || !Upt07VisibilityPassed(visibility, visibilityIgnore))
        {
            gUpt07CurrentReservoirs[index] = currentPacked;
            return;
        }
        merged.logical.selected.flags =
            (merged.logical.selected.flags & ~visibilityProof)
            | visibilityProof;
    }
    gUpt07CurrentReservoirs[index] = Upt07PackTemporalResult(merged);
}
