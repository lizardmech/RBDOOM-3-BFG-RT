import upt04_initial_random;
import upt04_initial_bindings;
import upt04_initial_control;
import upt04_light_contract;
import upt04_local_light_proposal;
import upt04_random_contract;
import upt04_random_white_hash;
import upt04_replay_random;

// Shared current-frame emissive identity selector. The publication's logical
// count is authoritative because the structured allocation retains a stale
// capacity tail across frames.
public Upt04LocalLightSelection Upt04SelectInitialWeightedEmissive(
    Upt04LocalLightRanges ranges,
    float identityRandom,
    float2 surfaceRandom,
    uint emissiveTrials,
    uint totalTrials)
{
    Upt04LocalLightSelection result = (Upt04LocalLightSelection)0;
    if (ranges.emissiveCount == 0u || emissiveTrials == 0u
        || totalTrials == 0u || !isfinite(identityRandom)
        || identityRandom < 0.0f || identityRandom >= 1.0f)
        return result;

    if (!Upt04EmissiveDistributionValid(
            gUpt04Control.emissiveDistributionCountAndValid))
        return result;
    const uint distributionCount = Upt04EmissiveDistributionCount(
        gUpt04Control.emissiveDistributionCountAndValid);
    uint distributionCapacity = 0u;
    uint distributionStride = 0u;
    gUpt04EmissiveDistribution.GetDimensions(
        distributionCapacity, distributionStride);
    if (distributionCount == 0u || distributionCount > distributionCapacity)
        return result;
    uint low = 0u;
    uint high = distributionCount;
    [loop]
    while (low < high)
    {
        const uint mid = low + ((high - low) >> 1u);
        const Upt04EmissiveDistributionEntry entry =
            gUpt04EmissiveDistribution[mid];
        if (identityRandom <= saturate(entry.cumulativePdf))
            high = mid;
        else
            low = mid + 1u;
    }
    if (low >= distributionCount)
        return result;

    const Upt04EmissiveDistributionEntry entry =
        gUpt04EmissiveDistribution[low];
    const float previousCdf = low != 0u
        ? saturate(gUpt04EmissiveDistribution[low - 1u].cumulativePdf)
        : 0.0f;
    const float currentCdf = saturate(entry.cumulativePdf);
    const float conditionalPdf = currentCdf - previousCdf;
    const float domainMixture = float(emissiveTrials) / float(totalTrials);
    if (entry.emissiveTriangleIndex == kUpt04InvalidLightIndex
        || entry.denseLightIndex == kUpt04InvalidLightIndex
        || entry.denseLightIndex < ranges.emissiveStart
        || entry.denseLightIndex - ranges.emissiveStart >= ranges.emissiveCount
        || !Upt04FinitePositive(conditionalPdf)
        || !Upt04FinitePositive(domainMixture))
        return result;

    result.valid = 1u;
    result.denseIndex = entry.denseLightIndex;
    result.type = kUpt04LightEmissiveTriangle;
    result.requiresTriangleReplay = 1u;
    result.selectionPdf = conditionalPdf * domainMixture;
    result.sampleCoordinates = surfaceRandom;
    result.reserved = entry.emissiveTriangleIndex + 1u;
    return result;
}

// Draw one candidate from the same bounded typed schedule used by accepted D0,
// but keep the path vertex and candidate ordinal in their separate random-key
// fields. Secondary NEE streams this complete schedule through an inner RIS
// and traces visibility only for its winner.
public Upt04LocalLightSelection Upt04SelectInitialLightFromRandoms(
    uint candidateOrdinal,
    float identityRandom,
    float2 surfaceRandom)
{
    Upt04LocalLightSelection result = (Upt04LocalLightSelection)0;
    Upt04LocalLightRanges ranges;
    ranges.emissiveStart = gUpt04Control.emissiveRangeStart;
    ranges.emissiveCount = gUpt04Control.emissiveRangeCount;
    ranges.analyticStart = gUpt04Control.analyticRangeStart;
    ranges.analyticCount = gUpt04Control.analyticRangeCount;
    const uint proposalParity =
        (gUpt04Control.availabilityFlags & kUpt04DirectProposalParity) != 0u
            ? 1u : 0u;
    const uint configuredEmissiveTrials = Upt04ConfiguredEmissiveTrialCount(
        gUpt04Control.availabilityFlags);
    const uint candidateCount = Upt04DirectRisCandidateCount(
        ranges, proposalParity, configuredEmissiveTrials);
    if (candidateOrdinal >= candidateCount)
        return result;

    const uint emissiveTrials = Upt04DirectRisDomainTrialCount(
        ranges, kUpt04LightEmissiveTriangle, proposalParity,
        configuredEmissiveTrials);
    const bool selectEmissive = candidateOrdinal < emissiveTrials;
    const uint selectedType = selectEmissive
        ? kUpt04LightEmissiveTriangle : kUpt04LightDoomAnalytic;
    Upt04LocalLightRanges typedRanges = (Upt04LocalLightRanges)0;
    if (selectEmissive)
    {
        typedRanges.emissiveStart = ranges.emissiveStart;
        typedRanges.emissiveCount = ranges.emissiveCount;
    }
    else
    {
        typedRanges.analyticStart = ranges.analyticStart;
        typedRanges.analyticCount = ranges.analyticCount;
    }
    if (proposalParity != 0u && !selectEmissive)
    {
        const uint analyticOrdinal = candidateOrdinal - emissiveTrials;
        const uint analyticTrials = Upt04DirectRisDomainTrialCount(
            ranges, kUpt04LightDoomAnalytic, proposalParity,
            configuredEmissiveTrials);
        const float stride = max(
            1.0f, float(ranges.analyticCount) / float(max(analyticTrials, 1u)));
        const float lightIndexInRange = min(
            (float(analyticOrdinal) + identityRandom) * stride,
            float(max(ranges.analyticCount, 1u) - 1u));
        identityRandom = (lightIndexInRange + 0.5f)
            / float(max(ranges.analyticCount, 1u));
    }
    if (!isfinite(identityRandom) || identityRandom < 0.0f
        || identityRandom >= 1.0f || !all(isfinite(surfaceRandom))
        || any(surfaceRandom < 0.0f) || any(surfaceRandom >= 1.0f))
        return result;

    if (proposalParity != 0u && selectEmissive)
    {
        result = Upt04SelectInitialWeightedEmissive(
            ranges,
            identityRandom,
            surfaceRandom,
            emissiveTrials,
            candidateCount);
        if (result.valid != 0u)
            return result;
    }

    result = Upt04SelectUniformLocalLight(
        typedRanges, identityRandom, surfaceRandom);
    result.selectionPdf = Upt04DirectRisIdentityPdf(
        ranges, selectedType, proposalParity, configuredEmissiveTrials);
    return result;
}

public Upt04LocalLightSelection Upt04SelectInitialLightAtVertex(
    uint2 pixel,
    uint pathVertex,
    uint candidateOrdinal)
{
    float identityRandom = Upt04WhiteHash(
        Upt04InitialRandomKeyAtVertexSample(
            pixel,
            pathVertex,
            candidateOrdinal,
            kUpt04StreamInitialLocalLightIdentity,
            kUpt04DimensionInitialLocalLightIdentity));
    const float2 surfaceRandom = float2(
        Upt04WhiteHash(Upt04InitialRandomKeyAtVertexSample(
            pixel,
            pathVertex,
            candidateOrdinal,
            kUpt04StreamInitialLocalLightSurfaceU,
            kUpt04DimensionInitialLocalLightSurfaceU)),
        Upt04WhiteHash(Upt04InitialRandomKeyAtVertexSample(
            pixel,
            pathVertex,
            candidateOrdinal,
            kUpt04StreamInitialLocalLightSurfaceV,
            kUpt04DimensionInitialLocalLightSurfaceV)));
    return Upt04SelectInitialLightFromRandoms(
        candidateOrdinal, identityRandom, surfaceRandom);
}

public Upt04LocalLightSelection Upt04SelectInitialLightFromReplay(
    uint replayKey,
    uint replayIndex,
    uint pathVertex,
    uint candidateOrdinal)
{
    const float identityRandom = Upt04WhiteHash(
        Upt04PersistedReplayRandomKey(
            replayKey,
            replayIndex,
            pathVertex,
            candidateOrdinal,
            kUpt04StreamInitialLocalLightIdentity,
            kUpt04DimensionInitialLocalLightIdentity));
    const float2 surfaceRandom = float2(
        Upt04WhiteHash(Upt04PersistedReplayRandomKey(
            replayKey,
            replayIndex,
            pathVertex,
            candidateOrdinal,
            kUpt04StreamInitialLocalLightSurfaceU,
            kUpt04DimensionInitialLocalLightSurfaceU)),
        Upt04WhiteHash(Upt04PersistedReplayRandomKey(
            replayKey,
            replayIndex,
            pathVertex,
            candidateOrdinal,
            kUpt04StreamInitialLocalLightSurfaceV,
            kUpt04DimensionInitialLocalLightSurfaceV)));
    return Upt04SelectInitialLightFromRandoms(
        candidateOrdinal, identityRandom, surfaceRandom);
}
