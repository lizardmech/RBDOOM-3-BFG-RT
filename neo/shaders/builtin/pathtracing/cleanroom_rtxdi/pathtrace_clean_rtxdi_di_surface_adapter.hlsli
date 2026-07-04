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

uint PathTraceCleanRoomLoadTriangleMaterialIndex(uint instanceId, uint primitiveIndex)
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

    return SmokeRigidRouteTriangleMaterialIndexes[routedPrimitiveIndex];
}

#if defined(CLEAN_RTXDI_DI_TRANSMISSION_PRODUCER_ENTRY)
uint PathTraceCleanRtxdiDiTransmissionLoadTriangleMaterialId(uint instanceId, uint primitiveIndex)
{
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

    return SmokeRigidRouteTriangleMaterials[routedPrimitiveIndex];
}

uint PathTraceCleanRtxdiDiTransmissionLoadTriangleClassAndFlags(uint instanceId, uint primitiveIndex)
{
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

    return RT_SMOKE_SURFACE_CLASS_RIGID_ENTITY;
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

    const uint resolvedMaterialIndex = PathTraceCleanRoomResolveLiveMaterialIndex(record);

    RAB_Material material = RAB_EmptyMaterial();
    material.materialId = surface.materialId;
    material.materialIndex = resolvedMaterialIndex;
    material.flags = record.materialAndSurface.z;
    material.alphaCutoff = record.albedoAndAlphaCutoff.w;
    material.diffuseAlbedo = saturate(record.albedoAndAlphaCutoff.xyz);
    material.roughness = saturate(record.geometricNormalAndRoughness.w);
    material.specularF0 = max(record.specularF0AndReserved.xyz, float3(0.0, 0.0, 0.0));
    PathTraceCleanRoomApplyLiveMaterialClassifierBsdf(
        material.materialIndex,
        material.diffuseAlbedo,
        material.specularF0,
        material.roughness);
    material.opacity = saturate(record.shadingNormalAndOpacity.w);
    material.emissiveRadiance = max(record.emissiveAndHeight.xyz, float3(0.0, 0.0, 0.0));
    material.emissiveTextureIndex = record.instancePrimitiveObject.w;
    surface.materialIndex = resolvedMaterialIndex;
    surface.material = material;
    return surface;
}

RAB_Surface PathTraceCleanRoomSurfaceForView(PathTracePrimarySurfaceRecord record)
{
    if (CleanRtxdiDiView == 16u ||
        PathTraceCleanRoomLiveMaterialClassifierBsdfActive(PathTraceCleanRoomResolveLiveMaterialIndex(record)))
    {
        return PathTraceCleanRoomMaterialSurfaceFromRecord(record);
    }
    return PathTraceCleanRoomSurfaceFromRecord(record);
}

PathTracePrimarySurfaceRecord PathTraceCleanRtxdiDiPackResolvedPrimarySurfaceRecord(RAB_Surface surface)
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
        surface.flags);
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

float PathTraceCleanRtxdiDiResolvedSurfaceRRDepth(RAB_Surface surface)
{
    const float viewZ = dot(
        surface.worldPos - CleanRtxdiDiCameraOriginAndValid.xyz,
        CleanRtxdiDiCameraForwardAndTanX.xyz);
    return max(viewZ, 0.0);
}

uint PathTraceCleanRtxdiDiResolvedSurfaceResetMask(RAB_Surface surface)
{
    if (!RAB_IsSurfaceValid(surface))
    {
        return RT_RR_RESET_INVALID_SURFACE;
    }
    return RT_RR_RESET_OBJECT_MOTION_UNAVAILABLE;
}

void PathTraceCleanRtxdiDiWriteResolvedSurfaceRrGuides(uint2 pixel, RAB_Surface surface)
{
    PathTraceRRGuideAlbedo[pixel] = float4(saturate(surface.material.diffuseAlbedo), 1.0);
    PathTraceRRGuideSpecularAlbedo[pixel] = float4(saturate(surface.material.specularF0), 1.0);
    PathTraceRRGuideNormalRoughness[pixel] = float4(
        RAB_SafeNormalize(surface.shadingNormal, surface.geometryNormal),
        saturate(surface.material.roughness));
    PathTraceRRGuideDepth[pixel] = PathTraceCleanRtxdiDiResolvedSurfaceRRDepth(surface);
    PathTraceRRGuidePosition[pixel] = float4(surface.worldPos, 1.0);
    PathTraceRRMotionVectors[pixel] = float2(0.0, 0.0);
    PathTraceMotionVectors[pixel] = float4(0.0, 0.0, 0.0, 0.0);
    PathTraceMotionVectorMask[pixel] = 0u;
    PathTraceRRGuideResetMask[pixel] = PathTraceCleanRtxdiDiResolvedSurfaceResetMask(surface);
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
