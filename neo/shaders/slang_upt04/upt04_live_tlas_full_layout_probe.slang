import upt04_geometry_bindings;
import upt04_initial_bindings;
import upt03_reservoir_contract;

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId != uint3(0, 0, 0)) ||
        gUpt04Control.surfaceCount == 0u)
        return;

    RayDesc ray;
    ray.Origin = float3(0.0f, 0.0f, 0.0f);
    ray.TMin = 0.01f;
    ray.Direction = float3(0.0f, 0.0f, 1.0f);
    ray.TMax = 1.0f;

    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(gUpt04Scene, RAY_FLAG_NONE, 0xffu, ray);
    while (query.Proceed()) {}

    Upt03PackedReservoir proof = (Upt03PackedReservoir)0;
    proof.word15 = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 1u : 0u;
    gUpt04Reservoirs[0] = proof;
}
