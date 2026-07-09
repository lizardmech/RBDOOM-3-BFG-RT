#ifndef RB_PATH_TRACE_REFLECTION_SECONDARY_HLSLI
#define RB_PATH_TRACE_REFLECTION_SECONDARY_HLSLI

// Shared reflection secondary service (dedicated_reflection_secondary_steps.txt).
//
// Jobs:
//   Trace a simplified reflection ray (Dirac glass / high-gloss callers).
//   Shade the hit with direct lights + emissive (not full clean DI / ReSTIR-PT).
//
// Callers multiply by Fresnel / specular weight. This module does not own
// glass Fresnel, PSR lane select, or RR guide policy.
//
// Phase 0: extracted from pathtrace_clean_rtxdi_di_transmission_transport.hlsli
// with behavior-preserving wrappers under the old PathTraceCleanRtxdiDi* names.

#if defined(CLEAN_RTXDI_DI_TRANSMISSION_PSR_TRANSPORT) || defined(CLEAN_RTXDI_DI_REFLECTION_SECONDARY)

// ---------------------------------------------------------------------------
// Budget / result contract
// ---------------------------------------------------------------------------

// Budget for simplified secondary shade. Host packs sample count in
// CleanRtxdiDiMotionVectorInfo.y (1-8). Shadows off via
// CLEAN_FLAG_REFLECTION_SECONDARY_NO_SHADOWS.
struct PathTraceReflectionSecondaryBudget
{
    uint maxAnalyticSamples;
    uint enableShadows;
};

PathTraceReflectionSecondaryBudget PathTraceReflectionSecondaryBudgetFromConstants()
{
    PathTraceReflectionSecondaryBudget budget;
    budget.maxAnalyticSamples =
        clamp((uint)max(CleanRtxdiDiMotionVectorInfo.y, 1.0), 1u, 8u);
    budget.enableShadows =
        ((CleanRtxdiDiFlags & CLEAN_FLAG_REFLECTION_SECONDARY_NO_SHADOWS) == 0u) ? 1u : 0u;
    return budget;
}

PathTraceReflectionSecondaryBudget PathTraceReflectionSecondaryBudgetDefault()
{
    return PathTraceReflectionSecondaryBudgetFromConstants();
}

struct PathTraceReflectionSecondaryHit
{
    bool valid;
    float hitT;
    float3 hitPosition;
    float3 rayDirection;
    PathTraceCleanRtxdiPayload payload;
};

PathTraceReflectionSecondaryHit PathTraceReflectionSecondaryHitEmpty()
{
    PathTraceReflectionSecondaryHit hit;
    hit.valid = false;
    hit.hitT = 0.0;
    hit.hitPosition = float3(0.0, 0.0, 0.0);
    hit.rayDirection = float3(0.0, 0.0, 0.0);
    // payload filled by Trace via EmptyTransmissionTracePayload + TraceRay.
    return hit;
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

// Reflection ray from a glass (or other) surface using a prepared candidate
// direction. Same pane-skip / FORCE_NON_OPAQUE policy as transmission PSR.
bool PathTraceReflectionSecondaryTrace(
    RAB_Surface surface,
    PathTraceCleanRtxdiDiReflectionPsrCandidate candidate,
    out PathTraceReflectionSecondaryHit hit)
{
    hit = PathTraceReflectionSecondaryHitEmpty();
    PathTraceCleanRtxdiPayload hitPayload =
        PathTraceCleanRtxdiDiEmptyTransmissionTracePayload(surface);
    hit.hitPosition = RAB_GetSurfaceWorldPos(surface);
    if (!candidate.valid || dot(candidate.direction, candidate.direction) <= 1.0e-8)
    {
        hit.payload = hitPayload;
        return false;
    }

    hit.rayDirection = RAB_SafeNormalize(candidate.direction, candidate.faceForwardNormal);
    if (dot(hit.rayDirection, hit.rayDirection) <= 1.0e-8)
    {
        hit.payload = hitPayload;
        return false;
    }

    RayDesc ray;
    ray.Origin = RAB_GetSurfaceWorldPos(surface) + hit.rayDirection * 0.05;
    ray.Direction = hit.rayDirection;
    ray.TMin = 0.01;
    ray.TMax = 100000.0;
    // TraceRay payload must be a local; nested struct members can ICE DXC.
    TraceRay(SmokeScene, RAY_FLAG_FORCE_NON_OPAQUE, 0xff, 0, 1, 0, ray, hitPayload);
    hit.payload = hitPayload;
    hit.hitPosition = ray.Origin + hit.rayDirection * hitPayload.hitT;
    hit.hitT = hitPayload.hitT;
    hit.valid =
        hitPayload.value != 0u &&
        hitPayload.hitMaterialIndex < (uint)TextureInfo.z;
    return hit.valid;
}

// Build a mirror candidate from the surface view and trace (glass Option B /
// secondary path without a pre-built PSR candidate).
bool PathTraceReflectionSecondaryTraceMirrorFromSurface(
    RAB_Surface surface,
    out PathTraceReflectionSecondaryHit hit)
{
    const float3 normal =
        RAB_SafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    const float3 viewDirection = RAB_SafeNormalize(RAB_GetSurfaceViewDir(surface), normal);
    const float3 faceForwardNormal = dot(normal, viewDirection) >= 0.0 ? normal : -normal;
    PathTraceCleanRtxdiDiReflectionPsrCandidate candidate =
        PathTraceCleanRtxdiDiReflectionPsrCandidateEmpty();
    candidate.valid = true;
    candidate.rejectReason = RT_CLEAN_RTXDI_DI_REFLECTION_PSR_REJECT_NONE;
    candidate.direction = reflect(-viewDirection, faceForwardNormal);
    candidate.faceForwardNormal = faceForwardNormal;
    candidate.throughput = float3(1.0, 1.0, 1.0);
    return PathTraceReflectionSecondaryTrace(surface, candidate, hit);
}

// ---------------------------------------------------------------------------
// Simplified direct shade (not full clean DI)
// ---------------------------------------------------------------------------

bool PathTraceReflectionSecondaryAnalyticPayloadValid(PathTraceDoomAnalyticLightCandidate light)
{
    const float3 radiance = max(light.colorAndIntensity.rgb, float3(0.0, 0.0, 0.0));
    const float luminance = PathTraceCleanRoomLuminance(radiance);
    return all(light.originAndRadius.xyz == light.originAndRadius.xyz) &&
        all(abs(light.originAndRadius.xyz) < float3(3.402823e+38, 3.402823e+38, 3.402823e+38)) &&
        all(radiance == radiance) &&
        all(abs(radiance) < float3(3.402823e+38, 3.402823e+38, 3.402823e+38)) &&
        luminance > 0.0 &&
        light.originAndRadius.w > 0.0 &&
        light.originAndRadius.w < 3.402823e+38 &&
        light.doomRadiusAndArea.x > 0.0 &&
        light.doomRadiusAndArea.x < 3.402823e+38;
}

RAB_LightInfo PathTraceReflectionSecondaryBuildAnalyticLightInfo(
    PathTraceDoomAnalyticLightCandidate light,
    uint lightIndex)
{
    RAB_LightInfo lightInfo = RAB_EmptyLightInfo();
    if (!PathTraceReflectionSecondaryAnalyticPayloadValid(light))
    {
        return lightInfo;
    }

    lightInfo.lightType = RAB_LIGHT_TYPE_DOOM_ANALYTIC_SPHERE;
    lightInfo.lightIndex = lightIndex;
    lightInfo.unifiedLightType = PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC;
    lightInfo.materialIndex = RAB_INVALID_LIGHT_INDEX;
    lightInfo.flags = light.flags;
    lightInfo.position = light.originAndRadius.xyz;
    lightInfo.radius = max(light.originAndRadius.w, 0.01);
    lightInfo.normal = float3(0.0, 0.0, 1.0);
    lightInfo.radiance =
        max(light.colorAndIntensity.rgb, float3(0.0, 0.0, 0.0)) *
        max(CleanRtxdiDiDoomAnalyticLightInfo.z, 0.0);
    lightInfo.influenceRadius = max(light.doomRadiusAndArea.x, lightInfo.radius);
    lightInfo.area = max(light.doomRadiusAndArea.y, 1.0e-4);
    lightInfo.weight =
        PathTraceCleanRoomLuminance(lightInfo.radiance) *
        lightInfo.area *
        lightInfo.influenceRadius;
    return lightInfo;
}

float PathTraceReflectionSecondaryTraceVisibility(RAB_Surface surface, float3 samplePosition)
{
    const float3 surfacePosition = RAB_GetSurfaceWorldPos(surface);
    const float3 toSample = samplePosition - surfacePosition;
    const float distanceSquared = dot(toSample, toSample);
    if (distanceSquared <= 1.0e-6)
    {
        return 0.0;
    }

    const float distance = sqrt(distanceSquared);
    const float3 direction = toSample / distance;
    const float3 shadingNormal =
        RAB_SafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    const float3 geometricNormal =
        RAB_SafeNormalize(RAB_GetSurfaceGeoNormal(surface), shadingNormal);
    if (dot(shadingNormal, direction) <= 0.0 || dot(geometricNormal, direction) <= 0.0)
    {
        return 0.0;
    }

    const float normalSign = dot(geometricNormal, shadingNormal) >= 0.0 ? 1.0 : -1.0;
    RayDesc shadowRay;
    shadowRay.Origin = surfacePosition + geometricNormal * (normalSign * 0.75) + direction * 0.25;
    shadowRay.Direction = direction;
    shadowRay.TMin = 0.01;
    shadowRay.TMax = max(distance - 0.5, 0.01);

    PathTraceCleanRtxdiPayload shadowPayload;
    shadowPayload.value = 0u;
    shadowPayload.rayMode = 1u;
    shadowPayload.ignoreInstanceId = 0xffffffffu;
    shadowPayload.ignorePrimitiveIndex = 0xffffffffu;
    shadowPayload.ignoreMaterialIndex = 0xffffffffu;
    shadowPayload.hitInstanceId = 0xffffffffu;
    shadowPayload.hitPrimitiveIndex = 0xffffffffu;
    shadowPayload.hitMaterialId = 0xffffffffu;
    shadowPayload.hitMaterialIndex = 0xffffffffu;
    shadowPayload.hitTriangleClassAndFlags = 0u;
    shadowPayload.hitT = 0.0;
    shadowPayload.hitBarycentrics = float2(0.0, 0.0);

    TraceRay(
        SmokeScene,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_NON_OPAQUE,
        0xff,
        1,
        1,
        1,
        shadowRay,
        shadowPayload);

    return shadowPayload.value == 0u ? 1.0 : 0.0;
}

float PathTraceReflectionSecondaryNeeMisWeight(
    RAB_Surface surface,
    RAB_LightSample lightSample,
    float3 lightDir)
{
    if (!RAB_IsReplayableLightSample(lightSample) || lightSample.solidAnglePdf <= 0.0)
    {
        return 0.0;
    }

    const float scatterPdf = RAB_GetSurfaceBrdfPdf(surface, lightDir);
    if (scatterPdf <= 0.0)
    {
        return 1.0;
    }

    return lightSample.solidAnglePdf / max(lightSample.solidAnglePdf + scatterPdf, 1.0e-6);
}

bool PathTraceReflectionSecondaryAccumulateAnalyticLight(
    inout float3 radiance,
    RAB_Surface hitSurface,
    RAB_LightInfo lightInfo,
    float sourcePdf,
    PathTraceReflectionSecondaryBudget budget,
    inout RTXDI_RandomSamplerState rng)
{
    if (!RAB_IsLightInfoValid(lightInfo) || sourcePdf <= 1.0e-8)
    {
        return false;
    }

    const float2 uv = float2(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng));
    const RAB_LightSample lightSample = RAB_SamplePolymorphicLight(lightInfo, hitSurface, uv);
    if (!RAB_IsReplayableLightSample(lightSample) ||
        lightSample.solidAnglePdf <= 1.0e-8 ||
        PathTraceCleanRoomLuminance(lightSample.radiance) <= 0.0)
    {
        return false;
    }

    float3 lightDir;
    float lightDistance;
    RAB_GetLightDirDistance(hitSurface, lightSample, lightDir, lightDistance);
    const float ndotl = saturate(dot(RAB_GetSurfaceNormal(hitSurface), lightDir));
    if (ndotl <= 0.0)
    {
        return false;
    }

    const float3 brdf =
        RAB_EvaluateSurfaceBrdf(hitSurface, lightDir, RAB_GetSurfaceViewDir(hitSurface));
    if (PathTraceCleanRoomLuminance(brdf) <= 0.0)
    {
        return false;
    }

    float visibility = 1.0;
    if (budget.enableShadows != 0u)
    {
        visibility = PathTraceReflectionSecondaryTraceVisibility(hitSurface, lightSample.position);
        if (visibility <= 0.0)
        {
            return false;
        }
    }

    const float misWeight =
        PathTraceReflectionSecondaryNeeMisWeight(hitSurface, lightSample, lightDir);
    if (misWeight <= 0.0)
    {
        return false;
    }

    radiance += brdf * lightSample.radiance * ndotl * visibility /
        max(sourcePdf * lightSample.solidAnglePdf, 1.0e-6) *
        misWeight;
    return true;
}

// Simplified direct shade at a reflection hit. Uses the full current Doom
// analytic domain (not the camera portal subset) so reflections through
// doorways do not hard-cut light classes.
//
// Budget.maxAnalyticSamples independent uniform draws of the analytic domain;
// each draw is an unbiased estimate of full-domain direct, then averaged.
float3 PathTraceReflectionSecondaryShade(
    RAB_Surface hitSurface,
    PathTraceReflectionSecondaryBudget budget,
    inout RTXDI_RandomSamplerState rng)
{
    if (!RAB_IsSurfaceValid(hitSurface))
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float3 emissive = max(hitSurface.material.emissiveRadiance, float3(0.0, 0.0, 0.0));
    float3 radiance = emissive;
    const uint analyticCount = CleanRtxdiDiDoomAnalyticFullCurrentCount;
    if (analyticCount == 0u)
    {
        return radiance;
    }

    const uint sampleCount = clamp(budget.maxAnalyticSamples, 1u, 8u);
    const float sourcePdf = 1.0 / max((float)analyticCount, 1.0);
    float3 directSum = float3(0.0, 0.0, 0.0);
    [loop]
    for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
    {
        float3 sampleRadiance = float3(0.0, 0.0, 0.0);
        const uint lightIndex =
            min((uint)(RTXDI_GetNextRandom(rng) * analyticCount), analyticCount - 1u);
        const RAB_LightInfo lightInfo = PathTraceReflectionSecondaryBuildAnalyticLightInfo(
            DoomAnalyticLights[lightIndex],
            lightIndex);
        PathTraceReflectionSecondaryAccumulateAnalyticLight(
            sampleRadiance,
            hitSurface,
            lightInfo,
            sourcePdf,
            budget,
            rng);
        directSum += sampleRadiance;
    }
    radiance = emissive + directSum / (float)sampleCount;
    return radiance;
}

float3 PathTraceReflectionSecondaryShadeDefault(
    RAB_Surface hitSurface,
    inout RTXDI_RandomSamplerState rng)
{
    return PathTraceReflectionSecondaryShade(
        hitSurface,
        PathTraceReflectionSecondaryBudgetDefault(),
        rng);
}

// ---------------------------------------------------------------------------
// Compatibility wrappers (legacy PathTraceCleanRtxdiDi* names)
// ---------------------------------------------------------------------------

bool PathTraceCleanRtxdiDiTraceReflectionPsrHit(
    RAB_Surface surface,
    PathTraceCleanRtxdiDiReflectionPsrCandidate candidate,
    out PathTraceCleanRtxdiPayload hitPayload,
    out float3 hitPosition,
    out float3 rayDirection)
{
    PathTraceReflectionSecondaryHit hit;
    const bool ok = PathTraceReflectionSecondaryTrace(surface, candidate, hit);
    hitPayload = hit.payload;
    hitPosition = hit.hitPosition;
    rayDirection = hit.rayDirection;
    return ok;
}

bool PathTraceCleanRtxdiDiTraceReflectionHit(
    RAB_Surface surface,
    out PathTraceCleanRtxdiPayload hitPayload,
    out float3 hitPosition,
    out float3 rayDirection)
{
    PathTraceReflectionSecondaryHit hit;
    const bool ok = PathTraceReflectionSecondaryTraceMirrorFromSurface(surface, hit);
    hitPayload = hit.payload;
    hitPosition = hit.hitPosition;
    rayDirection = hit.rayDirection;
    return ok;
}

bool PathTraceCleanRtxdiDiReflectionAnalyticPayloadValid(PathTraceDoomAnalyticLightCandidate light)
{
    return PathTraceReflectionSecondaryAnalyticPayloadValid(light);
}

RAB_LightInfo PathTraceCleanRtxdiDiBuildReflectionAnalyticLightInfo(
    PathTraceDoomAnalyticLightCandidate light,
    uint lightIndex)
{
    return PathTraceReflectionSecondaryBuildAnalyticLightInfo(light, lightIndex);
}

float PathTraceCleanRtxdiDiReflectionTraceVisibility(RAB_Surface surface, float3 samplePosition)
{
    return PathTraceReflectionSecondaryTraceVisibility(surface, samplePosition);
}

float PathTraceCleanRtxdiDiReflectionSecondaryNeeMisWeight(
    RAB_Surface surface,
    RAB_LightSample lightSample,
    float3 lightDir)
{
    return PathTraceReflectionSecondaryNeeMisWeight(surface, lightSample, lightDir);
}

bool PathTraceCleanRtxdiDiAccumulateReflectionAnalyticLight(
    inout float3 radiance,
    RAB_Surface hitSurface,
    RAB_LightInfo lightInfo,
    float sourcePdf,
    inout RTXDI_RandomSamplerState rng)
{
    return PathTraceReflectionSecondaryAccumulateAnalyticLight(
        radiance,
        hitSurface,
        lightInfo,
        sourcePdf,
        PathTraceReflectionSecondaryBudgetDefault(),
        rng);
}

float3 PathTraceCleanRtxdiDiShadeReflectionHit(
    RAB_Surface hitSurface,
    inout RTXDI_RandomSamplerState rng)
{
    return PathTraceReflectionSecondaryShadeDefault(hitSurface, rng);
}

#endif // CLEAN_RTXDI_DI_TRANSMISSION_PSR_TRANSPORT || CLEAN_RTXDI_DI_REFLECTION_SECONDARY

#endif // RB_PATH_TRACE_REFLECTION_SECONDARY_HLSLI
