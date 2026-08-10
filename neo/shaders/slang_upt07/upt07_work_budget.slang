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
};

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
