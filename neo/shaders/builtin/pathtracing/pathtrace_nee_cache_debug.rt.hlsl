#include "../../vulkan.hlsli"
#define RB_PT_RIGID_HIT_ROUTE_INSTANCE_COUNT() \
    PathTraceNeeCacheRigidRouteInstanceCount()
#include "PathTraceSkinnedHitRoute.hlsli"
#include "RtxdiBridge/RAB_UnifiedLightRecord.hlsli"
#include "RtxdiBridge/RAB_NeeCache.hlsli"

static const float RTXDI_PI = 3.14159265358979323846;

struct PathTraceNeeCachePayload
{
    uint hit;
    float hitT;
    float3 normal;
    float3 geometricNormal;
    uint instanceId;
    uint primitiveIndex;
};

struct PathTraceNeeCacheShadowPayload
{
    uint hit;
};

struct PathTraceNeeCacheProviderResult
{
    uint selectedDenseRluIndex;
    uint sourceLabel;
    uint fallbackReason;
    uint cellIndex;
    uint candidateSlot;
    uint flags;
    float sourcePdf;
    float invSourcePdf;
    float mixtureProbability;
    float reserved0;
    uint reserved1;
    uint reserved2;
};

struct PathTraceNeeCacheCellRecord
{
    uint flags;
    uint hash;
    uint taskOffset;
    uint taskCount;
    uint candidateOffset;
    uint candidateCount;
    uint reserved0;
    uint reserved1;
};

struct PathTraceNeeCacheTaskRecord
{
    uint denseRluIndex;
    uint taskClass;
    float accumulatedValue;
    float decayState;
    uint cellIndex;
    uint flags;
    uint reserved0;
    uint reserved1;
};

struct PathTraceNeeCacheCandidateRecord
{
    uint denseRluIndex;
    uint lightClass;
    float sourcePdf;
    float invSourcePdf;
    float candidateWeight;
    uint cellIndex;
    uint candidateSlot;
    uint flags;
};

static const uint PATH_TRACE_NEE_CACHE_SOURCE_NONE = 0u;
static const uint PATH_TRACE_NEE_CACHE_SOURCE_CACHE_ANALYTIC = 1u;
static const uint PATH_TRACE_NEE_CACHE_SOURCE_CACHE_EMISSIVE = 2u;
static const uint PATH_TRACE_NEE_CACHE_SOURCE_FALLBACK_FULL_RLU = 3u;
static const uint PATH_TRACE_NEE_CACHE_SOURCE_FALLBACK_TYPED_RLU = 4u;

static const uint PATH_TRACE_NEE_CACHE_FALLBACK_NONE = 0u;
static const uint PATH_TRACE_NEE_CACHE_FALLBACK_DISABLED = 1u;
static const uint PATH_TRACE_NEE_CACHE_FALLBACK_NO_RLU = 2u;
static const uint PATH_TRACE_NEE_CACHE_FALLBACK_EMPTY_CELL = 3u;
static const uint PATH_TRACE_NEE_CACHE_FALLBACK_INVALID_CANDIDATE = 4u;
static const uint PATH_TRACE_NEE_CACHE_FALLBACK_ZERO_SOURCE_PDF = 5u;
static const uint PATH_TRACE_NEE_CACHE_FALLBACK_RAB_REPLAY_FAILED = 6u;
static const uint PATH_TRACE_NEE_CACHE_FALLBACK_CACHE_ONLY_DIAGNOSTIC = 7u;

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

struct PathTraceEmissiveLightRemap
{
    int previousToCurrentIndex;
    int currentToPreviousIndex;
    uint flags;
    uint padding0;
};

#ifndef PATH_TRACE_NEE_CACHE_COMPUTE_UPDATE
RaytracingAccelerationStructure SmokeScene : register(t0);
VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> SmokeOutput : register(u1);
StructuredBuffer<PathTraceSmokeVertex> SmokeStaticVertices : register(t3);
StructuredBuffer<uint> SmokeStaticIndices : register(t4);
StructuredBuffer<PathTraceSmokeVertex> SmokeDynamicVertices : register(t6);
StructuredBuffer<PathTraceSmokeVertex> SmokeSkinnedCurrentVertices : register(t29);
StructuredBuffer<uint> SmokeDynamicIndices : register(t7);
StructuredBuffer<PathTraceSmokeVertex> SmokeRigidRouteVertices : register(t22);
StructuredBuffer<uint> SmokeRigidRouteIndices : register(t23);
StructuredBuffer<PathTraceRigidRouteInstance> SmokeRigidRouteInstances : register(t26);
#endif
StructuredBuffer<PathTraceDoomAnalyticLightCandidate> DoomAnalyticLights : register(t27);
StructuredBuffer<PathTraceDoomAnalyticLightCandidateIdentity> DoomAnalyticCurrentIdentities : register(t42);
StructuredBuffer<PathTraceDoomAnalyticLightCandidateIdentity> DoomAnalyticPreviousIdentities : register(t43);
StructuredBuffer<PathTraceDoomAnalyticLightRemap> DoomAnalyticRemap : register(t44);
StructuredBuffer<PathTraceDoomAnalyticLightCandidate> DoomAnalyticPreviousLights : register(t45);
StructuredBuffer<PathTraceSmokeEmissiveTriangle> SmokeEmissiveTriangles : register(t16);
StructuredBuffer<PathTraceSmokeEmissiveTriangle> SmokePreviousEmissiveTriangles : register(t57);
StructuredBuffer<PathTraceEmissiveLightRemap> SmokeEmissiveRemap : register(t58);
StructuredBuffer<PathTraceUnifiedLightRecord> PathTraceUnifiedLights : register(t59);
StructuredBuffer<PathTraceUnifiedLightRecord> PathTraceUnifiedPreviousLights : register(t60);
StructuredBuffer<uint> PathTraceUnifiedLightRemap : register(t61);
StructuredBuffer<uint> PathTraceRestirLightManagerCurrentToPrevious : register(t64);
StructuredBuffer<uint> PathTraceRestirLightManagerPreviousToCurrent : register(t65);
StructuredBuffer<PathTraceUnifiedLightRecord> PathTraceRestirLightManagerCurrentPayload : register(t66);
StructuredBuffer<PathTraceUnifiedLightRecord> PathTraceRestirLightManagerPreviousPayload : register(t67);
RWStructuredBuffer<PathTraceNeeCacheProviderResult> PathTraceNeeCacheProviderResults : register(u74);
RWStructuredBuffer<PathTraceNeeCacheCellRecord> PathTraceNeeCacheCells : register(u75);
RWStructuredBuffer<PathTraceNeeCacheTaskRecord> PathTraceNeeCacheTasks : register(u76);
RWStructuredBuffer<PathTraceNeeCacheCandidateRecord> PathTraceNeeCacheCandidates : register(u77);

static const uint PATH_TRACE_NEE_CACHE_DOOM_ANALYTIC_IDENTITY_VALID = 0x00000001u;
static const uint PATH_TRACE_NEE_CACHE_DOOM_ANALYTIC_IDENTITY_SAMPLEABLE = 0x00000002u;

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
    float4 NeeCacheInfo0 : packoffset(c115);
    float4 NeeCacheInfo1 : packoffset(c116);
    float4 NeeCacheInfo2 : packoffset(c117);
    float4 NeeCacheInfo3 : packoffset(c118);
};

static const uint RT_SMOKE_EMISSIVE_TRIANGLE_HISTORY_DYNAMIC = 0x00020000u;
static const uint RT_SMOKE_TEXTURE_FLAG_RESERVOIR_TWO_SIDED_EMISSIVES = 0x00000040u;
static const uint RT_PT_SAFETY_DISABLE_ANALYTIC_LIGHT_LOOP = 0x00000004u;
static const uint RT_PT_SAFETY_DISABLE_EMISSIVE_TRIANGLE_SAMPLING = 0x00000008u;

bool PathTraceSafetyDisabled(uint bit)
{
    return (((uint)SafetyInfo.x) & bit) != 0u;
}

#define RB_PT_ENABLE_RESTIR_LIGHT_MANAGER_RAB 1
#include "RtxdiBridge/RAB_LightInfoRuntime.hlsli"
#include "RtxdiBridge/RAB_LightLoadRuntime.hlsli"
#include "RtxdiBridge/RAB_LightSamplingCore.hlsli"

float PathTraceNeeCacheDebugLightSampleTargetPdfForSurface(RAB_LightSample lightSample, RAB_Surface surface)
{
    if (!RAB_IsReplayableLightSample(lightSample) || !RAB_IsSurfaceValid(surface))
    {
        return 0.0;
    }

    float3 lightDir;
    float lightDistance;
    RAB_GetLightDirDistance(surface, lightSample, lightDir, lightDistance);
    const float ndotl = saturate(dot(RAB_GetSurfaceNormal(surface), lightDir));
    const float3 reflected = GetDiffuseAlbedo(surface.material) * (1.0 / RTXDI_PI) * lightSample.radiance * ndotl;
    return RAB_Luminance(reflected) / max(lightSample.solidAnglePdf, 1.0e-6);
}

float3 PathTraceNeeCacheDebugReflectedRadianceForSurface(float3 samplePosition, float3 radiance, RAB_Surface surface)
{
    if (!RAB_IsSurfaceValid(surface))
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float3 toLight = samplePosition - RAB_GetSurfaceWorldPos(surface);
    const float distanceSquared = max(dot(toLight, toLight), 1.0e-6);
    const float3 lightDir = toLight * rsqrt(distanceSquared);
    const float ndotl = saturate(dot(RAB_GetSurfaceNormal(surface), lightDir));
    return GetDiffuseAlbedo(surface.material) * (1.0 / RTXDI_PI) * max(radiance, float3(0.0, 0.0, 0.0)) * ndotl;
}

float3 PathTraceNeeCacheSafeNormalize(float3 value, float3 fallback)
{
    const float len2 = dot(value, value);
    return len2 > 1.0e-20 ? value * rsqrt(len2) : fallback;
}

uint PathTraceNeeCacheStaticVertexCount() { return (uint)max(GeometryInfo0.x, 0.0); }
uint PathTraceNeeCacheStaticIndexCount() { return (uint)max(GeometryInfo0.y, 0.0); }
uint PathTraceNeeCacheStaticTriangleCount() { return (uint)max(GeometryInfo0.z, 0.0); }
uint PathTraceNeeCacheDynamicVertexCount() { return (uint)max(GeometryInfo0.w, 0.0); }
uint PathTraceNeeCacheDynamicIndexCount() { return (uint)max(GeometryInfo1.x, 0.0); }
uint PathTraceNeeCacheDynamicTriangleCount() { return (uint)max(GeometryInfo1.y, 0.0); }
uint PathTraceNeeCacheRigidRouteVertexCount() { return (uint)max(GeometryInfo1.z, 0.0); }
uint PathTraceNeeCacheRigidRouteIndexCount() { return (uint)max(GeometryInfo1.w, 0.0); }
uint PathTraceNeeCacheRigidRouteTriangleCount() { return (uint)max(GeometryInfo2.x, 0.0); }
uint PathTraceNeeCacheRigidRouteInstanceCount() { return (uint)max(GeometryInfo2.y, 0.0); }

uint2 PathTraceNeeCacheDispatchTileOffset()
{
    return uint2(max(DispatchTileInfo.x, 0.0), max(DispatchTileInfo.y, 0.0));
}

uint2 PathTraceNeeCacheFullOutputSize()
{
    const uint width = (uint)max(DispatchTileInfo.z, 0.0);
    const uint height = (uint)max(DispatchTileInfo.w, 0.0);
#ifdef PATH_TRACE_NEE_CACHE_COMPUTE_UPDATE
    return uint2(width, height);
#else
    return (width > 0u && height > 0u) ? uint2(width, height) : DispatchRaysDimensions().xy;
#endif
}

float4 PathTraceNeeCacheMissColor(uint view)
{
    if (view == 1u)
    {
        return float4(0.02, 0.06, 0.16, 1.0);
    }
    if (view == 4u)
    {
        return float4(0.01, 0.01, 0.03, 1.0);
    }
    if (view == 5u)
    {
        return float4(0.02, 0.01, 0.01, 1.0);
    }
    if (view == 3u)
    {
        return float4(0.06, 0.04, 0.02, 1.0);
    }
    return float4(0.0, 0.0, 0.0, 1.0);
}

uint PathTraceNeeCacheTaskSlotCount()
{
    return max((uint)NeeCacheInfo2.x, 1u);
}

uint PathTraceNeeCacheCandidateSlotCount()
{
    return max((uint)NeeCacheInfo1.w, 1u);
}

uint PathTraceNeeCacheFrameIndex()
{
    return (uint)NeeCacheInfo3.w;
}

float PathTraceNeeCacheFallbackProbability()
{
    return saturate(NeeCacheInfo2.y);
}

float PathTraceNeeCacheCacheProbability()
{
    return 1.0 - PathTraceNeeCacheFallbackProbability();
}

uint PathTraceNeeCacheSourceDomain()
{
    return clamp((uint)max(NeeCacheInfo0.w, 0.0), 0u, 3u);
}

bool PathTraceNeeCacheSourceDomainAllowsEmissive()
{
    const uint sourceDomain = PathTraceNeeCacheSourceDomain();
    return sourceDomain == 0u || sourceDomain == 1u || sourceDomain == 3u;
}

bool PathTraceNeeCacheSourceDomainAllowsAnalytic()
{
    const uint sourceDomain = PathTraceNeeCacheSourceDomain();
    return sourceDomain == 0u || sourceDomain == 2u || sourceDomain == 3u;
}

float PathTraceNeeCacheTypedClassProbability(bool analytic, uint emissiveRangeCount, uint analyticRangeCount)
{
    const uint sourceDomain = PathTraceNeeCacheSourceDomain();
    if (sourceDomain != 3u)
    {
        return 1.0;
    }
    if (emissiveRangeCount == 0u || analyticRangeCount == 0u)
    {
        return 1.0;
    }
    return 0.5;
}

uint PathTraceNeeCacheDebugDispatchMode()
{
    return (uint)NeeCacheInfo3.x;
}

uint PathTraceNeeCacheCurrentRluCount()
{
    return (uint)max(NeeCacheInfo3.z, 0.0);
}

uint PathTraceNeeCacheStableDoomAnalyticCount()
{
    return (uint)max(RestirLightManagerControlInfo.z, 0.0);
}

bool PathTraceNeeCacheCurrentRluReady()
{
    return NeeCacheInfo3.y >= 0.5 && PathTraceNeeCacheCurrentRluCount() > 0u;
}

uint2 PathTraceNeeCacheCurrentEmissiveRange()
{
    const uint offset = (uint)max(RestirLightManagerRangeInfo.x, 0.0);
    const uint count = (uint)max(RestirLightManagerRangeInfo.y, 0.0);
    return uint2(offset, count);
}

uint2 PathTraceNeeCacheCurrentDoomAnalyticRange()
{
    const uint offset = (uint)max(RestirLightManagerRangeInfo.z, 0.0);
    const uint count = (uint)max(RestirLightManagerRangeInfo.w, 0.0);
    return uint2(offset, count);
}

uint2 PathTraceNeeCacheCurrentStableDoomAnalyticRange()
{
    const uint2 range = PathTraceNeeCacheCurrentDoomAnalyticRange();
    return uint2(range.x, min(range.y, PathTraceNeeCacheStableDoomAnalyticCount()));
}

float3 PathTraceNeeCacheTaskHeat(float value)
{
    value = saturate(value);
    if (value < 0.5)
    {
        return lerp(float3(0.02, 0.08, 0.28), float3(0.08, 0.75, 0.55), value * 2.0);
    }
    return lerp(float3(0.08, 0.75, 0.55), float3(1.0, 0.58, 0.08), (value - 0.5) * 2.0);
}

float3 PathTraceNeeCacheCandidateColor(uint denseRluIndex)
{
    uint hash = denseRluIndex * 747796405u + 2891336453u;
    hash ^= hash >> 16u;
    hash *= 2246822519u;
    return lerp(float3(0.10, 0.05, 0.02), float3(1.00, 0.66, 0.16), ((hash >> 8u) & 255u) / 255.0);
}

void PathTraceNeeCacheDecayTaskCell(uint cellIndex)
{
    const uint taskSlots = PathTraceNeeCacheTaskSlotCount();
    const uint taskIndex = cellIndex * taskSlots;
    if (PathTraceNeeCacheTasks[taskIndex].flags == 0u)
    {
        return;
    }

    const uint frameIndex = PathTraceNeeCacheFrameIndex();
    const uint previousFrame = PathTraceNeeCacheTasks[taskIndex].reserved1;
    if (previousFrame == frameIndex)
    {
        return;
    }

    const uint frameGap = frameIndex > previousFrame ? min(frameIndex - previousFrame, 16u) : 1u;
    uint count = PathTraceNeeCacheTasks[taskIndex].reserved0;
    [loop]
    for (uint i = 0u; i < frameGap; ++i)
    {
        count = (count * 7u) / 8u;
    }

    PathTraceNeeCacheTasks[taskIndex].reserved0 = count;
    PathTraceNeeCacheTasks[taskIndex].reserved1 = frameIndex;
    PathTraceNeeCacheTasks[taskIndex].accumulatedValue = saturate(log2((float)count + 1.0) / 16.0);
    PathTraceNeeCacheTasks[taskIndex].decayState = 0.875;
    if (count == 0u)
    {
        PathTraceNeeCacheTasks[taskIndex].flags = 0u;
        PathTraceNeeCacheCells[cellIndex].flags = 0u;
        PathTraceNeeCacheCells[cellIndex].taskCount = 0u;
        PathTraceNeeCacheCells[cellIndex].reserved0 = 0u;
        PathTraceNeeCacheCells[cellIndex].reserved1 = 0u;
        PathTraceNeeCacheProviderResults[cellIndex].selectedDenseRluIndex = 0xffffffffu;
        PathTraceNeeCacheProviderResults[cellIndex].sourceLabel = PATH_TRACE_NEE_CACHE_SOURCE_NONE;
        PathTraceNeeCacheProviderResults[cellIndex].fallbackReason = PATH_TRACE_NEE_CACHE_FALLBACK_EMPTY_CELL;
        PathTraceNeeCacheProviderResults[cellIndex].cellIndex = cellIndex;
        PathTraceNeeCacheProviderResults[cellIndex].candidateSlot = 0xffffffffu;
        PathTraceNeeCacheProviderResults[cellIndex].flags = 0u;
        PathTraceNeeCacheProviderResults[cellIndex].sourcePdf = 0.0;
        PathTraceNeeCacheProviderResults[cellIndex].invSourcePdf = 0.0;
        PathTraceNeeCacheProviderResults[cellIndex].mixtureProbability = 0.0;
    }
}

float PathTraceNeeCacheEmissiveCandidateWeight(PathTraceUnifiedLightRecord record, PathTraceNeeCacheCellDebug cell)
{
    if (record.type != PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
    {
        return 0.0;
    }
    const float area = max(record.normalAndArea.w, 0.0);
    const float sourceWeight = max(record.sourceWeight, record.radianceAndLuminance.w);
    if (area <= 1.0e-6 || sourceWeight <= 0.0)
    {
        return 0.0;
    }

    const float3 cellCenter = (float3(cell.coord) + 0.5) * max(cell.cellSize, 1.0);
    const float3 toCell = cellCenter - record.positionAndRadius.xyz;
    const float distanceSquared = dot(toCell, toCell);
    const float normalFacing = saturate(dot(normalize(toCell + float3(0.0, 0.0, 1.0e-6)), record.normalAndArea.xyz));
    const float cellRadius = max(cell.cellSize, 1.0) * 0.8660254;
    return sourceWeight * area * (0.2 + 0.8 * normalFacing) / max(distanceSquared + cellRadius * cellRadius, 1.0);
}

float PathTraceNeeCacheAnalyticCandidateWeight(PathTraceUnifiedLightRecord record, PathTraceNeeCacheCellDebug cell)
{
    if (record.type != PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
    {
        return 0.0;
    }
    const float radius = max(record.positionAndRadius.w, 1.0e-3);
    const float influenceRadius = max(record.uvOrDoomParams.x, radius);
    const float luminance = max(record.radianceAndLuminance.w, 0.0);
    const float sourceWeight = max(record.sourceWeight, luminance);
    if (sourceWeight <= 0.0 || luminance <= 0.0)
    {
        return 0.0;
    }

    const float3 cellCenter = (float3(cell.coord) + 0.5) * max(cell.cellSize, 1.0);
    const float cellRadius = max(cell.cellSize, 1.0) * 0.8660254;
    const float3 toCell = cellCenter - record.positionAndRadius.xyz;
    const float distanceToCell = length(toCell);
    const float influenceDistance = influenceRadius + cellRadius;
    const float edgeDistance = max(distanceToCell - radius, 0.0);
    const float falloff = saturate(1.0 - edgeDistance / max(influenceDistance - radius, 1.0));
    return sourceWeight * falloff * falloff /
        max(dot(toCell, toCell) + radius * radius + cellRadius * cellRadius, 1.0);
}

bool PathTraceNeeCacheAnalyticStructurallyValid(PathTraceUnifiedLightRecord record)
{
    if (record.type != PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
    {
        return false;
    }
    if (record.sourceIndex == PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX)
    {
        return false;
    }
    if ((record.flags & PATH_TRACE_NEE_CACHE_DOOM_ANALYTIC_IDENTITY_VALID) == 0u)
    {
        return false;
    }
    return true;
}

bool PathTraceNeeCacheAnalyticStableCacheable(PathTraceUnifiedLightRecord record)
{
    return record.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC &&
        (record.flags & PATH_TRACE_RLU_LIGHT_FLAG_STABLE_CACHEABLE) != 0u;
}

bool PathTraceNeeCacheAnalyticRluReplayValid(uint denseIndex)
{
    if (!PathTraceNeeCacheCurrentRluReady() || denseIndex >= PathTraceNeeCacheCurrentRluCount())
    {
        return false;
    }
    const RAB_LightInfo lightInfo = RAB_LoadActiveRrxLightInfo(denseIndex, false);
    return RAB_IsLightInfoValid(lightInfo) &&
        lightInfo.unifiedLightType == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC;
}

float4 PathTraceNeeCacheBuildCandidatesForCell(PathTraceNeeCacheCellDebug cell, uint view);

uint PathTraceNeeCachePcgHash(uint hash)
{
    uint state = hash * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float PathTraceNeeCacheHashToUnitFloat(uint hash)
{
    const uint mixed = PathTraceNeeCachePcgHash(hash);
    return ((mixed & 0x00ffffffu) + 0.5) / 16777216.0;
}

PathTraceNeeCacheProviderResult PathTraceNeeCacheMakeInvalidProviderResult(uint cellIndex, uint fallbackReason)
{
    PathTraceNeeCacheProviderResult result = (PathTraceNeeCacheProviderResult)0;
    result.selectedDenseRluIndex = 0xffffffffu;
    result.sourceLabel = PATH_TRACE_NEE_CACHE_SOURCE_NONE;
    result.fallbackReason = fallbackReason;
    result.cellIndex = cellIndex;
    result.candidateSlot = 0xffffffffu;
    result.flags = 0u;
    result.sourcePdf = 0.0;
    result.invSourcePdf = 0.0;
    result.mixtureProbability = 0.0;
    return result;
}

bool PathTraceNeeCacheCandidateUsable(PathTraceNeeCacheCandidateRecord candidate, uint currentRluCount)
{
    return candidate.flags != 0u &&
        candidate.denseRluIndex < currentRluCount &&
        candidate.sourcePdf > 0.0 &&
        candidate.invSourcePdf > 0.0 &&
        (candidate.lightClass == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC ||
            candidate.lightClass == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE);
}

bool PathTraceNeeCacheCandidateReplayRecord(PathTraceNeeCacheCandidateRecord candidate, uint currentRluCount, out PathTraceUnifiedLightRecord record)
{
    record = (PathTraceUnifiedLightRecord)0;
    if (!PathTraceNeeCacheCandidateUsable(candidate, currentRluCount))
    {
        return false;
    }

    const RAB_LightInfo lightInfo = RAB_LoadActiveRrxLightInfo(candidate.denseRluIndex, false);
    if (!RAB_IsLightInfoValid(lightInfo) || lightInfo.unifiedLightType != candidate.lightClass)
    {
        return false;
    }

    record = PathTraceRestirLightManagerCurrentPayload[candidate.denseRluIndex];
    if (candidate.lightClass == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC &&
        !PathTraceNeeCacheAnalyticStableCacheable(record))
    {
        return false;
    }
    return record.type == candidate.lightClass;
}

float PathTraceNeeCacheCurrentCandidateWeight(PathTraceNeeCacheCandidateRecord candidate, PathTraceNeeCacheCellDebug cell, uint currentRluCount, out bool replayValid)
{
    replayValid = false;
    PathTraceUnifiedLightRecord record;
    if (!PathTraceNeeCacheCandidateReplayRecord(candidate, currentRluCount, record))
    {
        return 0.0;
    }
    replayValid = true;

    if (candidate.lightClass == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
    {
        return PathTraceNeeCacheAnalyticCandidateWeight(record, cell);
    }
    if (candidate.lightClass == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
    {
        return PathTraceNeeCacheEmissiveCandidateWeight(record, cell);
    }
    return 0.0;
}

uint PathTraceNeeCacheSourceLabelFromCandidate(PathTraceNeeCacheCandidateRecord candidate)
{
    if (candidate.lightClass == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
    {
        return PATH_TRACE_NEE_CACHE_SOURCE_CACHE_ANALYTIC;
    }
    if (candidate.lightClass == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
    {
        return PATH_TRACE_NEE_CACHE_SOURCE_CACHE_EMISSIVE;
    }
    return PATH_TRACE_NEE_CACHE_SOURCE_NONE;
}

PathTraceNeeCacheCandidateRecord PathTraceNeeCacheSelectWeightedCandidateForCell(PathTraceNeeCacheCellDebug cell, out float selectedCachePdf, out uint replayRejectedCount, out uint zeroCurrentWeightCount)
{
    const uint candidateSlots = PathTraceNeeCacheCandidateSlotCount();
    const uint baseSlot = cell.cellIndex * candidateSlots;
    const uint currentRluCount = PathTraceNeeCacheCurrentRluCount();
    PathTraceNeeCacheCandidateRecord selected = (PathTraceNeeCacheCandidateRecord)0;
    selected.denseRluIndex = 0xffffffffu;
    selected.candidateSlot = 0xffffffffu;
    selectedCachePdf = 0.0;
    replayRejectedCount = 0u;
    zeroCurrentWeightCount = 0u;
    float totalWeight = 0.0;

    const PathTraceNeeCacheCellRecord storedCell = PathTraceNeeCacheCells[cell.cellIndex];
    if (storedCell.flags == 0u || storedCell.hash != cell.hash)
    {
        return selected;
    }

    [loop]
    for (uint slot = 0u; slot < candidateSlots; ++slot)
    {
        const PathTraceNeeCacheCandidateRecord candidate = PathTraceNeeCacheCandidates[baseSlot + slot];
        if (!PathTraceNeeCacheCandidateUsable(candidate, currentRluCount))
        {
            continue;
        }

        bool replayValid = false;
        const float currentWeight = PathTraceNeeCacheCurrentCandidateWeight(candidate, cell, currentRluCount, replayValid);
        if (!replayValid)
        {
            ++replayRejectedCount;
            continue;
        }
        if (currentWeight <= 0.0)
        {
            ++zeroCurrentWeightCount;
            continue;
        }
        totalWeight += currentWeight;
    }
    if (totalWeight <= 0.0)
    {
        return selected;
    }

    const float threshold = PathTraceNeeCacheHashToUnitFloat(cell.hash ^ (PathTraceNeeCacheFrameIndex() * 668265263u)) * totalWeight;
    float cumulativeWeight = 0.0;
    [loop]
    for (uint selectSlot = 0u; selectSlot < candidateSlots; ++selectSlot)
    {
        const PathTraceNeeCacheCandidateRecord candidate = PathTraceNeeCacheCandidates[baseSlot + selectSlot];
        if (!PathTraceNeeCacheCandidateUsable(candidate, currentRluCount))
        {
            continue;
        }

        bool replayValid = false;
        const float currentWeight = PathTraceNeeCacheCurrentCandidateWeight(candidate, cell, currentRluCount, replayValid);
        if (!replayValid || currentWeight <= 0.0)
        {
            continue;
        }
        cumulativeWeight += currentWeight;
        if (selected.denseRluIndex == 0xffffffffu && cumulativeWeight >= threshold)
        {
            selected = candidate;
        }
    }
    if (selected.denseRluIndex == 0xffffffffu)
    {
        return selected;
    }

    float selectedIdentityWeight = 0.0;
    [loop]
    for (uint pdfSlot = 0u; pdfSlot < candidateSlots; ++pdfSlot)
    {
        const PathTraceNeeCacheCandidateRecord candidate = PathTraceNeeCacheCandidates[baseSlot + pdfSlot];
        if (!PathTraceNeeCacheCandidateUsable(candidate, currentRluCount))
        {
            continue;
        }
        if (candidate.denseRluIndex == selected.denseRluIndex)
        {
            bool replayValid = false;
            const float currentWeight = PathTraceNeeCacheCurrentCandidateWeight(candidate, cell, currentRluCount, replayValid);
            if (replayValid && currentWeight > 0.0)
            {
                selectedIdentityWeight += currentWeight;
            }
        }
    }
    selectedCachePdf = selectedIdentityWeight / max(totalWeight, 1.0e-8);
    return selected;
}

PathTraceNeeCacheProviderResult PathTraceNeeCacheMakeFallbackResult(PathTraceNeeCacheCellDebug cell, uint fallbackReason, float mixtureProbability)
{
    const uint currentRluCount = PathTraceNeeCacheCurrentRluCount();
    if (!PathTraceNeeCacheCurrentRluReady() || currentRluCount == 0u)
    {
        return PathTraceNeeCacheMakeInvalidProviderResult(cell.cellIndex, PATH_TRACE_NEE_CACHE_FALLBACK_NO_RLU);
    }

    const uint sourceDomain = PathTraceNeeCacheSourceDomain();
    const uint2 emissiveRange = PathTraceNeeCacheCurrentEmissiveRange();
    const uint2 analyticRange = PathTraceNeeCacheCurrentDoomAnalyticRange();
    const uint emissiveRangeOffset = min(emissiveRange.x, currentRluCount);
    const uint emissiveRangeCount = min(emissiveRange.y, currentRluCount - emissiveRangeOffset);
    const uint analyticRangeOffset = min(analyticRange.x, currentRluCount);
    const uint analyticRangeCount = min(analyticRange.y, currentRluCount - analyticRangeOffset);
    uint rangeOffset = 0u;
    uint rangeCount = currentRluCount;
    uint sourceLabel = PATH_TRACE_NEE_CACHE_SOURCE_FALLBACK_FULL_RLU;
    float classProbability = 1.0;

    if (sourceDomain == 1u)
    {
        rangeOffset = emissiveRangeOffset;
        rangeCount = emissiveRangeCount;
        sourceLabel = PATH_TRACE_NEE_CACHE_SOURCE_FALLBACK_TYPED_RLU;
    }
    else if (sourceDomain == 2u)
    {
        rangeOffset = analyticRangeOffset;
        rangeCount = analyticRangeCount;
        sourceLabel = PATH_TRACE_NEE_CACHE_SOURCE_FALLBACK_TYPED_RLU;
    }
    else if (sourceDomain == 3u)
    {
        const bool useAnalytic =
            analyticRangeCount > 0u &&
            (emissiveRangeCount == 0u ||
                PathTraceNeeCacheHashToUnitFloat(cell.hash ^ (PathTraceNeeCacheFrameIndex() * 1597334677u)) >= 0.5);
        rangeOffset = useAnalytic ? analyticRangeOffset : emissiveRangeOffset;
        rangeCount = useAnalytic ? analyticRangeCount : emissiveRangeCount;
        classProbability = PathTraceNeeCacheTypedClassProbability(useAnalytic, emissiveRangeCount, analyticRangeCount);
        sourceLabel = PATH_TRACE_NEE_CACHE_SOURCE_FALLBACK_TYPED_RLU;
    }

    if (rangeCount == 0u)
    {
        return PathTraceNeeCacheMakeInvalidProviderResult(cell.cellIndex, PATH_TRACE_NEE_CACHE_FALLBACK_NO_RLU);
    }

    const uint denseIndex = rangeOffset + ((cell.hash ^ (PathTraceNeeCacheFrameIndex() * 747796405u)) % rangeCount);
    const float selectedMixtureProbability = saturate(mixtureProbability);
    const float sourcePdf = selectedMixtureProbability * classProbability / max((float)rangeCount, 1.0);
    if (sourcePdf <= 0.0)
    {
        return PathTraceNeeCacheMakeInvalidProviderResult(cell.cellIndex, PATH_TRACE_NEE_CACHE_FALLBACK_DISABLED);
    }

    PathTraceNeeCacheProviderResult result = (PathTraceNeeCacheProviderResult)0;
    result.selectedDenseRluIndex = denseIndex;
    result.sourceLabel = sourceLabel;
    result.fallbackReason = fallbackReason;
    result.cellIndex = cell.cellIndex;
    result.candidateSlot = 0xffffffffu;
    result.flags = 1u;
    result.sourcePdf = sourcePdf;
    result.invSourcePdf = 1.0 / max(sourcePdf, 1.0e-8);
    result.mixtureProbability = selectedMixtureProbability;
    return result;
}

PathTraceNeeCacheProviderResult PathTraceNeeCacheSelectProposal(PathTraceNeeCacheCellDebug cell)
{
    if (!PathTraceNeeCacheCurrentRluReady())
    {
        return PathTraceNeeCacheMakeInvalidProviderResult(cell.cellIndex, PATH_TRACE_NEE_CACHE_FALLBACK_NO_RLU);
    }

    const float fallbackProbability = PathTraceNeeCacheFallbackProbability();
    const float cacheProbability = PathTraceNeeCacheCacheProbability();
    float selectedCachePdf = 0.0;
    uint replayRejectedCount = 0u;
    uint zeroCurrentWeightCount = 0u;
    const PathTraceNeeCacheCandidateRecord selectedCandidate = PathTraceNeeCacheSelectWeightedCandidateForCell(cell, selectedCachePdf, replayRejectedCount, zeroCurrentWeightCount);
    const bool hasCacheCandidate =
        PathTraceNeeCacheCandidateUsable(selectedCandidate, PathTraceNeeCacheCurrentRluCount()) &&
        selectedCachePdf > 0.0;
    const bool chooseFallback = !hasCacheCandidate ||
        PathTraceNeeCacheHashToUnitFloat(cell.hash ^ (PathTraceNeeCacheFrameIndex() * 2891336453u)) < fallbackProbability;

    if (chooseFallback)
    {
        const float forcedFallbackProbability = hasCacheCandidate ? fallbackProbability : 1.0;
        const uint reason = hasCacheCandidate
            ? PATH_TRACE_NEE_CACHE_FALLBACK_NONE
            : (replayRejectedCount > 0u
                ? PATH_TRACE_NEE_CACHE_FALLBACK_RAB_REPLAY_FAILED
                : (zeroCurrentWeightCount > 0u
                    ? PATH_TRACE_NEE_CACHE_FALLBACK_INVALID_CANDIDATE
                    : PATH_TRACE_NEE_CACHE_FALLBACK_EMPTY_CELL));
        return PathTraceNeeCacheMakeFallbackResult(cell, reason, forcedFallbackProbability);
    }

    const float sourcePdf = cacheProbability * selectedCachePdf;
    if (sourcePdf <= 0.0)
    {
        return PathTraceNeeCacheMakeFallbackResult(cell, PATH_TRACE_NEE_CACHE_FALLBACK_ZERO_SOURCE_PDF, 1.0);
    }

    PathTraceNeeCacheProviderResult result = (PathTraceNeeCacheProviderResult)0;
    result.selectedDenseRluIndex = selectedCandidate.denseRluIndex;
    result.sourceLabel = PathTraceNeeCacheSourceLabelFromCandidate(selectedCandidate);
    result.fallbackReason = PATH_TRACE_NEE_CACHE_FALLBACK_NONE;
    result.cellIndex = cell.cellIndex;
    result.candidateSlot = selectedCandidate.candidateSlot;
    result.flags = 1u;
    result.sourcePdf = sourcePdf;
    result.invSourcePdf = 1.0 / max(sourcePdf, 1.0e-8);
    result.mixtureProbability = cacheProbability;
    return result;
}

void PathTraceNeeCacheWriteProviderResult(PathTraceNeeCacheCellDebug cell)
{
    PathTraceNeeCacheProviderResults[cell.cellIndex] = PathTraceNeeCacheSelectProposal(cell);
}

float2 PathTraceNeeCacheDenseRluSampleUv(uint denseIndex, uint salt)
{
    const uint seedA = PathTraceNeeCacheHashCell(int3((int)denseIndex, (int)salt, 17), 0u);
    const uint seedB = PathTraceNeeCacheHashCell(int3((int)(denseIndex ^ salt), (int)(salt * 131u), 29), 0u);
    return float2(PathTraceNeeCacheHashToUnitFloat(seedA), PathTraceNeeCacheHashToUnitFloat(seedB));
}

#ifndef PATH_TRACE_NEE_CACHE_COMPUTE_UPDATE
float3 PathTraceNeeCacheTransformObjectVectorToWorld(float3 objectVector)
{
    const float3x4 objectToWorld = ObjectToWorld3x4();
    return float3(
        dot(objectToWorld[0].xyz, objectVector),
        dot(objectToWorld[1].xyz, objectVector),
        dot(objectToWorld[2].xyz, objectVector));
}

float3 PathTraceNeeCacheTransformObjectNormalToWorld(float3 objectNormal, float3 fallback)
{
    return PathTraceNeeCacheSafeNormalize(PathTraceNeeCacheTransformObjectVectorToWorld(objectNormal), fallback);
}

RAB_Surface PathTraceNeeCacheBuildFlatSurface(PathTraceNeeCachePayload payload, float3 rayOrigin, float3 rayDirection)
{
    RAB_Surface surface = RAB_EmptySurface();
    if (payload.hit == 0u)
    {
        return surface;
    }

    const float3 hitPosition = rayOrigin + rayDirection * payload.hitT;
    const float3 viewDir = PathTraceNeeCacheSafeNormalize(rayOrigin - hitPosition, -rayDirection);
    float3 geometryNormal = PathTraceNeeCacheSafeNormalize(payload.geometricNormal, viewDir);
    float3 shadingNormal = PathTraceNeeCacheSafeNormalize(payload.normal, geometryNormal);
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
    surface.instanceId = payload.instanceId;
    surface.primitiveIndex = payload.primitiveIndex;
    surface.surfaceClass = 0u;
    surface.flags = 0u;
    surface.material = RAB_EmptyMaterial();
    surface.material.diffuseAlbedo = float3(0.78, 0.78, 0.78);
    surface.material.roughness = 1.0;
    surface.material.specularF0 = float3(0.0, 0.0, 0.0);
    surface.material.opacity = 1.0;
    return surface;
}

float3 PathTraceNeeCacheShadeDenseRluThroughRab(uint denseIndex, RAB_Surface surface, float2 sampleUv)
{
    if (!RAB_IsSurfaceValid(surface) ||
        !PathTraceNeeCacheCurrentRluReady() ||
        denseIndex >= PathTraceNeeCacheCurrentRluCount())
    {
        return float3(0.0, 0.0, 0.0);
    }

    const RAB_LightInfo lightInfo = RAB_LoadActiveRrxLightInfo(denseIndex, false);
    if (!RAB_IsLightInfoValid(lightInfo))
    {
        return float3(0.0, 0.0, 0.0);
    }

    const RAB_LightSample lightSample = RAB_SampleActiveRrxPolymorphicLight(lightInfo, surface, sampleUv);
    if (!RAB_IsReplayableLightSample(lightSample))
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float targetPdf = max(PathTraceNeeCacheDebugLightSampleTargetPdfForSurface(lightSample, surface), 0.0);
    if (targetPdf <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float3 reflectedRadiance = PathTraceNeeCacheDebugReflectedRadianceForSurface(lightSample.position, lightSample.radiance, surface);
    return reflectedRadiance / max(lightSample.solidAnglePdf, 1.0e-6);
}

float4 PathTraceNeeCacheFlatConsumedLightView(PathTraceNeeCacheCellDebug cell, RAB_Surface surface)
{
    PathTraceNeeCacheBuildCandidatesForCell(cell, 11u);

    const uint candidateSlots = PathTraceNeeCacheCandidateSlotCount();
    const uint baseSlot = cell.cellIndex * candidateSlots;
    const uint currentRluCount = PathTraceNeeCacheCurrentRluCount();
    float3 directLight = float3(0.0, 0.0, 0.0);
    uint validCount = 0u;

    [loop]
    for (uint slot = 0u; slot < candidateSlots; ++slot)
    {
        const PathTraceNeeCacheCandidateRecord candidate = PathTraceNeeCacheCandidates[baseSlot + slot];
        if (candidate.flags == 0u || candidate.denseRluIndex >= currentRluCount)
        {
            continue;
        }

        if (candidate.lightClass == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC ||
            candidate.lightClass == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
        {
            const float3 contribution = PathTraceNeeCacheShadeDenseRluThroughRab(
                candidate.denseRluIndex,
                surface,
                PathTraceNeeCacheDenseRluSampleUv(candidate.denseRluIndex, cell.hash ^ candidate.candidateSlot));
            if (dot(contribution, float3(1.0, 1.0, 1.0)) > 0.0)
            {
                ++validCount;
            }
            directLight += contribution;
        }
    }

    const float3 flatBase = validCount != 0u ? float3(0.78, 0.78, 0.78) : float3(0.10, 0.04, 0.03);
    const float exposure = 0.04;
    const float3 mappedLight = 1.0 - exp(-directLight * exposure);
    return float4(saturate(flatBase * (0.025 + mappedLight)), 1.0);
}

float4 PathTraceNeeCacheFlatFullRluLightView(RAB_Surface surface)
{
    if (!PathTraceNeeCacheCurrentRluReady())
    {
        return float4(0.02, 0.12, 0.65, 1.0);
    }

    const uint currentRluCount = PathTraceNeeCacheCurrentRluCount();
    const uint2 emissiveRange = PathTraceNeeCacheCurrentEmissiveRange();
    const uint2 analyticRange = PathTraceNeeCacheCurrentDoomAnalyticRange();
    const uint emissiveRangeOffset = min(emissiveRange.x, currentRluCount);
    const uint emissiveRangeCount = min(emissiveRange.y, currentRluCount - emissiveRangeOffset);
    const uint analyticRangeOffset = min(analyticRange.x, currentRluCount);
    const uint analyticRangeCount = min(analyticRange.y, currentRluCount - analyticRangeOffset);
    float3 directLight = float3(0.0, 0.0, 0.0);
    uint validCount = 0u;

    [loop]
    for (uint localIndex = 0u; localIndex < analyticRangeCount; ++localIndex)
    {
        const uint denseIndex = analyticRangeOffset + localIndex;
        const float3 contribution = PathTraceNeeCacheShadeDenseRluThroughRab(
            denseIndex,
            surface,
            PathTraceNeeCacheDenseRluSampleUv(denseIndex, 0x9e3779b9u));
        if (dot(contribution, float3(1.0, 1.0, 1.0)) > 0.0)
        {
            ++validCount;
        }
        directLight += contribution;
    }

    [loop]
    for (uint localIndex = 0u; localIndex < emissiveRangeCount; ++localIndex)
    {
        const uint denseIndex = emissiveRangeOffset + localIndex;
        const float3 contribution = PathTraceNeeCacheShadeDenseRluThroughRab(
            denseIndex,
            surface,
            PathTraceNeeCacheDenseRluSampleUv(denseIndex, 0x85ebca6bu));
        if (dot(contribution, float3(1.0, 1.0, 1.0)) > 0.0)
        {
            ++validCount;
        }
        directLight += contribution;
    }

    const float3 flatBase = validCount != 0u ? float3(0.78, 0.78, 0.78) : float3(0.04, 0.05, 0.08);
    const float exposure = 0.04;
    const float3 mappedLight = 1.0 - exp(-directLight * exposure);
    return float4(saturate(flatBase * (0.025 + mappedLight)), 1.0);
}
#endif

PathTraceNeeCacheCandidateRecord PathTraceNeeCacheMakeEmptyCandidate(uint cellIndex, uint slotIndex)
{
    PathTraceNeeCacheCandidateRecord candidate = (PathTraceNeeCacheCandidateRecord)0;
    candidate.denseRluIndex = 0xffffffffu;
    candidate.lightClass = 0u;
    candidate.sourcePdf = 0.0;
    candidate.invSourcePdf = 0.0;
    candidate.candidateWeight = 0.0;
    candidate.cellIndex = cellIndex;
    candidate.candidateSlot = slotIndex;
    candidate.flags = 0u;
    return candidate;
}

uint PathTraceNeeCacheHitUpdateSeed(PathTraceNeeCacheCellDebug cell)
{
    const uint3 localQuantized = uint3(saturate(cell.localUv) * 1023.0);
    uint hash = cell.hash ^ (PathTraceNeeCacheFrameIndex() * 747796405u);
    hash ^= localQuantized.x * 2246822519u;
    hash ^= localQuantized.y * 3266489917u;
    hash ^= localQuantized.z * 668265263u;
    hash ^= hash >> 16u;
    hash *= 2246822519u;
    hash ^= hash >> 13u;
    hash *= 3266489917u;
    hash ^= hash >> 16u;
    return hash;
}

uint PathTraceNeeCacheRisProposalCount(uint rangeCount, uint lightClass)
{
    if (rangeCount == 0u)
    {
        return 0u;
    }

    const uint classBudget =
        lightClass == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC
            ? (uint)max(RestirLightManagerSampleInfo.y, 0.0)
            : (lightClass == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE
                ? (uint)max(RestirLightManagerSampleInfo.x, 0.0)
                : (uint)max(RestirLightManagerSampleInfo.z, 0.0));
    const uint fallbackBudget = max(PathTraceNeeCacheCandidateSlotCount() * 4u, 8u);
    return min(rangeCount, max(classBudget, fallbackBudget));
}

PathTraceNeeCacheCandidateRecord PathTraceNeeCacheBuildRisCandidateFromRange(
    PathTraceNeeCacheCellDebug cell,
    uint slot,
    uint rangeOffset,
    uint rangeCount,
    uint requiredClass,
    float classProbability,
    uint seed)
{
    const uint slotIndex = cell.cellIndex * PathTraceNeeCacheCandidateSlotCount() + slot;
    PathTraceNeeCacheCandidateRecord selected = PathTraceNeeCacheMakeEmptyCandidate(cell.cellIndex, slot);
    if (rangeCount == 0u)
    {
        return selected;
    }

    const uint proposalCount = PathTraceNeeCacheRisProposalCount(rangeCount, requiredClass);
    if (proposalCount == 0u)
    {
        return selected;
    }

    const float proposalInvSourcePdf = (float)rangeCount / max(max(classProbability, 1.0e-8) * (float)proposalCount, 1.0e-8);
    float risWeightSum = 0.0;
    float selectedWeight = 0.0;
    uint acceptedProposalCount = 0u;

    [loop]
    for (uint proposalIndex = 0u; proposalIndex < proposalCount; ++proposalIndex)
    {
        const uint proposalHash = PathTraceNeeCachePcgHash(
            seed ^ (slot * 1013904223u) ^ (proposalIndex * 2654435761u));
        const uint denseIndex = rangeOffset + (proposalHash % rangeCount);
        const PathTraceUnifiedLightRecord record = PathTraceRestirLightManagerCurrentPayload[denseIndex];
        if (requiredClass != 0u && record.type != requiredClass)
        {
            continue;
        }

        float candidateWeight = 0.0;
        if (record.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
        {
            if (!PathTraceNeeCacheAnalyticStableCacheable(record) ||
                !PathTraceNeeCacheAnalyticStructurallyValid(record))
            {
                continue;
            }
            candidateWeight = PathTraceNeeCacheAnalyticCandidateWeight(record, cell);
        }
        else if (record.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
        {
            const float sourceWeight = max(record.sourceWeight, record.radianceAndLuminance.w);
            const float area = max(record.normalAndArea.w, 0.0);
            if (sourceWeight <= 0.0 || area <= 1.0e-6)
            {
                continue;
            }
            candidateWeight = PathTraceNeeCacheEmissiveCandidateWeight(record, cell);
        }
        else
        {
            continue;
        }

        if (candidateWeight <= 0.0)
        {
            continue;
        }

        const float risWeight = candidateWeight * proposalInvSourcePdf;
        ++acceptedProposalCount;
        risWeightSum += risWeight;
        const float selector = PathTraceNeeCacheHashToUnitFloat(proposalHash ^ 0x9e3779b9u);
        if (acceptedProposalCount == 1u || selector < risWeight / max(risWeightSum, 1.0e-8))
        {
            selected.denseRluIndex = denseIndex;
            selected.lightClass = record.type;
            selected.candidateWeight = candidateWeight;
            selectedWeight = candidateWeight;
        }
    }

    if (acceptedProposalCount == 0u || selected.denseRluIndex == 0xffffffffu || selectedWeight <= 0.0 || risWeightSum <= 0.0)
    {
        return PathTraceNeeCacheMakeEmptyCandidate(cell.cellIndex, slot);
    }

    selected.sourcePdf = selectedWeight / max(risWeightSum, 1.0e-8);
    selected.invSourcePdf = risWeightSum / max(selectedWeight, 1.0e-8);
    selected.cellIndex = cell.cellIndex;
    selected.candidateSlot = slot;
    selected.flags = 1u;
    return selected;
}

float4 PathTraceNeeCacheBuildCandidatesForCell(PathTraceNeeCacheCellDebug cell, uint view)
{
    const uint candidateSlots = PathTraceNeeCacheCandidateSlotCount();
    const uint baseSlot = cell.cellIndex * candidateSlots;
    const uint2 emissiveRange = PathTraceNeeCacheCurrentEmissiveRange();
    const uint2 analyticRange = PathTraceNeeCacheCurrentDoomAnalyticRange();
    const uint currentRluCount = PathTraceNeeCacheCurrentRluCount();
    const uint emissiveRangeOffset = min(emissiveRange.x, currentRluCount);
    const uint emissiveRangeCount = min(emissiveRange.y, currentRluCount - emissiveRangeOffset);
    const uint analyticRangeOffset = min(analyticRange.x, currentRluCount);
    const uint analyticRangeCount = min(analyticRange.y, currentRluCount - analyticRangeOffset);
    const uint cacheableFullRangeCount = min(currentRluCount, emissiveRangeCount + analyticRangeCount);
    const bool sourceAllowsEmissive = PathTraceNeeCacheSourceDomainAllowsEmissive();
    const bool sourceAllowsAnalytic = PathTraceNeeCacheSourceDomainAllowsAnalytic();
    const uint activeEmissiveRangeCount = sourceAllowsEmissive ? emissiveRangeCount : 0u;
    const uint activeAnalyticRangeCount = sourceAllowsAnalytic ? analyticRangeCount : 0u;

    const uint ownerKey = cell.hash != 0u ? cell.hash : 1u;
    uint previousOwnerKey = 0u;
    InterlockedCompareExchange(PathTraceNeeCacheCells[cell.cellIndex].reserved0, 0u, ownerKey, previousOwnerKey);
    if (previousOwnerKey != 0u && previousOwnerKey != ownerKey)
    {
        return float4(0.10, 0.02, 0.16, 1.0);
    }

    const uint frameUpdateKey = PathTraceNeeCacheFrameIndex() + 1u;
    uint previousFrameUpdateKey = 0u;
    InterlockedExchange(PathTraceNeeCacheCells[cell.cellIndex].reserved1, frameUpdateKey, previousFrameUpdateKey);
    if (previousFrameUpdateKey == frameUpdateKey)
    {
        return float4(0.02, 0.18, 0.26, 1.0);
    }

    const PathTraceNeeCacheCellRecord previousCell = PathTraceNeeCacheCells[cell.cellIndex];
    if (previousCell.flags != 0u && previousCell.hash != cell.hash)
    {
        return float4(0.10, 0.02, 0.16, 1.0);
    }
    if (previousOwnerKey == 0u)
    {
        PathTraceNeeCacheProviderResults[cell.cellIndex] =
            PathTraceNeeCacheMakeInvalidProviderResult(cell.cellIndex, PATH_TRACE_NEE_CACHE_FALLBACK_EMPTY_CELL);
    }

    PathTraceNeeCacheCells[cell.cellIndex].flags = 1u;
    PathTraceNeeCacheCells[cell.cellIndex].hash = cell.hash;
    PathTraceNeeCacheCells[cell.cellIndex].candidateOffset = baseSlot;
    PathTraceNeeCacheCells[cell.cellIndex].candidateCount = 0u;

    if (!PathTraceNeeCacheCurrentRluReady())
    {
        PathTraceNeeCacheProviderResults[cell.cellIndex] =
            PathTraceNeeCacheMakeInvalidProviderResult(cell.cellIndex, PATH_TRACE_NEE_CACHE_FALLBACK_NO_RLU);
        return float4(0.02, 0.12, 0.65, 1.0);
    }
    if (view == 5u && activeEmissiveRangeCount == 0u)
    {
        PathTraceNeeCacheWriteProviderResult(cell);
        return float4(0.34, 0.04, 0.55, 1.0);
    }
    if ((view == 6u || view == 7u || view == 10u) && activeAnalyticRangeCount == 0u)
    {
        PathTraceNeeCacheWriteProviderResult(cell);
        return float4(0.34, 0.04, 0.55, 1.0);
    }

    const uint updateSeed = PathTraceNeeCacheHitUpdateSeed(cell);
    const uint updateSlot = updateSeed % candidateSlots;
    PathTraceNeeCacheCandidateRecord updatedCandidate = PathTraceNeeCacheMakeEmptyCandidate(cell.cellIndex, updateSlot);
    if (PathTraceNeeCacheSourceDomain() == 0u)
    {
        const bool chooseAnalytic =
            activeAnalyticRangeCount > 0u &&
            (activeEmissiveRangeCount == 0u ||
                (PathTraceNeeCacheHashToUnitFloat(updateSeed ^ 0x165667b1u) *
                    (float)(activeEmissiveRangeCount + activeAnalyticRangeCount)) >= (float)activeEmissiveRangeCount);
        const float classProbability = activeEmissiveRangeCount + activeAnalyticRangeCount > 0u
            ? (float)(chooseAnalytic ? activeAnalyticRangeCount : activeEmissiveRangeCount) /
                max((float)(activeEmissiveRangeCount + activeAnalyticRangeCount), 1.0)
            : 1.0;
        updatedCandidate = PathTraceNeeCacheBuildRisCandidateFromRange(
            cell,
            updateSlot,
            chooseAnalytic ? analyticRangeOffset : emissiveRangeOffset,
            chooseAnalytic ? activeAnalyticRangeCount : activeEmissiveRangeCount,
            chooseAnalytic ? PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC : PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE,
            classProbability,
            updateSeed);
    }
    else if (PathTraceNeeCacheSourceDomain() == 1u)
    {
        updatedCandidate = PathTraceNeeCacheBuildRisCandidateFromRange(
            cell,
            updateSlot,
            emissiveRangeOffset,
            activeEmissiveRangeCount,
            PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE,
            1.0,
            updateSeed);
    }
    else if (PathTraceNeeCacheSourceDomain() == 2u)
    {
        updatedCandidate = PathTraceNeeCacheBuildRisCandidateFromRange(
            cell,
            updateSlot,
            analyticRangeOffset,
            activeAnalyticRangeCount,
            PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC,
            1.0,
            updateSeed);
    }
    else
    {
        const bool chooseAnalytic =
            activeAnalyticRangeCount > 0u &&
            (activeEmissiveRangeCount == 0u || PathTraceNeeCacheHashToUnitFloat(updateSeed ^ 0x85ebca6bu) >= 0.5);
        const float classProbability = PathTraceNeeCacheTypedClassProbability(chooseAnalytic, activeEmissiveRangeCount, activeAnalyticRangeCount);
        updatedCandidate = PathTraceNeeCacheBuildRisCandidateFromRange(
            cell,
            updateSlot,
            chooseAnalytic ? analyticRangeOffset : emissiveRangeOffset,
            chooseAnalytic ? activeAnalyticRangeCount : activeEmissiveRangeCount,
            chooseAnalytic ? PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC : PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE,
            classProbability,
            updateSeed);
    }

    bool updatedReplayValid = false;
    const float updatedCurrentWeight = PathTraceNeeCacheCurrentCandidateWeight(updatedCandidate, cell, currentRluCount, updatedReplayValid);
    if (updatedReplayValid && updatedCurrentWeight > 0.0)
    {
        const uint stableTargetSlot =
            ((updatedCandidate.denseRluIndex ^ (updatedCandidate.denseRluIndex >> 16u) ^ cell.hash) % candidateSlots);
        uint admitSlot = 0xffffffffu;
        uint duplicateSlot = 0xffffffffu;
        uint emptySlot = 0xffffffffu;
        bool targetSlotReplaceable = false;

        [loop]
        for (uint admitScanSlot = 0u; admitScanSlot < candidateSlots; ++admitScanSlot)
        {
            const PathTraceNeeCacheCandidateRecord existingCandidate = PathTraceNeeCacheCandidates[baseSlot + admitScanSlot];
            bool existingReplayValid = false;
            const float existingWeight = PathTraceNeeCacheCurrentCandidateWeight(existingCandidate, cell, currentRluCount, existingReplayValid);
            if (!existingReplayValid || existingWeight <= 0.0)
            {
                if (admitScanSlot == stableTargetSlot)
                {
                    targetSlotReplaceable = true;
                }
                else if (emptySlot == 0xffffffffu)
                {
                    emptySlot = admitScanSlot;
                }
                continue;
            }

            if (existingCandidate.denseRluIndex == updatedCandidate.denseRluIndex)
            {
                duplicateSlot = admitScanSlot;
            }
            if (admitScanSlot == stableTargetSlot)
            {
                targetSlotReplaceable = updatedCurrentWeight > existingWeight * 2.0;
            }
        }

        if (duplicateSlot != 0xffffffffu)
        {
            admitSlot = duplicateSlot;
        }
        else if (targetSlotReplaceable)
        {
            admitSlot = stableTargetSlot;
        }
        else if (emptySlot != 0xffffffffu)
        {
            admitSlot = emptySlot;
        }

        if (admitSlot != 0xffffffffu)
        {
            updatedCandidate.cellIndex = cell.cellIndex;
            updatedCandidate.candidateSlot = admitSlot;
            updatedCandidate.candidateWeight = updatedCurrentWeight;
            PathTraceNeeCacheCandidates[baseSlot + admitSlot] = updatedCandidate;
        }
    }

    uint persistentCandidateCount = 0u;
    [loop]
    for (uint countSlot = 0u; countSlot < candidateSlots; ++countSlot)
    {
        const PathTraceNeeCacheCandidateRecord candidate = PathTraceNeeCacheCandidates[baseSlot + countSlot];
        if (PathTraceNeeCacheCandidateUsable(candidate, currentRluCount))
        {
            ++persistentCandidateCount;
        }
    }

    PathTraceNeeCacheCells[cell.cellIndex].candidateCount = persistentCandidateCount;
    PathTraceNeeCacheWriteProviderResult(cell);
    return float4(0.02, 0.18 + 0.06 * persistentCandidateCount, 0.26, 1.0);
}

#ifndef PATH_TRACE_NEE_CACHE_COMPUTE_UPDATE
float4 PathTraceNeeCacheVisualizeBuiltCandidatesForCell(PathTraceNeeCacheCellDebug cell, uint view)
{
    const uint candidateSlots = PathTraceNeeCacheCandidateSlotCount();
    const uint baseSlot = cell.cellIndex * candidateSlots;
    const uint currentRluCount = PathTraceNeeCacheCurrentRluCount();
    const PathTraceNeeCacheProviderResult providerResult = PathTraceNeeCacheProviderResults[cell.cellIndex];

    if (view == 8u)
    {
        if (providerResult.flags == 0u)
        {
            return float4(0.12, 0.02, 0.02, 1.0);
        }
        if (providerResult.sourceLabel == PATH_TRACE_NEE_CACHE_SOURCE_CACHE_ANALYTIC)
        {
            return float4(0.90, 0.54, 0.08, 1.0);
        }
        if (providerResult.sourceLabel == PATH_TRACE_NEE_CACHE_SOURCE_CACHE_EMISSIVE)
        {
            return float4(0.08, 0.68, 0.86, 1.0);
        }
        if (providerResult.sourceLabel == PATH_TRACE_NEE_CACHE_SOURCE_FALLBACK_FULL_RLU)
        {
            return float4(0.12, 0.25 + 0.55 * providerResult.mixtureProbability, 0.95, 1.0);
        }
        return float4(0.30, 0.05, 0.45, 1.0);
    }
    if (view == 9u)
    {
        if (providerResult.fallbackReason == PATH_TRACE_NEE_CACHE_FALLBACK_NONE)
        {
            return float4(0.05, 0.78, 0.20, 1.0);
        }
        if (providerResult.fallbackReason == PATH_TRACE_NEE_CACHE_FALLBACK_NO_RLU)
        {
            return float4(0.02, 0.12, 0.65, 1.0);
        }
        if (providerResult.fallbackReason == PATH_TRACE_NEE_CACHE_FALLBACK_EMPTY_CELL)
        {
            return float4(0.90, 0.48, 0.05, 1.0);
        }
        if (providerResult.fallbackReason == PATH_TRACE_NEE_CACHE_FALLBACK_ZERO_SOURCE_PDF)
        {
            return float4(0.85, 0.02, 0.02, 1.0);
        }
        return float4(0.45, 0.08, 0.62, 1.0);
    }

    float bestEmissiveWeight = 0.0;
    uint bestEmissiveDenseIndex = 0xffffffffu;
    float bestEmissiveSourcePdf = 0.0;
    float bestAnalyticWeight = 0.0;
    uint bestAnalyticDenseIndex = 0xffffffffu;
    float bestAnalyticInvSourcePdf = 0.0;
    uint bestAnalyticReplayValid = 0u;
    uint replayRejectedCount = 0u;
    uint zeroCurrentWeightCount = 0u;

    [loop]
    for (uint slot = 0u; slot < candidateSlots; ++slot)
    {
        const PathTraceNeeCacheCandidateRecord candidate = PathTraceNeeCacheCandidates[baseSlot + slot];
        if (!PathTraceNeeCacheCandidateUsable(candidate, currentRluCount))
        {
            continue;
        }

        bool replayValid = false;
        const float currentWeight = PathTraceNeeCacheCurrentCandidateWeight(candidate, cell, currentRluCount, replayValid);
        if (!replayValid)
        {
            ++replayRejectedCount;
            continue;
        }
        if (currentWeight <= 0.0)
        {
            ++zeroCurrentWeightCount;
            continue;
        }

        if (candidate.lightClass == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE && currentWeight >= bestEmissiveWeight)
        {
            bestEmissiveWeight = currentWeight;
            bestEmissiveDenseIndex = candidate.denseRluIndex;
            bestEmissiveSourcePdf = candidate.sourcePdf;
        }
        if (candidate.lightClass == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC && currentWeight >= bestAnalyticWeight)
        {
            bestAnalyticWeight = currentWeight;
            bestAnalyticDenseIndex = candidate.denseRluIndex;
            bestAnalyticInvSourcePdf = candidate.invSourcePdf;
            bestAnalyticReplayValid = 1u;
        }
    }

    if (view == 6u)
    {
        if (bestAnalyticDenseIndex == 0xffffffffu)
        {
            return replayRejectedCount != 0u ? float4(0.70, 0.02, 0.02, 1.0) :
                (zeroCurrentWeightCount != 0u ? float4(0.22, 0.02, 0.01, 1.0) : float4(0.12, 0.03, 0.02, 1.0));
        }
        const float weightBand = saturate(log2(bestAnalyticWeight + 1.0) / 10.0);
        const float3 identityColor = PathTraceNeeCacheCellColor(bestAnalyticDenseIndex ^ (bestAnalyticDenseIndex << 13u));
        return float4(lerp(identityColor * 0.30, identityColor, max(weightBand, 0.25)), 1.0);
    }
    if (view == 7u)
    {
        if (bestAnalyticDenseIndex == 0xffffffffu)
        {
            return float4(0.12, 0.03, 0.02, 1.0);
        }
        const float sourcePdfBand = saturate(log2(bestAnalyticInvSourcePdf + 1.0) / 12.0);
        const float3 identityColor = PathTraceNeeCacheCellColor(bestAnalyticDenseIndex ^ (bestAnalyticDenseIndex << 13u));
        const float3 pdfColor = lerp(float3(0.02, 0.12, 0.38), float3(0.95, 0.72, 0.18), sourcePdfBand);
        return float4(lerp(pdfColor, identityColor, 0.45), 1.0);
    }
    if (view == 10u)
    {
        return bestAnalyticReplayValid != 0u ? float4(0.05, 0.85, 0.18, 1.0) : float4(0.85, 0.02, 0.02, 1.0);
    }

    if (bestEmissiveDenseIndex == 0xffffffffu)
    {
        return replayRejectedCount != 0u ? float4(0.70, 0.02, 0.02, 1.0) :
            (zeroCurrentWeightCount != 0u ? float4(0.22, 0.02, 0.01, 1.0) : float4(0.12, 0.03, 0.02, 1.0));
    }

    const float pdfBand = saturate(log2(1.0 / max(bestEmissiveSourcePdf, 1.0e-8)) / 16.0);
    const float weightBand = saturate(log2(bestEmissiveWeight + 1.0) / 8.0);
    const float3 identityColor = PathTraceNeeCacheCandidateColor(bestEmissiveDenseIndex);
    return float4(lerp(identityColor * 0.35, identityColor, max(weightBand, 0.25)) + float3(pdfBand * 0.12, 0.0, 0.0), 1.0);
}

float PathTraceNeeCacheAccumulatePrimaryHitTask(PathTraceNeeCacheCellDebug cell)
{
    const uint taskSlots = PathTraceNeeCacheTaskSlotCount();
    const uint taskIndex = cell.cellIndex * taskSlots;
    const uint frameIndex = PathTraceNeeCacheFrameIndex();
    uint previousCount = 0u;
    InterlockedAdd(PathTraceNeeCacheTasks[taskIndex].reserved0, 1u, previousCount);
    const uint nextCount = min(previousCount + 1u, 1048575u);
    const float value = saturate(log2((float)nextCount + 1.0) / 16.0);

    PathTraceNeeCacheTasks[taskIndex].denseRluIndex = 0xffffffffu;
    PathTraceNeeCacheTasks[taskIndex].taskClass = 1u;
    PathTraceNeeCacheTasks[taskIndex].accumulatedValue = value;
    PathTraceNeeCacheTasks[taskIndex].decayState = 1.0;
    PathTraceNeeCacheTasks[taskIndex].cellIndex = cell.cellIndex;
    PathTraceNeeCacheTasks[taskIndex].flags = 1u;

    PathTraceNeeCacheCells[cell.cellIndex].flags = 1u;
    PathTraceNeeCacheCells[cell.cellIndex].hash = cell.hash;
    PathTraceNeeCacheCells[cell.cellIndex].taskOffset = taskIndex;
    PathTraceNeeCacheCells[cell.cellIndex].taskCount = 1u;
    PathTraceNeeCacheCells[cell.cellIndex].candidateOffset = cell.cellIndex * PathTraceNeeCacheCandidateSlotCount();
    PathTraceNeeCacheCells[cell.cellIndex].candidateCount = 0u;
    return value;
}

float4 PathTraceNeeCacheDebugColor(PathTraceNeeCachePayload payload, float3 rayOrigin, float3 rayDirection, uint view)
{
    const float3 worldPosition = rayOrigin + rayDirection * payload.hitT;
    const uint enabled = (uint)NeeCacheInfo0.x;
    const uint cellResolution = max((uint)NeeCacheInfo1.x, 1u);
    const float minRange = max(NeeCacheInfo1.y, 1.0);
    const uint cellCount = max((uint)NeeCacheInfo1.z, 1u);
    const PathTraceNeeCacheCellDebug cell = PathTraceNeeCacheMapWorldPositionToCell(
        worldPosition,
        CameraOriginAndTMax.xyz,
        cellResolution,
        minRange,
        cellCount);

    if (enabled == 0u || cell.valid == 0u)
    {
        return float4(0.55, 0.05, 0.04, 1.0);
    }

    const float boundary = cell.boundary < 0.035 ? 1.0 : 0.0;
    if (view == 1u)
    {
        const float band = 0.35 + 0.65 * (((cell.hash >> 5u) & 31u) / 31.0);
        return float4(0.08, 0.45 + 0.35 * band, 0.18 + 0.20 * band, 1.0);
    }
    if (view == 2u)
    {
        return float4(PathTraceNeeCacheCellColor(cell.hash), 1.0);
    }
    if (view == 3u)
    {
        const float3 emptyColor = lerp(float3(0.18, 0.12, 0.05), float3(0.75, 0.48, 0.14), ((cell.hash >> 11u) & 15u) / 15.0);
        return float4(boundary > 0.0 ? float3(1.0, 0.92, 0.35) : emptyColor, 1.0);
    }
    if (view == 4u)
    {
        const float taskValue = PathTraceNeeCacheAccumulatePrimaryHitTask(cell);
        return float4(PathTraceNeeCacheTaskHeat(taskValue), 1.0);
    }
    if (view == 5u || view == 6u || view == 7u || view == 8u || view == 9u || view == 10u)
    {
        return PathTraceNeeCacheVisualizeBuiltCandidatesForCell(cell, view);
    }
    if (view == 11u)
    {
        const RAB_Surface surface = PathTraceNeeCacheBuildFlatSurface(payload, rayOrigin, rayDirection);
        return PathTraceNeeCacheFlatConsumedLightView(cell, surface);
    }
    if (view == 12u)
    {
        const RAB_Surface surface = PathTraceNeeCacheBuildFlatSurface(payload, rayOrigin, rayDirection);
        return PathTraceNeeCacheFlatFullRluLightView(surface);
    }
    return float4(0.04, 0.04, 0.04, 1.0);
}

[shader("raygeneration")]
void RayGen()
{
    if (PathTraceNeeCacheDebugDispatchMode() == 2u)
    {
        const uint2 dispatchPixel = DispatchRaysIndex().xy;
        const uint2 dispatchDimensions = DispatchRaysDimensions().xy;
        const uint linearDispatchIndex = dispatchPixel.x + dispatchPixel.y * dispatchDimensions.x;
        const uint cellCount = max((uint)NeeCacheInfo1.z, 1u);
        if (linearDispatchIndex < cellCount)
        {
            PathTraceNeeCacheDecayTaskCell(linearDispatchIndex);
        }
        return;
    }
    const uint2 pixel = DispatchRaysIndex().xy + PathTraceNeeCacheDispatchTileOffset();
    const uint2 dimensions = PathTraceNeeCacheFullOutputSize();
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    const uint view = (uint)NeeCacheInfo0.y;
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

    PathTraceNeeCachePayload payload;
    payload.hit = 0u;
    payload.hitT = 0.0;
    payload.normal = float3(0.0, 0.0, 1.0);
    payload.geometricNormal = float3(0.0, 0.0, 1.0);
    payload.instanceId = 0xffffffffu;
    payload.primitiveIndex = 0xffffffffu;
    TraceRay(SmokeScene, RAY_FLAG_FORCE_OPAQUE, 0xff, 0, 1, 0, ray, payload);
    if (payload.hit == 0u)
    {
        if (PathTraceNeeCacheDebugDispatchMode() != 3u)
        {
            SmokeOutput[pixel] = PathTraceNeeCacheMissColor(view);
        }
        return;
    }

    const float3 worldPosition = ray.Origin + ray.Direction * payload.hitT;
    if (PathTraceNeeCacheDebugDispatchMode() == 3u)
    {
        const uint enabled = (uint)NeeCacheInfo0.x;
        const uint cellResolution = max((uint)NeeCacheInfo1.x, 1u);
        const float minRange = max(NeeCacheInfo1.y, 1.0);
        const uint cellCount = max((uint)NeeCacheInfo1.z, 1u);
        if (enabled != 0u)
        {
            const PathTraceNeeCacheCellDebug cell = PathTraceNeeCacheMapWorldPositionToCell(
                worldPosition,
                CameraOriginAndTMax.xyz,
                cellResolution,
                minRange,
                cellCount);
            if (cell.valid != 0u)
            {
                PathTraceNeeCacheBuildCandidatesForCell(cell, view);
            }
        }
        return;
    }
    SmokeOutput[pixel] = PathTraceNeeCacheDebugColor(payload, ray.Origin, ray.Direction, view);
}

[shader("miss")]
void Miss(inout PathTraceNeeCachePayload payload)
{
    payload.hit = 0u;
}

[shader("miss")]
void ShadowMiss(inout PathTraceNeeCacheShadowPayload payload)
{
    payload.hit = 0u;
}

[shader("anyhit")]
void AnyHit(inout PathTraceNeeCachePayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
}

[shader("anyhit")]
void ShadowAnyHit(inout PathTraceNeeCacheShadowPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    payload.hit = 1u;
}

[shader("closesthit")]
void ClosestHit(inout PathTraceNeeCachePayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    payload.hit = 1u;
    payload.hitT = RayTCurrent();
    const uint instanceId = InstanceID();
    const uint primitiveIndex = PrimitiveIndex();
    payload.instanceId = instanceId;
    payload.primitiveIndex = primitiveIndex;
    const float3 barycentrics = float3(1.0 - attributes.barycentrics.x - attributes.barycentrics.y, attributes.barycentrics.x, attributes.barycentrics.y);
    const float3 rayFallback = PathTraceNeeCacheSafeNormalize(-WorldRayDirection(), float3(0.0, 0.0, 1.0));
    payload.normal = rayFallback;
    payload.geometricNormal = rayFallback;

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
            return;
        }
        const PathTraceSmokeVertex v0 = SmokeSkinnedCurrentVertices[vertexIndex0];
        const PathTraceSmokeVertex v1 = SmokeSkinnedCurrentVertices[vertexIndex1];
        const PathTraceSmokeVertex v2 = SmokeSkinnedCurrentVertices[vertexIndex2];
        const float3 objectGeometricNormal = PathTraceNeeCacheSafeNormalize(
            cross(v1.position.xyz - v0.position.xyz, v2.position.xyz - v0.position.xyz),
            rayFallback);
        payload.geometricNormal =
            PathTraceNeeCacheTransformObjectNormalToWorld(objectGeometricNormal, rayFallback);
        const float3 objectNormal = PathTraceNeeCacheSafeNormalize(
            v0.normal.xyz * barycentrics.x +
                v1.normal.xyz * barycentrics.y +
                v2.normal.xyz * barycentrics.z,
            objectGeometricNormal);
        payload.normal =
            PathTraceNeeCacheTransformObjectNormalToWorld(objectNormal, payload.geometricNormal);
        return;
    }

    if (PathTraceIsRigidHitRouteInstance(instanceId))
    {
        const uint routeInstanceIndex = instanceId - 2u;
        if (routeInstanceIndex >= PathTraceNeeCacheRigidRouteInstanceCount())
        {
            return;
        }

        const PathTraceRigidRouteInstance routeInstance = SmokeRigidRouteInstances[routeInstanceIndex];
        if (primitiveIndex >= routeInstance.triangleCount ||
            routeInstance.triangleOffset + primitiveIndex >= PathTraceNeeCacheRigidRouteTriangleCount())
        {
            return;
        }

        const uint routeIndexOffset = routeInstance.indexOffset + primitiveIndex * 3u;
        if (routeIndexOffset + 2u >= PathTraceNeeCacheRigidRouteIndexCount())
        {
            return;
        }

        const uint i0 = SmokeRigidRouteIndices[routeIndexOffset + 0u];
        const uint i1 = SmokeRigidRouteIndices[routeIndexOffset + 1u];
        const uint i2 = SmokeRigidRouteIndices[routeIndexOffset + 2u];
        if (i0 >= routeInstance.vertexCount || i1 >= routeInstance.vertexCount || i2 >= routeInstance.vertexCount ||
            routeInstance.vertexOffset + i0 >= PathTraceNeeCacheRigidRouteVertexCount() ||
            routeInstance.vertexOffset + i1 >= PathTraceNeeCacheRigidRouteVertexCount() ||
            routeInstance.vertexOffset + i2 >= PathTraceNeeCacheRigidRouteVertexCount())
        {
            return;
        }

        const float3 p0 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i0].position.xyz;
        const float3 p1 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i1].position.xyz;
        const float3 p2 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i2].position.xyz;
        const float3 objectNormal = PathTraceNeeCacheSafeNormalize(cross(p1 - p0, p2 - p0), rayFallback);
        payload.geometricNormal = PathTraceNeeCacheTransformObjectNormalToWorld(objectNormal, rayFallback);
        payload.normal = payload.geometricNormal;
        return;
    }

    if (instanceId >= 2u)
    {
        return;
    }

    const bool staticInstance = instanceId == 0u;
    const uint triangleCount = staticInstance ? PathTraceNeeCacheStaticTriangleCount() : PathTraceNeeCacheDynamicTriangleCount();
    const uint indexCount = staticInstance ? PathTraceNeeCacheStaticIndexCount() : PathTraceNeeCacheDynamicIndexCount();
    const uint vertexCount = staticInstance ? PathTraceNeeCacheStaticVertexCount() : PathTraceNeeCacheDynamicVertexCount();
    const uint indexOffset = primitiveIndex * 3u;
    if (primitiveIndex >= triangleCount || indexOffset + 2u >= indexCount)
    {
        return;
    }

    const uint i0 = staticInstance ? SmokeStaticIndices[indexOffset + 0u] : SmokeDynamicIndices[indexOffset + 0u];
    const uint i1 = staticInstance ? SmokeStaticIndices[indexOffset + 1u] : SmokeDynamicIndices[indexOffset + 1u];
    const uint i2 = staticInstance ? SmokeStaticIndices[indexOffset + 2u] : SmokeDynamicIndices[indexOffset + 2u];
    if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
    {
        return;
    }

    const float3 p0 = staticInstance ? SmokeStaticVertices[i0].position.xyz : SmokeDynamicVertices[i0].position.xyz;
    const float3 p1 = staticInstance ? SmokeStaticVertices[i1].position.xyz : SmokeDynamicVertices[i1].position.xyz;
    const float3 p2 = staticInstance ? SmokeStaticVertices[i2].position.xyz : SmokeDynamicVertices[i2].position.xyz;
    const float3 n0 = staticInstance ? SmokeStaticVertices[i0].normal.xyz : SmokeDynamicVertices[i0].normal.xyz;
    const float3 n1 = staticInstance ? SmokeStaticVertices[i1].normal.xyz : SmokeDynamicVertices[i1].normal.xyz;
    const float3 n2 = staticInstance ? SmokeStaticVertices[i2].normal.xyz : SmokeDynamicVertices[i2].normal.xyz;
    const float3 geometricNormal = PathTraceNeeCacheSafeNormalize(cross(p1 - p0, p2 - p0), rayFallback);
    payload.geometricNormal = geometricNormal;
    payload.normal = PathTraceNeeCacheSafeNormalize(n0 * barycentrics.x + n1 * barycentrics.y + n2 * barycentrics.z, geometricNormal);
}

[shader("closesthit")]
void ShadowClosestHit(inout PathTraceNeeCacheShadowPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    payload.hit = 1u;
}
#endif
