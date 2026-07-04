#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_TRANSMISSION_TRANSPORT_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_TRANSMISSION_TRANSPORT_HLSLI

#if defined(CLEAN_RTXDI_DI_TRANSMISSION_PSR_TRANSPORT)

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
    out PathTraceCleanRtxdiPayload hitPayload,
    out float3 hitPosition,
    out float3 rayDirection)
{
    const float3 normal = RAB_SafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    const float3 viewDirection = RAB_SafeNormalize(RAB_GetSurfaceViewDir(surface), normal);
    // Thin-walled glass: entry and exit refractions cancel, so the transmitted
    // ray keeps the incident direction.
    rayDirection = -viewDirection;
    hitPayload = PathTraceCleanRtxdiDiEmptyTransmissionTracePayload(surface);

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

#endif

#endif
