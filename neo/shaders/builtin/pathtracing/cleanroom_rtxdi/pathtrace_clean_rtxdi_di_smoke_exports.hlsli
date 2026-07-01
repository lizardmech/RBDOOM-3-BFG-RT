#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_SMOKE_EXPORTS_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_SMOKE_EXPORTS_HLSLI

bool PathTraceCleanRoomMaterialDoesNotOccludeVisibility(uint materialIndex)
{
    if (materialIndex >= (uint)TextureInfo.z)
    {
        return false;
    }

    const PathTraceSmokeMaterial material = PathTraceCleanRoomLoadSmokeMaterial(materialIndex);
    const uint transparentCardFlags =
        RT_SMOKE_MATERIAL_ADDITIVE_DECAL |
        RT_SMOKE_MATERIAL_FILTER_DECAL |
        RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_LUMA |
        RT_SMOKE_MATERIAL_PORTAL_WINDOW_FALLBACK |
        RT_SMOKE_MATERIAL_OBJECT_GLASS_FALLBACK |
        RT_SMOKE_MATERIAL_ADDITIVE_DECAL_WHITE_KEY |
        RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_MAGENTA_KEY;
    return (material.flags & transparentCardFlags) != 0u;
}

[shader("miss")]
void Miss(inout PathTraceCleanRtxdiPayload payload)
{
    payload.value = 0u;
}

[shader("miss")]
void ShadowMiss(inout PathTraceCleanRtxdiPayload payload)
{
    payload.value = 0u;
}

[shader("anyhit")]
void AnyHit(inout PathTraceCleanRtxdiPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    if (payload.rayMode == 2u &&
        InstanceID() == payload.ignoreInstanceId)
    {
        if (PathTraceCleanRoomIsCachedRigidRouteHit(payload.ignoreInstanceId))
        {
            IgnoreHit();
            return;
        }

        const uint primitiveIndex = PrimitiveIndex();
        const uint materialIndex = PathTraceCleanRoomLoadTriangleMaterialIndex(payload.ignoreInstanceId, primitiveIndex);
        if (primitiveIndex == payload.ignorePrimitiveIndex || materialIndex == payload.ignoreMaterialIndex)
        {
            IgnoreHit();
        }
    }
}

[shader("anyhit")]
void ShadowAnyHit(inout PathTraceCleanRtxdiPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    const uint instanceId = InstanceID();
    if (PathTraceCleanRoomIsCachedRigidRouteHit(instanceId))
    {
        IgnoreHit();
        return;
    }

    const uint primitiveIndex = PrimitiveIndex();
    const uint materialIndex = PathTraceCleanRoomLoadTriangleMaterialIndex(instanceId, primitiveIndex);
    if (PathTraceCleanRoomMaterialDoesNotOccludeVisibility(materialIndex))
    {
        IgnoreHit();
    }

    if (payload.rayMode == 2u &&
        instanceId == payload.ignoreInstanceId)
    {
        if (primitiveIndex == payload.ignorePrimitiveIndex || materialIndex == payload.ignoreMaterialIndex)
        {
            IgnoreHit();
        }
    }
}

[shader("closesthit")]
void ClosestHit(inout PathTraceCleanRtxdiPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    payload.value = 1u;
}

[shader("closesthit")]
void ShadowClosestHit(inout PathTraceCleanRtxdiPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    payload.value = 1u;
}

#endif
