#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_RR_GUIDE_EXPORT_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_RR_GUIDE_EXPORT_HLSLI

// Legacy deterministic clear-glass RR policy (shipping / Remix comparison
// notes). The stable stochastic PSR lane does NOT use this hybrid policy: its
// guides strictly follow the one selected integration surface so RR inputs and
// beauty ownership agree pixel-for-pixel.
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

float PathTraceCleanRtxdiDiResolvedSurfaceRRDepthFromViewZ(float viewZ)
{
    // Match primary-producer default DLSS-RR depth mode 2 (hyperbolic hardware
    // depth). near comes from CleanRtxdiDiToyPathInfo.y when host packed it;
    // far defaults to the same 100000 used by the clean-path RR contract.
    const float zNearPacked = CleanRtxdiDiToyPathInfo.y;
    const float zNear = max(zNearPacked > 1.0e-5 ? zNearPacked : 0.2, 1.0e-4);
    const float zFar = 100000.0;
    const float safeViewZ = max(viewZ, zNear);
    return saturate((zFar / max(zFar - zNear, 1.0e-4)) * (1.0 - zNear / safeViewZ));
}

float PathTraceCleanRtxdiDiResolvedSurfaceRRDepth(RAB_Surface surface)
{
    const float viewZ = dot(
        surface.worldPos - CleanRtxdiDiCameraOriginAndValid.xyz,
        CleanRtxdiDiCameraForwardAndTanX.xyz);
    return PathTraceCleanRtxdiDiResolvedSurfaceRRDepthFromViewZ(viewZ);
}

uint PathTraceCleanRtxdiDiResolvedSurfaceMotionSource(RAB_Surface surface)
{
    if (surface.instanceId == 0u && surface.surfaceClass == 0u)
    {
        return 1u;
    }
    if (surface.surfaceClass == RT_SMOKE_SURFACE_CLASS_SKINNED_DEFORMED)
    {
        return 2u;
    }
    if (surface.surfaceClass == RT_SMOKE_SURFACE_CLASS_RIGID_ENTITY)
    {
        return 3u;
    }
    return 4u;
}

#if !defined(RB_PT_SKINNED_HIT_ROUTE_LIGHTWEIGHT)
bool PathTraceCleanRtxdiDiResolvedSurfaceBarycentrics(
    float3 position,
    float3 p0,
    float3 p1,
    float3 p2,
    out float3 barycentrics)
{
    barycentrics = float3(0.0, 0.0, 0.0);
    const float3 edge0 = p1 - p0;
    const float3 edge1 = p2 - p0;
    const float3 offset = position - p0;
    const float d00 = dot(edge0, edge0);
    const float d01 = dot(edge0, edge1);
    const float d11 = dot(edge1, edge1);
    const float d20 = dot(offset, edge0);
    const float d21 = dot(offset, edge1);
    const float denominator = d00 * d11 - d01 * d01;
    if (abs(denominator) <= 1.0e-10)
    {
        return false;
    }
    const float inverseDenominator = 1.0 / denominator;
    const float v =
        (d11 * d20 - d01 * d21) * inverseDenominator;
    const float w =
        (d00 * d21 - d01 * d20) * inverseDenominator;
    barycentrics = float3(1.0 - v - w, v, w);
    return all(barycentrics == barycentrics);
}

bool PathTraceCleanRtxdiDiTryResolvedSkinnedPreviousPosition(
    RAB_Surface surface,
    out float3 previousWorldPosition)
{
    previousWorldPosition = float3(0.0, 0.0, 0.0);
    if (surface.surfaceClass !=
            RT_SMOKE_SURFACE_CLASS_SKINNED_DEFORMED ||
        !PathTraceIsSkinnedHitRouteInstance(surface.instanceId))
    {
        return false;
    }

    PathTraceSkinnedHitRouteGpuRecord route;
    PathTraceSkinnedHitRouteGpuTriangle routeTriangle;
    uint current0;
    uint current1;
    uint current2;
    uint previous0;
    uint previous1;
    uint previous2;
    if (!PathTraceLoadSkinnedHitRoutePreviousTriangleData(
            surface.instanceId,
            surface.primitiveIndex,
            route,
            routeTriangle,
            current0,
            current1,
            current2,
            previous0,
            previous1,
            previous2))
    {
        return false;
    }

    float3 barycentrics;
    if (!PathTraceCleanRtxdiDiResolvedSurfaceBarycentrics(
            surface.worldPos,
            SmokeSkinnedCurrentVertices[current0].position.xyz,
            SmokeSkinnedCurrentVertices[current1].position.xyz,
            SmokeSkinnedCurrentVertices[current2].position.xyz,
            barycentrics))
    {
        return false;
    }

    previousWorldPosition =
        SmokeSkinnedPreviousPositions[previous0].
            previousPosition.xyz * barycentrics.x +
        SmokeSkinnedPreviousPositions[previous1].
            previousPosition.xyz * barycentrics.y +
        SmokeSkinnedPreviousPositions[previous2].
            previousPosition.xyz * barycentrics.z;
    return all(previousWorldPosition ==
        previousWorldPosition);
}
#endif

void PathTraceCleanRtxdiDiWriteResolvedSurfaceRrMotion(
    uint2 pixel,
    RAB_Surface surface)
{
    if (CleanRtxdiDiMotionVectorInfo.x < 0.5)
    {
        return;
    }

    const uint sourceKind = PathTraceCleanRtxdiDiResolvedSurfaceMotionSource(surface);
    const uint sourceBits = (sourceKind & 0x0fu) << PT_MOTION_VECTOR_MASK_SOURCE_SHIFT;
    if (!RAB_IsSurfaceValid(surface) || CleanRtxdiDiPrevCameraOriginAndValid.w < 0.5)
    {
        PathTraceMotionVectors[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        PathTraceRRMotionVectors[pixel] = float2(0.0, 0.0);
        PathTraceMotionVectorMask[pixel] = sourceBits |
            (RT_PRIMARY_SURFACE_DEBUG_MISSING_PREVIOUS_CAMERA << PT_MOTION_VECTOR_MASK_INVALID_REASON_SHIFT);
        return;
    }

    float3 previousWorldPosition = surface.worldPos;
#if !defined(RB_PT_SKINNED_HIT_ROUTE_LIGHTWEIGHT)
    if (surface.surfaceClass ==
            RT_SMOKE_SURFACE_CLASS_SKINNED_DEFORMED &&
        !PathTraceCleanRtxdiDiTryResolvedSkinnedPreviousPosition(
            surface,
            previousWorldPosition))
    {
        PathTraceMotionVectors[pixel] =
            float4(0.0, 0.0, 0.0, 0.0);
        PathTraceRRMotionVectors[pixel] =
            float2(0.0, 0.0);
        PathTraceMotionVectorMask[pixel] = sourceBits |
            (RT_PRIMARY_SURFACE_DEBUG_REJECTED_PREVIOUS <<
                PT_MOTION_VECTOR_MASK_INVALID_REASON_SHIFT);
        return;
    }
#endif
    const float3 delta =
        previousWorldPosition -
        CleanRtxdiDiPrevCameraOriginAndValid.xyz;
    const float previousViewZ = dot(delta, CleanRtxdiDiPrevCameraForwardAndTanX.xyz);
    const float3 currentDelta = surface.worldPos - CleanRtxdiDiCameraOriginAndValid.xyz;
    const float currentViewZ = dot(currentDelta, CleanRtxdiDiCameraForwardAndTanX.xyz);
    if (previousViewZ <= 0.05 || currentViewZ <= 0.05)
    {
        PathTraceMotionVectors[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        PathTraceRRMotionVectors[pixel] = float2(0.0, 0.0);
        PathTraceMotionVectorMask[pixel] = sourceBits |
            (RT_PRIMARY_SURFACE_DEBUG_PREVIOUS_BEHIND_CAMERA << PT_MOTION_VECTOR_MASK_INVALID_REASON_SHIFT);
        return;
    }

    const float ndcX = -dot(delta, CleanRtxdiDiPrevCameraLeftAndTanY.xyz) /
        max(previousViewZ * CleanRtxdiDiPrevCameraForwardAndTanX.w, 1.0e-5);
    const float ndcY = -dot(delta, CleanRtxdiDiPrevCameraUpAndTanY.xyz) /
        max(previousViewZ * CleanRtxdiDiPrevCameraLeftAndTanY.w, 1.0e-5);
    const float currentNdcX = -dot(currentDelta, CleanRtxdiDiCameraLeftAndTanY.xyz) /
        max(currentViewZ * CleanRtxdiDiCameraForwardAndTanX.w, 1.0e-5);
    const float currentNdcY = -dot(currentDelta, CleanRtxdiDiCameraUpAndTanY.xyz) /
        max(currentViewZ * CleanRtxdiDiCameraLeftAndTanY.w, 1.0e-5);
    const uint2 dimensions = uint2(CleanRtxdiDiWidth, CleanRtxdiDiHeight);
    const float2 previousPixelFloat =
        (float2(ndcX, ndcY) * 0.5 + 0.5) * float2(max(dimensions, uint2(1u, 1u)));
    const float2 currentPixelFloat =
        (float2(currentNdcX, currentNdcY) * 0.5 + 0.5) * float2(max(dimensions, uint2(1u, 1u)));
    // A refracted continuation hit is generally not on the original glass
    // pixel's camera ray. Project the resolved world point through both
    // cameras; otherwise the refraction offset itself looks like motion while
    // the camera is stationary.
    const float2 motionPixels = previousPixelFloat - currentPixelFloat;

    if (!all(motionPixels == motionPixels))
    {
        PathTraceMotionVectors[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        PathTraceRRMotionVectors[pixel] = float2(0.0, 0.0);
        PathTraceMotionVectorMask[pixel] = sourceBits |
            (RT_PRIMARY_SURFACE_DEBUG_REJECTED_PREVIOUS << PT_MOTION_VECTOR_MASK_INVALID_REASON_SHIFT);
        return;
    }

    const float previousDepth = PathTraceCleanRtxdiDiResolvedSurfaceRRDepthFromViewZ(previousViewZ);
    const float currentDepth = PathTraceCleanRtxdiDiResolvedSurfaceRRDepth(surface);
    PathTraceMotionVectors[pixel] = float4(motionPixels, previousDepth - currentDepth, 0.0);
    PathTraceRRMotionVectors[pixel] = motionPixels;
    PathTraceMotionVectorMask[pixel] = PT_MOTION_VECTOR_MASK_VALID | sourceBits;
}

// RR guides for a PSR-resolved replacement surface (transmission or reflection
// as DI primary). Geometry guides come from this surface; for clear glass the
// producer should pass the behind-glass surface here and then blend reflection
// materials separately.
//
// Motion is rebuilt from this resolved surface below. The original primary
// vector belongs to the glass pane and is inconsistent with these guides.
//
// reflectionRayDistance:
//   > 0  -> write PathTraceRRGuideHitDistance as the mirror ray length.
//   <= 0 -> leave hit-distance buffer unchanged for this pixel.
// Specular albedo for RR: material F0 plus emissive color for all self-lit
// surfaces (static texture and variable/parm). Preserve chromaticity only.
float3 PathTraceCleanRtxdiDiRrSpecularAlbedoFromSurface(RAB_Surface surface)
{
    float3 specular = saturate(surface.material.specularF0);
    const float3 emissive = max(surface.material.emissiveRadiance, float3(0.0, 0.0, 0.0));
    const float peak = max(max(emissive.r, emissive.g), emissive.b);
    if (peak > 1.0e-4)
    {
        return max(specular, saturate(emissive / max(peak, 1.0e-5)));
    }
    return specular;
}

void PathTraceCleanRtxdiDiWriteResolvedSurfaceRrGuides(
    uint2 pixel,
    RAB_Surface surface,
    uint psrResolvedFlag,
    float reflectionRayDistance,
    bool laneChanged)
{
    PathTraceRRGuideAlbedo[pixel] = float4(saturate(surface.material.diffuseAlbedo), 1.0);
    PathTraceRRGuideSpecularAlbedo[pixel] = float4(
        PathTraceCleanRtxdiDiRrSpecularAlbedoFromSurface(surface),
        1.0);
    PathTraceRRGuideNormalRoughness[pixel] = float4(
        RAB_SafeNormalize(surface.shadingNormal, surface.geometryNormal),
        saturate(surface.material.roughness));
    PathTraceRRGuideDepth[pixel] = PathTraceCleanRtxdiDiResolvedSurfaceRRDepth(surface);
    PathTraceRRGuidePosition[pixel] = float4(surface.worldPos, 1.0);
    PathTraceCleanRtxdiDiWriteResolvedSurfaceRrMotion(pixel, surface);

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
