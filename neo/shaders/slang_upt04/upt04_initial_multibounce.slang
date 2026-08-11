import upt04_geometry_contract;
import upt04_geometry_decode;
import upt04_hit_contract;
import upt04_initial_bindings;
import upt04_initial_indirect;
import upt04_initial_work_contract;
import upt04_light_contract;
import upt04_material_contract;
import upt04_material_provider_contract;
import upt04_path_event_contract;
import upt04_random_contract;
import upt04_random_white_hash;
import upt04_replay_random;
import upt04_transport_initial_bounded;
import upt04_emissive_lookup;
import upt05_material_openpbr;

public struct Upt04SecondContinuationResult
{
    public Upt04InitialTraceWork work;
    public float localDirectionalPdf;
};

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
        && state.budget.visibilityRays <= 2u
        && state.budget.continuationRays + state.budget.visibilityRays <= 4u;
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
    const Upt04MaterialEmissionEvaluation emission =
        TMaterialSet.EvaluateEmissiveEndpoint(
            hitSurface.materialIndex,
            hitSurface.triangleClassAndFlags,
            hitSurface.texCoord,
            float2(0.0f),
            float2(0.0f));
    if (emission.valid == 0u)
        return Upt04InvalidateInitialWork(endpoint);

    float3 contribution = endpoint.pathThroughput * emission.radiance;
    uint reverseStatus = 0u;
    const float neePdf = any(emission.radiance > 0.0f)
        ? Upt04ExactNeePdfForEmissiveHit<TMaterialSet>(
            hit.instanceId,
            hit.primitiveId,
            secondarySurface.position,
            hit.barycentrics,
            reverseStatus)
        : 0.0f;
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
