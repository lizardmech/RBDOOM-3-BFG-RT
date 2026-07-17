#ifndef RBPT_GI_SPATIAL_RESAMPLING_HLSLI
#define RBPT_GI_SPATIAL_RESAMPLING_HLSLI

#include "cleanroom_common/restir_gi_parameters.hlsli"

// rbdoom-owned replacement for Rtxdi/GI/SpatialResampling.hlsli.
// Clean-room sources:
//   - ReSTIR GI: Ouyang et al. 2021 (reconnection-shift jacobian, spatial
//     reservoir merge, 1/Z bias correction).
//   - GRIS framework: Lin et al. 2022 / Wyman et al. 2023 course notes
//     (shift-mapped resampling weights, MIS-weighted streaming RIS).
//   - Enhanced mode: Lin, Kettunen, Wyman 2026, "ReSTIR PT Enhanced"
//     (CC BY 4.0), Section 3: reciprocal neighbor selection via
//     Gaussian-permuted reuse textures. Pairwise MIS weights below are
//     re-derived from first principles; the normalization proof is in the
//     comment block at RBPT_GI_PAIRWISE_MIS.
// The NVIDIA header was not opened while writing this file. The public
// call-site contract (function/struct names, parameter fields) was taken
// from this repo's own shaders.
//
// Call-site compatibility (pathtrace_clean_restir_gi.rt.hlsl):
//   - #define RTXDI_NEIGHBOR_OFFSETS_BUFFER <float2 array> before include
//   - RTXDI_GISpatialResampling(pixel, surface, sourceReservoirIndex,
//         inputReservoir, rng, runtimeParams, reservoirParams, sparams)
//   - sparams fields: samplingRadius, numSamples, depthThreshold,
//         normalThreshold, biasCorrectionMode (+ new optional fields below)
//
// Required externally defined types/functions (all already rbdoom-owned or
// part of the parallel replacement work; override the macros if names move):
//   RTXDI_GIReservoir with fields: position, normal, radiance (float3),
//       weightSum (holds the unbiased contribution weight W of a finalized
//       reservoir), M (confidence), age
//   RTXDI_EmptyGIReservoir(), RTXDI_IsValidGIReservoir(r)
//   RAB_Surface, RAB_IsSurfaceValid, RAB_GetGBufferSurface(int2,bool),
//       RAB_GetSurfaceWorldPos, RAB_GetSurfaceNormal
//   RAB_LoadGIReservoir(int2 pixel, int page)
//   RTXDI_RandomSamplerState + RTXDI_GetNextRandom (replacements/
//       RandomSamplerState.hlsli)

// ---------------------------------------------------------------------------
// Adapter macros
// ---------------------------------------------------------------------------

#ifndef RBPT_GI_LOAD_RESERVOIR
#define RBPT_GI_LOAD_RESERVOIR(pixel, page) RAB_LoadGIReservoir(pixel, page)
#endif

#ifndef RBPT_GI_GET_SURFACE
#define RBPT_GI_GET_SURFACE(pixel) RAB_GetGBufferSurface(pixel, false)
#endif

#ifndef RBPT_GI_CLAMP_NEIGHBOR_PIXEL
#define RBPT_GI_CLAMP_NEIGHBOR_PIXEL(pixel) (pixel)
#endif

// Linear view depth used only for the relative-depth similarity gate.
// Default: distance from surface position to a camera origin the including
// shader must expose. Override with the lane's own accessor if one exists.
#ifndef RBPT_GI_SURFACE_LINEAR_DEPTH
#define RBPT_GI_SURFACE_LINEAR_DEPTH(surface) \
    length(RAB_GetSurfaceWorldPos(surface) - RBPT_GI_CAMERA_ORIGIN)
#endif

// Bias-correction mode constants. Keep numeric values aligned with the
// parameter-struct replacement (Step 5) and the existing cvar meaning.
#ifndef RTXDI_BIAS_CORRECTION_OFF
#define RTXDI_BIAS_CORRECTION_OFF        0
#define RTXDI_BIAS_CORRECTION_BASIC      1
#define RTXDI_BIAS_CORRECTION_PAIRWISE   2
#define RTXDI_BIAS_CORRECTION_RAY_TRACED 3
#endif

// ---------------------------------------------------------------------------
// Target function
// ---------------------------------------------------------------------------
// The GI lane's resampling target is the luminance of the cached incoming
// radiance (receiver-independent; geometry enters through the shift
// jacobian, not the target). Override to add e.g. a BRDF factor - if you
// do, the target becomes receiver-DEPENDENT and the pairwise MIS terms
// below already evaluate it per-receiver correctly via the macro.

#ifndef RBPT_GI_TARGET_LUMINANCE
#define RBPT_GI_TARGET_LUMINANCE(radiance) \
    dot(radiance, float3(0.2126, 0.7152, 0.0722))
#endif

#ifndef RBPT_GI_TARGET_PDF
#define RBPT_GI_TARGET_PDF(surface, reservoir) \
    RBPT_GI_TARGET_LUMINANCE((reservoir).radiance)
#endif

#ifndef RBPT_GI_VALIDATE_REUSE_SAMPLE
#define RBPT_GI_VALIDATE_REUSE_SAMPLE(pixel, neighborPixel, sampleIndex, surface, reservoir) true
#endif

// p-hat of a reservoir sample as seen from `surface`. With the default
// luminance target the surface argument is unused.
float RBPT_GITargetPdf(RAB_Surface surface, RTXDI_GIReservoir r)
{
    return RBPT_GI_TARGET_PDF(surface, r);
}

// ---------------------------------------------------------------------------
// Reconnection-shift jacobian (Ouyang et al. 2021, Eq. 11)
// ---------------------------------------------------------------------------
// A GI sample is a fixed secondary point x_s with normal n_s. Reusing it at
// a different receiver changes the solid-angle measure. The jacobian of the
// shift from receiver A's solid-angle domain to receiver B's is
//
//     J(A->B) = (|cos phi_B| / |cos phi_A|) * (d_A^2 / d_B^2)
//
// where phi_X is the angle at the SAMPLE between n_s and the direction
// toward receiver X, and d_X = ||x_s - x_X||.
//
// Returns 0 for invalid shifts (sample behind either receiver's hemisphere
// at the sample surface, degenerate distances, or J outside the cutoff).

float RBPT_GIReconnectionJacobian(
    float3 receiverFromPos,  // receiver A position (sample's original owner)
    float3 receiverToPos,    // receiver B position (sample's new owner)
    RTXDI_GIReservoir r,
    float jacobianCutoff)
{
    const float3 toFrom = receiverFromPos - r.position;
    const float3 toTo = receiverToPos - r.position;
    const float dFrom2 = dot(toFrom, toFrom);
    const float dTo2 = dot(toTo, toTo);
    if (dFrom2 <= 1.0e-12 || dTo2 <= 1.0e-12)
    {
        return 0.0;
    }

    // cos terms; rsqrt folds the normalization of toFrom/toTo.
    const float cosFrom = dot(r.normal, toFrom) * rsqrt(dFrom2);
    const float cosTo = dot(r.normal, toTo) * rsqrt(dTo2);

    // The sample must face both receivers; a sample on the back side of
    // its own surface relative to a receiver cannot have been generated
    // from there and cannot be reused there.
    if (cosFrom <= 1.0e-4 || cosTo <= 1.0e-4)
    {
        return 0.0;
    }

    const float jacobian = (cosTo / cosFrom) * (dFrom2 / dTo2);

    const float cutoff = (jacobianCutoff > 0.0) ? jacobianCutoff : 10.0;
    if (jacobian < 1.0 / cutoff || jacobian > cutoff)
    {
        return 0.0; // extreme measure change: reject rather than clamp
    }
    return jacobian;
}

// ---------------------------------------------------------------------------
// Similarity gates
// ---------------------------------------------------------------------------

bool RBPT_GISurfacesSimilar(
    RAB_Surface a, RAB_Surface b,
    float depthThreshold, float normalThreshold)
{
    const float depthA = RBPT_GI_SURFACE_LINEAR_DEPTH(a);
    const float depthB = RBPT_GI_SURFACE_LINEAR_DEPTH(b);
    if (abs(depthA - depthB) > depthThreshold * max(depthA, 1.0e-4))
    {
        return false;
    }
    if (dot(RAB_GetSurfaceNormal(a), RAB_GetSurfaceNormal(b)) < normalThreshold)
    {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Streaming reservoir update
// ---------------------------------------------------------------------------

struct RBPT_GIStream
{
    RTXDI_GIReservoir selected;
    float wSum;
    float M;
    float selectedTargetAtReceiver; // p-hat_q of the (shifted) selected sample
    int selectedSourceSlot;         // -1 = canonical/current, >=0 = neighbor
};

RBPT_GIStream RBPT_GIStreamInit()
{
    RBPT_GIStream s;
    s.selected = RTXDI_EmptyGIReservoir();
    s.wSum = 0.0;
    s.M = 0.0;
    s.selectedTargetAtReceiver = 0.0;
    s.selectedSourceSlot = -2;
    return s;
}

// w = full resampling weight of this candidate (MIS weight already applied
// by the caller). targetAtReceiver = p-hat_q of the candidate's (shifted)
// sample, WITHOUT jacobian; it is remembered for the selected candidate and
// divides wSum at finalize.
void RBPT_GIStreamUpdate(
    inout RBPT_GIStream s,
    RTXDI_GIReservoir candidate,
    float w,
    float targetAtReceiver,
    float confidence,
    int sourceSlot,
    inout RTXDI_RandomSamplerState rng)
{
    s.M += confidence;
    if (!(w > 0.0)) // also rejects NaN
    {
        return;
    }
    s.wSum += w;
    if (RTXDI_GetNextRandom(rng) * s.wSum <= w)
    {
        s.selected = candidate;
        s.selectedTargetAtReceiver = targetAtReceiver;
        s.selectedSourceSlot = sourceSlot;
    }
}

// ---------------------------------------------------------------------------
// Pairwise MIS weights (enhanced path)
// ---------------------------------------------------------------------------
// Construction (re-derived; equivalent in role to Lin et al. 2022's
// pairwise MIS): canonical technique c (the input reservoir, confidence
// M_c) and k neighbor techniques (confidence M_i). Using the balance
// heuristic within each (i, c) pair, with the canonical discounted k-fold
// because it appears in every pair:
//
//   m_i(x)  =        M_i p-hat_i(x) / (k M_i p-hat_i(x) + M_c p-hat_c(x))
//   m_c(x)  = (1/k) Sum_i [ M_c p-hat_c(x)
//                         / (k M_i p-hat_i(x) + M_c p-hat_c(x)) ]
//
// Normalization proof: for any x,
//   Sum_i m_i(x) + m_c(x)
//     = Sum_i [ (M_i p_i) / D_i ] + (1/k) Sum_i [ (M_c p_c) / D_i ]
//   with D_i = k M_i p_i + M_c p_c. Multiply the first sum's terms by k/k:
//     = (1/k) Sum_i [ (k M_i p_i + M_c p_c) / D_i ] = (1/k) * k = 1.   QED
//
// p-hat_j(x) means: target of sample x evaluated in technique j's domain,
// including the measure change, i.e. p-hat at j's receiver TIMES the
// jacobian of the shift into j's domain. With the luminance-only target
// the luminance cancels in each ratio and only confidences and jacobians
// remain - the code below keeps the general form so a BRDF-augmented
// target stays correct.
//
// Cost note: m_i needs the neighbor->canonical jacobian of the neighbor's
// sample, and m_c needs the canonical->neighbor jacobian of the canonical
// sample - the "two shifts per neighbor" of ReSTIR PT Enhanced Sec. 3.
// For GI's reconnection-only shift both are a few ALU ops, so the
// reciprocal pairing below buys symmetry/correlation quality more than
// raw speed (unlike full ReSTIR PT, where shifts trace rays).

// ---------------------------------------------------------------------------
// Neighbor selection
// ---------------------------------------------------------------------------

#ifdef RBPT_GI_SPATIAL_RECIPROCAL
// Enhanced neighbor selection: reciprocal reuse textures
// (ReSTIR PT Enhanced, Sec. 3). The integrating shader binds one int2
// delta texture per neighbor slot (different sizes to avoid tiling beats,
// e.g. 254/230/210/186) and provides:
//   #define RBPT_GI_REUSE_DELTA(slot, texel) (int2 delta lookup)
//   #define RBPT_GI_REUSE_SIZE(slot)         (uint, texture side length)
// plus a per-frame uint of random transform bits (from the C++ side):
//   bit 0: mirror x, bit 1: mirror y, bit 2: transpose,
//   bits 8..15 / 16..23: x / y offset (texels).
// A mirrored/transposed lookup must transform the DELTA too, or the
// pairing stops being self-inverting - handled here.
int2 RBPT_GIReuseNeighborDelta(uint slot, uint2 pixel, uint transformBits)
{
    const uint size = RBPT_GI_REUSE_SIZE(slot);
    const uint2 offset = uint2((transformBits >> 8) & 0xffu,
                               (transformBits >> 16) & 0xffu);
    uint2 texel = (pixel + offset) % size;
    if (transformBits & 1u) { texel.x = size - 1u - texel.x; }
    if (transformBits & 2u) { texel.y = size - 1u - texel.y; }
    if (transformBits & 4u) { texel = texel.yx; }

    int2 delta = RBPT_GI_REUSE_DELTA(slot, texel);

    if (transformBits & 4u) { delta = delta.yx; }
    if (transformBits & 2u) { delta.y = -delta.y; }
    if (transformBits & 1u) { delta.x = -delta.x; }
    return delta;
}
#endif

// Basic neighbor selection: random point from the precompiled disc offset
// sequence, scaled by the sampling radius (same data the DI lane uses).
int2 RBPT_GIDiscNeighborDelta(
    inout RTXDI_RandomSamplerState rng,
    float samplingRadius,
    uint neighborOffsetMask)
{
    const uint offsetCount = neighborOffsetMask + 1u;
    const uint index = min(uint(RTXDI_GetNextRandom(rng) * offsetCount),
                           neighborOffsetMask);
    const float2 offset = RTXDI_NEIGHBOR_OFFSETS_BUFFER[index] * samplingRadius;
    return int2(round(offset));
}

// ---------------------------------------------------------------------------
// Main entry point - signature-compatible with the call site
// ---------------------------------------------------------------------------

RTXDI_GIReservoir RTXDI_GISpatialResampling(
    uint2 pixel,
    RAB_Surface surface,
    uint sourceReservoirIndex,
    RTXDI_GIReservoir inputReservoir,
    inout RTXDI_RandomSamplerState rng,
    RTXDI_RuntimeParameters runtimeParams,
    RTXDI_ReservoirBufferParameters reservoirParams,
    RTXDI_GISpatialResamplingParameters sparams,
    out uint4 debugStats)
{
    debugStats = uint4(0u, 0u, 0u, 0u);
    const float3 receiverPos = RAB_GetSurfaceWorldPos(surface);
    const uint k = max(sparams.numSamples, 1u);

    const bool canonicalValid = RTXDI_IsValidGIReservoir(inputReservoir);
    const float canonicalTarget = canonicalValid
        ? RBPT_GITargetPdf(surface, inputReservoir) : 0.0;
    const float Mc = max(inputReservoir.M, canonicalValid ? 1.0 : 0.0);

    const bool pairwise =
        sparams.biasCorrectionMode == RTXDI_BIAS_CORRECTION_PAIRWISE;
    const float centralTechniqueWeight = pairwise
        ? max(sparams.pairwiseCentralWeight, 0.01)
        : 1.0;
    const float pairwiseCentralM = Mc * centralTechniqueWeight;

    RBPT_GIStream stream = RBPT_GIStreamInit();

    // Mature history uses one neighbor, so sharing only the neighbor-location
    // stream over a 4x4 tile lets a whole small region replace a coherent bad
    // temporal sample consistently. Winner selection remains per pixel.
    RTXDI_RandomSamplerState neighborRng = rng;
    if (sparams.sharedTilePattern != 0u &&
        inputReservoir.M >= max(sparams.fastHistoryLength, 1u))
    {
        neighborRng = RTXDI_InitRandomSamplerForPass(
            pixel / 4u,
            runtimeParams.frameIndex,
            0x52525821u,
            0u);
        neighborRng.useBlueNoise = rng.useBlueNoise;
    }

    // Pass 1 over neighbors: gather valid candidates and (for pairwise)
    // accumulate the canonical MIS weight terms.
    float canonicalMis = 1.0; // non-pairwise modes use post-hoc Z instead
    if (pairwise)
    {
        canonicalMis = 0.0;
    }

    // For BASIC (1/Z) we need each accepted participant's data again after
    // selection. k is small (<= 4 in this lane); keep fixed arrays.
    #define RBPT_GI_MAX_SPATIAL_SAMPLES 8
    float  neighborM[RBPT_GI_MAX_SPATIAL_SAMPLES];
    float3 neighborPos[RBPT_GI_MAX_SPATIAL_SAMPLES];
    int2   neighborPixels[RBPT_GI_MAX_SPATIAL_SAMPLES];
    uint   acceptedCount = 0;
    uint   similarCount = 0;
    uint   pairsSeen = 0; // pairwise: neighbor techniques that exist at all

    for (uint i = 0; i < k && i < RBPT_GI_MAX_SPATIAL_SAMPLES; ++i)
    {
#ifdef RBPT_GI_SPATIAL_RECIPROCAL
        const int2 delta = RBPT_GIReuseNeighborDelta(
            i, pixel, RBPT_GI_REUSE_TRANSFORM_BITS);
#else
        const int2 delta = RBPT_GIDiscNeighborDelta(
            neighborRng, sparams.samplingRadius, runtimeParams.neighborOffsetMask);
#endif
        const int2 neighborPixel = RBPT_GI_CLAMP_NEIGHBOR_PIXEL(int2(pixel) + delta);
        if (all(neighborPixel == int2(pixel)))
        {
            continue;
        }

        const RAB_Surface neighborSurface = RBPT_GI_GET_SURFACE(neighborPixel);
        if (!RAB_IsSurfaceValid(neighborSurface))
        {
            continue;
        }
        if (!RBPT_GISurfacesSimilar(surface, neighborSurface,
                sparams.depthThreshold, sparams.normalThreshold))
        {
            continue;
        }
        similarCount++;

        const RTXDI_GIReservoir neighbor = RBPT_GI_LOAD_RESERVOIR(
            neighborPixel, int(sourceReservoirIndex));

        const float3 neighborSurfacePos =
            RAB_GetSurfaceWorldPos(neighborSurface);
        const float Mi = max(neighbor.M, 0.0);

        // Pairwise canonical term for this pair needs the jacobian of the
        // CANONICAL sample shifted into the neighbor's domain - defined
        // even when the neighbor's own reservoir is empty/invalid.
        if (pairwise && canonicalValid)
        {
            pairsSeen++;
            float targetCanAtNeighbor = 0.0;
            if (Mi > 0.0)
            {
                const float jCanToNeighbor = RBPT_GIReconnectionJacobian(
                    receiverPos, neighborSurfacePos, inputReservoir,
                    sparams.jacobianCutoff);
                targetCanAtNeighbor =
                    RBPT_GITargetPdf(neighborSurface, inputReservoir) *
                    jCanToNeighbor;
            }
            const float denom =
                k * Mi * targetCanAtNeighbor + pairwiseCentralM * canonicalTarget;
            // Neighbor cannot represent the canonical sample (denominator
            // collapses to the canonical term): full pair weight to c.
            canonicalMis += (denom > 0.0)
                ? (pairwiseCentralM * canonicalTarget) / denom
                : 1.0;
        }

        if (!RTXDI_IsValidGIReservoir(neighbor) || Mi <= 0.0)
        {
            continue;
        }

        // Shift the neighbor's sample into the current domain.
        const float jNeighborToCan = RBPT_GIReconnectionJacobian(
            neighborSurfacePos, receiverPos, neighbor,
            sparams.jacobianCutoff);
        if (jNeighborToCan == 0.0)
        {
            continue;
        }

        // p-hat_q of the shifted sample (no jacobian - the jacobian enters
        // the resampling weight, not the target value).
        const float targetAtReceiver = RBPT_GITargetPdf(surface, neighbor);
        if (!(targetAtReceiver > 0.0))
        {
            continue;
        }
        if (!RBPT_GI_VALIDATE_REUSE_SAMPLE(pixel, neighborPixel, i, surface, neighbor))
        {
            continue;
        }

        float mis;
        if (pairwise)
        {
            // m_i = M_i p_i(s_i) / (k M_i p_i(s_i) + M_c p_c(s_i))
            // p_i(s_i): target in the neighbor's own domain = neighbor-side
            // p-hat, no jacobian. p_c(s_i): target in the canonical domain =
            // receiver-side p-hat times the i->c jacobian.
            const float pOwn =
                RBPT_GITargetPdf(neighborSurface, neighbor);
            const float pCan = targetAtReceiver * jNeighborToCan;
            const float denom = k * Mi * pOwn + pairwiseCentralM * pCan;
            mis = (denom > 0.0) ? (Mi * pOwn) / denom : 0.0;
        }
        else
        {
            // Confidence-weighted RIS; bias handled by 1/M or 1/Z below.
            mis = Mi;
        }

        // GRIS candidate weight: m * p-hat_q(T(s)) * |J| * W.
        const float w =
            mis * targetAtReceiver * jNeighborToCan * neighbor.weightSum;

        RBPT_GIStreamUpdate(stream, neighbor, w, targetAtReceiver, Mi,
                            int(acceptedCount), rng);

        neighborM[acceptedCount] = Mi;
        neighborPos[acceptedCount] = neighborSurfacePos;
        neighborPixels[acceptedCount] = neighborPixel;
        acceptedCount++;
    }

    // Canonical candidate.
    if (canonicalValid)
    {
        float mis;
        if (pairwise)
        {
            // Average the accumulated pair terms. Pairs whose neighbor
            // never existed (clamped to self, invalid surface, failed
            // similarity gates) have no technique that could claim any
            // sample, so their pair weight for the canonical is exactly 1;
            // contributing 0 instead would underweight the canonical and
            // visibly dim pixels with few valid neighbors.
            mis = (canonicalMis + float(k - pairsSeen)) / float(k);
        }
        else
        {
            mis = Mc;
        }
        const float w = mis * canonicalTarget * inputReservoir.weightSum;
        RBPT_GIStreamUpdate(stream, inputReservoir, w, canonicalTarget,
                            Mc, -1, rng);
    }

    if (!(stream.wSum > 0.0) || stream.selectedTargetAtReceiver <= 0.0 ||
        !RTXDI_IsValidGIReservoir(stream.selected))
    {
        // Nothing usable: pass the input through unchanged.
        debugStats = uint4(similarCount, acceptedCount, 0u, k);
        return inputReservoir;
    }

    // Finalize: W_out = wSum / (normalization * p-hat_q(selected)).
    float normalization;
    if (pairwise)
    {
        // MIS weights already sum to 1 across candidates.
        normalization = 1.0;
    }
    else if (sparams.biasCorrectionMode >= RTXDI_BIAS_CORRECTION_BASIC)
    {
        // 1/Z (Ouyang et al. 2021): count the confidence of every
        // participant whose domain could have produced the selected sample.
        // The winning source domain is always counted; that domain already
        // proved it can produce the sample, so a failed re-evaluation must not
        // inflate W by dropping it from Z.
        float Z = canonicalValid ? Mc : 0.0;
        for (uint n = 0; n < acceptedCount; ++n)
        {
            if (stream.selectedSourceSlot == int(n))
            {
                Z += neighborM[n];
                continue;
            }

            const float jSelToNeighbor = RBPT_GIReconnectionJacobian(
                receiverPos, neighborPos[n], stream.selected,
                sparams.jacobianCutoff);
            const RAB_Surface neighborSurface = RBPT_GI_GET_SURFACE(
                neighborPixels[n]);
            if (jSelToNeighbor > 0.0 &&
                RBPT_GITargetPdf(neighborSurface, stream.selected) > 0.0)
            {
                Z += neighborM[n];
            }
        }
        normalization = max(Z, 1.0);
    }
    else
    {
        // 1/M: cheapest, biased where supports differ.
        normalization = max(stream.M, 1.0);
    }

    RTXDI_GIReservoir result = stream.selected;
    result.weightSum =
        stream.wSum / (normalization * stream.selectedTargetAtReceiver);
    result.M = min(stream.M, 65504.0); // fp16-safe confidence cap upstream of
                                       // the history clamp applied elsewhere
    debugStats = uint4(
        similarCount,
        acceptedCount,
        stream.selectedSourceSlot >= 0 ? 1u : 0u,
        k);
    return result;
}

#endif // RBPT_GI_SPATIAL_RESAMPLING_HLSLI
