#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_REFLECTION_SIDECAR_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_REFLECTION_SIDECAR_HLSLI

// Reflection sidecar (u90/t90) shared by:
//   - Dense hybrid mirror radiance (rgb = radiance * Fresnel)
//   - Reflection PSR lane metadata (rgb = selected lobe throughput)
//
// Alpha contract (keep ranges disjoint):
//
//   0.0   = empty
//   0.125 = reflection candidate rejected
//   0.25  = mirror trace missed
//   0.375 = valid candidate (debug intermediate)
//   0.5   = transmission lane selected (PSR)
//   0.875 = dense hybrid shaded radiance present (compose ADD)
//   1.0   = reflection lane selected + surface packed (PSR)
//
// Hybrid radiance and PSR reflection-selected use distinct alpha values because
// one is additive radiance and the other is selected-surface throughput.

static const float RT_CLEAN_RTXDI_DI_REFLECTION_PSR_EMPTY = 0.0;
static const float RT_CLEAN_RTXDI_DI_REFLECTION_PSR_REJECTED = 0.125;
static const float RT_CLEAN_RTXDI_DI_REFLECTION_PSR_MISSED = 0.25;
static const float RT_CLEAN_RTXDI_DI_REFLECTION_PSR_CANDIDATE = 0.375;
static const float RT_CLEAN_RTXDI_DI_REFLECTION_PSR_TRANSMISSION_SELECTED = 0.5;
static const float RT_CLEAN_RTXDI_DI_REFLECTION_SIDECAR_HYBRID_RADIANCE = 0.875;
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

// Dense hybrid radiance write, distinct from PSR surface ownership.
float4 PathTraceCleanRtxdiDiReflectionSidecarRadiance(float3 radiance)
{
    return float4(
        max(radiance, float3(0.0, 0.0, 0.0)),
        RT_CLEAN_RTXDI_DI_REFLECTION_SIDECAR_HYBRID_RADIANCE);
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

bool PathTraceCleanRtxdiDiReflectionSidecarIsHybridRadiance(float4 sidecar)
{
    return sidecar.a >= 0.75 && sidecar.a < 0.95;
}

bool PathTraceCleanRtxdiDiReflectionSidecarIsReflectionSelected(float4 sidecar)
{
    return sidecar.a >= 0.95;
}

// Compose gate: dense hybrid shaded radiance present.
bool PathTraceCleanRtxdiDiReflectionSidecarHasRadiance(float4 sidecar)
{
    return PathTraceCleanRtxdiDiReflectionSidecarIsHybridRadiance(sidecar);
}

float3 PathTraceCleanRtxdiDiReflectionSidecarRgb(float4 sidecar)
{
    return max(sidecar.rgb, float3(0.0, 0.0, 0.0));
}

// Lane / hybrid-radiance mask:
//   green  = PSR reflection-owned surface
//   orange = dense hybrid shaded radiance
//   blue   = transmission selected
//   cyan   = candidate only
//   magenta = rejected
//   yellow = miss
//   gray   = empty
float4 PathTraceCleanRtxdiDiReflectionPsrLaneDebugColor(float4 sidecar)
{
    if (PathTraceCleanRtxdiDiReflectionSidecarIsReflectionSelected(sidecar))
    {
        return float4(0.05, 0.9, 0.05, 1.0);
    }
    if (PathTraceCleanRtxdiDiReflectionSidecarIsHybridRadiance(sidecar))
    {
        return float4(0.95, 0.55, 0.05, 1.0);
    }
    if (PathTraceCleanRtxdiDiReflectionSidecarIsTransmissionSelected(sidecar))
    {
        return float4(0.15, 0.35, 0.95, 1.0);
    }
    if (PathTraceCleanRtxdiDiReflectionSidecarIsCandidate(sidecar))
    {
        return float4(0.05, 0.85, 0.85, 1.0);
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
