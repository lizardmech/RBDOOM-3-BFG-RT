#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_SHARED_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_SHARED_HLSLI

#include "../../../vulkan.hlsli"
#include "../PathTracePrimarySurface.hlsli"
#include "../PathTraceMaterialFeatureTypes.hlsli"
#include "../cleanroom_common/pathtrace_liquid_pool_control.hlsli"
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

#if defined(CLEAN_RTXDI_DI_TRACE_HIT_SURFACE_ADAPTER)
static const uint RT_CLEAN_RTXDI_DI_LIQUID_POOL_CANDIDATE_CAPACITY = 4u;
#endif

struct PathTraceCleanRtxdiPayload
{
    uint value;
    uint rayMode;
    uint ignoreInstanceId;
    uint ignorePrimitiveIndex;
    uint ignoreMaterialIndex;
#if defined(CLEAN_RTXDI_DI_TRACE_HIT_SURFACE_ADAPTER)
    uint hitInstanceId;
    uint hitPrimitiveIndex;
    uint hitMaterialId;
    uint hitMaterialIndex;
    uint hitTriangleClassAndFlags;
    float hitT;
    float2 hitBarycentrics;
    // Ray-mode 3 blend-through accumulator. Additive emissive cards do not
    // own the resolved behind-glass geometry, but their radiance must survive
    // while traversal continues to the opaque receiver.
    float3 passthroughEmissiveRadiance;
    // LPD-06A candidate transport. Keep the complete five-word contributor
    // identity plus hit distance until the committed receiver is known.
    uint liquidRawCount;
    uint liquidRetainedCount;
    uint liquidStatusMask;
    uint liquidRejectionCount;
    uint liquidInstanceId[RT_CLEAN_RTXDI_DI_LIQUID_POOL_CANDIDATE_CAPACITY];
    uint liquidMaterialIndex[RT_CLEAN_RTXDI_DI_LIQUID_POOL_CANDIDATE_CAPACITY];
    uint liquidPrimitiveIndex[RT_CLEAN_RTXDI_DI_LIQUID_POOL_CANDIDATE_CAPACITY];
    uint liquidBarycentricXBits[RT_CLEAN_RTXDI_DI_LIQUID_POOL_CANDIDATE_CAPACITY];
    uint liquidBarycentricYBits[RT_CLEAN_RTXDI_DI_LIQUID_POOL_CANDIDATE_CAPACITY];
    float liquidHitT[RT_CLEAN_RTXDI_DI_LIQUID_POOL_CANDIDATE_CAPACITY];
#endif
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
VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> PathTraceRRGuidePosition : register(u79);
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
Texture2D<float4> SmokeFallbackTexture : register(t14);
StructuredBuffer<PathTraceDynamicMaterialRecord> SmokeDynamicMaterials : register(t15);
StructuredBuffer<PathTraceSmokeEmissiveTriangle> SmokeEmissiveTriangles : register(t16);
StructuredBuffer<PathTraceEmissiveDistributionEntry> SmokeEmissiveDistribution : register(t46);
StructuredBuffer<PathTraceSmokeVertex> SmokeRigidRouteVertices : register(t22);
StructuredBuffer<uint> SmokeRigidRouteIndices : register(t23);
StructuredBuffer<uint> SmokeRigidRouteTriangleMaterials : register(t24);
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
StructuredBuffer<PathTraceMaterialFeatureRecord> PathTraceMaterialFeatures : register(t80);
StructuredBuffer<PathTraceMaterialFeatureParameterRecord> PathTraceMaterialFeatureParameters : register(t81);
RWStructuredBuffer<uint> PathTraceLiquidPoolStatusCounters : register(u94);
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_MATERIAL_FEATURE_SIDECAR 1
RWStructuredBuffer<RTXDI_PackedDIReservoir> CleanRtxdiDiCurrentReservoirs : register(u69);
RWStructuredBuffer<RTXDI_PackedDIReservoir> CleanRtxdiDiTemporalReservoirs : register(u70);
RWStructuredBuffer<RTXDI_PackedDIReservoir> CleanRtxdiDiPreviousReservoirs : register(u71);
RWStructuredBuffer<RTXDI_PackedDIReservoir> CleanRtxdiDiSpatialReservoirs : register(u72);
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

cbuffer PathTraceMaterialFeatureRuntimeConstants : register(b88)
{
    float4 PathTraceMaterialFeatureRuntimeInfoPacked;
    float4 PathTraceMaterialFeatureParams0;
    float4 PathTraceMaterialFeatureParams1;
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

bool PathTraceCleanRtxdiDiLiquidPoolCollectionEnabled()
{
    const uint controlFlags = PathTraceCleanRtxdiDiLiquidPoolControlFlags();
    return PathTraceCleanRtxdiDiLiquidPoolMode() != 0u &&
        (controlFlags & (RT_LIQUID_POOL_CONTROL_TELEMETRY_READY |
            RT_LIQUID_POOL_CONTROL_REQUESTED |
            RT_LIQUID_POOL_CONTROL_PARAMETERS_READY)) ==
            (RT_LIQUID_POOL_CONTROL_TELEMETRY_READY |
                RT_LIQUID_POOL_CONTROL_REQUESTED |
                RT_LIQUID_POOL_CONTROL_PARAMETERS_READY) &&
        (controlFlags & RT_LIQUID_POOL_CONTROL_ROUTE_DISABLED) == 0u;
}

void PathTraceCleanRtxdiDiApplyBlueNoiseToggle(inout RTXDI_RandomSamplerState rng)
{
#ifdef RBPT_ENABLE_BLUE_NOISE
    rng.useBlueNoise = ((CleanRtxdiDiFlags & CLEAN_FLAG_BLUE_NOISE) != 0u) ? rng.useBlueNoise : 0u;
#else
    rng.useBlueNoise = 0u;
#endif
}

PathTraceMaterialFeatureRuntimeInfo PathTraceCleanRtxdiDiLoadMaterialFeatureRuntimeInfo()
{
    return LoadPathTraceMaterialFeatureRuntimeInfo(PathTraceMaterialFeatureRuntimeInfoPacked);
}

struct PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams
{
    float4 params0;
    float4 params1;
};

PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams PathTraceCleanRtxdiDiLoadMaterialFeatureRuntimeParams()
{
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams params;
    params.params0 = PathTraceMaterialFeatureParams0;
    params.params1 = PathTraceMaterialFeatureParams1;
    return params;
}

static const uint RT_SMOKE_EMISSIVE_TRIANGLE_HISTORY_DYNAMIC = 0x00020000u;
static const uint RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF = 0x00040000u;
static const uint RT_SMOKE_MATERIAL_DIFFUSE_YCOCG = 0x00000002u;
static const uint RT_SMOKE_MATERIAL_ADDITIVE_DECAL = 0x00000004u;
static const uint RT_SMOKE_MATERIAL_EMISSIVE = 0x00000008u;
static const uint RT_SMOKE_MATERIAL_FILTER_DECAL = 0x00000010u;
static const uint RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_LUMA = 0x00000040u;
static const uint RT_SMOKE_MATERIAL_FORCE_DEBUG_ALBEDO = 0x00000080u;
static const uint RT_SMOKE_MATERIAL_PORTAL_WINDOW_FALLBACK = 0x00000200u;
static const uint RT_SMOKE_MATERIAL_OBJECT_GLASS_FALLBACK = 0x00000400u;
static const uint RT_SMOKE_MATERIAL_ADDITIVE_DECAL_WHITE_KEY = 0x00000800u;
static const uint RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_MAGENTA_KEY = 0x00001000u;
static const uint RT_SMOKE_TRIANGLE_CLASS_MASK = 0x0000ffffu;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_SHIFT = 24u;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_MASK = 0x0f000000u;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_GUI_SCREEN = 5u;
static const uint RT_SMOKE_SURFACE_CLASS_RIGID_ENTITY = 1u;
static const uint RT_SMOKE_SURFACE_CLASS_SKINNED_DEFORMED = 2u;
static const uint RT_SMOKE_SURFACE_CLASS_TRANSLUCENT = 3u;
static const uint RT_SMOKE_TEXTURE_FLAG_USE_NORMAL_MAPS = 0x00000008u;
static const uint RT_SMOKE_TEXTURE_FLAG_USE_SPECULAR_MAPS = 0x00000010u;
static const uint RT_SMOKE_TEXTURE_FLAG_USE_EMISSIVE_MAPS = 0x00000020u;
static const uint RT_SMOKE_TEXTURE_FLAG_RESERVOIR_TWO_SIDED_EMISSIVES = 0x00000040u;
static const uint RT_SMOKE_TEXTURE_FLAG_NORMAL_MAP_FLIP_GREEN = 0x00000100u;
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
static const uint CLEAN_FLAG_TRANSMISSION_PSR_PHASE = 1u << 20u;
static const uint CLEAN_FLAG_GLASS_DISTORTION = 1u << 22u;
static const uint CLEAN_FLAG_GLASS_REFRACTED_PSR = 1u << 23u;
static const uint CLEAN_FLAG_GLASS_REFLECTION_PSR = 1u << 25u;
// When set, reflection secondary simplified shade skips shadow rays.
static const uint CLEAN_FLAG_REFLECTION_SECONDARY_NO_SHADOWS = 1u << 26u;
static const uint CLEAN_FLAG_OPAQUE_MIRROR_REFLECTION = 1u << 27u;
static const uint CLEAN_SURFACE_FLAG_TRANSMISSION_PSR_RESOLVED = 0x80000000u;
static const uint CLEAN_SURFACE_FLAG_TRANSMISSION_PSR_REFRACTED = 0x40000000u;
static const uint CLEAN_SURFACE_FLAG_REFLECTION_PSR_RESOLVED = 0x20000000u;
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

#endif
