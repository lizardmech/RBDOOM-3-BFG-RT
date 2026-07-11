float3 PathTraceCleanRoomMotionVectorDiagnosticColor(float2 motionPixels)
{
    if (any(!(motionPixels == motionPixels)))
    {
        return float3(1.0, 0.0, 1.0);
    }

    const float maxVisualizedPixels = 32.0;
    const float magnitude = length(motionPixels);
    const float2 signedDirection = saturate(motionPixels / (maxVisualizedPixels * 2.0) + 0.5);
    return float3(signedDirection, saturate(magnitude / maxVisualizedPixels));
}

float3 PathTraceCleanRoomMotionVectorInvalidColor(uint motionMask)
{
    const uint reason = (motionMask >> PT_MOTION_VECTOR_MASK_INVALID_REASON_SHIFT) & 0xffu;
    if (reason == 1u)
    {
        return float3(1.0, 0.05, 0.05);
    }
    if (reason == 2u || reason == 13u)
    {
        return float3(1.0, 0.85, 0.05);
    }
    if (reason == 3u || reason == 15u)
    {
        return float3(1.0, 0.35, 0.05);
    }
    if (reason == 4u || reason == 17u)
    {
        return float3(0.9, 0.05, 1.0);
    }
    if (reason == 7u || reason == 18u)
    {
        return float3(0.05, 0.35, 1.0);
    }
    return float3(0.6, 0.6, 0.6);
}

float3 PathTraceCleanRoomRRMotionVectorColor(uint2 pixel, uint2 dimensions)
{
    const uint motionMask = PathTraceMotionVectorMask[pixel];
    const bool valid = (motionMask & PT_MOTION_VECTOR_MASK_VALID) != 0u;
    const bool rrHalf = pixel.x >= (dimensions.x >> 1u);
    const float2 motionPixels = rrHalf ? PathTraceRRMotionVectors[pixel] : PathTraceMotionVectors[pixel].xy;
    float3 color = PathTraceCleanRoomMotionVectorDiagnosticColor(motionPixels);
    if (!valid)
    {
        return color * 0.2 + PathTraceCleanRoomMotionVectorInvalidColor(motionMask) * 0.8;
    }

    const uint sourceKind = (motionMask >> PT_MOTION_VECTOR_MASK_SOURCE_SHIFT) & 0x0fu;
    const float3 sourceTint =
        sourceKind == 1u ? float3(0.05, 0.25, 0.05) :
        sourceKind == 2u ? float3(0.25, 0.05, 0.25) :
        sourceKind == 3u ? float3(0.05, 0.20, 0.25) :
        float3(0.08, 0.08, 0.08);
    color = saturate(color + sourceTint);
    if (abs((int)pixel.x - (int)(dimensions.x >> 1u)) <= 1)
    {
        return float3(1.0, 1.0, 1.0);
    }
    return color;
}

float3 PathTraceCleanRoomToneMapRRInput(float3 hdrColor)
{
    hdrColor = max(hdrColor, float3(0.0, 0.0, 0.0));
    return hdrColor / (hdrColor + float3(1.0, 1.0, 1.0));
}

float3 PathTraceCleanRoomRRResetMaskColor(uint resetMask)
{
    if (resetMask == 0u)
    {
        return float3(0.03, 0.45, 0.12);
    }
    if ((resetMask & RT_RR_RESET_STOCHASTIC_TRANSLUCENT) != 0u)
    {
        return float3(0.75, 0.15, 0.85);
    }
    if ((resetMask & RT_RR_RESET_MATERIAL_MISMATCH) != 0u)
    {
        return float3(1.0, 0.6, 0.05);
    }
    if ((resetMask & RT_RR_RESET_REJECTED_PREVIOUS) != 0u)
    {
        return float3(0.95, 0.05, 0.05);
    }
    if ((resetMask & RT_RR_RESET_MISSING_PREVIOUS) != 0u)
    {
        return float3(0.6, 0.1, 0.1);
    }
    if ((resetMask & RT_RR_RESET_OBJECT_MOTION_UNAVAILABLE) != 0u)
    {
        return float3(0.8, 0.8, 0.15);
    }
    if ((resetMask & RT_RR_RESET_INVALID_SURFACE) != 0u)
    {
        return float3(0.02, 0.02, 0.02);
    }
    return float3(0.8, 0.3, 0.1);
}

float3 PathTraceCleanRoomRRInputTileColor(uint2 sourcePixel, uint tile)
{
    if (tile == 0u)
    {
        return saturate(PathTraceRRGuideAlbedo[sourcePixel].rgb);
    }
    if (tile == 1u)
    {
        const float4 normalRoughness = PathTraceRRGuideNormalRoughness[sourcePixel];
        return saturate(float3(normalRoughness.rg * 0.5 + 0.5, normalRoughness.a));
    }
    if (tile == 2u)
    {
        return saturate(PathTraceRRGuideSpecularAlbedo[sourcePixel].rgb);
    }
    if (tile == 3u)
    {
        return PathTraceCleanRoomToneMapRRInput(PathTraceRRInputColor[sourcePixel].rgb);
    }
    if (tile == 6u)
    {
        const float3 inputColor = PathTraceCleanRoomToneMapRRInput(PathTraceRRInputColor[sourcePixel].rgb);
        const float3 specularGuide = saturate(PathTraceRRGuideSpecularAlbedo[sourcePixel].rgb);
        return saturate(max(inputColor, specularGuide));
    }
    if (tile == 7u)
    {
        const float hitDistance = PathTraceRRGuideHitDistance[sourcePixel];
        if (!(hitDistance == hitDistance))
        {
            return float3(1.0, 0.0, 1.0);
        }
        if (hitDistance <= 0.0)
        {
            return float3(0.0, 0.0, 0.0);
        }
        const float normalizedHitDistance = saturate(hitDistance / 1000.0);
        return float3(normalizedHitDistance, normalizedHitDistance, normalizedHitDistance);
    }
    if (tile == 4u)
    {
        const float depth = PathTraceRRGuideDepth[sourcePixel];
        const float hitDistance = PathTraceRRGuideHitDistance[sourcePixel];
        return float3(saturate(depth / 4096.0), saturate(hitDistance / 4096.0), 0.0);
    }

    const bool validMotion = (PathTraceMotionVectorMask[sourcePixel] & PT_MOTION_VECTOR_MASK_VALID) != 0u;
    const float3 motionColor = PathTraceCleanRoomMotionVectorDiagnosticColor(PathTraceRRMotionVectors[sourcePixel]);
    const float3 resetColor = PathTraceCleanRoomRRResetMaskColor(PathTraceRRGuideResetMask[sourcePixel]);
    return validMotion ? saturate(motionColor) : saturate(resetColor);
}

float3 PathTraceCleanRoomRRGuideAlbedoDebugColor(uint2 pixel)
{
    return saturate(PathTraceRRGuideAlbedo[pixel].rgb);
}

float3 PathTraceCleanRoomRRGuideSpecularAlbedoDebugColor(uint2 pixel)
{
    return saturate(PathTraceRRGuideSpecularAlbedo[pixel].rgb);
}

float3 PathTraceCleanRoomRRDepthContractBandColor(uint2 pixel, uint2 dimensions)
{
    const uint bandCount = 5u;
    const bool forcedBand = CleanRtxdiDiView8Band < bandCount;
    const uint band = forcedBand ? CleanRtxdiDiView8Band : min((pixel.y * bandCount) / max(dimensions.y, 1u), bandCount - 1u);

    PathTracePrimarySurfaceRecord record;
    const bool validRecord = PathTraceCleanRoomLoadSurfaceRecord(pixel, dimensions, record);
    const float hitTDepth = validRecord ? max(record.worldPositionAndViewDepth.w, 0.0) : 0.0;
    const float rrDepth = max(PathTraceRRGuideDepth[pixel], 0.0);

    if (!forcedBand)
    {
        const uint bandStart = (band * max(dimensions.y, 1u)) / bandCount;
        if (pixel.y == bandStart)
        {
            return float3(1.0, 1.0, 1.0);
        }
    }

    if (band == 0u)
    {
        return float3(saturate(hitTDepth / 4096.0), 0.0, 0.0);
    }
    if (band == 1u)
    {
        // rrDepth is hardware [0,1] finite-far projection depth.
        return float3(0.0, saturate(rrDepth), 0.0);
    }
    if (band == 2u)
    {
        const float viewZDepth = dot(record.worldPositionAndViewDepth.xyz - CleanRtxdiDiCameraOriginAndValid.xyz, CleanRtxdiDiCameraForwardAndTanX.xyz);
        return float3(saturate(abs(hitTDepth - viewZDepth) / 512.0), saturate(rrDepth), saturate(hitTDepth / 4096.0));
    }
    if (band == 3u)
    {
        const float motionZ = PathTraceMotionVectors[pixel].z;
        return float3(saturate(motionZ / 128.0 + 0.5), saturate(abs(motionZ) / 128.0), 0.0);
    }

    return PathTraceCleanRoomRRResetMaskColor(PathTraceRRGuideResetMask[pixel]);
}

float3 PathTraceCleanRoomPrimaryHitReprojectionErrorColor(uint2 pixel, uint2 dimensions)
{
    PathTracePrimarySurfaceRecord record;
    if (!PathTraceCleanRoomLoadSurfaceRecord(pixel, dimensions, record))
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float3 delta = record.worldPositionAndViewDepth.xyz - CleanRtxdiDiCameraOriginAndValid.xyz;
    const float forwardDistance = dot(delta, CleanRtxdiDiCameraForwardAndTanX.xyz);
    if (forwardDistance <= 1.0e-4)
    {
        return float3(0.0, 0.0, 1.0);
    }

    const float ndcX = -dot(delta, CleanRtxdiDiCameraLeftAndTanY.xyz) / max(forwardDistance * CleanRtxdiDiCameraForwardAndTanX.w, 1.0e-5);
    const float ndcY = -dot(delta, CleanRtxdiDiCameraUpAndTanY.xyz) / max(forwardDistance * CleanRtxdiDiCameraLeftAndTanY.w, 1.0e-5);
    const float2 projectedPixel = (float2(ndcX, ndcY) * 0.5 + 0.5) * float2(max(dimensions, uint2(1u, 1u)));
    if (!all(projectedPixel == projectedPixel))
    {
        return float3(1.0, 0.0, 1.0);
    }

    const float2 sourcePixel = float2(pixel) + 0.5;
    const float reprojectionErrorPixels = length(projectedPixel - sourcePixel);
    const float errorColor = saturate(reprojectionErrorPixels / 4.0);
    const float outsideFrame = (abs(ndcX) > 1.0 || abs(ndcY) > 1.0) ? 1.0 : 0.0;
    const float stableColor = 1.0 - saturate(reprojectionErrorPixels);
    return saturate(float3(errorColor, stableColor, outsideFrame));
}

float3 PathTraceCleanRoomPreviousHitReprojectionErrorColor(uint2 pixel, uint2 dimensions)
{
    PathTracePrimarySurfaceRecord record;
    if (!PathTraceCleanRoomLoadSurfaceRecord(pixel, dimensions, record))
    {
        return float3(0.0, 0.0, 0.0);
    }
    if (CleanRtxdiDiPrevCameraOriginAndValid.w < 0.5)
    {
        return float3(0.0, 1.0, 1.0);
    }

    const bool hasObjectPrevious =
        (record.header.y & (RT_PRIMARY_SURFACE_HAS_OBJECT_MOTION | RT_PRIMARY_SURFACE_HAS_PREVIOUS_POSITION)) ==
            (RT_PRIMARY_SURFACE_HAS_OBJECT_MOTION | RT_PRIMARY_SURFACE_HAS_PREVIOUS_POSITION) &&
        record.previousPositionOrMotion.w >= 0.5;
    const float3 previousPosition = hasObjectPrevious ? record.previousPositionOrMotion.xyz : record.worldPositionAndViewDepth.xyz;
    const float3 delta = previousPosition - CleanRtxdiDiPrevCameraOriginAndValid.xyz;
    const float forwardDistance = dot(delta, CleanRtxdiDiPrevCameraForwardAndTanX.xyz);
    if (forwardDistance <= 1.0e-4)
    {
        return float3(0.0, 0.0, 1.0);
    }

    const float ndcX = -dot(delta, CleanRtxdiDiPrevCameraLeftAndTanY.xyz) / max(forwardDistance * CleanRtxdiDiPrevCameraForwardAndTanX.w, 1.0e-5);
    const float ndcY = -dot(delta, CleanRtxdiDiPrevCameraUpAndTanY.xyz) / max(forwardDistance * CleanRtxdiDiPrevCameraLeftAndTanY.w, 1.0e-5);
    const float2 projectedPreviousPixel = (float2(ndcX, ndcY) * 0.5 + 0.5) * float2(max(dimensions, uint2(1u, 1u)));
    if (!all(projectedPreviousPixel == projectedPreviousPixel))
    {
        return float3(1.0, 0.0, 1.0);
    }

    const float2 exportedPreviousPixel = float2(pixel) + 0.5 + PathTraceRRMotionVectors[pixel];
    const float reprojectionErrorPixels = length(projectedPreviousPixel - exportedPreviousPixel);
    const float errorColor = saturate(reprojectionErrorPixels / 4.0);
    const float coherentColor = 1.0 - saturate(reprojectionErrorPixels);
    const float outsideFrame = (abs(ndcX) > 1.0 || abs(ndcY) > 1.0) ? 1.0 : 0.0;
    const bool validMotion = (PathTraceMotionVectorMask[pixel] & PT_MOTION_VECTOR_MASK_VALID) != 0u;
    if (!validMotion)
    {
        return saturate(float3(1.0, 0.55 * coherentColor, outsideFrame));
    }
    if (!hasObjectPrevious)
    {
        return saturate(float3(errorColor, coherentColor, 0.25 + outsideFrame));
    }
    return saturate(float3(errorColor, coherentColor, outsideFrame));
}

float3 PathTraceCleanRoomRRInputMosaicColor(uint2 pixel, uint2 dimensions)
{
    const uint selectedTile = CleanRtxdiDiView8Band <= 7u ? CleanRtxdiDiView8Band : 0xffffffffu;
    if (selectedTile <= 7u)
    {
        return PathTraceCleanRoomRRInputTileColor(pixel, selectedTile);
    }

    const uint columns = 3u;
    const uint rows = 2u;
    const uint cellWidth = max(dimensions.x / columns, 1u);
    const uint cellHeight = max(dimensions.y / rows, 1u);
    const uint column = min(pixel.x / cellWidth, columns - 1u);
    const uint row = min(pixel.y / cellHeight, rows - 1u);
    const uint tile = row * columns + column;
    const uint2 cellOrigin = uint2(column * cellWidth, row * cellHeight);
    const uint2 cellExtent = max(uint2(
        column == columns - 1u ? dimensions.x - cellOrigin.x : cellWidth,
        row == rows - 1u ? dimensions.y - cellOrigin.y : cellHeight), uint2(1u, 1u));
    const uint2 localPixel = min(pixel - cellOrigin, cellExtent - 1u);
    const uint2 sourcePixel = min((localPixel * dimensions) / cellExtent, dimensions - 1u);

    if (localPixel.x == 0u || localPixel.y == 0u)
    {
        return float3(1.0, 1.0, 1.0);
    }

    return PathTraceCleanRoomRRInputTileColor(sourcePixel, tile);
}
