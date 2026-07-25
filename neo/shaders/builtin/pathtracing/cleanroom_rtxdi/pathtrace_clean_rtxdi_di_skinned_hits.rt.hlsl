#include "../../../vulkan.hlsli"
#include "../PathTraceMaterialFeatureTypes.hlsli"
#include "../PathTraceSkinnedHitRoute.hlsli"
#include "../cleanroom_common/pathtrace_liquid_pool_control.hlsli"

// GEO-08 compact hit-only library for the clean-DI sentinel pipeline.
//
// The sentinel source is already close to DXC's SPIR-V ID ceiling. Keep this
// module independent from pathtrace_clean_rtxdi_di_shared.hlsli: it owns only
// the payload fields and bindings reachable from the four skinned hit exports.
// Its hit groups are appended after the legacy primary/shadow groups and are
// selected by a TLAS instance contribution of two.

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

StructuredBuffer<PathTraceSmokeMaterial> SmokeMaterials : register(t13);
StructuredBuffer<PathTraceMaterialFeatureRecord>
    PathTraceMaterialFeatures : register(t80);

// These offsets mirror PathTraceCleanRtxdiDiSentinelConstants. Using explicit
// packoffsets makes this module consume the existing b2 binding without
// duplicating the full DI constants declaration.
cbuffer PathTraceCleanRtxdiDiSkinnedHitConstants : register(b2)
{
    uint CleanRtxdiDiFlags : packoffset(c2.z);
    uint CleanRtxdiDiTemporalFlags : packoffset(c3.z);
    float4 CleanRtxdiDiTextureInfo : packoffset(c9);
};

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

bool CleanDiSkinnedLoadTriangle(
    uint instanceId,
    uint primitiveIndex,
    out PathTraceSkinnedHitRouteGpuTriangle routeTriangle)
{
    PathTraceSkinnedHitRouteGpuRecord route;
    routeTriangle = (PathTraceSkinnedHitRouteGpuTriangle)0;
    return PathTraceLoadSkinnedHitRoute(instanceId, route) &&
        PathTraceLoadSkinnedHitRouteTriangle(
            route,
            primitiveIndex,
            routeTriangle);
}

bool CleanDiSkinnedLoadMaterialFeature(
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

bool CleanDiSkinnedLiquidPoolCollectionEnabled()
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

bool CleanDiSkinnedMaterialIsSemanticLiquidPool(uint materialIndex)
{
    PathTraceMaterialFeature feature;
    return CleanDiSkinnedLoadMaterialFeature(materialIndex, feature) &&
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

bool CleanDiSkinnedMaterialDoesNotOccludeVisibility(uint materialIndex)
{
    const uint materialCount =
        (uint)max(CleanRtxdiDiTextureInfo.z, 0.0);
    if (materialIndex >= materialCount)
    {
        return false;
    }

    const PathTraceSmokeMaterial material = SmokeMaterials[materialIndex];
    PathTraceMaterialFeature feature;
    if (CleanDiSkinnedLoadMaterialFeature(materialIndex, feature) &&
        feature.materialKind ==
            RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_GLASS &&
        (feature.materialCaps &
            RT_PATH_TRACE_MATERIAL_CAP_PATH_TRANSMISSION) != 0u)
    {
        return true;
    }
    if ((CleanRtxdiDiFlags &
            CLEAN_FLAG_LIQUID_MODIFIER_VISIBILITY) != 0u &&
        CleanDiSkinnedMaterialIsSemanticLiquidPool(materialIndex))
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
void CleanDiSkinnedAnyHit(
    inout PathTraceCleanRtxdiPayload payload,
    BuiltInTriangleIntersectionAttributes attributes)
{
    const uint instanceId = InstanceID();
    const uint primitiveIndex = PrimitiveIndex();
    PathTraceSkinnedHitRouteGpuTriangle routeTriangle;
    if (!CleanDiSkinnedLoadTriangle(
            instanceId,
            primitiveIndex,
            routeTriangle))
    {
        return;
    }

    if (payload.rayMode == 2u &&
        instanceId == payload.ignoreInstanceId &&
        (primitiveIndex == payload.ignorePrimitiveIndex ||
            routeTriangle.materialIndex == payload.ignoreMaterialIndex))
    {
        IgnoreHit();
    }
}

[shader("anyhit")]
void CleanDiSkinnedShadowAnyHit(
    inout PathTraceCleanRtxdiPayload payload,
    BuiltInTriangleIntersectionAttributes attributes)
{
    const uint instanceId = InstanceID();
    const uint primitiveIndex = PrimitiveIndex();
    PathTraceSkinnedHitRouteGpuTriangle routeTriangle;
    if (!CleanDiSkinnedLoadTriangle(
            instanceId,
            primitiveIndex,
            routeTriangle))
    {
        return;
    }

    const uint materialIndex = routeTriangle.materialIndex;
    if ((CleanDiSkinnedLiquidPoolCollectionEnabled() &&
            CleanDiSkinnedMaterialIsSemanticLiquidPool(materialIndex)) ||
        CleanDiSkinnedMaterialDoesNotOccludeVisibility(materialIndex) ||
        (payload.rayMode == 2u &&
            instanceId == payload.ignoreInstanceId &&
            (primitiveIndex == payload.ignorePrimitiveIndex ||
                materialIndex == payload.ignoreMaterialIndex)))
    {
        IgnoreHit();
    }
}

[shader("closesthit")]
void CleanDiSkinnedClosestHit(
    inout PathTraceCleanRtxdiPayload payload,
    BuiltInTriangleIntersectionAttributes attributes)
{
    payload.value = 1u;
}

[shader("closesthit")]
void CleanDiSkinnedShadowClosestHit(
    inout PathTraceCleanRtxdiPayload payload,
    BuiltInTriangleIntersectionAttributes attributes)
{
    payload.value = 1u;
}
