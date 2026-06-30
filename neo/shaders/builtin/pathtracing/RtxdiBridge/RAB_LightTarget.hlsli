#ifndef RB_PATH_TRACING_RAB_LIGHT_TARGET_HLSLI
#define RB_PATH_TRACING_RAB_LIGHT_TARGET_HLSLI

#include "RAB_Brdf.hlsli"
#include "RAB_LightSamplingCore.hlsli"
#include "../PathTraceMaterialLightTarget.hlsli"

float RAB_GetLightSampleTargetPdfForSurface(RAB_LightSample lightSample, RAB_Surface surface)
{
#ifdef RB_RAB_CLEAN_REFERENCE_DOOM_ANALYTIC
    if (PathTraceCleanReferenceRabEnabled() && lightSample.lightType == RAB_LIGHT_TYPE_DOOM_ANALYTIC_SPHERE)
    {
        return PathTraceCleanReferenceRabTargetPdf(lightSample, surface);
    }
#endif
    return MaterialEvaluateOpaqueDirectLightSampleTargetPdf(lightSample, surface);
}

float3 RAB_GetReflectedBsdfRadianceForSurface(float3 incomingRadianceLocation, float3 incomingRadiance, RAB_Surface surface)
{
    return MaterialEvaluateOpaqueDirectReflectedRadiance(incomingRadianceLocation, incomingRadiance, surface);
}

float RAB_GetLightTargetPdfForVolume(RAB_LightInfo lightInfo, float3 volumeCenter, float volumeRadius)
{
    if (!RAB_IsLightInfoValid(lightInfo))
    {
        return 0.0;
    }

    return lightInfo.weight;
}

float RAB_GetPTSampleTargetPdfForSurface(float3 samplePosition, float3 radiance, RAB_Surface surface)
{
    return MaterialEvaluateOpaqueDirectSampleTargetPdf(samplePosition, radiance, surface);
}

#endif
