#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_TRANSMISSION_PROBE_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_TRANSMISSION_PROBE_HLSLI

// GEO-10 transmission isolation is local to the transmission producer. Keep
// its diagnostic encoding out of the shared clean-DI ABI so unrelated shader
// families do not inherit this ladder.
static const uint CLEAN_FLAG_TRANSMISSION_TRACE_PROBE_SHIFT = 28u;
static const uint CLEAN_FLAG_TRANSMISSION_TRACE_PROBE_MASK =
    7u << CLEAN_FLAG_TRANSMISSION_TRACE_PROBE_SHIFT;
static const uint CLEAN_FLAG_TRANSMISSION_ITERATIVE_RESOLVE = 1u << 31u;

uint PathTraceCleanRtxdiDiTransmissionTraceProbeMode()
{
    return (CleanRtxdiDiFlags &
        CLEAN_FLAG_TRANSMISSION_TRACE_PROBE_MASK) >>
        CLEAN_FLAG_TRANSMISSION_TRACE_PROBE_SHIFT;
}

bool PathTraceCleanRtxdiDiTransmissionIterativeResolveEnabled()
{
    return (CleanRtxdiDiFlags &
        CLEAN_FLAG_TRANSMISSION_ITERATIVE_RESOLVE) != 0u;
}

bool PathTraceCleanRtxdiDiTransmissionTupleDiagnosticEnabled()
{
    // Probe mode 7 alone belongs to the stage-16 IgnoreHit isolate. The
    // iterative+mode-7 combination is reserved for GEO-10 stage 22.
    return PathTraceCleanRtxdiDiTransmissionIterativeResolveEnabled() &&
        PathTraceCleanRtxdiDiTransmissionTraceProbeMode() == 7u;
}

bool PathTraceCleanRtxdiDiTransmissionClosestHitPositionDiagnosticEnabled()
{
    // Probe mode 6 alone belongs to the rejected stage-15 repeated-any-hit
    // isolate. The iterative+mode-6 combination is reserved for stage 23.
    return PathTraceCleanRtxdiDiTransmissionIterativeResolveEnabled() &&
        PathTraceCleanRtxdiDiTransmissionTraceProbeMode() == 6u;
}

#endif
