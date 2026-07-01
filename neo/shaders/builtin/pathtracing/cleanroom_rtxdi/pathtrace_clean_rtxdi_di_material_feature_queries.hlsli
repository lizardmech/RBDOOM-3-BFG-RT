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

#endif
