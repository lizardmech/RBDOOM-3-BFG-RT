#ifndef PATH_TRACE_LIQUID_POOL_CONTROL_HLSLI
#define PATH_TRACE_LIQUID_POOL_CONTROL_HLSLI

// Frozen route/source legend shared by every liquid-pool producer and
// secondary consumer. Keep these values synchronized with
// docs/liquid_pool_decals/control_transport_contract.txt.
static const uint RT_LIQUID_POOL_SOURCE_NONE = 0u;
static const uint RT_LIQUID_POOL_SOURCE_PRIMARY = 1u;
static const uint RT_LIQUID_POOL_SOURCE_CLEAN_DI_REFLECTION = 2u;
static const uint RT_LIQUID_POOL_SOURCE_RESTIR_REFLECTION = 3u;
static const uint RT_LIQUID_POOL_SOURCE_GI_FIRST_INDIRECT = 4u;
static const uint RT_LIQUID_POOL_SOURCE_GI_CONTINUATION = 5u;
static const uint RT_LIQUID_POOL_SOURCE_GI_RAY_QUERY = 6u;
static const uint RT_LIQUID_POOL_SOURCE_INVALID = 7u;

static const uint RT_LIQUID_POOL_STATUS_CANDIDATE = 1u << 0u;
static const uint RT_LIQUID_POOL_STATUS_RECEIVER_VALID = 1u << 1u;
static const uint RT_LIQUID_POOL_STATUS_APPLIED = 1u << 2u;
static const uint RT_LIQUID_POOL_STATUS_OVERFLOW = 1u << 3u;
static const uint RT_LIQUID_POOL_STATUS_RECEIVER_REJECTED = 1u << 4u;
static const uint RT_LIQUID_POOL_STATUS_DUPLICATE_APPLY = 1u << 5u;
static const uint RT_LIQUID_POOL_STATUS_FAIL_CLOSED = 1u << 6u;
static const uint RT_LIQUID_POOL_STATUS_INVALID_ROUTE = 1u << 7u;

// The fourth control word is a bitfield rather than an unrelated padding lane.
static const uint RT_LIQUID_POOL_CONTROL_TELEMETRY_READY = 1u << 0u;
static const uint RT_LIQUID_POOL_CONTROL_REQUESTED = 1u << 1u;
static const uint RT_LIQUID_POOL_CONTROL_ROUTE_DISABLED = 1u << 2u;
static const uint RT_LIQUID_POOL_CONTROL_PARAMETERS_READY = 1u << 3u;

bool PathTraceLiquidPoolDebugPageIsValid(uint debug, uint page)
{
    if (debug == 0u)
    {
        return true;
    }
    if (debug >= 1u && debug <= 3u)
    {
        return page == 0u;
    }
    if (debug == 4u)
    {
        return page <= 2u;
    }
    if (debug == 5u)
    {
        return page <= 3u;
    }
    if (debug == 6u)
    {
        return page <= 1u;
    }
    return false;
}

uint PathTraceLiquidPoolControlInitialStatus(uint controlFlags, uint debug, uint page)
{
    uint status = (controlFlags & RT_LIQUID_POOL_CONTROL_ROUTE_DISABLED) != 0u
        ? RT_LIQUID_POOL_STATUS_FAIL_CLOSED
        : 0u;
    if (!PathTraceLiquidPoolDebugPageIsValid(debug, page))
    {
        status |= RT_LIQUID_POOL_STATUS_INVALID_ROUTE;
    }
    return status;
}

float4 PathTraceLiquidPoolRouteDiagnostic(uint sourceId, uint statusMask)
{
    return float4((float)sourceId, (float)statusMask, 0.0, 0.0);
}

#endif
