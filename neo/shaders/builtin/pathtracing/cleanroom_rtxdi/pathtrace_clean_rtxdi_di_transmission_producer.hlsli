#if defined(CLEAN_RTXDI_DI_TRANSMISSION_PRODUCER_ENTRY)

VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> PathTraceCleanRtxdiDiTransmissionOutput : register(u87);

float4 PathTraceCleanRoomTransmissionProducerPayload(
    uint2 pixel,
    uint2 dimensions,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams)
{
    PathTracePrimarySurfaceRecord record;
    if (!PathTraceCleanRoomLoadSurfaceRecord(pixel, dimensions, record))
    {
        return float4(1.0, 1.0, 1.0, 0.0);
    }

    const RAB_Surface surface = PathTraceCleanRoomMaterialSurfaceFromRecord(record);
    if (!PathTraceCleanRtxdiDiMaterialSupportsTransmission(surface))
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
    PathTracePrimarySurfaceRecord record;
    if (!PathTraceCleanRoomLoadSurfaceRecord(pixel, dimensions, record))
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    const RAB_Surface surface = PathTraceCleanRoomMaterialSurfaceFromRecord(record);
    if (!PathTraceCleanRtxdiDiMaterialSupportsTransmission(surface))
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
    PathTracePrimarySurfaceRecord record;
    if (!PathTraceCleanRoomLoadSurfaceRecord(pixel, dimensions, record))
    {
        return currentColor;
    }

    const RAB_Surface surface = PathTraceCleanRoomMaterialSurfaceFromRecord(record);
    if (!PathTraceCleanRtxdiDiMaterialSupportsTransmission(surface))
    {
        return currentColor;
    }

    const PathTraceCleanRtxdiDiGlassMaterialParams materialParams =
        PathTraceCleanRtxdiDiLoadGlassMaterialParams(surface, runtimeParams);
    const PathTraceCleanRtxdiDiGlassThinPayload payload =
        PathTraceCleanRtxdiDiBuildGlassThinPayload(surface, materialParams);
    return PathTraceCleanRtxdiDiComposeThinGlassColor(currentColor, materialParams, payload);
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
        LoadPathTraceMaterialFeatureRuntimeInfo(PathTraceMaterialFeatureRuntimeInfoPacked);
    if (runtimeInfo.writesOutputColor)
    {
        if (runtimeInfo.debugMode >= 0.5)
        {
            SmokeOutput[pixel] = PathTraceCleanRoomTransmissionProducerDebugColor(pixel, dimensions, runtimeParams);
        }
        else
        {
            SmokeOutput[pixel] = PathTraceCleanRoomTransmissionProducerComposeColor(
                pixel,
                dimensions,
                SmokeOutput[pixel],
                runtimeParams);
        }
    }
}

#endif
