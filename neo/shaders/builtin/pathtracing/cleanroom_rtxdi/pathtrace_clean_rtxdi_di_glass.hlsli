#if defined(CLEAN_RTXDI_DI_GLASS_ENTRY)

PathTraceMaterialFeature PathTraceCleanRtxdiDiGlassFeatureForSurface(RAB_Surface surface)
{
    PathTraceMaterialFeature feature;
    if (PathTraceCleanRtxdiDiLoadMaterialFeature(surface.materialIndex, feature))
    {
        return feature;
    }
    return BuildMaterialFeatureFromPrimarySurface(surface);
}

bool PathTraceCleanRtxdiDiGlassFeatureSupported(PathTraceMaterialFeature feature)
{
    return feature.materialKind == RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_GLASS &&
        (feature.materialCaps & RT_PATH_TRACE_MATERIAL_CAP_PATH_TRANSMISSION) != 0u &&
        (feature.materialCaps & RT_PATH_TRACE_MATERIAL_CAP_DEBUG_FAIL_CLOSED) == 0u &&
        (feature.lobeCaps & RT_PATH_TRACE_MATERIAL_LOBE_SPECULAR_TRANSMISSION) != 0u &&
        (feature.passSupport & RT_PATH_TRACE_MATERIAL_PASS_TRANSMISSION_PRODUCER) != 0u;
}

float4 PathTraceCleanRtxdiDiGlassDebugColor(
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
    const PathTraceMaterialFeature feature = PathTraceCleanRtxdiDiGlassFeatureForSurface(surface);
    if (PathTraceCleanRtxdiDiGlassFeatureSupported(feature))
    {
        const PathTraceCleanRtxdiDiGlassMaterialParams materialParams =
            PathTraceCleanRtxdiDiLoadGlassMaterialParams(surface, runtimeParams);
        const PathTraceCleanRtxdiDiGlassThinPayload payload =
            PathTraceCleanRtxdiDiBuildGlassThinPayload(surface, materialParams);
        return float4(saturate(payload.transmission + payload.reflection * 0.25), 1.0);
    }

    if (feature.materialKind == RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_GLASS)
    {
        return float4(0.65, 0.05, 0.85, 1.0);
    }

    return float4(0.015, 0.015, 0.025, 1.0);
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
        LoadPathTraceMaterialFeatureRuntimeInfo(PathTraceMaterialFeatureRuntimeInfoPacked);
    if (!runtimeInfo.ready || runtimeInfo.debugMode < 0.5)
    {
        return;
    }

    const PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams =
        PathTraceCleanRtxdiDiLoadMaterialFeatureRuntimeParams();
    SmokeOutput[pixel] = PathTraceCleanRtxdiDiGlassDebugColor(pixel, dimensions, runtimeParams);
}

#endif
