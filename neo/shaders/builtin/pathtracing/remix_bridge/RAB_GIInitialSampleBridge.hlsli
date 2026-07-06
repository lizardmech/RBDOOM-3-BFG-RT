#ifndef RB_PATH_TRACING_REMIX_RAB_GI_INITIAL_SAMPLE_BRIDGE_HLSLI
#define RB_PATH_TRACING_REMIX_RAB_GI_INITIAL_SAMPLE_BRIDGE_HLSLI

// RTX Remix-shaped ReSTIR GI initial-sample input bridge.
//
// This header freezes the data contract consumed by Remix
// restir_gi_temporal_reuse.comp.slang before temporal resampling. It does not
// construct reservoir weights, target PDFs, MIS, virtual samples, portal
// transforms, or visibility. Those are sensitive Remix/RTXDI math boundaries
// and must be supplied by exact future bridge slices.

#include "RAB_SurfaceBridge.hlsli"
#include "RAB_GIReservoirBridge.hlsli"

static const uint REMIX_RESTIR_GI_INVALID_PORTAL_INDEX = 0xffffffffu;

static const uint REMIX_RESTIR_GI_INITIAL_FLAG_SELECTED_SURFACE = 1u << 0;
static const uint REMIX_RESTIR_GI_INITIAL_FLAG_NON_OPAQUE_HIT = 1u << 1;
static const uint REMIX_RESTIR_GI_INITIAL_FLAG_PORTAL_HIT = 1u << 2;
static const uint REMIX_RESTIR_GI_INITIAL_FLAG_VIRTUAL_SAMPLE_REQUESTED = 1u << 3;

static const uint REMIX_RESTIR_GI_INITIAL_DEFERRED_RADIANCE_FACTORING = 1u << 0;
static const uint REMIX_RESTIR_GI_INITIAL_DEFERRED_FIREFLY_FILTERING = 1u << 1;
static const uint REMIX_RESTIR_GI_INITIAL_DEFERRED_PORTAL_TRANSFORM = 1u << 2;
static const uint REMIX_RESTIR_GI_INITIAL_DEFERRED_VIRTUAL_SAMPLE = 1u << 3;
static const uint REMIX_RESTIR_GI_INITIAL_DEFERRED_TARGET_PDF = 1u << 4;
static const uint REMIX_RESTIR_GI_INITIAL_DEFERRED_RESERVOIR_STREAMING = 1u << 5;

struct RemixRestirGIRawInitialSample
{
    uint valid;
    uint flags;
    float3 radiance;
    float indirectPathLength;
    float sourcePdf;
    float3 hitPosition;
    float3 hitNormal;
    uint portalIndex;
};

struct RemixRestirGIInitialSampleControls
{
    float fireflyFilteringLuminanceThreshold;
    uint enableVirtualSample;
    float virtualSampleMaxDistanceRatio;
    float virtualSampleRoughnessThreshold;
};

struct RemixRestirGIPreparedInitialSample
{
    uint valid;
    uint flags;
    uint deferredFeatureMask;
    float3 radiance;
    float indirectPathLength;
    float3 position;
    float3 normal;
    uint portalIndex;
    RTXDI_GIReservoir reservoir;
};

#ifndef REMIX_RAB_GI_INITIAL_SAMPLE_EXTERNAL_CALLBACKS
RemixRestirGIRawInitialSample RemixRAB_LoadRawGIInitialSample(uint2 pixel)
{
    RemixRestirGIRawInitialSample sample = (RemixRestirGIRawInitialSample)0;
    sample.portalIndex = REMIX_RESTIR_GI_INVALID_PORTAL_INDEX;
    return sample;
}

RTXDI_GIReservoir RemixRAB_LoadPreparedGIInitialReservoir(
    uint2 pixel,
    RAB_Surface surface,
    RemixRestirGIRawInitialSample rawSample,
    RemixRestirGIInitialSampleControls controls)
{
    return RTXDI_EmptyGIReservoir();
}
#else
// External-callback override point: the active driver shader provides the
// definitions after including this contract; prototypes keep the contract
// functions below compilable.
RemixRestirGIRawInitialSample RemixRAB_LoadRawGIInitialSample(uint2 pixel);
RTXDI_GIReservoir RemixRAB_LoadPreparedGIInitialReservoir(
    uint2 pixel,
    RAB_Surface surface,
    RemixRestirGIRawInitialSample rawSample,
    RemixRestirGIInitialSampleControls controls);
#endif

bool RemixRestirGIIsValidRawInitialSample(RemixRestirGIRawInitialSample sample)
{
    return sample.valid != 0u &&
        (sample.flags & REMIX_RESTIR_GI_INITIAL_FLAG_SELECTED_SURFACE) != 0u &&
        all(sample.radiance == sample.radiance) &&
        all(sample.hitPosition == sample.hitPosition) &&
        all(sample.hitNormal == sample.hitNormal);
}

uint RemixRestirGIClassifyInitialSampleDeferredFeatures(
    RemixRestirGIRawInitialSample sample,
    RemixRestirGIInitialSampleControls controls)
{
    uint mask = REMIX_RESTIR_GI_INITIAL_DEFERRED_RADIANCE_FACTORING;
    mask |= REMIX_RESTIR_GI_INITIAL_DEFERRED_TARGET_PDF;
    mask |= REMIX_RESTIR_GI_INITIAL_DEFERRED_RESERVOIR_STREAMING;
    mask |= controls.fireflyFilteringLuminanceThreshold > 0.0
        ? REMIX_RESTIR_GI_INITIAL_DEFERRED_FIREFLY_FILTERING
        : 0u;
    mask |= sample.portalIndex != REMIX_RESTIR_GI_INVALID_PORTAL_INDEX
        ? REMIX_RESTIR_GI_INITIAL_DEFERRED_PORTAL_TRANSFORM
        : 0u;
    mask |= controls.enableVirtualSample != 0u
        ? REMIX_RESTIR_GI_INITIAL_DEFERRED_VIRTUAL_SAMPLE
        : 0u;
    return mask;
}

RemixRestirGIPreparedInitialSample RemixRestirGIBuildInitialSampleContract(
    RAB_Surface surface,
    uint2 pixel,
    RemixRestirGIInitialSampleControls controls)
{
    RemixRestirGIRawInitialSample rawSample = RemixRAB_LoadRawGIInitialSample(pixel);

    RemixRestirGIPreparedInitialSample prepared = (RemixRestirGIPreparedInitialSample)0;
    prepared.portalIndex = REMIX_RESTIR_GI_INVALID_PORTAL_INDEX;
    prepared.reservoir = RTXDI_EmptyGIReservoir();
    prepared.deferredFeatureMask = RemixRestirGIClassifyInitialSampleDeferredFeatures(rawSample, controls);

    if (!RAB_IsSurfaceValid(surface) || !RemixRestirGIIsValidRawInitialSample(rawSample))
    {
        return prepared;
    }

    prepared.valid = 1u;
    prepared.flags = rawSample.flags;
    prepared.radiance = rawSample.radiance;
    prepared.indirectPathLength = rawSample.indirectPathLength;
    prepared.position = rawSample.hitPosition;
    prepared.normal = rawSample.hitNormal;
    prepared.portalIndex = rawSample.portalIndex;

    // Do not synthesize a reservoir here. Remix's initial GI reservoir depends
    // on exact radiance factoring, target PDF, RIS update, and finalization.
    // The active bridge must provide it through an exact Remix/RTXDI helper-
    // shaped path.
    prepared.reservoir = RemixRAB_LoadPreparedGIInitialReservoir(
        pixel,
        surface,
        rawSample,
        controls);
    return prepared;
}

#endif
