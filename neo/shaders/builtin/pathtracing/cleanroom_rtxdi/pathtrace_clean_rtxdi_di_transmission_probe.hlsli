#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_TRANSMISSION_PROBE_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_TRANSMISSION_PROBE_HLSLI

// GEO-10 transmission isolation is local to the transmission producer. Keep
// its diagnostic encoding out of the shared clean-DI ABI so unrelated shader
// families do not inherit this ladder.
static const uint CLEAN_FLAG_TRANSMISSION_TRACE_PROBE_SHIFT = 28u;
static const uint CLEAN_FLAG_TRANSMISSION_TRACE_PROBE_MASK =
    7u << CLEAN_FLAG_TRANSMISSION_TRACE_PROBE_SHIFT;

uint PathTraceCleanRtxdiDiTransmissionTraceProbeMode()
{
    return (CleanRtxdiDiFlags &
        CLEAN_FLAG_TRANSMISSION_TRACE_PROBE_MASK) >>
        CLEAN_FLAG_TRANSMISSION_TRACE_PROBE_SHIFT;
}

#endif
