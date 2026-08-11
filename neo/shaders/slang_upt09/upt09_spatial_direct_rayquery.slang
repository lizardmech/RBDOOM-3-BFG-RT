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
#ifndef UPT_DIAGNOSTIC_LAMBERT
#define UPT_DIAGNOSTIC_LAMBERT 0
#endif
#if UPT_DIAGNOSTIC_LAMBERT
import upt07_material_lambert_diagnostic;
#define UPT09_SELECTED_PRIMARY_MATERIAL_SET Upt07LambertDiagnosticMaterialSet
#else
import upt07_material_openpbr;
#define UPT09_SELECTED_PRIMARY_MATERIAL_SET Upt07OpenPbrMaterialSet
#endif
import upt07_material_provider_contract;
import upt07_emissive_material;
import upt07_temporal_reservoir;
import upt09_spatial_direct_bindings;
#ifndef UPT10_SHARED_SPATIAL
#define UPT10_SHARED_SPATIAL 0
#endif
#ifndef UPT31_STORED_DIRECT_SOURCE_TARGET
#define UPT31_STORED_DIRECT_SOURCE_TARGET 0
#endif
#ifndef UPT32_WORKGROUP_RECIPROCAL_PAIRING
#define UPT32_WORKGROUP_RECIPROCAL_PAIRING 0
#endif
#if UPT10_SHARED_SPATIAL
import upt07_work_budget;
import upt07_temporal_indirect;
#include "../slang_upt10/upt10_reuse_contract.slangh"
#endif

#if UPT32_WORKGROUP_RECIPROCAL_PAIRING
// One mapped sample per lane. The group barrier turns the old interleaved
// leader/idle pair into a full-occupancy shift pre-pass followed by one
// publication per lane. Keeping only the mapped result in shared memory
// avoids a third full-resolution reservoir page.
groupshared Upt03PackedReservoir gUpt32ShiftedCandidates[64u];
groupshared uint4 gUpt32ShiftMetadata[64u];
groupshared Upt04TraceRequest gUpt32DirectVisibility[64u];
groupshared Upt04VisibilityIgnore gUpt32DirectVisibilityIgnore[64u];
groupshared Upt07PendingIndirectVisibility gUpt32IndirectVisibility[64u];
groupshared uint gUpt32ShiftValid[64u];
#endif

public static const uint kUpt09MaximumNeighborAttempts = 12u;

public static const float2 kUpt09NeighborOffsets[32] =
{
    float2( 0.096f,  0.071f), float2(-0.154f,  0.141f),
    float2( 0.032f, -0.287f), float2( 0.247f,  0.246f),
    float2(-0.365f, -0.065f), float2( 0.362f, -0.248f),
    float2(-0.135f,  0.492f), float2(-0.282f, -0.451f),
    float2( 0.553f,  0.087f), float2(-0.502f,  0.321f),
    float2( 0.160f, -0.615f), float2( 0.364f,  0.563f),
    float2(-0.681f, -0.157f), float2( 0.648f, -0.331f),
    float2(-0.245f,  0.718f), float2(-0.390f, -0.686f),
    float2( 0.793f,  0.201f), float2(-0.762f,  0.356f),
    float2( 0.313f, -0.803f), float2( 0.407f,  0.779f),
    float2(-0.869f, -0.272f), float2( 0.857f, -0.347f),
    float2(-0.392f,  0.849f), float2(-0.393f, -0.864f),
    float2( 0.908f,  0.358f), float2(-0.929f,  0.306f),
    float2( 0.488f, -0.858f), float2( 0.341f,  0.930f),
    float2(-0.909f, -0.442f), float2( 0.981f, -0.226f),
    float2(-0.589f,  0.829f), float2(-0.242f, -0.971f)
};

public float Upt09Random(uint2 pixel, uint ordinal, uint stream)
{
    uint value = Upt04HashWord(pixel.x ^ (pixel.y * 0x9e3779b9u));
    value = Upt04HashWord(value ^ gUpt09Control.frameSampleIndex);
    value = Upt04HashWord(value ^ (ordinal * 0x85ebca6bu));
    value = Upt04HashWord(value ^ stream);
    return float(value >> 8u) * (1.0f / 16777216.0f);
}

public bool Upt09VisibilityPassed(
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
        gUpt09Scene,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
        0xffu,
        ray);
    while (query.Proceed())
    {
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

public bool Upt09DirectReservoirEligible(Upt07DecodedReservoir decoded)
{
    if (decoded.valid == 0u || decoded.logical.hasSelectedSample == 0u)
        return false;
    const Upt03Candidate candidate = decoded.logical.selected;
    return (candidate.eventKind == 1u || candidate.eventKind == 2u)
        && (candidate.flags & kUpt04CandidateFlagGlobal) == 0u
        && !Upt03IsDirectSpatialRescue(candidate)
        && candidate.pathLength == 1u
        && candidate.reconnectionVertexLength == 0u;
}

public uint Upt09EvaluateDirectSample(
    Upt03Candidate source,
    Upt04PrimarySurfaceRecord receiver,
    out Upt04LocalLightSample sample,
    out uint emissive,
    out uint denseIndex)
{
    sample = (Upt04LocalLightSample)0;
    emissive = source.eventKind == 2u ? 1u : 0u;
    denseIndex = 0xffffffffu;
    if ((source.eventKind != 1u && source.eventKind != 2u)
        || source.identity0 == 0u)
        return 0u;
    denseIndex = source.identity0 - 1u;
    const bool inEmissiveRange = denseIndex >= gUpt09Control.emissiveRangeStart
        && denseIndex - gUpt09Control.emissiveRangeStart
            < gUpt09Control.emissiveRangeCount;
    const bool inAnalyticRange = denseIndex >= gUpt09Control.analyticRangeStart
        && denseIndex - gUpt09Control.analyticRangeStart
            < gUpt09Control.analyticRangeCount;
    if (denseIndex >= gUpt09Control.currentLightCount
        || (emissive != 0u ? !inEmissiveRange : !inAnalyticRange))
        return 0u;
    const Upt04UnifiedLightRecord light = Upt09LoadLight(denseIndex);
    const uint expectedType = emissive != 0u
        ? kUpt04LightEmissiveTriangle : kUpt04LightDoomAnalytic;
    if (light.type != expectedType
        || source.identity1 != Upt04StableLightIdentityFingerprint(light))
        return 0u;

    if (emissive != 0u)
    {
        const Upt04EmissiveGeometrySample replay =
            Upt04ReplayEmissiveTriangleBarycentrics(
                light,
                receiver.worldPositionAndViewDepth.xyz,
                float2(source.sampleCoord0, source.sampleCoord1),
                (gUpt09Control.geometryAvailabilityFlags
                    & kUpt04EmissiveReplayTwoSided) != 0u);
        if (replay.status == kUpt04SampleInvalid)
            return 0u;
        const float4 regionWeights = UptDirectEmissiveRegionWeights(
            replay,
            gUpt09Control.materialCount,
            gUpt09Control.logicalTextureCount,
            gUpt09Control.emissiveScale);
        const uint region = Upt04EmissiveRegionFromBarycentrics(
            replay.barycentrics);
        const Upt04MaterialEmissionEvaluation emission =
            UptDirectEvaluateEmissiveEndpoint(
                replay.materialIndex,
                replay.triangleClassAndFlags,
                replay.emissionTexCoord,
                float2(0.0f),
                float2(0.0f),
                gUpt09Control.materialCount,
                gUpt09Control.logicalTextureCount,
                gUpt09Control.emissiveScale);
        if (emission.valid == 0u)
            return 0u;
        // Preserve valid black mask texels. Otherwise temporal/spatial feedback
        // promotes the triangle-level estimate into a persistent ghost light.
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
                light, gUpt09Control.geometryAvailabilityFlags))
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

public uint Upt09ShiftPrimaryDirectUnshadowed<
    TMaterialSet : IUpt07PrimaryMaterialSet>(
    Upt03Candidate source,
    Upt04PrimarySurfaceRecord receiver,
    out Upt03Candidate shifted,
    out Upt04TraceRequest visibility,
    out Upt04VisibilityIgnore visibilityIgnore)
{
    shifted = (Upt03Candidate)0;
    visibility = (Upt04TraceRequest)0;
    visibilityIgnore = Upt04NoVisibilityIgnore();
    Upt04LocalLightSample sample;
    uint emissive;
    uint denseIndex;
    if (Upt09EvaluateDirectSample(
            source, receiver, sample, emissive, denseIndex) == 0u)
        return 0u;
    const Upt04MaterialEvaluation material =
        TMaterialSet.EvaluatePrimaryDirect(
            receiver, sample.direction, sample.radiance);
    if (material.valid == 0u)
        return 0u;
    Upt04LocalLightRanges ranges;
    ranges.emissiveStart = gUpt09Control.emissiveRangeStart;
    ranges.emissiveCount = gUpt09Control.emissiveRangeCount;
    ranges.analyticStart = gUpt09Control.analyticRangeStart;
    ranges.analyticCount = gUpt09Control.analyticRangeCount;
    const float proposalPdf = Upt04DirectRisIdentityPdf(
        ranges,
        emissive != 0u
            ? kUpt04LightEmissiveTriangle : kUpt04LightDoomAnalytic,
        (source.flags & kUpt04CandidateFlagDirectProposalParity) != 0u
            ? 1u : 0u,
        Upt04ConfiguredEmissiveTrialCount(
            gUpt09Control.geometryAvailabilityFlags))
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
        gUpt09Control.geometryAvailabilityFlags);
    shifted.eventKind = emissive != 0u ? 2u : 1u;
    shifted.lobeMask = material.lobeMask;
    shifted.flags = source.flags
        & kUpt04CandidateFlagDirectProposalParity;
    shifted.age = source.age;
    shifted.pathLength = 1u;
    shifted.reconnectionVertexLength = 0u;
    shifted.identity0 = denseIndex + 1u;
    shifted.identity1 = Upt04StableLightIdentityFingerprint(
        Upt09LoadLight(denseIndex));
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
    shifted.contributionR = contribution.x;
    shifted.contributionG = contribution.y;
    shifted.contributionB = contribution.z;
    shifted.target = Upt04DirectReservoirTarget(
        contribution,
        gUpt09Control.geometryAvailabilityFlags);
    if (!Upt04FinitePositive(shifted.target))
        return 0u;

    if (!Upt04BuildPrimaryVisibilityRequest(
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
            Upt09LoadLight(denseIndex);
        visibilityIgnore = Upt04MakeVisibilityIgnore(
            visibilityLight.instanceId, visibilityLight.primitiveIndex);
    }
    return 1u;
}

public float Upt09PrimaryDirectTargetAtReceiver<
    TMaterialSet : IUpt07PrimaryMaterialSet>(
    Upt03Candidate source,
    Upt04PrimarySurfaceRecord receiver)
{
    Upt03Candidate shifted;
    Upt04TraceRequest visibility;
    Upt04VisibilityIgnore visibilityIgnore;
    return Upt09ShiftPrimaryDirectUnshadowed<TMaterialSet>(
            source, receiver, shifted, visibility, visibilityIgnore) != 0u
        ? max(shifted.target, 0.0f) : 0.0f;
}

public bool Upt09SpatialSurfacesCompatible(
    Upt04PrimarySurfaceRecord receiver,
    Upt04PrimarySurfaceRecord neighbor)
{
    if (!Upt04PrimarySurfaceRecordValid(neighbor)
        || receiver.materialAndSurface.w != neighbor.materialAndSurface.w
        || !Upt04PrimarySurfaceAlphaReuseCompatible(receiver, neighbor)
        || !Upt04PrimarySurfaceMaterialsSimilar(receiver, neighbor))
        return false;
    const float3 receiverNormal = normalize(
        receiver.geometricNormalAndRoughness.xyz);
    const float3 neighborNormal = normalize(
        neighbor.geometricNormalAndRoughness.xyz);
    if (!all(isfinite(receiverNormal)) || !all(isfinite(neighborNormal))
        || dot(receiverNormal, neighborNormal) < 0.85f)
        return false;
    const float receiverDepth = receiver.worldPositionAndViewDepth.w;
    const float neighborDepth = neighbor.worldPositionAndViewDepth.w;
    const float depthTolerance = max(0.25f, abs(receiverDepth) * 0.10f);
    const float planeDistance = abs(dot(
        neighbor.worldPositionAndViewDepth.xyz
            - receiver.worldPositionAndViewDepth.xyz,
        receiverNormal));
    return isfinite(receiverDepth) && isfinite(neighborDepth)
        && abs(receiverDepth - neighborDepth) <= depthTolerance
        && isfinite(planeDistance) && planeDistance <= depthTolerance;
}

public int2 Upt09NeighborPixel(
    uint2 pixel,
    uint ordinal,
    uint2 dimensions)
{
    // Production walks a randomized contiguous portion of the 32-entry table,
    // so an input reservoir cannot be admitted repeatedly merely because two
    // independent draws selected the same offset.  The old with-replacement
    // schedule is retained only by proof mode 2 for a controlled A/B.
    const uint startIndex = min(
        uint(Upt09Random(pixel, 0u, 0x09110001u) * 32.0f), 31u);
    const uint offsetIndex = gUpt09Control.proofMode == 2u
        ? min(uint(Upt09Random(pixel, ordinal, 0x09110001u) * 32.0f), 31u)
        : ((startIndex + ordinal) & 31u);
    const int2 offset = int2(round(
        kUpt09NeighborOffsets[offsetIndex]
            * max(gUpt09Control.neighborRadius, 1.0f)));
    return clamp(
        int2(pixel) + offset,
        int2(0, 0),
        int2(dimensions) - int2(1, 1));
}

public Upt03PackedReservoir Upt09PackSpatialResult(
    Upt03LogicalReservoir logical,
    uint needsRescue)
{
    Upt03PackedReservoir packed = Upt03PackReservoir(logical);
    if (logical.hasSelectedSample == 0u && needsRescue != 0u)
        packed.word0 |= kUpt03PackedNeedsRescueBit;
    return packed;
}

#if UPT10_SHARED_SPATIAL
public bool Upt09SharedReservoirEligible(Upt07DecodedReservoir decoded)
{
    if (decoded.valid == 0u || decoded.logical.hasSelectedSample == 0u)
        return false;
    if ((decoded.logical.selected.flags & kUpt04CandidateFlagGlobal) == 0u)
        return Upt09DirectReservoirEligible(decoded);
    return Upt07IndirectHistoryEligible(decoded);
}

public float Upt09SharedPairwiseWeight(
    float sourceTarget,
    float crossTarget,
    uint sourceM,
    uint crossM)
{
    const float sourceTerm = Upt04FinitePositive(sourceTarget)
        ? float(sourceM) * sourceTarget : 0.0f;
    const float crossTerm = Upt04FinitePositive(crossTarget)
        ? float(crossM) * crossTarget : 0.0f;
    const float denominator = sourceTerm + crossTerm;
    return sourceTerm > 0.0f && Upt04FinitePositive(denominator)
        ? sourceTerm / denominator : 0.0f;
}

public uint Upt09ShiftSharedSample<
    TMaterialSet : IUpt07PrimaryMaterialSet>(
    Upt03Candidate source,
    Upt04PrimarySurfaceRecord sourceReceiver,
    Upt04PrimarySurfaceRecord targetReceiver,
    out Upt10ShiftResult result,
    out Upt04TraceRequest directVisibility,
    out Upt04VisibilityIgnore directVisibilityIgnore,
    out Upt07PendingIndirectVisibility indirectVisibility,
    inout Upt07WorkCounters work)
{
    result = (Upt10ShiftResult)0;
    result.shiftJacobian = 1.0f;
    directVisibility = (Upt04TraceRequest)0;
    directVisibilityIgnore = Upt04NoVisibilityIgnore();
    indirectVisibility = (Upt07PendingIndirectVisibility)0;
    indirectVisibility.ignore = Upt04NoVisibilityIgnore();

    const bool directRoute =
        (source.flags & kUpt04CandidateFlagGlobal) == 0u;
    uint shiftedValid = 0u;
    if (directRoute)
    {
        result.routeLabel = kUpt10RoutePrimaryNee;
#if UPT31_STORED_DIRECT_SOURCE_TARGET
        // Every accepted D0/T0/S0 writer publishes selected.target in the
        // selected sample's current receiver domain. Reuse that canonical
        // source-domain value rather than replaying the same emitter solely
        // to recover one scalar. The target-receiver replay below remains
        // unchanged and still performs all compatibility/PDF/material checks.
        result.sourceTarget = source.target;
#else
        result.sourceTarget =
            Upt09PrimaryDirectTargetAtReceiver<TMaterialSet>(
                source, sourceReceiver);
#endif
        shiftedValid = Upt09ShiftPrimaryDirectUnshadowed<TMaterialSet>(
            source,
            targetReceiver,
            result.shifted,
            directVisibility,
            directVisibilityIgnore);
        result.deferredVisibilityRequired = shiftedValid != 0u
                && result.shifted.status == kUpt04SampleValidPositive
            ? 1u : 0u;
    }
    else
    {
        result.routeLabel = kUpt10RouteReplayOrReconnect;
        result.sourceTarget = source.target;
        uint failureReason;
        shiftedValid = Upt07ReplayIndirectSample(
            source,
            sourceReceiver,
            targetReceiver,
            0u,
            failureReason,
            result.shifted,
            indirectVisibility,
            result.shiftJacobian,
            work);
        result.mappingVisibilityConsumed = shiftedValid;
        result.deferredVisibilityRequired = indirectVisibility.valid;
        if (shiftedValid != 0u)
            result.shifted.age = source.age;
    }
    if (shiftedValid == 0u || !Upt04FinitePositive(result.sourceTarget)
        || !Upt04FinitePositive(result.shiftJacobian))
        return 0u;

    result.targetTarget = max(result.shifted.target, 0.0f);
    result.mappingStatus = result.shifted.status == kUpt04SampleValidPositive
        ? kUpt10ShiftValidPositive : kUpt10ShiftValidZero;
    return 1u;
}

public void Upt09PublishSharedFallback(uint index)
{
    const Upt04PrimarySurfaceRecord receiver = Upt09LoadSurface(index);
    gUpt09OutputReservoirs[index] = Upt04PrimarySurfaceRecordValid(receiver)
        ? gUpt09InputReservoirs[index] : (Upt03PackedReservoir)0;
}

public bool Upt09SharedWinnerVisibilityPassed(
    Upt03Candidate selected,
    Upt04TraceRequest directVisibility,
    Upt04VisibilityIgnore directVisibilityIgnore,
    Upt07PendingIndirectVisibility indirectVisibility,
    inout Upt07WorkCounters work)
{
    const uint visibilityProof = kUpt04CandidateFlagVisibilityKnown
        | kUpt04CandidateFlagVisibilityPassed;
    if ((selected.flags & kUpt04CandidateFlagGlobal) == 0u)
        return Upt09VisibilityPassed(
            directVisibility, directVisibilityIgnore);
    if ((selected.flags & visibilityProof) == visibilityProof)
        return true;
    if (indirectVisibility.valid == 0u)
        return false;
    const Upt04HitFacts visibilityHit = Upt07TraceIndirect(
        indirectVisibility.request,
        1u,
        indirectVisibility.ignore,
        work);
    return visibilityHit.status != kUpt04HitTriangle;
}

// Publish one direction of a reciprocal pair. sourceToTarget is the only
// mapped proposal; targetToSource supplies the reciprocal target needed by the
// target pixel's canonical pairwise-MIS term. Both shifts were computed once
// by the pair leader and are reused by the opposite publication below.
public void Upt09PublishSharedPairDirection(
    uint2 targetPixel,
    uint targetIndex,
    Upt07DecodedReservoir target,
    Upt07DecodedReservoir source,
    Upt10ShiftResult sourceToTarget,
    Upt10ShiftResult targetToSource,
    Upt04TraceRequest sourceDirectVisibility,
    Upt04VisibilityIgnore sourceDirectVisibilityIgnore,
    Upt07PendingIndirectVisibility sourceIndirectVisibility,
    inout Upt07WorkCounters work)
{
    const Upt03PackedReservoir targetPacked =
        gUpt09InputReservoirs[targetIndex];
    const uint targetM = min(
        target.logical.effectiveM, gUpt09Control.maximumInputM);
    const uint sourceM = min(
        source.logical.effectiveM, gUpt09Control.maximumInputM);
    if (targetM == 0u || sourceM == 0u
        || targetM > 0xffffffffu - sourceM)
    {
        gUpt09OutputReservoirs[targetIndex] = targetPacked;
        return;
    }

    Upt10MappedProposalInput sourceProposal =
        (Upt10MappedProposalInput)0;
    sourceProposal.status = sourceToTarget.mappingStatus;
    sourceProposal.routeLabel = sourceToTarget.routeLabel;
    sourceProposal.sourceConfidence = sourceM;
    sourceProposal.sourceScaledWeight = source.logical.weightSum;
    sourceProposal.sourceTarget = sourceToTarget.sourceTarget;
    sourceProposal.targetTarget = sourceToTarget.targetTarget;
    sourceProposal.shiftJacobian = sourceToTarget.shiftJacobian;
    sourceProposal.pairwiseMis = Upt09SharedPairwiseWeight(
        sourceToTarget.sourceTarget,
        sourceToTarget.targetTarget,
        sourceM,
        targetM);

    Upt10MappedProposalInput targetProposal =
        (Upt10MappedProposalInput)0;
    targetProposal.status = kUpt10ShiftValidPositive;
    targetProposal.routeLabel = targetToSource.routeLabel;
    targetProposal.sourceConfidence = targetM;
    targetProposal.sourceScaledWeight = target.logical.weightSum;
    targetProposal.sourceTarget = targetToSource.sourceTarget;
    targetProposal.targetTarget = targetToSource.sourceTarget;
    targetProposal.shiftJacobian = 1.0f;
    targetProposal.pairwiseMis = Upt09SharedPairwiseWeight(
        targetToSource.sourceTarget,
        targetToSource.targetTarget,
        targetM,
        sourceM);

    Upt10MergeControl mergeControl = (Upt10MergeControl)0;
    mergeControl.winnerRandom = Upt09Random(
        targetPixel, 0u, 0x09110004u);
    const Upt10MergeResult merge = Upt10MergeMappedPair(
        sourceProposal, targetProposal, mergeControl);
    if (merge.selectedOrdinal == 0xffffffffu
        || !Upt04FinitePositive(merge.totalMappedMass))
    {
        gUpt09OutputReservoirs[targetIndex] = targetPacked;
        return;
    }

    Upt03LogicalReservoir output = target.logical;
    output.effectiveM = merge.outputConfidence;
    output.weightSum = merge.totalMappedMass;
    const bool selectedSource = merge.selectedOrdinal == 0u;
    if (selectedSource)
        output.selected = sourceToTarget.shifted;
    else
        output.selected.target = targetToSource.sourceTarget;

    if (selectedSource && !Upt09SharedWinnerVisibilityPassed(
            output.selected,
            sourceDirectVisibility,
            sourceDirectVisibilityIgnore,
            sourceIndirectVisibility,
            work))
    {
        gUpt09OutputReservoirs[targetIndex] = targetPacked;
        return;
    }

    if (selectedSource)
    {
        const uint visibilityProof = kUpt04CandidateFlagVisibilityKnown
            | kUpt04CandidateFlagVisibilityPassed;
        output.selected.flags =
            (output.selected.flags & ~visibilityProof) | visibilityProof;
    }
    gUpt09OutputReservoirs[targetIndex] = Upt03PackReservoir(output);
}

// Enhanced paired spatial reuse: exactly one lane owns a disjoint two-pixel
// pair, computes A->B and B->A once, then publishes both directions. Four
// phases alternate horizontal/vertical adjacency and shift the pairing origin
// so the same fixed edge is not retained every frame. Non-leader partner lanes
// issue no replay work and perform no writes.
public void Upt09ExecuteSharedReciprocalPair(uint2 pixel, uint index)
{
    const uint2 dimensions = uint2(
        gUpt09Control.renderWidth, gUpt09Control.renderHeight);
    const uint phase = gUpt09Control.frameSampleIndex & 3u;
    const bool vertical = phase >= 2u;
    const bool shiftedOrigin = (phase & 1u) != 0u;
    const uint coordinate = vertical ? pixel.y : pixel.x;
    const uint extent = vertical ? dimensions.y : dimensions.x;
    const uint leaderParity = shiftedOrigin ? 1u : 0u;

    if (shiftedOrigin && coordinate == 0u)
    {
        Upt09PublishSharedFallback(index);
        return;
    }
    if ((coordinate & 1u) != leaderParity)
        return;
    if (coordinate + 1u >= extent)
    {
        Upt09PublishSharedFallback(index);
        return;
    }

    const uint2 partnerPixel = pixel
        + (vertical ? uint2(0u, 1u) : uint2(1u, 0u));
    const uint partnerIndex = partnerPixel.y * dimensions.x + partnerPixel.x;
    if (partnerIndex >= gUpt09Control.surfaceCount)
    {
        Upt09PublishSharedFallback(index);
        return;
    }

    const Upt04PrimarySurfaceRecord receiver = Upt09LoadSurface(index);
    const Upt04PrimarySurfaceRecord partnerReceiver =
        Upt09LoadSurface(partnerIndex);
    if (!Upt04PrimarySurfaceRecordValid(receiver)
        || !Upt04PrimarySurfaceRecordValid(partnerReceiver))
    {
        gUpt09OutputReservoirs[index] = Upt04PrimarySurfaceRecordValid(receiver)
            ? gUpt09InputReservoirs[index] : (Upt03PackedReservoir)0;
        gUpt09OutputReservoirs[partnerIndex] =
            Upt04PrimarySurfaceRecordValid(partnerReceiver)
            ? gUpt09InputReservoirs[partnerIndex] : (Upt03PackedReservoir)0;
        return;
    }
    if (!Upt09SpatialSurfacesCompatible(receiver, partnerReceiver)
        || !Upt09SpatialSurfacesCompatible(partnerReceiver, receiver))
    {
        gUpt09OutputReservoirs[index] = gUpt09InputReservoirs[index];
        gUpt09OutputReservoirs[partnerIndex] =
            gUpt09InputReservoirs[partnerIndex];
        return;
    }

    const Upt07DecodedReservoir center = Upt07DecodeReservoir(
        gUpt09InputReservoirs[index]);
    const Upt07DecodedReservoir partner = Upt07DecodeReservoir(
        gUpt09InputReservoirs[partnerIndex]);
    if (!Upt09SharedReservoirEligible(center)
        || !Upt09SharedReservoirEligible(partner))
    {
        gUpt09OutputReservoirs[index] = gUpt09InputReservoirs[index];
        gUpt09OutputReservoirs[partnerIndex] =
            gUpt09InputReservoirs[partnerIndex];
        return;
    }

    // The accepted duplication path identifies a propagated path by its
    // persisted replay key. Never stream the exact same selected path as both
    // canonical and partner proposals merely because it occupies two pixels.
    const uint centerSampleId = center.logical.selected.replayKey
        & ~kUpt03ReplayKeyRescueBit;
    const uint partnerSampleId = partner.logical.selected.replayKey
        & ~kUpt03ReplayKeyRescueBit;
    if (centerSampleId != 0u && centerSampleId == partnerSampleId)
    {
        gUpt09OutputReservoirs[index] = gUpt09InputReservoirs[index];
        gUpt09OutputReservoirs[partnerIndex] =
            gUpt09InputReservoirs[partnerIndex];
        return;
    }

    Upt07WorkCounters work = (Upt07WorkCounters)0;
    Upt07WorkInitialize(work);
    Upt10ShiftResult centerToPartner;
    Upt04TraceRequest centerDirectVisibility;
    Upt04VisibilityIgnore centerDirectVisibilityIgnore;
    Upt07PendingIndirectVisibility centerIndirectVisibility;
    const uint centerShiftValid =
        Upt09ShiftSharedSample<UPT09_SELECTED_PRIMARY_MATERIAL_SET>(
            center.logical.selected,
            receiver,
            partnerReceiver,
            centerToPartner,
            centerDirectVisibility,
            centerDirectVisibilityIgnore,
            centerIndirectVisibility,
            work);

    Upt10ShiftResult partnerToCenter;
    Upt04TraceRequest partnerDirectVisibility;
    Upt04VisibilityIgnore partnerDirectVisibilityIgnore;
    Upt07PendingIndirectVisibility partnerIndirectVisibility;
    const uint partnerShiftValid =
        Upt09ShiftSharedSample<UPT09_SELECTED_PRIMARY_MATERIAL_SET>(
            partner.logical.selected,
            partnerReceiver,
            receiver,
            partnerToCenter,
            partnerDirectVisibility,
            partnerDirectVisibilityIgnore,
            partnerIndirectVisibility,
            work);
    if (centerShiftValid == 0u || partnerShiftValid == 0u)
    {
        gUpt09OutputReservoirs[index] = gUpt09InputReservoirs[index];
        gUpt09OutputReservoirs[partnerIndex] =
            gUpt09InputReservoirs[partnerIndex];
        return;
    }

    Upt09PublishSharedPairDirection(
        pixel,
        index,
        center,
        partner,
        partnerToCenter,
        centerToPartner,
        partnerDirectVisibility,
        partnerDirectVisibilityIgnore,
        partnerIndirectVisibility,
        work);
    Upt09PublishSharedPairDirection(
        partnerPixel,
        partnerIndex,
        partner,
        center,
        centerToPartner,
        partnerToCenter,
        centerDirectVisibility,
        centerDirectVisibilityIgnore,
        centerIndirectVisibility,
        work);
}

#if UPT32_WORKGROUP_RECIPROCAL_PAIRING
public uint Upt32PairMask(uint2 groupPixel)
{
    uint seed = Upt04HashWord(gUpt09Control.frameSampleIndex ^ 0x32f0a511u);
    seed = Upt04HashWord(seed ^ (groupPixel.x * 0x9e3779b9u));
    seed = Upt04HashWord(seed ^ (groupPixel.y * 0x85ebca6bu));
    // XOR by any nonzero six-bit mask is a fixed-point-free involution over
    // the 64 lanes. Unlike the old four adjacent phases, this exposes each
    // pixel to a wider, decorrelated partner while retaining exact pairing.
    return 1u + (seed % 63u);
}

public uint2 Upt32TranslatedPixel(
    uint2 dispatchPixel,
    uint2 dimensions)
{
    // When the image has exact 8x8 coverage, translate the complete dispatch
    // lattice by a frame-varying toroidal offset. This is a bijection, so each
    // output is still written exactly once, while the workgroup boundary moves
    // across the image instead of leaving a persistent 8x8 reuse partition.
    // Non-multiple extents retain the unshifted edge-safe mapping.
    if ((dimensions.x & 7u) != 0u || (dimensions.y & 7u) != 0u)
        return dispatchPixel;
    uint seed = Upt04HashWord(
        gUpt09Control.frameSampleIndex ^ 0x32b0ad71u);
    const uint2 offset = uint2(seed & 7u, (seed >> 3u) & 7u);
    return (dispatchPixel + offset) % dimensions;
}

public Upt03PackedReservoir Upt32PackShiftedCandidate(
    Upt03Candidate candidate)
{
    Upt03LogicalReservoir logical = (Upt03LogicalReservoir)0;
    logical.hasSelectedSample = 1u;
    logical.effectiveM = 1u;
    logical.weightSum = 1.0f;
    logical.selected = candidate;
    return Upt03PackReservoir(logical);
}

public Upt10ShiftResult Upt32LoadShiftResult(uint laneIndex)
{
    Upt10ShiftResult result = (Upt10ShiftResult)0;
    const Upt07DecodedReservoir decoded = Upt07DecodeReservoir(
        gUpt32ShiftedCandidates[laneIndex]);
    const uint4 metadata = gUpt32ShiftMetadata[laneIndex];
    result.shifted = decoded.logical.selected;
    result.sourceTarget = asfloat(metadata.x);
    result.targetTarget = asfloat(metadata.y);
    result.shiftJacobian = asfloat(metadata.z);
    result.mappingStatus = metadata.w & 0xffu;
    result.routeLabel = (metadata.w >> 8u) & 0xffu;
    return result;
}

public void Upt32StoreInvalidShift(uint laneIndex)
{
    gUpt32ShiftedCandidates[laneIndex] = (Upt03PackedReservoir)0;
    gUpt32ShiftMetadata[laneIndex] = uint4(0u, 0u, 0u, 0u);
    gUpt32DirectVisibility[laneIndex] = (Upt04TraceRequest)0;
    gUpt32DirectVisibilityIgnore[laneIndex] = Upt04NoVisibilityIgnore();
    gUpt32IndirectVisibility[laneIndex] =
        (Upt07PendingIndirectVisibility)0;
    gUpt32ShiftValid[laneIndex] = 0u;
}

public void Upt32PrepareLaneShift(
    uint2 pixel,
    uint index,
    uint2 partnerPixel,
    uint partnerIndex,
    uint laneIndex,
    uint laneValid,
    uint partnerValid)
{
    Upt32StoreInvalidShift(laneIndex);
    if (laneValid == 0u || partnerValid == 0u)
        return;

    const Upt04PrimarySurfaceRecord receiver = Upt09LoadSurface(index);
    const Upt04PrimarySurfaceRecord partnerReceiver =
        Upt09LoadSurface(partnerIndex);
    if (!Upt04PrimarySurfaceRecordValid(receiver)
        || !Upt04PrimarySurfaceRecordValid(partnerReceiver)
        || !Upt09SpatialSurfacesCompatible(receiver, partnerReceiver)
        || !Upt09SpatialSurfacesCompatible(partnerReceiver, receiver))
    {
        return;
    }

    const Upt07DecodedReservoir center = Upt07DecodeReservoir(
        gUpt09InputReservoirs[index]);
    const Upt07DecodedReservoir partner = Upt07DecodeReservoir(
        gUpt09InputReservoirs[partnerIndex]);
    if (!Upt09SharedReservoirEligible(center)
        || !Upt09SharedReservoirEligible(partner))
    {
        return;
    }

    const uint centerSampleId = center.logical.selected.replayKey
        & ~kUpt03ReplayKeyRescueBit;
    const uint partnerSampleId = partner.logical.selected.replayKey
        & ~kUpt03ReplayKeyRescueBit;
    if (centerSampleId != 0u && centerSampleId == partnerSampleId)
        return;

    Upt07WorkCounters work = (Upt07WorkCounters)0;
    Upt07WorkInitialize(work);
    Upt10ShiftResult shifted;
    Upt04TraceRequest directVisibility;
    Upt04VisibilityIgnore directVisibilityIgnore;
    Upt07PendingIndirectVisibility indirectVisibility;
    const uint shiftedValid =
        Upt09ShiftSharedSample<UPT09_SELECTED_PRIMARY_MATERIAL_SET>(
            center.logical.selected,
            receiver,
            partnerReceiver,
            shifted,
            directVisibility,
            directVisibilityIgnore,
            indirectVisibility,
            work);
    if (shiftedValid == 0u)
        return;

    gUpt32ShiftedCandidates[laneIndex] =
        Upt32PackShiftedCandidate(shifted.shifted);
    gUpt32ShiftMetadata[laneIndex] = uint4(
        asuint(shifted.sourceTarget),
        asuint(shifted.targetTarget),
        asuint(shifted.shiftJacobian),
        (shifted.mappingStatus & 0xffu)
            | ((shifted.routeLabel & 0xffu) << 8u));
    gUpt32DirectVisibility[laneIndex] = directVisibility;
    gUpt32DirectVisibilityIgnore[laneIndex] = directVisibilityIgnore;
    gUpt32IndirectVisibility[laneIndex] = indirectVisibility;
    gUpt32ShiftValid[laneIndex] = 1u;
}

public void Upt32ExecuteWorkgroupReciprocalPair(
    uint3 dispatchThreadId,
    uint3 groupThreadId,
    uint3 groupId)
{
    const uint laneIndex = groupThreadId.y * 8u + groupThreadId.x;
    const uint2 dimensions = uint2(
        gUpt09Control.renderWidth, gUpt09Control.renderHeight);
    const uint2 pixel = Upt32TranslatedPixel(
        dispatchThreadId.xy, dimensions);
    const uint index = pixel.y * dimensions.x + pixel.x;
    const uint laneValid = pixel.x < dimensions.x
            && pixel.y < dimensions.y
            && index < gUpt09Control.surfaceCount
        ? 1u : 0u;

    const uint pairMask = Upt32PairMask(groupId.xy);
    const uint partnerLaneIndex = laneIndex ^ pairMask;
    const uint2 partnerLocal = uint2(
        partnerLaneIndex & 7u, partnerLaneIndex >> 3u);
    const uint2 partnerDispatchPixel = groupId.xy * 8u + partnerLocal;
    const uint2 partnerPixel = Upt32TranslatedPixel(
        partnerDispatchPixel, dimensions);
    const uint partnerIndex =
        partnerPixel.y * dimensions.x + partnerPixel.x;
    // Toroidal translation is only a scheduling device.  Do not let a pair
    // crossing the image wrap reconnect the opposite screen edges: the
    // surface tests intentionally tolerate coplanar separation and therefore
    // cannot by themselves distinguish that scheduling seam.
    const uint2 pairScreenDelta = uint2(
        max(pixel.x, partnerPixel.x) - min(pixel.x, partnerPixel.x),
        max(pixel.y, partnerPixel.y) - min(pixel.y, partnerPixel.y));
    const uint pairScreenLocal = pairScreenDelta.x <= 7u
            && pairScreenDelta.y <= 7u
        ? 1u : 0u;
    const uint partnerValid = partnerPixel.x < dimensions.x
            && partnerPixel.y < dimensions.y
            && partnerIndex < gUpt09Control.surfaceCount
            && pairScreenLocal != 0u
        ? 1u : 0u;

    // Scope the large receiver/reservoir/mapping state before the barrier so
    // it is not live across the resampling half of the kernel.
    {
        Upt32PrepareLaneShift(
            pixel,
            index,
            partnerPixel,
            partnerIndex,
            laneIndex,
            laneValid,
            partnerValid);
    }
    GroupMemoryBarrierWithGroupSync();

    if (laneValid == 0u)
        return;
    if (partnerValid == 0u
        || gUpt32ShiftValid[laneIndex] == 0u
        || gUpt32ShiftValid[partnerLaneIndex] == 0u)
    {
        Upt09PublishSharedFallback(index);
        return;
    }

    const Upt04PrimarySurfaceRecord receiver = Upt09LoadSurface(index);
    const Upt07DecodedReservoir center = Upt07DecodeReservoir(
        gUpt09InputReservoirs[index]);
    const Upt07DecodedReservoir partner = Upt07DecodeReservoir(
        gUpt09InputReservoirs[partnerIndex]);
    if (!Upt04PrimarySurfaceRecordValid(receiver)
        || !Upt09SharedReservoirEligible(center)
        || !Upt09SharedReservoirEligible(partner))
    {
        Upt09PublishSharedFallback(index);
        return;
    }

    const Upt10ShiftResult centerToPartner =
        Upt32LoadShiftResult(laneIndex);
    const Upt10ShiftResult partnerToCenter =
        Upt32LoadShiftResult(partnerLaneIndex);
    Upt07WorkCounters work = (Upt07WorkCounters)0;
    Upt07WorkInitialize(work);
    Upt09PublishSharedPairDirection(
        pixel,
        index,
        center,
        partner,
        partnerToCenter,
        centerToPartner,
        gUpt32DirectVisibility[partnerLaneIndex],
        gUpt32DirectVisibilityIgnore[partnerLaneIndex],
        gUpt32IndirectVisibility[partnerLaneIndex],
        work);
}
#endif
#endif

[numthreads(8, 8, 1)]
void main(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint3 groupThreadId : SV_GroupThreadID,
    uint3 groupId : SV_GroupID)
{
#if UPT32_WORKGROUP_RECIPROCAL_PAIRING
    Upt32ExecuteWorkgroupReciprocalPair(
        dispatchThreadId, groupThreadId, groupId);
    return;
#else
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gUpt09Control.renderWidth
        || pixel.y >= gUpt09Control.renderHeight)
        return;
    const uint index = pixel.y * gUpt09Control.renderWidth + pixel.x;
    if (index >= gUpt09Control.surfaceCount)
        return;

    // This is an exact ownership control: S0 still dispatches, publishes the
    // alternate page, and participates in the normal page flip, but performs
    // no decode, reuse, visibility, or normalization.  Any image difference
    // from spatial-off in this mode is therefore outside the S0 estimator.
    if (gUpt09Control.proofMode == 1u)
    {
        gUpt09OutputReservoirs[index] = gUpt09InputReservoirs[index];
        return;
    }

#if UPT10_SHARED_SPATIAL
    Upt09ExecuteSharedReciprocalPair(pixel, index);
    return;
#else

    const Upt04PrimarySurfaceRecord receiver = Upt09LoadSurface(index);
    if (!Upt04PrimarySurfaceRecordValid(receiver))
    {
        gUpt09OutputReservoirs[index] = (Upt03PackedReservoir)0;
        return;
    }

    const Upt07DecodedReservoir center =
        Upt07DecodeReservoir(gUpt09InputReservoirs[index]);
    const bool centerSelected = Upt09DirectReservoirEligible(center);
    const bool pairwiseProof = gUpt09Control.proofMode == 6u;
    // Mode 4 mirrors the accepted legacy DI admission boundary exactly: an
    // empty center is not spatially rescued.  It is a proof control for the
    // distinction between ordinary selected-sample reuse and UPT09's separate
    // disocclusion/empty-center rescue policy.
    if ((gUpt09Control.proofMode == 4u || pairwiseProof) && !centerSelected)
    {
        gUpt09OutputReservoirs[index] = gUpt09InputReservoirs[index];
        return;
    }
    const uint centerM = center.valid != 0u
        ? min(center.logical.effectiveM, gUpt09Control.maximumInputM) : 0u;
    const float centerReceiverTarget = centerSelected
        && (gUpt09Control.proofMode == 5u || pairwiseProof)
        ? Upt09PrimaryDirectTargetAtReceiver<UPT09_SELECTED_PRIMARY_MATERIAL_SET>(
            center.logical.selected, receiver)
        : (centerSelected ? center.logical.selected.target : 0.0f);
    uint outputM = centerM;
    float totalMass = centerSelected && !pairwiseProof
        ? center.logical.weightSum * float(centerM) : 0.0f;
    Upt03Candidate selected = centerSelected && !pairwiseProof
        ? center.logical.selected : (Upt03Candidate)0;
    float selectedSourceTarget = centerSelected
        ? centerReceiverTarget
        : 0.0f;
    float selectedReceiverTarget = centerSelected && !pairwiseProof
        ? centerReceiverTarget : 0.0f;
    uint selectedFromNeighbor = 0u;
    Upt04TraceRequest selectedVisibility = (Upt04TraceRequest)0;
    Upt04VisibilityIgnore selectedVisibilityIgnore =
        Upt04NoVisibilityIgnore();

    int2 acceptedPixels[kUpt09MaximumNeighborAttempts];
    uint acceptedM[kUpt09MaximumNeighborAttempts];
    uint acceptedCount = 0u;
    int2 sampledPixels[kUpt09MaximumNeighborAttempts];
    uint sampledCount = 0u;
    const uint requestedAttempts = min(
        centerSelected
            ? gUpt09Control.regularNeighborCount
            : gUpt09Control.rescueNeighborCount,
        kUpt09MaximumNeighborAttempts);
    const uint2 dimensions = uint2(
        gUpt09Control.renderWidth, gUpt09Control.renderHeight);
    const uint pairwiseK = max(requestedAttempts, 1u);
    float pairwiseCanonicalMisSum = 0.0f;
    uint pairwisePairsSeen = 0u;
    [loop]
    for (uint ordinal = 0u; ordinal < requestedAttempts; ++ordinal)
    {
        const int2 neighborPixel = Upt09NeighborPixel(
            pixel, ordinal, dimensions);
        if (all(neighborPixel == int2(pixel)))
            continue;
        // Distinct table entries can still collapse to the same edge pixel
        // after clamping.  Never stream one physical reservoir twice.  Proof
        // mode 2 deliberately retains the old behavior for comparison.
        bool duplicatePixel = false;
        if (gUpt09Control.proofMode != 2u)
        {
            [loop]
            for (uint sampledIndex = 0u;
                 sampledIndex < sampledCount;
                 ++sampledIndex)
            {
                duplicatePixel = duplicatePixel
                    || all(sampledPixels[sampledIndex] == neighborPixel);
            }
            if (duplicatePixel)
                continue;
            sampledPixels[sampledCount] = neighborPixel;
            ++sampledCount;
        }
        const uint neighborIndex = uint(neighborPixel.y) * dimensions.x
            + uint(neighborPixel.x);
        const Upt04PrimarySurfaceRecord neighborSurface =
            Upt09LoadSurface(neighborIndex);
        if (!Upt09SpatialSurfacesCompatible(receiver, neighborSurface))
            continue;
        const Upt07DecodedReservoir neighbor = Upt07DecodeReservoir(
            gUpt09InputReservoirs[neighborIndex]);
        const uint neighborM = neighbor.valid != 0u
            ? min(neighbor.logical.effectiveM,
                gUpt09Control.maximumInputM)
            : 0u;

        // Pairwise MIS compares the canonical technique with every physical,
        // surface-compatible neighbor technique before looking at the
        // neighbor's selected sample.  This is deliberately not the failing
        // post-hoc basic-correction denominator: each pair's two balance
        // weights sum locally, so a selected light having almost no support
        // on other neighbors cannot collapse a global divisor and recursively
        // amplify its published weight.
        if (pairwiseProof)
        {
            ++pairwisePairsSeen;
            const float centerTargetAtNeighbor = neighborM > 0u
                ? Upt09PrimaryDirectTargetAtReceiver<UPT09_SELECTED_PRIMARY_MATERIAL_SET>(
                    center.logical.selected, neighborSurface)
                : 0.0f;
            const float canonicalTerm = float(centerM)
                * centerReceiverTarget;
            const float pairDenominator = float(pairwiseK)
                    * float(neighborM) * centerTargetAtNeighbor
                + canonicalTerm;
            pairwiseCanonicalMisSum += Upt04FinitePositive(pairDenominator)
                ? canonicalTerm / pairDenominator
                : 1.0f;
        }

        if (neighbor.valid == 0u)
            continue;

        if (neighborM == 0u || outputM > 0xffffffffu - neighborM)
            continue;

        // A count-only record is a defined zero trial, not an invalid
        // reservoir. D0 deliberately publishes M=1 without a selected sample
        // when its complete proposal schedule finds no positive candidate.
        // Dropping those records here conditioned the spatial estimator on
        // neighbors that happened to find illumination: adding neighbors
        // brightened sparse surfaces, while reducing the schedule made the
        // same surfaces dark. Preserve their confidence and include their
        // source domain in the later bias-correction denominator, but never
        // allow them to become the selected sample or issue a visibility ray.
        if (neighbor.logical.hasSelectedSample == 0u)
        {
            if (gUpt09Control.proofMode == 4u)
                continue;
            outputM += neighborM;
            acceptedPixels[acceptedCount] = neighborPixel;
            acceptedM[acceptedCount] = neighborM;
            ++acceptedCount;
            continue;
        }
        if (!Upt09DirectReservoirEligible(neighbor))
            continue;

        Upt03Candidate shifted;
        Upt04TraceRequest visibility;
        Upt04VisibilityIgnore visibilityIgnore;
        if (Upt09ShiftPrimaryDirectUnshadowed<UPT09_SELECTED_PRIMARY_MATERIAL_SET>(
                neighbor.logical.selected,
                receiver,
                shifted,
                visibility,
                visibilityIgnore) == 0u)
            continue;
        outputM += neighborM;
        acceptedPixels[acceptedCount] = neighborPixel;
        acceptedM[acceptedCount] = neighborM;
        ++acceptedCount;

        // Basic correction needs the target of this sample in its source
        // domain twice: once to shift the reservoir mass and, if it wins, once
        // as the normalization numerator.  Treating the packed target as an
        // immutable source value is not safe after prior temporal/spatial
        // reuse.  Mode 5 isolates that contract by replaying the light at the
        // actual neighbor surface; modes 0..4 retain their prior behavior for
        // a strict runtime A/B.
        const float neighborSourceTarget =
            (gUpt09Control.proofMode == 5u || pairwiseProof)
            ? Upt09PrimaryDirectTargetAtReceiver<UPT09_SELECTED_PRIMARY_MATERIAL_SET>(
                neighbor.logical.selected, neighborSurface)
            : neighbor.logical.selected.target;
        float neighborMass = 0.0f;
        if (shifted.status == kUpt04SampleValidPositive
            && Upt04FinitePositive(neighborSourceTarget))
        {
            if (pairwiseProof)
            {
                // m_i = M_i p_i / (k M_i p_i + M_c p_c).  Direct-light
                // replay has no reconnection Jacobian: p_i is the freshly
                // evaluated target at the donor and p_c is the shifted target
                // at this receiver.  MIS already carries confidence, so do
                // not multiply by M a second time.
                const float pairDenominator = float(pairwiseK)
                        * float(neighborM) * neighborSourceTarget
                    + float(centerM) * shifted.target;
                const float neighborMis = Upt04FinitePositive(pairDenominator)
                    ? float(neighborM) * neighborSourceTarget
                        / pairDenominator
                    : 0.0f;
                const float neighborUcw = Upt03ScaledWeightToUcw(
                    neighbor.logical.weightSum, neighborSourceTarget);
                neighborMass = neighborMis * shifted.target * neighborUcw;
            }
            else
            {
                neighborMass = neighbor.logical.weightSum * float(neighborM)
                    * (shifted.target / neighborSourceTarget);
            }
        }
        if (!isfinite(neighborMass) || neighborMass <= 0.0f)
            continue;
        const float newMass = totalMass + neighborMass;
        if (!Upt04FinitePositive(newMass))
            continue;
        if (selected.status != kUpt04SampleValidPositive
            || Upt09Random(pixel, ordinal, 0x09110002u) * newMass
                < neighborMass)
        {
            selected = shifted;
            selectedSourceTarget = neighborSourceTarget;
            selectedReceiverTarget = shifted.target;
            selectedFromNeighbor = 1u;
            selectedVisibility = visibility;
            selectedVisibilityIgnore = visibilityIgnore;
        }
        totalMass = newMass;
    }

    if (pairwiseProof)
    {
        // A missing physical pair gives the canonical technique the complete
        // pair weight.  Average over the configured k pairs, then stream the
        // canonical candidate exactly once.  This mirrors the normalization
        // proof in the repo-owned clean-room GI pairwise implementation while
        // using UPT's direct-light target/replay contract.
        const uint missingPairs = pairwiseK > pairwisePairsSeen
            ? pairwiseK - pairwisePairsSeen : 0u;
        const float canonicalMis =
            (pairwiseCanonicalMisSum + float(missingPairs))
            / float(pairwiseK);
        const float canonicalUcw = Upt03ScaledWeightToUcw(
            center.logical.weightSum, centerReceiverTarget);
        const float canonicalMass = canonicalMis * centerReceiverTarget
            * canonicalUcw;
        const float newMass = totalMass + canonicalMass;
        if (Upt04FinitePositive(canonicalMass)
            && Upt04FinitePositive(newMass)
            && (selected.status != kUpt04SampleValidPositive
                || Upt09Random(pixel, pairwiseK, 0x09110003u) * newMass
                    < canonicalMass))
        {
            selected = center.logical.selected;
            selected.target = centerReceiverTarget;
            selectedSourceTarget = centerReceiverTarget;
            selectedReceiverTarget = centerReceiverTarget;
            selectedFromNeighbor = 0u;
            selectedVisibility = (Upt04TraceRequest)0;
            selectedVisibilityIgnore = Upt04NoVisibilityIgnore();
        }
        if (Upt04FinitePositive(newMass))
            totalMass = newMass;
    }

    Upt03LogicalReservoir result = (Upt03LogicalReservoir)0;
    result.effectiveM = outputM;
    if (selected.status != kUpt04SampleValidPositive
        || !Upt04FinitePositive(totalMass)
        || !Upt04FinitePositive(selectedSourceTarget))
    {
        gUpt09OutputReservoirs[index] = Upt09PackSpatialResult(result, 1u);
        return;
    }

    float finalizedMeanWeight = 0.0f;
    if (pairwiseProof)
    {
        // Pair weights already form a partition of unity. totalMass is the
        // current-domain target-scaled UCW and is therefore exactly UPT's
        // persisted weight convention. Do not divide by the selected target:
        // that would persist UCW while resolve still expects pHat * UCW.
        finalizedMeanWeight = totalMass;
    }
    else
    {
        float normalizationDenominator = selected.target * float(centerM);
        [loop]
        for (uint acceptedIndex = 0u;
             acceptedIndex < acceptedCount;
             ++acceptedIndex)
        {
            const int2 neighborPixel = acceptedPixels[acceptedIndex];
            const uint neighborIndex = uint(neighborPixel.y) * dimensions.x
                + uint(neighborPixel.x);
            normalizationDenominator +=
                Upt09PrimaryDirectTargetAtReceiver<UPT09_SELECTED_PRIMARY_MATERIAL_SET>(
                    selected,
                    Upt09LoadSurface(neighborIndex))
                * float(acceptedM[acceptedIndex]);
        }
        // Mode 3 removes only the pairwise-MIS/basic-correction denominator.
        // It keeps the same surfaces, shifted candidates, selection, and
        // visibility, and instead finalizes the streamed scalar weight with
        // the ordinary RIS 1/M estimator.
        // RTXDI BASIC correction uses the selected sample's target in its
        // supplying domain as pi. UPT stores pHat_receiver(selected) * W;
        // pHat_receiver cancels the final RTXDI denominator, leaving the same
        // source-domain numerator here.
        finalizedMeanWeight = gUpt09Control.proofMode == 3u
            ? totalMass / float(outputM)
            : totalMass * selectedSourceTarget / normalizationDenominator;
    }
    if (!Upt04FinitePositive(finalizedMeanWeight))
    {
        gUpt09OutputReservoirs[index] = Upt09PackSpatialResult(result, 1u);
        return;
    }

    if (selectedFromNeighbor != 0u)
    {
        selected.flags =
            (selected.flags & ~(kUpt04CandidateFlagVisibilityKnown
                | kUpt04CandidateFlagVisibilityPassed))
            | kUpt04CandidateFlagVisibilityKnown;
        if (!Upt09VisibilityPassed(
                selectedVisibility, selectedVisibilityIgnore))
        {
            gUpt09OutputReservoirs[index] = Upt09PackSpatialResult(result, 1u);
            return;
        }
        selected.flags |= kUpt04CandidateFlagVisibilityPassed;
    }
    // Empty-center rescue is useful for current-frame coverage, but it is not
    // a statistically independent source reservoir.  Publishing it without a
    // stamp allowed the same borrowed sample to donate again through temporal
    // and spatial reuse, so basic correction preserved and expanded a bright
    // island while 1/M merely turned the same feedback loop into progressive
    // darkness.  Direct candidates have no replay payload, so word4 provides
    // a zero-cost one-frame stamp.  Mode 2 intentionally omits it to reproduce
    // the former recursive behavior.
    if (!centerSelected && selectedFromNeighbor != 0u
        && gUpt09Control.proofMode != 2u)
    {
        selected.replayKey |= kUpt03ReplayKeyRescueBit;
    }
    result.hasSelectedSample = 1u;
    result.weightSum = finalizedMeanWeight;
    result.selected = selected;
    gUpt09OutputReservoirs[index] = Upt09PackSpatialResult(result, 0u);
#endif
#endif
}
