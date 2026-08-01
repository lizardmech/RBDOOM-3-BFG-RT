import upt04_initial_shared;
import upt04_initial_bindings;
import upt04_hit_contract;

[shader("raygeneration")]
void main()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    if (pixel.x >= gUpt04Control.renderWidth || pixel.y >= gUpt04Control.renderHeight)
        return;
    if (pixel.y * gUpt04Control.renderWidth + pixel.x >= gUpt04Control.surfaceCount)
        return;
    const Upt04InitialTraceWork work = Upt04PrepareInitialTraceWork(pixel);
    Upt04HitFacts hit = Upt04MissFacts(work.ray.tMax);
    if (work.valid != 0u && work.traceRequired != 0u)
    {
        RayDesc ray;
        ray.Origin = work.ray.origin;
        ray.TMin = work.ray.tMin;
        ray.Direction = work.ray.direction;
        ray.TMax = work.ray.tMax;
        TraceRay(gUpt04Scene,
            RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
            0xffu, 0u, 0u, 0u, ray, hit);
    }
    Upt04FinishInitialTraceWork(work, hit);
}
