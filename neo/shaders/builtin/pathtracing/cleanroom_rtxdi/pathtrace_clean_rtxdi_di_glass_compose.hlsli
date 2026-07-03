#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_GLASS_COMPOSE_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_GLASS_COMPOSE_HLSLI

struct PathTraceCleanRtxdiDiGlassComposeResult
{
    bool supported;
    float4 color;
    float4 transmissionPayload;
};

PathTraceCleanRtxdiDiGlassComposeResult PathTraceCleanRtxdiDiEmptyGlassComposeResult(float4 currentColor)
{
    PathTraceCleanRtxdiDiGlassComposeResult result;
    result.supported = false;
    result.color = currentColor;
    result.transmissionPayload = float4(1.0, 1.0, 1.0, 0.0);
    return result;
}

float PathTraceCleanRtxdiDiGlassColorEnergy(float4 color)
{
    return dot(abs(color.rgb), float3(1.0, 1.0, 1.0));
}

float4 PathTraceCleanRtxdiDiGlassOutputSourceColor(
    Texture2D<float4> outputColorSource,
    uint2 pixel,
    float4 fallbackColor)
{
    const float4 outputSource = outputColorSource.Load(int3(pixel, 0));
    if (PathTraceCleanRtxdiDiGlassColorEnergy(outputSource) > 1.0e-5)
    {
        return outputSource;
    }
    return fallbackColor;
}

float4 PathTraceCleanRtxdiDiGlassTransmissionPayload(
    RAB_Surface surface,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams)
{
    if (!PathTraceCleanRtxdiDiGlassSurfaceSupported(surface))
    {
        return float4(1.0, 1.0, 1.0, 0.0);
    }

    const PathTraceCleanRtxdiDiGlassThinPayload payload =
        PathTraceCleanRtxdiDiBuildGlassThinPayload(surface, runtimeParams);
    return float4(payload.transmission, payload.weight);
}

float4 PathTraceCleanRtxdiDiGlassTransmissionPayloadForPixel(
    uint2 pixel,
    uint2 dimensions,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams)
{
    RAB_Surface surface;
    if (!PathTraceCleanRtxdiDiLoadGlassMaterialSurface(pixel, dimensions, surface))
    {
        return float4(1.0, 1.0, 1.0, 0.0);
    }

    return PathTraceCleanRtxdiDiGlassTransmissionPayload(surface, runtimeParams);
}

float4 PathTraceCleanRtxdiDiGlassDebugColorForPixel(
    uint2 pixel,
    uint2 dimensions,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams,
    float4 invalidColor,
    float4 unsupportedColor,
    float4 translucentUnsupportedColor)
{
    RAB_Surface surface;
    if (!PathTraceCleanRtxdiDiLoadGlassMaterialSurface(pixel, dimensions, surface))
    {
        return invalidColor;
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

    return feature.materialKind == RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_GLASS
        ? translucentUnsupportedColor
        : unsupportedColor;
}

PathTraceCleanRtxdiDiGlassComposeResult PathTraceCleanRtxdiDiBuildGlassComposeResult(
    RAB_Surface surface,
    uint2 pixel,
    uint2 dimensions,
    float4 currentColor,
    Texture2D<float4> outputColorSource,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams)
{
    PathTraceCleanRtxdiDiGlassComposeResult result =
        PathTraceCleanRtxdiDiEmptyGlassComposeResult(currentColor);

    if (!PathTraceCleanRtxdiDiGlassSurfaceSupported(surface))
    {
        return result;
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
    const float4 sourceColor = PathTraceCleanRtxdiDiGlassOutputSourceColor(
        outputColorSource,
        sourcePixel,
        currentColor);

    result.supported = true;
    result.color = PathTraceCleanRtxdiDiComposeThinGlassColor(
        currentColor,
        sourceColor,
        materialParams,
        payload);
    result.transmissionPayload = float4(payload.transmission, payload.weight);
    return result;
}

PathTraceCleanRtxdiDiGlassComposeResult PathTraceCleanRtxdiDiBuildGlassComposeResultForPixel(
    uint2 pixel,
    uint2 dimensions,
    float4 currentColor,
    Texture2D<float4> outputColorSource,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams)
{
    RAB_Surface surface;
    if (!PathTraceCleanRtxdiDiLoadGlassMaterialSurface(pixel, dimensions, surface))
    {
        return PathTraceCleanRtxdiDiEmptyGlassComposeResult(currentColor);
    }

    return PathTraceCleanRtxdiDiBuildGlassComposeResult(
        surface,
        pixel,
        dimensions,
        currentColor,
        outputColorSource,
        runtimeParams);
}

#endif
