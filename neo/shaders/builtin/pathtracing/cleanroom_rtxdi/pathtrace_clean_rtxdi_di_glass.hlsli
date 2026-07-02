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
        LoadPathTraceMaterialFeatureRuntimeInfo(PathTraceMaterialFeatureRuntimeInfoPacked);
    if (runtimeInfo.ready < 0.5 || runtimeInfo.writesOutputColor < 0.5)
    {
        return;
    }

    const PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams =
        PathTraceCleanRtxdiDiLoadMaterialFeatureRuntimeParams();
    if (runtimeInfo.debugMode >= 0.5)
    {
        SmokeOutput[pixel] = PathTraceCleanRtxdiDiGlassDebugColor(pixel, dimensions, runtimeParams);
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
            }
        }
    }
}

#endif
