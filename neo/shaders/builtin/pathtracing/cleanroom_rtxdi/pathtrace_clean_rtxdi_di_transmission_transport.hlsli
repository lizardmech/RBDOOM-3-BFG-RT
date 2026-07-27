#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_TRANSMISSION_TRANSPORT_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_TRANSMISSION_TRANSPORT_HLSLI

#if defined(CLEAN_RTXDI_DI_TRANSMISSION_PSR_TRANSPORT)

static const uint CLEAN_RTXDI_DI_TRANSMISSION_GUIDE_POLICY_THIN_STRAIGHT = 0u;
static const uint CLEAN_RTXDI_DI_TRANSMISSION_GUIDE_POLICY_STRONG_REFRACTION = 1u;

struct PathTraceCleanRtxdiDiTransmissionPsrSample
{
    bool performPsr;
    bool useAlternateDisocclusionThreshold;
    bool penetrateSurface;
    float3 inputDirection;
    float3 attenuation;
    float distortionWeight;
    uint guidePolicy;
};

PathTraceCleanRtxdiDiTransmissionPsrSample PathTraceCleanRtxdiDiTransmissionPsrSampleEmpty()
{
    PathTraceCleanRtxdiDiTransmissionPsrSample sample;
    sample.performPsr = false;
    sample.useAlternateDisocclusionThreshold = false;
    sample.penetrateSurface = false;
    sample.inputDirection = float3(0.0, 0.0, 0.0);
    sample.attenuation = float3(1.0, 1.0, 1.0);
    sample.distortionWeight = 0.0;
    sample.guidePolicy = CLEAN_RTXDI_DI_TRANSMISSION_GUIDE_POLICY_THIN_STRAIGHT;
    return sample;
}

PathTraceCleanRtxdiDiTransmissionPsrSample PathTraceCleanRtxdiDiTransmissionPsrSampleThinStraight(
    RAB_Surface surface,
    PathTraceCleanRtxdiDiGlassThinPayload glassPayload)
{
    PathTraceCleanRtxdiDiTransmissionPsrSample sample = PathTraceCleanRtxdiDiTransmissionPsrSampleEmpty();
    const float3 normal = RAB_SafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    const float3 viewDirection = RAB_SafeNormalize(RAB_GetSurfaceViewDir(surface), normal);
    sample.performPsr = true;
    sample.penetrateSurface = true;
    sample.inputDirection = -viewDirection;
    sample.attenuation = saturate(glassPayload.transmission);
    sample.distortionWeight = 0.0;
    sample.guidePolicy = CLEAN_RTXDI_DI_TRANSMISSION_GUIDE_POLICY_THIN_STRAIGHT;
    return sample;
}

PathTraceCleanRtxdiDiTransmissionPsrSample PathTraceCleanRtxdiDiTransmissionPsrSampleRefracted(
    RAB_Surface surface,
    PathTraceCleanRtxdiDiGlassMaterialParams materialParams,
    PathTraceCleanRtxdiDiGlassThinPayload glassPayload)
{
    PathTraceCleanRtxdiDiTransmissionPsrSample sample = PathTraceCleanRtxdiDiTransmissionPsrSampleEmpty();
    const float3 normal = RAB_SafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    const float3 viewDirection = RAB_SafeNormalize(RAB_GetSurfaceViewDir(surface), normal);
    const float3 faceForwardNormal = dot(normal, viewDirection) >= 0.0 ? normal : -normal;
    const float strength = saturate(CleanRtxdiDiToyPathInfo.x);
    if (strength <= 1.0e-4)
    {
        return PathTraceCleanRtxdiDiTransmissionPsrSampleThinStraight(surface, glassPayload);
    }

    const float3 straightDirection = -viewDirection;
    const float eta = rcp(max(materialParams.ior, 1.0001));
    const float3 refractedDirection = refract(straightDirection, faceForwardNormal, eta);
    if (dot(refractedDirection, refractedDirection) <= 1.0e-8 ||
        dot(refractedDirection, straightDirection) <= 0.0)
    {
        return sample;
    }

    sample.performPsr = true;
    sample.useAlternateDisocclusionThreshold = true;
    sample.penetrateSurface = true;
    sample.inputDirection = RAB_SafeNormalize(
        lerp(straightDirection, RAB_SafeNormalize(refractedDirection, straightDirection), strength),
        straightDirection);
    sample.attenuation = saturate(glassPayload.transmission);
    sample.distortionWeight = saturate(1.0 - abs(dot(normal, viewDirection)));
    sample.guidePolicy = CLEAN_RTXDI_DI_TRANSMISSION_GUIDE_POLICY_STRONG_REFRACTION;
    return sample;
}

PathTraceCleanRtxdiPayload PathTraceCleanRtxdiDiEmptyTransmissionTracePayload(RAB_Surface surface)
{
    PathTraceCleanRtxdiPayload payload = (PathTraceCleanRtxdiPayload)0;
    payload.value = 0u;
    payload.rayMode = 3u;
    payload.ignoreInstanceId = surface.instanceId;
    payload.ignorePrimitiveIndex = surface.primitiveIndex;
    payload.ignoreMaterialIndex = surface.materialIndex;
    payload.hitInstanceId = 0xffffffffu;
    payload.hitPrimitiveIndex = 0xffffffffu;
    payload.hitMaterialId = 0xffffffffu;
    payload.hitMaterialIndex = 0xffffffffu;
    payload.hitTriangleClassAndFlags = 0u;
    payload.hitT = 0.0;
    payload.hitBarycentrics = float2(0.0, 0.0);
    payload.passthroughEmissiveRadiance = float3(0.0, 0.0, 0.0);
    payload.liquidStatusMask = PathTraceLiquidPoolControlInitialStatus(
        PathTraceCleanRtxdiDiLiquidPoolControlFlags(),
        PathTraceCleanRtxdiDiLiquidPoolDebug(),
        PathTraceCleanRtxdiDiLiquidPoolPage());
    return payload;
}

bool PathTraceCleanRtxdiDiTraceTransmissionHit(
    RAB_Surface surface,
    PathTraceCleanRtxdiDiTransmissionPsrSample sample,
    out PathTraceCleanRtxdiPayload hitPayload,
    out float3 hitPosition,
    out float3 rayDirection)
{
    hitPayload = PathTraceCleanRtxdiDiEmptyTransmissionTracePayload(surface);
    hitPosition = RAB_GetSurfaceWorldPos(surface);
    rayDirection = RAB_SafeNormalize(sample.inputDirection, -RAB_GetSurfaceViewDir(surface));
    if (!sample.performPsr || dot(rayDirection, rayDirection) <= 1.0e-8)
    {
        return false;
    }

    RayDesc ray;
    ray.Origin = RAB_GetSurfaceWorldPos(surface) + rayDirection * 0.05;
    ray.Direction = rayDirection;
    ray.TMin = 0.01;
    ray.TMax = 100000.0;
    // Force non-opaque so the rayMode 3 anyhit filter always runs; it skips the
    // source pane and transparent-carded glass so the ray reaches the backdrop.
    TraceRay(SmokeScene, RAY_FLAG_FORCE_NON_OPAQUE, 0xff, 0, 0, 0, ray, hitPayload);
    hitPosition = ray.Origin + rayDirection * hitPayload.hitT;
    return hitPayload.value != 0u && hitPayload.hitMaterialIndex < (uint)TextureInfo.z;
}

// Reflection PSR candidate (step 3): transport-only decision, no shading.
struct PathTraceCleanRtxdiDiReflectionPsrCandidate
{
    bool valid;
    uint rejectReason;
    float3 direction;
    float3 throughput;
    float3 faceForwardNormal;
};

PathTraceCleanRtxdiDiReflectionPsrCandidate PathTraceCleanRtxdiDiReflectionPsrCandidateEmpty()
{
    PathTraceCleanRtxdiDiReflectionPsrCandidate candidate;
    candidate.valid = false;
    candidate.rejectReason = RT_CLEAN_RTXDI_DI_REFLECTION_PSR_REJECT_INVALID_DIRECTION;
    candidate.direction = float3(0.0, 0.0, 0.0);
    candidate.throughput = float3(0.0, 0.0, 0.0);
    candidate.faceForwardNormal = float3(0.0, 0.0, 1.0);
    return candidate;
}

PathTraceCleanRtxdiDiReflectionPsrCandidate PathTraceCleanRtxdiDiBuildReflectionPsrCandidate(
    RAB_Surface surface,
    PathTraceCleanRtxdiDiGlassThinPayload glassPayload)
{
    PathTraceCleanRtxdiDiReflectionPsrCandidate candidate =
        PathTraceCleanRtxdiDiReflectionPsrCandidateEmpty();
    const float3 normal = RAB_SafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    const float3 viewDirection = RAB_SafeNormalize(RAB_GetSurfaceViewDir(surface), normal);
    const float3 faceForwardNormal = dot(normal, viewDirection) >= 0.0 ? normal : -normal;
    candidate.faceForwardNormal = faceForwardNormal;
    candidate.throughput = max(glassPayload.reflection, float3(0.0, 0.0, 0.0));

    // Reject when reflection energy is effectively zero (transmission-only glass).
    if (PathTraceCleanRoomLuminance(candidate.throughput) <= 1.0e-5)
    {
        candidate.rejectReason = RT_CLEAN_RTXDI_DI_REFLECTION_PSR_REJECT_ZERO_THROUGHPUT;
        return candidate;
    }

    const float3 reflectedDirection = reflect(-viewDirection, faceForwardNormal);
    if (dot(reflectedDirection, reflectedDirection) <= 1.0e-8)
    {
        candidate.rejectReason = RT_CLEAN_RTXDI_DI_REFLECTION_PSR_REJECT_INVALID_DIRECTION;
        return candidate;
    }

    const float3 safeDirection = RAB_SafeNormalize(reflectedDirection, faceForwardNormal);
    // Must leave the front of the pane (same side the viewer sees).
    if (dot(safeDirection, faceForwardNormal) <= 1.0e-4)
    {
        candidate.rejectReason = RT_CLEAN_RTXDI_DI_REFLECTION_PSR_REJECT_INVALID_DIRECTION;
        return candidate;
    }

    // Origin offset used by the mirror trace must remain finite/non-zero.
    const float3 originOffset = safeDirection * 0.05;
    if (dot(originOffset, originOffset) <= 1.0e-12)
    {
        candidate.rejectReason = RT_CLEAN_RTXDI_DI_REFLECTION_PSR_REJECT_INVALID_DIRECTION;
        return candidate;
    }

    candidate.valid = true;
    candidate.rejectReason = RT_CLEAN_RTXDI_DI_REFLECTION_PSR_REJECT_NONE;
    candidate.direction = safeDirection;
    return candidate;
}

// Glass PSR lane selection (steps 4 + 7b).
//
// Policy (Phase A / 7b): deterministic energy ownership + temporal stickiness.
// Beauty and RR guides share the same surface identity so DLSS-RR sees stable
// material albedo/normal/depth instead of a per-frame coin flip.
//
//   1. Extreme Fresnel energy forces reflection or transmission.
//   2. Otherwise stick to the previous frame's glass PSR lane when still valid.
//   3. Otherwise pick the higher-energy lobe (pReflection >= 0.5).
//
// selectedThroughput is the raw deterministic/sticky selected-lobe weight.
struct PathTraceCleanRtxdiDiReflectionPsrSelection
{
    bool reflectionSelected;
    bool transmissionSelected;
    bool failClosed;
    bool laneChanged;
    bool sticky;
    float pReflection;
    float pTransmission;
    float3 selectedThroughput;
    float3 reflectionThroughput;
    float3 transmissionThroughput;
};

PathTraceCleanRtxdiDiReflectionPsrSelection PathTraceCleanRtxdiDiReflectionPsrSelectionFailClosed()
{
    PathTraceCleanRtxdiDiReflectionPsrSelection selection;
    selection.reflectionSelected = false;
    selection.transmissionSelected = false;
    selection.failClosed = true;
    selection.laneChanged = true;
    selection.sticky = false;
    selection.pReflection = 0.0;
    selection.pTransmission = 0.0;
    selection.selectedThroughput = float3(0.0, 0.0, 0.0);
    selection.reflectionThroughput = float3(0.0, 0.0, 0.0);
    selection.transmissionThroughput = float3(0.0, 0.0, 0.0);
    return selection;
}

PathTraceCleanRtxdiDiReflectionPsrSelection PathTraceCleanRtxdiDiSelectReflectionPsrLane(
    bool reflectionValid,
    bool transmissionValid,
    float3 reflectionThroughput,
    float3 transmissionThroughput,
    bool previousReflectionLane,
    bool previousTransmissionLane,
    bool previousGlassPsrValid)
{
    PathTraceCleanRtxdiDiReflectionPsrSelection selection =
        PathTraceCleanRtxdiDiReflectionPsrSelectionFailClosed();
    selection.reflectionThroughput = max(reflectionThroughput, float3(0.0, 0.0, 0.0));
    selection.transmissionThroughput = max(transmissionThroughput, float3(0.0, 0.0, 0.0));
    selection.failClosed = false;
    selection.laneChanged = true;
    selection.sticky = false;

    if (!reflectionValid && !transmissionValid)
    {
        selection.failClosed = true;
        return selection;
    }

    if (reflectionValid && !transmissionValid)
    {
        selection.reflectionSelected = true;
        selection.pReflection = 1.0;
        selection.pTransmission = 0.0;
        selection.selectedThroughput = selection.reflectionThroughput;
        selection.laneChanged = !(previousGlassPsrValid && previousReflectionLane);
        selection.sticky = previousGlassPsrValid && previousReflectionLane;
        return selection;
    }

    if (!reflectionValid && transmissionValid)
    {
        selection.transmissionSelected = true;
        selection.pReflection = 0.0;
        selection.pTransmission = 1.0;
        selection.selectedThroughput = selection.transmissionThroughput;
        selection.laneChanged = !(previousGlassPsrValid && previousTransmissionLane);
        selection.sticky = previousGlassPsrValid && previousTransmissionLane;
        return selection;
    }

    const float reflectionLuma = PathTraceCleanRoomLuminance(selection.reflectionThroughput);
    const float transmissionLuma = PathTraceCleanRoomLuminance(selection.transmissionThroughput);
    const float denom = max(reflectionLuma + transmissionLuma, 1.0e-5);
    const float pReflection = clamp(reflectionLuma / denom, 0.0, 1.0);
    selection.pReflection = pReflection;
    selection.pTransmission = 1.0 - pReflection;

    // Strong Fresnel dominance: force the high-energy lobe (stable, primary-like).
    const float kForceReflection = 0.85;
    const float kForceTransmission = 0.15;
    bool chooseReflection = pReflection >= 0.5;
    bool sticky = false;

    if (pReflection >= kForceReflection)
    {
        chooseReflection = true;
    }
    else if (pReflection <= kForceTransmission)
    {
        chooseReflection = false;
    }
    else if (previousGlassPsrValid && previousReflectionLane && reflectionValid)
    {
        chooseReflection = true;
        sticky = true;
    }
    else if (previousGlassPsrValid && previousTransmissionLane && transmissionValid)
    {
        chooseReflection = false;
        sticky = true;
    }

    if (chooseReflection)
    {
        selection.reflectionSelected = true;
        selection.selectedThroughput = selection.reflectionThroughput;
        selection.laneChanged = !(previousGlassPsrValid && previousReflectionLane);
        selection.sticky = sticky && previousReflectionLane;
    }
    else
    {
        selection.transmissionSelected = true;
        selection.selectedThroughput = selection.transmissionThroughput;
        selection.laneChanged = !(previousGlassPsrValid && previousTransmissionLane);
        selection.sticky = sticky && previousTransmissionLane;
    }
    return selection;
}

// Reflection trace and bounded analytic shade service.
#include "pathtrace_reflection_secondary.hlsli"

#endif

#endif
