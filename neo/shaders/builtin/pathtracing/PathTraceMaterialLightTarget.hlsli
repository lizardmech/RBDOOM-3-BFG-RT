#ifndef RB_PATH_TRACE_MATERIAL_LIGHT_TARGET_HLSLI
#define RB_PATH_TRACE_MATERIAL_LIGHT_TARGET_HLSLI

// Material-owned direct-light target helpers. RAB entry points wrap these so
// ReSTIR consumers do not embed material-specific lighting behavior.

float MaterialEvaluateOpaqueDirectLightSampleTargetPdf(RAB_LightSample lightSample, RAB_Surface surface)
{
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

    RAB_Surface targetSurface = surface;
#ifdef RB_RAB_CLEAN_RTXDI_DI_SENTINEL
    targetSurface.material.diffuseAlbedo = max(targetSurface.material.diffuseAlbedo, float3(0.2, 0.2, 0.2));
    targetSurface.material.roughness = max(targetSurface.material.roughness, 0.0009);
#endif

    float3 lightDir;
    float lightDistance;
    RAB_GetLightDirDistance(targetSurface, lightSample, lightDir, lightDistance);
    const float3 brdf = EvaluateOpaqueDirectBrdf(targetSurface, lightDir, RAB_GetSurfaceViewDir(targetSurface));
    const float ndotl = saturate(dot(RAB_GetSurfaceNormal(targetSurface), lightDir));
    const float3 reflected = brdf * lightSample.radiance * ndotl;
    const float targetPdf = RAB_Luminance(reflected) / max(lightSample.solidAnglePdf, 1.0e-6);
#ifdef RB_RAB_CLEAN_DIAGNOSTIC_RELAX_BRDF_GATES
    if (lightSample.lightType == 1u &&
        (CleanRtxdiDiFlags & CLEAN_RAB_DIAGNOSTIC_DOOM_TARGET_FLOOR) != 0u)
    {
        return max(targetPdf, max(RAB_Luminance(lightSample.radiance), 1.0e-4));
    }
#endif
    return targetPdf;
}

float3 MaterialEvaluateOpaqueDirectReflectedRadiance(float3 incomingRadianceLocation, float3 incomingRadiance, RAB_Surface surface)
{
    if (!RAB_IsSurfaceValid(surface))
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float3 toLight = incomingRadianceLocation - RAB_GetSurfaceWorldPos(surface);
    const float distanceSquared = max(dot(toLight, toLight), 1.0e-6);
    const float3 lightDir = toLight * rsqrt(distanceSquared);
    const float ndotl = saturate(dot(RAB_GetSurfaceNormal(surface), lightDir));
    return EvaluateOpaqueDirectBrdf(surface, lightDir, RAB_GetSurfaceViewDir(surface)) * max(incomingRadiance, float3(0.0, 0.0, 0.0)) * ndotl;
}

float MaterialEvaluateOpaqueDirectSampleTargetPdf(float3 samplePosition, float3 radiance, RAB_Surface surface)
{
    if (!RAB_IsSurfaceValid(surface))
    {
        return 0.0;
    }

    const float3 toSample = samplePosition - RAB_GetSurfaceWorldPos(surface);
    const float distanceSquared = max(dot(toSample, toSample), 1.0e-6);
    const float3 sampleDir = toSample * rsqrt(distanceSquared);
    const float3 brdf = EvaluateOpaqueDirectBrdf(surface, sampleDir, RAB_GetSurfaceViewDir(surface));
    const float ndotl = saturate(dot(RAB_GetSurfaceNormal(surface), sampleDir));
    return RAB_Luminance(brdf * max(radiance, float3(0.0, 0.0, 0.0)) * ndotl);
}

#endif
