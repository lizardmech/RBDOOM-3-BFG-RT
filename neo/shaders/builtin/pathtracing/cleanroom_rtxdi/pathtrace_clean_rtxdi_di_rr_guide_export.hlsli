#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_RR_GUIDE_EXPORT_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_RR_GUIDE_EXPORT_HLSLI

// Clear-glass RR policy (shipping / Remix comparison notes):
//   - Albedo + specular: behind-glass materials PLUS reflected materials at a
//     fixed blend weight (not Fresnel/angle). Never full overwrite.
//   - Normals: behind-glass only. Mirror normals are for pure mirrors; clear
//     glass behaves as if it has no reflection normal. Heavy refraction /
//     non-transparent glass coatings are deferred.
//   - Depth / position / motion: ignore the glass pane; track the see-through
//     (behind-glass) surface. Do not zero motion or leave glass-pane depth.
//   - Specular hit distance: mirror ray length when reflection energy is present.

// Fixed Remix-like material blend (not angle-dependent Fresnel).
static const float RT_CLEAN_RTXDI_DI_GLASS_RR_REFLECTION_MATERIAL_BLEND = 0.35;

float PathTraceCleanRtxdiDiResolvedSurfaceRRDepth(RAB_Surface surface)
{
    const float viewZ = dot(
        surface.worldPos - CleanRtxdiDiCameraOriginAndValid.xyz,
        CleanRtxdiDiCameraForwardAndTanX.xyz);
    // Match primary-producer default DLSS-RR depth mode 2 (hyperbolic hardware
    // depth). near comes from CleanRtxdiDiToyPathInfo.y when host packed it;
    // far defaults to the same 100000 used by the clean-path RR contract.
    const float zNearPacked = CleanRtxdiDiToyPathInfo.y;
    const float zNear = max(zNearPacked > 1.0e-5 ? zNearPacked : 0.2, 1.0e-4);
    const float zFar = 100000.0;
    const float safeViewZ = max(viewZ, zNear);
    return saturate((zFar / max(zFar - zNear, 1.0e-4)) * (1.0 - zNear / safeViewZ));
}

// Camera motion for a resolved (usually behind-glass) world position. Clear
// glass must not disable motion; shipping titles ignore the pane.
bool PathTraceCleanRtxdiDiWriteCameraMotionForSurface(uint2 pixel, RAB_Surface surface)
{
    const uint width = CleanRtxdiDiWidth != 0u ? CleanRtxdiDiWidth : DispatchRaysDimensions().x;
    const uint height = CleanRtxdiDiHeight != 0u ? CleanRtxdiDiHeight : DispatchRaysDimensions().y;
    if (width == 0u || height == 0u || pixel.x >= width || pixel.y >= height ||
        !RAB_IsSurfaceValid(surface) ||
        CleanRtxdiDiPrevCameraOriginAndValid.w < 0.5)
    {
        PathTraceRRMotionVectors[pixel] = float2(0.0, 0.0);
        PathTraceMotionVectors[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        PathTraceMotionVectorMask[pixel] = 0u;
        return false;
    }

    const float3 delta = surface.worldPos - CleanRtxdiDiPrevCameraOriginAndValid.xyz;
    const float forwardDistance = dot(delta, CleanRtxdiDiPrevCameraForwardAndTanX.xyz);
    if (forwardDistance <= 0.05)
    {
        PathTraceRRMotionVectors[pixel] = float2(0.0, 0.0);
        PathTraceMotionVectors[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        PathTraceMotionVectorMask[pixel] = 0u;
        return false;
    }

    const float ndcX = -dot(delta, CleanRtxdiDiPrevCameraLeftAndTanY.xyz) /
        max(forwardDistance * CleanRtxdiDiPrevCameraForwardAndTanX.w, 1.0e-5);
    const float ndcY = -dot(delta, CleanRtxdiDiPrevCameraUpAndTanY.xyz) /
        max(forwardDistance * CleanRtxdiDiPrevCameraLeftAndTanY.w, 1.0e-5);
    if (abs(ndcX) > 1.0 || abs(ndcY) > 1.0)
    {
        PathTraceRRMotionVectors[pixel] = float2(0.0, 0.0);
        PathTraceMotionVectors[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        PathTraceMotionVectorMask[pixel] = 0u;
        return false;
    }

    const float2 previousPixelFloat =
        (float2(ndcX, ndcY) * 0.5 + 0.5) * float2(width, height);
    if (!all(previousPixelFloat == previousPixelFloat) ||
        previousPixelFloat.x < 0.0 || previousPixelFloat.y < 0.0 ||
        previousPixelFloat.x >= (float)width || previousPixelFloat.y >= (float)height)
    {
        PathTraceRRMotionVectors[pixel] = float2(0.0, 0.0);
        PathTraceMotionVectors[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        PathTraceMotionVectorMask[pixel] = 0u;
        return false;
    }

    const float2 motionPixels = previousPixelFloat - (float2(pixel) + 0.5);
    PathTraceRRMotionVectors[pixel] = motionPixels;
    PathTraceMotionVectors[pixel] = float4(motionPixels, 0.0, 0.0);
    PathTraceMotionVectorMask[pixel] = PT_MOTION_VECTOR_MASK_VALID;
    return true;
}

// RR guides for a PSR-resolved replacement surface (transmission or reflection
// as DI primary). Geometry/motion always come from this surface; for clear glass
// the producer should pass the behind-glass surface here and then blend
// reflection materials separately.
//
// reflectionRayDistance:
//   > 0  -> write PathTraceRRGuideHitDistance as the mirror ray length.
//   <= 0 -> leave hit-distance buffer unchanged for this pixel.
void PathTraceCleanRtxdiDiWriteResolvedSurfaceRrGuides(
    uint2 pixel,
    RAB_Surface surface,
    uint psrResolvedFlag,
    float reflectionRayDistance,
    bool laneChanged)
{
    PathTraceRRGuideAlbedo[pixel] = float4(saturate(surface.material.diffuseAlbedo), 1.0);
    PathTraceRRGuideSpecularAlbedo[pixel] = float4(saturate(surface.material.specularF0), 1.0);
    PathTraceRRGuideNormalRoughness[pixel] = float4(
        RAB_SafeNormalize(surface.shadingNormal, surface.geometryNormal),
        saturate(surface.material.roughness));
    PathTraceRRGuideDepth[pixel] = PathTraceCleanRtxdiDiResolvedSurfaceRRDepth(surface);
    PathTraceRRGuidePosition[pixel] = float4(surface.worldPos, 1.0);

    // Camera motion of the resolved surface (behind-glass for clear glass).
    // Never permanently zero motion on glass pixels.
    PathTraceCleanRtxdiDiWriteCameraMotionForSurface(pixel, surface);

    uint resetMask = 0u;
    if (!RAB_IsSurfaceValid(surface))
    {
        resetMask = RT_RR_RESET_INVALID_SURFACE;
    }
    else if (laneChanged)
    {
        // Real identity change (glass -> T, T -> R, first PSR frame, etc.).
        resetMask = RT_RR_RESET_STOCHASTIC_TRANSLUCENT;
    }
    PathTraceRRGuideResetMask[pixel] = resetMask;

    // Specular hit distance = actual reflection ray length when reflection PSR
    // owns the specular reconstruction path.
    if ((psrResolvedFlag & CLEAN_SURFACE_FLAG_REFLECTION_PSR_RESOLVED) != 0u &&
        reflectionRayDistance > 0.0)
    {
        PathTraceRRGuideHitDistance[pixel] = reflectionRayDistance;
    }
}

// Additive fixed-ratio blend of mirrored material albedo/spec into the current
// (behind-glass) guides. Does not touch normals, depth, position, motion, or
// reset — those stay on the see-through surface for clear glass.
void PathTraceCleanRtxdiDiBlendReflectionMaterialRrGuides(
    uint2 pixel,
    RAB_Surface reflectionSurface,
    float reflectionRayDistance)
{
    if (!RAB_IsSurfaceValid(reflectionSurface))
    {
        return;
    }

    const float w = RT_CLEAN_RTXDI_DI_GLASS_RR_REFLECTION_MATERIAL_BLEND;
    const float3 behindAlbedo = saturate(PathTraceRRGuideAlbedo[pixel].rgb);
    const float3 behindSpecular = saturate(PathTraceRRGuideSpecularAlbedo[pixel].rgb);
    const float3 reflectionAlbedo = saturate(reflectionSurface.material.diffuseAlbedo);
    const float3 reflectionSpecular = saturate(reflectionSurface.material.specularF0);

    PathTraceRRGuideAlbedo[pixel] = float4(
        saturate(behindAlbedo + reflectionAlbedo * w),
        1.0);
    PathTraceRRGuideSpecularAlbedo[pixel] = float4(
        saturate(behindSpecular + reflectionSpecular * w),
        1.0);

    if (reflectionRayDistance > 0.0)
    {
        PathTraceRRGuideHitDistance[pixel] = reflectionRayDistance;
    }
}

// Back-compat name used by older call sites: blend only, no normal overwrite.
void PathTraceCleanRtxdiDiWriteReflectionMaterialRrGuides(
    uint2 pixel,
    RAB_Surface reflectionSurface,
    float reflectionRayDistance)
{
    PathTraceCleanRtxdiDiBlendReflectionMaterialRrGuides(
        pixel,
        reflectionSurface,
        reflectionRayDistance);
}

// Clear-glass RR geometry from behind-glass, then fixed-ratio reflection
// material blend. Used after R-owned DI publish so depth/normal/motion ignore
// the pane while albedo/spec still carry reflection structure.
void PathTraceCleanRtxdiDiWriteClearGlassRrGuides(
    uint2 pixel,
    RAB_Surface behindGlassSurface,
    bool hasReflectionMaterials,
    RAB_Surface reflectionSurface,
    float reflectionRayDistance,
    bool laneChanged)
{
    PathTraceCleanRtxdiDiWriteResolvedSurfaceRrGuides(
        pixel,
        behindGlassSurface,
        CLEAN_SURFACE_FLAG_TRANSMISSION_PSR_RESOLVED,
        0.0,
        laneChanged);

    if (hasReflectionMaterials)
    {
        PathTraceCleanRtxdiDiBlendReflectionMaterialRrGuides(
            pixel,
            reflectionSurface,
            reflectionRayDistance);
    }
}

void PathTraceCleanRtxdiDiWriteResolvedSurfaceRrGuides(uint2 pixel, RAB_Surface surface)
{
    PathTraceCleanRtxdiDiWriteResolvedSurfaceRrGuides(
        pixel,
        surface,
        CLEAN_SURFACE_FLAG_TRANSMISSION_PSR_RESOLVED,
        0.0,
        true);
}

bool PathTraceCleanRtxdiDiPublishResolvedPrimarySurface(
    uint2 pixel,
    uint2 dimensions,
    inout RAB_Surface surface,
    uint psrResolvedFlag,
    float reflectionRayDistance,
    bool laneChanged)
{
    const uint width = CleanRtxdiDiWidth != 0u ? CleanRtxdiDiWidth : dimensions.x;
    const uint height = CleanRtxdiDiHeight != 0u ? CleanRtxdiDiHeight : dimensions.y;
    if (width == 0u || height == 0u || pixel.x >= width || pixel.y >= height)
    {
        return false;
    }

    const uint recordIndex = pixel.y * width + pixel.x;
    surface.linearDepth = dot(
        surface.worldPos - CleanRtxdiDiCameraOriginAndValid.xyz,
        CleanRtxdiDiCameraForwardAndTanX.xyz);
    PrimarySurfaceHistoryCurrent[recordIndex] =
        PathTraceCleanRtxdiDiPackResolvedPrimarySurfaceRecord(surface, psrResolvedFlag);
    PathTraceCleanRtxdiDiWriteResolvedSurfaceRrGuides(
        pixel,
        surface,
        psrResolvedFlag,
        reflectionRayDistance,
        laneChanged);
    return true;
}

bool PathTraceCleanRtxdiDiPublishResolvedPrimarySurface(
    uint2 pixel,
    uint2 dimensions,
    inout RAB_Surface surface,
    uint psrResolvedFlag,
    float reflectionRayDistance)
{
    return PathTraceCleanRtxdiDiPublishResolvedPrimarySurface(
        pixel,
        dimensions,
        surface,
        psrResolvedFlag,
        reflectionRayDistance,
        true);
}

bool PathTraceCleanRtxdiDiPublishResolvedPrimarySurface(
    uint2 pixel,
    uint2 dimensions,
    inout RAB_Surface surface,
    uint psrResolvedFlag)
{
    return PathTraceCleanRtxdiDiPublishResolvedPrimarySurface(
        pixel,
        dimensions,
        surface,
        psrResolvedFlag,
        0.0,
        true);
}

bool PathTraceCleanRtxdiDiPublishResolvedPrimarySurface(
    uint2 pixel,
    uint2 dimensions,
    inout RAB_Surface surface)
{
    return PathTraceCleanRtxdiDiPublishResolvedPrimarySurface(
        pixel,
        dimensions,
        surface,
        CLEAN_SURFACE_FLAG_TRANSMISSION_PSR_RESOLVED,
        0.0,
        true);
}

#endif
