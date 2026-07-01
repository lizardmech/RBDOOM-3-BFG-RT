#if defined(CLEAN_RTXDI_DI_TRANSMISSION_PRODUCER_ENTRY)

VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> PathTraceCleanRtxdiDiTransmissionOutput : register(u87);

float4 PathTraceCleanRoomTransmissionProducerSentinel(uint2 pixel, uint2 dimensions)
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

    return float4(0.0, 0.85, 1.0, 1.0);
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

    const float4 sentinel = PathTraceCleanRoomTransmissionProducerSentinel(pixel, dimensions);
    PathTraceCleanRtxdiDiTransmissionOutput[pixel] = sentinel;
    const PathTraceMaterialFeatureRuntimeInfo runtimeInfo =
        LoadPathTraceMaterialFeatureRuntimeInfo(PathTraceMaterialFeatureRuntimeInfoPacked);
    if (runtimeInfo.writesOutputColor)
    {
        SmokeOutput[pixel] = sentinel;
    }
}

#endif
