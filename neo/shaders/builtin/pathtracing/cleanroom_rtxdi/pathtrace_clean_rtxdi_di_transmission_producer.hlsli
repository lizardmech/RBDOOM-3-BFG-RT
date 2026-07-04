#if defined(CLEAN_RTXDI_DI_TRANSMISSION_PRODUCER_ENTRY)

VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> PathTraceCleanRtxdiDiTransmissionOutput : register(u87);

uint PathTraceCleanRtxdiDiTransmissionReservoirBlockCount(uint dimension)
{
    return (dimension + RTXDI_RESERVOIR_BLOCK_SIZE - 1u) / RTXDI_RESERVOIR_BLOCK_SIZE;
}

RTXDI_ReservoirBufferParameters PathTraceCleanRtxdiDiTransmissionReservoirParams(uint2 dimensions)
{
    RTXDI_ReservoirBufferParameters params = (RTXDI_ReservoirBufferParameters)0;
    const uint blockCountX = max(PathTraceCleanRtxdiDiTransmissionReservoirBlockCount(dimensions.x), 1u);
    const uint blockCountY = max(PathTraceCleanRtxdiDiTransmissionReservoirBlockCount(dimensions.y), 1u);
    params.reservoirBlockRowPitch = blockCountX * RTXDI_RESERVOIR_BLOCK_SIZE * RTXDI_RESERVOIR_BLOCK_SIZE;
    params.reservoirArrayPitch = params.reservoirBlockRowPitch * blockCountY;
    return params;
}

bool PathTraceCleanRtxdiDiTransmissionLoadSpatialReservoir(
    uint2 pixel,
    uint2 dimensions,
    out RTXDI_DIReservoir reservoir)
{
    reservoir = RTXDI_EmptyDIReservoir();
    const uint width = CleanRtxdiDiWidth != 0u ? CleanRtxdiDiWidth : dimensions.x;
    const uint height = CleanRtxdiDiHeight != 0u ? CleanRtxdiDiHeight : dimensions.y;
    if (width == 0u || height == 0u || pixel.x >= width || pixel.y >= height)
    {
        return false;
    }

    const uint reservoirIndex = RTXDI_ReservoirPositionToPointer(
        PathTraceCleanRtxdiDiTransmissionReservoirParams(uint2(width, height)),
        pixel,
        0u);
    if (reservoirIndex >= CleanRtxdiDiReservoirCount)
    {
        return false;
    }

    reservoir = RTXDI_UnpackDIReservoir(CleanRtxdiDiSpatialReservoirs[reservoirIndex]);
    return RTXDI_IsValidDIReservoir(reservoir);
}

float4 PathTraceCleanRtxdiDiTransmissionProducerDebugColor(
    uint2 pixel,
    uint2 dimensions,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams,
    float debugMode)
{
    if (debugMode >= 3.5)
    {
        // PSR reservoir diagnostic:
        //   green = PSR pixel has a valid post-spatial DI reservoir
        //   blue  = PSR replacement happened, but no valid reservoir reached compose
        //   yellow/red/gray match the basic PSR hit diagnostic below
        const float4 psrPayload = PathTraceCleanRtxdiDiTransmissionOutput[pixel];
        if (psrPayload.a > 0.5)
        {
            RTXDI_DIReservoir reservoir;
            if (PathTraceCleanRtxdiDiTransmissionLoadSpatialReservoir(pixel, dimensions, reservoir) &&
                RTXDI_IsValidDIReservoir(reservoir) &&
                reservoir.M > 0.0)
            {
                const float target = saturate(reservoir.targetPdf * 0.1);
                return float4(0.05 + target, 0.9, 0.05, 1.0);
            }
            return float4(0.05, 0.12, 0.95, 1.0);
        }
        if (psrPayload.a > 0.1)
        {
            return float4(0.9, 0.9, 0.05, 1.0);
        }

        RAB_Surface surface;
        const bool glassNow =
            PathTraceCleanRtxdiDiLoadGlassMaterialSurface(pixel, dimensions, surface) &&
            PathTraceCleanRtxdiDiGlassSurfaceSupported(surface);
        return glassNow
            ? float4(0.9, 0.05, 0.05, 1.0)
            : float4(0.15, 0.15, 0.15, 1.0);
    }

    if (debugMode >= 2.5)
    {
        // PSR diagnostic:
        //   green  = record replaced this frame (PSR fully working)
        //   yellow = PSR phase saw glass but its continuation trace missed
        //   red    = pixel still classifies as glass; PSR phase never touched it
        //   gray   = not glass
        const float4 psrPayload = PathTraceCleanRtxdiDiTransmissionOutput[pixel];
        if (psrPayload.a > 0.5)
        {
            return float4(0.05, 0.9, 0.05, 1.0);
        }
        if (psrPayload.a > 0.1)
        {
            return float4(0.9, 0.9, 0.05, 1.0);
        }

        RAB_Surface surface;
        const bool glassNow =
            PathTraceCleanRtxdiDiLoadGlassMaterialSurface(pixel, dimensions, surface) &&
            PathTraceCleanRtxdiDiGlassSurfaceSupported(surface);
        return glassNow
            ? float4(0.9, 0.05, 0.05, 1.0)
            : float4(0.15, 0.15, 0.15, 1.0);
    }

    if (debugMode >= 1.5)
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
        float strictValidity;
        float relaxedValidity;
        const float4 invalidSourceColor = float4(0.0, 0.0, 0.0, 0.0);
        PathTraceCleanRtxdiDiGlassOutputSourceColorChromaticWithValidity(
            PathTraceCleanRtxdiDiOutputColorSource,
            float2(pixel),
            sourcePixel,
            dimensions,
            surface,
            true,
            invalidSourceColor,
            strictValidity);
        PathTraceCleanRtxdiDiGlassOutputSourceColorChromaticWithValidity(
            PathTraceCleanRtxdiDiOutputColorSource,
            float2(pixel),
            sourcePixel,
            dimensions,
            surface,
            false,
            invalidSourceColor,
            relaxedValidity);
        const float currentSourceEnergy = saturate(
            PathTraceCleanRtxdiDiGlassColorEnergy(
                PathTraceCleanRtxdiDiOutputColorSource.Load(int3(pixel, 0))) * 0.1);
        return float4(
            saturate(strictValidity),
            saturate(relaxedValidity),
            currentSourceEnergy,
            1.0);
    }

    return PathTraceCleanRtxdiDiGlassDebugColorForPixel(
        pixel,
        dimensions,
        runtimeParams,
        float4(0.0, 0.0, 0.0, 1.0),
        float4(0.02, 0.02, 0.02, 1.0),
        float4(0.02, 0.02, 0.02, 1.0));
}

void PathTraceCleanRtxdiDiTransmissionPsrPhase(
    uint2 pixel,
    uint2 dimensions,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams)
{
    PathTraceCleanRtxdiDiTransmissionOutput[pixel] = float4(1.0, 1.0, 1.0, 0.0);

    RAB_Surface glassSurface;
    if (!PathTraceCleanRtxdiDiLoadGlassMaterialSurface(pixel, dimensions, glassSurface) ||
        !PathTraceCleanRtxdiDiGlassSurfaceSupported(glassSurface))
    {
        return;
    }

    const PathTraceCleanRtxdiDiGlassMaterialParams materialParams =
        PathTraceCleanRtxdiDiLoadGlassMaterialParams(glassSurface, runtimeParams);
    const PathTraceCleanRtxdiDiGlassThinPayload glassPayload =
        PathTraceCleanRtxdiDiBuildGlassThinPayload(glassSurface, materialParams);

    // Sentinel alpha 0.25: glass detected, continuation trace pending/missed.
    // Overwritten with 1.0 below when the record replacement succeeds.
    PathTraceCleanRtxdiDiTransmissionOutput[pixel] = float4(1.0, 1.0, 1.0, 0.25);

    PathTraceCleanRtxdiPayload hitPayload;
    float3 hitPosition;
    float3 rayDirection;
    if (!PathTraceCleanRtxdiDiTraceTransmissionHit(glassSurface, hitPayload, hitPosition, rayDirection))
    {
        return;
    }

    RAB_Surface hitSurface;
    if (!PathTraceCleanRtxdiDiBuildResolvedSurfaceFromTraceHit(hitPayload, hitPosition, rayDirection, hitSurface))
    {
        return;
    }

    if (!PathTraceCleanRtxdiDiPublishResolvedPrimarySurface(pixel, dimensions, hitSurface))
    {
        return;
    }

    PathTraceCleanRtxdiDiTransmissionOutput[pixel] = float4(saturate(glassPayload.transmission), 1.0);
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
    if ((CleanRtxdiDiFlags & CLEAN_FLAG_TRANSMISSION_PSR_PHASE) != 0u)
    {
        PathTraceCleanRtxdiDiTransmissionPsrPhase(pixel, dimensions, runtimeParams);
        return;
    }

    // Post-DI phase is debug-only for transmission. Glass owns normal sidecar
    // compose from the shaded output-color source.
    const PathTraceMaterialFeatureRuntimeInfo runtimeInfo =
        PathTraceCleanRtxdiDiLoadMaterialFeatureRuntimeInfo();
    if (runtimeInfo.writesOutputColor && runtimeInfo.debugMode >= 0.5)
    {
        // Route debug output through the composed-color store so it also lands
        // in the DLSS-RR input; presentation may not read SmokeOutput.
        PathTraceCleanRtxdiDiStoreGlassComposedColor(
            pixel,
            PathTraceCleanRtxdiDiTransmissionProducerDebugColor(
                pixel,
                dimensions,
                runtimeParams,
                runtimeInfo.debugMode));
    }
}

#endif
