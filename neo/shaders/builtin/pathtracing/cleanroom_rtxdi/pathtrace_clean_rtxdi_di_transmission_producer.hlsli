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

bool PathTraceCleanRtxdiDiTransmissionProjectWorldPixel(
    float3 worldPosition,
    uint2 dimensions,
    out uint2 projectedPixel)
{
    projectedPixel = uint2(0u, 0u);
    const float3 delta = worldPosition - CleanRtxdiDiCameraOriginAndValid.xyz;
    const float forwardDistance = dot(delta, CleanRtxdiDiCameraForwardAndTanX.xyz);
    if (forwardDistance <= 1.0e-4)
    {
        return false;
    }

    const float ndcX = -dot(delta, CleanRtxdiDiCameraLeftAndTanY.xyz) /
        max(forwardDistance * CleanRtxdiDiCameraForwardAndTanX.w, 1.0e-5);
    const float ndcY = -dot(delta, CleanRtxdiDiCameraUpAndTanY.xyz) /
        max(forwardDistance * CleanRtxdiDiCameraLeftAndTanY.w, 1.0e-5);
    if (abs(ndcX) > 1.0 || abs(ndcY) > 1.0)
    {
        return false;
    }

    const float2 projectedPixelFloat = (float2(ndcX, ndcY) * 0.5 + 0.5) *
        float2(max(dimensions, uint2(1u, 1u)));
    projectedPixel = min(
        uint2(max(projectedPixelFloat, float2(0.0, 0.0))),
        max(dimensions, uint2(1u, 1u)) - 1u);
    return true;
}

RAB_LightInfo PathTraceCleanRtxdiDiTransmissionLoadRluLightInfo(uint lightIndex)
{
    RAB_LightInfo lightInfo = RAB_EmptyLightInfo();
    if (lightIndex >= CleanRtxdiDiRluCurrentLightCount)
    {
        return lightInfo;
    }

    const PathTraceUnifiedLightRecord light = CleanRtxdiDiRluCurrentLights[lightIndex];
    if (light.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
    {
        const float3 radiance = max(light.radianceAndLuminance.rgb, float3(0.0, 0.0, 0.0)) *
            max(CleanRtxdiDiDoomAnalyticLightInfo.z, 0.0);
        const float luminance = RAB_Luminance(radiance);
        if (light.sourceWeight <= 0.0 ||
            luminance <= 0.0 ||
            light.positionAndRadius.w <= 0.0 ||
            light.uvOrDoomParams.x <= 0.0)
        {
            return lightInfo;
        }

        lightInfo.lightType = RAB_LIGHT_TYPE_DOOM_ANALYTIC_SPHERE;
        lightInfo.lightIndex = lightIndex;
        lightInfo.unifiedLightType = PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC;
        lightInfo.materialIndex = RAB_INVALID_LIGHT_INDEX;
        lightInfo.flags = light.flags;
        lightInfo.position = light.positionAndRadius.xyz;
        lightInfo.radius = max(light.positionAndRadius.w, 0.01);
        lightInfo.influenceRadius = max(light.uvOrDoomParams.x, lightInfo.radius);
        lightInfo.normal = float3(0.0, 0.0, 1.0);
        lightInfo.area = max(light.uvOrDoomParams.y, 1.0e-4);
        lightInfo.radiance = radiance;
        lightInfo.weight = luminance * lightInfo.area * lightInfo.influenceRadius;
        return lightInfo;
    }

    if (light.type != PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE ||
        light.sourceIndex == PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX ||
        light.sourceIndex >= CleanRtxdiDiCurrentEmissiveTriangleCount ||
        light.sourceWeight <= 0.0)
    {
        return lightInfo;
    }

    const PathTraceSmokeEmissiveTriangle tri = SmokeEmissiveTriangles[light.sourceIndex];
    if (tri.materialIndex >= (uint)CleanRtxdiDiTextureInfo.z ||
        (tri.padding0 & RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF) != 0u)
    {
        return lightInfo;
    }

    const PathTraceSmokeMaterial material = PathTraceCleanRoomLoadSmokeMaterial(tri.materialIndex);
    const float3 radiance = PathTraceCleanRoomTexturedEmissiveRadiance(light, false);
    if ((material.flags & RT_SMOKE_MATERIAL_EMISSIVE) == 0u ||
        RAB_Luminance(radiance) <= 0.0)
    {
        return lightInfo;
    }

    lightInfo.lightType = RAB_LIGHT_TYPE_EMISSIVE_TRIANGLE;
    lightInfo.lightIndex = lightIndex;
    lightInfo.unifiedLightType = PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE;
    lightInfo.materialIndex = light.materialOrLightId;
    lightInfo.flags = light.flags;
    lightInfo.position = light.positionAndRadius.xyz;
    lightInfo.radius = 0.0;
    lightInfo.influenceRadius = 0.0;
    lightInfo.normal = RAB_SafeNormalize(tri.normalAndLuminance.xyz, float3(0.0, 0.0, 1.0));
    lightInfo.area = max(tri.centerAndArea.w, 1.0e-4);
    lightInfo.radiance = radiance;
    lightInfo.weight = light.sourceWeight;
    lightInfo.sourceIndex = light.sourceIndex;
    lightInfo.hasTriangleGeometry = 0u;
    lightInfo.emissiveTextureIndex = material.emissiveTextureIndex;
    lightInfo.emissiveTextureWidth = material.emissiveTextureWidth;
    lightInfo.emissiveTextureHeight = material.emissiveTextureHeight;
    lightInfo.emissiveActiveStage = 1u;
    lightInfo.emissiveColor = max(material.emissiveColor.rgb, float3(0.0, 0.0, 0.0));
    lightInfo.trianglePosition0 = lightInfo.position;
    lightInfo.trianglePosition1 = lightInfo.position;
    lightInfo.trianglePosition2 = lightInfo.position;
    lightInfo.triangleUv0 = tri.centroidUvAndWeight.xy;
    lightInfo.triangleUv1 = tri.centroidUvAndWeight.xy;
    lightInfo.triangleUv2 = tri.centroidUvAndWeight.xy;
    return lightInfo;
}

PathTraceCleanRtxdiPayload PathTraceCleanRtxdiDiEmptyTransmissionTracePayload(RAB_Surface surface)
{
    PathTraceCleanRtxdiPayload payload;
    payload.value = 0u;
    payload.rayMode = 3u;
    payload.ignoreInstanceId = surface.instanceId;
    payload.ignorePrimitiveIndex = surface.primitiveIndex;
    payload.ignoreMaterialIndex = surface.materialIndex;
    payload.hitInstanceId = 0xffffffffu;
    payload.hitPrimitiveIndex = 0xffffffffu;
    payload.hitMaterialId = 0xffffffffu;
    payload.hitMaterialIndex = 0xffffffffu;
    payload.hitTriangleClassAndFlags = 0u;
    payload.hitT = 0.0;
    payload.hitBarycentrics = float2(0.0, 0.0);
    return payload;
}

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

// Primary surface replacement (PSR): trace the transmitted ray through thin glass
// before DI runs, and overwrite the glass pixel's primary-surface record with the
// hit so the entire pipeline (RTXDI, GI, guides, denoiser) shades it natively.
// The glass throughput is stashed in the transmission output for the late compose.
bool PathTraceCleanRtxdiDiTraceTransmissionHit(
    RAB_Surface surface,
    out PathTraceCleanRtxdiPayload hitPayload,
    out float3 hitPosition,
    out float3 rayDirection)
{
    const float3 normal = RAB_SafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    const float3 viewDirection = RAB_SafeNormalize(RAB_GetSurfaceViewDir(surface), normal);
    // Thin-walled glass: entry and exit refractions cancel, so the transmitted
    // ray keeps the incident direction.
    rayDirection = -viewDirection;
    hitPayload = PathTraceCleanRtxdiDiEmptyTransmissionTracePayload(surface);

    RayDesc ray;
    ray.Origin = RAB_GetSurfaceWorldPos(surface) + rayDirection * 0.05;
    ray.Direction = rayDirection;
    ray.TMin = 0.01;
    ray.TMax = 100000.0;
    // Force non-opaque so the rayMode 3 anyhit filter always runs; it skips the
    // source pane and transparent-carded glass so the ray reaches the backdrop.
    TraceRay(SmokeScene, RAY_FLAG_FORCE_NON_OPAQUE, 0xff, 0, 1, 0, ray, hitPayload);
    hitPosition = ray.Origin + rayDirection * hitPayload.hitT;
    return hitPayload.value != 0u && hitPayload.hitMaterialIndex < (uint)TextureInfo.z;
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

    const uint width = CleanRtxdiDiWidth != 0u ? CleanRtxdiDiWidth : dimensions.x;
    const uint height = CleanRtxdiDiHeight != 0u ? CleanRtxdiDiHeight : dimensions.y;
    if (width == 0u || height == 0u || pixel.x >= width || pixel.y >= height)
    {
        return;
    }
    const uint recordIndex = pixel.y * width + pixel.x;

    const float viewDepth = dot(
        hitPosition - CleanRtxdiDiCameraOriginAndValid.xyz,
        CleanRtxdiDiCameraForwardAndTanX.xyz);
    hitSurface.linearDepth = viewDepth;
    PrimarySurfaceHistoryCurrent[recordIndex] =
        PathTraceCleanRtxdiDiPackResolvedPrimarySurfaceRecord(hitSurface);
    PathTraceCleanRtxdiDiWriteResolvedSurfaceRrGuides(pixel, hitSurface);

    PathTraceCleanRtxdiDiTransmissionOutput[pixel] = float4(saturate(glassPayload.transmission), 1.0);
}

float4 PathTraceCleanRtxdiDiTransmissionProducerComposeColor(
    uint2 pixel,
    uint2 dimensions,
    float4 currentColor,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams)
{
    // PSR pixels: the current color already holds the behind-glass surface shaded
    // by the full pipeline; apply the stored thin-glass throughput.
    const float4 psrPayload = PathTraceCleanRtxdiDiTransmissionOutput[pixel];
    if (psrPayload.a > 0.5)
    {
        return currentColor;
    }

    // No PSR happened (trace miss or unsupported). Do not fall back to the
    // older screen-space glass composite; leave the DI result unchanged.
    return currentColor;
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

    // Late compose phase: u87 holds the PSR throughput written before DI ran;
    // do not overwrite it here.
    const PathTraceMaterialFeatureRuntimeInfo runtimeInfo =
        PathTraceCleanRtxdiDiLoadMaterialFeatureRuntimeInfo();
    if (runtimeInfo.writesOutputColor)
    {
        if (runtimeInfo.debugMode >= 0.5)
        {
            // Route debug output through the composed-color store so it also
            // lands in the DLSS-RR input; presentation may not read SmokeOutput.
            PathTraceCleanRtxdiDiStoreGlassComposedColor(
                pixel,
                PathTraceCleanRtxdiDiTransmissionProducerDebugColor(
                    pixel,
                    dimensions,
                    runtimeParams,
                    runtimeInfo.debugMode));
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
