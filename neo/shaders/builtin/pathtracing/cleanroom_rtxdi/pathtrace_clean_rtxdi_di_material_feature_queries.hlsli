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

bool PathTraceCleanRtxdiDiMaterialSupportsTransmission(RAB_Surface surface)
{
    return MaterialSupportsTransmission(surface);
}

bool PathTraceCleanRtxdiDiMaterialSupportsOpaqueDirect(RAB_Surface surface)
{
    return RAB_SurfaceSupportsOpaqueDiffuseBrdf(surface);
}

float3 PathTraceCleanRtxdiDiMaterialEvaluateOpaqueDirectBrdf(RAB_Surface surface, float3 wi, float3 wo)
{
    return RAB_EvaluateSurfaceBrdf(surface, wi, wo);
}

float PathTraceCleanRtxdiDiMaterialEvaluateLightSampleTargetPdf(RAB_LightSample lightSample, RAB_Surface surface)
{
    return RAB_GetLightSampleTargetPdfForSurface(lightSample, surface);
}

float3 PathTraceCleanRtxdiDiMaterialEvaluateReflectedRadiance(float3 incomingRadianceLocation, float3 incomingRadiance, RAB_Surface surface)
{
    return RAB_GetReflectedBsdfRadianceForSurface(incomingRadianceLocation, incomingRadiance, surface);
}

#endif
