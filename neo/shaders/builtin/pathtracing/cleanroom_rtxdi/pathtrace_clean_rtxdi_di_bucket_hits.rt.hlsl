#include "../../../vulkan.hlsli"
#include "../PathTraceMaterialFeatureTypes.hlsli"
#include "../cleanroom_common/pathtrace_liquid_pool_control.hlsli"

// GEO-10 compact hit-only library for static-bucket clean-DI traversal.
//
// Keep this module independent from pathtrace_clean_rtxdi_di_shared.hlsli.
// Large clean-DI libraries retain their stable compiled-out route, while
// bucket instances select these hit groups through a dedicated SBT
// contribution. The module consumes the existing b2 prefix and t13/t80 plus
// the six checked bucket streams already present in the clean binding layout.

struct PathTraceCleanRtxdiPayload
{
    uint value;
    uint rayMode;
    uint ignoreInstanceId;
    uint ignorePrimitiveIndex;
    uint ignoreMaterialIndex;
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

StructuredBuffer<PathTraceSmokeMaterial> SmokeMaterials : register(t13);
StructuredBuffer<PathTraceMaterialFeatureRecord>
    PathTraceMaterialFeatures : register(t80);

cbuffer PathTraceCleanRtxdiDiBucketHitConstants : register(b2)
{
    uint CleanRtxdiDiFlags : packoffset(c2.z);
    uint CleanRtxdiDiTemporalFlags : packoffset(c3.z);
    float4 CleanRtxdiDiTextureInfo : packoffset(c9);
    uint4 CleanRtxdiDiStaticBucketRouteInfo : packoffset(c30);
};

#define RB_PT_STATIC_BUCKET_ROUTES_REGISTER t100
#define RB_PT_STATIC_BUCKET_VERTICES_REGISTER t101
#define RB_PT_STATIC_BUCKET_INDICES_REGISTER t102
#define RB_PT_STATIC_BUCKET_CLASSES_REGISTER t103
#define RB_PT_STATIC_BUCKET_MATERIALS_REGISTER t104
#define RB_PT_STATIC_BUCKET_MATERIAL_INDEXES_REGISTER t105
#define RB_PT_ENABLE_STATIC_BUCKET_SHADER_CONSUMERS 1
#include "../PathTraceStaticBucketRoute.hlsli"
#undef RB_PT_ENABLE_STATIC_BUCKET_SHADER_CONSUMERS
#undef RB_PT_STATIC_BUCKET_ROUTES_REGISTER
#undef RB_PT_STATIC_BUCKET_VERTICES_REGISTER
#undef RB_PT_STATIC_BUCKET_INDICES_REGISTER
#undef RB_PT_STATIC_BUCKET_CLASSES_REGISTER
#undef RB_PT_STATIC_BUCKET_MATERIALS_REGISTER
#undef RB_PT_STATIC_BUCKET_MATERIAL_INDEXES_REGISTER

static const uint CLEAN_FLAG_LIQUID_MODIFIER_VISIBILITY = 1u << 21u;
static const uint CLEAN_LIQUID_MODE_SHIFT = 2u;
static const uint CLEAN_LIQUID_CONTROL_SHIFT = 9u;

static const uint RT_SMOKE_MATERIAL_ADDITIVE_DECAL = 0x00000004u;
static const uint RT_SMOKE_MATERIAL_FILTER_DECAL = 0x00000010u;
static const uint RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_LUMA = 0x00000040u;
static const uint RT_SMOKE_MATERIAL_PORTAL_WINDOW_FALLBACK = 0x00000200u;
static const uint RT_SMOKE_MATERIAL_OBJECT_GLASS_FALLBACK = 0x00000400u;
static const uint RT_SMOKE_MATERIAL_ADDITIVE_DECAL_WHITE_KEY = 0x00000800u;
static const uint RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_MAGENTA_KEY = 0x00001000u;

bool CleanDiBucketLoadTriangle(
    uint instanceId,
    uint primitiveIndex,
    out uint packedTriangleIndex,
    out uint materialIndex)
{
    PathTraceStaticBucketRouteRecord route;
    uint3 packedVertexIndexes;
    packedTriangleIndex = 0u;
    materialIndex = 0xffffffffu;
    if (!PathTraceTryLoadStaticBucketTriangleRoute(
            instanceId,
            primitiveIndex,
            CleanRtxdiDiStaticBucketRouteInfo,
            route,
            packedTriangleIndex,
            packedVertexIndexes))
    {
        return false;
    }

    materialIndex =
        SmokeStaticBucketTriangleMaterialIndexes[packedTriangleIndex];
    return true;
}

bool CleanDiBucketLoadMaterialFeature(
    uint materialIndex,
    out PathTraceMaterialFeature feature)
{
    feature = (PathTraceMaterialFeature)0;
    const uint materialCount =
        (uint)max(CleanRtxdiDiTextureInfo.z, 0.0);
    if (materialIndex >= materialCount)
    {
        return false;
    }

    const PathTraceMaterialFeatureRecord record =
        PathTraceMaterialFeatures[materialIndex];
    if (record.recordAbiVersion !=
        RT_PATH_TRACE_MATERIAL_FEATURE_RECORD_ABI_VERSION)
    {
        return false;
    }

    feature = PathTraceMaterialFeatureFromRecord(record);
    return feature.parameterRecordIndex == materialIndex ||
        feature.parameterRecordIndex == 0xffffffffu;
}

bool CleanDiBucketLiquidPoolCollectionEnabled()
{
    const uint mode =
        (CleanRtxdiDiTemporalFlags >> CLEAN_LIQUID_MODE_SHIFT) & 3u;
    const uint controlFlags =
        (CleanRtxdiDiTemporalFlags >> CLEAN_LIQUID_CONTROL_SHIFT) & 15u;
    return mode != 0u &&
        (controlFlags &
            (RT_LIQUID_POOL_CONTROL_TELEMETRY_READY |
                RT_LIQUID_POOL_CONTROL_REQUESTED |
                RT_LIQUID_POOL_CONTROL_PARAMETERS_READY)) ==
            (RT_LIQUID_POOL_CONTROL_TELEMETRY_READY |
                RT_LIQUID_POOL_CONTROL_REQUESTED |
                RT_LIQUID_POOL_CONTROL_PARAMETERS_READY) &&
        (controlFlags & RT_LIQUID_POOL_CONTROL_ROUTE_DISABLED) == 0u;
}

bool CleanDiBucketMaterialIsSemanticLiquidPool(uint materialIndex)
{
    PathTraceMaterialFeature feature;
    return CleanDiBucketLoadMaterialFeature(materialIndex, feature) &&
        feature.materialKind ==
            RT_PATH_TRACE_MATERIAL_KIND_LIQUID_POOL_MODIFIER &&
        feature.modifierKind ==
            RT_PATH_TRACE_MATERIAL_MODIFIER_LIQUID_POOL_UNION &&
        (feature.materialCaps &
            (RT_PATH_TRACE_MATERIAL_CAP_RECEIVER_MODIFIER |
                RT_PATH_TRACE_MATERIAL_CAP_IDEMPOTENT_MODIFIER_BLEND)) ==
            (RT_PATH_TRACE_MATERIAL_CAP_RECEIVER_MODIFIER |
                RT_PATH_TRACE_MATERIAL_CAP_IDEMPOTENT_MODIFIER_BLEND);
}

bool CleanDiBucketMaterialDoesNotOccludeVisibility(uint materialIndex)
{
    const uint materialCount =
        (uint)max(CleanRtxdiDiTextureInfo.z, 0.0);
    if (materialIndex >= materialCount)
    {
        return false;
    }

    const PathTraceSmokeMaterial material = SmokeMaterials[materialIndex];
    PathTraceMaterialFeature feature;
    if (CleanDiBucketLoadMaterialFeature(materialIndex, feature) &&
        feature.materialKind ==
            RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_GLASS &&
        (feature.materialCaps &
            RT_PATH_TRACE_MATERIAL_CAP_PATH_TRANSMISSION) != 0u)
    {
        return true;
    }
    if ((CleanRtxdiDiFlags &
            CLEAN_FLAG_LIQUID_MODIFIER_VISIBILITY) != 0u &&
        CleanDiBucketMaterialIsSemanticLiquidPool(materialIndex))
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

[shader("anyhit")]
void CleanDiBucketAnyHit(
    inout PathTraceCleanRtxdiPayload payload,
    BuiltInTriangleIntersectionAttributes attributes)
{
    if (payload.rayMode != 2u ||
        InstanceID() != payload.ignoreInstanceId)
    {
        return;
    }

    const uint primitiveIndex = PrimitiveIndex();
    uint packedTriangleIndex;
    uint materialIndex;
    if (CleanDiBucketLoadTriangle(
            InstanceID(),
            primitiveIndex,
            packedTriangleIndex,
            materialIndex) &&
        (primitiveIndex == payload.ignorePrimitiveIndex ||
            materialIndex == payload.ignoreMaterialIndex))
    {
        IgnoreHit();
    }
}

[shader("anyhit")]
void CleanDiBucketShadowAnyHit(
    inout PathTraceCleanRtxdiPayload payload,
    BuiltInTriangleIntersectionAttributes attributes)
{
    const uint instanceId = InstanceID();
    const uint primitiveIndex = PrimitiveIndex();
    uint packedTriangleIndex;
    uint materialIndex;
    if (!CleanDiBucketLoadTriangle(
            instanceId,
            primitiveIndex,
            packedTriangleIndex,
            materialIndex))
    {
        return;
    }

    if ((CleanDiBucketLiquidPoolCollectionEnabled() &&
            CleanDiBucketMaterialIsSemanticLiquidPool(materialIndex)) ||
        CleanDiBucketMaterialDoesNotOccludeVisibility(materialIndex) ||
        (payload.rayMode == 2u &&
            instanceId == payload.ignoreInstanceId &&
            (primitiveIndex == payload.ignorePrimitiveIndex ||
                materialIndex == payload.ignoreMaterialIndex)))
    {
        IgnoreHit();
    }
}

[shader("closesthit")]
void CleanDiBucketClosestHit(
    inout PathTraceCleanRtxdiPayload payload,
    BuiltInTriangleIntersectionAttributes attributes)
{
    payload.value = 1u;
}

[shader("closesthit")]
void CleanDiBucketShadowClosestHit(
    inout PathTraceCleanRtxdiPayload payload,
    BuiltInTriangleIntersectionAttributes attributes)
{
    payload.value = 1u;
}
