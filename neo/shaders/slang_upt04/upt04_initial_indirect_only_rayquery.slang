import upt04_backend_rayquery;
import upt04_hit_contract;
import upt04_initial_bindings;
import upt04_initial_shared;
import upt04_initial_work_contract;
import upt03_reservoir_contract;
import upt04_transport_initial_bounded;
import upt04_initial_indirect;
import upt04_initial_reservoir_stream;
import upt05_material_set_openpbr;

#ifndef UPT04_SPLIT_INITIAL_INDIRECT
#define UPT04_SPLIT_INITIAL_INDIRECT 0
#endif

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gUpt04Control.renderWidth || pixel.y >= gUpt04Control.renderHeight)
        return;
    const uint surfaceIndex = pixel.y * gUpt04Control.renderWidth + pixel.x;
    if (surfaceIndex >= gUpt04Control.surfaceCount)
        return;

    uint directVisibilityRays = 0u;
#if UPT04_SPLIT_INITIAL_INDIRECT
    Upt03LogicalReservoir reservoir = Upt04LoadIntermediateInitialReservoir(
        surfaceIndex,
        directVisibilityRays);
#else
    Upt03LogicalReservoir reservoir = (Upt03LogicalReservoir)0;
#endif
    Upt04BoundedTransportState transport = Upt04BeginBoundedTransport();
#if UPT04_SPLIT_INITIAL_INDIRECT
    transport.budget.visibilityRays = directVisibilityRays;
#endif
    if (!Upt04CompletePrimaryNeeStage(transport))
    {
        Upt04WriteInitialReservoir(surfaceIndex, reservoir);
        return;
    }
    Upt04InitialTraceWork continuation =
        Upt04PrepareIndirectPathTreeWork<Upt05OpenPbrMaterialSet>(pixel, transport);
    Upt04HitFacts hit = Upt04MissFacts(continuation.ray.tMax);
    if (continuation.valid != 0u && continuation.traceRequired != 0u)
        hit = Upt04TraceRayQuery(continuation.ray);
    const Upt04IndirectHitTrialResult indirect =
        Upt04PrepareIndirectHitTrials<Upt05OpenPbrMaterialSet>(
            continuation, hit, pixel);
    Upt04StreamInitialTraceWork(
        reservoir,
        indirect.endpointCandidate,
        pixel,
#if UPT04_SPLIT_INITIAL_INDIRECT
        1u);
#else
        0u);
#endif

    Upt04InitialTraceWork secondaryNee = indirect.neeWork;
    hit = Upt04MissFacts(secondaryNee.ray.tMax);
    if (secondaryNee.valid != 0u && secondaryNee.traceRequired != 0u)
        hit = Upt04TraceIndirectVisibilityRayQuery(
            secondaryNee.ray,
            Upt04IndirectNeeVisibilityIgnore(secondaryNee));
    secondaryNee = Upt04AdvanceInitialTraceWork(secondaryNee, hit);
    Upt04StreamInitialTraceWork(
        reservoir,
        secondaryNee,
        pixel,
#if UPT04_SPLIT_INITIAL_INDIRECT
        2u);
#else
        1u);
#endif
    Upt04WriteInitialReservoir(surfaceIndex, reservoir);
}
