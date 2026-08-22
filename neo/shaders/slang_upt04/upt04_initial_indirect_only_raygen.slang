import upt04_hit_contract;
import upt04_initial_bindings;
import upt04_initial_shared;
import upt04_initial_work_contract;
import upt03_reservoir_contract;
import upt04_transport_initial_bounded;
import upt04_initial_indirect;
import upt04_initial_reservoir_stream;
import upt05_material_set_openpbr;

public void Upt04TraceIndirectRaygen(
    Upt04InitialTraceWork work,
    inout Upt04HitFacts hit)
{
    if (work.valid == 0u || work.traceRequired == 0u)
        return;
    RayDesc ray;
    ray.Origin = work.ray.origin;
    ray.TMin = work.ray.tMin;
    ray.Direction = work.ray.direction;
    ray.TMax = work.ray.tMax;
    const uint rayFlags = work.transport.stage == kUpt04TransportIndirectContinuation
        ? RAY_FLAG_FORCE_OPAQUE
        : (RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH);
    TraceRay(gUpt04Scene,
        rayFlags,
        0xffu, 0u, 0u, 0u, ray, hit);
}

[shader("raygeneration")]
void main()
{
    const uint2 pixel = DispatchRaysIndex().xy;
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
    Upt04TraceIndirectRaygen(continuation, hit);
    const Upt04IndirectHitTrialResult indirect =
        Upt04PrepareIndirectHitTrials<Upt05OpenPbrMaterialSet>(
            continuation, hit, pixel, Upt04EmptyLocalLightSelection());
    Upt04StreamInitialTraceWork(
        reservoir, indirect.endpointCandidate, pixel, 0u);

    Upt04InitialTraceWork secondaryNee = indirect.neeWork;
    hit = Upt04MissFacts(secondaryNee.ray.tMax);
    Upt04TraceIndirectRaygen(secondaryNee, hit);
    secondaryNee = Upt04AdvanceInitialTraceWork(secondaryNee, hit);
    Upt04StreamInitialTraceWork(reservoir, secondaryNee, pixel, 1u);
    Upt04WriteInitialReservoir(surfaceIndex, reservoir);
}