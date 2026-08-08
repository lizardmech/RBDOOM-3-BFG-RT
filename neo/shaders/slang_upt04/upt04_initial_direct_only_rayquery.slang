import upt03_reservoir_contract;
import upt04_backend_rayquery;
import upt04_hit_contract;
import upt04_initial_bindings;
import upt04_initial_direct_only_shared;
import upt04_initial_direct;
import upt04_initial_work_contract;
import upt04_primary_surface_live_contract;
import upt04_initial_reservoir_stream;
import upt05_material_set_openpbr;

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
        const Upt04PrimarySurfaceRecord surface =
            Upt04LoadPrimarySurfaceRecord(surfaceIndex);
        Upt03PackedReservoir proof = (Upt03PackedReservoir)0;
        proof.word15 = surface.header.x;
        gUpt04Reservoirs[surfaceIndex] = proof;
        return;
    }
    Upt04InitialTraceWork work =
        Upt04PrepareStaticInitialFamilyTraceWork<Upt05OpenPbrMaterialSet>(
        pixel,
        kUpt04InitialFamilyDirect);
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
        gUpt04Reservoirs[surfaceIndex] = proof;
        return;
    }
    work = Upt04AdvanceInitialTraceWork(work, hit);
    Upt04FinishDirectPass(work);
}
