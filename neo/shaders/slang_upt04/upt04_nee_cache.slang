// Consume-only NEE-cache lookup. Matches NEECACHE-01 / RAB_NeeCache cell
// hash. Does not run the update walk or RAB replay (Remix dispatchNEE
// consume, not NeeCache::sample). Bindings 74-77 are the ABI slots.
import upt04_initial_control;
import upt04_light_contract;
import upt04_initial_light_selection;

#ifndef UPT04_NEE_CACHE
#define UPT04_NEE_CACHE 0
#endif

public static const uint kUpt04NeeCacheCellCount = 65536u;
public static const uint kUpt04NeeCacheCandidateSlots = 8u;
public static const uint kUpt04NeeCacheCellResolution = 8u;
public static const float kUpt04NeeCacheMinRange = 256.0f;

public struct Upt04NeeCacheCandidateRecord
{
    uint denseRluIndex;
    uint lightClass;
    float sourcePdf;
    float invSourcePdf;
    float candidateWeight;
    uint cellIndex;
    uint candidateSlot;
    uint flags;
};

public struct Upt04NeeCacheCellRecord
{
    uint flags;
    uint hash;
    uint taskOffset;
    uint taskCount;
    uint candidateOffset;
    uint candidateCount;
    uint reserved0;
    uint reserved1;
};

#if UPT04_NEE_CACHE
[[vk::binding(75, 0)]] StructuredBuffer<Upt04NeeCacheCellRecord>
    gUpt04NeeCacheCells;
[[vk::binding(77, 0)]] StructuredBuffer<Upt04NeeCacheCandidateRecord>
    gUpt04NeeCacheCandidates;
#endif

public uint Upt04NeeCacheHashCell(int3 coord, uint level)
{
    uint hash = asuint(coord.x) * 73856093u;
    hash ^= asuint(coord.y) * 19349663u;
    hash ^= asuint(coord.z) * 83492791u;
    hash ^= level * 2654435761u;
    hash ^= hash >> 16u;
    hash *= 2246822519u;
    hash ^= hash >> 13u;
    hash *= 3266489917u;
    hash ^= hash >> 16u;
    return hash;
}

public float Upt04NeeCacheHashToUnit(uint hash)
{
    return float(hash & 0x00ffffffu) / 16777216.0f;
}

public Upt04LocalLightSelection Upt04TryNeeCacheSelection(
    float3 worldPosition,
    Upt04InitialLightSchedule schedule,
    float identityRandom,
    float2 surfaceRandom,
    uint currentLightCount,
    uint frameSampleIndex)
{
    Upt04LocalLightSelection result = (Upt04LocalLightSelection)0;
#if !UPT04_NEE_CACHE
    return result;
#else
    if (!all(isfinite(worldPosition)))
        return result;
    const float cellSize = max(
        kUpt04NeeCacheMinRange / float(kUpt04NeeCacheCellResolution), 1.0f);
    const int3 coord = int3(floor(worldPosition / cellSize));
    const uint hash = Upt04NeeCacheHashCell(coord, 0u);
    const uint cellIndex = hash % kUpt04NeeCacheCellCount;
    uint cellCount;
    uint cellStride;
    gUpt04NeeCacheCells.GetDimensions(cellCount, cellStride);
    if (cellIndex >= cellCount)
        return result;
    const Upt04NeeCacheCellRecord cell = gUpt04NeeCacheCells[cellIndex];
    if (cell.flags == 0u || cell.hash != hash)
        return result;
    uint candidateCount;
    uint candidateStride;
    gUpt04NeeCacheCandidates.GetDimensions(candidateCount, candidateStride);
    const uint baseSlot = cellIndex * kUpt04NeeCacheCandidateSlots;
    if (baseSlot >= candidateCount)
        return result;
    float totalWeight = 0.0f;
    const uint currentLights = currentLightCount;
    [loop]
    for (uint slot = 0u; slot < kUpt04NeeCacheCandidateSlots; ++slot)
    {
        if (baseSlot + slot >= candidateCount)
            break;
        const Upt04NeeCacheCandidateRecord candidate =
            gUpt04NeeCacheCandidates[baseSlot + slot];
        if (candidate.denseRluIndex == 0xffffffffu
            || candidate.denseRluIndex >= currentLights
            || !Upt04FinitePositive(candidate.candidateWeight))
            continue;
        totalWeight += candidate.candidateWeight;
    }
    if (!Upt04FinitePositive(totalWeight))
        return result;
    const float unit = (isfinite(identityRandom)
        && identityRandom >= 0.0f && identityRandom < 1.0f)
        ? identityRandom
        : Upt04NeeCacheHashToUnit(hash ^ (frameSampleIndex * 668265263u));
    const float threshold = unit * totalWeight;
    float cumulative = 0.0f;
    Upt04NeeCacheCandidateRecord selected = (Upt04NeeCacheCandidateRecord)0;
    selected.denseRluIndex = 0xffffffffu;
    [loop]
    for (uint pick = 0u; pick < kUpt04NeeCacheCandidateSlots; ++pick)
    {
        if (baseSlot + pick >= candidateCount)
            break;
        const Upt04NeeCacheCandidateRecord candidate =
            gUpt04NeeCacheCandidates[baseSlot + pick];
        if (candidate.denseRluIndex == 0xffffffffu
            || candidate.denseRluIndex >= currentLights
            || !Upt04FinitePositive(candidate.candidateWeight))
            continue;
        cumulative += candidate.candidateWeight;
        if (selected.denseRluIndex == 0xffffffffu && cumulative >= threshold)
            selected = candidate;
    }
    if (selected.denseRluIndex == 0xffffffffu)
        return result;
    const bool emissive =
        selected.denseRluIndex >= schedule.ranges.emissiveStart
        && selected.denseRluIndex - schedule.ranges.emissiveStart
            < schedule.ranges.emissiveCount;
    result.valid = 1u;
    result.denseIndex = selected.denseRluIndex;
    result.type = emissive
        ? kUpt04LightEmissiveTriangle
        : kUpt04LightDoomAnalytic;
    result.requiresTriangleReplay = emissive ? 1u : 0u;
    result.selectionPdf = Upt04FinitePositive(selected.sourcePdf)
        ? selected.sourcePdf
        : (selected.candidateWeight / totalWeight);
    result.sampleCoordinates = surfaceRandom;
    return result;
#endif
}