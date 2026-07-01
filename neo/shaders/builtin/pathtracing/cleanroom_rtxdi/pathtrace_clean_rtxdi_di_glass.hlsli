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
        const float viewFacing = saturate(abs(dot(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceViewDir(surface))));
        const float3 baseColor = runtimeParams.params0.xyz;
        const float viewFacingScale = runtimeParams.params0.w;
        return float4(baseColor.x, baseColor.y + viewFacingScale * viewFacing, baseColor.z, 1.0);
    }

    if (feature.materialKind == RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_GLASS)
    {
        return float4(runtimeParams.params1.xyz, 1.0);
    }

    const float unsupportedOpaque = runtimeParams.params1.w;
    return float4(unsupportedOpaque, unsupportedOpaque, unsupportedOpaque + 0.01, 1.0);
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
