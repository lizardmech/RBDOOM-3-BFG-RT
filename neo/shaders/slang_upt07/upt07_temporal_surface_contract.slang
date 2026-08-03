import upt04_primary_surface_live_contract;

public struct Upt07PreviousCamera
{
    public float3 origin;
    public uint valid;
    public float3 forward;
    public float tanX;
    public float3 left;
    public float tanY;
    public float3 up;
    public uint reserved;
};

public struct Upt07PreviousProjection
{
    public int2 pixel;
    public float viewDepth;
    public uint valid;
};

public int2 Upt07HistoryTap(uint index)
{
    if (index == 1u) return int2(-1, 0);
    if (index == 2u) return int2(1, 0);
    if (index == 3u) return int2(0, -1);
    if (index == 4u) return int2(0, 1);
    return int2(0, 0);
}

public Upt07PreviousProjection Upt07ProjectPrevious(
    Upt04PrimarySurfaceRecord current,
    Upt07PreviousCamera camera,
    uint2 dimensions)
{
    Upt07PreviousProjection result = (Upt07PreviousProjection)0;
    result.pixel = int2(-1, -1);
    if (camera.valid == 0u || dimensions.x == 0u || dimensions.y == 0u)
        return result;
    const float3 previousPosition = current.previousPositionOrMotion.w >= 0.5f
        ? current.previousPositionOrMotion.xyz
        : current.worldPositionAndViewDepth.xyz;
    const float3 delta = previousPosition - camera.origin;
    result.viewDepth = dot(delta, camera.forward);
    if (!isfinite(result.viewDepth) || result.viewDepth <= 0.05f
        || !isfinite(camera.tanX) || !isfinite(camera.tanY)
        || camera.tanX <= 0.0f || camera.tanY <= 0.0f)
        return result;
    const float2 ndc = float2(
        -dot(delta, camera.left) / (result.viewDepth * camera.tanX),
        -dot(delta, camera.up) / (result.viewDepth * camera.tanY));
    if (!all(isfinite(ndc)) || any(abs(ndc) > 1.0f))
        return result;
    const float2 pixelFloat = (ndc * 0.5f + 0.5f) * float2(dimensions);
    result.pixel = int2(floor(pixelFloat));
    result.valid = result.pixel.x >= 0 && result.pixel.y >= 0
            && uint(result.pixel.x) < dimensions.x
            && uint(result.pixel.y) < dimensions.y
        ? 1u : 0u;
    return result;
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
    const float currentLengthSquared = dot(
        current.geometricNormalAndRoughness.xyz,
        current.geometricNormalAndRoughness.xyz);
    const float previousLengthSquared = dot(
        previous.geometricNormalAndRoughness.xyz,
        previous.geometricNormalAndRoughness.xyz);
    if (!isfinite(currentLengthSquared) || !isfinite(previousLengthSquared)
        || currentLengthSquared <= 1.0e-12f || previousLengthSquared <= 1.0e-12f)
        return false;
    const float3 currentNormal = current.geometricNormalAndRoughness.xyz
        * rsqrt(currentLengthSquared);
    const float3 previousNormal = previous.geometricNormalAndRoughness.xyz
        * rsqrt(previousLengthSquared);
    if (dot(currentNormal, previousNormal) < 0.85f
        || abs(current.geometricNormalAndRoughness.w
            - previous.geometricNormalAndRoughness.w) > 0.20f)
        return false;
    const float depthTolerance = max(0.10f, abs(projectedPreviousDepth) * 0.10f);
    return isfinite(previous.worldPositionAndViewDepth.w)
        && abs(previous.worldPositionAndViewDepth.w - projectedPreviousDepth)
            <= depthTolerance;
}
