import upt04_backend_rayquery;
import upt04_continuation_hit_bindings;
import upt04_hit_contract;
import upt04_initial_bindings;
import upt04_initial_shared;
import upt04_initial_work_contract;
import upt04_primary_surface_live_contract;
import upt04_transport_initial_bounded;
import upt05_material_set_openpbr;

// C0 owns only deterministic primary-continuation generation and one inline
// trace. Material/geometry shading and reservoir work remain in D0.
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gUpt04Control.renderWidth
        || pixel.y >= gUpt04Control.renderHeight)
        return;
    const uint surfaceIndex = pixel.y * gUpt04Control.renderWidth + pixel.x;
    if (surfaceIndex >= gUpt04Control.surfaceCount)
        return;

    Upt04HitFacts hit = Upt04MissFacts(0.0f);
    Upt04InitialTraceWork receiverWork;
    Upt04PrimarySurfaceRecord primarySurface;
    if (Upt04LoadInitialSurface(pixel, receiverWork, primarySurface))
    {
        Upt04BoundedTransportState transport = Upt04BeginBoundedTransport();
        if (Upt04CompletePrimaryNeeStage(transport))
        {
            const Upt04InitialTraceWork continuation =
                Upt04PrepareIndirectPathTreeWorkFromSurface<
                    Upt05OpenPbrMaterialSet>(
                        pixel,
                        receiverWork.surfaceIndex,
                        primarySurface,
                        transport);
            hit = Upt04MissFacts(continuation.ray.tMax);
            if (continuation.valid != 0u && continuation.traceRequired != 0u)
                hit = Upt04TraceRayQuery(continuation.ray);
        }
    }
    gUpt04ContinuationHits[surfaceIndex] = hit;
}
