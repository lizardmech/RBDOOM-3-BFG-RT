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

    float3 lightDir;
    float lightDistance;
    RAB_GetLightDirDistance(surface, lightSample, lightDir, lightDistance);
    const float3 brdf = EvaluateOpaqueDirectBrdf(surface, lightDir, RAB_GetSurfaceViewDir(surface));
    const float ndotl = saturate(dot(RAB_GetSurfaceNormal(surface), lightDir));
    const float3 reflected = brdf * lightSample.radiance * ndotl;
    return RAB_Luminance(reflected) / max(lightSample.solidAnglePdf, 1.0e-6);
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
