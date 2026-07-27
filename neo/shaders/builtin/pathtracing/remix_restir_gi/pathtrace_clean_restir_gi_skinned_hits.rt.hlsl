#include "../../../vulkan.hlsli"
#include "../PathTraceMaterialFeatureTypes.hlsli"
#include "../cleanroom_common/pathtrace_liquid_pool_control.hlsli"
#include "../cleanroom_common/pathtrace_liquid_pool_modifier.hlsli"

// GEO-08 compact skinned-hit library for the clean ReSTIR-GI ray pipeline.
//
// Keep the hit payload, material rejection, texture sampling, and liquid-pool
// candidate behavior aligned with pathtrace_clean_restir_gi.rt.hlsl.

#ifndef CLEAN_GI_HIT_ANY_EXPORT
#define CLEAN_GI_HIT_ANY_EXPORT CleanGiSkinnedAnyHit
#endif
#ifndef CLEAN_GI_HIT_SHADOW_ANY_EXPORT
#define CLEAN_GI_HIT_SHADOW_ANY_EXPORT CleanGiSkinnedShadowAnyHit
#endif
#ifndef CLEAN_GI_HIT_CLOSEST_EXPORT
#define CLEAN_GI_HIT_CLOSEST_EXPORT CleanGiSkinnedClosestHit
#endif
#ifndef CLEAN_GI_HIT_SHADOW_CLOSEST_EXPORT
#define CLEAN_GI_HIT_SHADOW_CLOSEST_EXPORT CleanGiSkinnedShadowClosestHit
#endif

static const uint CLEAN_RESTIR_GI_LIQUID_POOL_CANDIDATE_CAPACITY = 4u;

struct CleanGiLiquidPoolCandidateSet
{
    uint rawCount;
    uint retainedCount;
    uint statusMask;
    uint rejectionCount;
    uint instanceId[CLEAN_RESTIR_GI_LIQUID_POOL_CANDIDATE_CAPACITY];
    uint materialIndex[CLEAN_RESTIR_GI_LIQUID_POOL_CANDIDATE_CAPACITY];
    uint primitiveIndex[CLEAN_RESTIR_GI_LIQUID_POOL_CANDIDATE_CAPACITY];
    uint barycentricXBits[CLEAN_RESTIR_GI_LIQUID_POOL_CANDIDATE_CAPACITY];
    uint barycentricYBits[CLEAN_RESTIR_GI_LIQUID_POOL_CANDIDATE_CAPACITY];
    float hitT[CLEAN_RESTIR_GI_LIQUID_POOL_CANDIDATE_CAPACITY];
};

struct PathTraceCleanRestirGiPayload
{
    uint value;
    uint rayMode;
    uint ignoreInstanceId;
    uint ignorePrimitiveIndex;
    uint ignoreMaterialIndex;
    uint hitInstanceId;
    uint hitPrimitiveIndex;
    float hitT;
    float2 hitBarycentrics;
    CleanGiLiquidPoolCandidateSet liquidPool;
};

struct PathTraceSmokeMaterial
{
    float4 debugAlbedo;
    float4 emissiveColor;
    uint diffuseTextureIndex;
    uint alphaTextureIndex;
    uint normalTextureIndex;
    uint specularTextureIndex;
    uint emissiveTextureIndex;
    float alphaCutoff;
    uint flags;
    uint textureWidth;
    uint textureHeight;
    uint alphaTextureWidth;
    uint alphaTextureHeight;
    uint normalTextureWidth;
    uint normalTextureHeight;
    uint specularTextureWidth;
    uint specularTextureHeight;
    uint emissiveTextureWidth;
    uint emissiveTextureHeight;
    uint padding0;
    uint padding1;
    uint padding2;
};

struct PathTraceDynamicMaterialRecord
{
    float4 color;
    float4 texMatrix0;
    float4 texMatrix1;
    uint materialIndex;
    uint materialId;
    uint stageIndex;
    uint flags;
};

struct PathTraceSmokeVertex
{
    float4 position;
    float4 normal;
    float4 texCoord;
    float4 color;
    float4 color2;
    float4 tangent;
    float4 bitangent;
};

#include "../PathTraceSkinnedHitRoute.hlsli"
#define CleanGiHitRouteRecord PathTraceSkinnedHitRouteGpuRecord
#define CleanGiHitRouteTriangle PathTraceSkinnedHitRouteGpuTriangle

StructuredBuffer<PathTraceSmokeMaterial> SmokeMaterials : register(t13);
Texture2D<float4> SmokeFallbackTexture : register(t14);
StructuredBuffer<PathTraceSmokeVertex>
    SmokeSkinnedCurrentVertices : register(t29);
StructuredBuffer<PathTraceDynamicMaterialRecord>
    SmokeDynamicMaterials : register(t76);
StructuredBuffer<PathTraceMaterialFeatureParameterRecord>
    PathTraceMaterialFeatureParameters : register(t87);
VK_BINDING(0, 1) Texture2D<float4>
    SmokeDiffuseTextures[] : register(t0, space1);
SamplerState SmokeMaterialSampler : register(s0);

// Frozen byte offsets in PathTraceCleanRestirGiConstants:
//   144 = mirrored DI TextureInfo
//   464 = mirrored DI EmissiveDistributionInfo
//   516 = GI frame index
//   672 = GI liquid mode
//   684 = GI liquid control flags
//   688 = GI liquid parameter count
cbuffer PathTraceCleanRestirGiSkinnedHitConstants : register(b2)
{
    float4 CleanRtxdiDiTextureInfo : packoffset(c9);
    float4 CleanRtxdiDiEmissiveDistributionInfo : packoffset(c29);
    uint CleanRestirGiFrameIndex : packoffset(c32.y);
    uint CleanRestirGiLiquidPoolMode : packoffset(c42.x);
    uint CleanRestirGiLiquidPoolControlFlags : packoffset(c42.w);
    uint CleanRestirGiLiquidPoolParameterCount : packoffset(c43.x);
};

static const uint RT_SMOKE_TRIANGLE_CLASS_MASK = 0x0000ffffu;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_SHIFT = 24u;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_MASK = 0x0f000000u;
static const uint RT_SMOKE_SURFACE_CLASS_TRANSLUCENT = 3u;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_OBJECT_GLASS = 1u;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_PORTAL_WINDOW = 4u;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_GUI_SCREEN = 5u;

static const uint RT_SMOKE_MATERIAL_ALPHA_TEST = 0x00000001u;
static const uint RT_SMOKE_MATERIAL_DIFFUSE_YCOCG = 0x00000002u;
static const uint RT_SMOKE_MATERIAL_ADDITIVE_DECAL = 0x00000004u;
static const uint RT_SMOKE_MATERIAL_EMISSIVE = 0x00000008u;
static const uint RT_SMOKE_MATERIAL_FILTER_DECAL = 0x00000010u;
static const uint RT_SMOKE_MATERIAL_FILTER_DECAL_BLACK_KEY = 0x00000020u;
static const uint RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_LUMA = 0x00000040u;
static const uint RT_SMOKE_MATERIAL_FORCE_DEBUG_ALBEDO = 0x00000080u;
static const uint RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_DARK_KEY = 0x00000100u;
static const uint RT_SMOKE_MATERIAL_PORTAL_WINDOW_FALLBACK = 0x00000200u;
static const uint RT_SMOKE_MATERIAL_OBJECT_GLASS_FALLBACK = 0x00000400u;
static const uint RT_SMOKE_MATERIAL_ADDITIVE_DECAL_WHITE_KEY = 0x00000800u;
static const uint RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_MAGENTA_KEY = 0x00001000u;

static const uint RT_SMOKE_DYNAMIC_MATERIAL_RECORD_VALID = 0x00000001u;
static const uint RT_SMOKE_DYNAMIC_MATERIAL_RECORD_STAGE_ENABLED = 0x00000002u;
static const uint RT_SMOKE_DYNAMIC_MATERIAL_RECORD_SELECTED_EMISSIVE = 0x00000004u;
static const uint RT_SMOKE_DYNAMIC_MATERIAL_RECORD_REPLACE_EMISSIVE = 0x00000200u;
static const uint RT_SMOKE_MATERIAL_DYNAMIC_EMISSIVE_REGISTER_MASK = 0x0000003cu;

uint CleanGiSkinnedTriangleSurfaceClass(uint triangleClassAndFlags)
{
    return triangleClassAndFlags & RT_SMOKE_TRIANGLE_CLASS_MASK;
}

uint CleanGiSkinnedTriangleTranslucentSubtype(uint triangleClassAndFlags)
{
    return (triangleClassAndFlags &
        RT_SMOKE_TRANSLUCENT_SUBTYPE_MASK) >>
        RT_SMOKE_TRANSLUCENT_SUBTYPE_SHIFT;
}

bool CleanGiSkinnedLoadRouteTriangle(
    uint instanceId,
    uint primitiveIndex,
    out CleanGiHitRouteRecord route,
    out CleanGiHitRouteTriangle routeTriangle)
{
    route = (CleanGiHitRouteRecord)0;
    routeTriangle = (CleanGiHitRouteTriangle)0;
    return PathTraceLoadSkinnedHitRoute(instanceId, route) &&
        PathTraceLoadSkinnedHitRouteTriangle(
            route,
            primitiveIndex,
            routeTriangle);
}

bool CleanGiSkinnedLoadTriangleVertices(
    uint instanceId,
    uint primitiveIndex,
    out CleanGiHitRouteTriangle routeTriangle,
    out PathTraceSmokeVertex v0,
    out PathTraceSmokeVertex v1,
    out PathTraceSmokeVertex v2)
{
    routeTriangle = (CleanGiHitRouteTriangle)0;
    v0 = (PathTraceSmokeVertex)0;
    v1 = (PathTraceSmokeVertex)0;
    v2 = (PathTraceSmokeVertex)0;

    PathTraceSkinnedHitRouteGpuRecord route;
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

    v0 = SmokeSkinnedCurrentVertices[vertexIndex0];
    v1 = SmokeSkinnedCurrentVertices[vertexIndex1];
    v2 = SmokeSkinnedCurrentVertices[vertexIndex2];
    return true;
}

uint CleanGiSkinnedDynamicMaterialRecordCount()
{
    return (uint)max(CleanRtxdiDiEmissiveDistributionInfo.w, 0.0);
}

void CleanGiSkinnedApplyDynamicMaterialRecord(
    uint materialIndex,
    inout PathTraceSmokeMaterial material)
{
    const uint recordCount =
        CleanGiSkinnedDynamicMaterialRecordCount();
    if (materialIndex >= recordCount)
    {
        return;
    }

    const PathTraceDynamicMaterialRecord record =
        SmokeDynamicMaterials[materialIndex];
    if ((record.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_VALID) == 0u ||
        record.materialIndex != materialIndex ||
        (record.flags &
            RT_SMOKE_DYNAMIC_MATERIAL_RECORD_SELECTED_EMISSIVE) == 0u)
    {
        return;
    }
    if ((material.flags & RT_SMOKE_MATERIAL_EMISSIVE) == 0u ||
        (material.padding0 &
            RT_SMOKE_MATERIAL_DYNAMIC_EMISSIVE_REGISTER_MASK) == 0u)
    {
        return;
    }

    const bool stageEnabled =
        (record.flags &
            RT_SMOKE_DYNAMIC_MATERIAL_RECORD_STAGE_ENABLED) != 0u &&
        record.texMatrix0.w != 0.0 &&
        max(max(record.color.r, record.color.g), record.color.b) > 1.0e-5;
    if (!stageEnabled)
    {
        material.emissiveColor = float4(0.0, 0.0, 0.0, 1.0);
        material.flags &= ~RT_SMOKE_MATERIAL_EMISSIVE;
        return;
    }

    const float stageAlpha = saturate(record.color.a);
    const float3 stageScale =
        max(record.color.rgb, float3(0.0, 0.0, 0.0)) * stageAlpha;
    if ((record.flags &
            RT_SMOKE_DYNAMIC_MATERIAL_RECORD_REPLACE_EMISSIVE) != 0u)
    {
        material.emissiveColor.rgb = stageScale;
    }
    else
    {
        material.emissiveColor.rgb *= stageScale;
    }
    material.emissiveColor.a = stageAlpha;
    if (max(max(material.emissiveColor.r, material.emissiveColor.g),
            material.emissiveColor.b) <= 1.0e-5)
    {
        material.flags &= ~RT_SMOKE_MATERIAL_EMISSIVE;
    }
    else
    {
        material.flags |= RT_SMOKE_MATERIAL_EMISSIVE;
    }
}

PathTraceSmokeMaterial CleanGiSkinnedLoadSmokeMaterial(uint materialIndex)
{
    PathTraceSmokeMaterial material = (PathTraceSmokeMaterial)0;
    material.debugAlbedo = float4(0.5, 0.5, 0.5, 1.0);
    material.diffuseTextureIndex = 0xffffffffu;
    material.alphaTextureIndex = 0xffffffffu;
    material.normalTextureIndex = 0xffffffffu;
    material.specularTextureIndex = 0xffffffffu;
    material.emissiveTextureIndex = 0xffffffffu;
    material.textureWidth = 1u;
    material.textureHeight = 1u;
    material.alphaTextureWidth = 1u;
    material.alphaTextureHeight = 1u;
    material.normalTextureWidth = 1u;
    material.normalTextureHeight = 1u;
    material.specularTextureWidth = 1u;
    material.specularTextureHeight = 1u;
    material.emissiveTextureWidth = 1u;
    material.emissiveTextureHeight = 1u;
    const uint materialCount =
        (uint)max(CleanRtxdiDiTextureInfo.z, 0.0);
    if (materialIndex < materialCount)
    {
        material = SmokeMaterials[materialIndex];
        CleanGiSkinnedApplyDynamicMaterialRecord(
            materialIndex,
            material);
    }
    return material;
}

float4 CleanGiSkinnedTextureLoad(
    uint textureIndex,
    uint textureWidth,
    uint textureHeight,
    float2 wrappedTexCoord,
    bool bindlessEnabled,
    bool bilinearFilter)
{
    const uint width = max(textureWidth, 1u);
    const uint height = max(textureHeight, 1u);
    if (!bilinearFilter)
    {
        const uint2 texel = min(
            (uint2)floor(wrappedTexCoord * float2(width, height)),
            uint2(width - 1u, height - 1u));
        return bindlessEnabled
            ? SmokeDiffuseTextures[
                NonUniformResourceIndex(textureIndex)].Load(
                    int3(texel, 0))
            : SmokeFallbackTexture.Load(int3(0, 0, 0));
    }

    const float2 scaled =
        wrappedTexCoord * float2(width, height) - float2(0.5, 0.5);
    const int2 baseTexel = (int2)floor(scaled);
    const float2 fracPart = frac(scaled);
    const uint2 texel00 = uint2(
        (baseTexel.x % (int)width + (int)width) % (int)width,
        (baseTexel.y % (int)height + (int)height) % (int)height);
    const uint2 texel10 = uint2((texel00.x + 1u) % width, texel00.y);
    const uint2 texel01 = uint2(texel00.x, (texel00.y + 1u) % height);
    const uint2 texel11 =
        uint2((texel00.x + 1u) % width, (texel00.y + 1u) % height);
    const float4 c00 = bindlessEnabled
        ? SmokeDiffuseTextures[
            NonUniformResourceIndex(textureIndex)].Load(
                int3(texel00, 0))
        : SmokeFallbackTexture.Load(int3(0, 0, 0));
    const float4 c10 = bindlessEnabled
        ? SmokeDiffuseTextures[
            NonUniformResourceIndex(textureIndex)].Load(
                int3(texel10, 0))
        : SmokeFallbackTexture.Load(int3(0, 0, 0));
    const float4 c01 = bindlessEnabled
        ? SmokeDiffuseTextures[
            NonUniformResourceIndex(textureIndex)].Load(
                int3(texel01, 0))
        : SmokeFallbackTexture.Load(int3(0, 0, 0));
    const float4 c11 = bindlessEnabled
        ? SmokeDiffuseTextures[
            NonUniformResourceIndex(textureIndex)].Load(
                int3(texel11, 0))
        : SmokeFallbackTexture.Load(int3(0, 0, 0));
    return lerp(
        lerp(c00, c10, fracPart.x),
        lerp(c01, c11, fracPart.x),
        fracPart.y);
}

float4 CleanGiSkinnedSampleTexture(
    uint textureIndex,
    uint textureWidth,
    uint textureHeight,
    float2 texCoord,
    float4 fallback)
{
    const uint textureCount = (uint)max(CleanRtxdiDiTextureInfo.x, 0.0);
    const uint sampleMethod = (uint)max(CleanRtxdiDiTextureInfo.y, 0.0);
    const uint textureFlags = (uint)max(CleanRtxdiDiTextureInfo.w, 0.0);
    const bool bindlessEnabled = (textureFlags & 1u) != 0u;
    const bool bilinearFilter = (textureFlags & 2u) != 0u;
    if (sampleMethod == 0u ||
        textureIndex == 0xffffffffu ||
        textureIndex >= textureCount ||
        !all(texCoord == texCoord) ||
        any(abs(texCoord) > 65536.0))
    {
        return fallback;
    }

    const float2 wrappedTexCoord = frac(texCoord);
    const float4 sampled = sampleMethod == 2u
        ? CleanGiSkinnedTextureLoad(
            textureIndex,
            textureWidth,
            textureHeight,
            wrappedTexCoord,
            bindlessEnabled,
            bilinearFilter)
        : (bindlessEnabled
            ? SmokeDiffuseTextures[
                NonUniformResourceIndex(textureIndex)].SampleLevel(
                    SmokeMaterialSampler,
                    wrappedTexCoord,
                    0.0)
            : SmokeFallbackTexture.SampleLevel(
                SmokeMaterialSampler,
                wrappedTexCoord,
                0.0));
    return (!all(sampled == sampled) || any(abs(sampled) > 65504.0))
        ? fallback
        : sampled;
}

float3 CleanGiSkinnedConvertYCoCgToRGB(float4 ycocg)
{
    ycocg.z = (ycocg.z * 31.875) + 1.0;
    ycocg.z = 1.0 / ycocg.z;
    ycocg.xy *= ycocg.z;
    return saturate(float3(
        dot(ycocg, float4(1.0, -1.0, 0.0, 1.0)),
        dot(ycocg, float4(0.0, 1.0, -0.50196078, 1.0)),
        dot(ycocg, float4(-1.0, -1.0, 1.00392156, 1.0))));
}

float4 CleanGiSkinnedSampleDecodedDiffuseTexture(
    PathTraceSmokeMaterial material,
    float2 texCoord)
{
    float4 texel = CleanGiSkinnedSampleTexture(
        material.diffuseTextureIndex,
        material.textureWidth,
        material.textureHeight,
        texCoord,
        material.debugAlbedo);
    const bool textureDecodeEnabled =
        (((uint)CleanRtxdiDiTextureInfo.w) & 4u) != 0u;
    if (textureDecodeEnabled &&
        (material.flags & RT_SMOKE_MATERIAL_DIFFUSE_YCOCG) != 0u)
    {
        texel.rgb = CleanGiSkinnedConvertYCoCgToRGB(texel);
    }
    return texel;
}

float4 CleanGiSkinnedSampleDiffuseTexture(
    PathTraceSmokeMaterial material,
    float2 texCoord)
{
    return (material.flags &
            RT_SMOKE_MATERIAL_FORCE_DEBUG_ALBEDO) != 0u
        ? material.debugAlbedo
        : CleanGiSkinnedSampleDecodedDiffuseTexture(material, texCoord);
}

float4 CleanGiSkinnedSampleAlphaTexture(
    PathTraceSmokeMaterial material,
    float2 texCoord)
{
    return material.alphaTextureIndex != 0xffffffffu
        ? CleanGiSkinnedSampleTexture(
            material.alphaTextureIndex,
            material.alphaTextureWidth,
            material.alphaTextureHeight,
            texCoord,
            CleanGiSkinnedSampleDiffuseTexture(material, texCoord))
        : CleanGiSkinnedSampleDiffuseTexture(material, texCoord);
}

float CleanGiSkinnedAlphaCoverage(
    PathTraceSmokeMaterial material,
    float2 texCoord)
{
    if ((material.flags &
            RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_DARK_KEY) != 0u)
    {
        const float3 decoded = saturate(
            CleanGiSkinnedSampleDecodedDiffuseTexture(
                material,
                texCoord).rgb);
        return 1.0 - max(max(decoded.r, decoded.g), decoded.b);
    }
    if ((material.flags &
            RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_LUMA) != 0u)
    {
        const float3 decoded = saturate(
            CleanGiSkinnedSampleDecodedDiffuseTexture(
                material,
                texCoord).rgb);
        return max(max(decoded.r, decoded.g), decoded.b);
    }
    if ((material.flags &
            RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_MAGENTA_KEY) != 0u)
    {
        const float3 decoded = saturate(
            CleanGiSkinnedSampleDecodedDiffuseTexture(
                material,
                texCoord).rgb);
        const float keyDistance = max(
            abs(decoded.r - 1.0),
            max(abs(decoded.g), abs(decoded.b - 1.0)));
        return keyDistance <= 0.08 ? 0.0 : 1.0;
    }
    return saturate(
        CleanGiSkinnedSampleAlphaTexture(material, texCoord).a);
}

uint CleanGiSkinnedHash(uint hash)
{
    hash ^= hash >> 16;
    hash *= 2246822519u;
    hash ^= hash >> 13;
    hash *= 3266489917u;
    hash ^= hash >> 16;
    return hash;
}

float CleanGiSkinnedVisibilityRandom(
    uint2 pixel,
    uint instanceId,
    uint primitiveIndex,
    uint salt)
{
    const uint hash = CleanGiSkinnedHash(
        instanceId * 1597334677u ^
        primitiveIndex * 3812015801u ^
        pixel.x * 2798796415u ^
        pixel.y * 1979697957u ^
        CleanRestirGiFrameIndex * 3266489917u ^
        salt);
    return ((hash >> 8) & 0x00ffffffu) *
        (1.0 / 16777215.0);
}

bool CleanGiSkinnedMaterialRejectsHit(
    uint2 pixel,
    uint instanceId,
    uint primitiveIndex,
    float2 barycentrics,
    bool shadowRay)
{
    CleanGiHitRouteTriangle routeTriangle;
    PathTraceSmokeVertex v0;
    PathTraceSmokeVertex v1;
    PathTraceSmokeVertex v2;
    if (!CleanGiSkinnedLoadTriangleVertices(
            instanceId,
            primitiveIndex,
            routeTriangle,
            v0,
            v1,
            v2))
    {
        return false;
    }

    const uint materialIndex = routeTriangle.materialIndex;
    const uint materialCount =
        (uint)max(CleanRtxdiDiTextureInfo.z, 0.0);
    if (materialIndex >= materialCount)
    {
        return false;
    }

    const PathTraceSmokeMaterial material =
        CleanGiSkinnedLoadSmokeMaterial(materialIndex);
    const uint surfaceClass =
        CleanGiSkinnedTriangleSurfaceClass(
            routeTriangle.triangleClassAndFlags);
    const uint translucentSubtype =
        CleanGiSkinnedTriangleTranslucentSubtype(
            routeTriangle.triangleClassAndFlags);
    const bool glassTransmissionSurface =
        (surfaceClass == RT_SMOKE_SURFACE_CLASS_TRANSLUCENT &&
            (translucentSubtype ==
                    RT_SMOKE_TRANSLUCENT_SUBTYPE_OBJECT_GLASS ||
                translucentSubtype ==
                    RT_SMOKE_TRANSLUCENT_SUBTYPE_PORTAL_WINDOW)) ||
        (material.flags &
            (RT_SMOKE_MATERIAL_OBJECT_GLASS_FALLBACK |
                RT_SMOKE_MATERIAL_PORTAL_WINDOW_FALLBACK)) != 0u;
    if (glassTransmissionSurface)
    {
        return true;
    }

    const float b1 = saturate(barycentrics.x);
    const float b2 = saturate(barycentrics.y);
    const float b0 = saturate(1.0 - b1 - b2);
    const float2 texCoord =
        v0.texCoord.xy * b0 +
        v1.texCoord.xy * b1 +
        v2.texCoord.xy * b2;
    const float4 vertexColor =
        saturate(v0.color * b0 + v1.color * b1 + v2.color * b2);

    if (surfaceClass == RT_SMOKE_SURFACE_CLASS_TRANSLUCENT &&
        translucentSubtype == RT_SMOKE_TRANSLUCENT_SUBTYPE_GUI_SCREEN &&
        vertexColor.a <= 0.03)
    {
        return true;
    }

    const float coverage =
        saturate(CleanGiSkinnedAlphaCoverage(material, texCoord));
    if ((material.flags & RT_SMOKE_MATERIAL_ALPHA_TEST) != 0u &&
        coverage < material.alphaCutoff)
    {
        return true;
    }

    float visibilityCoverage = coverage;
    if ((material.flags & RT_SMOKE_MATERIAL_ADDITIVE_DECAL) != 0u)
    {
        const float3 albedo = saturate(
            CleanGiSkinnedSampleDiffuseTexture(
                material,
                texCoord).rgb);
        visibilityCoverage =
            (material.flags &
                RT_SMOKE_MATERIAL_ADDITIVE_DECAL_WHITE_KEY) != 0u
            ? 1.0 - min(min(albedo.r, albedo.g), albedo.b)
            : max(max(albedo.r, albedo.g), albedo.b);
        visibilityCoverage = saturate(visibilityCoverage * 0.5);
    }
    else if ((material.flags & RT_SMOKE_MATERIAL_FILTER_DECAL) != 0u)
    {
        const float3 albedo = saturate(
            CleanGiSkinnedSampleDiffuseTexture(
                material,
                texCoord).rgb);
        const bool blackKey =
            (material.flags &
                RT_SMOKE_MATERIAL_FILTER_DECAL_BLACK_KEY) != 0u;
        visibilityCoverage = blackKey
            ? max(max(albedo.r, albedo.g), albedo.b)
            : 1.0 - min(min(albedo.r, albedo.g), albedo.b);
        visibilityCoverage = saturate(visibilityCoverage * 0.5);
    }

    const uint alphaMaterialFlags =
        RT_SMOKE_MATERIAL_ADDITIVE_DECAL |
        RT_SMOKE_MATERIAL_FILTER_DECAL |
        RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_DARK_KEY |
        RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_LUMA |
        RT_SMOKE_MATERIAL_ADDITIVE_DECAL_WHITE_KEY |
        RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_MAGENTA_KEY;
    const bool alphaDriven =
        material.alphaTextureIndex != 0xffffffffu ||
        (material.flags & alphaMaterialFlags) != 0u;
    if (!alphaDriven)
    {
        return false;
    }
    if (visibilityCoverage <= 0.001)
    {
        return true;
    }
    if (visibilityCoverage >= 0.999)
    {
        return false;
    }

    const uint salt = shadowRay ? 0xa8f31b2du : 0x51d734bbu;
    return CleanGiSkinnedVisibilityRandom(
        pixel,
        instanceId,
        primitiveIndex,
        salt) > visibilityCoverage;
}

bool CleanGiSkinnedLiquidPoolCollectionEnabled()
{
    const uint required =
        RT_LIQUID_POOL_CONTROL_REQUESTED |
        RT_LIQUID_POOL_CONTROL_PARAMETERS_READY;
    return CleanRestirGiLiquidPoolMode != 0u &&
        (CleanRestirGiLiquidPoolControlFlags & required) == required &&
        (CleanRestirGiLiquidPoolControlFlags &
            RT_LIQUID_POOL_CONTROL_ROUTE_DISABLED) == 0u;
}

bool CleanGiSkinnedLiquidPoolParameterRowIsTyped(
    PathTraceMaterialFeatureParameterRecord parameters)
{
    return all(isfinite(parameters.params0)) &&
        all(isfinite(parameters.params1)) &&
        all(parameters.params0.xyz >=
            RT_PATH_TRACE_LIQUID_POOL_TRANSMITTANCE_MIN) &&
        all(parameters.params0.xyz <= 1.0) &&
        parameters.params0.w >= 0.0 &&
        parameters.params0.w <=
            RT_PATH_TRACE_LIQUID_POOL_OPTICAL_DEPTH_MAX &&
        parameters.params1.x >=
            RT_PATH_TRACE_LIQUID_POOL_COAT_ROUGHNESS_MIN &&
        parameters.params1.x <=
            RT_PATH_TRACE_LIQUID_POOL_COAT_ROUGHNESS_MAX &&
        parameters.params1.y >=
            RT_PATH_TRACE_LIQUID_POOL_DIELECTRIC_IOR_MIN &&
        parameters.params1.y <=
            RT_PATH_TRACE_LIQUID_POOL_DIELECTRIC_IOR_MAX;
}

bool CleanGiSkinnedMaterialIsSemanticLiquidPool(uint materialIndex)
{
    uint allocatedCount = 0u;
    uint allocatedStride = 0u;
    PathTraceMaterialFeatureParameters.GetDimensions(
        allocatedCount,
        allocatedStride);
    const uint materialCount =
        (uint)max(CleanRtxdiDiTextureInfo.z, 0.0);
    if (CleanRestirGiLiquidPoolParameterCount != materialCount ||
        allocatedCount < materialCount ||
        allocatedStride !=
            RT_PATH_TRACE_MATERIAL_FEATURE_PARAMETER_RECORD_STRIDE ||
        materialIndex >= materialCount)
    {
        return false;
    }

    const PathTraceSmokeMaterial material =
        CleanGiSkinnedLoadSmokeMaterial(materialIndex);
    const PathTraceMaterialFeatureParameterRecord parameters =
        PathTraceMaterialFeatureParameters[materialIndex];
    return (material.flags &
            RT_PATH_TRACE_FEATURE_MATERIAL_DETAIL_DECAL_LIQUID_POOL) != 0u &&
        CleanGiSkinnedLiquidPoolParameterRowIsTyped(parameters);
}

bool CleanGiSkinnedTryGetLiquidPoolStageColor(
    uint materialIndex,
    out float4 stageColor)
{
    stageColor = float4(1.0, 1.0, 1.0, 1.0);
    uint recordCount = 0u;
    uint recordStride = 0u;
    SmokeDynamicMaterials.GetDimensions(recordCount, recordStride);
    if (materialIndex >= recordCount)
    {
        return true;
    }

    const PathTraceDynamicMaterialRecord record =
        SmokeDynamicMaterials[materialIndex];
    if ((record.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_VALID) == 0u ||
        record.materialIndex != materialIndex)
    {
        return true;
    }
    if ((record.flags &
            RT_SMOKE_DYNAMIC_MATERIAL_RECORD_STAGE_ENABLED) == 0u ||
        record.texMatrix0.w == 0.0)
    {
        return false;
    }

    stageColor = float4(
        max(record.color.rgb, 0.0),
        saturate(record.color.a));
    return stageColor.a > 0.0;
}

void CleanGiSkinnedStoreLiquidPoolCandidate(
    inout CleanGiLiquidPoolCandidateSet candidates,
    uint instanceId,
    uint materialIndex,
    uint primitiveIndex,
    float2 barycentrics,
    float hitT)
{
    const LiquidPoolContributorKey key =
        LiquidPoolMakeContributorKey(
            instanceId,
            primitiveIndex,
            materialIndex,
            barycentrics);
    candidates.statusMask |= RT_LIQUID_POOL_STATUS_CANDIDATE;
    [unroll]
    for (uint slot = 0u;
        slot < CLEAN_RESTIR_GI_LIQUID_POOL_CANDIDATE_CAPACITY;
        ++slot)
    {
        if (slot < candidates.retainedCount &&
            candidates.instanceId[slot] == key.instanceId &&
            candidates.primitiveIndex[slot] == key.primitiveIndex &&
            candidates.materialIndex[slot] == key.materialIndex &&
            candidates.barycentricXBits[slot] ==
                key.barycentricXBits &&
            candidates.barycentricYBits[slot] ==
                key.barycentricYBits)
        {
            return;
        }
    }

    candidates.rawCount = candidates.rawCount == 0xffffffffu
        ? 0xffffffffu
        : candidates.rawCount + 1u;
    if (candidates.retainedCount >=
        CLEAN_RESTIR_GI_LIQUID_POOL_CANDIDATE_CAPACITY)
    {
        candidates.statusMask |= RT_LIQUID_POOL_STATUS_OVERFLOW;
        return;
    }

    const uint slot = candidates.retainedCount++;
    candidates.instanceId[slot] = key.instanceId;
    candidates.materialIndex[slot] = key.materialIndex;
    candidates.primitiveIndex[slot] = key.primitiveIndex;
    candidates.barycentricXBits[slot] = key.barycentricXBits;
    candidates.barycentricYBits[slot] = key.barycentricYBits;
    candidates.hitT[slot] = hitT;
}

bool CleanGiSkinnedCollectLiquidPoolCandidate(
    inout CleanGiLiquidPoolCandidateSet candidates,
    uint instanceId,
    uint primitiveIndex,
    uint materialIndex,
    float2 barycentrics,
    float hitT)
{
    if (!CleanGiSkinnedLiquidPoolCollectionEnabled() ||
        !CleanGiSkinnedMaterialIsSemanticLiquidPool(materialIndex))
    {
        return false;
    }
    candidates.statusMask |= RT_LIQUID_POOL_STATUS_CANDIDATE;

    CleanGiHitRouteTriangle routeTriangle;
    PathTraceSmokeVertex v0;
    PathTraceSmokeVertex v1;
    PathTraceSmokeVertex v2;
    if (!CleanGiSkinnedLoadTriangleVertices(
            instanceId,
            primitiveIndex,
            routeTriangle,
            v0,
            v1,
            v2))
    {
        candidates.statusMask |= RT_LIQUID_POOL_STATUS_FAIL_CLOSED;
        candidates.rejectionCount =
            candidates.rejectionCount == 0xffffffffu
            ? 0xffffffffu
            : candidates.rejectionCount + 1u;
        return true;
    }

    const float b1 = saturate(barycentrics.x);
    const float b2 = saturate(barycentrics.y);
    const float b0 = saturate(1.0 - b1 - b2);
    const float3 cardPosition =
        v0.position.xyz * b0 +
        v1.position.xyz * b1 +
        v2.position.xyz * b2;
    const float3 cardPlaneNormal =
        cross(v1.position.xyz - v0.position.xyz,
            v2.position.xyz - v0.position.xyz);
    const float2 cardTexCoord =
        v0.texCoord.xy * b0 +
        v1.texCoord.xy * b1 +
        v2.texCoord.xy * b2;
    if (!LiquidPoolFinite3(cardPosition) ||
        !LiquidPoolFinite3(cardPlaneNormal) ||
        !LiquidPoolFinite2(cardTexCoord))
    {
        candidates.statusMask |= RT_LIQUID_POOL_STATUS_FAIL_CLOSED;
        candidates.rejectionCount =
            candidates.rejectionCount == 0xffffffffu
            ? 0xffffffffu
            : candidates.rejectionCount + 1u;
        return true;
    }

    const PathTraceSmokeMaterial material =
        CleanGiSkinnedLoadSmokeMaterial(materialIndex);
    float4 stageColor;
    if (!CleanGiSkinnedTryGetLiquidPoolStageColor(
            materialIndex,
            stageColor))
    {
        candidates.statusMask |= RT_LIQUID_POOL_STATUS_FAIL_CLOSED;
        candidates.rejectionCount =
            candidates.rejectionCount == 0xffffffffu
            ? 0xffffffffu
            : candidates.rejectionCount + 1u;
        return true;
    }

    const float coverage =
        saturate(CleanGiSkinnedAlphaCoverage(
            material,
            cardTexCoord)) *
        saturate(stageColor.a);
    if (coverage > 0.0)
    {
        CleanGiSkinnedStoreLiquidPoolCandidate(
            candidates,
            instanceId,
            materialIndex,
            primitiveIndex,
            barycentrics,
            hitT);
    }
    return true;
}

[shader("anyhit")]
void CLEAN_GI_HIT_ANY_EXPORT(
    inout PathTraceCleanRestirGiPayload payload,
    BuiltInTriangleIntersectionAttributes attributes)
{
    const uint instanceId = InstanceID();
    const uint primitiveIndex = PrimitiveIndex();
    CleanGiHitRouteRecord route;
    CleanGiHitRouteTriangle routeTriangle;
    if (!CleanGiSkinnedLoadRouteTriangle(
            instanceId,
            primitiveIndex,
            route,
            routeTriangle))
    {
        return;
    }

    if (CleanGiSkinnedCollectLiquidPoolCandidate(
            payload.liquidPool,
            instanceId,
            primitiveIndex,
            routeTriangle.materialIndex,
            attributes.barycentrics,
            RayTCurrent()))
    {
        IgnoreHit();
        return;
    }
    if (payload.rayMode != 0u)
    {
        return;
    }
    if (CleanGiSkinnedMaterialRejectsHit(
            DispatchRaysIndex().xy,
            instanceId,
            primitiveIndex,
            attributes.barycentrics,
            false))
    {
        IgnoreHit();
    }
}

[shader("anyhit")]
void CLEAN_GI_HIT_SHADOW_ANY_EXPORT(
    inout PathTraceCleanRestirGiPayload payload,
    BuiltInTriangleIntersectionAttributes attributes)
{
    const uint instanceId = InstanceID();
    const uint primitiveIndex = PrimitiveIndex();
    CleanGiHitRouteRecord route;
    CleanGiHitRouteTriangle routeTriangle;
    if (!CleanGiSkinnedLoadRouteTriangle(
            instanceId,
            primitiveIndex,
            route,
            routeTriangle))
    {
        return;
    }

    if (CleanGiSkinnedLiquidPoolCollectionEnabled() &&
        CleanGiSkinnedMaterialIsSemanticLiquidPool(
            routeTriangle.materialIndex))
    {
        IgnoreHit();
        return;
    }
    if (CleanGiSkinnedMaterialRejectsHit(
            DispatchRaysIndex().xy,
            instanceId,
            primitiveIndex,
            attributes.barycentrics,
            true))
    {
        IgnoreHit();
    }
}

[shader("closesthit")]
void CLEAN_GI_HIT_CLOSEST_EXPORT(
    inout PathTraceCleanRestirGiPayload payload,
    BuiltInTriangleIntersectionAttributes attributes)
{
    payload.value = 1u;
    payload.hitInstanceId = InstanceID();
    payload.hitPrimitiveIndex = PrimitiveIndex();
    payload.hitT = RayTCurrent();
    payload.hitBarycentrics = attributes.barycentrics;
}

[shader("closesthit")]
void CLEAN_GI_HIT_SHADOW_CLOSEST_EXPORT(
    inout PathTraceCleanRestirGiPayload payload,
    BuiltInTriangleIntersectionAttributes attributes)
{
    payload.value = 1u;
}
