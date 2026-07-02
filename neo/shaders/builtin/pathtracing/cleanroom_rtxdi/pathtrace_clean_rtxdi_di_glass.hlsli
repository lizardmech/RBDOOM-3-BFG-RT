#if defined(CLEAN_RTXDI_DI_GLASS_ENTRY)

Texture2D<float4> PathTraceCleanRtxdiDiOutputColorSource : register(t89);
VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> PathTraceCleanRtxdiDiGlassGuideCandidate0 : register(u90);
VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> PathTraceCleanRtxdiDiGlassGuideCandidate1 : register(u91);
VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> PathTraceCleanRtxdiDiGlassGuideCandidate2 : register(u92);

void PathTraceCleanRtxdiDiGlassClearGuideCandidate(uint2 pixel)
{
    PathTraceCleanRtxdiDiGlassGuideCandidate0[pixel] = float4(0.0, 0.0, 0.0, 0.0);
    PathTraceCleanRtxdiDiGlassGuideCandidate1[pixel] = float4(0.0, 0.0, 0.0, 0.0);
    PathTraceCleanRtxdiDiGlassGuideCandidate2[pixel] = float4(0.0, 0.0, 0.0, 0.0);
}

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

void PathTraceCleanRtxdiDiGlassExportRrSpecularGuide(
    uint2 pixel,
    PathTraceCleanRtxdiDiGlassMaterialParams materialParams,
    PathTraceCleanRtxdiDiGlassThinPayload payload)
{
    const float3 currentSpecular = saturate(PathTraceRRGuideSpecularAlbedo[pixel].rgb);
    const float3 glassSpecular = saturate(
        payload.reflection *
        max(materialParams.reflectionBoost, 1.0) *
        saturate(payload.weight));
    PathTraceRRGuideSpecularAlbedo[pixel] = float4(max(currentSpecular, glassSpecular), 1.0);
}

void PathTraceCleanRtxdiDiGlassExportRrInputColor(
    uint2 pixel,
    float4 sourceColor,
    PathTraceCleanRtxdiDiGlassMaterialParams materialParams,
    PathTraceCleanRtxdiDiGlassThinPayload payload)
{
    PathTraceRRInputColor[pixel] = PathTraceCleanRtxdiDiComposeThinGlassColor(
        PathTraceRRInputColor[pixel],
        sourceColor,
        materialParams,
        payload);
}

void PathTraceCleanRtxdiDiGlassExportGuideCandidate(
    uint2 pixel,
    uint2 sourcePixel,
    uint2 dimensions,
    float glassWeight)
{
    PathTracePrimarySurfaceRecord sourceRecord;
    if (!PathTraceCleanRoomLoadSurfaceRecord(sourcePixel, dimensions, sourceRecord))
    {
        return;
    }

    const RAB_Surface sourceSurface = PathTraceCleanRoomSurfaceForView(sourceRecord);
    if (!RAB_IsSurfaceValid(sourceSurface))
    {
        return;
    }

    const float4 sourceNormalRoughness = PathTraceRRGuideNormalRoughness[sourcePixel];
    const float sourceDepth = PathTraceRRGuideDepth[sourcePixel];
    const float2 sourceMotion = PathTraceRRMotionVectors[sourcePixel] + (float2(sourcePixel) - float2(pixel));
    const uint sourceResetMask = PathTraceRRGuideResetMask[sourcePixel];
    const float roughness = saturate(sourceNormalRoughness.w);
    const float validWeight = saturate(glassWeight);

    PathTraceCleanRtxdiDiGlassGuideCandidate0[pixel] =
        float4(sourceNormalRoughness.xyz, sourceDepth);
    PathTraceCleanRtxdiDiGlassGuideCandidate1[pixel] =
        float4(sourceMotion.xy, float(sourceResetMask), validWeight);
    PathTraceCleanRtxdiDiGlassGuideCandidate2[pixel] =
        float4(sourceSurface.worldPos, roughness);
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
    PathTraceCleanRtxdiDiGlassExportGuideCandidate(pixel, sourcePixel, dimensions, payload.weight);

    const float4 candidate0 = PathTraceCleanRtxdiDiGlassGuideCandidate0[pixel];
    const float4 candidate1 = PathTraceCleanRtxdiDiGlassGuideCandidate1[pixel];
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

    PathTraceCleanRtxdiDiGlassClearGuideCandidate(pixel);

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
                const float4 sourceColor = PathTraceCleanRtxdiDiOutputColorSource.Load(int3(sourcePixel, 0));
                SmokeOutput[pixel] = PathTraceCleanRtxdiDiComposeThinGlassColor(
                    SmokeOutput[pixel],
                    sourceColor,
                    materialParams,
                    payload);
                PathTraceCleanRtxdiDiGlassExportRrSpecularGuide(pixel, materialParams, payload);
                PathTraceCleanRtxdiDiGlassExportRrInputColor(pixel, sourceColor, materialParams, payload);
                PathTraceCleanRtxdiDiGlassExportGuideCandidate(pixel, sourcePixel, dimensions, payload.weight);
            }
        }
    }
}

#endif
