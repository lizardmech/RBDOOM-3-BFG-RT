#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_RR_GUIDE_EXPORT_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_RR_GUIDE_EXPORT_HLSLI

float PathTraceCleanRtxdiDiResolvedSurfaceRRDepth(RAB_Surface surface)
{
    const float viewZ = dot(
        surface.worldPos - CleanRtxdiDiCameraOriginAndValid.xyz,
        CleanRtxdiDiCameraForwardAndTanX.xyz);
    return max(viewZ, 0.0);
}

uint PathTraceCleanRtxdiDiResolvedSurfaceResetMask(RAB_Surface surface)
{
    if (!RAB_IsSurfaceValid(surface))
    {
        return RT_RR_RESET_INVALID_SURFACE;
    }
    return RT_RR_RESET_OBJECT_MOTION_UNAVAILABLE;
}

// RR guides for a PSR-resolved replacement surface (transmission or reflection).
//
// reflectionRayDistance:
//   > 0  -> write PathTraceRRGuideHitDistance as the mirror/continuation ray
//          length (not primary view depth). Required for reflection PSR.
//   <= 0 -> leave hit-distance buffer unchanged for this pixel.
//
// Motion is deliberately zeroed with OBJECT_MOTION_UNAVAILABLE rather than
// reusing glass-pane motion. Stochastic glass lane selection also sets
// STOCHASTIC_TRANSLUCENT so RR resets when reflection/transmission flips.
void PathTraceCleanRtxdiDiWriteResolvedSurfaceRrGuides(
    uint2 pixel,
    RAB_Surface surface,
    uint psrResolvedFlag,
    float reflectionRayDistance)
{
    PathTraceRRGuideAlbedo[pixel] = float4(saturate(surface.material.diffuseAlbedo), 1.0);
    PathTraceRRGuideSpecularAlbedo[pixel] = float4(saturate(surface.material.specularF0), 1.0);
    PathTraceRRGuideNormalRoughness[pixel] = float4(
        RAB_SafeNormalize(surface.shadingNormal, surface.geometryNormal),
        saturate(surface.material.roughness));
    PathTraceRRGuideDepth[pixel] = PathTraceCleanRtxdiDiResolvedSurfaceRRDepth(surface);
    PathTraceRRGuidePosition[pixel] = float4(surface.worldPos, 1.0);

    // Do not reproject with glass motion. Prefer reset over a wrong vector.
    PathTraceRRMotionVectors[pixel] = float2(0.0, 0.0);
    PathTraceMotionVectors[pixel] = float4(0.0, 0.0, 0.0, 0.0);
    PathTraceMotionVectorMask[pixel] = 0u;

    uint resetMask = PathTraceCleanRtxdiDiResolvedSurfaceResetMask(surface);
    if ((psrResolvedFlag & CLEAN_SURFACE_FLAG_REFLECTION_PSR_RESOLVED) != 0u ||
        (psrResolvedFlag & CLEAN_SURFACE_FLAG_TRANSMISSION_PSR_RESOLVED) != 0u)
    {
        resetMask |= RT_RR_RESET_STOCHASTIC_TRANSLUCENT;
        resetMask |= RT_RR_RESET_OBJECT_MOTION_UNAVAILABLE;
    }
    if ((psrResolvedFlag & CLEAN_SURFACE_FLAG_REFLECTION_PSR_RESOLVED) != 0u)
    {
        // Lane identity is reflection-specific; force material mismatch reset
        // relative to glass history when present.
        resetMask |= RT_RR_RESET_MATERIAL_MISMATCH;
    }
    PathTraceRRGuideResetMask[pixel] = resetMask;

    // Specular hit distance = actual reflection ray length for reflection PSR.
    // Never substitute primary linear depth here.
    if ((psrResolvedFlag & CLEAN_SURFACE_FLAG_REFLECTION_PSR_RESOLVED) != 0u &&
        reflectionRayDistance > 0.0)
    {
        PathTraceRRGuideHitDistance[pixel] = reflectionRayDistance;
    }
}

void PathTraceCleanRtxdiDiWriteResolvedSurfaceRrGuides(uint2 pixel, RAB_Surface surface)
{
    PathTraceCleanRtxdiDiWriteResolvedSurfaceRrGuides(
        pixel,
        surface,
        CLEAN_SURFACE_FLAG_TRANSMISSION_PSR_RESOLVED,
        0.0);
}

bool PathTraceCleanRtxdiDiPublishResolvedPrimarySurface(
    uint2 pixel,
    uint2 dimensions,
    inout RAB_Surface surface,
    uint psrResolvedFlag,
    float reflectionRayDistance)
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
        reflectionRayDistance);
    return true;
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
        0.0);
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
        0.0);
}

#endif
