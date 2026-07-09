#ifndef RB_PATH_TRACING_CLEANROOM_COMMON_RESTIR_DI_INITIAL_SAMPLING_HLSLI
#define RB_PATH_TRACING_CLEANROOM_COMMON_RESTIR_DI_INITIAL_SAMPLING_HLSLI

#include "cleanroom_common/restir_di_parameters.hlsli"
#include "cleanroom_common/restir_di_reservoir.hlsli"
#include "Rtxdi/Utils/RandomSamplerState.hlsli"

// rbdoom-owned DI initial sampling, replacing Rtxdi/DI/InitialSampling.hlsli.
// The candidate loop is streaming RIS over uniformly chosen local lights
// (Bitterli et al. 2020, Alg. 3): each of N candidates is drawn uniformly
// from the light region (sourcePdf = 1/numLights, so invSourcePdf =
// numLights), weighted by the surface target function, and the survivor is
// normalized by 1/N via RTXDI_FinalizeResampling. The returned reservoir
// carries M = 1, matching how the repo's seed producers treat fresh initial
// reservoirs.
//
// Like the NVIDIA convention this replaces, the including shader must define
// the RAB_* callbacks before this header is included:
//     RAB_LoadLightInfo(uint lightIndex, bool previousFrame)
//     RAB_SamplePolymorphicLight(lightInfo, surface, uv)
//     RAB_GetLightSampleTargetPdfForSurface(lightSample, surface)
//     RAB_GetConservativeVisibility(surface, lightSample)
//     RAB_IsLightInfoValid / RAB_EmptyLightSample
//
// Scope honestly limited to what the repo exercises today: uniform local
// sampling only. All current call sites pass numInfiniteLightSamples =
// numEnvironmentSamples = numBrdfSamples = 0 and brdfCutoff = 0.0; those
// strategies are not implemented, and their candidate counts are ignored.
// If a future call site enables them, the MIS accounting in
// RTXDI_ComputeInitialSamplingMisData must be extended at the same time -
// do not just flip the counts on.

struct RTXDI_InitialSamplingMisData
{
    float localLightMisWeight;
};

// MIS weight applied to local-light candidates. With local sampling as the
// only active strategy the balance-heuristic weight is exactly 1; the
// brdf-strategy split is left unimplemented (see header note).
RTXDI_InitialSamplingMisData RTXDI_ComputeInitialSamplingMisData(
    RTXDI_DIInitialSamplingParameters params)
{
    RTXDI_InitialSamplingMisData misData = (RTXDI_InitialSamplingMisData)0;
    misData.localLightMisWeight = params.numLocalLightSamples > 0u ? 1.0 : 0.0;
    return misData;
}

float2 RTXDI_RandomlySelectLocalLightUV(inout RTXDI_RandomSamplerState rng)
{
    return float2(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng));
}

// Evaluate one light candidate at a fixed (lightIndex, uv) and stream it
// into the reservoir with weight targetPdf * invSourcePdf * misWeight.
// On selection the shaded sample is written through to selectedSample so
// the caller never has to re-sample the winner.
bool RTXDI_StreamLocalLightAtUVIntoReservoir(
    inout RTXDI_RandomSamplerState rng,
    RTXDI_InitialSamplingMisData misData,
    RAB_Surface surface,
    float brdfCutoff,
    float misWeight,
    uint lightIndex,
    float2 uv,
    float invSourcePdf,
    RAB_LightInfo lightInfo,
    inout RTXDI_DIReservoir reservoir,
    inout RAB_LightSample selectedSample)
{
    const RAB_LightSample candidateSample = RAB_SamplePolymorphicLight(lightInfo, surface, uv);
    float targetPdf = max(RAB_GetLightSampleTargetPdfForSurface(candidateSample, surface), 0.0);
    if (candidateSample.valid == 0u || candidateSample.solidAnglePdf <= 0.0)
    {
        targetPdf = 0.0;
    }

    // brdfCutoff implements a brdf-strategy interaction that no repo call
    // site enables (always 0.0 = disabled). Nonzero values are not supported.
    const bool selected = RTXDI_StreamSample(
        reservoir,
        lightIndex,
        uv,
        RTXDI_GetNextRandom(rng),
        targetPdf,
        max(invSourcePdf, 0.0) * max(misWeight, 0.0));
    if (selected)
    {
        selectedSample = candidateSample;
    }
    return selected;
}

// Uniform local-light RIS: draw params.numLocalLightSamples candidates from
// localLightBufferRegion, stream them, normalize by candidate count, and
// hand back a fresh reservoir with M = 1.
RTXDI_DIReservoir RTXDI_SampleLocalLights(
    inout RTXDI_RandomSamplerState rng,
    inout RTXDI_RandomSamplerState coherentRng,
    RAB_Surface surface,
    RTXDI_DIInitialSamplingParameters params,
    RTXDI_InitialSamplingMisData misData,
    ReSTIRDI_LocalLightSamplingMode localLightSamplingMode,
    RTXDI_LightBufferRegion localLightBufferRegion,
    out RAB_LightSample selectedSample)
{
    selectedSample = RAB_EmptyLightSample();
    RTXDI_DIReservoir reservoir = RTXDI_EmptyDIReservoir();
    const uint lightCount = localLightBufferRegion.numLights;
    if (lightCount == 0u || params.numLocalLightSamples == 0u)
    {
        return reservoir;
    }

    // Only ReSTIRDI_LocalLightSamplingMode_UNIFORM is implemented; other
    // modes (presampled tiles, ReGIR) fall back to uniform selection, which
    // is unbiased for every mode - just not variance-optimal.
    const float invSourcePdf = (float)lightCount;

    [loop]
    for (uint sampleIndex = 0u; sampleIndex < params.numLocalLightSamples; ++sampleIndex)
    {
        const uint offset = min(
            (uint)(RTXDI_GetNextRandom(rng) * (float)lightCount),
            lightCount - 1u);
        const uint lightIndex = localLightBufferRegion.firstLightIndex + offset;
        const RAB_LightInfo lightInfo = RAB_LoadLightInfo(lightIndex, false);
        const float2 uv = RTXDI_RandomlySelectLocalLightUV(rng);
        if (!RAB_IsLightInfoValid(lightInfo))
        {
            // Black-noise rule: do not inflate M for invalid/zero candidates.
            continue;
        }

        RTXDI_StreamLocalLightAtUVIntoReservoir(
            rng,
            misData,
            surface,
            params.brdfCutoff,
            misData.localLightMisWeight,
            lightIndex,
            uv,
            invSourcePdf,
            lightInfo,
            reservoir,
            selectedSample);
    }

    RTXDI_FinalizeResampling(reservoir, 1.0, max(reservoir.M, 1.0));
    reservoir.M = 1.0;
    return reservoir;
}

// Top-level initial sampling over all light strategies. Only the local-light
// strategy is active (see header note); infinite/environment/brdf candidate
// counts are ignored. Optionally traces the selected sample's visibility and
// discards occluded samples, storing the result for downstream reuse.
RTXDI_DIReservoir RTXDI_SampleLightsForSurface(
    inout RTXDI_RandomSamplerState rng,
    inout RTXDI_RandomSamplerState coherentRng,
    RAB_Surface surface,
    RTXDI_DIInitialSamplingParameters params,
    RTXDI_LightBufferParameters lightBufferParams,
    out RAB_LightSample selectedSample)
{
    selectedSample = RAB_EmptyLightSample();
    RTXDI_DIReservoir reservoir = RTXDI_SampleLocalLights(
        rng,
        coherentRng,
        surface,
        params,
        RTXDI_ComputeInitialSamplingMisData(params),
        params.localLightSamplingMode,
        lightBufferParams.localLightBufferRegion,
        selectedSample);

    if (params.enableInitialVisibility != 0u && RTXDI_IsValidDIReservoir(reservoir))
    {
        const float visibility = RAB_GetConservativeVisibility(surface, selectedSample) ? 1.0 : 0.0;
        RTXDI_StoreVisibilityInDIReservoir(reservoir, visibility.xxx, true);
    }

    return reservoir;
}

#endif
