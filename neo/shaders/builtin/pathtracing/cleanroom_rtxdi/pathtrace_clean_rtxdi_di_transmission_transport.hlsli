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
    PathTraceCleanRtxdiPayload payload;
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
    TraceRay(SmokeScene, RAY_FLAG_FORCE_NON_OPAQUE, 0xff, 0, 1, 0, ray, hitPayload);
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

bool PathTraceCleanRtxdiDiTraceReflectionHit(
    RAB_Surface surface,
    out PathTraceCleanRtxdiPayload hitPayload,
    out float3 hitPosition,
    out float3 rayDirection)
{
    const float3 normal = RAB_SafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    const float3 viewDirection = RAB_SafeNormalize(RAB_GetSurfaceViewDir(surface), normal);
    const float3 faceForwardNormal = dot(normal, viewDirection) >= 0.0 ? normal : -normal;
    rayDirection = reflect(-viewDirection, faceForwardNormal);
    hitPayload = PathTraceCleanRtxdiDiEmptyTransmissionTracePayload(surface);

    RayDesc ray;
    ray.Origin = RAB_GetSurfaceWorldPos(surface) + rayDirection * 0.05;
    ray.Direction = rayDirection;
    ray.TMin = 0.01;
    ray.TMax = 100000.0;
    // Keep rayMode 3 and forced any-hit filtering so the source pane is skipped
    // exactly like the transmission continuation ray.
    TraceRay(SmokeScene, RAY_FLAG_FORCE_NON_OPAQUE, 0xff, 0, 1, 0, ray, hitPayload);
    hitPosition = ray.Origin + rayDirection * hitPayload.hitT;
    return hitPayload.value != 0u && hitPayload.hitMaterialIndex < (uint)TextureInfo.z;
}

bool PathTraceCleanRtxdiDiReflectionAnalyticPayloadValid(PathTraceDoomAnalyticLightCandidate light)
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

RAB_LightInfo PathTraceCleanRtxdiDiBuildReflectionAnalyticLightInfo(
    PathTraceDoomAnalyticLightCandidate light,
    uint lightIndex)
{
    RAB_LightInfo lightInfo = RAB_EmptyLightInfo();
    if (!PathTraceCleanRtxdiDiReflectionAnalyticPayloadValid(light))
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

float PathTraceCleanRtxdiDiReflectionTraceVisibility(RAB_Surface surface, float3 samplePosition)
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

float PathTraceCleanRtxdiDiReflectionSecondaryNeeMisWeight(RAB_Surface surface, RAB_LightSample lightSample, float3 lightDir)
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

bool PathTraceCleanRtxdiDiAccumulateReflectionAnalyticLight(
    inout float3 radiance,
    RAB_Surface hitSurface,
    RAB_LightInfo lightInfo,
    float sourcePdf,
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

    const float visibility =
        PathTraceCleanRtxdiDiReflectionTraceVisibility(hitSurface, lightSample.position);
    if (visibility <= 0.0)
    {
        return false;
    }

    const float misWeight =
        PathTraceCleanRtxdiDiReflectionSecondaryNeeMisWeight(hitSurface, lightSample, lightDir);
    if (misWeight <= 0.0)
    {
        return false;
    }

    radiance += brdf * lightSample.radiance * ndotl * visibility /
        max(sourcePdf * lightSample.solidAnglePdf, 1.0e-6) *
        misWeight;
    return true;
}

float3 PathTraceCleanRtxdiDiShadeReflectionHit(
    RAB_Surface hitSurface,
    inout RTXDI_RandomSamplerState rng)
{
    if (!RAB_IsSurfaceValid(hitSurface))
    {
        return float3(0.0, 0.0, 0.0);
    }

    float3 radiance = max(hitSurface.material.emissiveRadiance, float3(0.0, 0.0, 0.0));
    // Reflection rays may see through a doorway/portal different from the
    // camera's current DI light domain. Use the full current Doom analytic
    // domain here to avoid hard one-portal discontinuities in the reflection
    // sidecar diagnostic.
    const uint analyticCount = CleanRtxdiDiDoomAnalyticFullCurrentCount;
    if (analyticCount == 0u)
    {
        return radiance;
    }

    const uint lightIndex = min((uint)(RTXDI_GetNextRandom(rng) * analyticCount), analyticCount - 1u);
    const RAB_LightInfo lightInfo =
        PathTraceCleanRtxdiDiBuildReflectionAnalyticLightInfo(DoomAnalyticLights[lightIndex], lightIndex);
    PathTraceCleanRtxdiDiAccumulateReflectionAnalyticLight(
        radiance,
        hitSurface,
        lightInfo,
        1.0 / max((float)analyticCount, 1.0),
        rng);
    return radiance;
}

#endif

#endif
