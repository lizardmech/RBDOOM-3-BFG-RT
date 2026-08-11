import upt03_reservoir_contract;
import upt04_initial_work_contract;
import upt04_initial_random;
import upt04_initial_bindings;
import upt04_initial_control;
import upt04_hit_contract;
import upt04_geometry_decode;
import upt04_geometry_contract;
import upt04_light_contract;
import upt04_local_light_proposal;
import upt04_local_light_evaluation;
import upt04_material_contract;
import upt04_material_provider_contract;
import upt04_path_event_contract;
import upt04_primary_surface_live_contract;
import upt04_random_contract;
import upt04_random_white_hash;
import upt04_replay_random;
import upt04_transport_initial_bounded;
import upt04_emissive_lookup;
import upt04_initial_light_selection;

public struct Upt04IndirectHitTrialResult
{
    public Upt04InitialTraceWork endpointCandidate;
    public Upt04InitialTraceWork neeWork;
    public uint diagnostics;
};

public static const uint kUpt04IndirectDiagContinuationMiss = 1u << 0u;
public static const uint kUpt04IndirectDiagContinuationHit = 1u << 1u;
public static const uint kUpt04IndirectDiagGeometryFailure = 1u << 2u;
public static const uint kUpt04IndirectDiagEndpointEmissionFailure = 1u << 3u;
public static const uint kUpt04IndirectDiagEndpointEmissionZero = 1u << 4u;
public static const uint kUpt04IndirectDiagEndpointEmissionPositive = 1u << 5u;
public static const uint kUpt04IndirectDiagReverseLookupFailure = 1u << 6u;
public static const uint kUpt04IndirectDiagReversePdfZero = 1u << 7u;
public static const uint kUpt04IndirectDiagReversePdfPositive = 1u << 8u;
public static const uint kUpt04IndirectDiagSecondarySelectionFailure = 1u << 9u;
public static const uint kUpt04IndirectDiagSecondaryMaterialFailure = 1u << 10u;
public static const uint kUpt04IndirectDiagSecondaryPdfRayFailure = 1u << 11u;

public bool Upt04IndirectHitHasStableStaticLocator(Upt04HitFacts hit)
{
    if (hit.status != kUpt04HitTriangle || hit.geometryId != 0u
        || hit.instanceId > kUpt03PackedReconnectInstanceMask)
        return false;
    if (hit.instanceId == 0u)
        return true;
    return (gUpt04Control.availabilityFlags & kUpt04RouteStaticBuckets) != 0u
        && (hit.instanceId & kUpt04StaticBucketInstanceBit) != 0u
        && (hit.instanceId & ~kUpt04ShaderInstanceMask) == 0u;
}

public struct Upt04SecondaryNeeProposal
{
    public uint valid;
    public uint stage;
    public uint lightIndex;
    public uint lightType;
    public uint lobeMask;
    public float2 sampleCoordinates;
    public float3 rawContribution;
    public float target;
    public float sourcePdf;
    public float emissiveReplayPdfScale;
    public float3 sampleDirection;
    public float sampleDistance;
};

public Upt04SecondaryNeeProposal Upt04EvaluateSecondaryNeeProposal<
    TMaterialSet : IUpt04MaterialSet>(
    Upt04InitialTraceWork work,
    Upt04GeometryHitSurface hitSurface,
    Upt04PreparedDirectMaterial preparedMaterial,
    Upt04InitialLightSchedule schedule,
    uint2 pixel,
    uint candidateOrdinal)
{
    Upt04SecondaryNeeProposal proposal = (Upt04SecondaryNeeProposal)0;
    const Upt04LocalLightSelection selection =
        Upt04SelectInitialLightFromReplay(
            schedule,
            pixel,
            work.replayKey,
            work.replayIndex,
            1u,
            candidateOrdinal);
    if (selection.valid == 0u
        || selection.denseIndex >= gUpt04Control.currentLightCount)
        return proposal;
    proposal.stage = 1u;

    const Upt04UnifiedLightRecord light =
        Upt04LoadCurrentLightEvaluationRecord(
            selection.denseIndex, selection.type);
    if (selection.type == kUpt04LightEmissiveTriangle
        && selection.reserved != 0u
        && light.sourceIndex != selection.reserved - 1u)
        return proposal;
    const Upt04EvaluatedLocalLight evaluated =
        Upt04EvaluateSelectedLocalLight<TMaterialSet>(
            selection,
            light,
            hitSurface.position,
            gUpt04Control.availabilityFlags,
            gUpt04Control.emissiveScale);
    if (evaluated.valid == 0u
        || evaluated.sample.status != kUpt04SampleValidPositive)
        return proposal;
    proposal.stage = 2u;

    const Upt04MaterialEvaluation material = TMaterialSet.EvaluatePreparedDirect(
        preparedMaterial,
        evaluated.sample.direction,
        evaluated.sample.radiance);
    if (material.valid == 0u)
        return proposal;
    proposal.stage = 3u;

    const float sourcePdf = selection.selectionPdf
        * evaluated.sample.solidAnglePdf;
    const float pathPdf = work.pathPdf * sourcePdf;
    const float3 rawContribution = work.pathThroughput
        * material.weightedContribution;
    const float target = max(rawContribution.x,
        max(rawContribution.y, rawContribution.z));
    if (!Upt04FinitePositive(sourcePdf)
        || !Upt04FinitePositive(pathPdf)
        || !all(isfinite(rawContribution))
        || !Upt04FinitePositive(target)
        || !Upt04PrimaryVisibilityEndpointValid(
            hitSurface.position,
            hitSurface.geometricNormal,
            hitSurface.shadingNormal,
            evaluated.sample.direction,
            evaluated.sample.distance))
        return proposal;

    proposal.valid = 1u;
    proposal.stage = 4u;
    proposal.lightIndex = selection.denseIndex;
    proposal.lightType = selection.type;
    proposal.lobeMask = material.lobeMask;
    proposal.sampleCoordinates = evaluated.storedSampleCoordinates;
    proposal.rawContribution = rawContribution;
    proposal.target = target;
    proposal.sourcePdf = sourcePdf;
    proposal.emissiveReplayPdfScale =
        selection.type == kUpt04LightEmissiveTriangle
        ? asfloat(evaluated.sample.reserved) : 1.0f;
    if (!Upt04FinitePositive(proposal.emissiveReplayPdfScale)
        || proposal.emissiveReplayPdfScale > 4.0f)
        return (Upt04SecondaryNeeProposal)0;
    proposal.sampleDirection = evaluated.sample.direction;
    proposal.sampleDistance = evaluated.sample.distance;
    return proposal;
}

public Upt04InitialTraceWork Upt04MakeIndirectZero(
    Upt04InitialTraceWork work,
    Upt04HitFacts hit)
{
    work.candidateStatus = kUpt04SampleValidZero;
    work.candidateReady = 1u;
    work.traceRequired = 0u;
    work.contribution = float3(0.0f);
    work.target = 0.0f;
    if (hit.status == kUpt04HitTriangle)
    {
        if (hit.instanceId == 0xffffffffu)
            return Upt04InvalidateInitialWork(work);
        work.identity0 = hit.instanceId + 1u;
        work.identity1 = hit.primitiveId + 1u;
        work.sampleCoordinates = hit.barycentrics;
        work.pathLength = 1u;
        work.reconnectionVertexLength = 1u;
    }
    else
    {
        work.identity0 = work.surfaceIndex + 1u;
        work.identity1 = 0u;
        work.sampleCoordinates = float2(0.0f);
        work.pathLength = 1u;
        work.reconnectionVertexLength = 0u;
    }
    return work;
}

public Upt04InitialTraceWork Upt04PrepareIndirectTrial<
    TMaterialSet : IUpt04MaterialSet>(
    Upt04InitialTraceWork work,
    Upt04PrimarySurfaceRecord surface,
    uint2 pixel,
    Upt04BoundedTransportState transport)
{
    // Persist the complete continuation RNG seed before sampling. Temporal
    // and spatial providers can reproduce the same lobe and direction without
    // retaining or guessing the source pixel.
    work.replayKey = Upt04InitialReplayKey(pixel);
    work.replayIndex = gUpt04Control.frameSampleIndex;
    const Upt04PathEvent continuation = TMaterialSet.SamplePrimaryContinuation(
        surface,
        Upt04WhiteHash(Upt04PersistedReplayRandomKey(
            work.replayKey,
            work.replayIndex,
            0u,
            0u,
            kUpt04StreamMaterialLobeSelection,
            kUpt04DimensionMaterialLobeSelection)),
        float2(
            Upt04WhiteHash(Upt04PersistedReplayRandomKey(
                work.replayKey,
                work.replayIndex,
                0u,
                0u,
                kUpt04StreamIndirectContinuationU,
                kUpt04DimensionIndirectContinuationU)),
            Upt04WhiteHash(Upt04PersistedReplayRandomKey(
                work.replayKey,
                work.replayIndex,
                0u,
                0u,
                kUpt04StreamIndirectContinuationV,
                kUpt04DimensionIndirectContinuationV))));
    if (continuation.valid == 0u || !Upt04FinitePositive(continuation.pdf))
        return work;

    work.familyKind = kUpt04InitialFamilyIndirect;
    work.eventKind = 4u; // tau=2, BSDF continuation technique.
    work.lobeMask = continuation.lobeMask;
    work.candidateFlags = kUpt04CandidateFlagGlobal
        | kUpt04CandidateFlagReplayable
        | kUpt04CandidateFlagNeedsShift
        | (Upt04DirectProposalParityValue(
                gUpt04Control.availabilityFlags) != 0u
            ? kUpt04CandidateFlagDirectProposalParity : 0u);
    work.proposalPdf = continuation.pdf;
    work.pathPdf = continuation.pdf;
    work.partialJacobian = 1.0f;
    work.accumulatedRouletteProbability = 1.0f;
    work.pathThroughput = continuation.throughput;
    work.transport = transport;
    if (!Upt04AdmitIndirectContinuation(work.transport)
        || !Upt04RayBudgetWithinD0Ceiling(work.transport))
        return Upt04InvalidateInitialWork(work);
    if (!Upt04BuildContinuationRequest(
            surface.worldPositionAndViewDepth.xyz,
            surface.geometricNormalAndRoughness.xyz,
            surface.shadingNormalAndOpacity.xyz,
            continuation.direction,
            work.ray))
        return Upt04InvalidateInitialWork(work);
    work.traceRequired = 1u;
    work.valid = 1u;
    return work;
}

public Upt04IndirectHitTrialResult Upt04PrepareIndirectHitTrials<
    TMaterialSet : IUpt04MaterialSet>(
    Upt04InitialTraceWork work,
    Upt04HitFacts hit,
    uint2 pixel)
{
    Upt04IndirectHitTrialResult result = (Upt04IndirectHitTrialResult)0;
    if (work.valid == 0u || work.familyKind != kUpt04InitialFamilyIndirect
        || work.transport.stage != kUpt04TransportIndirectContinuation
        || work.traceRequired == 0u)
        return result;
    work.traceRequired = 0u;
    if (hit.status == kUpt04HitMiss)
    {
        result.diagnostics |= kUpt04IndirectDiagContinuationMiss;
        result.endpointCandidate = Upt04MakeIndirectZero(work, hit);
        return result;
    }
    result.diagnostics |= kUpt04IndirectDiagContinuationHit;

    const Upt04GeometryHitSurface hitSurface = Upt04DecodeGeometryHit(hit);
    if (hitSurface.valid == 0u || hit.instanceId == 0xffffffffu)
    {
        result.diagnostics |= kUpt04IndirectDiagGeometryFailure;
        return result;
    }

    Upt04InitialTraceWork endpoint = work;
    // The continuation query reached this exact endpoint in the current TLAS.
    // Preserve that proof through reservoir packing so T0 can keep a
    // current-selected endpoint without treating it as an unverified shifted
    // history sample and falling back to the raw D0 reservoir.
    endpoint.candidateFlags |= kUpt04CandidateFlagVisibilityKnown
        | kUpt04CandidateFlagVisibilityPassed;
    endpoint.identity0 = hit.instanceId + 1u;
    if (endpoint.identity0 == 0u)
        return result;
    endpoint.identity1 = hit.primitiveId + 1u;
    endpoint.sampleCoordinates = hit.barycentrics;
    endpoint.pathLength = 1u;
    endpoint.reconnectionVertexLength = 1u;
    const Upt04MaterialEmissionEvaluation endpointEmission =
        TMaterialSet.EvaluateEmissiveEndpoint(
            hitSurface.materialIndex,
            hitSurface.triangleClassAndFlags,
            hitSurface.texCoord,
            float2(0.0f),
            float2(0.0f));
    if (endpointEmission.valid == 0u)
    {
        result.diagnostics |= kUpt04IndirectDiagEndpointEmissionFailure;
        return result;
    }
    result.diagnostics |= any(endpointEmission.radiance > 0.0f)
        ? kUpt04IndirectDiagEndpointEmissionPositive
        : kUpt04IndirectDiagEndpointEmissionZero;

    float3 endpointContribution = endpoint.pathThroughput
        * endpointEmission.radiance;
    const Upt04PrimarySurfaceRecord primaryReceiver =
        Upt04LoadPrimarySurfaceRecord(work.surfaceIndex);
    if (!Upt04PrimarySurfaceRecordValid(primaryReceiver))
    {
        result.diagnostics |= kUpt04IndirectDiagGeometryFailure;
        return result;
    }
    uint reverseStatus = 0u;
    const float endpointNeePdf = any(endpointEmission.radiance > 0.0f)
        ? Upt04ExactNeePdfForEmissiveHit<TMaterialSet>(
            hit.instanceId,
            hit.primitiveId,
            primaryReceiver.worldPositionAndViewDepth.xyz,
            hit.barycentrics,
            reverseStatus)
        : 0.0f;
    if (reverseStatus != 0u)
    {
        result.diagnostics |= reverseStatus == kUpt04ReverseNeeLookupFailure
            ? kUpt04IndirectDiagReverseLookupFailure
            : (reverseStatus == kUpt04ReverseNeePositivePdf
                ? kUpt04IndirectDiagReversePdfPositive
                : kUpt04IndirectDiagReversePdfZero);
    }
    const float endpointMis = Upt04BalanceHeuristic(
        work.pathPdf,
        endpointNeePdf);
    endpointContribution *= endpointMis;
    const float endpointTarget = max(endpointContribution.x,
        max(endpointContribution.y, endpointContribution.z));
    if (!all(isfinite(endpointContribution)) || !isfinite(endpointTarget))
        return result;
    endpoint.contribution = endpointContribution;
    endpoint.target = endpointTarget;
    // Primary continuation throughput already contains 1 / p_bsdf. Fold that
    // inner sampling UCW into the PSS candidate and retain the physical PDF
    // only in pathPdf for replay; streaming it again as proposalPdf would
    // apply the inverse BSDF density twice.
    endpoint.proposalPdf = 1.0f;
    endpoint.pathPdf = work.pathPdf;
    endpoint.candidateStatus = endpointTarget > 0.0f
        ? kUpt04SampleValidPositive
        : kUpt04SampleValidZero;
    endpoint.candidateReady = 1u;
    result.endpointCandidate = endpoint;

    Upt04LocalLightRanges ranges;
    ranges.emissiveStart = gUpt04Control.emissiveRangeStart;
    ranges.emissiveCount = gUpt04Control.emissiveRangeCount;
    ranges.analyticStart = gUpt04Control.analyticRangeStart;
    ranges.analyticCount = gUpt04Control.analyticRangeCount;
    if (!Upt04LocalLightRangesStructurallyValid(
            ranges, gUpt04Control.currentLightCount)
        || Upt04LocalLightRangeCount(ranges) == 0u)
    {
        result.diagnostics |= kUpt04IndirectDiagSecondarySelectionFailure;
        return result;
    }

    const Upt04PreparedDirectMaterial preparedMaterial =
        TMaterialSet.PrepareSecondaryDirect(hitSurface, -work.ray.direction);
    if (preparedMaterial.valid == 0u)
    {
        result.diagnostics |= kUpt04IndirectDiagSecondaryMaterialFailure;
        return result;
    }

    const Upt04InitialLightSchedule schedule =
        Upt04MakeSecondaryLightSchedule(ranges);
    const uint candidateCount = schedule.candidateCount;
    Upt04SecondaryNeeProposal selected = (Upt04SecondaryNeeProposal)0;
    float weightSum = 0.0f;
    uint deepestStage = 0u;
    [loop]
    for (uint candidateOrdinal = 0u;
         candidateOrdinal < candidateCount;
         ++candidateOrdinal)
    {
        const Upt04SecondaryNeeProposal proposal =
            Upt04EvaluateSecondaryNeeProposal<TMaterialSet>(
                work,
                hitSurface,
                preparedMaterial,
                schedule,
                pixel,
                candidateOrdinal);
        deepestStage = max(deepestStage, proposal.stage);
        if (proposal.valid == 0u)
            continue;
        const float candidateWeight = proposal.target / proposal.sourcePdf;
        const float newWeightSum = weightSum + candidateWeight;
        if (!Upt04FinitePositive(candidateWeight)
            || !Upt04FinitePositive(newWeightSum))
            continue;
        const float selectionRandom = Upt04WhiteHash(
            Upt04PersistedReplayRandomKey(
                work.replayKey,
                work.replayIndex,
                1u,
                candidateOrdinal,
                kUpt04StreamInitialReservoirSelection,
                kUpt04DimensionInitialReservoirSelection));
        if (selected.valid == 0u
            || selectionRandom * newWeightSum < candidateWeight)
            selected = proposal;
        weightSum = newWeightSum;
    }
    if (selected.valid == 0u || candidateCount == 0u)
    {
        result.diagnostics |= deepestStage <= 1u
            ? kUpt04IndirectDiagSecondarySelectionFailure
            : (deepestStage == 2u
                ? kUpt04IndirectDiagSecondaryMaterialFailure
                : kUpt04IndirectDiagSecondaryPdfRayFailure);
        return result;
    }
    const float finalizedMass = weightSum / float(candidateCount);
    const float effectiveProposalPdf = selected.target / finalizedMass;
    if (!Upt04FinitePositive(finalizedMass)
        || !Upt04FinitePositive(effectiveProposalPdf))
    {
        result.diagnostics |= kUpt04IndirectDiagSecondaryPdfRayFailure;
        return result;
    }

    Upt04InitialTraceWork nee = work;
    nee.eventKind = selected.lightType == kUpt04LightEmissiveTriangle
        ? 2u : 1u;
    nee.lightIndex = selected.lightIndex;
    nee.lightType = selected.lightType;
    nee.identity0 = selected.lightIndex + 1u;
    if (nee.identity0 == 0u)
        return result;
    if (selected.lightIndex >= gUpt04Control.currentLightCount)
        return result;
    const Upt04UnifiedLightRecord selectedLight =
        Upt04LoadCurrentLightIdentityRecord(selected.lightIndex);
    nee.identity1 = Upt04StableLightIdentityFingerprint(selectedLight);
    nee.sampleCoordinates = selected.sampleCoordinates;
    nee.pathLength = 2u;
    if (Upt04IndirectHitHasStableStaticLocator(hit))
    {
        nee.reconnectionVertexLength = 2u;
        nee.reconnectionInstanceId = hit.instanceId;
        nee.reconnectionPrimitiveId = hit.primitiveId;
        nee.reconnectionFrontFace = hit.frontFace;
        nee.reconnectionBarycentrics = hit.barycentrics;
    }
    else
    {
        // No stable previous-to-current geometry identity exists yet for
        // dynamic, rigid, or skinned routes. Keep exact random replay.
        nee.reconnectionVertexLength = 1u;
    }
    nee.proposalPdf = effectiveProposalPdf;
    nee.pathPdf = work.pathPdf * selected.sourcePdf;
    nee.partialJacobian = selected.emissiveReplayPdfScale;
    nee.lobeMask = work.lobeMask | selected.lobeMask;
    // Preserve the inner-RIS estimator while retaining the physical target
    // needed by temporal shift mapping. Streaming target / proposalPdf still
    // contributes exactly finalizedMass, and resolve still produces
    // rawContribution * finalizedMass / target. This is algebraically
    // identical to folding the UCW into contribution with proposalPdf = 1,
    // but it makes the selected NEE vertex independently reconnectable.
    // Zero/malformed trials remain in the fixed candidateCount denominator
    // and are never selectable.
    nee.contribution = selected.rawContribution;
    nee.target = selected.target;
    if (!all(isfinite(nee.contribution)) || !Upt04FinitePositive(nee.target))
    {
        result.diagnostics |= kUpt04IndirectDiagSecondaryPdfRayFailure;
        return result;
    }
    nee.candidateStatus = kUpt04SampleValidPositive;

    if (!Upt04AdmitIndirectNeeVisibility(nee.transport)
        || !Upt04RayBudgetWithinD0Ceiling(nee.transport))
    {
        result.diagnostics |= kUpt04IndirectDiagSecondaryPdfRayFailure;
        return result;
    }
    if (!Upt04BuildPrimaryVisibilityRequest(
            hitSurface.position,
            hitSurface.geometricNormal,
            hitSurface.shadingNormal,
            selected.sampleDirection,
            selected.sampleDistance,
            nee.ray))
    {
        result.diagnostics |= kUpt04IndirectDiagSecondaryPdfRayFailure;
        return result;
    }
    nee.traceRequired = 1u;
    nee.candidateReady = 0u;
    result.neeWork = nee;
    return result;
}

public Upt04VisibilityIgnore Upt04IndirectNeeVisibilityIgnore(
    Upt04InitialTraceWork work)
{
    if (work.valid == 0u || work.familyKind != kUpt04InitialFamilyIndirect
        || work.transport.stage != kUpt04TransportIndirectNeeVisibility
        || work.lightType != kUpt04LightEmissiveTriangle
        || work.lightIndex >= gUpt04Control.currentLightCount)
        return Upt04NoVisibilityIgnore();
    const Upt04UnifiedLightRecord light =
        Upt04LoadCurrentLightVisibilityRecord(work.lightIndex);
    return light.type == kUpt04LightEmissiveTriangle
        ? Upt04MakeVisibilityIgnore(light.instanceId, light.primitiveIndex)
        : Upt04NoVisibilityIgnore();
}

public Upt04InitialTraceWork Upt04CompleteIndirectNeeVisibility(
    Upt04InitialTraceWork work,
    Upt04HitFacts hit)
{
    if (work.valid == 0u || work.familyKind != kUpt04InitialFamilyIndirect
        || work.transport.stage != kUpt04TransportIndirectNeeVisibility
        || work.traceRequired == 0u)
        return Upt04InvalidateInitialWork(work);
    work.traceRequired = 0u;
    work.candidateReady = 1u;
    work.candidateFlags |= kUpt04CandidateFlagVisibilityKnown;
    if (hit.status == kUpt04HitMiss)
        work.candidateFlags |= kUpt04CandidateFlagVisibilityPassed;
    else
    {
        work.candidateStatus = kUpt04SampleValidZero;
        work.contribution = float3(0.0f);
        work.target = 0.0f;
    }
    return work;
}
