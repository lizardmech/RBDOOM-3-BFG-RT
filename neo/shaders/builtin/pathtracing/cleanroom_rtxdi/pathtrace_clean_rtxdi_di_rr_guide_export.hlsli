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

void PathTraceCleanRtxdiDiWriteResolvedSurfaceRrGuides(uint2 pixel, RAB_Surface surface)
{
    PathTraceRRGuideAlbedo[pixel] = float4(saturate(surface.material.diffuseAlbedo), 1.0);
    PathTraceRRGuideSpecularAlbedo[pixel] = float4(saturate(surface.material.specularF0), 1.0);
    PathTraceRRGuideNormalRoughness[pixel] = float4(
        RAB_SafeNormalize(surface.shadingNormal, surface.geometryNormal),
        saturate(surface.material.roughness));
    PathTraceRRGuideDepth[pixel] = PathTraceCleanRtxdiDiResolvedSurfaceRRDepth(surface);
    PathTraceRRGuidePosition[pixel] = float4(surface.worldPos, 1.0);
    PathTraceRRMotionVectors[pixel] = float2(0.0, 0.0);
    PathTraceMotionVectors[pixel] = float4(0.0, 0.0, 0.0, 0.0);
    PathTraceMotionVectorMask[pixel] = 0u;
    PathTraceRRGuideResetMask[pixel] = PathTraceCleanRtxdiDiResolvedSurfaceResetMask(surface);
}

bool PathTraceCleanRtxdiDiPublishResolvedPrimarySurface(
    uint2 pixel,
    uint2 dimensions,
    inout RAB_Surface surface,
    uint psrResolvedFlag)
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
    PathTraceCleanRtxdiDiWriteResolvedSurfaceRrGuides(pixel, surface);
    return true;
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
        CLEAN_SURFACE_FLAG_TRANSMISSION_PSR_RESOLVED);
}

#endif
