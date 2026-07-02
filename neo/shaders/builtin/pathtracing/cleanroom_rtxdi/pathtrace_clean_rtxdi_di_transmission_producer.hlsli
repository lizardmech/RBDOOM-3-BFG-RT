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
        SmokeOutput[pixel] = PathTraceCleanRoomTransmissionProducerDebugColor(pixel, dimensions, runtimeParams);
    }
}

#endif
