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
#ifndef UPT_FROZEN_STATIC_SCENE
#define UPT_FROZEN_STATIC_SCENE 0
#endif

#ifndef UPT_FROZEN_FLAT_LIGHTS
#define UPT_FROZEN_FLAT_LIGHTS UPT_FROZEN_STATIC_SCENE
#endif
#ifndef UPT_DIAGNOSTIC_LAMBERT
#define UPT_DIAGNOSTIC_LAMBERT 0
#endif
#if UPT_DIAGNOSTIC_LAMBERT
import upt07_material_lambert_diagnostic;
#define UPT07_SELECTED_PRIMARY_MATERIAL_SET Upt07LambertDiagnosticMaterialSet
#else
import upt07_material_openpbr;
#define UPT07_SELECTED_PRIMARY_MATERIAL_SET Upt07OpenPbrMaterialSet
#endif
import upt07_material_provider_contract;
import upt07_temporal_direct_bindings;
import upt07_temporal_reservoir;
import upt07_temporal_surface_contract;
import upt07_emissive_material;
import upt07_work_budget;
#ifndef UPT07_INDIRECT_REPLAY
#define UPT07_INDIRECT_REPLAY 0
#endif
#ifndef UPT07_EARLY_RECONNECT
#define UPT07_EARLY_RECONNECT 0
#endif
#ifndef UPT07_ROUTE_DIAGNOSTICS
#define UPT07_ROUTE_DIAGNOSTICS 0
#endif
#ifndef UPT07_BOTTLENECK_STAGE
#define UPT07_BOTTLENECK_STAGE 0
#endif
#ifndef UPT10_SHARED_REUSE_ADAPTER
#define UPT10_SHARED_REUSE_ADAPTER 0
#endif
#ifndef UPT10_COMMON_GRIS_MERGE
#define UPT10_COMMON_GRIS_MERGE 0
#endif
#if UPT10_SHARED_REUSE_ADAPTER
#include "../slang_upt10/upt10_reuse_contract.slangh"
#endif
#if UPT07_INDIRECT_REPLAY
import upt07_temporal_indirect;
#endif

#ifndef UPT07_DUPLICATION_MAP
#define UPT07_DUPLICATION_MAP 0
#endif

#if UPT07_INDIRECT_REPLAY && UPT07_ROUTE_DIAGNOSTICS
public static const uint kUpt07RouteDiagnosticsFlag = 1u << 22u;

public void Upt07RouteDiag(uint counter)
{
    if ((gUpt07Control.geometryAvailabilityFlags
            & kUpt07RouteDiagnosticsFlag) == 0u)
        return;
    uint ignored;
    InterlockedAdd(gUpt07ReconnectDiagnostics[counter], 1u, ignored);
}
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
    Upt04VisibilityIgnore ignore,
    inout Upt07WorkCounters work)
{
    RayDesc ray;
    ray.Origin = request.origin;
    ray.TMin = request.tMin;
    ray.Direction = request.direction;
    ray.TMax = request.tMax;
    RayQuery<0> query = RayQuery<0>();
    Upt07WorkAdd(work, kUpt07WorkRayQueries);
    query.TraceRayInline(
        gUpt07Scene,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
        0xffu,
        ray);
    while (query.Proceed())
    {
        Upt07WorkAdd(work, kUpt07WorkProceedIterations);
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
            continue;
        uint candidatePrimitiveId;
        if (!Upt04CanonicalizeHardwarePrimitive(
                query.CandidateInstanceID(),
                query.CandidateGeometryIndex(),
                query.CandidatePrimitiveIndex(),
                candidatePrimitiveId))
            continue;
        if (Upt04VisibilityIgnoresCandidate(
                ignore,
                query.CandidateInstanceID(),
                candidatePrimitiveId))
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
    out Upt04UnifiedLightRecord light,
    inout Upt07WorkCounters work)
{
    emissive = source.eventKind == 2u ? 1u : 0u;
    currentDenseIndex = 0xffffffffu;
    light = (Upt04UnifiedLightRecord)0;
    if ((source.eventKind != 1u && source.eventKind != 2u)
        || source.identity0 == 0u)
        return 0u;
#if UPT_FROZEN_FLAT_LIGHTS
    if (source.eventKind == 2u)
        return 0u;
#endif
    const uint sourceDenseIndex = source.identity0 - 1u;
    if (sourceIsHistory != 0u)
    {
#if UPT_FROZEN_FLAT_LIGHTS
        currentDenseIndex = sourceDenseIndex;
#else
        if (sourceDenseIndex >= gUpt07Control.previousToCurrentLightCount)
            return 0u;
        Upt07WorkAdd(work, kUpt07WorkStableRemapLoads);
        currentDenseIndex = gUpt07PreviousToCurrentLights[sourceDenseIndex];
        if (currentDenseIndex == 0xffffffffu)
            return 0u;
#endif
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
    Upt07WorkAdd(work, kUpt07WorkLightRecordLoads);
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
    out uint currentDenseIndex,
    inout Upt07WorkCounters work)
{
    sample = (Upt04LocalLightSample)0;
    Upt04UnifiedLightRecord light;
    if (Upt07ResolveDirectLight(
            source, sourceIsHistory, emissive, currentDenseIndex, light,
            work) == 0u)
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
    Upt04PrimarySurfaceRecord receiver,
    inout Upt07WorkCounters work)
{
    if ((source.flags & kUpt04CandidateFlagGlobal) != 0u
        || source.pathLength != 1u
        || source.reconnectionVertexLength != 0u)
        return 0.0f;
    Upt04LocalLightSample sample;
    uint emissive;
    uint currentDenseIndex;
    if (Upt07EvaluateDirectSample(
            source, receiver, 0u, sample, emissive, currentDenseIndex,
            work) == 0u
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
    out Upt03Candidate shifted,
    inout Upt07WorkCounters work)
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
            source, receiver, 1u, sample, emissive, currentDenseIndex,
            work) == 0u)
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
    Upt07WorkAdd(work, kUpt07WorkLightRecordLoads);
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
    out Upt04VisibilityIgnore visibilityIgnore,
    inout Upt07WorkCounters work)
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
            currentDenseIndex,
            work) == 0u
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
        Upt07WorkAdd(work, kUpt07WorkLightRecordLoads);
        const Upt04UnifiedLightRecord visibilityLight =
            Upt07LoadLight(currentDenseIndex);
        visibilityIgnore = Upt04MakeVisibilityIgnore(
            visibilityLight.instanceId, visibilityLight.primitiveIndex);
    }
    return 1u;
}

#if UPT10_SHARED_REUSE_ADAPTER
public uint Upt07ShiftThroughSharedAdapter<
    TMaterialSet : IUpt07PrimaryMaterialSet>(
    Upt03Candidate source,
    Upt04PrimarySurfaceRecord sourceReceiver,
    Upt04PrimarySurfaceRecord targetReceiver,
    uint sourceIsHistory,
    uint directRoute,
    out uint failureReason,
    out Upt10ShiftResult result,
    out Upt07PendingIndirectVisibility pendingVisibility,
    inout Upt07WorkCounters work)
{
    result = (Upt10ShiftResult)0;
    result.shiftJacobian = 1.0f;
    result.sourceTarget = source.target;
    pendingVisibility = (Upt07PendingIndirectVisibility)0;
    pendingVisibility.ignore = Upt04NoVisibilityIgnore();
    failureReason = kUpt07RejectShiftInvalid;

    uint shiftedValid = 0u;
    if (directRoute != 0u)
    {
        result.routeLabel = kUpt10RoutePrimaryNee;
        if (sourceIsHistory != 0u)
        {
            shiftedValid = Upt07ShiftPrimaryDirect<TMaterialSet>(
                source, targetReceiver, result.shifted, work);
        }
        else
        {
            result.shifted = source;
            result.shifted.target =
                Upt07PrimaryDirectTargetAtReceiver<TMaterialSet>(
                    source, targetReceiver, work);
            result.shifted.status = result.shifted.target > 0.0f
                ? kUpt04SampleValidPositive : kUpt04SampleValidZero;
            shiftedValid = 1u;
        }
        result.deferredVisibilityRequired =
            result.shifted.status == kUpt04SampleValidPositive ? 1u : 0u;
    }
    else
    {
        result.routeLabel = kUpt10RouteReplayOrReconnect;
        shiftedValid = Upt07ReplayIndirectSample(
            source,
            sourceReceiver,
            targetReceiver,
            sourceIsHistory,
            failureReason,
            result.shifted,
            pendingVisibility,
            result.shiftJacobian,
            work);
        result.mappingVisibilityConsumed = shiftedValid;
        result.deferredVisibilityRequired = pendingVisibility.valid;
    }

    if (shiftedValid == 0u)
        return 0u;
    result.targetTarget = result.shifted.target;
    result.mappingStatus = result.shifted.status == kUpt04SampleValidPositive
        ? kUpt10ShiftValidPositive : kUpt10ShiftValidZero;
    failureReason = result.mappingStatus == kUpt10ShiftValidPositive
        ? 0u : kUpt07RejectShiftedValidZero;
    return 1u;
}

#if UPT10_COMMON_GRIS_MERGE
float Upt10PairwiseBalanceWeight(float sourceTarget, float targetTarget,
    uint sourceConfidence, uint targetConfidence)
{
    const float sourceTerm = isfinite(sourceTarget) && sourceTarget > 0.0f
        ? float(sourceConfidence) * sourceTarget : 0.0f;
    const float targetTerm = isfinite(targetTarget) && targetTarget > 0.0f
        ? float(targetConfidence) * targetTarget : 0.0f;
    const float denominator = sourceTerm + targetTerm;
    return sourceTerm > 0.0f && isfinite(denominator) && denominator > 0.0f
        ? sourceTerm / denominator : 0.0f;
}

Upt07TemporalMergeResult Upt10MergeTemporalSharedGris(
    Upt07DecodedReservoir current,
    Upt07DecodedReservoir history,
    Upt10ShiftResult historyToCurrent,
    uint historyShiftValid,
    Upt10ShiftResult currentToHistory,
    uint currentShiftValid,
    float currentTargetAtCurrent,
    float historyTargetAtHistory,
    uint historyFailureReason,
    float selectionRandom,
    uint maximumHistoryM,
    uint maximumHistoryAge,
    float maximumHistoryContributionRatio)
{
    Upt07TemporalMergeResult result = (Upt07TemporalMergeResult)0;
    if (current.valid != 0u)
        result.logical = current.logical;
    const bool currentSelected = current.valid != 0u
        && current.logical.hasSelectedSample != 0u;
    const bool historySelected = history.valid != 0u
        && history.logical.hasSelectedSample != 0u;
    const bool historyAgeUsable = maximumHistoryAge >= 63u
        || history.logical.selected.age < maximumHistoryAge;
    const bool historyUsable = historyShiftValid != 0u
        && historySelected
        && historyAgeUsable
        && Upt03StructurallyValid(historyToCurrent.shifted)
        && Upt10FinitePositive(historyToCurrent.shiftJacobian)
        && isfinite(selectionRandom) && selectionRandom >= 0.0f
        && selectionRandom < 1.0f;
    if (!historyUsable)
    {
        result.rejectionReason = historyFailureReason != 0u
            ? historyFailureReason : kUpt07RejectMergeMath;
        result.needsRescue = currentSelected ? 0u : 1u;
        if (!currentSelected)
        {
            result.logical.hasSelectedSample = 0u;
            result.logical.weightSum = 0.0f;
        }
        return result;
    }

    const uint historyM = min(history.logical.effectiveM, maximumHistoryM);
    const uint currentM = current.valid != 0u
        ? current.logical.effectiveM : 0u;
    if (historyM == 0u || currentM > 0xffffffffu - historyM)
    {
        result.rejectionReason = kUpt07RejectMergeMath;
        result.needsRescue = currentSelected ? 0u : 1u;
        return result;
    }

    const bool shiftedHistoryPositive =
        historyToCurrent.mappingStatus == kUpt10ShiftValidPositive;
    const float currentContributionScale = currentSelected
        ? current.logical.selected.target * current.logical.weightSum : 0.0f;
    const float historyContributionScale = shiftedHistoryPositive
        ? historyToCurrent.targetTarget * history.logical.weightSum
            * historyToCurrent.shiftJacobian
        : 0.0f;
    if (maximumHistoryContributionRatio > 0.0f
        && history.logical.selected.age != 0u
        && Upt10FinitePositive(currentContributionScale)
        && Upt10FinitePositive(historyContributionScale)
        && historyContributionScale > currentContributionScale
            * max(maximumHistoryContributionRatio, 1.0f))
    {
        result.rejectionReason = kUpt07RejectHistoryOutlier;
        result.needsRescue = currentSelected ? 0u : 1u;
        return result;
    }

    const float currentTargetAtHistory = currentShiftValid != 0u
            && currentToHistory.mappingStatus == kUpt10ShiftValidPositive
        ? currentToHistory.targetTarget : 0.0f;
    const float currentMis = Upt10PairwiseBalanceWeight(
        currentTargetAtCurrent,
        currentTargetAtHistory,
        currentM,
        historyM);
    const float historyMis = Upt10PairwiseBalanceWeight(
        historyTargetAtHistory,
        historyToCurrent.targetTarget,
        historyM,
        currentM);

    Upt10MappedProposalInput historyProposal =
        (Upt10MappedProposalInput)0;
    historyProposal.status = historyToCurrent.mappingStatus;
    historyProposal.routeLabel = historyToCurrent.routeLabel;
    historyProposal.sourceConfidence = historyM;
    historyProposal.sourceScaledWeight = history.logical.weightSum;
    historyProposal.sourceTarget = historyTargetAtHistory;
    historyProposal.targetTarget = historyToCurrent.targetTarget;
    historyProposal.shiftJacobian = historyToCurrent.shiftJacobian;
    historyProposal.pairwiseMis = historyMis;

    Upt10MappedProposalInput currentProposal =
        (Upt10MappedProposalInput)0;
    currentProposal.status = currentSelected
            && Upt10FinitePositive(currentTargetAtCurrent)
        ? kUpt10ShiftValidPositive
        : (current.valid != 0u && currentM > 0u
            ? kUpt10ShiftValidZero : kUpt10ShiftInvalid);
    currentProposal.routeLabel = currentShiftValid != 0u
        ? currentToHistory.routeLabel : kUpt10RouteNone;
    currentProposal.sourceConfidence = currentM;
    currentProposal.sourceScaledWeight = currentSelected
        ? current.logical.weightSum : 0.0f;
    currentProposal.sourceTarget = currentSelected
        ? currentTargetAtCurrent : 0.0f;
    currentProposal.targetTarget = currentSelected
        ? currentTargetAtCurrent : 0.0f;
    currentProposal.shiftJacobian = 1.0f;
    currentProposal.pairwiseMis = currentMis;

    Upt10MergeControl mergeControl = (Upt10MergeControl)0;
    mergeControl.winnerRandom = selectionRandom;
    // History is proposal zero so the deterministic selection interval remains
    // identical to the accepted temporal pairwise implementation.
    const Upt10MergeResult merge = Upt10MergeMappedPair(
        historyProposal, currentProposal, mergeControl);
    if ((merge.validMask & 1u) == 0u
        || !Upt10FiniteNonNegative(merge.totalMappedMass))
    {
        result.rejectionReason = kUpt07RejectMergeMath;
        result.needsRescue = currentSelected ? 0u : 1u;
        return result;
    }

    result.logical.hasSelectedSample = merge.totalMappedMass > 0.0f ? 1u : 0u;
    result.logical.effectiveM = merge.outputConfidence;
    result.logical.weightSum = merge.totalMappedMass;
    if (merge.selectedOrdinal == 0u)
    {
        result.logical.selected = historyToCurrent.shifted;
        result.logical.selected.age = min(
            history.logical.selected.age + 1u, 63u);
        result.selectedHistory = 1u;
    }
    else if (merge.selectedOrdinal == 1u && currentSelected)
    {
        result.logical.selected.target = currentTargetAtCurrent;
    }
    result.historyAccepted = 1u;
    if (result.logical.hasSelectedSample == 0u)
        result.rejectionReason = kUpt07RejectShiftedValidZero;
    result.needsRescue = result.logical.hasSelectedSample != 0u ? 0u : 1u;
    return result;
}
#endif
#endif

#if UPT07_BOTTLENECK_STAGE > 0
uint Upt07BottleneckMix(uint digest, uint value)
{
    return (digest ^ value) * 16777619u;
}

void Upt07PublishBottleneckProbe(
    uint index,
    Upt03PackedReservoir currentPacked,
    uint digest)
{
    // Every probe invocation writes one observable digest so Slang cannot
    // dead-strip the cumulative work. Republish the untouched D0 record so
    // every probe mode consumes the same previous-frame D0 population.
    gUpt07BottleneckProbe[index] = digest;
    gUpt07CurrentReservoirs[index] = currentPacked;
}

#if UPT07_BOTTLENECK_STAGE == 12
void Upt07PublishWorkBudget(
    uint index,
    Upt07WorkCounters work)
{
    const uint base = index * kUpt07WorkRecordWordCount;
    gUpt07BottleneckProbe[base + 0u] = work.rayQueries;
    gUpt07BottleneckProbe[base + 1u] = work.proceedIterations;
    gUpt07BottleneckProbe[base + 2u] = work.stableRemapLoads;
    gUpt07BottleneckProbe[base + 3u] = work.emissiveHashLoads;
    gUpt07BottleneckProbe[base + 4u] = work.lightRecordLoads;
    gUpt07BottleneckProbe[base + 5u] = work.geometryResolves;
    gUpt07BottleneckProbe[base + 6u] = work.materialRecordLoads;
    gUpt07BottleneckProbe[base + 7u] = work.textureSamples;
    [unroll]
    for (uint site = 0u; site < kUpt07WorkSiteCount; ++site)
        gUpt07BottleneckProbe[base + kUpt07WorkCounterCount + site] =
            Upt07WorkSitePacked(work, site);
    gUpt07BottleneckProbe[base + kUpt07WorkCounterCount
        + kUpt07WorkSiteCount] = Upt07WorkBudgetViolationMask(work);
}
#endif
#endif

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

    Upt07WorkCounters work = (Upt07WorkCounters)0;
    Upt07WorkInitialize(work);
#if UPT07_BOTTLENECK_STAGE == 12
    // Initialize this pixel's complete record. No full-buffer clear is needed,
    // including when an invalid receiver returns before any expensive work.
    const uint workBase = index * kUpt07WorkRecordWordCount;
    [unroll]
    for (uint workSlot = 0u; workSlot < kUpt07WorkRecordWordCount;
        ++workSlot)
        gUpt07BottleneckProbe[workBase + workSlot] = 0u;
#endif
    const Upt03PackedReservoir currentPacked = gUpt07CurrentReservoirs[index];
    const Upt07DecodedReservoir current = Upt07DecodeReservoir(currentPacked);
    const Upt04PrimarySurfaceRecord receiver = Upt07LoadCurrentSurface(index);
    if (!Upt04PrimarySurfaceRecordValid(receiver))
    {
        gUpt07CurrentReservoirs[index] = currentPacked;
        return;
    }
#if UPT07_INDIRECT_REPLAY && UPT07_ROUTE_DIAGNOSTICS
    Upt07RouteDiag(0u);
    if (current.valid == 0u || current.logical.hasSelectedSample == 0u)
        Upt07RouteDiag(29u);
    else if (Upt07DirectHistoryEligible(current))
        Upt07RouteDiag(1u);
    else if (current.logical.selected.eventKind == 4u
        && current.logical.selected.pathLength == 1u)
        Upt07RouteDiag(2u);
    else if ((current.logical.selected.flags & kUpt04CandidateFlagGlobal) != 0u
        && current.logical.selected.pathLength == 2u)
        Upt07RouteDiag(3u);
#endif

    // Only a dynamic emissive selection bypasses temporal merge.  Analytic
    // lights remain replayable when their current payload changes; identity
    // remapping, not the persistent-cache flag, owns one-frame continuity.
    if (Upt07DirectHistoryEligible(current))
    {
        uint currentEmissive;
        uint currentDenseIndex;
        Upt04UnifiedLightRecord currentLight;
        if (Upt07ResolveDirectLight(
                current.logical.selected,
                0u,
                currentEmissive,
                currentDenseIndex,
                currentLight,
                work) != 0u
            && !Upt07DirectLightTemporalReplayable(currentLight))
        {
#if UPT07_BOTTLENECK_STAGE == 12
            Upt07PublishWorkBudget(index, work);
#endif
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
    const bool directHistoryEligible = Upt07DirectHistoryEligible(history);
#if UPT07_INDIRECT_REPLAY
    const bool indirectHistoryEligible = Upt07IndirectHistoryEligible(history);
#else
    const bool indirectHistoryEligible = false;
#endif
#if UPT07_BOTTLENECK_STAGE == 1
    uint bottleneckDigest = 2166136261u;
    bottleneckDigest = Upt07BottleneckMix(bottleneckDigest, historyIndex);
    bottleneckDigest = Upt07BottleneckMix(bottleneckDigest, history.valid);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, history.logical.hasSelectedSample);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, history.logical.selected.eventKind);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, directHistoryEligible ? 1u : 0u);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, indirectHistoryEligible ? 1u : 0u);
    Upt07PublishBottleneckProbe(index, currentPacked, bottleneckDigest);
    return;
#endif
#if UPT07_INDIRECT_REPLAY && UPT07_ROUTE_DIAGNOSTICS
    if (history.valid != 0u && history.logical.hasSelectedSample != 0u)
    {
        Upt07RouteDiag(4u);
        const uint historyAge = history.logical.selected.age;
        Upt07RouteDiag(historyAge == 0u ? 20u
            : (historyAge <= 3u ? 21u
                : (historyAge <= 15u ? 22u
                    : (historyAge <= 31u ? 23u : 24u))));
        if (directHistoryEligible)
            Upt07RouteDiag(5u);
        else if (indirectHistoryEligible
            && history.logical.selected.eventKind == 4u)
            Upt07RouteDiag(6u);
        else if (indirectHistoryEligible)
            Upt07RouteDiag(7u);
        else
            Upt07RouteDiag(8u);
    }
    else
    {
        Upt07RouteDiag(28u);
    }
#endif
#if UPT07_INDIRECT_REPLAY
    Upt07PendingIndirectVisibility shiftedIndirectVisibility =
        (Upt07PendingIndirectVisibility)0;
    shiftedIndirectVisibility.ignore = Upt04NoVisibilityIgnore();
    float historyShiftJacobian = 1.0f;
#else
    const float historyShiftJacobian = 1.0f;
#endif
#if UPT10_SHARED_REUSE_ADAPTER
    Upt10ShiftResult sharedHistoryShift = (Upt10ShiftResult)0;
    Upt10ShiftResult sharedCurrentShift = (Upt10ShiftResult)0;
    uint sharedCurrentShiftValid = 0u;
    if (directHistoryEligible || indirectHistoryEligible)
    {
        shiftedValid = Upt07ShiftThroughSharedAdapter<
            UPT07_SELECTED_PRIMARY_MATERIAL_SET>(
                history.logical.selected,
                historyReceiver,
                receiver,
                1u,
                directHistoryEligible ? 1u : 0u,
                historyFailureReason,
                sharedHistoryShift,
                shiftedIndirectVisibility,
                work);
        shifted = sharedHistoryShift.shifted;
        historyShiftJacobian = sharedHistoryShift.shiftJacobian;
    }
#else
    if (directHistoryEligible)
    {
        historyFailureReason = kUpt07RejectShiftInvalid;
        shiftedValid = Upt07ShiftPrimaryDirect<UPT07_SELECTED_PRIMARY_MATERIAL_SET>(
            history.logical.selected,
            receiver,
            shifted,
            work);
        if (shiftedValid != 0u)
            historyFailureReason = shifted.status == kUpt04SampleValidPositive
                ? 0u : kUpt07RejectShiftedValidZero;
    }
    else if (indirectHistoryEligible)
    {
#if UPT07_INDIRECT_REPLAY
#if UPT07_ROUTE_DIAGNOSTICS
        Upt07RouteDiag(9u);
#endif
        historyFailureReason = kUpt07RejectShiftInvalid;
        uint indirectFailureReason = kUpt07RejectIndirectStructure;
        Upt07WorkBeginSite(work, kUpt07WorkSiteHistoryReplay);
        shiftedValid = Upt07ReplayIndirectSample(
            history.logical.selected,
            historyReceiver,
            receiver,
            1u,
            indirectFailureReason,
            shifted,
            shiftedIndirectVisibility,
            historyShiftJacobian,
            work);
        Upt07WorkEndSite(work);
#if UPT07_ROUTE_DIAGNOSTICS
        Upt07RouteDiag(shiftedValid != 0u ? 10u : 11u);
#endif
        if (shiftedValid == 0u)
            historyFailureReason = indirectFailureReason;
        if (shiftedValid != 0u)
            historyFailureReason = shifted.status == kUpt04SampleValidPositive
                ? 0u : kUpt07RejectShiftedValidZero;
#endif
    }
#endif

#if UPT07_BOTTLENECK_STAGE == 2
    uint bottleneckDigest = 2166136261u;
    bottleneckDigest = Upt07BottleneckMix(bottleneckDigest, historyIndex);
    bottleneckDigest = Upt07BottleneckMix(bottleneckDigest, shiftedValid);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, historyFailureReason);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, shifted.eventKind);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, shifted.identity0);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, shifted.identity1);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, asuint(historyShiftJacobian));
    Upt07PublishBottleneckProbe(index, currentPacked, bottleneckDigest);
    return;
#endif

    const uint historyIdentity = shiftedValid != 0u ? shifted.identity0 : 0u;
    Upt07DecodedReservoir seededCurrent = current;
    uint previousBestAdmitted = 0u;
    if ((gUpt07Control.geometryAvailabilityFlags
            & kUpt07PreviousBestSeedFlag) != 0u
        && shiftedValid != 0u
        && directHistoryEligible
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
    const float temporalSelectionRandom =
        Upt07SelectionRandom(pixel, historyIdentity);
    const uint pairwiseMis = (gUpt07Control.geometryAvailabilityFlags
            & kUpt07PairwiseMisFlag) != 0u ? 1u : 0u;
    const bool basicSelectsHistory = pairwiseMis == 0u
        && Upt07BasicTemporalWouldSelectHistory(
            seededCurrent,
            confidenceHistory,
            shifted,
            confidenceShiftedValid,
            temporalSelectionRandom,
            adaptiveMaximumHistoryM,
            historyShiftJacobian);
#if UPT07_INDIRECT_REPLAY && UPT07_ROUTE_DIAGNOSTICS
    Upt07RouteDiag(basicSelectsHistory ? 15u : 16u);
#endif

#if UPT07_BOTTLENECK_STAGE == 6
    uint bottleneckDigest = 2166136261u;
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, previousBestAdmitted);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, confidenceShiftedValid);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, adaptiveMaximumHistoryM);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, basicSelectsHistory ? 1u : 0u);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, seededCurrent.valid);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, seededCurrent.logical.hasSelectedSample);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, seededCurrent.logical.selected.identity0);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, asuint(seededCurrent.logical.weightSum));
    Upt07PublishBottleneckProbe(index, currentPacked, bottleneckDigest);
    return;
#endif

    float currentTargetAtHistory = 0.0f;
    float currentTargetAtCurrent = 0.0f;
    if (seededCurrent.valid != 0u
        && seededCurrent.logical.hasSelectedSample != 0u)
    {
        const Upt03Candidate currentCandidate = seededCurrent.logical.selected;
#if UPT10_SHARED_REUSE_ADAPTER
        const bool currentDirect =
            (currentCandidate.flags & kUpt04CandidateFlagGlobal) == 0u;
        currentTargetAtCurrent = currentDirect
            ? Upt07PrimaryDirectTargetAtReceiver<UPT07_SELECTED_PRIMARY_MATERIAL_SET>(
                currentCandidate, receiver, work)
            : (isfinite(currentCandidate.target)
                && currentCandidate.target > 0.0f
                    ? currentCandidate.target : 0.0f);
#if UPT07_BOTTLENECK_STAGE != 7 && UPT07_BOTTLENECK_STAGE != 8
        if (confidenceShiftedValid != 0u
            && Upt04PrimarySurfaceRecordValid(historyReceiver)
            && (pairwiseMis != 0u || !basicSelectsHistory))
        {
            Upt07PendingIndirectVisibility unusedCurrentVisibility;
            uint currentShiftFailure;
            Upt07WorkBeginSite(work, kUpt07WorkSiteReciprocalReplay);
            sharedCurrentShiftValid =
                Upt07ShiftThroughSharedAdapter<
                    UPT07_SELECTED_PRIMARY_MATERIAL_SET>(
                        currentCandidate,
                        receiver,
                        historyReceiver,
                        0u,
                        currentDirect ? 1u : 0u,
                        currentShiftFailure,
                        sharedCurrentShift,
                        unusedCurrentVisibility,
                        work);
            Upt07WorkEndSite(work);
            if (sharedCurrentShiftValid != 0u
                && sharedCurrentShift.mappingStatus
                    == kUpt10ShiftValidPositive)
                currentTargetAtHistory = sharedCurrentShift.targetTarget;
        }
#endif
#else
        if ((currentCandidate.flags & kUpt04CandidateFlagGlobal) == 0u)
        {
            currentTargetAtCurrent =
                Upt07PrimaryDirectTargetAtReceiver<UPT07_SELECTED_PRIMARY_MATERIAL_SET>(
                    currentCandidate, receiver, work);
#if UPT07_BOTTLENECK_STAGE != 7
            if (confidenceShiftedValid != 0u
                && Upt04PrimarySurfaceRecordValid(historyReceiver)
                && (pairwiseMis != 0u || !basicSelectsHistory))
                currentTargetAtHistory =
                    Upt07PrimaryDirectTargetAtReceiver<UPT07_SELECTED_PRIMARY_MATERIAL_SET>(
                        currentCandidate, historyReceiver, work);
#endif
        }
        else
        {
#if UPT07_INDIRECT_REPLAY
            if (!Upt07IndirectHistoryEligible(seededCurrent))
                currentTargetAtCurrent = 0.0f;
            else
            {
            currentTargetAtCurrent = isfinite(currentCandidate.target)
                    && currentCandidate.target > 0.0f
                ? currentCandidate.target : 0.0f;
#if UPT07_BOTTLENECK_STAGE != 7 && UPT07_BOTTLENECK_STAGE != 8
            if (confidenceShiftedValid != 0u
                && Upt04PrimarySurfaceRecordValid(historyReceiver)
                && (pairwiseMis != 0u || !basicSelectsHistory))
            {
#if UPT07_ROUTE_DIAGNOSTICS
                Upt07RouteDiag(12u);
#endif
                Upt03Candidate currentAtHistory;
                uint currentShiftFailure;
                float unusedCurrentJacobian;
                Upt07PendingIndirectVisibility unusedCurrentVisibility;
                Upt07WorkBeginSite(work, kUpt07WorkSiteReciprocalReplay);
                const uint currentAtHistoryValid = Upt07ReplayIndirectSample(
                        currentCandidate,
                        receiver,
                        historyReceiver,
                        0u,
                        currentShiftFailure,
                        currentAtHistory,
                        unusedCurrentVisibility,
                        unusedCurrentJacobian,
                        work);
                Upt07WorkEndSite(work);
#if UPT07_ROUTE_DIAGNOSTICS
                Upt07RouteDiag(currentAtHistoryValid != 0u ? 13u : 14u);
#endif
                if (currentAtHistoryValid != 0u
                    && currentAtHistory.status == kUpt04SampleValidPositive)
                    currentTargetAtHistory = currentAtHistory.target;
            }
#endif
            }
#endif
        }
#endif
    }

#if UPT07_BOTTLENECK_STAGE == 7 || UPT07_BOTTLENECK_STAGE == 8
    uint bottleneckDigest = 2166136261u;
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, confidenceShiftedValid);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, basicSelectsHistory ? 1u : 0u);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, seededCurrent.logical.selected.eventKind);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, seededCurrent.logical.selected.identity0);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, asuint(currentTargetAtCurrent));
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, asuint(currentTargetAtHistory));
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, adaptiveMaximumHistoryM);
    Upt07PublishBottleneckProbe(index, currentPacked, bottleneckDigest);
    return;
#endif
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

#if UPT07_BOTTLENECK_STAGE == 3 || UPT07_BOTTLENECK_STAGE == 9 || UPT07_BOTTLENECK_STAGE == 10 || UPT07_BOTTLENECK_STAGE == 11
    uint bottleneckDigest = 2166136261u;
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, confidenceShiftedValid);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, basicSelectsHistory ? 1u : 0u);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, asuint(currentTargetAtCurrent));
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, asuint(currentTargetAtHistory));
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, asuint(historyTargetAtHistory));
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, adaptiveMaximumHistoryM);
    Upt07PublishBottleneckProbe(index, currentPacked, bottleneckDigest);
    return;
#endif

#if UPT10_COMMON_GRIS_MERGE
    Upt07TemporalMergeResult merged = Upt10MergeTemporalSharedGris(
        seededCurrent,
        confidenceHistory,
        sharedHistoryShift,
        confidenceShiftedValid,
        sharedCurrentShift,
        sharedCurrentShiftValid,
        currentTargetAtCurrent,
        historyTargetAtHistory,
        historyFailureReason,
        temporalSelectionRandom,
        adaptiveMaximumHistoryM,
        max(gUpt07Control.maximumHistoryAge, 1u),
        float(gUpt07Control.maximumHistoryContributionRatio));
#else
    Upt07TemporalMergeResult merged = Upt07MergeTemporal(
        seededCurrent,
        confidenceHistory,
        shifted,
        confidenceShiftedValid,
        currentTargetAtCurrent,
        currentTargetAtHistory,
        historyTargetAtHistory,
        historyFailureReason,
        temporalSelectionRandom,
        adaptiveMaximumHistoryM,
        max(gUpt07Control.maximumHistoryAge, 1u),
        float(gUpt07Control.maximumHistoryContributionRatio),
        pairwiseMis,
        historyShiftJacobian);
#endif
#if UPT07_INDIRECT_REPLAY && UPT07_ROUTE_DIAGNOSTICS
    if (merged.logical.hasSelectedSample == 0u)
        Upt07RouteDiag(19u);
    else if (merged.selectedHistory != 0u)
    {
        Upt07RouteDiag(17u);
        if ((merged.logical.selected.flags & kUpt04CandidateFlagGlobal) == 0u)
            Upt07RouteDiag(27u);
        else if (merged.logical.selected.eventKind == 4u)
            Upt07RouteDiag(25u);
        else
            Upt07RouteDiag(26u);
    }
    else
        Upt07RouteDiag(18u);
#endif
#if UPT07_INDIRECT_REPLAY && UPT07_EARLY_RECONNECT
    if (merged.selectedHistory != 0u && indirectHistoryEligible)
        Upt07ReconnectDiag(kUpt07ReconnectDiagHistorySelected);
#endif

#if UPT07_BOTTLENECK_STAGE == 4
    uint bottleneckDigest = 2166136261u;
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, merged.logical.hasSelectedSample);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, merged.selectedHistory);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, merged.logical.selected.eventKind);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, merged.logical.selected.identity0);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, asuint(merged.logical.weightSum));
    Upt07PublishBottleneckProbe(index, currentPacked, bottleneckDigest);
    return;
#endif

    // Regression guard: both RTXDI FullSample and the accepted legacy DI lane
    // re-establish final visibility after reuse. UPT-05 deliberately cannot
    // trace, so deleting the old T0 ray left history-selected radiance fully
    // unshadowed and allowed it to overwrite a correct D0 count-only result.
    // Trace only a final winner without a current-frame visibility proof. The
    // indirect replay provider already reconstructed its exact secondary-NEE
    // request, but deliberately did not traverse it before selection. If the
    // winner is occluded (or its request is unavailable), publish the original
    // D0 reservoir rather than retaining any mass from rejected history.
    const uint visibilityProof = kUpt04CandidateFlagVisibilityKnown
        | kUpt04CandidateFlagVisibilityPassed;
    // A visibility bit carried by a history record is only proof for the old
    // frame. selectedHistory must therefore force a new query even when that
    // stale bit is still present. The flag-only test is retained for any
    // current candidate route that reaches T0 without D0 visibility proof.
    const bool mergedIndirect = merged.logical.hasSelectedSample != 0u
        && (merged.logical.selected.flags & kUpt04CandidateFlagGlobal) != 0u;
#if UPT07_BOTTLENECK_STAGE == 5
    // 1 = no query required, 2 = query passed, 3 = query failed/unavailable.
    uint bottleneckVisibilityResult = 1u;
#endif
#if UPT07_INDIRECT_REPLAY
    if (merged.logical.hasSelectedSample != 0u
        && ((merged.selectedHistory != 0u && !mergedIndirect)
            || (merged.logical.selected.flags & visibilityProof) != visibilityProof))
    {
        bool visibilityPassed = false;
        if (mergedIndirect)
        {
            // Only a shifted history secondary-NEE sample can own this
            // transient request. Fresh D0 indirect winners already contain a
            // current-frame proof; all other missing-proof routes fail closed.
            if (merged.selectedHistory != 0u
                && shiftedIndirectVisibility.valid != 0u)
            {
                Upt07WorkBeginSite(
                    work, kUpt07WorkSiteFinalIndirectVisibility);
                const Upt04HitFacts indirectVisibilityHit = Upt07TraceIndirect(
                    shiftedIndirectVisibility.request,
                    1u,
                    shiftedIndirectVisibility.ignore,
                    work);
                Upt07WorkEndSite(work);
                visibilityPassed = indirectVisibilityHit.status
                    != kUpt04HitTriangle;
            }
        }
        else
        {
            Upt04TraceRequest visibility;
            Upt04VisibilityIgnore visibilityIgnore;
            Upt07WorkBeginSite(work, kUpt07WorkSiteFinalDirectVisibility);
            const uint directVisibilityBuilt = Upt07BuildSelectedVisibility(
                merged.logical.selected,
                receiver,
                visibility,
                visibilityIgnore,
                work);
            visibilityPassed = directVisibilityBuilt != 0u
                && Upt07VisibilityPassed(visibility, visibilityIgnore, work);
            Upt07WorkEndSite(work);
        }
#if UPT07_BOTTLENECK_STAGE == 5
        bottleneckVisibilityResult = visibilityPassed ? 2u : 3u;
        if (visibilityPassed)
        {
            merged.logical.selected.flags =
                (merged.logical.selected.flags & ~visibilityProof)
                | visibilityProof;
        }
#else
        if (!visibilityPassed)
        {
#if UPT07_ROUTE_DIAGNOSTICS
            Upt07RouteDiag(31u);
#endif
#if UPT07_BOTTLENECK_STAGE == 12
            Upt07PublishWorkBudget(index, work);
#endif
            gUpt07CurrentReservoirs[index] = currentPacked;
            return;
        }
        merged.logical.selected.flags =
            (merged.logical.selected.flags & ~visibilityProof)
            | visibilityProof;
#endif
    }
#else
    if (merged.logical.hasSelectedSample != 0u
        && ((merged.selectedHistory != 0u && !mergedIndirect)
            || (merged.logical.selected.flags & visibilityProof) != visibilityProof))
    {
        Upt04TraceRequest visibility;
        Upt04VisibilityIgnore visibilityIgnore;
        Upt07WorkBeginSite(work, kUpt07WorkSiteFinalDirectVisibility);
        const uint directVisibilityBuilt = Upt07BuildSelectedVisibility(
                merged.logical.selected,
                receiver,
                visibility,
                visibilityIgnore,
                work);
        const bool directVisibilityPassed = directVisibilityBuilt != 0u
            && Upt07VisibilityPassed(visibility, visibilityIgnore, work);
        Upt07WorkEndSite(work);
        if (!directVisibilityPassed)
        {
#if UPT07_BOTTLENECK_STAGE == 12
            Upt07PublishWorkBudget(index, work);
#endif
            gUpt07CurrentReservoirs[index] = currentPacked;
            return;
        }
        merged.logical.selected.flags =
            (merged.logical.selected.flags & ~visibilityProof)
            | visibilityProof;
    }
#endif
#if UPT07_BOTTLENECK_STAGE == 5
    uint bottleneckDigest = 2166136261u;
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, merged.logical.hasSelectedSample);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, merged.selectedHistory);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, merged.logical.selected.eventKind);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, merged.logical.selected.identity0);
    bottleneckDigest = Upt07BottleneckMix(
        bottleneckDigest, bottleneckVisibilityResult);
    Upt07PublishBottleneckProbe(index, currentPacked, bottleneckDigest);
    return;
#endif
    gUpt07CurrentReservoirs[index] = Upt07PackTemporalResult(merged);
#if UPT07_BOTTLENECK_STAGE == 12
    Upt07PublishWorkBudget(index, work);
#endif
}
