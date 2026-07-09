#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_TRANSMISSION_SIDECAR_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_TRANSMISSION_SIDECAR_HLSLI

static const float RT_CLEAN_RTXDI_DI_TRANSMISSION_SIDECAR_EMPTY = 0.0;
static const float RT_CLEAN_RTXDI_DI_TRANSMISSION_SIDECAR_PENDING = 0.25;
// Reflection-PSR owns the primary surface for this pixel. Distinct from RESOLVED
// so glass beauty compose can detect ownership even if t90 is unbound.
static const float RT_CLEAN_RTXDI_DI_TRANSMISSION_SIDECAR_REFLECTION_PSR_OWNED = 0.75;
static const float RT_CLEAN_RTXDI_DI_TRANSMISSION_SIDECAR_RESOLVED = 1.0;

float4 PathTraceCleanRtxdiDiTransmissionSidecarEmpty()
{
    return float4(1.0, 1.0, 1.0, RT_CLEAN_RTXDI_DI_TRANSMISSION_SIDECAR_EMPTY);
}

float4 PathTraceCleanRtxdiDiTransmissionSidecarPending()
{
    return float4(1.0, 1.0, 1.0, RT_CLEAN_RTXDI_DI_TRANSMISSION_SIDECAR_PENDING);
}

float4 PathTraceCleanRtxdiDiTransmissionSidecarReflectionPsrOwned(float3 reflectionThroughput)
{
    return float4(
        saturate(reflectionThroughput),
        RT_CLEAN_RTXDI_DI_TRANSMISSION_SIDECAR_REFLECTION_PSR_OWNED);
}

float4 PathTraceCleanRtxdiDiTransmissionSidecarResolved(float3 transmission, float reflectionEnergy)
{
    return float4(
        saturate(transmission),
        RT_CLEAN_RTXDI_DI_TRANSMISSION_SIDECAR_RESOLVED + saturate(reflectionEnergy));
}

bool PathTraceCleanRtxdiDiTransmissionSidecarIsReflectionPsrOwned(float4 sidecar)
{
    return sidecar.a > 0.65 && sidecar.a < 0.85;
}

bool PathTraceCleanRtxdiDiTransmissionSidecarHasResolvedPayload(float4 sidecar)
{
    // Includes reflection-PSR-owned (0.75) and classic resolved (>= 1.0).
    return sidecar.a > 0.5;
}

bool PathTraceCleanRtxdiDiTransmissionSidecarHasPendingPayload(float4 sidecar)
{
    return sidecar.a > 0.1;
}

float3 PathTraceCleanRtxdiDiTransmissionSidecarTransmission(float4 sidecar)
{
    return max(sidecar.rgb, float3(0.0, 0.0, 0.0));
}

float PathTraceCleanRtxdiDiTransmissionSidecarReflectionEnergy(float4 sidecar)
{
    return saturate(sidecar.a - RT_CLEAN_RTXDI_DI_TRANSMISSION_SIDECAR_RESOLVED);
}

float PathTraceCleanRtxdiDiTransmissionSidecarWeight(float4 sidecar)
{
    return 1.0;
}

float3 PathTraceCleanRtxdiDiTransmissionSidecarOverlayColor(float4 sidecar)
{
    return PathTraceCleanRtxdiDiTransmissionSidecarTransmission(sidecar);
}

#endif
