#include "upt00_contracts.hlsli"

[[vk::binding(0, 0)]] RaytracingAccelerationStructure gScene;
[[vk::binding(1, 0)]] StructuredBuffer<Upt00RayInput> gRays;
[[vk::binding(2, 0)]] RWStructuredBuffer<Upt00RayOutput> gOutput;

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;
    const Upt00RayInput input = gRays[index];

    RayDesc ray;
    ray.Origin = input.origin;
    ray.TMin = input.tMin;
    ray.Direction = input.direction;
    ray.TMax = input.tMax;

    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(gScene, RAY_FLAG_NONE, 0xffu, ray);
    while (query.Proceed())
    {
    }

    Upt00RayOutput output;
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        output.hit = 1u;
        output.instanceId = query.CommittedInstanceID();
        output.primitiveId = query.CommittedPrimitiveIndex();
        output.geometryId = query.CommittedGeometryIndex();
        output.hitT = query.CommittedRayT();
        output.barycentrics = query.CommittedTriangleBarycentrics();
        output.frontFace = query.CommittedTriangleFrontFace() ? 1u : 0u;
    }
    else
    {
        output.hit = 0u;
        output.instanceId = 0xffffffffu;
        output.primitiveId = 0xffffffffu;
        output.geometryId = 0xffffffffu;
        output.hitT = input.tMax;
        output.barycentrics = float2(0.0f, 0.0f);
        output.frontFace = 0u;
    }
    gOutput[index] = output;
}
