#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_GLASS_FEATURE_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_GLASS_FEATURE_HLSLI

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

bool PathTraceCleanRtxdiDiGlassSurfaceSupported(RAB_Surface surface)
{
    return PathTraceCleanRtxdiDiGlassFeatureSupported(
        PathTraceCleanRtxdiDiGlassFeatureForSurface(surface));
}

#endif
