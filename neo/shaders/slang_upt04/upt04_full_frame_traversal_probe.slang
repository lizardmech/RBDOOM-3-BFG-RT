import upt04_initial_control;
import upt04_primary_surface_live_contract;

// Full-frame traversal-isolation proof.  This deliberately does not import
// material, light, reservoir, geometry-decode, or initial-sampling modules.
// Each valid primary receiver contributes one opaque inline ray and one uint
// result so Nsight can distinguish traversal cost from D0 live-state pressure.
[[vk::binding(0, 0)]] RaytracingAccelerationStructure gScene;
[[vk::binding(1, 0)]] StructuredBuffer<Upt04PrimarySurfaceRecord> gPrimarySurfaces;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> gResults;
[[vk::push_constant]] ConstantBuffer<Upt04InitialControl> gControl;

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gControl.renderWidth || pixel.y >= gControl.renderHeight)
        return;

    const uint surfaceIndex = pixel.y * gControl.renderWidth + pixel.x;
    if (surfaceIndex >= gControl.surfaceCount)
        return;

    const uint4 header = gPrimarySurfaces[surfaceIndex].header;
    const float3 origin =
        gPrimarySurfaces[surfaceIndex].worldPositionAndViewDepth.xyz;
    const float3 geometricNormal =
        gPrimarySurfaces[surfaceIndex].geometricNormalAndRoughness.xyz;
    if (header.x != kUpt04PrimarySurfaceVersion ||
        (header.y & kUpt04PrimarySurfaceValid) == 0u ||
        !all(isfinite(origin)) || !all(isfinite(geometricNormal)))
    {
        gResults[surfaceIndex] = 0u;
        return;
    }

    RayDesc ray;
    ray.Origin = origin;
    ray.TMin = 0.01f;
    ray.Direction = normalize(geometricNormal);
    ray.TMax = 10000.0f;

    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(gScene, RAY_FLAG_NONE, 0xffu, ray);
    while (query.Proceed()) {}

    gResults[surfaceIndex] =
        query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 1u : 0u;
}
