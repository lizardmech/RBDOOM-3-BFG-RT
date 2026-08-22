import upt03_reservoir_contract;
import upt04_backend_rayquery;
import upt04_hit_contract;
import upt04_initial_bindings;
import upt04_initial_direct_only_shared;
import upt04_initial_direct;
import upt04_initial_light_selection;
import upt04_initial_random;
import upt04_initial_work_contract;
import upt04_light_contract;
import upt04_material_contract;
import upt04_primary_surface_live_contract;
import upt04_initial_reservoir_stream;
import upt04_reservoir_image;
import upt04_primary_image;
import upt04_random_contract;
import upt04_random_white_hash;
import upt05_material_set_openpbr;

#ifndef UPT47_REFLECTION_POSITIVE_ONLY
#define UPT47_REFLECTION_POSITIVE_ONLY 0
#endif

#ifndef UPT04_SPLIT_INITIAL_DIRECT
#define UPT04_SPLIT_INITIAL_DIRECT 0
#endif

public void Upt04FinishDirectPass(Upt04InitialTraceWork work)
{
#if UPT04_SPLIT_INITIAL_DIRECT
    Upt04FinishIntermediateDirectTraceWork(work);
#else
    Upt04FinishInitialTraceWork(work);
#endif
}

#if UPT47_REFLECTION_POSITIVE_ONLY
// Literal legacy reflection-D0 estimator shape.  The legacy path retains four
// positive RIS proposals and, after selecting one, retries the other retained
// proposals when visibility rejects the winner.  The ordinary UPT D0 contract
// traces only its selected winner; using that path here turned every occluded
// winner into an empty reflection pixel even when the same eight-draw RIS set
// contained usable lights.
//
// This remains private to the glass-reflection child artifact.  It is bounded
// to four visibility rays, preserves one published reservoir sample, and does
// not alter the accepted main UPT D0 ray ceiling.
public void Upt47ExecuteLegacyReflectionInitial(uint2 pixel)
{
    const uint surfaceIndex = pixel.y * gUpt04Control.renderWidth + pixel.x;
    const Upt04PrimarySurfaceRecord surface =
        Upt04LoadPrimarySurfaceRecord(surfaceIndex);
    if (!Upt04PrimarySurfaceRecordValid(surface))
    {
        Upt04StorePackedReservoir(surfaceIndex, (Upt03PackedReservoir)0);
        return;
    }

    Upt04InitialLightSchedule schedule =
        Upt04MakeInitialLightSchedule(Upt04InitialDirectLightRanges());
    // The host deliberately stores r_pathTracingReflectionSecondarySamples in
    // the existing trial-count payload.  Reflection is analytic-only, so make
    // that payload authoritative for this private schedule instead of leaving
    // it attached to the disabled emissive domain.
    schedule.emissiveTrials = 0u;
    schedule.analyticTrials = min(
        min(schedule.configuredEmissiveTrials, 16u),
        schedule.ranges.analyticCount);
    schedule.candidateCount = schedule.analyticTrials;
    if (schedule.candidateCount == 0u)
    {
        Upt04StorePackedReservoir(surfaceIndex, (Upt03PackedReservoir)0);
        return;
    }

    const Upt04PreparedDirectMaterial preparedMaterial =
        Upt05OpenPbrMaterialSet.PreparePrimaryDirect(surface);
    static const uint kUpt47RetryCapacity = 4u;
    Upt04DirectRisProposal retryProposals[kUpt47RetryCapacity];
    [unroll]
    for (uint slot = 0u; slot < kUpt47RetryCapacity; ++slot)
        retryProposals[slot] = (Upt04DirectRisProposal)0;

    float weightSum = 0.0f;
    uint positiveCandidateCount = 0u;
    uint retryCount = 0u;
    uint selectedSlot = 0xffffffffu;
    [loop]
    for (uint candidateOrdinal = 0u;
         candidateOrdinal < schedule.candidateCount;
         ++candidateOrdinal)
    {
        const Upt04DirectRisProposal proposal =
            Upt04EvaluateDirectRisProposal<Upt05OpenPbrMaterialSet>(
                surface,
                preparedMaterial,
                schedule,
                pixel,
                candidateOrdinal);
        if (proposal.valid == 0u)
            continue;
        const float candidateWeight = proposal.target
            / proposal.sourceProposalPdf;
        const float newWeightSum = weightSum + candidateWeight;
        if (!Upt04FinitePositive(candidateWeight)
            || !Upt04FinitePositive(newWeightSum))
            continue;

        ++positiveCandidateCount;
        const float selectionRandom = Upt04WhiteHash(
            Upt04InitialRandomKeyAtVertex(
                pixel,
                candidateOrdinal,
                kUpt04StreamInitialReservoirSelection,
                kUpt04DimensionInitialReservoirSelection));
        const bool selectThis = selectedSlot == 0xffffffffu
            || selectionRandom * newWeightSum < candidateWeight;
        weightSum = newWeightSum;

        if (retryCount < kUpt47RetryCapacity)
        {
            retryProposals[retryCount] = proposal;
            if (selectThis)
                selectedSlot = retryCount;
            ++retryCount;
        }
        else if (selectThis)
        {
            // Match legacy: retain the late selected proposal in slot zero so
            // it remains available to the visibility retry walk.
            retryProposals[0] = proposal;
            selectedSlot = 0u;
        }
    }

    if (retryCount == 0u || positiveCandidateCount == 0u
        || !Upt04FinitePositive(weightSum))
    {
        Upt04StorePackedReservoir(surfaceIndex, (Upt03PackedReservoir)0);
        return;
    }
    if (selectedSlot >= retryCount)
        selectedSlot = 0u;

    // Legacy conditions on positive proposals here.  This is intentionally
    // biased and brighter than the fixed-schedule UPT estimator; it is the
    // energy behavior that accompanies legacy's zero-rejection rule.
    const float finalizedMass = weightSum / float(positiveCandidateCount);
    [loop]
    for (uint attempt = 0u; attempt < retryCount; ++attempt)
    {
        const uint slot = (selectedSlot + attempt) % retryCount;
        Upt04InitialTraceWork work = (Upt04InitialTraceWork)0;
        work.surfaceIndex = surfaceIndex;
        work = Upt04BuildDirectProposalTraceWork(
            work,
            surface,
            retryProposals[slot],
            finalizedMass,
            kUpt04D0DiagFreshAnalyticSelected);
        if (work.valid == 0u || work.traceRequired == 0u)
            continue;

        const Upt04HitFacts hit = Upt04TraceVisibilityRayQuery(
            work.ray, Upt04DirectVisibilityIgnore(work));
        work = Upt04CompleteDirectVisibility(work, hit);
        if (work.valid != 0u
            && work.candidateStatus == kUpt04SampleValidPositive)
        {
            Upt04FinishInitialTraceWork(work);
            return;
        }
    }

    Upt04StorePackedReservoir(surfaceIndex, (Upt03PackedReservoir)0);
}
#endif

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gUpt04Control.renderWidth || pixel.y >= gUpt04Control.renderHeight)
        return;
    if (pixel.y * gUpt04Control.renderWidth + pixel.x >= gUpt04Control.surfaceCount)
        return;
#if UPT47_REFLECTION_POSITIVE_ONLY
    Upt47ExecuteLegacyReflectionInitial(pixel);
    return;
#else
    const uint surfaceIndex = pixel.y * gUpt04Control.renderWidth + pixel.x;
#if !UPT04_SPLIT_INITIAL_DIRECT
    if (gUpt04Control.shaderProofMode <= 1u)
    {
        Upt04StorePackedReservoir(surfaceIndex, (Upt03PackedReservoir)0);
        return;
    }
    if (gUpt04Control.shaderProofMode == 2u)
    {
        const Upt04PrimarySurfaceRecord surface =
            Upt04LoadPrimarySurfaceRecord(surfaceIndex);
        Upt03PackedReservoir proof = (Upt03PackedReservoir)0;
        proof.word15 = surface.header.x;
        Upt04StorePackedReservoir(surfaceIndex, proof);
        return;
    }
#endif
    Upt04InitialTraceWork work =
        Upt04PrepareStaticInitialFamilyTraceWork<Upt05OpenPbrMaterialSet>(
            pixel,
            kUpt04InitialFamilyDirect);
#if UPT04_SPLIT_INITIAL_DIRECT
    Upt04HitFacts hit = Upt04MissFacts(work.ray.tMax);
    if (work.valid != 0u && work.traceRequired != 0u)
        hit = Upt04TraceVisibilityRayQuery(
            work.ray, Upt04DirectVisibilityIgnore(work));
    work = Upt04AdvanceInitialTraceWork(work, hit);
    Upt04FinishDirectPass(work);
#else
    if (gUpt04Control.shaderProofMode == 3u)
    {
        Upt04FinishDirectPass(work);
        return;
    }
    Upt04TraceRequest queryRequest = work.ray;
    const bool fixedRayProof = gUpt04Control.shaderProofMode == 4u;
    if (fixedRayProof)
    {
        queryRequest.origin = float3(0.0f, 0.0f, 0.0f);
        queryRequest.tMin = 0.01f;
        queryRequest.direction = float3(0.0f, 0.0f, 1.0f);
        queryRequest.tMax = 1.0f;
    }
    Upt04HitFacts hit = Upt04MissFacts(queryRequest.tMax);
    if (fixedRayProof || (work.valid != 0u && work.traceRequired != 0u))
        hit = Upt04TraceRayQueryProof(
            queryRequest,
            fixedRayProof ? gUpt04Control.shaderProofMode >= 6u : true,
            !fixedRayProof,
            fixedRayProof
                ? Upt04NoVisibilityIgnore()
                : Upt04DirectVisibilityIgnore(work),
            false);
    if (fixedRayProof)
    {
        Upt03PackedReservoir proof = (Upt03PackedReservoir)0;
        proof.word15 = hit.status;
        Upt04StorePackedReservoir(surfaceIndex, proof);
        return;
    }
    work = Upt04AdvanceInitialTraceWork(work, hit);
    Upt04FinishDirectPass(work);
#endif
#endif
}
