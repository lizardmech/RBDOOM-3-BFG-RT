#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_GLASS_COMPOSE_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_GLASS_COMPOSE_HLSLI

Texture2D<float4> PathTraceCleanRtxdiDiOutputColorSource : register(t89);

static const float RT_CLEAN_RTXDI_DI_GLASS_SUPPORTED_SOURCE_TAP_WEIGHT = 0.25;
static const float RT_CLEAN_RTXDI_DI_GLASS_SOURCE_DEPTH_EPSILON = 0.05;
static const float RT_CLEAN_RTXDI_DI_GLASS_CHROMATIC_REFRACTION_SCALE = 0.06;

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

float PathTraceCleanRtxdiDiGlassSourceTapMaterialWeight(
    int2 pixel,
    uint2 dimensions,
    RAB_Surface referenceSurface,
    bool requireBehindSurface)
{
    RAB_Surface sourceSurface;
    if (!PathTraceCleanRtxdiDiLoadGlassMaterialSurface((uint2)pixel, dimensions, sourceSurface))
    {
        return 1.0;
    }

    if (PathTraceCleanRtxdiDiGlassSurfaceSupported(sourceSurface))
    {
        return RT_CLEAN_RTXDI_DI_GLASS_SUPPORTED_SOURCE_TAP_WEIGHT;
    }

    if (requireBehindSurface && RAB_IsSurfaceValid(referenceSurface))
    {
        const float referenceDepth = RAB_GetSurfaceLinearDepth(referenceSurface);
        const float sourceDepth = RAB_GetSurfaceLinearDepth(sourceSurface);
        const float depthEpsilon = max(
            RT_CLEAN_RTXDI_DI_GLASS_SOURCE_DEPTH_EPSILON,
            abs(referenceDepth) * 0.001);
        if (sourceDepth <= referenceDepth + depthEpsilon)
        {
            return 0.0;
        }
    }

    return 1.0;
}

float4 PathTraceCleanRtxdiDiGlassLoadOutputSourceTap(
    Texture2D<float4> outputColorSource,
    int2 pixel,
    uint2 dimensions,
    RAB_Surface referenceSurface,
    bool requireBehindSurface,
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
        ? PathTraceCleanRtxdiDiGlassSourceTapMaterialWeight(pixel, dimensions, referenceSurface, requireBehindSurface)
        : 0.0;
    return outputSource;
}

float4 PathTraceCleanRtxdiDiGlassOutputSourceColorBilinearWithValidity(
    Texture2D<float4> outputColorSource,
    float2 samplePixel,
    uint2 dimensions,
    RAB_Surface referenceSurface,
    bool requireBehindSurface,
    float4 fallbackColor,
    out float validWeightSum)
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
    const float4 c00 = PathTraceCleanRtxdiDiGlassLoadOutputSourceTap(
        outputColorSource, p00, dimensions, referenceSurface, requireBehindSurface, tapValid00);
    const float4 c10 = PathTraceCleanRtxdiDiGlassLoadOutputSourceTap(
        outputColorSource, p10, dimensions, referenceSurface, requireBehindSurface, tapValid10);
    const float4 c01 = PathTraceCleanRtxdiDiGlassLoadOutputSourceTap(
        outputColorSource, p01, dimensions, referenceSurface, requireBehindSurface, tapValid01);
    const float4 c11 = PathTraceCleanRtxdiDiGlassLoadOutputSourceTap(
        outputColorSource, p11, dimensions, referenceSurface, requireBehindSurface, tapValid11);
    const float4 validWeights = sampleWeights * float4(tapValid00, tapValid10, tapValid01, tapValid11);
    validWeightSum = dot(validWeights, float4(1.0, 1.0, 1.0, 1.0));
    if (validWeightSum <= 1.0e-5)
    {
        return fallbackColor;
    }

    return (c00 * validWeights.x + c10 * validWeights.y + c01 * validWeights.z + c11 * validWeights.w) /
        validWeightSum;
}

float4 PathTraceCleanRtxdiDiGlassOutputSourceColorBilinear(
    Texture2D<float4> outputColorSource,
    float2 samplePixel,
    uint2 dimensions,
    RAB_Surface referenceSurface,
    bool requireBehindSurface,
    float4 fallbackColor)
{
    float validWeightSum;
    return PathTraceCleanRtxdiDiGlassOutputSourceColorBilinearWithValidity(
        outputColorSource,
        samplePixel,
        dimensions,
        referenceSurface,
        requireBehindSurface,
        fallbackColor,
        validWeightSum);
}

float4 PathTraceCleanRtxdiDiGlassOutputSourceColorChromaticWithValidity(
    Texture2D<float4> outputColorSource,
    float2 currentPixel,
    float2 refractedSamplePixel,
    uint2 dimensions,
    RAB_Surface referenceSurface,
    bool requireBehindSurface,
    float4 fallbackColor,
    out float validWeight)
{
    const float2 refractedOffset = refractedSamplePixel - currentPixel;
    const float2 channelOffset = refractedOffset * RT_CLEAN_RTXDI_DI_GLASS_CHROMATIC_REFRACTION_SCALE;
    float redValidity;
    float greenValidity;
    float blueValidity;
    const float4 redSource = PathTraceCleanRtxdiDiGlassOutputSourceColorBilinearWithValidity(
        outputColorSource,
        refractedSamplePixel - channelOffset,
        dimensions,
        referenceSurface,
        requireBehindSurface,
        fallbackColor,
        redValidity);
    const float4 greenSource = PathTraceCleanRtxdiDiGlassOutputSourceColorBilinearWithValidity(
        outputColorSource,
        refractedSamplePixel,
        dimensions,
        referenceSurface,
        requireBehindSurface,
        fallbackColor,
        greenValidity);
    const float4 blueSource = PathTraceCleanRtxdiDiGlassOutputSourceColorBilinearWithValidity(
        outputColorSource,
        refractedSamplePixel + channelOffset,
        dimensions,
        referenceSurface,
        requireBehindSurface,
        fallbackColor,
        blueValidity);

    validWeight = min(redValidity, min(greenValidity, blueValidity));
    return float4(redSource.r, greenSource.g, blueSource.b, greenSource.a);
}

float4 PathTraceCleanRtxdiDiGlassOutputSourceColorChromatic(
    Texture2D<float4> outputColorSource,
    float2 currentPixel,
    float2 refractedSamplePixel,
    uint2 dimensions,
    RAB_Surface referenceSurface,
    float4 fallbackColor)
{
    float validWeight;
    return PathTraceCleanRtxdiDiGlassOutputSourceColorChromaticWithValidity(
        outputColorSource,
        currentPixel,
        refractedSamplePixel,
        dimensions,
        referenceSurface,
        true,
        fallbackColor,
        validWeight);
}

void PathTraceCleanRtxdiDiStoreGlassComposedColor(uint2 pixel, float4 color)
{
    SmokeOutput[pixel] = color;
    PathTraceRRInputColor[pixel] = color;
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

#endif
