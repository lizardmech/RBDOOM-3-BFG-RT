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
import upt04_material_set_live_probe;
import upt04_primary_surface_live_contract;
import upt04_random_white_hash;
import upt04_random_contract;
import upt04_transport_initial_bounded;

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

public Upt04InitialTraceWork Upt04PrepareStaticInitialFamilyTraceWork(
    uint2 pixel,
    uint familyKind)
{
    Upt04InitialTraceWork work = (Upt04InitialTraceWork)0;
    if (pixel.x >= gUpt04Control.renderWidth
        || pixel.y >= gUpt04Control.renderHeight)
        return work;
    const uint surfaceIndex = pixel.y * gUpt04Control.renderWidth + pixel.x;
    if (surfaceIndex >= gUpt04Control.surfaceCount)
        return work;
    work.surfaceIndex = surfaceIndex;
    const Upt04PrimarySurfaceRecord surface = gUpt04PrimarySurfaces[surfaceIndex];
    if (!Upt04PrimarySurfaceRecordValid(surface))
        return work;
    if (familyKind == kUpt04InitialFamilyDirect)
        return Upt04LocalLightProviderAvailable()
            ? Upt04PrepareDirectTrial(work, surface, pixel, 1.0f)
            : work;
    if (familyKind == kUpt04InitialFamilyIndirect)
        return Upt04PrimaryContinuationAvailable(surface)
            ? Upt04PrepareIndirectTrial(work, surface, pixel, 1.0f)
            : work;
    return work;
}

public Upt04InitialTraceWork Upt04PrepareInitialTraceWork(uint2 pixel)
{
    Upt04InitialTraceWork work = (Upt04InitialTraceWork)0;
    if (pixel.x >= gUpt04Control.renderWidth
        || pixel.y >= gUpt04Control.renderHeight)
        return work;
    const uint surfaceIndex = pixel.y * gUpt04Control.renderWidth + pixel.x;
    if (surfaceIndex >= gUpt04Control.surfaceCount)
        return work;
    work.surfaceIndex = surfaceIndex;

    const Upt04PrimarySurfaceRecord surface = gUpt04PrimarySurfaces[surfaceIndex];
    if (!Upt04PrimarySurfaceRecordValid(surface))
        return work;
    const bool directAvailable = Upt04DirectFamilyAvailable();
    const bool indirectAvailable =
        (gUpt04Control.enabledFamilyMask & kUpt04FamilyIndirect) != 0u
        && Upt04PrimaryContinuationAvailable(surface);
    const uint familyCount = (directAvailable ? 1u : 0u)
        + (indirectAvailable ? 1u : 0u);
    if (familyCount == 0u)
        return work;

    const float familyPdf = 1.0f / float(familyCount);
    const uint familyOrdinal = min(
        uint(Upt04WhiteHash(Upt04InitialRandomKey(
            pixel,
            kUpt04StreamInitialFamilySelection,
            kUpt04DimensionInitialFamilySelection)) * float(familyCount)),
        familyCount - 1u);
    const bool chooseDirect = directAvailable && familyOrdinal == 0u;
    return chooseDirect
        ? Upt04PrepareDirectTrial(work, surface, pixel, familyPdf)
        : Upt04PrepareIndirectTrial(work, surface, pixel, familyPdf);
}

public Upt04InitialTraceWork Upt04AdvanceInitialTraceWork(
    Upt04InitialTraceWork work,
    Upt04HitFacts hit)
{
    if (work.valid == 0u || work.candidateReady != 0u)
        return work;
    if (work.familyKind == kUpt04InitialFamilyDirect)
        return Upt04CompleteDirectVisibility(work, hit);
    if (work.familyKind != kUpt04InitialFamilyIndirect)
        return Upt04InvalidateInitialWork(work);
    const uint2 pixel = uint2(
        work.surfaceIndex % gUpt04Control.renderWidth,
        work.surfaceIndex / gUpt04Control.renderWidth);
    if (work.transport.stage == kUpt04TransportIndirectContinuation)
        return Upt04PrepareIndirectHitTrial(work, hit, pixel);
    if (work.transport.stage == kUpt04TransportIndirectNeeVisibility)
        return Upt04CompleteIndirectNeeVisibility(work, hit);
    return Upt04InvalidateInitialWork(work);
}

public void Upt04FinishInitialTraceWork(Upt04InitialTraceWork work)
{
    Upt03LogicalReservoir reservoir = (Upt03LogicalReservoir)0;
    if (work.valid != 0u && work.candidateReady != 0u)
    {
        Upt03Candidate candidate = (Upt03Candidate)0;
        candidate.status = work.candidateStatus;
        candidate.eventKind = work.eventKind;
        candidate.lobeMask = work.lobeMask;
        candidate.flags = work.candidateFlags;
        candidate.pathLength = work.pathLength;
        candidate.reconnectionVertexLength = work.reconnectionVertexLength;
        candidate.identity0 = work.identity0;
        candidate.identity1 = work.identity1;
        candidate.replayKey = work.replayKey;
        candidate.replayIndex = work.replayIndex;
        candidate.generationFingerprint = kUpt03ExpectedGeneration;
        candidate.sampleCoord0 = work.sampleCoordinates.x;
        candidate.sampleCoord1 = work.sampleCoordinates.y;
        if (candidate.status == kUpt04SampleValidPositive)
        {
            candidate.contributionR = work.contribution.x;
            candidate.contributionG = work.contribution.y;
            candidate.contributionB = work.contribution.z;
            candidate.target = work.target;
        }
        candidate.proposalPdf = work.proposalPdf;
        candidate.pathPdf = work.pathPdf;
        candidate.partialJacobian = work.partialJacobian;
        candidate.accumulatedRouletteProbability =
            work.accumulatedRouletteProbability;
        const uint2 pixel = uint2(
            work.surfaceIndex % gUpt04Control.renderWidth,
            work.surfaceIndex / gUpt04Control.renderWidth);
        Upt03StreamCandidate(
            reservoir,
            candidate,
            Upt04WhiteHash(Upt04InitialRandomKey(
                pixel,
                kUpt04StreamInitialReservoirSelection,
                kUpt04DimensionInitialReservoirSelection)));
    }
    gUpt04Reservoirs[work.surfaceIndex] = Upt03PackReservoir(reservoir);
}
