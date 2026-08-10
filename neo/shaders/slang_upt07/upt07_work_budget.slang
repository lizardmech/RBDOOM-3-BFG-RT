#ifndef UPT07_BOTTLENECK_STAGE
#define UPT07_BOTTLENECK_STAGE 0
#endif

// Logical executed-work counters. These count renderer contract operations,
// not compiler instructions: every increment names one potentially dependent
// memory or traversal operation that can multiply per pixel. The production
// specializations compile every helper below to a no-op.
public static const uint kUpt07WorkRayQueries = 0u;
public static const uint kUpt07WorkProceedIterations = 1u;
public static const uint kUpt07WorkStableRemapLoads = 2u;
public static const uint kUpt07WorkEmissiveHashLoads = 3u;
public static const uint kUpt07WorkLightRecordLoads = 4u;
public static const uint kUpt07WorkGeometryResolves = 5u;
public static const uint kUpt07WorkMaterialRecordLoads = 6u;
public static const uint kUpt07WorkTextureSamples = 7u;
public static const uint kUpt07WorkCounterCount = 8u;
public static const uint kUpt07WorkSiteHistoryReplay = 0u;
public static const uint kUpt07WorkSiteReciprocalReplay = 1u;
public static const uint kUpt07WorkSiteFinalIndirectVisibility = 2u;
public static const uint kUpt07WorkSiteFinalDirectVisibility = 3u;
public static const uint kUpt07WorkSiteCount = 4u;
public static const uint kUpt07WorkInvalidSite = 0xffffffffu;
public static const uint kUpt07WorkRecordWordCount =
    kUpt07WorkCounterCount + kUpt07WorkSiteCount + 1u;

// Diagnostic T0 ceilings. They are intentionally strict for the accepted
// baseline unified route. Alpha-tested traversal is scene-dependent and is
// therefore reported but not assigned a false universal Proceed ceiling.
public static const uint kUpt07BudgetRayQueriesPerPixel = 3u;
public static const uint kUpt07BudgetStableRemapLoadsPerPixel = 2u;
public static const uint kUpt07BudgetEmissiveHashLoadsPerPixel = 32u;
public static const uint kUpt07BudgetLightRecordLoadsPerPixel = 8u;

public struct Upt07WorkCounters
{
    public uint rayQueries;
    public uint proceedIterations;
    public uint stableRemapLoads;
    public uint emissiveHashLoads;
    public uint lightRecordLoads;
    public uint geometryResolves;
    public uint materialRecordLoads;
    public uint textureSamples;
#if UPT07_BOTTLENECK_STAGE == 12
    public uint currentSite;
    public uint historyReplayPacked;
    public uint reciprocalReplayPacked;
    public uint finalIndirectVisibilityPacked;
    public uint finalDirectVisibilityPacked;
#endif
};

public uint Upt07WorkIncrementPackedByte(uint packed, uint shift)
{
    const uint oldValue = (packed >> shift) & 0xffu;
    const uint newValue = min(oldValue + 1u, 0xffu);
    return (packed & ~(0xffu << shift)) | (newValue << shift);
}

public void Upt07WorkInitialize(inout Upt07WorkCounters counters)
{
#if UPT07_BOTTLENECK_STAGE == 12
    counters.currentSite = kUpt07WorkInvalidSite;
#endif
}

public void Upt07WorkBeginSite(
    inout Upt07WorkCounters counters,
    uint site)
{
#if UPT07_BOTTLENECK_STAGE == 12
    counters.currentSite = site;
#endif
}

public void Upt07WorkEndSite(inout Upt07WorkCounters counters)
{
#if UPT07_BOTTLENECK_STAGE == 12
    counters.currentSite = kUpt07WorkInvalidSite;
#endif
}

public uint Upt07WorkMetricShift(uint slot)
{
    if (slot == kUpt07WorkRayQueries) return 0u;
    if (slot == kUpt07WorkProceedIterations) return 8u;
    if (slot == kUpt07WorkGeometryResolves) return 16u;
    return 24u;
}

public void Upt07WorkAddSite(inout Upt07WorkCounters counters, uint slot)
{
#if UPT07_BOTTLENECK_STAGE == 12
    if (counters.currentSite >= kUpt07WorkSiteCount
        || (slot != kUpt07WorkRayQueries
            && slot != kUpt07WorkProceedIterations
            && slot != kUpt07WorkGeometryResolves
            && slot != kUpt07WorkMaterialRecordLoads))
        return;
    const uint shift = Upt07WorkMetricShift(slot);
    if (counters.currentSite == kUpt07WorkSiteHistoryReplay)
        counters.historyReplayPacked = Upt07WorkIncrementPackedByte(
            counters.historyReplayPacked, shift);
    else if (counters.currentSite == kUpt07WorkSiteReciprocalReplay)
        counters.reciprocalReplayPacked = Upt07WorkIncrementPackedByte(
            counters.reciprocalReplayPacked, shift);
    else if (counters.currentSite == kUpt07WorkSiteFinalIndirectVisibility)
        counters.finalIndirectVisibilityPacked = Upt07WorkIncrementPackedByte(
            counters.finalIndirectVisibilityPacked, shift);
    else
        counters.finalDirectVisibilityPacked = Upt07WorkIncrementPackedByte(
            counters.finalDirectVisibilityPacked, shift);
#endif
}

public uint Upt07WorkSitePacked(Upt07WorkCounters counters, uint site)
{
#if UPT07_BOTTLENECK_STAGE == 12
    if (site == kUpt07WorkSiteHistoryReplay)
        return counters.historyReplayPacked;
    if (site == kUpt07WorkSiteReciprocalReplay)
        return counters.reciprocalReplayPacked;
    if (site == kUpt07WorkSiteFinalIndirectVisibility)
        return counters.finalIndirectVisibilityPacked;
    return counters.finalDirectVisibilityPacked;
#else
    return 0u;
#endif
}

public void Upt07WorkAdd(inout Upt07WorkCounters counters, uint slot)
{
#if UPT07_BOTTLENECK_STAGE == 12
    if (slot == kUpt07WorkRayQueries) ++counters.rayQueries;
    else if (slot == kUpt07WorkProceedIterations) ++counters.proceedIterations;
    else if (slot == kUpt07WorkStableRemapLoads) ++counters.stableRemapLoads;
    else if (slot == kUpt07WorkEmissiveHashLoads) ++counters.emissiveHashLoads;
    else if (slot == kUpt07WorkLightRecordLoads) ++counters.lightRecordLoads;
    else if (slot == kUpt07WorkGeometryResolves) ++counters.geometryResolves;
    else if (slot == kUpt07WorkMaterialRecordLoads) ++counters.materialRecordLoads;
    else if (slot == kUpt07WorkTextureSamples) ++counters.textureSamples;
    Upt07WorkAddSite(counters, slot);
#endif
}

public uint Upt07WorkBudgetViolationMask(Upt07WorkCounters counters)
{
    uint mask = 0u;
    if (counters.rayQueries > kUpt07BudgetRayQueriesPerPixel) mask |= 1u << 0u;
    if (counters.stableRemapLoads > kUpt07BudgetStableRemapLoadsPerPixel) mask |= 1u << 1u;
    if (counters.emissiveHashLoads > kUpt07BudgetEmissiveHashLoadsPerPixel) mask |= 1u << 2u;
    if (counters.lightRecordLoads > kUpt07BudgetLightRecordLoadsPerPixel) mask |= 1u << 3u;
    return mask;
}
