import upt03_reservoir_contract;
import upt04_backend_rayquery;
import upt04_hit_contract;
import upt04_initial_bindings;
import upt04_initial_direct_only_shared;
import upt04_initial_work_contract;
import upt04_primary_surface_live_contract;
import upt04_initial_reservoir_stream;

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gUpt04Control.renderWidth || pixel.y >= gUpt04Control.renderHeight)
        return;
    if (pixel.y * gUpt04Control.renderWidth + pixel.x >= gUpt04Control.surfaceCount)
        return;
    const uint surfaceIndex = pixel.y * gUpt04Control.renderWidth + pixel.x;
    if (gUpt04Control.shaderProofMode <= 1u)
    {
        gUpt04Reservoirs[surfaceIndex] = (Upt03PackedReservoir)0;
        return;
    }
    if (gUpt04Control.shaderProofMode == 2u)
    {
        const Upt04PrimarySurfaceRecord surface = gUpt04PrimarySurfaces[surfaceIndex];
        Upt03PackedReservoir proof = (Upt03PackedReservoir)0;
        proof.word15 = surface.header.x;
        gUpt04Reservoirs[surfaceIndex] = proof;
        return;
    }
    Upt04InitialTraceWork work = Upt04PrepareStaticInitialFamilyTraceWork(
        pixel,
        kUpt04InitialFamilyDirect);
    if (gUpt04Control.shaderProofMode == 3u)
    {
        Upt04FinishInitialTraceWork(work);
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
            gUpt04Control.shaderProofMode >= 6u);
    if (fixedRayProof)
    {
        Upt03PackedReservoir proof = (Upt03PackedReservoir)0;
        proof.word15 = hit.status;
        gUpt04Reservoirs[surfaceIndex] = proof;
        return;
    }
    work = Upt04AdvanceInitialTraceWork(work, hit);
    Upt04FinishInitialTraceWork(work);
}
