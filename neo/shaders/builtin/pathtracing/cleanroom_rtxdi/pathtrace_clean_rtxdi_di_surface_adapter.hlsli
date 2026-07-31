#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_SURFACE_ADAPTER_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_SURFACE_ADAPTER_HLSLI

bool PathTraceCleanRoomLoadSurfaceRecord(uint2 pixel, uint2 dimensions, out PathTracePrimarySurfaceRecord record)
{
    record = (PathTracePrimarySurfaceRecord)0;
    const uint width = CleanRtxdiDiWidth != 0u ? CleanRtxdiDiWidth : dimensions.x;
    const uint height = CleanRtxdiDiHeight != 0u ? CleanRtxdiDiHeight : dimensions.y;
    if (width == 0u || height == 0u || pixel.x >= width || pixel.y >= height)
    {
        return false;
    }

    record = PrimarySurfaceHistoryCurrent[pixel.y * width + pixel.x];
    return record.header.x == RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_VERSION &&
        (record.header.y & RT_PRIMARY_SURFACE_VALID) != 0u;
}

bool PathTraceCleanRoomLoadSurfaceRecordSigned(int2 pixel, uint2 dimensions, bool previousFrame, out PathTracePrimarySurfaceRecord record)
{
    record = (PathTracePrimarySurfaceRecord)0;
    const uint width = CleanRtxdiDiWidth != 0u ? CleanRtxdiDiWidth : dimensions.x;
    const uint height = CleanRtxdiDiHeight != 0u ? CleanRtxdiDiHeight : dimensions.y;
    if (width == 0u || height == 0u || pixel.x < 0 || pixel.y < 0 || (uint)pixel.x >= width || (uint)pixel.y >= height)
    {
        return false;
    }

    const uint index = (uint)pixel.y * width + (uint)pixel.x;
    if (previousFrame)
    {
        record = PrimarySurfaceHistoryPrevious[index];
    }
    else
    {
        record = PrimarySurfaceHistoryCurrent[index];
    }
    return record.header.x == RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_VERSION &&
        (record.header.y & RT_PRIMARY_SURFACE_VALID) != 0u;
}

bool PathTraceCleanRoomIsCachedRigidRouteHit(uint instanceId)
{
    if (instanceId < 2u)
    {
        return false;
    }

    const uint routeInstanceIndex = instanceId - 2u;
    const uint rigidRouteInstanceCount = (uint)max(ToyPathInfo.w, 0.0);
    if (routeInstanceIndex >= rigidRouteInstanceCount)
    {
        return false;
    }

    const PathTraceRigidRouteInstance routeInstance = SmokeRigidRouteInstances[routeInstanceIndex];
    return (routeInstance.flags & PT_RIGID_ROUTE_CACHED_SOURCE) != 0u;
}

#if !RB_PT_ENABLE_STATIC_BUCKET_SHADER_CONSUMERS
#define PathTraceCleanRoomLoadNonStaticTriangleMaterialIndex \
    PathTraceCleanRoomLoadTriangleMaterialIndex
#endif

uint PathTraceCleanRoomLoadNonStaticTriangleMaterialIndex(
    uint instanceId,
    uint primitiveIndex)
{
    if (instanceId == 0u)
    {
        return primitiveIndex < CleanRtxdiDiStaticTriangleCount
            ? SmokeStaticTriangleMaterialIndexes[primitiveIndex]
            : 0xffffffffu;
    }

    if (instanceId == 1u)
    {
        return primitiveIndex < CleanRtxdiDiDynamicTriangleCount
            ? SmokeDynamicTriangleMaterialIndexes[primitiveIndex]
            : 0xffffffffu;
    }

#if !defined(RB_PT_SKINNED_HIT_ROUTE_LIGHTWEIGHT)
    if (PathTraceIsSkinnedHitRouteInstance(instanceId))
    {
        PathTraceSkinnedHitRouteGpuRecord route;
        PathTraceSkinnedHitRouteGpuTriangle routeTriangle;
        return PathTraceLoadSkinnedHitRoute(instanceId, route) &&
            PathTraceLoadSkinnedHitRouteTriangle(
                route,
                primitiveIndex,
                routeTriangle)
            ? routeTriangle.materialIndex
            : 0xffffffffu;
    }

    if (!PathTraceIsRigidHitRouteInstance(instanceId))
    {
        return 0xffffffffu;
    }
#endif
    const uint routeInstanceIndex = instanceId - 2u;
    const uint rigidRouteInstanceCount = (uint)max(ToyPathInfo.w, 0.0);
    if (routeInstanceIndex >= rigidRouteInstanceCount)
    {
        return 0xffffffffu;
    }

    const PathTraceRigidRouteInstance routeInstance = SmokeRigidRouteInstances[routeInstanceIndex];
    const uint routedPrimitiveIndex = routeInstance.triangleOffset + primitiveIndex;
    if (primitiveIndex >= routeInstance.triangleCount ||
        routedPrimitiveIndex >= CleanRtxdiDiRigidRouteTriangleCount)
    {
        return 0xffffffffu;
    }

    return routeInstance.materialIndex;
}

#if RB_PT_ENABLE_STATIC_BUCKET_SHADER_CONSUMERS
uint PathTraceCleanRoomLoadTriangleMaterialIndex(
    uint instanceId,
    uint primitiveIndex)
{
#if defined(CLEAN_RTXDI_DI_INITIAL_ENTRY) || \
    defined(CLEAN_RTXDI_DI_TEMPORAL_ENTRY) || \
    defined(CLEAN_RTXDI_DI_GLASS_ENTRY) || \
    defined(CLEAN_RTXDI_DI_TRACE_HIT_SURFACE_ADAPTER)
    if (PathTraceIsStaticBucketRouteInstance(
            instanceId,
            CleanRtxdiDiStaticBucketRouteInfo))
    {
        PathTraceStaticGeometryAddress address;
        return PathTraceCleanRtxdiDiTryResolveStaticBucketHardwareHit(
                instanceId,
                0u,
                primitiveIndex,
                address)
            ? SmokeStaticBucketTriangleMaterialIndexes[
                address.triangleIndex]
            : 0xffffffffu;
    }
#endif

    return PathTraceCleanRoomLoadNonStaticTriangleMaterialIndex(
        instanceId,
        primitiveIndex);
}
#else
#undef PathTraceCleanRoomLoadNonStaticTriangleMaterialIndex
#endif

#if defined(CLEAN_RTXDI_DI_TRACE_HIT_SURFACE_ADAPTER)
uint PathTraceCleanRtxdiDiTraceHitLoadTriangleMaterialId(uint instanceId, uint primitiveIndex)
{
    if (PathTraceIsStaticBucketRouteInstance(
            instanceId,
            CleanRtxdiDiStaticBucketRouteInfo))
    {
        PathTraceStaticGeometryAddress address;
        return PathTraceCleanRtxdiDiTryResolveStaticBucketHardwareHit(
                instanceId,
                0u,
                primitiveIndex,
                address)
            ? SmokeStaticBucketTriangleMaterials[
                address.triangleIndex]
            : 0xffffffffu;
    }

    if (instanceId == 0u)
    {
        return primitiveIndex < CleanRtxdiDiStaticTriangleCount
            ? SmokeStaticTriangleMaterials[primitiveIndex]
            : 0xffffffffu;
    }

    if (instanceId == 1u)
    {
        return primitiveIndex < CleanRtxdiDiDynamicTriangleCount
            ? SmokeDynamicTriangleMaterials[primitiveIndex]
            : 0xffffffffu;
    }

#if !defined(RB_PT_SKINNED_HIT_ROUTE_LIGHTWEIGHT)
    if (PathTraceIsSkinnedHitRouteInstance(instanceId))
    {
        PathTraceSkinnedHitRouteGpuRecord route;
        PathTraceSkinnedHitRouteGpuTriangle routeTriangle;
        return PathTraceLoadSkinnedHitRoute(instanceId, route) &&
            PathTraceLoadSkinnedHitRouteTriangle(
                route,
                primitiveIndex,
                routeTriangle)
            ? routeTriangle.materialId
            : 0xffffffffu;
    }

    if (!PathTraceIsRigidHitRouteInstance(instanceId))
    {
        return 0xffffffffu;
    }
#endif
    const uint routeInstanceIndex = instanceId - 2u;
    const uint rigidRouteInstanceCount = (uint)max(ToyPathInfo.w, 0.0);
    if (routeInstanceIndex >= rigidRouteInstanceCount)
    {
        return 0xffffffffu;
    }

    const PathTraceRigidRouteInstance routeInstance = SmokeRigidRouteInstances[routeInstanceIndex];
    const uint routedPrimitiveIndex = routeInstance.triangleOffset + primitiveIndex;
    if (primitiveIndex >= routeInstance.triangleCount ||
        routedPrimitiveIndex >= CleanRtxdiDiRigidRouteTriangleCount)
    {
        return 0xffffffffu;
    }

    return routeInstance.materialId;
}

uint PathTraceCleanRtxdiDiTraceHitLoadTriangleClassAndFlags(uint instanceId, uint primitiveIndex)
{
    if (PathTraceIsStaticBucketRouteInstance(
            instanceId,
            CleanRtxdiDiStaticBucketRouteInfo))
    {
        PathTraceStaticGeometryAddress address;
        return PathTraceCleanRtxdiDiTryResolveStaticBucketHardwareHit(
                instanceId,
                0u,
                primitiveIndex,
                address)
            ? SmokeStaticBucketTriangleClasses[address.triangleIndex]
            : 0u;
    }

    if (instanceId == 0u)
    {
        return primitiveIndex < CleanRtxdiDiStaticTriangleCount
            ? SmokeStaticTriangleClasses[primitiveIndex]
            : 0u;
    }

    if (instanceId == 1u)
    {
        return primitiveIndex < CleanRtxdiDiDynamicTriangleCount
            ? SmokeDynamicTriangleClasses[primitiveIndex]
            : 0u;
    }

#if !defined(RB_PT_SKINNED_HIT_ROUTE_LIGHTWEIGHT)
    if (PathTraceIsSkinnedHitRouteInstance(instanceId))
    {
        PathTraceSkinnedHitRouteGpuRecord route;
        PathTraceSkinnedHitRouteGpuTriangle routeTriangle;
        return PathTraceLoadSkinnedHitRoute(instanceId, route) &&
            PathTraceLoadSkinnedHitRouteTriangle(
                route,
                primitiveIndex,
                routeTriangle)
            ? routeTriangle.triangleClassAndFlags
            : 0u;
    }
    return PathTraceIsRigidHitRouteInstance(instanceId)
        ? RT_SMOKE_SURFACE_CLASS_RIGID_ENTITY
        : 0u;
#else
    return RT_SMOKE_SURFACE_CLASS_RIGID_ENTITY;
#endif
}
#endif

uint PathTraceCleanRoomResolveLiveMaterialIndex(PathTracePrimarySurfaceRecord record)
{
    const uint recordMaterialIndex = record.materialAndSurface.y;
    const uint liveMaterialIndex = PathTraceCleanRoomLoadTriangleMaterialIndex(
        record.instancePrimitiveObject.x,
        record.instancePrimitiveObject.y);
    return liveMaterialIndex < (uint)TextureInfo.z ? liveMaterialIndex : recordMaterialIndex;
}

RAB_Surface PathTraceCleanRoomSurfaceFromRecord(PathTracePrimarySurfaceRecord record)
{
    RAB_Surface surface = RAB_EmptySurface();
    if (record.header.x != RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_VERSION ||
        (record.header.y & RT_PRIMARY_SURFACE_VALID) == 0u)
    {
        return surface;
    }

    surface.valid = 1u;
    surface.worldPos = record.worldPositionAndViewDepth.xyz;
    surface.linearDepth = record.worldPositionAndViewDepth.w;
    surface.geometryNormal = PathTraceCleanRoomSafeNormalize(record.geometricNormalAndRoughness.xyz, float3(0.0, 0.0, 1.0));
    surface.shadingNormal = PathTraceCleanRoomSafeNormalize(record.shadingNormalAndOpacity.xyz, surface.geometryNormal);
    surface.viewDir = PathTraceCleanRoomSafeNormalize(record.viewDirectionAndReserved.xyz, -surface.shadingNormal);
    surface.materialId = record.materialAndSurface.x;
    surface.materialIndex = record.materialAndSurface.y;
    surface.surfaceClass = record.materialAndSurface.w & 0xffu;
    surface.material = RAB_EmptyMaterial();
    surface.material.materialId = surface.materialId;
    surface.material.materialIndex = surface.materialIndex;
    surface.material.diffuseAlbedo = float3(0.5, 0.5, 0.5);
    surface.material.roughness = 1.0;
    surface.material.opacity = 1.0;
    return surface;
}

RAB_Surface PathTraceCleanRoomMaterialSurfaceFromRecord(PathTracePrimarySurfaceRecord record)
{
    RAB_Surface surface = PathTraceCleanRoomSurfaceFromRecord(record);
    if (!RAB_IsSurfaceValid(surface))
    {
        return surface;
    }

    surface.flags = record.header.w;
    surface.instanceId = record.instancePrimitiveObject.x;
    surface.primitiveIndex = record.instancePrimitiveObject.y;

#if defined(CLEAN_DI_VIEW_STATIC)
    const uint resolvedMaterialIndex = record.materialAndSurface.y;
#else
    const uint resolvedMaterialIndex = PathTraceCleanRoomResolveLiveMaterialIndex(record);
#endif

    RAB_Material material = RAB_EmptyMaterial();
    material.materialId = surface.materialId;
    material.materialIndex = resolvedMaterialIndex;
    material.flags = record.materialAndSurface.z;
    material.alphaCutoff = record.albedoAndAlphaCutoff.w;
    material.diffuseAlbedo = saturate(record.albedoAndAlphaCutoff.xyz);
    material.roughness = saturate(record.geometricNormalAndRoughness.w);
    material.specularF0 = max(record.specularF0AndReserved.xyz, float3(0.0, 0.0, 0.0));
#if !defined(CLEAN_DI_VIEW_STATIC)
    if ((record.header.w &
            (CLEAN_SURFACE_FLAG_TRANSMISSION_PSR_RESOLVED |
                CLEAN_SURFACE_FLAG_REFLECTION_PSR_RESOLVED)) == 0u)
    {
        PathTraceCleanRoomApplyLiveMaterialClassifierBsdf(
            material.materialIndex,
            material.diffuseAlbedo,
            material.specularF0,
            material.roughness);
    }
#endif
    material.opacity = saturate(record.shadingNormalAndOpacity.w);
    if ((record.header.w &
            (CLEAN_SURFACE_FLAG_TRANSMISSION_PSR_RESOLVED |
                CLEAN_SURFACE_FLAG_REFLECTION_PSR_RESOLVED)) != 0u)
    {
        material.opacity = max(material.opacity, 1.0);
    }
    material.emissiveRadiance = max(record.emissiveAndHeight.xyz, float3(0.0, 0.0, 0.0));
    material.emissiveTextureIndex = record.instancePrimitiveObject.w;
    surface.materialIndex = resolvedMaterialIndex;
    surface.material = material;
    return surface;
}

RAB_Surface PathTraceCleanRoomSurfaceForView(PathTracePrimarySurfaceRecord record)
{
    if (CleanRtxdiDiView == 16u)
    {
        return PathTraceCleanRoomMaterialSurfaceFromRecord(record);
    }
    if (PathTraceCleanRoomLiveMaterialClassifierBsdfActive(PathTraceCleanRoomResolveLiveMaterialIndex(record)))
    {
        return PathTraceCleanRoomMaterialSurfaceFromRecord(record);
    }
    return PathTraceCleanRoomSurfaceFromRecord(record);
}

PathTracePrimarySurfaceRecord PathTraceCleanRtxdiDiPackResolvedPrimarySurfaceRecord(
    RAB_Surface surface,
    uint psrResolvedFlag)
{
    PathTracePrimarySurfaceRecord record = (PathTracePrimarySurfaceRecord)0;
    record.header.x = RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_VERSION;
    if (!RAB_IsSurfaceValid(surface))
    {
        record.header.z = RT_PRIMARY_SURFACE_DEBUG_MISSING_CURRENT;
        return record;
    }

    uint validFlags = RT_PRIMARY_SURFACE_VALID;
    if (CleanRtxdiDiPrevCameraOriginAndValid.w >= 0.5)
    {
        validFlags |= RT_PRIMARY_SURFACE_HAS_CAMERA_REPROJECTION;
    }

    record.header = uint4(
        RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_VERSION,
        validFlags,
        RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION,
        surface.flags | psrResolvedFlag);
    record.worldPositionAndViewDepth = float4(surface.worldPos, surface.linearDepth);
    record.geometricNormalAndRoughness = float4(surface.geometryNormal, surface.material.roughness);
    record.shadingNormalAndOpacity = float4(surface.shadingNormal, surface.material.opacity);
    record.viewDirectionAndReserved = float4(surface.viewDir, 0.0);
    record.albedoAndAlphaCutoff = float4(surface.material.diffuseAlbedo, surface.material.alphaCutoff);
    record.specularF0AndReserved = float4(surface.material.specularF0, 0.0);
    record.emissiveAndHeight = float4(surface.material.emissiveRadiance, 0.0);
    record.previousPositionOrMotion = float4(0.0, 0.0, 0.0, 0.0);
    record.materialAndSurface = uint4(
        surface.materialId,
        surface.materialIndex,
        surface.material.flags,
        surface.surfaceClass);
    record.instancePrimitiveObject = uint4(
        surface.instanceId,
        surface.primitiveIndex,
        0u,
        surface.material.emissiveTextureIndex);
    return record;
}

RAB_Surface RAB_GetGBufferSurface(int2 pixel, bool previousFrame)
{
    const uint2 dimensions = uint2(
        CleanRtxdiDiWidth != 0u ? CleanRtxdiDiWidth : DispatchRaysDimensions().x,
        CleanRtxdiDiHeight != 0u ? CleanRtxdiDiHeight : DispatchRaysDimensions().y);
    PathTracePrimarySurfaceRecord record;
    if (!PathTraceCleanRoomLoadSurfaceRecordSigned(pixel, dimensions, previousFrame, record))
    {
        return RAB_EmptySurface();
    }
    return PathTraceCleanRoomSurfaceForView(record);
}

#endif
