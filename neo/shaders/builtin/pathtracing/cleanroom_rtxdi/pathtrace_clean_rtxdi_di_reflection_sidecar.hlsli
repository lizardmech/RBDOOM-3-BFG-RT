#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_REFLECTION_SIDECAR_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_REFLECTION_SIDECAR_HLSLI

// Reflection sidecar (u90/t90) shared by:
//   - Option B shaded mirror radiance (rgb = radiance * Fresnel, a = valid)
//   - Reflection PSR lane metadata (rgb = throughput or fallback radiance, a = lane state)
//
// Alpha contract for reflection PSR (v1). Option B radiance-valid uses the same
// top state value so existing compose (a > 0.5) keeps working.
//
//   0.0  = empty / no reflection PSR activity
//   0.25 = reflection PSR candidate but mirror trace missed
//   0.5  = transmission selected (reflection not selected for this pixel)
//   1.0  = reflection selected (PSR) or Option B radiance present
//
// Do not silently retune these values without updating producer, compose, and
// debug views together.

static const float RT_CLEAN_RTXDI_DI_REFLECTION_PSR_EMPTY = 0.0;
static const float RT_CLEAN_RTXDI_DI_REFLECTION_PSR_MISSED = 0.25;
static const float RT_CLEAN_RTXDI_DI_REFLECTION_PSR_TRANSMISSION_SELECTED = 0.5;
static const float RT_CLEAN_RTXDI_DI_REFLECTION_PSR_REFLECTION_SELECTED = 1.0;

float4 PathTraceCleanRtxdiDiReflectionSidecarEmpty()
{
    return float4(0.0, 0.0, 0.0, RT_CLEAN_RTXDI_DI_REFLECTION_PSR_EMPTY);
}

float4 PathTraceCleanRtxdiDiReflectionSidecarMissed()
{
    return float4(0.0, 0.0, 0.0, RT_CLEAN_RTXDI_DI_REFLECTION_PSR_MISSED);
}

float4 PathTraceCleanRtxdiDiReflectionSidecarTransmissionSelected(float3 throughput)
{
    return float4(
        max(throughput, float3(0.0, 0.0, 0.0)),
        RT_CLEAN_RTXDI_DI_REFLECTION_PSR_TRANSMISSION_SELECTED);
}

float4 PathTraceCleanRtxdiDiReflectionSidecarReflectionSelected(float3 throughputOrRadiance)
{
    return float4(
        max(throughputOrRadiance, float3(0.0, 0.0, 0.0)),
        RT_CLEAN_RTXDI_DI_REFLECTION_PSR_REFLECTION_SELECTED);
}

// Option B radiance write: keeps the historical a=1.0 valid marker.
float4 PathTraceCleanRtxdiDiReflectionSidecarRadiance(float3 radiance)
{
    return PathTraceCleanRtxdiDiReflectionSidecarReflectionSelected(radiance);
}

bool PathTraceCleanRtxdiDiReflectionSidecarIsEmpty(float4 sidecar)
{
    return sidecar.a < 0.125;
}

bool PathTraceCleanRtxdiDiReflectionSidecarIsMissed(float4 sidecar)
{
    return sidecar.a >= 0.125 && sidecar.a < 0.375;
}

bool PathTraceCleanRtxdiDiReflectionSidecarIsTransmissionSelected(float4 sidecar)
{
    return sidecar.a >= 0.375 && sidecar.a < 0.75;
}

bool PathTraceCleanRtxdiDiReflectionSidecarIsReflectionSelected(float4 sidecar)
{
    return sidecar.a >= 0.75;
}

// Option B compose gate (historical): treat reflection-selected / radiance as present.
bool PathTraceCleanRtxdiDiReflectionSidecarHasRadiance(float4 sidecar)
{
    return sidecar.a > 0.5;
}

float3 PathTraceCleanRtxdiDiReflectionSidecarRgb(float4 sidecar)
{
    return max(sidecar.rgb, float3(0.0, 0.0, 0.0));
}

// Lane-status debug colors (beauty-neutral; debug views only):
//   green  = reflection selected / radiance present
//   red    = transmission selected
//   yellow = candidate miss
//   gray   = empty
float4 PathTraceCleanRtxdiDiReflectionPsrLaneDebugColor(float4 sidecar)
{
    if (PathTraceCleanRtxdiDiReflectionSidecarIsReflectionSelected(sidecar))
    {
        return float4(0.05, 0.9, 0.05, 1.0);
    }
    if (PathTraceCleanRtxdiDiReflectionSidecarIsTransmissionSelected(sidecar))
    {
        return float4(0.9, 0.05, 0.05, 1.0);
    }
    if (PathTraceCleanRtxdiDiReflectionSidecarIsMissed(sidecar))
    {
        return float4(0.9, 0.9, 0.05, 1.0);
    }
    return float4(0.08, 0.08, 0.08, 1.0);
}

#endif
