#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_RR_GUIDE_EXPORT_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_RR_GUIDE_EXPORT_HLSLI

// Clear-glass RR policy (shipping / Remix comparison notes):
//   - Albedo + specular: behind-glass materials PLUS reflected materials at a
//     fixed blend weight (not Fresnel/angle). Never full overwrite.
//   - Normals: behind-glass only. Mirror normals are for pure mirrors; clear
//     glass behaves as if it has no reflection normal. Heavy refraction /
//     non-transparent glass coatings are deferred.
//   - Depth / position: ignore the glass pane; track the see-through
//     (behind-glass) surface. Do not leave glass-pane depth.
//   - Motion: do not zero or recompute here — keep primary-producer vectors
//     (full jitter-corrected path). Broken glass-only reprojection polluted RR.
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

// RR guides for a PSR-resolved replacement surface (transmission or reflection
// as DI primary). Geometry guides come from this surface; for clear glass the
// producer should pass the behind-glass surface here and then blend reflection
// materials separately.
//
// Motion vectors: intentionally NOT written here. The primary-surface producer
// already exported correct current-to-previous pixel motion (with RR jitter
// correction). Glass PSR used to zero them (looked like motion disabled) or
// recompute with a half-baked projector that dumped garbage / "translucent"
// looking values into the RR motion input. Leave primary motion alone until a
// full primary-equivalent behind-glass motion path exists.
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
