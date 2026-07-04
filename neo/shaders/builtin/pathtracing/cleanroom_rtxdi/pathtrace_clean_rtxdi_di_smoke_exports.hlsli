#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_SMOKE_EXPORTS_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_SMOKE_EXPORTS_HLSLI

bool PathTraceCleanRoomMaterialDoesNotOccludeVisibility(uint materialIndex)
{
    if (materialIndex >= (uint)TextureInfo.z)
    {
        return false;
    }

    const PathTraceSmokeMaterial material = PathTraceCleanRoomLoadSmokeMaterial(materialIndex);
    PathTraceMaterialFeature feature;
    if (PathTraceCleanRtxdiDiLoadMaterialFeature(materialIndex, feature) &&
        feature.materialKind == RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_GLASS &&
        (feature.materialCaps & RT_PATH_TRACE_MATERIAL_CAP_PATH_TRANSMISSION) != 0u)
    {
        return true;
    }

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

#if defined(CLEAN_RTXDI_DI_TRACE_HIT_SURFACE_ADAPTER)
static const uint RT_SMOKE_MATERIAL_ALPHA_TEST_TRANSMISSION = 0x00000001u;
static const uint RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_DARK_KEY_TRANSMISSION = 0x00000100u;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_OBJECT_GLASS = 1u;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_PORTAL_WINDOW = 4u;

bool PathTraceCleanRoomTransmissionMaterialAlwaysTransmits(uint materialIndex)
{
    if (materialIndex >= (uint)TextureInfo.z)
    {
        return false;
    }

    const PathTraceSmokeMaterial material = PathTraceCleanRoomLoadSmokeMaterial(materialIndex);
    PathTraceMaterialFeature feature;
    if (PathTraceCleanRtxdiDiLoadMaterialFeature(materialIndex, feature) &&
        feature.materialKind == RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_GLASS &&
        (feature.materialCaps & RT_PATH_TRACE_MATERIAL_CAP_PATH_TRANSMISSION) != 0u)
    {
        return true;
    }

    const uint alwaysTransparentFlags =
        RT_SMOKE_MATERIAL_ADDITIVE_DECAL |
        RT_SMOKE_MATERIAL_FILTER_DECAL |
        RT_SMOKE_MATERIAL_PORTAL_WINDOW_FALLBACK |
        RT_SMOKE_MATERIAL_OBJECT_GLASS_FALLBACK |
        RT_SMOKE_MATERIAL_ADDITIVE_DECAL_WHITE_KEY;
    return (material.flags & alwaysTransparentFlags) != 0u;
}

float2 PathTraceCleanRoomTransmissionInterpolateTexCoord(uint instanceId, uint primitiveIndex, float2 hitBarycentrics)
{
    const float3 barycentrics = float3(
        1.0 - hitBarycentrics.x - hitBarycentrics.y,
        hitBarycentrics.x,
        hitBarycentrics.y);

    if (instanceId == 0u || instanceId == 1u)
    {
        const uint vertexCount = instanceId == 0u ? (uint)max(CleanRtxdiDiGeometryInfo0.x, 0.0) : (uint)max(CleanRtxdiDiGeometryInfo0.w, 0.0);
        const uint indexCount = instanceId == 0u ? (uint)max(CleanRtxdiDiGeometryInfo0.y, 0.0) : (uint)max(CleanRtxdiDiGeometryInfo1.x, 0.0);
        const uint triangleCount = instanceId == 0u ? (uint)max(CleanRtxdiDiGeometryInfo0.z, 0.0) : (uint)max(CleanRtxdiDiGeometryInfo1.y, 0.0);
        const uint indexOffset = primitiveIndex * 3u;
        if (primitiveIndex >= triangleCount || indexOffset + 2u >= indexCount)
        {
            return float2(0.0, 0.0);
        }

        const uint i0 = instanceId == 0u ? SmokeStaticIndices[indexOffset + 0u] : SmokeDynamicIndices[indexOffset + 0u];
        const uint i1 = instanceId == 0u ? SmokeStaticIndices[indexOffset + 1u] : SmokeDynamicIndices[indexOffset + 1u];
        const uint i2 = instanceId == 0u ? SmokeStaticIndices[indexOffset + 2u] : SmokeDynamicIndices[indexOffset + 2u];
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
        {
            return float2(0.0, 0.0);
        }

        const float2 uv0 = (instanceId == 0u ? SmokeStaticVertices[i0].texCoord : SmokeDynamicVertices[i0].texCoord).xy;
        const float2 uv1 = (instanceId == 0u ? SmokeStaticVertices[i1].texCoord : SmokeDynamicVertices[i1].texCoord).xy;
        const float2 uv2 = (instanceId == 0u ? SmokeStaticVertices[i2].texCoord : SmokeDynamicVertices[i2].texCoord).xy;
        return uv0 * barycentrics.x + uv1 * barycentrics.y + uv2 * barycentrics.z;
    }

    const uint routeInstanceIndex = instanceId - 2u;
    const uint rigidRouteInstanceCount = (uint)max(ToyPathInfo.w, 0.0);
    if (instanceId < 2u || routeInstanceIndex >= rigidRouteInstanceCount)
    {
        return float2(0.0, 0.0);
    }

    const PathTraceRigidRouteInstance routeInstance = SmokeRigidRouteInstances[routeInstanceIndex];
    const uint routeIndexOffset = routeInstance.indexOffset + primitiveIndex * 3u;
    const uint rigidRouteVertexCount = (uint)max(CleanRtxdiDiGeometryInfo1.z, 0.0);
    const uint rigidRouteIndexCount = (uint)max(CleanRtxdiDiGeometryInfo1.w, 0.0);
    if (primitiveIndex >= routeInstance.triangleCount ||
        routeIndexOffset + 2u >= rigidRouteIndexCount)
    {
        return float2(0.0, 0.0);
    }

    const uint i0 = SmokeRigidRouteIndices[routeIndexOffset + 0u];
    const uint i1 = SmokeRigidRouteIndices[routeIndexOffset + 1u];
    const uint i2 = SmokeRigidRouteIndices[routeIndexOffset + 2u];
    if (i0 >= routeInstance.vertexCount || i1 >= routeInstance.vertexCount || i2 >= routeInstance.vertexCount ||
        routeInstance.vertexOffset + i0 >= rigidRouteVertexCount ||
        routeInstance.vertexOffset + i1 >= rigidRouteVertexCount ||
        routeInstance.vertexOffset + i2 >= rigidRouteVertexCount)
    {
        return float2(0.0, 0.0);
    }

    const float2 uv0 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i0].texCoord.xy;
    const float2 uv1 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i1].texCoord.xy;
    const float2 uv2 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i2].texCoord.xy;
    return uv0 * barycentrics.x + uv1 * barycentrics.y + uv2 * barycentrics.z;
}

float3 PathTraceCleanRoomTransmissionConvertYCoCgToRGB(float4 ycocg)
{
    ycocg.z = (ycocg.z * 31.875) + 1.0;
    ycocg.z = 1.0 / ycocg.z;
    ycocg.xy *= ycocg.z;
    return saturate(float3(
        dot(ycocg, float4(1.0, -1.0, 0.0, 1.0)),
        dot(ycocg, float4(0.0, 1.0, -0.50196078, 1.0)),
        dot(ycocg, float4(-1.0, -1.0, 1.00392156, 1.0))));
}

float4 PathTraceCleanRoomTransmissionSampleDecodedDiffuse(PathTraceSmokeMaterial material, float2 texCoord)
{
    if ((material.flags & RT_SMOKE_MATERIAL_FORCE_DEBUG_ALBEDO) != 0u)
    {
        return saturate(material.debugAlbedo);
    }

    float4 texel = PathTraceCleanRoomSampleTexture(
        material.diffuseTextureIndex,
        material.textureWidth,
        material.textureHeight,
        texCoord,
        material.debugAlbedo);
    const bool textureDecodeEnabled = (((uint)TextureInfo.w) & 4u) != 0u;
    if (textureDecodeEnabled && (material.flags & RT_SMOKE_MATERIAL_DIFFUSE_YCOCG) != 0u)
    {
        texel.rgb = PathTraceCleanRoomTransmissionConvertYCoCgToRGB(texel);
    }
    return saturate(texel);
}

float PathTraceCleanRoomTransmissionAlphaCoverage(PathTraceSmokeMaterial material, float2 texCoord)
{
    if ((material.flags & RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_DARK_KEY_TRANSMISSION) != 0u)
    {
        const float3 decoded = PathTraceCleanRoomTransmissionSampleDecodedDiffuse(material, texCoord).rgb;
        return 1.0 - max(max(decoded.r, decoded.g), decoded.b);
    }

    if ((material.flags & RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_LUMA) != 0u)
    {
        const float3 decoded = PathTraceCleanRoomTransmissionSampleDecodedDiffuse(material, texCoord).rgb;
        return max(max(decoded.r, decoded.g), decoded.b);
    }

    if ((material.flags & RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_MAGENTA_KEY) != 0u)
    {
        const float3 decoded = PathTraceCleanRoomTransmissionSampleDecodedDiffuse(material, texCoord).rgb;
        const float keyDistance = max(abs(decoded.r - 1.0), max(abs(decoded.g), abs(decoded.b - 1.0)));
        return keyDistance <= 0.08 ? 0.0 : 1.0;
    }

    const float4 fallback = PathTraceCleanRoomTransmissionSampleDecodedDiffuse(material, texCoord);
    if (material.alphaTextureIndex != 0xffffffffu)
    {
        return PathTraceCleanRoomSampleTexture(
            material.alphaTextureIndex,
            material.alphaTextureWidth,
            material.alphaTextureHeight,
            texCoord,
            fallback).a;
    }
    return fallback.a;
}

bool PathTraceCleanRoomTriangleDoesNotOccludeTransmission(uint instanceId, uint primitiveIndex, uint materialIndex)
{
    if (PathTraceCleanRoomTransmissionMaterialAlwaysTransmits(materialIndex))
    {
        return true;
    }

    const uint triangleClassAndFlags =
        PathTraceCleanRtxdiDiTraceHitLoadTriangleClassAndFlags(instanceId, primitiveIndex);
    const uint surfaceClass = triangleClassAndFlags & RT_SMOKE_TRIANGLE_CLASS_MASK;
    const uint translucentSubtype =
        (triangleClassAndFlags & RT_SMOKE_TRANSLUCENT_SUBTYPE_MASK) >> RT_SMOKE_TRANSLUCENT_SUBTYPE_SHIFT;
    return surfaceClass == RT_SMOKE_SURFACE_CLASS_TRANSLUCENT &&
        (translucentSubtype == RT_SMOKE_TRANSLUCENT_SUBTYPE_OBJECT_GLASS ||
         translucentSubtype == RT_SMOKE_TRANSLUCENT_SUBTYPE_PORTAL_WINDOW);
}

bool PathTraceCleanRoomTransmissionAlphaRejectsHit(uint instanceId, uint primitiveIndex, float2 hitBarycentrics, uint materialIndex)
{
    if (materialIndex >= (uint)TextureInfo.z)
    {
        return false;
    }

    const PathTraceSmokeMaterial material = PathTraceCleanRoomLoadSmokeMaterial(materialIndex);
    if ((material.flags & RT_SMOKE_MATERIAL_ALPHA_TEST_TRANSMISSION) == 0u)
    {
        return false;
    }

    const float2 texCoord = PathTraceCleanRoomTransmissionInterpolateTexCoord(instanceId, primitiveIndex, hitBarycentrics);
    return PathTraceCleanRoomTransmissionAlphaCoverage(material, texCoord) < material.alphaCutoff;
}
#endif

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
#if defined(CLEAN_RTXDI_DI_TRACE_HIT_SURFACE_ADAPTER)
    if (payload.rayMode == 3u)
    {
        const uint instanceId = InstanceID();
        const uint primitiveIndex = PrimitiveIndex();
        const uint materialIndex = PathTraceCleanRoomLoadTriangleMaterialIndex(instanceId, primitiveIndex);
        const bool ignoredSource =
            instanceId == payload.ignoreInstanceId &&
            (primitiveIndex == payload.ignorePrimitiveIndex || materialIndex == payload.ignoreMaterialIndex);
        if (ignoredSource ||
            PathTraceCleanRoomTriangleDoesNotOccludeTransmission(instanceId, primitiveIndex, materialIndex) ||
            PathTraceCleanRoomTransmissionAlphaRejectsHit(instanceId, primitiveIndex, attributes.barycentrics, materialIndex))
        {
            IgnoreHit();
            return;
        }
    }
#endif

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
#if defined(CLEAN_RTXDI_DI_TRACE_HIT_SURFACE_ADAPTER)
    payload.hitInstanceId = InstanceID();
    payload.hitPrimitiveIndex = PrimitiveIndex();
    payload.hitMaterialId = PathTraceCleanRtxdiDiTraceHitLoadTriangleMaterialId(payload.hitInstanceId, payload.hitPrimitiveIndex);
    payload.hitMaterialIndex = PathTraceCleanRoomLoadTriangleMaterialIndex(payload.hitInstanceId, payload.hitPrimitiveIndex);
    payload.hitTriangleClassAndFlags = PathTraceCleanRtxdiDiTraceHitLoadTriangleClassAndFlags(payload.hitInstanceId, payload.hitPrimitiveIndex);
    payload.hitT = RayTCurrent();
    payload.hitBarycentrics = attributes.barycentrics;
#endif
}

[shader("closesthit")]
void ShadowClosestHit(inout PathTraceCleanRtxdiPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    payload.value = 1u;
}

#endif
