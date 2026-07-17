#ifndef RB_PATH_TRACING_CLEANROOM_COMMON_RESTIR_GI_TEMPORAL_RESAMPLING_HLSLI
#define RB_PATH_TRACING_CLEANROOM_COMMON_RESTIR_GI_TEMPORAL_RESAMPLING_HLSLI

#include "cleanroom_common/restir_gi_parameters.hlsli"
#include "cleanroom_common/restir_gi_reservoir.hlsli"
#include "Rtxdi/Utils/RandomSamplerState.hlsli"

// rbdoom-owned GI temporal resampling, replacing
// Rtxdi/GI/TemporalResampling.hlsli. Math: Ouyang et al. 2021 (ReSTIR GI) -
// two-candidate streaming merge of the fresh initial sample with the
// back-projected history sample. Candidate weight is
// m * p-hat_q(T(s)) * W. Mode 0 finalizes by 1/M, mode 1 retains the local
// support-count BASIC normalization, and mode 2 uses the Remix-shaped
// selected-target numerator over the target-pdf-weighted domain sum.
//
// The including shader must define, before this header:
//   RAB_GetGBufferSurface(int2 pixel, bool previousFrame)   (bounds-safe)
//   RAB_IsSurfaceValid / RAB_GetSurfaceNormal / RAB_GetSurfaceLinearDepth /
//   RAB_GetSurfaceWorldPos
//   RAB_GetGISampleTargetPdfForSurface(samplePosition, sampleRadiance, surface)
//   RTXDI_LoadGIReservoir (Rtxdi/GI/Reservoir.hlsli storage, i.e. the GI
//   reservoir bridge with RTXDI_GI_RESERVOIR_BUFFER set up)
//
// Scope notes:
//   - biasCorrectionMode: 0 = 1/M, 1 = local support-count BASIC, 2 =
//     Remix-shaped target-PDF MIS normalization. Ray-traced temporal
//     revalidation remains deferred to the lighting-validation slice.
//   - enableFallbackSampling: when the back-projected pixel fails its gates,
//     one extra candidate pixel is tried from a small jitter disc around it
//     (recovers history across small reprojection misses at thin geometry).
//   - Jacobian validity is delegated to RAB_ValidateGISampleWithJacobian when
//     the bridge callback is present. The local cutoff is only the standalone
//     fallback for direct includes.

#ifndef RBPT_GI_TEMPORAL_JACOBIAN_CUTOFF
#define RBPT_GI_TEMPORAL_JACOBIAN_CUTOFF 10.0
#endif

#ifndef RBPT_GI_TEMPORAL_FALLBACK_RADIUS
#define RBPT_GI_TEMPORAL_FALLBACK_RADIUS 3.0
#endif

// Reconnection-shift jacobian (Ouyang et al. 2021, Eq. 11). Same math as
// RBPT_GIReconnectionJacobian in the GI spatial replacement; named
// differently so both headers stay self-contained without an include-order
// contract between them.
float RBPT_GITemporalReconnectionJacobian(
    float3 receiverFromPos,
    float3 receiverToPos,
    RTXDI_GIReservoir reservoir)
{
    const float3 toFrom = receiverFromPos - reservoir.position;
    const float3 toTo = receiverToPos - reservoir.position;
    const float dFrom2 = dot(toFrom, toFrom);
    const float dTo2 = dot(toTo, toTo);
    if (dFrom2 <= 1.0e-12 || dTo2 <= 1.0e-12)
    {
        return 0.0;
    }

    const float cosFrom = dot(reservoir.normal, toFrom) * rsqrt(dFrom2);
    const float cosTo = dot(reservoir.normal, toTo) * rsqrt(dTo2);
    if (cosFrom <= 1.0e-4 || cosTo <= 1.0e-4)
    {
        return 0.0;
    }

    return (cosTo / cosFrom) * (dFrom2 / dTo2);
}

bool RBPT_GITemporalValidateJacobian(float jacobian)
{
#ifdef RBPT_HAS_GI_TEMPORAL_JACOBIAN_VALIDATION_CALLBACK
    return RAB_ValidateGISampleWithJacobian(jacobian);
#else
    if (jacobian < 1.0 / RBPT_GI_TEMPORAL_JACOBIAN_CUTOFF ||
        jacobian > RBPT_GI_TEMPORAL_JACOBIAN_CUTOFF ||
        !(jacobian > 0.0) ||
        !(jacobian == jacobian))
    {
        return false;
    }
    return true;
#endif
}

bool RBPT_GITemporalSurfacesSimilar(
    RAB_Surface surface,
    RAB_Surface previousSurface,
    float expectedPreviousDepth,
    RTXDI_GITemporalResamplingParameters tparams)
{
    const float previousDepth = RAB_GetSurfaceLinearDepth(previousSurface);
    if (abs(previousDepth - expectedPreviousDepth) >
        tparams.depthThreshold * max(abs(expectedPreviousDepth), 1.0e-4))
    {
        return false;
    }
    return dot(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceNormal(previousSurface)) >=
        tparams.normalThreshold;
}

// Find a history reservoir for this pixel: the back-projected pixel first,
// optionally one jittered fallback. Returns false when no candidate passes
// the surface gates with a valid, young-enough reservoir.
bool RBPT_GITemporalFindHistory(
    uint2 pixelPosition,
    RAB_Surface surface,
    float3 screenSpaceMotion,
    uint temporalInputPage,
    RTXDI_RuntimeParameters runtimeParams,
    RTXDI_ReservoirBufferParameters reservoirParams,
    RTXDI_GITemporalResamplingParameters tparams,
    bool useDlssRrRandomizedReprojection,
    inout RTXDI_RandomSamplerState rng,
    out RTXDI_GIReservoir historyReservoir,
    out float3 historySurfacePos,
    out int2 historyPixel)
{
    historyReservoir = RTXDI_EmptyGIReservoir();
    historySurfacePos = float3(0.0, 0.0, 0.0);
    historyPixel = int2(0, 0);

    int2 basePixel = int2(round(float2(pixelPosition) + screenSpaceMotion.xy));
    if (tparams.enablePermutationSampling != 0u)
    {
        RTXDI_ApplyPermutationSampling(basePixel, tparams.uniformRandomNumber);
    }
    if (useDlssRrRandomizedReprojection && tparams.dlssRrTemporalRandomizationRadius > 0.0)
    {
        // A wide stochastic reprojection deliberately trades coherent history
        // islands for high-frequency temporal error that RR can reconstruct.
        // sqrt(r) keeps samples uniform over the disc rather than clustering
        // them at the reprojected center.
        const float angle = RTXDI_GetNextRandom(rng) * 6.28318530718;
        const float radius = sqrt(RTXDI_GetNextRandom(rng)) *
            tparams.dlssRrTemporalRandomizationRadius;
        basePixel += int2(round(float2(cos(angle), sin(angle)) * radius));
    }
    const float expectedPreviousDepth =
        RAB_GetSurfaceLinearDepth(surface) + screenSpaceMotion.z;

    const uint attemptCount = tparams.enableFallbackSampling != 0u ? 2u : 1u;
    [loop]
    for (uint attempt = 0u; attempt < attemptCount; ++attempt)
    {
        int2 candidatePixel = basePixel;
        if (attempt > 0u)
        {
            const float2 jitter = float2(
                RTXDI_GetNextRandom(rng) * 2.0 - 1.0,
                RTXDI_GetNextRandom(rng) * 2.0 - 1.0) * RBPT_GI_TEMPORAL_FALLBACK_RADIUS;
            candidatePixel = basePixel + int2(round(jitter));
        }

        // Bounds-safe loader: off-screen pixels return an invalid surface.
        const RAB_Surface previousSurface = RAB_GetGBufferSurface(candidatePixel, true);
        if (!RAB_IsSurfaceValid(previousSurface) ||
            !RBPT_GITemporalSurfacesSimilar(surface, previousSurface, expectedPreviousDepth, tparams))
        {
            continue;
        }

        const RTXDI_GIReservoir candidate = RTXDI_LoadGIReservoir(
            reservoirParams,
            RTXDI_PixelPosToReservoirPos(uint2(candidatePixel), runtimeParams.activeCheckerboardField),
            temporalInputPage);
        if (!RTXDI_IsValidGIReservoir(candidate) ||
            !(candidate.weightSum > 0.0 && candidate.weightSum < 1.0e20) ||
            !all(candidate.radiance == candidate.radiance) ||
            !all(candidate.position == candidate.position) ||
            (tparams.maxReservoirAge > 0u && candidate.age >= tparams.maxReservoirAge))
        {
            continue;
        }

        historyReservoir = candidate;
        historySurfacePos = RAB_GetSurfaceWorldPos(previousSurface);
        historyPixel = candidatePixel;
        return true;
    }
    return false;
}

RTXDI_GIReservoir RTXDI_GITemporalResampling(
    uint2 pixelPosition,
    RAB_Surface surface,
    float3 screenSpaceMotion,
    uint temporalInputPage,
    RTXDI_GIReservoir inputReservoir,
    inout RTXDI_RandomSamplerState rng,
    RTXDI_RuntimeParameters runtimeParams,
    RTXDI_ReservoirBufferParameters reservoirParams,
    RTXDI_GITemporalResamplingParameters tparams)
{
    if (!RAB_IsSurfaceValid(surface) || tparams.maxHistoryLength == 0u)
    {
        return inputReservoir;
    }

    // Remix's RR compatibility mode only decorrelates the diffuse share. The
    // fresh candidate receives more effective confidence at the same time so
    // a randomly fetched coherent history sample cannot dominate the merge.
    const bool useDlssRrRandomizedReprojection =
        tparams.enableDlssRrCompatibility != 0u &&
        RTXDI_GetNextRandom(rng) < saturate(tparams.dlssRrDiffuseProbability);
    if (useDlssRrRandomizedReprojection && RTXDI_IsValidGIReservoir(inputReservoir))
    {
        inputReservoir.M = min((uint)((float)inputReservoir.M / 0.26), 255u);
    }

    RTXDI_GIReservoir historyReservoir;
    float3 historySurfacePos;
    int2 historyPixel;
    if (!RBPT_GITemporalFindHistory(
        pixelPosition,
        surface,
        screenSpaceMotion,
        temporalInputPage,
        runtimeParams,
        reservoirParams,
        tparams,
        useDlssRrRandomizedReprojection,
        rng,
        historyReservoir,
        historySurfacePos,
        historyPixel))
    {
        return inputReservoir;
    }

    // Clamp accumulated confidence so stale history cannot dominate fresh
    // candidates (Bitterli 2020 Sec. 5; deployed-Remix-style low caps).
    historyReservoir.M = min(
        historyReservoir.M,
        tparams.maxHistoryLength * max(inputReservoir.M, 1u));

    const float3 receiverPos = RAB_GetSurfaceWorldPos(surface);
    const bool canonicalValid =
        RTXDI_IsValidGIReservoir(inputReservoir) && inputReservoir.weightSum > 0.0;
    if (!canonicalValid &&
        tparams.maxReservoirAge > 0u &&
        historyReservoir.age >= min(tparams.maxReservoirAge, 4u))
    {
        return inputReservoir;
    }

    const float canonicalM = canonicalValid ? (float)inputReservoir.M : 0.0;
    const float historyM = (float)historyReservoir.M;

    // Jacobian validation only. The temporal output feeds itself on the next
    // frame, so multiplying stored W by J every frame can turn a single bad
    // shifted sample into a persistent blotchy cluster. Spatial reuse applies
    // J to one-frame neighbor proposals; temporal history must only reject
    // inadmissible shifts until exact Remix gradient/visibility validation
    // exists.
    const float jHistoryToCurrent = RBPT_GITemporalReconnectionJacobian(
        historySurfacePos, receiverPos, historyReservoir);
    const bool historyJacobianValid = RBPT_GITemporalValidateJacobian(jHistoryToCurrent);
    const float historyTargetAtCurrent = historyJacobianValid
        ? RAB_GetGISampleTargetPdfForSurface(historyReservoir.position, historyReservoir.radiance, surface)
        : 0.0;
    if (!(historyTargetAtCurrent > 0.0))
    {
        return inputReservoir;
    }

    const float canonicalTarget = canonicalValid
        ? RAB_GetGISampleTargetPdfForSurface(inputReservoir.position, inputReservoir.radiance, surface)
        : 0.0;

    // Two-candidate streaming merge; confidence-weighted RIS, bias handled
    // by the reference-shaped normalization below.
    RTXDI_GIReservoir selected = RTXDI_EmptyGIReservoir();
    float selectedTargetAtCurrent = 0.0;
    bool selectedHistory = false;
    float wSum = 0.0;

    if (canonicalValid && canonicalTarget > 0.0)
    {
        wSum += canonicalM * canonicalTarget * inputReservoir.weightSum;
        selected = inputReservoir;
        selectedTargetAtCurrent = canonicalTarget;
    }

    const float historyWeight =
        historyM * historyTargetAtCurrent * historyReservoir.weightSum;
    if (historyWeight > 0.0)
    {
        wSum += historyWeight;
        if (RTXDI_GetNextRandom(rng) * wSum <= historyWeight)
        {
            selected = historyReservoir;
            selectedTargetAtCurrent = historyTargetAtCurrent;
            selectedHistory = true;
        }
    }

    if (!(wSum > 0.0) || selectedTargetAtCurrent <= 0.0 || !RTXDI_IsValidGIReservoir(selected))
    {
        return inputReservoir;
    }

    const float mergedM = canonicalM + historyM;
    float normalizationNumerator = 1.0;
    float normalizationDenominator = selectedTargetAtCurrent * max(mergedM, 1.0);
    if (tparams.biasCorrectionMode >= 2u)
    {
        // Remix-shaped temporal MIS normalization. Evaluate the selected
        // sample in both the current and previous receiver domains, then use
        // the selected source-domain target as the numerator and the
        // confidence-weighted target sum as the denominator.
        const RAB_Surface historySurface = RAB_GetGBufferSurface(historyPixel, true);
        const float selectedTargetAtHistory = RAB_IsSurfaceValid(historySurface)
            ? RAB_GetGISampleTargetPdfForSurface(selected.position, selected.radiance, historySurface)
            : 0.0;
        const float piSum =
            selectedTargetAtCurrent * canonicalM +
            selectedTargetAtHistory * historyM;
        normalizationNumerator = selectedHistory
            ? selectedTargetAtHistory
            : selectedTargetAtCurrent;
        normalizationDenominator = piSum * selectedTargetAtCurrent;
        if (!(normalizationNumerator > 0.0) || !(normalizationDenominator > 0.0))
        {
            return inputReservoir;
        }
    }
    else if (tparams.biasCorrectionMode >= uint(RTXDI_BIAS_CORRECTION_BASIC))
    {
        // 1/Z: count each temporal domain that could have generated the
        // selected sample. The winning source domain is always counted; that
        // domain already proved it can produce the sample, so a failed
        // re-evaluation must not inflate W by dropping it from Z.
        float Z = 0.0;
        if (selectedHistory)
        {
            if (canonicalM > 0.0)
            {
                Z += canonicalM;
            }
            Z += historyM;
        }
        else
        {
            Z += canonicalM;
            const float jSelectedToHistory = RBPT_GITemporalReconnectionJacobian(
                receiverPos, historySurfacePos, selected);
            float selectedTargetAtHistory = 0.0;
            if (RBPT_GITemporalValidateJacobian(jSelectedToHistory))
            {
                const RAB_Surface historySurface = RAB_GetGBufferSurface(historyPixel, true);
                selectedTargetAtHistory = RAB_IsSurfaceValid(historySurface)
                    ? RAB_GetGISampleTargetPdfForSurface(selected.position, selected.radiance, historySurface)
                    : 0.0;
            }
            if (selectedTargetAtHistory > 0.0)
            {
                Z += historyM;
            }
        }
        if (!(Z > 0.0))
        {
            return inputReservoir;
        }
        normalizationDenominator = max(Z, 1.0) * selectedTargetAtCurrent;
    }

    RTXDI_GIReservoir result = selected;
    result.weightSum = (wSum * normalizationNumerator) / normalizationDenominator;
    if (!(result.weightSum > 0.0 && result.weightSum < 1.0e20))
    {
        return inputReservoir;
    }
    result.M = (uint)min(mergedM, 255.0); // packed-ABI confidence cap
    result.age = selectedHistory ? min(historyReservoir.age + 1u, 0xffu) : inputReservoir.age;
    return result;
}

#endif
