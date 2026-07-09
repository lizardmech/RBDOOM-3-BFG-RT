#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_RR_GUIDE_EXPORT_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_RR_GUIDE_EXPORT_HLSLI

float PathTraceCleanRtxdiDiResolvedSurfaceRRDepth(RAB_Surface surface)
{
    const float viewZ = dot(
        surface.worldPos - CleanRtxdiDiCameraOriginAndValid.xyz,
        CleanRtxdiDiCameraForwardAndTanX.xyz);
    return max(viewZ, 0.0);
}

// RR guides for a PSR-resolved replacement surface (transmission or reflection).
//
// Step 7b policy:
//   - Albedo / specular albedo are pure material attributes of the resolved
//     surface (never reflection radiance).
//   - Depth/position ignore the glass pane; they track the resolved hit.
//   - Motion is zero for now (no virtual motion yet). Do not permanently OR
//     OBJECT_MOTION_UNAVAILABLE every frame — that blocks RR reconstruction.
//   - Reset only when the glass lane identity changes (or the surface is
//     invalid). Sticky same-lane frames leave reset clear so RR can accumulate.
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

    // Zero motion without forcing a permanent motion-unavailable reset. Camera
    // motion alone is still usable for sticky same-lane frames.
    PathTraceRRMotionVectors[pixel] = float2(0.0, 0.0);
    PathTraceMotionVectors[pixel] = float4(0.0, 0.0, 0.0, 0.0);
    PathTraceMotionVectorMask[pixel] = 0u;

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
