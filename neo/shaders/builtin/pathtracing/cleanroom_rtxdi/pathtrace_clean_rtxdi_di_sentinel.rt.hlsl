#include "../../../vulkan.hlsli"
#include "../PathTracePrimarySurface.hlsli"
#ifdef RTXDI_ENABLE_PRESAMPLING
#undef RTXDI_ENABLE_PRESAMPLING
#endif
#define RTXDI_ENABLE_PRESAMPLING 0
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

struct PathTraceEmissiveDistributionEntry
{
    uint emissiveTriangleIndex;
    float cumulativePdf;
    float weight;
    float padding0;
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

VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> SmokeOutput : register(u1);
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> PathTraceMotionVectors : register(u39);
VK_IMAGE_FORMAT("r32ui") RWTexture2D<uint> PathTraceMotionVectorMask : register(u40);
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> PathTraceRRGuideAlbedo : register(u48);
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> PathTraceRRGuideNormalRoughness : register(u49);
VK_IMAGE_FORMAT("r32f") RWTexture2D<float> PathTraceRRGuideDepth : register(u50);
VK_IMAGE_FORMAT("r32f") RWTexture2D<float> PathTraceRRGuideHitDistance : register(u51);
VK_IMAGE_FORMAT("r32ui") RWTexture2D<uint> PathTraceRRGuideResetMask : register(u52);
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> PathTraceRRGuideSpecularAlbedo : register(u53);
VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> PathTraceRRInputColor : register(u54);
VK_IMAGE_FORMAT("rg16f") RWTexture2D<float2> PathTraceRRMotionVectors : register(u78);
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
StructuredBuffer<PathTraceEmissiveDistributionEntry> SmokeEmissiveDistribution : register(t46);
StructuredBuffer<PathTraceSmokeVertex> SmokeRigidRouteVertices : register(t22);
StructuredBuffer<uint> SmokeRigidRouteIndices : register(t23);
StructuredBuffer<uint> SmokeRigidRouteTriangleMaterialIndexes : register(t25);
StructuredBuffer<PathTraceRigidRouteInstance> SmokeRigidRouteInstances : register(t26);
StructuredBuffer<PathTraceDoomAnalyticLightCandidate> DoomAnalyticLights : register(t27);
RWStructuredBuffer<PathTracePrimarySurfaceRecord> PrimarySurfaceHistoryCurrent : register(u30);
RWStructuredBuffer<PathTracePrimarySurfaceRecord> PrimarySurfaceHistoryPrevious : register(u31);
StructuredBuffer<PathTraceDoomAnalyticLightCandidateIdentity> DoomAnalyticCurrentIdentities : register(t42);
StructuredBuffer<PathTraceDoomAnalyticLightCandidateIdentity> DoomAnalyticPreviousIdentities : register(t43);
StructuredBuffer<PathTraceDoomAnalyticLightRemap> DoomAnalyticRemap : register(t44);
StructuredBuffer<PathTraceDoomAnalyticLightCandidate> DoomAnalyticPreviousLights : register(t45);
StructuredBuffer<PathTraceSmokeEmissiveTriangle> SmokePreviousEmissiveTriangles : register(t57);
RWStructuredBuffer<RTXDI_PackedDIReservoir> CleanRtxdiDiCurrentReservoirs : register(u69);
RWStructuredBuffer<RTXDI_PackedDIReservoir> CleanRtxdiDiTemporalReservoirs : register(u70);
RWStructuredBuffer<RTXDI_PackedDIReservoir> CleanRtxdiDiPreviousReservoirs : register(u71);
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

static const uint RT_SMOKE_EMISSIVE_TRIANGLE_HISTORY_DYNAMIC = 0x00020000u;
static const uint RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF = 0x00040000u;
static const uint RT_SMOKE_MATERIAL_ADDITIVE_DECAL = 0x00000004u;
static const uint RT_SMOKE_MATERIAL_EMISSIVE = 0x00000008u;
static const uint RT_SMOKE_MATERIAL_FILTER_DECAL = 0x00000010u;
static const uint RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_LUMA = 0x00000040u;
static const uint RT_SMOKE_MATERIAL_PORTAL_WINDOW_FALLBACK = 0x00000200u;
static const uint RT_SMOKE_MATERIAL_OBJECT_GLASS_FALLBACK = 0x00000400u;
static const uint RT_SMOKE_MATERIAL_ADDITIVE_DECAL_WHITE_KEY = 0x00000800u;
static const uint RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_MAGENTA_KEY = 0x00001000u;
static const uint RT_SMOKE_TEXTURE_FLAG_USE_EMISSIVE_MAPS = 0x00000020u;
static const uint RT_SMOKE_TEXTURE_FLAG_RESERVOIR_TWO_SIDED_EMISSIVES = 0x00000040u;
#define DoomAnalyticLightInfo CleanRtxdiDiDoomAnalyticLightInfo
#define MotionVectorInfo CleanRtxdiDiMotionVectorInfo
#define RestirPTSurfaceInfo CleanRtxdiDiRestirPTSurfaceInfo
#define TextureInfo CleanRtxdiDiTextureInfo
#define ToyPathInfo CleanRtxdiDiToyPathInfo
#include "../pathtrace_material_classifier.hlsli"
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
static const uint CLEAN_RAB_DIAGNOSTIC_FORCE_EMISSIVE_VISIBILITY = 1u << 14u;
static const uint CLEAN_RAB_DIAGNOSTIC_DISABLE_RIGID_EMISSIVE_TEMPORAL = 1u << 19u;
static const uint PT_RIGID_ROUTE_HAS_PREVIOUS_TRANSFORM = 0x00000001u;
static const uint PT_RIGID_ROUTE_TRANSFORM_CONTINUOUS = 0x00000002u;
static const uint PT_RIGID_ROUTE_CACHED_SOURCE = 0x00000004u;
#define RB_RAB_LIGHT_SAMPLING_CORE_ONLY 1
#define RB_RAB_CLEAN_RTXDI_DI_SENTINEL 1
#define RB_RAB_CLEAN_DIAGNOSTIC_RELAX_BRDF_GATES 1
#define RB_RAB_CLEAN_REFERENCE_DOOM_ANALYTIC 1 // clean reference-RAB identity radiance modes live in the included helper
#include "../RtxdiBridge/RAB_UnifiedLightRecord.hlsli"
// The clean RTXDI target function uses Remix-style material floors before reservoir weighting.
#include "../RtxdiBridge/RAB_LightTarget.hlsli"
#include "../RtxdiBridge/RAB_NeeCache.hlsli"

StructuredBuffer<uint> CleanRtxdiDiRluCurrentToPrevious : register(t64);
StructuredBuffer<uint> CleanRtxdiDiRluPreviousToCurrent : register(t65);
StructuredBuffer<PathTraceUnifiedLightRecord> CleanRtxdiDiRluCurrentLights : register(t66);
StructuredBuffer<PathTraceUnifiedLightRecord> CleanRtxdiDiRluPreviousLights : register(t67);
StructuredBuffer<PathTraceNeeCacheProviderResult> CleanRtxdiDiNeeCacheProviderResults : register(t74);
StructuredBuffer<PathTraceNeeCacheCellRecord> CleanRtxdiDiNeeCacheCells : register(t75);
StructuredBuffer<PathTraceNeeCacheCandidateRecord> CleanRtxdiDiNeeCacheCandidates : register(t77);

static const uint CLEAN_DOOM_ANALYTIC_IDENTITY_VALID = 1u << 0u;
static const uint CLEAN_DOOM_ANALYTIC_IDENTITY_SAMPLEABLE = 1u << 1u;
static const uint CLEAN_DOOM_ANALYTIC_IDENTITY_REMAP_VALID = 1u << 2u;
static const float CLEAN_RTXDI_PI = 3.14159265358979323846;
static const uint CLEAN_INITIAL_STATUS_VALID = 0u;
static const uint CLEAN_INITIAL_STATUS_NO_LIGHT_MODE = 1u;
static const uint CLEAN_INITIAL_STATUS_DEFERRED_LIGHT_MODE = 2u;
static const uint CLEAN_INITIAL_STATUS_NO_LIGHTS = 3u;
static const uint CLEAN_INITIAL_STATUS_INVALID_SURFACE = 4u;
static const uint CLEAN_INITIAL_STATUS_INVALID_IDENTITY = 5u;
static const uint CLEAN_INITIAL_STATUS_INVALID_PAYLOAD = 6u;
static const uint CLEAN_INITIAL_STATUS_ZERO_TARGET_PDF = 7u;
static const uint CLEAN_INITIAL_STATUS_BAD_SAMPLE_PDF = 8u;
static const uint CLEAN_INITIAL_STATUS_EXTERNAL_CURRENT_EMPTY = 9u;
static const uint CLEAN_INITIAL_STATUS_EXTERNAL_UNSUPPORTED_LIGHT = 10u;
static const uint CLEAN_FLAG_EXTERNAL_PDFNEE_CURRENT = 1u << 0u;
static const uint CLEAN_FLAG_REMIX_LIGHT_UNIVERSE = 1u << 10u;
static const uint CLEAN_FLAG_NEE_CACHE_PROVIDER = 1u << 11u;
static const uint CLEAN_FLAG_PREVIOUS_BEST_APPROXIMATION = 1u << 12u;
static const uint CLEAN_FLAG_INITIAL_VISIBILITY = 1u << 17u;
static const uint CLEAN_FLAG_RESOLVE_SOLID_ANGLE_PDF = 1u << 18u;
static const uint CLEAN_TEMPORAL_FLAG_ENABLE = 1u << 0u;
static const uint CLEAN_TEMPORAL_FLAG_PREVIOUS_VALID = 1u << 1u;
static const uint CLEAN_TEMPORAL_DIAG_CURRENT_VALID = 1u << 0u;
static const uint CLEAN_TEMPORAL_DIAG_ENABLED = 1u << 1u;
static const uint CLEAN_TEMPORAL_DIAG_PREVIOUS_FRAME_VALID = 1u << 2u;
static const uint CLEAN_TEMPORAL_DIAG_CURRENT_SURFACE_VALID = 1u << 3u;
static const uint CLEAN_TEMPORAL_DIAG_MOTION_VALID = 1u << 4u;
static const uint CLEAN_TEMPORAL_DIAG_PREVIOUS_SURFACE_VALID = 1u << 5u;
static const uint CLEAN_TEMPORAL_DIAG_PREVIOUS_RESERVOIR_VALID = 1u << 6u;
static const uint CLEAN_TEMPORAL_DIAG_PREVIOUS_LIGHT_MAPPED = 1u << 7u;
static const uint CLEAN_TEMPORAL_DIAG_TEMPORAL_RESERVOIR_VALID = 1u << 8u;
static const uint CLEAN_TEMPORAL_DIAG_SDK_REUSED_PREVIOUS = 1u << 9u;
static const uint CLEAN_TEMPORAL_DIAG_CURRENT_CANDIDATE = 1u << 10u;
static const uint CLEAN_TEMPORAL_DIAG_CAMERA_REPROJECTED = 1u << 11u;
static const uint CLEAN_TEMPORAL_DIAG_SDK_CALLED = 1u << 12u;
static const uint CLEAN_TEMPORAL_DIAG_PREVIOUS_TARGET_AT_CURRENT = 1u << 13u;
static const uint CLEAN_TEMPORAL_DIAG_SDK_SELECTED_PREVIOUS_SAMPLE = 1u << 14u;
static const uint CLEAN_TEMPORAL_DIAG_TEMPORAL_OUTPUT_CHANGED = 1u << 15u;
static const uint CLEAN_TEMPORAL_DIAG_TEMPORAL_SAMPLE_PIXEL_VALID = 1u << 16u;
static const uint CLEAN_TEMPORAL_DIAG_PREVIOUS_PIXEL_IN_BOUNDS = 1u << 17u;
static const float CLEAN_TEMPORAL_AUDIT_FLAG_SCALE = 262143.0;
static const uint PT_MOTION_VECTOR_MASK_VALID = 0x00000001u;
static const uint PT_MOTION_VECTOR_MASK_SOURCE_SHIFT = 1u;
static const uint PT_MOTION_VECTOR_MASK_INVALID_REASON_SHIFT = 5u;
static const uint RT_RR_RESET_INVALID_SURFACE = 0x00000001u;
static const uint RT_RR_RESET_MISSING_PREVIOUS = 0x00000002u;
static const uint RT_RR_RESET_REJECTED_PREVIOUS = 0x00000004u;
static const uint RT_RR_RESET_MATERIAL_MISMATCH = 0x00000008u;
static const uint RT_RR_RESET_OBJECT_MOTION_UNAVAILABLE = 0x00000010u;
static const uint RT_RR_RESET_STOCHASTIC_TRANSLUCENT = 0x00000020u;
static const uint CLEAN_RAB_LIGHT_TYPE_DOOM_ANALYTIC_SPHERE = 1u;
static const uint CLEAN_RAB_LIGHT_TYPE_SYNTHETIC_CONSTANT = 2u;
static const float3 CLEAN_SYNTHETIC_CONSTANT_RADIANCE = float3(2.0, 2.0, 2.0);
static const uint CLEAN_SYNTHETIC_OVERLAP_LIGHT_COUNT = 4u;

struct PathTraceCleanRtxdiDiInitialResult
{
    RTXDI_DIReservoir reservoir;
    PathTracePrimarySurfaceRecord surface;
    PathTraceDoomAnalyticLightCandidate light;
    uint selectedLightIndex;
    uint status;
    float2 sampleUv;
    float3 samplePosition;
    float3 sampleRadiance;
    float solidAnglePdf;
    float targetPdf;
    float invSourcePdf;
    float visibility;
    uint previousBestSourceValid;
    uint previousBestTranslationValid;
    uint previousBestCandidateValid;
    uint previousBestSelected;
};

struct PathTraceCleanRtxdiDiTemporalResult
{
    RTXDI_DIReservoir reservoir;
    uint flags;
    int2 previousPixel;
    int2 temporalSamplePixel;
    float previousM;
    float previousTargetPdf;
    float previousTargetAtCurrentPdf;
    float previousInvPdf;
    float temporalTargetPdf;
    float temporalInvPdf;
};

bool PathTraceCleanRoomSyntheticConstantMode()
{
    return CleanRtxdiDiView == 9u;
}

bool PathTraceCleanRoomSyntheticAnalyticPayloadMode()
{
    return CleanRtxdiDiView == 10u;
}

bool PathTraceCleanRoomSyntheticOverlapMode()
{
    return CleanRtxdiDiView == 11u;
}

bool PathTraceCleanRoomLoadMotion(
    uint2 pixel,
    PathTracePrimarySurfaceRecord currentSurface,
    uint2 dimensions,
    out float3 screenSpaceMotion,
    out bool cameraReprojected);

bool PathTraceCleanRoomRealAnalyticPointProxyMode()
{
    return false;
}

bool PathTraceCleanRoomSyntheticMode()
{
    return PathTraceCleanRoomSyntheticConstantMode() ||
        PathTraceCleanRoomSyntheticAnalyticPayloadMode() ||
        PathTraceCleanRoomSyntheticOverlapMode();
}

bool PathTraceCleanRoomSyntheticSingleLightMode()
{
    return PathTraceCleanRoomSyntheticConstantMode() ||
        (PathTraceCleanRoomSyntheticAnalyticPayloadMode() && (uint)clamp(CleanRtxdiDiRestirPTSurfaceInfo.z, 1.0, 8.0) <= 1u);
}

uint PathTraceCleanRoomSyntheticLightCount()
{
    if (PathTraceCleanRoomSyntheticOverlapMode())
    {
        return CLEAN_SYNTHETIC_OVERLAP_LIGHT_COUNT;
    }
    if (PathTraceCleanRoomSyntheticAnalyticPayloadMode())
    {
        const uint requestedCount = (uint)clamp(CleanRtxdiDiRestirPTSurfaceInfo.z, 1.0, 8.0);
        return min(requestedCount, min(CleanRtxdiDiAnalyticLightCount, CleanRtxdiDiAnalyticIdentityCount));
    }
    return 1u;
}

uint PathTraceCleanRoomInitialLightCount()
{
    if ((CleanRtxdiDiFlags & CLEAN_FLAG_REMIX_LIGHT_UNIVERSE) != 0u)
    {
        return CleanRtxdiDiRluCurrentLightCount;
    }

    const uint analyticCount = min(CleanRtxdiDiAnalyticLightCount, CleanRtxdiDiAnalyticIdentityCount);
    return analyticCount;
}

bool PathTraceCleanRoomRluDoomAnalyticRange(out uint firstLightIndex, out uint lightCount)
{
    firstLightIndex = 0u;
    lightCount = 0u;
    if ((CleanRtxdiDiFlags & CLEAN_FLAG_REMIX_LIGHT_UNIVERSE) == 0u)
    {
        return false;
    }

    const uint first = min(CleanRtxdiDiRluDoomAnalyticRangeOffset, CleanRtxdiDiRluCurrentLightCount);
    const uint count = min(CleanRtxdiDiRluDoomAnalyticRangeCount, CleanRtxdiDiRluCurrentLightCount - first);
    if (count == 0u)
    {
        return false;
    }

    firstLightIndex = first;
    lightCount = count;
    return true;
}

uint2 PathTraceCleanRoomRluTypedRange(uint lightType)
{
    if ((CleanRtxdiDiFlags & CLEAN_FLAG_REMIX_LIGHT_UNIVERSE) == 0u)
    {
        return uint2(0u, 0u);
    }

    uint2 range = uint2(0u, 0u);
    if (lightType == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
    {
        range = uint2(
            (uint)max(CleanRtxdiDiRluRangeInfo.x, 0.0),
            (uint)max(CleanRtxdiDiRluRangeInfo.y, 0.0));
    }
    else if (lightType == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
    {
        range = uint2(
            (uint)max(CleanRtxdiDiRluRangeInfo.z, 0.0),
            (uint)max(CleanRtxdiDiRluRangeInfo.w, 0.0));
    }

    const uint first = min(range.x, CleanRtxdiDiRluCurrentLightCount);
    const uint count = min(range.y, CleanRtxdiDiRluCurrentLightCount - first);
    return uint2(first, count);
}

uint PathTraceCleanRoomRluTypedSampleCount(uint lightType)
{
    if ((CleanRtxdiDiFlags & CLEAN_FLAG_REMIX_LIGHT_UNIVERSE) == 0u)
    {
        return 0u;
    }

    if (lightType == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
    {
        return (uint)max(CleanRtxdiDiRluSampleInfo.x, 0.0);
    }
    if (lightType == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
    {
        return (uint)max(CleanRtxdiDiRluSampleInfo.y, 0.0);
    }
    return 0u;
}

uint PathTraceCleanRoomRluTotalSampleCount()
{
    return (CleanRtxdiDiFlags & CLEAN_FLAG_REMIX_LIGHT_UNIVERSE) != 0u
        ? (uint)max(CleanRtxdiDiRluSampleInfo.z, 0.0)
        : 0u;
}

uint PathTraceCleanRoomRluNonEmptyRangeCount()
{
    return (CleanRtxdiDiFlags & CLEAN_FLAG_REMIX_LIGHT_UNIVERSE) != 0u
        ? (uint)max(CleanRtxdiDiRluSampleInfo.w, 0.0)
        : 0u;
}

bool PathTraceCleanRoomRluTypedProposalInfo(
    uint lightType,
    out uint2 range,
    out uint sampleCount,
    out uint totalSampleCount,
    out uint nonEmptyRangeCount)
{
    range = PathTraceCleanRoomRluTypedRange(lightType);
    sampleCount = PathTraceCleanRoomRluTypedSampleCount(lightType);
    totalSampleCount = PathTraceCleanRoomRluTotalSampleCount();
    nonEmptyRangeCount = PathTraceCleanRoomRluNonEmptyRangeCount();
    return range.y > 0u && sampleCount > 0u && totalSampleCount > 0u && nonEmptyRangeCount > 0u;
}

float PathTraceCleanRoomRluTypedSourcePdf(uint lightType)
{
    uint2 range;
    uint sampleCount;
    uint totalSampleCount;
    uint nonEmptyRangeCount;
    if (!PathTraceCleanRoomRluTypedProposalInfo(lightType, range, sampleCount, totalSampleCount, nonEmptyRangeCount))
    {
        return 0.0;
    }

    return (float)sampleCount / max((float)range.y * (float)totalSampleCount, 1.0e-8);
}

#include "pathtrace_clean_rtxdi_di_emissive_distribution.hlsli"
void PathTraceCleanRoomAnalyticDiagnosticRange(out uint firstLightIndex, out uint lightCount)
{
    if (PathTraceCleanRoomRluDoomAnalyticRange(firstLightIndex, lightCount))
    {
        return;
    }

    firstLightIndex = 0u;
    lightCount = PathTraceCleanRoomInitialLightCount();
}

void PathTraceCleanRoomInitialLightRegion(out RTXDI_LightBufferRegion region)
{
    region = (RTXDI_LightBufferRegion)0;
    if ((CleanRtxdiDiFlags & CLEAN_FLAG_REMIX_LIGHT_UNIVERSE) != 0u &&
        PathTraceCleanRoomRluDoomAnalyticRange(region.firstLightIndex, region.numLights))
    {
        return;
    }

    region.firstLightIndex = 0u;
    region.numLights = PathTraceCleanRoomInitialLightCount();
}

bool PathTraceCleanRoomLightIndexInRegion(uint lightIndex, RTXDI_LightBufferRegion region)
{
    if (region.numLights == 0u || lightIndex < region.firstLightIndex)
    {
        return false;
    }

    return (lightIndex - region.firstLightIndex) < region.numLights;
}

bool PathTraceCleanRoomRemixLightUniverseEnabled()
{
    return (CleanRtxdiDiFlags & CLEAN_FLAG_REMIX_LIGHT_UNIVERSE) != 0u;
}

bool PathTraceCleanRoomMixedRluDomainEnabled()
{
    return PathTraceCleanRoomRemixLightUniverseEnabled() && CleanRtxdiDiRluDomain == 2u;
}

bool PathTraceCleanRoomExternalPdfNeeCurrentEnabled()
{
    return (CleanRtxdiDiFlags & CLEAN_FLAG_EXTERNAL_PDFNEE_CURRENT) != 0u;
}

bool PathTraceCleanRoomNeeCacheProviderEnabled()
{
    return (CleanRtxdiDiFlags & CLEAN_FLAG_NEE_CACHE_PROVIDER) != 0u &&
        CleanRtxdiDiNeeCacheInfo0.x >= 0.5 &&
        CleanRtxdiDiNeeCacheInfo1.z > 0.0 &&
        CleanRtxdiDiNeeCacheInfo1.w > 0.0;
}

uint PathTraceCleanRoomNeeCacheSourceDomain()
{
    return clamp((uint)max(CleanRtxdiDiNeeCacheInfo0.z, 0.0), 0u, 3u);
}

uint PathTraceCleanRoomNeeCacheCandidateSlots()
{
    return max((uint)max(CleanRtxdiDiNeeCacheInfo0.w, 0.0), 1u);
}

uint PathTraceCleanRoomNeeCacheCellCount()
{
    return max((uint)max(CleanRtxdiDiNeeCacheInfo1.z, 0.0), 1u);
}

uint PathTraceCleanRoomExternalPdfNeeEmissiveOffset()
{
    return 0u;
}

uint PathTraceCleanRoomExternalPdfNeeToAnalyticIndex(uint lightIndex)
{
    const uint offset = PathTraceCleanRoomExternalPdfNeeEmissiveOffset();
    return lightIndex >= offset ? (lightIndex - offset) : 0xffffffffu;
}

float3 PathTraceCleanRoomSentinelColor(uint2 pixel)
{
    const uint checker = ((pixel.x >> 5u) ^ (pixel.y >> 5u)) & 1u;
    const float3 a = float3(0.95, 0.10, 0.85);
    const float3 b = float3(0.05, 0.85, 0.95);
    return checker != 0u ? a : b;
}

float3 PathTraceCleanRoomPrimarySurfaceStatusColor(uint2 pixel, uint2 dimensions)
{
    const uint width = CleanRtxdiDiWidth != 0u ? CleanRtxdiDiWidth : dimensions.x;
    const uint height = CleanRtxdiDiHeight != 0u ? CleanRtxdiDiHeight : dimensions.y;
    if (width == 0u || height == 0u || pixel.x >= width || pixel.y >= height)
    {
        return float3(0.05, 0.00, 0.00);
    }

    const uint index = pixel.y * width + pixel.x;
    const PathTracePrimarySurfaceRecord record = PrimarySurfaceHistoryCurrent[index];
    if (record.header.x != RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_VERSION ||
        (record.header.y & RT_PRIMARY_SURFACE_VALID) == 0u)
    {
        return float3(0.95, 0.05, 0.10);
    }

    const uint surfaceClass = record.materialAndSurface.w & 0xffu;
    const bool opaqueSurface =
        surfaceClass == 0u ||
        surfaceClass == 1u ||
        surfaceClass == 2u;
    if (!opaqueSurface || record.shadingNormalAndOpacity.w < 0.99)
    {
        return float3(1.00, 0.78, 0.05);
    }

    return float3(0.05, 0.90, 0.18);
}

uint PathTraceCleanRoomHash(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

float PathTraceCleanRoomRandom01(uint seed)
{
    return (float)(PathTraceCleanRoomHash(seed) & 0x00ffffffu) * (1.0 / 16777215.0);
}

RTXDI_RandomSamplerState PathTraceCleanRoomInitDecorrelatedSampler(uint2 pixel, uint frameIndex, uint pass)
{
    uint seed = pixel.x * 0x9e3779b9u;
    seed ^= pixel.y * 0x85ebca6bu;
    seed ^= frameIndex * 0xc2b2ae35u;
    seed ^= pass * 0x27d4eb2fu;
    seed = PathTraceCleanRoomHash(seed ^ (seed >> 11u));
    return RTXDI_CreateRandomSamplerFromDirectSeed(seed, 1u);
}

float3 PathTraceCleanRoomHashColor(uint value)
{
    const uint hashValue = PathTraceCleanRoomHash(value);
    const float3 color = float3(
        (float)((hashValue >> 0u) & 255u),
        (float)((hashValue >> 8u) & 255u),
        (float)((hashValue >> 16u) & 255u)) * (1.0 / 255.0);
    return color * 0.75 + float3(0.18, 0.18, 0.18);
}

float3 PathTraceCleanRoomSafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-8 ? value * rsqrt(lengthSquared) : fallback;
}

float PathTraceCleanRoomLuminance(float3 value)
{
    return dot(max(value, float3(0.0, 0.0, 0.0)), float3(0.2126, 0.7152, 0.0722));
}

#include "pathtrace_clean_rtxdi_di_material_texture.hlsli"

uint PathTraceCleanRoomReservoirBlockCount(uint dimension)
{
    return (dimension + RTXDI_RESERVOIR_BLOCK_SIZE - 1u) / RTXDI_RESERVOIR_BLOCK_SIZE;
}

RTXDI_ReservoirBufferParameters PathTraceCleanRoomReservoirParams(uint2 dimensions)
{
    RTXDI_ReservoirBufferParameters params = (RTXDI_ReservoirBufferParameters)0;
    const uint blockCountX = max(PathTraceCleanRoomReservoirBlockCount(dimensions.x), 1u);
    const uint blockCountY = max(PathTraceCleanRoomReservoirBlockCount(dimensions.y), 1u);
    params.reservoirBlockRowPitch = blockCountX * RTXDI_RESERVOIR_BLOCK_SIZE * RTXDI_RESERVOIR_BLOCK_SIZE;
    params.reservoirArrayPitch = params.reservoirBlockRowPitch * blockCountY;
    return params;
}

uint PathTraceCleanRoomReservoirPointer(uint2 pixel, uint2 dimensions)
{
    return RTXDI_ReservoirPositionToPointer(PathTraceCleanRoomReservoirParams(dimensions), pixel, 0u);
}

float3 PathTraceCleanRoomPerpendicular(float3 normal)
{
    const float3 axis = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    return PathTraceCleanRoomSafeNormalize(cross(axis, normal), float3(1.0, 0.0, 0.0));
}

float3 PathTraceCleanRoomSampleCone(float3 axis, float cosThetaMax, float2 uv)
{
    const float cosTheta = lerp(1.0, cosThetaMax, saturate(uv.x));
    const float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    const float phi = 2.0 * CLEAN_RTXDI_PI * saturate(uv.y);
    const float3 tangent = PathTraceCleanRoomPerpendicular(axis);
    const float3 bitangent = PathTraceCleanRoomSafeNormalize(cross(axis, tangent), float3(0.0, 1.0, 0.0));
    return PathTraceCleanRoomSafeNormalize(axis * cosTheta + tangent * (cos(phi) * sinTheta) + bitangent * (sin(phi) * sinTheta), axis);
}

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

uint PathTraceCleanRoomLoadTriangleMaterialIndex(uint instanceId, uint primitiveIndex);

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

bool PathTraceCleanRoomAnalyticPayloadValid(PathTraceDoomAnalyticLightCandidate light)
{
    const float3 radiance = max(light.colorAndIntensity.rgb, float3(0.0, 0.0, 0.0));
    const float luminance = dot(radiance, float3(0.2126, 0.7152, 0.0722));
    return all(light.originAndRadius.xyz == light.originAndRadius.xyz) &&
        all(abs(light.originAndRadius.xyz) < float3(3.402823e+38, 3.402823e+38, 3.402823e+38)) &&
        all(radiance == radiance) &&
        all(abs(radiance) < float3(3.402823e+38, 3.402823e+38, 3.402823e+38)) &&
        luminance > 0.0 &&
        light.originAndRadius.w > 0.0 &&
        light.originAndRadius.w < 3.402823e+38 &&
        light.doomRadiusAndArea.x > 0.0 &&
        light.doomRadiusAndArea.x < 3.402823e+38;
}

bool PathTraceCleanRoomAnalyticIdentityValid(PathTraceDoomAnalyticLightCandidateIdentity identity)
{
    return (identity.flags & CLEAN_DOOM_ANALYTIC_IDENTITY_VALID) != 0u;
}

bool PathTraceCleanRoomAnalyticIdentitySampleable(PathTraceDoomAnalyticLightCandidateIdentity identity)
{
    const uint identityRequiredFlags = CLEAN_DOOM_ANALYTIC_IDENTITY_VALID | CLEAN_DOOM_ANALYTIC_IDENTITY_SAMPLEABLE;
    return (identity.flags & identityRequiredFlags) == identityRequiredFlags;
}

uint PathTraceCleanRoomAnalyticIdentityRemapIndex(PathTraceDoomAnalyticLightCandidateIdentity identity)
{
    return identity.padding0;
}

bool PathTraceCleanRoomAnalyticRemapValid(PathTraceDoomAnalyticLightRemap remap)
{
    return (remap.flags & CLEAN_DOOM_ANALYTIC_IDENTITY_REMAP_VALID) != 0u;
}

RAB_LightInfo PathTraceCleanRoomBuildAnalyticLightInfo(PathTraceDoomAnalyticLightCandidate light, uint lightIndex)
{
    RAB_LightInfo lightInfo = RAB_EmptyLightInfo();
    if (!PathTraceCleanRoomAnalyticPayloadValid(light))
    {
        return lightInfo;
    }

    lightInfo.lightType = RAB_LIGHT_TYPE_DOOM_ANALYTIC_SPHERE;
    lightInfo.lightIndex = lightIndex;
    lightInfo.unifiedLightType = PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC;
    lightInfo.materialIndex = RAB_INVALID_LIGHT_INDEX;
    lightInfo.flags = light.flags;
    lightInfo.position = light.originAndRadius.xyz;
    lightInfo.radius = max(light.originAndRadius.w, 0.01);
    lightInfo.normal = float3(0.0, 0.0, 1.0);
    lightInfo.radiance = max(light.colorAndIntensity.rgb, float3(0.0, 0.0, 0.0)) * max(CleanRtxdiDiDoomAnalyticLightInfo.z, 0.0);
    lightInfo.influenceRadius = max(light.doomRadiusAndArea.x, lightInfo.radius);
    lightInfo.area = max(light.doomRadiusAndArea.y, 1.0e-4);
    lightInfo.weight = PathTraceCleanRoomLuminance(lightInfo.radiance) * lightInfo.area * lightInfo.influenceRadius;
    return lightInfo;
}

float3 PathTraceCleanRoomMatClassRouteColor(uint route)
{
    if (route == RT_MATCLASS_ROUTE_REAL_PBR_RMAO)
    {
        return float3(0.10, 0.35, 1.00);
    }
    if (route == RT_MATCLASS_ROUTE_LEGACY_SPEC_GLOSS)
    {
        return float3(0.05, 0.90, 0.25);
    }
    if (route == RT_MATCLASS_ROUTE_SURFACE_TYPE_FALLBACK)
    {
        return float3(1.00, 0.62, 0.05);
    }
    return float3(0.18, 0.02, 0.02);
}

float3 PathTraceCleanRoomMatClassSurfaceClassColor(uint surfaceClass)
{
    if (surfaceClass == 1u) return float3(0.00, 0.22, 1.00);
    if (surfaceClass == 2u) return float3(0.48, 0.48, 0.43);
    if (surfaceClass == 3u) return float3(0.95, 0.48, 0.40);
    if (surfaceClass == 4u) return float3(0.42, 0.24, 0.08);
    if (surfaceClass == 5u) return float3(0.72, 0.52, 0.22);
    if (surfaceClass == 6u) return float3(0.05, 0.45, 1.00);
    if (surfaceClass == 7u) return float3(0.45, 1.00, 0.92);
    if (surfaceClass == 8u) return float3(0.70, 0.42, 1.00);
    if (surfaceClass == 9u) return float3(1.00, 0.08, 0.04);
    if (surfaceClass == 10u) return float3(1.00, 0.92, 0.05);
    return float3(0.25, 0.25, 0.25);
}

float3 PathTraceCleanRoomVisibleMatClassDebug(float3 color, float3 fallback)
{
    return PathTraceCleanRoomLuminance(color) > 0.02 ? color : fallback;
}

bool PathTraceCleanRoomDirectReservoirUnsupportedDebugColor(PathTracePrimarySurfaceRecord record, out float3 color)
{
    color = float3(0.0, 0.0, 0.0);
    if (CleanRtxdiDiView != 24u)
    {
        return false;
    }

    RAB_Surface surface = PathTraceCleanRoomMaterialSurfaceFromRecord(record);
    if (!RAB_IsSurfaceValid(surface) ||
        MaterialSupportedByPass(surface, RT_PATH_TRACE_MATERIAL_PASS_DIRECT_RESERVOIR))
    {
        return false;
    }

    color = MaterialFailClosedDebugColor(surface, RT_PATH_TRACE_MATERIAL_PASS_DIRECT_RESERVOIR).rgb;
    return true;
}

float3 PathTraceCleanRoomMaterialClassifierDebugColor(uint2 pixel, uint2 dimensions)
{
    PathTracePrimarySurfaceRecord record;
    if (!PathTraceCleanRoomLoadSurfaceRecord(pixel, dimensions, record))
    {
        return float3(0.75, 0.00, 0.75);
    }

    float3 unsupportedDirectReservoirColor;
    if (PathTraceCleanRoomDirectReservoirUnsupportedDebugColor(record, unsupportedDirectReservoirColor))
    {
        return unsupportedDirectReservoirColor;
    }

    const uint materialIndex = PathTraceCleanRoomResolveLiveMaterialIndex(record);
    const PathTraceSmokeMaterial material = PathTraceCleanRoomLoadSmokeMaterial(materialIndex);
    const bool hasClassifierRecord = material.padding1 != 0u;
    const bool right = pixel.x * 2u >= dimensions.x;
    const bool bottom = pixel.y * 2u >= dimensions.y;

    if (!hasClassifierRecord)
    {
        const uint checker = ((pixel.x >> 4u) ^ (pixel.y >> 4u)) & 1u;
        return checker != 0u ? float3(0.75, 0.00, 0.75) : float3(1.0, 1.0, 1.0);
    }

    if (!right && !bottom)
    {
        return PathTraceCleanRoomVisibleMatClassDebug(
            PathTraceCleanRoomMatClassRouteColor(SmokeMatClassRoute(material)),
            float3(0.75, 0.05, 0.05));
    }
    if (right && !bottom)
    {
        return PathTraceCleanRoomVisibleMatClassDebug(
            PathTraceCleanRoomMatClassSurfaceClassColor(SmokeMatClassSurfaceClass(material)),
            float3(0.10, 0.55, 0.95));
    }
    if (!right)
    {
        float3 debugAlbedo = saturate(record.albedoAndAlphaCutoff.xyz);
        float roughness = saturate(record.geometricNormalAndRoughness.w);
        float3 specularF0 = max(record.specularF0AndReserved.xyz, float3(0.0, 0.0, 0.0));
        SmokeApplyMaterialClassifierBsdf(material, debugAlbedo, specularF0, roughness);
        return PathTraceCleanRoomVisibleMatClassDebug(
            float3(roughness, roughness, roughness),
            float3(0.12, 0.12, 0.12));
    }

    return PathTraceCleanRoomVisibleMatClassDebug(
        SmokeMatClassDynamicDebugColor(material),
        float3(0.04, 0.04, 0.04));
}

bool PathTraceCleanRoomRluPayloadValid(PathTraceUnifiedLightRecord light)
{
    if (light.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
    {
        const float3 radiance = max(light.radianceAndLuminance.rgb, float3(0.0, 0.0, 0.0));
        const float luminance = dot(radiance, float3(0.2126, 0.7152, 0.0722));
        return light.sourceIndex != PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX &&
            all(light.positionAndRadius.xyz == light.positionAndRadius.xyz) &&
            all(abs(light.positionAndRadius.xyz) < float3(3.402823e+38, 3.402823e+38, 3.402823e+38)) &&
            all(light.normalAndArea.xyz == light.normalAndArea.xyz) &&
            all(abs(light.normalAndArea.xyz) < float3(3.402823e+38, 3.402823e+38, 3.402823e+38)) &&
            dot(light.normalAndArea.xyz, light.normalAndArea.xyz) > 1.0e-8 &&
            all(radiance == radiance) &&
            all(abs(radiance) < float3(3.402823e+38, 3.402823e+38, 3.402823e+38)) &&
            luminance > 0.0 &&
            light.normalAndArea.w > 1.0e-6 &&
            light.normalAndArea.w < 3.402823e+38 &&
            light.sourceWeight > 0.0 &&
            light.sourceWeight < 3.402823e+38;
    }

    const float3 radiance = max(light.radianceAndLuminance.rgb, float3(0.0, 0.0, 0.0));
    const float luminance = dot(radiance, float3(0.2126, 0.7152, 0.0722));
    return light.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC &&
        light.sourceIndex != PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX &&
        all(light.positionAndRadius.xyz == light.positionAndRadius.xyz) &&
        all(abs(light.positionAndRadius.xyz) < float3(3.402823e+38, 3.402823e+38, 3.402823e+38)) &&
        all(radiance == radiance) &&
        all(abs(radiance) < float3(3.402823e+38, 3.402823e+38, 3.402823e+38)) &&
        luminance > 0.0 &&
        light.positionAndRadius.w > 0.0 &&
        light.positionAndRadius.w < 3.402823e+38 &&
        light.uvOrDoomParams.x > 0.0 &&
        light.uvOrDoomParams.x < 3.402823e+38;
}

float3 PathTraceCleanRoomTransformRigidRoutePoint(PathTraceRigidRouteInstance routeInstance, float3 localPoint, bool previousFrame)
{
    const uint continuousFlags = PT_RIGID_ROUTE_HAS_PREVIOUS_TRANSFORM | PT_RIGID_ROUTE_TRANSFORM_CONTINUOUS;
    const bool usePreviousTransform = previousFrame && ((routeInstance.flags & continuousFlags) == continuousFlags);
    const float4 row0 = usePreviousTransform ? routeInstance.previousObjectToWorld0 : routeInstance.currentObjectToWorld0;
    const float4 row1 = usePreviousTransform ? routeInstance.previousObjectToWorld1 : routeInstance.currentObjectToWorld1;
    const float4 row2 = usePreviousTransform ? routeInstance.previousObjectToWorld2 : routeInstance.currentObjectToWorld2;
    return float3(
        dot(row0, float4(localPoint, 1.0)),
        dot(row1, float4(localPoint, 1.0)),
        dot(row2, float4(localPoint, 1.0)));
}

bool PathTraceCleanRoomLoadEmissiveTriangleGeometry(
    PathTraceSmokeEmissiveTriangle emissiveTriangle,
    bool previousFrame,
    out float3 p0,
    out float3 p1,
    out float3 p2,
    out float2 uv0,
    out float2 uv1,
    out float2 uv2)
{
    p0 = emissiveTriangle.centerAndArea.xyz;
    p1 = emissiveTriangle.centerAndArea.xyz;
    p2 = emissiveTriangle.centerAndArea.xyz;
    uv0 = emissiveTriangle.centroidUvAndWeight.xy;
    uv1 = emissiveTriangle.centroidUvAndWeight.xy;
    uv2 = emissiveTriangle.centroidUvAndWeight.xy;

    const uint instanceId = emissiveTriangle.instanceId;
    const uint primitiveIndex = emissiveTriangle.primitiveIndex;
    if (instanceId == 0u)
    {
        const uint staticVertexCount = (uint)max(CleanRtxdiDiGeometryInfo0.x, 0.0);
        const uint staticIndexCount = (uint)max(CleanRtxdiDiGeometryInfo0.y, 0.0);
        const uint staticTriangleCount = (uint)max(CleanRtxdiDiGeometryInfo0.z, 0.0);
        const uint indexOffset = primitiveIndex * 3u;
        if (primitiveIndex >= staticTriangleCount || indexOffset + 2u >= staticIndexCount)
        {
            return false;
        }

        const uint i0 = SmokeStaticIndices[indexOffset + 0u];
        const uint i1 = SmokeStaticIndices[indexOffset + 1u];
        const uint i2 = SmokeStaticIndices[indexOffset + 2u];
        if (i0 >= staticVertexCount || i1 >= staticVertexCount || i2 >= staticVertexCount)
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
        const uint dynamicVertexCount = (uint)max(CleanRtxdiDiGeometryInfo0.w, 0.0);
        const uint dynamicIndexCount = (uint)max(CleanRtxdiDiGeometryInfo1.x, 0.0);
        const uint dynamicTriangleCount = (uint)max(CleanRtxdiDiGeometryInfo1.y, 0.0);
        const uint indexOffset = primitiveIndex * 3u;
        if (primitiveIndex >= dynamicTriangleCount || indexOffset + 2u >= dynamicIndexCount)
        {
            return false;
        }

        const uint i0 = SmokeDynamicIndices[indexOffset + 0u];
        const uint i1 = SmokeDynamicIndices[indexOffset + 1u];
        const uint i2 = SmokeDynamicIndices[indexOffset + 2u];
        if (i0 >= dynamicVertexCount || i1 >= dynamicVertexCount || i2 >= dynamicVertexCount)
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
    const uint rigidRouteInstanceCount = (uint)max(ToyPathInfo.w, 0.0);
    if (routeInstanceIndex >= rigidRouteInstanceCount)
    {
        return false;
    }

    const PathTraceRigidRouteInstance routeInstance = SmokeRigidRouteInstances[routeInstanceIndex];
    const uint rigidRouteVertexCount = (uint)max(CleanRtxdiDiGeometryInfo1.z, 0.0);
    const uint rigidRouteIndexCount = (uint)max(CleanRtxdiDiGeometryInfo1.w, 0.0);
    const uint routeIndexOffset = routeInstance.indexOffset + primitiveIndex * 3u;
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

    const PathTraceSmokeVertex v0 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i0];
    const PathTraceSmokeVertex v1 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i1];
    const PathTraceSmokeVertex v2 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i2];
    p0 = PathTraceCleanRoomTransformRigidRoutePoint(routeInstance, v0.position.xyz, previousFrame);
    p1 = PathTraceCleanRoomTransformRigidRoutePoint(routeInstance, v1.position.xyz, previousFrame);
    p2 = PathTraceCleanRoomTransformRigidRoutePoint(routeInstance, v2.position.xyz, previousFrame);
    uv0 = v0.texCoord.xy;
    uv1 = v1.texCoord.xy;
    uv2 = v2.texCoord.xy;
    return true;
}

bool PathTraceCleanRoomResolveTriangleNormalAndArea(
    float3 p0,
    float3 p1,
    float3 p2,
    float3 payloadNormal,
    out float3 triangleNormal,
    out float triangleArea)
{
    triangleNormal = float3(0.0, 0.0, 1.0);
    triangleArea = 0.0;

    if (!all(p0 == p0) || !all(p1 == p1) || !all(p2 == p2) ||
        any(abs(p0) >= float3(3.402823e+38, 3.402823e+38, 3.402823e+38)) ||
        any(abs(p1) >= float3(3.402823e+38, 3.402823e+38, 3.402823e+38)) ||
        any(abs(p2) >= float3(3.402823e+38, 3.402823e+38, 3.402823e+38)))
    {
        return false;
    }

    const float3 edge0 = p1 - p0;
    const float3 edge1 = p2 - p0;
    const float3 crossValue = cross(edge0, edge1);
    const float doubleAreaSquared = dot(crossValue, crossValue);
    if (doubleAreaSquared <= 1.0e-12)
    {
        return false;
    }

    const float doubleArea = sqrt(doubleAreaSquared);
    triangleNormal = crossValue / doubleArea;
    const float3 safePayloadNormal = PathTraceCleanRoomSafeNormalize(payloadNormal, triangleNormal);
    if (dot(triangleNormal, safePayloadNormal) < 0.0)
    {
        triangleNormal = -triangleNormal;
    }
    triangleArea = 0.5 * doubleArea;
    return triangleArea > 1.0e-6 && triangleArea < 3.402823e+38;
}

RAB_LightInfo PathTraceCleanRoomBuildRluLightInfo(PathTraceUnifiedLightRecord light, uint lightIndex, bool previousFrame)
{
    RAB_LightInfo lightInfo = RAB_EmptyLightInfo();
    if (PathTraceCleanRoomMixedRluDomainEnabled() &&
        light.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC &&
        light.sourceIndex != PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX)
    {
        const uint sourceIndex = light.sourceIndex;
        if (previousFrame)
        {
            // Remix temporal replay loads previous-domain lights by dense previous
            // index. The RLU previous payload is authoritative here; sourceIndex
            // remains only a diagnostic/source-local handle.
            if (!PathTraceCleanRoomRluPayloadValid(light))
            {
                return lightInfo;
            }
        }
        else
        {
            if (sourceIndex >= CleanRtxdiDiDoomAnalyticFullCurrentCount)
            {
                return RAB_EmptyLightInfo();
            }
            return PathTraceCleanRoomBuildAnalyticLightInfo(DoomAnalyticLights[sourceIndex], lightIndex);
        }
    }

    if (!PathTraceCleanRoomRluPayloadValid(light))
    {
        return lightInfo;
    }

    if (light.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
    {
        PathTraceUnifiedLightRecord geometryLight = light;
        if (previousFrame && light.instanceId >= 2u && lightIndex < CleanRtxdiDiRluPreviousToCurrentCount)
        {
            const uint currentLightIndex = CleanRtxdiDiRluPreviousToCurrent[lightIndex];
            if (currentLightIndex < CleanRtxdiDiRluCurrentLightCount)
            {
                const PathTraceUnifiedLightRecord currentLight = CleanRtxdiDiRluCurrentLights[currentLightIndex];
                if (currentLight.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE && currentLight.instanceId >= 2u)
                {
                    geometryLight = currentLight;
                }
            }
        }

        const uint sourceCount = previousFrame ? CleanRtxdiDiPreviousEmissiveTriangleCount : CleanRtxdiDiCurrentEmissiveTriangleCount;
        if (light.sourceIndex == PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX || light.sourceIndex >= sourceCount)
        {
            return lightInfo;
        }

        PathTraceSmokeEmissiveTriangle emissiveTriangle;
        if (previousFrame)
        {
            emissiveTriangle = SmokePreviousEmissiveTriangles[light.sourceIndex];
        }
        else
        {
            emissiveTriangle = SmokeEmissiveTriangles[light.sourceIndex];
        }
        if (previousFrame &&
            light.instanceId >= 2u &&
            geometryLight.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE &&
            geometryLight.instanceId >= 2u &&
            geometryLight.sourceIndex < CleanRtxdiDiCurrentEmissiveTriangleCount)
        {
            emissiveTriangle = SmokeEmissiveTriangles[geometryLight.sourceIndex];
        }
        float3 p0;
        float3 p1;
        float3 p2;
        float2 uv0;
        float2 uv1;
        float2 uv2;
        const bool hasTriangleGeometry = PathTraceCleanRoomLoadEmissiveTriangleGeometry(
            emissiveTriangle,
            previousFrame,
            p0,
            p1,
            p2,
            uv0,
            uv1,
            uv2);
        if (!hasTriangleGeometry)
        {
            return lightInfo;
        }

        float3 triangleNormal;
        float triangleArea;
        if (!PathTraceCleanRoomResolveTriangleNormalAndArea(
            p0,
            p1,
            p2,
            light.normalAndArea.xyz,
            triangleNormal,
            triangleArea))
        {
            return lightInfo;
        }

        if (emissiveTriangle.materialIndex >= (uint)TextureInfo.z)
        {
            return lightInfo;
        }
        const PathTraceSmokeMaterial emissiveMaterial = PathTraceCleanRoomLoadSmokeMaterial(emissiveTriangle.materialIndex);
        const bool activeEmissiveMaterial =
            (emissiveMaterial.flags & RT_SMOKE_MATERIAL_EMISSIVE) != 0u &&
            (emissiveTriangle.padding0 & RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF) == 0u;
        if (!activeEmissiveMaterial)
        {
            return lightInfo;
        }

        const float3 replayRadiance = PathTraceCleanRoomTexturedEmissiveRadiance(light, previousFrame);
        if (PathTraceCleanRoomLuminance(replayRadiance) <= 0.0)
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
        lightInfo.normal = triangleNormal;
        lightInfo.radiance = replayRadiance;
        lightInfo.influenceRadius = 0.0;
        lightInfo.area = triangleArea;
        lightInfo.weight = light.sourceWeight;
        lightInfo.sourceIndex = light.sourceIndex;
        lightInfo.hasTriangleGeometry = 1u;
        lightInfo.emissiveTextureIndex = emissiveMaterial.emissiveTextureIndex;
        lightInfo.emissiveTextureWidth = emissiveMaterial.emissiveTextureWidth;
        lightInfo.emissiveTextureHeight = emissiveMaterial.emissiveTextureHeight;
        lightInfo.emissiveActiveStage = 1u;
        lightInfo.emissiveColor = max(emissiveMaterial.emissiveColor.rgb, float3(0.0, 0.0, 0.0));
        lightInfo.trianglePosition0 = p0;
        lightInfo.trianglePosition1 = p1;
        lightInfo.trianglePosition2 = p2;
        lightInfo.triangleUv0 = uv0;
        lightInfo.triangleUv1 = uv1;
        lightInfo.triangleUv2 = uv2;
        return lightInfo;
    }

    lightInfo.lightType = RAB_LIGHT_TYPE_DOOM_ANALYTIC_SPHERE;
    lightInfo.lightIndex = lightIndex;
    lightInfo.unifiedLightType = PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC;
    lightInfo.materialIndex = RAB_INVALID_LIGHT_INDEX;
    lightInfo.flags = light.flags;
    lightInfo.position = light.positionAndRadius.xyz;
    lightInfo.radius = max(light.positionAndRadius.w, 0.01);
    lightInfo.normal = float3(0.0, 0.0, 1.0);
    lightInfo.radiance = max(light.radianceAndLuminance.rgb, float3(0.0, 0.0, 0.0)) * max(CleanRtxdiDiDoomAnalyticLightInfo.z, 0.0);
    lightInfo.influenceRadius = max(light.uvOrDoomParams.x, lightInfo.radius);
    lightInfo.area = max(light.uvOrDoomParams.y, 1.0e-4);
    lightInfo.weight = PathTraceCleanRoomLuminance(lightInfo.radiance) * lightInfo.area * lightInfo.influenceRadius;
    return lightInfo;
}

RAB_LightInfo PathTraceCleanRoomBuildSyntheticConstantLightInfo()
{
    RAB_LightInfo lightInfo = RAB_EmptyLightInfo();
    lightInfo.lightType = RAB_LIGHT_TYPE_DOOM_ANALYTIC_SPHERE;
    lightInfo.lightIndex = 0u;
    lightInfo.unifiedLightType = PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC;
    lightInfo.materialIndex = RAB_INVALID_LIGHT_INDEX;
    const float3 forward = PathTraceCleanRoomSafeNormalize(CleanRtxdiDiCameraForwardAndTanX.xyz, float3(1.0, 0.0, 0.0));
    lightInfo.position = CleanRtxdiDiCameraOriginAndValid.xyz + forward * 96.0;
    lightInfo.radiance = CLEAN_SYNTHETIC_CONSTANT_RADIANCE;
    lightInfo.normal = float3(0.0, 0.0, 1.0);
    lightInfo.radius = 12.0;
    lightInfo.influenceRadius = 220.0;
    lightInfo.area = 4.0 * CLEAN_RTXDI_PI * lightInfo.radius * lightInfo.radius;
    lightInfo.weight = PathTraceCleanRoomLuminance(lightInfo.radiance) * lightInfo.area * lightInfo.influenceRadius;
    return lightInfo;
}

uint PathTraceCleanRoomFindFixedAnalyticLightIndex(uint requestedSampleableIndex)
{
    const uint lightCount = min(CleanRtxdiDiAnalyticLightCount, CleanRtxdiDiAnalyticIdentityCount);
    const uint firstSampleableIndex = (uint)clamp(CleanRtxdiDiRestirPTSurfaceInfo.x, 0.0, 64.0);
    uint sampleableIndex = 0u;
    for (uint index = 0u; index < lightCount; ++index)
    {
        if (PathTraceCleanRoomAnalyticIdentitySampleable(DoomAnalyticCurrentIdentities[index]) &&
            PathTraceCleanRoomAnalyticPayloadValid(DoomAnalyticLights[index]))
        {
            if (sampleableIndex == firstSampleableIndex + requestedSampleableIndex)
            {
                return index;
            }
            ++sampleableIndex;
        }
    }
    return 0xffffffffu;
}

RAB_LightInfo PathTraceCleanRoomBuildSyntheticAnalyticPayloadLightInfo(uint lightIndex)
{
    const uint sourceIndex = PathTraceCleanRoomFindFixedAnalyticLightIndex(lightIndex);
    if (sourceIndex == 0xffffffffu)
    {
        return RAB_EmptyLightInfo();
    }

    const PathTraceDoomAnalyticLightCandidate light = DoomAnalyticLights[sourceIndex];
    RAB_LightInfo lightInfo = RAB_EmptyLightInfo();
    lightInfo.lightType = RAB_LIGHT_TYPE_DOOM_ANALYTIC_SPHERE;
    lightInfo.lightIndex = lightIndex;
    lightInfo.unifiedLightType = PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC;
    lightInfo.materialIndex = RAB_INVALID_LIGHT_INDEX;
    lightInfo.flags = light.flags;
    lightInfo.position = light.originAndRadius.xyz;
    lightInfo.radius = max(light.originAndRadius.w, 0.01);
    lightInfo.normal = float3(0.0, 0.0, 1.0);
    lightInfo.radiance = max(light.colorAndIntensity.rgb, float3(0.0, 0.0, 0.0)) * max(CleanRtxdiDiDoomAnalyticLightInfo.z, 0.0);
    lightInfo.influenceRadius = max(light.doomRadiusAndArea.x, lightInfo.radius);
    lightInfo.area = max(light.doomRadiusAndArea.y, 1.0e-4);
    lightInfo.weight = PathTraceCleanRoomLuminance(lightInfo.radiance) * lightInfo.area * lightInfo.influenceRadius;
    return lightInfo;
}

RAB_LightInfo PathTraceCleanRoomBuildSyntheticOverlapLightInfo(uint lightIndex)
{
    if (lightIndex >= CLEAN_SYNTHETIC_OVERLAP_LIGHT_COUNT ||
        CleanRtxdiDiCameraOriginAndValid.w < 0.5)
    {
        return RAB_EmptyLightInfo();
    }

    static const float3 overlapRadiance[CLEAN_SYNTHETIC_OVERLAP_LIGHT_COUNT] =
    {
        float3(0.55, 0.48, 0.40),
        float3(0.40, 0.52, 0.60),
        float3(0.58, 0.42, 0.52),
        float3(0.46, 0.56, 0.44)
    };
    static const float3 overlapOffsets[CLEAN_SYNTHETIC_OVERLAP_LIGHT_COUNT] =
    {
        float3(0.0, 0.0, 0.0),
        float3(18.0, 4.0, 5.0),
        float3(-14.0, 13.0, -3.0),
        float3(7.0, -15.0, 8.0)
    };

    const float3 forward = PathTraceCleanRoomSafeNormalize(CleanRtxdiDiCameraForwardAndTanX.xyz, float3(1.0, 0.0, 0.0));
    const float3 left = PathTraceCleanRoomSafeNormalize(CleanRtxdiDiCameraLeftAndTanY.xyz, float3(0.0, 1.0, 0.0));
    const float3 up = PathTraceCleanRoomSafeNormalize(CleanRtxdiDiCameraUpAndTanY.xyz, float3(0.0, 0.0, 1.0));
    const float3 offset = overlapOffsets[lightIndex];

    RAB_LightInfo lightInfo = RAB_EmptyLightInfo();
    lightInfo.lightType = RAB_LIGHT_TYPE_DOOM_ANALYTIC_SPHERE;
    lightInfo.lightIndex = lightIndex;
    lightInfo.unifiedLightType = PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC;
    lightInfo.materialIndex = RAB_INVALID_LIGHT_INDEX;
    lightInfo.position = CleanRtxdiDiCameraOriginAndValid.xyz +
        forward * (92.0 + offset.z) +
        left * offset.x +
        up * offset.y;
    lightInfo.radiance = overlapRadiance[lightIndex];
    lightInfo.normal = float3(0.0, 0.0, 1.0);
    lightInfo.radius = 12.0;
    lightInfo.influenceRadius = 220.0;
    lightInfo.area = 4.0 * CLEAN_RTXDI_PI * lightInfo.radius * lightInfo.radius;
    lightInfo.weight = PathTraceCleanRoomLuminance(lightInfo.radiance) * lightInfo.area * lightInfo.influenceRadius;
    return lightInfo;
}

bool PathTraceCleanRoomIsRoutedRigidEmissiveRecord(PathTraceUnifiedLightRecord light)
{
    return light.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE && light.instanceId >= 2u;
}

int RAB_TranslateLightIndex(uint lightIndex, bool currentToPrevious)
{
    if (PathTraceCleanRoomSyntheticMode())
    {
        return lightIndex < PathTraceCleanRoomSyntheticLightCount() ? (int)lightIndex : -1;
    }

    if (PathTraceCleanRoomRemixLightUniverseEnabled())
    {
        if (currentToPrevious)
        {
            if (lightIndex >= CleanRtxdiDiRluCurrentLightCount || lightIndex >= CleanRtxdiDiRluCurrentToPreviousCount)
            {
                return -1;
            }

            if ((CleanRtxdiDiFlags & CLEAN_RAB_DIAGNOSTIC_DISABLE_RIGID_EMISSIVE_TEMPORAL) != 0u &&
                PathTraceCleanRoomIsRoutedRigidEmissiveRecord(CleanRtxdiDiRluCurrentLights[lightIndex]))
            {
                return -1;
            }

            const uint previousIndex = CleanRtxdiDiRluCurrentToPrevious[lightIndex];
            if (previousIndex >= CleanRtxdiDiRluPreviousLightCount)
            {
                return -1;
            }

            if ((CleanRtxdiDiFlags & CLEAN_RAB_DIAGNOSTIC_DISABLE_RIGID_EMISSIVE_TEMPORAL) != 0u &&
                PathTraceCleanRoomIsRoutedRigidEmissiveRecord(CleanRtxdiDiRluPreviousLights[previousIndex]))
            {
                return -1;
            }

            return (int)previousIndex;
        }

        if (lightIndex >= CleanRtxdiDiRluPreviousLightCount || lightIndex >= CleanRtxdiDiRluPreviousToCurrentCount)
        {
            return -1;
        }

        if ((CleanRtxdiDiFlags & CLEAN_RAB_DIAGNOSTIC_DISABLE_RIGID_EMISSIVE_TEMPORAL) != 0u &&
            PathTraceCleanRoomIsRoutedRigidEmissiveRecord(CleanRtxdiDiRluPreviousLights[lightIndex]))
        {
            return -1;
        }

        const uint currentIndex = CleanRtxdiDiRluPreviousToCurrent[lightIndex];
        if (currentIndex >= CleanRtxdiDiRluCurrentLightCount)
        {
            return -1;
        }

        if ((CleanRtxdiDiFlags & CLEAN_RAB_DIAGNOSTIC_DISABLE_RIGID_EMISSIVE_TEMPORAL) != 0u &&
            PathTraceCleanRoomIsRoutedRigidEmissiveRecord(CleanRtxdiDiRluCurrentLights[currentIndex]))
        {
            return -1;
        }

        return (int)currentIndex;
    }

    if (PathTraceCleanRoomExternalPdfNeeCurrentEnabled())
    {
        const uint emissiveOffset = PathTraceCleanRoomExternalPdfNeeEmissiveOffset();
        const uint analyticIndex = PathTraceCleanRoomExternalPdfNeeToAnalyticIndex(lightIndex);
        if (analyticIndex == 0xffffffffu)
        {
            return -1;
        }

        if (currentToPrevious)
        {
            if (analyticIndex >= CleanRtxdiDiAnalyticLightCount || analyticIndex >= CleanRtxdiDiAnalyticIdentityCount)
            {
                return -1;
            }

            const PathTraceDoomAnalyticLightCandidateIdentity currentIdentity = DoomAnalyticCurrentIdentities[analyticIndex];
            const uint remapIndex = PathTraceCleanRoomAnalyticIdentityRemapIndex(currentIdentity);
            if (!PathTraceCleanRoomAnalyticIdentityValid(currentIdentity) || remapIndex >= CleanRtxdiDiAnalyticRemapCount)
            {
                return -1;
            }

            const PathTraceDoomAnalyticLightRemap remap = DoomAnalyticRemap[remapIndex];
            if (!PathTraceCleanRoomAnalyticRemapValid(remap) || remap.currentToPreviousCandidateIndex < 0)
            {
                return -1;
            }

            const uint previousIndex = (uint)remap.currentToPreviousCandidateIndex;
            return previousIndex < CleanRtxdiDiPreviousAnalyticIdentityCount ? (int)(emissiveOffset + previousIndex) : -1;
        }

        if (analyticIndex >= CleanRtxdiDiPreviousAnalyticLightCount || analyticIndex >= CleanRtxdiDiPreviousAnalyticIdentityCount)
        {
            return -1;
        }

        const PathTraceDoomAnalyticLightCandidateIdentity previousIdentity = DoomAnalyticPreviousIdentities[analyticIndex];
        const uint remapIndex = PathTraceCleanRoomAnalyticIdentityRemapIndex(previousIdentity);
        if (!PathTraceCleanRoomAnalyticIdentityValid(previousIdentity) || remapIndex >= CleanRtxdiDiAnalyticRemapCount)
        {
            return -1;
        }

        const PathTraceDoomAnalyticLightRemap remap = DoomAnalyticRemap[remapIndex];
        if (!PathTraceCleanRoomAnalyticRemapValid(remap) || remap.previousToCurrentCandidateIndex < 0)
        {
            return -1;
        }

        const uint currentIndex = (uint)remap.previousToCurrentCandidateIndex;
        return currentIndex < CleanRtxdiDiAnalyticLightCount && currentIndex < CleanRtxdiDiAnalyticIdentityCount ? (int)(emissiveOffset + currentIndex) : -1;
    }

    if (currentToPrevious)
    {
        if (lightIndex >= CleanRtxdiDiAnalyticLightCount || lightIndex >= CleanRtxdiDiAnalyticIdentityCount)
        {
            return -1;
        }

        const PathTraceDoomAnalyticLightCandidateIdentity currentIdentity = DoomAnalyticCurrentIdentities[lightIndex];
        const uint remapIndex = PathTraceCleanRoomAnalyticIdentityRemapIndex(currentIdentity);
        if (!PathTraceCleanRoomAnalyticIdentityValid(currentIdentity) || remapIndex >= CleanRtxdiDiAnalyticRemapCount)
        {
            return -1;
        }

        const PathTraceDoomAnalyticLightRemap remap = DoomAnalyticRemap[remapIndex];
        if (!PathTraceCleanRoomAnalyticRemapValid(remap) || remap.currentToPreviousCandidateIndex < 0)
        {
            return -1;
        }

        const uint previousIndex = (uint)remap.currentToPreviousCandidateIndex;
        return previousIndex < CleanRtxdiDiPreviousAnalyticIdentityCount ? (int)previousIndex : -1;
    }

    if (lightIndex >= CleanRtxdiDiPreviousAnalyticLightCount || lightIndex >= CleanRtxdiDiPreviousAnalyticIdentityCount)
    {
        return -1;
    }

    const PathTraceDoomAnalyticLightCandidateIdentity previousIdentity = DoomAnalyticPreviousIdentities[lightIndex];
    const uint remapIndex = PathTraceCleanRoomAnalyticIdentityRemapIndex(previousIdentity);
    if (!PathTraceCleanRoomAnalyticIdentityValid(previousIdentity) || remapIndex >= CleanRtxdiDiAnalyticRemapCount)
    {
        return -1;
    }

    const PathTraceDoomAnalyticLightRemap remap = DoomAnalyticRemap[remapIndex];
    if (!PathTraceCleanRoomAnalyticRemapValid(remap) || remap.previousToCurrentCandidateIndex < 0)
    {
        return -1;
    }

    const uint currentIndex = (uint)remap.previousToCurrentCandidateIndex;
    return currentIndex < CleanRtxdiDiAnalyticLightCount && currentIndex < CleanRtxdiDiAnalyticIdentityCount ? (int)currentIndex : -1;
}

RAB_LightInfo RAB_LoadLightInfo(uint lightIndex, bool previousFrame)
{
    if (PathTraceCleanRoomSyntheticConstantMode())
    {
        if (lightIndex == 0u)
        {
            return PathTraceCleanRoomBuildSyntheticConstantLightInfo();
        }
        return RAB_EmptyLightInfo();
    }
    if (PathTraceCleanRoomSyntheticAnalyticPayloadMode())
    {
        if (lightIndex < PathTraceCleanRoomSyntheticLightCount())
        {
            return PathTraceCleanRoomBuildSyntheticAnalyticPayloadLightInfo(lightIndex);
        }
        return RAB_EmptyLightInfo();
    }
    if (PathTraceCleanRoomSyntheticOverlapMode())
    {
        return PathTraceCleanRoomBuildSyntheticOverlapLightInfo(lightIndex);
    }

    if (PathTraceCleanRoomRemixLightUniverseEnabled())
    {
        if (previousFrame)
        {
            if (lightIndex >= CleanRtxdiDiRluPreviousLightCount)
            {
                return RAB_EmptyLightInfo();
            }
            return PathTraceCleanRoomBuildRluLightInfo(CleanRtxdiDiRluPreviousLights[lightIndex], lightIndex, true);
        }

        if (lightIndex >= CleanRtxdiDiRluCurrentLightCount)
        {
            return RAB_EmptyLightInfo();
        }
        return PathTraceCleanRoomBuildRluLightInfo(CleanRtxdiDiRluCurrentLights[lightIndex], lightIndex, false);
    }

    if (PathTraceCleanRoomExternalPdfNeeCurrentEnabled())
    {
        const uint analyticIndex = PathTraceCleanRoomExternalPdfNeeToAnalyticIndex(lightIndex);
        if (analyticIndex == 0xffffffffu)
        {
            return RAB_EmptyLightInfo();
        }

        if (previousFrame)
        {
            if (analyticIndex >= CleanRtxdiDiPreviousAnalyticLightCount)
            {
                return RAB_EmptyLightInfo();
            }
            return PathTraceCleanRoomBuildAnalyticLightInfo(DoomAnalyticPreviousLights[analyticIndex], lightIndex);
        }

        if (analyticIndex >= CleanRtxdiDiAnalyticLightCount)
        {
            return RAB_EmptyLightInfo();
        }
        return PathTraceCleanRoomBuildAnalyticLightInfo(DoomAnalyticLights[analyticIndex], lightIndex);
    }

    if (previousFrame)
    {
        if (lightIndex >= CleanRtxdiDiPreviousAnalyticLightCount)
        {
            return RAB_EmptyLightInfo();
        }
        return PathTraceCleanRoomBuildAnalyticLightInfo(DoomAnalyticPreviousLights[lightIndex], lightIndex);
    }

    if (lightIndex >= CleanRtxdiDiAnalyticLightCount)
    {
        return RAB_EmptyLightInfo();
    }
    return PathTraceCleanRoomBuildAnalyticLightInfo(DoomAnalyticLights[lightIndex], lightIndex);
}

float PathTraceCleanRoomTraceVisibility(PathTracePrimarySurfaceRecord surface, float3 samplePosition);

float PathTraceCleanRoomAnalyticTargetPdf(PathTracePrimarySurfaceRecord surface, PathTraceDoomAnalyticLightCandidate light)
{
    const float3 toLight = light.originAndRadius.xyz - surface.worldPositionAndViewDepth.xyz;
    const float distanceSquared = max(dot(toLight, toLight), 1.0e-6);
    const float3 lightDirection = toLight * rsqrt(distanceSquared);
    const float3 normal = normalize(surface.shadingNormalAndOpacity.xyz);
    const float ndotl = saturate(dot(normal, lightDirection));
    const float3 radiance = max(light.colorAndIntensity.rgb, float3(0.0, 0.0, 0.0));
    return dot(radiance * (ndotl * (1.0 / CLEAN_RTXDI_PI)), float3(0.2126, 0.7152, 0.0722));
}

float PathTraceCleanRoomAnalyticTargetPdfFromSample(PathTracePrimarySurfaceRecord surface, float3 samplePosition, float3 sampleRadiance)
{
    const float3 toSample = samplePosition - surface.worldPositionAndViewDepth.xyz;
    const float distanceSquared = max(dot(toSample, toSample), 1.0e-6);
    const float3 lightDirection = toSample * rsqrt(distanceSquared);
    const float3 normal = PathTraceCleanRoomSafeNormalize(surface.shadingNormalAndOpacity.xyz, surface.geometricNormalAndRoughness.xyz);
    const float ndotl = saturate(dot(normal, lightDirection));
    const float3 flatDiffuse = float3(0.5, 0.5, 0.5) * (1.0 / CLEAN_RTXDI_PI);
    return PathTraceCleanRoomLuminance(flatDiffuse * max(sampleRadiance, float3(0.0, 0.0, 0.0)) * ndotl);
}

float PathTraceCleanRoomSphereHitT(float3 origin, float3 direction, float3 center, float radius, float fallbackT)
{
    const float3 originToCenter = origin - center;
    const float halfB = dot(originToCenter, direction);
    const float c = dot(originToCenter, originToCenter) - radius * radius;
    const float discriminant = halfB * halfB - c;
    if (discriminant <= 0.0)
    {
        return fallbackT;
    }

    const float t = -halfB - sqrt(discriminant);
    return t > 0.01 ? t : fallbackT;
}

float PathTraceCleanRoomDoomLightInfluence(float centerDistance, PathTraceDoomAnalyticLightCandidate light)
{
    const float influenceRadius = max(light.doomRadiusAndArea.x, light.originAndRadius.w);
    if (influenceRadius <= 0.0 || centerDistance >= influenceRadius)
    {
        return 0.0;
    }

    const float normalizedDistance = saturate(centerDistance / influenceRadius);
    return saturate(1.0 - normalizedDistance * normalizedDistance);
}

bool RAB_GetConservativeVisibility(RAB_Surface surface, RAB_LightSample lightSample)
{
    PathTracePrimarySurfaceRecord record = (PathTracePrimarySurfaceRecord)0;
    record.header = uint4(RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_VERSION, RT_PRIMARY_SURFACE_VALID, 0u, 0u);
    record.worldPositionAndViewDepth = float4(surface.worldPos, surface.linearDepth);
    record.geometricNormalAndRoughness = float4(surface.geometryNormal, 0.5);
    record.shadingNormalAndOpacity = float4(surface.shadingNormal, 1.0);
    record.viewDirectionAndReserved = float4(surface.viewDir, 0.0);
    return PathTraceCleanRoomTraceVisibility(record, lightSample.position) > 0.0;
}

float PathTraceCleanRoomTraceVisibilityWithIgnore(
    PathTracePrimarySurfaceRecord surface,
    float3 samplePosition,
    uint ignoreInstanceId,
    uint ignorePrimitiveIndex,
    uint ignoreMaterialIndex);

bool RAB_TraceRayForLocalLight(float3 rayOrigin, float3 rayDirection, float tMin, float tMax, out uint lightIndex, out float2 randXY)
{
    lightIndex = RTXDI_InvalidLightIndex;
    randXY = float2(0.0, 0.0);
    return false;
}

float RAB_EvaluateLocalLightSourcePdf(uint lightIndex)
{
    return 0.0;
}

float2 RAB_GetEnvironmentMapRandXYFromDir(float3 sampleDir)
{
    return float2(0.0, 0.0);
}

float RAB_EvaluateEnvironmentMapSamplingPdf(float3 sampleDir)
{
    return 0.0;
}

bool RAB_GetTemporalConservativeVisibility(RAB_Surface surface, RAB_Surface previousSurface, RAB_LightSample lightSample)
{
    if (lightSample.lightType == RAB_LIGHT_TYPE_EMISSIVE_TRIANGLE)
    {
        if ((CleanRtxdiDiFlags & CLEAN_RAB_DIAGNOSTIC_FORCE_EMISSIVE_VISIBILITY) != 0u)
        {
            return true;
        }

        if (PathTraceCleanRoomRemixLightUniverseEnabled())
        {
            const int currentLightIndex = RAB_TranslateLightIndex(lightSample.lightIndex, false);
            if (currentLightIndex >= 0 && (uint)currentLightIndex < CleanRtxdiDiRluCurrentLightCount)
            {
                const PathTraceUnifiedLightRecord light = CleanRtxdiDiRluCurrentLights[(uint)currentLightIndex];
                if (light.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
                {
                    PathTracePrimarySurfaceRecord record = (PathTracePrimarySurfaceRecord)0;
                    record.header = uint4(RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_VERSION, RT_PRIMARY_SURFACE_VALID, 0u, 0u);
                    record.worldPositionAndViewDepth = float4(surface.worldPos, surface.linearDepth);
                    record.geometricNormalAndRoughness = float4(surface.geometryNormal, 0.5);
                    record.shadingNormalAndOpacity = float4(surface.shadingNormal, 1.0);
                    record.viewDirectionAndReserved = float4(surface.viewDir, 0.0);

                    const bool historicalDynamicEmissive = (light.flags & RT_SMOKE_EMISSIVE_TRIANGLE_HISTORY_DYNAMIC) != 0u;
                    if (!historicalDynamicEmissive && light.instanceId >= 2u)
                    {
                        const uint routeInstanceIndex = light.instanceId - 2u;
                        const uint rigidRouteInstanceCount = (uint)max(ToyPathInfo.w, 0.0);
                        if (routeInstanceIndex < rigidRouteInstanceCount)
                        {
                            const PathTraceRigidRouteInstance routeInstance = SmokeRigidRouteInstances[routeInstanceIndex];
                            const uint continuousFlags = PT_RIGID_ROUTE_HAS_PREVIOUS_TRANSFORM | PT_RIGID_ROUTE_TRANSFORM_CONTINUOUS;
                            if ((routeInstance.flags & continuousFlags) == continuousFlags)
                            {
                                return true;
                            }
                        }
                    }

                    const uint ignoreInstanceId = historicalDynamicEmissive ? 0xffffffffu : light.instanceId;
                    const uint ignorePrimitiveIndex = historicalDynamicEmissive ? 0xffffffffu : light.primitiveIndex;
                    return PathTraceCleanRoomTraceVisibilityWithIgnore(
                        record,
                        lightSample.position,
                        ignoreInstanceId,
                        ignorePrimitiveIndex,
                        light.materialOrLightId) > 0.0;
                }
            }
        }
    }

    return RAB_GetConservativeVisibility(surface, lightSample);
}

#include "Rtxdi/DI/InitialSampling.hlsli"
#include "Rtxdi/DI/TemporalResampling.hlsli"

bool PathTraceCleanRoomTryStoredReservoirVisibility(RTXDI_DIReservoir reservoir, out float visibility)
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

float PathTraceCleanRoomTraceVisibilityWithIgnore(
    PathTracePrimarySurfaceRecord surface,
    float3 samplePosition,
    uint ignoreInstanceId,
    uint ignorePrimitiveIndex,
    uint ignoreMaterialIndex)
{
    const float3 surfacePosition = surface.worldPositionAndViewDepth.xyz;
    const float3 toSample = samplePosition - surfacePosition;
    const float distanceSquared = dot(toSample, toSample);
    if (distanceSquared <= 1.0e-6)
    {
        return 0.0;
    }

    const float distance = sqrt(distanceSquared);
    const float3 direction = toSample / distance;
    const float3 shadingNormal = PathTraceCleanRoomSafeNormalize(surface.shadingNormalAndOpacity.xyz, surface.geometricNormalAndRoughness.xyz);
    const float3 geometricNormal = PathTraceCleanRoomSafeNormalize(surface.geometricNormalAndRoughness.xyz, shadingNormal);
    if (dot(shadingNormal, direction) <= 0.0 || dot(geometricNormal, direction) <= 0.0)
    {
        return 0.0;
    }

    const float normalSign = dot(geometricNormal, shadingNormal) >= 0.0 ? 1.0 : -1.0;
    RayDesc shadowRay;
    shadowRay.Origin = surfacePosition + geometricNormal * (normalSign * 0.75) + direction * 0.25;
    shadowRay.Direction = direction;
    shadowRay.TMin = 0.01;
    shadowRay.TMax = max(distance - 0.5, 0.01);

    PathTraceCleanRtxdiPayload shadowPayload;
    shadowPayload.value = 0u;
    shadowPayload.rayMode = ignoreInstanceId != 0xffffffffu ? 2u : 1u;
    shadowPayload.ignoreInstanceId = ignoreInstanceId;
    shadowPayload.ignorePrimitiveIndex = ignorePrimitiveIndex;
    shadowPayload.ignoreMaterialIndex = ignoreMaterialIndex;
    const uint rayFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_NON_OPAQUE;
    TraceRay(
        SmokeScene,
        rayFlags,
        0xff,
        1,
        1,
        1,
        shadowRay,
        shadowPayload);

    return shadowPayload.value == 0u ? 1.0 : 0.0;
}

bool PathTraceCleanRoomMaterialDoesNotOccludeVisibility(uint materialIndex)
{
    if (materialIndex >= (uint)TextureInfo.z)
    {
        return false;
    }

    const PathTraceSmokeMaterial material = PathTraceCleanRoomLoadSmokeMaterial(materialIndex);
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

float PathTraceCleanRoomTraceVisibility(PathTracePrimarySurfaceRecord surface, float3 samplePosition)
{
    return PathTraceCleanRoomTraceVisibilityWithIgnore(surface, samplePosition, 0xffffffffu, 0xffffffffu, 0xffffffffu);
}

float PathTraceCleanRoomSelectedSampleVisibility(PathTracePrimarySurfaceRecord surface, RTXDI_DIReservoir reservoir, RAB_LightSample lightSample)
{
    if (lightSample.lightType == RAB_LIGHT_TYPE_EMISSIVE_TRIANGLE)
    {
        if ((CleanRtxdiDiFlags & CLEAN_RAB_DIAGNOSTIC_FORCE_EMISSIVE_VISIBILITY) != 0u)
        {
            return 1.0;
        }

        if (PathTraceCleanRoomExternalPdfNeeCurrentEnabled() || PathTraceCleanRoomNeeCacheProviderEnabled())
        {
            float storedVisibility = 0.0;
            if (PathTraceCleanRoomTryStoredReservoirVisibility(reservoir, storedVisibility))
            {
                if (storedVisibility > 1.0e-8)
                {
                    return storedVisibility;
                }
            }
        }

        const uint lightIndex = RTXDI_GetDIReservoirLightIndex(reservoir);
        if (lightIndex < CleanRtxdiDiRluCurrentLightCount)
        {
            const PathTraceUnifiedLightRecord light = CleanRtxdiDiRluCurrentLights[lightIndex];
            if (light.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
            {
                const bool historicalDynamicEmissive = (light.flags & RT_SMOKE_EMISSIVE_TRIANGLE_HISTORY_DYNAMIC) != 0u;
                const uint ignoreInstanceId = historicalDynamicEmissive ? 0xffffffffu : light.instanceId;
                const uint ignorePrimitiveIndex = historicalDynamicEmissive ? 0xffffffffu : light.primitiveIndex;
                return PathTraceCleanRoomTraceVisibilityWithIgnore(
                    surface,
                    lightSample.position,
                    ignoreInstanceId,
                    ignorePrimitiveIndex,
                    light.materialOrLightId);
            }
        }
    }

    return PathTraceCleanRoomTraceVisibility(surface, lightSample.position);
}

uint PathTraceCleanRoomReservoirIndex(uint2 pixel, uint2 dimensions)
{
    return PathTraceCleanRoomReservoirPointer(pixel, dimensions);
}

float3 PathTraceCleanRoomStatusColor(uint status)
{
    if (status == CLEAN_INITIAL_STATUS_NO_LIGHT_MODE)
    {
        return float3(1.0, 0.0, 1.0);
    }
    if (status == CLEAN_INITIAL_STATUS_DEFERRED_LIGHT_MODE)
    {
        return float3(1.0, 0.35, 0.0);
    }
    if (status == CLEAN_INITIAL_STATUS_NO_LIGHTS)
    {
        return float3(0.95, 0.0, 0.12);
    }
    if (status == CLEAN_INITIAL_STATUS_INVALID_SURFACE)
    {
        return float3(0.18, 0.0, 0.22);
    }
    if (status == CLEAN_INITIAL_STATUS_INVALID_IDENTITY)
    {
        return float3(1.0, 0.82, 0.0);
    }
    if (status == CLEAN_INITIAL_STATUS_INVALID_PAYLOAD)
    {
        return float3(1.0, 0.18, 0.0);
    }
    if (status == CLEAN_INITIAL_STATUS_ZERO_TARGET_PDF)
    {
        return float3(0.08, 0.10, 1.0);
    }
    if (status == CLEAN_INITIAL_STATUS_BAD_SAMPLE_PDF)
    {
        return float3(0.0, 0.95, 1.0);
    }
    if (status == CLEAN_INITIAL_STATUS_EXTERNAL_CURRENT_EMPTY)
    {
        return float3(0.0, 0.0, 0.0);
    }
    if (status == CLEAN_INITIAL_STATUS_EXTERNAL_UNSUPPORTED_LIGHT)
    {
        return float3(0.65, 0.0, 1.0);
    }
    return float3(0.05, 0.90, 0.18);
}

float2 PathTraceCleanRoomRandomLightUv(inout RTXDI_RandomSamplerState rng)
{
    return float2(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng));
}

struct PathTraceCleanRoomPreviousBestSeed
{
    RTXDI_DIReservoir reservoir;
    uint sourceValid;
    uint translationValid;
    uint candidateValid;
    uint currentLightIndex;
};

bool PathTraceCleanRoomPreviousBestInitialSeedEnabled()
{
    return
        (CleanRtxdiDiFlags & CLEAN_FLAG_PREVIOUS_BEST_APPROXIMATION) != 0u &&
        (CleanRtxdiDiTemporalFlags & CLEAN_TEMPORAL_FLAG_PREVIOUS_VALID) != 0u &&
        PathTraceCleanRoomRemixLightUniverseEnabled() &&
        !PathTraceCleanRoomExternalPdfNeeCurrentEnabled() &&
        !PathTraceCleanRoomNeeCacheProviderEnabled() &&
        CleanRtxdiDiReservoirCount > 0u;
}

bool PathTraceCleanRoomInitialVisibilityEnabled()
{
    return (CleanRtxdiDiFlags & CLEAN_FLAG_INITIAL_VISIBILITY) != 0u &&
        !PathTraceCleanRoomSyntheticMode();
}

PathTraceCleanRoomPreviousBestSeed PathTraceCleanRoomResolvePreviousBestInitialSeed(
    uint2 pixel,
    uint2 dimensions,
    PathTracePrimarySurfaceRecord currentSurface,
    RAB_Surface surface,
    inout RTXDI_RandomSamplerState rng)
{
    PathTraceCleanRoomPreviousBestSeed seed = (PathTraceCleanRoomPreviousBestSeed)0;
    seed.reservoir = RTXDI_EmptyDIReservoir();
    seed.currentLightIndex = 0xffffffffu;

    if (!PathTraceCleanRoomPreviousBestInitialSeedEnabled())
    {
        return seed;
    }

    float3 screenSpaceMotion;
    bool cameraReprojected = false;
    if (!PathTraceCleanRoomLoadMotion(pixel, currentSurface, dimensions, screenSpaceMotion, cameraReprojected))
    {
        return seed;
    }

    const int2 previousPixel = int2(round(float2(pixel) + screenSpaceMotion.xy));
    if (previousPixel.x < 0 ||
        previousPixel.y < 0 ||
        (uint)previousPixel.x >= dimensions.x ||
        (uint)previousPixel.y >= dimensions.y)
    {
        return seed;
    }

    const RTXDI_DIReservoir previousReservoir =
        RTXDI_LoadDIReservoir(PathTraceCleanRoomReservoirParams(dimensions), (uint2)previousPixel, 0u);
    if (!RTXDI_IsValidDIReservoir(previousReservoir) || previousReservoir.M <= 0.0)
    {
        return seed;
    }
    seed.sourceValid = 1u;

    const int translatedLightIndex =
        RAB_TranslateLightIndex(RTXDI_GetDIReservoirLightIndex(previousReservoir), false);
    if (translatedLightIndex < 0 || (uint)translatedLightIndex >= CleanRtxdiDiRluCurrentLightCount)
    {
        return seed;
    }
    seed.translationValid = 1u;
    seed.currentLightIndex = (uint)translatedLightIndex;

    const RAB_LightInfo lightInfo = RAB_LoadLightInfo(seed.currentLightIndex, false);
    if (!RAB_IsLightInfoValid(lightInfo))
    {
        return seed;
    }

    const float2 uv = RTXDI_GetDIReservoirSampleUV(previousReservoir);
    const RAB_LightSample lightSample = RAB_SamplePolymorphicLight(lightInfo, surface, uv);
    float targetPdf = RAB_IsReplayableLightSample(lightSample)
        ? max(RAB_GetLightSampleTargetPdfForSurface(lightSample, surface), 0.0)
        : 0.0;
    if (targetPdf <= 1.0e-8)
    {
        return seed;
    }

    RTXDI_StreamSample(
        seed.reservoir,
        seed.currentLightIndex,
        uv,
        RTXDI_GetNextRandom(rng),
        targetPdf,
        1.0);
    if (!RTXDI_IsValidDIReservoir(seed.reservoir))
    {
        seed.reservoir = RTXDI_EmptyDIReservoir();
        return seed;
    }

    RTXDI_FinalizeResampling(seed.reservoir, 1.0, max(seed.reservoir.M, 1.0));
    seed.reservoir.M = 1.0;
    seed.reservoir.packedVisibility = RTXDI_PackedDIReservoir_VisibilityMask;
    seed.reservoir.spatialDistance = int2(0, 0);
    seed.reservoir.age = 0u;
    seed.reservoir.canonicalWeight = 1.0;
    seed.candidateValid = 1u;
    return seed;
}

bool PathTraceCleanRoomStreamTypedRluRangeIntoReservoir(
    inout RTXDI_DIReservoir reservoir,
    inout RTXDI_RandomSamplerState rng,
    RAB_Surface surface,
    uint lightType,
    uint2 range,
    uint sampleCount,
    uint totalSampleCount,
    bool excludeLight,
    uint excludedLightIndex)
{
    if (range.y == 0u || sampleCount == 0u || totalSampleCount == 0u)
    {
        return false;
    }

    const uint boundedSampleCount = sampleCount;
    if (boundedSampleCount == 0u)
    {
        return false;
    }

    const float stride = max(1.0, (float)range.y / (float)boundedSampleCount);
    const float uniformSourcePdf = PathTraceCleanRoomRluTypedSourcePdf(lightType);
    if (uniformSourcePdf <= 0.0)
    {
        return false;
    }

    bool streamedAny = false;
    [loop]
    for (uint sampleIndex = 0u; sampleIndex < boundedSampleCount; ++sampleIndex)
    {
        const float lightIndexInRange = min(
            ((float)sampleIndex + RTXDI_GetNextRandom(rng)) * stride,
            (float)range.y - 1.0);
        uint lightIndex = range.x + min((uint)lightIndexInRange, range.y - 1u);
        float sourcePdf = uniformSourcePdf;
        if (lightType == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
        {
            uint weightedLightIndex;
            float weightedSourcePdf;
            if (PathTraceCleanRoomSelectWeightedEmissiveRluCandidate(
                range,
                boundedSampleCount,
                totalSampleCount,
                RTXDI_GetNextRandom(rng),
                weightedLightIndex,
                weightedSourcePdf))
            {
                lightIndex = weightedLightIndex;
                sourcePdf = weightedSourcePdf;
            }
        }

        const float invSourcePdf = 1.0 / max(sourcePdf, 1.0e-8);
        if (!(excludeLight && lightIndex == excludedLightIndex))
        {
            const RAB_LightInfo lightInfo = RAB_LoadLightInfo(lightIndex, false);
            float targetPdf = 0.0;
            const float2 uv = PathTraceCleanRoomRandomLightUv(rng);
            if (RAB_IsLightInfoValid(lightInfo))
            {
                const RAB_LightSample lightSample = RAB_SamplePolymorphicLight(lightInfo, surface, uv);
                targetPdf = RAB_IsReplayableLightSample(lightSample)
                    ? max(RAB_GetLightSampleTargetPdfForSurface(lightSample, surface), 0.0)
                    : 0.0;
            }

            RTXDI_StreamSample(
                reservoir,
                lightIndex,
                uv,
                RTXDI_GetNextRandom(rng),
                targetPdf,
                invSourcePdf);
            streamedAny = streamedAny || targetPdf > 0.0;
        }
    }

    return streamedAny;
}

PathTraceCleanRtxdiDiInitialResult PathTraceCleanRoomRunTypedRluInitialProducer(uint2 pixel, uint2 dimensions)
{
    PathTraceCleanRtxdiDiInitialResult result = (PathTraceCleanRtxdiDiInitialResult)0;
    result.reservoir = RTXDI_EmptyDIReservoir();
    result.selectedLightIndex = 0xffffffffu;
    result.status = CLEAN_INITIAL_STATUS_VALID;

    if (CleanRtxdiDiLightMode == 0u)
    {
        result.status = CLEAN_INITIAL_STATUS_NO_LIGHT_MODE;
        return result;
    }
    if (CleanRtxdiDiLightMode != 1u)
    {
        result.status = CLEAN_INITIAL_STATUS_DEFERRED_LIGHT_MODE;
        return result;
    }
    if (!PathTraceCleanRoomRemixLightUniverseEnabled() || CleanRtxdiDiRluCurrentLightCount == 0u)
    {
        result.status = CLEAN_INITIAL_STATUS_NO_LIGHTS;
        return result;
    }

    uint2 emissiveRange;
    uint emissiveSampleCount;
    uint totalSampleCount;
    uint nonEmptyRangeCount;
    const bool emissiveRangeUsable = PathTraceCleanRoomRluTypedProposalInfo(
        PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE,
        emissiveRange,
        emissiveSampleCount,
        totalSampleCount,
        nonEmptyRangeCount);
    uint2 doomAnalyticRange;
    uint doomAnalyticSampleCount;
    uint doomAnalyticTotalSampleCount;
    uint doomAnalyticNonEmptyRangeCount;
    const bool doomAnalyticRangeUsable = PathTraceCleanRoomRluTypedProposalInfo(
        PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC,
        doomAnalyticRange,
        doomAnalyticSampleCount,
        doomAnalyticTotalSampleCount,
        doomAnalyticNonEmptyRangeCount);

    if (!emissiveRangeUsable && !doomAnalyticRangeUsable)
    {
        result.status = CLEAN_INITIAL_STATUS_NO_LIGHTS;
        return result;
    }

    if (!PathTraceCleanRoomLoadSurfaceRecord(pixel, dimensions, result.surface))
    {
        result.status = CLEAN_INITIAL_STATUS_INVALID_SURFACE;
        return result;
    }

    const RAB_Surface surface = PathTraceCleanRoomSurfaceForView(result.surface);
    if (!RAB_IsSurfaceValid(surface))
    {
        result.status = CLEAN_INITIAL_STATUS_INVALID_SURFACE;
        return result;
    }

    RTXDI_RandomSamplerState rng =
        PathTraceCleanRoomInitDecorrelatedSampler(pixel, CleanRtxdiDiFrameIndex, 0x524c553cu);
    const PathTraceCleanRoomPreviousBestSeed previousBestSeed =
        PathTraceCleanRoomResolvePreviousBestInitialSeed(pixel, dimensions, result.surface, surface, rng);
    const bool streamedEmissive = PathTraceCleanRoomStreamTypedRluRangeIntoReservoir(
        result.reservoir,
        rng,
        surface,
        PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE,
        emissiveRange,
        emissiveSampleCount,
        totalSampleCount,
        previousBestSeed.translationValid != 0u,
        previousBestSeed.currentLightIndex);
    const bool streamedDoomAnalytic = PathTraceCleanRoomStreamTypedRluRangeIntoReservoir(
        result.reservoir,
        rng,
        surface,
        PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC,
        doomAnalyticRange,
        doomAnalyticSampleCount,
        totalSampleCount,
        previousBestSeed.translationValid != 0u,
        previousBestSeed.currentLightIndex);

    result.previousBestSourceValid = previousBestSeed.sourceValid;
    result.previousBestTranslationValid = previousBestSeed.translationValid;
    result.previousBestCandidateValid = previousBestSeed.candidateValid;

    if (!streamedEmissive && !streamedDoomAnalytic && previousBestSeed.candidateValid == 0u)
    {
        result.status = CLEAN_INITIAL_STATUS_ZERO_TARGET_PDF;
        return result;
    }

    if (RTXDI_IsValidDIReservoir(result.reservoir))
    {
        RTXDI_FinalizeResampling(result.reservoir, 1.0, max(result.reservoir.M, 1.0));
        result.reservoir.M = 1.0;
        result.reservoir.packedVisibility = RTXDI_PackedDIReservoir_VisibilityMask;
        result.reservoir.spatialDistance = int2(0, 0);
        result.reservoir.age = 0u;
        result.reservoir.canonicalWeight = 1.0;
    }

    if (previousBestSeed.candidateValid != 0u)
    {
        RTXDI_DIReservoir combinedReservoir = RTXDI_EmptyDIReservoir();
        if (RTXDI_IsValidDIReservoir(result.reservoir))
        {
            RTXDI_CombineDIReservoirs(
                combinedReservoir,
                result.reservoir,
                RTXDI_GetNextRandom(rng),
                result.reservoir.targetPdf);
        }

        const bool selectedPreviousBest = RTXDI_CombineDIReservoirs(
            combinedReservoir,
            previousBestSeed.reservoir,
            RTXDI_GetNextRandom(rng),
            previousBestSeed.reservoir.targetPdf);
        if (RTXDI_IsValidDIReservoir(combinedReservoir))
        {
            RTXDI_FinalizeResampling(combinedReservoir, 1.0, max(combinedReservoir.M, 1.0));
            combinedReservoir.M = 1.0;
            combinedReservoir.packedVisibility = RTXDI_PackedDIReservoir_VisibilityMask;
            combinedReservoir.spatialDistance = int2(0, 0);
            combinedReservoir.age = 0u;
            combinedReservoir.canonicalWeight = 1.0;
            result.reservoir = combinedReservoir;
            result.previousBestSelected = selectedPreviousBest ? 1u : 0u;
        }
    }

    result.status = RTXDI_IsValidDIReservoir(result.reservoir) ? CLEAN_INITIAL_STATUS_VALID : CLEAN_INITIAL_STATUS_ZERO_TARGET_PDF;
    if (result.status != CLEAN_INITIAL_STATUS_VALID)
    {
        return result;
    }

    result.selectedLightIndex = RTXDI_GetDIReservoirLightIndex(result.reservoir);
    if (result.selectedLightIndex >= CleanRtxdiDiRluCurrentLightCount)
    {
        result.status = CLEAN_INITIAL_STATUS_BAD_SAMPLE_PDF;
        result.reservoir = RTXDI_EmptyDIReservoir();
        return result;
    }

    result.sampleUv = RTXDI_GetDIReservoirSampleUV(result.reservoir);
    const RAB_LightInfo selectedLightInfo = RAB_LoadLightInfo(result.selectedLightIndex, false);
    const RAB_LightSample selectedSample = RAB_SamplePolymorphicLight(selectedLightInfo, surface, result.sampleUv);
    if (!RAB_IsLightInfoValid(selectedLightInfo) ||
        !RAB_IsReplayableLightSample(selectedSample) ||
        selectedSample.solidAnglePdf <= 1.0e-8)
    {
        result.status = CLEAN_INITIAL_STATUS_BAD_SAMPLE_PDF;
        result.reservoir = RTXDI_EmptyDIReservoir();
        return result;
    }

    result.samplePosition = selectedSample.position;
    result.sampleRadiance = selectedSample.radiance;
    result.solidAnglePdf = selectedSample.solidAnglePdf;
    result.targetPdf = result.reservoir.targetPdf;
    result.invSourcePdf = RTXDI_GetDIReservoirInvPdf(result.reservoir);
    result.visibility = PathTraceCleanRoomSelectedSampleVisibility(result.surface, result.reservoir, selectedSample);
    RTXDI_StoreVisibilityInDIReservoir(
        result.reservoir,
        result.visibility.xxx,
        PathTraceCleanRoomInitialVisibilityEnabled());
    return result;
}

PathTraceCleanRtxdiDiInitialResult PathTraceCleanRoomRunInitialProducer(uint2 pixel, uint2 dimensions)
{
    PathTraceCleanRtxdiDiInitialResult result = (PathTraceCleanRtxdiDiInitialResult)0;
    result.reservoir = RTXDI_EmptyDIReservoir();
    result.selectedLightIndex = 0xffffffffu;
    result.status = CLEAN_INITIAL_STATUS_VALID;

    const bool syntheticMode = PathTraceCleanRoomSyntheticMode();
    if (!syntheticMode && PathTraceCleanRoomRemixLightUniverseEnabled())
    {
        return PathTraceCleanRoomRunTypedRluInitialProducer(pixel, dimensions);
    }
    if (!syntheticMode && CleanRtxdiDiLightMode == 0u)
    {
        result.status = CLEAN_INITIAL_STATUS_NO_LIGHT_MODE;
        return result;
    }
    if (!syntheticMode && CleanRtxdiDiLightMode != 1u)
    {
        result.status = CLEAN_INITIAL_STATUS_DEFERRED_LIGHT_MODE;
        return result;
    }

    RTXDI_LightBufferRegion localLightRegion = (RTXDI_LightBufferRegion)0;
    if (syntheticMode)
    {
        localLightRegion.firstLightIndex = 0u;
        localLightRegion.numLights = PathTraceCleanRoomSyntheticLightCount();
    }
    else
    {
        PathTraceCleanRoomInitialLightRegion(localLightRegion);
    }
    if (localLightRegion.numLights == 0u)
    {
        result.status = CLEAN_INITIAL_STATUS_NO_LIGHTS;
        return result;
    }

    if (!PathTraceCleanRoomLoadSurfaceRecord(pixel, dimensions, result.surface))
    {
        result.status = CLEAN_INITIAL_STATUS_INVALID_SURFACE;
        return result;
    }

    const RAB_Surface surface = PathTraceCleanRoomSurfaceForView(result.surface);
    if (!RAB_IsSurfaceValid(surface))
    {
        result.status = CLEAN_INITIAL_STATUS_INVALID_SURFACE;
        return result;
    }

    RTXDI_DIInitialSamplingParameters sampleParams = (RTXDI_DIInitialSamplingParameters)0;
    sampleParams.numLocalLightSamples = syntheticMode
        ? 1u
        : max(CleanRtxdiDiCandidateCount, 1u);
    sampleParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode_UNIFORM;
    sampleParams.enableInitialVisibility = 0u;

    RTXDI_InitialSamplingMisData misData = RTXDI_ComputeInitialSamplingMisData(sampleParams);
    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(pixel, CleanRtxdiDiFrameIndex, 0x4d534449u);
    RTXDI_RandomSamplerState coherentRng = RTXDI_InitRandomSampler(pixel / RTXDI_TILE_SIZE_IN_PIXELS, CleanRtxdiDiFrameIndex, 0x4d534449u);
    RAB_LightSample selectedSample = RAB_EmptyLightSample();
    result.reservoir = RTXDI_SampleLocalLights(
        rng,
        coherentRng,
        surface,
        sampleParams,
        misData,
        ReSTIRDI_LocalLightSamplingMode_UNIFORM,
        localLightRegion,
        selectedSample);

    result.status = RTXDI_IsValidDIReservoir(result.reservoir) ? CLEAN_INITIAL_STATUS_VALID : CLEAN_INITIAL_STATUS_ZERO_TARGET_PDF;
    if (result.status != CLEAN_INITIAL_STATUS_VALID)
    {
        return result;
    }

    result.selectedLightIndex = RTXDI_GetDIReservoirLightIndex(result.reservoir);
    result.sampleUv = RTXDI_GetDIReservoirSampleUV(result.reservoir);
    result.samplePosition = selectedSample.position;
    result.sampleRadiance = selectedSample.radiance;
    result.solidAnglePdf = selectedSample.solidAnglePdf;
    result.targetPdf = result.reservoir.targetPdf;
    result.invSourcePdf = RTXDI_GetDIReservoirInvPdf(result.reservoir);
    if ((!syntheticMode && !PathTraceCleanRoomLightIndexInRegion(result.selectedLightIndex, localLightRegion)) ||
        selectedSample.valid == 0u || selectedSample.solidAnglePdf <= 1.0e-8)
    {
        result.status = CLEAN_INITIAL_STATUS_BAD_SAMPLE_PDF;
        result.reservoir = RTXDI_EmptyDIReservoir();
        return result;
    }

    if (!syntheticMode && !PathTraceCleanRoomRemixLightUniverseEnabled())
    {
        result.light = DoomAnalyticLights[result.selectedLightIndex];
    }
    result.visibility = PathTraceCleanRoomSyntheticOverlapMode()
        ? PathTraceCleanRoomTraceVisibility(result.surface, result.samplePosition)
        : (syntheticMode ? 1.0 : PathTraceCleanRoomTraceVisibility(result.surface, result.samplePosition));
    RTXDI_StoreVisibilityInDIReservoir(
        result.reservoir,
        result.visibility.xxx,
        PathTraceCleanRoomInitialVisibilityEnabled());
    return result;
}

PathTraceCleanRtxdiDiInitialResult PathTraceCleanRoomRunExternalPdfNeeCurrentProducer(uint2 pixel, uint2 dimensions)
{
    PathTraceCleanRtxdiDiInitialResult result = (PathTraceCleanRtxdiDiInitialResult)0;
    result.reservoir = RTXDI_EmptyDIReservoir();
    result.selectedLightIndex = 0xffffffffu;
    result.status = CLEAN_INITIAL_STATUS_VALID;

    if (CleanRtxdiDiLightMode != 1u)
    {
        result.status = CLEAN_INITIAL_STATUS_DEFERRED_LIGHT_MODE;
        return result;
    }

    if (!PathTraceCleanRoomLoadSurfaceRecord(pixel, dimensions, result.surface))
    {
        result.status = CLEAN_INITIAL_STATUS_INVALID_SURFACE;
        return result;
    }

    const RAB_Surface surface = PathTraceCleanRoomSurfaceForView(result.surface);
    if (!RAB_IsSurfaceValid(surface))
    {
        result.status = CLEAN_INITIAL_STATUS_INVALID_SURFACE;
        return result;
    }

    const uint reservoirIndex = PathTraceCleanRoomReservoirIndex(pixel, dimensions);
    if (reservoirIndex >= CleanRtxdiDiReservoirCount)
    {
        result.status = CLEAN_INITIAL_STATUS_EXTERNAL_CURRENT_EMPTY;
        return result;
    }

    result.reservoir = RTXDI_UnpackDIReservoir(CleanRtxdiDiCurrentReservoirs[reservoirIndex]);
    if (!RTXDI_IsValidDIReservoir(result.reservoir) || result.reservoir.M <= 0.0)
    {
        result.reservoir = RTXDI_EmptyDIReservoir();
        result.status = CLEAN_INITIAL_STATUS_EXTERNAL_CURRENT_EMPTY;
        return result;
    }

    result.selectedLightIndex = RTXDI_GetDIReservoirLightIndex(result.reservoir);
    if (PathTraceCleanRoomRemixLightUniverseEnabled())
    {
        if (result.selectedLightIndex >= CleanRtxdiDiRluCurrentLightCount)
        {
            result.reservoir = RTXDI_EmptyDIReservoir();
            result.status = CLEAN_INITIAL_STATUS_EXTERNAL_UNSUPPORTED_LIGHT;
            return result;
        }
    }
    else if (PathTraceCleanRoomExternalPdfNeeToAnalyticIndex(result.selectedLightIndex) == 0xffffffffu)
    {
        result.reservoir = RTXDI_EmptyDIReservoir();
        result.status = CLEAN_INITIAL_STATUS_EXTERNAL_UNSUPPORTED_LIGHT;
        return result;
    }

    const RAB_LightInfo lightInfo = RAB_LoadLightInfo(result.selectedLightIndex, false);
    if (!RAB_IsLightInfoValid(lightInfo))
    {
        result.reservoir = RTXDI_EmptyDIReservoir();
        result.status = CLEAN_INITIAL_STATUS_EXTERNAL_UNSUPPORTED_LIGHT;
        return result;
    }

    result.sampleUv = RTXDI_GetDIReservoirSampleUV(result.reservoir);
    const RAB_LightSample selectedSample = RAB_SamplePolymorphicLight(lightInfo, surface, result.sampleUv);
    if (!RAB_IsReplayableLightSample(selectedSample))
    {
        result.reservoir = RTXDI_EmptyDIReservoir();
        result.status = CLEAN_INITIAL_STATUS_BAD_SAMPLE_PDF;
        return result;
    }

    result.samplePosition = selectedSample.position;
    result.sampleRadiance = selectedSample.radiance;
    result.solidAnglePdf = selectedSample.solidAnglePdf;
    result.targetPdf = result.reservoir.targetPdf;
    result.invSourcePdf = RTXDI_GetDIReservoirInvPdf(result.reservoir);
    result.visibility = PathTraceCleanRoomSelectedSampleVisibility(result.surface, result.reservoir, selectedSample);
    RTXDI_StoreVisibilityInDIReservoir(
        result.reservoir,
        result.visibility.xxx,
        PathTraceCleanRoomInitialVisibilityEnabled());
    return result;
}

#include "pathtrace_clean_rtxdi_di_nee_cache.hlsli"
PathTraceCleanRtxdiDiInitialResult PathTraceCleanRoomRunSelectedInitialProducer(uint2 pixel, uint2 dimensions)
{
    if (PathTraceCleanRoomExternalPdfNeeCurrentEnabled())
    {
        return PathTraceCleanRoomRunExternalPdfNeeCurrentProducer(pixel, dimensions);
    }

    PathTraceCleanRtxdiDiInitialResult initialResult =
        PathTraceCleanRoomRunInitialProducer(pixel, dimensions);

    if (PathTraceCleanRoomNeeCacheProviderEnabled())
    {
        if (initialResult.status == CLEAN_INITIAL_STATUS_VALID &&
            RTXDI_IsValidDIReservoir(initialResult.reservoir))
        {
            PathTraceCleanRoomTryAugmentInitialResultWithNeeCache(pixel, initialResult);
            return initialResult;
        }

        const PathTraceCleanRtxdiDiInitialResult providerResult =
            PathTraceCleanRoomRunNeeCacheProviderProducer(pixel, dimensions);
        if (providerResult.status == CLEAN_INITIAL_STATUS_VALID &&
            RTXDI_IsValidDIReservoir(providerResult.reservoir))
        {
            return providerResult;
        }
    }

    return initialResult;
}

float3 PathTraceCleanRoomFlatDiffuseResolveReservoir(PathTracePrimarySurfaceRecord surfaceRecord, RTXDI_DIReservoir reservoir)
{
    if (!RTXDI_IsValidDIReservoir(reservoir))
    {
        return float3(0.0, 0.0, 0.0);
    }

    const uint lightIndex = RTXDI_GetDIReservoirLightIndex(reservoir);
    if (!PathTraceCleanRoomSyntheticMode() &&
        !PathTraceCleanRoomExternalPdfNeeCurrentEnabled() &&
        !PathTraceCleanRoomNeeCacheProviderEnabled() &&
        lightIndex >= PathTraceCleanRoomInitialLightCount())
    {
        return float3(0.0, 0.0, 0.0);
    }

    const RAB_Surface surface = PathTraceCleanRoomSurfaceForView(surfaceRecord);
    const RAB_LightInfo lightInfo = RAB_LoadLightInfo(lightIndex, false);
    const RAB_LightSample lightSample = RAB_SamplePolymorphicLight(lightInfo, surface, RTXDI_GetDIReservoirSampleUV(reservoir));
    if (!RAB_IsReplayableLightSample(lightSample))
    {
        return float3(0.0, 0.0, 0.0);
    }
    const bool referenceDoomAnalytic = PathTraceCleanReferenceRabEnabled() &&
        lightSample.lightType == RAB_LIGHT_TYPE_DOOM_ANALYTIC_SPHERE;

    const float3 toSample = lightSample.position - surfaceRecord.worldPositionAndViewDepth.xyz;
    const float3 lightDirection = PathTraceCleanRoomSafeNormalize(toSample, PathTraceCleanRoomSafeNormalize(surfaceRecord.shadingNormalAndOpacity.xyz, surfaceRecord.geometricNormalAndRoughness.xyz));
    const float ndotl = saturate(dot(PathTraceCleanRoomSafeNormalize(surfaceRecord.shadingNormalAndOpacity.xyz, surfaceRecord.geometricNormalAndRoughness.xyz), lightDirection));
    const float3 flatDiffuse = float3(0.5, 0.5, 0.5) * (1.0 / CLEAN_RTXDI_PI);
    float visibility = PathTraceCleanRoomSyntheticSingleLightMode()
        ? 1.0
        : PathTraceCleanRoomSelectedSampleVisibility(surfaceRecord, reservoir, lightSample);
    if (!PathTraceCleanRoomSyntheticSingleLightMode() && CleanRtxdiDiResolveVisibilityReuse != 0u)
    {
        float reusedVisibility = 0.0;
        if (PathTraceCleanRoomTryStoredReservoirVisibility(reservoir, reusedVisibility))
        {
            visibility = reusedVisibility;
        }
    }
    if (referenceDoomAnalytic && (CleanRtxdiDiReferenceRab == 9u || CleanRtxdiDiReferenceRab == 10u))
    {
        visibility = 1.0;
    }
    const float3 reflectedRadiance = referenceDoomAnalytic
        ? PathTraceCleanReferenceRabReflectedRadiance(lightSample.position, lightSample.radiance, surface)
        : CleanRtxdiDiResolveBrdfTarget != 0u
        ? RAB_GetReflectedBsdfRadianceForSurface(lightSample.position, lightSample.radiance, surface)
        : max(lightSample.radiance, float3(0.0, 0.0, 0.0)) * flatDiffuse * ndotl;
    const float reservoirThroughput = referenceDoomAnalytic && CleanRtxdiDiReferenceRab == 10u
        ? 1.0
        : max(RTXDI_GetDIReservoirInvPdf(reservoir), 0.0) /
            ((CleanRtxdiDiFlags & CLEAN_FLAG_RESOLVE_SOLID_ANGLE_PDF) != 0u ? max(lightSample.solidAnglePdf, 1.0e-6) : 1.0);
    return reflectedRadiance *
        reservoirThroughput *
        visibility;
}

float3 PathTraceCleanRoomFlatDiffuseResolve(PathTraceCleanRtxdiDiInitialResult result)
{
    if (result.status == CLEAN_INITIAL_STATUS_ZERO_TARGET_PDF)
    {
        return float3(0.0, 0.0, 0.0);
    }
    if (result.status != CLEAN_INITIAL_STATUS_VALID)
    {
        return PathTraceCleanRoomStatusColor(result.status);
    }

    return PathTraceCleanRoomFlatDiffuseResolveReservoir(result.surface, result.reservoir);
}

#include "pathtrace_clean_rtxdi_di_diagnostics.hlsli"
// Includes the clean temporal producer; keep this line current when temporal include contracts change.
#include "pathtrace_clean_rtxdi_di_temporal_output.hlsli"
float3 PathTraceCleanRoomAnalyticLightStatusColor(uint2 pixel, uint2 dimensions)
{
    if (CleanRtxdiDiLightMode == 0u)
    {
        return float3(1.0, 0.0, 1.0);
    }
    if (CleanRtxdiDiLightMode != 1u)
    {
        return float3(1.0, 0.35, 0.0);
    }

    const uint lightCount = min(CleanRtxdiDiAnalyticLightCount, CleanRtxdiDiAnalyticIdentityCount);
    if (lightCount == 0u)
    {
        return float3(0.95, 0.0, 0.12);
    }

    const uint selectedIndex = PathTraceCleanRoomHash(pixel.x + pixel.y * 4099u) % lightCount;
    const PathTraceDoomAnalyticLightCandidateIdentity identity = DoomAnalyticCurrentIdentities[selectedIndex];
    const float3 identityColor = PathTraceCleanRoomHashColor(identity.universeIndex ^ (selectedIndex * 1664525u));
    const uint identityRequiredFlags = CLEAN_DOOM_ANALYTIC_IDENTITY_VALID | CLEAN_DOOM_ANALYTIC_IDENTITY_SAMPLEABLE;
    if ((identity.flags & identityRequiredFlags) != identityRequiredFlags)
    {
        return lerp(float3(1.0, 0.82, 0.0), identityColor, 0.25);
    }

    const PathTraceDoomAnalyticLightCandidate light = DoomAnalyticLights[selectedIndex];
    if (!PathTraceCleanRoomAnalyticPayloadValid(light))
    {
        return lerp(float3(1.0, 0.18, 0.0), identityColor, 0.25);
    }

    PathTracePrimarySurfaceRecord surface;
    if (!PathTraceCleanRoomLoadSurfaceRecord(pixel, dimensions, surface))
    {
        return float3(0.18, 0.0, 0.22);
    }

    const float targetPdf = PathTraceCleanRoomAnalyticTargetPdf(surface, light);
    if (targetPdf <= 1.0e-8 || !(targetPdf == targetPdf))
    {
        return lerp(float3(0.08, 0.10, 1.0), identityColor, 0.20);
    }

    const uint band = (pixel.y / 64u) % 3u;
    if (band == 1u)
    {
        return identityColor;
    }
    if (band == 2u)
    {
        return float3(0.0, 0.95, 0.72);
    }
    return lerp(float3(0.0, 0.85, 0.18), identityColor, 0.55);
}

float3 PathTraceCleanRoomDeferredColor(uint2 pixel, uint view)
{
    const uint stripe = ((pixel.x >> 5u) + (pixel.y >> 4u) + view) & 1u;
    const uint band = ((pixel.y / 48u) + view) % 3u;
    float3 a = float3(0.35, 0.35, 0.35);
    float3 b = float3(0.75, 0.75, 0.75);

    if (view == 2u)
    {
        a = float3(0.55, 0.05, 0.10);
        b = float3(1.00, 0.25, 0.30);
    }
    else if (view == 3u)
    {
        a = float3(0.55, 0.32, 0.00);
        b = float3(1.00, 0.75, 0.05);
    }
    else if (view == 4u)
    {
        a = float3(0.05, 0.20, 0.65);
        b = float3(0.25, 0.55, 1.00);
    }
    else if (view == 5u)
    {
        a = float3(0.05, 0.45, 0.18);
        b = float3(0.30, 1.00, 0.48);
    }
    else if (view == 6u)
    {
        a = float3(0.35, 0.08, 0.60);
        b = float3(0.85, 0.35, 1.00);
    }
    else if (view == 7u)
    {
        a = float3(0.08, 0.48, 0.50);
        b = float3(0.35, 1.00, 0.95);
    }
    else if (view == 8u)
    {
        a = float3(0.55, 0.18, 0.02);
        b = float3(1.00, 0.48, 0.12);
    }

    float3 color = stripe != 0u ? a : b;
    if (band == 1u)
    {
        color *= 0.55;
    }
    else if (band == 2u)
    {
        color = color * 0.75 + float3(0.08, 0.08, 0.08);
    }
    return color;
}

#include "pathtrace_clean_rtxdi_di_rr_debug.hlsli"

#if defined(CLEAN_RTXDI_DI_INITIAL_ENTRY) || defined(CLEAN_RTXDI_DI_TEMPORAL_ENTRY)
bool PathTraceCleanRoomInitialRayGenView(uint view)
{
    return view == 4u || view == 7u ||
        (view == 8u && (CleanRtxdiDiTemporalFlags & CLEAN_TEMPORAL_FLAG_ENABLE) == 0u);
}

bool PathTraceCleanRoomTemporalRayGenView(uint view)
{
    return view == 5u || view == 6u || view == 8u ||
        view == 9u || view == 10u || view == 11u || view == 16u;
}
#endif

#if defined(CLEAN_RTXDI_DI_TRANSMISSION_PRODUCER_ENTRY)
#include "pathtrace_clean_rtxdi_di_transmission_producer.hlsli"
#elif defined(CLEAN_RTXDI_DI_INITIAL_ENTRY)
[shader("raygeneration")]
void RayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dimensions = DispatchRaysDimensions().xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    const uint view = CleanRtxdiDiView;
    if (view < 1u || view > 24u)
    {
        SmokeOutput[pixel] = float4(1.0, 0.0, 1.0, 1.0);
        return;
    }

    if (PathTraceCleanRoomInitialRayGenView(view))
    {
        SmokeOutput[pixel] = float4(PathTraceCleanRoomInitialReservoirOutput(pixel, dimensions, view), 1.0);
        return;
    }

    if (PathTraceCleanRoomTemporalRayGenView(view))
    {
        PathTraceCleanRoomStoreInitialReservoir(pixel, dimensions);
    }
}
#elif defined(CLEAN_RTXDI_DI_TEMPORAL_ENTRY)
[shader("raygeneration")]
void RayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dimensions = DispatchRaysDimensions().xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    const uint view = CleanRtxdiDiView;
    if (PathTraceCleanRoomTemporalRayGenView(view))
    {
        SmokeOutput[pixel] = float4(PathTraceCleanRoomTemporalReservoirOutputFromStoredCurrent(pixel, dimensions, view), 1.0);
    }
}
#else
[shader("raygeneration")]
void RayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dimensions = DispatchRaysDimensions().xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    const uint view = CleanRtxdiDiView;
    if (view < 1u || view > 24u)
    {
        SmokeOutput[pixel] = float4(1.0, 0.0, 1.0, 1.0);
        return;
    }

    if (view == 8u && CleanRtxdiDiView8Band == 10u)
    {
        SmokeOutput[pixel] = float4(PathTraceCleanRoomNeeCacheSecondaryCandidateFieldColorForPixel(pixel, dimensions), 1.0);
        return;
    }

    float3 color = PathTraceCleanRoomDeferredColor(pixel, view);
    if (view == 1u)
    {
        color = PathTraceCleanRoomSentinelColor(pixel);
    }
    else if (view == 2u)
    {
        color = PathTraceCleanRoomPrimarySurfaceStatusColor(pixel, dimensions);
    }
    else if (view == 3u)
    {
        color = PathTraceCleanRoomAnalyticLightStatusColor(pixel, dimensions);
    }
    else if (view == 4u || view == 7u || (view == 8u && (CleanRtxdiDiTemporalFlags & CLEAN_TEMPORAL_FLAG_ENABLE) == 0u))
    {
        color = PathTraceCleanRoomInitialReservoirOutput(pixel, dimensions, view);
    }
    else if (view == 13u)
    {
        color = PathTraceCleanRoomRealAnalyticOneSampleDiagnosticColor(pixel, dimensions);
    }
    else if (view == 14u)
    {
        color = PathTraceCleanRoomRealAnalyticTargetFactorDiagnosticColor(pixel, dimensions);
    }
    else if (view == 15u)
    {
        color = PathTraceCleanRoomRealAnalyticBinaryGateDiagnosticColor(pixel, dimensions);
    }
    else if (view == 17u)
    {
        color = PathTraceCleanRoomRRMotionVectorColor(pixel, dimensions);
    }
    else if (view == 18u)
    {
        color = PathTraceCleanRoomRRInputMosaicColor(pixel, dimensions);
    }
    else if (view == 19u)
    {
        color = PathTraceCleanRoomRRGuideAlbedoDebugColor(pixel);
    }
    else if (view == 20u)
    {
        color = PathTraceCleanRoomRRGuideSpecularAlbedoDebugColor(pixel);
    }
    else if (view == 21u)
    {
        color = PathTraceCleanRoomRRDepthContractBandColor(pixel, dimensions);
    }
    else if (view == 22u)
    {
        color = PathTraceCleanRoomPrimaryHitReprojectionErrorColor(pixel, dimensions);
    }
    else if (view == 23u)
    {
        color = PathTraceCleanRoomPreviousHitReprojectionErrorColor(pixel, dimensions);
    }
    else if (view == 12u || view == 24u)
    {
        color = PathTraceCleanRoomMaterialClassifierDebugColor(pixel, dimensions);
    }
    else if (view == 5u || view == 6u || view == 8u || view == 9u || view == 10u || view == 11u || view == 16u)
    {
        color = PathTraceCleanRoomTemporalReservoirOutput(pixel, dimensions, view);
    }
    SmokeOutput[pixel] = float4(color, 1.0);
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
}

[shader("closesthit")]
void ShadowClosestHit(inout PathTraceCleanRtxdiPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    payload.value = 1u;
}
