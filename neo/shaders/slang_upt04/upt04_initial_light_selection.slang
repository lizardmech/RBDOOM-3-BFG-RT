import upt04_initial_random;
import upt04_initial_bindings;
import upt04_initial_control;
import upt04_light_contract;
import upt04_light_tile_contract;
import upt04_local_light_proposal;
import upt04_random_contract;
import upt04_random_white_hash;
import upt04_replay_random;

#ifndef UPT04_LIGHT_TILES
#define UPT04_LIGHT_TILES 0
#endif

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

public Upt04InitialLightSchedule Upt04MakeBounceLightSchedule(
    Upt04LocalLightRanges ranges,
    uint bounceIndex)
{
    Upt04InitialLightSchedule schedule = Upt04MakeInitialLightSchedule(ranges);
    schedule.emissiveTrials = Upt04InverseSquareNeeTrialCount(
        schedule.emissiveTrials, bounceIndex);
    schedule.analyticTrials = Upt04InverseSquareNeeTrialCount(
        schedule.analyticTrials, bounceIndex);
    schedule.candidateCount = schedule.emissiveTrials
        + schedule.analyticTrials;
    return schedule;
}

public Upt04InitialLightSchedule Upt04MakeSecondaryLightSchedule(
    Upt04LocalLightRanges ranges)
{
    return Upt04MakeBounceLightSchedule(
        ranges, kUpt04SecondaryNeeBounceIndex);
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

#if UPT04_LIGHT_TILES
public uint Upt04SelectCoherentLightTile(uint2 pixel, uint pathVertex)
{
    const uint2 screenTile = pixel >> 3u;
    uint key = gUpt04Control.frameSampleIndex ^ 0x68bc21ebu;
    key = Upt04HashWord(key ^ (screenTile.x * 0x9e3779b9u));
    key = Upt04HashWord(key ^ (screenTile.y * 0x85ebca6bu));
    key = Upt04HashWord(key ^ (pathVertex * 0xc2b2ae35u));
    return key % kUpt04LightTileCount;
}

public Upt04LocalLightSelection Upt04SelectInitialLightFromTile(
    Upt04InitialLightSchedule schedule,
    uint2 pixel,
    uint pathVertex,
    uint candidateOrdinal,
    float identityRandom,
    float2 surfaceRandom)
{
    Upt04LocalLightSelection result = (Upt04LocalLightSelection)0;
    if (candidateOrdinal >= schedule.candidateCount
        || !isfinite(identityRandom) || identityRandom < 0.0f
        || identityRandom >= 1.0f || !all(isfinite(surfaceRandom))
        || any(surfaceRandom < 0.0f) || any(surfaceRandom >= 1.0f))
        return result;

    const bool emissive = candidateOrdinal < schedule.emissiveTrials;
    const uint tileIndex = Upt04SelectCoherentLightTile(pixel, pathVertex);
    uint entryInDomain = 0u;
    uint domain = kUpt04LightTileDomainEmissive;
    if (emissive)
    {
        entryInDomain = min(
            uint(identityRandom * float(kUpt04LightTileDomainSize)),
            kUpt04LightTileDomainSize - 1u);
    }
    else
    {
        const uint analyticOrdinal = candidateOrdinal
            - schedule.emissiveTrials;
        if (analyticOrdinal >= schedule.analyticTrials
            || analyticOrdinal >= kUpt04LightTileAnalyticOrdinalCount)
            return result;
        const uint variant = min(
            uint(identityRandom
                * float(kUpt04LightTileAnalyticVariantsPerOrdinal)),
            kUpt04LightTileAnalyticVariantsPerOrdinal - 1u);
        entryInDomain = analyticOrdinal
            + variant * kUpt04LightTileAnalyticOrdinalCount;
        domain = kUpt04LightTileDomainAnalytic;
    }

    const Upt04LightTileEntry entry = gUpt04LightTiles[
        Upt04LightTileLinearIndex(domain, tileIndex, entryInDomain)];
    const uint expectedType = emissive
        ? kUpt04LightEmissiveTriangle : kUpt04LightDoomAnalytic;
    const bool inExpectedRange = emissive
        ? entry.denseLightIndex >= schedule.ranges.emissiveStart
            && entry.denseLightIndex - schedule.ranges.emissiveStart
                < schedule.ranges.emissiveCount
        : entry.denseLightIndex >= schedule.ranges.analyticStart
            && entry.denseLightIndex - schedule.ranges.analyticStart
                < schedule.ranges.analyticCount;
    if (entry.denseLightIndex == kUpt04InvalidLightIndex
        || entry.lightType != expectedType || !inExpectedRange
        || !Upt04FinitePositive(entry.selectionPdf))
        return result;

    result.valid = 1u;
    result.denseIndex = entry.denseLightIndex;
    result.type = entry.lightType;
    result.requiresTriangleReplay = emissive ? 1u : 0u;
    result.selectionPdf = entry.selectionPdf;
    result.sampleCoordinates = surfaceRandom;
    result.reserved = emissive ? entry.replayGuard : 0u;
    return result;
}
#endif

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
    uint2 pixel,
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
#if UPT04_LIGHT_TILES
    return Upt04SelectInitialLightFromTile(
        schedule,
        pixel,
        pathVertex,
        candidateOrdinal,
        identityRandom,
        surfaceRandom);
#else
    return Upt04SelectInitialLightFromRandoms(
        schedule, candidateOrdinal, identityRandom, surfaceRandom);
#endif
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
        Upt04MakeInitialLightSchedule(ranges), uint2(0u),
        replayKey,
        replayIndex,
        pathVertex,
        candidateOrdinal);
}
