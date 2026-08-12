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
    // Uses the formerly reserved final camera word without growing the frozen
    // 128-byte push-constant ABI: 0 exact, 1 dither, 2 dither + probes.
    public uint historySearchMode;
};

public struct Upt07PreviousProjection
{
    public int2 pixel;
    public float linearDepth;
    public uint valid;
};

public Upt07PreviousProjection Upt07ProjectPreviousForSearch(
    Upt04PrimarySurfaceRecord current,
    Upt07PreviousCamera camera,
    uint2 dimensions,
    float2 pixelJitter,
    uint borderMargin)
{
    Upt07PreviousProjection result = (Upt07PreviousProjection)0;
    result.pixel = int2(-1, -1);
    if (camera.valid == 0u || dimensions.x == 0u || dimensions.y == 0u)
        return result;
    const float3 previousPosition = current.previousPositionOrMotion.w >= 0.5f
        ? current.previousPositionOrMotion.xyz
        : current.worldPositionAndViewDepth.xyz;
    const float3 delta = previousPosition - camera.origin;
    const float forwardDistance = dot(delta, camera.forward);
    result.linearDepth = length(delta);
    if (!isfinite(forwardDistance) || forwardDistance <= 0.05f
        || !isfinite(result.linearDepth)
        || !isfinite(camera.tanX) || !isfinite(camera.tanY)
        || camera.tanX <= 0.0f || camera.tanY <= 0.0f)
        return result;
    const float2 ndc = float2(
        -dot(delta, camera.left) / (forwardDistance * camera.tanX),
        -dot(delta, camera.up) / (forwardDistance * camera.tanY));
    if (!all(isfinite(ndc)) || !all(isfinite(pixelJitter)))
        return result;
    const float2 pixelFloat = (ndc * 0.5f + 0.5f) * float2(dimensions);
    const float margin = float(borderMargin) + 1.0f;
    if (!all(isfinite(pixelFloat))
        || pixelFloat.x < -margin || pixelFloat.y < -margin
        || pixelFloat.x >= float(dimensions.x) + margin
        || pixelFloat.y >= float(dimensions.y) + margin)
        return result;
    result.pixel = int2(floor(pixelFloat + pixelJitter));
    // A randomized probe may recover a center just outside the viewport.
    // Every actual surface load still performs an explicit signed bounds test.
    result.valid = 1u;
    return result;
}

public Upt07PreviousProjection Upt07ProjectPrevious(
    Upt04PrimarySurfaceRecord current,
    Upt07PreviousCamera camera,
    uint2 dimensions)
{
    const Upt07PreviousProjection result = Upt07ProjectPreviousForSearch(
        current, camera, dimensions, float2(0.0f), 0u);
    if (result.valid == 0u || result.pixel.x < 0 || result.pixel.y < 0
        || uint(result.pixel.x) >= dimensions.x
        || uint(result.pixel.y) >= dimensions.y)
        return (Upt07PreviousProjection)0;
    return result;
}

public bool Upt07SurfacesCompatibleScored(
    Upt04PrimarySurfaceRecord current,
    Upt04PrimarySurfaceRecord previous,
    float projectedPreviousLinearDepth,
    out float compatibilityScore)
{
    compatibilityScore = 1.0e30f;
    if (!Upt04PrimarySurfaceRecordValid(previous)
        || !Upt04PrimarySurfaceAlphaReuseCompatible(current, previous))
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
    // Keep material identity and roughness out of the temporal gate: both can
    // legitimately change on one animated receiver. Surface class is part of
    // the compact history sidecar, however, and must not cross from a rigid or
    // skinned receiver onto revealed world geometry. Match the RTXDI PT/GI
    // normal threshold rather than the older, unusually permissive DI gate.
    if (current.materialAndSurface.w != previous.materialAndSurface.w
        || dot(currentNormal, previousNormal) < 0.60f)
        return false;
    const float depthError = abs(previous.worldPositionAndViewDepth.w
        - projectedPreviousLinearDepth);
    const float depthTolerance = 0.10f * max(
        previous.worldPositionAndViewDepth.w,
        projectedPreviousLinearDepth);
    if (!isfinite(previous.worldPositionAndViewDepth.w)
        || !isfinite(depthError) || !isfinite(depthTolerance)
        || depthTolerance < 0.0f || depthError > depthTolerance)
        return false;
    compatibilityScore = depthError / max(depthTolerance, 1.0e-6f)
        + (1.0f - saturate(dot(currentNormal, previousNormal)));
    return isfinite(compatibilityScore);
}

public bool Upt07SurfacesCompatible(
    Upt04PrimarySurfaceRecord current,
    Upt04PrimarySurfaceRecord previous,
    float projectedPreviousLinearDepth)
{
    float compatibilityScore;
    return Upt07SurfacesCompatibleScored(
        current,
        previous,
        projectedPreviousLinearDepth,
        compatibilityScore);
}
