#if defined(CLEAN_RTXDI_DI_GLASS_ENTRY)

Texture2D<float4> PathTraceCleanRtxdiDiTransmissionSidecar : register(t87);

float4 PathTraceCleanRtxdiDiGlassDebugColor(
    uint2 pixel,
    uint2 dimensions,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams)
{
    const float4 sidecar = PathTraceCleanRtxdiDiTransmissionSidecar.Load(int3(pixel, 0));
    if (PathTraceCleanRtxdiDiTransmissionSidecarHasResolvedPayload(sidecar))
    {
        return float4(0.05 + 0.5 * saturate(sidecar.r), 0.9, 0.05 + 0.25 * saturate(sidecar.b), 1.0);
    }
    if (PathTraceCleanRtxdiDiTransmissionSidecarHasPendingPayload(sidecar))
    {
        return float4(0.9, 0.9, 0.05, 1.0);
    }

    return PathTraceCleanRtxdiDiGlassDebugColorForPixel(
        pixel,
        dimensions,
        runtimeParams,
        float4(0.0, 0.0, 0.0, 1.0),
        float4(0.015, 0.015, 0.025, 1.0),
        float4(0.65, 0.05, 0.85, 1.0));
}

float4 PathTraceCleanRtxdiDiGlassSidecarComposeColor(
    uint2 pixel,
    uint2 dimensions,
    float4 fallbackColor,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams)
{
    const float4 baseColor = PathTraceCleanRtxdiDiGlassOutputSourceColor(
        PathTraceCleanRtxdiDiOutputColorSource,
        pixel,
        fallbackColor);
    const float4 sidecar = PathTraceCleanRtxdiDiTransmissionSidecar.Load(int3(pixel, 0));
    if (!PathTraceCleanRtxdiDiTransmissionSidecarHasResolvedPayload(sidecar))
    {
        return fallbackColor;
    }

    const PathTraceCleanRtxdiDiGlassMaterialParams materialParams =
        PathTraceCleanRtxdiDiDefaultGlassMaterialParams(runtimeParams);
    const float3 transmission = max(
        PathTraceCleanRtxdiDiTransmissionSidecarTransmission(sidecar),
        float3(
            materialParams.transmissionFloor,
            materialParams.transmissionFloor,
            materialParams.transmissionFloor));
    const float reflectionEnergy =
        PathTraceCleanRtxdiDiTransmissionSidecarReflectionEnergy(sidecar) *
        materialParams.reflectionBoost;
    float3 reflectionColor = float3(0.08, 0.085, 0.09);
    RAB_Surface surface;
    if (PathTraceCleanRtxdiDiLoadGlassMaterialSurface(pixel, dimensions, surface))
    {
        const PathTraceCleanRtxdiDiGlassThinPayload payload =
            PathTraceCleanRtxdiDiBuildGlassThinPayload(surface, materialParams);
        const float2 reflectionSamplePixel = PathTraceCleanRtxdiDiGlassReflectionSamplePosition(
            pixel,
            dimensions,
            surface,
            materialParams,
            payload);
        const float4 reflectedSource = PathTraceCleanRtxdiDiGlassOutputSourceColorBilinear(
            PathTraceCleanRtxdiDiOutputColorSource,
            reflectionSamplePixel,
            dimensions,
            surface,
            false,
            float4(reflectionColor, 1.0));
        reflectionColor = max(reflectedSource.rgb, reflectionColor);
    }

    return float4(baseColor.rgb * transmission + reflectionColor * reflectionEnergy, baseColor.a);
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

    const PathTraceMaterialFeatureRuntimeInfo runtimeInfo =
        PathTraceCleanRtxdiDiLoadMaterialFeatureRuntimeInfo();
    if (runtimeInfo.ready < 0.5 || runtimeInfo.writesOutputColor < 0.5)
    {
        return;
    }

    const PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams =
        PathTraceCleanRtxdiDiLoadMaterialFeatureRuntimeParams();
    if (runtimeInfo.debugMode >= 0.5)
    {
        PathTraceCleanRtxdiDiStoreGlassComposedColor(
            pixel,
            PathTraceCleanRtxdiDiGlassDebugColor(pixel, dimensions, runtimeParams));
    }
    else
    {
        PathTraceCleanRtxdiDiStoreGlassComposedColor(
            pixel,
            PathTraceCleanRtxdiDiGlassSidecarComposeColor(pixel, dimensions, SmokeOutput[pixel], runtimeParams));
    }
}

#endif
