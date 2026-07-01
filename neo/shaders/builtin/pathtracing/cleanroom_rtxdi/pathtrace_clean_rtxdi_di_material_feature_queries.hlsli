#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_QUERIES_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_QUERIES_HLSLI

bool PathTraceCleanRtxdiDiMaterialSupportedByPass(RAB_Surface surface, uint passKind)
{
    return MaterialSupportedByPass(surface, passKind);
}

float4 PathTraceCleanRtxdiDiMaterialFailClosedDebugColor(RAB_Surface surface, uint passKind)
{
    return MaterialFailClosedDebugColor(surface, passKind);
}

bool PathTraceCleanRtxdiDiLoadMaterialFeature(uint materialIndex, out PathTraceMaterialFeature feature)
{
    feature = (PathTraceMaterialFeature)0;
#if defined(RB_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_SIDECAR)
    const uint materialCount = (uint)TextureInfo.z;
    if (materialIndex >= materialCount)
    {
        return false;
    }

    feature = PathTraceMaterialFeatureFromRecord(PathTraceMaterialFeatures[materialIndex]);
    return feature.parameterRecordIndex == materialIndex ||
        feature.parameterRecordIndex == 0xffffffffu;
#else
    return false;
#endif
}

bool PathTraceMaterialFeatureSupportsTransmission(PathTraceMaterialFeature feature)
{
    return (feature.materialCaps & RT_PATH_TRACE_MATERIAL_CAP_PATH_TRANSMISSION) != 0u &&
        (feature.materialCaps & RT_PATH_TRACE_MATERIAL_CAP_DEBUG_FAIL_CLOSED) == 0u &&
        (feature.passSupport & RT_PATH_TRACE_MATERIAL_PASS_PATH_INTEGRATOR) != 0u &&
        (feature.passSupport & RT_PATH_TRACE_MATERIAL_PASS_TRANSMISSION_PRODUCER) != 0u;
}

bool PathTraceCleanRtxdiDiMaterialSupportsTransmission(RAB_Surface surface)
{
    PathTraceMaterialFeature feature;
    if (PathTraceCleanRtxdiDiLoadMaterialFeature(surface.materialIndex, feature))
    {
        return PathTraceMaterialFeatureSupportsTransmission(feature);
    }
    return MaterialSupportsTransmission(surface);
}

bool PathTraceCleanRtxdiDiMaterialRelaxOpaqueDirectGates()
{
    return (CleanRtxdiDiFlags & CLEAN_RAB_DIAGNOSTIC_RELAX_BRDF_GATES) != 0u;
}

bool PathTraceCleanRtxdiDiMaterialSupportsOpaqueDirect(RAB_Surface surface)
{
    if (PathTraceCleanRtxdiDiMaterialRelaxOpaqueDirectGates())
    {
        return RAB_IsSurfaceValid(surface) && surface.material.opacity > 0.0;
    }
    return MaterialSupportsOpaqueDirect(surface);
}

RAB_Surface PathTraceCleanRtxdiDiMaterialTargetSurface(RAB_Surface surface)
{
    RAB_Surface targetSurface = surface;
    targetSurface.material.diffuseAlbedo = max(targetSurface.material.diffuseAlbedo, float3(0.2, 0.2, 0.2));
    targetSurface.material.roughness = max(targetSurface.material.roughness, 0.0009);
    return targetSurface;
}

float3 PathTraceCleanRtxdiDiMaterialEvaluateOpaqueDirectBrdf(RAB_Surface surface, float3 wi, float3 wo)
{
    if (!PathTraceCleanRtxdiDiMaterialSupportsOpaqueDirect(surface))
    {
        return float3(0.0, 0.0, 0.0);
    }

    if (PathTraceCleanRtxdiDiMaterialRelaxOpaqueDirectGates())
    {
        const float3 normal = RAB_SafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
        if (dot(normal, wi) <= 0.0)
        {
            return float3(0.0, 0.0, 0.0);
        }
        return GetDiffuseAlbedo(surface.material) * (1.0 / RT_PATH_TRACE_OPAQUE_DIRECT_PI);
    }

    return EvaluateOpaqueDirectBrdf(surface, wi, wo);
}

float PathTraceCleanRtxdiDiMaterialEvaluateLightSampleTargetPdf(RAB_LightSample lightSample, RAB_Surface surface)
{
#ifdef RB_RAB_CLEAN_REFERENCE_DOOM_ANALYTIC
    if (PathTraceCleanReferenceRabEnabled() && lightSample.lightType == RAB_LIGHT_TYPE_DOOM_ANALYTIC_SPHERE)
    {
        return PathTraceCleanReferenceRabTargetPdf(lightSample, surface);
    }
#endif
    if (!RAB_IsReplayableLightSample(lightSample) || !RAB_IsSurfaceValid(surface))
    {
        return 0.0;
    }
    if (!all(lightSample.radiance == lightSample.radiance) ||
        !all(abs(lightSample.radiance) < float3(3.402823e+38, 3.402823e+38, 3.402823e+38)) ||
        RAB_Luminance(lightSample.radiance) <= 0.0)
    {
        return 0.0;
    }

    RAB_Surface targetSurface = PathTraceCleanRtxdiDiMaterialTargetSurface(surface);
    float3 lightDir;
    float lightDistance;
    RAB_GetLightDirDistance(targetSurface, lightSample, lightDir, lightDistance);
    const float3 brdf = PathTraceCleanRtxdiDiMaterialEvaluateOpaqueDirectBrdf(
        targetSurface,
        lightDir,
        RAB_GetSurfaceViewDir(targetSurface));
    const float ndotl = saturate(dot(RAB_GetSurfaceNormal(targetSurface), lightDir));
    const float targetPdf = RAB_Luminance(brdf * lightSample.radiance * ndotl) / max(lightSample.solidAnglePdf, 1.0e-6);
    if (lightSample.lightType == 1u &&
        (CleanRtxdiDiFlags & CLEAN_RAB_DIAGNOSTIC_DOOM_TARGET_FLOOR) != 0u)
    {
        return max(targetPdf, max(RAB_Luminance(lightSample.radiance), 1.0e-4));
    }
    return targetPdf;
}

float3 PathTraceCleanRtxdiDiMaterialEvaluateReflectedRadiance(float3 incomingRadianceLocation, float3 incomingRadiance, RAB_Surface surface)
{
    if (!RAB_IsSurfaceValid(surface))
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float3 toLight = incomingRadianceLocation - RAB_GetSurfaceWorldPos(surface);
    const float distanceSquared = max(dot(toLight, toLight), 1.0e-6);
    const float3 lightDir = toLight * rsqrt(distanceSquared);
    const float ndotl = saturate(dot(RAB_GetSurfaceNormal(surface), lightDir));
    return PathTraceCleanRtxdiDiMaterialEvaluateOpaqueDirectBrdf(surface, lightDir, RAB_GetSurfaceViewDir(surface)) *
        max(incomingRadiance, float3(0.0, 0.0, 0.0)) *
        ndotl;
}

#endif
