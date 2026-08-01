[[vk::binding(0, 0)]] RaytracingAccelerationStructure gScene;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> gOutput;

[numthreads(1, 1, 1)]
void main()
{
    RayDesc ray;
    ray.Origin = float3(0.0f, 0.0f, 0.0f);
    ray.TMin = 0.01f;
    ray.Direction = float3(0.0f, 0.0f, 1.0f);
    ray.TMax = 1.0f;

    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(gScene, RAY_FLAG_NONE, 0xffu, ray);
    while (query.Proceed()) {}

    gOutput[0] = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 1u : 0u;
}
