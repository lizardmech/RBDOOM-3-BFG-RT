#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_REFLECTION_SIDECAR_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_REFLECTION_SIDECAR_HLSLI

// Reflection sidecar (u90/t90) shared by:
//   - Option B shaded mirror radiance (rgb = radiance * Fresnel)
//   - Reflection PSR lane metadata (rgb = selected lobe throughput)
//
// Alpha contract (keep ranges disjoint):
//
//   0.0   = empty
//   0.125 = reflection candidate rejected
//   0.25  = mirror trace missed
//   0.375 = valid candidate (debug intermediate)
//   0.5   = transmission lane selected (PSR)
//   0.875 = Option B shaded radiance present (compose ADD)
//   1.0   = reflection lane selected + surface packed (PSR)
//
// Option B and PSR reflection-selected MUST NOT share the same alpha: a shared
// a=1 value made PSR skip when Option B ran, and compose then suppressed Option B
// whenever PSR was enabled — killing all glass reflections.

static const float RT_CLEAN_RTXDI_DI_REFLECTION_PSR_EMPTY = 0.0;
static const float RT_CLEAN_RTXDI_DI_REFLECTION_PSR_REJECTED = 0.125;
static const float RT_CLEAN_RTXDI_DI_REFLECTION_PSR_MISSED = 0.25;
static const float RT_CLEAN_RTXDI_DI_REFLECTION_PSR_CANDIDATE = 0.375;
static const float RT_CLEAN_RTXDI_DI_REFLECTION_PSR_TRANSMISSION_SELECTED = 0.5;
static const float RT_CLEAN_RTXDI_DI_REFLECTION_SIDECAR_OPTION_B_RADIANCE = 0.875;
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

// Option B radiance write: distinct alpha so PSR can still run and compose
// can tell radiance-add from PSR surface ownership.
float4 PathTraceCleanRtxdiDiReflectionSidecarRadiance(float3 radiance)
{
    return float4(
        max(radiance, float3(0.0, 0.0, 0.0)),
        RT_CLEAN_RTXDI_DI_REFLECTION_SIDECAR_OPTION_B_RADIANCE);
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

bool PathTraceCleanRtxdiDiReflectionSidecarIsOptionBRadiance(float4 sidecar)
{
    return sidecar.a >= 0.75 && sidecar.a < 0.95;
}

bool PathTraceCleanRtxdiDiReflectionSidecarIsReflectionSelected(float4 sidecar)
{
    return sidecar.a >= 0.95;
}

// Option B compose gate: shaded radiance present (not PSR throughput state).
bool PathTraceCleanRtxdiDiReflectionSidecarHasRadiance(float4 sidecar)
{
    return PathTraceCleanRtxdiDiReflectionSidecarIsOptionBRadiance(sidecar);
}

float3 PathTraceCleanRtxdiDiReflectionSidecarRgb(float4 sidecar)
{
    return max(sidecar.rgb, float3(0.0, 0.0, 0.0));
}

// Lane / Option B mask:
//   green  = PSR reflection-owned surface
//   orange = Option B shaded radiance
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
    if (PathTraceCleanRtxdiDiReflectionSidecarIsOptionBRadiance(sidecar))
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
