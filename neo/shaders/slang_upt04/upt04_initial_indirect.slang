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
import upt04_material_contract;
import upt04_path_event_contract;
import upt04_primary_surface_live_contract;
import upt04_random_contract;
import upt04_random_white_hash;
import upt04_transport_initial_bounded;

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
    float familyPdf)
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
    if (continuation.valid == 0u)
        return work;
    const float proposalPdf = familyPdf * continuation.pdf;
    if (!Upt04FinitePositive(proposalPdf))
        return work;

    work.familyKind = kUpt04InitialFamilyIndirect;
    work.eventKind = 4u;
    work.lobeMask = continuation.lobeMask;
    work.candidateFlags = kUpt04CandidateFlagGlobal
        | kUpt04CandidateFlagReplayable
        | kUpt04CandidateFlagNeedsShift;
    work.replayKey = Upt04InitialReplayKey(pixel);
    work.replayIndex = gUpt04Control.frameSampleIndex;
    work.proposalPdf = proposalPdf;
    work.pathPdf = proposalPdf;
    work.partialJacobian = 1.0f;
    work.accumulatedRouletteProbability = 1.0f;
    work.pathThroughput = continuation.throughput;
    work.transport = Upt04BeginBoundedTransport();
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

public Upt04InitialTraceWork Upt04PrepareIndirectHitTrial(
    Upt04InitialTraceWork work,
    Upt04HitFacts hit,
    uint2 pixel)
{
    if (work.valid == 0u || work.familyKind != kUpt04InitialFamilyIndirect
        || work.transport.stage != kUpt04TransportIndirectContinuation
        || work.traceRequired == 0u)
        return Upt04InvalidateInitialWork(work);
    work.traceRequired = 0u;
    if (hit.status == kUpt04HitMiss)
        return Upt04MakeIndirectZero(work, hit);

    const Upt04GeometryHitSurface hitSurface = Upt04DecodeGeometryHit(hit);
    if (hitSurface.valid == 0u)
        return Upt04InvalidateInitialWork(work);
    work.identity0 = hit.instanceId + 1u;
    if (work.identity0 == 0u)
        return Upt04InvalidateInitialWork(work);
    work.identity1 = hit.primitiveId + 1u;
    work.sampleCoordinates = hit.barycentrics;
    work.pathLength = 1u;
    work.reconnectionVertexLength = 1u;

    const Upt04MaterialEmissionEvaluation endpointEmission =
        Upt04EvaluateEmissiveEndpoint(
            hitSurface.materialIndex,
            hitSurface.triangleClassAndFlags,
            hitSurface.texCoord);
    if (endpointEmission.valid == 0u)
        return Upt04InvalidateInitialWork(work);
    const float3 endpointContribution = work.pathThroughput
        * endpointEmission.radiance;
    const float endpointTarget = max(endpointContribution.x,
        max(endpointContribution.y, endpointContribution.z));
    if (!all(isfinite(endpointContribution)) || !isfinite(endpointTarget))
        return Upt04InvalidateInitialWork(work);
    if (endpointTarget > 0.0f)
    {
        work.contribution = endpointContribution;
        work.target = endpointTarget;
        work.candidateStatus = kUpt04SampleValidPositive;
        work.candidateReady = 1u;
        return work;
    }

    Upt04LocalLightRanges ranges;
    ranges.emissiveStart = gUpt04Control.emissiveRangeStart;
    ranges.emissiveCount = gUpt04Control.emissiveRangeCount;
    ranges.analyticStart = gUpt04Control.analyticRangeStart;
    ranges.analyticCount = gUpt04Control.analyticRangeCount;
    if (!Upt04LocalLightRangesStructurallyValid(
            ranges,
            gUpt04Control.currentLightCount))
        return Upt04InvalidateInitialWork(work);
    if (Upt04LocalLightRangeCount(ranges) == 0u)
        return Upt04MakeIndirectZero(work, hit);

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
        return Upt04InvalidateInitialWork(work);
    const Upt04UnifiedLightRecord light = gUpt04CurrentLights[selection.denseIndex];
    const Upt04EvaluatedLocalLight evaluated = Upt04EvaluateSelectedLocalLight(
        selection,
        light,
        hitSurface.position);
    if (evaluated.valid == 0u)
        return Upt04InvalidateInitialWork(work);
    const Upt04MaterialEvaluation material = Upt04EvaluateSecondaryDirectProbe(
        hitSurface,
        evaluated.sample.direction,
        evaluated.sample.radiance);
    if (material.valid == 0u)
        return Upt04InvalidateInitialWork(work);

    const float neePdf = selection.selectionPdf * evaluated.sample.solidAnglePdf;
    const float proposalPdf = work.proposalPdf * neePdf;
    if (!Upt04FinitePositive(proposalPdf))
        return Upt04InvalidateInitialWork(work);
    work.proposalPdf = proposalPdf;
    work.pathPdf = proposalPdf;
    work.lightIndex = selection.denseIndex;
    work.lightType = selection.type;
    work.identity0 = selection.denseIndex + 1u;
    if (work.identity0 == 0u)
        return Upt04InvalidateInitialWork(work);
    work.identity1 = light.sourceIndex + 1u;
    work.contribution = work.pathThroughput * material.weightedContribution;
    work.target = max(work.contribution.x,
        max(work.contribution.y, work.contribution.z));
    if (!all(isfinite(work.contribution)) || !isfinite(work.target))
        return Upt04InvalidateInitialWork(work);
    work.candidateStatus = evaluated.sample.status == kUpt04SampleValidPositive
            && work.target > 0.0f
        ? kUpt04SampleValidPositive
        : kUpt04SampleValidZero;
    if (work.candidateStatus == kUpt04SampleValidZero)
        return Upt04MakeIndirectZero(work, hit);

    if (!Upt04AdmitIndirectNeeVisibility(work.transport)
        || !Upt04RayBudgetWithinD0Ceiling(work.transport))
        return Upt04InvalidateInitialWork(work);
    work.ray.origin = hitSurface.position;
    work.ray.tMin = 0.01f;
    work.ray.direction = evaluated.sample.direction;
    work.ray.tMax = max(evaluated.sample.distance - 0.02f,
        work.ray.tMin + 1.0e-4f);
    if (!Upt04TraceRequestValid(work.ray))
        return Upt04InvalidateInitialWork(work);
    work.traceRequired = 1u;
    return work;
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
