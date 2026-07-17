#ifndef RB_PATH_TRACING_REMIX_RESTIR_GI_TEMPORAL_REUSE_RT_HLSL
#define RB_PATH_TRACING_REMIX_RESTIR_GI_TEMPORAL_REUSE_RT_HLSL

// RTX Remix-shaped ReSTIR GI temporal contract.
//
// This file is intentionally inactive. It freezes the rbdoom contract boundary
// for the Remix restir_gi_temporal_reuse.comp.slang file-equivalent without
// approximating Remix-specific math. Reservoir combination, temporal sampling,
// Jacobian weighting, and bias correction are delegated to RTXDI GI helpers.
// Initial-sample shaping, portals, reflection reprojection, exact visibility,
// virtual samples, firefly filtering, and gradient validation are deferred
// until their inputs can be matched exactly.

#define RTXDI_GI_ALLOWED_BIAS_CORRECTION RTXDI_BIAS_CORRECTION_BASIC

#include "../remix_bridge/RAB_SurfaceBridge.hlsli"
#include "../remix_bridge/RAB_GIReservoirBridge.hlsli"
#include "../remix_bridge/RAB_GIInitialSampleBridge.hlsli"
#include "../remix_bridge/RAB_GITemporalValidationBridge.hlsli"
#include "Rtxdi/Utils/RandomSamplerState.hlsli"

#include "Rtxdi/GI/TemporalResampling.hlsli"

static const uint REMIX_RESTIR_GI_TEMPORAL_RNG_PASS = 0x52525808u;

struct RemixRestirGITemporalReuseDesc
{
    uint2 pixel;
    uint frameIndex;
    float3 screenSpaceMotion;
    uint initSamplePage;
    uint temporalInputPage;
    uint temporalOutputPage;
    uint activeCheckerboardField;
    uint enableTemporalReuse;
    uint enableLightingValidation;
    uint enableReflectionReprojection;
    uint enableVirtualSamples;
    uint enablePortalTransform;
    float fireflyFilteringLuminanceThreshold;
    float virtualSampleMaxDistanceRatio;
    float virtualSampleRoughnessThreshold;
    float lightingValidationThreshold;
    float lightingValidationDepthTolerance;
    uint numActivePortals;
    float depthThreshold;
    float normalThreshold;
    uint maxHistoryLength;
    uint enableFallbackSampling;
    uint biasCorrectionMode;
    uint maxReservoirAge;
    uint enablePermutationSampling;
    uint uniformRandomNumber;
    uint enableDlssRrCompatibility;
    float dlssRrTemporalRandomizationRadius;
    float dlssRrDiffuseProbability;
};

struct RemixRestirGITemporalReuseResult
{
    RTXDI_GIReservoir initialReservoir;
    RTXDI_GIReservoir temporalReservoir;
    uint storedInitialReservoir;
    uint storedTemporalReservoir;
    uint staleSampleRejected;
    uint deferredFeatureMask;
};

static const uint REMIX_RESTIR_GI_DEFERRED_INITIAL_SAMPLE_SHAPING = 1u << 0;
static const uint REMIX_RESTIR_GI_DEFERRED_LIGHTING_VALIDATION = 1u << 1;
static const uint REMIX_RESTIR_GI_DEFERRED_REFLECTION_REPROJECTION = 1u << 2;
static const uint REMIX_RESTIR_GI_DEFERRED_VIRTUAL_SAMPLES = 1u << 3;
static const uint REMIX_RESTIR_GI_DEFERRED_PORTAL_TRANSFORM = 1u << 4;
static const uint REMIX_RESTIR_GI_DEFERRED_VISIBILITY = 1u << 5;

RTXDI_RuntimeParameters RemixRestirGITemporalRuntimeParameters(RemixRestirGITemporalReuseDesc desc)
{
    RTXDI_RuntimeParameters params = (RTXDI_RuntimeParameters)0;
    params.activeCheckerboardField = desc.activeCheckerboardField;
    params.frameIndex = desc.frameIndex;
    return params;
}

RTXDI_GITemporalResamplingParameters RemixRestirGITemporalParameters(RemixRestirGITemporalReuseDesc desc)
{
    RTXDI_GITemporalResamplingParameters params = (RTXDI_GITemporalResamplingParameters)0;
    params.depthThreshold = desc.depthThreshold;
    params.normalThreshold = desc.normalThreshold;
    params.maxHistoryLength = desc.maxHistoryLength;
    params.enableFallbackSampling = desc.enableFallbackSampling;
    // Mode 2 is the rbdoom clean-room target-PDF MIS normalization A/B. Keep
    // it intact for the local temporal implementation instead of clamping it
    // to the support-count BASIC mode here.
    params.biasCorrectionMode = min(desc.biasCorrectionMode, 2u);
    params.maxReservoirAge = desc.maxReservoirAge;
    params.enablePermutationSampling = desc.enablePermutationSampling;
    params.uniformRandomNumber = desc.uniformRandomNumber;
    params.enableDlssRrCompatibility = desc.enableDlssRrCompatibility;
    params.dlssRrTemporalRandomizationRadius = desc.dlssRrTemporalRandomizationRadius;
    params.dlssRrDiffuseProbability = desc.dlssRrDiffuseProbability;
    return params;
}

uint RemixRestirGIGetDeferredFeatureMask(RemixRestirGITemporalReuseDesc desc)
{
    uint mask = REMIX_RESTIR_GI_DEFERRED_INITIAL_SAMPLE_SHAPING;
    mask |= desc.enableLightingValidation != 0u ? REMIX_RESTIR_GI_DEFERRED_LIGHTING_VALIDATION : 0u;
    mask |= desc.enableReflectionReprojection != 0u ? REMIX_RESTIR_GI_DEFERRED_REFLECTION_REPROJECTION : 0u;
    mask |= desc.enableVirtualSamples != 0u ? REMIX_RESTIR_GI_DEFERRED_VIRTUAL_SAMPLES : 0u;
    mask |= desc.enablePortalTransform != 0u ? REMIX_RESTIR_GI_DEFERRED_PORTAL_TRANSFORM : 0u;
    // Local mode 2 is target-PDF MIS normalization, not the higher RTXDI
    // ray-traced bias-correction mode that would request deferred visibility.
    mask |= desc.biasCorrectionMode > 2u ? REMIX_RESTIR_GI_DEFERRED_VISIBILITY : 0u;
    return mask;
}

RTXDI_GIReservoir RemixRestirGIAcceptPreparedInitialReservoir(
    RemixRestirGIPreparedInitialSample sample)
{
    // RRX-08A accepts only a reservoir that an exact future Remix-shaped
    // initial-sample bridge has already prepared. It intentionally does not
    // reconstruct Remix radiance factoring, firefly filtering, virtual-sample
    // movement, portal transforms, MIS, or target-PDF math here.
    if (sample.valid == 0u)
    {
        return RTXDI_EmptyGIReservoir();
    }
    return sample.reservoir;
}

RemixRestirGITemporalReuseResult RemixRestirGIRunTemporalReuseContract(
    RAB_Surface currentSurface,
    RemixRestirGITemporalReuseDesc desc)
{
    RemixRestirGITemporalReuseResult result = (RemixRestirGITemporalReuseResult)0;
    result.initialReservoir = RTXDI_EmptyGIReservoir();
    result.temporalReservoir = RTXDI_EmptyGIReservoir();
    result.deferredFeatureMask = RemixRestirGIGetDeferredFeatureMask(desc);

    if (!RAB_IsSurfaceValid(currentSurface))
    {
        RAB_StoreGIReservoir(result.temporalReservoir, int2(desc.pixel), int(desc.temporalOutputPage));
        result.storedTemporalReservoir = 1u;
        return result;
    }

    RemixRestirGIInitialSampleControls initialControls = (RemixRestirGIInitialSampleControls)0;
    initialControls.fireflyFilteringLuminanceThreshold = desc.fireflyFilteringLuminanceThreshold;
    initialControls.enableVirtualSample = desc.enableVirtualSamples;
    initialControls.virtualSampleMaxDistanceRatio = desc.virtualSampleMaxDistanceRatio;
    initialControls.virtualSampleRoughnessThreshold = desc.virtualSampleRoughnessThreshold;

    RemixRestirGIPreparedInitialSample initialSample =
        RemixRestirGIBuildInitialSampleContract(currentSurface, desc.pixel, initialControls);

    result.deferredFeatureMask |= initialSample.deferredFeatureMask;
    result.initialReservoir = RemixRestirGIAcceptPreparedInitialReservoir(initialSample);

    RAB_StoreGIReservoir(result.initialReservoir, int2(desc.pixel), int(desc.initSamplePage));
    result.storedInitialReservoir = 1u;

    if (desc.enableTemporalReuse == 0u)
    {
        result.temporalReservoir = result.initialReservoir;
    }
    else
    {
        RTXDI_RandomSamplerState rng = RTXDI_InitRandomSamplerForPass(
            desc.pixel,
            desc.frameIndex,
            REMIX_RESTIR_GI_TEMPORAL_RNG_PASS,
            0u);
        // Keep the temporal winner-selection coin flip white-noise. Feeding
        // the producer STBN mask into this recursive decision imprints its
        // frame-to-frame structure into reservoir history, turning the
        // producer's fine blue-noise error into persistent low-frequency
        // islands. Producer/initial and spatial sampling retain STBN.
        CleanGiDisableBlueNoise(rng);

        result.temporalReservoir = RTXDI_GITemporalResampling(
            desc.pixel,
            currentSurface,
            desc.screenSpaceMotion,
            desc.temporalInputPage,
            result.initialReservoir,
            rng,
            RemixRestirGITemporalRuntimeParameters(desc),
            RemixRAB_GIReservoirParams,
            RemixRestirGITemporalParameters(desc));
    }

    // Exact Remix gradient sample validation depends on the functional RRX-07
    // gradient pipeline. The default bridge records the dependency but does not
    // synthesize stale-sample rejection from local math.
    RemixRestirGITemporalValidationDesc validationDesc = (RemixRestirGITemporalValidationDesc)0;
    validationDesc.enableLightingValidation = desc.enableLightingValidation;
    validationDesc.lightingValidationThreshold = desc.lightingValidationThreshold;
    validationDesc.gradientDepthTolerance = desc.lightingValidationDepthTolerance;
    validationDesc.enableRayTracedVisibility = desc.biasCorrectionMode > uint(RTXDI_BIAS_CORRECTION_BASIC);
    validationDesc.numActivePortals = desc.numActivePortals;

    RemixRestirGITemporalValidationResult validationResult =
        RemixRAB_ValidateGITemporalReservoir(
            currentSurface,
            result.initialReservoir,
            result.temporalReservoir,
            validationDesc);

    result.temporalReservoir = validationResult.reservoir;
    result.staleSampleRejected = validationResult.staleSampleRejected;

    RAB_StoreGIReservoir(result.temporalReservoir, int2(desc.pixel), int(desc.temporalOutputPage));
    result.storedTemporalReservoir = 1u;
    return result;
}

#endif
