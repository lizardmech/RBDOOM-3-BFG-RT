#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_GLASS_COMPOSE_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_GLASS_COMPOSE_HLSLI

Texture2D<float4> PathTraceCleanRtxdiDiOutputColorSource : register(t89);

static const float RT_CLEAN_RTXDI_DI_GLASS_SUPPORTED_SOURCE_TAP_WEIGHT = 0.25;

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

float PathTraceCleanRtxdiDiGlassSourceTapMaterialWeight(int2 pixel, uint2 dimensions)
{
    RAB_Surface sourceSurface;
    if (!PathTraceCleanRtxdiDiLoadGlassMaterialSurface((uint2)pixel, dimensions, sourceSurface))
    {
        return 1.0;
    }

    return PathTraceCleanRtxdiDiGlassSurfaceSupported(sourceSurface)
        ? RT_CLEAN_RTXDI_DI_GLASS_SUPPORTED_SOURCE_TAP_WEIGHT
        : 1.0;
}

float4 PathTraceCleanRtxdiDiGlassLoadOutputSourceTap(
    Texture2D<float4> outputColorSource,
    int2 pixel,
    uint2 dimensions,
    out float validWeight)
{
    if (pixel.x < 0 ||
        pixel.y < 0 ||
        pixel.x >= (int)dimensions.x ||
        pixel.y >= (int)dimensions.y)
    {
        validWeight = 0.0;
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const float4 outputSource = outputColorSource.Load(int3(pixel, 0));
    validWeight = PathTraceCleanRtxdiDiGlassColorEnergy(outputSource) > 1.0e-5
        ? PathTraceCleanRtxdiDiGlassSourceTapMaterialWeight(pixel, dimensions)
        : 0.0;
    return outputSource;
}

float4 PathTraceCleanRtxdiDiGlassOutputSourceColorBilinear(
    Texture2D<float4> outputColorSource,
    float2 samplePixel,
    uint2 dimensions,
    float4 fallbackColor)
{
    const float2 basePixel = floor(samplePixel);
    const float2 fraction = samplePixel - basePixel;

    const int2 p00 = int2(basePixel);
    const int2 p10 = int2(basePixel + float2(1.0, 0.0));
    const int2 p01 = int2(basePixel + float2(0.0, 1.0));
    const int2 p11 = int2(basePixel + float2(1.0, 1.0));

    const float4 sampleWeights = float4(
        (1.0 - fraction.x) * (1.0 - fraction.y),
        fraction.x * (1.0 - fraction.y),
        (1.0 - fraction.x) * fraction.y,
        fraction.x * fraction.y);

    float tapValid00;
    float tapValid10;
    float tapValid01;
    float tapValid11;
    const float4 c00 = PathTraceCleanRtxdiDiGlassLoadOutputSourceTap(outputColorSource, p00, dimensions, tapValid00);
    const float4 c10 = PathTraceCleanRtxdiDiGlassLoadOutputSourceTap(outputColorSource, p10, dimensions, tapValid10);
    const float4 c01 = PathTraceCleanRtxdiDiGlassLoadOutputSourceTap(outputColorSource, p01, dimensions, tapValid01);
    const float4 c11 = PathTraceCleanRtxdiDiGlassLoadOutputSourceTap(outputColorSource, p11, dimensions, tapValid11);
    const float4 validWeights = sampleWeights * float4(tapValid00, tapValid10, tapValid01, tapValid11);
    const float validWeightSum = dot(validWeights, float4(1.0, 1.0, 1.0, 1.0));
    if (validWeightSum <= 1.0e-5)
    {
        return fallbackColor;
    }

    return (c00 * validWeights.x + c10 * validWeights.y + c01 * validWeights.z + c11 * validWeights.w) /
        validWeightSum;
}

void PathTraceCleanRtxdiDiStoreGlassComposedColor(uint2 pixel, float4 color)
{
    SmokeOutput[pixel] = color;
    PathTraceRRInputColor[pixel] = color;
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
    const float2 sourcePixel = PathTraceCleanRtxdiDiGlassRefractionSamplePosition(
        pixel,
        dimensions,
        surface,
        materialParams,
        payload);
    const float2 reflectedSourcePixel = PathTraceCleanRtxdiDiGlassReflectionSamplePosition(
        pixel,
        dimensions,
        surface,
        materialParams,
        payload);
    const float4 sourceColor = PathTraceCleanRtxdiDiGlassOutputSourceColorBilinear(
        PathTraceCleanRtxdiDiOutputColorSource,
        sourcePixel,
        dimensions,
        currentColor);
    const float4 reflectedSourceColor = PathTraceCleanRtxdiDiGlassOutputSourceColorBilinear(
        PathTraceCleanRtxdiDiOutputColorSource,
        reflectedSourcePixel,
        dimensions,
        currentColor);

    result.supported = true;
    result.color = PathTraceCleanRtxdiDiComposeThinGlassColor(
        currentColor,
        sourceColor,
        reflectedSourceColor,
        materialParams,
        payload);
    result.transmissionPayload = float4(payload.transmission, payload.weight);
    return result;
}

PathTraceCleanRtxdiDiGlassComposeResult PathTraceCleanRtxdiDiBuildGlassComposeResultForPixel(
    uint2 pixel,
    uint2 dimensions,
    float4 currentColor,
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
        runtimeParams);
}

#endif
