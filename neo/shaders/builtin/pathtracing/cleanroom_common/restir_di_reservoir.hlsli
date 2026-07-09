#ifndef RB_PATH_TRACING_CLEANROOM_COMMON_RESTIR_DI_RESERVOIR_HLSLI
#define RB_PATH_TRACING_CLEANROOM_COMMON_RESTIR_DI_RESERVOIR_HLSLI

#include "cleanroom_common/restir_di_parameters.hlsli"
#include "cleanroom_common/restir_shared_sampled_light_data.hlsli"
#include "Rtxdi/Utils/RandomSamplerState.hlsli"

// rbdoom-owned DI reservoir, replacing Rtxdi/DI/Reservoir.hlsli. The public
// surface (struct fields and RTXDI_* function names) is taken from this
// repo's call sites only; the weighted-reservoir math follows Bitterli et
// al. 2020 "Spatiotemporal reservoir resampling" (streaming RIS,
// W = wSum / (targetPdf * normalization)). The packed layout below is an
// rbdoom-defined ABI: it only has to round-trip through this header, since
// every producer and consumer of the packed buffers compiles against it.

#ifndef RTXDI_DIReservoir_LightValidBit
#define RTXDI_DIReservoir_LightValidBit RTXDI_LIGHT_DATA_VALID_BIT
#endif

#ifndef RTXDI_DIReservoir_LightIndexMask
#define RTXDI_DIReservoir_LightIndexMask RTXDI_LIGHT_DATA_INDEX_MASK
#endif

// Live-struct visibility encoding: 3x8-bit unorm in the low 24 bits.
// Call sites assign this mask directly to mean "fully visible".
#ifndef RTXDI_PackedDIReservoir_VisibilityMask
#define RTXDI_PackedDIReservoir_VisibilityMask 0x00ffffffu
#endif

// Largest confidence value the packed format can carry (16-bit M field).
#ifndef RTXDI_PackedDIReservoir_MaxM
#define RTXDI_PackedDIReservoir_MaxM 0xffffu
#endif

#ifndef RTXDI_DIRESERVOIR_MAX_M
#define RTXDI_DIRESERVOIR_MAX_M 65535.0
#endif

struct RTXDI_PackedDIReservoir
{
    uint lightData;
    uint uvData;
    uint mVisibility;
    uint distanceAge;
    float targetPdf;
    float weight;
};

struct RTXDI_DIReservoir
{
    uint lightData;          // RTXDI_DIReservoir_LightValidBit | light index
    uint uvData;             // 16.16 fixed-point sample UV
    float weightSum;         // running RIS weight sum; holds W after RTXDI_FinalizeResampling
    float targetPdf;         // target function value of the selected sample
    float M;                 // confidence / sample count
    uint packedVisibility;   // 3x8 unorm visibility, see VisibilityMask
    int2 spatialDistance;    // accumulated pixel offset of the selected sample's origin
    uint age;                // frames since the selected sample was generated
    float canonicalWeight;   // in-register only (pairwise MIS scratch); not persisted
};

struct RTXDI_VisibilityReuseParameters
{
    uint maxAge;
    float maxDistance;
};

RTXDI_DIReservoir RTXDI_EmptyDIReservoir()
{
    RTXDI_DIReservoir reservoir = (RTXDI_DIReservoir)0;
    return reservoir;
}

bool RBPT_DIFiniteFloat(float value)
{
    return value == value && value > -RTXDI_MAX_FLOAT32 && value < RTXDI_MAX_FLOAT32;
}

bool RBPT_DIFinitePositive(float value)
{
    return value > 0.0 && RBPT_DIFiniteFloat(value);
}

bool RTXDI_IsValidDIReservoir(RTXDI_DIReservoir reservoir)
{
    return (reservoir.lightData & RTXDI_DIReservoir_LightValidBit) != 0u &&
        RBPT_DIFinitePositive(reservoir.M) &&
        RBPT_DIFinitePositive(reservoir.targetPdf) &&
        RBPT_DIFinitePositive(reservoir.weightSum);
}

uint RTXDI_GetDIReservoirLightIndex(RTXDI_DIReservoir reservoir)
{
    return reservoir.lightData & RTXDI_DIReservoir_LightIndexMask;
}

uint RBPT_DIPackSampleUv(float2 uv)
{
    const uint2 quantized = uint2(round(saturate(uv) * 65535.0));
    return quantized.x | (quantized.y << 16);
}

float2 RTXDI_GetDIReservoirSampleUV(RTXDI_DIReservoir reservoir)
{
    return float2(
        (float)(reservoir.uvData & 0xffffu),
        (float)((reservoir.uvData >> 16) & 0xffffu)) / 65535.0;
}

// After RTXDI_FinalizeResampling, weightSum holds the unbiased contribution
// weight W (the inverse selection pdf of the surviving sample).
float RTXDI_GetDIReservoirInvPdf(RTXDI_DIReservoir reservoir)
{
    return reservoir.weightSum;
}

// Streaming RIS update (Bitterli 2020, Alg. 2): candidate resampling weight
// is targetPdf / sourcePdf. Returns true when the candidate replaces the
// current selection.
//
// Production note (black-noise): zero-target candidates must NOT inflate M or
// enter the reservoir. Counting them in M dilutes W = wSum/(p_hat*M) and lets
// empty/zero draws starve valid samples in temporal/spatial merges — classic
// "black noise on well-lit surfaces." Mild bias vs textbook RIS; required for
// solid 1-SPP DI look. Callers should still prefer skipping the call when
// targetPdf is known zero.
bool RTXDI_StreamSample(
    inout RTXDI_DIReservoir reservoir,
    uint lightIndex,
    float2 uv,
    float random,
    float targetPdf,
    float invSourcePdf)
{
    const float risWeight = max(targetPdf, 0.0) * max(invSourcePdf, 0.0);
    if (risWeight <= 0.0)
    {
        return false;
    }

    reservoir.M = min(reservoir.M + 1.0, RTXDI_DIRESERVOIR_MAX_M);
    reservoir.weightSum += risWeight;
    const bool selectSample = random * reservoir.weightSum <= risWeight;
    if (selectSample)
    {
        reservoir.lightData = RTXDI_DIReservoir_LightValidBit | (lightIndex & RTXDI_DIReservoir_LightIndexMask);
        reservoir.uvData = RBPT_DIPackSampleUv(uv);
        reservoir.targetPdf = targetPdf;
    }
    return selectSample;
}

// Generic streaming-RIS merge step (Bitterli 2020, Alg. 2/4): fold one
// candidate reservoir into the running one with an explicit resampling
// weight (targetPdf * sampleNormalization) and explicit confidence
// increment. Confidence is always accumulated, even for rejected
// candidates, so the accounting stays unbiased.
bool RTXDI_InternalSimpleResample(
    inout RTXDI_DIReservoir reservoir,
    RTXDI_DIReservoir newReservoir,
    float random,
    float targetPdf = 1.0,
    float sampleNormalization = 1.0,
    float sampleM = 1.0)
{
    reservoir.M = min(reservoir.M + max(sampleM, 0.0), RTXDI_DIRESERVOIR_MAX_M);

    const float risWeight = max(targetPdf, 0.0) * max(sampleNormalization, 0.0);
    if ((newReservoir.lightData & RTXDI_DIReservoir_LightValidBit) == 0u || risWeight <= 0.0)
    {
        return false;
    }

    reservoir.weightSum += risWeight;
    const bool selectCandidate = random * reservoir.weightSum <= risWeight;
    if (selectCandidate)
    {
        reservoir.lightData = newReservoir.lightData;
        reservoir.uvData = newReservoir.uvData;
        reservoir.targetPdf = targetPdf;
        reservoir.packedVisibility = newReservoir.packedVisibility;
        reservoir.spatialDistance = newReservoir.spatialDistance;
        reservoir.age = newReservoir.age;
    }
    return selectCandidate;
}

// Merge a finalized reservoir into a running one. The candidate's resampling
// weight is its target function value re-evaluated at the destination domain
// times its contribution weight W times its confidence M.
bool RTXDI_CombineDIReservoirs(
    inout RTXDI_DIReservoir reservoir,
    RTXDI_DIReservoir candidate,
    float random,
    float targetPdf)
{
    return RTXDI_InternalSimpleResample(
        reservoir,
        candidate,
        random,
        targetPdf,
        max(candidate.weightSum, 0.0) * max(candidate.M, 0.0),
        candidate.M);
}

// Convert the accumulated weight sum into the contribution weight
// W = wSum / targetPdf * (numerator / denominator). Callers pass
// (1, M) for the plain 1/M estimator or MIS-style numerator/denominator
// pairs for bias-corrected variants.
void RTXDI_FinalizeResampling(
    inout RTXDI_DIReservoir reservoir,
    float normalizationNumerator,
    float normalizationDenominator)
{
    if (!RBPT_DIFinitePositive(reservoir.targetPdf) ||
        !RBPT_DIFinitePositive(normalizationNumerator) ||
        !RBPT_DIFinitePositive(normalizationDenominator) ||
        !RBPT_DIFinitePositive(reservoir.weightSum))
    {
        reservoir.weightSum = 0.0;
        return;
    }

    const float finalizedWeight = (reservoir.weightSum * normalizationNumerator) /
        (reservoir.targetPdf * normalizationDenominator);
    reservoir.weightSum = RBPT_DIFinitePositive(finalizedWeight) ? finalizedWeight : 0.0;
}

uint RBPT_DIPackVisibility(float3 visibility)
{
    const uint3 quantized = uint3(round(saturate(visibility) * 255.0));
    return quantized.x | (quantized.y << 8) | (quantized.z << 16);
}

float3 RBPT_DIUnpackVisibility(uint packedVisibility)
{
    return float3(
        (float)(packedVisibility & 0xffu),
        (float)((packedVisibility >> 8) & 0xffu),
        (float)((packedVisibility >> 16) & 0xffu)) / 255.0;
}

// Record the visibility of the currently selected sample so later passes can
// reuse it instead of re-tracing. The sample was just shaded at this pixel,
// so the reuse provenance (distance/age) resets. With discardInvisibleSamples
// the fully occluded sample is dropped by zeroing its contribution weight.
void RTXDI_StoreVisibilityInDIReservoir(
    inout RTXDI_DIReservoir reservoir,
    float3 visibility,
    bool discardInvisibleSamples)
{
    reservoir.packedVisibility = RBPT_DIPackVisibility(visibility) & RTXDI_PackedDIReservoir_VisibilityMask;
    reservoir.spatialDistance = int2(0, 0);
    reservoir.age = 0u;
    if (discardInvisibleSamples && all(visibility <= float3(0.0, 0.0, 0.0)))
    {
        reservoir.weightSum = 0.0;
    }
}

// Stored visibility is reusable while the selected sample has not travelled
// too far (spatially or temporally) from where it was last shaded. A zero
// field means "nothing stored" - indistinguishable from stored-fully-occluded
// by design; that case only costs a redundant shadow ray, never bias.
bool RTXDI_GetDIReservoirVisibility(
    RTXDI_DIReservoir reservoir,
    RTXDI_VisibilityReuseParameters params,
    out float3 visibility)
{
    const uint storedBits = reservoir.packedVisibility & RTXDI_PackedDIReservoir_VisibilityMask;
    const float distance = max(abs((float)reservoir.spatialDistance.x), abs((float)reservoir.spatialDistance.y));
    if (storedBits == 0u || reservoir.age > params.maxAge || distance > params.maxDistance)
    {
        visibility = float3(0.0, 0.0, 0.0);
        return false;
    }

    visibility = RBPT_DIUnpackVisibility(storedBits);
    return true;
}

// Packed ABI (rbdoom-defined):
//   mVisibility  = M (16-bit uint, [15:0]) | visibility 3x5-bit unorm ([30:16])
//   distanceAge  = spatialDistance.x+128 ([7:0]) | .y+128 ([15:8]) | age ([31:16])
//   weight       = weightSum (raw float)
// canonicalWeight is per-pass scratch and intentionally not persisted; it
// unpacks to 1.0.

uint RBPT_DIPackVisibility24To15(uint packedVisibility)
{
    const uint r = (packedVisibility >> 3) & 0x1fu;
    const uint g = (packedVisibility >> 11) & 0x1fu;
    const uint b = (packedVisibility >> 19) & 0x1fu;
    return r | (g << 5) | (b << 10);
}

uint RBPT_DIUnpackVisibility15To24(uint packedVisibility15)
{
    const uint3 quantized5 = uint3(
        packedVisibility15 & 0x1fu,
        (packedVisibility15 >> 5) & 0x1fu,
        (packedVisibility15 >> 10) & 0x1fu);
    const uint3 quantized8 = uint3(round(float3(quantized5) * (255.0 / 31.0)));
    return quantized8.x | (quantized8.y << 8) | (quantized8.z << 16);
}

RTXDI_PackedDIReservoir RTXDI_PackDIReservoir(RTXDI_DIReservoir reservoir)
{
    RTXDI_PackedDIReservoir packedReservoir = (RTXDI_PackedDIReservoir)0;
    if (!RTXDI_IsValidDIReservoir(reservoir))
    {
        return packedReservoir;
    }

    packedReservoir.lightData = reservoir.lightData;
    packedReservoir.uvData = reservoir.uvData;
    packedReservoir.mVisibility =
        (uint(clamp(reservoir.M, 0.0, RTXDI_DIRESERVOIR_MAX_M)) & 0xffffu) |
        (RBPT_DIPackVisibility24To15(reservoir.packedVisibility & RTXDI_PackedDIReservoir_VisibilityMask) << 16);
    const uint2 biasedDistance = uint2(clamp(reservoir.spatialDistance, int2(-128, -128), int2(127, 127)) + int2(128, 128));
    packedReservoir.distanceAge =
        biasedDistance.x |
        (biasedDistance.y << 8) |
        (min(reservoir.age, 0xffffu) << 16);
    packedReservoir.targetPdf = reservoir.targetPdf;
    packedReservoir.weight = reservoir.weightSum;
    return packedReservoir;
}

RTXDI_DIReservoir RTXDI_UnpackDIReservoir(RTXDI_PackedDIReservoir packedReservoir)
{
    RTXDI_DIReservoir reservoir = RTXDI_EmptyDIReservoir();
    reservoir.lightData = packedReservoir.lightData;
    reservoir.uvData = packedReservoir.uvData;
    reservoir.M = (float)(packedReservoir.mVisibility & 0xffffu);
    reservoir.packedVisibility = RBPT_DIUnpackVisibility15To24((packedReservoir.mVisibility >> 16) & 0x7fffu);
    reservoir.spatialDistance = int2(
        (int)(packedReservoir.distanceAge & 0xffu) - 128,
        (int)((packedReservoir.distanceAge >> 8) & 0xffu) - 128);
    reservoir.age = (packedReservoir.distanceAge >> 16) & 0xffffu;
    reservoir.targetPdf = packedReservoir.targetPdf;
    reservoir.weightSum = packedReservoir.weight;
    reservoir.canonicalWeight = 1.0;
    if (!RTXDI_IsValidDIReservoir(reservoir))
    {
        return RTXDI_EmptyDIReservoir();
    }

    return reservoir;
}

#endif
