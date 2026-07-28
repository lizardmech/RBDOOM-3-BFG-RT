#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_HIT_SURFACE_ADAPTER_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_HIT_SURFACE_ADAPTER_HLSLI

#if defined(CLEAN_RTXDI_DI_TRACE_HIT_SURFACE_ADAPTER)

static const uint RT_SMOKE_MATERIAL_OVERRIDE_ZERO_ROUGHNESS_RESOLVED_SURFACE = 0x00000001u;
static const uint RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_DARK_KEY_RESOLVED_SURFACE = 0x00000100u;
static const uint RT_SMOKE_MATERIAL_SKY_ENVIRONMENT_RESOLVED_SURFACE = 0x00040000u;
static const uint RT_SMOKE_TEXTURE_FLAG_TOY_FAKE_PBR_SPECULAR_RESOLVED_SURFACE = 0x00000080u;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_OBJECT_GLASS_RESOLVED_SURFACE = 1u;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_PORTAL_WINDOW_RESOLVED_SURFACE = 4u;

float PathTraceCleanRtxdiDiSurfaceAdapterLinear1(float c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 PathTraceCleanRtxdiDiSurfaceAdapterLinear3(float3 c)
{
    return float3(
        PathTraceCleanRtxdiDiSurfaceAdapterLinear1(c.r),
        PathTraceCleanRtxdiDiSurfaceAdapterLinear1(c.g),
        PathTraceCleanRtxdiDiSurfaceAdapterLinear1(c.b));
}

bool PathTraceCleanRtxdiDiSurfaceAdapterToyFakePBRSpecularEnabled()
{
    return (((uint)TextureInfo.w) & RT_SMOKE_TEXTURE_FLAG_TOY_FAKE_PBR_SPECULAR_RESOLVED_SURFACE) != 0u;
}

void PathTraceCleanRtxdiDiSurfaceAdapterPBRFromSpecmap(float3 specMap, out float3 F0, out float roughness)
{
    const float specLum = dot(float3(0.2125, 0.7154, 0.0721), specMap);
    F0 = float3(0.04, 0.04, 0.04);
    const float contrastMid = 0.214;
    const float contrastAmount = 2.0;
    float contrast = saturate((specLum - contrastMid) / (1.0 - contrastMid));
    contrast += saturate(specLum / contrastMid) - 1.0;
    contrast = exp2(contrastAmount * contrast);
    F0 *= contrast;
    const float linearBrightness = PathTraceCleanRtxdiDiSurfaceAdapterLinear1(2.0 * specLum);
    const float specPow = max(0.0, ((8.0 * linearBrightness) / max(F0.y, 1.0e-4)) - 2.0);
    F0 *= min(1.0, linearBrightness / max(F0.y * 0.25, 1.0e-4));
    roughness = sqrt(2.0 / (specPow + 2.0));
    const float glossiness = saturate(1.0 - roughness);
    const float metallic = step(0.7, glossiness);
    const float3 glossColor = PathTraceCleanRtxdiDiSurfaceAdapterLinear3(specMap.rgb);
    F0 = lerp(F0, glossColor, metallic);
    roughness = sqrt(roughness);
}

struct PathTraceCleanRtxdiDiTraceHitSurface
{
    bool valid;
    float2 texCoord;
    float2 normalTexCoord;
    float3 geometricNormal;
    float3 shadingNormal;
    float3 tangent;
    float3 bitangent;
    float4 vertexColor;
    float4 vertexColorAdd;
};

float3 PathTraceCleanRtxdiDiTransformRouteVector(PathTraceRigidRouteInstance routeInstance, float3 localVector)
{
    return float3(
        dot(routeInstance.currentObjectToWorld0.xyz, localVector),
        dot(routeInstance.currentObjectToWorld1.xyz, localVector),
        dot(routeInstance.currentObjectToWorld2.xyz, localVector));
}

float3 PathTraceCleanRtxdiDiTransformRoutePoint(PathTraceRigidRouteInstance routeInstance, float3 localPoint)
{
    return float3(
        dot(routeInstance.currentObjectToWorld0.xyz, localPoint) + routeInstance.currentObjectToWorld0.w,
        dot(routeInstance.currentObjectToWorld1.xyz, localPoint) + routeInstance.currentObjectToWorld1.w,
        dot(routeInstance.currentObjectToWorld2.xyz, localPoint) + routeInstance.currentObjectToWorld2.w);
}

float PathTraceCleanRtxdiDiRouteHandedness(PathTraceRigidRouteInstance routeInstance)
{
    const float3 x = routeInstance.currentObjectToWorld0.xyz;
    const float3 y = routeInstance.currentObjectToWorld1.xyz;
    const float3 z = routeInstance.currentObjectToWorld2.xyz;
    return dot(cross(x, y), z) < 0.0 ? -1.0 : 1.0;
}

bool PathTraceCleanRtxdiDiBuildTraceHitTangentBasis(
    float3 normal,
    float4 capturedTangent,
    float4 capturedBitangent,
    float handednessSign,
    out float3 tangent,
    out float3 bitangent)
{
    const float3 tangentFallback = RAB_BuildPerpendicular(normal);
    const float3 bitangentFallback = RAB_SafeNormalize(cross(normal, tangentFallback), float3(0.0, 1.0, 0.0));
    const float3 projectedTangent = capturedTangent.xyz - normal * dot(normal, capturedTangent.xyz);
    const float tangentLengthSquared = dot(projectedTangent, projectedTangent);
    if (tangentLengthSquared <= 1.0e-8 || abs(capturedTangent.w) < 0.5)
    {
        tangent = tangentFallback;
        bitangent = bitangentFallback;
        return false;
    }

    tangent = projectedTangent * rsqrt(tangentLengthSquared);
    const float3 projectedBitangent =
        capturedBitangent.xyz -
        normal * dot(normal, capturedBitangent.xyz) -
        tangent * dot(tangent, capturedBitangent.xyz);
    const float bitangentLengthSquared = dot(projectedBitangent, projectedBitangent);
    if (bitangentLengthSquared > 1.0e-8)
    {
        bitangent = projectedBitangent * rsqrt(bitangentLengthSquared);
    }
    else
    {
        const float bitangentSign = (capturedTangent.w < 0.0 ? -1.0 : 1.0) * handednessSign;
        bitangent = RAB_SafeNormalize(cross(normal, tangent) * bitangentSign, bitangentFallback);
    }
    return true;
}

PathTraceCleanRtxdiDiTraceHitSurface PathTraceCleanRtxdiDiEmptyTraceHitSurface()
{
    PathTraceCleanRtxdiDiTraceHitSurface surface;
    surface.valid = false;
    surface.texCoord = float2(0.0, 0.0);
    surface.normalTexCoord = float2(0.0, 0.0);
    surface.geometricNormal = float3(0.0, 0.0, 1.0);
    surface.shadingNormal = float3(0.0, 0.0, 1.0);
    surface.tangent = float3(1.0, 0.0, 0.0);
    surface.bitangent = float3(0.0, 1.0, 0.0);
    surface.vertexColor = float4(1.0, 1.0, 1.0, 1.0);
    surface.vertexColorAdd = float4(0.0, 0.0, 0.0, 0.0);
    return surface;
}

void PathTraceCleanRtxdiDiPopulateIdentityTraceHitSurface(
    PathTraceSmokeVertex v0,
    PathTraceSmokeVertex v1,
    PathTraceSmokeVertex v2,
    float3 barycentrics,
    inout PathTraceCleanRtxdiDiTraceHitSurface surface)
{
    surface.texCoord =
        v0.texCoord.xy * barycentrics.x +
        v1.texCoord.xy * barycentrics.y +
        v2.texCoord.xy * barycentrics.z;
    surface.normalTexCoord =
        v0.texCoord.zw * barycentrics.x +
        v1.texCoord.zw * barycentrics.y +
        v2.texCoord.zw * barycentrics.z;
    surface.vertexColor =
        v0.color * barycentrics.x +
        v1.color * barycentrics.y +
        v2.color * barycentrics.z;
    surface.vertexColorAdd =
        v0.color2 * barycentrics.x +
        v1.color2 * barycentrics.y +
        v2.color2 * barycentrics.z;
    surface.geometricNormal = RAB_SafeNormalize(
        cross(
            v1.position.xyz - v0.position.xyz,
            v2.position.xyz - v0.position.xyz),
        surface.geometricNormal);
    surface.shadingNormal = RAB_SafeNormalize(
        v0.normal.xyz * barycentrics.x +
            v1.normal.xyz * barycentrics.y +
            v2.normal.xyz * barycentrics.z,
        surface.geometricNormal);
    const float4 capturedTangent =
        v0.tangent * barycentrics.x +
        v1.tangent * barycentrics.y +
        v2.tangent * barycentrics.z;
    const float4 capturedBitangent =
        v0.bitangent * barycentrics.x +
        v1.bitangent * barycentrics.y +
        v2.bitangent * barycentrics.z;
    PathTraceCleanRtxdiDiBuildTraceHitTangentBasis(
        surface.shadingNormal,
        capturedTangent,
        capturedBitangent,
        1.0,
        surface.tangent,
        surface.bitangent);
    surface.valid = true;
}

bool PathTraceCleanRtxdiDiLoadTraceHitSurface(
    PathTraceCleanRtxdiPayload payload,
    out PathTraceCleanRtxdiDiTraceHitSurface surface)
{
    surface = PathTraceCleanRtxdiDiEmptyTraceHitSurface();
    const float3 barycentrics = float3(
        1.0 - payload.hitBarycentrics.x - payload.hitBarycentrics.y,
        payload.hitBarycentrics.x,
        payload.hitBarycentrics.y);

    if (PathTraceIsStaticBucketRouteInstance(
            payload.hitInstanceId,
            CleanRtxdiDiStaticBucketRouteInfo))
    {
        PathTraceStaticBucketRouteRecord route;
        uint packedTriangleIndex;
        uint3 packedVertexIndexes;
        if (!PathTraceCleanRtxdiDiTryLoadStaticBucketTriangleRoute(
                payload.hitInstanceId,
                payload.hitPrimitiveIndex,
                route,
                packedTriangleIndex,
                packedVertexIndexes))
        {
            return false;
        }

        PathTraceCleanRtxdiDiPopulateIdentityTraceHitSurface(
            SmokeStaticBucketVertices[packedVertexIndexes.x],
            SmokeStaticBucketVertices[packedVertexIndexes.y],
            SmokeStaticBucketVertices[packedVertexIndexes.z],
            barycentrics,
            surface);
        return true;
    }

    if (payload.hitInstanceId == 0u || payload.hitInstanceId == 1u)
    {
        const uint vertexCount = payload.hitInstanceId == 0u ? (uint)max(CleanRtxdiDiGeometryInfo0.x, 0.0) : (uint)max(CleanRtxdiDiGeometryInfo0.w, 0.0);
        const uint indexCount = payload.hitInstanceId == 0u ? (uint)max(CleanRtxdiDiGeometryInfo0.y, 0.0) : (uint)max(CleanRtxdiDiGeometryInfo1.x, 0.0);
        const uint triangleCount = payload.hitInstanceId == 0u ? (uint)max(CleanRtxdiDiGeometryInfo0.z, 0.0) : (uint)max(CleanRtxdiDiGeometryInfo1.y, 0.0);
        const uint indexOffset = payload.hitPrimitiveIndex * 3u;
        if (payload.hitPrimitiveIndex >= triangleCount || indexOffset + 2u >= indexCount)
        {
            return false;
        }

        const uint i0 = payload.hitInstanceId == 0u ? SmokeStaticIndices[indexOffset + 0u] : SmokeDynamicIndices[indexOffset + 0u];
        const uint i1 = payload.hitInstanceId == 0u ? SmokeStaticIndices[indexOffset + 1u] : SmokeDynamicIndices[indexOffset + 1u];
        const uint i2 = payload.hitInstanceId == 0u ? SmokeStaticIndices[indexOffset + 2u] : SmokeDynamicIndices[indexOffset + 2u];
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
        {
            return false;
        }

        const float2 uv0 = (payload.hitInstanceId == 0u ? SmokeStaticVertices[i0].texCoord : SmokeDynamicVertices[i0].texCoord).xy;
        const float2 uv1 = (payload.hitInstanceId == 0u ? SmokeStaticVertices[i1].texCoord : SmokeDynamicVertices[i1].texCoord).xy;
        const float2 uv2 = (payload.hitInstanceId == 0u ? SmokeStaticVertices[i2].texCoord : SmokeDynamicVertices[i2].texCoord).xy;
        const float2 normalUv0 = (payload.hitInstanceId == 0u ? SmokeStaticVertices[i0].texCoord : SmokeDynamicVertices[i0].texCoord).zw;
        const float2 normalUv1 = (payload.hitInstanceId == 0u ? SmokeStaticVertices[i1].texCoord : SmokeDynamicVertices[i1].texCoord).zw;
        const float2 normalUv2 = (payload.hitInstanceId == 0u ? SmokeStaticVertices[i2].texCoord : SmokeDynamicVertices[i2].texCoord).zw;
        const float3 p0 = (payload.hitInstanceId == 0u ? SmokeStaticVertices[i0].position : SmokeDynamicVertices[i0].position).xyz;
        const float3 p1 = (payload.hitInstanceId == 0u ? SmokeStaticVertices[i1].position : SmokeDynamicVertices[i1].position).xyz;
        const float3 p2 = (payload.hitInstanceId == 0u ? SmokeStaticVertices[i2].position : SmokeDynamicVertices[i2].position).xyz;
        const float3 n0 = (payload.hitInstanceId == 0u ? SmokeStaticVertices[i0].normal : SmokeDynamicVertices[i0].normal).xyz;
        const float3 n1 = (payload.hitInstanceId == 0u ? SmokeStaticVertices[i1].normal : SmokeDynamicVertices[i1].normal).xyz;
        const float3 n2 = (payload.hitInstanceId == 0u ? SmokeStaticVertices[i2].normal : SmokeDynamicVertices[i2].normal).xyz;
        const float4 c0 = payload.hitInstanceId == 0u ? SmokeStaticVertices[i0].color : SmokeDynamicVertices[i0].color;
        const float4 c1 = payload.hitInstanceId == 0u ? SmokeStaticVertices[i1].color : SmokeDynamicVertices[i1].color;
        const float4 c2 = payload.hitInstanceId == 0u ? SmokeStaticVertices[i2].color : SmokeDynamicVertices[i2].color;
        const float4 c20 = payload.hitInstanceId == 0u ? SmokeStaticVertices[i0].color2 : SmokeDynamicVertices[i0].color2;
        const float4 c21 = payload.hitInstanceId == 0u ? SmokeStaticVertices[i1].color2 : SmokeDynamicVertices[i1].color2;
        const float4 c22 = payload.hitInstanceId == 0u ? SmokeStaticVertices[i2].color2 : SmokeDynamicVertices[i2].color2;
        const float4 t0 = payload.hitInstanceId == 0u ? SmokeStaticVertices[i0].tangent : SmokeDynamicVertices[i0].tangent;
        const float4 t1 = payload.hitInstanceId == 0u ? SmokeStaticVertices[i1].tangent : SmokeDynamicVertices[i1].tangent;
        const float4 t2 = payload.hitInstanceId == 0u ? SmokeStaticVertices[i2].tangent : SmokeDynamicVertices[i2].tangent;
        const float4 b0 = payload.hitInstanceId == 0u ? SmokeStaticVertices[i0].bitangent : SmokeDynamicVertices[i0].bitangent;
        const float4 b1 = payload.hitInstanceId == 0u ? SmokeStaticVertices[i1].bitangent : SmokeDynamicVertices[i1].bitangent;
        const float4 b2 = payload.hitInstanceId == 0u ? SmokeStaticVertices[i2].bitangent : SmokeDynamicVertices[i2].bitangent;

        surface.texCoord = uv0 * barycentrics.x + uv1 * barycentrics.y + uv2 * barycentrics.z;
        surface.normalTexCoord = normalUv0 * barycentrics.x + normalUv1 * barycentrics.y + normalUv2 * barycentrics.z;
        surface.vertexColor = c0 * barycentrics.x + c1 * barycentrics.y + c2 * barycentrics.z;
        surface.vertexColorAdd = c20 * barycentrics.x + c21 * barycentrics.y + c22 * barycentrics.z;
        surface.geometricNormal = RAB_SafeNormalize(cross(p1 - p0, p2 - p0), surface.geometricNormal);
        surface.shadingNormal = RAB_SafeNormalize(n0 * barycentrics.x + n1 * barycentrics.y + n2 * barycentrics.z, surface.geometricNormal);
        const float4 capturedTangent = t0 * barycentrics.x + t1 * barycentrics.y + t2 * barycentrics.z;
        const float4 capturedBitangent = b0 * barycentrics.x + b1 * barycentrics.y + b2 * barycentrics.z;
        PathTraceCleanRtxdiDiBuildTraceHitTangentBasis(
            surface.shadingNormal,
            capturedTangent,
            capturedBitangent,
            1.0,
            surface.tangent,
            surface.bitangent);
        surface.valid = true;
        return true;
    }

    if (PathTraceIsSkinnedHitRouteInstance(payload.hitInstanceId))
    {
        PathTraceSkinnedHitRouteGpuRecord route;
        PathTraceSkinnedHitRouteGpuTriangle routeTriangle;
        uint vertexIndex0;
        uint vertexIndex1;
        uint vertexIndex2;
        if (!PathTraceLoadSkinnedHitRouteTriangleData(
                payload.hitInstanceId,
                payload.hitPrimitiveIndex,
                route,
                routeTriangle,
                vertexIndex0,
                vertexIndex1,
                vertexIndex2))
        {
            return false;
        }
        const PathTraceSmokeVertex v0 =
            SmokeSkinnedCurrentVertices[vertexIndex0];
        const PathTraceSmokeVertex v1 =
            SmokeSkinnedCurrentVertices[vertexIndex1];
        const PathTraceSmokeVertex v2 =
            SmokeSkinnedCurrentVertices[vertexIndex2];
        surface.texCoord =
            v0.texCoord.xy * barycentrics.x +
            v1.texCoord.xy * barycentrics.y +
            v2.texCoord.xy * barycentrics.z;
        surface.normalTexCoord =
            v0.texCoord.zw * barycentrics.x +
            v1.texCoord.zw * barycentrics.y +
            v2.texCoord.zw * barycentrics.z;
        surface.vertexColor =
            v0.color * barycentrics.x +
            v1.color * barycentrics.y +
            v2.color * barycentrics.z;
        surface.vertexColorAdd =
            v0.color2 * barycentrics.x +
            v1.color2 * barycentrics.y +
            v2.color2 * barycentrics.z;
        surface.geometricNormal = RAB_SafeNormalize(
            cross(
                v1.position.xyz - v0.position.xyz,
                v2.position.xyz - v0.position.xyz),
            surface.geometricNormal);
        surface.shadingNormal = RAB_SafeNormalize(
            v0.normal.xyz * barycentrics.x +
                v1.normal.xyz * barycentrics.y +
                v2.normal.xyz * barycentrics.z,
            surface.geometricNormal);
        const float4 capturedTangent =
            v0.tangent * barycentrics.x +
            v1.tangent * barycentrics.y +
            v2.tangent * barycentrics.z;
        const float4 capturedBitangent =
            v0.bitangent * barycentrics.x +
            v1.bitangent * barycentrics.y +
            v2.bitangent * barycentrics.z;
        PathTraceCleanRtxdiDiBuildTraceHitTangentBasis(
            surface.shadingNormal,
            capturedTangent,
            capturedBitangent,
            1.0,
            surface.tangent,
            surface.bitangent);
        surface.valid = true;
        return true;
    }

    const uint routeInstanceIndex = payload.hitInstanceId - 2u;
    const uint rigidRouteInstanceCount = (uint)max(ToyPathInfo.w, 0.0);
    if (!PathTraceIsRigidHitRouteInstance(payload.hitInstanceId) ||
        routeInstanceIndex >= rigidRouteInstanceCount)
    {
        return false;
    }

    const PathTraceRigidRouteInstance routeInstance = SmokeRigidRouteInstances[routeInstanceIndex];
    const uint routeIndexOffset = routeInstance.indexOffset + payload.hitPrimitiveIndex * 3u;
    const uint rigidRouteVertexCount = (uint)max(CleanRtxdiDiGeometryInfo1.z, 0.0);
    const uint rigidRouteIndexCount = (uint)max(CleanRtxdiDiGeometryInfo1.w, 0.0);
    if (payload.hitPrimitiveIndex >= routeInstance.triangleCount ||
        routeIndexOffset + 2u >= rigidRouteIndexCount)
    {
        return false;
    }

    const uint i0 = SmokeRigidRouteIndices[routeIndexOffset + 0u];
    const uint i1 = SmokeRigidRouteIndices[routeIndexOffset + 1u];
    const uint i2 = SmokeRigidRouteIndices[routeIndexOffset + 2u];
    if (i0 >= routeInstance.vertexCount || i1 >= routeInstance.vertexCount || i2 >= routeInstance.vertexCount ||
        routeInstance.vertexOffset + i0 >= rigidRouteVertexCount ||
        routeInstance.vertexOffset + i1 >= rigidRouteVertexCount ||
        routeInstance.vertexOffset + i2 >= rigidRouteVertexCount)
    {
        return false;
    }

    const float2 uv0 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i0].texCoord.xy;
    const float2 uv1 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i1].texCoord.xy;
    const float2 uv2 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i2].texCoord.xy;
    const float2 normalUv0 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i0].texCoord.zw;
    const float2 normalUv1 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i1].texCoord.zw;
    const float2 normalUv2 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i2].texCoord.zw;
    const float3 p0 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i0].position.xyz;
    const float3 p1 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i1].position.xyz;
    const float3 p2 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i2].position.xyz;
    const float3 n0 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i0].normal.xyz;
    const float3 n1 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i1].normal.xyz;
    const float3 n2 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i2].normal.xyz;
    const float4 c0 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i0].color;
    const float4 c1 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i1].color;
    const float4 c2 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i2].color;
    const float4 c20 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i0].color2;
    const float4 c21 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i1].color2;
    const float4 c22 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i2].color2;
    const float4 t0 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i0].tangent;
    const float4 t1 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i1].tangent;
    const float4 t2 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i2].tangent;
    const float4 b0 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i0].bitangent;
    const float4 b1 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i1].bitangent;
    const float4 b2 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i2].bitangent;
    const float3 objectGeometricNormal = RAB_SafeNormalize(cross(p1 - p0, p2 - p0), float3(0.0, 0.0, 1.0));
    const float3 objectShadingNormal = RAB_SafeNormalize(n0 * barycentrics.x + n1 * barycentrics.y + n2 * barycentrics.z, objectGeometricNormal);
    const float4 capturedTangent = t0 * barycentrics.x + t1 * barycentrics.y + t2 * barycentrics.z;
    const float4 capturedBitangent = b0 * barycentrics.x + b1 * barycentrics.y + b2 * barycentrics.z;
    float3 objectTangent;
    float3 objectBitangent;
    PathTraceCleanRtxdiDiBuildTraceHitTangentBasis(
        objectShadingNormal,
        capturedTangent,
        capturedBitangent,
        PathTraceCleanRtxdiDiRouteHandedness(routeInstance),
        objectTangent,
        objectBitangent);

    surface.texCoord = uv0 * barycentrics.x + uv1 * barycentrics.y + uv2 * barycentrics.z;
    surface.normalTexCoord = normalUv0 * barycentrics.x + normalUv1 * barycentrics.y + normalUv2 * barycentrics.z;
    surface.vertexColor = c0 * barycentrics.x + c1 * barycentrics.y + c2 * barycentrics.z;
    surface.vertexColorAdd = c20 * barycentrics.x + c21 * barycentrics.y + c22 * barycentrics.z;
    surface.geometricNormal = RAB_SafeNormalize(PathTraceCleanRtxdiDiTransformRouteVector(routeInstance, objectGeometricNormal), surface.geometricNormal);
    surface.shadingNormal = RAB_SafeNormalize(PathTraceCleanRtxdiDiTransformRouteVector(routeInstance, objectShadingNormal), surface.geometricNormal);
    surface.tangent = RAB_SafeNormalize(
        PathTraceCleanRtxdiDiTransformRouteVector(routeInstance, objectTangent),
        RAB_BuildPerpendicular(surface.shadingNormal));
    surface.bitangent = RAB_SafeNormalize(PathTraceCleanRtxdiDiTransformRouteVector(routeInstance, objectBitangent), RAB_SafeNormalize(cross(surface.shadingNormal, surface.tangent), float3(0.0, 1.0, 0.0)));
    surface.valid = true;
    return true;
}

bool PathTraceCleanRtxdiDiComputeTriangleBarycentrics(
    float3 position,
    float3 p0,
    float3 p1,
    float3 p2,
    out float3 barycentrics)
{
    barycentrics = float3(1.0, 0.0, 0.0);
    const float3 v0 = p1 - p0;
    const float3 v1 = p2 - p0;
    const float3 v2 = position - p0;
    const float d00 = dot(v0, v0);
    const float d01 = dot(v0, v1);
    const float d11 = dot(v1, v1);
    const float d20 = dot(v2, v0);
    const float d21 = dot(v2, v1);
    const float denom = d00 * d11 - d01 * d01;
    if (abs(denom) <= 1.0e-10)
    {
        return false;
    }

    const float v = (d11 * d20 - d01 * d21) / denom;
    const float w = (d00 * d21 - d01 * d20) / denom;
    barycentrics = float3(1.0 - v - w, v, w);
    if (!all(barycentrics == barycentrics))
    {
        return false;
    }

    barycentrics = max(barycentrics, float3(0.0, 0.0, 0.0));
    const float sum = barycentrics.x + barycentrics.y + barycentrics.z;
    if (sum <= 1.0e-8)
    {
        return false;
    }
    barycentrics /= sum;
    return true;
}

bool PathTraceCleanRtxdiDiReconstructPrimarySurfaceBarycentrics(
    RAB_Surface primarySurface,
    out float2 hitBarycentrics)
{
    hitBarycentrics = float2(0.0, 0.0);
    const uint instanceId = primarySurface.instanceId;
    const uint primitiveIndex = primarySurface.primitiveIndex;

    if (PathTraceIsStaticBucketRouteInstance(
            instanceId,
            CleanRtxdiDiStaticBucketRouteInfo))
    {
        PathTraceStaticBucketRouteRecord route;
        uint packedTriangleIndex;
        uint3 packedVertexIndexes;
        if (!PathTraceCleanRtxdiDiTryLoadStaticBucketTriangleRoute(
                instanceId,
                primitiveIndex,
                route,
                packedTriangleIndex,
                packedVertexIndexes))
        {
            return false;
        }

        float3 barycentrics;
        if (!PathTraceCleanRtxdiDiComputeTriangleBarycentrics(
                primarySurface.worldPos,
                SmokeStaticBucketVertices[
                    packedVertexIndexes.x].position.xyz,
                SmokeStaticBucketVertices[
                    packedVertexIndexes.y].position.xyz,
                SmokeStaticBucketVertices[
                    packedVertexIndexes.z].position.xyz,
                barycentrics))
        {
            return false;
        }
        hitBarycentrics = barycentrics.yz;
        return true;
    }

    if (instanceId == 0u || instanceId == 1u)
    {
        const uint vertexCount = instanceId == 0u ? (uint)max(CleanRtxdiDiGeometryInfo0.x, 0.0) : (uint)max(CleanRtxdiDiGeometryInfo0.w, 0.0);
        const uint indexCount = instanceId == 0u ? (uint)max(CleanRtxdiDiGeometryInfo0.y, 0.0) : (uint)max(CleanRtxdiDiGeometryInfo1.x, 0.0);
        const uint triangleCount = instanceId == 0u ? (uint)max(CleanRtxdiDiGeometryInfo0.z, 0.0) : (uint)max(CleanRtxdiDiGeometryInfo1.y, 0.0);
        const uint indexOffset = primitiveIndex * 3u;
        if (primitiveIndex >= triangleCount || indexOffset + 2u >= indexCount)
        {
            return false;
        }

        const uint i0 = instanceId == 0u ? SmokeStaticIndices[indexOffset + 0u] : SmokeDynamicIndices[indexOffset + 0u];
        const uint i1 = instanceId == 0u ? SmokeStaticIndices[indexOffset + 1u] : SmokeDynamicIndices[indexOffset + 1u];
        const uint i2 = instanceId == 0u ? SmokeStaticIndices[indexOffset + 2u] : SmokeDynamicIndices[indexOffset + 2u];
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
        {
            return false;
        }

        const float3 p0 = (instanceId == 0u ? SmokeStaticVertices[i0].position : SmokeDynamicVertices[i0].position).xyz;
        const float3 p1 = (instanceId == 0u ? SmokeStaticVertices[i1].position : SmokeDynamicVertices[i1].position).xyz;
        const float3 p2 = (instanceId == 0u ? SmokeStaticVertices[i2].position : SmokeDynamicVertices[i2].position).xyz;
        float3 barycentrics;
        if (!PathTraceCleanRtxdiDiComputeTriangleBarycentrics(primarySurface.worldPos, p0, p1, p2, barycentrics))
        {
            return false;
        }
        hitBarycentrics = barycentrics.yz;
        return true;
    }

    if (PathTraceIsSkinnedHitRouteInstance(instanceId))
    {
        PathTraceSkinnedHitRouteGpuRecord route;
        PathTraceSkinnedHitRouteGpuTriangle routeTriangle;
        uint vertexIndex0;
        uint vertexIndex1;
        uint vertexIndex2;
        if (!PathTraceLoadSkinnedHitRouteTriangleData(
                instanceId,
                primitiveIndex,
                route,
                routeTriangle,
                vertexIndex0,
                vertexIndex1,
                vertexIndex2))
        {
            return false;
        }
        const float3 p0 =
            SmokeSkinnedCurrentVertices[vertexIndex0].position.xyz;
        const float3 p1 =
            SmokeSkinnedCurrentVertices[vertexIndex1].position.xyz;
        const float3 p2 =
            SmokeSkinnedCurrentVertices[vertexIndex2].position.xyz;
        float3 barycentrics;
        if (!PathTraceCleanRtxdiDiComputeTriangleBarycentrics(
                primarySurface.worldPos,
                p0,
                p1,
                p2,
                barycentrics))
        {
            return false;
        }
        hitBarycentrics = barycentrics.yz;
        return true;
    }

    const uint routeInstanceIndex = instanceId - 2u;
    const uint rigidRouteInstanceCount = (uint)max(ToyPathInfo.w, 0.0);
    if (!PathTraceIsRigidHitRouteInstance(instanceId) ||
        routeInstanceIndex >= rigidRouteInstanceCount)
    {
        return false;
    }

    const PathTraceRigidRouteInstance routeInstance = SmokeRigidRouteInstances[routeInstanceIndex];
    const uint routeIndexOffset = routeInstance.indexOffset + primitiveIndex * 3u;
    const uint rigidRouteVertexCount = (uint)max(CleanRtxdiDiGeometryInfo1.z, 0.0);
    const uint rigidRouteIndexCount = (uint)max(CleanRtxdiDiGeometryInfo1.w, 0.0);
    if (primitiveIndex >= routeInstance.triangleCount ||
        routeIndexOffset + 2u >= rigidRouteIndexCount)
    {
        return false;
    }

    const uint i0 = SmokeRigidRouteIndices[routeIndexOffset + 0u];
    const uint i1 = SmokeRigidRouteIndices[routeIndexOffset + 1u];
    const uint i2 = SmokeRigidRouteIndices[routeIndexOffset + 2u];
    if (i0 >= routeInstance.vertexCount || i1 >= routeInstance.vertexCount || i2 >= routeInstance.vertexCount ||
        routeInstance.vertexOffset + i0 >= rigidRouteVertexCount ||
        routeInstance.vertexOffset + i1 >= rigidRouteVertexCount ||
        routeInstance.vertexOffset + i2 >= rigidRouteVertexCount)
    {
        return false;
    }

    const float3 p0 = PathTraceCleanRtxdiDiTransformRoutePoint(routeInstance, SmokeRigidRouteVertices[routeInstance.vertexOffset + i0].position.xyz);
    const float3 p1 = PathTraceCleanRtxdiDiTransformRoutePoint(routeInstance, SmokeRigidRouteVertices[routeInstance.vertexOffset + i1].position.xyz);
    const float3 p2 = PathTraceCleanRtxdiDiTransformRoutePoint(routeInstance, SmokeRigidRouteVertices[routeInstance.vertexOffset + i2].position.xyz);
    float3 barycentrics;
    if (!PathTraceCleanRtxdiDiComputeTriangleBarycentrics(primarySurface.worldPos, p0, p1, p2, barycentrics))
    {
        return false;
    }
    hitBarycentrics = barycentrics.yz;
    return true;
}

bool PathTraceCleanRtxdiDiLoadPrimaryRecordTraceHitSurface(
    RAB_Surface primarySurface,
    out PathTraceCleanRtxdiDiTraceHitSurface hitSurface)
{
    hitSurface = PathTraceCleanRtxdiDiEmptyTraceHitSurface();
    float2 hitBarycentrics;
    if (!PathTraceCleanRtxdiDiReconstructPrimarySurfaceBarycentrics(primarySurface, hitBarycentrics))
    {
        return false;
    }

    PathTraceCleanRtxdiPayload payload = (PathTraceCleanRtxdiPayload)0;
    payload.hitInstanceId = primarySurface.instanceId;
    payload.hitPrimitiveIndex = primarySurface.primitiveIndex;
    payload.hitBarycentrics = hitBarycentrics;
    return PathTraceCleanRtxdiDiLoadTraceHitSurface(payload, hitSurface);
}

float3 PathTraceCleanRtxdiDiTraceHitDecodeNormal(
    PathTraceSmokeMaterial material,
    float2 texCoord,
    float3 normal,
    float3 tangent,
    float3 bitangent)
{
    if ((material.normalTextureIndex == 0xffffffffu) ||
        ((((uint)TextureInfo.w) & RT_SMOKE_TEXTURE_FLAG_USE_NORMAL_MAPS) == 0u))
    {
        return normal;
    }

    const float4 bump = PathTraceCleanRoomSampleTexture(
        material.normalTextureIndex,
        material.normalTextureWidth,
        material.normalTextureHeight,
        texCoord,
        float4(0.5, 0.5, 1.0, 1.0)) * 2.0 - 1.0;
    if (!all(bump == bump))
    {
        return normal;
    }

    const float normalMapFlipGreen =
        ((((uint)TextureInfo.w) & RT_SMOKE_TEXTURE_FLAG_NORMAL_MAP_FLIP_GREEN) != 0u) ? 1.0 : 0.0;
    const float2 normalXY = SmokeMatClassNormalXY(material, bump, normalMapFlipGreen);
    float3 decoded = float3(normalXY, 0.0);
    const float xyLengthSquared = dot(decoded.xy, decoded.xy);
    if (xyLengthSquared >= 1.0)
    {
        decoded.xy *= rsqrt(xyLengthSquared);
        decoded.z = 0.0;
    }
    else
    {
        decoded.z = sqrt(1.0 - xyLengthSquared);
    }
    return RAB_SafeNormalize(tangent * decoded.x + bitangent * decoded.y + normal * decoded.z, normal);
}

float3 PathTraceCleanRtxdiDiConstrainResolvedSurfaceShadingNormal(float3 shadingNormal, float3 geometryNormal)
{
    const float minGeometryDot = 0.02;
    geometryNormal = RAB_SafeNormalize(geometryNormal, float3(0.0, 0.0, 1.0));
    shadingNormal = RAB_SafeNormalize(shadingNormal, geometryNormal);

    const float geometryDot = dot(shadingNormal, geometryNormal);
    if (geometryDot >= minGeometryDot)
    {
        return shadingNormal;
    }

    float3 tangentComponent = shadingNormal - geometryNormal * geometryDot;
    const float tangentLengthSquared = dot(tangentComponent, tangentComponent);
    if (tangentLengthSquared <= 1.0e-8)
    {
        return geometryNormal;
    }

    const float tangentScale = sqrt(max(1.0 - minGeometryDot * minGeometryDot, 0.0) / tangentLengthSquared);
    return RAB_SafeNormalize(tangentComponent * tangentScale + geometryNormal * minGeometryDot, geometryNormal);
}

float3 PathTraceCleanRtxdiDiTraceHitSampleSpecular(PathTraceSmokeMaterial material, float2 texCoord)
{
    if ((material.specularTextureIndex == 0xffffffffu) ||
        ((((uint)TextureInfo.w) & RT_SMOKE_TEXTURE_FLAG_USE_SPECULAR_MAPS) == 0u))
    {
        return float3(0.0, 0.0, 0.0);
    }
    return saturate(PathTraceCleanRoomSampleTexture(
        material.specularTextureIndex,
        material.specularTextureWidth,
        material.specularTextureHeight,
        texCoord,
        float4(0.0, 0.0, 0.0, 1.0)).rgb);
}

float3 PathTraceCleanRtxdiDiTraceHitConvertYCoCgToRGB(float4 ycocg)
{
    ycocg.z = (ycocg.z * 31.875) + 1.0;
    ycocg.z = 1.0 / ycocg.z;
    ycocg.xy *= ycocg.z;
    return saturate(float3(
        dot(ycocg, float4(1.0, -1.0, 0.0, 1.0)),
        dot(ycocg, float4(0.0, 1.0, -0.50196078, 1.0)),
        dot(ycocg, float4(-1.0, -1.0, 1.00392156, 1.0))));
}

float4 PathTraceCleanRtxdiDiTraceHitSampleDecodedDiffuse(PathTraceSmokeMaterial material, float2 texCoord)
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
        texel.rgb = PathTraceCleanRtxdiDiTraceHitConvertYCoCgToRGB(texel);
    }
    return saturate(texel);
}

float4 PathTraceCleanRtxdiDiTraceHitSurfaceAlbedo(
    PathTraceSmokeMaterial material,
    PathTraceCleanRtxdiDiTraceHitSurface hitSurface,
    uint triangleClassAndFlags)
{
    float4 albedo = PathTraceCleanRtxdiDiTraceHitSampleDecodedDiffuse(material, hitSurface.texCoord);
    const uint surfaceClass = triangleClassAndFlags & RT_SMOKE_TRIANGLE_CLASS_MASK;
    const uint translucentSubtype = (triangleClassAndFlags & RT_SMOKE_TRANSLUCENT_SUBTYPE_MASK) >> RT_SMOKE_TRANSLUCENT_SUBTYPE_SHIFT;
    if (surfaceClass == RT_SMOKE_SURFACE_CLASS_TRANSLUCENT &&
        translucentSubtype == RT_SMOKE_TRANSLUCENT_SUBTYPE_GUI_SCREEN)
    {
        albedo = material.diffuseTextureIndex != 0xffffffffu
            ? float4(albedo.rgb * hitSurface.vertexColor.rgb, albedo.a * hitSurface.vertexColor.a)
            : hitSurface.vertexColor;
    }
    return saturate(albedo);
}

float4 PathTraceCleanRtxdiDiTraceHitSampleAlphaTexture(PathTraceSmokeMaterial material, float2 texCoord)
{
    const float4 fallback = PathTraceCleanRtxdiDiTraceHitSampleDecodedDiffuse(material, texCoord);
    if (material.alphaTextureIndex != 0xffffffffu)
    {
        return PathTraceCleanRoomSampleTexture(
            material.alphaTextureIndex,
            material.alphaTextureWidth,
            material.alphaTextureHeight,
            texCoord,
            fallback);
    }
    return fallback;
}

float PathTraceCleanRtxdiDiTraceHitAlphaCoverage(PathTraceSmokeMaterial material, float2 texCoord)
{
    if ((material.flags & RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_DARK_KEY_RESOLVED_SURFACE) != 0u)
    {
        const float3 decoded = PathTraceCleanRtxdiDiTraceHitSampleDecodedDiffuse(material, texCoord).rgb;
        return 1.0 - max(max(decoded.r, decoded.g), decoded.b);
    }

    if ((material.flags & RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_LUMA) != 0u)
    {
        const float3 decoded = PathTraceCleanRtxdiDiTraceHitSampleDecodedDiffuse(material, texCoord).rgb;
        return max(max(decoded.r, decoded.g), decoded.b);
    }

    if ((material.flags & RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_MAGENTA_KEY) != 0u)
    {
        const float3 decoded = PathTraceCleanRtxdiDiTraceHitSampleDecodedDiffuse(material, texCoord).rgb;
        const float keyDistance = max(abs(decoded.r - 1.0), max(abs(decoded.g), abs(decoded.b - 1.0)));
        return keyDistance <= 0.08 ? 0.0 : 1.0;
    }

    return PathTraceCleanRtxdiDiTraceHitSampleAlphaTexture(material, texCoord).a;
}

bool PathTraceCleanRtxdiDiTraceHitUsesUnlitColorFallback(
    PathTraceSmokeMaterial material,
    uint surfaceClass,
    uint translucentSubtype)
{
    if (surfaceClass != RT_SMOKE_SURFACE_CLASS_TRANSLUCENT)
    {
        return false;
    }

    if (translucentSubtype == RT_SMOKE_TRANSLUCENT_SUBTYPE_OBJECT_GLASS_RESOLVED_SURFACE ||
        translucentSubtype == RT_SMOKE_TRANSLUCENT_SUBTYPE_PORTAL_WINDOW_RESOLVED_SURFACE ||
        translucentSubtype == RT_SMOKE_TRANSLUCENT_SUBTYPE_GUI_SCREEN)
    {
        return false;
    }

    return (material.flags & RT_SMOKE_MATERIAL_EMISSIVE) != 0u;
}

float3 PathTraceCleanRtxdiDiTraceHitSurfaceEmissive(
    PathTraceSmokeMaterial material,
    PathTraceCleanRtxdiDiTraceHitSurface hitSurface,
    uint triangleClassAndFlags)
{
    const uint surfaceClass = triangleClassAndFlags & RT_SMOKE_TRIANGLE_CLASS_MASK;
    const bool activeEmissiveStage = (triangleClassAndFlags & RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF) == 0u;
    if ((material.flags & RT_SMOKE_MATERIAL_EMISSIVE) == 0u ||
        !activeEmissiveStage ||
        surfaceClass == RT_SMOKE_SURFACE_CLASS_SKINNED_DEFORMED ||
        ((((uint)TextureInfo.w) & RT_SMOKE_TEXTURE_FLAG_USE_EMISSIVE_MAPS) == 0u))
    {
        return float3(0.0, 0.0, 0.0);
    }

    float3 emissive = max(material.emissiveColor.rgb, float3(0.0, 0.0, 0.0));
    if (material.emissiveTextureIndex != 0xffffffffu)
    {
        emissive *= saturate(PathTraceCleanRoomSampleTexture(
            material.emissiveTextureIndex,
            material.emissiveTextureWidth,
            material.emissiveTextureHeight,
            hitSurface.texCoord,
            float4(1.0, 1.0, 1.0, 1.0)).rgb);
    }
    return emissive * 1.75;
}

struct PathTraceCleanRtxdiDiLiquidPoolResolve
{
    LiquidPoolResolvedFilm film;
    LiquidPoolEffectiveReceiverMaterial effective;
    uint rawCount;
    uint retainedCount;
    uint validatedCount;
    uint rejectionCount;
    uint statusMask;
};

PathTraceCleanRtxdiDiLiquidPoolResolve PathTraceCleanRtxdiDiLiquidPoolResolveEmpty()
{
    PathTraceCleanRtxdiDiLiquidPoolResolve result = (PathTraceCleanRtxdiDiLiquidPoolResolve)0;
    result.film = LiquidPoolResolvedFilmIdentity();
    return result;
}

bool PathTraceCleanRtxdiDiTryLoadLiquidPoolParameters(
    uint materialIndex,
    out PathTraceMaterialFeatureParameterRecord parameters)
{
    parameters = PathTraceDefaultLiquidPoolMaterialFeatureParameters();
    uint parameterRecordCount = 0u;
    uint parameterRecordStride = 0u;
    PathTraceMaterialFeatureParameters.GetDimensions(parameterRecordCount, parameterRecordStride);
    const uint materialCount = (uint)max(TextureInfo.z, 0.0);
    if (parameterRecordCount < materialCount ||
        parameterRecordStride != RT_PATH_TRACE_MATERIAL_FEATURE_PARAMETER_RECORD_STRIDE ||
        materialIndex >= materialCount)
    {
        return false;
    }

    parameters = PathTraceSanitizeLiquidPoolMaterialFeatureParameters(
        PathTraceMaterialFeatureParameters[materialIndex]);
    return true;
}

bool PathTraceCleanRtxdiDiLiquidPoolUsesInvertedFilterBlackKey(
    PathTraceMaterialFeatureParameterRecord parameters,
    PathTraceSmokeMaterial material)
{
    static const uint RT_PATH_TRACE_ORDERED_STAGE_OPERATION_SHIFT = 7u;
    static const uint RT_PATH_TRACE_ORDERED_STAGE_OPERATION_MASK = 0x7u;
    static const uint RT_PATH_TRACE_ORDERED_STAGE_OPERATION_INVERTED_FILTER_BLACK_KEY = 4u;

    [unroll]
    for (uint stageSlot = 0u; stageSlot < RT_PATH_TRACE_ORDERED_STAGE_CAPACITY; ++stageSlot)
    {
        const uint stageWord = PathTraceMaterialOrderedStageWord(parameters, stageSlot);
        const uint textureWord = PathTraceMaterialOrderedStageTextureWord(parameters, stageSlot);
        if (!PathTraceMaterialOrderedStageValid(stageWord) ||
            !PathTraceMaterialOrderedStageTextureValid(textureWord) ||
            PathTraceMaterialOrderedStageTextureIndex(textureWord) != material.diffuseTextureIndex)
        {
            continue;
        }

        const uint operation =
            (stageWord >> RT_PATH_TRACE_ORDERED_STAGE_OPERATION_SHIFT) &
            RT_PATH_TRACE_ORDERED_STAGE_OPERATION_MASK;
        if (operation == RT_PATH_TRACE_ORDERED_STAGE_OPERATION_INVERTED_FILTER_BLACK_KEY)
        {
            return true;
        }
    }
    return false;
}

uint PathTraceCleanRtxdiDiLiquidPoolDiagnosticHash(LiquidPoolContributorKey key)
{
    uint hash = 2166136261u;
    hash = (hash ^ key.instanceId) * 16777619u;
    hash = (hash ^ key.primitiveIndex) * 16777619u;
    hash = (hash ^ key.materialIndex) * 16777619u;
    hash = (hash ^ key.barycentricXBits) * 16777619u;
    return (hash ^ key.barycentricYBits) * 16777619u;
}

bool PathTraceCleanRtxdiDiTryBuildLiquidPoolCardEvidence(
    PathTraceCleanRtxdiPayload receiverPayload,
    uint instanceId,
    uint primitiveIndex,
    float2 barycentrics,
    float candidateHitT,
    float3 receiverPosition,
    float3 rayDirection,
    out float3 cardPosition,
    out float3 cardPlaneNormal,
    out float2 cardTexCoord,
    out float3 cardTangent,
    out float3 cardBitangent)
{
    cardPosition = 0.0;
    cardPlaneNormal = 0.0;
    cardTexCoord = 0.0;
    cardTangent = 0.0;
    cardBitangent = 0.0;
    PathTraceCleanRtxdiPayload cardPayload = receiverPayload;
    cardPayload.hitInstanceId = instanceId;
    cardPayload.hitPrimitiveIndex = primitiveIndex;
    cardPayload.hitBarycentrics = barycentrics;
    PathTraceCleanRtxdiDiTraceHitSurface cardSurface;
    if (!PathTraceCleanRtxdiDiLoadTraceHitSurface(cardPayload, cardSurface))
    {
        return false;
    }

    const float3 normalizedRay = RAB_SafeNormalize(rayDirection, float3(0.0, 0.0, 1.0));
    cardPosition = receiverPosition + normalizedRay * (candidateHitT - receiverPayload.hitT);
    cardPlaneNormal = cardSurface.geometricNormal;
    cardTexCoord = cardSurface.texCoord;
    cardTangent = cardSurface.tangent;
    cardBitangent = cardSurface.bitangent;
    return LiquidPoolFinite3(cardPosition) &&
        LiquidPoolFinite3(cardPlaneNormal) &&
        LiquidPoolFinite2(cardTexCoord);
}

float2 PathTraceCleanRtxdiDiLiquidPoolCoverageTexelSize(PathTraceSmokeMaterial material)
{
    const uint diffuseCoverageFlags =
        RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_DARK_KEY_TRANSMISSION |
        RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_LUMA |
        RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_MAGENTA_KEY;
    const bool coverageUsesDiffuse =
        (material.flags & diffuseCoverageFlags) != 0u ||
        material.alphaTextureIndex == 0xffffffffu;
    const uint width = coverageUsesDiffuse ? material.textureWidth : material.alphaTextureWidth;
    const uint height = coverageUsesDiffuse ? material.textureHeight : material.alphaTextureHeight;
    return rcp(float2(max(width, 1u), max(height, 1u)));
}

void PathTraceCleanRtxdiDiLiquidPoolSaturatingIncrement(uint counterIndex)
{
    uint observed;
    InterlockedCompareExchange(
        PathTraceLiquidPoolStatusCounters[counterIndex],
        0xffffffffu,
        0xffffffffu,
        observed);
    while (observed != 0xffffffffu)
    {
        uint previous;
        InterlockedCompareExchange(
            PathTraceLiquidPoolStatusCounters[counterIndex],
            observed,
            observed + 1u,
            previous);
        if (previous == observed)
        {
            return;
        }
        observed = previous;
    }
}

void PathTraceCleanRtxdiDiPublishLiquidPoolExceptionalStatus(uint statusMask)
{
    const uint exceptionalMask = statusMask &
        (RT_LIQUID_POOL_STATUS_OVERFLOW |
            RT_LIQUID_POOL_STATUS_DUPLICATE_APPLY |
            RT_LIQUID_POOL_STATUS_FAIL_CLOSED |
            RT_LIQUID_POOL_STATUS_INVALID_ROUTE);
    if (exceptionalMask == 0u ||
        (PathTraceCleanRtxdiDiLiquidPoolControlFlags() & RT_LIQUID_POOL_CONTROL_TELEMETRY_READY) == 0u)
    {
        return;
    }

    uint ignored;
    InterlockedOr(
        PathTraceLiquidPoolStatusCounters[RT_LIQUID_POOL_SOURCE_CLEAN_DI_REFLECTION],
        exceptionalMask,
        ignored);
    if ((exceptionalMask & RT_LIQUID_POOL_STATUS_OVERFLOW) != 0u)
    {
        PathTraceCleanRtxdiDiLiquidPoolSaturatingIncrement(
            8u + RT_LIQUID_POOL_SOURCE_CLEAN_DI_REFLECTION);
    }
}

PathTraceCleanRtxdiDiLiquidPoolResolve PathTraceCleanRtxdiDiResolveLiquidPool(
    inout RAB_Surface surface,
    PathTraceCleanRtxdiPayload payload,
    float3 rayDirection)
{
    PathTraceCleanRtxdiDiLiquidPoolResolve result = PathTraceCleanRtxdiDiLiquidPoolResolveEmpty();
    result.rawCount = payload.liquidRawCount;
    result.retainedCount = payload.liquidRetainedCount;
    result.rejectionCount = payload.liquidRejectionCount;
    result.statusMask = payload.liquidStatusMask;
    result.effective = LiquidPoolApplyResolvedFilm(
        surface.material.diffuseAlbedo,
        surface.material.specularF0,
        surface.material.roughness,
        result.film,
        0u);

    if (!PathTraceCleanRtxdiDiLiquidPoolCollectionEnabled() ||
        !RAB_IsSurfaceValid(surface) ||
        payload.liquidRetainedCount == 0u)
    {
        PathTraceCleanRtxdiDiPublishLiquidPoolExceptionalStatus(result.statusMask);
        return result;
    }

    PathTraceMaterialFeature receiverFeature;
    const bool receiverFeatureValid =
        PathTraceCleanRtxdiDiLoadMaterialFeature(surface.materialIndex, receiverFeature);
    const uint receiverOpaque = receiverFeatureValid &&
        (receiverFeature.materialCaps & RT_PATH_TRACE_MATERIAL_CAP_OPAQUE_DIRECT) != 0u &&
        surface.surfaceClass != RT_SMOKE_SURFACE_CLASS_TRANSLUCENT;
    const uint receiverTransmission = !receiverFeatureValid ||
        (receiverFeature.materialCaps & RT_PATH_TRACE_MATERIAL_CAP_PATH_TRANSMISSION) != 0u;

    [loop]
    for (uint slot = 0u;
        slot < min(payload.liquidRetainedCount, RT_CLEAN_RTXDI_DI_LIQUID_POOL_CANDIDATE_CAPACITY);
        ++slot)
    {
        const uint instanceId = payload.liquidInstanceId[slot];
        const uint materialIndex = payload.liquidMaterialIndex[slot];
        const uint primitiveIndex = payload.liquidPrimitiveIndex[slot];
        const float2 barycentrics = float2(
            asfloat(payload.liquidBarycentricXBits[slot]),
            asfloat(payload.liquidBarycentricYBits[slot]));
        float3 cardPosition;
        float3 cardPlaneNormal;
        float2 cardTexCoord;
        float3 cardTangent;
        float3 cardBitangent;
        const bool cardValid = PathTraceCleanRtxdiDiTryBuildLiquidPoolCardEvidence(
            payload,
            instanceId,
            primitiveIndex,
            barycentrics,
            payload.liquidHitT[slot],
            surface.worldPos,
            rayDirection,
            cardPosition,
            cardPlaneNormal,
            cardTexCoord,
            cardTangent,
            cardBitangent);

        LiquidPoolReceiverEvidence evidence = (LiquidPoolReceiverEvidence)0;
        evidence.cardPosition = cardPosition;
        evidence.cardPlaneNormal = cardPlaneNormal;
        evidence.receiverPosition = surface.worldPos;
        evidence.receiverGeometryNormal = surface.geometryNormal;
        evidence.rayDirection = rayDirection;
        // Geometry routing is independent for projected cards and receivers.
        // Trace ordering plus the shared plane/material predicate proves the
        // receiver consistently with primary visibility.
        evidence.domainAccepted = cardValid;
        evidence.identityAccepted = cardValid;
        evidence.receiverOpaque = receiverOpaque;
        evidence.receiverPathTransmission = receiverTransmission;
        if (!LiquidPoolAcceptsReceiver(evidence))
        {
            result.rejectionCount += 1u;
            result.statusMask |= RT_LIQUID_POOL_STATUS_RECEIVER_REJECTED;
            continue;
        }

        PathTraceMaterialFeatureParameterRecord parameters;
        float4 stageColor;
        if (!PathTraceCleanRtxdiDiTryLoadLiquidPoolParameters(materialIndex, parameters) ||
            !PathTraceCleanRtxdiDiTryGetLiquidPoolStageColor(materialIndex, stageColor))
        {
            result.rejectionCount += 1u;
            result.statusMask |= RT_LIQUID_POOL_STATUS_RECEIVER_REJECTED;
            continue;
        }

        const PathTraceSmokeMaterial material = PathTraceCleanRoomLoadSmokeMaterial(materialIndex);
        LiquidPoolReducerCandidate candidate = (LiquidPoolReducerCandidate)0;
        candidate.coverage =
            saturate(PathTraceCleanRtxdiDiLiquidPoolAlphaCoverage(material, cardTexCoord)) *
            saturate(stageColor.a);
        candidate.height = 1.0;
        const float3 authoredSourceRgb = saturate(
            max(PathTraceCleanRtxdiDiLiquidPoolSampleDecodedDiffuse(material, cardTexCoord).rgb, 0.0) *
            max(stageColor.rgb, 0.0));
        candidate.decalRgb = PathTraceCleanRtxdiDiLiquidPoolUsesInvertedFilterBlackKey(parameters, material)
            ? 1.0 - authoredSourceRgb
            : authoredSourceRgb;
        candidate.referenceTransmittance = parameters.params0.xyz;
        candidate.opticalDepthScale = parameters.params0.w;
        candidate.coatRoughness = parameters.params1.x;
        candidate.dielectricIor = parameters.params1.y;
        candidate.authoredNormalStrength = parameters.params1.z;
        candidate.key = LiquidPoolMakeContributorKey(
            instanceId,
            primitiveIndex,
            materialIndex,
            barycentrics);
        candidate.diagnosticHash = PathTraceCleanRtxdiDiLiquidPoolDiagnosticHash(candidate.key);
        candidate.valid = candidate.coverage > 0.0 ? 1u : 0u;
        if (candidate.valid == 0u)
        {
            continue;
        }
        result.film = LiquidPoolReduceCandidate(result.film, candidate);
        result.validatedCount += 1u;
    }

    if (result.validatedCount > 0u)
    {
        result.statusMask |= RT_LIQUID_POOL_STATUS_RECEIVER_VALID;
    }
    result.film.overflowed =
        (result.statusMask & RT_LIQUID_POOL_STATUS_OVERFLOW) != 0u ? 1u : 0u;
    if (result.film.overflowed == 0u)
    {
        result.effective = LiquidPoolApplyResolvedFilm(
            surface.material.diffuseAlbedo,
            surface.material.specularF0,
            surface.material.roughness,
            result.film,
            (surface.flags & RT_PATH_TRACE_SURFACE_FLAG_LIQUID_FILM_APPLIED) != 0u ? 1u : 0u);
        if ((surface.flags & RT_PATH_TRACE_SURFACE_FLAG_LIQUID_FILM_APPLIED) != 0u &&
            result.film.valid != 0u)
        {
            result.statusMask |= RT_LIQUID_POOL_STATUS_DUPLICATE_APPLY;
        }
        else if (PathTraceCleanRtxdiDiLiquidPoolMode() >= 2u && result.effective.applied != 0u)
        {
            surface.material.diffuseAlbedo = result.effective.albedo;
            surface.material.specularF0 = result.effective.specularF0;
            surface.material.roughness = result.effective.roughness;
            surface.flags |= RT_PATH_TRACE_SURFACE_FLAG_LIQUID_FILM_APPLIED;
            result.statusMask |= RT_LIQUID_POOL_STATUS_APPLIED;

            if (PathTraceCleanRtxdiDiLiquidPoolMode() == 3u &&
                (surface.flags & RT_PATH_TRACE_SURFACE_FLAG_LIQUID_FILM_NORMAL_APPLIED) == 0u)
            {
                float winnerHitT = 0.0;
                uint winnerFound = 0u;
                [loop]
                for (uint slot = 0u;
                    slot < min(payload.liquidRetainedCount, RT_CLEAN_RTXDI_DI_LIQUID_POOL_CANDIDATE_CAPACITY);
                    ++slot)
                {
                    if (payload.liquidInstanceId[slot] == result.film.winnerKey.instanceId &&
                        payload.liquidPrimitiveIndex[slot] == result.film.winnerKey.primitiveIndex &&
                        payload.liquidMaterialIndex[slot] == result.film.winnerKey.materialIndex &&
                        payload.liquidBarycentricXBits[slot] == result.film.winnerKey.barycentricXBits &&
                        payload.liquidBarycentricYBits[slot] == result.film.winnerKey.barycentricYBits)
                    {
                        winnerHitT = payload.liquidHitT[slot];
                        winnerFound = 1u;
                        break;
                    }
                }

                if (winnerFound != 0u)
                {
                    const float2 winnerBarycentrics = float2(
                        asfloat(result.film.winnerKey.barycentricXBits),
                        asfloat(result.film.winnerKey.barycentricYBits));
                    float3 winnerPosition;
                    float3 winnerPlaneNormal;
                    float2 winnerTexCoord;
                    float3 winnerTangent;
                    float3 winnerBitangent;
                    if (PathTraceCleanRtxdiDiTryBuildLiquidPoolCardEvidence(
                        payload,
                        result.film.winnerKey.instanceId,
                        result.film.winnerKey.primitiveIndex,
                        winnerBarycentrics,
                        winnerHitT,
                        surface.worldPos,
                        rayDirection,
                        winnerPosition,
                        winnerPlaneNormal,
                        winnerTexCoord,
                        winnerTangent,
                        winnerBitangent))
                    {
                        const PathTraceSmokeMaterial winnerMaterial =
                            PathTraceCleanRoomLoadSmokeMaterial(result.film.winnerKey.materialIndex);
                        float4 winnerStageColor;
                        if (PathTraceCleanRtxdiDiTryGetLiquidPoolStageColor(
                            result.film.winnerKey.materialIndex,
                            winnerStageColor))
                        {
                            const float2 texelSize =
                                PathTraceCleanRtxdiDiLiquidPoolCoverageTexelSize(winnerMaterial);
                            const float stageAlpha = saturate(winnerStageColor.a);
                            const float coverageLeft = saturate(
                                PathTraceCleanRtxdiDiLiquidPoolAlphaCoverage(
                                    winnerMaterial, winnerTexCoord - float2(texelSize.x, 0.0))) * stageAlpha;
                            const float coverageRight = saturate(
                                PathTraceCleanRtxdiDiLiquidPoolAlphaCoverage(
                                    winnerMaterial, winnerTexCoord + float2(texelSize.x, 0.0))) * stageAlpha;
                            const float coverageDown = saturate(
                                PathTraceCleanRtxdiDiLiquidPoolAlphaCoverage(
                                    winnerMaterial, winnerTexCoord - float2(0.0, texelSize.y))) * stageAlpha;
                            const float coverageUp = saturate(
                                PathTraceCleanRtxdiDiLiquidPoolAlphaCoverage(
                                    winnerMaterial, winnerTexCoord + float2(0.0, texelSize.y))) * stageAlpha;
                            const LiquidPoolEffectiveFilmNormal filmNormal =
                                LiquidPoolApplyFilmOwnedNormal(
                                    surface.shadingNormal,
                                    surface.geometryNormal,
                                    winnerTangent,
                                    winnerBitangent,
                                    0.5 * float2(
                                        coverageRight - coverageLeft,
                                        coverageUp - coverageDown),
                                    result.film.coverage,
                                    result.film.authoredNormalStrength,
                                    0u);
                            if (filmNormal.applied != 0u)
                            {
                                surface.shadingNormal = filmNormal.shadingNormal;
                                surface.flags |= RT_PATH_TRACE_SURFACE_FLAG_LIQUID_FILM_NORMAL_APPLIED;
                            }
                        }
                    }
                }
            }
        }
    }
    PathTraceCleanRtxdiDiPublishLiquidPoolExceptionalStatus(result.statusMask);
    return result;
}

float3 PathTraceCleanRtxdiDiLiquidPoolStatusColor(uint statusMask)
{
    if ((statusMask & (RT_LIQUID_POOL_STATUS_OVERFLOW | RT_LIQUID_POOL_STATUS_INVALID_ROUTE)) != 0u)
        return float3(1.0, 0.0, 1.0);
    if ((statusMask & RT_LIQUID_POOL_STATUS_APPLIED) != 0u) return float3(0.0, 1.0, 0.0);
    if ((statusMask & RT_LIQUID_POOL_STATUS_RECEIVER_VALID) != 0u) return float3(0.0, 1.0, 1.0);
    if ((statusMask & RT_LIQUID_POOL_STATUS_RECEIVER_REJECTED) != 0u) return float3(1.0, 0.2, 0.0);
    if ((statusMask & RT_LIQUID_POOL_STATUS_CANDIDATE) != 0u) return float3(1.0, 1.0, 0.0);
    return float3(0.0, 0.0, 0.0);
}

float4 PathTraceCleanRtxdiDiLiquidPoolDiagnosticTuple(
    PathTraceCleanRtxdiPayload payload,
    inout PathTraceCleanRtxdiDiLiquidPoolResolve resolved)
{
    const uint debug = PathTraceCleanRtxdiDiLiquidPoolDebug();
    const uint page = PathTraceCleanRtxdiDiLiquidPoolPage();
    if (debug == 1u && page == 0u)
    {
        return float4(PathTraceCleanRtxdiDiLiquidPoolStatusColor(resolved.statusMask), (float)resolved.statusMask);
    }
    if (debug == 2u && page == 0u)
    {
        return float4((float)resolved.rawCount, (float)resolved.retainedCount,
            (float)RT_CLEAN_RTXDI_DI_LIQUID_POOL_CANDIDATE_CAPACITY, (float)resolved.statusMask);
    }
    if (debug == 3u && page == 0u)
    {
        return float4((float)resolved.validatedCount, resolved.film.coverage,
            resolved.film.height, (float)resolved.statusMask);
    }
    if (debug == 4u && page == 0u)
    {
        return float4(asfloat(resolved.film.winnerKey.instanceId),
            asfloat(resolved.film.winnerKey.primitiveIndex),
            asfloat(resolved.film.winnerKey.materialIndex),
            asfloat(resolved.statusMask));
    }
    if (debug == 4u && page == 1u)
    {
        return float4(asfloat(resolved.film.winnerKey.barycentricXBits),
            asfloat(resolved.film.winnerKey.barycentricYBits),
            asfloat(resolved.film.diagnosticHash),
            asfloat(resolved.statusMask));
    }
    if (debug == 4u && page == 2u)
    {
        return float4((float)resolved.rejectionCount, resolved.film.coverage,
            (float)RT_LIQUID_POOL_SOURCE_CLEAN_DI_REFLECTION, (float)resolved.statusMask);
    }
    if (debug == 5u && page == 0u)
    {
        return float4(resolved.effective.albedo, (float)resolved.effective.applied);
    }
    if (debug == 5u && page == 1u)
    {
        return float4(resolved.effective.specularF0, resolved.effective.roughness);
    }
    if (debug == 5u && page == 2u)
    {
        return float4(resolved.effective.transmittance, resolved.film.height);
    }
    if (debug == 5u && page == 3u)
    {
        const float coatF0 = LiquidPoolDielectricF0(resolved.film.dielectricIor);
        const float opticalDepth = clamp(
            resolved.film.opticalDepthScale * resolved.film.height,
            0.0,
            LIQUID_POOL_D_MAX);
        return float4(resolved.film.coverage, coatF0, resolved.film.coatRoughness, opticalDepth);
    }
    if (debug == 6u && page == 0u)
    {
        return float4((float)RT_LIQUID_POOL_SOURCE_CLEAN_DI_REFLECTION,
            (float)resolved.statusMask,
            (resolved.statusMask & RT_LIQUID_POOL_STATUS_APPLIED) != 0u ? 1.0 : 0.0,
            0.0);
    }
    if (debug == 6u && page == 1u)
    {
        return float4(0.0, 1.0, 0.0, 1.0);
    }

    resolved.statusMask |= RT_LIQUID_POOL_STATUS_INVALID_ROUTE;
    PathTraceCleanRtxdiDiPublishLiquidPoolExceptionalStatus(resolved.statusMask);
    return float4((float)RT_LIQUID_POOL_SOURCE_INVALID, (float)resolved.statusMask, 0.0, 0.0);
}

bool PathTraceCleanRtxdiDiBuildResolvedSurfaceFromTraceHit(
    PathTraceCleanRtxdiPayload payload,
    float3 worldPosition,
    float3 rayDirection,
    out RAB_Surface surface,
    out PathTraceCleanRtxdiDiLiquidPoolResolve liquidResolve)
{
    surface = RAB_EmptySurface();
    liquidResolve = PathTraceCleanRtxdiDiLiquidPoolResolveEmpty();
    if (payload.hitMaterialIndex >= (uint)TextureInfo.z)
    {
        return false;
    }

    PathTraceCleanRtxdiDiTraceHitSurface hitSurface;
    if (!PathTraceCleanRtxdiDiLoadTraceHitSurface(payload, hitSurface))
    {
        return false;
    }

    const PathTraceSmokeMaterial material = PathTraceCleanRoomLoadSmokeMaterial(payload.hitMaterialIndex);
    const float4 diffuse = PathTraceCleanRtxdiDiTraceHitSurfaceAlbedo(
        material,
        hitSurface,
        payload.hitTriangleClassAndFlags);
    float3 albedo = diffuse.rgb;
    const float3 specularTexel = PathTraceCleanRtxdiDiTraceHitSampleSpecular(material, hitSurface.texCoord);
    float3 specularF0 = specularTexel;
    float roughness = 1.0;
    if (PathTraceCleanRtxdiDiSurfaceAdapterToyFakePBRSpecularEnabled())
    {
        PathTraceCleanRtxdiDiSurfaceAdapterPBRFromSpecmap(saturate(specularTexel), specularF0, roughness);
    }
    SmokeApplyMaterialClassifierBsdfWithSpecularTexel(material, albedo, specularTexel, specularF0, roughness);
    const bool fullMetalOverride = SmokeMaterialHasFullMetalOverride(material);
    SmokeApplyFullMetalOverride(material, albedo, specularF0);
    if ((material.padding0 & RT_SMOKE_MATERIAL_OVERRIDE_ZERO_ROUGHNESS_RESOLVED_SURFACE) != 0u)
    {
        roughness = 0.0;
        if (!fullMetalOverride)
        {
            specularF0 = max(specularF0, float3(0.85, 0.85, 0.85));
        }
    }

    float3 shadingNormal = PathTraceCleanRtxdiDiTraceHitDecodeNormal(
        material,
        hitSurface.normalTexCoord,
        hitSurface.shadingNormal,
        hitSurface.tangent,
        hitSurface.bitangent);
    const float3 viewDir = RAB_SafeNormalize(-rayDirection, shadingNormal);
    float3 geometryNormal = RAB_SafeNormalize(hitSurface.geometricNormal, shadingNormal);
    if (dot(geometryNormal, viewDir) < 0.0)
    {
        geometryNormal = -geometryNormal;
    }
    if (dot(shadingNormal, viewDir) < 0.0)
    {
        shadingNormal = -shadingNormal;
    }
    shadingNormal = PathTraceCleanRtxdiDiConstrainResolvedSurfaceShadingNormal(shadingNormal, geometryNormal);

    RAB_Material rabMaterial = RAB_EmptyMaterial();
    rabMaterial.materialId = payload.hitMaterialId;
    rabMaterial.materialIndex = payload.hitMaterialIndex;
    rabMaterial.flags = material.flags;
    rabMaterial.alphaCutoff = material.alphaCutoff;
    rabMaterial.diffuseAlbedo = albedo;
    rabMaterial.specularF0 = specularF0;
    rabMaterial.roughness = saturate(roughness);
    rabMaterial.opacity = (material.flags & 0x00000001u) != 0u
        ? saturate(PathTraceCleanRtxdiDiTraceHitAlphaCoverage(material, hitSurface.texCoord))
        : 1.0;
    rabMaterial.emissiveRadiance =
        PathTraceCleanRtxdiDiTraceHitSurfaceEmissive(material, hitSurface, payload.hitTriangleClassAndFlags) *
        max(CleanRtxdiDiToyPathInfo.z, 0.0);
    const uint surfaceClass = payload.hitTriangleClassAndFlags & RT_SMOKE_TRIANGLE_CLASS_MASK;
    const uint translucentSubtype =
        (payload.hitTriangleClassAndFlags & RT_SMOKE_TRANSLUCENT_SUBTYPE_MASK) >> RT_SMOKE_TRANSLUCENT_SUBTYPE_SHIFT;
    if ((payload.hitTriangleClassAndFlags & RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF) == 0u &&
        PathTraceCleanRtxdiDiTraceHitUsesUnlitColorFallback(material, surfaceClass, translucentSubtype))
    {
        rabMaterial.emissiveRadiance = max(rabMaterial.emissiveRadiance, albedo);
    }
    rabMaterial.emissiveTextureIndex = material.emissiveTextureIndex;
    if ((material.flags & RT_SMOKE_MATERIAL_SKY_ENVIRONMENT_RESOLVED_SURFACE) != 0u)
    {
        rabMaterial.diffuseAlbedo = float3(0.0, 0.0, 0.0);
        rabMaterial.specularF0 = float3(0.0, 0.0, 0.0);
        rabMaterial.roughness = 1.0;
        rabMaterial.opacity = 1.0;
        // The PSR pass publishes authored stage color only.  A narrow compute
        // stage resolves the renderer-owned cube before DI consumes this
        // record; cube access inside this RT library is intentionally avoided.
        rabMaterial.emissiveRadiance = max(
            material.emissiveColor.rgb,
            float3(0.0, 0.0, 0.0));
        rabMaterial.emissiveTextureIndex = 0xffffffffu;
    }

    surface.valid = 1u;
    surface.worldPos = worldPosition;
    surface.linearDepth = payload.hitT;
    surface.geometryNormal = geometryNormal;
    surface.shadingNormal = shadingNormal;
    surface.viewDir = viewDir;
    surface.materialId = payload.hitMaterialId;
    surface.materialIndex = payload.hitMaterialIndex;
    surface.surfaceClass = surfaceClass;
    surface.flags = payload.hitTriangleClassAndFlags;
    surface.material = rabMaterial;
    surface.instanceId = payload.hitInstanceId;
    surface.primitiveIndex = payload.hitPrimitiveIndex;
    liquidResolve = PathTraceCleanRtxdiDiResolveLiquidPool(surface, payload, rayDirection);
    return true;
}

float4 PathTraceCleanRtxdiDiBucketResolvedTupleDiagnostic(
    PathTraceCleanRtxdiPayload payload,
    float3 hitPosition,
    RAB_Surface resolvedSurface)
{
    // Cyan: the accepted hit belongs to a non-bucket route. This is expected
    // for skinned, rigid, or dynamic geometry and provides a control color.
    if (!PathTraceIsStaticBucketRouteInstance(
            payload.hitInstanceId,
            CleanRtxdiDiStaticBucketRouteInfo))
    {
        return float4(0.0, 1.0, 1.0, 1.0);
    }

    PathTraceStaticBucketRouteRecord route;
    uint packedTriangleIndex;
    uint3 packedVertexIndexes;
    if (!PathTraceCleanRtxdiDiTryLoadStaticBucketTriangleRoute(
            payload.hitInstanceId,
            payload.hitPrimitiveIndex,
            route,
            packedTriangleIndex,
            packedVertexIndexes))
    {
        // Magenta: canonical bucket identity could not be decoded.
        return float4(1.0, 0.0, 1.0, 1.0);
    }

    const PathTraceSmokeVertex v0 =
        SmokeStaticBucketVertices[packedVertexIndexes.x];
    const PathTraceSmokeVertex v1 =
        SmokeStaticBucketVertices[packedVertexIndexes.y];
    const PathTraceSmokeVertex v2 =
        SmokeStaticBucketVertices[packedVertexIndexes.z];
    const float3 hardwareBarycentrics = float3(
        1.0 - payload.hitBarycentrics.x - payload.hitBarycentrics.y,
        payload.hitBarycentrics.x,
        payload.hitBarycentrics.y);
    const float3 replayPosition =
        v0.position.xyz * hardwareBarycentrics.x +
        v1.position.xyz * hardwareBarycentrics.y +
        v2.position.xyz * hardwareBarycentrics.z;
    const float positionTolerance =
        max(0.05, max(length(hitPosition), 1.0) * 1.0e-5);
    if (distance(replayPosition, hitPosition) > positionTolerance)
    {
        // Red: InstanceID + PrimitiveIndex addresses a different triangle
        // than the one the hardware intersection attributes describe.
        return float4(1.0, 0.0, 0.0, 1.0);
    }

    float3 reconstructedBarycentrics;
    if (!PathTraceCleanRtxdiDiComputeTriangleBarycentrics(
            hitPosition,
            v0.position.xyz,
            v1.position.xyz,
            v2.position.xyz,
            reconstructedBarycentrics) ||
        max(
            abs(reconstructedBarycentrics.y -
                payload.hitBarycentrics.x),
            abs(reconstructedBarycentrics.z -
                payload.hitBarycentrics.y)) > 0.01)
    {
        // Orange: the packed triangle is spatially compatible, but the
        // transported hardware barycentrics do not replay the same point.
        return float4(1.0, 0.25, 0.0, 1.0);
    }

    const bool metadataMatches =
        SmokeStaticBucketTriangleMaterials[packedTriangleIndex] ==
            payload.hitMaterialId &&
        SmokeStaticBucketTriangleMaterialIndexes[packedTriangleIndex] ==
            payload.hitMaterialIndex &&
        SmokeStaticBucketTriangleClasses[packedTriangleIndex] ==
            payload.hitTriangleClassAndFlags;
    if (!metadataMatches)
    {
        // Yellow: geometry identity is exact, but material/class metadata is
        // not the row published by closest hit.
        return float4(1.0, 1.0, 0.0, 1.0);
    }

    const bool resolvedTupleMatches =
        resolvedSurface.instanceId == payload.hitInstanceId &&
        resolvedSurface.primitiveIndex == payload.hitPrimitiveIndex &&
        resolvedSurface.materialId == payload.hitMaterialId &&
        resolvedSurface.materialIndex == payload.hitMaterialIndex &&
        resolvedSurface.surfaceClass ==
            (payload.hitTriangleClassAndFlags &
                RT_SMOKE_TRIANGLE_CLASS_MASK);
    if (!resolvedTupleMatches)
    {
        // Blue: closest hit and packed replay agree, but resolved-surface
        // construction changed the identity/material tuple.
        return float4(0.0, 0.0, 1.0, 1.0);
    }

    // Green: hardware identity, packed triangle, barycentrics, metadata, and
    // resolved record are exact. A remaining visual defect is then confined
    // to decoded attributes/material evaluation rather than addressing.
    return float4(0.0, 1.0, 0.0, 1.0);
}

float4 PathTraceCleanRtxdiDiBucketClosestHitPositionDiagnostic(
    PathTraceCleanRtxdiPayload payload,
    float3 raygenHitPosition)
{
    if (!PathTraceIsStaticBucketRouteInstance(
            payload.hitInstanceId,
            CleanRtxdiDiStaticBucketRouteInfo))
    {
        // Cyan: non-bucket skinned/rigid/dynamic control.
        return float4(0.0, 1.0, 1.0, 1.0);
    }

    const float3 closestHitPosition =
        payload.passthroughEmissiveRadiance;
    const float positionTolerance =
        max(0.05, max(length(closestHitPosition), 1.0) * 1.0e-5);
    if (distance(raygenHitPosition, closestHitPosition) >
        positionTolerance)
    {
        // Violet: closest hit was correct, but the bounded raygen loop
        // reconstructed a different final world position.
        return float4(0.55, 0.0, 1.0, 1.0);
    }

    PathTraceStaticBucketRouteRecord route;
    uint packedTriangleIndex;
    uint3 packedVertexIndexes;
    if (!PathTraceCleanRtxdiDiTryLoadStaticBucketTriangleRoute(
            payload.hitInstanceId,
            payload.hitPrimitiveIndex,
            route,
            packedTriangleIndex,
            packedVertexIndexes))
    {
        // Magenta: the bucket hardware identity cannot be replayed.
        return float4(1.0, 0.0, 1.0, 1.0);
    }

    const PathTraceSmokeVertex v0 =
        SmokeStaticBucketVertices[packedVertexIndexes.x];
    const PathTraceSmokeVertex v1 =
        SmokeStaticBucketVertices[packedVertexIndexes.y];
    const PathTraceSmokeVertex v2 =
        SmokeStaticBucketVertices[packedVertexIndexes.z];
    const float3 hardwareBarycentrics = float3(
        1.0 - payload.hitBarycentrics.x - payload.hitBarycentrics.y,
        payload.hitBarycentrics.x,
        payload.hitBarycentrics.y);
    const float3 replayPosition =
        v0.position.xyz * hardwareBarycentrics.x +
        v1.position.xyz * hardwareBarycentrics.y +
        v2.position.xyz * hardwareBarycentrics.z;
    if (distance(replayPosition, closestHitPosition) >
        positionTolerance)
    {
        // Red: raygen transport is exact, but the bucket identity/buffer row
        // does not reproduce the triangle intersected by hardware.
        return float4(1.0, 0.0, 0.0, 1.0);
    }

    float3 reconstructedBarycentrics;
    if (!PathTraceCleanRtxdiDiComputeTriangleBarycentrics(
            closestHitPosition,
            v0.position.xyz,
            v1.position.xyz,
            v2.position.xyz,
            reconstructedBarycentrics) ||
        max(
            abs(reconstructedBarycentrics.y -
                payload.hitBarycentrics.x),
            abs(reconstructedBarycentrics.z -
                payload.hitBarycentrics.y)) > 0.01)
    {
        // Orange: positions agree, but barycentric transport does not.
        return float4(1.0, 0.25, 0.0, 1.0);
    }

    // Green: closest hit, bounded-loop reconstruction, and bucket replay all
    // resolve the same point.
    return float4(0.0, 1.0, 0.0, 1.0);
}

#endif

#endif
