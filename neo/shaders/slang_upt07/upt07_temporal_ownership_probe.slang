// UPT-07 pre-production temporal ownership proof.
//
// This kernel deliberately performs no path shift, target re-evaluation, or
// ray work and is not copied into the runtime shader directory.  It proves the
// two-page access pattern before those costs are admitted: each invocation
// reads only its own current record and exact reprojected history record, then
// overwrites only its own current record.

import upt03_reservoir_contract;
import upt04_primary_surface_live_contract;
import upt07_temporal_surface_contract;

public struct Upt07TemporalProofControl
{
    public uint renderWidth;
    public uint renderHeight;
    public uint surfaceCount;
    public uint historyAvailable;
    public float4 previousCameraOriginAndValid;
    public float4 previousCameraForwardAndTanX;
    public float4 previousCameraLeftAndTanY;
    public float4 previousCameraUpAndReserved;
};

[[vk::binding(0, 0)]] StructuredBuffer<Upt04PrimarySurfaceRecord> gUpt07CurrentSurfaces;
[[vk::binding(1, 0)]] StructuredBuffer<Upt04PrimarySurfaceRecord> gUpt07PreviousSurfaces;
[[vk::binding(2, 0)]] RWStructuredBuffer<Upt03PackedReservoir> gUpt07CurrentReservoirs;
[[vk::binding(3, 0)]] StructuredBuffer<Upt03PackedReservoir> gUpt07HistoryReservoirs;
[[vk::push_constant]] ConstantBuffer<Upt07TemporalProofControl> gUpt07Control;

public bool Upt07Selected(Upt03PackedReservoir reservoir)
{
    return (reservoir.word0 & 0x0fu) == kUpt03PackedSelectedTag
        && reservoir.word1 != 0u && reservoir.word2 != 0u;
}

public Upt03PackedReservoir Upt07RequestRescue(Upt03PackedReservoir current)
{
    if (Upt07Selected(current))
        return current;
    const uint lowTag = current.word0 & 0x03u;
    if (lowTag == 0u || lowTag == kUpt03PackedCountOnlyTag)
        current.word0 |= kUpt03PackedNeedsRescueBit;
    return current;
}

public bool Upt07FindHistory(
    Upt04PrimarySurfaceRecord current,
    out Upt03PackedReservoir history)
{
    history = (Upt03PackedReservoir)0;
    // historyAvailable is host-uniform page metadata admission. No previous
    // surface or reservoir element is read before it succeeds.
    if (gUpt07Control.historyAvailable == 0u)
        return false;
    Upt07PreviousCamera camera;
    camera.origin = gUpt07Control.previousCameraOriginAndValid.xyz;
    camera.valid = gUpt07Control.previousCameraOriginAndValid.w >= 0.5f ? 1u : 0u;
    camera.forward = gUpt07Control.previousCameraForwardAndTanX.xyz;
    camera.tanX = gUpt07Control.previousCameraForwardAndTanX.w;
    camera.left = gUpt07Control.previousCameraLeftAndTanY.xyz;
    camera.tanY = gUpt07Control.previousCameraLeftAndTanY.w;
    camera.up = gUpt07Control.previousCameraUpAndReserved.xyz;
    camera.historySearchMode = 0u;
    const uint2 dimensions = uint2(
        gUpt07Control.renderWidth, gUpt07Control.renderHeight);
    const Upt07PreviousProjection projection = Upt07ProjectPrevious(
        current, camera, dimensions);
    if (projection.valid == 0u)
        return false;
    const int2 pixel = projection.pixel;
    const uint index = uint(pixel.y) * gUpt07Control.renderWidth
        + uint(pixel.x);
    const Upt04PrimarySurfaceRecord previous = gUpt07PreviousSurfaces[index];
    if (!Upt07SurfacesCompatible(current, previous, projection.linearDepth))
        return false;
    const Upt03PackedReservoir candidate = gUpt07HistoryReservoirs[index];
    if (!Upt07Selected(candidate))
        return false;
    history = candidate;
    return true;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gUpt07Control.renderWidth
        || pixel.y >= gUpt07Control.renderHeight)
        return;
    const uint index = pixel.y * gUpt07Control.renderWidth + pixel.x;
    if (index >= gUpt07Control.surfaceCount)
        return;

    Upt03PackedReservoir current = gUpt07CurrentReservoirs[index];
    const Upt04PrimarySurfaceRecord receiver = gUpt07CurrentSurfaces[index];
    if (!Upt04PrimarySurfaceRecordValid(receiver))
    {
        gUpt07CurrentReservoirs[index] = current;
        return;
    }
    Upt03PackedReservoir history;
    if (!Upt07FindHistory(receiver, history))
    {
        gUpt07CurrentReservoirs[index] = Upt07RequestRescue(current);
        return;
    }
    if (!Upt07Selected(current))
    {
        // Proof-only recovery. A production UPT-07 kernel must replace this
        // copy with current-domain shift/re-evaluation and weighted merging.
        const uint oldAge = (history.word0 >> 18u) & 0x3fu;
        history.word0 = (history.word0 & ~(0x3fu << 18u))
            | (min(oldAge + 1u, 63u) << 18u);
        gUpt07CurrentReservoirs[index] = history;
        return;
    }
    gUpt07CurrentReservoirs[index] = current;
}
