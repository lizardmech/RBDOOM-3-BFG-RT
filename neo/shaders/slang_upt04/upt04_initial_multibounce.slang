import upt04_geometry_contract;
import upt04_geometry_decode;
import upt04_hit_contract;
import upt04_initial_bindings;
import upt04_initial_control;
import upt04_initial_indirect;
import upt04_initial_light_selection;
import upt04_initial_work_contract;
import upt04_light_contract;
import upt04_local_light_evaluation;
import upt04_local_light_proposal;
import upt04_material_contract;
import upt04_material_provider_contract;
import upt04_path_event_contract;
import upt04_random_contract;
import upt04_random_white_hash;
import upt04_replay_random;
import upt04_transport_initial_bounded;
import upt04_emissive_lookup;
import upt05_material_openpbr;

#ifndef UPT04_STATIC_ANALYTIC_INDIRECT_ONLY
#define UPT04_STATIC_ANALYTIC_INDIRECT_ONLY 0
#endif
#ifndef UPT04_EMISSIVE_COMPACT_CLASSIFY
#define UPT04_EMISSIVE_COMPACT_CLASSIFY 0
#endif
import upt04_emissive_compact;
import upt04_material_emission;

public struct Upt04SecondContinuationResult
{
    public Upt04InitialTraceWork work;
    public float localDirectionalPdf;
};

public struct Upt04SecondHitTrialResult
{
    public Upt04InitialTraceWork endpointCandidate;
    public Upt04InitialTraceWork neeWork;
    public Upt04GeometryHitSurface endpointSurface;
};

public static const uint kUpt04ThirdNeeBounceIndex = 3u;

public Upt04PathEvent Upt04SampleThreeVertexPreparedContinuation(
    Upt04PreparedDirectMaterial prepared,
    float lobeRandom,
    float2 directionRandom)
{
    Upt04PathEvent result = (Upt04PathEvent)0;
    if (prepared.valid == 0u)
        return result;
    Upt05OpenPbrParameters parameters;
    parameters.baseColor = prepared.baseColor;
    parameters.roughness = prepared.roughness;
    parameters.specularF0 = prepared.specularF0;
    const Upt05OpenPbrSample sampled = Upt05SampleOpenPbr(
        parameters,
        prepared.normal,
        prepared.viewDirection,
        lobeRandom,
        directionRandom);
    if (sampled.valid == 0u || !Upt04FinitePositive(sampled.directionalPdf))
        return result;
    result.direction = sampled.direction;
    result.pdf = sampled.directionalPdf;
    result.throughput = sampled.throughput;
    result.lobeMask = sampled.lobeMask;
    result.valid = all(isfinite(result.throughput)) ? 1u : 0u;
    return result;
}

public bool Upt04AdmitSecondIndirectContinuation(
    inout Upt04BoundedTransportState state)
{
    if (state.valid == 0u
        || state.stage != kUpt04TransportIndirectContinuation
        || state.budget.continuationRays != 1u
        || state.budget.visibilityRays > 2u)
        return false;
    state.budget.continuationRays = 2u;
    return true;
}

public bool Upt04RayBudgetWithinThreeVertexCeiling(
    Upt04BoundedTransportState state)
{
    return state.valid != 0u
        && state.budget.continuationRays <= 2u
        && state.budget.visibilityRays <= 3u
        && state.budget.continuationRays + state.budget.visibilityRays <= 5u;
}

public bool Upt04AdmitThirdVertexNeeVisibility(
    inout Upt04BoundedTransportState state)
{
    if (state.valid == 0u
        || state.stage != kUpt04TransportIndirectContinuation
        || state.budget.continuationRays != 2u
        || state.budget.visibilityRays > 2u)
        return false;
    state.budget.visibilityRays += 1u;
    state.stage = kUpt04TransportIndirectNeeVisibility;
    return true;
}

public Upt04SecondContinuationResult Upt04PrepareSecondContinuation<
    TMaterialSet : IUpt04MaterialSet>(
    Upt04InitialTraceWork firstContinuation,
    Upt04GeometryHitSurface secondarySurface)
{
    Upt04SecondContinuationResult result = (Upt04SecondContinuationResult)0;
    if (firstContinuation.valid == 0u
        || secondarySurface.valid == 0u
        || firstContinuation.transport.stage
            != kUpt04TransportIndirectContinuation)
        return result;

    const Upt04PreparedDirectMaterial prepared =
        TMaterialSet.PrepareSecondaryDirect(
            secondarySurface, -firstContinuation.ray.direction);
    if (prepared.valid == 0u)
        return result;
    const Upt04PathEvent continuation =
        Upt04SampleThreeVertexPreparedContinuation(
            prepared,
            Upt04WhiteHash(Upt04PersistedReplayRandomKey(
                firstContinuation.replayKey,
                firstContinuation.replayIndex,
                1u,
                0u,
                kUpt04StreamMaterialLobeSelection,
                kUpt04DimensionMaterialLobeSelection)),
            float2(
                Upt04WhiteHash(Upt04PersistedReplayRandomKey(
                    firstContinuation.replayKey,
                    firstContinuation.replayIndex,
                    1u,
                    0u,
                    kUpt04StreamIndirectContinuationU,
                    kUpt04DimensionIndirectContinuationU)),
                Upt04WhiteHash(Upt04PersistedReplayRandomKey(
                    firstContinuation.replayKey,
                    firstContinuation.replayIndex,
                    1u,
                    0u,
                    kUpt04StreamIndirectContinuationV,
                    kUpt04DimensionIndirectContinuationV))));
    if (continuation.valid == 0u
        || !Upt04FinitePositive(continuation.pdf)
        || !all(isfinite(continuation.throughput)))
        return result;

    // RTXDI FullSample orders this biased cutoff after the current BSDF
    // throughput multiplication but before roulette and ray emission. It is
    // intentionally initial-only; reuse must replay the stored surviving path
    // without applying this state-dependent kill again.
    const float3 uncompensatedPathThroughput =
        firstContinuation.pathThroughput * continuation.throughput;
    const float minimumPathThroughput = clamp(
        asfloat(gUpt04Control.reservedControl1), 0.0f, 1.0f);
    if (!all(isfinite(uncompensatedPathThroughput))
        || (minimumPathThroughput > 0.0f
            && dot(uncompensatedPathThroughput,
                    uncompensatedPathThroughput)
                < minimumPathThroughput * minimumPathThroughput))
        return result;

    const float q = clamp(
        asfloat(gUpt04Control.reservedControl0), 0.0f, 1.0f);
    const float rouletteRandom = Upt04WhiteHash(
        Upt04PersistedReplayRandomKey(
            firstContinuation.replayKey,
            firstContinuation.replayIndex,
            1u,
            0u,
            kUpt04StreamIndirectRouletteSurvival,
            kUpt04DimensionIndirectRouletteSurvival));
    // Roulette termination is not a reservoir candidate and emits no ray.
    if (!Upt04FinitePositive(q) || rouletteRandom >= q)
        return result;

    Upt04InitialTraceWork work = firstContinuation;
    work.lobeMask |= continuation.lobeMask;
    work.pathPdf *= continuation.pdf * q;
    work.pathThroughput = uncompensatedPathThroughput / q;
    work.accumulatedRouletteProbability *= q;
    if (!Upt04FinitePositive(work.pathPdf)
        || !Upt04FinitePositive(work.accumulatedRouletteProbability)
        || !all(isfinite(work.pathThroughput)))
        return result;
    if (!Upt04AdmitSecondIndirectContinuation(work.transport)
        || !Upt04RayBudgetWithinThreeVertexCeiling(work.transport))
        return result;
    if (!Upt04BuildContinuationRequest(
            secondarySurface.position,
            secondarySurface.geometricNormal,
            secondarySurface.shadingNormal,
            continuation.direction,
            work.ray))
        return result;
    work.traceRequired = 1u;
    work.candidateReady = 0u;
    work.valid = 1u;
    result.work = work;
    result.localDirectionalPdf = continuation.pdf;
    return result;
}

public Upt04InitialTraceWork Upt04EvaluateSecondEndpoint<
    TMaterialSet : IUpt04MaterialSet>(
    Upt04SecondContinuationResult continuation,
    Upt04GeometryHitSurface secondarySurface,
    Upt04HitFacts hit)
{
    Upt04InitialTraceWork endpoint = continuation.work;
    endpoint.traceRequired = 0u;
    endpoint.candidateReady = 0u;
    if (endpoint.valid == 0u
        || !Upt04FinitePositive(continuation.localDirectionalPdf))
        return Upt04InvalidateInitialWork(endpoint);

    endpoint.eventKind = 4u;
    endpoint.pathLength = 2u;
    endpoint.reconnectionVertexLength = 1u;
    endpoint.proposalPdf = endpoint.accumulatedRouletteProbability;
    if (!Upt04FinitePositive(endpoint.proposalPdf))
        return Upt04InvalidateInitialWork(endpoint);

    if (hit.status == kUpt04HitMiss)
    {
        endpoint.identity0 = endpoint.surfaceIndex + 1u;
        endpoint.identity1 = 0u;
        endpoint.sampleCoordinates = float2(0.0f);
        endpoint.contribution = float3(0.0f);
        endpoint.target = 0.0f;
        endpoint.candidateStatus = kUpt04SampleValidZero;
        endpoint.candidateReady = 1u;
        return endpoint;
    }
    if (hit.status != kUpt04HitTriangle || hit.instanceId == 0xffffffffu)
        return Upt04InvalidateInitialWork(endpoint);

    const Upt04GeometryHitSurface hitSurface = Upt04DecodeGeometryHit(hit);
    if (hitSurface.valid == 0u)
        return Upt04InvalidateInitialWork(endpoint);
    Upt04MaterialEmissionEvaluation emission =
        (Upt04MaterialEmissionEvaluation)0;
#if UPT04_STATIC_ANALYTIC_INDIRECT_ONLY
    emission.valid = 1u;
#elif UPT04_EMISSIVE_COMPACT_CLASSIFY
    emission.valid = 1u;
    emission.radiance = Upt56ClassifyHitRadiance(
        hit.instanceId,
        hit.primitiveId,
        hit.barycentrics,
        hitSurface.materialIndex,
        endpoint.ray.direction,
        false);
#else
    emission = TMaterialSet.EvaluateEmissiveEndpoint(
        hitSurface.materialIndex,
        hitSurface.triangleClassAndFlags,
        hitSurface.texCoord,
        float2(0.0f),
        float2(0.0f));
#endif
    if (emission.valid == 0u)
        return Upt04InvalidateInitialWork(endpoint);

    float3 contribution = endpoint.pathThroughput * emission.radiance;
    uint reverseStatus = 0u;
#if UPT04_STATIC_ANALYTIC_INDIRECT_ONLY || UPT04_EMISSIVE_COMPACT_CLASSIFY
    const float neePdf = 0.0f;
#else
    const float neePdf = any(emission.radiance > 0.0f)
        ? Upt04ExactNeePdfForEmissiveHit<TMaterialSet>(
            hit.instanceId,
            hit.primitiveId,
            secondarySurface.position,
            hit.barycentrics,
            kUpt04ThirdNeeBounceIndex,
            reverseStatus)
        : 0.0f;
#endif
    contribution *= Upt04BalanceHeuristic(
        continuation.localDirectionalPdf, neePdf);
    const float rawTarget = max(contribution.x,
        max(contribution.y, contribution.z));
    const float target = rawTarget * endpoint.accumulatedRouletteProbability;
    if (!all(isfinite(contribution)) || !isfinite(target) || target < 0.0f)
        return Upt04InvalidateInitialWork(endpoint);

    endpoint.candidateFlags |= kUpt04CandidateFlagVisibilityKnown
        | kUpt04CandidateFlagVisibilityPassed;
    endpoint.identity0 = hit.instanceId + 1u;
    endpoint.identity1 = hit.primitiveId + 1u;
    endpoint.sampleCoordinates = hit.barycentrics;
    endpoint.contribution = contribution;
    endpoint.target = target;
    endpoint.candidateStatus = target > 0.0f
        ? kUpt04SampleValidPositive : kUpt04SampleValidZero;
    endpoint.candidateReady = 1u;
    return endpoint;
}

public Upt04InitialTraceWork Upt04PrepareThirdVertexNee<
    TMaterialSet : IUpt04MaterialSet>(
    Upt04SecondContinuationResult continuation,
    Upt04GeometryHitSurface endpointSurface,
    uint2 pixel)
{
    Upt04InitialTraceWork invalid = continuation.work;
    invalid.traceRequired = 0u;
    invalid.candidateReady = 0u;
    if (invalid.valid == 0u || endpointSurface.valid == 0u)
        return Upt04InvalidateInitialWork(invalid);

    Upt04LocalLightRanges ranges;
    ranges.emissiveStart = gUpt04Control.emissiveRangeStart;
#if UPT04_STATIC_ANALYTIC_INDIRECT_ONLY
    ranges.emissiveCount = 0u;
#else
    ranges.emissiveCount = gUpt04Control.emissiveRangeCount;
#endif
    ranges.analyticStart = gUpt04Control.analyticRangeStart;
    ranges.analyticCount = gUpt04Control.analyticRangeCount;
    const uint threeVertexProbe = Upt04ThreeVertexBottleneckProbe(
        gUpt04Control.indirectPolicyFlags);
    if (threeVertexProbe == 5u || threeVertexProbe == 6u)
        ranges.emissiveCount = 0u;
    else if (threeVertexProbe == 7u || threeVertexProbe == 8u)
        ranges.analyticCount = 0u;
    if (!Upt04LocalLightRangesStructurallyValid(
            ranges, gUpt04Control.currentLightCount)
        || Upt04LocalLightRangeCount(ranges) == 0u)
        return Upt04InvalidateInitialWork(invalid);

    Upt04PreparedDirectMaterial preparedMaterial;
    if (threeVertexProbe >= 9u && threeVertexProbe <= 15u)
    {
        preparedMaterial = (Upt04PreparedDirectMaterial)0;
        preparedMaterial.normal = normalize(
            dot(endpointSurface.shadingNormal, endpointSurface.shadingNormal)
                    > 1.0e-8f
                ? endpointSurface.shadingNormal
                : endpointSurface.geometricNormal);
        preparedMaterial.viewDirection = normalize(
            -continuation.work.ray.direction);
        preparedMaterial.baseColor = float3(0.5f);
        preparedMaterial.roughness = 1.0f;
        preparedMaterial.specularF0 = float3(0.0f);
        preparedMaterial.valid = all(isfinite(preparedMaterial.normal))
                && all(isfinite(preparedMaterial.viewDirection))
            ? 1u : 0u;
    }
    else
    {
        preparedMaterial = TMaterialSet.PrepareSecondaryDirect(
            endpointSurface, -continuation.work.ray.direction);
    }
    if (preparedMaterial.valid == 0u)
        return Upt04InvalidateInitialWork(invalid);

    const Upt04InitialLightSchedule schedule =
        Upt04MakeBounceLightSchedule(ranges, kUpt04ThirdNeeBounceIndex);
    const uint candidateCount = threeVertexProbe == 14u
            || threeVertexProbe == 15u
        ? 1u : schedule.candidateCount;
    Upt04SecondaryNeeProposal selected = (Upt04SecondaryNeeProposal)0;
    float weightSum = 0.0f;
    if (threeVertexProbe == 14u)
    {
        const bool useEmissive = ranges.emissiveCount != 0u;
        selected.valid = 1u;
        selected.lightIndex = useEmissive
            ? ranges.emissiveStart : ranges.analyticStart;
        selected.lightType = useEmissive
            ? kUpt04LightEmissiveTriangle : kUpt04LightDoomAnalytic;
        selected.sampleCoordinates = float2(0.5f);
        selected.rawContribution = float3(1.0f);
        selected.suffixRadiance = float3(1.0f);
        selected.target = 1.0f;
        selected.sourcePdf = 1.0f;
        selected.emissiveReplayPdfScale = 1.0f;
        selected.sampleDirection = preparedMaterial.normal;
        selected.sampleDistance = 1.0f;
        weightSum = 1.0f;
    }
    [loop]
    for (uint candidateOrdinal = 0u;
         threeVertexProbe != 14u && candidateOrdinal < candidateCount;
         ++candidateOrdinal)
    {
        Upt04SecondaryNeeProposal proposal = (Upt04SecondaryNeeProposal)0;
        if ((threeVertexProbe >= 11u && threeVertexProbe <= 13u)
            || threeVertexProbe == 15u)
        {
            const Upt04LocalLightSelection selection =
                Upt04SelectInitialLightFromReplay(
                    schedule,
                    pixel,
                    continuation.work.replayKey,
                    continuation.work.replayIndex,
                    2u,
                    candidateOrdinal);
            if (selection.valid != 0u
                && selection.denseIndex < gUpt04Control.currentLightCount)
            {
                proposal.valid = 1u;
                proposal.lightIndex = selection.denseIndex;
                proposal.lightType = selection.type;
                proposal.sampleCoordinates = selection.sampleCoordinates;
                proposal.sourcePdf = max(selection.selectionPdf, 1.0e-6f);
                proposal.target = 1.0f
                    + float(selection.denseIndex & 7u) * (1.0f / 16.0f);
                proposal.rawContribution = float3(proposal.target);
                proposal.suffixRadiance = proposal.rawContribution;
                proposal.emissiveReplayPdfScale = 1.0f;
                proposal.sampleDirection = preparedMaterial.normal;
                proposal.sampleDistance = 1.0f;

                if (threeVertexProbe == 12u || threeVertexProbe == 13u)
                {
                    const Upt04UnifiedLightRecord light =
                        Upt04LoadCurrentLightEvaluationRecord(
                            selection.denseIndex, selection.type);
                    const float recordWitness = 1.0f
                        + frac(abs(light.positionAndRadius.x)) * 0.25f;
                    proposal.target *= recordWitness;
                    proposal.rawContribution = float3(proposal.target);
                    proposal.suffixRadiance = proposal.rawContribution;

                    if (threeVertexProbe == 13u)
                    {
                        const Upt04EvaluatedLocalLight evaluated =
                            Upt04EvaluateSelectedLocalLight<TMaterialSet>(
                                selection,
                                light,
                                endpointSurface.position,
                                gUpt04Control.availabilityFlags,
                                gUpt04Control.emissiveScale);
                        if (evaluated.valid == 0u
                            || evaluated.sample.status
                                != kUpt04SampleValidPositive)
                        {
                            proposal = (Upt04SecondaryNeeProposal)0;
                        }
                        else
                        {
                            proposal.sampleCoordinates =
                                evaluated.storedSampleCoordinates;
                            proposal.sourcePdf = selection.selectionPdf
                                * evaluated.sample.solidAnglePdf;
                            proposal.target = max(
                                max(evaluated.sample.radiance.x,
                                    evaluated.sample.radiance.y),
                                evaluated.sample.radiance.z);
                            proposal.rawContribution = float3(proposal.target);
                            proposal.suffixRadiance = proposal.rawContribution;
                            proposal.emissiveReplayPdfScale =
                                selection.type == kUpt04LightEmissiveTriangle
                                ? asfloat(evaluated.sample.reserved) : 1.0f;
                            proposal.sampleDirection =
                                evaluated.sample.direction;
                            proposal.sampleDistance =
                                evaluated.sample.distance;
                            proposal.valid = Upt04FinitePositive(
                                    proposal.sourcePdf)
                                && Upt04FinitePositive(proposal.target)
                                && Upt04FinitePositive(
                                    proposal.emissiveReplayPdfScale)
                                ? 1u : 0u;
                        }
                    }
                }
            }
        }
        else
        {
            proposal = Upt04EvaluateSecondaryNeeProposal<TMaterialSet>(
                continuation.work,
                endpointSurface,
                preparedMaterial,
                schedule,
                pixel,
                2u,
                candidateOrdinal);
        }
        if (proposal.valid == 0u)
            continue;
        const float candidateWeight = proposal.target / proposal.sourcePdf;
        const float newWeightSum = weightSum + candidateWeight;
        if (!Upt04FinitePositive(candidateWeight)
            || !Upt04FinitePositive(newWeightSum))
            continue;
        const float selectionRandom = Upt04WhiteHash(
            Upt04PersistedReplayRandomKey(
                continuation.work.replayKey,
                continuation.work.replayIndex,
                2u,
                candidateOrdinal,
                kUpt04StreamInitialReservoirSelection,
                kUpt04DimensionInitialReservoirSelection));
        if (selected.valid == 0u
            || selectionRandom * newWeightSum < candidateWeight)
            selected = proposal;
        weightSum = newWeightSum;
    }
    if (selected.valid == 0u || candidateCount == 0u)
        return Upt04InvalidateInitialWork(invalid);

    const float finalizedMass = weightSum / float(candidateCount);
    const float effectiveProposalPdf = selected.target / finalizedMass;
    if (!Upt04FinitePositive(finalizedMass)
        || !Upt04FinitePositive(effectiveProposalPdf))
        return Upt04InvalidateInitialWork(invalid);

    Upt04InitialTraceWork nee = continuation.work;
    nee.eventKind = selected.lightType == kUpt04LightEmissiveTriangle
        ? 2u : 1u;
    nee.lightIndex = selected.lightIndex;
    nee.lightType = selected.lightType;
    nee.identity0 = selected.lightIndex + 1u;
    if (nee.identity0 == 0u
        || selected.lightIndex >= gUpt04Control.currentLightCount)
        return Upt04InvalidateInitialWork(invalid);
    const Upt04UnifiedLightRecord selectedLight =
        Upt04LoadCurrentLightIdentityRecord(selected.lightIndex);
    nee.identity1 = Upt04StableLightIdentityFingerprint(selectedLight);
    nee.sampleCoordinates = selected.sampleCoordinates;
    nee.pathLength = 3u;
    // No stable x3 geometry remap exists. T0/S0 replay both persisted
    // continuation draws and reconnect the stored light at the replayed x3.
    nee.reconnectionVertexLength = 1u;
    nee.proposalPdf = effectiveProposalPdf;
    nee.pathPdf = continuation.work.pathPdf * selected.sourcePdf;
    nee.partialJacobian = selected.emissiveReplayPdfScale;
    nee.lobeMask = continuation.work.lobeMask | selected.lobeMask;
    nee.contribution = selected.rawContribution;
    nee.target = selected.target;
    nee.candidateStatus = kUpt04SampleValidPositive;
    if (!Upt04FinitePositive(nee.pathPdf)
        || !all(isfinite(nee.contribution))
        || !Upt04FinitePositive(nee.target)
        || !Upt04AdmitThirdVertexNeeVisibility(nee.transport)
        || !Upt04RayBudgetWithinThreeVertexCeiling(nee.transport)
        || !Upt04BuildPrimaryVisibilityRequest(
            endpointSurface.position,
            endpointSurface.geometricNormal,
            endpointSurface.shadingNormal,
            selected.sampleDirection,
            selected.sampleDistance,
            nee.ray))
        return Upt04InvalidateInitialWork(invalid);
    nee.traceRequired = 1u;
    nee.candidateReady = 0u;
    nee.valid = 1u;
    return nee;
}
