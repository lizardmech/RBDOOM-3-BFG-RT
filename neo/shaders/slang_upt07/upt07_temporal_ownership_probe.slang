// UPT-07 pre-production temporal ownership proof.
//
// This kernel deliberately performs no path shift, target re-evaluation, or
// ray work and is not copied into the runtime shader directory.  It proves the
// two-page access pattern before those costs are admitted: each invocation
// reads only its own current record, searches a bounded neighborhood in the
// other physical history page, and overwrites only its own current record.

import upt03_reservoir_contract;
import upt04_primary_surface_live_contract;

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

public static const int2 kUpt07HistoryTap0 = int2(0, 0);
public static const int2 kUpt07HistoryTap1 = int2(-1, 0);
public static const int2 kUpt07HistoryTap2 = int2(1, 0);
public static const int2 kUpt07HistoryTap3 = int2(0, -1);
public static const int2 kUpt07HistoryTap4 = int2(0, 1);

public int2 Upt07HistoryTap(uint index)
{
    if (index == 1u) return kUpt07HistoryTap1;
    if (index == 2u) return kUpt07HistoryTap2;
    if (index == 3u) return kUpt07HistoryTap3;
    if (index == 4u) return kUpt07HistoryTap4;
    return kUpt07HistoryTap0;
}

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

public bool Upt07ProjectPrevious(
    Upt04PrimarySurfaceRecord current,
    out int2 previousPixel,
    out float previousDepth)
{
    previousPixel = int2(-1, -1);
    previousDepth = 0.0f;
    if (gUpt07Control.previousCameraOriginAndValid.w < 0.5f)
        return false;
    const float3 previousPosition = current.previousPositionOrMotion.w >= 0.5f
        ? current.previousPositionOrMotion.xyz
        : current.worldPositionAndViewDepth.xyz;
    const float3 delta = previousPosition
        - gUpt07Control.previousCameraOriginAndValid.xyz;
    previousDepth = dot(delta, gUpt07Control.previousCameraForwardAndTanX.xyz);
    if (!isfinite(previousDepth) || previousDepth <= 0.05f)
        return false;
    const float tanX = gUpt07Control.previousCameraForwardAndTanX.w;
    const float tanY = gUpt07Control.previousCameraLeftAndTanY.w;
    if (!isfinite(tanX) || !isfinite(tanY) || tanX <= 0.0f || tanY <= 0.0f)
        return false;
    const float2 ndc = float2(
        -dot(delta, gUpt07Control.previousCameraLeftAndTanY.xyz)
            / (previousDepth * tanX),
        -dot(delta, gUpt07Control.previousCameraUpAndReserved.xyz)
            / (previousDepth * tanY));
    if (!all(isfinite(ndc)) || any(abs(ndc) > 1.0f))
        return false;
    const float2 pixelFloat = (ndc * 0.5f + 0.5f)
        * float2(gUpt07Control.renderWidth, gUpt07Control.renderHeight);
    previousPixel = int2(floor(pixelFloat));
    return previousPixel.x >= 0 && previousPixel.y >= 0
        && uint(previousPixel.x) < gUpt07Control.renderWidth
        && uint(previousPixel.y) < gUpt07Control.renderHeight;
}

public bool Upt07SurfacesCompatible(
    Upt04PrimarySurfaceRecord current,
    Upt04PrimarySurfaceRecord previous,
    float projectedPreviousDepth)
{
    if (!Upt04PrimarySurfaceRecordValid(previous)
        || current.materialAndSurface.x != previous.materialAndSurface.x
        || current.materialAndSurface.y != previous.materialAndSurface.y
        || current.materialAndSurface.w != previous.materialAndSurface.w)
        return false;
    const float3 currentNormal = normalize(current.geometricNormalAndRoughness.xyz);
    const float3 previousNormal = normalize(previous.geometricNormalAndRoughness.xyz);
    if (!all(isfinite(currentNormal)) || !all(isfinite(previousNormal))
        || dot(currentNormal, previousNormal) < 0.85f
        || abs(current.geometricNormalAndRoughness.w
            - previous.geometricNormalAndRoughness.w) > 0.20f)
        return false;
    const float depthTolerance = max(0.10f, abs(projectedPreviousDepth) * 0.10f);
    return isfinite(previous.worldPositionAndViewDepth.w)
        && abs(previous.worldPositionAndViewDepth.w - projectedPreviousDepth)
            <= depthTolerance;
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
    int2 center;
    float previousDepth;
    if (!Upt07ProjectPrevious(current, center, previousDepth))
        return false;
    [ForceUnroll]
    for (uint tap = 0u; tap < 5u; ++tap)
    {
        const int2 pixel = center + Upt07HistoryTap(tap);
        if (pixel.x < 0 || pixel.y < 0
            || uint(pixel.x) >= gUpt07Control.renderWidth
            || uint(pixel.y) >= gUpt07Control.renderHeight)
            continue;
        const uint index = uint(pixel.y) * gUpt07Control.renderWidth
            + uint(pixel.x);
        const Upt04PrimarySurfaceRecord previous = gUpt07PreviousSurfaces[index];
        if (!Upt07SurfacesCompatible(current, previous, previousDepth))
            continue;
        const Upt03PackedReservoir candidate = gUpt07HistoryReservoirs[index];
        if (!Upt07Selected(candidate))
            continue;
        history = candidate;
        return true;
    }
    return false;
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
