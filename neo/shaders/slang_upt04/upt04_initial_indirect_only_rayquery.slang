import upt04_backend_rayquery;
import upt04_hit_contract;
import upt04_initial_bindings;
import upt04_initial_shared;
import upt04_initial_work_contract;

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gUpt04Control.renderWidth || pixel.y >= gUpt04Control.renderHeight)
        return;
    if (pixel.y * gUpt04Control.renderWidth + pixel.x >= gUpt04Control.surfaceCount)
        return;
    Upt04InitialTraceWork work = Upt04PrepareStaticInitialFamilyTraceWork(
        pixel,
        kUpt04InitialFamilyIndirect);
    Upt04HitFacts hit = Upt04MissFacts(work.ray.tMax);
    if (work.valid != 0u && work.traceRequired != 0u)
        hit = Upt04TraceRayQuery(work.ray);
    work = Upt04AdvanceInitialTraceWork(work, hit);
    hit = Upt04MissFacts(work.ray.tMax);
    if (work.valid != 0u && work.traceRequired != 0u)
        hit = Upt04TraceRayQuery(work.ray);
    work = Upt04AdvanceInitialTraceWork(work, hit);
    Upt04FinishInitialTraceWork(work);
}
