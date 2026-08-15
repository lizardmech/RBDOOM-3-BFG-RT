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
#ifndef UPT_DIAGNOSTIC_LAMBERT
#define UPT_DIAGNOSTIC_LAMBERT 0
#endif

public bool Upt07CachedX2HitHasStableStaticLocator(Upt04HitFacts hit)
{
    if (hit.status != kUpt04HitTriangle || hit.geometryId != 0u
        || hit.instanceId > kUpt04ShaderInstanceMask)
        return false;
    if (hit.instanceId == 0u)
        return true;
    return Upt04StaticBucketRouteAvailable()
        && (hit.instanceId & kUpt04StaticBucketInstanceBit) != 0u
        && (hit.instanceId & ~kUpt04ShaderInstanceMask) == 0u;
}
#if UPT_DIAGNOSTIC_LAMBERT
import upt07_material_lambert_diagnostic;
#else
import upt04_material_primary_live;
#endif
import upt04_path_event_contract;
import upt04_primary_surface_live_contract;
import upt04_random_contract;
import upt04_replay_random;
import upt04_random_white_hash;
import upt04_transport_initial_bounded;
import upt07_emissive_material;
import upt07_indirect_material;
#ifndef UPT09_SPATIAL_INDIRECT_REPLAY
#define UPT09_SPATIAL_INDIRECT_REPLAY 0
#endif
#if UPT09_SPATIAL_INDIRECT_REPLAY
import upt09_spatial_direct_bindings;
#define gUpt07Control gUpt09Control
#define gUpt07Scene gUpt09Scene
#define Upt07LoadLight Upt09LoadLight
#else
import upt07_temporal_direct_bindings;
#endif
import upt07_temporal_reservoir;
import upt07_work_budget;

#ifndef UPT43_TEMPORAL_REPLAY_CLASSIFY
#define UPT43_TEMPORAL_REPLAY_CLASSIFY 0
#endif

#ifndef UPT_FROZEN_STATIC_SCENE
#define UPT_FROZEN_STATIC_SCENE 0
#endif


#ifndef UPT_FROZEN_FLAT_LIGHTS
#define UPT_FROZEN_FLAT_LIGHTS UPT_FROZEN_STATIC_SCENE
#endif

#ifndef UPT07_EARLY_RECONNECT
#define UPT07_EARLY_RECONNECT 0
#endif
#ifndef UPT07_BOTTLENECK_STAGE
#define UPT07_BOTTLENECK_STAGE 0
#endif
#ifndef UPT07_THREE_VERTEX_REPLAY
#define UPT07_THREE_VERTEX_REPLAY 0
#endif

#if UPT07_THREE_VERTEX_REPLAY
public bool Upt07ThreeVertexEndpointStructureValid(Upt03Candidate candidate)
{
    return candidate.eventKind == 4u
        && candidate.pathLength == 2u
        && candidate.reconnectionVertexLength == 1u
        && isfinite(candidate.proposalPdf)
        && isfinite(candidate.accumulatedRouletteProbability)
        && candidate.proposalPdf > 0.0f
        && candidate.proposalPdf <= 1.0f
        && candidate.accumulatedRouletteProbability > 0.0f
        && candidate.accumulatedRouletteProbability <= 1.0f
        && abs(candidate.proposalPdf
                - candidate.accumulatedRouletteProbability) <= 0.002f;
}
#endif

public static const uint kUpt07IndirectReplayFlag = 1u << 25u;
#if UPT07_EARLY_RECONNECT
public static const uint kUpt07EarlyReconnectFlag = 1u << 24u;
public static const uint kUpt07ReconnectDiagnosticsFlag = 1u << 23u;
public static const float kUpt07ReconnectMinimumRoughness = 0.2f;
public static const float kUpt07ReconnectFootprintScale = 0.0002f;

public static const uint kUpt07ReconnectDiagAttempted = 0u;
public static const uint kUpt07ReconnectDiagLocatorDecoded = 1u;
public static const uint kUpt07ReconnectDiagFootprintAccepted = 2u;
public static const uint kUpt07ReconnectDiagFootprintRejected = 3u;
public static const uint kUpt07ReconnectDiagVisibilityExecuted = 4u;
public static const uint kUpt07ReconnectDiagVisibilityPassed = 5u;
public static const uint kUpt07ReconnectDiagVisibilityFailed = 6u;
public static const uint kUpt07ReconnectDiagPositive = 7u;
public static const uint kUpt07ReconnectDiagZero = 8u;
public static const uint kUpt07ReconnectDiagReplayFallback = 9u;
public static const uint kUpt07ReconnectDiagNoStaticLocator = 10u;
public static const uint kUpt07ReconnectDiagInvalidLocator = 11u;
public static const uint kUpt07ReconnectDiagSourceEvaluationFailure = 12u;
public static const uint kUpt07ReconnectDiagTargetEvaluationFailure = 13u;
public static const uint kUpt07ReconnectDiagJacobianFailure = 14u;
public static const uint kUpt07ReconnectDiagHistorySelected = 15u;

public void Upt07ReconnectDiag(uint counter)
{
    if ((gUpt07Control.geometryAvailabilityFlags
            & kUpt07ReconnectDiagnosticsFlag) == 0u)
        return;
    uint ignored;
    InterlockedAdd(gUpt07ReconnectDiagnostics[counter], 1u, ignored);
}

#endif

// Transient T0 work only. Secondary-NEE replay reconstructs this request while
// the secondary vertex and selected light are live, but traversal is deferred
// until the temporal merge proves that the replayed history sample won.
public struct Upt07PendingIndirectVisibility
{
    public uint valid;
    public Upt04TraceRequest request;
    public Upt04VisibilityIgnore ignore;
};

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
        && (candidate.reconnectionVertexLength == 1u
            || candidate.reconnectionVertexLength == 2u);
#if UPT07_THREE_VERTEX_REPLAY
    const bool threeVertexEndpoint =
        Upt07ThreeVertexEndpointStructureValid(candidate);
    return endpoint || secondaryNee || threeVertexEndpoint;
#else
    return endpoint || secondaryNee;
#endif
}

public Upt04HitFacts Upt07TraceIndirect(
    Upt04TraceRequest request,
    uint visibility,
    Upt04VisibilityIgnore ignore,
    inout Upt07WorkCounters work)
{
    Upt04HitFacts result = Upt04MissFacts(request.tMax);
    RayDesc ray;
    ray.Origin = request.origin;
    ray.TMin = request.tMin;
    ray.Direction = request.direction;
    ray.TMax = request.tMax;
    RayQuery<0> query = RayQuery<0>();
    Upt07WorkAdd(work, kUpt07WorkRayQueries);
    const uint flags = visibility != 0u
        ? RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
        : RAY_FLAG_NONE;
    query.TraceRayInline(gUpt07Scene, flags, 0xffu, ray);
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
        if (visibility != 0u && Upt04VisibilityIgnoresCandidate(
                ignore,
                query.CandidateInstanceID(),
                candidatePrimitiveId))
            continue;
        Upt04HitFacts candidate = (Upt04HitFacts)0;
        candidate.status = kUpt04HitTriangle;
        candidate.instanceId = query.CandidateInstanceID();
        candidate.geometryId = 0u;
        candidate.primitiveId = candidatePrimitiveId;
        candidate.barycentrics = query.CandidateTriangleBarycentrics();
        candidate.hitT = query.CandidateTriangleRayT();
        candidate.frontFace = 1u;
        if (Upt07MaterialAlwaysTransmitsCandidate(candidate, work)
            || Upt07MaterialRejectsAlphaClipCandidate(candidate, work))
            continue;
        query.CommitNonOpaqueTriangleHit();
    }
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        result.status = kUpt04HitTriangle;
        result.instanceId = query.CommittedInstanceID();
        result.geometryId = 0u;
        if (!Upt04CanonicalizeHardwarePrimitive(
                result.instanceId,
                query.CommittedGeometryIndex(),
                query.CommittedPrimitiveIndex(),
                result.primitiveId))
            return Upt04MissFacts(request.tMax);
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
    out float conditionalIdentityPdf,
    inout Upt07WorkCounters work)
{
#if UPT_FROZEN_FLAT_LIGHTS
    denseLightIndex = kUpt04InvalidLightIndex;
    conditionalIdentityPdf = 0.0f;
    return false;
#else
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
        Upt07WorkAdd(work, kUpt07WorkEmissiveHashLoads);
        const Upt04EmissiveLookupEntry entry =
            gUpt04EmissiveLookup[(first + probe) & mask];
        if (entry.occupied == 0u)
            return false;
        if (entry.instanceId == instanceId
            && entry.primitiveIndex == primitiveIndex)
        {
            if (entry.denseLightIndex >= gUpt07Control.currentLightCount)
                return false;
            Upt07WorkAdd(work, kUpt07WorkLightRecordLoads);
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
#endif
}

public float Upt07ExactNeePdfForEmissiveHit(
    uint instanceId,
    uint primitiveIndex,
    float3 receiverPosition,
    float2 hitBarycentrics,
    uint proposalParity,
    inout Upt07WorkCounters work)
{
    uint denseLightIndex;
    float conditionalIdentityPdf;
    if (!Upt07FindEmissiveDenseLight(
            instanceId, primitiveIndex,
            denseLightIndex, conditionalIdentityPdf, work))
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
    Upt07WorkAdd(work, kUpt07WorkLightRecordLoads);
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
    out float selectionPdf,
    inout Upt07WorkCounters work)
{
    denseIndex = kUpt04InvalidLightIndex;
    lightType = kUpt04LightInvalid;
    selectionPdf = 0.0f;
    if ((source.eventKind != 1u && source.eventKind != 2u)
        || source.identity0 == 0u)
        return 0u;
#if UPT_FROZEN_FLAT_LIGHTS
    if (source.eventKind == 2u)
        return 0u;
#endif
    denseIndex = source.identity0 - 1u;
    if (sourceIsHistory != 0u)
    {
#if UPT09_SPATIAL_INDIRECT_REPLAY
        // Spatial donors and receivers belong to the same current-frame light
        // universe. A previous-frame identity is never legal on this route.
        return 0u;
#else
#if UPT_FROZEN_FLAT_LIGHTS
        // Frozen light payload indices are invariant for the lifetime of the
        // captured scene; no previous/current identity translation exists.
#else
        if (denseIndex >= gUpt07Control.previousToCurrentLightCount)
            return 0u;
        Upt07WorkAdd(work, kUpt07WorkStableRemapLoads);
        denseIndex = gUpt07PreviousToCurrentLights[denseIndex];
        if (denseIndex == 0xffffffffu)
            return 0u;
#endif
#endif
    }
    if (denseIndex >= gUpt07Control.currentLightCount)
        return 0u;
    Upt07WorkAdd(work, kUpt07WorkLightRecordLoads);
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
        || (!Upt03HasCachedX2Endpoint(source)
            && source.identity1 != Upt04StableLightIdentityFingerprint(light)))
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
        // The current light payload is built from the same exact CDF and
        // publishes its conditional identity probability in sourcePdf. Dense
        // remap, range/type checks and the stable fingerprint above already
        // prove this record is the stored light. Hashing instance/primitive
        // again only to rediscover the same dense index adds up to sixteen
        // serialized global reads for every secondary-emissive replay.
        const float conditionalIdentityPdf = light.sourcePdf;
        if (!Upt04FinitePositive(conditionalIdentityPdf))
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
        float replayPdfScale;
        const bool persistedReplayPdf = Upt04DecodeEmissiveReplayPdfScale(
            source.proposalPdf, replayPdfScale);
#if UPT09_SPATIAL_INDIRECT_REPLAY
        // Same-frame S0 consumes D0/T0 reservoirs published by this exact
        // contract. Fail closed on an untagged transition record so the
        // spatial specialization contains no four-probe texture fallback.
        if (!persistedReplayPdf)
            return 0u;
#else
        if (!persistedReplayPdf)
        {
            const float4 regionWeights = UptDirectEmissiveRegionWeights(
                replay,
                gUpt07Control.materialCount,
                gUpt07Control.logicalTextureCount,
                gUpt07Control.emissiveScale);
            const uint region = Upt04EmissiveRegionFromBarycentrics(
                replay.barycentrics);
            replayPdfScale = 4.0f * Upt04EmissiveRegionSelectionPdf(
                regionWeights, region);
        }
#endif
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
        sample.solidAnglePdf = replay.solidAnglePdf * replayPdfScale;
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

// RTXDI-shaped common x2 mapping. The reservoir already owns the exact
// reconnectible endpoint and its outgoing suffix radiance, so mapping a new
// primary receiver requires no continuation trace and no x2 geometry/material
// reconstruction. Both visibility segments are deferred until this mapped
// proposal wins the reservoir update.
public uint Upt07TryCachedX2Reconnect(
    Upt03Candidate source,
    Upt04PrimarySurfaceRecord sourceReceiver,
    Upt04PrimarySurfaceRecord targetReceiver,
    uint sourceIsHistory,
    uint storedDenseIndex,
    uint storedLightType,
    out Upt03Candidate shifted,
    out float shiftJacobian,
    out Upt07PendingIndirectVisibility pendingVisibility,
    inout Upt07WorkCounters work)
{
    Upt07WorkAddRoute(work, kUpt07WorkRouteCachedX2Attempted);
    shifted = (Upt03Candidate)0;
    shiftJacobian = 1.0f;
    pendingVisibility = (Upt07PendingIndirectVisibility)0;
    pendingVisibility.ignore = Upt04NoVisibilityIgnore();
    if (!Upt03CachedX2EndpointValid(source)
        || !Upt04PrimarySurfaceRecordValid(sourceReceiver)
        || !Upt04PrimarySurfaceRecordValid(targetReceiver)
        || storedDenseIndex >= gUpt07Control.currentLightCount)
        return 0u;

    Upt07WorkAdd(work, kUpt07WorkLightRecordLoads);
    const Upt04UnifiedLightRecord storedLight = Upt07LoadLight(storedDenseIndex);
    if (storedLight.type != storedLightType)
        return 0u;
    if (sourceIsHistory != 0u)
    {
        if (storedLightType == kUpt04LightDoomAnalytic)
        {
            if ((storedLight.flags & kUpt04LightStableCacheable) == 0u)
                return 0u;
        }
        else if ((storedLight.flags & kUpt04EmissiveHistoryDynamic) != 0u)
        {
            return 0u;
        }
    }

    const float3 x2Position = source.reconnectionPosition;
    const float3 x2Normal = Upt04DecodeVertexOctahedral(
        source.reconnectionNormalPacked);
    const float3 suffixRadiance = float3(
        source.contributionR,
        source.contributionG,
        source.contributionB);
    const float3 sourceDelta = x2Position
        - sourceReceiver.worldPositionAndViewDepth.xyz;
    const float3 targetDelta = x2Position
        - targetReceiver.worldPositionAndViewDepth.xyz;
    const float sourceDistanceSquared = dot(sourceDelta, sourceDelta);
    const float targetDistanceSquared = dot(targetDelta, targetDelta);
    if (!Upt04FinitePositive(sourceDistanceSquared)
        || !Upt04FinitePositive(targetDistanceSquared))
        return 0u;
    const float sourceDistance = sqrt(sourceDistanceSquared);
    const float targetDistance = sqrt(targetDistanceSquared);
    const float3 sourceDirection = sourceDelta / sourceDistance;
    const float3 targetDirection = targetDelta / targetDistance;
    const float sourceCosine = saturate(dot(x2Normal, -sourceDirection));
    const float targetCosine = saturate(dot(x2Normal, -targetDirection));
    if (!Upt04FinitePositive(sourceCosine)
        || !Upt04FinitePositive(targetCosine))
        return 0u;

    const Upt04MaterialEvaluation targetPrimary =
        Upt04EvaluatePrimaryDirectProbe(
            targetReceiver, targetDirection, suffixRadiance);
    if (targetPrimary.valid == 0u
        || !Upt04FinitePositive(targetPrimary.directionalPdf))
        return 0u;
    const float3 contribution = targetPrimary.weightedContribution
        / targetPrimary.directionalPdf;
    const float target = max(contribution.x,
        max(contribution.y, contribution.z));
    shiftJacobian = (targetCosine / targetDistanceSquared)
        * (sourceDistanceSquared / sourceCosine);
    if (!all(isfinite(contribution))
        || !Upt04FinitePositive(target)
        || !Upt04FinitePositive(shiftJacobian))
        return 0u;

    if (!Upt04BuildPrimaryVisibilityRequest(
            targetReceiver.worldPositionAndViewDepth.xyz,
            targetReceiver.geometricNormalAndRoughness.xyz,
            targetReceiver.shadingNormalAndOpacity.xyz,
            targetDirection,
            targetDistance,
            pendingVisibility.request))
        return 0u;

    pendingVisibility.valid = 1u;

    shifted = source;
    shifted.status = kUpt04SampleValidPositive;
    shifted.flags &= ~(kUpt04CandidateFlagVisibilityKnown
        | kUpt04CandidateFlagVisibilityPassed);
    shifted.age = 0u;
    shifted.identity0 = storedDenseIndex + 1u;
    shifted.lobeMask |= targetPrimary.lobeMask;
    shifted.target = target;
    Upt07WorkAddRoute(work, kUpt07WorkRouteCachedX2Admitted);
    return 1u;
}

public uint Upt07ReplayIndirectSample(
    Upt03Candidate source,
    Upt04PrimarySurfaceRecord sourceReceiver,
    Upt04PrimarySurfaceRecord receiver,
    uint sourceIsHistory,
    out uint failureReason,
    out Upt03Candidate shifted,
    out Upt07PendingIndirectVisibility pendingVisibility,
    out float shiftJacobian,
    inout Upt07WorkCounters work)
{
    failureReason = kUpt07RejectIndirectStructure;
    shifted = (Upt03Candidate)0;
    pendingVisibility = (Upt07PendingIndirectVisibility)0;
    pendingVisibility.ignore = Upt04NoVisibilityIgnore();
    shiftJacobian = 1.0f;
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
        && (source.reconnectionVertexLength == 1u
            || source.reconnectionVertexLength == 2u);
#if UPT07_THREE_VERTEX_REPLAY
    const bool threeVertexEndpoint =
        Upt07ThreeVertexEndpointStructureValid(source);
    if (!endpoint && !secondaryNee && !threeVertexEndpoint)
        return 0u;
#else
    if (!endpoint && !secondaryNee)
        return 0u;
#endif
    uint storedDenseIndex = kUpt04InvalidLightIndex;
    uint storedLightType = kUpt04LightInvalid;
    float storedSelectionPdf = 0.0f;
    if (secondaryNee && Upt07ResolveStoredSecondaryLight(
            source,
            sourceIsHistory,
            storedDenseIndex,
            storedLightType,
            storedSelectionPdf,
            work) == 0u)
    {
        failureReason = kUpt07RejectIndirectIdentity;
        return 0u;
    }

    if (secondaryNee && source.reconnectionVertexLength == 2u)
    {
        const uint cachedResult = Upt07TryCachedX2Reconnect(
            source,
            sourceReceiver,
            receiver,
            sourceIsHistory,
            storedDenseIndex,
            storedLightType,
            shifted,
            shiftJacobian,
            pendingVisibility,
            work);
        if (cachedResult != 0u)
        {
            failureReason = 0u;
            return 1u;
        }
        Upt07WorkAddRoute(work, kUpt07WorkRouteCachedX2Fallback);
    }

#if UPT43_TEMPORAL_REPLAY_CLASSIFY
    // S43.4 classifier boundary. Direct and cached-x2 mappings have already
    // completed. Do not let the full-screen specialization retain any of the
    // continuation-trace or endpoint-reconstruction closure below.
    failureReason = kUpt07NeedsCompactReplay;
    return 0u;
#else
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

#if UPT07_BOTTLENECK_STAGE == 10
    if (sourceIsHistory == 0u)
    {
        // Price reciprocal identity resolution, random continuation replay and
        // exact trace-request construction without admitting the second ray.
        // Fold every request/path input into the observable scalar so this
        // cannot collapse to the stage-8 no-reciprocal specialization.
        const float requestDigest =
            abs(dot(continuationRay.origin,
                float3(0.131f, 0.173f, 0.197f)))
            + abs(dot(continuationRay.direction,
                float3(0.211f, 0.233f, 0.257f)))
            + abs(continuationRay.tMin) * 0.277f
            + abs(continuationRay.tMax) * 0.293f
            + abs(dot(continuation.throughput,
                float3(0.313f, 0.331f, 0.347f)))
            + abs(continuation.pdf) * 0.367f
            + float(continuation.lobeMask & 0xffu) * 1.0e-4f
            + float(storedDenseIndex & 0xffffu) * 1.0e-5f
            + float(storedLightType & 0xffu) * 1.0e-4f
            + abs(storedSelectionPdf) * 0.389f;
        shifted.status = kUpt04SampleValidPositive;
        shifted.eventKind = source.eventKind;
        shifted.target = requestDigest + 1.0e-6f;
        failureReason = 0u;
        return 1u;
    }
#endif

    failureReason = kUpt07RejectIndirectGeometry;
    const Upt04HitFacts hit = Upt07TraceIndirect(
        continuationRay, 0u, Upt04NoVisibilityIgnore(), work);
    if (hit.status != kUpt04HitTriangle || hit.instanceId == 0xffffffffu)
        return 0u;

#if UPT07_BOTTLENECK_STAGE == 11
    if (sourceIsHistory == 0u)
    {
        // Retain every committed-hit fact while stopping before vertex/index,
        // material and interpolant loads in Upt04DecodeGeometryHit.
        const float committedHitDigest =
            abs(hit.hitT) * 0.131f
            + abs(dot(hit.barycentrics, float2(0.173f, 0.197f)))
            + float(hit.instanceId & 0xffffu) * 1.0e-5f
            + float(hit.geometryId & 0xffffu) * 1.0e-6f
            + float(hit.primitiveId & 0xffffu) * 1.0e-6f
            + float(hit.frontFace & 1u) * 1.0e-4f;
        shifted.status = kUpt04SampleValidPositive;
        shifted.eventKind = source.eventKind;
        shifted.identity0 = hit.instanceId + 1u;
        shifted.identity1 = hit.primitiveId + 1u;
        shifted.sampleCoord0 = hit.barycentrics.x;
        shifted.sampleCoord1 = hit.barycentrics.y;
        shifted.target = committedHitDigest + 1.0e-6f;
        failureReason = 0u;
        return 1u;
    }
#endif

    Upt07WorkAdd(work, kUpt07WorkGeometryResolves);
    const Upt04GeometryHitSurface hitSurface = Upt04DecodeGeometryHit(hit);
    if (hitSurface.valid == 0u)
        return 0u;

#if UPT07_BOTTLENECK_STAGE == 9
    if (sourceIsHistory == 0u)
    {
        // Stage 9 prices reciprocal continuation generation, traversal and
        // committed-hit decoding without retaining endpoint/light/material
        // evaluation. Fold every decoded surface field into the observable
        // target so the compiler cannot replace this with a hit-only query.
        const float decodedSurfaceDigest =
            abs(dot(hitSurface.position, float3(0.131f, 0.173f, 0.197f)))
            + abs(dot(hitSurface.geometricNormal,
                float3(0.211f, 0.233f, 0.257f)))
            + abs(dot(hitSurface.shadingNormal,
                float3(0.277f, 0.293f, 0.313f)))
            + abs(dot(hitSurface.tangent, float3(0.331f, 0.347f, 0.367f)))
            + abs(dot(hitSurface.bitangent, float3(0.389f, 0.409f, 0.431f)))
            + abs(dot(hitSurface.texCoord, float2(0.449f, 0.467f)))
            + abs(dot(hitSurface.normalTexCoord, float2(0.487f, 0.503f)))
            + abs(dot(hitSurface.vertexColor,
                float4(0.521f, 0.541f, 0.557f, 0.577f)))
            + abs(dot(hitSurface.vertexColorAdd,
                float4(0.599f, 0.617f, 0.641f, 0.659f)))
            + float(hitSurface.materialIndex & 0xffffu) * 1.0e-5f
            + float(hitSurface.triangleClassAndFlags & 0xffffu) * 1.0e-6f
            + float(hitSurface.frontFace & 1u) * 1.0e-4f;
        shifted.status = kUpt04SampleValidPositive;
        shifted.eventKind = source.eventKind;
        shifted.identity0 = hit.instanceId + 1u;
        shifted.identity1 = hit.primitiveId + 1u;
        shifted.sampleCoord0 = hit.barycentrics.x;
        shifted.sampleCoord1 = hit.barycentrics.y;
        shifted.target = decodedSurfaceDigest + 1.0e-6f;
        failureReason = 0u;
        return 1u;
    }
#endif

    shifted.status = kUpt04SampleValidPositive;
    shifted.eventKind = source.eventKind;
    shifted.lobeMask = continuation.lobeMask;
    shifted.flags = kUpt04CandidateFlagGlobal
        | kUpt04CandidateFlagReplayable
        | kUpt04CandidateFlagNeedsShift
        | (source.flags & kUpt04CandidateFlagDirectProposalParity);
    shifted.replayKey = source.replayKey & 0x7fffffffu;
    shifted.replayIndex = source.replayIndex;
    shifted.generationFingerprint = kUpt03ExpectedGeneration;
    shifted.proposalPdf = source.proposalPdf;
    shifted.partialJacobian = 1.0f;
    shifted.accumulatedRouletteProbability = 1.0f;

#if UPT07_THREE_VERTEX_REPLAY
    if (threeVertexEndpoint)
    {
        failureReason = kUpt07RejectIndirectContinuation;
        // D0 persists the exact clamped probability in both proposalPdf and
        // accumulatedRouletteProbability. Replay consumes that per-sample q;
        // it never re-reads the live cvar and never reapplies the biased
        // minimum-throughput cutoff.
        const float q = clamp(source.proposalPdf, 0.0f, 1.0f);
        const float rouletteRandom = Upt04WhiteHash(
            Upt04PersistedReplayRandomKey(
                source.replayKey,
                source.replayIndex,
                1u,
                0u,
                kUpt04StreamIndirectRouletteSurvival,
                kUpt04DimensionIndirectRouletteSurvival));
        // A stored path-length-two endpoint exists only on the survival
        // branch.  Any disagreement means the persisted payload is stale or
        // incompatible with this specialization and must fail closed.
        if (rouletteRandom >= q)
            return 0u;

        const Upt04PreparedDirectMaterial preparedSecondary =
            Upt07PrepareSecondaryDirect(hitSurface, -continuation.direction);
        if (preparedSecondary.valid == 0u)
            return 0u;
        const Upt04PathEvent secondContinuation =
            Upt07SampleSecondaryContinuation(
                preparedSecondary,
                Upt04WhiteHash(Upt04PersistedReplayRandomKey(
                    source.replayKey,
                    source.replayIndex,
                    1u,
                    0u,
                    kUpt04StreamMaterialLobeSelection,
                    kUpt04DimensionMaterialLobeSelection)),
                float2(
                    Upt04WhiteHash(Upt04PersistedReplayRandomKey(
                        source.replayKey,
                        source.replayIndex,
                        1u,
                        0u,
                        kUpt04StreamIndirectContinuationU,
                        kUpt04DimensionIndirectContinuationU)),
                    Upt04WhiteHash(Upt04PersistedReplayRandomKey(
                        source.replayKey,
                        source.replayIndex,
                        1u,
                        0u,
                        kUpt04StreamIndirectContinuationV,
                        kUpt04DimensionIndirectContinuationV))));
        if (secondContinuation.valid == 0u
            || !Upt04FinitePositive(secondContinuation.pdf)
            || !all(isfinite(secondContinuation.throughput)))
            return 0u;

        Upt04TraceRequest secondRay;
        if (!Upt04BuildContinuationRequest(
                hitSurface.position,
                hitSurface.geometricNormal,
                hitSurface.shadingNormal,
                secondContinuation.direction,
                secondRay))
            return 0u;
        failureReason = kUpt07RejectIndirectGeometry;
        const Upt04HitFacts secondHit = Upt07TraceIndirect(
            secondRay, 0u, Upt04NoVisibilityIgnore(), work);
        if (secondHit.status != kUpt04HitTriangle
            || secondHit.instanceId == 0xffffffffu)
            return 0u;
        Upt07WorkAdd(work, kUpt07WorkGeometryResolves);
        const Upt04GeometryHitSurface endpointSurface =
            Upt04DecodeGeometryHit(secondHit);
        if (endpointSurface.valid == 0u)
            return 0u;

        failureReason = kUpt07RejectIndirectEndpoint;
        const Upt04MaterialEmissionEvaluation emission =
            Upt07EvaluateHitEmission(
                endpointSurface, secondContinuation.direction);
        if (emission.valid == 0u)
            return 0u;
        float3 contribution = continuation.throughput
            * (secondContinuation.throughput / q)
            * emission.radiance;
        const float neePdf = any(emission.radiance > 0.0f)
            ? Upt07ExactNeePdfForEmissiveHit(
                secondHit.instanceId,
                secondHit.primitiveId,
                hitSurface.position,
                secondHit.barycentrics,
                (source.flags & kUpt04CandidateFlagDirectProposalParity) != 0u
                    ? 1u : 0u,
                work)
            : 0.0f;
        contribution *= Upt04BalanceHeuristic(
            secondContinuation.pdf, neePdf);
        const float rawTarget = max(contribution.x,
            max(contribution.y, contribution.z));
        const float target = rawTarget * q;
        const float pathPdf = continuation.pdf
            * secondContinuation.pdf * q;
        if (!all(isfinite(contribution)) || !isfinite(target)
            || target < 0.0f || !Upt04FinitePositive(pathPdf))
            return 0u;

        shifted.eventKind = 4u;
        shifted.lobeMask = continuation.lobeMask
            | secondContinuation.lobeMask;
        shifted.identity0 = secondHit.instanceId + 1u;
        shifted.identity1 = secondHit.primitiveId + 1u;
        shifted.sampleCoord0 = secondHit.barycentrics.x;
        shifted.sampleCoord1 = secondHit.barycentrics.y;
        shifted.pathLength = 2u;
        shifted.reconnectionVertexLength = 1u;
        shifted.pathPdf = pathPdf;
        shifted.proposalPdf = q;
        shifted.accumulatedRouletteProbability = q;
        shifted.flags |= kUpt04CandidateFlagVisibilityKnown
            | kUpt04CandidateFlagVisibilityPassed;
        shifted.contributionR = contribution.x;
        shifted.contributionG = contribution.y;
        shifted.contributionB = contribution.z;
        shifted.target = target;
        if (!Upt04FinitePositive(target))
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
#endif

    if (endpoint)
    {
        failureReason = kUpt07RejectIndirectEndpoint;
        const Upt04MaterialEmissionEvaluation emission =
            Upt07EvaluateHitEmission(hitSurface, continuation.direction);
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
                    ? 1u : 0u,
                work)
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
        // The continuation query reaches the sampled endpoint itself, so it
        // is also the complete current-frame visibility proof for this event.
        shifted.flags |= kUpt04CandidateFlagVisibilityKnown
            | kUpt04CandidateFlagVisibilityPassed;
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
    Upt07WorkAdd(work, kUpt07WorkLightRecordLoads);
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
    Upt07WorkAdd(work, kUpt07WorkMaterialRecordLoads);
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
    if (!Upt04BuildPrimaryVisibilityRequest(
            hitSurface.position,
            hitSurface.geometricNormal,
            hitSurface.shadingNormal,
            selectedSample.direction,
            selectedSample.distance,
            pendingVisibility.request))
        return 0u;
    if (storedLightType == kUpt04LightEmissiveTriangle)
        pendingVisibility.ignore = Upt04MakeVisibilityIgnore(
            storedLight.instanceId, storedLight.primitiveIndex);
    const uint storedFingerprint =
        Upt04StableLightIdentityFingerprint(storedLight);
    pendingVisibility.valid = 1u;
    shifted.eventKind = storedLightType == kUpt04LightEmissiveTriangle
        ? 2u : 1u;
    shifted.lobeMask = continuation.lobeMask | selectedMaterial.lobeMask;
    shifted.identity0 = storedDenseIndex + 1u;
    shifted.identity1 = storedFingerprint;
    shifted.sampleCoord0 = source.sampleCoord0;
    shifted.sampleCoord1 = source.sampleCoord1;
    shifted.pathLength = 2u;
    if (Upt07CachedX2HitHasStableStaticLocator(hit))
    {
        shifted.reconnectionVertexLength = 2u;
        shifted.reconnectionPosition = hitSurface.position;
        shifted.reconnectionNormalPacked = Upt04EncodeVertexOctahedral(
            hitSurface.geometricNormal);
        shifted.contributionR = selectedMaterial.weightedContribution.x;
        shifted.contributionG = selectedMaterial.weightedContribution.y;
        shifted.contributionB = selectedMaterial.weightedContribution.z;
        shifted.identity1 = shifted.reconnectionNormalPacked;
    }
    else
    {
        shifted.reconnectionVertexLength = 1u;
    }
    shifted.pathPdf = pathPdf;
    if (shifted.reconnectionVertexLength != 2u)
    {
        shifted.contributionR = contribution.x;
        shifted.contributionG = contribution.y;
        shifted.contributionB = contribution.z;
    }
    shifted.target = target;
    failureReason = 0u;
    return 1u;
#endif
}
