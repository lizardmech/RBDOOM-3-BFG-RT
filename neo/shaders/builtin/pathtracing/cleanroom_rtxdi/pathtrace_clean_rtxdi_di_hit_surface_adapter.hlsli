#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_HIT_SURFACE_ADAPTER_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_HIT_SURFACE_ADAPTER_HLSLI

#if defined(CLEAN_RTXDI_DI_TRACE_HIT_SURFACE_ADAPTER)

static const uint RT_SMOKE_MATERIAL_OVERRIDE_ZERO_ROUGHNESS_RESOLVED_SURFACE = 0x00000001u;
static const uint RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_DARK_KEY_RESOLVED_SURFACE = 0x00000100u;
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
    surface.geometricNormal = float3(0.0, 0.0, 1.0);
    surface.shadingNormal = float3(0.0, 0.0, 1.0);
    surface.tangent = float3(1.0, 0.0, 0.0);
    surface.bitangent = float3(0.0, 1.0, 0.0);
    surface.vertexColor = float4(1.0, 1.0, 1.0, 1.0);
    surface.vertexColorAdd = float4(0.0, 0.0, 0.0, 0.0);
    return surface;
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

    const uint routeInstanceIndex = payload.hitInstanceId - 2u;
    const uint rigidRouteInstanceCount = (uint)max(ToyPathInfo.w, 0.0);
    if (payload.hitInstanceId < 2u || routeInstanceIndex >= rigidRouteInstanceCount)
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

    const float2 normalXY = SmokeMatClassNormalXY(material, bump, RestirPTSurfaceInfo.y);
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

bool PathTraceCleanRtxdiDiBuildResolvedSurfaceFromTraceHit(
    PathTraceCleanRtxdiPayload payload,
    float3 worldPosition,
    float3 rayDirection,
    out RAB_Surface surface)
{
    surface = RAB_EmptySurface();
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
        hitSurface.texCoord,
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
    rabMaterial.opacity = saturate(PathTraceCleanRtxdiDiTraceHitAlphaCoverage(material, hitSurface.texCoord));
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
    return true;
}

#endif

#endif
