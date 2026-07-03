#if defined(CLEAN_RTXDI_DI_GLASS_ENTRY)

Texture2D<float4> PathTraceCleanRtxdiDiOutputColorSource : register(t89);

float4 PathTraceCleanRtxdiDiGlassDebugColor(
    uint2 pixel,
    uint2 dimensions,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams)
{
    RAB_Surface surface;
    if (!PathTraceCleanRtxdiDiLoadGlassMaterialSurface(pixel, dimensions, surface))
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    const PathTraceMaterialFeature feature = PathTraceCleanRtxdiDiGlassFeatureForSurface(surface);
    if (PathTraceCleanRtxdiDiGlassFeatureSupported(feature))
    {
        const PathTraceCleanRtxdiDiGlassMaterialParams materialParams =
            PathTraceCleanRtxdiDiLoadGlassMaterialParams(surface, runtimeParams);
        const PathTraceCleanRtxdiDiGlassThinPayload payload =
            PathTraceCleanRtxdiDiBuildGlassThinPayload(surface, materialParams);
        return float4(saturate(payload.transmission + payload.reflection * 0.25), 1.0);
    }

    if (feature.materialKind == RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_GLASS)
    {
        return float4(0.65, 0.05, 0.85, 1.0);
    }

    return float4(0.015, 0.015, 0.025, 1.0);
}

bool PathTraceCleanRtxdiDiGlassBuildGuideCandidate(
    uint2 pixel,
    uint2 sourcePixel,
    uint2 dimensions,
    float glassWeight,
    out float4 candidate0,
    out float4 candidate1,
    out float4 candidate2)
{
    candidate0 = float4(0.0, 0.0, 0.0, 0.0);
    candidate1 = float4(0.0, 0.0, 0.0, 0.0);
    candidate2 = float4(0.0, 0.0, 0.0, 0.0);

    PathTracePrimarySurfaceRecord sourceRecord;
    if (!PathTraceCleanRoomLoadSurfaceRecord(sourcePixel, dimensions, sourceRecord))
    {
        return false;
    }

    const RAB_Surface sourceSurface = PathTraceCleanRoomSurfaceForView(sourceRecord);
    if (!RAB_IsSurfaceValid(sourceSurface))
    {
        return false;
    }

    const float4 sourceNormalRoughness = PathTraceRRGuideNormalRoughness[sourcePixel];
    const float sourceDepth = PathTraceRRGuideDepth[sourcePixel];
    const float2 sourceMotion = PathTraceRRMotionVectors[sourcePixel] + (float2(sourcePixel) - float2(pixel));
    const uint sourceResetMask = PathTraceRRGuideResetMask[sourcePixel];
    const float roughness = saturate(sourceNormalRoughness.w);
    const float validWeight = saturate(glassWeight);

    candidate0 = float4(sourceNormalRoughness.xyz, sourceDepth);
    candidate1 = float4(sourceMotion.xy, float(sourceResetMask), validWeight);
    candidate2 = float4(sourceSurface.worldPos, roughness);
    return true;
}

float3 PathTraceCleanRtxdiDiGlassGuideCandidateDebugColor(
    uint2 pixel,
    uint2 dimensions,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams,
    float debugMode)
{
    RAB_Surface surface;
    if (!PathTraceCleanRtxdiDiLoadGlassMaterialSurface(pixel, dimensions, surface))
    {
        return float3(0.0, 0.0, 0.0);
    }

    const PathTraceMaterialFeature feature = PathTraceCleanRtxdiDiGlassFeatureForSurface(surface);
    if (!PathTraceCleanRtxdiDiGlassFeatureSupported(feature))
    {
        return feature.materialKind == RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_GLASS
            ? float3(0.3, 0.0, 0.4)
            : float3(0.015, 0.015, 0.025);
    }

    const PathTraceCleanRtxdiDiGlassMaterialParams materialParams =
        PathTraceCleanRtxdiDiLoadGlassMaterialParams(surface, runtimeParams);
    const PathTraceCleanRtxdiDiGlassThinPayload payload =
        PathTraceCleanRtxdiDiBuildGlassThinPayload(surface, materialParams);
    const uint2 sourcePixel = PathTraceCleanRtxdiDiGlassRefractionSamplePixel(
        pixel,
        dimensions,
        surface,
        materialParams,
        payload);
    float4 candidate0;
    float4 candidate1;
    float4 candidate2;
    if (!PathTraceCleanRtxdiDiGlassBuildGuideCandidate(
            pixel,
            sourcePixel,
            dimensions,
            payload.weight,
            candidate0,
            candidate1,
            candidate2))
    {
        return float3(0.0, 0.0, 0.0);
    }

    if (debugMode < 2.5)
    {
        return saturate(float3(candidate0.xy * 0.5 + 0.5, candidate0.w / 4096.0));
    }
    if (debugMode < 3.5)
    {
        return saturate(float3(abs(candidate1.xy) / 32.0, candidate1.w));
    }

    const float2 sourceDeltaPixels = abs(float2(sourcePixel) - float2(pixel));
    const float offsetMagnitude = length(sourceDeltaPixels) / 16.0;
    return saturate(float3(sourceDeltaPixels / 16.0, offsetMagnitude));
}

[shader("raygeneration")]
void RayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dimensions = DispatchRaysDimensions().xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    const PathTraceMaterialFeatureRuntimeInfo runtimeInfo =
        PathTraceCleanRtxdiDiLoadMaterialFeatureRuntimeInfo();
    if (runtimeInfo.ready < 0.5 || runtimeInfo.writesOutputColor < 0.5)
    {
        return;
    }

    const PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams =
        PathTraceCleanRtxdiDiLoadMaterialFeatureRuntimeParams();
    if (runtimeInfo.debugMode >= 0.5)
    {
        SmokeOutput[pixel] = runtimeInfo.debugMode >= 1.5
            ? float4(PathTraceCleanRtxdiDiGlassGuideCandidateDebugColor(pixel, dimensions, runtimeParams, runtimeInfo.debugMode), 1.0)
            : PathTraceCleanRtxdiDiGlassDebugColor(pixel, dimensions, runtimeParams);
    }
    else
    {
        RAB_Surface surface;
        if (PathTraceCleanRtxdiDiLoadGlassMaterialSurface(pixel, dimensions, surface))
        {
            const PathTraceMaterialFeature feature = PathTraceCleanRtxdiDiGlassFeatureForSurface(surface);
            if (PathTraceCleanRtxdiDiGlassFeatureSupported(feature))
            {
                const PathTraceCleanRtxdiDiGlassComposeResult result =
                    PathTraceCleanRtxdiDiBuildGlassComposeResult(
                        surface,
                        pixel,
                        dimensions,
                        SmokeOutput[pixel],
                        PathTraceCleanRtxdiDiOutputColorSource,
                        runtimeParams);
                if (result.supported)
                {
                    SmokeOutput[pixel] = result.color;
                    PathTraceRRInputColor[pixel] = result.color;
                }
            }
        }
    }
}

#endif
