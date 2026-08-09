import upt03_reservoir_contract;
import upt04_emissive_replay;
import upt04_geometry_contract;
import upt04_geometry_bindings;
import upt04_geometry_decode;
import upt04_hit_contract;
import upt04_initial_control;
import upt04_initial_work_contract;
import upt04_light_contract;
import upt04_local_light_evaluation;
import upt04_local_light_proposal;
import upt04_material_contract;
import upt04_material_provider_contract;
import upt04_material_primary_live;
import upt04_path_event_contract;
import upt04_primary_surface_live_contract;
import upt04_random_contract;
import upt04_replay_random;
import upt04_random_white_hash;
import upt04_transport_initial_bounded;
import upt07_emissive_material;
import upt07_indirect_material;
import upt07_temporal_direct_bindings;
import upt07_temporal_reservoir;

public static const uint kUpt07IndirectReplayFlag = 1u << 25u;

public struct Upt07IndirectMaterialSet : IUpt04MaterialSet
{
    static Upt04PreparedDirectMaterial PreparePrimaryDirect(
        Upt04PrimarySurfaceRecord surface)
    {
        return Upt04PreparePrimaryDirectProbe(surface);
    }

    static Upt04PreparedDirectMaterial PrepareSecondaryDirect(
        Upt04GeometryHitSurface surface,
        float3 viewDirection)
    {
        return Upt07PrepareSecondaryDirect(surface, viewDirection);
    }

    static Upt04MaterialEvaluation EvaluatePreparedDirect(
        Upt04PreparedDirectMaterial material,
        float3 lightDirection,
        float3 incidentRadiance)
    {
        return Upt04EvaluatePreparedDirectProbe(
            material, lightDirection, incidentRadiance);
    }

    static Upt04MaterialEvaluation EvaluatePrimaryDirect(
        Upt04PrimarySurfaceRecord surface,
        float3 lightDirection,
        float3 incidentRadiance)
    {
        return Upt04EvaluatePrimaryDirectProbe(
            surface, lightDirection, incidentRadiance);
    }

    static bool PrimaryContinuationAvailable(Upt04PrimarySurfaceRecord surface)
    {
        return Upt04PrimaryContinuationAvailable(surface);
    }

    static Upt04PathEvent SamplePrimaryContinuation(
        Upt04PrimarySurfaceRecord surface,
        float lobeRandom,
        float2 directionRandom)
    {
        return Upt04SamplePrimaryContinuation(
            surface, lobeRandom, directionRandom);
    }

    static Upt04MaterialEmissionEvaluation EvaluateEmissiveEndpoint(
        uint materialIndex,
        uint triangleClassAndFlags,
        float2 uv,
        float2 uvGradient0,
        float2 uvGradient1)
    {
        return UptDirectEvaluateEmissiveEndpoint(
            materialIndex,
            triangleClassAndFlags,
            uv,
            uvGradient0,
            uvGradient1,
            gUpt07Control.materialCount,
            gUpt07Control.logicalTextureCount,
            gUpt07Control.emissiveScale);
    }

    static Upt04MaterialEvaluation EvaluateSecondaryDirect(
        Upt04GeometryHitSurface surface,
        float3 viewDirection,
        float3 lightDirection,
        float3 incidentRadiance)
    {
        return Upt07EvaluateSecondaryDirect(
            surface, viewDirection, lightDirection, incidentRadiance);
    }
};

public bool Upt07IndirectHistoryEligible(Upt07DecodedReservoir reservoir)
{
    if (reservoir.valid == 0u || reservoir.logical.hasSelectedSample == 0u)
        return false;
    const Upt03Candidate candidate = reservoir.logical.selected;
    if ((candidate.flags & (kUpt04CandidateFlagGlobal
            | kUpt04CandidateFlagReplayable
            | kUpt04CandidateFlagNeedsShift))
        != (kUpt04CandidateFlagGlobal
            | kUpt04CandidateFlagReplayable
            | kUpt04CandidateFlagNeedsShift)
        || candidate.replayKey == 0u)
        return false;
    const bool endpoint = candidate.eventKind == 4u
        && candidate.pathLength == 1u
        && candidate.reconnectionVertexLength == 1u;
    const bool secondaryNee = (candidate.eventKind == 1u
            || candidate.eventKind == 2u)
        && candidate.pathLength == 2u
        && candidate.reconnectionVertexLength == 1u;
    return endpoint || secondaryNee;
}

public Upt04HitFacts Upt07TraceIndirect(
    Upt04TraceRequest request,
    uint visibility,
    Upt04VisibilityIgnore ignore)
{
    Upt04HitFacts result = Upt04MissFacts(request.tMax);
    RayDesc ray;
    ray.Origin = request.origin;
    ray.TMin = request.tMin;
    ray.Direction = request.direction;
    ray.TMax = request.tMax;
    RayQuery<0> query = RayQuery<0>();
    const uint flags = visibility != 0u
        ? (RAY_FLAG_FORCE_NON_OPAQUE
            | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH)
        : RAY_FLAG_FORCE_NON_OPAQUE;
    query.TraceRayInline(gUpt07Scene, flags, 0xffu, ray);
    while (query.Proceed())
    {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
            continue;
        if (visibility != 0u && Upt04VisibilityIgnoresCandidate(
                ignore,
                query.CandidateInstanceID(),
                query.CandidatePrimitiveIndex()))
            continue;
        Upt04HitFacts candidate = (Upt04HitFacts)0;
        candidate.status = kUpt04HitTriangle;
        candidate.instanceId = query.CandidateInstanceID();
        candidate.geometryId = query.CandidateGeometryIndex();
        candidate.primitiveId = query.CandidatePrimitiveIndex();
        candidate.barycentrics = query.CandidateTriangleBarycentrics();
        candidate.hitT = query.CandidateTriangleRayT();
        candidate.frontFace = 1u;
        if (Upt07MaterialRejectsAlphaClipCandidate(candidate))
            continue;
        query.CommitNonOpaqueTriangleHit();
    }
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        result.status = kUpt04HitTriangle;
        result.instanceId = query.CommittedInstanceID();
        result.geometryId = query.CommittedGeometryIndex();
        result.primitiveId = query.CommittedPrimitiveIndex();
        result.barycentrics = query.CommittedTriangleBarycentrics();
        result.hitT = query.CommittedRayT();
        result.frontFace = query.CommittedTriangleFrontFace() ? 1u : 0u;
    }
    return result;
}

public Upt04PathEvent Upt07ReplayContinuation(
    Upt03Candidate source,
    Upt04PrimarySurfaceRecord receiver)
{
    return Upt04SamplePrimaryContinuation(
        receiver,
        Upt04WhiteHash(Upt04PersistedReplayRandomKey(
            source.replayKey,
            source.replayIndex,
            0u,
            0u,
            kUpt04StreamMaterialLobeSelection,
            kUpt04DimensionMaterialLobeSelection)),
        float2(
            Upt04WhiteHash(Upt04PersistedReplayRandomKey(
                source.replayKey,
                source.replayIndex,
                0u,
                0u,
                kUpt04StreamIndirectContinuationU,
                kUpt04DimensionIndirectContinuationU)),
            Upt04WhiteHash(Upt04PersistedReplayRandomKey(
                source.replayKey,
                source.replayIndex,
                0u,
                0u,
                kUpt04StreamIndirectContinuationV,
                kUpt04DimensionIndirectContinuationV))));
}

public bool Upt07FindEmissiveDenseLight(
    uint instanceId,
    uint primitiveIndex,
    out uint denseLightIndex,
    out float conditionalIdentityPdf)
{
    denseLightIndex = kUpt04InvalidLightIndex;
    conditionalIdentityPdf = 0.0f;
    uint physicalCount;
    uint stride;
    gUpt04EmissiveLookup.GetDimensions(physicalCount, stride);
    if (!Upt04EmissiveLookupValid(
            gUpt07Control.geometryAvailabilityFlags,
            gUpt07Control.emissiveLookupCapacityAndValid))
        return false;
    const uint capacity = Upt04EmissiveLookupCapacity(
        gUpt07Control.emissiveLookupCapacityAndValid);
    if (capacity < 2u || (capacity & (capacity - 1u)) != 0u
        || capacity > physicalCount || stride != 20u)
        return false;
    const uint mask = capacity - 1u;
    uint hash = instanceId ^ (primitiveIndex + 0x9e3779b9u
        + (instanceId << 6u) + (instanceId >> 2u));
    hash = Upt04HashWord(hash);
    const uint first = hash & mask;
    [unroll]
    for (uint probe = 0u; probe < kUpt04EmissiveLookupMaxProbes; ++probe)
    {
        const Upt04EmissiveLookupEntry entry =
            gUpt04EmissiveLookup[(first + probe) & mask];
        if (entry.occupied == 0u)
            return false;
        if (entry.instanceId == instanceId
            && entry.primitiveIndex == primitiveIndex)
        {
            if (entry.denseLightIndex >= gUpt07Control.currentLightCount)
                return false;
            const Upt04UnifiedLightRecord light =
                Upt07LoadLight(entry.denseLightIndex);
            if (light.type != kUpt04LightEmissiveTriangle
                || light.instanceId != instanceId
                || light.primitiveIndex != primitiveIndex)
                return false;
            denseLightIndex = entry.denseLightIndex;
            conditionalIdentityPdf = max(entry.conditionalIdentityPdf, 0.0f);
            return true;
        }
    }
    return false;
}

public float Upt07ExactNeePdfForEmissiveHit(
    uint instanceId,
    uint primitiveIndex,
    float3 receiverPosition,
    float2 hitBarycentrics,
    uint proposalParity)
{
    uint denseLightIndex;
    float conditionalIdentityPdf;
    if (!Upt07FindEmissiveDenseLight(
            instanceId, primitiveIndex,
            denseLightIndex, conditionalIdentityPdf))
        return 0.0f;
    Upt04LocalLightRanges ranges;
    ranges.emissiveStart = gUpt07Control.emissiveRangeStart;
    ranges.emissiveCount = gUpt07Control.emissiveRangeCount;
    ranges.analyticStart = gUpt07Control.analyticRangeStart;
    ranges.analyticCount = gUpt07Control.analyticRangeCount;
    if (!Upt04LocalLightRangesStructurallyValid(
            ranges, gUpt07Control.currentLightCount))
        return 0.0f;
    const uint parity = proposalParity != 0u ? 1u : 0u;
    const uint configured = Upt04ConfiguredEmissiveTrialCount(
        gUpt07Control.geometryAvailabilityFlags);
    const uint emissiveTrials = Upt04InverseSquareNeeTrialCount(
        Upt04DirectRisDomainTrialCount(
            ranges, kUpt04LightEmissiveTriangle, parity, configured),
        kUpt04SecondaryNeeBounceIndex);
    const uint analyticTrials = Upt04InverseSquareNeeTrialCount(
        Upt04DirectRisDomainTrialCount(
            ranges, kUpt04LightDoomAnalytic, parity, configured),
        kUpt04SecondaryNeeBounceIndex);
    const uint candidateCount = emissiveTrials + analyticTrials;
    float selectionPdf = 0.0f;
    if (parity != 0u && Upt04EmissiveDistributionValid(
            gUpt07Control.emissiveDistributionCountAndValid))
        selectionPdf = candidateCount != 0u
            ? conditionalIdentityPdf * float(emissiveTrials)
                / float(candidateCount)
            : 0.0f;
    else
        selectionPdf = ranges.emissiveCount != 0u && candidateCount != 0u
            ? float(emissiveTrials)
                / float(candidateCount * ranges.emissiveCount)
            : 0.0f;
    if (!Upt04FinitePositive(selectionPdf))
        return 0.0f;
    const Upt04EmissiveGeometrySample replay =
        Upt04ReplayEmissiveTriangleBarycentrics(
            Upt07LoadLight(denseLightIndex),
            receiverPosition,
            hitBarycentrics,
            (gUpt07Control.geometryAvailabilityFlags
                & kUpt04EmissiveReplayTwoSided) != 0u);
    if (replay.status == kUpt04SampleInvalid
        || !Upt04FinitePositive(replay.solidAnglePdf))
        return 0.0f;
    const float4 weights = UptDirectEmissiveRegionWeights(
        replay,
        gUpt07Control.materialCount,
        gUpt07Control.logicalTextureCount,
        gUpt07Control.emissiveScale);
    const uint region = Upt04EmissiveRegionFromBarycentrics(
        replay.barycentrics);
    const float solidAnglePdf = replay.solidAnglePdf
        * (4.0f * Upt04EmissiveRegionSelectionPdf(weights, region));
    const float neePdf = float(candidateCount)
        * selectionPdf * solidAnglePdf;
    return Upt04FinitePositive(neePdf) ? neePdf : 0.0f;
}

public uint Upt07ResolveStoredSecondaryLight(
    Upt03Candidate source,
    uint sourceIsHistory,
    out uint denseIndex,
    out uint lightType,
    out float selectionPdf)
{
    denseIndex = kUpt04InvalidLightIndex;
    lightType = kUpt04LightInvalid;
    selectionPdf = 0.0f;
    if ((source.eventKind != 1u && source.eventKind != 2u)
        || source.identity0 == 0u)
        return 0u;
    denseIndex = source.identity0 - 1u;
    if (sourceIsHistory != 0u)
    {
        if (denseIndex >= gUpt07Control.previousToCurrentLightCount)
            return 0u;
        denseIndex = gUpt07PreviousToCurrentLights[denseIndex];
        if (denseIndex == 0xffffffffu)
            return 0u;
    }
    if (denseIndex >= gUpt07Control.currentLightCount)
        return 0u;
    const Upt04UnifiedLightRecord light = Upt07LoadLight(denseIndex);
    lightType = source.eventKind == 2u
        ? kUpt04LightEmissiveTriangle : kUpt04LightDoomAnalytic;
    const bool inExpectedRange = lightType == kUpt04LightEmissiveTriangle
        ? denseIndex >= gUpt07Control.emissiveRangeStart
            && denseIndex - gUpt07Control.emissiveRangeStart
                < gUpt07Control.emissiveRangeCount
        : denseIndex >= gUpt07Control.analyticRangeStart
            && denseIndex - gUpt07Control.analyticRangeStart
                < gUpt07Control.analyticRangeCount;
    const bool dynamicEmissive = light.type == kUpt04LightEmissiveTriangle
        && (light.flags & kUpt04EmissiveHistoryDynamic) != 0u;
    if (!inExpectedRange || dynamicEmissive || light.type != lightType
        || source.identity1 != Upt04StableLightIdentityFingerprint(light))
        return 0u;

    Upt04LocalLightRanges ranges;
    ranges.emissiveStart = gUpt07Control.emissiveRangeStart;
    ranges.emissiveCount = gUpt07Control.emissiveRangeCount;
    ranges.analyticStart = gUpt07Control.analyticRangeStart;
    ranges.analyticCount = gUpt07Control.analyticRangeCount;
    if (!Upt04LocalLightRangesStructurallyValid(
            ranges, gUpt07Control.currentLightCount))
        return 0u;
    const uint parity =
        (source.flags & kUpt04CandidateFlagDirectProposalParity) != 0u
            ? 1u : 0u;
    const uint configured = Upt04ConfiguredEmissiveTrialCount(
        gUpt07Control.geometryAvailabilityFlags);
    const uint emissiveTrials = Upt04InverseSquareNeeTrialCount(
        Upt04DirectRisDomainTrialCount(
            ranges, kUpt04LightEmissiveTriangle, parity, configured),
        kUpt04SecondaryNeeBounceIndex);
    const uint analyticTrials = Upt04InverseSquareNeeTrialCount(
        Upt04DirectRisDomainTrialCount(
            ranges, kUpt04LightDoomAnalytic, parity, configured),
        kUpt04SecondaryNeeBounceIndex);
    const uint candidateCount = emissiveTrials + analyticTrials;
    if (candidateCount == 0u)
        return 0u;

    if (lightType == kUpt04LightEmissiveTriangle
        && parity != 0u
        && Upt04EmissiveDistributionValid(
            gUpt07Control.emissiveDistributionCountAndValid))
    {
        uint physicalDistributionCount;
        uint distributionStride;
        gUpt07EmissiveDistribution.GetDimensions(
            physicalDistributionCount, distributionStride);
        const uint logicalDistributionCount = Upt04EmissiveDistributionCount(
            gUpt07Control.emissiveDistributionCountAndValid);
        if (logicalDistributionCount == 0u
            || logicalDistributionCount > physicalDistributionCount
            || distributionStride != 16u)
            return 0u;
        uint lookupDenseIndex;
        float conditionalIdentityPdf;
        if (!Upt07FindEmissiveDenseLight(
                light.instanceId,
                light.primitiveIndex,
                lookupDenseIndex,
                conditionalIdentityPdf)
            || lookupDenseIndex != denseIndex)
            return 0u;
        selectionPdf = conditionalIdentityPdf
            * float(emissiveTrials) / float(candidateCount);
    }
    else
    {
        const uint domainCount = lightType == kUpt04LightEmissiveTriangle
            ? ranges.emissiveCount : ranges.analyticCount;
        const uint domainTrials = lightType == kUpt04LightEmissiveTriangle
            ? emissiveTrials : analyticTrials;
        selectionPdf = domainCount != 0u
            ? float(domainTrials) / float(candidateCount * domainCount)
            : 0.0f;
    }
    return Upt04FinitePositive(selectionPdf) ? 1u : 0u;
}

public uint Upt07ReplayStoredSecondaryLight(
    Upt03Candidate source,
    uint lightType,
    Upt04UnifiedLightRecord light,
    float3 receiverPosition,
    out Upt04LocalLightSample sample)
{
    sample = (Upt04LocalLightSample)0;
    if (lightType == kUpt04LightEmissiveTriangle)
    {
        const Upt04EmissiveGeometrySample replay =
            Upt04ReplayEmissiveTriangleBarycentrics(
                light,
                receiverPosition,
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
        sample.status = replay.status == kUpt04SampleValidPositive
                && any(emission.radiance > 0.0f)
            ? kUpt04SampleValidPositive : kUpt04SampleValidZero;
        sample.position = replay.position;
        sample.normal = replay.normal;
        sample.distance = replay.distance;
        sample.direction = replay.direction;
        sample.solidAnglePdf = replay.solidAnglePdf
            * (4.0f * Upt04EmissiveRegionSelectionPdf(
                regionWeights, region));
        sample.radiance = emission.radiance;
        return 1u;
    }
    if (!Upt04AnalyticLightCurrentPortalEligible(
            light, gUpt07Control.geometryAvailabilityFlags))
        return 0u;
    sample = Upt04SampleAnalyticSphere(
        light,
        receiverPosition,
        float2(source.sampleCoord0, source.sampleCoord1));
    return sample.status != kUpt04SampleInvalid ? 1u : 0u;
}

public uint Upt07ReplayIndirectSample(
    Upt03Candidate source,
    Upt04PrimarySurfaceRecord receiver,
    uint sourceIsHistory,
    out uint failureReason,
    out Upt03Candidate shifted)
{
    failureReason = kUpt07RejectIndirectStructure;
    shifted = (Upt03Candidate)0;
    if ((gUpt07Control.geometryAvailabilityFlags
            & kUpt07IndirectReplayFlag) == 0u
        || source.replayKey == 0u
        || !Upt04PrimarySurfaceRecordValid(receiver))
        return 0u;
    const bool endpoint = source.eventKind == 4u
        && source.pathLength == 1u
        && source.reconnectionVertexLength == 1u;
    const bool secondaryNee = (source.eventKind == 1u
            || source.eventKind == 2u)
        && source.pathLength == 2u
        && source.reconnectionVertexLength == 1u;
    if (!endpoint && !secondaryNee)
        return 0u;
    uint storedDenseIndex = kUpt04InvalidLightIndex;
    uint storedLightType = kUpt04LightInvalid;
    float storedSelectionPdf = 0.0f;
    if (secondaryNee && Upt07ResolveStoredSecondaryLight(
            source,
            sourceIsHistory,
            storedDenseIndex,
            storedLightType,
            storedSelectionPdf) == 0u)
    {
        failureReason = kUpt07RejectIndirectIdentity;
        return 0u;
    }

    failureReason = kUpt07RejectIndirectContinuation;
    const Upt04PathEvent continuation = Upt07ReplayContinuation(
        source, receiver);
    if (continuation.valid == 0u
        || !Upt04FinitePositive(continuation.pdf))
        return 0u;
    Upt04TraceRequest continuationRay;
    if (!Upt04BuildContinuationRequest(
            receiver.worldPositionAndViewDepth.xyz,
            receiver.geometricNormalAndRoughness.xyz,
            receiver.shadingNormalAndOpacity.xyz,
            continuation.direction,
            continuationRay))
        return 0u;
    failureReason = kUpt07RejectIndirectGeometry;
    const Upt04HitFacts hit = Upt07TraceIndirect(
        continuationRay, 0u, Upt04NoVisibilityIgnore());
    if (hit.status != kUpt04HitTriangle || hit.instanceId == 0xffffffffu)
        return 0u;
    const Upt04GeometryHitSurface hitSurface = Upt04DecodeGeometryHit(hit);
    if (hitSurface.valid == 0u)
        return 0u;

    shifted.status = kUpt04SampleValidPositive;
    shifted.eventKind = source.eventKind;
    shifted.lobeMask = continuation.lobeMask;
    shifted.flags = kUpt04CandidateFlagGlobal
        | kUpt04CandidateFlagReplayable
        | kUpt04CandidateFlagNeedsShift
        | kUpt04CandidateFlagVisibilityKnown
        | kUpt04CandidateFlagVisibilityPassed
        | (source.flags & kUpt04CandidateFlagDirectProposalParity);
    shifted.replayKey = source.replayKey & 0x7fffffffu;
    shifted.replayIndex = source.replayIndex;
    shifted.generationFingerprint = kUpt03ExpectedGeneration;
    shifted.proposalPdf = source.proposalPdf;
    shifted.partialJacobian = 1.0f;
    shifted.accumulatedRouletteProbability = 1.0f;

    if (endpoint)
    {
        failureReason = kUpt07RejectIndirectEndpoint;
        const Upt04MaterialEmissionEvaluation emission =
            Upt07EvaluateHitEmission(hitSurface);
        if (emission.valid == 0u)
            return 0u;
        float3 contribution = continuation.throughput * emission.radiance;
        const float neePdf = any(emission.radiance > 0.0f)
            ? Upt07ExactNeePdfForEmissiveHit(
                hit.instanceId,
                hit.primitiveId,
                receiver.worldPositionAndViewDepth.xyz,
                hit.barycentrics,
                (source.flags & kUpt04CandidateFlagDirectProposalParity) != 0u
                    ? 1u : 0u)
            : 0.0f;
        contribution *= Upt04BalanceHeuristic(continuation.pdf, neePdf);
        if (!all(isfinite(contribution)))
            return 0u;
        shifted.identity0 = hit.instanceId + 1u;
        shifted.identity1 = hit.primitiveId + 1u;
        shifted.sampleCoord0 = hit.barycentrics.x;
        shifted.sampleCoord1 = hit.barycentrics.y;
        shifted.pathLength = 1u;
        shifted.reconnectionVertexLength = 1u;
        shifted.pathPdf = continuation.pdf;
        shifted.contributionR = contribution.x;
        shifted.contributionG = contribution.y;
        shifted.contributionB = contribution.z;
        shifted.target = max(contribution.x,
            max(contribution.y, contribution.z));
        if (!Upt04FinitePositive(shifted.target))
        {
            shifted.status = kUpt04SampleValidZero;
            shifted.contributionR = 0.0f;
            shifted.contributionG = 0.0f;
            shifted.contributionB = 0.0f;
            shifted.target = 0.0f;
        }
        failureReason = 0u;
        return 1u;
    }

    // Section 6.2.3 forced NEE reconnection: random replay has already
    // reconstructed the secondary vertex. Keep the stored selected light
    // vertex instead of replaying every inner-RIS candidate and sampling the
    // emissive CDF again. D0 persists the algebraically equivalent raw-target
    // representation, so current-domain target evaluation remains exact.
    failureReason = kUpt07RejectIndirectInnerRis;
    const Upt04UnifiedLightRecord storedLight =
        Upt07LoadLight(storedDenseIndex);
    Upt04LocalLightSample selectedSample;
    if (Upt07ReplayStoredSecondaryLight(
            source,
            storedLightType,
            storedLight,
            hitSurface.position,
            selectedSample) == 0u
        || selectedSample.status != kUpt04SampleValidPositive)
        return 0u;
    const Upt04MaterialEvaluation selectedMaterial =
        Upt07EvaluateSecondaryDirect(
            hitSurface,
            -continuation.direction,
            selectedSample.direction,
            selectedSample.radiance);
    const float sourcePdf = storedSelectionPdf
        * selectedSample.solidAnglePdf;
    const float pathPdf = continuation.pdf * sourcePdf;
    const float3 contribution = continuation.throughput
        * selectedMaterial.weightedContribution;
    const float target = max(contribution.x,
        max(contribution.y, contribution.z));
    if (selectedMaterial.valid == 0u
        || !Upt04FinitePositive(sourcePdf)
        || !Upt04FinitePositive(pathPdf)
        || !all(isfinite(contribution))
        || !Upt04FinitePositive(target))
        return 0u;

    failureReason = kUpt07RejectIndirectVisibility;
    Upt04TraceRequest visibility;
    if (!Upt04BuildPrimaryVisibilityRequest(
            hitSurface.position,
            hitSurface.geometricNormal,
            hitSurface.shadingNormal,
            selectedSample.direction,
            selectedSample.distance,
            visibility))
        return 0u;
    Upt04VisibilityIgnore ignore = Upt04NoVisibilityIgnore();
    if (storedLightType == kUpt04LightEmissiveTriangle)
        ignore = Upt04MakeVisibilityIgnore(
            storedLight.instanceId, storedLight.primitiveIndex);
    const uint storedFingerprint =
        Upt04StableLightIdentityFingerprint(storedLight);
    const Upt04HitFacts occluder = Upt07TraceIndirect(
        visibility, 1u, ignore);
    shifted.eventKind = storedLightType == kUpt04LightEmissiveTriangle
        ? 2u : 1u;
    shifted.lobeMask = continuation.lobeMask | selectedMaterial.lobeMask;
    shifted.identity0 = storedDenseIndex + 1u;
    shifted.identity1 = storedFingerprint;
    shifted.sampleCoord0 = source.sampleCoord0;
    shifted.sampleCoord1 = source.sampleCoord1;
    shifted.pathLength = 2u;
    shifted.reconnectionVertexLength = 1u;
    shifted.pathPdf = pathPdf;
    if (occluder.status == kUpt04HitTriangle)
    {
        shifted.status = kUpt04SampleValidZero;
        shifted.contributionR = 0.0f;
        shifted.contributionG = 0.0f;
        shifted.contributionB = 0.0f;
        shifted.target = 0.0f;
    }
    else
    {
        shifted.contributionR = contribution.x;
        shifted.contributionG = contribution.y;
        shifted.contributionB = contribution.z;
        shifted.target = target;
    }
    failureReason = 0u;
    return 1u;
}
