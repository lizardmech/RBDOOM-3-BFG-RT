#if defined(CLEAN_RTXDI_DI_TRANSMISSION_PRODUCER_ENTRY)

VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> PathTraceCleanRtxdiDiTransmissionOutput : register(u87);
Texture2D<float4> PathTraceCleanRtxdiDiOutputColorSource : register(t89);

float PathTraceCleanRoomTransmissionProducerColorEnergy(float4 color)
{
    return dot(abs(color.rgb), float3(1.0, 1.0, 1.0));
}

float4 PathTraceCleanRoomTransmissionProducerSourceColor(uint2 pixel, float4 fallbackColor)
{
    const float4 outputSource = PathTraceCleanRtxdiDiOutputColorSource.Load(int3(pixel, 0));
    const float4 rrInputSource = PathTraceRRInputColor[pixel];
    if (PathTraceCleanRoomTransmissionProducerColorEnergy(outputSource) > 1.0e-5)
    {
        return outputSource;
    }
    if (PathTraceCleanRoomTransmissionProducerColorEnergy(rrInputSource) > 1.0e-5)
    {
        return rrInputSource;
    }
    return fallbackColor;
}

float4 PathTraceCleanRoomTransmissionProducerPayload(
    uint2 pixel,
    uint2 dimensions,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams)
{
    RAB_Surface surface;
    if (!PathTraceCleanRtxdiDiLoadGlassMaterialSurface(pixel, dimensions, surface))
    {
        return float4(1.0, 1.0, 1.0, 0.0);
    }

    if (!PathTraceCleanRtxdiDiGlassSurfaceSupported(surface))
    {
        return float4(1.0, 1.0, 1.0, 0.0);
    }

    const PathTraceCleanRtxdiDiGlassThinPayload payload =
        PathTraceCleanRtxdiDiBuildGlassThinPayload(surface, runtimeParams);
    return float4(payload.transmission, payload.weight);
}

float4 PathTraceCleanRoomTransmissionProducerDebugColor(
    uint2 pixel,
    uint2 dimensions,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams)
{
    RAB_Surface surface;
    if (!PathTraceCleanRtxdiDiLoadGlassMaterialSurface(pixel, dimensions, surface))
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    if (!PathTraceCleanRtxdiDiGlassSurfaceSupported(surface))
    {
        return float4(0.02, 0.02, 0.02, 1.0);
    }

    const PathTraceCleanRtxdiDiGlassThinPayload payload =
        PathTraceCleanRtxdiDiBuildGlassThinPayload(surface, runtimeParams);
    return float4(saturate(payload.transmission + payload.reflection * 0.25), 1.0);
}

float4 PathTraceCleanRoomTransmissionProducerComposeColor(
    uint2 pixel,
    uint2 dimensions,
    float4 currentColor,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams)
{
    RAB_Surface surface;
    if (!PathTraceCleanRtxdiDiLoadGlassMaterialSurface(pixel, dimensions, surface))
    {
        return currentColor;
    }

    if (!PathTraceCleanRtxdiDiGlassSurfaceSupported(surface))
    {
        return currentColor;
    }

    const PathTraceCleanRtxdiDiGlassMaterialParams materialParams =
        PathTraceCleanRtxdiDiLoadGlassMaterialParams(surface, runtimeParams);
    const PathTraceCleanRtxdiDiGlassThinPayload payload =
        PathTraceCleanRtxdiDiBuildGlassThinPayload(surface, materialParams);
    return PathTraceCleanRtxdiDiComposeThinGlassColor(currentColor, currentColor, materialParams, payload);
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
    const float4 payload = PathTraceCleanRoomTransmissionProducerPayload(pixel, dimensions, runtimeParams);
    PathTraceCleanRtxdiDiTransmissionOutput[pixel] = payload;
    const PathTraceMaterialFeatureRuntimeInfo runtimeInfo =
        PathTraceCleanRtxdiDiLoadMaterialFeatureRuntimeInfo();
    if (runtimeInfo.writesOutputColor)
    {
        if (runtimeInfo.debugMode >= 0.5)
        {
            SmokeOutput[pixel] = PathTraceCleanRoomTransmissionProducerDebugColor(pixel, dimensions, runtimeParams);
        }
        else
        {
            const float4 baseColor = PathTraceCleanRoomTransmissionProducerSourceColor(pixel, SmokeOutput[pixel]);
            const float4 composedColor = PathTraceCleanRoomTransmissionProducerComposeColor(
                pixel,
                dimensions,
                baseColor,
                runtimeParams);
            SmokeOutput[pixel] = composedColor;
            PathTraceRRInputColor[pixel] = composedColor;
        }
    }
}

#endif
