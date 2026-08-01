import upt04_initial_shared;
import upt04_initial_work_contract;
import upt04_backend_rayquery;
import upt04_initial_bindings;
import upt04_hit_contract;
import upt03_reservoir_contract;
import upt04_transport_initial_bounded;
import upt04_initial_indirect;
import upt04_initial_reservoir_stream;
import upt04_primary_surface_live_contract;
import upt04_light_contract;
import upt04_initial_control;

// This entry point is intentionally separate from every production entry.
// The fixed 16-word tuple is a bring-up observable, not production state.
[[vk::binding(23, 0)]] RWStructuredBuffer<uint> gUpt04Diagnostics;

public static const uint kUpt04DiagReceiverValid = 0u;
public static const uint kUpt04DiagReceiverInvalid = 1u;
public static const uint kUpt04DiagDirectInvalid = 2u;
public static const uint kUpt04DiagDirectValidZero = 3u;
public static const uint kUpt04DiagDirectValidPositive = 4u;
public static const uint kUpt04DiagIndirectInvalid = 5u;
public static const uint kUpt04DiagIndirectValidZero = 6u;
public static const uint kUpt04DiagIndirectValidPositive = 7u;
public static const uint kUpt04DiagSelectedPrimaryNee = 8u;
public static const uint kUpt04DiagSelectedBsdfEndpoint = 9u;
public static const uint kUpt04DiagSelectedSecondaryNee = 10u;
public static const uint kUpt04DiagContinuationRays = 11u;
public static const uint kUpt04DiagVisibilityRays = 12u;
public static const uint kUpt04DiagCanonicalEmptyWrites = 13u;
public static const uint kUpt04DiagCandidateInputs = 14u;
public static const uint kUpt04DiagRayCeilingViolations = 15u;

public void Upt04DiagIncrement(uint counter)
{
    uint ignored;
    InterlockedAdd(gUpt04Diagnostics[counter], 1u, ignored);
}

public void Upt04DiagCandidate(Upt04InitialTraceWork work, bool direct)
{
    const uint base = direct ? kUpt04DiagDirectInvalid : kUpt04DiagIndirectInvalid;
    uint statusOffset = 0u;
    if (work.valid != 0u && work.candidateReady != 0u)
    {
        statusOffset = work.candidateStatus == 2u ? 2u
            : (work.candidateStatus == 1u ? 1u : 0u);
    }
    Upt04DiagIncrement(base + statusOffset);
    Upt04DiagIncrement(kUpt04DiagCandidateInputs);
}

public void Upt04DiagRay(Upt04InitialTraceWork work, bool continuation)
{
    if (work.valid == 0u || work.traceRequired == 0u)
        return;
    Upt04DiagIncrement(continuation
        ? kUpt04DiagContinuationRays
        : kUpt04DiagVisibilityRays);
    if (!Upt04RayBudgetWithinD0Ceiling(work.transport))
        Upt04DiagIncrement(kUpt04DiagRayCeilingViolations);
}

public void Upt04DiagSelection(Upt03LogicalReservoir reservoir)
{
    if (reservoir.hasSelectedSample == 0u)
    {
        Upt04DiagIncrement(kUpt04DiagCanonicalEmptyWrites);
        return;
    }
    const bool indirect =
        (reservoir.selected.flags & kUpt04CandidateFlagGlobal) != 0u;
    if (!indirect)
        Upt04DiagIncrement(kUpt04DiagSelectedPrimaryNee);
    else if (reservoir.selected.eventKind == 4u)
        Upt04DiagIncrement(kUpt04DiagSelectedBsdfEndpoint);
    else
        Upt04DiagIncrement(kUpt04DiagSelectedSecondaryNee);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gUpt04Control.renderWidth || pixel.y >= gUpt04Control.renderHeight)
        return;
    const uint surfaceIndex = pixel.y * gUpt04Control.renderWidth + pixel.x;
    if (surfaceIndex >= gUpt04Control.surfaceCount)
        return;

    Upt04InitialTraceWork receiverWork;
    Upt04PrimarySurfaceRecord receiverSurface;
    if (!Upt04LoadInitialSurface(pixel, receiverWork, receiverSurface))
    {
        Upt04DiagIncrement(kUpt04DiagReceiverInvalid);
        gUpt04Reservoirs[surfaceIndex] = (Upt03PackedReservoir)0;
        Upt04DiagIncrement(kUpt04DiagCanonicalEmptyWrites);
        return;
    }
    Upt04DiagIncrement(kUpt04DiagReceiverValid);

    Upt03LogicalReservoir reservoir = (Upt03LogicalReservoir)0;
    Upt04BoundedTransportState transport = Upt04BeginBoundedTransport();

    const bool directEnabled =
        (gUpt04Control.enabledFamilyMask & kUpt04FamilyLocalLight) != 0u;
    Upt04InitialTraceWork direct = Upt04PrepareDirectPathTreeWork(pixel);
    Upt04HitFacts hit = Upt04MissFacts(direct.ray.tMax);
    Upt04DiagRay(direct, false);
    if (direct.valid != 0u && direct.traceRequired != 0u)
        hit = Upt04TraceRayQuery(direct.ray);
    direct = Upt04AdvanceInitialTraceWork(direct, hit);
    if (directEnabled)
        Upt04DiagCandidate(direct, true);
    Upt04StreamInitialTraceWork(reservoir, direct, pixel, 0u);
    if (direct.valid != 0u)
        transport = direct.transport;
    if (!Upt04CompletePrimaryNeeStage(transport))
    {
        Upt04DiagSelection(reservoir);
        Upt04WriteInitialReservoir(surfaceIndex, reservoir);
        return;
    }

    const bool indirectEnabled =
        (gUpt04Control.enabledFamilyMask & kUpt04FamilyIndirect) != 0u;
    Upt04InitialTraceWork continuation =
        Upt04PrepareIndirectPathTreeWork(pixel, transport);
    hit = Upt04MissFacts(continuation.ray.tMax);
    Upt04DiagRay(continuation, true);
    if (continuation.valid != 0u && continuation.traceRequired != 0u)
        hit = Upt04TraceRayQuery(continuation.ray);
    const Upt04IndirectHitTrialResult indirect =
        Upt04PrepareIndirectHitTrials(continuation, hit, pixel);
    if (indirectEnabled)
        Upt04DiagCandidate(indirect.endpointCandidate, false);
    Upt04StreamInitialTraceWork(
        reservoir, indirect.endpointCandidate, pixel, 1u);

    Upt04InitialTraceWork secondaryNee = indirect.neeWork;
    hit = Upt04MissFacts(secondaryNee.ray.tMax);
    Upt04DiagRay(secondaryNee, false);
    if (secondaryNee.valid != 0u && secondaryNee.traceRequired != 0u)
        hit = Upt04TraceRayQuery(secondaryNee.ray);
    secondaryNee = Upt04AdvanceInitialTraceWork(secondaryNee, hit);
    if (indirectEnabled)
        Upt04DiagCandidate(secondaryNee, false);
    Upt04StreamInitialTraceWork(reservoir, secondaryNee, pixel, 2u);
    Upt04DiagSelection(reservoir);
    Upt04WriteInitialReservoir(surfaceIndex, reservoir);
}
