#ifndef RB_PATH_TRACE_REFLECTION_SECONDARY_HLSLI
#define RB_PATH_TRACE_REFLECTION_SECONDARY_HLSLI

// Shared reflection secondary service (dedicated_reflection_secondary_steps.txt).
//
// Jobs:
//   Trace a simplified reflection ray (Dirac glass / high-gloss callers).
//   Add exact hit emissive, then bounded analytic RIS direct lighting.
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

// Budget for simplified secondary shade.
// Host packs RIS candidate count M in CleanRtxdiDiMotionVectorInfo.y (1-16).
// Validated default M=8; candidates select one analytic shade and at most one shadow ray.
// Shadows off via CLEAN_FLAG_REFLECTION_SECONDARY_NO_SHADOWS.
//
// Quality path is RIS (M candidates -> 1 light + 1 shadow), NOT multi-SPP
// averaging. Multi-SPP cannot match 1-SPP primary DI / RTXDI cost quality.
struct PathTraceReflectionSecondaryBudget
{
    uint risCandidateCount;
    uint enableShadows;
};

PathTraceReflectionSecondaryBudget PathTraceReflectionSecondaryBudgetFromConstants()
{
    PathTraceReflectionSecondaryBudget budget;
    budget.risCandidateCount =
        clamp((uint)max(CleanRtxdiDiMotionVectorInfo.y, 1.0), 1u, 16u);
    budget.enableShadows =
        ((CleanRtxdiDiFlags & CLEAN_FLAG_REFLECTION_SECONDARY_NO_SHADOWS) == 0u) ? 1u : 0u;
    return budget;
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

// Build a mirror candidate from the surface view and trace (dense glass hybrid /
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

// Target weight for RIS light selection (no visibility). Proxy of direct
// contribution so reservoir prefers strong, front-facing lights at the hit.
float PathTraceReflectionSecondaryRisTargetWeight(
    RAB_Surface hitSurface,
    RAB_LightInfo lightInfo)
{
    if (!RAB_IsLightInfoValid(lightInfo))
    {
        return 0.0;
    }

    const float3 hitPos = RAB_GetSurfaceWorldPos(hitSurface);
    const float3 toLight = lightInfo.position - hitPos;
    const float distSq = max(dot(toLight, toLight), 1.0e-6);
    const float3 lightDir = toLight * rsqrt(distSq);
    const float3 shadingNormal = RAB_SafeNormalize(
        RAB_GetSurfaceNormal(hitSurface),
        RAB_GetSurfaceGeoNormal(hitSurface));
    const float ndotl = saturate(dot(shadingNormal, lightDir));
    if (ndotl <= 0.0)
    {
        return 0.0;
    }

    // lightInfo.weight already folds luminance * area * influence radius.
    return max(lightInfo.weight, 0.0) * ndotl / distSq;
}

// Simplified direct shade at a reflection hit (Remix secondary NEE shape):
//   RIS over M uniform light candidates from the full Doom analytic domain
//   -> select ONE light -> ONE area sample + optional ONE shadow ray.
// Zero-weight candidates are skipped (same black-noise rule as DI StreamSample:
// do not inflate M / admit empty draws that starve valid lights).
// If the selected light fails shade/visibility, retry other stored positive-
// weight candidates instead of writing pure black reflection energy.
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

    const uint candidateCount = clamp(budget.risCandidateCount, 1u, 16u);
    const float proposalPdf = 1.0 / max((float)analyticCount, 1.0);

    // Short list of positive-weight candidates for visibility retries.
    const uint kMaxRetryLights = 4u;
    uint retryLightIndex[4];
    float retryTargetWeight[4];
    RAB_LightInfo retryLightInfo[4];
    uint retryCount = 0u;

    float weightSum = 0.0;
    uint selectedSlot = 0xffffffffu;
    uint positiveCandidateCount = 0u;

    [loop]
    for (uint candidateIndex = 0u; candidateIndex < candidateCount; ++candidateIndex)
    {
        const uint lightIndex =
            min((uint)(RTXDI_GetNextRandom(rng) * analyticCount), analyticCount - 1u);
        const RAB_LightInfo lightInfo = PathTraceReflectionSecondaryBuildAnalyticLightInfo(
            DoomAnalyticLights[lightIndex],
            lightIndex);
        const float targetWeight =
            PathTraceReflectionSecondaryRisTargetWeight(hitSurface, lightInfo);
        // Do not admit zero-target candidates (black-noise rule).
        if (targetWeight <= 1.0e-10)
        {
            continue;
        }

        positiveCandidateCount += 1u;
        const float risWeight = targetWeight / max(proposalPdf, 1.0e-8);
        weightSum += risWeight;

        // Streaming RIS selection among positive-weight candidates only.
        const bool selectThis =
            weightSum > 0.0 &&
            RTXDI_GetNextRandom(rng) * weightSum < risWeight;

        if (retryCount < kMaxRetryLights)
        {
            retryLightIndex[retryCount] = lightIndex;
            retryTargetWeight[retryCount] = targetWeight;
            retryLightInfo[retryCount] = lightInfo;
            if (selectThis)
            {
                selectedSlot = retryCount;
            }
            retryCount += 1u;
        }
        else if (selectThis)
        {
            // Keep the current selection in slot 0 for shade attempts.
            retryLightIndex[0] = lightIndex;
            retryTargetWeight[0] = targetWeight;
            retryLightInfo[0] = lightInfo;
            selectedSlot = 0u;
        }
    }

    if (retryCount == 0u || weightSum <= 1.0e-10 || positiveCandidateCount == 0u)
    {
        return radiance;
    }

    if (selectedSlot >= retryCount)
    {
        selectedSlot = 0u;
    }

    // UCW uses count of positive candidates only (zeros never entered).
    const float ucwBase = weightSum / max((float)positiveCandidateCount, 1.0);

    // Try selected first, then other positive candidates if shade/visibility fails.
    [loop]
    for (uint attempt = 0u; attempt < retryCount; ++attempt)
    {
        const uint slot = (selectedSlot + attempt) % retryCount;
        const float pHat = retryTargetWeight[slot];
        if (pHat <= 1.0e-10)
        {
            continue;
        }

        float3 direct = float3(0.0, 0.0, 0.0);
        if (!PathTraceReflectionSecondaryAccumulateAnalyticLight(
            direct,
            hitSurface,
            retryLightInfo[slot],
            1.0,
            budget,
            rng))
        {
            // Zero result after selection: do not accept black — try next.
            continue;
        }

        const float ucw = ucwBase / max(pHat, 1.0e-8);
        radiance += direct * ucw;
        break;
    }

    return radiance;
}

#endif // CLEAN_RTXDI_DI_TRANSMISSION_PSR_TRANSPORT || CLEAN_RTXDI_DI_REFLECTION_SECONDARY

#endif // RB_PATH_TRACE_REFLECTION_SECONDARY_HLSLI
