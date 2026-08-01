import upt03_reservoir_contract;
import upt04_hit_contract;
import upt04_light_contract;
import upt04_primary_surface_live_contract;
import upt04_material_contract;
import upt04_initial_control;
import upt04_initial_bindings;
import upt04_local_light_proposal;
import upt04_transport_initial_bounded;
import upt04_material_set_live_probe;
import upt04_random_contract;
import upt04_random_white_hash;
import upt04_path_event_contract;

public struct Upt04InitialTraceWork
{
    public uint valid;
    public uint surfaceIndex;
    public uint lightIndex;
    public uint lightType;
    public float proposalPdf;
    public float2 sampleCoordinates;
    public Upt04TraceRequest ray;
    public Upt04MaterialEvaluation material;
    public Upt04BoundedTransportState transport;
};

public Upt04RandomKey Upt04InitialRandomKey(
    uint2 pixel,
    uint streamNamespace,
    uint dimension)
{
    Upt04RandomKey key = (Upt04RandomKey)0;
    key.pixelX = pixel.x;
    key.pixelY = pixel.y;
    key.frameSampleIndex = gUpt04Control.frameSampleIndex;
    key.passNamespace = kUpt04PassInitial;
    key.streamNamespace = streamNamespace;
    key.dimension = dimension;
    return key;
}

public Upt04InitialTraceWork Upt04PrepareInitialTraceWork(uint2 pixel)
{
    Upt04InitialTraceWork work = (Upt04InitialTraceWork)0;
    if (pixel.x >= gUpt04Control.renderWidth || pixel.y >= gUpt04Control.renderHeight)
        return work;
    const uint surfaceIndex = pixel.y * gUpt04Control.renderWidth + pixel.x;
    if (surfaceIndex >= gUpt04Control.surfaceCount)
        return work;
    work.surfaceIndex = surfaceIndex;
    if ((gUpt04Control.enabledFamilyMask & kUpt04FamilyLocalLight) == 0u)
        return work;

    const Upt04PrimarySurfaceRecord surface = gUpt04PrimarySurfaces[surfaceIndex];
    if (!Upt04PrimarySurfaceRecordValid(surface))
        return work;

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
    if (selection.valid == 0u)
        return work;
    if (selection.denseIndex >= gUpt04Control.currentLightCount)
        return work;

    const Upt04UnifiedLightRecord light = gUpt04CurrentLights[selection.denseIndex];
    if (!Upt04SelectionMatchesRecord(selection, light))
        return work;
    // Fail closed: emissive triangles require exact vertex replay.  This compile
    // closure never substitutes their center for a sampled point.
    if (selection.requiresTriangleReplay != 0u)
        return work;

    const Upt04LocalLightSample lightSample = Upt04SampleAnalyticSphere(
        light,
        surface.worldPositionAndViewDepth.xyz,
        selection.sampleCoordinates);
    if (lightSample.valid == 0u)
        return work;

    work.transport = Upt04BeginBoundedTransport();
    if (!Upt04AdmitDirectVisibility(work.transport)
        || !Upt04RayBudgetWithinD0Ceiling(work.transport))
        return work;

    work.ray.origin = surface.worldPositionAndViewDepth.xyz;
    work.ray.tMin = 0.01f;
    work.ray.direction = lightSample.direction;
    work.ray.tMax = max(lightSample.distance - 0.02f, work.ray.tMin + 1.0e-4f);
    if (!Upt04TraceRequestValid(work.ray))
        return work;

    const float3 bindingProbe = Upt04MaterialBindingProbe(
        surface.materialAndSurface.y,
        selection.sampleCoordinates);
    work.material = Upt04EvaluatePrimaryDirectProbe(
        surface,
        lightSample.direction,
        lightSample.radiance,
        bindingProbe);
    if (work.material.valid == 0u)
        return work;

    work.lightIndex = selection.denseIndex;
    work.lightType = selection.type;
    work.proposalPdf = selection.selectionPdf * lightSample.solidAnglePdf;
    if (!Upt04FinitePositive(work.proposalPdf))
        return work;
    work.sampleCoordinates = selection.sampleCoordinates;
    work.valid = 1u;
    return work;
}

public void Upt04FinishInitialTraceWork(Upt04InitialTraceWork work, Upt04HitFacts hit)
{
    Upt03LogicalReservoir reservoir = (Upt03LogicalReservoir)0;
    if (work.valid != 0u)
    {
        Upt03Candidate candidate = (Upt03Candidate)0;
        candidate.status = hit.status == kUpt04HitMiss && work.material.target > 0.0f ? 2u : 1u;
        candidate.eventKind = work.lightType == kUpt04LightEmissiveTriangle ? 2u : kUpt04EventAnalytic;
        candidate.lobeMask = work.material.lobeMask;
        candidate.flags = 1u;
        candidate.identity0 = work.lightIndex + 1u;
        candidate.generationFingerprint = kUpt03ExpectedGeneration;
        candidate.sampleCoord0 = work.sampleCoordinates.x;
        candidate.sampleCoord1 = work.sampleCoordinates.y;
        if (candidate.status == 2u)
        {
            candidate.contributionR = work.material.weightedContribution.x;
            candidate.contributionG = work.material.weightedContribution.y;
            candidate.contributionB = work.material.weightedContribution.z;
            candidate.target = work.material.target;
        }
        candidate.proposalPdf = work.proposalPdf;
        candidate.pathPdf = work.proposalPdf;
        candidate.partialJacobian = 1.0f;
        candidate.accumulatedRouletteProbability = 1.0f;
        const uint2 pixel = uint2(
            work.surfaceIndex % gUpt04Control.renderWidth,
            work.surfaceIndex / gUpt04Control.renderWidth);
        Upt03StreamCandidate(
            reservoir,
            candidate,
            Upt04WhiteHash(Upt04InitialRandomKey(
                pixel,
                kUpt04StreamInitialReservoirSelection,
                kUpt04DimensionInitialReservoirSelection)));
    }
    gUpt04Reservoirs[work.surfaceIndex] = Upt03PackReservoir(reservoir);
}
