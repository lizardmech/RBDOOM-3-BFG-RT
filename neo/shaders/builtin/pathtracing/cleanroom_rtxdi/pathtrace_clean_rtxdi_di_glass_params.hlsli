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

static const uint RT_PATH_TRACE_OBJECT_GLASS_PARAM0_TRANSMITTANCE_R = 0u;
static const uint RT_PATH_TRACE_OBJECT_GLASS_PARAM0_TRANSMITTANCE_G = 1u;
static const uint RT_PATH_TRACE_OBJECT_GLASS_PARAM0_TRANSMITTANCE_B = 2u;
static const uint RT_PATH_TRACE_OBJECT_GLASS_PARAM0_THICKNESS = 3u;

static const uint RT_PATH_TRACE_OBJECT_GLASS_PARAM1_IOR = 0u;
static const uint RT_PATH_TRACE_OBJECT_GLASS_PARAM1_STRENGTH = 1u;
static const uint RT_PATH_TRACE_OBJECT_GLASS_PARAM1_REFLECTION_BOOST = 2u;
static const uint RT_PATH_TRACE_OBJECT_GLASS_PARAM1_TRANSMISSION_FLOOR = 3u;

PathTraceCleanRtxdiDiGlassMaterialParams PathTraceCleanRtxdiDiDefaultGlassMaterialParams(
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams)
{
    PathTraceCleanRtxdiDiGlassMaterialParams params;
    params.transmittanceColor = saturate(float3(
        runtimeParams.params0[RT_PATH_TRACE_OBJECT_GLASS_PARAM0_TRANSMITTANCE_R],
        runtimeParams.params0[RT_PATH_TRACE_OBJECT_GLASS_PARAM0_TRANSMITTANCE_G],
        runtimeParams.params0[RT_PATH_TRACE_OBJECT_GLASS_PARAM0_TRANSMITTANCE_B]));
    params.thickness = max(runtimeParams.params0[RT_PATH_TRACE_OBJECT_GLASS_PARAM0_THICKNESS], 0.0);
    params.ior = max(runtimeParams.params1[RT_PATH_TRACE_OBJECT_GLASS_PARAM1_IOR], 1.0001);
    params.strength = saturate(runtimeParams.params1[RT_PATH_TRACE_OBJECT_GLASS_PARAM1_STRENGTH]);
    params.reflectionBoost = max(runtimeParams.params1[RT_PATH_TRACE_OBJECT_GLASS_PARAM1_REFLECTION_BOOST], 0.0);
    params.transmissionFloor = max(runtimeParams.params1[RT_PATH_TRACE_OBJECT_GLASS_PARAM1_TRANSMISSION_FLOOR], 0.0);
    return params;
}

void PathTraceCleanRtxdiDiApplyGlassParameterRecord(
    inout PathTraceCleanRtxdiDiGlassMaterialParams params,
    PathTraceMaterialFeatureParameterRecord record)
{
    params.transmittanceColor = saturate(float3(
        record.params0[RT_PATH_TRACE_OBJECT_GLASS_PARAM0_TRANSMITTANCE_R],
        record.params0[RT_PATH_TRACE_OBJECT_GLASS_PARAM0_TRANSMITTANCE_G],
        record.params0[RT_PATH_TRACE_OBJECT_GLASS_PARAM0_TRANSMITTANCE_B]));
    params.thickness = max(record.params0[RT_PATH_TRACE_OBJECT_GLASS_PARAM0_THICKNESS], 0.0);
    params.ior = max(record.params1[RT_PATH_TRACE_OBJECT_GLASS_PARAM1_IOR], 1.0001);
    params.strength = saturate(record.params1[RT_PATH_TRACE_OBJECT_GLASS_PARAM1_STRENGTH]);
    params.reflectionBoost = max(record.params1[RT_PATH_TRACE_OBJECT_GLASS_PARAM1_REFLECTION_BOOST], 0.0);
    params.transmissionFloor = max(record.params1[RT_PATH_TRACE_OBJECT_GLASS_PARAM1_TRANSMISSION_FLOOR], 0.0);
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
    }

    return params;
}

#endif
