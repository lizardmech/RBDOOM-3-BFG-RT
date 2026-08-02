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
import upt05_material_set_openpbr;
import upt04_random_white_hash;

// This entry point is intentionally separate from every production entry.
// The first 16 words are the bring-up counters.  Words 16..19 are a
// group-reduced, order-independent signature of every packed reservoir.
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
public static const uint kUpt04DiagReservoirHash0 = 16u;
public static const uint kUpt04DiagReservoirHash1 = 17u;
public static const uint kUpt04DiagReservoirHash2 = 18u;
public static const uint kUpt04DiagReservoirHash3 = 19u;

groupshared uint gUpt04GroupReservoirHash0;
groupshared uint gUpt04GroupReservoirHash1;
groupshared uint gUpt04GroupReservoirHash2;
groupshared uint gUpt04GroupReservoirHash3;

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

public uint Upt04DiagMixReservoirWord(uint hash, uint word, uint salt)
{
    return Upt04HashWord(hash ^ Upt04HashWord(word + salt));
}

public uint4 Upt04DiagReservoirHash(
    uint surfaceIndex,
    Upt03PackedReservoir packed)
{
    uint4 hash = uint4(
        Upt04HashWord(surfaceIndex ^ 0x243f6a88u),
        Upt04HashWord(surfaceIndex ^ 0x85a308d3u),
        Upt04HashWord(surfaceIndex ^ 0x13198a2eu),
        Upt04HashWord(surfaceIndex ^ 0x03707344u));
#define UPT04_DIAG_MIX_WORD(field, ordinal) \
    hash.x = Upt04DiagMixReservoirWord(hash.x, packed.field, 0x9e3779b9u * (ordinal)); \
    hash.y = Upt04DiagMixReservoirWord(hash.y, packed.field, 0x7f4a7c15u * (17u - (ordinal))); \
    hash.z = Upt04DiagMixReservoirWord(hash.z, packed.field, 0x94d049bbu ^ (ordinal)); \
    hash.w = Upt04DiagMixReservoirWord(hash.w, packed.field, 0x5bd1e995u + (ordinal));
    UPT04_DIAG_MIX_WORD(word0, 1u)
    UPT04_DIAG_MIX_WORD(word1, 2u)
    UPT04_DIAG_MIX_WORD(word2, 3u)
    UPT04_DIAG_MIX_WORD(word3, 4u)
    UPT04_DIAG_MIX_WORD(word4, 5u)
    UPT04_DIAG_MIX_WORD(word5, 6u)
    UPT04_DIAG_MIX_WORD(word6, 7u)
    UPT04_DIAG_MIX_WORD(word7, 8u)
    UPT04_DIAG_MIX_WORD(word8, 9u)
    UPT04_DIAG_MIX_WORD(word9, 10u)
    UPT04_DIAG_MIX_WORD(word10, 11u)
    UPT04_DIAG_MIX_WORD(word11, 12u)
    UPT04_DIAG_MIX_WORD(word12, 13u)
    UPT04_DIAG_MIX_WORD(word13, 14u)
    UPT04_DIAG_MIX_WORD(word14, 15u)
    UPT04_DIAG_MIX_WORD(word15, 16u)
#undef UPT04_DIAG_MIX_WORD
    return hash;
}

public Upt03PackedReservoir Upt04BuildDiagnosticReservoir(
    uint2 pixel,
    uint surfaceIndex)
{
    Upt03PackedReservoir packedEmpty = (Upt03PackedReservoir)0;
    Upt04InitialTraceWork receiverWork;
    Upt04PrimarySurfaceRecord receiverSurface;
    if (!Upt04LoadInitialSurface(pixel, receiverWork, receiverSurface))
    {
        Upt04DiagIncrement(kUpt04DiagReceiverInvalid);
        Upt04DiagIncrement(kUpt04DiagCanonicalEmptyWrites);
        return packedEmpty;
    }
    Upt04DiagIncrement(kUpt04DiagReceiverValid);

    Upt03LogicalReservoir reservoir = (Upt03LogicalReservoir)0;
    Upt04BoundedTransportState transport = Upt04BeginBoundedTransport();

    const bool directEnabled =
        (gUpt04Control.enabledFamilyMask & kUpt04FamilyLocalLight) != 0u;
    Upt04InitialTraceWork direct =
        Upt04PrepareDirectPathTreeWork<Upt05OpenPbrMaterialSet>(pixel);
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
        return Upt04FinalizeInitialReservoir(reservoir);
    }

    const bool indirectEnabled =
        (gUpt04Control.enabledFamilyMask & kUpt04FamilyIndirect) != 0u;
    Upt04InitialTraceWork continuation =
        Upt04PrepareIndirectPathTreeWork<Upt05OpenPbrMaterialSet>(pixel, transport);
    hit = Upt04MissFacts(continuation.ray.tMax);
    Upt04DiagRay(continuation, true);
    if (continuation.valid != 0u && continuation.traceRequired != 0u)
        hit = Upt04TraceRayQuery(continuation.ray);
    const Upt04IndirectHitTrialResult indirect =
        Upt04PrepareIndirectHitTrials<Upt05OpenPbrMaterialSet>(
            continuation, hit, pixel);
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
    return Upt04FinalizeInitialReservoir(reservoir);
}

[numthreads(8, 8, 1)]
void main(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint groupIndex : SV_GroupIndex)
{
    if (groupIndex == 0u)
    {
        gUpt04GroupReservoirHash0 = 0u;
        gUpt04GroupReservoirHash1 = 0u;
        gUpt04GroupReservoirHash2 = 0u;
        gUpt04GroupReservoirHash3 = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    const uint2 pixel = dispatchThreadId.xy;
    const uint surfaceIndex = pixel.y * gUpt04Control.renderWidth + pixel.x;
    if (pixel.x < gUpt04Control.renderWidth
        && pixel.y < gUpt04Control.renderHeight
        && surfaceIndex < gUpt04Control.surfaceCount)
    {
        const Upt03PackedReservoir packed =
            Upt04BuildDiagnosticReservoir(pixel, surfaceIndex);
        gUpt04Reservoirs[surfaceIndex] = packed;
        const uint4 hash = Upt04DiagReservoirHash(surfaceIndex, packed);
        uint ignored;
        InterlockedXor(gUpt04GroupReservoirHash0, hash.x, ignored);
        InterlockedXor(gUpt04GroupReservoirHash1, hash.y, ignored);
        InterlockedXor(gUpt04GroupReservoirHash2, hash.z, ignored);
        InterlockedXor(gUpt04GroupReservoirHash3, hash.w, ignored);
    }
    GroupMemoryBarrierWithGroupSync();
    if (groupIndex == 0u)
    {
        uint ignored;
        InterlockedXor(
            gUpt04Diagnostics[kUpt04DiagReservoirHash0],
            gUpt04GroupReservoirHash0,
            ignored);
        InterlockedXor(
            gUpt04Diagnostics[kUpt04DiagReservoirHash1],
            gUpt04GroupReservoirHash1,
            ignored);
        InterlockedXor(
            gUpt04Diagnostics[kUpt04DiagReservoirHash2],
            gUpt04GroupReservoirHash2,
            ignored);
        InterlockedXor(
            gUpt04Diagnostics[kUpt04DiagReservoirHash3],
            gUpt04GroupReservoirHash3,
            ignored);
    }
}
