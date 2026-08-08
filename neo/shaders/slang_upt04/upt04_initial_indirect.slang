import upt04_initial_work_contract;
import upt04_initial_random;
import upt04_initial_bindings;
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
import upt04_transport_initial_bounded;
import upt04_emissive_lookup;

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
    const Upt04PathEvent continuation = TMaterialSet.SamplePrimaryContinuation(
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
    {
        result.diagnostics |= kUpt04IndirectDiagSecondarySelectionFailure;
        return result;
    }
    const Upt04UnifiedLightRecord light = Upt04LoadCurrentLightRecord(selection.denseIndex);
    const Upt04EvaluatedLocalLight evaluated =
        Upt04EvaluateSelectedLocalLight<TMaterialSet>(
            selection,
            light,
            hitSurface.position,
            gUpt04Control.availabilityFlags,
            gUpt04Control.emissiveScale);
    if (evaluated.valid == 0u)
    {
        result.diagnostics |= kUpt04IndirectDiagSecondarySelectionFailure;
        return result;
    }
    const Upt04MaterialEvaluation material = TMaterialSet.EvaluateSecondaryDirect(
        hitSurface,
        -work.ray.direction,
        evaluated.sample.direction,
        evaluated.sample.radiance);
    if (material.valid == 0u)
    {
        result.diagnostics |= kUpt04IndirectDiagSecondaryMaterialFailure;
        return result;
    }

    const float neePdf = selection.selectionPdf * evaluated.sample.solidAnglePdf;
    const float pathPdf = work.pathPdf * neePdf;
    if (!Upt04FinitePositive(neePdf) || !Upt04FinitePositive(pathPdf))
    {
        result.diagnostics |= kUpt04IndirectDiagSecondaryPdfRayFailure;
        return result;
    }

    Upt04InitialTraceWork nee = work;
    nee.eventKind = selection.type == kUpt04LightEmissiveTriangle ? 2u : 1u;
    nee.lightIndex = selection.denseIndex;
    nee.lightType = selection.type;
    nee.identity0 = selection.denseIndex + 1u;
    if (nee.identity0 == 0u)
        return result;
    nee.identity1 = Upt04StableLightIdentityFingerprint(light);
    nee.sampleCoordinates = evaluated.storedSampleCoordinates;
    nee.pathLength = 2u;
    nee.reconnectionVertexLength = 1u;
    nee.proposalPdf = 1.0f;
    nee.pathPdf = pathPdf;
    nee.lobeMask = work.lobeMask | material.lobeMask;
    // pathThroughput already carries the primary BSDF sampling inverse. The
    // secondary light sample is a one-sample NEE estimator in this bounded
    // slice, so fold its inverse density here before the outer path reservoir.
    nee.contribution = nee.pathThroughput
        * material.weightedContribution / neePdf;
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
    {
        result.diagnostics |= kUpt04IndirectDiagSecondaryPdfRayFailure;
        return result;
    }
    if (!Upt04BuildPrimaryVisibilityRequest(
            hitSurface.position,
            hitSurface.geometricNormal,
            hitSurface.shadingNormal,
            evaluated.sample.direction,
            evaluated.sample.distance,
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
        Upt04LoadCurrentLightRecord(work.lightIndex);
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
