#include "../../../vulkan.hlsli"
#include "../PathTracePrimarySurface.hlsli"
#include "../PathTraceMaterialFeatureTypes.hlsli"
#include "../cleanroom_common/pathtrace_liquid_pool_control.hlsli"
#include "Rtxdi/DI/Reservoir.hlsli"
#include "Rtxdi/RtxdiParameters.h"
#include "Rtxdi/DI/ReSTIRDIParameters.h"
#include "Rtxdi/Utils/RandomSamplerState.hlsli"
#include "Rtxdi/Utils/Math.hlsli"

#ifndef RTXDI_ALLOWED_BIAS_CORRECTION
#define RTXDI_ALLOWED_BIAS_CORRECTION RTXDI_BIAS_CORRECTION_RAY_TRACED
#endif

struct PathTraceCleanRtxdiPayload
{
    uint value;
    uint rayMode;
    uint ignoreInstanceId;
    uint ignorePrimitiveIndex;
    uint ignoreMaterialIndex;
};

struct PathTraceRigidRouteInstance
{
    uint vertexOffset;
    uint indexOffset;
    uint triangleOffset;
    uint materialId;
    uint materialIndex;
    uint vertexCount;
    uint indexCount;
    uint triangleCount;
    uint flags;
    uint instanceIdLo;
    uint instanceIdHi;
    uint padding0;
    float4 currentObjectToWorld0;
    float4 currentObjectToWorld1;
    float4 currentObjectToWorld2;
    float4 previousObjectToWorld0;
    float4 previousObjectToWorld1;
    float4 previousObjectToWorld2;
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

struct PathTraceSmokeEmissiveTriangle
{
    float4 centerAndArea;
    float4 normalAndLuminance;
    float4 uvBounds;
    float4 centroidUvAndWeight;
    float4 estimatedRadianceAndLuminance;
    float4 sampleWeightAndPdf;
    uint materialIndex;
    uint instanceId;
    uint primitiveIndex;
    uint flags;
    uint emissiveTextureIndex;
    uint emissiveTextureWidth;
    uint emissiveTextureHeight;
    uint materialId;
    uint universeMaterialIndex;
    uint identityHashLo;
    uint identityHashHi;
    uint padding0;
};

#include "../pathtrace_material_classifier.hlsli"
#include "../RtxdiBridge/RAB_UnifiedLightRecord.hlsli"

VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> SmokeOutput : register(u1);
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> PathTraceRRGuideAlbedo : register(u48);
VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> PathTraceRRInputColor : register(u54);
RaytracingAccelerationStructure SmokeScene : register(t0);
StructuredBuffer<PathTraceSmokeVertex> SmokeStaticVertices : register(t3);
StructuredBuffer<uint> SmokeStaticIndices : register(t4);
StructuredBuffer<PathTraceSmokeVertex> SmokeDynamicVertices : register(t6);
StructuredBuffer<uint> SmokeDynamicIndices : register(t7);
StructuredBuffer<uint> SmokeStaticTriangleMaterialIndexes : register(t11);
StructuredBuffer<uint> SmokeDynamicTriangleMaterialIndexes : register(t12);
StructuredBuffer<PathTraceSmokeMaterial> SmokeMaterials : register(t13);
Texture2D<float4> SmokeFallbackTexture : register(t14);
StructuredBuffer<PathTraceDynamicMaterialRecord> SmokeDynamicMaterials : register(t15);
StructuredBuffer<PathTraceSmokeEmissiveTriangle> SmokeEmissiveTriangles : register(t16);
StructuredBuffer<PathTraceSmokeVertex> SmokeRigidRouteVertices : register(t22);
StructuredBuffer<uint> SmokeRigidRouteIndices : register(t23);
StructuredBuffer<uint> SmokeRigidRouteTriangleMaterialIndexes : register(t25);
StructuredBuffer<PathTraceRigidRouteInstance> SmokeRigidRouteInstances : register(t26);
RWStructuredBuffer<PathTracePrimarySurfaceRecord> PrimarySurfaceHistoryCurrent : register(u30);
RWStructuredBuffer<RTXDI_PackedDIReservoir> CleanRtxdiDiPreviousReservoirs : register(u71);
RWStructuredBuffer<RTXDI_PackedDIReservoir> CleanRtxdiDiSpatialReservoirs : register(u72);
StructuredBuffer<PathTraceUnifiedLightRecord> CleanRtxdiDiRluCurrentLights : register(t66);
StructuredBuffer<PathTraceMaterialFeatureRecord> PathTraceMaterialFeatures : register(t80);
StructuredBuffer<PathTraceMaterialFeatureParameterRecord> PathTraceMaterialFeatureParameters : register(t81);
RWStructuredBuffer<uint> PathTraceLiquidPoolStatusCounters : register(u94);
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_SIDECAR 1
VK_BINDING(0, 1) Texture2D<float4> SmokeDiffuseTextures[] : register(t0, space1);
SamplerState SmokeMaterialSampler : register(s0);

#define RTXDI_LIGHT_RESERVOIR_BUFFER CleanRtxdiDiPreviousReservoirs
#include "Rtxdi/DI/ReservoirStorage.hlsli"

cbuffer PathTraceCleanRtxdiDiSentinelConstants : register(b2)
{
    uint CleanRtxdiDiView;
    uint CleanRtxdiDiStatus;
    uint CleanRtxdiDiWidth;
    uint CleanRtxdiDiHeight;
    uint CleanRtxdiDiAnalyticLightCount;
    uint CleanRtxdiDiAnalyticIdentityCount;
    uint CleanRtxdiDiLightMode;
    uint CleanRtxdiDiFrameIndex;
    uint CleanRtxdiDiReservoirCount;
    uint CleanRtxdiDiCandidateCount;
    uint CleanRtxdiDiFlags;
    uint CleanRtxdiDiPreviousAnalyticLightCount;
    uint CleanRtxdiDiPreviousAnalyticIdentityCount;
    uint CleanRtxdiDiAnalyticRemapCount;
    uint CleanRtxdiDiTemporalFlags;
    uint CleanRtxdiDiHistoryResetCount;
    uint CleanRtxdiDiView8Band;
    uint CleanRtxdiDiResolveVisibilityReuse;
    uint CleanRtxdiDiResolveBrdfTarget;
    uint CleanRtxdiDiReferenceRab;
    uint CleanRtxdiDiRluCurrentLightCount;
    uint CleanRtxdiDiRluPreviousLightCount;
    uint CleanRtxdiDiRluCurrentToPreviousCount;
    uint CleanRtxdiDiRluPreviousToCurrentCount;
    uint CleanRtxdiDiTemporalAudit;
    uint CleanRtxdiDiStaticTriangleCount;
    uint CleanRtxdiDiDynamicTriangleCount;
    uint CleanRtxdiDiRigidRouteTriangleCount;
    uint CleanRtxdiDiCurrentEmissiveTriangleCount;
    uint CleanRtxdiDiPreviousEmissiveTriangleCount;
    uint CleanRtxdiDiRluDoomAnalyticRangeOffset;
    uint CleanRtxdiDiRluDoomAnalyticRangeCount;
    uint CleanRtxdiDiDoomAnalyticFullCurrentCount;
    uint CleanRtxdiDiDoomAnalyticFullPreviousCount;
    uint CleanRtxdiDiRluDomain;
    uint CleanRtxdiDiTemporalFireflyClamp;
    float4 CleanRtxdiDiTextureInfo;
    float4 CleanRtxdiDiPrevCameraOriginAndValid;
    float4 CleanRtxdiDiPrevCameraForwardAndTanX;
    float4 CleanRtxdiDiPrevCameraLeftAndTanY;
    float4 CleanRtxdiDiPrevCameraUpAndTanY;
    float4 CleanRtxdiDiCameraOriginAndValid;
    float4 CleanRtxdiDiCameraForwardAndTanX;
    float4 CleanRtxdiDiCameraLeftAndTanY;
    float4 CleanRtxdiDiCameraUpAndTanY;
    float4 CleanRtxdiDiDoomAnalyticLightInfo;
    float4 CleanRtxdiDiMotionVectorInfo;
    float4 CleanRtxdiDiRestirPTSurfaceInfo;
    float4 CleanRtxdiDiNeeCacheInfo0;
    float4 CleanRtxdiDiNeeCacheInfo1;
    float4 CleanRtxdiDiRluRangeInfo;
    float4 CleanRtxdiDiRluSampleInfo;
    float4 CleanRtxdiDiToyPathInfo;
    float4 CleanRtxdiDiGeometryInfo0;
    float4 CleanRtxdiDiGeometryInfo1;
    float4 CleanRtxdiDiSpatialInfo;
    float4 CleanRtxdiDiEmissiveDistributionInfo;
};

static const uint CLEAN_FLAG_BLUE_NOISE = 1u << 24u;
static const uint CLEAN_LIQUID_MODE_SHIFT = 2u;
static const uint CLEAN_LIQUID_DEBUG_SHIFT = 4u;
static const uint CLEAN_LIQUID_PAGE_SHIFT = 7u;
static const uint CLEAN_LIQUID_CONTROL_SHIFT = 9u;

uint PathTraceCleanRtxdiDiLiquidPoolMode()
{
    return (CleanRtxdiDiTemporalFlags >> CLEAN_LIQUID_MODE_SHIFT) & 3u;
}

uint PathTraceCleanRtxdiDiLiquidPoolDebug()
{
    return (CleanRtxdiDiTemporalFlags >> CLEAN_LIQUID_DEBUG_SHIFT) & 7u;
}

uint PathTraceCleanRtxdiDiLiquidPoolPage()
{
    return (CleanRtxdiDiTemporalFlags >> CLEAN_LIQUID_PAGE_SHIFT) & 3u;
}

uint PathTraceCleanRtxdiDiLiquidPoolControlFlags()
{
    return (CleanRtxdiDiTemporalFlags >> CLEAN_LIQUID_CONTROL_SHIFT) & 15u;
}

bool PathTraceCleanRtxdiDiWriteLiquidPoolRouteDiagnostic(uint2 pixel)
{
    const uint debug = PathTraceCleanRtxdiDiLiquidPoolDebug();
    if (debug != 6u)
    {
        return false;
    }
    const uint page = PathTraceCleanRtxdiDiLiquidPoolPage();
    const uint status = PathTraceLiquidPoolControlInitialStatus(
        PathTraceCleanRtxdiDiLiquidPoolControlFlags(), debug, page);
    const uint source = (status & RT_LIQUID_POOL_STATUS_INVALID_ROUTE) != 0u
        ? RT_LIQUID_POOL_SOURCE_INVALID
        : RT_LIQUID_POOL_SOURCE_CLEAN_DI_REFLECTION;
    SmokeOutput[pixel] = PathTraceLiquidPoolRouteDiagnostic(source, status);
    const uint exceptional = status &
        (RT_LIQUID_POOL_STATUS_OVERFLOW |
            RT_LIQUID_POOL_STATUS_DUPLICATE_APPLY |
            RT_LIQUID_POOL_STATUS_FAIL_CLOSED |
            RT_LIQUID_POOL_STATUS_INVALID_ROUTE);
    if (all(pixel == uint2(0u, 0u)) && exceptional != 0u &&
        (PathTraceCleanRtxdiDiLiquidPoolControlFlags() & RT_LIQUID_POOL_CONTROL_TELEMETRY_READY) != 0u)
    {
        uint ignored;
        InterlockedOr(
            PathTraceLiquidPoolStatusCounters[RT_LIQUID_POOL_SOURCE_CLEAN_DI_REFLECTION],
            exceptional,
            ignored);
    }
    return true;
}

void PathTraceCleanRtxdiDiApplyBlueNoiseToggle(inout RTXDI_RandomSamplerState rng)
{
#ifdef RBPT_ENABLE_BLUE_NOISE
    rng.useBlueNoise = ((CleanRtxdiDiFlags & CLEAN_FLAG_BLUE_NOISE) != 0u) ? rng.useBlueNoise : 0u;
#else
    rng.useBlueNoise = 0u;
#endif
}

static const uint RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF = 0x00040000u;
static const uint RT_SMOKE_MATERIAL_ADDITIVE_DECAL = 0x00000004u;
static const uint RT_SMOKE_MATERIAL_DIFFUSE_YCOCG = 0x00000002u;
static const uint RT_SMOKE_MATERIAL_EMISSIVE = 0x00000008u;
static const uint RT_SMOKE_MATERIAL_FILTER_DECAL = 0x00000010u;
static const uint RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_LUMA = 0x00000040u;
static const uint RT_SMOKE_MATERIAL_FORCE_DEBUG_ALBEDO = 0x00000080u;
static const uint RT_SMOKE_MATERIAL_PORTAL_WINDOW_FALLBACK = 0x00000200u;
static const uint RT_SMOKE_MATERIAL_OBJECT_GLASS_FALLBACK = 0x00000400u;
static const uint RT_SMOKE_MATERIAL_ADDITIVE_DECAL_WHITE_KEY = 0x00000800u;
static const uint RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_MAGENTA_KEY = 0x00001000u;
static const uint RT_SMOKE_DYNAMIC_MATERIAL_RECORD_VALID = 0x00000001u;
static const uint RT_SMOKE_DYNAMIC_MATERIAL_RECORD_STAGE_ENABLED = 0x00000002u;
static const uint RT_SMOKE_DYNAMIC_MATERIAL_RECORD_SELECTED_EMISSIVE = 0x00000004u;
static const uint RT_SMOKE_DYNAMIC_MATERIAL_RECORD_REPLACE_EMISSIVE = 0x00000200u;
static const uint RT_SMOKE_MATERIAL_DYNAMIC_EMISSIVE_REGISTER_MASK = 0x0000003cu;
static const uint RT_SMOKE_TEXTURE_FLAG_USE_EMISSIVE_MAPS = 0x00000020u;
static const uint RT_SMOKE_TEXTURE_FLAG_RESERVOIR_TWO_SIDED_EMISSIVES = 0x00000040u;
static const float CLEAN_RTXDI_PI = 3.14159265358979323846;
#define DoomAnalyticLightInfo CleanRtxdiDiDoomAnalyticLightInfo
#define MotionVectorInfo CleanRtxdiDiMotionVectorInfo
#define RestirPTSurfaceInfo CleanRtxdiDiRestirPTSurfaceInfo
#define TextureInfo CleanRtxdiDiTextureInfo
#define ToyPathInfo CleanRtxdiDiToyPathInfo
bool RAB_RestirLightManagerRABEnabled()
{
    return false;
}
bool RAB_UnifiedLightSampleEnabled()
{
    return false;
}
static const uint CLEAN_RAB_DIAGNOSTIC_RELAX_BRDF_GATES = 1u << 8u;
static const uint CLEAN_RAB_DIAGNOSTIC_DOOM_TARGET_FLOOR = 1u << 9u;
static const uint CLEAN_RAB_DIAGNOSTIC_DUMMY_EMISSIVE_NORMALS = 1u << 13u;
static const uint CLEAN_FLAG_RESOLVE_SOLID_ANGLE_PDF = 1u << 18u;
static const uint CLEAN_FLAG_LIQUID_MODIFIER_VISIBILITY = 1u << 21u;
static const uint CLEAN_SURFACE_FLAG_TRANSMISSION_PSR_RESOLVED = 0x80000000u;
static const uint CLEAN_SURFACE_FLAG_TRANSMISSION_PSR_REFRACTED = 0x40000000u;
static const uint CLEAN_SURFACE_FLAG_REFLECTION_PSR_RESOLVED = 0x20000000u;
#define RB_RAB_LIGHT_SAMPLING_CORE_ONLY 1
#define RB_RAB_CLEAN_RTXDI_DI_SENTINEL 1
#define RB_RAB_CLEAN_DIAGNOSTIC_RELAX_BRDF_GATES 1
// The clean RTXDI target function uses Remix-style material floors before reservoir weighting.
#include "../RtxdiBridge/RAB_LightTarget.hlsli"
#include "pathtrace_clean_rtxdi_di_material_feature_queries.hlsli"

uint CleanRtxdiDiDynamicMaterialRecordCount()
{
    return (uint)max(CleanRtxdiDiEmissiveDistributionInfo.w, 0.0);
}

void CleanRtxdiDiApplyDynamicMaterialRecord(uint materialIndex, inout PathTraceSmokeMaterial material)
{
    const uint recordCount = CleanRtxdiDiDynamicMaterialRecordCount();
    if (materialIndex >= recordCount)
    {
        return;
    }

    const PathTraceDynamicMaterialRecord record = SmokeDynamicMaterials[materialIndex];
    if ((record.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_VALID) == 0u ||
        record.materialIndex != materialIndex ||
        (record.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_SELECTED_EMISSIVE) == 0u)
    {
        return;
    }
    if ((material.flags & RT_SMOKE_MATERIAL_EMISSIVE) == 0u ||
        (material.padding0 & RT_SMOKE_MATERIAL_DYNAMIC_EMISSIVE_REGISTER_MASK) == 0u)
    {
        return;
    }

    const bool stageEnabled =
        (record.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_STAGE_ENABLED) != 0u &&
        record.texMatrix0.w != 0.0 &&
        max(max(record.color.r, record.color.g), record.color.b) > 1.0e-5;
    if (!stageEnabled)
    {
        material.emissiveColor = float4(0.0, 0.0, 0.0, 1.0);
        material.flags &= ~RT_SMOKE_MATERIAL_EMISSIVE;
        return;
    }

    const float stageAlpha = saturate(record.color.a);
    const float3 stageScale = max(record.color.rgb, float3(0.0, 0.0, 0.0)) * stageAlpha;
    if ((record.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_REPLACE_EMISSIVE) != 0u)
    {
        material.emissiveColor.rgb = stageScale;
    }
    else
    {
        material.emissiveColor.rgb *= stageScale;
    }
    material.emissiveColor.a = stageAlpha;
    if (max(max(material.emissiveColor.r, material.emissiveColor.g), material.emissiveColor.b) <= 1.0e-5)
    {
        material.flags &= ~RT_SMOKE_MATERIAL_EMISSIVE;
    }
    else
    {
        material.flags |= RT_SMOKE_MATERIAL_EMISSIVE;
    }
}

static const float2 CLEAN_RTXDI_DI_SPATIAL_NEIGHBOR_OFFSETS[32] =
{
    float2( 0.096,  0.071), float2(-0.154,  0.141), float2( 0.032, -0.287), float2( 0.247,  0.246),
    float2(-0.365, -0.065), float2( 0.362, -0.248), float2(-0.135,  0.492), float2(-0.282, -0.451),
    float2( 0.553,  0.087), float2(-0.502,  0.321), float2( 0.160, -0.615), float2( 0.364,  0.563),
    float2(-0.681, -0.157), float2( 0.648, -0.331), float2(-0.245,  0.718), float2(-0.390, -0.686),
    float2( 0.793,  0.201), float2(-0.762,  0.356), float2( 0.313, -0.803), float2( 0.407,  0.779),
    float2(-0.869, -0.272), float2( 0.857, -0.347), float2(-0.392,  0.849), float2(-0.393, -0.864),
    float2( 0.908,  0.358), float2(-0.929,  0.306), float2( 0.488, -0.858), float2( 0.341,  0.930),
    float2(-0.909, -0.442), float2( 0.981, -0.226), float2(-0.589,  0.829), float2(-0.242, -0.971)
};

float3 CleanSafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-8 ? value * rsqrt(lengthSquared) : fallback;
}

float CleanLuminance(float3 value)
{
    return dot(max(value, float3(0.0, 0.0, 0.0)), float3(0.2126, 0.7152, 0.0722));
}

uint CleanReservoirBlockCount(uint dimension)
{
    return (dimension + RTXDI_RESERVOIR_BLOCK_SIZE - 1u) / RTXDI_RESERVOIR_BLOCK_SIZE;
}

RTXDI_ReservoirBufferParameters CleanReservoirParams(uint2 dimensions)
{
    RTXDI_ReservoirBufferParameters params = (RTXDI_ReservoirBufferParameters)0;
    const uint blockCountX = max(CleanReservoirBlockCount(dimensions.x), 1u);
    const uint blockCountY = max(CleanReservoirBlockCount(dimensions.y), 1u);
    params.reservoirBlockRowPitch = blockCountX * RTXDI_RESERVOIR_BLOCK_SIZE * RTXDI_RESERVOIR_BLOCK_SIZE;
    params.reservoirArrayPitch = params.reservoirBlockRowPitch * blockCountY;
    return params;
}

uint CleanReservoirIndex(uint2 pixel, uint2 dimensions)
{
    return RTXDI_ReservoirPositionToPointer(CleanReservoirParams(dimensions), pixel, 0u);
}

bool CleanLoadSurface(uint2 pixel, uint2 dimensions, out PathTracePrimarySurfaceRecord record)
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

float3 CleanSurfaceNormal(PathTracePrimarySurfaceRecord record)
{
    return CleanSafeNormalize(record.shadingNormalAndOpacity.xyz, record.geometricNormalAndRoughness.xyz);
}

float3 CleanSurfaceViewDir(PathTracePrimarySurfaceRecord record)
{
    return CleanSafeNormalize(record.viewDirectionAndReserved.xyz, -CleanSurfaceNormal(record));
}

float3 CleanTransformRigidRoutePoint(PathTraceRigidRouteInstance routeInstance, float3 localPoint)
{
    return float3(
        dot(routeInstance.currentObjectToWorld0, float4(localPoint, 1.0)),
        dot(routeInstance.currentObjectToWorld1, float4(localPoint, 1.0)),
        dot(routeInstance.currentObjectToWorld2, float4(localPoint, 1.0)));
}

bool CleanLoadEmissiveTriangleGeometry(PathTraceSmokeEmissiveTriangle tri, out float3 p0, out float3 p1, out float3 p2, out float2 uv0, out float2 uv1, out float2 uv2)
{
    p0 = tri.centerAndArea.xyz;
    p1 = tri.centerAndArea.xyz;
    p2 = tri.centerAndArea.xyz;
    uv0 = tri.centroidUvAndWeight.xy;
    uv1 = tri.centroidUvAndWeight.xy;
    uv2 = tri.centroidUvAndWeight.xy;

    const uint primitiveIndex = tri.primitiveIndex;
    if (tri.instanceId == 0u)
    {
        const uint vertexCount = (uint)max(CleanRtxdiDiGeometryInfo0.x, 0.0);
        const uint indexCount = (uint)max(CleanRtxdiDiGeometryInfo0.y, 0.0);
        const uint triangleCount = (uint)max(CleanRtxdiDiGeometryInfo0.z, 0.0);
        const uint indexOffset = primitiveIndex * 3u;
        if (primitiveIndex >= triangleCount || indexOffset + 2u >= indexCount)
        {
            return false;
        }
        const uint i0 = SmokeStaticIndices[indexOffset + 0u];
        const uint i1 = SmokeStaticIndices[indexOffset + 1u];
        const uint i2 = SmokeStaticIndices[indexOffset + 2u];
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
        {
            return false;
        }
        const PathTraceSmokeVertex v0 = SmokeStaticVertices[i0];
        const PathTraceSmokeVertex v1 = SmokeStaticVertices[i1];
        const PathTraceSmokeVertex v2 = SmokeStaticVertices[i2];
        p0 = v0.position.xyz;
        p1 = v1.position.xyz;
        p2 = v2.position.xyz;
        uv0 = v0.texCoord.xy;
        uv1 = v1.texCoord.xy;
        uv2 = v2.texCoord.xy;
        return true;
    }

    if (tri.instanceId == 1u)
    {
        const uint vertexCount = (uint)max(CleanRtxdiDiGeometryInfo0.w, 0.0);
        const uint indexCount = (uint)max(CleanRtxdiDiGeometryInfo1.x, 0.0);
        const uint triangleCount = (uint)max(CleanRtxdiDiGeometryInfo1.y, 0.0);
        const uint indexOffset = primitiveIndex * 3u;
        if (primitiveIndex >= triangleCount || indexOffset + 2u >= indexCount)
        {
            return false;
        }
        const uint i0 = SmokeDynamicIndices[indexOffset + 0u];
        const uint i1 = SmokeDynamicIndices[indexOffset + 1u];
        const uint i2 = SmokeDynamicIndices[indexOffset + 2u];
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
        {
            return false;
        }
        const PathTraceSmokeVertex v0 = SmokeDynamicVertices[i0];
        const PathTraceSmokeVertex v1 = SmokeDynamicVertices[i1];
        const PathTraceSmokeVertex v2 = SmokeDynamicVertices[i2];
        p0 = v0.position.xyz;
        p1 = v1.position.xyz;
        p2 = v2.position.xyz;
        uv0 = v0.texCoord.xy;
        uv1 = v1.texCoord.xy;
        uv2 = v2.texCoord.xy;
        return true;
    }

    const uint routeInstanceIndex = tri.instanceId - 2u;
    const uint routeInstanceCount = (uint)max(CleanRtxdiDiToyPathInfo.w, 0.0);
    if (routeInstanceIndex >= routeInstanceCount)
    {
        return false;
    }

    const PathTraceRigidRouteInstance route = SmokeRigidRouteInstances[routeInstanceIndex];
    const uint vertexCount = (uint)max(CleanRtxdiDiGeometryInfo1.z, 0.0);
    const uint indexCount = (uint)max(CleanRtxdiDiGeometryInfo1.w, 0.0);
    const uint indexOffset = route.indexOffset + primitiveIndex * 3u;
    if (primitiveIndex >= route.triangleCount || indexOffset + 2u >= indexCount)
    {
        return false;
    }
    const uint i0 = SmokeRigidRouteIndices[indexOffset + 0u];
    const uint i1 = SmokeRigidRouteIndices[indexOffset + 1u];
    const uint i2 = SmokeRigidRouteIndices[indexOffset + 2u];
    if (i0 >= route.vertexCount || i1 >= route.vertexCount || i2 >= route.vertexCount ||
        route.vertexOffset + i0 >= vertexCount || route.vertexOffset + i1 >= vertexCount || route.vertexOffset + i2 >= vertexCount)
    {
        return false;
    }
    const PathTraceSmokeVertex v0 = SmokeRigidRouteVertices[route.vertexOffset + i0];
    const PathTraceSmokeVertex v1 = SmokeRigidRouteVertices[route.vertexOffset + i1];
    const PathTraceSmokeVertex v2 = SmokeRigidRouteVertices[route.vertexOffset + i2];
    p0 = CleanTransformRigidRoutePoint(route, v0.position.xyz);
    p1 = CleanTransformRigidRoutePoint(route, v1.position.xyz);
    p2 = CleanTransformRigidRoutePoint(route, v2.position.xyz);
    uv0 = v0.texCoord.xy;
    uv1 = v1.texCoord.xy;
    uv2 = v2.texCoord.xy;
    return true;
}

uint CleanLoadTriangleMaterialIndex(uint instanceId, uint primitiveIndex)
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
    const uint routeInstanceCount = (uint)max(CleanRtxdiDiToyPathInfo.w, 0.0);
    if (routeInstanceIndex >= routeInstanceCount)
    {
        return 0xffffffffu;
    }

    const PathTraceRigidRouteInstance route = SmokeRigidRouteInstances[routeInstanceIndex];
    const uint routedPrimitiveIndex = route.triangleOffset + primitiveIndex;
    if (primitiveIndex >= route.triangleCount ||
        routedPrimitiveIndex >= CleanRtxdiDiRigidRouteTriangleCount)
    {
        return 0xffffffffu;
    }

    return SmokeRigidRouteTriangleMaterialIndexes[routedPrimitiveIndex];
}

uint CleanResolveLiveMaterialIndex(PathTracePrimarySurfaceRecord record)
{
    const uint recordMaterialIndex = record.materialAndSurface.y;
    const uint liveMaterialIndex = CleanLoadTriangleMaterialIndex(
        record.instancePrimitiveObject.x,
        record.instancePrimitiveObject.y);
    return liveMaterialIndex < (uint)CleanRtxdiDiTextureInfo.z ? liveMaterialIndex : recordMaterialIndex;
}

bool CleanLoadSurfaceTriangleGeometry(PathTracePrimarySurfaceRecord record, out float3 p0, out float3 p1, out float3 p2, out float2 uv0, out float2 uv1, out float2 uv2)
{
    p0 = record.worldPositionAndViewDepth.xyz;
    p1 = record.worldPositionAndViewDepth.xyz;
    p2 = record.worldPositionAndViewDepth.xyz;
    uv0 = float2(0.0, 0.0);
    uv1 = float2(0.0, 0.0);
    uv2 = float2(0.0, 0.0);

    const uint instanceId = record.instancePrimitiveObject.x;
    const uint primitiveIndex = record.instancePrimitiveObject.y;
    if (instanceId == 0u)
    {
        const uint vertexCount = (uint)max(CleanRtxdiDiGeometryInfo0.x, 0.0);
        const uint indexCount = (uint)max(CleanRtxdiDiGeometryInfo0.y, 0.0);
        const uint triangleCount = (uint)max(CleanRtxdiDiGeometryInfo0.z, 0.0);
        const uint indexOffset = primitiveIndex * 3u;
        if (primitiveIndex >= triangleCount || indexOffset + 2u >= indexCount)
        {
            return false;
        }
        const uint i0 = SmokeStaticIndices[indexOffset + 0u];
        const uint i1 = SmokeStaticIndices[indexOffset + 1u];
        const uint i2 = SmokeStaticIndices[indexOffset + 2u];
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
        {
            return false;
        }
        const PathTraceSmokeVertex v0 = SmokeStaticVertices[i0];
        const PathTraceSmokeVertex v1 = SmokeStaticVertices[i1];
        const PathTraceSmokeVertex v2 = SmokeStaticVertices[i2];
        p0 = v0.position.xyz;
        p1 = v1.position.xyz;
        p2 = v2.position.xyz;
        uv0 = v0.texCoord.xy;
        uv1 = v1.texCoord.xy;
        uv2 = v2.texCoord.xy;
        return true;
    }

    if (instanceId == 1u)
    {
        const uint vertexCount = (uint)max(CleanRtxdiDiGeometryInfo0.w, 0.0);
        const uint indexCount = (uint)max(CleanRtxdiDiGeometryInfo1.x, 0.0);
        const uint triangleCount = (uint)max(CleanRtxdiDiGeometryInfo1.y, 0.0);
        const uint indexOffset = primitiveIndex * 3u;
        if (primitiveIndex >= triangleCount || indexOffset + 2u >= indexCount)
        {
            return false;
        }
        const uint i0 = SmokeDynamicIndices[indexOffset + 0u];
        const uint i1 = SmokeDynamicIndices[indexOffset + 1u];
        const uint i2 = SmokeDynamicIndices[indexOffset + 2u];
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
        {
            return false;
        }
        const PathTraceSmokeVertex v0 = SmokeDynamicVertices[i0];
        const PathTraceSmokeVertex v1 = SmokeDynamicVertices[i1];
        const PathTraceSmokeVertex v2 = SmokeDynamicVertices[i2];
        p0 = v0.position.xyz;
        p1 = v1.position.xyz;
        p2 = v2.position.xyz;
        uv0 = v0.texCoord.xy;
        uv1 = v1.texCoord.xy;
        uv2 = v2.texCoord.xy;
        return true;
    }

    const uint routeInstanceIndex = instanceId - 2u;
    const uint routeInstanceCount = (uint)max(CleanRtxdiDiToyPathInfo.w, 0.0);
    if (routeInstanceIndex >= routeInstanceCount)
    {
        return false;
    }

    const PathTraceRigidRouteInstance route = SmokeRigidRouteInstances[routeInstanceIndex];
    const uint vertexCount = (uint)max(CleanRtxdiDiGeometryInfo1.z, 0.0);
    const uint indexCount = (uint)max(CleanRtxdiDiGeometryInfo1.w, 0.0);
    const uint indexOffset = route.indexOffset + primitiveIndex * 3u;
    if (primitiveIndex >= route.triangleCount || indexOffset + 2u >= indexCount)
    {
        return false;
    }
    const uint i0 = SmokeRigidRouteIndices[indexOffset + 0u];
    const uint i1 = SmokeRigidRouteIndices[indexOffset + 1u];
    const uint i2 = SmokeRigidRouteIndices[indexOffset + 2u];
    if (i0 >= route.vertexCount || i1 >= route.vertexCount || i2 >= route.vertexCount ||
        route.vertexOffset + i0 >= vertexCount || route.vertexOffset + i1 >= vertexCount || route.vertexOffset + i2 >= vertexCount)
    {
        return false;
    }
    const PathTraceSmokeVertex v0 = SmokeRigidRouteVertices[route.vertexOffset + i0];
    const PathTraceSmokeVertex v1 = SmokeRigidRouteVertices[route.vertexOffset + i1];
    const PathTraceSmokeVertex v2 = SmokeRigidRouteVertices[route.vertexOffset + i2];
    p0 = CleanTransformRigidRoutePoint(route, v0.position.xyz);
    p1 = CleanTransformRigidRoutePoint(route, v1.position.xyz);
    p2 = CleanTransformRigidRoutePoint(route, v2.position.xyz);
    uv0 = v0.texCoord.xy;
    uv1 = v1.texCoord.xy;
    uv2 = v2.texCoord.xy;
    return true;
}

float3 CleanSurfaceFallbackAlbedo(PathTracePrimarySurfaceRecord record)
{
    float3 albedo = saturate(record.albedoAndAlphaCutoff.xyz);
    const bool liquidFilmApplied =
        (record.header.w & RT_PATH_TRACE_SURFACE_FLAG_LIQUID_FILM_APPLIED) != 0u;
    const bool invalidDebugAlbedo = all(abs(albedo - float3(1.0, 0.0, 1.0)) < float3(0.001, 0.001, 0.001));
    if ((!liquidFilmApplied && CleanLuminance(albedo) <= 1.0e-5) || invalidDebugAlbedo)
    {
        albedo = float3(0.5, 0.5, 0.5);
    }
    return albedo;
}

float3 CleanBarycentric(float3 position, float3 p0, float3 p1, float3 p2)
{
    const float3 v0 = p1 - p0;
    const float3 v1 = p2 - p0;
    const float3 v2 = position - p0;
    const float d00 = dot(v0, v0);
    const float d01 = dot(v0, v1);
    const float d11 = dot(v1, v1);
    const float d20 = dot(v2, v0);
    const float d21 = dot(v2, v1);
    const float denom = d00 * d11 - d01 * d01;
    if (abs(denom) <= 1.0e-8)
    {
        return float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0);
    }

    const float invDenom = 1.0 / denom;
    const float baryY = (d11 * d20 - d01 * d21) * invDenom;
    const float baryZ = (d00 * d21 - d01 * d20) * invDenom;
    const float baryX = 1.0 - baryY - baryZ;
    const float3 bary = saturate(float3(baryX, baryY, baryZ));
    const float sumBary = max(dot(bary, float3(1.0, 1.0, 1.0)), 1.0e-6);
    return bary / sumBary;
}

PathTraceSmokeMaterial CleanLoadSmokeMaterial(uint materialIndex);
float4 CleanSampleDecodedDiffuse(PathTraceSmokeMaterial material, float2 texCoord, float4 fallback);

bool CleanLiveMaterialClassifierBsdfActive(uint materialIndex)
{
    const PathTraceSmokeMaterial material = CleanLoadSmokeMaterial(materialIndex);
    return SmokeMatClassRoute(material) == RT_MATCLASS_ROUTE_REAL_PBR_RMAO ||
        SmokeMatClassDrivesLegacySpec(material) ||
        SmokeMatClassHasPackedBsdf(material);
}

void CleanApplyLiveMaterialClassifierBsdf(
    uint materialIndex,
    inout float3 albedo,
    inout float3 specularF0,
    inout float roughness)
{
    const PathTraceSmokeMaterial material = CleanLoadSmokeMaterial(materialIndex);
    SmokeApplyMaterialClassifierBsdf(material, albedo, specularF0, roughness);
}

float3 CleanTexturedSurfaceAlbedo(PathTracePrimarySurfaceRecord record)
{
    const float3 fallbackAlbedo = CleanSurfaceFallbackAlbedo(record);
    const bool liquidFilmApplied =
        (record.header.w & RT_PATH_TRACE_SURFACE_FLAG_LIQUID_FILM_APPLIED) != 0u;
    if ((record.header.w &
            (CLEAN_SURFACE_FLAG_TRANSMISSION_PSR_RESOLVED |
                CLEAN_SURFACE_FLAG_REFLECTION_PSR_RESOLVED)) != 0u)
    {
        return fallbackAlbedo;
    }

    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dimensions = DispatchRaysDimensions().xy;
    if (pixel.x < dimensions.x && pixel.y < dimensions.y)
    {
        const float3 guideAlbedo = saturate(PathTraceRRGuideAlbedo[pixel].rgb);
        const bool invalidGuideAlbedo = all(abs(guideAlbedo - float3(1.0, 0.0, 1.0)) < float3(0.001, 0.001, 0.001));
        if ((liquidFilmApplied || CleanLuminance(guideAlbedo) > 1.0e-5) && !invalidGuideAlbedo)
        {
            return guideAlbedo;
        }
    }

    return fallbackAlbedo;
}

bool CleanTriangleNormalArea(float3 p0, float3 p1, float3 p2, float3 payloadNormal, out float3 normal, out float area)
{
    const float3 crossValue = cross(p1 - p0, p2 - p0);
    const float doubleAreaSquared = dot(crossValue, crossValue);
    normal = CleanSafeNormalize(payloadNormal, float3(0.0, 0.0, 1.0));
    area = 0.0;
    if (doubleAreaSquared <= 1.0e-12)
    {
        return false;
    }
    const float doubleArea = sqrt(doubleAreaSquared);
    normal = crossValue / doubleArea;
    const float3 safePayloadNormal = CleanSafeNormalize(payloadNormal, normal);
    if (dot(normal, safePayloadNormal) < 0.0)
    {
        normal = -normal;
    }
    area = 0.5 * doubleArea;
    return area > 1.0e-6;
}

RAB_Surface CleanSurfaceFromRecord(PathTracePrimarySurfaceRecord record)
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
    surface.geometryNormal = CleanSafeNormalize(record.geometricNormalAndRoughness.xyz, float3(0.0, 0.0, 1.0));
    surface.shadingNormal = CleanSafeNormalize(record.shadingNormalAndOpacity.xyz, surface.geometryNormal);
    surface.viewDir = CleanSafeNormalize(record.viewDirectionAndReserved.xyz, -surface.shadingNormal);
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

RAB_Surface CleanMaterialSurfaceFromRecord(PathTracePrimarySurfaceRecord record)
{
    RAB_Surface surface = CleanSurfaceFromRecord(record);
    if (!RAB_IsSurfaceValid(surface))
    {
        return surface;
    }

    surface.flags = record.header.w;
    surface.instanceId = record.instancePrimitiveObject.x;
    surface.primitiveIndex = record.instancePrimitiveObject.y;

    const uint resolvedMaterialIndex = CleanResolveLiveMaterialIndex(record);

    RAB_Material material = RAB_EmptyMaterial();
    material.materialId = surface.materialId;
    material.materialIndex = resolvedMaterialIndex;
    material.flags = record.materialAndSurface.z;
    material.alphaCutoff = record.albedoAndAlphaCutoff.w;
    material.diffuseAlbedo = saturate(record.albedoAndAlphaCutoff.xyz);
    material.roughness = saturate(record.geometricNormalAndRoughness.w);
    material.specularF0 = max(record.specularF0AndReserved.xyz, float3(0.0, 0.0, 0.0));
    if ((record.header.w &
            (CLEAN_SURFACE_FLAG_TRANSMISSION_PSR_RESOLVED |
                CLEAN_SURFACE_FLAG_REFLECTION_PSR_RESOLVED)) == 0u)
    {
        CleanApplyLiveMaterialClassifierBsdf(
            material.materialIndex,
            material.diffuseAlbedo,
            material.specularF0,
            material.roughness);
    }
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

RAB_Surface CleanSurfaceForView(PathTracePrimarySurfaceRecord record)
{
    if (CleanRtxdiDiView == 16u ||
        CleanLiveMaterialClassifierBsdfActive(CleanResolveLiveMaterialIndex(record)))
    {
        return CleanMaterialSurfaceFromRecord(record);
    }
    return CleanSurfaceFromRecord(record);
}

PathTraceSmokeMaterial CleanLoadSmokeMaterial(uint materialIndex)
{
    PathTraceSmokeMaterial material = (PathTraceSmokeMaterial)0;
    material.debugAlbedo = float4(1.0, 0.0, 1.0, 1.0);
    material.diffuseTextureIndex = 0xffffffffu;
    material.alphaTextureIndex = 0xffffffffu;
    material.normalTextureIndex = 0xffffffffu;
    material.specularTextureIndex = 0xffffffffu;
    material.emissiveTextureIndex = 0xffffffffu;
    material.emissiveColor = float4(0.0, 0.0, 0.0, 1.0);
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
    if (materialIndex < (uint)CleanRtxdiDiTextureInfo.z)
    {
        material = SmokeMaterials[materialIndex];
        CleanRtxdiDiApplyDynamicMaterialRecord(materialIndex, material);
    }
    return material;
}

bool CleanMaterialDoesNotOccludeVisibility(uint instanceId, uint materialIndex)
{
    if (materialIndex >= (uint)CleanRtxdiDiTextureInfo.z)
    {
        return false;
    }

    PathTraceMaterialFeature feature;
    if (instanceId <= 1u &&
        (CleanRtxdiDiFlags & CLEAN_FLAG_LIQUID_MODIFIER_VISIBILITY) != 0u &&
        PathTraceCleanRtxdiDiLoadMaterialFeature(materialIndex, feature) &&
        feature.materialKind == RT_PATH_TRACE_MATERIAL_KIND_LIQUID_POOL_MODIFIER &&
        feature.modifierKind == RT_PATH_TRACE_MATERIAL_MODIFIER_LIQUID_POOL_UNION &&
        (feature.materialCaps & (RT_PATH_TRACE_MATERIAL_CAP_RECEIVER_MODIFIER |
            RT_PATH_TRACE_MATERIAL_CAP_IDEMPOTENT_MODIFIER_BLEND)) ==
            (RT_PATH_TRACE_MATERIAL_CAP_RECEIVER_MODIFIER |
                RT_PATH_TRACE_MATERIAL_CAP_IDEMPOTENT_MODIFIER_BLEND))
    {
        return true;
    }

    const PathTraceSmokeMaterial material = CleanLoadSmokeMaterial(materialIndex);
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

float4 CleanLoadTextureTexel(uint textureIndex, uint2 texel, bool bindlessEnabled)
{
    return bindlessEnabled
        ? SmokeDiffuseTextures[NonUniformResourceIndex(textureIndex)].Load(int3(texel, 0))
        : SmokeFallbackTexture.Load(int3(0, 0, 0));
}

float4 CleanSampleTextureLoad(uint textureIndex, uint textureWidth, uint textureHeight, float2 wrappedTexCoord, bool bindlessEnabled, bool bilinearFilter)
{
    const uint width = max(textureWidth, 1u);
    const uint height = max(textureHeight, 1u);
    if (!bilinearFilter || width == 1u || height == 1u)
    {
        const uint2 texel = min(uint2(wrappedTexCoord * float2(width, height)), uint2(width - 1u, height - 1u));
        return CleanLoadTextureTexel(textureIndex, texel, bindlessEnabled);
    }

    const float2 texelPosition = wrappedTexCoord * float2(width, height) - 0.5;
    const int2 baseTexel = int2(floor(texelPosition));
    const float2 fraction = frac(texelPosition);
    const uint2 textureSize = uint2(width, height);
    const uint2 texel00 = uint2((baseTexel + int2(0, 0) + int2(width, height)) % int2(textureSize));
    const uint2 texel10 = uint2((baseTexel + int2(1, 0) + int2(width, height)) % int2(textureSize));
    const uint2 texel01 = uint2((baseTexel + int2(0, 1) + int2(width, height)) % int2(textureSize));
    const uint2 texel11 = uint2((baseTexel + int2(1, 1) + int2(width, height)) % int2(textureSize));
    const float4 c00 = CleanLoadTextureTexel(textureIndex, texel00, bindlessEnabled);
    const float4 c10 = CleanLoadTextureTexel(textureIndex, texel10, bindlessEnabled);
    const float4 c01 = CleanLoadTextureTexel(textureIndex, texel01, bindlessEnabled);
    const float4 c11 = CleanLoadTextureTexel(textureIndex, texel11, bindlessEnabled);
    return lerp(lerp(c00, c10, fraction.x), lerp(c01, c11, fraction.x), fraction.y);
}

float4 CleanSampleTexture(uint textureIndex, uint textureWidth, uint textureHeight, float2 texCoord, float4 fallback)
{
    const uint textureCount = (uint)TextureInfo.x;
    const uint sampleMethod = (uint)TextureInfo.y;
    const uint textureFlags = (uint)TextureInfo.w;
    const bool bindlessEnabled = (textureFlags & 1u) != 0u;
    const bool bilinearFilter = (textureFlags & 2u) != 0u;
    if (sampleMethod == 0u || textureIndex == 0xffffffffu || textureIndex >= textureCount)
    {
        return fallback;
    }
    if (!all(texCoord == texCoord) || any(abs(texCoord) > 65536.0))
    {
        return fallback;
    }

    const float2 wrappedTexCoord = frac(texCoord);
    const float4 sampled = sampleMethod == 2u
        ? CleanSampleTextureLoad(textureIndex, textureWidth, textureHeight, wrappedTexCoord, bindlessEnabled, bilinearFilter)
        : (bindlessEnabled
            ? SmokeDiffuseTextures[NonUniformResourceIndex(textureIndex)].SampleLevel(SmokeMaterialSampler, wrappedTexCoord, 0.0)
            : SmokeFallbackTexture.SampleLevel(SmokeMaterialSampler, wrappedTexCoord, 0.0));
    if (!all(sampled == sampled) || any(abs(sampled) > 65504.0))
    {
        return fallback;
    }
    return sampled;
}

float3 CleanConvertYCoCgToRGB(float4 ycocg)
{
    ycocg.z = (ycocg.z * 31.875) + 1.0;
    ycocg.z = 1.0 / ycocg.z;
    ycocg.xy *= ycocg.z;
    return saturate(float3(
        dot(ycocg, float4(1.0, -1.0, 0.0, 1.0)),
        dot(ycocg, float4(0.0, 1.0, -0.50196078, 1.0)),
        dot(ycocg, float4(-1.0, -1.0, 1.00392156, 1.0))));
}

float4 CleanSampleDecodedDiffuse(PathTraceSmokeMaterial material, float2 texCoord, float4 fallback)
{
    if ((material.flags & RT_SMOKE_MATERIAL_FORCE_DEBUG_ALBEDO) != 0u)
    {
        return saturate(material.debugAlbedo);
    }

    float4 texel = CleanSampleTexture(
        material.diffuseTextureIndex,
        material.textureWidth,
        material.textureHeight,
        texCoord,
        fallback);
    const bool textureDecodeEnabled = (((uint)TextureInfo.w) & 4u) != 0u;
    if (textureDecodeEnabled && (material.flags & RT_SMOKE_MATERIAL_DIFFUSE_YCOCG) != 0u)
    {
        texel.rgb = CleanConvertYCoCgToRGB(texel);
    }
    return saturate(texel);
}

float3 CleanTexturedEmissiveRadiance(PathTraceUnifiedLightRecord light)
{
    const float emissiveScale = max(CleanRtxdiDiToyPathInfo.z, 0.0);
    const float3 fallbackRadiance = max(light.radianceAndLuminance.rgb, float3(0.0, 0.0, 0.0)) * emissiveScale;
    if (emissiveScale <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }

    const uint sourceIndex = light.sourceIndex;
    if (sourceIndex >= CleanRtxdiDiCurrentEmissiveTriangleCount)
    {
        return fallbackRadiance;
    }

    const PathTraceSmokeEmissiveTriangle emissiveTriangle = SmokeEmissiveTriangles[sourceIndex];
    if (emissiveTriangle.materialIndex >= (uint)CleanRtxdiDiTextureInfo.z)
    {
        return fallbackRadiance;
    }

    const PathTraceSmokeMaterial material = CleanLoadSmokeMaterial(emissiveTriangle.materialIndex);
    const bool activeEmissiveStage = (emissiveTriangle.padding0 & RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF) == 0u;
    if ((material.flags & RT_SMOKE_MATERIAL_EMISSIVE) == 0u || !activeEmissiveStage)
    {
        return float3(0.0, 0.0, 0.0);
    }

    float3 radiance = max(material.emissiveColor.rgb, float3(0.0, 0.0, 0.0));
    if ((((uint)TextureInfo.w) & RT_SMOKE_TEXTURE_FLAG_USE_EMISSIVE_MAPS) != 0u &&
        material.emissiveTextureIndex != 0xffffffffu)
    {
        radiance *= saturate(CleanSampleTexture(
            material.emissiveTextureIndex,
            material.emissiveTextureWidth,
            material.emissiveTextureHeight,
            emissiveTriangle.centroidUvAndWeight.xy,
            float4(1.0, 1.0, 1.0, 1.0)).rgb);
    }
    radiance *= 1.75 * emissiveScale;
    return CleanLuminance(radiance) > 0.0 ? radiance : fallbackRadiance;
}

RAB_LightInfo CleanLoadRluLightInfo(uint lightIndex)
{
    RAB_LightInfo lightInfo = RAB_EmptyLightInfo();
    if (lightIndex >= CleanRtxdiDiRluCurrentLightCount)
    {
        return lightInfo;
    }

    const PathTraceUnifiedLightRecord light = CleanRtxdiDiRluCurrentLights[lightIndex];
    if (light.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
    {
        const float3 radiance = max(light.radianceAndLuminance.rgb, float3(0.0, 0.0, 0.0)) * max(CleanRtxdiDiDoomAnalyticLightInfo.z, 0.0);
        const float luminance = CleanLuminance(radiance);
        if (light.sourceWeight <= 0.0 || luminance <= 0.0 || light.positionAndRadius.w <= 0.0 || light.uvOrDoomParams.x <= 0.0)
        {
            return lightInfo;
        }
        lightInfo.lightType = RAB_LIGHT_TYPE_DOOM_ANALYTIC_SPHERE;
        lightInfo.lightIndex = lightIndex;
        lightInfo.unifiedLightType = PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC;
        lightInfo.materialIndex = RAB_INVALID_LIGHT_INDEX;
        lightInfo.flags = light.flags;
        lightInfo.position = light.positionAndRadius.xyz;
        lightInfo.radius = max(light.positionAndRadius.w, 0.01);
        lightInfo.influenceRadius = max(light.uvOrDoomParams.x, lightInfo.radius);
        lightInfo.normal = float3(0.0, 0.0, 1.0);
        lightInfo.area = max(light.uvOrDoomParams.y, 1.0e-4);
        lightInfo.radiance = radiance;
        lightInfo.weight = luminance * lightInfo.area * lightInfo.influenceRadius;
        return lightInfo;
    }

    if (light.type != PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE ||
        light.sourceIndex == PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX ||
        light.sourceIndex >= CleanRtxdiDiCurrentEmissiveTriangleCount ||
        light.sourceWeight <= 0.0)
    {
        return lightInfo;
    }

    const PathTraceSmokeEmissiveTriangle tri = SmokeEmissiveTriangles[light.sourceIndex];
    if (tri.materialIndex >= (uint)CleanRtxdiDiTextureInfo.z ||
        (tri.padding0 & RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF) != 0u)
    {
        return lightInfo;
    }

    const PathTraceSmokeMaterial material = CleanLoadSmokeMaterial(tri.materialIndex);
    if ((material.flags & RT_SMOKE_MATERIAL_EMISSIVE) == 0u)
    {
        return lightInfo;
    }

    float3 p0;
    float3 p1;
    float3 p2;
    float2 uv0;
    float2 uv1;
    float2 uv2;
    if (!CleanLoadEmissiveTriangleGeometry(tri, p0, p1, p2, uv0, uv1, uv2))
    {
        return lightInfo;
    }
    float3 normal;
    float area;
    if (!CleanTriangleNormalArea(p0, p1, p2, light.normalAndArea.xyz, normal, area))
    {
        return lightInfo;
    }

    const float3 radiance = CleanTexturedEmissiveRadiance(light);
    if (CleanLuminance(radiance) <= 0.0)
    {
        return lightInfo;
    }

    lightInfo.lightType = RAB_LIGHT_TYPE_EMISSIVE_TRIANGLE;
    lightInfo.lightIndex = lightIndex;
    lightInfo.unifiedLightType = PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE;
    lightInfo.materialIndex = light.materialOrLightId;
    lightInfo.flags = light.flags;
    lightInfo.position = light.positionAndRadius.xyz;
    lightInfo.radius = 0.0;
    lightInfo.influenceRadius = 0.0;
    lightInfo.normal = normal;
    lightInfo.area = area;
    lightInfo.radiance = radiance;
    lightInfo.weight = light.sourceWeight;
    lightInfo.sourceIndex = light.sourceIndex;
    lightInfo.hasTriangleGeometry = 1u;
    lightInfo.emissiveTextureIndex = material.emissiveTextureIndex;
    lightInfo.emissiveTextureWidth = material.emissiveTextureWidth;
    lightInfo.emissiveTextureHeight = material.emissiveTextureHeight;
    lightInfo.emissiveActiveStage = 1u;
    lightInfo.emissiveColor = max(material.emissiveColor.rgb, float3(0.0, 0.0, 0.0));
    lightInfo.trianglePosition0 = p0;
    lightInfo.trianglePosition1 = p1;
    lightInfo.trianglePosition2 = p2;
    lightInfo.triangleUv0 = uv0;
    lightInfo.triangleUv1 = uv1;
    lightInfo.triangleUv2 = uv2;
    return lightInfo;
}

float CleanTargetPdf(uint lightIndex, float2 sampleUv, RAB_Surface surface)
{
    const RAB_LightInfo lightInfo = CleanLoadRluLightInfo(lightIndex);
    const RAB_LightSample sample = RAB_SamplePolymorphicLight(lightInfo, surface, sampleUv);
    return max(PathTraceCleanRtxdiDiMaterialEvaluateLightSampleTargetPdf(sample, surface), 0.0);
}

float CleanTraceVisibility(RAB_Surface surface, RAB_LightInfo lightInfo, RAB_LightSample sample)
{
    const float3 surfacePosition = RAB_GetSurfaceWorldPos(surface);
    const float3 toSample = sample.position - surfacePosition;
    const float distanceSquared = dot(toSample, toSample);
    if (distanceSquared <= 1.0e-6)
    {
        return 0.0;
    }

    const float distance = sqrt(distanceSquared);
    const float3 direction = toSample / distance;
    const float3 shadingNormal = RAB_GetSurfaceNormal(surface);
    const float3 geometricNormal = RAB_GetSurfaceGeoNormal(surface);
    if (dot(shadingNormal, direction) <= 0.0 || dot(geometricNormal, direction) <= 0.0)
    {
        return 0.0;
    }

    RayDesc ray;
    ray.Origin = surfacePosition + geometricNormal * 0.75 + direction * 0.25;
    ray.Direction = direction;
    ray.TMin = 0.01;
    ray.TMax = max(distance - 0.5, 0.01);

    PathTraceCleanRtxdiPayload payload;
    payload.value = 0u;
    payload.rayMode = lightInfo.lightType == RAB_LIGHT_TYPE_EMISSIVE_TRIANGLE ? 2u : 1u;
    payload.ignoreInstanceId = 0xffffffffu;
    payload.ignorePrimitiveIndex = 0xffffffffu;
    payload.ignoreMaterialIndex = 0xffffffffu;
    if (lightInfo.lightType == RAB_LIGHT_TYPE_EMISSIVE_TRIANGLE &&
        lightInfo.sourceIndex < CleanRtxdiDiCurrentEmissiveTriangleCount)
    {
        const PathTraceSmokeEmissiveTriangle tri = SmokeEmissiveTriangles[lightInfo.sourceIndex];
        payload.ignoreInstanceId = tri.instanceId;
        payload.ignorePrimitiveIndex = tri.primitiveIndex;
        payload.ignoreMaterialIndex = tri.materialIndex;
    }

    TraceRay(SmokeScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_NON_OPAQUE, 0xff, 1, 1, 1, ray, payload);
    return payload.value == 0u ? 1.0 : 0.0;
}

bool CleanTryStoredReservoirVisibility(RTXDI_DIReservoir reservoir, out float visibility)
{
    RTXDI_VisibilityReuseParameters visibilityReuseParams = (RTXDI_VisibilityReuseParameters)0;
    visibilityReuseParams.maxAge = 16u;
    visibilityReuseParams.maxDistance = 4.0;

    float3 reusedVisibility = float3(0.0, 0.0, 0.0);
    if (RTXDI_GetDIReservoirVisibility(reservoir, visibilityReuseParams, reusedVisibility))
    {
        visibility = saturate(dot(reusedVisibility, float3(0.33333334, 0.33333334, 0.33333334)));
        return true;
    }

    visibility = 0.0;
    return false;
}

bool CleanSkipResolveVisibilityTrace(uint2 pixel, uint visibilityMode)
{
    if (visibilityMode == 2u)
    {
        return true;
    }
    if (visibilityMode == 3u)
    {
        uint h = pixel.x * 0x8da6b343u;
        h ^= pixel.y * 0xd8163841u;
        h ^= CleanRtxdiDiFrameIndex * 0xcb1ab31fu;
        h ^= h >> 16u;
        h *= 0x7feb352du;
        h ^= h >> 15u;
        return (h & 1u) != 0u;
    }
    return false;
}

float3 CleanResolve(uint2 pixel, uint lightIndex, float2 sampleUv, PathTracePrimarySurfaceRecord surfaceRecord, RAB_Surface surface, RTXDI_DIReservoir reservoir)
{
    const float3 receiverAlbedo = CleanTexturedSurfaceAlbedo(surfaceRecord);
    if ((CleanRtxdiDiResolveBrdfTarget & 1u) != 0u)
    {
        return receiverAlbedo;
    }

    const float3 surfaceEmission = max(surface.material.emissiveRadiance, float3(0.0, 0.0, 0.0));
    if (!RTXDI_IsValidDIReservoir(reservoir))
    {
        return surfaceEmission;
    }

    const RAB_LightInfo lightInfo = CleanLoadRluLightInfo(lightIndex);
    const RAB_LightSample sample = RAB_SamplePolymorphicLight(lightInfo, surface, sampleUv);
    if (!RAB_IsReplayableLightSample(sample) || sample.solidAnglePdf <= 0.0)
    {
        return surfaceEmission;
    }

    surface.material.diffuseAlbedo = receiverAlbedo;
    const float3 reflected = PathTraceCleanRtxdiDiMaterialEvaluateReflectedRadiance(sample.position, sample.radiance, surface);
    const uint visibilityMode = min(CleanRtxdiDiResolveVisibilityReuse, 3u);
    float visibility = 0.0;
    if (CleanSkipResolveVisibilityTrace(pixel, visibilityMode))
    {
        visibility = 1.0;
    }
    else
    {
        bool reusedVisibilityValid = false;
        if (visibilityMode == 1u || visibilityMode == 3u)
        {
            reusedVisibilityValid = CleanTryStoredReservoirVisibility(reservoir, visibility);
        }
        if (!reusedVisibilityValid)
        {
            visibility = CleanTraceVisibility(surface, lightInfo, sample);
        }
    }
    const float resolvePdf = (CleanRtxdiDiFlags & CLEAN_FLAG_RESOLVE_SOLID_ANGLE_PDF) != 0u ? max(sample.solidAnglePdf, 1.0e-6) : 1.0;
    const float3 selectedLightContribution =
        reflected * visibility * RTXDI_GetDIReservoirInvPdf(reservoir) / resolvePdf;
    return selectedLightContribution + surfaceEmission;
}

int2 CleanClampPixel(int2 pixel, uint2 dimensions)
{
    return clamp(pixel, int2(0, 0), int2(max(dimensions.x, 1u) - 1u, max(dimensions.y, 1u) - 1u));
}

RTXDI_DIReservoir CleanSpatialReuse(uint2 pixel, uint2 dimensions, PathTracePrimarySurfaceRecord surfaceRecord, RAB_Surface surface, RTXDI_DIReservoir centerReservoir)
{
    if (!RTXDI_IsValidDIReservoir(centerReservoir))
    {
        return centerReservoir;
    }
    if (CleanRtxdiDiSpatialInfo.w <= 0.5)
    {
        return centerReservoir;
    }

    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSamplerForPass(pixel, CleanRtxdiDiFrameIndex, 0x52525807u, 0u);
    PathTraceCleanRtxdiDiApplyBlueNoiseToggle(rng);
    RTXDI_DIReservoir result = RTXDI_EmptyDIReservoir();
    RTXDI_CombineDIReservoirs(result, centerReservoir, 0.5, centerReservoir.targetPdf);
    int2 selectedSourcePixel = int2(pixel);
    int2 acceptedNeighborPixels[16];
    float acceptedNeighborM[16];
    uint acceptedNeighborCount = 0u;

    const uint requestedSamples = clamp((uint)max(CleanRtxdiDiSpatialInfo.x, 1.0), 1u, 16u);
    const uint disocclusionSamples = clamp((uint)max(CleanRtxdiDiSpatialInfo.y, 1.0), 1u, 16u);
    const uint targetHistoryLength = (uint)clamp(CleanRtxdiDiRestirPTSurfaceInfo.y, 1.0, 64.0);
    const uint spatialSamples = min(centerReservoir.M < targetHistoryLength ? max(requestedSamples, disocclusionSamples) : requestedSamples, 16u);
    const float radius = max(CleanRtxdiDiSpatialInfo.z, 1.0);
    for (uint i = 0u; i < spatialSamples; ++i)
    {
        const uint offsetIndex = (uint)(RTXDI_GetNextRandom(rng) * 32.0) & 31u;
        const int2 neighborPixel = CleanClampPixel(int2(pixel) + int2(CLEAN_RTXDI_DI_SPATIAL_NEIGHBOR_OFFSETS[offsetIndex] * radius), dimensions);
        PathTracePrimarySurfaceRecord neighborSurface;
        if (!CleanLoadSurface((uint2)neighborPixel, dimensions, neighborSurface))
        {
            continue;
        }
        if (!RTXDI_IsValidNeighbor(
            CleanSurfaceNormal(surfaceRecord),
            CleanSurfaceNormal(neighborSurface),
            surfaceRecord.worldPositionAndViewDepth.w,
            neighborSurface.worldPositionAndViewDepth.w,
            0.35,
            0.10))
        {
            continue;
        }

        RTXDI_DIReservoir neighborReservoir = RTXDI_LoadDIReservoir(CleanReservoirParams(dimensions), (uint2)neighborPixel, 0u);
        if (!RTXDI_IsValidDIReservoir(neighborReservoir))
        {
            continue;
        }

        const float targetPdf = CleanTargetPdf(
            RTXDI_GetDIReservoirLightIndex(neighborReservoir),
            RTXDI_GetDIReservoirSampleUV(neighborReservoir),
            surface);
        neighborReservoir.spatialDistance += neighborPixel - int2(pixel);
        const bool selectedNeighbor = RTXDI_CombineDIReservoirs(result, neighborReservoir, RTXDI_GetNextRandom(rng), targetPdf);
        if (selectedNeighbor)
        {
            selectedSourcePixel = neighborPixel;
        }
        if (acceptedNeighborCount < 16u)
        {
            acceptedNeighborPixels[acceptedNeighborCount] = neighborPixel;
            acceptedNeighborM[acceptedNeighborCount] = neighborReservoir.M;
            acceptedNeighborCount++;
        }
    }

    if (RTXDI_IsValidDIReservoir(result))
    {
        const uint selectedLightIndex = RTXDI_GetDIReservoirLightIndex(result);
        const float2 selectedSampleUv = RTXDI_GetDIReservoirSampleUV(result);
        const float centerSelectedTargetPdf = max(result.targetPdf, 0.0);
        float selectedSourceTargetPdf = centerSelectedTargetPdf;
        if (any(selectedSourcePixel != int2(pixel)))
        {
            PathTracePrimarySurfaceRecord selectedSourceSurfaceRecord;
            if (CleanLoadSurface((uint2)selectedSourcePixel, dimensions, selectedSourceSurfaceRecord))
            {
                selectedSourceTargetPdf = max(CleanTargetPdf(
                    selectedLightIndex,
                    selectedSampleUv,
                    CleanSurfaceForView(selectedSourceSurfaceRecord)), 0.0);
            }
        }
        float targetPdfSum = centerSelectedTargetPdf * max(centerReservoir.M, 0.0);

        for (uint i = 0u; i < acceptedNeighborCount; ++i)
        {
            PathTracePrimarySurfaceRecord neighborSurfaceRecord;
            if (!CleanLoadSurface((uint2)acceptedNeighborPixels[i], dimensions, neighborSurfaceRecord))
            {
                continue;
            }

            const RAB_Surface neighborSurface = CleanSurfaceForView(neighborSurfaceRecord);
            const float neighborSelectedTargetPdf = CleanTargetPdf(selectedLightIndex, selectedSampleUv, neighborSurface);
            targetPdfSum += max(neighborSelectedTargetPdf, 0.0) * max(acceptedNeighborM[i], 0.0);
        }

        RTXDI_FinalizeResampling(result, selectedSourceTargetPdf, max(targetPdfSum, 1.0e-6));
    }
    return result;
}

[shader("raygeneration")]
void RayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dimensions = DispatchRaysDimensions().xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    if (PathTraceCleanRtxdiDiWriteLiquidPoolRouteDiagnostic(pixel))
    {
        return;
    }

    PathTracePrimarySurfaceRecord surface;
    if (!CleanLoadSurface(pixel, dimensions, surface))
    {
        SmokeOutput[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    const uint reservoirIndex = CleanReservoirIndex(pixel, dimensions);
    if (reservoirIndex >= CleanRtxdiDiReservoirCount)
    {
        SmokeOutput[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    const RTXDI_DIReservoir centerReservoir = RTXDI_LoadDIReservoir(CleanReservoirParams(dimensions), pixel, 0u);
    const RAB_Surface reuseSurface = CleanSurfaceForView(surface);
    const RTXDI_DIReservoir spatialReservoir = CleanSpatialReuse(pixel, dimensions, surface, reuseSurface, centerReservoir);
    CleanRtxdiDiSpatialReservoirs[reservoirIndex] = RTXDI_PackDIReservoir(spatialReservoir);

    if (CleanRtxdiDiView == 8u && CleanRtxdiDiView8Band == 16u)
    {
        const bool acceptedNeighbor = spatialReservoir.M > centerReservoir.M;
        SmokeOutput[pixel] = float4(acceptedNeighbor ? float3(0.0, 0.90, 0.20) : float3(0.20, 0.28, 0.95), 1.0);
        return;
    }

    const float3 resolvedRadiance = CleanResolve(
        pixel,
        RTXDI_GetDIReservoirLightIndex(spatialReservoir),
        RTXDI_GetDIReservoirSampleUV(spatialReservoir),
        surface,
        reuseSurface,
        spatialReservoir);
    PathTraceRRInputColor[pixel] = float4(resolvedRadiance, 1.0);
    SmokeOutput[pixel] = float4(resolvedRadiance, 1.0);
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
        InstanceID() == payload.ignoreInstanceId &&
        PrimitiveIndex() == payload.ignorePrimitiveIndex)
    {
        IgnoreHit();
    }
}

[shader("anyhit")]
void ShadowAnyHit(inout PathTraceCleanRtxdiPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    const uint instanceId = InstanceID();
    const uint primitiveIndex = PrimitiveIndex();
    const uint materialIndex = CleanLoadTriangleMaterialIndex(instanceId, primitiveIndex);
    if (CleanMaterialDoesNotOccludeVisibility(instanceId, materialIndex))
    {
        IgnoreHit();
    }

    if (payload.rayMode == 2u &&
        instanceId == payload.ignoreInstanceId &&
        primitiveIndex == payload.ignorePrimitiveIndex)
    {
        IgnoreHit();
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
