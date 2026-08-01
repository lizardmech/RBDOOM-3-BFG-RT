import upt04_initial_work_contract;
import upt04_initial_random;
import upt04_initial_bindings;
import upt04_hit_contract;
import upt04_light_contract;
import upt04_local_light_proposal;
import upt04_local_light_evaluation;
import upt04_material_contract;
import upt04_material_set_live_probe;
import upt04_primary_surface_live_contract;
import upt04_random_contract;
import upt04_random_white_hash;
import upt04_transport_initial_bounded;

public Upt04InitialTraceWork Upt04PrepareDirectTrial(
    Upt04InitialTraceWork work,
    Upt04PrimarySurfaceRecord surface,
    uint2 pixel,
    float familyPdf)
{
    Upt04LocalLightRanges ranges;
    ranges.emissiveStart = gUpt04Control.emissiveRangeStart;
    ranges.emissiveCount = gUpt04Control.emissiveRangeCount;
    ranges.analyticStart = gUpt04Control.analyticRangeStart;
    ranges.analyticCount = gUpt04Control.analyticRangeCount;
    const Upt04LocalLightSelection selection = Upt04SelectUniformLocalLight(
        ranges,
        Upt04WhiteHash(Upt04InitialRandomKey(
            pixel,
            kUpt04StreamInitialLocalLightIdentity,
            kUpt04DimensionInitialLocalLightIdentity)),
        float2(
            Upt04WhiteHash(Upt04InitialRandomKey(
                pixel,
                kUpt04StreamInitialLocalLightSurfaceU,
                kUpt04DimensionInitialLocalLightSurfaceU)),
            Upt04WhiteHash(Upt04InitialRandomKey(
                pixel,
                kUpt04StreamInitialLocalLightSurfaceV,
                kUpt04DimensionInitialLocalLightSurfaceV))));
    if (selection.valid == 0u
        || selection.denseIndex >= gUpt04Control.currentLightCount)
        return work;

    const Upt04UnifiedLightRecord light = gUpt04CurrentLights[selection.denseIndex];
    const Upt04EvaluatedLocalLight evaluated = Upt04EvaluateSelectedLocalLight(
        selection,
        light,
        surface.worldPositionAndViewDepth.xyz);
    if (evaluated.valid == 0u)
        return work;

    const float3 bindingProbe = Upt04MaterialBindingProbe(
        surface.materialAndSurface.y,
        selection.sampleCoordinates);
    const Upt04MaterialEvaluation material = Upt04EvaluatePrimaryDirectProbe(
        surface,
        evaluated.sample.direction,
        evaluated.sample.radiance,
        bindingProbe);
    if (material.valid == 0u)
        return work;

    const float proposalPdf = familyPdf * selection.selectionPdf
        * evaluated.sample.solidAnglePdf;
    if (!Upt04FinitePositive(proposalPdf))
        return work;

    work.familyKind = kUpt04InitialFamilyDirect;
    work.eventKind = selection.type == kUpt04LightEmissiveTriangle
        ? 2u
        : 1u;
    work.lobeMask = material.lobeMask;
    work.identity0 = selection.denseIndex + 1u;
    if (work.identity0 == 0u)
        return work;
    work.identity1 = light.sourceIndex + 1u;
    work.lightIndex = selection.denseIndex;
    work.lightType = selection.type;
    work.proposalPdf = proposalPdf;
    work.pathPdf = proposalPdf;
    work.partialJacobian = 1.0f;
    work.accumulatedRouletteProbability = 1.0f;
    work.sampleCoordinates = evaluated.storedSampleCoordinates;
    work.contribution = material.weightedContribution;
    work.target = material.target;
    work.candidateStatus = evaluated.sample.status == kUpt04SampleValidPositive
            && material.target > 0.0f
        ? kUpt04SampleValidPositive
        : kUpt04SampleValidZero;
    work.transport = Upt04BeginBoundedTransport();
    if (work.candidateStatus == kUpt04SampleValidPositive)
    {
        if (!Upt04AdmitDirectVisibility(work.transport)
            || !Upt04RayBudgetWithinD0Ceiling(work.transport))
            return Upt04InvalidateInitialWork(work);
        work.ray.origin = surface.worldPositionAndViewDepth.xyz;
        work.ray.tMin = 0.01f;
        work.ray.direction = evaluated.sample.direction;
        work.ray.tMax = max(evaluated.sample.distance - 0.02f,
            work.ray.tMin + 1.0e-4f);
        if (!Upt04TraceRequestValid(work.ray))
            return Upt04InvalidateInitialWork(work);
        work.traceRequired = 1u;
    }
    else
    {
        work.candidateReady = 1u;
    }
    work.valid = 1u;
    return work;
}

public Upt04InitialTraceWork Upt04CompleteDirectVisibility(
    Upt04InitialTraceWork work,
    Upt04HitFacts hit)
{
    if (work.valid == 0u || work.familyKind != kUpt04InitialFamilyDirect
        || work.transport.stage != kUpt04TransportDirectVisibility
        || work.traceRequired == 0u)
        return Upt04InvalidateInitialWork(work);
    work.traceRequired = 0u;
    work.candidateReady = 1u;
    work.candidateFlags = kUpt04CandidateFlagVisibilityKnown;
    if (hit.status == kUpt04HitMiss)
        work.candidateFlags |= kUpt04CandidateFlagVisibilityPassed;
    else
        work.candidateStatus = kUpt04SampleValidZero;
    return work;
}
