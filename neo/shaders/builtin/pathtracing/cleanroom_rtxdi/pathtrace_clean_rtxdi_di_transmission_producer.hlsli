#if defined(CLEAN_RTXDI_DI_TRANSMISSION_PRODUCER_ENTRY)

// CONTRACT (stable_reflection_hit_lighting_steps.txt):
// This producer owns clear-glass transport and the current-frame reflection-hit
// record. Stable hit lighting shades that record; compose applies R/T energy;
// RR owns guide policy. The mirror record is ephemeral feature-pass output, not
// a reflection ReSTIR history or permission to replay primary DI reservoirs.

VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> PathTraceCleanRtxdiDiTransmissionOutput : register(u87);
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> PathTraceCleanRtxdiDiReflectionSidecarOutput : register(u90);
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> PathTraceCleanRtxdiDiGlassDistortionSidecarOutput : register(u91);

static const float RT_CLEAN_RTXDI_DI_GLASS_COSMETIC_DISTORTION_MAX_PIXELS = 12.0;
static const float RT_CLEAN_RTXDI_DI_GLASS_COSMETIC_DISTORTION_MAX_BLEND = 0.85;
static const float RT_CLEAN_RTXDI_DI_GLASS_COSMETIC_DISTORTION_PRESENTATION_SCALE = 2.0;

float4 PathTraceCleanRtxdiDiGlassDistortionSidecarEmpty()
{
    return float4(0.0, 0.0, 0.0, 0.0);
}

RAB_Surface PathTraceCleanRtxdiDiGlassDistortionSurface(
    RAB_Surface glassSurface)
{
    RAB_Surface distortionSurface = glassSurface;
    if (glassSurface.materialIndex >= (uint)TextureInfo.z)
    {
        return distortionSurface;
    }

    PathTraceCleanRtxdiDiTraceHitSurface hitSurface;
    if (!PathTraceCleanRtxdiDiLoadPrimaryRecordTraceHitSurface(glassSurface, hitSurface))
    {
        return distortionSurface;
    }

    const PathTraceSmokeMaterial material =
        PathTraceCleanRoomLoadSmokeMaterial(glassSurface.materialIndex);
    float3 smoothNormal = RAB_SafeNormalize(hitSurface.shadingNormal, RAB_GetSurfaceNormal(glassSurface));
    float3 shadingNormal = PathTraceCleanRtxdiDiTraceHitDecodeNormal(
        material,
        hitSurface.normalTexCoord,
        smoothNormal,
        hitSurface.tangent,
        hitSurface.bitangent);
    const float3 viewDir = RAB_SafeNormalize(RAB_GetSurfaceViewDir(glassSurface), shadingNormal);
    if (dot(smoothNormal, viewDir) < 0.0)
    {
        smoothNormal = -smoothNormal;
    }
    if (dot(shadingNormal, viewDir) < 0.0)
    {
        shadingNormal = -shadingNormal;
    }
    shadingNormal = PathTraceCleanRtxdiDiConstrainResolvedSurfaceShadingNormal(
        shadingNormal,
        smoothNormal);

    distortionSurface.geometryNormal = smoothNormal;
    distortionSurface.shadingNormal = shadingNormal;
    distortionSurface.viewDir = viewDir;
    return distortionSurface;
}

float2 PathTraceCleanRtxdiDiGlassNormalMapTangentDistortionPixelOffset(
    RAB_Surface glassSurface,
    float pixelMagnitude,
    float grazing,
    float weight)
{
    if (glassSurface.materialIndex >= (uint)TextureInfo.z)
    {
        return float2(0.0, 0.0);
    }

    PathTraceCleanRtxdiDiTraceHitSurface hitSurface;
    if (!PathTraceCleanRtxdiDiLoadPrimaryRecordTraceHitSurface(glassSurface, hitSurface))
    {
        return float2(0.0, 0.0);
    }

    const PathTraceSmokeMaterial material =
        PathTraceCleanRoomLoadSmokeMaterial(glassSurface.materialIndex);
    if ((material.normalTextureIndex == 0xffffffffu) ||
        ((((uint)TextureInfo.w) & RT_SMOKE_TEXTURE_FLAG_USE_NORMAL_MAPS) == 0u))
    {
        return float2(0.0, 0.0);
    }

    const float4 bump = PathTraceCleanRoomSampleTexture(
        material.normalTextureIndex,
        material.normalTextureWidth,
        material.normalTextureHeight,
        hitSurface.normalTexCoord,
        float4(0.5, 0.5, 1.0, 1.0)) * 2.0 - 1.0;
    if (!all(bump == bump))
    {
        return float2(0.0, 0.0);
    }

    const float normalMapFlipGreen =
        ((((uint)TextureInfo.w) & RT_SMOKE_TEXTURE_FLAG_NORMAL_MAP_FLIP_GREEN) != 0u) ? 1.0 : 0.0;
    const float2 normalXY = SmokeMatClassNormalXY(material, bump, normalMapFlipGreen);
    const float normalMapLength = length(normalXY);
    if (normalMapLength <= RT_CLEAN_RTXDI_DI_GLASS_EPSILON)
    {
        return float2(0.0, 0.0);
    }

    const float2 tangentProjection =
        PathTraceCleanRtxdiDiGlassProjectScreenDirection(hitSurface.tangent);
    const float2 bitangentProjection =
        PathTraceCleanRtxdiDiGlassProjectScreenDirection(hitSurface.bitangent);
    const float2 offsetDirection =
        tangentProjection * normalXY.x + bitangentProjection * normalXY.y;
    if (dot(offsetDirection, offsetDirection) <= RT_CLEAN_RTXDI_DI_GLASS_EPSILON)
    {
        return float2(0.0, 0.0);
    }
    const float2 normalizedDirection =
        PathTraceCleanRtxdiDiGlassNormalizeScreenDirection(
            offsetDirection,
            PathTraceCleanRtxdiDiGlassProjectScreenDirection(RAB_GetSurfaceNormal(glassSurface)));

    const float offsetScale =
        max(pixelMagnitude, 0.0) *
        saturate(weight) *
        saturate(normalMapLength * 1.5) *
        (0.55 + 0.45 * saturate(grazing));
    return normalizedDirection * offsetScale;
}

float4 PathTraceCleanRtxdiDiGlassNormalMapDiagnosticSidecar(RAB_Surface glassSurface)
{
    if (glassSurface.materialIndex >= (uint)TextureInfo.z)
    {
        return float4(0.95, 0.05, 0.05, 1.0);
    }

    PathTraceCleanRtxdiDiTraceHitSurface hitSurface;
    if (!PathTraceCleanRtxdiDiLoadPrimaryRecordTraceHitSurface(glassSurface, hitSurface))
    {
        return float4(0.95, 0.05, 0.05, 1.0);
    }

    const PathTraceSmokeMaterial material =
        PathTraceCleanRoomLoadSmokeMaterial(glassSurface.materialIndex);
    if ((material.normalTextureIndex == 0xffffffffu) ||
        ((((uint)TextureInfo.w) & RT_SMOKE_TEXTURE_FLAG_USE_NORMAL_MAPS) == 0u))
    {
        return float4(0.95, 0.55, 0.05, 1.0);
    }

    const float4 bump = PathTraceCleanRoomSampleTexture(
        material.normalTextureIndex,
        material.normalTextureWidth,
        material.normalTextureHeight,
        hitSurface.normalTexCoord,
        float4(0.5, 0.5, 1.0, 1.0)) * 2.0 - 1.0;
    if (!all(bump == bump))
    {
        return float4(0.95, 0.05, 0.95, 1.0);
    }

    const float normalMapFlipGreen =
        ((((uint)TextureInfo.w) & RT_SMOKE_TEXTURE_FLAG_NORMAL_MAP_FLIP_GREEN) != 0u) ? 1.0 : 0.0;
    const float2 normalXY = SmokeMatClassNormalXY(material, bump, normalMapFlipGreen);
    return float4(
        0.5 + 0.5 * normalXY,
        saturate(length(normalXY) * 2.0),
        1.0);
}

float4 PathTraceCleanRtxdiDiGlassDistortionSidecarBuild(
    RAB_Surface glassSurface,
    PathTraceCleanRtxdiDiGlassMaterialParams materialParams,
    PathTraceCleanRtxdiDiGlassThinPayload glassPayload)
{
    if ((CleanRtxdiDiFlags & CLEAN_FLAG_GLASS_DISTORTION) == 0u)
    {
        return PathTraceCleanRtxdiDiGlassDistortionSidecarEmpty();
    }

    const RAB_Surface distortionSurface =
        PathTraceCleanRtxdiDiGlassDistortionSurface(glassSurface);
    const float3 normal = RAB_SafeNormalize(RAB_GetSurfaceNormal(distortionSurface), RAB_GetSurfaceGeoNormal(distortionSurface));
    const float3 viewDirection = RAB_SafeNormalize(RAB_GetSurfaceViewDir(distortionSurface), normal);
    const float grazing = 1.0 - saturate(abs(dot(normal, viewDirection)));
    const float2 refractionOffset =
        PathTraceCleanRtxdiDiGlassRefractionPixelOffset(
            distortionSurface,
            materialParams,
            glassPayload);
    const float2 bumpOffset =
        4.0 * PathTraceCleanRtxdiDiGlassNormalDistortionPixelOffset(
            distortionSurface,
            RT_CLEAN_RTXDI_DI_GLASS_COSMETIC_DISTORTION_MAX_PIXELS,
            max(grazing, 0.65),
            glassPayload.weight);
    const float2 normalMapOffset =
        PathTraceCleanRtxdiDiGlassNormalMapTangentDistortionPixelOffset(
            glassSurface,
            RT_CLEAN_RTXDI_DI_GLASS_COSMETIC_DISTORTION_MAX_PIXELS,
            max(grazing, 0.65),
            glassPayload.weight);
    const float2 offset = clamp(
        refractionOffset + bumpOffset + normalMapOffset,
        float2(
            -RT_CLEAN_RTXDI_DI_GLASS_COSMETIC_DISTORTION_MAX_PIXELS,
            -RT_CLEAN_RTXDI_DI_GLASS_COSMETIC_DISTORTION_MAX_PIXELS),
        float2(
            RT_CLEAN_RTXDI_DI_GLASS_COSMETIC_DISTORTION_MAX_PIXELS,
            RT_CLEAN_RTXDI_DI_GLASS_COSMETIC_DISTORTION_MAX_PIXELS));
    const float offsetLength = length(offset);
    if (offsetLength <= 0.05)
    {
        return PathTraceCleanRtxdiDiGlassDistortionSidecarEmpty();
    }

    const float blend = saturate(0.35 + 0.65 * offsetLength / RT_CLEAN_RTXDI_DI_GLASS_COSMETIC_DISTORTION_MAX_PIXELS) *
        RT_CLEAN_RTXDI_DI_GLASS_COSMETIC_DISTORTION_MAX_BLEND;
    return float4(offset, blend, 1.0);
}

float4 PathTraceCleanRtxdiDiGlassApplyCosmeticDistortion(
    uint2 pixel,
    uint2 dimensions,
    float4 baseColor)
{
    if ((CleanRtxdiDiFlags & CLEAN_FLAG_GLASS_DISTORTION) == 0u)
    {
        return baseColor;
    }

    const float4 distortion = PathTraceCleanRtxdiDiGlassDistortionSidecarOutput[pixel];
    if (distortion.a < 0.5 || distortion.z <= 0.0)
    {
        return baseColor;
    }

    float validWeight;
    const float presentationMaxPixels =
        RT_CLEAN_RTXDI_DI_GLASS_COSMETIC_DISTORTION_MAX_PIXELS *
        RT_CLEAN_RTXDI_DI_GLASS_COSMETIC_DISTORTION_PRESENTATION_SCALE;
    const float2 samplePixel =
        float2(pixel) + clamp(
            distortion.xy * RT_CLEAN_RTXDI_DI_GLASS_COSMETIC_DISTORTION_PRESENTATION_SCALE,
            float2(
                -presentationMaxPixels,
                -presentationMaxPixels),
            float2(
                presentationMaxPixels,
                presentationMaxPixels));
    const float4 distortedColor = PathTraceCleanRtxdiDiGlassOutputSourceColorBilinearWithValidity(
        PathTraceCleanRtxdiDiOutputColorSource,
        samplePixel,
        dimensions,
        RAB_EmptySurface(),
        false,
        baseColor,
        validWeight);
    return lerp(baseColor, distortedColor, saturate(distortion.z) * saturate(validWeight));
}

float4 PathTraceCleanRtxdiDiGlassCosmeticDistortionPreviewColor(
    uint2 pixel,
    uint2 dimensions)
{
    const float4 distortion = PathTraceCleanRtxdiDiGlassDistortionSidecarOutput[pixel];
    const float4 transmissionSidecar = PathTraceCleanRtxdiDiTransmissionOutput[pixel];
    if (distortion.a < 0.5 ||
        !PathTraceCleanRtxdiDiTransmissionSidecarHasResolvedPayload(transmissionSidecar))
    {
        return float4(0.015, 0.015, 0.015, 1.0);
    }

    const float4 baseColor = PathTraceCleanRtxdiDiGlassOutputSourceColor(
        PathTraceCleanRtxdiDiOutputColorSource,
        pixel,
        SmokeOutput[pixel]);
    return PathTraceCleanRtxdiDiGlassApplyCosmeticDistortion(pixel, dimensions, baseColor);
}

float4 PathTraceCleanRtxdiDiGlassCosmeticDistortionDifferenceColor(
    uint2 pixel,
    uint2 dimensions)
{
    const float4 transmissionSidecar = PathTraceCleanRtxdiDiTransmissionOutput[pixel];
    if (!PathTraceCleanRtxdiDiTransmissionSidecarHasResolvedPayload(transmissionSidecar))
    {
        return float4(0.015, 0.015, 0.015, 1.0);
    }

    const float4 baseColor = PathTraceCleanRtxdiDiGlassOutputSourceColor(
        PathTraceCleanRtxdiDiOutputColorSource,
        pixel,
        SmokeOutput[pixel]);
    const float4 distortedColor =
        PathTraceCleanRtxdiDiGlassApplyCosmeticDistortion(pixel, dimensions, baseColor);
    return float4(saturate(abs(distortedColor.rgb - baseColor.rgb) * 8.0), 1.0);
}

float3 PathTraceCleanRtxdiDiGlassDebugChecker(float2 samplePixel)
{
    const float2 checkerCoord = floor(samplePixel / 8.0);
    const float checker = frac((checkerCoord.x + checkerCoord.y) * 0.5) >= 0.5 ? 1.0 : 0.0;
    const float2 fineCoord = abs(frac(samplePixel / 8.0) - 0.5);
    const float gridLine = max(
        smoothstep(0.42, 0.48, fineCoord.x),
        smoothstep(0.42, 0.48, fineCoord.y));
    const float3 colorA = float3(0.05, 0.12, 0.95);
    const float3 colorB = float3(0.95, 0.85, 0.08);
    return lerp(lerp(colorA, colorB, checker), float3(1.0, 1.0, 1.0), gridLine * 0.35);
}

float4 PathTraceCleanRtxdiDiGlassCosmeticDistortionCheckerColor(uint2 pixel)
{
    const float4 distortion = PathTraceCleanRtxdiDiGlassDistortionSidecarOutput[pixel];
    const float3 baseChecker = PathTraceCleanRtxdiDiGlassDebugChecker(float2(pixel));
    if (distortion.a < 0.5 || distortion.z <= 0.0)
    {
        return float4(baseChecker * 0.45, 1.0);
    }

    const float2 samplePixel =
        float2(pixel) + clamp(
            distortion.xy,
            float2(
                -RT_CLEAN_RTXDI_DI_GLASS_COSMETIC_DISTORTION_MAX_PIXELS,
                -RT_CLEAN_RTXDI_DI_GLASS_COSMETIC_DISTORTION_MAX_PIXELS),
            float2(
                RT_CLEAN_RTXDI_DI_GLASS_COSMETIC_DISTORTION_MAX_PIXELS,
                RT_CLEAN_RTXDI_DI_GLASS_COSMETIC_DISTORTION_MAX_PIXELS));
    const float3 warpedChecker = PathTraceCleanRtxdiDiGlassDebugChecker(samplePixel);
    return float4(warpedChecker, 1.0);
}

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
    if (debugMode >= 10.5)
    {
        // Step 3 candidate classification from the reflection sidecar written
        // during the PSR phase. Do not re-load PrimarySurfaceHistoryCurrent:
        // transmission PSR has already replaced glass with the backdrop surface
        // by the time producer debug colors run (same trap as mode 3 "still glass").
        //
        //   gray  = no glass / empty
        //   green = valid reflection candidate (or later reflection-selected)
        //   cyan  = transmission only (zero reflection throughput)
        //   red   = rejected reflection (invalid mirror direction)
        //   yellow = candidate existed but mirror trace missed
        const float4 reflectionSidecar = PathTraceCleanRtxdiDiReflectionSidecarOutput[pixel];
        if (PathTraceCleanRtxdiDiReflectionSidecarIsCandidate(reflectionSidecar) ||
            PathTraceCleanRtxdiDiReflectionSidecarIsReflectionSelected(reflectionSidecar))
        {
            return float4(0.05, 0.9, 0.05, 1.0);
        }
        if (PathTraceCleanRtxdiDiReflectionSidecarIsTransmissionSelected(reflectionSidecar))
        {
            return float4(0.05, 0.55, 0.9, 1.0);
        }
        if (PathTraceCleanRtxdiDiReflectionSidecarIsRejected(reflectionSidecar))
        {
            return float4(0.9, 0.05, 0.05, 1.0);
        }
        if (PathTraceCleanRtxdiDiReflectionSidecarIsMissed(reflectionSidecar))
        {
            return float4(0.9, 0.9, 0.05, 1.0);
        }
        return float4(0.08, 0.08, 0.08, 1.0);
    }

    if (debugMode >= 9.5)
    {
        // Reflection PSR lane mask (steps 4-7b sticky/deterministic):
        //   green  = reflection owned + mirror hit (should be stable over time)
        //   blue   = transmission owned
        //   yellow = reflection chosen but mirror miss
        //   magenta = rejected candidate
        //   gray   = empty
        return PathTraceCleanRtxdiDiReflectionPsrLaneDebugColor(
            PathTraceCleanRtxdiDiReflectionSidecarOutput[pixel]);
    }

    if (debugMode >= 8.5)
    {
        return PathTraceCleanRtxdiDiGlassCosmeticDistortionCheckerColor(pixel);
    }

    if (debugMode >= 7.5)
    {
        return PathTraceCleanRtxdiDiGlassDistortionSidecarOutput[pixel];
    }

    if (debugMode >= 6.5)
    {
        return PathTraceCleanRtxdiDiGlassCosmeticDistortionDifferenceColor(pixel, dimensions);
    }

    if (debugMode >= 5.5)
    {
        return PathTraceCleanRtxdiDiGlassCosmeticDistortionPreviewColor(pixel, dimensions);
    }

    if (debugMode >= 4.5)
    {
        const float4 distortion = PathTraceCleanRtxdiDiGlassDistortionSidecarOutput[pixel];
        if (distortion.a < 0.5)
        {
            return float4(0.015, 0.015, 0.015, 1.0);
        }
        const float2 normalizedOffset = saturate(
            0.5 + 0.5 * distortion.xy / RT_CLEAN_RTXDI_DI_GLASS_COSMETIC_DISTORTION_MAX_PIXELS);
        return float4(normalizedOffset, saturate(distortion.z * 4.0), 1.0);
    }

    if (debugMode >= 3.5)
    {
        const float4 reflectionSidecar = PathTraceCleanRtxdiDiReflectionSidecarOutput[pixel];
        if (!PathTraceCleanRtxdiDiReflectionSidecarHasRadiance(reflectionSidecar))
        {
            return float4(0.015, 0.015, 0.015, 1.0);
        }
        return float4(saturate(PathTraceCleanRtxdiDiReflectionSidecarRgb(reflectionSidecar) * 4.0), 1.0);
    }

    if (debugMode >= 2.5)
    {
        // PSR diagnostic:
        //   green  = record replaced this frame (PSR fully working)
        //   yellow = PSR phase saw glass but its continuation trace missed
        //   red    = pixel still classifies as glass; PSR phase never touched it
        //   gray   = not glass
        const float4 psrPayload = PathTraceCleanRtxdiDiTransmissionOutput[pixel];
        if (PathTraceCleanRtxdiDiTransmissionSidecarHasResolvedPayload(psrPayload))
        {
            return float4(0.05, 0.9, 0.05, 1.0);
        }
        if (PathTraceCleanRtxdiDiTransmissionSidecarHasPendingPayload(psrPayload))
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

bool PathTraceCleanRtxdiDiTransmissionProducerComposeColor(
    uint2 pixel,
    uint2 dimensions,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams,
    out float4 composedColor)
{
    composedColor = float4(0.0, 0.0, 0.0, 1.0);
    const float4 transmissionSidecar = PathTraceCleanRtxdiDiTransmissionOutput[pixel];
    const float4 reflectionSidecar = PathTraceCleanRtxdiDiReflectionSidecarOutput[pixel];
    if (!PathTraceCleanRtxdiDiTransmissionSidecarHasResolvedPayload(transmissionSidecar))
    {
        // Opaque zero-roughness mirrors retain their primary DI surface and
        // simply add the current-frame secondary reflection to its color.
        if (!PathTraceCleanRtxdiDiReflectionSidecarHasRadiance(reflectionSidecar))
        {
            return false;
        }
        const float4 opaqueBaseColor = PathTraceCleanRtxdiDiGlassOutputSourceColor(
            PathTraceCleanRtxdiDiOutputColorSource,
            pixel,
            SmokeOutput[pixel]);
        composedColor = float4(
            opaqueBaseColor.rgb + PathTraceCleanRtxdiDiReflectionSidecarRgb(reflectionSidecar),
            opaqueBaseColor.a);
        return true;
    }

    const float4 baseColorSource = PathTraceCleanRtxdiDiGlassOutputSourceColor(
        PathTraceCleanRtxdiDiOutputColorSource,
        pixel,
        SmokeOutput[pixel]);
    const float4 baseColor = PathTraceCleanRtxdiDiGlassApplyCosmeticDistortion(
        pixel,
        dimensions,
        baseColorSource);
    const PathTraceCleanRtxdiDiGlassMaterialParams materialParams =
        PathTraceCleanRtxdiDiDefaultGlassMaterialParams(runtimeParams);
    // Reflection-PSR owns the primary: DI already shaded the mirrored hit.
    // Pass it through modulated by Fresnel throughput and boost.
    if (PathTraceCleanRtxdiDiReflectionSidecarIsReflectionSelected(reflectionSidecar) ||
        PathTraceCleanRtxdiDiTransmissionSidecarIsReflectionPsrOwned(transmissionSidecar))
    {
        float3 reflectionWeight = PathTraceCleanRtxdiDiReflectionSidecarIsReflectionSelected(reflectionSidecar)
            ? PathTraceCleanRtxdiDiReflectionSidecarRgb(reflectionSidecar)
            : PathTraceCleanRtxdiDiTransmissionSidecarTransmission(transmissionSidecar);
        reflectionWeight = max(reflectionWeight, float3(0.0, 0.0, 0.0));
        composedColor = float4(
            baseColor.rgb * reflectionWeight * max(materialParams.reflectionBoost, 0.0),
            baseColor.a);
        return true;
    }

    const float3 transmission = PathTraceCleanRtxdiDiGlassTransmissionWithFloor(
        PathTraceCleanRtxdiDiTransmissionSidecarTransmission(transmissionSidecar),
        materialParams);
    // Transmission-owned: attenuate behind-hit DI and add the dense hybrid
    // reflection radiance when present (alpha 0.875).
    const bool hasReflectionRadiance =
        PathTraceCleanRtxdiDiReflectionSidecarHasRadiance(reflectionSidecar);
    const float reflectionScale =
        hasReflectionRadiance ? max(materialParams.reflectionBoost, 0.0) : 0.0;
    const float3 reflectedRadiance =
        hasReflectionRadiance ? PathTraceCleanRtxdiDiReflectionSidecarRgb(reflectionSidecar) : float3(0.0, 0.0, 0.0);
    composedColor = float4(
        baseColor.rgb * transmission + reflectedRadiance * reflectionScale,
        baseColor.a);
    return true;
}

bool PathTraceCleanRtxdiDiTryOpaqueMirrorReflection(
    uint2 pixel,
    RAB_Surface surface)
{
    if ((CleanRtxdiDiFlags & CLEAN_FLAG_OPAQUE_MIRROR_REFLECTION) == 0u ||
        !PathTraceCleanRtxdiDiMaterialSupportsOpaqueDirect(surface) ||
        saturate(surface.material.roughness) > 1.0e-3)
    {
        return false;
    }

    const float3 normal =
        RAB_SafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    const float3 viewDirection = RAB_SafeNormalize(RAB_GetSurfaceViewDir(surface), normal);
    const float3 faceForwardNormal = dot(normal, viewDirection) >= 0.0 ? normal : -normal;
    const float3 fresnel = PathTraceCleanRtxdiDiMaterialFresnelSchlick(
        saturate(surface.material.specularF0),
        saturate(dot(faceForwardNormal, viewDirection)));
    if (PathTraceCleanRoomLuminance(fresnel) <= 1.0e-5)
    {
        return false;
    }

    PathTraceReflectionSecondaryHit reflectionHit;
    if (!PathTraceReflectionSecondaryTraceMirrorFromSurface(surface, reflectionHit))
    {
        return true;
    }

    RAB_Surface reflectionSurface;
    if (!PathTraceCleanRtxdiDiBuildResolvedSurfaceFromTraceHit(
        reflectionHit.payload,
        reflectionHit.hitPosition,
        reflectionHit.rayDirection,
        reflectionSurface))
    {
        return true;
    }

    RTXDI_RandomSamplerState reflectionRng =
        RTXDI_InitRandomSamplerForPass(pixel, CleanRtxdiDiFrameIndex, 0x4f4d4952u, 0u);
    PathTraceCleanRtxdiDiApplyBlueNoiseToggle(reflectionRng);
    const float3 reflectionRadiance = PathTraceReflectionSecondaryShade(
        reflectionSurface,
        PathTraceReflectionSecondaryBudgetFromConstants(),
        reflectionRng) * fresnel;
    PathTraceCleanRtxdiDiReflectionSidecarOutput[pixel] =
        PathTraceCleanRtxdiDiReflectionSidecarRadiance(reflectionRadiance);
    const uint textureFlags = (uint)max(TextureInfo.w, 0.0);
    if ((textureFlags & (1u << 12u)) != 0u &&
        (reflectionSurface.material.flags & RT_SMOKE_MATERIAL_SKY_ENVIRONMENT_RESOLVED_SURFACE) != 0u)
    {
        PathTraceRRGuidePosition[pixel] = float4(reflectionHit.rayDirection, -2.0);
    }
    if (reflectionHit.hitT > 0.0)
    {
        PathTraceRRGuideHitDistance[pixel] = reflectionHit.hitT;
    }
    return true;
}

void PathTraceCleanRtxdiDiTransmissionPsrPhase(
    uint2 pixel,
    uint2 dimensions,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams)
{
    PathTraceCleanRtxdiDiTransmissionOutput[pixel] =
        PathTraceCleanRtxdiDiTransmissionSidecarEmpty();
    PathTraceCleanRtxdiDiReflectionSidecarOutput[pixel] =
        PathTraceCleanRtxdiDiReflectionSidecarEmpty();
    PathTraceCleanRtxdiDiGlassDistortionSidecarOutput[pixel] =
        PathTraceCleanRtxdiDiGlassDistortionSidecarEmpty();

    RAB_Surface glassSurface;
    if (!PathTraceCleanRtxdiDiLoadGlassMaterialSurface(pixel, dimensions, glassSurface))
    {
        return;
    }
    if (!PathTraceCleanRtxdiDiGlassSurfaceSupported(glassSurface))
    {
        PathTraceCleanRtxdiDiTryOpaqueMirrorReflection(pixel, glassSurface);
        return;
    }

    const PathTraceCleanRtxdiDiGlassMaterialParams materialParams =
        PathTraceCleanRtxdiDiLoadGlassMaterialParams(glassSurface, runtimeParams);
    const PathTraceCleanRtxdiDiGlassThinPayload glassPayload =
        PathTraceCleanRtxdiDiBuildGlassThinPayload(glassSurface, materialParams);
    const PathTraceMaterialFeatureRuntimeInfo runtimeInfo =
        PathTraceCleanRtxdiDiLoadMaterialFeatureRuntimeInfo();
    PathTraceCleanRtxdiDiGlassDistortionSidecarOutput[pixel] =
        runtimeInfo.debugMode >= 7.5 && runtimeInfo.debugMode < 8.5
            ? PathTraceCleanRtxdiDiGlassNormalMapDiagnosticSidecar(glassSurface)
            : PathTraceCleanRtxdiDiGlassDistortionSidecarBuild(glassSurface, materialParams, glassPayload);

    const bool reflectionPsrEnabled = (CleanRtxdiDiFlags & CLEAN_FLAG_GLASS_REFLECTION_PSR) != 0u;
    // Large authored portal windows must remain reliably see-through. The
    // sticky reflection/transmission PSR owner selection can otherwise make a
    // whole panel reflection-owned at some locations and hide emissives or the
    // sky directly behind it. Keep transmission as the primary owner for this
    // material class; the dense reflection sidecar below still adds its mirror
    // contribution without replacing the behind-window surface.
    const bool forcePortalWindowTransmissionPrimary =
        (glassSurface.material.flags & RT_SMOKE_MATERIAL_PORTAL_WINDOW_FALLBACK) != 0u;
    const PathTraceCleanRtxdiDiReflectionPsrCandidate reflectionCandidate =
        PathTraceCleanRtxdiDiBuildReflectionPsrCandidate(glassSurface, glassPayload);

    // One mirror trace for RR guides + optional secondary shade. Shade is
    // deferred until after PSR lane select: when R owns, clean DI lights the
    // promoted surface (Remix 1-SPP class) — do not pay RIS shade or double-
    // count. When T owns, RIS secondary shades this hit for hybrid R add.
    float3 reflectionRadiance = float3(0.0, 0.0, 0.0);
    bool reflectionRadianceValid = false;
    RAB_Surface reflectionGuideSurface = RAB_EmptySurface();
    bool reflectionGuideSurfaceValid = false;
    float reflectionGuideHitT = 0.0;
    float3 reflectionGuideRayDirection = float3(0.0, 0.0, 0.0);
    if (reflectionPsrEnabled)
    {
        PathTraceReflectionSecondaryHit reflectionHit;
        if (PathTraceReflectionSecondaryTraceMirrorFromSurface(glassSurface, reflectionHit))
        {
            RAB_Surface reflectionSurface;
            if (PathTraceCleanRtxdiDiBuildResolvedSurfaceFromTraceHit(
                reflectionHit.payload,
                reflectionHit.hitPosition,
                reflectionHit.rayDirection,
                reflectionSurface))
            {
                reflectionGuideSurface = reflectionSurface;
                reflectionGuideSurfaceValid = true;
                reflectionGuideHitT = max(reflectionHit.hitT, 0.0);
                reflectionGuideRayDirection = reflectionHit.rayDirection;
            }
        }
    }

    // Sentinel alpha 0.25: glass detected, continuation trace pending/missed.
    // Overwritten with 1.0 below when the record replacement succeeds.
    PathTraceCleanRtxdiDiTransmissionOutput[pixel] =
        PathTraceCleanRtxdiDiTransmissionSidecarPending();

    PathTraceCleanRtxdiDiTransmissionPsrSample transmissionSample;
    // Portal windows must stay transmission-owned so their backdrop is never
    // hidden by sticky reflection ownership.  That ownership policy is
    // independent of the transmission ray direction: authored window glass
    // may still use the refracted continuation below.
    if ((CleanRtxdiDiFlags & CLEAN_FLAG_GLASS_REFRACTED_PSR) != 0u)
    {
        transmissionSample = PathTraceCleanRtxdiDiTransmissionPsrSampleRefracted(
            glassSurface,
            materialParams,
            glassPayload);
    }
    else
    {
        transmissionSample = PathTraceCleanRtxdiDiTransmissionPsrSampleThinStraight(
            glassSurface,
            glassPayload);
    }
    if (!transmissionSample.performPsr)
    {
        transmissionSample = PathTraceCleanRtxdiDiTransmissionPsrSampleThinStraight(glassSurface, glassPayload);
    }

    // Reflection PSR lane selection + pack.
    bool reflectionPrimaryPublished = false;
    bool glassPsrLaneChanged = true;
    RAB_Surface publishedReflectionSurface = RAB_EmptySurface();
    float publishedReflectionHitT = 0.0;
    if (reflectionPsrEnabled)
    {
        const bool transmissionLaneValid =
            transmissionSample.performPsr &&
            PathTraceCleanRoomLuminance(max(glassPayload.transmission, float3(0.0, 0.0, 0.0))) > 1.0e-5;
        const bool reflectionLaneValid =
            reflectionCandidate.valid && !forcePortalWindowTransmissionPrimary;

        PathTracePrimarySurfaceRecord previousRecord;
        const bool previousRecordValid =
            PathTraceCleanRoomLoadSurfaceRecordSigned(
                int2(pixel),
                dimensions,
                true,
                previousRecord);
        const bool previousReflectionLane =
            previousRecordValid &&
            (previousRecord.header.w & CLEAN_SURFACE_FLAG_REFLECTION_PSR_RESOLVED) != 0u;
        const bool previousTransmissionLane =
            previousRecordValid &&
            (previousRecord.header.w & CLEAN_SURFACE_FLAG_TRANSMISSION_PSR_RESOLVED) != 0u;
        const bool previousGlassPsrValid = previousReflectionLane || previousTransmissionLane;

        if (!reflectionLaneValid &&
            reflectionCandidate.rejectReason ==
                RT_CLEAN_RTXDI_DI_REFLECTION_PSR_REJECT_INVALID_DIRECTION)
        {
            PathTraceCleanRtxdiDiReflectionSidecarOutput[pixel] =
                PathTraceCleanRtxdiDiReflectionSidecarRejected();
        }
        else
        {
            const PathTraceCleanRtxdiDiReflectionPsrSelection laneSelection =
                PathTraceCleanRtxdiDiSelectReflectionPsrLane(
                    reflectionLaneValid,
                    transmissionLaneValid,
                    reflectionCandidate.throughput,
                    glassPayload.transmission,
                    previousReflectionLane,
                    previousTransmissionLane,
                    previousGlassPsrValid);
            glassPsrLaneChanged = laneSelection.laneChanged;

            if (laneSelection.failClosed)
            {
                PathTraceCleanRtxdiDiReflectionSidecarOutput[pixel] =
                    PathTraceCleanRtxdiDiReflectionSidecarEmpty();
            }
            else if (laneSelection.reflectionSelected)
            {
                // Reuse the one mirror trace above. A miss remains a miss; do
                // not fire a duplicate ray along the same direction.
                RAB_Surface reflectionHitSurface = reflectionGuideSurface;
                float reflectionPsrHitT = reflectionGuideHitT;
                bool haveHit = reflectionGuideSurfaceValid;

                if (haveHit)
                {
                    // DI opaque-direct path needs non-zero opacity.
                    reflectionHitSurface.material.opacity = max(
                        reflectionHitSurface.material.opacity,
                        1.0);
                    if (PathTraceCleanRtxdiDiPublishResolvedPrimarySurface(
                        pixel,
                        dimensions,
                        reflectionHitSurface,
                        CLEAN_SURFACE_FLAG_REFLECTION_PSR_RESOLVED,
                        reflectionPsrHitT,
                        glassPsrLaneChanged))
                    {
                        PathTraceCleanRtxdiDiReflectionSidecarOutput[pixel] =
                            PathTraceCleanRtxdiDiReflectionSidecarReflectionSelected(
                                laneSelection.selectedThroughput);
                        PathTraceCleanRtxdiDiTransmissionOutput[pixel] =
                            PathTraceCleanRtxdiDiTransmissionSidecarReflectionPsrOwned(
                                laneSelection.selectedThroughput);
                        publishedReflectionSurface = reflectionHitSurface;
                        publishedReflectionHitT = reflectionPsrHitT;
                        reflectionPrimaryPublished = true;
                    }
                }
                if (!reflectionPrimaryPublished)
                {
                    PathTraceCleanRtxdiDiReflectionSidecarOutput[pixel] =
                        PathTraceCleanRtxdiDiReflectionSidecarMissed();
                    glassPsrLaneChanged = true;
                }
            }
            else
            {
                PathTraceCleanRtxdiDiReflectionSidecarOutput[pixel] =
                    PathTraceCleanRtxdiDiReflectionSidecarTransmissionSelected(
                        laneSelection.selectedThroughput);
            }
        }
    }

    // Dense hybrid reflection: transmission-owned pixels keep the primary DI
    // surface and receive one current-frame mirror-hit shade in the sidecar.
    // PathTraceReflectionSecondaryShade adds exact hit emissive first, then
    // commits at most one analytic RIS light shade and one shadow ray.
    if (!reflectionPrimaryPublished &&
        reflectionGuideSurfaceValid &&
        reflectionPsrEnabled &&
        PathTraceCleanRoomLuminance(max(glassPayload.reflection, float3(0.0, 0.0, 0.0))) > 1.0e-5)
    {
        RTXDI_RandomSamplerState reflectionRng =
            RTXDI_InitRandomSamplerForPass(pixel, CleanRtxdiDiFrameIndex, 0x4752464cu, 0u);
        PathTraceCleanRtxdiDiApplyBlueNoiseToggle(reflectionRng);
        reflectionRadiance = PathTraceReflectionSecondaryShade(
            reflectionGuideSurface,
            PathTraceReflectionSecondaryBudgetFromConstants(),
            reflectionRng) * glassPayload.reflection;
        reflectionRadianceValid =
            PathTraceCleanRoomLuminance(reflectionRadiance) > 1.0e-8;
    }
    if (reflectionRadianceValid)
    {
        PathTraceCleanRtxdiDiReflectionSidecarOutput[pixel] =
            PathTraceCleanRtxdiDiReflectionSidecarRadiance(reflectionRadiance);
    }

    // Always resolve the behind-glass hit for clear-glass RR geometry when
    // possible. DI primary may be reflection-owned, but depth/normal/motion
    // ignore the pane and track the see-through surface (shipping/Remix).
    PathTraceCleanRtxdiPayload hitPayload;
    float3 hitPosition;
    float3 rayDirection;
    RAB_Surface behindGlassSurface = RAB_EmptySurface();
    bool behindGlassValid = false;
    if (PathTraceCleanRtxdiDiTraceTransmissionHit(
        glassSurface,
        transmissionSample,
        hitPayload,
        hitPosition,
        rayDirection) &&
        PathTraceCleanRtxdiDiBuildResolvedSurfaceFromTraceHit(
            hitPayload,
            hitPosition,
            rayDirection,
            behindGlassSurface))
    {
        behindGlassValid = true;
        behindGlassSurface.material.emissiveRadiance +=
            max(hitPayload.passthroughEmissiveRadiance, float3(0.0, 0.0, 0.0));
        if (transmissionSample.guidePolicy == CLEAN_RTXDI_DI_TRANSMISSION_GUIDE_POLICY_STRONG_REFRACTION)
        {
            behindGlassSurface.flags |= CLEAN_SURFACE_FLAG_TRANSMISSION_PSR_REFRACTED;
        }
    }

    if (reflectionPrimaryPublished)
    {
        // DI primary stays the mirrored hit. Rewrite RR guides to clear-glass
        // policy: behind geometry + fixed-ratio reflection material blend.
        if (behindGlassValid)
        {
            RAB_Surface reflectionMaterials = reflectionGuideSurface;
            float reflectionHitT = reflectionGuideHitT;
            if (RAB_IsSurfaceValid(publishedReflectionSurface))
            {
                reflectionMaterials = publishedReflectionSurface;
                reflectionHitT = publishedReflectionHitT;
            }
            const bool hasReflectionMaterials = RAB_IsSurfaceValid(reflectionMaterials);
            PathTraceCleanRtxdiDiWriteClearGlassRrGuides(
                pixel,
                behindGlassSurface,
                hasReflectionMaterials,
                reflectionMaterials,
                reflectionHitT,
                glassPsrLaneChanged);
        }
        return;
    }

    if (!behindGlassValid)
    {
        return;
    }

    // When reflection PSR is off, still avoid permanent RR reset: sticky
    // transmission-resolved history leaves reset clear.
    bool transmissionPublishLaneChanged = glassPsrLaneChanged;
    if (!reflectionPsrEnabled)
    {
        PathTracePrimarySurfaceRecord previousTransmissionRecord;
        const bool previousTransmissionValid =
            PathTraceCleanRoomLoadSurfaceRecordSigned(
                int2(pixel),
                dimensions,
                true,
                previousTransmissionRecord);
        transmissionPublishLaneChanged = !(
            previousTransmissionValid &&
            (previousTransmissionRecord.header.w & CLEAN_SURFACE_FLAG_TRANSMISSION_PSR_RESOLVED) != 0u);
    }

    if (!PathTraceCleanRtxdiDiPublishResolvedPrimarySurface(
        pixel,
        dimensions,
        behindGlassSurface,
        CLEAN_SURFACE_FLAG_TRANSMISSION_PSR_RESOLVED,
        0.0,
        transmissionPublishLaneChanged))
    {
        return;
    }

    // Behind-glass owns geometry guides; deterministic mirror-hit material
    // detail is blended densely into albedo/specular for RR reconstruction.
    if (reflectionPsrEnabled && reflectionGuideSurfaceValid)
    {
        PathTraceCleanRtxdiDiBlendReflectionMaterialRrGuides(
            pixel,
            reflectionGuideSurface,
            reflectionGuideHitT);
    }

    const float overlayStrength = PathTraceCleanRtxdiDiGlassOverlayStrength(glassPayload);
    PathTraceCleanRtxdiDiTransmissionOutput[pixel] =
        PathTraceCleanRtxdiDiTransmissionSidecarResolved(transmissionSample.attenuation, overlayStrength);
    const uint textureFlags = (uint)max(TextureInfo.w, 0.0);
    if (reflectionRadianceValid &&
        (textureFlags & (1u << 12u)) != 0u &&
        (reflectionGuideSurface.material.flags & RT_SMOKE_MATERIAL_SKY_ENVIRONMENT_RESOLVED_SURFACE) != 0u)
    {
        PathTraceRRGuidePosition[pixel] = float4(reflectionGuideRayDirection, -2.0);
    }
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

    // Post-DI phase either visualizes producer diagnostics or composes the
    // producer-owned transmission/reflection sidecars without requiring a
    // separate glass-pass t90 SRV.
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
        return;
    }

    if (runtimeInfo.writesOutputColor)
    {
        float4 composedColor;
        if (PathTraceCleanRtxdiDiTransmissionProducerComposeColor(
            pixel,
            dimensions,
            runtimeParams,
            composedColor))
        {
            PathTraceCleanRtxdiDiStoreGlassComposedColor(pixel, composedColor);
        }
    }
}

#endif
