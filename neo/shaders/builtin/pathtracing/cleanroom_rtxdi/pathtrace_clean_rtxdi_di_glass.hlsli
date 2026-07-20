#if defined(CLEAN_RTXDI_DI_GLASS_ENTRY)

Texture2D<float4> PathTraceCleanRtxdiDiTransmissionSidecar : register(t87);
Texture2D<float4> PathTraceCleanRtxdiDiReflectionSidecar : register(t90);

bool PathTraceCleanRtxdiDiGlassSidecarComposeEnabled(PathTraceMaterialFeatureRuntimeInfo runtimeInfo)
{
    return runtimeInfo.frameIndex >= 1.5;
}

bool PathTraceCleanRtxdiDiGlassSidecarAvailable(PathTraceMaterialFeatureRuntimeInfo runtimeInfo)
{
    return PathTraceCleanRtxdiDiGlassSidecarComposeEnabled(runtimeInfo);
}

bool PathTraceCleanRtxdiDiGlassProducerOnly(PathTraceMaterialFeatureRuntimeInfo runtimeInfo)
{
    return runtimeInfo.frameIndex >= 0.5 && runtimeInfo.frameIndex < 1.5;
}

bool PathTraceCleanRtxdiDiGlassCurrentPixelSupported(uint2 pixel, uint2 dimensions)
{
    RAB_Surface surface;
    return PathTraceCleanRtxdiDiLoadGlassMaterialSurface(pixel, dimensions, surface) &&
        PathTraceCleanRtxdiDiGlassSurfaceSupported(surface);
}

float4 PathTraceCleanRtxdiDiGlassDebugColor(
    uint2 pixel,
    uint2 dimensions,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams)
{
    const PathTraceMaterialFeatureRuntimeInfo runtimeInfo =
        PathTraceCleanRtxdiDiLoadMaterialFeatureRuntimeInfo();
    const bool producerOnly = PathTraceCleanRtxdiDiGlassProducerOnly(runtimeInfo);
    if (producerOnly)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    const bool sidecarAvailable = PathTraceCleanRtxdiDiGlassSidecarAvailable(runtimeInfo);
    const bool sidecarComposeEnabled = PathTraceCleanRtxdiDiGlassSidecarComposeEnabled(runtimeInfo);
    const float4 sidecar = sidecarAvailable
        ? PathTraceCleanRtxdiDiTransmissionSidecar.Load(int3(pixel, 0))
        : PathTraceCleanRtxdiDiTransmissionSidecarEmpty();
    const bool sidecarResolved =
        sidecarAvailable &&
        PathTraceCleanRtxdiDiTransmissionSidecarHasResolvedPayload(sidecar);
    const bool sidecarPending =
        sidecarAvailable &&
        !sidecarResolved &&
        PathTraceCleanRtxdiDiTransmissionSidecarHasPendingPayload(sidecar);
    const bool glassPixel =
        sidecarResolved ||
        sidecarPending ||
        (!sidecarAvailable && PathTraceCleanRtxdiDiGlassCurrentPixelSupported(pixel, dimensions));
    if (runtimeInfo.debugMode >= 2.5)
    {
        if (!glassPixel)
        {
            return float4(0.0, 0.0, 0.0, 1.0);
        }
        if (!sidecarAvailable)
        {
            return float4(0.95, 0.05, 0.05, 1.0);
        }
        if (sidecarResolved)
        {
            const float reflectionEnergy =
                PathTraceCleanRtxdiDiTransmissionSidecarReflectionEnergy(sidecar);
            const float debugEnergy = saturate(reflectionEnergy * 4.0);
            return float4(
                0.05 + 0.95 * debugEnergy,
                0.15 + 0.85 * debugEnergy,
                0.95 - 0.75 * debugEnergy,
                1.0);
        }
        if (sidecarPending)
        {
            return float4(0.9, 0.9, 0.05, 1.0);
        }
        return sidecarComposeEnabled
            ? float4(0.05, 0.15, 0.95, 1.0)
            : float4(0.55, 0.05, 0.95, 1.0);
    }
    if (runtimeInfo.debugMode >= 1.5)
    {
        if (!glassPixel)
        {
            return float4(0.0, 0.0, 0.0, 1.0);
        }
        if (!sidecarAvailable)
        {
            return float4(0.95, 0.05, 0.05, 1.0);
        }
        if (sidecarResolved)
        {
            return float4(PathTraceCleanRtxdiDiTransmissionSidecarTransmission(sidecar), 1.0);
        }
        if (sidecarPending)
        {
            return float4(0.9, 0.9, 0.05, 1.0);
        }
        return sidecarComposeEnabled
            ? float4(0.05, 0.15, 0.95, 1.0)
            : float4(0.55, 0.05, 0.95, 1.0);
    }
    if (sidecarResolved)
    {
        return float4(0.05 + 0.5 * saturate(sidecar.r), 0.9, 0.05 + 0.25 * saturate(sidecar.b), 1.0);
    }
    if (sidecarPending)
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

// CONTRACT (stable_reflection_hit_lighting_steps.txt):
// Compose applies transmission/reflection energy to already-lit signals. It
// does not trace, select lights, or shade a reflection hit. Reflection sidecar
// radiance is unowned by compose and already contains its Fresnel weight.
float4 PathTraceCleanRtxdiDiComposeThinGlassSidecarColor(
    float4 baseColor,
    float4 transmissionSidecar,
    float4 reflectionSidecar,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams)
{
    const PathTraceCleanRtxdiDiGlassMaterialParams materialParams =
        PathTraceCleanRtxdiDiDefaultGlassMaterialParams(runtimeParams);
    const float weight = PathTraceCleanRtxdiDiTransmissionSidecarWeight(transmissionSidecar);
    const float3 sidecarRgb =
        PathTraceCleanRtxdiDiTransmissionSidecarTransmission(transmissionSidecar);

    // PSR reflection-owned: DI already shaded the mirrored primary. Modulate by
    // Fresnel lobe weight * boost (single-surface energy). Detect via t90 or
    // the transmission-sidecar ownership marker (0.75).
    if (PathTraceCleanRtxdiDiReflectionSidecarIsReflectionSelected(reflectionSidecar) ||
        PathTraceCleanRtxdiDiTransmissionSidecarIsReflectionPsrOwned(transmissionSidecar))
    {
        float3 reflectionWeight = PathTraceCleanRtxdiDiReflectionSidecarIsReflectionSelected(reflectionSidecar)
            ? PathTraceCleanRtxdiDiReflectionSidecarRgb(reflectionSidecar)
            : PathTraceCleanRtxdiDiTransmissionSidecarTransmission(transmissionSidecar);
        reflectionWeight = max(reflectionWeight, float3(0.0, 0.0, 0.0));
        return float4(
            baseColor.rgb * reflectionWeight * max(materialParams.reflectionBoost, 0.0),
            baseColor.a);
    }

    const float3 throughput =
        PathTraceCleanRtxdiDiGlassTransmissionWithFloor(sidecarRgb, materialParams);

    // Transmission-owned: attenuate behind-glass DI and ADD shaded mirror
    // radiance from the one dense hybrid reflection shade (alpha 0.875).
    float3 composed = baseColor.rgb * throughput;

    if (PathTraceCleanRtxdiDiReflectionSidecarHasRadiance(reflectionSidecar))
    {
        composed += PathTraceCleanRtxdiDiReflectionSidecarRgb(reflectionSidecar) *
            max(materialParams.reflectionBoost, 0.0);
    }
    return float4(lerp(baseColor.rgb, composed, weight), baseColor.a);
}

float4 PathTraceCleanRtxdiDiGlassOpaqueOverlayComposeColor(
    uint2 pixel,
    uint2 dimensions,
    float4 baseColor,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams,
    out bool composed)
{
    composed = false;
    RAB_Surface surface;
    if (!PathTraceCleanRtxdiDiLoadGlassMaterialSurface(pixel, dimensions, surface) ||
        !PathTraceCleanRtxdiDiGlassSurfaceSupported(surface))
    {
        return baseColor;
    }

    const PathTraceCleanRtxdiDiGlassMaterialParams materialParams =
        PathTraceCleanRtxdiDiLoadGlassMaterialParams(surface, runtimeParams);
    const PathTraceCleanRtxdiDiGlassThinPayload payload =
        PathTraceCleanRtxdiDiBuildGlassThinPayload(surface, materialParams);
    const float3 surfaceTerm =
        saturate(payload.reflection) * max(materialParams.reflectionBoost, 0.0) +
        PathTraceCleanRtxdiDiGlassTransmissionWithFloor(payload.transmission, materialParams) *
            max(materialParams.transmissionFloor, 0.0);
    composed = true;
    return float4(baseColor.rgb + surfaceTerm, baseColor.a);
}

bool PathTraceCleanRtxdiDiTryGlassSidecarComposeColor(
    uint2 pixel,
    uint2 dimensions,
    float4 fallbackColor,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams,
    bool sidecarComposeEnabled,
    out float4 composedColor)
{
    const float4 baseColor = PathTraceCleanRtxdiDiGlassOutputSourceColor(
        PathTraceCleanRtxdiDiOutputColorSource,
        pixel,
        fallbackColor);
    const float4 transmissionSidecar = sidecarComposeEnabled
        ? PathTraceCleanRtxdiDiTransmissionSidecar.Load(int3(pixel, 0))
        : PathTraceCleanRtxdiDiTransmissionSidecarEmpty();
    const float4 reflectionSidecar = sidecarComposeEnabled
        ? PathTraceCleanRtxdiDiReflectionSidecar.Load(int3(pixel, 0))
        : PathTraceCleanRtxdiDiReflectionSidecarEmpty();
    if (!sidecarComposeEnabled ||
        !PathTraceCleanRtxdiDiTransmissionSidecarHasResolvedPayload(transmissionSidecar))
    {
        bool composed;
        composedColor = PathTraceCleanRtxdiDiGlassOpaqueOverlayComposeColor(
            pixel,
            dimensions,
            baseColor,
            runtimeParams,
            composed);
        return composed;
    }

    composedColor = PathTraceCleanRtxdiDiComposeThinGlassSidecarColor(
        baseColor,
        transmissionSidecar,
        reflectionSidecar,
        runtimeParams);
    return true;
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

    // The pre-DI PSR producer owns liquid debug output. Composing glass after
    // clean DI would otherwise replace its authoritative secondary-hit tuple.
    if (PathTraceCleanRtxdiDiLiquidPoolDebug() != 0u)
    {
        return;
    }

    const PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams =
        PathTraceCleanRtxdiDiLoadMaterialFeatureRuntimeParams();
    const bool sidecarComposeEnabled = PathTraceCleanRtxdiDiGlassSidecarComposeEnabled(runtimeInfo);
    if (runtimeInfo.debugMode >= 0.5)
    {
        PathTraceCleanRtxdiDiStoreGlassComposedColor(
            pixel,
            PathTraceCleanRtxdiDiGlassDebugColor(pixel, dimensions, runtimeParams));
    }
    else
    {
        float4 composedColor;
        if (PathTraceCleanRtxdiDiTryGlassSidecarComposeColor(
            pixel,
            dimensions,
            SmokeOutput[pixel],
            runtimeParams,
            sidecarComposeEnabled,
            composedColor))
        {
            PathTraceCleanRtxdiDiStoreGlassComposedColor(
                pixel,
                composedColor);
        }
    }
}

#endif
