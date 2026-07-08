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
//   0.125 = reflection candidate rejected (bad direction / no offset)
//   0.25 = reflection PSR candidate but mirror trace missed
//   0.375 = valid reflection candidate (step 3; not yet lane-selected)
//   0.5  = transmission selected / transmission-only (reflection not selected)
//   1.0  = reflection selected (PSR) or Option B radiance present
//
// States with a <= 0.5 never feed Option B radiance compose (gate is a > 0.5).
// Do not silently retune these values without updating producer, compose, and
// debug views together.

static const float RT_CLEAN_RTXDI_DI_REFLECTION_PSR_EMPTY = 0.0;
static const float RT_CLEAN_RTXDI_DI_REFLECTION_PSR_REJECTED = 0.125;
static const float RT_CLEAN_RTXDI_DI_REFLECTION_PSR_MISSED = 0.25;
static const float RT_CLEAN_RTXDI_DI_REFLECTION_PSR_CANDIDATE = 0.375;
static const float RT_CLEAN_RTXDI_DI_REFLECTION_PSR_TRANSMISSION_SELECTED = 0.5;
static const float RT_CLEAN_RTXDI_DI_REFLECTION_PSR_REFLECTION_SELECTED = 1.0;

// Candidate reject reasons (stored only in producer logic / live debug, not alpha).
static const uint RT_CLEAN_RTXDI_DI_REFLECTION_PSR_REJECT_NONE = 0u;
static const uint RT_CLEAN_RTXDI_DI_REFLECTION_PSR_REJECT_ZERO_THROUGHPUT = 1u;
static const uint RT_CLEAN_RTXDI_DI_REFLECTION_PSR_REJECT_INVALID_DIRECTION = 2u;

float4 PathTraceCleanRtxdiDiReflectionSidecarEmpty()
{
    return float4(0.0, 0.0, 0.0, RT_CLEAN_RTXDI_DI_REFLECTION_PSR_EMPTY);
}

float4 PathTraceCleanRtxdiDiReflectionSidecarRejected()
{
    return float4(0.0, 0.0, 0.0, RT_CLEAN_RTXDI_DI_REFLECTION_PSR_REJECTED);
}

float4 PathTraceCleanRtxdiDiReflectionSidecarMissed()
{
    return float4(0.0, 0.0, 0.0, RT_CLEAN_RTXDI_DI_REFLECTION_PSR_MISSED);
}

float4 PathTraceCleanRtxdiDiReflectionSidecarCandidate(float3 reflectionThroughput)
{
    return float4(
        max(reflectionThroughput, float3(0.0, 0.0, 0.0)),
        RT_CLEAN_RTXDI_DI_REFLECTION_PSR_CANDIDATE);
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
    return sidecar.a < 0.0625;
}

bool PathTraceCleanRtxdiDiReflectionSidecarIsRejected(float4 sidecar)
{
    return sidecar.a >= 0.0625 && sidecar.a < 0.1875;
}

bool PathTraceCleanRtxdiDiReflectionSidecarIsMissed(float4 sidecar)
{
    return sidecar.a >= 0.1875 && sidecar.a < 0.3125;
}

bool PathTraceCleanRtxdiDiReflectionSidecarIsCandidate(float4 sidecar)
{
    return sidecar.a >= 0.3125 && sidecar.a < 0.4375;
}

bool PathTraceCleanRtxdiDiReflectionSidecarIsTransmissionSelected(float4 sidecar)
{
    return sidecar.a >= 0.4375 && sidecar.a < 0.75;
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
//   cyan   = valid reflection candidate (not yet selected)
//   red    = transmission selected
//   magenta = rejected candidate
//   yellow = trace miss
//   gray   = empty
float4 PathTraceCleanRtxdiDiReflectionPsrLaneDebugColor(float4 sidecar)
{
    if (PathTraceCleanRtxdiDiReflectionSidecarIsReflectionSelected(sidecar))
    {
        return float4(0.05, 0.9, 0.05, 1.0);
    }
    if (PathTraceCleanRtxdiDiReflectionSidecarIsCandidate(sidecar))
    {
        return float4(0.05, 0.85, 0.85, 1.0);
    }
    if (PathTraceCleanRtxdiDiReflectionSidecarIsTransmissionSelected(sidecar))
    {
        return float4(0.9, 0.05, 0.05, 1.0);
    }
    if (PathTraceCleanRtxdiDiReflectionSidecarIsRejected(sidecar))
    {
        return float4(0.9, 0.05, 0.9, 1.0);
    }
    if (PathTraceCleanRtxdiDiReflectionSidecarIsMissed(sidecar))
    {
        return float4(0.9, 0.9, 0.05, 1.0);
    }
    return float4(0.08, 0.08, 0.08, 1.0);
}

// Step 3 candidate classification colors (from live candidate, not only alpha):
//   gray  = no glass
//   cyan  = transmission only (zero reflection throughput)
//   red   = rejected reflection (invalid direction)
//   green = reflection candidate
float4 PathTraceCleanRtxdiDiReflectionPsrCandidateDebugColor(
    bool isGlass,
    bool candidateValid,
    uint rejectReason)
{
    if (!isGlass)
    {
        return float4(0.08, 0.08, 0.08, 1.0);
    }
    if (candidateValid)
    {
        return float4(0.05, 0.9, 0.05, 1.0);
    }
    if (rejectReason == RT_CLEAN_RTXDI_DI_REFLECTION_PSR_REJECT_ZERO_THROUGHPUT)
    {
        return float4(0.05, 0.55, 0.9, 1.0);
    }
    return float4(0.9, 0.05, 0.05, 1.0);
}

#endif
