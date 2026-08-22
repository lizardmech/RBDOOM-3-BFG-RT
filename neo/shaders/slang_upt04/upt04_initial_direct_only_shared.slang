import upt04_initial_bindings;
import upt04_primary_image;
import upt04_initial_control;
import upt04_initial_direct;
import upt04_initial_reservoir_stream;
import upt04_initial_work_contract;
import upt04_hit_contract;
import upt04_primary_surface_live_contract;
import upt04_material_provider_contract;

public Upt04InitialTraceWork Upt04PrepareStaticInitialFamilyTraceWork<
    TMaterialSet : IUpt04MaterialSet>(
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
    if (familyKind != kUpt04InitialFamilyDirect)
        return work;
    // Split D0 is still the unified estimator: the direct and indirect
    // artifacts are only a scheduling split.  Respect the runtime family mask
    // before doing any direct proposal work so diagnostic component views (and
    // any future unified-family admission policy) cannot leave a hidden direct
    // winner in the shared reservoir for T0/S0 to propagate.
    if ((Upt04EnabledFamilyMask(gUpt04Control.enabledFamilyMask)
            & kUpt04FamilyLocalLight) == 0u)
        return work;
    const Upt04PrimarySurfaceRecord surface =
        Upt04LoadPrimarySurfaceRecord(surfaceIndex);
    return Upt04PrimarySurfaceRecordValid(surface)
        ? Upt04PrepareDirectTrial<TMaterialSet>(work, surface, pixel)
        : work;
}

public Upt04InitialTraceWork Upt04AdvanceInitialTraceWork(
    Upt04InitialTraceWork work,
    Upt04HitFacts hit)
{
    if (work.valid == 0u || work.candidateReady != 0u)
        return work;
    return work.familyKind == kUpt04InitialFamilyDirect
        ? Upt04CompleteDirectVisibility(work, hit)
        : Upt04InvalidateInitialWork(work);
}
