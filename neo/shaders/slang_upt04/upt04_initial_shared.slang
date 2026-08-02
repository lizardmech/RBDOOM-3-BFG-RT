import upt03_reservoir_contract;
import upt04_initial_bindings;
import upt04_initial_control;
import upt04_initial_direct;
import upt04_initial_indirect;
import upt04_initial_random;
import upt04_initial_work_contract;
import upt04_hit_contract;
import upt04_light_contract;
import upt04_local_light_proposal;
import upt04_material_provider_contract;
import upt04_primary_surface_live_contract;
import upt04_random_white_hash;
import upt04_random_contract;
import upt04_transport_initial_bounded;
import upt04_initial_reservoir_stream;

public bool Upt04LocalLightProviderAvailable()
{
    Upt04LocalLightRanges ranges;
    ranges.emissiveStart = gUpt04Control.emissiveRangeStart;
    ranges.emissiveCount = gUpt04Control.emissiveRangeCount;
    ranges.analyticStart = gUpt04Control.analyticRangeStart;
    ranges.analyticCount = gUpt04Control.analyticRangeCount;
    return Upt04LocalLightRangesStructurallyValid(
            ranges,
            gUpt04Control.currentLightCount)
        && Upt04LocalLightRangeCount(ranges) > 0u;
}

public bool Upt04DirectFamilyAvailable()
{
    return (gUpt04Control.enabledFamilyMask & kUpt04FamilyLocalLight) != 0u
        && Upt04LocalLightProviderAvailable();
}

public bool Upt04LoadInitialSurface(
    uint2 pixel,
    out Upt04InitialTraceWork work,
    out Upt04PrimarySurfaceRecord surface)
{
    work = (Upt04InitialTraceWork)0;
    surface = (Upt04PrimarySurfaceRecord)0;
    if (pixel.x >= gUpt04Control.renderWidth
        || pixel.y >= gUpt04Control.renderHeight)
        return false;
    const uint surfaceIndex = pixel.y * gUpt04Control.renderWidth + pixel.x;
    if (surfaceIndex >= gUpt04Control.surfaceCount)
        return false;
    work.surfaceIndex = surfaceIndex;
    surface = gUpt04PrimarySurfaces[surfaceIndex];
    return Upt04PrimarySurfaceRecordValid(surface);
}

public Upt04InitialTraceWork Upt04PrepareDirectPathTreeWorkFromSurface<
    TMaterialSet : IUpt04MaterialSet>(
    uint2 pixel,
    uint surfaceIndex,
    Upt04PrimarySurfaceRecord surface)
{
    Upt04InitialTraceWork work = (Upt04InitialTraceWork)0;
    work.surfaceIndex = surfaceIndex;
    if (!Upt04PrimarySurfaceRecordValid(surface)
        || !Upt04DirectFamilyAvailable())
        return work;
    return Upt04PrepareDirectTrial<TMaterialSet>(work, surface, pixel);
}

public Upt04InitialTraceWork Upt04PrepareDirectPathTreeWork<
    TMaterialSet : IUpt04MaterialSet>(uint2 pixel)
{
    Upt04InitialTraceWork work;
    Upt04PrimarySurfaceRecord surface;
    if (!Upt04LoadInitialSurface(pixel, work, surface))
        return work;
    return Upt04PrepareDirectPathTreeWorkFromSurface<TMaterialSet>(
        pixel, work.surfaceIndex, surface);
}

public Upt04InitialTraceWork Upt04PrepareIndirectPathTreeWorkFromSurface<
    TMaterialSet : IUpt04MaterialSet>(
    uint2 pixel,
    uint surfaceIndex,
    Upt04PrimarySurfaceRecord surface,
    Upt04BoundedTransportState transport)
{
    Upt04InitialTraceWork work = (Upt04InitialTraceWork)0;
    work.surfaceIndex = surfaceIndex;
    if (!Upt04PrimarySurfaceRecordValid(surface))
        return work;
    const bool indirectAvailable =
        (gUpt04Control.enabledFamilyMask & kUpt04FamilyIndirect) != 0u
        && TMaterialSet.PrimaryContinuationAvailable(surface);
    return indirectAvailable
        ? Upt04PrepareIndirectTrial<TMaterialSet>(work, surface, pixel, transport)
        : work;
}

public Upt04InitialTraceWork Upt04PrepareIndirectPathTreeWork<
    TMaterialSet : IUpt04MaterialSet>(
    uint2 pixel,
    Upt04BoundedTransportState transport)
{
    Upt04InitialTraceWork work;
    Upt04PrimarySurfaceRecord surface;
    if (!Upt04LoadInitialSurface(pixel, work, surface))
        return work;
    return Upt04PrepareIndirectPathTreeWorkFromSurface<TMaterialSet>(
        pixel, work.surfaceIndex, surface, transport);
}

public Upt04InitialTraceWork Upt04PrepareStaticInitialFamilyTraceWork<
    TMaterialSet : IUpt04MaterialSet>(
    uint2 pixel,
    uint familyKind)
{
    if (familyKind == kUpt04InitialFamilyDirect)
        return Upt04PrepareDirectPathTreeWork<TMaterialSet>(pixel);
    if (familyKind == kUpt04InitialFamilyIndirect)
    {
        Upt04BoundedTransportState transport = Upt04BeginBoundedTransport();
        if (!Upt04CompletePrimaryNeeStage(transport))
            return (Upt04InitialTraceWork)0;
        return Upt04PrepareIndirectPathTreeWork<TMaterialSet>(pixel, transport);
    }
    return (Upt04InitialTraceWork)0;
}

public Upt04InitialTraceWork Upt04AdvanceInitialTraceWork(
    Upt04InitialTraceWork work,
    Upt04HitFacts hit)
{
    if (work.valid == 0u || work.candidateReady != 0u)
        return work;
    if (work.familyKind == kUpt04InitialFamilyDirect)
        return Upt04CompleteDirectVisibility(work, hit);
    if (work.familyKind == kUpt04InitialFamilyIndirect
        && work.transport.stage == kUpt04TransportIndirectNeeVisibility)
        return Upt04CompleteIndirectNeeVisibility(work, hit);
    return Upt04InvalidateInitialWork(work);
}
