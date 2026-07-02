#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_GLASS_PARAMS_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_GLASS_PARAMS_HLSLI

struct PathTraceCleanRtxdiDiGlassMaterialParams
{
    float3 transmittanceColor;
    float thickness;
    float ior;
    float strength;
    float reflectionBoost;
    float transmissionFloor;
};

float3 PathTraceCleanRtxdiDiGlassMaterialTintFromSurface(RAB_Surface surface, float3 fallbackTint)
{
    const float3 surfaceTint = saturate(surface.material.diffuseAlbedo);
    const float tintMax = max(surfaceTint.x, max(surfaceTint.y, surfaceTint.z));
    if (tintMax <= 0.05)
    {
        return fallbackTint;
    }
    return max(surfaceTint, float3(0.05, 0.05, 0.05));
}

PathTraceCleanRtxdiDiGlassMaterialParams PathTraceCleanRtxdiDiDefaultGlassMaterialParams(
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams)
{
    PathTraceCleanRtxdiDiGlassMaterialParams params;
    params.transmittanceColor = saturate(runtimeParams.params0.xyz);
    params.thickness = max(runtimeParams.params0.w, 0.0);
    params.ior = max(runtimeParams.params1.x, 1.0001);
    params.strength = saturate(runtimeParams.params1.y);
    params.reflectionBoost = max(runtimeParams.params1.z, 0.0);
    params.transmissionFloor = max(runtimeParams.params1.w, 0.0);
    return params;
}

void PathTraceCleanRtxdiDiApplyGlassParameterRecord(
    inout PathTraceCleanRtxdiDiGlassMaterialParams params,
    PathTraceMaterialFeatureParameterRecord record)
{
    params.transmittanceColor = saturate(record.params0.xyz);
    params.thickness = max(record.params0.w, 0.0);
    params.ior = max(record.params1.x, 1.0001);
    params.strength = saturate(record.params1.y);
    params.reflectionBoost = max(record.params1.z, 0.0);
    params.transmissionFloor = max(record.params1.w, 0.0);
}

PathTraceCleanRtxdiDiGlassMaterialParams PathTraceCleanRtxdiDiLoadGlassMaterialParams(
    RAB_Surface surface,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams)
{
    PathTraceCleanRtxdiDiGlassMaterialParams params =
        PathTraceCleanRtxdiDiDefaultGlassMaterialParams(runtimeParams);

    PathTraceMaterialFeature feature;
    if (PathTraceCleanRtxdiDiLoadMaterialFeature(surface.materialIndex, feature) &&
        feature.materialKind == RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_GLASS &&
        feature.parameterRecordIndex == surface.materialIndex)
    {
        PathTraceCleanRtxdiDiApplyGlassParameterRecord(
            params,
            PathTraceMaterialFeatureParameters[feature.parameterRecordIndex]);
        params.transmittanceColor =
            PathTraceCleanRtxdiDiGlassMaterialTintFromSurface(surface, params.transmittanceColor);
    }

    return params;
}

#endif
