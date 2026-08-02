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

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gUpt04Control.renderWidth || pixel.y >= gUpt04Control.renderHeight)
        return;
    const uint surfaceIndex = pixel.y * gUpt04Control.renderWidth + pixel.x;
    if (surfaceIndex >= gUpt04Control.surfaceCount)
        return;

    Upt03LogicalReservoir reservoir = (Upt03LogicalReservoir)0;
    Upt04BoundedTransportState transport = Upt04BeginBoundedTransport();
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
        reservoir, indirect.endpointCandidate, pixel, 0u);

    Upt04InitialTraceWork secondaryNee = indirect.neeWork;
    hit = Upt04MissFacts(secondaryNee.ray.tMax);
    if (secondaryNee.valid != 0u && secondaryNee.traceRequired != 0u)
        hit = Upt04TraceRayQuery(secondaryNee.ray);
    secondaryNee = Upt04AdvanceInitialTraceWork(secondaryNee, hit);
    Upt04StreamInitialTraceWork(reservoir, secondaryNee, pixel, 1u);
    Upt04WriteInitialReservoir(surfaceIndex, reservoir);
}
