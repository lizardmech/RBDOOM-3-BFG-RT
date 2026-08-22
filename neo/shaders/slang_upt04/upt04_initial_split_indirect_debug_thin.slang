// Debug X produce: same 8x8 dispatch and binding layout, no GI body.
// Load primary + passthrough the split-direct intermediate reservoir.
// If this d0b is still NI-shaped, the cliff is hosting/dispatch, not
// decode/NEE/OpenPBR.
#ifndef UPT04_DEBUG_THIN_RAY
#define UPT04_DEBUG_THIN_RAY 0
#endif
import upt04_initial_bindings;
import upt04_primary_image;
import upt04_initial_control;
import upt04_initial_reservoir_stream;
import upt03_reservoir_contract;
import upt04_primary_surface_live_contract;

public struct Upt04X3Token
{
    public uint4 a;
    public uint4 b;
    public uint4 c;
    public uint4 d;
    public uint4 e;
    public uint4 f;
    public uint4 g;
    public uint4 h;
};

public [[vk::binding(37, 0)]] RWStructuredBuffer<Upt04X3Token> gUpt04X3Queue;
public [[vk::binding(38, 0)]] RWStructuredBuffer<uint> gUpt04X3Meta;
public [[vk::binding(39, 0)]] RWStructuredBuffer<uint> gUpt04X3DispatchArgs;

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

    uint directVisibilityRays = 0u;
    Upt03LogicalReservoir reservoir = Upt04LoadIntermediateInitialReservoir(
        surfaceIndex, directVisibilityRays);
    const Upt04PrimarySurfaceRecord primary =
        Upt04LoadPrimarySurfaceRecord(surfaceIndex);
    if (!Upt04PrimarySurfaceRecordValid(primary))
    {
        Upt04WriteInitialReservoir(surfaceIndex, reservoir);
        return;
    }
#if UPT04_DEBUG_THIN_RAY
    // One closest-hit RayQuery, no material/geometry decode. Keep the
    // committed T live so Slang cannot erase the trace.
    const float3 origin = primary.worldPositionAndViewDepth.xyz
        + primary.geometricNormalAndRoughness.xyz * 0.001f;
    const float3 direction = primary.geometricNormalAndRoughness.xyz;
    RayDesc ray;
    ray.Origin = origin;
    ray.TMin = 0.001f;
    ray.Direction = direction;
    ray.TMax = 1.0e4f;
    RayQuery<0> query = RayQuery<0>();
    query.TraceRayInline(gUpt04Scene, RAY_FLAG_NONE, 0xffu, ray);
    while (query.Proceed())
    {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
            query.CommitNonOpaqueTriangleHit();
    }
    float keep = 0.0f;
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        keep = query.CommittedRayT();
    if (asuint(keep) == gUpt04Control.surfaceCount)
        return;
#endif
    Upt04WriteInitialReservoir(surfaceIndex, reservoir);
}