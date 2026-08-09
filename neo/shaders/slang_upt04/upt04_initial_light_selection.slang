import upt04_initial_random;
import upt04_initial_bindings;
import upt04_initial_control;
import upt04_light_contract;
import upt04_local_light_proposal;
import upt04_random_contract;
import upt04_random_white_hash;
import upt04_replay_random;

// Immutable per-receiver schedule.  Keep control-buffer loads and domain-count
// arithmetic out of the candidate loop while preserving the exact proposal
// ordering and PDFs used by the accepted parity paths.
public struct Upt04InitialLightSchedule
{
    public Upt04LocalLightRanges ranges;
    public uint proposalParity;
    public uint configuredEmissiveTrials;
    public uint emissiveTrials;
    public uint analyticTrials;
    public uint candidateCount;
};

public Upt04InitialLightSchedule Upt04MakeInitialLightSchedule(
    Upt04LocalLightRanges ranges)
{
    Upt04InitialLightSchedule schedule;
    schedule.ranges = ranges;
    schedule.proposalParity = Upt04DirectProposalParityValue(
        gUpt04Control.availabilityFlags);
    schedule.configuredEmissiveTrials = Upt04ConfiguredEmissiveTrialCount(
        gUpt04Control.availabilityFlags);
    schedule.emissiveTrials = Upt04DirectRisDomainTrialCount(
        ranges,
        kUpt04LightEmissiveTriangle,
        schedule.proposalParity,
        schedule.configuredEmissiveTrials);
    schedule.analyticTrials = Upt04DirectRisDomainTrialCount(
        ranges,
        kUpt04LightDoomAnalytic,
        schedule.proposalParity,
        schedule.configuredEmissiveTrials);
    schedule.candidateCount = schedule.emissiveTrials
        + schedule.analyticTrials;
    return schedule;
}

public float Upt04InitialLightScheduleIdentityPdf(
    Upt04InitialLightSchedule schedule,
    uint lightType)
{
    const uint domainCount = lightType == kUpt04LightEmissiveTriangle
        ? schedule.ranges.emissiveCount : schedule.ranges.analyticCount;
    const uint domainTrials = lightType == kUpt04LightEmissiveTriangle
        ? schedule.emissiveTrials : schedule.analyticTrials;
    return domainCount != 0u && domainTrials != 0u
            && schedule.candidateCount != 0u
        ? float(domainTrials) / float(schedule.candidateCount * domainCount)
        : 0.0f;
}

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
    Upt04InitialLightSchedule schedule,
    uint candidateOrdinal,
    float identityRandom,
    float2 surfaceRandom)
{
    Upt04LocalLightSelection result = (Upt04LocalLightSelection)0;
    if (candidateOrdinal >= schedule.candidateCount)
        return result;

    const bool selectEmissive = candidateOrdinal < schedule.emissiveTrials;
    const uint selectedType = selectEmissive
        ? kUpt04LightEmissiveTriangle : kUpt04LightDoomAnalytic;
    Upt04LocalLightRanges typedRanges = (Upt04LocalLightRanges)0;
    if (selectEmissive)
    {
        typedRanges.emissiveStart = schedule.ranges.emissiveStart;
        typedRanges.emissiveCount = schedule.ranges.emissiveCount;
    }
    else
    {
        typedRanges.analyticStart = schedule.ranges.analyticStart;
        typedRanges.analyticCount = schedule.ranges.analyticCount;
    }
    if (schedule.proposalParity != 0u && !selectEmissive)
    {
        const uint analyticOrdinal = candidateOrdinal
            - schedule.emissiveTrials;
        const float stride = max(
            1.0f,
            float(schedule.ranges.analyticCount)
                / float(max(schedule.analyticTrials, 1u)));
        const float lightIndexInRange = min(
            (float(analyticOrdinal) + identityRandom) * stride,
            float(max(schedule.ranges.analyticCount, 1u) - 1u));
        identityRandom = (lightIndexInRange + 0.5f)
            / float(max(schedule.ranges.analyticCount, 1u));
    }
    if (!isfinite(identityRandom) || identityRandom < 0.0f
        || identityRandom >= 1.0f || !all(isfinite(surfaceRandom))
        || any(surfaceRandom < 0.0f) || any(surfaceRandom >= 1.0f))
        return result;

    if (schedule.proposalParity != 0u && selectEmissive)
    {
        result = Upt04SelectInitialWeightedEmissive(
            schedule.ranges,
            identityRandom,
            surfaceRandom,
            schedule.emissiveTrials,
            schedule.candidateCount);
        if (result.valid != 0u)
            return result;
    }

    result = Upt04SelectUniformLocalLight(
        typedRanges, identityRandom, surfaceRandom);
    result.selectionPdf = Upt04InitialLightScheduleIdentityPdf(
        schedule, selectedType);
    return result;
}

public Upt04LocalLightSelection Upt04SelectInitialLightFromRandoms(
    uint candidateOrdinal,
    float identityRandom,
    float2 surfaceRandom)
{
    Upt04LocalLightRanges ranges;
    ranges.emissiveStart = gUpt04Control.emissiveRangeStart;
    ranges.emissiveCount = gUpt04Control.emissiveRangeCount;
    ranges.analyticStart = gUpt04Control.analyticRangeStart;
    ranges.analyticCount = gUpt04Control.analyticRangeCount;
    return Upt04SelectInitialLightFromRandoms(
        Upt04MakeInitialLightSchedule(ranges),
        candidateOrdinal,
        identityRandom,
        surfaceRandom);
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
    Upt04InitialLightSchedule schedule,
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
        schedule, candidateOrdinal, identityRandom, surfaceRandom);
}

public Upt04LocalLightSelection Upt04SelectInitialLightFromReplay(
    uint replayKey,
    uint replayIndex,
    uint pathVertex,
    uint candidateOrdinal)
{
    Upt04LocalLightRanges ranges;
    ranges.emissiveStart = gUpt04Control.emissiveRangeStart;
    ranges.emissiveCount = gUpt04Control.emissiveRangeCount;
    ranges.analyticStart = gUpt04Control.analyticRangeStart;
    ranges.analyticCount = gUpt04Control.analyticRangeCount;
    return Upt04SelectInitialLightFromReplay(
        Upt04MakeInitialLightSchedule(ranges),
        replayKey,
        replayIndex,
        pathVertex,
        candidateOrdinal);
}
