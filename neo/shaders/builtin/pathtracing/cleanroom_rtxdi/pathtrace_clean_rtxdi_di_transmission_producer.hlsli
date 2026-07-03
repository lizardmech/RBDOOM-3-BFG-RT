#if defined(CLEAN_RTXDI_DI_TRANSMISSION_PRODUCER_ENTRY)

VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> PathTraceCleanRtxdiDiTransmissionOutput : register(u87);

float4 PathTraceCleanRtxdiDiTransmissionProducerSourceColor(uint2 pixel, float4 fallbackColor)
{
    return PathTraceCleanRtxdiDiGlassOutputSourceColor(
        PathTraceCleanRtxdiDiOutputColorSource,
        pixel,
        fallbackColor);
}

float4 PathTraceCleanRtxdiDiTransmissionProducerPayload(
    uint2 pixel,
    uint2 dimensions,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams)
{
    return PathTraceCleanRtxdiDiGlassTransmissionPayloadForPixel(pixel, dimensions, runtimeParams);
}

float4 PathTraceCleanRtxdiDiTransmissionProducerDebugColor(
    uint2 pixel,
    uint2 dimensions,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams)
{
    return PathTraceCleanRtxdiDiGlassDebugColorForPixel(
        pixel,
        dimensions,
        runtimeParams,
        float4(0.0, 0.0, 0.0, 1.0),
        float4(0.02, 0.02, 0.02, 1.0),
        float4(0.02, 0.02, 0.02, 1.0));
}

float4 PathTraceCleanRtxdiDiTransmissionProducerComposeColor(
    uint2 pixel,
    uint2 dimensions,
    float4 currentColor,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams)
{
    const PathTraceCleanRtxdiDiGlassComposeResult result =
        PathTraceCleanRtxdiDiBuildGlassComposeResultForPixel(
            pixel,
            dimensions,
            currentColor,
            PathTraceCleanRtxdiDiOutputColorSource,
            runtimeParams);
    return result.color;
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

    const PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams =
        PathTraceCleanRtxdiDiLoadMaterialFeatureRuntimeParams();
    const float4 payload = PathTraceCleanRtxdiDiTransmissionProducerPayload(pixel, dimensions, runtimeParams);
    PathTraceCleanRtxdiDiTransmissionOutput[pixel] = payload;
    const PathTraceMaterialFeatureRuntimeInfo runtimeInfo =
        PathTraceCleanRtxdiDiLoadMaterialFeatureRuntimeInfo();
    if (runtimeInfo.writesOutputColor)
    {
        if (runtimeInfo.debugMode >= 0.5)
        {
            SmokeOutput[pixel] = PathTraceCleanRtxdiDiTransmissionProducerDebugColor(pixel, dimensions, runtimeParams);
        }
        else
        {
            const float4 baseColor = PathTraceCleanRtxdiDiTransmissionProducerSourceColor(pixel, SmokeOutput[pixel]);
            const float4 composedColor = PathTraceCleanRtxdiDiTransmissionProducerComposeColor(
                pixel,
                dimensions,
                baseColor,
                runtimeParams);
            PathTraceCleanRtxdiDiStoreGlassComposedColor(pixel, composedColor);
        }
    }
}

#endif
