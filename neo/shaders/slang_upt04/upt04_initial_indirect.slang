import upt04_initial_work_contract;
import upt04_initial_random;
import upt04_initial_bindings;
import upt04_hit_contract;
import upt04_geometry_decode;
import upt04_geometry_contract;
import upt04_light_contract;
import upt04_local_light_proposal;
import upt04_local_light_evaluation;
import upt04_material_set_live_probe;
import upt04_material_primary_live;
import upt04_material_contract;
import upt04_path_event_contract;
import upt04_primary_surface_live_contract;
import upt04_random_contract;
import upt04_random_white_hash;
import upt04_transport_initial_bounded;
import upt04_emissive_lookup;

public struct Upt04IndirectHitTrialResult
{
    public Upt04InitialTraceWork endpointCandidate;
    public Upt04InitialTraceWork neeWork;
};

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

public Upt04InitialTraceWork Upt04PrepareIndirectTrial(
    Upt04InitialTraceWork work,
    Upt04PrimarySurfaceRecord surface,
    uint2 pixel,
    Upt04BoundedTransportState transport)
{
    const Upt04PathEvent continuation = Upt04SamplePrimaryContinuation(
        surface,
        Upt04WhiteHash(Upt04InitialRandomKey(
            pixel,
            kUpt04StreamMaterialLobeSelection,
            kUpt04DimensionMaterialLobeSelection)),
        float2(
            Upt04WhiteHash(Upt04InitialRandomKey(
                pixel,
                kUpt04StreamIndirectContinuationU,
                kUpt04DimensionIndirectContinuationU)),
            Upt04WhiteHash(Upt04InitialRandomKey(
                pixel,
                kUpt04StreamIndirectContinuationV,
                kUpt04DimensionIndirectContinuationV))));
    if (continuation.valid == 0u || !Upt04FinitePositive(continuation.pdf))
        return work;

    work.familyKind = kUpt04InitialFamilyIndirect;
    work.eventKind = 4u; // tau=2, BSDF continuation technique.
    work.lobeMask = continuation.lobeMask;
    work.candidateFlags = kUpt04CandidateFlagGlobal
        | kUpt04CandidateFlagReplayable
        | kUpt04CandidateFlagNeedsShift;
    work.replayKey = Upt04InitialReplayKey(pixel);
    work.replayIndex = gUpt04Control.frameSampleIndex;
    work.proposalPdf = continuation.pdf;
    work.pathPdf = continuation.pdf;
    work.partialJacobian = 1.0f;
    work.accumulatedRouletteProbability = 1.0f;
    work.pathThroughput = continuation.throughput;
    work.transport = transport;
    if (!Upt04AdmitIndirectContinuation(work.transport)
        || !Upt04RayBudgetWithinD0Ceiling(work.transport))
        return Upt04InvalidateInitialWork(work);
    work.ray.origin = surface.worldPositionAndViewDepth.xyz;
    work.ray.tMin = 0.01f;
    work.ray.direction = continuation.direction;
    work.ray.tMax = 1.0e30f;
    if (!Upt04TraceRequestValid(work.ray))
        return Upt04InvalidateInitialWork(work);
    work.traceRequired = 1u;
    work.valid = 1u;
    return work;
}

public Upt04IndirectHitTrialResult Upt04PrepareIndirectHitTrials(
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
        result.endpointCandidate = Upt04MakeIndirectZero(work, hit);
        return result;
    }

    const Upt04GeometryHitSurface hitSurface = Upt04DecodeGeometryHit(hit);
    if (hitSurface.valid == 0u || hit.instanceId == 0xffffffffu)
        return result;

    Upt04InitialTraceWork endpoint = work;
    endpoint.identity0 = hit.instanceId + 1u;
    if (endpoint.identity0 == 0u)
        return result;
    endpoint.identity1 = hit.primitiveId + 1u;
    endpoint.sampleCoordinates = hit.barycentrics;
    endpoint.pathLength = 1u;
    endpoint.reconnectionVertexLength = 1u;
    const Upt04MaterialEmissionEvaluation endpointEmission =
        Upt04EvaluateEmissiveEndpoint(
            hitSurface.materialIndex,
            hitSurface.triangleClassAndFlags,
            hitSurface.texCoord);
    if (endpointEmission.valid == 0u)
        return result;

    float3 endpointContribution = endpoint.pathThroughput
        * endpointEmission.radiance;
    const float endpointNeePdf = Upt04ExactNeePdfForEmissiveHit(
        hit.instanceId,
        hit.primitiveId,
        work.ray.origin,
        hitSurface.position);
    const float endpointMis = Upt04BalanceHeuristic(
        endpoint.proposalPdf,
        endpointNeePdf);
    endpointContribution *= endpointMis;
    const float endpointTarget = max(endpointContribution.x,
        max(endpointContribution.y, endpointContribution.z));
    if (!all(isfinite(endpointContribution)) || !isfinite(endpointTarget))
        return result;
    endpoint.contribution = endpointContribution;
    endpoint.target = endpointTarget;
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
        return result;

    const Upt04LocalLightSelection selection = Upt04SelectUniformLocalLight(
        ranges,
        Upt04WhiteHash(Upt04InitialRandomKeyAtVertex(
            pixel,
            1u,
            kUpt04StreamInitialLocalLightIdentity,
            kUpt04DimensionInitialLocalLightIdentity)),
        float2(
            Upt04WhiteHash(Upt04InitialRandomKeyAtVertex(
                pixel,
                1u,
                kUpt04StreamInitialLocalLightSurfaceU,
                kUpt04DimensionInitialLocalLightSurfaceU)),
            Upt04WhiteHash(Upt04InitialRandomKeyAtVertex(
                pixel,
                1u,
                kUpt04StreamInitialLocalLightSurfaceV,
                kUpt04DimensionInitialLocalLightSurfaceV))));
    if (selection.valid == 0u
        || selection.denseIndex >= gUpt04Control.currentLightCount)
        return result;
    const Upt04UnifiedLightRecord light = gUpt04CurrentLights[selection.denseIndex];
    const Upt04EvaluatedLocalLight evaluated = Upt04EvaluateSelectedLocalLight(
        selection,
        light,
        hitSurface.position);
    if (evaluated.valid == 0u)
        return result;
    const Upt04MaterialEvaluation material = Upt04EvaluateSecondaryDirectProbe(
        hitSurface,
        evaluated.sample.direction,
        evaluated.sample.radiance);
    if (material.valid == 0u)
        return result;

    const float neePdf = selection.selectionPdf * evaluated.sample.solidAnglePdf;
    const float proposalPdf = work.proposalPdf * neePdf;
    if (!Upt04FinitePositive(proposalPdf))
        return result;

    Upt04InitialTraceWork nee = work;
    nee.eventKind = selection.type == kUpt04LightEmissiveTriangle ? 2u : 1u;
    nee.lightIndex = selection.denseIndex;
    nee.lightType = selection.type;
    nee.identity0 = selection.denseIndex + 1u;
    if (nee.identity0 == 0u)
        return result;
    nee.identity1 = light.sourceIndex + 1u;
    nee.sampleCoordinates = evaluated.storedSampleCoordinates;
    nee.pathLength = 2u;
    nee.reconnectionVertexLength = 1u;
    nee.proposalPdf = proposalPdf;
    nee.pathPdf = proposalPdf;
    nee.contribution = nee.pathThroughput * material.weightedContribution;
    nee.target = max(nee.contribution.x,
        max(nee.contribution.y, nee.contribution.z));
    if (!all(isfinite(nee.contribution)) || !isfinite(nee.target))
        return result;
    nee.candidateStatus = evaluated.sample.status == kUpt04SampleValidPositive
            && nee.target > 0.0f
        ? kUpt04SampleValidPositive
        : kUpt04SampleValidZero;
    if (nee.candidateStatus == kUpt04SampleValidZero)
    {
        nee.candidateReady = 1u;
        nee.traceRequired = 0u;
        nee.contribution = float3(0.0f);
        nee.target = 0.0f;
        result.neeWork = nee;
        return result;
    }

    if (!Upt04AdmitIndirectNeeVisibility(nee.transport)
        || !Upt04RayBudgetWithinD0Ceiling(nee.transport))
        return result;
    nee.ray.origin = hitSurface.position;
    nee.ray.tMin = 0.01f;
    nee.ray.direction = evaluated.sample.direction;
    nee.ray.tMax = max(evaluated.sample.distance - 0.02f,
        nee.ray.tMin + 1.0e-4f);
    if (!Upt04TraceRequestValid(nee.ray))
        return result;
    nee.traceRequired = 1u;
    nee.candidateReady = 0u;
    result.neeWork = nee;
    return result;
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
