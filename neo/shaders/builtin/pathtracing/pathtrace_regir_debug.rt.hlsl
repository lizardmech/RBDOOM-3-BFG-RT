#include "../../vulkan.hlsli"
#include "PathTraceSkinnedHitRoute.hlsli"
#include "RtxdiBridge/RAB_ReGIR.hlsli"
#include "Rtxdi/Utils/Math.hlsli"

struct PathTraceReGIRPayload
{
    uint value;
    float hitT;
    float3 normal;
    float3 geometricNormal;
    float2 texCoord;
    float4 vertexColor;
    float4 vertexColorAdd;
    uint surfaceClass;
    uint translucentSubtype;
    uint triangleClassAndFlags;
    uint materialId;
    uint materialIndex;
    uint instanceId;
    uint primitiveIndex;
};

struct PathTraceReGIRShadowPayload
{
    uint hit;
};

struct PathTraceDoomAnalyticLightCandidate
{
    float4 originAndRadius;
    float4 colorAndIntensity;
    float4 doomRadiusAndArea;
    uint flags;
    uint renderLightIndex;
    uint entityNumber;
    uint padding0;
};

struct PathTraceDoomAnalyticLightCandidateIdentity
{
    uint universeIndex;
    uint flags;
    uint invalidReasonFlags;
    uint padding0;
};

struct PathTraceDoomAnalyticLightRemap
{
    int previousToCurrentCandidateIndex;
    int currentToPreviousCandidateIndex;
    uint flags;
    uint invalidReasonFlags;
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

struct PathTraceEmissiveLightRemap
{
    int previousToCurrentIndex;
    int currentToPreviousIndex;
    uint flags;
    uint padding0;
};

#include "RtxdiBridge/RAB_UnifiedLightRecord.hlsli"

VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> SmokeOutput : register(u1);
RaytracingAccelerationStructure SmokeScene : register(t0);
StructuredBuffer<PathTraceSmokeVertex> SmokeStaticVertices : register(t3);
StructuredBuffer<uint> SmokeStaticIndices : register(t4);
StructuredBuffer<uint> SmokeStaticTriangleClasses : register(t5);
StructuredBuffer<PathTraceSmokeVertex> SmokeDynamicVertices : register(t6);
StructuredBuffer<uint> SmokeDynamicIndices : register(t7);
StructuredBuffer<uint> SmokeDynamicTriangleClasses : register(t8);
StructuredBuffer<uint> SmokeStaticTriangleMaterials : register(t9);
StructuredBuffer<uint> SmokeDynamicTriangleMaterials : register(t10);
StructuredBuffer<uint> SmokeStaticTriangleMaterialIndexes : register(t11);
StructuredBuffer<uint> SmokeDynamicTriangleMaterialIndexes : register(t12);
StructuredBuffer<PathTraceSmokeMaterial> SmokeMaterials : register(t13);
StructuredBuffer<PathTraceSmokeEmissiveTriangle> SmokeEmissiveTriangles : register(t16);
StructuredBuffer<PathTraceSmokeVertex> SmokeRigidRouteVertices : register(t22);
StructuredBuffer<uint> SmokeRigidRouteIndices : register(t23);
StructuredBuffer<uint> SmokeRigidRouteTriangleMaterials : register(t24);
StructuredBuffer<uint> SmokeRigidRouteTriangleMaterialIndexes : register(t25);
StructuredBuffer<PathTraceRigidRouteInstance> SmokeRigidRouteInstances : register(t26);
StructuredBuffer<PathTraceSmokeEmissiveTriangle> SmokePreviousEmissiveTriangles : register(t57);
StructuredBuffer<PathTraceEmissiveLightRemap> SmokeEmissiveRemap : register(t58);
StructuredBuffer<PathTraceDoomAnalyticLightCandidate> DoomAnalyticLights : register(t27);
StructuredBuffer<PathTraceDoomAnalyticLightCandidateIdentity> DoomAnalyticCurrentIdentities : register(t42);
StructuredBuffer<PathTraceDoomAnalyticLightCandidateIdentity> DoomAnalyticPreviousIdentities : register(t43);
StructuredBuffer<PathTraceDoomAnalyticLightRemap> DoomAnalyticRemap : register(t44);
StructuredBuffer<PathTraceDoomAnalyticLightCandidate> DoomAnalyticPreviousLights : register(t45);
StructuredBuffer<PathTraceUnifiedLightRecord> PathTraceUnifiedLights : register(t59);
StructuredBuffer<PathTraceUnifiedLightRecord> PathTraceUnifiedPreviousLights : register(t60);
StructuredBuffer<uint> PathTraceUnifiedLightRemap : register(t61);
StructuredBuffer<uint> PathTraceRestirLightManagerCurrentToPrevious : register(t64);
StructuredBuffer<uint> PathTraceRestirLightManagerPreviousToCurrent : register(t65);
StructuredBuffer<PathTraceUnifiedLightRecord> PathTraceRestirLightManagerCurrentPayload : register(t66);
StructuredBuffer<PathTraceUnifiedLightRecord> PathTraceRestirLightManagerPreviousPayload : register(t67);
RWStructuredBuffer<PathTraceReGIRCandidateRecord> ReGIRCandidateCache : register(u72);

cbuffer PathTraceSmokeConstants : register(b2)
{
    float4 CameraOriginAndTMax : packoffset(c0);
    float4 CameraForwardAndTanX : packoffset(c1);
    float4 CameraLeftAndTanY : packoffset(c2);
    float4 CameraUpAndDebugMode : packoffset(c3);
    float4 TextureInfo : packoffset(c4);
    float4 ToyPathInfo : packoffset(c72);
    float4 EmissiveInfo : packoffset(c73);
    float4 DoomAnalyticLightInfo : packoffset(c76);
    float4 DoomAnalyticLightRemapInfo : packoffset(c77);
    float4 SafetyInfo : packoffset(c85);
    float4 GeometryInfo0 : packoffset(c86);
    float4 GeometryInfo1 : packoffset(c87);
    float4 GeometryInfo2 : packoffset(c88);
    float4 DispatchTileInfo : packoffset(c91);
    float4 MotionVectorInfo : packoffset(c93);
    float4 RestirPTSurfaceInfo : packoffset(c94);
    float4 UnifiedLightInfo : packoffset(c99);
    float4 RestirLightManagerInfo : packoffset(c100);
    float4 RestirLightManagerControlInfo : packoffset(c101);
    float4 RestirLightManagerRangeInfo : packoffset(c102);
    float4 RestirLightManagerSampleInfo : packoffset(c103);
    float4 ReGIRInfo0 : packoffset(c110);
    float4 ReGIRInfo1 : packoffset(c111);
    float4 ReGIRInfo2 : packoffset(c112);
    float4 ReGIRInfo3 : packoffset(c113);
    float4 ReGIRInfo4 : packoffset(c114);
};

static const uint RT_SMOKE_EMISSIVE_TRIANGLE_HISTORY_DYNAMIC = 0x00020000u;
static const uint RT_SMOKE_TRIANGLE_CLASS_MASK = 0x0000ffffu;
static const uint RT_SMOKE_TRIANGLE_FORCE_GEOMETRIC_NORMAL = 0x00010000u;
static const uint RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF = 0x00040000u;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_SHIFT = 24u;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_MASK = 0x0f000000u;
static const uint RT_SMOKE_SURFACE_CLASS_RIGID_ENTITY = 1u;
static const uint RT_SMOKE_TEXTURE_FLAG_RESERVOIR_TWO_SIDED_EMISSIVES = 0x00000040u;
static const uint RT_SMOKE_MATERIAL_EMISSIVE = 0x00000008u;
static const uint RT_PT_SAFETY_DISABLE_ANALYTIC_LIGHT_LOOP = 0x00000004u;
static const uint RT_PT_SAFETY_DISABLE_EMISSIVE_TRIANGLE_SAMPLING = 0x00000008u;

bool PathTraceSafetyDisabled(uint bit)
{
    return (((uint)SafetyInfo.x) & bit) != 0u;
}

uint PathTraceReGIRStaticVertexCount() { return (uint)max(GeometryInfo0.x, 0.0); }
uint PathTraceReGIRStaticIndexCount() { return (uint)max(GeometryInfo0.y, 0.0); }
uint PathTraceReGIRStaticTriangleCount() { return (uint)max(GeometryInfo0.z, 0.0); }
uint PathTraceReGIRDynamicVertexCount() { return (uint)max(GeometryInfo0.w, 0.0); }
uint PathTraceReGIRDynamicIndexCount() { return (uint)max(GeometryInfo1.x, 0.0); }
uint PathTraceReGIRDynamicTriangleCount() { return (uint)max(GeometryInfo1.y, 0.0); }
uint PathTraceReGIRRigidRouteVertexCount() { return (uint)max(GeometryInfo1.z, 0.0); }
uint PathTraceReGIRRigidRouteIndexCount() { return (uint)max(GeometryInfo1.w, 0.0); }
uint PathTraceReGIRRigidRouteTriangleCount() { return (uint)max(GeometryInfo2.x, 0.0); }
uint PathTraceReGIRRigidRouteInstanceCount() { return (uint)max(GeometryInfo2.y, 0.0); }

float3 PathTraceReGIRSafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-8 ? value * rsqrt(lengthSquared) : fallback;
}

float3 PathTraceReGIRTransformObjectVectorToWorld(float3 objectVector)
{
    const float3x4 objectToWorld = ObjectToWorld3x4();
    return float3(
        dot(objectToWorld[0].xyz, objectVector),
        dot(objectToWorld[1].xyz, objectVector),
        dot(objectToWorld[2].xyz, objectVector));
}

float3 PathTraceReGIRTransformObjectNormalToWorld(float3 objectNormal, float3 fallback)
{
    return PathTraceReGIRSafeNormalize(PathTraceReGIRTransformObjectVectorToWorld(objectNormal), fallback);
}

PathTraceSmokeMaterial PathTraceReGIRLoadSmokeMaterial(uint materialIndex)
{
    PathTraceSmokeMaterial material = (PathTraceSmokeMaterial)0;
    material.debugAlbedo = float4(0.6, 0.6, 0.6, 1.0);
    material.emissiveColor = float4(0.0, 0.0, 0.0, 1.0);
    material.diffuseTextureIndex = 0xffffffffu;
    material.alphaTextureIndex = 0xffffffffu;
    material.normalTextureIndex = 0xffffffffu;
    material.specularTextureIndex = 0xffffffffu;
    material.emissiveTextureIndex = 0xffffffffu;
    material.alphaCutoff = 0.0;
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

    const uint materialCount = (uint)TextureInfo.z;
    if (materialIndex < materialCount)
    {
        material = SmokeMaterials[materialIndex];
    }
    return material;
}

uint PathTraceReGIRLoadTriangleMaterialIndex(uint instanceId, uint primitiveIndex)
{
    const uint materialCount = (uint)TextureInfo.z;
    uint materialIndex = 0xffffffffu;
    if (instanceId == 0u)
    {
        materialIndex = primitiveIndex < PathTraceReGIRStaticTriangleCount()
            ? SmokeStaticTriangleMaterialIndexes[primitiveIndex]
            : 0xffffffffu;
    }
    else if (instanceId == 1u)
    {
        materialIndex = primitiveIndex < PathTraceReGIRDynamicTriangleCount()
            ? SmokeDynamicTriangleMaterialIndexes[primitiveIndex]
            : 0xffffffffu;
    }
    else
    {
        const uint routeInstanceIndex = instanceId - 2u;
        if (routeInstanceIndex < PathTraceReGIRRigidRouteInstanceCount())
        {
            const PathTraceRigidRouteInstance routeInstance = SmokeRigidRouteInstances[routeInstanceIndex];
            if (primitiveIndex < routeInstance.triangleCount &&
                routeInstance.triangleOffset + primitiveIndex < PathTraceReGIRRigidRouteTriangleCount())
            {
                materialIndex = SmokeRigidRouteTriangleMaterialIndexes[routeInstance.triangleOffset + primitiveIndex];
            }
        }
    }
    return materialIndex < materialCount ? materialIndex : 0xffffffffu;
}

uint PathTraceReGIRLoadTriangleMaterialId(uint instanceId, uint primitiveIndex)
{
    if (instanceId == 0u)
    {
        return primitiveIndex < PathTraceReGIRStaticTriangleCount()
            ? SmokeStaticTriangleMaterials[primitiveIndex]
            : 0xffffffffu;
    }
    if (instanceId == 1u)
    {
        return primitiveIndex < PathTraceReGIRDynamicTriangleCount()
            ? SmokeDynamicTriangleMaterials[primitiveIndex]
            : 0xffffffffu;
    }

    const uint routeInstanceIndex = instanceId - 2u;
    if (routeInstanceIndex >= PathTraceReGIRRigidRouteInstanceCount())
    {
        return 0xffffffffu;
    }
    const PathTraceRigidRouteInstance routeInstance = SmokeRigidRouteInstances[routeInstanceIndex];
    return primitiveIndex < routeInstance.triangleCount &&
        routeInstance.triangleOffset + primitiveIndex < PathTraceReGIRRigidRouteTriangleCount()
        ? SmokeRigidRouteTriangleMaterials[routeInstance.triangleOffset + primitiveIndex]
        : 0xffffffffu;
}

uint PathTraceReGIRLoadTriangleClassAndFlags(uint instanceId, uint primitiveIndex)
{
    if (instanceId >= 2u)
    {
        return RT_SMOKE_SURFACE_CLASS_RIGID_ENTITY;
    }
    if (instanceId == 0u)
    {
        return primitiveIndex < PathTraceReGIRStaticTriangleCount() ? SmokeStaticTriangleClasses[primitiveIndex] : 0u;
    }
    return primitiveIndex < PathTraceReGIRDynamicTriangleCount() ? SmokeDynamicTriangleClasses[primitiveIndex] : 0u;
}

bool PathTraceReGIRTriangleRangeValid(uint instanceId, uint primitiveIndex)
{
    if (instanceId == 0u)
    {
        const uint indexOffset = primitiveIndex * 3u;
        return primitiveIndex < PathTraceReGIRStaticTriangleCount() &&
            indexOffset <= PathTraceReGIRStaticIndexCount() &&
            indexOffset + 2u < PathTraceReGIRStaticIndexCount();
    }
    if (instanceId == 1u)
    {
        const uint indexOffset = primitiveIndex * 3u;
        return primitiveIndex < PathTraceReGIRDynamicTriangleCount() &&
            indexOffset <= PathTraceReGIRDynamicIndexCount() &&
            indexOffset + 2u < PathTraceReGIRDynamicIndexCount();
    }

    const uint routeInstanceIndex = instanceId - 2u;
    if (routeInstanceIndex >= PathTraceReGIRRigidRouteInstanceCount())
    {
        return false;
    }
    const PathTraceRigidRouteInstance routeInstance = SmokeRigidRouteInstances[routeInstanceIndex];
    const uint routeIndexOffset = routeInstance.indexOffset + primitiveIndex * 3u;
    return primitiveIndex < routeInstance.triangleCount &&
        routeInstance.triangleOffset + primitiveIndex < PathTraceReGIRRigidRouteTriangleCount() &&
        routeIndexOffset <= PathTraceReGIRRigidRouteIndexCount() &&
        routeIndexOffset + 2u < PathTraceReGIRRigidRouteIndexCount();
}

#define RB_PT_ENABLE_RESTIR_LIGHT_MANAGER_RAB 1
#include "RtxdiBridge/RAB_LightInfoRuntime.hlsli"
#include "RtxdiBridge/RAB_LightLoadRuntime.hlsli"
#include "RtxdiBridge/RAB_LightTarget.hlsli"

uint2 PathTraceReGIRDispatchTileOffset()
{
    return uint2((uint)max(DispatchTileInfo.x, 0.0), (uint)max(DispatchTileInfo.y, 0.0));
}

uint2 PathTraceReGIRFullOutputSize()
{
    const uint2 size = uint2((uint)max(DispatchTileInfo.z, 0.0), (uint)max(DispatchTileInfo.w, 0.0));
    return (size.x > 0u && size.y > 0u) ? size : DispatchRaysDimensions().xy;
}

float4 PathTraceReGIRMissColor(uint view)
{
    if (view == 1u)
    {
        return float4(0.0, 0.16, 0.38, 1.0);
    }
    return float4(0.0, 0.0, 0.0, 1.0);
}

float4 PathTraceReGIRDebugColor(float3 worldPosition)
{
    const uint enable = (uint)ReGIRInfo0.x;
    const uint view = (uint)ReGIRInfo0.y;
    const uint mode = (uint)ReGIRInfo0.z;
    const uint centerMode = (uint)ReGIRInfo0.w;
    const float cellSize = ReGIRInfo1.x;
    const uint3 gridDimensions = uint3((uint)ReGIRInfo1.y, (uint)ReGIRInfo1.z, (uint)ReGIRInfo1.w);
    if (enable == 0u || view == 0u || mode == 0u)
    {
        return float4(0.25, 0.0, 0.0, 1.0);
    }

    const PathTraceReGIRCellDebug cell = PathTraceReGIRMapWorldPositionToCell(
        worldPosition,
        CameraOriginAndTMax.xyz,
        centerMode,
        mode,
        cellSize,
        gridDimensions,
        ReGIRInfo4.xyz);
    const bool boundary = cell.boundary < 0.035;
    if (view == 1u)
    {
        return cell.valid != 0u
            ? float4(boundary ? float3(1.0, 1.0, 1.0) : float3(0.0, 0.72, 0.24), 1.0)
            : float4(1.0, 0.0, 0.82, 1.0);
    }
    if (view == 2u)
    {
        const float3 color = PathTraceReGIRCellColor(cell.globalCoord);
        return float4(boundary ? float3(1.0, 1.0, 1.0) : color, 1.0);
    }
    if (view == 3u)
    {
        if (cell.valid == 0u)
        {
            return float4(1.0, 0.0, 0.82, 1.0);
        }
        return float4(boundary ? float3(1.0, 0.9, 0.0) : float3(0.0, 0.85, 0.18), 1.0);
    }
    return float4(0.45, 0.0, 0.0, 1.0);
}

float PathTraceReGIRLuminance(float3 value)
{
    return dot(max(value, float3(0.0, 0.0, 0.0)), float3(0.2126, 0.7152, 0.0722));
}

float PathTraceReGIRHashToUnitFloat(uint hash)
{
    return ((hash & 0x00ffffffu) + 0.5) / 16777216.0;
}

float3 PathTraceReGIRCellCenter(PathTraceReGIRCellDebug cell, float cellSize)
{
    return cell.worldCenter;
}

float PathTraceReGIRCellExtent(PathTraceReGIRCellDebug cell, float cellSize)
{
    const float3 extent = max(cell.worldExtent, float3(cellSize, cellSize, cellSize));
    return max(extent.x, max(extent.y, extent.z));
}

uint PathTraceReGIRProposalSeed(PathTraceReGIRCellDebug cell, uint slotInCell, uint proposalIndex, uint salt)
{
    return PathTraceReGIRHashCell(cell.globalCoord + int3(
        (int)(slotInCell + proposalIndex * 131u + salt),
        (int)(slotInCell * 17u + proposalIndex * 29u + salt * 17u),
        (int)(slotInCell * 31u + proposalIndex * 47u + salt * 31u)));
}

uint PathTraceReGIRBuildSampleCount(uint domainCount)
{
    const uint configuredSamples = clamp((uint)max(ReGIRInfo2.y, 1.0), 1u, 64u);
    return min(configuredSamples, domainCount);
}

uint PathTraceReGIRProposalDomainIndex(PathTraceReGIRCellDebug cell, uint slotInCell, uint proposalIndex, uint salt, uint domainCount)
{
    if (domainCount == 0u)
    {
        return 0u;
    }
    return PathTraceReGIRProposalSeed(cell, slotInCell, proposalIndex, salt) % domainCount;
}

float PathTraceReGIRAnalyticCellWeight(PathTraceDoomAnalyticLightCandidate light, float3 cellCenter, float cellSize)
{
    const float radius = max(light.originAndRadius.w, 1.0e-3);
    const float influenceRadius = max(light.doomRadiusAndArea.x, radius);
    const float cellRadius = max(cellSize, 1.0) * 0.8660254;
    const float influenceDistance = influenceRadius + cellRadius;
    const float3 toCell = cellCenter - light.originAndRadius.xyz;
    const float distanceToCell = length(toCell);
    if (distanceToCell > influenceDistance)
    {
        return 0.0;
    }

    const float3 radiance = max(light.colorAndIntensity.rgb, float3(0.0, 0.0, 0.0)) * max(DoomAnalyticLightInfo.z, 0.0);
    const float luminance = PathTraceReGIRLuminance(radiance);
    const float area = max(light.doomRadiusAndArea.y, 4.0 * 3.14159265 * radius * radius);
    const float edgeDistance = max(distanceToCell - radius, 0.0);
    const float influenceFalloff = saturate(1.0 - edgeDistance / max(influenceDistance - radius, 1.0));
    const float distanceWeight = 1.0 / max(dot(toCell, toCell) + radius * radius + cellSize * cellSize * 0.25, 1.0);
    return luminance * area * influenceFalloff * influenceFalloff * distanceWeight;
}

float PathTraceReGIREmissiveCellWeight(PathTraceSmokeEmissiveTriangle emissiveTriangle, float3 cellCenter, float cellSize)
{
    const float area = max(emissiveTriangle.centerAndArea.w, 0.0);
    if (area <= 1.0e-6)
    {
        return 0.0;
    }

    const float3 toCell = cellCenter - emissiveTriangle.centerAndArea.xyz;
    const float distanceSquared = dot(toCell, toCell);
    const float normalFacing = saturate(dot(normalize(toCell + float3(0.0, 0.0, 1.0e-6)), emissiveTriangle.normalAndLuminance.xyz));
    const float radianceLuminance = max(emissiveTriangle.estimatedRadianceAndLuminance.w, PathTraceReGIRLuminance(emissiveTriangle.estimatedRadianceAndLuminance.rgb));
    const float sourceWeight = max(max(emissiveTriangle.sampleWeightAndPdf.x, radianceLuminance), 0.0);
    const float cellRadius = max(cellSize, 1.0) * 0.8660254;
    const float distanceWeight = 1.0 / max(distanceSquared + cellRadius * cellRadius, 1.0);
    return sourceWeight * area * (0.2 + 0.8 * normalFacing) * distanceWeight;
}

bool PathTraceReGIRUseCurrentRabLightUniverse()
{
    return RAB_RestirLightManagerRABEnabled() &&
        RAB_RestirLightManagerRemixDenseDomainEnabled() &&
        RAB_GetCurrentRestirLightManagerCount() > 0u;
}

bool PathTraceReGIRRluAnalyticStableCacheable(uint denseIndex)
{
    if (!PathTraceReGIRUseCurrentRabLightUniverse() ||
        denseIndex >= RAB_GetCurrentRestirLightManagerCount())
    {
        return false;
    }

    const PathTraceUnifiedLightRecord record = PathTraceRestirLightManagerCurrentPayload[denseIndex];
    return record.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC &&
        (record.flags & PATH_TRACE_RLU_LIGHT_FLAG_STABLE_CACHEABLE) != 0u;
}

bool PathTraceReGIRSourceViewRequiresCurrentRlu()
{
    const uint debugView = (uint)ReGIRInfo0.y;
    return debugView >= 4u && debugView <= 10u;
}

uint PathTraceReGIRLightClassFromCurrentRluPayload(uint denseIndex)
{
    if (denseIndex >= RAB_GetCurrentRestirLightManagerCount())
    {
        return PATH_TRACE_REGIR_LIGHT_CLASS_NONE;
    }

    const PathTraceUnifiedLightRecord record = PathTraceRestirLightManagerCurrentPayload[denseIndex];
    if (record.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
    {
        return PATH_TRACE_REGIR_LIGHT_CLASS_DOOM_ANALYTIC;
    }
    if (record.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
    {
        return PATH_TRACE_REGIR_LIGHT_CLASS_EMISSIVE_TRIANGLE;
    }
    return PATH_TRACE_REGIR_LIGHT_CLASS_NONE;
}

uint PathTraceReGIRBuildSampleCountForClass(uint lightClass, uint domainCount)
{
    if (domainCount == 0u)
    {
        return 0u;
    }
    if (PathTraceReGIRUseCurrentRabLightUniverse())
    {
        const uint rluSampleCount =
            lightClass == PATH_TRACE_REGIR_LIGHT_CLASS_DOOM_ANALYTIC
                ? RAB_GetRestirLightManagerDoomAnalyticSampleCount()
                : (lightClass == PATH_TRACE_REGIR_LIGHT_CLASS_EMISSIVE_TRIANGLE
                    ? RAB_GetRestirLightManagerEmissiveSampleCount()
                    : 0u);
        return min(rluSampleCount, domainCount);
    }
    return PathTraceReGIRBuildSampleCount(domainCount);
}

uint2 PathTraceReGIRCurrentAnalyticRange()
{
    if (PathTraceReGIRUseCurrentRabLightUniverse())
    {
        const uint2 range = RAB_GetRestirLightManagerDoomAnalyticRange();
        return uint2(range.x, min(range.y, RAB_GetRestirLightManagerStableDoomAnalyticCount()));
    }

    return uint2(0u, (uint)max(DoomAnalyticLightInfo.x, 0.0));
}

uint2 PathTraceReGIRCurrentEmissiveRange()
{
    if (PathTraceReGIRUseCurrentRabLightUniverse())
    {
        return RAB_GetRestirLightManagerEmissiveRange();
    }

    return uint2(0u, (uint)max(EmissiveInfo.x, 0.0));
}

PathTraceReGIRCandidateRecord PathTraceReGIRBuildDeterministicSourceCandidate(
    PathTraceReGIRCellDebug cell,
    uint slotInCell,
    uint slotIndex,
    uint lightClass,
    uint rangeOffset,
    uint rangeCount,
    float classProbability,
    uint salt,
    uint fallbackEmptyReason)
{
    if (rangeCount == 0u)
    {
        return PathTraceReGIREmptyCandidate(cell.localCellIndex, slotIndex, fallbackEmptyReason);
    }

    const uint localIndex = PathTraceReGIRProposalDomainIndex(cell, slotInCell, 0u, salt, rangeCount);
    const uint denseIndex = rangeOffset + localIndex;
    const bool useRabLightUniverse = PathTraceReGIRUseCurrentRabLightUniverse();
    const uint resolvedLightClass = useRabLightUniverse
        ? PathTraceReGIRLightClassFromCurrentRluPayload(denseIndex)
        : lightClass;
    if (resolvedLightClass == PATH_TRACE_REGIR_LIGHT_CLASS_NONE)
    {
        return PathTraceReGIREmptyCandidate(cell.localCellIndex, slotIndex, PATH_TRACE_REGIR_EMPTY_UNSUPPORTED_DOMAIN);
    }
    if (useRabLightUniverse &&
        resolvedLightClass == PATH_TRACE_REGIR_LIGHT_CLASS_DOOM_ANALYTIC &&
        !PathTraceReGIRRluAnalyticStableCacheable(denseIndex))
    {
        return PathTraceReGIREmptyCandidate(cell.localCellIndex, slotIndex, PATH_TRACE_REGIR_EMPTY_UNSUPPORTED_DOMAIN);
    }

    PathTraceReGIRCandidateRecord candidate;
    candidate.lightIndex = useRabLightUniverse ? denseIndex : localIndex;
    candidate.lightClass = resolvedLightClass;
    candidate.invSourcePdf = (float)rangeCount / max(classProbability, 1.0e-8);
    candidate.sourcePdf = 1.0 / max(candidate.invSourcePdf, 1.0e-8);
    candidate.cellIndex = cell.localCellIndex;
    candidate.slotIndex = slotIndex;
    candidate.flags = PATH_TRACE_REGIR_CANDIDATE_VALID | PATH_TRACE_REGIR_CANDIDATE_WRITTEN;
    candidate.globalIdentity = useRabLightUniverse
        ? denseIndex
        : (resolvedLightClass == PATH_TRACE_REGIR_LIGHT_CLASS_DOOM_ANALYTIC ? PathTraceReGIRCurrentEmissiveRange().y + localIndex : localIndex);
    return candidate;
}

float PathTraceReGIRUnifiedAnalyticCellWeight(PathTraceUnifiedLightRecord record, float3 cellCenter, float cellSize)
{
    if (record.type != PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
    {
        return 0.0;
    }

    const float radius = max(record.positionAndRadius.w, 1.0e-3);
    const float influenceRadius = max(record.uvOrDoomParams.x, radius);
    const float cellRadius = max(cellSize, 1.0) * 0.8660254;
    const float influenceDistance = influenceRadius + cellRadius;
    const float3 toCell = cellCenter - record.positionAndRadius.xyz;
    const float distanceToCell = length(toCell);
    if (distanceToCell > influenceDistance)
    {
        return 0.0;
    }

    const float area = max(record.normalAndArea.w, 4.0 * 3.14159265 * radius * radius);
    const float edgeDistance = max(distanceToCell - radius, 0.0);
    const float influenceFalloff = saturate(1.0 - edgeDistance / max(influenceDistance - radius, 1.0));
    const float distanceWeight = 1.0 / max(dot(toCell, toCell) + radius * radius + cellSize * cellSize * 0.25, 1.0);
    return area * influenceFalloff * influenceFalloff * distanceWeight;
}

float PathTraceReGIRUnifiedEmissiveCellWeight(PathTraceUnifiedLightRecord record, float3 cellCenter, float cellSize)
{
    if (record.type != PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
    {
        return 0.0;
    }

    const float area = max(record.normalAndArea.w, 0.0);
    if (area <= 1.0e-6)
    {
        return 0.0;
    }

    const float3 toCell = cellCenter - record.positionAndRadius.xyz;
    const float distanceSquared = dot(toCell, toCell);
    const float normalFacing = saturate(dot(normalize(toCell + float3(0.0, 0.0, 1.0e-6)), record.normalAndArea.xyz));
    const float cellRadius = max(cellSize, 1.0) * 0.8660254;
    const float distanceWeight = 1.0 / max(distanceSquared + cellRadius * cellRadius, 1.0);
    return area * (0.2 + 0.8 * normalFacing) * distanceWeight;
}

PathTraceReGIRCandidateRecord PathTraceReGIRBuildAnalyticCandidate(PathTraceReGIRCellDebug cell, uint slotInCell, uint slotIndex, float classProbability)
{
    const bool useRabLightUniverse = PathTraceReGIRUseCurrentRabLightUniverse();
    const uint2 analyticRange = PathTraceReGIRCurrentAnalyticRange();
    const uint analyticCount = analyticRange.y;
    const uint emissiveCount = PathTraceReGIRCurrentEmissiveRange().y;
    const float cellSize = max(ReGIRInfo1.x, 1.0);
    const float3 cellCenter = PathTraceReGIRCellCenter(cell, cellSize);
    const float cellExtent = PathTraceReGIRCellExtent(cell, cellSize);
    if (analyticCount == 0u)
    {
        return PathTraceReGIREmptyCandidate(cell.localCellIndex, slotIndex, PATH_TRACE_REGIR_EMPTY_NO_ANALYTIC_LIGHTS);
    }

    uint selectedAnalytic = 0u;
    uint acceptedProposalCount = 0u;
    float selectedCellWeight = 0.0;
    float risWeightSum = 0.0;
    const uint proposalCount = PathTraceReGIRBuildSampleCountForClass(PATH_TRACE_REGIR_LIGHT_CLASS_DOOM_ANALYTIC, analyticCount);
    if (proposalCount == 0u)
    {
        return PathTraceReGIREmptyCandidate(cell.localCellIndex, slotIndex, PATH_TRACE_REGIR_EMPTY_NO_LOCAL_ANALYTIC_LIGHTS);
    }
    const float proposalInvSourcePdf = (float)analyticCount / max(max(classProbability, 1.0e-8) * (float)proposalCount, 1.0e-8);
    [loop]
    for (uint proposalIndex = 0u; proposalIndex < proposalCount; ++proposalIndex)
    {
        const uint analyticLocalIndex = PathTraceReGIRProposalDomainIndex(cell, slotInCell, proposalIndex, 101u, analyticCount);
        const uint analyticIndex = analyticRange.x + analyticLocalIndex;
        if (useRabLightUniverse && !PathTraceReGIRRluAnalyticStableCacheable(analyticIndex))
        {
            continue;
        }
        const float proposalWeight = useRabLightUniverse
            ? PathTraceReGIRUnifiedAnalyticCellWeight(PathTraceRestirLightManagerCurrentPayload[analyticIndex], cellCenter, cellExtent)
            : PathTraceReGIRAnalyticCellWeight(DoomAnalyticLights[analyticLocalIndex], cellCenter, cellExtent);
        if (proposalWeight > 0.0)
        {
            const float risWeight = proposalWeight * proposalInvSourcePdf;
            ++acceptedProposalCount;
            risWeightSum += risWeight;
            const float selector = PathTraceReGIRHashToUnitFloat(PathTraceReGIRProposalSeed(cell, slotInCell, proposalIndex, 151u));
            if (acceptedProposalCount == 1u || selector < risWeight / max(risWeightSum, 1.0e-8))
            {
                selectedAnalytic = analyticIndex;
                selectedCellWeight = proposalWeight;
            }
        }
    }

    if (acceptedProposalCount == 0u || selectedCellWeight <= 0.0 || risWeightSum <= 0.0)
    {
        return PathTraceReGIREmptyCandidate(cell.localCellIndex, slotIndex, PATH_TRACE_REGIR_EMPTY_NO_LOCAL_ANALYTIC_LIGHTS);
    }

    const float invSourcePdf = risWeightSum / max(selectedCellWeight, 1.0e-8);
    PathTraceReGIRCandidateRecord candidate;
    candidate.lightIndex = selectedAnalytic;
    candidate.lightClass = useRabLightUniverse ? PathTraceReGIRLightClassFromCurrentRluPayload(selectedAnalytic) : PATH_TRACE_REGIR_LIGHT_CLASS_DOOM_ANALYTIC;
    if (candidate.lightClass != PATH_TRACE_REGIR_LIGHT_CLASS_DOOM_ANALYTIC)
    {
        return PathTraceReGIREmptyCandidate(cell.localCellIndex, slotIndex, PATH_TRACE_REGIR_EMPTY_UNSUPPORTED_DOMAIN);
    }
    if (useRabLightUniverse && !PathTraceReGIRRluAnalyticStableCacheable(selectedAnalytic))
    {
        return PathTraceReGIREmptyCandidate(cell.localCellIndex, slotIndex, PATH_TRACE_REGIR_EMPTY_UNSUPPORTED_DOMAIN);
    }
    candidate.invSourcePdf = max(invSourcePdf, 1.0e-8);
    candidate.sourcePdf = 1.0 / candidate.invSourcePdf;
    candidate.cellIndex = cell.localCellIndex;
    candidate.slotIndex = slotIndex;
    candidate.flags = PATH_TRACE_REGIR_CANDIDATE_VALID | PATH_TRACE_REGIR_CANDIDATE_WRITTEN;
    candidate.globalIdentity = useRabLightUniverse ? selectedAnalytic : emissiveCount + selectedAnalytic;
    return candidate;
}

PathTraceReGIRCandidateRecord PathTraceReGIRBuildEmissiveCandidate(PathTraceReGIRCellDebug cell, uint slotInCell, uint slotIndex, float classProbability)
{
    const bool useRabLightUniverse = PathTraceReGIRUseCurrentRabLightUniverse();
    const uint2 emissiveRange = PathTraceReGIRCurrentEmissiveRange();
    const uint emissiveCount = emissiveRange.y;
    const float cellSize = max(ReGIRInfo1.x, 1.0);
    const float3 cellCenter = PathTraceReGIRCellCenter(cell, cellSize);
    const float cellExtent = PathTraceReGIRCellExtent(cell, cellSize);
    if (emissiveCount == 0u)
    {
        return PathTraceReGIREmptyCandidate(cell.localCellIndex, slotIndex, PATH_TRACE_REGIR_EMPTY_NO_EMISSIVE_LIGHTS);
    }

    uint selectedEmissive = 0u;
    uint acceptedProposalCount = 0u;
    float selectedCellWeight = 0.0;
    float risWeightSum = 0.0;
    const uint proposalCount = PathTraceReGIRBuildSampleCountForClass(PATH_TRACE_REGIR_LIGHT_CLASS_EMISSIVE_TRIANGLE, emissiveCount);
    if (proposalCount == 0u)
    {
        return PathTraceReGIREmptyCandidate(cell.localCellIndex, slotIndex, PATH_TRACE_REGIR_EMPTY_NO_LOCAL_EMISSIVE_LIGHTS);
    }
    const float proposalInvSourcePdf = (float)emissiveCount / max(max(classProbability, 1.0e-8) * (float)proposalCount, 1.0e-8);
    [loop]
    for (uint proposalIndex = 0u; proposalIndex < proposalCount; ++proposalIndex)
    {
        const uint emissiveLocalIndex = PathTraceReGIRProposalDomainIndex(cell, slotInCell, proposalIndex, 103u, emissiveCount);
        const uint emissiveIndex = emissiveRange.x + emissiveLocalIndex;
        const float proposalWeight = useRabLightUniverse
            ? PathTraceReGIRUnifiedEmissiveCellWeight(PathTraceRestirLightManagerCurrentPayload[emissiveIndex], cellCenter, cellExtent)
            : PathTraceReGIREmissiveCellWeight(SmokeEmissiveTriangles[emissiveLocalIndex], cellCenter, cellExtent);
        if (proposalWeight > 0.0)
        {
            const float risWeight = proposalWeight * proposalInvSourcePdf;
            ++acceptedProposalCount;
            risWeightSum += risWeight;
            const float selector = PathTraceReGIRHashToUnitFloat(PathTraceReGIRProposalSeed(cell, slotInCell, proposalIndex, 157u));
            if (acceptedProposalCount == 1u || selector < risWeight / max(risWeightSum, 1.0e-8))
            {
                selectedEmissive = emissiveIndex;
                selectedCellWeight = proposalWeight;
            }
        }
    }

    if (acceptedProposalCount == 0u || selectedCellWeight <= 0.0 || risWeightSum <= 0.0)
    {
        return PathTraceReGIREmptyCandidate(cell.localCellIndex, slotIndex, PATH_TRACE_REGIR_EMPTY_NO_LOCAL_EMISSIVE_LIGHTS);
    }

    const float invSourcePdf = risWeightSum / max(selectedCellWeight, 1.0e-8);
    PathTraceReGIRCandidateRecord candidate;
    candidate.lightIndex = selectedEmissive;
    candidate.lightClass = useRabLightUniverse ? PathTraceReGIRLightClassFromCurrentRluPayload(selectedEmissive) : PATH_TRACE_REGIR_LIGHT_CLASS_EMISSIVE_TRIANGLE;
    if (candidate.lightClass != PATH_TRACE_REGIR_LIGHT_CLASS_EMISSIVE_TRIANGLE)
    {
        return PathTraceReGIREmptyCandidate(cell.localCellIndex, slotIndex, PATH_TRACE_REGIR_EMPTY_UNSUPPORTED_DOMAIN);
    }
    candidate.invSourcePdf = max(invSourcePdf, 1.0e-8);
    candidate.sourcePdf = 1.0 / candidate.invSourcePdf;
    candidate.cellIndex = cell.localCellIndex;
    candidate.slotIndex = slotIndex;
    candidate.flags = PATH_TRACE_REGIR_CANDIDATE_VALID | PATH_TRACE_REGIR_CANDIDATE_WRITTEN;
    candidate.globalIdentity = selectedEmissive;
    return candidate;
}

PathTraceReGIRCandidateRecord PathTraceReGIRBuildCandidateForCellSlot(PathTraceReGIRCellDebug cell, uint slotInCell, uint slotIndex)
{
    const uint lightDomain = (uint)ReGIRInfo2.z;
    const uint debugView = (uint)ReGIRInfo0.y;
    if (PathTraceReGIRSourceViewRequiresCurrentRlu() && !PathTraceReGIRUseCurrentRabLightUniverse())
    {
        return PathTraceReGIREmptyCandidate(cell.localCellIndex, slotIndex, PATH_TRACE_REGIR_EMPTY_NO_CURRENT_RLU_SOURCE);
    }

    const uint2 analyticRange = PathTraceReGIRCurrentAnalyticRange();
    const uint2 emissiveRange = PathTraceReGIRCurrentEmissiveRange();
    const uint analyticCount = analyticRange.y;
    const uint emissiveCount = emissiveRange.y;
    const bool deterministicSourceDiagnostic = debugView == 9u;

    if (lightDomain == 0u)
    {
        if (deterministicSourceDiagnostic)
        {
            return PathTraceReGIRBuildDeterministicSourceCandidate(
                cell, slotInCell, slotIndex,
                PATH_TRACE_REGIR_LIGHT_CLASS_DOOM_ANALYTIC,
                analyticRange.x, analyticCount, 1.0, 101u,
                PATH_TRACE_REGIR_EMPTY_NO_ANALYTIC_LIGHTS);
        }
        return PathTraceReGIRBuildAnalyticCandidate(cell, slotInCell, slotIndex, 1.0);
    }
    if (lightDomain == 1u)
    {
        if (deterministicSourceDiagnostic)
        {
            return PathTraceReGIRBuildDeterministicSourceCandidate(
                cell, slotInCell, slotIndex,
                PATH_TRACE_REGIR_LIGHT_CLASS_EMISSIVE_TRIANGLE,
                emissiveRange.x, emissiveCount, 1.0, 103u,
                PATH_TRACE_REGIR_EMPTY_NO_EMISSIVE_LIGHTS);
        }
        return PathTraceReGIRBuildEmissiveCandidate(cell, slotInCell, slotIndex, 1.0);
    }
    if (lightDomain == 2u)
    {
        if (analyticCount == 0u && emissiveCount == 0u)
        {
            return PathTraceReGIREmptyCandidate(cell.localCellIndex, slotIndex, PATH_TRACE_REGIR_EMPTY_UNSUPPORTED_DOMAIN);
        }

        const uint lightsPerCell = max((uint)ReGIRInfo2.x, 1u);
        const bool bothClassesPresent = analyticCount > 0u && emissiveCount > 0u;
        const uint analyticSlotCount = bothClassesPresent ? ((lightsPerCell + 1u) >> 1u) : (analyticCount > 0u ? lightsPerCell : 0u);
        const uint emissiveSlotCount = bothClassesPresent ? (lightsPerCell >> 1u) : (emissiveCount > 0u ? lightsPerCell : 0u);
        const bool buildAnalyticSlot = bothClassesPresent ? ((slotInCell & 1u) == 0u) : (analyticCount > 0u);
        // Split-mode class mass is the deterministic slot-layout mass, not a
        // random class draw inside this shader. Future consumers must select
        // slots with this same mass before using candidate.sourcePdf.
        const float analyticClassProbability = (float)analyticSlotCount / max((float)lightsPerCell, 1.0);
        const float emissiveClassProbability = (float)emissiveSlotCount / max((float)lightsPerCell, 1.0);
        if (buildAnalyticSlot)
        {
            if (deterministicSourceDiagnostic)
            {
                return PathTraceReGIRBuildDeterministicSourceCandidate(
                    cell, slotInCell, slotIndex,
                    PATH_TRACE_REGIR_LIGHT_CLASS_DOOM_ANALYTIC,
                    analyticRange.x, analyticCount, analyticClassProbability, 101u,
                    PATH_TRACE_REGIR_EMPTY_NO_ANALYTIC_LIGHTS);
            }
            return PathTraceReGIRBuildAnalyticCandidate(cell, slotInCell, slotIndex, analyticClassProbability);
        }
        if (deterministicSourceDiagnostic)
        {
            return PathTraceReGIRBuildDeterministicSourceCandidate(
                cell, slotInCell, slotIndex,
                PATH_TRACE_REGIR_LIGHT_CLASS_EMISSIVE_TRIANGLE,
                emissiveRange.x, emissiveCount, emissiveClassProbability, 103u,
                PATH_TRACE_REGIR_EMPTY_NO_EMISSIVE_LIGHTS);
        }
        return PathTraceReGIRBuildEmissiveCandidate(cell, slotInCell, slotIndex, emissiveClassProbability);
    }
    return PathTraceReGIREmptyCandidate(cell.localCellIndex, slotIndex, PATH_TRACE_REGIR_EMPTY_UNSUPPORTED_DOMAIN);
}

void PathTraceReGIRBuildCandidateCache(uint linearDispatchIndex, uint dispatchPixelCount)
{
    const uint enable = (uint)ReGIRInfo0.x;
    const uint mode = (uint)ReGIRInfo0.z;
    const uint centerMode = (uint)ReGIRInfo0.w;
    const float cellSize = ReGIRInfo1.x;
    const uint3 gridDimensions = uint3((uint)ReGIRInfo1.y, (uint)ReGIRInfo1.z, (uint)ReGIRInfo1.w);
    const uint lightsPerCell = max((uint)ReGIRInfo2.x, 1u);
    const uint cellCount = (uint)ReGIRInfo2.w;
    const uint candidateSlotCount = (uint)ReGIRInfo3.y;
    if (enable == 0u || mode == 0u || dispatchPixelCount == 0u || cellCount == 0u || candidateSlotCount == 0u)
    {
        return;
    }

    for (uint slotIndex = linearDispatchIndex; slotIndex < candidateSlotCount; slotIndex += dispatchPixelCount)
    {
        const uint cellIndex = slotIndex / lightsPerCell;
        const uint slotInCell = slotIndex - cellIndex * lightsPerCell;
        if (cellIndex >= cellCount)
        {
            return;
        }

        const PathTraceReGIRCellDebug cell = PathTraceReGIRMapLocalCellIndex(
            cellIndex,
            CameraOriginAndTMax.xyz,
            centerMode,
            mode,
            cellSize,
            gridDimensions,
            ReGIRInfo4.xyz);
        if (cell.valid != 0u)
        {
            ReGIRCandidateCache[slotIndex] = PathTraceReGIRBuildCandidateForCellSlot(cell, slotInCell, slotIndex);
        }
        else
        {
            ReGIRCandidateCache[slotIndex] = PathTraceReGIREmptyCandidate(cellIndex, slotIndex, PATH_TRACE_REGIR_EMPTY_OUT_OF_VOLUME);
        }
    }
}

PathTraceReGIRCandidateRecord PathTraceReGIRReadPrimaryCandidate(PathTraceReGIRCellDebug cell, uint lightsPerCell)
{
    const uint slotIndex = cell.localCellIndex * lightsPerCell;
    return ReGIRCandidateCache[slotIndex];
}

PathTraceReGIRCandidateRecord PathTraceReGIRReadFirstCandidateOfClass(PathTraceReGIRCellDebug cell, uint lightsPerCell, uint lightClass)
{
    [loop]
    for (uint slotInCell = 0u; slotInCell < lightsPerCell; ++slotInCell)
    {
        const PathTraceReGIRCandidateRecord candidate = ReGIRCandidateCache[cell.localCellIndex * lightsPerCell + slotInCell];
        if ((candidate.flags & PATH_TRACE_REGIR_CANDIDATE_VALID) != 0u && candidate.lightClass == lightClass)
        {
            return candidate;
        }
    }
    return PathTraceReGIREmptyCandidate(cell.localCellIndex, cell.localCellIndex * lightsPerCell, PATH_TRACE_REGIR_EMPTY_UNSUPPORTED_DOMAIN);
}

float4 PathTraceReGIREmptyReasonColor(uint reason)
{
    if (reason == PATH_TRACE_REGIR_EMPTY_OUT_OF_VOLUME)
    {
        return float4(1.0, 0.0, 0.82, 1.0);
    }
    if (reason == PATH_TRACE_REGIR_EMPTY_NO_ANALYTIC_LIGHTS)
    {
        return float4(0.10, 0.28, 1.0, 1.0);
    }
    if (reason == PATH_TRACE_REGIR_EMPTY_NON_ANALYTIC_DOMAIN)
    {
        return float4(0.58, 0.18, 1.0, 1.0);
    }
    if (reason == PATH_TRACE_REGIR_EMPTY_NO_EMISSIVE_LIGHTS)
    {
        return float4(1.0, 0.55, 0.02, 1.0);
    }
    if (reason == PATH_TRACE_REGIR_EMPTY_UNSUPPORTED_DOMAIN)
    {
        return float4(0.95, 0.05, 0.05, 1.0);
    }
    if (reason == PATH_TRACE_REGIR_EMPTY_NO_LOCAL_ANALYTIC_LIGHTS)
    {
        return float4(0.0, 0.80, 1.0, 1.0);
    }
    if (reason == PATH_TRACE_REGIR_EMPTY_NO_LOCAL_EMISSIVE_LIGHTS)
    {
        return float4(1.0, 0.95, 0.05, 1.0);
    }
    if (reason == PATH_TRACE_REGIR_EMPTY_NO_CURRENT_RLU_SOURCE)
    {
        return float4(0.80, 0.0, 1.0, 1.0);
    }
    return float4(0.02, 0.02, 0.02, 1.0);
}

struct PathTraceReGIRReplayResult
{
    uint lightInfoValid;
    uint sampleValid;
    float solidAnglePdf;
    float targetPdf;
    float reflectedLuminance;
    float contributionLuminance;
};

PathTraceReGIRPayload PathTraceReGIRInitPayload()
{
    PathTraceReGIRPayload payload = (PathTraceReGIRPayload)0;
    payload.normal = float3(0.0, 0.0, 1.0);
    payload.geometricNormal = float3(0.0, 0.0, 1.0);
    payload.vertexColor = float4(1.0, 1.0, 1.0, 1.0);
    payload.vertexColorAdd = float4(0.5, 0.5, 0.5, 0.5);
    payload.materialId = 0xffffffffu;
    payload.materialIndex = 0xffffffffu;
    payload.instanceId = 0xffffffffu;
    payload.primitiveIndex = 0xffffffffu;
    return payload;
}

RAB_Material PathTraceReGIRBuildMaterialFromPayload(PathTraceReGIRPayload payload)
{
    const PathTraceSmokeMaterial smokeMaterial = PathTraceReGIRLoadSmokeMaterial(payload.materialIndex);
    RAB_Material material = RAB_EmptyMaterial();
    material.materialId = payload.materialId;
    material.materialIndex = payload.materialIndex;
    material.flags = smokeMaterial.flags;
    material.alphaCutoff = smokeMaterial.alphaCutoff;
    material.diffuseAlbedo = saturate(smokeMaterial.debugAlbedo.rgb * payload.vertexColor.rgb);
    material.roughness = 1.0;
    material.specularF0 = float3(0.0, 0.0, 0.0);
    material.opacity = saturate(smokeMaterial.debugAlbedo.a * payload.vertexColor.a);
    material.emissiveTextureIndex = smokeMaterial.emissiveTextureIndex;

    const bool activeEmissiveStage = (payload.triangleClassAndFlags & RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF) == 0u;
    material.emissiveRadiance = ((smokeMaterial.flags & RT_SMOKE_MATERIAL_EMISSIVE) != 0u && activeEmissiveStage)
        ? max(smokeMaterial.emissiveColor.rgb, float3(0.0, 0.0, 0.0)) * max(ToyPathInfo.z, 0.0)
        : float3(0.0, 0.0, 0.0);
    return material;
}

RAB_Surface PathTraceReGIRBuildSurfaceFromPrimaryHit(PathTraceReGIRPayload payload, float3 rayOrigin, float3 rayDirection)
{
    RAB_Surface surface = RAB_EmptySurface();
    if (payload.value == 0u)
    {
        return surface;
    }

    const float3 hitPosition = rayOrigin + rayDirection * payload.hitT;
    const float3 viewDir = RAB_SafeNormalize(rayOrigin - hitPosition, -rayDirection);
    float3 geometryNormal = RAB_SafeNormalize(payload.geometricNormal, viewDir);
    float3 shadingNormal = RAB_SafeNormalize(payload.normal, geometryNormal);
    if (dot(geometryNormal, viewDir) < 0.0)
    {
        geometryNormal = -geometryNormal;
    }
    if (dot(shadingNormal, viewDir) < 0.0)
    {
        shadingNormal = -shadingNormal;
    }

    surface.valid = 1u;
    surface.worldPos = hitPosition;
    surface.linearDepth = payload.hitT;
    surface.geometryNormal = geometryNormal;
    surface.shadingNormal = shadingNormal;
    surface.viewDir = viewDir;
    surface.materialId = payload.materialId;
    surface.materialIndex = payload.materialIndex;
    surface.instanceId = payload.instanceId;
    surface.primitiveIndex = payload.primitiveIndex;
    surface.surfaceClass = payload.surfaceClass;
    surface.flags = payload.triangleClassAndFlags;
    surface.material = PathTraceReGIRBuildMaterialFromPayload(payload);
    return surface;
}

float2 PathTraceReGIRReplaySampleUv(PathTraceReGIRCandidateRecord candidate, PathTraceReGIRCellDebug cell)
{
    const uint seedA = PathTraceReGIRHashCell(cell.globalCoord + int3((int)candidate.slotIndex, (int)candidate.lightIndex, 17));
    const uint seedB = PathTraceReGIRHashCell(cell.globalCoord + int3((int)candidate.globalIdentity, (int)candidate.cellIndex, 29));
    return float2(PathTraceReGIRHashToUnitFloat(seedA), PathTraceReGIRHashToUnitFloat(seedB));
}

PathTraceReGIRReplayResult PathTraceReGIRReplayCandidateThroughRAB(PathTraceReGIRCandidateRecord candidate, PathTraceReGIRCellDebug cell, RAB_Surface surface)
{
    PathTraceReGIRReplayResult result = (PathTraceReGIRReplayResult)0;
    if ((candidate.flags & PATH_TRACE_REGIR_CANDIDATE_VALID) == 0u ||
        candidate.globalIdentity == 0xffffffffu ||
        candidate.sourcePdf <= 0.0 ||
        !RAB_IsSurfaceValid(surface))
    {
        return result;
    }

    RAB_LightInfo lightInfo = RAB_EmptyLightInfo();
    if (PathTraceReGIRUseCurrentRabLightUniverse())
    {
        if (candidate.lightClass == PATH_TRACE_REGIR_LIGHT_CLASS_DOOM_ANALYTIC &&
            !PathTraceReGIRRluAnalyticStableCacheable(candidate.globalIdentity))
        {
            return result;
        }
        lightInfo = RAB_LoadActiveRrxLightInfo(candidate.globalIdentity, false);
    }
    else
    {
        lightInfo = RAB_LoadLightInfo(candidate.globalIdentity, false);
    }
    if (!RAB_IsLightInfoValid(lightInfo))
    {
        return result;
    }
    result.lightInfoValid = 1u;

    const RAB_LightSample lightSample = RAB_SamplePolymorphicLight(lightInfo, surface, PathTraceReGIRReplaySampleUv(candidate, cell));
    if (!RAB_IsReplayableLightSample(lightSample))
    {
        return result;
    }
    result.sampleValid = 1u;
    result.solidAnglePdf = max(lightSample.solidAnglePdf, 0.0);
    result.targetPdf = max(RAB_GetLightSampleTargetPdfForSurface(lightSample, surface), 0.0);
    result.reflectedLuminance = PathTraceReGIRLuminance(RAB_GetReflectedBsdfRadianceForSurface(lightSample.position, lightSample.radiance, surface));
    result.contributionLuminance = result.solidAnglePdf > 0.0
        ? result.reflectedLuminance * max(candidate.invSourcePdf, 0.0) / max(result.solidAnglePdf, 1.0e-6)
        : 0.0;
    return result;
}

uint PathTraceReGIRStableSlotInCell(PathTraceReGIRCellDebug cell, uint lightsPerCell)
{
    return PathTraceReGIRHashCell(cell.globalCoord) % max(lightsPerCell, 1u);
}

float3 PathTraceReGIRHeat(float value, float exposure)
{
    const float x = saturate(value * exposure);
    return saturate(float3(
        smoothstep(0.45, 0.95, x),
        smoothstep(0.08, 0.65, x) * (1.0 - smoothstep(0.82, 1.0, x) * 0.35),
        1.0 - smoothstep(0.18, 0.72, x)));
}

float4 PathTraceReGIRStableSlotColor(PathTraceReGIRCellDebug cell, uint lightsPerCell, bool boundary)
{
    const uint slotInCell = PathTraceReGIRStableSlotInCell(cell, lightsPerCell);
    const PathTraceReGIRCandidateRecord slotCandidate = ReGIRCandidateCache[cell.localCellIndex * lightsPerCell + slotInCell];
    if ((slotCandidate.flags & PATH_TRACE_REGIR_CANDIDATE_VALID) == 0u)
    {
        return float4(1.0, 0.42, 0.0, 1.0);
    }

    const float slotBand = ((slotInCell & 15u) + 1u) / 16.0;
    const float3 identityColor = PathTraceReGIRCellColor(int3(
        (int)slotCandidate.globalIdentity,
        (int)(slotCandidate.globalIdentity >> 8u),
        (int)(slotCandidate.slotIndex >> 4u)));
    const float3 classTint = slotCandidate.lightClass == PATH_TRACE_REGIR_LIGHT_CLASS_DOOM_ANALYTIC
        ? float3(0.20, 0.50, 1.0)
        : float3(1.0, 0.55, 0.15);
    const float3 color = lerp(identityColor, classTint, 0.28) * (0.55 + 0.45 * slotBand);
    return float4(boundary ? float3(1.0, 1.0, 1.0) : saturate(color), 1.0);
}

float4 PathTraceReGIRCellMeanContributionColor(PathTraceReGIRCellDebug cell, uint lightsPerCell, RAB_Surface primarySurface, bool boundary)
{
    if (boundary)
    {
        return float4(1.0, 1.0, 1.0, 1.0);
    }
    if (!RAB_IsSurfaceValid(primarySurface))
    {
        return float4(0.55, 0.0, 0.0, 1.0);
    }

    float sumContribution = 0.0;
    uint validSlots = 0u;
    uint replayableSlots = 0u;
    [loop]
    for (uint slotInCell = 0u; slotInCell < lightsPerCell; ++slotInCell)
    {
        const PathTraceReGIRCandidateRecord slotCandidate = ReGIRCandidateCache[cell.localCellIndex * lightsPerCell + slotInCell];
        if ((slotCandidate.flags & PATH_TRACE_REGIR_CANDIDATE_VALID) == 0u)
        {
            continue;
        }

        ++validSlots;
        const PathTraceReGIRReplayResult replay = PathTraceReGIRReplayCandidateThroughRAB(slotCandidate, cell, primarySurface);
        if (replay.sampleValid != 0u && replay.solidAnglePdf > 0.0)
        {
            ++replayableSlots;
            sumContribution += replay.contributionLuminance;
        }
    }

    if (validSlots == 0u)
    {
        return float4(1.0, 0.42, 0.0, 1.0);
    }

    const float meanContribution = sumContribution / max((float)validSlots, 1.0);
    const float replayBand = (float)replayableSlots / max((float)validSlots, 1.0);
    const float3 heat = PathTraceReGIRHeat(meanContribution, 0.25);
    const float3 color = lerp(float3(0.30, 0.0, 0.0), heat, replayBand);
    return float4(color, 1.0);
}

float4 PathTraceReGIRCandidateDebugColor(float3 worldPosition, RAB_Surface primarySurface)
{
    const uint enable = (uint)ReGIRInfo0.x;
    const uint view = (uint)ReGIRInfo0.y;
    const uint mode = (uint)ReGIRInfo0.z;
    const uint centerMode = (uint)ReGIRInfo0.w;
    const float cellSize = ReGIRInfo1.x;
    const uint3 gridDimensions = uint3((uint)ReGIRInfo1.y, (uint)ReGIRInfo1.z, (uint)ReGIRInfo1.w);
    const uint lightsPerCell = max((uint)ReGIRInfo2.x, 1u);
    const uint lightDomain = (uint)ReGIRInfo2.z;
    const uint cellCount = (uint)ReGIRInfo2.w;
    if (enable == 0u || mode == 0u)
    {
        return float4(0.25, 0.0, 0.0, 1.0);
    }
    if (PathTraceReGIRSourceViewRequiresCurrentRlu() && !PathTraceReGIRUseCurrentRabLightUniverse())
    {
        return PathTraceReGIREmptyReasonColor(PATH_TRACE_REGIR_EMPTY_NO_CURRENT_RLU_SOURCE);
    }

    const PathTraceReGIRCellDebug cell = PathTraceReGIRMapWorldPositionToCell(
        worldPosition,
        CameraOriginAndTMax.xyz,
        centerMode,
        mode,
        cellSize,
        gridDimensions,
        ReGIRInfo4.xyz);
    if (cell.valid == 0u || cell.localCellIndex >= cellCount)
    {
        return float4(1.0, 0.0, 0.82, 1.0);
    }

    const PathTraceReGIRCandidateRecord candidate = PathTraceReGIRReadPrimaryCandidate(cell, lightsPerCell);
    const bool candidateValid = (candidate.flags & PATH_TRACE_REGIR_CANDIDATE_VALID) != 0u;

    const bool boundary = cell.boundary < 0.035;
    if (view == 4u)
    {
        uint validSlots = 0u;
        [loop]
        for (uint slotInCell = 0u; slotInCell < lightsPerCell; ++slotInCell)
        {
            const PathTraceReGIRCandidateRecord slotCandidate = ReGIRCandidateCache[cell.localCellIndex * lightsPerCell + slotInCell];
            validSlots += (slotCandidate.flags & PATH_TRACE_REGIR_CANDIDATE_VALID) != 0u ? 1u : 0u;
        }
        const float occupancy = saturate((float)validSlots / max((float)lightsPerCell, 1.0));
        return float4(boundary ? float3(1.0, 1.0, 1.0) : float3(0.05, occupancy, 0.18), 1.0);
    }
    if (view == 8u)
    {
        uint validSlots = 0u;
        uint firstReason = 0u;
        [loop]
        for (uint slotInCell = 0u; slotInCell < lightsPerCell; ++slotInCell)
        {
            const PathTraceReGIRCandidateRecord slotCandidate = ReGIRCandidateCache[cell.localCellIndex * lightsPerCell + slotInCell];
            if ((slotCandidate.flags & PATH_TRACE_REGIR_CANDIDATE_VALID) != 0u)
            {
                ++validSlots;
            }
            else if (firstReason == 0u)
            {
                firstReason = (slotCandidate.flags >> 16u) & 0xffffu;
            }
        }
        if (validSlots > 0u)
        {
            const float occupancy = saturate((float)validSlots / max((float)lightsPerCell, 1.0));
            return float4(boundary ? float3(1.0, 1.0, 1.0) : float3(0.04, 0.35 + 0.65 * occupancy, 0.10), 1.0);
        }
        return boundary ? float4(1.0, 1.0, 1.0, 1.0) : PathTraceReGIREmptyReasonColor(firstReason);
    }
    if (view == 7u)
    {
        PathTraceReGIRCandidateRecord pdfCandidate = candidate;
        if (lightDomain == 2u)
        {
            const uint slotInCell = PathTraceReGIRHashCell(cell.globalCoord) % lightsPerCell;
            pdfCandidate = ReGIRCandidateCache[cell.localCellIndex * lightsPerCell + slotInCell];
        }
        if ((pdfCandidate.flags & PATH_TRACE_REGIR_CANDIDATE_VALID) == 0u)
        {
            return boundary ? float4(1.0, 1.0, 1.0, 1.0) : float4(1.0, 0.42, 0.0, 1.0);
        }

        const float pdfBand = saturate(pdfCandidate.sourcePdf * 128.0);
        const float invBand = saturate(pdfCandidate.invSourcePdf / 512.0);
        const PathTraceReGIRReplayResult replay = PathTraceReGIRReplayCandidateThroughRAB(pdfCandidate, cell, primarySurface);
        const float replayBand = replay.targetPdf > 0.0 ? 1.0 : (replay.sampleValid != 0u ? 0.66 : (replay.lightInfoValid != 0u ? 0.33 : 0.0));
        return float4(pdfBand, invBand, pdfCandidate.sourcePdf > 0.0 ? replayBand : 0.0, 1.0);
    }
    if (view == 9u)
    {
        return PathTraceReGIRStableSlotColor(cell, lightsPerCell, boundary);
    }
    if (view == 10u)
    {
        return PathTraceReGIRCellMeanContributionColor(cell, lightsPerCell, primarySurface, boundary);
    }
    if (!candidateValid)
    {
        return boundary ? float4(1.0, 1.0, 1.0, 1.0) : float4(1.0, 0.42, 0.0, 1.0);
    }
    if (view == 5u)
    {
        const PathTraceReGIRCandidateRecord analyticCandidate = PathTraceReGIRReadFirstCandidateOfClass(cell, lightsPerCell, PATH_TRACE_REGIR_LIGHT_CLASS_DOOM_ANALYTIC);
        if ((analyticCandidate.flags & PATH_TRACE_REGIR_CANDIDATE_VALID) == 0u)
        {
            return boundary ? float4(1.0, 1.0, 1.0, 1.0) : float4(0.75, 0.0, 0.0, 1.0);
        }
        uint identityHash = analyticCandidate.globalIdentity;
        if (PathTraceReGIRUseCurrentRabLightUniverse())
        {
            const PathTraceUnifiedLightRecord record = PathTraceRestirLightManagerCurrentPayload[analyticCandidate.globalIdentity];
            identityHash = record.identityA ^ (record.identityB * 1664525u) ^ analyticCandidate.globalIdentity;
        }
        else
        {
            const PathTraceDoomAnalyticLightCandidate analyticLight = DoomAnalyticLights[analyticCandidate.lightIndex];
            identityHash = analyticLight.renderLightIndex != 0xffffffffu ? analyticLight.renderLightIndex : analyticCandidate.globalIdentity;
        }
        const float3 color = PathTraceReGIRCellColor(int3(identityHash, identityHash >> 8u, identityHash >> 16u));
        return float4(boundary ? float3(1.0, 1.0, 1.0) : color, 1.0);
    }
    if (view == 6u)
    {
        const PathTraceReGIRCandidateRecord emissiveCandidate = PathTraceReGIRReadFirstCandidateOfClass(cell, lightsPerCell, PATH_TRACE_REGIR_LIGHT_CLASS_EMISSIVE_TRIANGLE);
        if ((emissiveCandidate.flags & PATH_TRACE_REGIR_CANDIDATE_VALID) == 0u)
        {
            return boundary ? float4(1.0, 1.0, 1.0, 1.0) : float4(0.75, 0.0, 0.0, 1.0);
        }
        int3 identityColor;
        if (PathTraceReGIRUseCurrentRabLightUniverse())
        {
            const PathTraceUnifiedLightRecord record = PathTraceRestirLightManagerCurrentPayload[emissiveCandidate.globalIdentity];
            identityColor = int3(record.identityA, record.identityB, emissiveCandidate.globalIdentity);
        }
        else
        {
            const PathTraceSmokeEmissiveTriangle emissiveTriangle = SmokeEmissiveTriangles[emissiveCandidate.lightIndex];
            identityColor = int3(emissiveTriangle.identityHashLo, emissiveTriangle.identityHashHi, emissiveCandidate.globalIdentity);
        }
        const float3 color = PathTraceReGIRCellColor(identityColor);
        return float4(boundary ? float3(1.0, 1.0, 1.0) : color, 1.0);
    }
    return PathTraceReGIRDebugColor(worldPosition);
}

[shader("raygeneration")]
void RayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy + PathTraceReGIRDispatchTileOffset();
    const uint2 dimensions = PathTraceReGIRFullOutputSize();
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    const uint2 dispatchDimensions = DispatchRaysDimensions().xy;
    const uint linearDispatchIndex = DispatchRaysIndex().x + DispatchRaysIndex().y * dispatchDimensions.x;
    const uint dispatchPixelCount = dispatchDimensions.x * dispatchDimensions.y;
    if ((uint)ReGIRInfo3.x == 1u)
    {
        PathTraceReGIRBuildCandidateCache(linearDispatchIndex, dispatchPixelCount);
        return;
    }

    const float2 uv = (float2(pixel) + 0.5) / float2(dimensions);
    const float2 ndc = uv * 2.0 - 1.0;
    RayDesc ray;
    ray.Origin = CameraOriginAndTMax.xyz;
    ray.Direction = normalize(
        CameraForwardAndTanX.xyz +
        CameraLeftAndTanY.xyz * (-ndc.x * CameraForwardAndTanX.w) +
        CameraUpAndDebugMode.xyz * (-ndc.y * CameraLeftAndTanY.w));
    ray.TMin = 0.1;
    ray.TMax = CameraOriginAndTMax.w;

    PathTraceReGIRPayload payload = PathTraceReGIRInitPayload();
    TraceRay(SmokeScene, RAY_FLAG_NONE, 0xff, 0, 1, 0, ray, payload);
    if (payload.value == 0u)
    {
        SmokeOutput[pixel] = PathTraceReGIRMissColor((uint)ReGIRInfo0.y);
        return;
    }

    const float3 worldPosition = ray.Origin + ray.Direction * payload.hitT;
    const uint view = (uint)ReGIRInfo0.y;
    RAB_Surface primarySurface = RAB_EmptySurface();
    if (view == 7u || view == 10u)
    {
        primarySurface = PathTraceReGIRBuildSurfaceFromPrimaryHit(payload, ray.Origin, ray.Direction);
    }
    SmokeOutput[pixel] = (view >= 4u && view <= 10u)
        ? PathTraceReGIRCandidateDebugColor(worldPosition, primarySurface)
        : PathTraceReGIRDebugColor(worldPosition);
}

[shader("miss")]
void Miss(inout PathTraceReGIRPayload payload)
{
    payload.value = 0u;
}

[shader("miss")]
void ShadowMiss(inout PathTraceReGIRShadowPayload payload)
{
    payload.hit = 0u;
}

[shader("anyhit")]
void AnyHit(inout PathTraceReGIRPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
}

[shader("anyhit")]
void ShadowAnyHit(inout PathTraceReGIRShadowPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    payload.hit = 1u;
}

[shader("closesthit")]
void ClosestHit(inout PathTraceReGIRPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    payload.value = 1u;
    payload.hitT = RayTCurrent();
    const uint view = (uint)ReGIRInfo0.y;
    if (view != 7u && view != 10u)
    {
        return;
    }

    const uint instanceId = InstanceID();
    const uint primitiveIndex = PrimitiveIndex();
    payload.instanceId = instanceId;
    payload.primitiveIndex = primitiveIndex;

    const float3 barycentrics = float3(
        1.0 - attributes.barycentrics.x - attributes.barycentrics.y,
        attributes.barycentrics.x,
        attributes.barycentrics.y);

    if (instanceId >= 2u)
    {
        const uint routeInstanceIndex = instanceId - 2u;
        if (routeInstanceIndex >= PathTraceReGIRRigidRouteInstanceCount())
        {
            return;
        }

        const PathTraceRigidRouteInstance routeInstance = SmokeRigidRouteInstances[routeInstanceIndex];
        const uint routeIndexOffset = routeInstance.indexOffset + primitiveIndex * 3u;
        if (primitiveIndex >= routeInstance.triangleCount ||
            routeInstance.triangleOffset + primitiveIndex >= PathTraceReGIRRigidRouteTriangleCount() ||
            routeIndexOffset > PathTraceReGIRRigidRouteIndexCount() ||
            routeIndexOffset + 2u >= PathTraceReGIRRigidRouteIndexCount())
        {
            return;
        }

        const uint i0 = SmokeRigidRouteIndices[routeIndexOffset + 0u];
        const uint i1 = SmokeRigidRouteIndices[routeIndexOffset + 1u];
        const uint i2 = SmokeRigidRouteIndices[routeIndexOffset + 2u];
        if (i0 >= routeInstance.vertexCount || i1 >= routeInstance.vertexCount || i2 >= routeInstance.vertexCount ||
            routeInstance.vertexOffset + i0 >= PathTraceReGIRRigidRouteVertexCount() ||
            routeInstance.vertexOffset + i1 >= PathTraceReGIRRigidRouteVertexCount() ||
            routeInstance.vertexOffset + i2 >= PathTraceReGIRRigidRouteVertexCount())
        {
            return;
        }

        const PathTraceSmokeVertex v0 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i0];
        const PathTraceSmokeVertex v1 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i1];
        const PathTraceSmokeVertex v2 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i2];
        const float3 p0 = v0.position.xyz;
        const float3 p1 = v1.position.xyz;
        const float3 p2 = v2.position.xyz;
        const float3 n0 = v0.normal.xyz;
        const float3 n1 = v1.normal.xyz;
        const float3 n2 = v2.normal.xyz;
        const float3 worldRayFallback = PathTraceReGIRSafeNormalize(-WorldRayDirection(), float3(0.0, 0.0, 1.0));
        const float3 objectGeometricNormal = PathTraceReGIRSafeNormalize(cross(p1 - p0, p2 - p0), worldRayFallback);
        payload.geometricNormal = PathTraceReGIRTransformObjectNormalToWorld(objectGeometricNormal, worldRayFallback);
        const float3 objectInterpolatedNormal = PathTraceReGIRSafeNormalize(n0 * barycentrics.x + n1 * barycentrics.y + n2 * barycentrics.z, objectGeometricNormal);
        payload.normal = PathTraceReGIRTransformObjectNormalToWorld(objectInterpolatedNormal, payload.geometricNormal);
        payload.texCoord = v0.texCoord.xy * barycentrics.x + v1.texCoord.xy * barycentrics.y + v2.texCoord.xy * barycentrics.z;
        payload.vertexColor = saturate(v0.color * barycentrics.x + v1.color * barycentrics.y + v2.color * barycentrics.z);
        payload.vertexColorAdd = saturate(v0.color2 * barycentrics.x + v1.color2 * barycentrics.y + v2.color2 * barycentrics.z);
        payload.surfaceClass = RT_SMOKE_SURFACE_CLASS_RIGID_ENTITY;
        payload.triangleClassAndFlags = RT_SMOKE_SURFACE_CLASS_RIGID_ENTITY;
        payload.materialId = SmokeRigidRouteTriangleMaterials[routeInstance.triangleOffset + primitiveIndex];
        payload.materialIndex = PathTraceReGIRLoadTriangleMaterialIndex(instanceId, primitiveIndex);
        return;
    }

    if (!PathTraceReGIRTriangleRangeValid(instanceId, primitiveIndex))
    {
        return;
    }

    const uint indexOffset = primitiveIndex * 3u;
    const uint i0 = instanceId == 0u ? SmokeStaticIndices[indexOffset + 0u] : SmokeDynamicIndices[indexOffset + 0u];
    const uint i1 = instanceId == 0u ? SmokeStaticIndices[indexOffset + 1u] : SmokeDynamicIndices[indexOffset + 1u];
    const uint i2 = instanceId == 0u ? SmokeStaticIndices[indexOffset + 2u] : SmokeDynamicIndices[indexOffset + 2u];
    const uint vertexCount = instanceId == 0u ? PathTraceReGIRStaticVertexCount() : PathTraceReGIRDynamicVertexCount();
    if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
    {
        return;
    }

    PathTraceSmokeVertex v0;
    PathTraceSmokeVertex v1;
    PathTraceSmokeVertex v2;
    if (instanceId == 0u)
    {
        v0 = SmokeStaticVertices[i0];
        v1 = SmokeStaticVertices[i1];
        v2 = SmokeStaticVertices[i2];
    }
    else
    {
        v0 = SmokeDynamicVertices[i0];
        v1 = SmokeDynamicVertices[i1];
        v2 = SmokeDynamicVertices[i2];
    }
    const float3 p0 = v0.position.xyz;
    const float3 p1 = v1.position.xyz;
    const float3 p2 = v2.position.xyz;
    const float3 n0 = v0.normal.xyz;
    const float3 n1 = v1.normal.xyz;
    const float3 n2 = v2.normal.xyz;
    const uint triangleClassAndFlags = PathTraceReGIRLoadTriangleClassAndFlags(instanceId, primitiveIndex);
    const bool forceGeometricNormal = (triangleClassAndFlags & RT_SMOKE_TRIANGLE_FORCE_GEOMETRIC_NORMAL) != 0u;
    payload.geometricNormal = PathTraceReGIRSafeNormalize(cross(p1 - p0, p2 - p0), float3(0.0, 0.0, 1.0));
    const float3 interpolatedNormal = PathTraceReGIRSafeNormalize(n0 * barycentrics.x + n1 * barycentrics.y + n2 * barycentrics.z, payload.geometricNormal);
    payload.normal = forceGeometricNormal ? payload.geometricNormal : interpolatedNormal;
    payload.texCoord = v0.texCoord.xy * barycentrics.x + v1.texCoord.xy * barycentrics.y + v2.texCoord.xy * barycentrics.z;
    payload.vertexColor = saturate(v0.color * barycentrics.x + v1.color * barycentrics.y + v2.color * barycentrics.z);
    payload.vertexColorAdd = saturate(v0.color2 * barycentrics.x + v1.color2 * barycentrics.y + v2.color2 * barycentrics.z);
    payload.surfaceClass = triangleClassAndFlags & RT_SMOKE_TRIANGLE_CLASS_MASK;
    payload.translucentSubtype = (triangleClassAndFlags & RT_SMOKE_TRANSLUCENT_SUBTYPE_MASK) >> RT_SMOKE_TRANSLUCENT_SUBTYPE_SHIFT;
    payload.triangleClassAndFlags = triangleClassAndFlags;
    payload.materialId = PathTraceReGIRLoadTriangleMaterialId(instanceId, primitiveIndex);
    payload.materialIndex = PathTraceReGIRLoadTriangleMaterialIndex(instanceId, primitiveIndex);
}

[shader("closesthit")]
void ShadowClosestHit(inout PathTraceReGIRShadowPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    payload.hit = 1u;
}
