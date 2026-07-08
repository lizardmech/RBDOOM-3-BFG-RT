// Clean-room Remix ReSTIR GI lane driver.
//
// RGI-01: route sentinel + GI reservoir page ABI round-trip (view 8).
// RGI-02: initial-sample producer - from each valid primary surface, BSDF-
//         sample one indirect ray, trace it, shade the secondary hit with the
//         DI lane's light machinery (analytic lights, diffuse BRDF, shadow
//         ray), and write the producer textures (views 1 and 2).
//
// Radiance factoring contract (remix_gi_contract.txt): the stored producer
// radiance is incoming radiance at the primary surface from the sampled
// indirect path. It excludes the primary-surface BSDF and primary albedo; the
// final shading pass applies the receiving BRDF. Alpha channel = indirect path
// length. Miss = zero radiance + invalid hit-geometry flag. The firefly clamp
// applies here only.
//
// io_whitelist: reads GI-I-01 (current primary surface), GI-I-04 (current
// light universe, secondary vertex only), GI-I-05 (TLAS rays), GI-I-06
// (NEE-cache query), GI-I-09 (parameters). Writes GI-O-01 (producer textures),
// GI-O-06 (debug views).
// Never reads DI reservoir pages.

#include "../../../vulkan.hlsli"
#include "../PathTracePrimarySurface.hlsli"
#include "../cleanroom_common/pathtrace_first_indirect_candidate.hlsli"
#include "Rtxdi/RtxdiParameters.h"
#include "Rtxdi/GI/ReSTIRGIParameters.h"
#include "Rtxdi/Utils/RandomSamplerState.hlsli"
#include "Rtxdi/Utils/Math.hlsli"

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
};

#if defined(CLEAN_RESTIR_GI_PRODUCER_RAYQUERY_CS)
void CleanGiTraceRayUnusedForProducerRayQueryCs(inout PathTraceCleanRestirGiPayload payload)
{
    payload.value = 0u;
}
#define TraceRay(Accel, Flags, Mask, RayContributionToHitGroupIndex, MultiplierForGeometryContributionToHitGroupIndex, MissShaderIndex, Ray, Payload) CleanGiTraceRayUnusedForProducerRayQueryCs(Payload)
#endif

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

static const uint PATH_TRACE_NEE_CACHE_SOURCE_CACHE_ANALYTIC = 1u;
static const uint PATH_TRACE_NEE_CACHE_SOURCE_CACHE_EMISSIVE = 2u;
static const uint PATH_TRACE_NEE_CACHE_SOURCE_FALLBACK_FULL_RLU = 3u;
static const uint PATH_TRACE_NEE_CACHE_SOURCE_FALLBACK_TYPED_RLU = 4u;
static const uint PATH_TRACE_NEE_CACHE_FALLBACK_EMPTY_CELL = 3u;
static const uint PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE_LOCAL = 1u;
static const uint PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC_LOCAL = 2u;

#include "../pathtrace_material_classifier.hlsli"
#include "../RtxdiBridge/RAB_UnifiedLightRecord.hlsli"

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
Texture2D<float4> SmokeFallbackTexture : register(t14);
StructuredBuffer<PathTraceSmokeEmissiveTriangle> SmokeEmissiveTriangles : register(t16);
StructuredBuffer<PathTraceSmokeVertex> SmokeRigidRouteVertices : register(t22);
StructuredBuffer<uint> SmokeRigidRouteIndices : register(t23);
StructuredBuffer<uint> SmokeRigidRouteTriangleMaterials : register(t24);
StructuredBuffer<uint> SmokeRigidRouteTriangleMaterialIndexes : register(t25);
StructuredBuffer<PathTraceRigidRouteInstance> SmokeRigidRouteInstances : register(t26);
StructuredBuffer<PathTraceDoomAnalyticLightCandidate> DoomAnalyticLights : register(t27);
StructuredBuffer<PathTraceEmissiveDistributionEntry> SmokeEmissiveDistribution : register(t46);
StructuredBuffer<PathTraceUnifiedLightRecord> CleanRestirGiRluCurrentLights : register(t66);
StructuredBuffer<PathTraceNeeCacheProviderResult> CleanRestirGiNeeCacheProviderResults : register(t74);
StructuredBuffer<PathTraceNeeCacheCellRecord> CleanRestirGiNeeCacheCells : register(t75);
StructuredBuffer<PathTraceDynamicMaterialRecord> SmokeDynamicMaterials : register(t76);
StructuredBuffer<PathTraceNeeCacheCandidateRecord> CleanRestirGiNeeCacheCandidates : register(t77);
RWStructuredBuffer<PathTracePrimarySurfaceRecord> PrimarySurfaceHistoryCurrent : register(u30);
RWStructuredBuffer<PathTracePrimarySurfaceRecord> PrimarySurfaceHistoryPrevious : register(u31);
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> PathTraceMotionVectors : register(u39);
VK_IMAGE_FORMAT("r32ui") RWTexture2D<uint> PathTraceMotionVectorMask : register(u40);
RWStructuredBuffer<RTXDI_PackedGIReservoir> RemixRAB_GIReservoirs : register(u80);
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> CleanRestirGiProducerRadiance : register(u81);
VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> CleanRestirGiProducerHitPosition : register(u82);
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> CleanRestirGiProducerHitNormal : register(u83);
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> CleanRestirGiIndirectDiffuse : register(u84);
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> CleanRestirGiIndirectDiffuseLobe : register(u85);
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> CleanRestirGiIndirectSpecularLobe : register(u86);

// CleanGI currently owns the first consumer, but the trace->shade payload is a
// first-indirect candidate surface rather than a GI-specific contract.
#define CleanGiProducerSurface PathTraceFirstIndirectCandidateSurface
#define CleanGiProducerResult PathTraceFirstIndirectCandidateResult
#define CleanGiFirstIndirectRaySample PathTraceFirstIndirectCandidateRaySample
RWStructuredBuffer<CleanGiProducerSurface> CleanGiProducerSurfaceBuffer : register(u92);

struct CleanGiSpecularSeedReceiverSurface
{
    float3 worldPos;
    uint valid;
    float3 geometryNormal;
    float roughness;
    float3 shadingNormal;
    uint surfaceClass;
    float3 viewDir;
    float opacity;
    float3 diffuseAlbedo;
    uint pad0;
    float3 specularF0;
    uint pad1;
};
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> PathTraceRRGuideAlbedo : register(u48);
VK_IMAGE_FORMAT("r32f") RWTexture2D<float> PathTraceRRGuideHitDistance : register(u51);
VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> PathTraceRRInputColor : register(u54);
VK_BINDING(0, 1) Texture2D<float4> SmokeDiffuseTextures[] : register(t0, space1);
SamplerState SmokeMaterialSampler : register(s0);

// The leading block mirrors PathTraceCleanRtxdiDiSentinelConstants exactly so
// shared DI-lane helper code compiles unchanged; the C++ side copies the live
// DI constants blob into it. GI-owned fields follow.
cbuffer PathTraceCleanRestirGiConstants : register(b2)
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
    // --- GI-owned fields below; offsets must match PathTraceCleanRestirGi.cpp ---
    uint CleanRestirGiView;
    uint CleanRestirGiTemporalEnabled;
    uint CleanRestirGiSpatialEnabled;
    uint CleanRestirGiBiasCorrection;
    uint CleanRestirGiJacobianEnabled;
    uint CleanRestirGiMaxHistoryLength;
    uint CleanRestirGiMaxReservoirAge;
    float CleanRestirGiFireflyThreshold;
    uint CleanRestirGiNeeCacheSeedEnabled;
    uint CleanRestirGiFrameIndex;
    uint CleanRestirGiPhase;
    uint CleanRestirGiResolveEnabled;
    uint CleanRestirGiSpecularProducerEnabled;
    uint CleanRestirGiRrHitDistanceEnabled;
    uint CleanRestirGiRrSpecularInputEnabled;
    uint CleanRestirGiNeeCacheSecondaryEnabled;
    uint CleanRestirGiNeeCacheSecondaryMode;
    float CleanRestirGiNeeCacheSecondaryRoughness;
    float CleanRestirGiNeeCacheSecondaryProbability;
    uint CleanRestirGiMaxBounces;
    uint CleanRestirGiContinuationRouletteEnabled;
    float CleanRestirGiContinuationRouletteMin;
    float CleanRestirGiContinuationRouletteMax;
    float CleanRestirGiContinuationDirectProbability;
    float CleanRestirGiSecondaryDirectProbability;
    uint CleanRestirGiContinuationOpaqueTrace;
    uint CleanRestirGiProducerOpaqueTrace;
    uint CleanRestirGiSecondaryDirectSamples;
    uint CleanRestirGiSecondaryRluCandidateCount;
    float CleanRestirGiContributionFireflyThreshold;
    // Runtime blue-noise toggle (RGI). The shader is statically compiled with
    // RBPT_ENABLE_BLUE_NOISE, so eligible GI producer/initial sampling RNGs can
    // pull blue noise; this field masks it on/off per cvar without a second
    // shader permutation. The following uints are diagnostic controls that
    // keep the following struct 16-byte aligned (must match
    // PathTraceCleanRestirGi.cpp).
    uint CleanRestirGiBlueNoiseEnabled;
    uint CleanRestirGiProducerRayQueryHitIdMode;
    uint CleanRestirGiSpatialVisibilityMode;
    uint CleanRestirGiGlossySecondRayEnabled;
    float CleanRestirGiGlossySecondRayMaxRoughness;
    uint CleanRestirGiFinalMixMode;
    RTXDI_ReservoirBufferParameters RemixRAB_GIReservoirParams;
    uint4 RemixRAB_GIReservoirPageInfo;
};

void CleanGiApplyBlueNoiseToggle(inout RTXDI_RandomSamplerState rng)
{
#ifdef RBPT_ENABLE_BLUE_NOISE
    rng.useBlueNoise = (CleanRestirGiBlueNoiseEnabled != 0u) ? rng.useBlueNoise : 0u;
#else
    rng.useBlueNoise = 0u;
#endif
}

void CleanGiDisableBlueNoise(inout RTXDI_RandomSamplerState rng)
{
    rng.useBlueNoise = 0u;
}

static const uint RT_SMOKE_TRIANGLE_CLASS_MASK = 0x0000ffffu;
static const uint RT_SMOKE_TRIANGLE_FORCE_GEOMETRIC_NORMAL = 0x00010000u;
static const uint RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF = 0x00040000u;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_SHIFT = 24u;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_MASK = 0x0f000000u;
static const uint RT_SMOKE_SURFACE_CLASS_RIGID_ENTITY = 1u;
static const uint RT_SMOKE_SURFACE_CLASS_SKINNED_DEFORMED = 2u;
static const uint RT_SMOKE_SURFACE_CLASS_TRANSLUCENT = 3u;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_OBJECT_GLASS = 1u;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_SMOKE_PARTICLE = 2u;
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
static const uint RT_SMOKE_TEXTURE_FLAG_USE_NORMAL_MAPS = 0x00000008u;
static const uint RT_SMOKE_TEXTURE_FLAG_USE_SPECULAR_MAPS = 0x00000010u;
static const uint RT_SMOKE_TEXTURE_FLAG_USE_EMISSIVE_MAPS = 0x00000020u;
static const uint RT_SMOKE_TEXTURE_FLAG_RESERVOIR_TWO_SIDED_EMISSIVES = 0x00000040u;
static const uint RT_SMOKE_TEXTURE_FLAG_TOY_FAKE_PBR_SPECULAR = 0x00000080u;
static const uint RT_SMOKE_MATERIAL_OVERRIDE_ZERO_ROUGHNESS = 0x00000001u;
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
static const uint CLEAN_FLAG_NEE_CACHE_PROVIDER = 1u << 11u;
#define RB_RAB_LIGHT_SAMPLING_CORE_ONLY 1
#define RB_RAB_CLEAN_RTXDI_DI_SENTINEL 1
#include "../RtxdiBridge/RAB_UnifiedLightRecord.hlsli"
#include "../RtxdiBridge/RAB_LightTarget.hlsli"
#include "../RtxdiBridge/RAB_NeeCache.hlsli"

#define REMIX_RAB_GI_RESERVOIR_BRIDGE_EXTERNAL_BINDINGS 1
#include "../remix_bridge/RAB_GIReservoirBridge.hlsli"

static const uint PT_MOTION_VECTOR_MASK_VALID = 0x00000001u;
static const uint CLEAN_GI_SURFACE_FLAG_TRANSMISSION_PSR_RESOLVED = 0x80000000u;
static const uint CLEAN_GI_SURFACE_FLAG_TRANSMISSION_PSR_REFRACTED = 0x40000000u;
static const uint CLEAN_GI_SURFACE_FLAG_REFLECTION_PSR_RESOLVED = 0x20000000u;

float3 CleanGiSafeNormalize(float3 value, float3 fallback);
float CleanGiLuminance(float3 value);
bool CleanGiAllFinite3(float3 value);
float CleanGiTraceVisibility(float3 fromPosition, float3 geometricNormal, float3 toPosition);
bool CleanGiToyFakePBRSpecularEnabled();
bool CleanGiSpecularProducerActive();
bool CleanGiSpecularSeedProducerActive();
bool CleanGiMixedFirstIndirectProducerActive();
float3 CleanGiEvaluateIndirectLobes(RAB_Surface surface, float3 sampleDir, float3 incomingRadiance);
bool CleanGiSampleSpecularProducerDirection(RAB_Surface surface, inout RTXDI_RandomSamplerState rng, out float3 bounceDir, out float solidAnglePdf);
void CleanGiProducerMixtureProbabilities(RAB_Surface surface, out float diffuseProbability, out float specularProbability);
float CleanGiProducerMixturePdf(RAB_Surface surface, float3 bounceDir);

bool CleanGiSurfaceRecordIsTransmissionPsrResolved(PathTracePrimarySurfaceRecord record)
{
    // Treat either glass PSR replacement (transmission or reflection) as a
    // fully resolved surface for material loading / temporal fail-closed.
    return (record.header.w &
            (CLEAN_GI_SURFACE_FLAG_TRANSMISSION_PSR_RESOLVED |
                CLEAN_GI_SURFACE_FLAG_REFLECTION_PSR_RESOLVED)) != 0u;
}

// ---------------------------------------------------------------------------
// Surface bridge callback (GI-I-01/GI-I-02): material RAB_Surface from the
// DI-owned primary surface history, current or previous frame.
// ---------------------------------------------------------------------------

RAB_Surface CleanGiMaterialSurfaceFromRecord(PathTracePrimarySurfaceRecord record)
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
    surface.geometryNormal = CleanGiSafeNormalize(record.geometricNormalAndRoughness.xyz, float3(0.0, 0.0, 1.0));
    surface.shadingNormal = CleanGiSafeNormalize(record.shadingNormalAndOpacity.xyz, surface.geometryNormal);
    surface.viewDir = CleanGiSafeNormalize(record.viewDirectionAndReserved.xyz, -surface.shadingNormal);
    surface.materialId = record.materialAndSurface.x;
    surface.materialIndex = record.materialAndSurface.y;
    surface.surfaceClass = record.materialAndSurface.w & 0xffu;
    surface.flags = record.header.w;
    surface.instanceId = record.instancePrimitiveObject.x;
    surface.primitiveIndex = record.instancePrimitiveObject.y;
    RAB_Material material = RAB_EmptyMaterial();
    material.materialId = surface.materialId;
    material.materialIndex = surface.materialIndex;
    material.flags = record.materialAndSurface.z;
    material.alphaCutoff = record.albedoAndAlphaCutoff.w;
    material.diffuseAlbedo = saturate(record.albedoAndAlphaCutoff.xyz);
    material.roughness = saturate(record.geometricNormalAndRoughness.w);
    material.specularF0 = max(record.specularF0AndReserved.xyz, float3(0.0, 0.0, 0.0));
    material.opacity = saturate(record.shadingNormalAndOpacity.w);
    material.emissiveRadiance = max(record.emissiveAndHeight.xyz, float3(0.0, 0.0, 0.0));
    material.emissiveTextureIndex = record.instancePrimitiveObject.w;
    surface.material = material;
    return surface;
}

float3 CleanGiReceiverGuideAlbedo(uint2 pixel, float3 fallbackAlbedo)
{
    const float3 guideAlbedo = saturate(PathTraceRRGuideAlbedo[pixel].rgb);
    const bool invalidGuideAlbedo =
        all(abs(guideAlbedo - float3(1.0, 0.0, 1.0)) < float3(0.001, 0.001, 0.001));
    const float guideLuminance = dot(guideAlbedo, float3(0.2126, 0.7152, 0.0722));
    return guideLuminance > 1.0e-5 && !invalidGuideAlbedo ? guideAlbedo : fallbackAlbedo;
}

RAB_Surface CleanGiMaterialSurfaceFromCurrentRecord(uint2 pixel, PathTracePrimarySurfaceRecord record)
{
    RAB_Surface surface = CleanGiMaterialSurfaceFromRecord(record);
    if (RAB_IsSurfaceValid(surface))
    {
        surface.material.diffuseAlbedo = CleanGiReceiverGuideAlbedo(pixel, surface.material.diffuseAlbedo);
    }
    return surface;
}

#define REMIX_RAB_SURFACE_BRIDGE_EXTERNAL_CALLBACKS 1
RAB_Surface RemixRAB_LoadSurface(int2 pixel, bool previousFrame)
{
    if (any(pixel < int2(0, 0)) ||
        pixel.x >= int(CleanRtxdiDiWidth) || pixel.y >= int(CleanRtxdiDiHeight) ||
        CleanRtxdiDiWidth == 0u || CleanRtxdiDiHeight == 0u)
    {
        return RAB_EmptySurface();
    }
    const uint index = uint(pixel.y) * CleanRtxdiDiWidth + uint(pixel.x);
    PathTracePrimarySurfaceRecord record = (PathTracePrimarySurfaceRecord)0;
    if (previousFrame)
    {
        record = PrimarySurfaceHistoryPrevious[index];
    }
    else
    {
        record = PrimarySurfaceHistoryCurrent[index];
    }
    if (previousFrame)
    {
        return CleanGiMaterialSurfaceFromRecord(record);
    }
    return CleanGiMaterialSurfaceFromCurrentRecord(uint2(pixel), record);
}

// ---------------------------------------------------------------------------
// Frozen GI temporal contract, driven through the REMIX_RAB_* override points
// (definitions follow the include; the bridges declare prototypes).
// ---------------------------------------------------------------------------

#define REMIX_RAB_GI_INITIAL_SAMPLE_EXTERNAL_CALLBACKS 1
#define REMIX_RAB_GI_TEMPORAL_VALIDATION_EXTERNAL_CALLBACKS 1
#include "restir_gi_temporal_reuse.rt.hlsl"

// GI target pdf (RAB_GetGISampleTargetPdfForSurface): lobe-weighted luminance
// of the cached radiance at the receiving surface, gated on facing.
float RemixRAB_GetGISampleTargetPdfForSurface(float3 samplePosition, float3 sampleRadiance, RAB_Surface surface)
{
    if (!RAB_IsSurfaceValid(surface))
    {
        return 0.0;
    }
    const float3 toSample = samplePosition - RAB_GetSurfaceWorldPos(surface);
    const float distanceSquared = dot(toSample, toSample);
    if (distanceSquared <= 1.0e-6)
    {
        return 0.0;
    }
    const float3 sampleDir = toSample * rsqrt(distanceSquared);
    if (dot(sampleDir, RAB_GetSurfaceGeoNormal(surface)) <= 0.0 ||
        dot(sampleDir, RAB_GetSurfaceNormal(surface)) <= 0.0)
    {
        return 0.0;
    }
    const float targetPdf = CleanGiLuminance(CleanGiEvaluateIndirectLobes(
        surface,
        sampleDir,
        max(sampleRadiance, float3(0.0, 0.0, 0.0))));
    return clamp(targetPdf, 0.0, 1.0e4);
}

// Jacobian validation: reject reused samples whose reconnection-shift solid-
// angle ratio is degenerate or extreme. Disabling the cvar accepts everything
// (diagnostic only).
bool RemixRAB_ValidateGISampleWithJacobian(float jacobian)
{
    if (CleanRestirGiJacobianEnabled == 0u)
    {
        return true;
    }
    return jacobian > 0.0 && jacobian < 10.0 && jacobian == jacobian;
}

bool RemixRAB_GetTemporalConservativeVisibility(
    RAB_Surface currentSurface,
    RAB_Surface temporalSurface,
    float3 samplePosition)
{
    return CleanGiTraceVisibility(
        RAB_GetSurfaceWorldPos(currentSurface),
        RAB_GetSurfaceGeoNormal(currentSurface),
        samplePosition) > 0.0;
}

// No local stale-sample validation. Screen-space or ray visibility substitutes
// are portal-sensitive in rbdoom; exact validation belongs to a future
// portal-aware gradient pipeline.
RemixRestirGITemporalValidationResult RemixRAB_ValidateGITemporalReservoir(
    RAB_Surface currentSurface,
    RTXDI_GIReservoir inputReservoir,
    RTXDI_GIReservoir resultReservoir,
    RemixRestirGITemporalValidationDesc desc)
{
    RemixRestirGITemporalValidationResult result = (RemixRestirGITemporalValidationResult)0;
    result.reservoir = resultReservoir;
    return result;
}

// Raw initial sample: this thread's producer outputs (written earlier in the
// same dispatch; GI-I-07).
RemixRestirGIRawInitialSample RemixRAB_LoadRawGIInitialSample(uint2 pixel)
{
    RemixRestirGIRawInitialSample sample = (RemixRestirGIRawInitialSample)0;
    sample.portalIndex = REMIX_RESTIR_GI_INVALID_PORTAL_INDEX;
    const float4 radianceAndLength = CleanRestirGiProducerRadiance[pixel];
    const float4 hitPositionAndValid = CleanRestirGiProducerHitPosition[pixel];
    const float4 hitNormalPacked = CleanRestirGiProducerHitNormal[pixel];
    if (hitPositionAndValid.w <= 0.0)
    {
        return sample;
    }
    sample.valid = 1u;
    sample.flags = REMIX_RESTIR_GI_INITIAL_FLAG_SELECTED_SURFACE;
    sample.radiance = max(radianceAndLength.rgb, float3(0.0, 0.0, 0.0));
    sample.indirectPathLength = radianceAndLength.a;
    sample.sourcePdf = max(hitPositionAndValid.w, 0.0);
    sample.hitPosition = hitPositionAndValid.xyz;
    sample.hitNormal = CleanGiSafeNormalize(hitNormalPacked.xyz, float3(0.0, 0.0, 1.0));
    return sample;
}

// Remix initial-reservoir recipe: single sample with embedded pdf (M=1,
// avgWeight=1), RIS update with target pdf at the receiving surface, then
// finalize with M forced to 1 (both inputs are MIS-weighted).
static const uint CLEAN_RESTIR_GI_INIT_MERGE_RNG_PASS = 0x52525814u;

RTXDI_GIReservoir RemixRAB_LoadPreparedGIInitialReservoir(
    uint2 pixel,
    RAB_Surface surface,
    RemixRestirGIRawInitialSample rawSample,
    RemixRestirGIInitialSampleControls controls)
{
    RTXDI_GIReservoir reservoir = RTXDI_EmptyGIReservoir();
    if (rawSample.valid == 0u || !RAB_IsSurfaceValid(surface))
    {
        return reservoir;
    }

    // Seed hooks: cvar-gated producers can preload the INIT page before the
    // raw diffuse initial sample joins the RIS update here.
    if (CleanRestirGiNeeCacheSeedEnabled != 0u || CleanGiSpecularSeedProducerActive())
    {
        reservoir = RAB_LoadGIReservoir(int2(pixel), int(RemixRAB_GetGIInitSampleReservoirIndex()));
    }
    const bool seededReservoirValid = RTXDI_IsValidGIReservoir(reservoir);

    const RTXDI_GIReservoir initialSample = RTXDI_MakeGIReservoir(
        rawSample.hitPosition,
        rawSample.hitNormal,
        rawSample.radiance,
        rawSample.sourcePdf);

    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSamplerForPass(
        pixel,
        CleanRestirGiFrameIndex,
        CLEAN_RESTIR_GI_INIT_MERGE_RNG_PASS,
        0u);
    CleanGiApplyBlueNoiseToggle(rng);
    const float targetPdf = RemixRAB_GetGISampleTargetPdfForSurface(initialSample.position, initialSample.radiance, surface);
    const bool selectedInitial = RTXDI_CombineGIReservoirs(reservoir, initialSample, RTXDI_GetNextRandom(rng), targetPdf);

    const float pNew = (!seededReservoirValid || selectedInitial)
        ? targetPdf
        : RemixRAB_GetGISampleTargetPdfForSurface(reservoir.position, reservoir.radiance, surface);
    reservoir.M = 1;
    RTXDI_FinalizeGIResampling(reservoir, 1.0, pNew * reservoir.M);
    return reservoir;
}

// ---------------------------------------------------------------------------
// Motion (GI-I-03): DI motion vectors with camera-reprojection fallback.
// ---------------------------------------------------------------------------

bool CleanGiProjectCameraMotion(PathTracePrimarySurfaceRecord currentSurface, uint2 pixel, uint2 dimensions, out float3 screenSpaceMotion)
{
    screenSpaceMotion = float3(0.0, 0.0, 0.0);
    if (CleanRtxdiDiPrevCameraOriginAndValid.w < 0.5)
    {
        return false;
    }
    const float3 delta = currentSurface.worldPositionAndViewDepth.xyz - CleanRtxdiDiPrevCameraOriginAndValid.xyz;
    const float forwardDistance = dot(delta, CleanRtxdiDiPrevCameraForwardAndTanX.xyz);
    if (forwardDistance <= 0.05)
    {
        return false;
    }
    const float ndcX = -dot(delta, CleanRtxdiDiPrevCameraLeftAndTanY.xyz) / max(forwardDistance * CleanRtxdiDiPrevCameraForwardAndTanX.w, 1.0e-5);
    const float ndcY = -dot(delta, CleanRtxdiDiPrevCameraUpAndTanY.xyz) / max(forwardDistance * CleanRtxdiDiPrevCameraLeftAndTanY.w, 1.0e-5);
    if (abs(ndcX) > 1.0 || abs(ndcY) > 1.0)
    {
        return false;
    }
    const float2 previousPixelFloat = (float2(ndcX, ndcY) * 0.5 + 0.5) * float2(dimensions);
    if (!all(previousPixelFloat == previousPixelFloat) ||
        previousPixelFloat.x < 0.0 || previousPixelFloat.y < 0.0 ||
        previousPixelFloat.x >= (float)dimensions.x || previousPixelFloat.y >= (float)dimensions.y)
    {
        return false;
    }
    const float previousLinearDepth = length(delta);
    screenSpaceMotion = float3(previousPixelFloat - (float2(pixel) + 0.5), previousLinearDepth - currentSurface.worldPositionAndViewDepth.w);
    return all(screenSpaceMotion == screenSpaceMotion);
}

bool CleanGiComputeLinearDepthMotionDelta(PathTracePrimarySurfaceRecord currentSurface, out float depthDelta)
{
    depthDelta = 0.0;
    if (CleanRtxdiDiPrevCameraOriginAndValid.w < 0.5)
    {
        return false;
    }

    const uint objectMotionFlags = RT_PRIMARY_SURFACE_HAS_OBJECT_MOTION | RT_PRIMARY_SURFACE_HAS_PREVIOUS_POSITION;
    const bool hasObjectPreviousPosition =
        currentSurface.header.x == RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_VERSION &&
        (currentSurface.header.y & objectMotionFlags) == objectMotionFlags &&
        currentSurface.previousPositionOrMotion.w >= 0.5;

    const float3 previousPosition = hasObjectPreviousPosition
        ? currentSurface.previousPositionOrMotion.xyz
        : currentSurface.worldPositionAndViewDepth.xyz;
    const float previousDepth = length(previousPosition - CleanRtxdiDiPrevCameraOriginAndValid.xyz);
    depthDelta = previousDepth - currentSurface.worldPositionAndViewDepth.w;
    return depthDelta == depthDelta;
}

float3 CleanGiLoadScreenSpaceMotion(uint2 pixel, PathTracePrimarySurfaceRecord currentSurface, uint2 dimensions)
{
    if (pixel.x < CleanRtxdiDiWidth && pixel.y < CleanRtxdiDiHeight)
    {
        const uint motionMask = PathTraceMotionVectorMask[pixel];
        if ((motionMask & PT_MOTION_VECTOR_MASK_VALID) != 0u)
        {
            const float2 motionXY = PathTraceMotionVectors[pixel].xy;
            float depthDelta = 0.0;
            if (all(motionXY == motionXY) &&
                CleanGiComputeLinearDepthMotionDelta(currentSurface, depthDelta))
            {
                return float3(motionXY, depthDelta);
            }
        }
    }
    float3 cameraMotion;
    if (CleanGiProjectCameraMotion(currentSurface, pixel, dimensions, cameraMotion))
    {
        return cameraMotion;
    }
    return float3(0.0, 0.0, 0.0);
}

// ---------------------------------------------------------------------------
// RGI-06: spatial reuse. Runs as a second dispatch (phase 1) so every
// pixel's TEMPORAL_OUTPUT page write has completed before neighbors read it.
// Neighbor offsets reuse the DI spatial pass's disk sequence (power-of-two
// count for the RTXDI offset mask).
// ---------------------------------------------------------------------------

static const float2 CleanGiNeighborOffsets[32] =
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
static const uint CLEAN_RESTIR_GI_NEIGHBOR_OFFSET_MASK = 31u;
static const uint CLEAN_RESTIR_GI_SPATIAL_RNG_PASS = 0x52525812u;

int2 RAB_ClampSamplePositionIntoView(int2 pixelPosition, bool previousFrame)
{
    const int width = int(max(CleanRtxdiDiWidth, 1u));
    const int height = int(max(CleanRtxdiDiHeight, 1u));
    return int2(clamp(pixelPosition.x, 0, width - 1), clamp(pixelPosition.y, 0, height - 1));
}

#define RTXDI_NEIGHBOR_OFFSETS_BUFFER CleanGiNeighborOffsets
#define RBPT_GI_SURFACE_LINEAR_DEPTH(surface) RAB_GetSurfaceLinearDepth(surface)
#define RBPT_GI_TARGET_PDF(surface, reservoir) \
    RemixRAB_GetGISampleTargetPdfForSurface((reservoir).position, (reservoir).radiance, (surface))
bool CleanGiValidateSpatialReuseSample(uint2 pixel, int2 neighborPixel, uint sampleIndex, RAB_Surface surface, RTXDI_GIReservoir reservoir)
{
    if (CleanRestirGiSpatialVisibilityMode == 0u)
    {
        return true;
    }
    if (CleanRestirGiSpatialVisibilityMode == 2u)
    {
        uint h = pixel.x * 0x8da6b343u;
        h ^= pixel.y * 0xd8163841u;
        h ^= uint(neighborPixel.x) * 0xcb1ab31fu;
        h ^= uint(neighborPixel.y) * 0x9e3779b9u;
        h ^= sampleIndex * 0x85ebca6bu;
        h ^= CleanRestirGiFrameIndex * 0xc2b2ae35u;
        h ^= h >> 16u;
        h *= 0x7feb352du;
        h ^= h >> 15u;
        if ((h & 1u) != 0u)
        {
            return true;
        }
    }
    return CleanGiTraceVisibility(
        RAB_GetSurfaceWorldPos(surface),
        RAB_GetSurfaceGeoNormal(surface),
        reservoir.position) > 0.0;
}
#define RBPT_GI_VALIDATE_REUSE_SAMPLE(pixel, neighborPixel, sampleIndex, surface, reservoir) \
    CleanGiValidateSpatialReuseSample(pixel, neighborPixel, sampleIndex, surface, reservoir)
#include "Rtxdi/GI/SpatialResampling.hlsli"

bool CleanGiSpecularProducerNeedsReuseQuarantine(RAB_Surface surface)
{
    if (!CleanGiSpecularProducerActive() || !RAB_IsSurfaceValid(surface))
    {
        return false;
    }

    const float roughness = saturate(GetRoughness(surface.material));
    const float specularLum = RAB_MaterialLuminance(GetSpecularF0(surface.material));
    return roughness < 0.28 && specularLum > 0.04;
}

// Spatial reuse over the TEMPORAL_OUTPUT page. Until the full Remix
// validation/gradient path exists, neighbor candidates are conservatively
// visibility-tested from the current receiver so spatial reuse cannot blur
// through contact-shadow blockers.
RTXDI_GIReservoir CleanGiRunSpatialReuse(uint2 pixel, RAB_Surface surface, RTXDI_GIReservoir inputReservoir)
{
    if (!RAB_IsSurfaceValid(surface))
    {
        return inputReservoir;
    }
    if (CleanGiSpecularProducerNeedsReuseQuarantine(surface))
    {
        return inputReservoir;
    }

    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSamplerForPass(
        pixel,
        CleanRestirGiFrameIndex,
        CLEAN_RESTIR_GI_SPATIAL_RNG_PASS,
        0u);
    CleanGiApplyBlueNoiseToggle(rng);

    RTXDI_RuntimeParameters params = (RTXDI_RuntimeParameters)0;
    params.activeCheckerboardField = 0u;
    params.neighborOffsetMask = CLEAN_RESTIR_GI_NEIGHBOR_OFFSET_MASK;

    const bool historyStarved = inputReservoir.M < CleanRestirGiMaxHistoryLength;

    RTXDI_GISpatialResamplingParameters sparams = (RTXDI_GISpatialResamplingParameters)0;
    sparams.samplingRadius = historyStarved ? 96.0 : 48.0;
    sparams.numSamples = historyStarved ? 2u : 1u;
    sparams.depthThreshold = 0.14;
    sparams.normalThreshold = 0.88;
    sparams.biasCorrectionMode = min(CleanRestirGiBiasCorrection, uint(RTXDI_BIAS_CORRECTION_BASIC));
    sparams.jacobianCutoff = 0.0;

    return RTXDI_GISpatialResampling(
        pixel,
        surface,
        RemixRAB_GetGITemporalOutputReservoirIndex(),
        inputReservoir,
        rng,
        params,
        RemixRAB_GIReservoirParams,
        sparams);
}

static const uint CLEAN_RESTIR_GI_PRODUCER_RNG_PASS = 0x52525810u;
static const uint CLEAN_RESTIR_GI_SPECULAR_PRODUCER_RNG_PASS = 0x52525813u;
static const uint CLEAN_RESTIR_GI_GLOSSY_SECOND_RAY_RNG_PASS = 0x52525815u;
static const float CLEAN_RESTIR_GI_FIREFLY_FACTOR = 30.0;
static const float CLEAN_RESTIR_GI_SPECULAR_PRODUCER_MAX_ROUGHNESS = 0.35;
static const float CLEAN_RESTIR_GI_SPECULAR_PRODUCER_MIN_F0 = 0.035;
static const float CLEAN_RESTIR_GI_SPECULAR_PRODUCER_METAL_F0 = 0.12;

uint CleanGiHashCombine3(uint a, uint b, uint c)
{
    return RBPT_RestirHashCombine(RBPT_RestirHashCombine(a, b), c);
}

uint CleanGiProducerRandomAvalanche(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

uint CleanGiProducerRandomSeed(uint2 pixel, uint frameIndex, uint passNamespace)
{
    uint seed = CleanGiProducerRandomAvalanche(pixel.x * 0x8da6b343u);
    seed ^= CleanGiProducerRandomAvalanche(pixel.y * 0xd8163841u);
    seed ^= CleanGiProducerRandomAvalanche((pixel.x + pixel.y) * 0xcb1ab31fu);
    seed ^= CleanGiProducerRandomAvalanche(frameIndex * 0x9e3779b9u);
    seed ^= CleanGiProducerRandomAvalanche(passNamespace);
    return CleanGiProducerRandomAvalanche(seed);
}

RTXDI_RandomSamplerState CleanGiInitProducerRandomSampler(uint2 pixel, uint frameIndex, uint passNamespace)
{
    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSamplerForPass(pixel, frameIndex, passNamespace, 0u);
    rng.seed = CleanGiProducerRandomSeed(pixel, frameIndex, passNamespace);
    CleanGiApplyBlueNoiseToggle(rng);
    return rng;
}

float CleanGiGetNextProducerRandom(inout RTXDI_RandomSamplerState rng)
{
#ifdef RBPT_ENABLE_BLUE_NOISE
    if (rng.useBlueNoise != 0u && rng.index < RBPT_BLUE_NOISE_DIMS)
    {
        return RTXDI_GetNextRandom(rng);
    }
#endif
    const uint dimension = rng.index;
    rng.index += 1u;
    rng.seed = CleanGiProducerRandomAvalanche(
        rng.seed ^
        CleanGiProducerRandomAvalanche(dimension + rng.dimensionBase * 0x45d9f3bu));
    const uint value = CleanGiProducerRandomAvalanche(
        rng.seed ^
        CleanGiProducerRandomAvalanche(dimension * 0x27d4eb2du + rng.frameIndex));
    return RBPT_RestirUintToUnitFloat(value);
}

#define RAB_GetNextRandom CleanGiGetNextProducerRandom

float3 CleanGiSafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-8 ? value * rsqrt(lengthSquared) : fallback;
}

float CleanGiLuminance(float3 value)
{
    return dot(max(value, float3(0.0, 0.0, 0.0)), float3(0.2126, 0.7152, 0.0722));
}

bool CleanGiAllFinite3(float3 value)
{
    return all(value == value) && all(abs(value) < float3(3.402823e+38, 3.402823e+38, 3.402823e+38));
}

// ---------------------------------------------------------------------------
// Surface loading (GI-I-01; same adapters as the DI lane)
// ---------------------------------------------------------------------------

bool CleanGiLoadSurfaceRecord(uint2 pixel, uint2 dimensions, out PathTracePrimarySurfaceRecord record)
{
    record = (PathTracePrimarySurfaceRecord)0;
#if defined(CLEAN_RESTIR_GI_PRODUCER_RAYQUERY_CS)
    const uint width = dimensions.x;
    const uint height = dimensions.y;
#else
    const uint width = CleanRtxdiDiWidth != 0u ? CleanRtxdiDiWidth : dimensions.x;
    const uint height = CleanRtxdiDiHeight != 0u ? CleanRtxdiDiHeight : dimensions.y;
#endif
    if (width == 0u || height == 0u || pixel.x >= width || pixel.y >= height)
    {
        return false;
    }
    record = PrimarySurfaceHistoryCurrent[pixel.y * width + pixel.x];
    return record.header.x == RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_VERSION &&
        (record.header.y & RT_PRIMARY_SURFACE_VALID) != 0u;
}

uint CleanGiLoadTriangleMaterialIndex(uint instanceId, uint primitiveIndex)
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

uint CleanGiLoadTriangleMaterialId(uint instanceId, uint primitiveIndex)
{
    if (instanceId == 0u)
    {
        return primitiveIndex < CleanRtxdiDiStaticTriangleCount
            ? SmokeStaticTriangleMaterials[primitiveIndex]
            : 0u;
    }
    if (instanceId == 1u)
    {
        return primitiveIndex < CleanRtxdiDiDynamicTriangleCount
            ? SmokeDynamicTriangleMaterials[primitiveIndex]
            : 0u;
    }
    const uint routeInstanceIndex = instanceId - 2u;
    const uint routeInstanceCount = (uint)max(CleanRtxdiDiToyPathInfo.w, 0.0);
    if (routeInstanceIndex >= routeInstanceCount)
    {
        return 0u;
    }
    const PathTraceRigidRouteInstance route = SmokeRigidRouteInstances[routeInstanceIndex];
    const uint routedPrimitiveIndex = route.triangleOffset + primitiveIndex;
    if (primitiveIndex >= route.triangleCount ||
        routedPrimitiveIndex >= CleanRtxdiDiRigidRouteTriangleCount)
    {
        return route.materialId;
    }
    return SmokeRigidRouteTriangleMaterials[routedPrimitiveIndex];
}

uint CleanGiLoadTriangleClassAndFlags(uint instanceId, uint primitiveIndex)
{
    if (instanceId == 0u)
    {
        return primitiveIndex < CleanRtxdiDiStaticTriangleCount
            ? SmokeStaticTriangleClasses[primitiveIndex]
            : 0u;
    }
    if (instanceId == 1u)
    {
        return primitiveIndex < CleanRtxdiDiDynamicTriangleCount
            ? SmokeDynamicTriangleClasses[primitiveIndex]
            : 0u;
    }
    return RT_SMOKE_SURFACE_CLASS_RIGID_ENTITY;
}

bool CleanGiHitMetadataInRange(uint instanceId, uint primitiveIndex)
{
    if (instanceId == 0u)
    {
        return primitiveIndex < CleanRtxdiDiStaticTriangleCount;
    }
    if (instanceId == 1u)
    {
        return primitiveIndex < CleanRtxdiDiDynamicTriangleCount;
    }
    const uint routeInstanceIndex = instanceId - 2u;
    const uint routeInstanceCount = (uint)max(CleanRtxdiDiToyPathInfo.w, 0.0);
    if (routeInstanceIndex >= routeInstanceCount)
    {
        return false;
    }
    const PathTraceRigidRouteInstance route = SmokeRigidRouteInstances[routeInstanceIndex];
    const uint routedPrimitiveIndex = route.triangleOffset + primitiveIndex;
    return primitiveIndex < route.triangleCount &&
        routedPrimitiveIndex >= route.triangleOffset &&
        routedPrimitiveIndex < CleanRtxdiDiRigidRouteTriangleCount;
}

uint CleanGiTriangleSurfaceClass(uint triangleClassAndFlags)
{
    return triangleClassAndFlags & RT_SMOKE_TRIANGLE_CLASS_MASK;
}

uint CleanGiTriangleTranslucentSubtype(uint triangleClassAndFlags)
{
    return (triangleClassAndFlags & RT_SMOKE_TRANSLUCENT_SUBTYPE_MASK) >> RT_SMOKE_TRANSLUCENT_SUBTYPE_SHIFT;
}

uint CleanGiDynamicMaterialRecordCount()
{
    return (uint)max(CleanRtxdiDiEmissiveDistributionInfo.w, 0.0);
}

void CleanGiApplyDynamicMaterialRecord(uint materialIndex, inout PathTraceSmokeMaterial material)
{
    const uint recordCount = CleanGiDynamicMaterialRecordCount();
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

PathTraceSmokeMaterial CleanGiLoadSmokeMaterial(uint materialIndex)
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
    const uint materialCount = (uint)TextureInfo.z;
    if (materialIndex < materialCount)
    {
        material = SmokeMaterials[materialIndex];
        CleanGiApplyDynamicMaterialRecord(materialIndex, material);
    }
    return material;
}

// ---------------------------------------------------------------------------
// Texture sampling for the secondary-hit material (same decode rules as the
// primary-surface producer)
// ---------------------------------------------------------------------------

float4 CleanGiTextureLoad(uint textureIndex, uint textureWidth, uint textureHeight, float2 wrappedTexCoord, bool bindlessEnabled, bool bilinearFilter)
{
    const uint width = max(textureWidth, 1u);
    const uint height = max(textureHeight, 1u);
    if (!bilinearFilter)
    {
        const uint2 texel = min((uint2)floor(wrappedTexCoord * float2(width, height)), uint2(width - 1u, height - 1u));
        return bindlessEnabled
            ? SmokeDiffuseTextures[NonUniformResourceIndex(textureIndex)].Load(int3(texel, 0))
            : SmokeFallbackTexture.Load(int3(0, 0, 0));
    }
    const float2 scaled = wrappedTexCoord * float2(width, height) - float2(0.5, 0.5);
    const int2 baseTexel = (int2)floor(scaled);
    const float2 fracPart = frac(scaled);
    const uint2 texel00 = uint2((baseTexel.x % (int)width + (int)width) % (int)width, (baseTexel.y % (int)height + (int)height) % (int)height);
    const uint2 texel10 = uint2((texel00.x + 1u) % width, texel00.y);
    const uint2 texel01 = uint2(texel00.x, (texel00.y + 1u) % height);
    const uint2 texel11 = uint2((texel00.x + 1u) % width, (texel00.y + 1u) % height);
    const float4 c00 = bindlessEnabled ? SmokeDiffuseTextures[NonUniformResourceIndex(textureIndex)].Load(int3(texel00, 0)) : SmokeFallbackTexture.Load(int3(0, 0, 0));
    const float4 c10 = bindlessEnabled ? SmokeDiffuseTextures[NonUniformResourceIndex(textureIndex)].Load(int3(texel10, 0)) : SmokeFallbackTexture.Load(int3(0, 0, 0));
    const float4 c01 = bindlessEnabled ? SmokeDiffuseTextures[NonUniformResourceIndex(textureIndex)].Load(int3(texel01, 0)) : SmokeFallbackTexture.Load(int3(0, 0, 0));
    const float4 c11 = bindlessEnabled ? SmokeDiffuseTextures[NonUniformResourceIndex(textureIndex)].Load(int3(texel11, 0)) : SmokeFallbackTexture.Load(int3(0, 0, 0));
    return lerp(lerp(c00, c10, fracPart.x), lerp(c01, c11, fracPart.x), fracPart.y);
}

float4 CleanGiSampleTexture(uint textureIndex, uint textureWidth, uint textureHeight, float2 texCoord, float4 fallback)
{
    const uint textureCount = (uint)TextureInfo.x;
    const uint sampleMethod = (uint)TextureInfo.y;
    const uint textureFlags = (uint)TextureInfo.w;
    const bool bindlessEnabled = (textureFlags & 1u) != 0u;
    const bool bilinearFilter = (textureFlags & 2u) != 0u;
    if (sampleMethod == 0u || textureIndex == 0xffffffffu || textureIndex >= textureCount ||
        !all(texCoord == texCoord) || any(abs(texCoord) > 65536.0))
    {
        return fallback;
    }
    const float2 wrappedTexCoord = frac(texCoord);
    float4 sampled = sampleMethod == 2u
        ? CleanGiTextureLoad(textureIndex, textureWidth, textureHeight, wrappedTexCoord, bindlessEnabled, bilinearFilter)
        : (bindlessEnabled
            ? SmokeDiffuseTextures[NonUniformResourceIndex(textureIndex)].SampleLevel(SmokeMaterialSampler, wrappedTexCoord, 0.0)
            : SmokeFallbackTexture.SampleLevel(SmokeMaterialSampler, wrappedTexCoord, 0.0));
    return (!all(sampled == sampled) || any(abs(sampled) > 65504.0)) ? fallback : sampled;
}

float3 CleanGiConvertYCoCgToRGB(float4 ycocg)
{
    ycocg.z = (ycocg.z * 31.875) + 1.0;
    ycocg.z = 1.0 / ycocg.z;
    ycocg.xy *= ycocg.z;
    return saturate(float3(
        dot(ycocg, float4(1.0, -1.0, 0.0, 1.0)),
        dot(ycocg, float4(0.0, 1.0, -0.50196078, 1.0)),
        dot(ycocg, float4(-1.0, -1.0, 1.00392156, 1.0))));
}

float4 CleanGiSampleDecodedDiffuseTexture(PathTraceSmokeMaterial material, float2 texCoord)
{
    float4 texel = CleanGiSampleTexture(material.diffuseTextureIndex, material.textureWidth, material.textureHeight, texCoord, material.debugAlbedo);
    const bool textureDecodeEnabled = (((uint)TextureInfo.w) & 4u) != 0u;
    if (textureDecodeEnabled && (material.flags & RT_SMOKE_MATERIAL_DIFFUSE_YCOCG) != 0u)
    {
        texel.rgb = CleanGiConvertYCoCgToRGB(texel);
    }
    return texel;
}

float4 CleanGiSampleDiffuseTexture(PathTraceSmokeMaterial material, float2 texCoord)
{
    return (material.flags & RT_SMOKE_MATERIAL_FORCE_DEBUG_ALBEDO) != 0u
        ? material.debugAlbedo
        : CleanGiSampleDecodedDiffuseTexture(material, texCoord);
}

float3 CleanGiSampleDiffuseAlbedo(PathTraceSmokeMaterial material, float2 texCoord)
{
    return saturate(CleanGiSampleDiffuseTexture(material, texCoord).rgb);
}

float4 CleanGiSampleSurfaceAlbedo(
    PathTraceSmokeMaterial material,
    float2 texCoord,
    uint surfaceClass,
    uint translucentSubtype,
    float4 vertexColor)
{
    float4 albedo = CleanGiSampleDiffuseTexture(material, texCoord);
    if (surfaceClass == RT_SMOKE_SURFACE_CLASS_TRANSLUCENT &&
        translucentSubtype == RT_SMOKE_TRANSLUCENT_SUBTYPE_GUI_SCREEN)
    {
        albedo = material.diffuseTextureIndex != 0xffffffffu
            ? float4(albedo.rgb * vertexColor.rgb, albedo.a * vertexColor.a)
            : vertexColor;
    }
    return saturate(albedo);
}

float4 CleanGiSampleAlphaTexture(PathTraceSmokeMaterial material, float2 texCoord)
{
    return material.alphaTextureIndex != 0xffffffffu
        ? CleanGiSampleTexture(
            material.alphaTextureIndex,
            material.alphaTextureWidth,
            material.alphaTextureHeight,
            texCoord,
            CleanGiSampleDiffuseTexture(material, texCoord))
        : CleanGiSampleDiffuseTexture(material, texCoord);
}

float CleanGiAlphaCoverage(PathTraceSmokeMaterial material, float2 texCoord)
{
    if ((material.flags & RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_DARK_KEY) != 0u)
    {
        const float3 decoded = saturate(CleanGiSampleDecodedDiffuseTexture(material, texCoord).rgb);
        return 1.0 - max(max(decoded.r, decoded.g), decoded.b);
    }
    if ((material.flags & RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_LUMA) != 0u)
    {
        const float3 decoded = saturate(CleanGiSampleDecodedDiffuseTexture(material, texCoord).rgb);
        return max(max(decoded.r, decoded.g), decoded.b);
    }
    if ((material.flags & RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_MAGENTA_KEY) != 0u)
    {
        const float3 decoded = saturate(CleanGiSampleDecodedDiffuseTexture(material, texCoord).rgb);
        const float keyDistance = max(abs(decoded.r - 1.0), max(abs(decoded.g), abs(decoded.b - 1.0)));
        return keyDistance <= 0.08 ? 0.0 : 1.0;
    }
    return saturate(CleanGiSampleAlphaTexture(material, texCoord).a);
}

float CleanGiSmokeLinear1(float c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 CleanGiSmokeLinear3(float3 c)
{
    return float3(CleanGiSmokeLinear1(c.r), CleanGiSmokeLinear1(c.g), CleanGiSmokeLinear1(c.b));
}

void CleanGiSmokePBRFromSpecmap(float3 specMap, out float3 F0, out float roughness)
{
    const float specLum = dot(float3(0.2125, 0.7154, 0.0721), specMap);
    F0 = float3(0.04, 0.04, 0.04);
    const float contrastMid = 0.214;
    const float contrastAmount = 2.0;
    float contrast = saturate((specLum - contrastMid) / (1.0 - contrastMid));
    contrast += saturate(specLum / contrastMid) - 1.0;
    contrast = exp2(contrastAmount * contrast);
    F0 *= contrast;
    const float linearBrightness = CleanGiSmokeLinear1(2.0 * specLum);
    const float specPow = max(0.0, ((8.0 * linearBrightness) / max(F0.y, 1.0e-4)) - 2.0);
    F0 *= min(1.0, linearBrightness / max(F0.y * 0.25, 1.0e-4));
    roughness = sqrt(2.0 / (specPow + 2.0));
    const float glossiness = saturate(1.0 - roughness);
    const float metallic = step(0.7, glossiness);
    const float3 glossColor = CleanGiSmokeLinear3(specMap.rgb);
    F0 = lerp(F0, glossColor, metallic);
    roughness = sqrt(roughness);
}

bool CleanGiToyFakePBRSpecularEnabled()
{
    return (((uint)TextureInfo.w) & RT_SMOKE_TEXTURE_FLAG_TOY_FAKE_PBR_SPECULAR) != 0u;
}

bool CleanGiSpecularProducerActive()
{
    return CleanRestirGiSpecularProducerEnabled != 0u;
}

bool CleanGiSpecularSeedProducerActive()
{
    return CleanRestirGiSpecularProducerEnabled == 1u;
}

bool CleanGiMixedFirstIndirectProducerActive()
{
    return CleanGiSpecularProducerActive() && !CleanGiSpecularSeedProducerActive();
}

bool CleanGiGlossySecondRayActive()
{
    return CleanRestirGiGlossySecondRayEnabled != 0u && CleanGiMixedFirstIndirectProducerActive();
}

bool CleanGiSurfaceSupportsSpecularProducer(RAB_Surface surface)
{
    if (!CleanGiSpecularProducerActive() || !RAB_SurfaceSupportsOpaqueDiffuseBrdf(surface))
    {
        return false;
    }

    const float roughness = saturate(GetRoughness(surface.material));
    const float specularLum = CleanGiLuminance(GetSpecularF0(surface.material));
    return specularLum >= CLEAN_RESTIR_GI_SPECULAR_PRODUCER_MIN_F0 &&
        (roughness <= CLEAN_RESTIR_GI_SPECULAR_PRODUCER_MAX_ROUGHNESS ||
            specularLum >= CLEAN_RESTIR_GI_SPECULAR_PRODUCER_METAL_F0);
}

bool CleanGiSurfaceSupportsGlossySecondRay(RAB_Surface surface)
{
    if (!CleanGiGlossySecondRayActive() || !RAB_SurfaceSupportsOpaqueDiffuseBrdf(surface))
    {
        return false;
    }

    const float roughness = saturate(GetRoughness(surface.material));
    const float specularLum = CleanGiLuminance(GetSpecularF0(surface.material));
    return specularLum >= CLEAN_RESTIR_GI_SPECULAR_PRODUCER_MIN_F0 &&
        roughness <= saturate(CleanRestirGiGlossySecondRayMaxRoughness);
}

float3 CleanGiSpecularProducerEligibilityColor(RAB_Surface surface)
{
    if (!CleanGiSpecularProducerActive())
    {
        return float3(0.0, 0.0, 0.25);
    }
    if (!RAB_SurfaceSupportsOpaqueDiffuseBrdf(surface))
    {
        return float3(0.08, 0.08, 0.08);
    }

    const float roughness = saturate(GetRoughness(surface.material));
    const float specularLum = CleanGiLuminance(GetSpecularF0(surface.material));
    const bool eligible = CleanGiSurfaceSupportsSpecularProducer(surface);
    return float3(eligible ? 1.0 : 0.0, saturate(1.0 - roughness), saturate(specularLum * 8.0));
}

float3 CleanGiSpecularReuseStateColor(RAB_Surface surface, RTXDI_GIReservoir reservoir)
{
    if (!CleanGiSpecularProducerActive())
    {
        return float3(0.0, 0.0, 0.25);
    }
    if (!RAB_IsSurfaceValid(surface))
    {
        return float3(0.08, 0.08, 0.08);
    }
    if (!CleanGiSurfaceSupportsSpecularProducer(surface))
    {
        return float3(0.02, 0.12, 0.04);
    }
    if (CleanGiSpecularProducerNeedsReuseQuarantine(surface))
    {
        return float3(1.0, 0.0, 0.0);
    }
    if (CleanRestirGiTemporalEnabled == 0u)
    {
        return float3(1.0, 0.45, 0.0);
    }
    if (!RTXDI_IsValidGIReservoir(reservoir))
    {
        return float3(0.85, 0.65, 0.0);
    }

    const float mRamp = saturate((float)reservoir.M / max((float)CleanRestirGiMaxHistoryLength, 1.0));
    const float ageRamp = saturate((float)reservoir.age / max((float)CleanRestirGiMaxReservoirAge, 1.0));
    return float3(0.0, max(mRamp, 0.12), ageRamp);
}

struct CleanGiIndirectLobeResult
{
    float3 diffuse;
    float3 specular;
    float hitDistance;
};

float3 CleanGiFresnelSchlick(float3 f0, float cosine)
{
    const float x = 1.0 - saturate(cosine);
    const float x2 = x * x;
    const float x5 = x2 * x2 * x;
    return saturate(f0) + (float3(1.0, 1.0, 1.0) - saturate(f0)) * x5;
}

float CleanGiSmithG1(float noV, float roughness)
{
    const float a = max(saturate(roughness) * saturate(roughness), 1.0e-3);
    const float noV2 = noV * noV;
    if (noV2 <= 0.0)
    {
        return 0.0;
    }
    const float tan2 = max(0.0, (1.0 - noV2) / noV2);
    return 2.0 / (1.0 + sqrt(1.0 + a * a * tan2));
}

float3 CleanGiEvaluateIndirectSpecularLobe(RAB_Surface surface, float3 sampleDir, float3 incomingRadiance)
{
    const float3 specularF0 = max(GetSpecularF0(surface.material), float3(0.0, 0.0, 0.0));
    if (max(max(specularF0.r, specularF0.g), specularF0.b) <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float3 normal = CleanGiSafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    const float3 geometryNormal = CleanGiSafeNormalize(RAB_GetSurfaceGeoNormal(surface), normal);
    const float3 viewDir = CleanGiSafeNormalize(RAB_GetSurfaceViewDir(surface), -normal);
    if (dot(geometryNormal, sampleDir) <= 0.0 || dot(geometryNormal, viewDir) <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float ndotl = saturate(dot(normal, sampleDir));
    const float ndotv = saturate(dot(normal, viewDir));
    if (ndotl <= 0.0 || ndotv <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float3 halfVector = CleanGiSafeNormalize(sampleDir + viewDir, normal);
    const float ndoth = saturate(dot(normal, halfVector));
    const float ldotH = saturate(dot(sampleDir, halfVector));
    if (ndoth <= 0.0 || ldotH <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float roughness = max(saturate(GetRoughness(surface.material)), 0.035);
    const float alpha = max(roughness * roughness, 1.0e-3);
    const float alphaSquared = alpha * alpha;
    const float denom = max(ndoth * ndoth * (alphaSquared - 1.0) + 1.0, 1.0e-4);
    const float distribution = alphaSquared / max(RTXDI_PI * denom * denom, 1.0e-6);
    const float geometry = CleanGiSmithG1(ndotl, roughness) * CleanGiSmithG1(ndotv, roughness);
    const float3 fresnel = CleanGiFresnelSchlick(specularF0, ldotH);
    const float3 reflected =
        fresnel *
        max(incomingRadiance, float3(0.0, 0.0, 0.0)) *
        (distribution * geometry * ndotl / max(4.0 * ndotl * ndotv, 1.0e-5));
    return CleanGiAllFinite3(reflected) ? reflected : float3(0.0, 0.0, 0.0);
}

CleanGiIndirectLobeResult CleanGiEvaluateIndirectLobesSplit(RAB_Surface surface, float3 sampleDir, float3 incomingRadiance)
{
    CleanGiIndirectLobeResult result = (CleanGiIndirectLobeResult)0;
    if (!RAB_SurfaceSupportsOpaqueDiffuseBrdf(surface))
    {
        return result;
    }

    const float3 normal = CleanGiSafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    if (dot(RAB_GetSurfaceGeoNormal(surface), sampleDir) <= 0.0)
    {
        return result;
    }

    const float ndotl = saturate(dot(normal, sampleDir));
    if (ndotl <= 0.0)
    {
        return result;
    }

    const float3 safeRadiance = max(incomingRadiance, float3(0.0, 0.0, 0.0));
    const float3 viewDir = CleanGiSafeNormalize(RAB_GetSurfaceViewDir(surface), normal);
    const float3 halfVector = CleanGiSafeNormalize(sampleDir + viewDir, normal);
    const float3 fresnel = CleanGiFresnelSchlick(max(GetSpecularF0(surface.material), float3(0.0, 0.0, 0.0)), saturate(dot(viewDir, halfVector)));
    const float diffuseWeight = saturate(1.0 - max(max(fresnel.r, fresnel.g), fresnel.b));
    result.diffuse = GetDiffuseAlbedo(surface.material) * safeRadiance * (diffuseWeight * ndotl / RTXDI_PI);
    result.specular = CleanGiEvaluateIndirectSpecularLobe(surface, sampleDir, safeRadiance);
    if (!CleanGiAllFinite3(result.diffuse))
    {
        result.diffuse = float3(0.0, 0.0, 0.0);
    }
    if (!CleanGiAllFinite3(result.specular))
    {
        result.specular = float3(0.0, 0.0, 0.0);
    }
    return result;
}

float3 CleanGiEvaluateIndirectLobes(RAB_Surface surface, float3 sampleDir, float3 incomingRadiance)
{
    const CleanGiIndirectLobeResult lobes = CleanGiEvaluateIndirectLobesSplit(surface, sampleDir, incomingRadiance);
    const float3 reflected = lobes.diffuse + lobes.specular;
    return CleanGiAllFinite3(reflected) ? reflected : float3(0.0, 0.0, 0.0);
}

float3 CleanGiSampleDirectSpecular(PathTraceSmokeMaterial material, float2 texCoord)
{
    if (material.specularTextureIndex == 0xffffffffu ||
        ((((uint)TextureInfo.w) & RT_SMOKE_TEXTURE_FLAG_USE_SPECULAR_MAPS) == 0u))
    {
        return float3(0.0, 0.0, 0.0);
    }
    return saturate(CleanGiSampleTexture(
        material.specularTextureIndex,
        material.specularTextureWidth,
        material.specularTextureHeight,
        texCoord,
        float4(0.0, 0.0, 0.0, 1.0)).rgb);
}

float3 CleanGiBuildPerpendicular(float3 normal)
{
    const float3 axis = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    return CleanGiSafeNormalize(cross(axis, normal), float3(1.0, 0.0, 0.0));
}

float3 CleanGiDecodeNormalTexture(
    PathTraceSmokeMaterial material,
    float2 texCoord,
    float3 normal,
    float3 tangent,
    float3 bitangent)
{
    if (material.normalTextureIndex == 0xffffffffu ||
        ((((uint)TextureInfo.w) & RT_SMOKE_TEXTURE_FLAG_USE_NORMAL_MAPS) == 0u))
    {
        return normal;
    }

    const float4 bump = CleanGiSampleTexture(
        material.normalTextureIndex,
        material.normalTextureWidth,
        material.normalTextureHeight,
        texCoord,
        float4(0.5, 0.5, 1.0, 1.0)) * 2.0 - 1.0;
    if (!all(bump == bump))
    {
        return normal;
    }

    const float2 normalXY = SmokeMatClassNormalXY(material, bump, 0.0);
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
    return CleanGiSafeNormalize(tangent * decoded.x + bitangent * decoded.y + normal * decoded.z, normal);
}

float3 CleanGiConstrainShadingNormal(float3 shadingNormal, float3 geometryNormal)
{
    const float minGeometryDot = 0.02;
    geometryNormal = CleanGiSafeNormalize(geometryNormal, float3(0.0, 0.0, 1.0));
    shadingNormal = CleanGiSafeNormalize(shadingNormal, geometryNormal);
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

    tangentComponent *= rsqrt(tangentLengthSquared);
    const float tangentScale = sqrt(max(1.0 - minGeometryDot * minGeometryDot, 0.0));
    return CleanGiSafeNormalize(tangentComponent * tangentScale + geometryNormal * minGeometryDot, geometryNormal);
}

bool CleanGiMaterialUsesUnlitColorFallback(PathTraceSmokeMaterial material, uint surfaceClass, uint translucentSubtype)
{
    if (surfaceClass != RT_SMOKE_SURFACE_CLASS_TRANSLUCENT)
    {
        return false;
    }
    if (translucentSubtype == RT_SMOKE_TRANSLUCENT_SUBTYPE_OBJECT_GLASS ||
        translucentSubtype == RT_SMOKE_TRANSLUCENT_SUBTYPE_PORTAL_WINDOW ||
        translucentSubtype == RT_SMOKE_TRANSLUCENT_SUBTYPE_GUI_SCREEN)
    {
        return false;
    }
    if ((material.flags & RT_SMOKE_MATERIAL_ADDITIVE_DECAL) != 0u)
    {
        return true;
    }
    return translucentSubtype == RT_SMOKE_TRANSLUCENT_SUBTYPE_SMOKE_PARTICLE &&
        material.alphaTextureIndex == 0xffffffffu;
}

float3 CleanGiSampleEmissiveRadiance(PathTraceSmokeMaterial material, float2 texCoord, uint surfaceClass, bool activeEmissiveStage)
{
    if ((material.flags & RT_SMOKE_MATERIAL_EMISSIVE) == 0u ||
        !activeEmissiveStage ||
        surfaceClass == RT_SMOKE_SURFACE_CLASS_SKINNED_DEFORMED ||
        ((((uint)TextureInfo.w) & RT_SMOKE_TEXTURE_FLAG_USE_EMISSIVE_MAPS) == 0u))
    {
        return float3(0.0, 0.0, 0.0);
    }
    const float emissiveScale = max(ToyPathInfo.z, 0.0);
    if (emissiveScale <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }
    float3 emissiveTexel = float3(1.0, 1.0, 1.0);
    if (material.emissiveTextureIndex != 0xffffffffu)
    {
        emissiveTexel = saturate(CleanGiSampleTexture(material.emissiveTextureIndex, material.emissiveTextureWidth, material.emissiveTextureHeight, texCoord, float4(1.0, 1.0, 1.0, 1.0)).rgb);
    }
    return max(material.emissiveColor.rgb, float3(0.0, 0.0, 0.0)) * emissiveTexel * (1.75 * emissiveScale);
}

RAB_Material CleanGiBuildMaterialFromHit(
    uint materialId,
    uint materialIndex,
    PathTraceSmokeMaterial smokeMaterial,
    float2 texCoord,
    uint surfaceClass,
    uint translucentSubtype,
    uint triangleClassAndFlags,
    float4 vertexColor)
{
    float3 materialAlbedo = CleanGiSampleSurfaceAlbedo(smokeMaterial, texCoord, surfaceClass, translucentSubtype, vertexColor).rgb;
    const float3 specularColor = CleanGiSampleDirectSpecular(smokeMaterial, texCoord);
    float3 specularF0 = specularColor;
    float roughness = 1.0;
    if (CleanGiToyFakePBRSpecularEnabled())
    {
        CleanGiSmokePBRFromSpecmap(saturate(specularColor), specularF0, roughness);
    }
    SmokeApplyMaterialClassifierBsdfWithSpecularTexel(smokeMaterial, materialAlbedo, saturate(specularColor), specularF0, roughness);
    const bool fullMetalOverride = SmokeMaterialHasFullMetalOverride(smokeMaterial);
    SmokeApplyFullMetalOverride(smokeMaterial, materialAlbedo, specularF0);
    if ((smokeMaterial.padding0 & RT_SMOKE_MATERIAL_OVERRIDE_ZERO_ROUGHNESS) != 0u)
    {
        roughness = 0.0;
        if (!fullMetalOverride)
        {
            specularF0 = max(specularF0, float3(0.85, 0.85, 0.85));
        }
    }
    const bool activeEmissiveStage = (triangleClassAndFlags & RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF) == 0u;

    RAB_Material material = RAB_EmptyMaterial();
    material.materialId = materialId;
    material.materialIndex = materialIndex;
    material.flags = smokeMaterial.flags;
    material.alphaCutoff = smokeMaterial.alphaCutoff;
    material.diffuseAlbedo = saturate(materialAlbedo);
    material.roughness = saturate(roughness);
    material.specularF0 = max(specularF0, float3(0.0, 0.0, 0.0));
    material.opacity = CleanGiAlphaCoverage(smokeMaterial, texCoord);
    material.emissiveRadiance = CleanGiSampleEmissiveRadiance(smokeMaterial, texCoord, surfaceClass, activeEmissiveStage);
    if (activeEmissiveStage && CleanGiMaterialUsesUnlitColorFallback(smokeMaterial, surfaceClass, translucentSubtype))
    {
        material.emissiveRadiance = max(material.emissiveRadiance, material.diffuseAlbedo);
    }
    material.emissiveTextureIndex = smokeMaterial.emissiveTextureIndex;
    return material;
}

// ---------------------------------------------------------------------------
// Secondary-hit geometry reconstruction
// ---------------------------------------------------------------------------

float3 CleanGiTransformRigidRoutePoint(PathTraceRigidRouteInstance routeInstance, float3 localPoint)
{
    return float3(
        dot(routeInstance.currentObjectToWorld0, float4(localPoint, 1.0)),
        dot(routeInstance.currentObjectToWorld1, float4(localPoint, 1.0)),
        dot(routeInstance.currentObjectToWorld2, float4(localPoint, 1.0)));
}

float3 CleanGiTransformRigidRouteVector(PathTraceRigidRouteInstance routeInstance, float3 localVector)
{
    return float3(
        dot(routeInstance.currentObjectToWorld0.xyz, localVector),
        dot(routeInstance.currentObjectToWorld1.xyz, localVector),
        dot(routeInstance.currentObjectToWorld2.xyz, localVector));
}

bool CleanGiLoadTriangleGeometryFull(
    uint instanceId,
    uint primitiveIndex,
    out float3 p0, out float3 p1, out float3 p2,
    out float3 n0, out float3 n1, out float3 n2,
    out float2 uv0, out float2 uv1, out float2 uv2,
    out float4 c0, out float4 c1, out float4 c2,
    out float4 c20, out float4 c21, out float4 c22)
{
    p0 = float3(0.0, 0.0, 0.0);
    p1 = float3(0.0, 0.0, 0.0);
    p2 = float3(0.0, 0.0, 0.0);
    n0 = float3(0.0, 0.0, 1.0);
    n1 = float3(0.0, 0.0, 1.0);
    n2 = float3(0.0, 0.0, 1.0);
    uv0 = float2(0.0, 0.0);
    uv1 = float2(0.0, 0.0);
    uv2 = float2(0.0, 0.0);
    c0 = float4(1.0, 1.0, 1.0, 1.0);
    c1 = float4(1.0, 1.0, 1.0, 1.0);
    c2 = float4(1.0, 1.0, 1.0, 1.0);
    c20 = float4(0.5, 0.5, 0.5, 0.5);
    c21 = float4(0.5, 0.5, 0.5, 0.5);
    c22 = float4(0.5, 0.5, 0.5, 0.5);

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
        p0 = v0.position.xyz; p1 = v1.position.xyz; p2 = v2.position.xyz;
        n0 = v0.normal.xyz; n1 = v1.normal.xyz; n2 = v2.normal.xyz;
        uv0 = v0.texCoord.xy; uv1 = v1.texCoord.xy; uv2 = v2.texCoord.xy;
        c0 = v0.color; c1 = v1.color; c2 = v2.color;
        c20 = v0.color2; c21 = v1.color2; c22 = v2.color2;
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
        p0 = v0.position.xyz; p1 = v1.position.xyz; p2 = v2.position.xyz;
        n0 = v0.normal.xyz; n1 = v1.normal.xyz; n2 = v2.normal.xyz;
        uv0 = v0.texCoord.xy; uv1 = v1.texCoord.xy; uv2 = v2.texCoord.xy;
        c0 = v0.color; c1 = v1.color; c2 = v2.color;
        c20 = v0.color2; c21 = v1.color2; c22 = v2.color2;
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
    p0 = CleanGiTransformRigidRoutePoint(route, v0.position.xyz);
    p1 = CleanGiTransformRigidRoutePoint(route, v1.position.xyz);
    p2 = CleanGiTransformRigidRoutePoint(route, v2.position.xyz);
    n0 = CleanGiTransformRigidRouteVector(route, v0.normal.xyz);
    n1 = CleanGiTransformRigidRouteVector(route, v1.normal.xyz);
    n2 = CleanGiTransformRigidRouteVector(route, v2.normal.xyz);
    uv0 = v0.texCoord.xy; uv1 = v1.texCoord.xy; uv2 = v2.texCoord.xy;
    c0 = v0.color; c1 = v1.color; c2 = v2.color;
    c20 = v0.color2; c21 = v1.color2; c22 = v2.color2;
    return true;
}

bool CleanGiLoadTriangleGeometry(
    uint instanceId,
    uint primitiveIndex,
    out float3 p0, out float3 p1, out float3 p2,
    out float2 uv0, out float2 uv1, out float2 uv2)
{
    p0 = float3(0.0, 0.0, 0.0);
    p1 = float3(0.0, 0.0, 0.0);
    p2 = float3(0.0, 0.0, 0.0);
    uv0 = float2(0.0, 0.0);
    uv1 = float2(0.0, 0.0);
    uv2 = float2(0.0, 0.0);

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
        p0 = v0.position.xyz; p1 = v1.position.xyz; p2 = v2.position.xyz;
        uv0 = v0.texCoord.xy; uv1 = v1.texCoord.xy; uv2 = v2.texCoord.xy;
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
        p0 = v0.position.xyz; p1 = v1.position.xyz; p2 = v2.position.xyz;
        uv0 = v0.texCoord.xy; uv1 = v1.texCoord.xy; uv2 = v2.texCoord.xy;
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
    p0 = CleanGiTransformRigidRoutePoint(route, v0.position.xyz);
    p1 = CleanGiTransformRigidRoutePoint(route, v1.position.xyz);
    p2 = CleanGiTransformRigidRoutePoint(route, v2.position.xyz);
    uv0 = v0.texCoord.xy; uv1 = v1.texCoord.xy; uv2 = v2.texCoord.xy;
    return true;
}

bool CleanGiLoadHitTexCoord(uint instanceId, uint primitiveIndex, float2 barycentrics, out float2 texCoord)
{
    texCoord = float2(0.0, 0.0);
    float3 p0, p1, p2;
    float2 uv0, uv1, uv2;
    if (!CleanGiLoadTriangleGeometry(instanceId, primitiveIndex, p0, p1, p2, uv0, uv1, uv2))
    {
        return false;
    }
    const float b1 = saturate(barycentrics.x);
    const float b2 = saturate(barycentrics.y);
    const float b0 = saturate(1.0 - b1 - b2);
    texCoord = uv0 * b0 + uv1 * b1 + uv2 * b2;
    return true;
}

float CleanGiHashToUnitFloat(uint hash)
{
    hash ^= hash >> 16;
    hash *= 2246822519u;
    hash ^= hash >> 13;
    hash *= 3266489917u;
    hash ^= hash >> 16;
    return ((hash >> 8) & 0x00ffffffu) * (1.0 / 16777215.0);
}

float CleanGiVisibilityRandom(uint2 pixel, uint instanceId, uint primitiveIndex, uint salt)
{
    return CleanGiHashToUnitFloat(
        instanceId * 1597334677u ^
        primitiveIndex * 3812015801u ^
        pixel.x * 2798796415u ^
        pixel.y * 1979697957u ^
        CleanRestirGiFrameIndex * 3266489917u ^
        salt);
}

bool CleanGiMaterialRejectsHit(uint2 pixel, uint instanceId, uint primitiveIndex, float2 barycentrics, bool shadowRay)
{
    const uint materialIndex = CleanGiLoadTriangleMaterialIndex(instanceId, primitiveIndex);
    if (materialIndex >= (uint)TextureInfo.z)
    {
        return false;
    }

    float3 p0, p1, p2;
    float3 n0, n1, n2;
    float2 uv0, uv1, uv2;
    float4 c0, c1, c2;
    float4 c20, c21, c22;
    if (!CleanGiLoadTriangleGeometryFull(
        instanceId,
        primitiveIndex,
        p0, p1, p2,
        n0, n1, n2,
        uv0, uv1, uv2,
        c0, c1, c2,
        c20, c21, c22))
    {
        return false;
    }
    const float b1 = saturate(barycentrics.x);
    const float b2 = saturate(barycentrics.y);
    const float b0 = saturate(1.0 - b1 - b2);
    const float2 texCoord = uv0 * b0 + uv1 * b1 + uv2 * b2;
    const float4 vertexColor = saturate(c0 * b0 + c1 * b1 + c2 * b2);

    const PathTraceSmokeMaterial material = CleanGiLoadSmokeMaterial(materialIndex);
    const uint triangleClassAndFlags = CleanGiLoadTriangleClassAndFlags(instanceId, primitiveIndex);
    const uint surfaceClass = CleanGiTriangleSurfaceClass(triangleClassAndFlags);
    const uint translucentSubtype = CleanGiTriangleTranslucentSubtype(triangleClassAndFlags);
    if (surfaceClass == RT_SMOKE_SURFACE_CLASS_TRANSLUCENT &&
        translucentSubtype == RT_SMOKE_TRANSLUCENT_SUBTYPE_GUI_SCREEN &&
        vertexColor.a <= 0.03)
    {
        return true;
    }

    const float coverage = saturate(CleanGiAlphaCoverage(material, texCoord));
    if ((material.flags & RT_SMOKE_MATERIAL_ALPHA_TEST) != 0u &&
        coverage < material.alphaCutoff)
    {
        return true;
    }

    float visibilityCoverage = coverage;
    if ((material.flags & RT_SMOKE_MATERIAL_ADDITIVE_DECAL) != 0u)
    {
        const float3 albedo = saturate(CleanGiSampleDiffuseTexture(material, texCoord).rgb);
        visibilityCoverage = (material.flags & RT_SMOKE_MATERIAL_ADDITIVE_DECAL_WHITE_KEY) != 0u
            ? 1.0 - min(min(albedo.r, albedo.g), albedo.b)
            : max(max(albedo.r, albedo.g), albedo.b);
        visibilityCoverage = saturate(visibilityCoverage * 0.5);
    }
    else if ((material.flags & RT_SMOKE_MATERIAL_FILTER_DECAL) != 0u)
    {
        const float3 albedo = saturate(CleanGiSampleDiffuseTexture(material, texCoord).rgb);
        const bool blackKey = (material.flags & RT_SMOKE_MATERIAL_FILTER_DECAL_BLACK_KEY) != 0u;
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
    return CleanGiVisibilityRandom(pixel, instanceId, primitiveIndex, salt) > visibilityCoverage;
}

bool CleanGiResolveTriangleNormalAndArea(
    float3 p0,
    float3 p1,
    float3 p2,
    float3 payloadNormal,
    out float3 triangleNormal,
    out float triangleArea)
{
    triangleNormal = float3(0.0, 0.0, 1.0);
    triangleArea = 0.0;
    if (!CleanGiAllFinite3(p0) || !CleanGiAllFinite3(p1) || !CleanGiAllFinite3(p2))
    {
        return false;
    }

    const float3 crossValue = cross(p1 - p0, p2 - p0);
    const float doubleAreaSquared = dot(crossValue, crossValue);
    if (doubleAreaSquared <= 1.0e-12)
    {
        return false;
    }

    const float doubleArea = sqrt(doubleAreaSquared);
    triangleNormal = crossValue / doubleArea;
    const float3 safePayloadNormal = CleanGiSafeNormalize(payloadNormal, triangleNormal);
    if (dot(triangleNormal, safePayloadNormal) < 0.0)
    {
        triangleNormal = -triangleNormal;
    }
    triangleArea = 0.5 * doubleArea;
    return triangleArea > 1.0e-6 && triangleArea < 3.402823e+38;
}

// ---------------------------------------------------------------------------
// Analytic light universe at the secondary vertex (GI-I-04)
// ---------------------------------------------------------------------------

bool CleanGiAnalyticPayloadValid(PathTraceDoomAnalyticLightCandidate light)
{
    const float3 radiance = max(light.colorAndIntensity.rgb, float3(0.0, 0.0, 0.0));
    const float luminance = CleanGiLuminance(radiance);
    return CleanGiAllFinite3(light.originAndRadius.xyz) &&
        CleanGiAllFinite3(radiance) &&
        luminance > 0.0 &&
        light.originAndRadius.w > 0.0 &&
        light.originAndRadius.w < 3.402823e+38 &&
        light.doomRadiusAndArea.x > 0.0 &&
        light.doomRadiusAndArea.x < 3.402823e+38;
}

float CleanGiDoomAnalyticSphereArea(float radius)
{
    const float safeRadius = max(radius, 0.01);
    return max(4.0 * RTXDI_PI * safeRadius * safeRadius, 1.0e-4);
}

RAB_LightInfo CleanGiBuildAnalyticLightInfo(PathTraceDoomAnalyticLightCandidate light, uint lightIndex)
{
    RAB_LightInfo lightInfo = RAB_EmptyLightInfo();
    if (!CleanGiAnalyticPayloadValid(light))
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
    lightInfo.area = CleanGiDoomAnalyticSphereArea(lightInfo.radius);
    lightInfo.weight = CleanGiLuminance(lightInfo.radiance) * lightInfo.area * lightInfo.influenceRadius;
    return lightInfo;
}

RAB_LightInfo CleanGiBuildRluAnalyticLightInfo(PathTraceUnifiedLightRecord light, uint lightIndex)
{
    RAB_LightInfo lightInfo = RAB_EmptyLightInfo();
    if (light.type != PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC ||
        !CleanGiAllFinite3(light.positionAndRadius.xyz) ||
        !CleanGiAllFinite3(light.radianceAndLuminance.rgb))
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
    lightInfo.normal = float3(0.0, 0.0, 1.0);
    lightInfo.radiance = max(light.radianceAndLuminance.rgb, float3(0.0, 0.0, 0.0)) * max(CleanRtxdiDiDoomAnalyticLightInfo.z, 0.0);
    lightInfo.influenceRadius = max(light.uvOrDoomParams.x, lightInfo.radius);
    lightInfo.area = light.normalAndArea.w > 1.0e-6 ? max(light.normalAndArea.w, 1.0e-4) : CleanGiDoomAnalyticSphereArea(lightInfo.radius);
    lightInfo.weight = CleanGiLuminance(lightInfo.radiance) * lightInfo.area * lightInfo.influenceRadius;
    return lightInfo;
}

RAB_LightInfo CleanGiBuildEmissiveLightInfo(uint sourceIndex, uint lightIndex)
{
    RAB_LightInfo lightInfo = RAB_EmptyLightInfo();
    if (sourceIndex >= CleanRtxdiDiCurrentEmissiveTriangleCount)
    {
        return lightInfo;
    }

    const PathTraceSmokeEmissiveTriangle emissiveTriangle = SmokeEmissiveTriangles[sourceIndex];
    if (emissiveTriangle.materialIndex >= (uint)TextureInfo.z ||
        (emissiveTriangle.padding0 & RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF) != 0u)
    {
        return lightInfo;
    }

    const PathTraceSmokeMaterial emissiveMaterial = CleanGiLoadSmokeMaterial(emissiveTriangle.materialIndex);
    if ((emissiveMaterial.flags & RT_SMOKE_MATERIAL_EMISSIVE) == 0u)
    {
        return lightInfo;
    }
    const uint triangleClassAndFlags = CleanGiLoadTriangleClassAndFlags(emissiveTriangle.instanceId, emissiveTriangle.primitiveIndex);
    const uint surfaceClass = CleanGiTriangleSurfaceClass(triangleClassAndFlags);
    const bool activeEmissiveStage =
        (emissiveTriangle.padding0 & RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF) == 0u &&
        (triangleClassAndFlags & RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF) == 0u;

    float3 p0, p1, p2;
    float2 uv0, uv1, uv2;
    if (!CleanGiLoadTriangleGeometry(emissiveTriangle.instanceId, emissiveTriangle.primitiveIndex, p0, p1, p2, uv0, uv1, uv2))
    {
        return lightInfo;
    }

    float3 triangleNormal;
    float triangleArea;
    if (!CleanGiResolveTriangleNormalAndArea(p0, p1, p2, emissiveTriangle.normalAndLuminance.xyz, triangleNormal, triangleArea))
    {
        return lightInfo;
    }

    const float3 centroidRadiance = CleanGiSampleEmissiveRadiance(emissiveMaterial, emissiveTriangle.centroidUvAndWeight.xy, surfaceClass, activeEmissiveStage);
    if (CleanGiLuminance(centroidRadiance) <= 0.0)
    {
        return lightInfo;
    }

    lightInfo.lightType = RAB_LIGHT_TYPE_EMISSIVE_TRIANGLE;
    lightInfo.lightIndex = lightIndex;
    lightInfo.unifiedLightType = PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE;
    lightInfo.materialIndex = emissiveTriangle.materialIndex;
    lightInfo.flags = emissiveTriangle.flags;
    lightInfo.position = emissiveTriangle.centerAndArea.xyz;
    lightInfo.radius = 0.0;
    lightInfo.normal = triangleNormal;
    lightInfo.radiance = centroidRadiance;
    lightInfo.influenceRadius = 0.0;
    lightInfo.area = triangleArea;
    lightInfo.weight = max(emissiveTriangle.sampleWeightAndPdf.x, CleanGiLuminance(centroidRadiance) * triangleArea);
    lightInfo.sourceIndex = sourceIndex;
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

RAB_LightInfo CleanGiLoadCurrentRluLightInfo(uint denseRluIndex)
{
    if (denseRluIndex >= CleanRtxdiDiRluCurrentLightCount)
    {
        return RAB_EmptyLightInfo();
    }

    const PathTraceUnifiedLightRecord light = CleanRestirGiRluCurrentLights[denseRluIndex];
    if (light.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
    {
        if (light.sourceIndex == PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX)
        {
            return RAB_EmptyLightInfo();
        }
        return CleanGiBuildEmissiveLightInfo(light.sourceIndex, denseRluIndex);
    }
    if (light.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
    {
        if (light.sourceIndex != PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX &&
            light.sourceIndex < CleanRtxdiDiDoomAnalyticFullCurrentCount)
        {
            return CleanGiBuildAnalyticLightInfo(DoomAnalyticLights[light.sourceIndex], denseRluIndex);
        }
        return CleanGiBuildRluAnalyticLightInfo(light, denseRluIndex);
    }
    return RAB_EmptyLightInfo();
}

float CleanGiSecondaryNeeMisWeight(RAB_Surface secondarySurface, RAB_LightSample lightSample, float3 lightDir)
{
    if (!RAB_IsReplayableLightSample(lightSample) || lightSample.solidAnglePdf <= 0.0)
    {
        return 0.0;
    }

    const float scatterPdf = RAB_GetSurfaceBrdfPdf(secondarySurface, lightDir);
    if (scatterPdf <= 0.0)
    {
        return 1.0;
    }

    return lightSample.solidAnglePdf / max(lightSample.solidAnglePdf + scatterPdf, 1.0e-6);
}

bool CleanGiAccumulateLightSample(
    inout float3 radiance,
    RAB_Surface secondarySurface,
    float3 hitGeometricNormal,
    RAB_LightInfo lightInfo,
    float sourcePdf,
    inout RTXDI_RandomSamplerState rng)
{
    if (!RAB_IsLightInfoValid(lightInfo) || sourcePdf <= 1.0e-8)
    {
        return false;
    }

    const float2 uv = float2(RAB_GetNextRandom(rng), RAB_GetNextRandom(rng));
    const RAB_LightSample lightSample = RAB_SamplePolymorphicLight(lightInfo, secondarySurface, uv);
    if (!RAB_IsReplayableLightSample(lightSample) ||
        lightSample.solidAnglePdf <= 1.0e-8 ||
        CleanGiLuminance(lightSample.radiance) <= 0.0)
    {
        return false;
    }

    float3 lightDir;
    float lightDistance;
    RAB_GetLightDirDistance(secondarySurface, lightSample, lightDir, lightDistance);
    const float ndotl = saturate(dot(RAB_GetSurfaceNormal(secondarySurface), lightDir));
    if (ndotl <= 0.0)
    {
        return false;
    }

    const float3 brdf = RAB_EvaluateSurfaceBrdf(secondarySurface, lightDir, RAB_GetSurfaceViewDir(secondarySurface));
    if (CleanGiLuminance(brdf) <= 0.0)
    {
        return false;
    }

    const float visibility = CleanGiTraceVisibility(
        RAB_GetSurfaceWorldPos(secondarySurface), hitGeometricNormal, lightSample.position);
    if (visibility <= 0.0)
    {
        return false;
    }

    const float misWeight = CleanGiSecondaryNeeMisWeight(secondarySurface, lightSample, lightDir);
    radiance += brdf * lightSample.radiance * ndotl * visibility * misWeight /
        max(sourcePdf * lightSample.solidAnglePdf, 1.0e-6);
    return true;
}

bool CleanGiEvaluateDirectLightSampleTarget(
    RAB_Surface secondarySurface,
    RAB_LightSample lightSample,
    out float targetPdf)
{
    targetPdf = 0.0;
    if (!RAB_IsReplayableLightSample(lightSample) ||
        lightSample.solidAnglePdf <= 1.0e-8 ||
        CleanGiLuminance(lightSample.radiance) <= 0.0)
    {
        return false;
    }

    float3 lightDir;
    float lightDistance;
    RAB_GetLightDirDistance(secondarySurface, lightSample, lightDir, lightDistance);
    const float ndotl = saturate(dot(RAB_GetSurfaceNormal(secondarySurface), lightDir));
    if (ndotl <= 0.0)
    {
        return false;
    }

    const float3 brdf = RAB_EvaluateSurfaceBrdf(secondarySurface, lightDir, RAB_GetSurfaceViewDir(secondarySurface));
    if (CleanGiLuminance(brdf) <= 0.0)
    {
        return false;
    }

    const float misWeight = CleanGiSecondaryNeeMisWeight(secondarySurface, lightSample, lightDir);
    targetPdf = CleanGiLuminance(brdf * lightSample.radiance * ndotl * misWeight) /
        max(lightSample.solidAnglePdf, 1.0e-6);
    return targetPdf > 1.0e-8 && targetPdf == targetPdf;
}

bool CleanGiAccumulateSelectedLightSample(
    inout float3 radiance,
    RAB_Surface secondarySurface,
    float3 hitGeometricNormal,
    RAB_LightSample lightSample,
    float sourcePdf)
{
    if (sourcePdf <= 1.0e-8)
    {
        return false;
    }

    float targetPdf;
    if (!CleanGiEvaluateDirectLightSampleTarget(secondarySurface, lightSample, targetPdf))
    {
        return false;
    }

    float3 lightDir;
    float lightDistance;
    RAB_GetLightDirDistance(secondarySurface, lightSample, lightDir, lightDistance);
    const float visibility = CleanGiTraceVisibility(
        RAB_GetSurfaceWorldPos(secondarySurface), hitGeometricNormal, lightSample.position);
    if (visibility <= 0.0)
    {
        return false;
    }

    const float ndotl = saturate(dot(RAB_GetSurfaceNormal(secondarySurface), lightDir));
    const float3 brdf = RAB_EvaluateSurfaceBrdf(secondarySurface, lightDir, RAB_GetSurfaceViewDir(secondarySurface));
    const float misWeight = CleanGiSecondaryNeeMisWeight(secondarySurface, lightSample, lightDir);
    radiance += brdf * lightSample.radiance * ndotl * visibility * misWeight /
        max(sourcePdf * lightSample.solidAnglePdf, 1.0e-6);
    return true;
}

bool CleanGiAccumulateRluRisLightSample(
    inout float3 radiance,
    RAB_Surface secondarySurface,
    float3 hitGeometricNormal,
    inout RTXDI_RandomSamplerState rng)
{
    const uint lightCount = CleanRtxdiDiRluCurrentLightCount;
    if (lightCount == 0u)
    {
        return false;
    }

    const uint candidateCount = clamp(CleanRestirGiSecondaryRluCandidateCount, 1u, 16u);
    const float invUniformSourcePdf = (float)lightCount;
    float weightSum = 0.0;
    float selectedTargetPdf = 0.0;
    RAB_LightSample selectedSample = RAB_EmptyLightSample();

    [loop]
    for (uint candidateIndex = 0u; candidateIndex < candidateCount; ++candidateIndex)
    {
        const uint denseIndex = min((uint)(RAB_GetNextRandom(rng) * (float)lightCount), lightCount - 1u);
        const RAB_LightInfo lightInfo = CleanGiLoadCurrentRluLightInfo(denseIndex);
        if (!RAB_IsLightInfoValid(lightInfo))
        {
            continue;
        }

        const float2 uv = float2(RAB_GetNextRandom(rng), RAB_GetNextRandom(rng));
        const RAB_LightSample candidateSample = RAB_SamplePolymorphicLight(lightInfo, secondarySurface, uv);
        float targetPdf;
        if (!CleanGiEvaluateDirectLightSampleTarget(secondarySurface, candidateSample, targetPdf))
        {
            continue;
        }

        const float risWeight = targetPdf * invUniformSourcePdf;
        weightSum += risWeight;
        if (RAB_GetNextRandom(rng) * weightSum <= risWeight)
        {
            selectedSample = candidateSample;
            selectedTargetPdf = targetPdf;
        }
    }

    if (selectedTargetPdf <= 1.0e-8 || weightSum <= 1.0e-8)
    {
        return false;
    }

    const float selectedSourcePdf = selectedTargetPdf * (float)candidateCount / max(weightSum, 1.0e-8);
    return CleanGiAccumulateSelectedLightSample(
        radiance,
        secondarySurface,
        hitGeometricNormal,
        selectedSample,
        selectedSourcePdf);
}

bool CleanGiSelectEmissiveDistributionSample(
    inout RTXDI_RandomSamplerState rng,
    out uint sourceIndex,
    out float sourcePdf)
{
    sourceIndex = 0xffffffffu;
    sourcePdf = 0.0;

    const uint distributionCount = min((uint)max(CleanRtxdiDiEmissiveDistributionInfo.x, 0.0), CleanRtxdiDiCurrentEmissiveTriangleCount);
    if (CleanRtxdiDiEmissiveDistributionInfo.y >= 0.5 && distributionCount > 0u)
    {
        const float selector = RAB_GetNextRandom(rng);
        uint low = 0u;
        uint high = distributionCount;
        [loop]
        while (low < high)
        {
            const uint mid = low + ((high - low) >> 1u);
            const PathTraceEmissiveDistributionEntry entry = SmokeEmissiveDistribution[mid];
            if (selector <= saturate(entry.cumulativePdf))
            {
                high = mid;
            }
            else
            {
                low = mid + 1u;
            }
        }
        const uint entryIndex = min(low, distributionCount - 1u);
        const PathTraceEmissiveDistributionEntry entry = SmokeEmissiveDistribution[entryIndex];
        if (entry.emissiveTriangleIndex < CleanRtxdiDiCurrentEmissiveTriangleCount)
        {
            const float previousCdf = entryIndex > 0u ? saturate(SmokeEmissiveDistribution[entryIndex - 1u].cumulativePdf) : 0.0;
            const float currentCdf = max(saturate(entry.cumulativePdf), previousCdf);
            sourceIndex = entry.emissiveTriangleIndex;
            sourcePdf = max(currentCdf - previousCdf, 1.0e-6);
            return true;
        }
    }

    const uint emissiveCount = CleanRtxdiDiCurrentEmissiveTriangleCount;
    if (emissiveCount == 0u)
    {
        return false;
    }
    sourceIndex = min((uint)(RAB_GetNextRandom(rng) * (float)emissiveCount), emissiveCount - 1u);
    sourcePdf = 1.0 / max((float)emissiveCount, 1.0);
    return true;
}

bool CleanGiNeeCacheProviderReady()
{
    return (CleanRtxdiDiFlags & CLEAN_FLAG_NEE_CACHE_PROVIDER) != 0u &&
        CleanRtxdiDiNeeCacheInfo0.x >= 0.5 &&
        CleanRtxdiDiNeeCacheInfo1.z > 0.0 &&
        CleanRtxdiDiNeeCacheInfo1.w > 0.0 &&
        CleanRtxdiDiRluCurrentLightCount > 0u;
}

float CleanGiNeeCacheTypedClassProbability(bool analytic, uint emissiveRangeCount, uint analyticRangeCount)
{
    const uint sourceDomain = clamp((uint)max(CleanRtxdiDiNeeCacheInfo0.z, 0.0), 0u, 3u);
    if (sourceDomain != 3u || emissiveRangeCount == 0u || analyticRangeCount == 0u)
    {
        return 1.0;
    }
    return 0.5;
}

bool CleanGiBuildNeeCacheFallbackProvider(
    PathTraceNeeCacheCellDebug cell,
    uint frameIndexSalt,
    out PathTraceNeeCacheProviderResult result)
{
    result = (PathTraceNeeCacheProviderResult)0;
    result.selectedDenseRluIndex = 0xffffffffu;
    result.sourceLabel = 0u;
    result.fallbackReason = PATH_TRACE_NEE_CACHE_FALLBACK_EMPTY_CELL;
    result.cellIndex = cell.cellIndex;
    result.candidateSlot = 0xffffffffu;

    const uint currentRluCount = CleanRtxdiDiRluCurrentLightCount;
    if (currentRluCount == 0u)
    {
        return false;
    }

    const uint sourceDomain = clamp((uint)max(CleanRtxdiDiNeeCacheInfo0.z, 0.0), 0u, 3u);
    const uint emissiveRangeOffset = min((uint)max(CleanRtxdiDiRluRangeInfo.x, 0.0), currentRluCount);
    const uint emissiveRangeCount = min((uint)max(CleanRtxdiDiRluRangeInfo.y, 0.0), currentRluCount - emissiveRangeOffset);
    const uint analyticRangeOffset = min((uint)max(CleanRtxdiDiRluRangeInfo.z, 0.0), currentRluCount);
    const uint analyticRangeCountRaw = min((uint)max(CleanRtxdiDiRluRangeInfo.w, 0.0), currentRluCount - analyticRangeOffset);
    const uint analyticSampleCount = (uint)max(CleanRtxdiDiRluSampleInfo.y, 0.0);
    const uint analyticRangeCount = analyticSampleCount > 0u ? min(analyticRangeCountRaw, analyticSampleCount) : analyticRangeCountRaw;

    uint rangeOffset = 0u;
    uint rangeCount = currentRluCount;
    float classProbability = 1.0;
    uint sourceLabel = PATH_TRACE_NEE_CACHE_SOURCE_FALLBACK_FULL_RLU;
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
                CleanGiHashToUnitFloat(cell.hash ^ (frameIndexSalt * 1597334677u)) >= 0.5);
        rangeOffset = useAnalytic ? analyticRangeOffset : emissiveRangeOffset;
        rangeCount = useAnalytic ? analyticRangeCount : emissiveRangeCount;
        classProbability = CleanGiNeeCacheTypedClassProbability(useAnalytic, emissiveRangeCount, analyticRangeCount);
        sourceLabel = PATH_TRACE_NEE_CACHE_SOURCE_FALLBACK_TYPED_RLU;
    }

    if (rangeCount == 0u)
    {
        return false;
    }

    const uint denseIndex = rangeOffset + ((cell.hash ^ (frameIndexSalt * 747796405u)) % rangeCount);
    if (denseIndex >= currentRluCount)
    {
        return false;
    }

    const float sourcePdf = classProbability / max((float)rangeCount, 1.0);
    if (sourcePdf <= 1.0e-8)
    {
        return false;
    }

    result.selectedDenseRluIndex = denseIndex;
    result.sourceLabel = sourceLabel;
    result.fallbackReason = PATH_TRACE_NEE_CACHE_FALLBACK_EMPTY_CELL;
    result.flags = 1u;
    result.sourcePdf = sourcePdf;
    result.invSourcePdf = 1.0 / max(sourcePdf, 1.0e-8);
    result.mixtureProbability = 1.0;
    return true;
}

bool CleanGiNeeCacheCandidateUsable(PathTraceNeeCacheCandidateRecord candidate, PathTraceNeeCacheCellDebug cell)
{
    if (candidate.flags == 0u ||
        candidate.cellIndex != cell.cellIndex ||
        candidate.denseRluIndex >= CleanRtxdiDiRluCurrentLightCount ||
        candidate.sourcePdf <= 1.0e-8 ||
        candidate.invSourcePdf <= 0.0 ||
        candidate.candidateWeight <= 0.0)
    {
        return false;
    }

    const PathTraceUnifiedLightRecord record = CleanRestirGiRluCurrentLights[candidate.denseRluIndex];
    return record.type == candidate.lightClass &&
        (candidate.lightClass == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC ||
            candidate.lightClass == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE);
}

float CleanGiNeeCacheCandidateIdentityWeight(PathTraceNeeCacheCandidateRecord candidate, PathTraceNeeCacheCellDebug cell)
{
    return CleanGiNeeCacheCandidateUsable(candidate, cell)
        ? max(candidate.candidateWeight, 0.0)
        : 0.0;
}

float CleanGiNeeCacheCacheIdentityPdf(PathTraceNeeCacheCellDebug cell, uint denseRluIndex, out bool hasCacheDistribution)
{
    hasCacheDistribution = false;

    const uint candidateSlots = max((uint)max(CleanRtxdiDiNeeCacheInfo0.w, 0.0), 0u);
    if (candidateSlots == 0u)
    {
        return 0.0;
    }

    const PathTraceNeeCacheCellRecord storedCell = CleanRestirGiNeeCacheCells[cell.cellIndex];
    if (storedCell.flags == 0u || storedCell.hash != cell.hash || storedCell.candidateCount == 0u)
    {
        return 0.0;
    }

    const uint baseSlot = cell.cellIndex * candidateSlots;
    float totalWeight = 0.0;
    float selectedIdentityWeight = 0.0;
    [loop]
    for (uint slot = 0u; slot < candidateSlots; ++slot)
    {
        const PathTraceNeeCacheCandidateRecord candidate = CleanRestirGiNeeCacheCandidates[baseSlot + slot];
        const float currentWeight = CleanGiNeeCacheCandidateIdentityWeight(candidate, cell);
        if (currentWeight <= 0.0)
        {
            continue;
        }

        totalWeight += currentWeight;
        if (candidate.denseRluIndex == denseRluIndex)
        {
            selectedIdentityWeight += currentWeight;
        }
    }

    hasCacheDistribution = totalWeight > 0.0;
    return hasCacheDistribution ? selectedIdentityWeight / max(totalWeight, 1.0e-8) : 0.0;
}

float CleanGiNeeCacheFallbackIdentityPdf(uint denseRluIndex)
{
    const uint currentRluCount = CleanRtxdiDiRluCurrentLightCount;
    if (currentRluCount == 0u || denseRluIndex >= currentRluCount)
    {
        return 0.0;
    }

    const uint sourceDomain = clamp((uint)max(CleanRtxdiDiNeeCacheInfo0.z, 0.0), 0u, 3u);
    if (sourceDomain == 0u)
    {
        return 1.0 / max((float)currentRluCount, 1.0);
    }

    const uint emissiveRangeOffset = min((uint)max(CleanRtxdiDiRluRangeInfo.x, 0.0), currentRluCount);
    const uint emissiveRangeCount = min((uint)max(CleanRtxdiDiRluRangeInfo.y, 0.0), currentRluCount - emissiveRangeOffset);
    const uint analyticRangeOffset = min((uint)max(CleanRtxdiDiRluRangeInfo.z, 0.0), currentRluCount);
    const uint analyticRangeCountRaw = min((uint)max(CleanRtxdiDiRluRangeInfo.w, 0.0), currentRluCount - analyticRangeOffset);
    const uint analyticSampleCount = (uint)max(CleanRtxdiDiRluSampleInfo.y, 0.0);
    const uint analyticRangeCount = analyticSampleCount > 0u ? min(analyticRangeCountRaw, analyticSampleCount) : analyticRangeCountRaw;

    const bool inEmissiveRange =
        denseRluIndex >= emissiveRangeOffset &&
        denseRluIndex < emissiveRangeOffset + emissiveRangeCount;
    const bool inAnalyticRange =
        denseRluIndex >= analyticRangeOffset &&
        denseRluIndex < analyticRangeOffset + analyticRangeCount;

    if (sourceDomain == 1u)
    {
        return inEmissiveRange ? 1.0 / max((float)emissiveRangeCount, 1.0) : 0.0;
    }
    if (sourceDomain == 2u)
    {
        return inAnalyticRange ? 1.0 / max((float)analyticRangeCount, 1.0) : 0.0;
    }

    if (inEmissiveRange)
    {
        return CleanGiNeeCacheTypedClassProbability(false, emissiveRangeCount, analyticRangeCount) /
            max((float)emissiveRangeCount, 1.0);
    }
    if (inAnalyticRange)
    {
        return CleanGiNeeCacheTypedClassProbability(true, emissiveRangeCount, analyticRangeCount) /
            max((float)analyticRangeCount, 1.0);
    }

    return 0.0;
}

float CleanGiNeeCacheMixtureIdentityPdf(PathTraceNeeCacheCellDebug cell, uint denseRluIndex)
{
    bool hasCacheDistribution = false;
    const float cacheIdentityPdf = CleanGiNeeCacheCacheIdentityPdf(cell, denseRluIndex, hasCacheDistribution);
    const float fallbackIdentityPdf = CleanGiNeeCacheFallbackIdentityPdf(denseRluIndex);
    const float fallbackProbability = saturate(CleanRtxdiDiNeeCacheInfo0.y);
    const float cacheProbability = 1.0 - fallbackProbability;
    return hasCacheDistribution
        ? cacheProbability * cacheIdentityPdf + fallbackProbability * fallbackIdentityPdf
        : fallbackIdentityPdf;
}

bool CleanGiNeeCacheSelectFallbackProposal(
    inout RTXDI_RandomSamplerState rng,
    out uint selectedDenseRluIndex,
    out float sourcePdf)
{
    selectedDenseRluIndex = 0xffffffffu;
    sourcePdf = 0.0;

    const uint currentRluCount = CleanRtxdiDiRluCurrentLightCount;
    if (currentRluCount == 0u)
    {
        return false;
    }

    const uint sourceDomain = clamp((uint)max(CleanRtxdiDiNeeCacheInfo0.z, 0.0), 0u, 3u);
    const uint emissiveRangeOffset = min((uint)max(CleanRtxdiDiRluRangeInfo.x, 0.0), currentRluCount);
    const uint emissiveRangeCount = min((uint)max(CleanRtxdiDiRluRangeInfo.y, 0.0), currentRluCount - emissiveRangeOffset);
    const uint analyticRangeOffset = min((uint)max(CleanRtxdiDiRluRangeInfo.z, 0.0), currentRluCount);
    const uint analyticRangeCountRaw = min((uint)max(CleanRtxdiDiRluRangeInfo.w, 0.0), currentRluCount - analyticRangeOffset);
    const uint analyticSampleCount = (uint)max(CleanRtxdiDiRluSampleInfo.y, 0.0);
    const uint analyticRangeCount = analyticSampleCount > 0u ? min(analyticRangeCountRaw, analyticSampleCount) : analyticRangeCountRaw;

    uint rangeOffset = 0u;
    uint rangeCount = currentRluCount;
    float classProbability = 1.0;

    if (sourceDomain == 1u)
    {
        rangeOffset = emissiveRangeOffset;
        rangeCount = emissiveRangeCount;
    }
    else if (sourceDomain == 2u)
    {
        rangeOffset = analyticRangeOffset;
        rangeCount = analyticRangeCount;
    }
    else if (sourceDomain == 3u)
    {
        const bool chooseAnalytic =
            analyticRangeCount > 0u &&
            (emissiveRangeCount == 0u || RAB_GetNextRandom(rng) >= 0.5);
        rangeOffset = chooseAnalytic ? analyticRangeOffset : emissiveRangeOffset;
        rangeCount = chooseAnalytic ? analyticRangeCount : emissiveRangeCount;
        classProbability = CleanGiNeeCacheTypedClassProbability(chooseAnalytic, emissiveRangeCount, analyticRangeCount);
    }

    if (rangeCount == 0u)
    {
        return false;
    }

    const uint localIndex = min((uint)(RAB_GetNextRandom(rng) * (float)rangeCount), rangeCount - 1u);
    const uint denseIndex = rangeOffset + localIndex;
    if (denseIndex >= currentRluCount)
    {
        return false;
    }

    selectedDenseRluIndex = denseIndex;
    sourcePdf = classProbability / max((float)rangeCount, 1.0);
    return sourcePdf > 1.0e-8;
}

bool CleanGiSelectNeeCacheCandidateProvider(
    PathTraceNeeCacheCellDebug cell,
    uint2 pixel,
    uint salt,
    out PathTraceNeeCacheProviderResult result)
{
    result = (PathTraceNeeCacheProviderResult)0;
    result.selectedDenseRluIndex = 0xffffffffu;
    result.cellIndex = cell.cellIndex;
    result.candidateSlot = 0xffffffffu;

    const uint candidateSlots = max((uint)max(CleanRtxdiDiNeeCacheInfo0.w, 0.0), 0u);
    if (candidateSlots == 0u)
    {
        return false;
    }

    const PathTraceNeeCacheCellRecord storedCell = CleanRestirGiNeeCacheCells[cell.cellIndex];
    if (storedCell.flags == 0u || storedCell.hash != cell.hash || storedCell.candidateCount == 0u)
    {
        return false;
    }

    const uint baseSlot = cell.cellIndex * candidateSlots;
    float totalWeight = 0.0;
    [loop]
    for (uint slot = 0u; slot < candidateSlots; ++slot)
    {
        const PathTraceNeeCacheCandidateRecord candidate = CleanRestirGiNeeCacheCandidates[baseSlot + slot];
        if (CleanGiNeeCacheCandidateUsable(candidate, cell))
        {
            totalWeight += max(candidate.candidateWeight, 0.0);
        }
    }
    if (totalWeight <= 0.0)
    {
        return false;
    }

    const uint selectorHash =
        cell.hash ^
        pixel.x * 2246822519u ^
        pixel.y * 3266489917u ^
        CleanRestirGiFrameIndex * 668265263u ^
        salt;
    const float threshold = CleanGiHashToUnitFloat(selectorHash) * totalWeight;
    float cumulativeWeight = 0.0;
    PathTraceNeeCacheCandidateRecord selected = (PathTraceNeeCacheCandidateRecord)0;
    selected.denseRluIndex = 0xffffffffu;
    [loop]
    for (uint selectSlot = 0u; selectSlot < candidateSlots; ++selectSlot)
    {
        const PathTraceNeeCacheCandidateRecord candidate = CleanRestirGiNeeCacheCandidates[baseSlot + selectSlot];
        if (!CleanGiNeeCacheCandidateUsable(candidate, cell))
        {
            continue;
        }
        cumulativeWeight += max(candidate.candidateWeight, 0.0);
        if (selected.denseRluIndex == 0xffffffffu && cumulativeWeight >= threshold)
        {
            selected = candidate;
        }
    }
    if (selected.denseRluIndex == 0xffffffffu)
    {
        return false;
    }

    const float sourcePdf = CleanGiNeeCacheMixtureIdentityPdf(cell, selected.denseRluIndex);
    if (sourcePdf <= 1.0e-8)
    {
        return false;
    }

    const float cacheProbability = 1.0 - saturate(CleanRtxdiDiNeeCacheInfo0.y);
    result.selectedDenseRluIndex = selected.denseRluIndex;
    result.sourceLabel = selected.lightClass == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC
        ? PATH_TRACE_NEE_CACHE_SOURCE_CACHE_ANALYTIC
        : PATH_TRACE_NEE_CACHE_SOURCE_CACHE_EMISSIVE;
    result.fallbackReason = 0u;
    result.candidateSlot = selected.candidateSlot;
    result.flags = 1u;
    result.sourcePdf = sourcePdf;
    result.invSourcePdf = 1.0 / max(sourcePdf, 1.0e-8);
    result.mixtureProbability = cacheProbability;
    return true;
}

bool CleanGiSelectNeeCacheSecondaryFastCandidate(
    PathTraceNeeCacheCellDebug cell,
    inout RTXDI_RandomSamplerState rng,
    out uint selectedDenseRluIndex,
    out float sourceSelectionPdf)
{
    selectedDenseRluIndex = 0xffffffffu;
    sourceSelectionPdf = 0.0;

    const uint candidateSlots = max((uint)max(CleanRtxdiDiNeeCacheInfo0.w, 0.0), 0u);
    const PathTraceNeeCacheCellRecord storedCell = CleanRestirGiNeeCacheCells[cell.cellIndex];
    const bool hasCacheCell =
        candidateSlots > 0u &&
        storedCell.flags != 0u &&
        storedCell.hash == cell.hash &&
        storedCell.candidateCount > 0u;

    const float fallbackProbability = saturate(CleanRtxdiDiNeeCacheInfo0.y);
    const bool tryFallback = RAB_GetNextRandom(rng) < fallbackProbability;
    bool selectedCacheCandidate = false;

    if (!tryFallback && hasCacheCell)
    {
        const uint baseSlot = cell.cellIndex * candidateSlots;
        float totalWeight = 0.0;
        [loop]
        for (uint slot = 0u; slot < candidateSlots; ++slot)
        {
            totalWeight += CleanGiNeeCacheCandidateIdentityWeight(
                CleanRestirGiNeeCacheCandidates[baseSlot + slot],
                cell);
        }

        if (totalWeight > 0.0)
        {
            const float threshold = RAB_GetNextRandom(rng) * totalWeight;
            float cumulativeWeight = 0.0;
            [loop]
            for (uint selectSlot = 0u; selectSlot < candidateSlots; ++selectSlot)
            {
                const PathTraceNeeCacheCandidateRecord candidate = CleanRestirGiNeeCacheCandidates[baseSlot + selectSlot];
                const float currentWeight = CleanGiNeeCacheCandidateIdentityWeight(candidate, cell);
                if (currentWeight <= 0.0)
                {
                    continue;
                }

                cumulativeWeight += currentWeight;
                if (!selectedCacheCandidate && cumulativeWeight >= threshold)
                {
                    selectedDenseRluIndex = candidate.denseRluIndex;
                    selectedCacheCandidate = true;
                }
            }
        }
    }

    if (!selectedCacheCandidate &&
        !CleanGiNeeCacheSelectFallbackProposal(rng, selectedDenseRluIndex, sourceSelectionPdf))
    {
        return false;
    }

    sourceSelectionPdf = CleanGiNeeCacheMixtureIdentityPdf(cell, selectedDenseRluIndex);
    if (sourceSelectionPdf <= 1.0e-8)
    {
        return false;
    }

    return selectedDenseRluIndex < CleanRtxdiDiRluCurrentLightCount;
}

bool CleanGiAccumulateNeeCacheProviderSample(
    inout float3 radiance,
    RAB_Surface secondarySurface,
    float3 hitGeometricNormal,
    bool primarySampledSpecular,
    inout RTXDI_RandomSamplerState rng)
{
    const uint secondaryMode = min(CleanRestirGiNeeCacheSecondaryMode, 2u);
    if (secondaryMode == 0u)
    {
        return false;
    }

    if (secondaryMode == 1u)
    {
        const float roughness = saturate(GetRoughness(secondarySurface.material));
        const float specularLuminance = CleanGiLuminance(GetSpecularF0(secondarySurface.material));
        if (!primarySampledSpecular ||
            roughness > CleanRestirGiNeeCacheSecondaryRoughness ||
            specularLuminance <= 0.04)
        {
            return false;
        }
    }

    const float attemptProbability = saturate(CleanRestirGiNeeCacheSecondaryProbability);
    if (attemptProbability <= 0.0 ||
        (attemptProbability < 1.0 && RAB_GetNextRandom(rng) >= attemptProbability))
    {
        return false;
    }

    if (!CleanGiNeeCacheProviderReady())
    {
        return false;
    }

    const PathTraceNeeCacheCellDebug cell = PathTraceNeeCacheMapWorldPositionToCell(
        RAB_GetSurfaceWorldPos(secondarySurface),
        CleanRtxdiDiCameraOriginAndValid.xyz,
        max((uint)max(CleanRtxdiDiNeeCacheInfo1.x, 1.0), 1u),
        max(CleanRtxdiDiNeeCacheInfo1.y, 1.0),
        max((uint)max(CleanRtxdiDiNeeCacheInfo1.z, 1.0), 1u));
    const uint cellCount = max((uint)max(CleanRtxdiDiNeeCacheInfo1.z, 1.0), 1u);
    if (cell.valid == 0u || cell.cellIndex >= cellCount)
    {
        return false;
    }

    uint selectedDenseRluIndex = 0xffffffffu;
    float sourceSelectionPdf = 0.0;
    if (!CleanGiSelectNeeCacheSecondaryFastCandidate(cell, rng, selectedDenseRluIndex, sourceSelectionPdf))
    {
        return false;
    }

    const RAB_LightInfo lightInfo = CleanGiLoadCurrentRluLightInfo(selectedDenseRluIndex);
    return CleanGiAccumulateLightSample(
        radiance,
        secondarySurface,
        hitGeometricNormal,
        lightInfo,
        sourceSelectionPdf,
        rng);
}

// ---------------------------------------------------------------------------
// Visibility ray from an arbitrary surface point (secondary vertex NEE)
// ---------------------------------------------------------------------------

float CleanGiTraceVisibility(float3 fromPosition, float3 geometricNormal, float3 toPosition)
{
    const float3 toSample = toPosition - fromPosition;
    const float distanceSquared = dot(toSample, toSample);
    if (distanceSquared <= 1.0e-6)
    {
        return 0.0;
    }
    const float distance = sqrt(distanceSquared);
    const float3 direction = toSample / distance;
    if (dot(geometricNormal, direction) <= 0.0)
    {
        return 0.0;
    }

    RayDesc shadowRay;
    shadowRay.Origin = fromPosition + geometricNormal * 0.75 + direction * 0.25;
    shadowRay.Direction = direction;
    shadowRay.TMin = 0.01;
    shadowRay.TMax = max(distance - 0.5, 0.01);

    PathTraceCleanRestirGiPayload shadowPayload = (PathTraceCleanRestirGiPayload)0;
    shadowPayload.rayMode = 1u;
    shadowPayload.ignoreInstanceId = 0xffffffffu;
    shadowPayload.ignorePrimitiveIndex = 0xffffffffu;
    shadowPayload.ignoreMaterialIndex = 0xffffffffu;
    const uint rayFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_NON_OPAQUE;
    TraceRay(SmokeScene, rayFlags, 0xff, 1, 1, 1, shadowRay, shadowPayload);
    return shadowPayload.value == 0u ? 1.0 : 0.0;
}

// ---------------------------------------------------------------------------
// Producer (RGI-02)
// ---------------------------------------------------------------------------

struct CleanGiSpecularProducerDebug
{
    CleanGiProducerResult producer;
    float3 bounceDir;
    float diffusePdf;
    float specularPdf;
    float mixturePdf;
    float diffuseProbability;
    float specularProbability;
    uint sampledDirection;
};

bool CleanGiTraceMaterialSurfaceRay(
    float3 origin,
    float3 originGeometricNormal,
    float3 rayDirection,
    uint ignoreInstanceId,
    uint ignorePrimitiveIndex,
    uint ignoreMaterialIndex,
    bool forceOpaqueTrace,
    out RAB_Surface hitSurface,
    out float3 hitGeometricNormal,
    out float3 hitEmissive,
    out float hitT)
{
    hitSurface = RAB_EmptySurface();
    hitGeometricNormal = -rayDirection;
    hitEmissive = float3(0.0, 0.0, 0.0);
    hitT = 0.0;

    RayDesc ray;
    ray.Origin = origin + originGeometricNormal * 0.5 + rayDirection * 0.25;
    ray.Direction = rayDirection;
    ray.TMin = 0.01;
    ray.TMax = 100000.0;

    PathTraceCleanRestirGiPayload payload = (PathTraceCleanRestirGiPayload)0;
    payload.rayMode = 1u;
    payload.ignoreInstanceId = ignoreInstanceId;
    payload.ignorePrimitiveIndex = ignorePrimitiveIndex;
    payload.ignoreMaterialIndex = ignoreMaterialIndex;
    const uint traceFlags = forceOpaqueTrace ? RAY_FLAG_FORCE_OPAQUE : RAY_FLAG_FORCE_NON_OPAQUE;
    TraceRay(SmokeScene, traceFlags, 0xff, 0, 1, 0, ray, payload);
    if (payload.value == 0u)
    {
        return false;
    }

    const float3 hitPosition = ray.Origin + rayDirection * payload.hitT;

    // Load hit material / class+flags once and reuse for the normal-map decode
    // and shading (previously loaded twice per hit).
    const uint hitMaterialIndex = CleanGiLoadTriangleMaterialIndex(payload.hitInstanceId, payload.hitPrimitiveIndex);
    const uint hitTriangleClassAndFlags = CleanGiLoadTriangleClassAndFlags(payload.hitInstanceId, payload.hitPrimitiveIndex);
    const PathTraceSmokeMaterial hitMaterial = CleanGiLoadSmokeMaterial(hitMaterialIndex);

    float3 p0, p1, p2;
    float3 n0, n1, n2;
    float2 uv0, uv1, uv2;
    float4 c0, c1, c2;
    float4 c20, c21, c22;
    float3 localGeometricNormal = -rayDirection;
    float3 hitShadingNormal = -rayDirection;
    float2 hitTexCoord = float2(0.0, 0.0);
    float4 hitVertexColor = float4(1.0, 1.0, 1.0, 1.0);
    if (CleanGiLoadTriangleGeometryFull(
        payload.hitInstanceId,
        payload.hitPrimitiveIndex,
        p0, p1, p2,
        n0, n1, n2,
        uv0, uv1, uv2,
        c0, c1, c2,
        c20, c21, c22))
    {
        const float3 crossValue = cross(p1 - p0, p2 - p0);
        localGeometricNormal = CleanGiSafeNormalize(crossValue, -rayDirection);
        if (dot(localGeometricNormal, rayDirection) > 0.0)
        {
            localGeometricNormal = -localGeometricNormal;
        }
        const float b1 = saturate(payload.hitBarycentrics.x);
        const float b2 = saturate(payload.hitBarycentrics.y);
        const float b0 = saturate(1.0 - b1 - b2);
        hitTexCoord = uv0 * b0 + uv1 * b1 + uv2 * b2;
        hitVertexColor = saturate(c0 * b0 + c1 * b1 + c2 * b2);

        const bool forceGeometricNormal = (hitTriangleClassAndFlags & RT_SMOKE_TRIANGLE_FORCE_GEOMETRIC_NORMAL) != 0u;
        float3 interpolatedNormal = CleanGiSafeNormalize(n0 * b0 + n1 * b1 + n2 * b2, localGeometricNormal);
        if (dot(interpolatedNormal, localGeometricNormal) < 0.0)
        {
            interpolatedNormal = -interpolatedNormal;
        }
        hitShadingNormal = forceGeometricNormal ? localGeometricNormal : interpolatedNormal;

        const float3 tangentFallback = CleanGiBuildPerpendicular(hitShadingNormal);
        const float3 bitangentFallback = CleanGiSafeNormalize(cross(hitShadingNormal, tangentFallback), float3(0.0, 1.0, 0.0));
        float3 hitTangent = tangentFallback;
        float3 hitBitangent = bitangentFallback;
        const float3 dp1 = p1 - p0;
        const float3 dp2 = p2 - p0;
        const float2 duv1 = uv1 - uv0;
        const float2 duv2 = uv2 - uv0;
        const float uvDeterminant = duv1.x * duv2.y - duv1.y * duv2.x;
        if (abs(uvDeterminant) > 1.0e-8)
        {
            const float inverseDeterminant = 1.0 / uvDeterminant;
            const float3 rawTangent = (dp1 * duv2.y - dp2 * duv1.y) * inverseDeterminant;
            const float3 rawBitangent = (dp2 * duv1.x - dp1 * duv2.x) * inverseDeterminant;
            hitTangent = CleanGiSafeNormalize(rawTangent - hitShadingNormal * dot(hitShadingNormal, rawTangent), tangentFallback);
            hitBitangent = CleanGiSafeNormalize(rawBitangent - hitShadingNormal * dot(hitShadingNormal, rawBitangent) - hitTangent * dot(hitTangent, rawBitangent), bitangentFallback);
            if (dot(cross(hitTangent, hitBitangent), hitShadingNormal) < 0.0)
            {
                hitBitangent = -hitBitangent;
            }
        }

        hitShadingNormal = CleanGiConstrainShadingNormal(
            CleanGiDecodeNormalTexture(hitMaterial, hitTexCoord, hitShadingNormal, hitTangent, hitBitangent),
            localGeometricNormal);
    }

    const uint hitMaterialId = CleanGiLoadTriangleMaterialId(payload.hitInstanceId, payload.hitPrimitiveIndex);
    const uint hitSurfaceClass = CleanGiTriangleSurfaceClass(hitTriangleClassAndFlags);
    const uint hitTranslucentSubtype = CleanGiTriangleTranslucentSubtype(hitTriangleClassAndFlags);
    const RAB_Material hitRabMaterial = CleanGiBuildMaterialFromHit(
        hitMaterialId,
        hitMaterialIndex,
        hitMaterial,
        hitTexCoord,
        hitSurfaceClass,
        hitTranslucentSubtype,
        hitTriangleClassAndFlags,
        hitVertexColor);

    hitSurface = RAB_EmptySurface();
    hitSurface.valid = 1u;
    hitSurface.worldPos = hitPosition;
    hitSurface.linearDepth = payload.hitT;
    hitSurface.geometryNormal = localGeometricNormal;
    hitSurface.shadingNormal = hitShadingNormal;
    hitSurface.viewDir = -rayDirection;
    hitSurface.materialId = hitMaterialId;
    hitSurface.materialIndex = hitMaterialIndex;
    hitSurface.instanceId = payload.hitInstanceId;
    hitSurface.primitiveIndex = payload.hitPrimitiveIndex;
    hitSurface.surfaceClass = hitSurfaceClass;
    hitSurface.flags = hitTriangleClassAndFlags;
    hitSurface.material = hitRabMaterial;

    hitGeometricNormal = localGeometricNormal;
    hitEmissive = hitRabMaterial.emissiveRadiance;
    hitT = payload.hitT;
    return true;
}

// Shades the secondary vertex: its own emissive plus one direct-light proposal.
// The NEE cache provider is the preferred proposal source when ready; otherwise
// the producer falls back to a mixed analytic/emissive proposal. Keep this as a
// single proposal so merely enabling the analytic domain does not add another
// shadow ray at every secondary vertex.
float3 CleanGiShadeDirectVertex(
    RAB_Surface secondarySurface,
    float3 hitGeometricNormal,
    float3 secondaryEmissive,
    bool primarySampledSpecular,
    bool allowNeeCache,
    float directSampleProbability,
    uint directSampleCount,
    inout RTXDI_RandomSamplerState rng)
{
    float3 radiance = secondaryEmissive;
    const float directProbability = saturate(directSampleProbability);
    if (directProbability <= 0.0)
    {
        return radiance;
    }

    if (directProbability < 1.0 && RAB_GetNextRandom(rng) >= directProbability)
    {
        return radiance;
    }
    const float directWeight = 1.0 / max(directProbability, 1.0e-6);
    const uint sampleCount = clamp(directSampleCount, 1u, 32u);
    const float sampleWeight = directWeight / max((float)sampleCount, 1.0);

    const uint analyticCount = CleanRtxdiDiAnalyticLightCount;
    const uint emissiveCount = CleanRtxdiDiCurrentEmissiveTriangleCount;
    const bool hasAnalyticDomain = analyticCount > 0u;
    const bool hasEmissiveDomain = emissiveCount > 0u;
    const float analyticDomainProbability = hasAnalyticDomain && hasEmissiveDomain
        ? 0.5
        : (hasAnalyticDomain ? 1.0 : 0.0);

    [loop]
    for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
    {
        float3 directRadiance = float3(0.0, 0.0, 0.0);
        if (allowNeeCache &&
            CleanRestirGiNeeCacheSecondaryEnabled != 0u &&
            CleanGiAccumulateNeeCacheProviderSample(directRadiance, secondarySurface, hitGeometricNormal, primarySampledSpecular, rng))
        {
            radiance += directRadiance * sampleWeight;
            continue;
        }

        if (CleanGiAccumulateRluRisLightSample(directRadiance, secondarySurface, hitGeometricNormal, rng))
        {
            radiance += directRadiance * sampleWeight;
            continue;
        }

        if (!hasAnalyticDomain && !hasEmissiveDomain)
        {
            continue;
        }

        const bool chooseAnalytic = hasAnalyticDomain &&
            (!hasEmissiveDomain || RAB_GetNextRandom(rng) < analyticDomainProbability);

        if (chooseAnalytic)
        {
            const uint lightIndex = min((uint)(RAB_GetNextRandom(rng) * analyticCount), analyticCount - 1u);
            const RAB_LightInfo lightInfo = CleanGiBuildAnalyticLightInfo(DoomAnalyticLights[lightIndex], lightIndex);
            CleanGiAccumulateLightSample(
                directRadiance,
                secondarySurface,
                hitGeometricNormal,
                lightInfo,
                analyticDomainProbability / max((float)analyticCount, 1.0),
                rng);
            radiance += directRadiance * sampleWeight;
            continue;
        }

        uint emissiveSourceIndex;
        float emissiveSourcePdf;
        if (CleanGiSelectEmissiveDistributionSample(rng, emissiveSourceIndex, emissiveSourcePdf))
        {
            const RAB_LightInfo emissiveInfo = CleanGiBuildEmissiveLightInfo(emissiveSourceIndex, emissiveSourceIndex);
            CleanGiAccumulateLightSample(
                directRadiance,
                secondarySurface,
                hitGeometricNormal,
                emissiveInfo,
                (1.0 - analyticDomainProbability) * emissiveSourcePdf,
                rng);
            radiance += directRadiance * sampleWeight;
        }
    }

    return radiance;
}

float3 CleanGiShadeDirectVertexDefaultOneSample(
    RAB_Surface secondarySurface,
    float3 hitGeometricNormal,
    float3 secondaryEmissive,
    inout RTXDI_RandomSamplerState rng)
{
    float3 radiance = secondaryEmissive;
    float3 directRadiance = float3(0.0, 0.0, 0.0);

    if (CleanGiAccumulateRluRisLightSample(directRadiance, secondarySurface, hitGeometricNormal, rng))
    {
        return radiance + directRadiance;
    }

    const uint analyticCount = CleanRtxdiDiAnalyticLightCount;
    const uint emissiveCount = CleanRtxdiDiCurrentEmissiveTriangleCount;
    const bool hasAnalyticDomain = analyticCount > 0u;
    const bool hasEmissiveDomain = emissiveCount > 0u;
    if (!hasAnalyticDomain && !hasEmissiveDomain)
    {
        return radiance;
    }

    const float analyticDomainProbability = hasAnalyticDomain && hasEmissiveDomain
        ? 0.5
        : (hasAnalyticDomain ? 1.0 : 0.0);
    const bool chooseAnalytic = hasAnalyticDomain &&
        (!hasEmissiveDomain || RAB_GetNextRandom(rng) < analyticDomainProbability);

    if (chooseAnalytic)
    {
        const uint lightIndex = min((uint)(RAB_GetNextRandom(rng) * analyticCount), analyticCount - 1u);
        const RAB_LightInfo lightInfo = CleanGiBuildAnalyticLightInfo(DoomAnalyticLights[lightIndex], lightIndex);
        CleanGiAccumulateLightSample(
            directRadiance,
            secondarySurface,
            hitGeometricNormal,
            lightInfo,
            analyticDomainProbability / max((float)analyticCount, 1.0),
            rng);
        return radiance + directRadiance;
    }

    uint emissiveSourceIndex;
    float emissiveSourcePdf;
    if (CleanGiSelectEmissiveDistributionSample(rng, emissiveSourceIndex, emissiveSourcePdf))
    {
        const RAB_LightInfo emissiveInfo = CleanGiBuildEmissiveLightInfo(emissiveSourceIndex, emissiveSourceIndex);
        CleanGiAccumulateLightSample(
            directRadiance,
            secondarySurface,
            hitGeometricNormal,
            emissiveInfo,
            (1.0 - analyticDomainProbability) * emissiveSourcePdf,
            rng);
        radiance += directRadiance;
    }

    return radiance;
}

uint CleanGiDiagnoseDirectLightSampleGate(
    RAB_Surface secondarySurface,
    float3 hitGeometricNormal,
    RAB_LightInfo lightInfo,
    float sourcePdf,
    inout RTXDI_RandomSamplerState rng,
    out float3 acceptedRadiance)
{
    acceptedRadiance = float3(0.0, 0.0, 0.0);
    if (!RAB_IsLightInfoValid(lightInfo) || sourcePdf <= 1.0e-8)
    {
        return 1u;
    }

    const float2 uv = float2(RAB_GetNextRandom(rng), RAB_GetNextRandom(rng));
    const RAB_LightSample lightSample = RAB_SamplePolymorphicLight(lightInfo, secondarySurface, uv);
    if (!RAB_IsReplayableLightSample(lightSample) || lightSample.solidAnglePdf <= 1.0e-8)
    {
        return 2u;
    }
    if (CleanGiLuminance(lightSample.radiance) <= 0.0)
    {
        return 3u;
    }

    float3 lightDir;
    float lightDistance;
    RAB_GetLightDirDistance(secondarySurface, lightSample, lightDir, lightDistance);
    const float ndotl = saturate(dot(RAB_GetSurfaceNormal(secondarySurface), lightDir));
    if (ndotl <= 0.0)
    {
        return 4u;
    }

    const float3 shadingNormal = RAB_SafeNormalize(
        RAB_GetSurfaceNormal(secondarySurface),
        RAB_GetSurfaceGeoNormal(secondarySurface));
    const float3 geometricNormal = RAB_SafeNormalize(
        RAB_GetSurfaceGeoNormal(secondarySurface),
        shadingNormal);
    const float3 viewDir = RAB_SafeNormalize(
        RAB_GetSurfaceViewDir(secondarySurface),
        geometricNormal);
    if (dot(geometricNormal, lightDir) <= 0.0)
    {
        return 9u;
    }
    if (!RAB_IsSurfaceValid(secondarySurface))
    {
        return 10u;
    }
    if (secondarySurface.surfaceClass == RT_SMOKE_SURFACE_CLASS_TRANSLUCENT)
    {
        return 11u;
    }
    if (secondarySurface.material.opacity <= 0.0)
    {
        return 12u;
    }
    if (dot(shadingNormal, viewDir) <= 0.0)
    {
        return 13u;
    }
    if (dot(geometricNormal, viewDir) <= 0.0)
    {
        return 14u;
    }
    if (CleanGiLuminance(GetDiffuseAlbedo(secondarySurface.material)) <= 0.0)
    {
        return 15u;
    }

    const float3 brdf = RAB_EvaluateSurfaceBrdf(secondarySurface, lightDir, RAB_GetSurfaceViewDir(secondarySurface));
    if (CleanGiLuminance(brdf) <= 0.0)
    {
        return 5u;
    }

    const float visibility = CleanGiTraceVisibility(
        RAB_GetSurfaceWorldPos(secondarySurface), hitGeometricNormal, lightSample.position);
    if (visibility <= 0.0)
    {
        return 6u;
    }

    const float misWeight = CleanGiSecondaryNeeMisWeight(secondarySurface, lightSample, lightDir);
    acceptedRadiance = brdf * lightSample.radiance * ndotl * visibility * misWeight /
        max(sourcePdf * lightSample.solidAnglePdf, 1.0e-6);
    return 0u;
}

float3 CleanGiProducerShadeGateColor(uint gate, float3 acceptedRadiance)
{
    if (gate == 0u)
    {
        const float3 safeRadiance = max(acceptedRadiance, float3(0.0, 0.0, 0.0));
        return saturate(safeRadiance / (safeRadiance + float3(1.0, 1.0, 1.0)));
    }
    if (gate == 1u)
    {
        return float3(0.0, 0.0, 0.85);
    }
    if (gate == 2u)
    {
        return float3(1.0, 0.45, 0.0);
    }
    if (gate == 3u)
    {
        return float3(1.0, 1.0, 0.0);
    }
    if (gate == 4u)
    {
        return float3(1.0, 0.0, 1.0);
    }
    if (gate == 5u)
    {
        return float3(0.0, 0.85, 1.0);
    }
    if (gate == 6u)
    {
        return float3(1.0, 0.0, 0.0);
    }
    if (gate == 7u)
    {
        return float3(0.25, 0.25, 0.25);
    }
    if (gate == 8u)
    {
        return float3(0.15, 0.15, 0.75);
    }
    if (gate == 9u)
    {
        return float3(0.0, 1.0, 0.45);
    }
    if (gate == 10u)
    {
        return float3(0.35, 0.0, 0.0);
    }
    if (gate == 11u)
    {
        return float3(1.0, 1.0, 1.0);
    }
    if (gate == 12u)
    {
        return float3(0.0, 0.0, 0.0);
    }
    if (gate == 13u)
    {
        return float3(0.0, 0.35, 1.0);
    }
    if (gate == 14u)
    {
        return float3(0.45, 0.0, 1.0);
    }
    if (gate == 15u)
    {
        return float3(0.7, 1.0, 0.0);
    }
    return float3(0.75, 0.0, 0.75);
}

float3 CleanGiProducerShadeGateDebugColor(
    RAB_Surface secondarySurface,
    bool primarySampledSpecular,
    inout RTXDI_RandomSamplerState rng)
{
    const float directProbability = saturate(CleanRestirGiSecondaryDirectProbability);
    if (directProbability <= 0.0)
    {
        return CleanGiLuminance(secondarySurface.material.emissiveRadiance) > 0.0
            ? float3(0.0, 0.85, 0.85)
            : CleanGiProducerShadeGateColor(8u, float3(0.0, 0.0, 0.0));
    }
    if (directProbability < 1.0 && RAB_GetNextRandom(rng) >= directProbability)
    {
        return CleanGiProducerShadeGateColor(7u, float3(0.0, 0.0, 0.0));
    }

    if (CleanRestirGiNeeCacheSecondaryEnabled != 0u)
    {
        float3 neeCacheRadiance = float3(0.0, 0.0, 0.0);
        if (CleanGiAccumulateNeeCacheProviderSample(
            neeCacheRadiance,
            secondarySurface,
            secondarySurface.geometryNormal,
            primarySampledSpecular,
            rng))
        {
            const float3 safeRadiance = max(neeCacheRadiance, float3(0.0, 0.0, 0.0));
            return saturate(safeRadiance / (safeRadiance + float3(1.0, 1.0, 1.0)));
        }
    }

    float3 acceptedRadiance = float3(0.0, 0.0, 0.0);
    const uint rluLightCount = CleanRtxdiDiRluCurrentLightCount;
    if (rluLightCount > 0u)
    {
        const uint denseIndex = min((uint)(RAB_GetNextRandom(rng) * (float)rluLightCount), rluLightCount - 1u);
        const RAB_LightInfo lightInfo = CleanGiLoadCurrentRluLightInfo(denseIndex);
        const uint gate = CleanGiDiagnoseDirectLightSampleGate(
            secondarySurface,
            secondarySurface.geometryNormal,
            lightInfo,
            1.0 / max((float)rluLightCount, 1.0),
            rng,
            acceptedRadiance);
        return CleanGiProducerShadeGateColor(gate, acceptedRadiance);
    }

    const uint analyticCount = CleanRtxdiDiAnalyticLightCount;
    const uint emissiveCount = CleanRtxdiDiCurrentEmissiveTriangleCount;
    const bool hasAnalyticDomain = analyticCount > 0u;
    const bool hasEmissiveDomain = emissiveCount > 0u;
    if (!hasAnalyticDomain && !hasEmissiveDomain)
    {
        return CleanGiProducerShadeGateColor(8u, acceptedRadiance);
    }

    const float analyticDomainProbability = hasAnalyticDomain && hasEmissiveDomain
        ? 0.5
        : (hasAnalyticDomain ? 1.0 : 0.0);
    const bool chooseAnalytic = hasAnalyticDomain &&
        (!hasEmissiveDomain || RAB_GetNextRandom(rng) < analyticDomainProbability);
    if (chooseAnalytic)
    {
        const uint lightIndex = min((uint)(RAB_GetNextRandom(rng) * analyticCount), analyticCount - 1u);
        const RAB_LightInfo lightInfo = CleanGiBuildAnalyticLightInfo(DoomAnalyticLights[lightIndex], lightIndex);
        const uint gate = CleanGiDiagnoseDirectLightSampleGate(
            secondarySurface,
            secondarySurface.geometryNormal,
            lightInfo,
            analyticDomainProbability / max((float)analyticCount, 1.0),
            rng,
            acceptedRadiance);
        return CleanGiProducerShadeGateColor(gate, acceptedRadiance);
    }

    uint emissiveSourceIndex;
    float emissiveSourcePdf;
    if (!CleanGiSelectEmissiveDistributionSample(rng, emissiveSourceIndex, emissiveSourcePdf))
    {
        return CleanGiProducerShadeGateColor(1u, acceptedRadiance);
    }

    const RAB_LightInfo emissiveInfo = CleanGiBuildEmissiveLightInfo(emissiveSourceIndex, emissiveSourceIndex);
    const uint gate = CleanGiDiagnoseDirectLightSampleGate(
        secondarySurface,
        secondarySurface.geometryNormal,
        emissiveInfo,
        (1.0 - analyticDomainProbability) * emissiveSourcePdf,
        rng,
        acceptedRadiance);
    return CleanGiProducerShadeGateColor(gate, acceptedRadiance);
}

bool CleanGiSampleContinuationDirection(
    RAB_Surface surface,
    bool primarySampledSpecular,
    inout RTXDI_RandomSamplerState rng,
    out float3 continuationDir,
    out float continuationPdf,
    out bool sampledSpecular)
{
    continuationDir = float3(0.0, 0.0, 0.0);
    continuationPdf = 0.0;
    sampledSpecular = false;

    if (!RAB_SurfaceSupportsOpaqueDiffuseBrdf(surface))
    {
        return false;
    }

    float diffuseProbability;
    float specularProbability;
    CleanGiProducerMixtureProbabilities(surface, diffuseProbability, specularProbability);
    const bool trySpecular =
        specularProbability > 0.0 &&
        (primarySampledSpecular || CleanGiSpecularProducerActive()) &&
        RAB_GetNextRandom(rng) < specularProbability;
    if (trySpecular)
    {
        float specularPdf;
        if (CleanGiSampleSpecularProducerDirection(surface, rng, continuationDir, specularPdf))
        {
            sampledSpecular = true;
            continuationPdf = CleanGiProducerMixturePdf(surface, continuationDir);
            return continuationPdf > 1.0e-8;
        }
    }

    const float3 normal = CleanGiSafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    const float3 geometryNormal = CleanGiSafeNormalize(RAB_GetSurfaceGeoNormal(surface), normal);
    continuationDir = RAB_CosineHemisphereDirection(normal, float2(RAB_GetNextRandom(rng), RAB_GetNextRandom(rng)));
    if (dot(geometryNormal, continuationDir) <= 0.0)
    {
        return false;
    }

    continuationPdf = CleanGiProducerMixturePdf(surface, continuationDir);
    return continuationPdf > 1.0e-8;
}

float CleanGiContinuationContinueProbability(RAB_Surface surface, bool sampledSpecular)
{
    if (CleanRestirGiContinuationRouletteEnabled == 0u)
    {
        return 1.0;
    }

    const float diffuseLuminance = CleanGiLuminance(GetDiffuseAlbedo(surface.material));
    const float specularLuminance = CleanGiLuminance(GetSpecularF0(surface.material));
    const float roughness = saturate(GetRoughness(surface.material));
    const float specularImportance = saturate(specularLuminance * 8.0) * saturate(1.0 - roughness);
    const float importance = sampledSpecular
        ? max(specularImportance, diffuseLuminance * 0.25)
        : diffuseLuminance;
    return clamp(
        importance,
        CleanRestirGiContinuationRouletteMin,
        CleanRestirGiContinuationRouletteMax);
}

float3 CleanGiTraceOneContinuationBounce(
    RAB_Surface secondarySurface,
    float3 secondaryGeometricNormal,
    bool primarySampledSpecular,
    inout RTXDI_RandomSamplerState rng)
{
    if (CleanRestirGiMaxBounces < 2u)
    {
        return float3(0.0, 0.0, 0.0);
    }

    float3 continuationDir;
    float continuationPdf;
    bool sampledSpecular;
    if (!CleanGiSampleContinuationDirection(
        secondarySurface,
        primarySampledSpecular,
        rng,
        continuationDir,
        continuationPdf,
        sampledSpecular))
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float continueProbability = CleanGiContinuationContinueProbability(secondarySurface, sampledSpecular);
    if (CleanRestirGiContinuationRouletteEnabled != 0u &&
        RAB_GetNextRandom(rng) >= continueProbability)
    {
        return float3(0.0, 0.0, 0.0);
    }

    RAB_Surface tertiarySurface;
    float3 tertiaryGeometricNormal;
    float3 tertiaryEmissive;
    float tertiaryHitT;
    if (!CleanGiTraceMaterialSurfaceRay(
        RAB_GetSurfaceWorldPos(secondarySurface),
        secondaryGeometricNormal,
        continuationDir,
        secondarySurface.instanceId,
        secondarySurface.primitiveIndex,
        secondarySurface.materialIndex,
        CleanRestirGiContinuationOpaqueTrace != 0u,
        tertiarySurface,
        tertiaryGeometricNormal,
        tertiaryEmissive,
        tertiaryHitT))
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float3 tertiaryOutgoing = CleanGiShadeDirectVertex(
        tertiarySurface,
        tertiaryGeometricNormal,
        tertiaryEmissive,
        sampledSpecular,
        false,
        CleanRestirGiContinuationDirectProbability,
        1u,
        rng);
    if (CleanGiLuminance(tertiaryOutgoing) <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float3 reflected = CleanGiEvaluateIndirectLobes(secondarySurface, continuationDir, tertiaryOutgoing) /
        max(continuationPdf * continueProbability, 1.0e-6);
    return CleanGiAllFinite3(reflected) ? max(reflected, float3(0.0, 0.0, 0.0)) : float3(0.0, 0.0, 0.0);
}

float3 CleanGiShadeSecondaryVertex(
    RAB_Surface secondarySurface,
    float3 hitGeometricNormal,
    float3 secondaryEmissive,
    bool primarySampledSpecular,
    inout RTXDI_RandomSamplerState rng)
{
    float3 radiance = CleanGiShadeDirectVertex(
        secondarySurface,
        hitGeometricNormal,
        secondaryEmissive,
        primarySampledSpecular,
        true,
        CleanRestirGiSecondaryDirectProbability,
        CleanRestirGiSecondaryDirectSamples,
        rng);

    radiance += CleanGiTraceOneContinuationBounce(
        secondarySurface,
        hitGeometricNormal,
        primarySampledSpecular,
        rng);

    return CleanGiAllFinite3(radiance) ? max(radiance, float3(0.0, 0.0, 0.0)) : float3(0.0, 0.0, 0.0);
}

// --- Producer trace/shade split -------------------------------------------
// CleanGiBuildProducerSurface does the bounce trace + geometry/material
// reconstruction and returns the secondary RAB_Surface WITHOUT any direct
// lighting. The expensive, divergent NEE shading lives in
// CleanGiShadeProducerSurface. Keeping them as distinct functions lets the
// trace and shade halves run as separate, narrower raygen entry points
// (FirstIndirectTraceRayGen / FirstIndirectShadeRayGen) that the GPU schedules at higher
// occupancy than the combined megakernel. The surface is handed between the two
// passes through CleanGiProducerSurfaceBuffer via the pack/unpack helpers.

static const uint CLEAN_GI_PRODUCER_TRACE_STATUS_OK = 0u;
static const uint CLEAN_GI_PRODUCER_TRACE_STATUS_PRIMARY_INVALID = 1u;
static const uint CLEAN_GI_PRODUCER_TRACE_STATUS_SAMPLE_REJECTED = 2u;
static const uint CLEAN_GI_PRODUCER_TRACE_STATUS_QUERY_MISS = 3u;
static const uint CLEAN_GI_PRODUCER_TRACE_STATUS_METADATA_MISS = 4u;
static const uint CLEAN_GI_PRODUCER_TRACE_STATUS_SURFACE_BUILD_MISS = 5u;
static const uint CLEAN_GI_PRODUCER_TRACE_STATUS_ANYHIT_REJECTED = 6u;
static const uint CLEAN_GI_PRODUCER_TRACE_STATUS_QUERY_MISS_PRIMARY_PROBE_HIT = 7u;
static const uint CLEAN_GI_PRODUCER_TRACE_STATUS_QUERY_MISS_TARGET_PROBE_HIT = 8u;
static const uint CLEAN_GI_PRODUCER_TRACE_STATUS_QUERY_MISS_PROBE_INPUT_INVALID = 9u;
static const uint CLEAN_GI_PRODUCER_TRACE_STATUS_SAMPLE_PDF_REJECTED = 10u;
static const uint CLEAN_GI_PRODUCER_TRACE_STATUS_SAMPLE_GEOMETRY_REJECTED = 11u;

CleanGiFirstIndirectRaySample CleanGiMakeFirstIndirectRaySample(
    float3 direction,
    float sourcePdf,
    uint lobeFlag,
    float lobeProbability,
    float lobePdf)
{
    CleanGiFirstIndirectRaySample sample = (CleanGiFirstIndirectRaySample)0;
    if (!CleanGiAllFinite3(direction))
    {
        return sample;
    }

    sample.direction = CleanGiSafeNormalize(direction, float3(0.0, 0.0, 1.0));
    sample.sourcePdf = sourcePdf;
    sample.randomsConsumed = PATH_TRACE_FIRST_INDIRECT_CANDIDATE_RANDOMS_TWO_D;
    sample.lobeProbability = lobeProbability;
    sample.lobePdf = lobePdf;
    if (sourcePdf > 1.0e-6)
    {
        sample.flags = PATH_TRACE_FIRST_INDIRECT_CANDIDATE_FLAG_VALID | lobeFlag;
    }
    return sample;
}

CleanGiProducerSurface CleanGiPackProducerSurface(RAB_Surface s, CleanGiFirstIndirectRaySample raySample)
{
    CleanGiProducerSurface g = (CleanGiProducerSurface)0;
    g.worldPos = s.worldPos;
    g.valid = s.valid;
    g.geometryNormal = s.geometryNormal;
    g.linearDepth = s.linearDepth;
    g.shadingNormal = s.shadingNormal;
    g.materialId = s.materialId;
    g.viewDir = s.viewDir;
    g.materialIndex = s.materialIndex;
    g.diffuseAlbedo = s.material.diffuseAlbedo;
    g.roughness = s.material.roughness;
    g.specularF0 = s.material.specularF0;
    g.opacity = s.material.opacity;
    g.emissiveRadiance = s.material.emissiveRadiance;
    g.alphaCutoff = s.material.alphaCutoff;
    g.instanceId = s.instanceId;
    g.primitiveIndex = s.primitiveIndex;
    g.surfaceClass = s.surfaceClass;
    g.surfaceFlags = s.flags;
    g.materialFlags = s.material.flags;
    g.emissiveTextureIndex = s.material.emissiveTextureIndex;
    g.primarySampledSpecular = PathTraceFirstIndirectCandidateRaySampleIsSpecular(raySample) ? 1u : 0u;
    g.sourcePdf = raySample.sourcePdf;
    return g;
}

RAB_Surface CleanGiUnpackProducerSurface(CleanGiProducerSurface g)
{
    RAB_Surface s = RAB_EmptySurface();
    s.valid = g.valid;
    s.worldPos = g.worldPos;
    s.linearDepth = g.linearDepth;
    s.geometryNormal = g.geometryNormal;
    s.shadingNormal = g.shadingNormal;
    s.viewDir = g.viewDir;
    s.materialId = g.materialId;
    s.materialIndex = g.materialIndex;
    s.instanceId = g.instanceId;
    s.primitiveIndex = g.primitiveIndex;
    s.surfaceClass = g.surfaceClass;
    s.flags = g.surfaceFlags;

    RAB_Material m = RAB_EmptyMaterial();
    m.materialId = g.materialId;
    m.materialIndex = g.materialIndex;
    m.flags = g.materialFlags;
    m.alphaCutoff = g.alphaCutoff;
    m.diffuseAlbedo = g.diffuseAlbedo;
    m.roughness = g.roughness;
    m.specularF0 = g.specularF0;
    m.opacity = g.opacity;
    m.emissiveRadiance = g.emissiveRadiance;
    m.emissiveTextureIndex = g.emissiveTextureIndex;
    s.material = m;
    return s;
}

CleanGiSpecularSeedReceiverSurface CleanGiBuildSpecularSeedReceiverSurface(uint2 pixel, PathTracePrimarySurfaceRecord record)
{
    CleanGiSpecularSeedReceiverSurface receiver = (CleanGiSpecularSeedReceiverSurface)0;
    if (record.header.x != RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_VERSION ||
        (record.header.y & RT_PRIMARY_SURFACE_VALID) == 0u)
    {
        return receiver;
    }

    receiver.worldPos = record.worldPositionAndViewDepth.xyz;
    receiver.valid = 1u;
    receiver.geometryNormal = CleanGiSafeNormalize(record.geometricNormalAndRoughness.xyz, float3(0.0, 0.0, 1.0));
    receiver.roughness = saturate(record.geometricNormalAndRoughness.w);
    receiver.shadingNormal = CleanGiSafeNormalize(record.shadingNormalAndOpacity.xyz, receiver.geometryNormal);
    receiver.surfaceClass = record.materialAndSurface.w & 0xffu;
    receiver.viewDir = CleanGiSafeNormalize(record.viewDirectionAndReserved.xyz, -receiver.shadingNormal);
    receiver.opacity = saturate(record.shadingNormalAndOpacity.w);
    receiver.diffuseAlbedo = CleanGiReceiverGuideAlbedo(pixel, saturate(record.albedoAndAlphaCutoff.xyz));
    receiver.specularF0 = max(record.specularF0AndReserved.xyz, float3(0.0, 0.0, 0.0));
    return receiver;
}

float CleanGiSpecularSeedReceiverTargetPdf(
    CleanGiSpecularSeedReceiverSurface receiver,
    float3 samplePosition,
    float3 sampleRadiance)
{
    if (receiver.valid == 0u || receiver.surfaceClass == 3u || receiver.opacity <= 0.0)
    {
        return 0.0;
    }

    const float3 toSample = samplePosition - receiver.worldPos;
    const float distanceSquared = dot(toSample, toSample);
    if (distanceSquared <= 1.0e-6)
    {
        return 0.0;
    }

    const float3 sampleDir = toSample * rsqrt(distanceSquared);
    const float3 normal = CleanGiSafeNormalize(receiver.shadingNormal, receiver.geometryNormal);
    const float3 geometryNormal = CleanGiSafeNormalize(receiver.geometryNormal, normal);
    const float3 viewDir = CleanGiSafeNormalize(receiver.viewDir, -normal);
    if (dot(sampleDir, geometryNormal) <= 0.0 || dot(sampleDir, normal) <= 0.0)
    {
        return 0.0;
    }

    const float ndotl = saturate(dot(normal, sampleDir));
    const float3 safeRadiance = max(sampleRadiance, float3(0.0, 0.0, 0.0));
    float3 reflected = receiver.diffuseAlbedo * safeRadiance * (ndotl / RTXDI_PI);

    const float3 specularF0 = max(receiver.specularF0, float3(0.0, 0.0, 0.0));
    if (max(max(specularF0.r, specularF0.g), specularF0.b) > 0.0 &&
        dot(geometryNormal, viewDir) > 0.0)
    {
        const float ndotv = saturate(dot(normal, viewDir));
        if (ndotv > 0.0)
        {
            const float3 halfVector = CleanGiSafeNormalize(sampleDir + viewDir, normal);
            const float ndoth = saturate(dot(normal, halfVector));
            const float ldotH = saturate(dot(sampleDir, halfVector));
            if (ndoth > 0.0 && ldotH > 0.0)
            {
                if (CleanGiToyFakePBRSpecularEnabled())
                {
                    const float roughness = max(saturate(receiver.roughness), 0.04);
                    const float rr = roughness * roughness;
                    const float rrrr = max(rr * rr, 1.0e-4);
                    const float D = max((ndoth * ndoth) * (rrrr - 1.0) + 1.0, 1.0e-4);
                    const float VFapprox = max((ldotH * ldotH) * (roughness + 0.5), 1.0e-4);
                    const float specularTerm = (rrrr / (4.0 * D * D * VFapprox)) * ndotl;
                    reflected += specularF0 * safeRadiance * specularTerm;
                }
                else
                {
                    reflected += specularF0 * safeRadiance * pow(ndoth, 32.0);
                }
            }
        }
    }

    return CleanGiAllFinite3(reflected) ? clamp(CleanGiLuminance(reflected), 0.0, 1.0e4) : 0.0;
}

bool CleanGiBuildProducerSurfaceFromHit(
    float3 rayOrigin,
    float3 bounceDir,
    float hitT,
    uint hitInstanceId,
    uint hitPrimitiveIndex,
    float2 hitBarycentrics,
    out RAB_Surface secondarySurface)
{
    secondarySurface = RAB_EmptySurface();

    if (hitT <= 0.0)
    {
        return false;
    }

    const float3 hitPosition = rayOrigin + bounceDir * hitT;

    // Load hit material / class+flags once; both the normal-map decode (inside
    // the geometry block below) and the surface build reuse these.
    const uint hitMaterialIndex = CleanGiLoadTriangleMaterialIndex(hitInstanceId, hitPrimitiveIndex);
    const uint hitTriangleClassAndFlags = CleanGiLoadTriangleClassAndFlags(hitInstanceId, hitPrimitiveIndex);
    const PathTraceSmokeMaterial hitMaterial = CleanGiLoadSmokeMaterial(hitMaterialIndex);

    float3 p0, p1, p2;
    float3 n0, n1, n2;
    float2 uv0, uv1, uv2;
    float4 c0, c1, c2;
    float4 c20, c21, c22;
    float3 hitGeometricNormal = -bounceDir;
    float3 hitShadingNormal = -bounceDir;
    float2 hitTexCoord = float2(0.0, 0.0);
    float4 hitVertexColor = float4(1.0, 1.0, 1.0, 1.0);
    if (CleanGiLoadTriangleGeometryFull(
        hitInstanceId,
        hitPrimitiveIndex,
        p0, p1, p2,
        n0, n1, n2,
        uv0, uv1, uv2,
        c0, c1, c2,
        c20, c21, c22))
    {
        const float3 crossValue = cross(p1 - p0, p2 - p0);
        hitGeometricNormal = CleanGiSafeNormalize(crossValue, -bounceDir);
        if (dot(hitGeometricNormal, bounceDir) > 0.0)
        {
            hitGeometricNormal = -hitGeometricNormal;
        }
        const float b1 = saturate(hitBarycentrics.x);
        const float b2 = saturate(hitBarycentrics.y);
        const float b0 = saturate(1.0 - b1 - b2);
        hitTexCoord = uv0 * b0 + uv1 * b1 + uv2 * b2;
        hitVertexColor = saturate(c0 * b0 + c1 * b1 + c2 * b2);

        const bool forceGeometricNormal = (hitTriangleClassAndFlags & RT_SMOKE_TRIANGLE_FORCE_GEOMETRIC_NORMAL) != 0u;
        float3 interpolatedNormal = CleanGiSafeNormalize(n0 * b0 + n1 * b1 + n2 * b2, hitGeometricNormal);
        if (dot(interpolatedNormal, hitGeometricNormal) < 0.0)
        {
            interpolatedNormal = -interpolatedNormal;
        }
        hitShadingNormal = forceGeometricNormal ? hitGeometricNormal : interpolatedNormal;

        const float3 tangentFallback = CleanGiBuildPerpendicular(hitShadingNormal);
        const float3 bitangentFallback = CleanGiSafeNormalize(cross(hitShadingNormal, tangentFallback), float3(0.0, 1.0, 0.0));
        float3 hitTangent = tangentFallback;
        float3 hitBitangent = bitangentFallback;
        const float3 dp1 = p1 - p0;
        const float3 dp2 = p2 - p0;
        const float2 duv1 = uv1 - uv0;
        const float2 duv2 = uv2 - uv0;
        const float uvDeterminant = duv1.x * duv2.y - duv1.y * duv2.x;
        if (abs(uvDeterminant) > 1.0e-8)
        {
            const float inverseDeterminant = 1.0 / uvDeterminant;
            const float3 rawTangent = (dp1 * duv2.y - dp2 * duv1.y) * inverseDeterminant;
            const float3 rawBitangent = (dp2 * duv1.x - dp1 * duv2.x) * inverseDeterminant;
            hitTangent = CleanGiSafeNormalize(rawTangent - hitShadingNormal * dot(hitShadingNormal, rawTangent), tangentFallback);
            hitBitangent = CleanGiSafeNormalize(rawBitangent - hitShadingNormal * dot(hitShadingNormal, rawBitangent) - hitTangent * dot(hitTangent, rawBitangent), bitangentFallback);
            if (dot(cross(hitTangent, hitBitangent), hitShadingNormal) < 0.0)
            {
                hitBitangent = -hitBitangent;
            }
        }

        hitShadingNormal = CleanGiConstrainShadingNormal(
            CleanGiDecodeNormalTexture(hitMaterial, hitTexCoord, hitShadingNormal, hitTangent, hitBitangent),
            hitGeometricNormal);
    }

    const uint hitMaterialId = CleanGiLoadTriangleMaterialId(hitInstanceId, hitPrimitiveIndex);
    const uint hitSurfaceClass = CleanGiTriangleSurfaceClass(hitTriangleClassAndFlags);
    const uint hitTranslucentSubtype = CleanGiTriangleTranslucentSubtype(hitTriangleClassAndFlags);
    const RAB_Material hitRabMaterial = CleanGiBuildMaterialFromHit(
        hitMaterialId,
        hitMaterialIndex,
        hitMaterial,
        hitTexCoord,
        hitSurfaceClass,
        hitTranslucentSubtype,
        hitTriangleClassAndFlags,
        hitVertexColor);

    secondarySurface.valid = 1u;
    secondarySurface.worldPos = hitPosition;
    secondarySurface.linearDepth = hitT;
    secondarySurface.geometryNormal = hitGeometricNormal;
    secondarySurface.shadingNormal = hitShadingNormal;
    secondarySurface.viewDir = -bounceDir;
    secondarySurface.materialId = hitMaterialId;
    secondarySurface.materialIndex = hitMaterialIndex;
    secondarySurface.instanceId = hitInstanceId;
    secondarySurface.primitiveIndex = hitPrimitiveIndex;
    secondarySurface.surfaceClass = hitSurfaceClass;
    secondarySurface.flags = hitTriangleClassAndFlags;
    secondarySurface.material = hitRabMaterial;
    return true;
}

bool CleanGiBuildProducerSurface(
    float3 primaryPosition,
    float3 primaryGeometricNormal,
    float3 bounceDir,
    float samplePdf,
    out RAB_Surface secondarySurface)
{
    secondarySurface = RAB_EmptySurface();

    if (samplePdf <= 1.0e-6 || dot(primaryGeometricNormal, bounceDir) <= 0.0)
    {
        return false;
    }

    RayDesc bounceRay;
    bounceRay.Origin = primaryPosition + primaryGeometricNormal * 0.5 + bounceDir * 0.25;
    bounceRay.Direction = bounceDir;
    bounceRay.TMin = 0.01;
    bounceRay.TMax = 100000.0;

    PathTraceCleanRestirGiPayload payload = (PathTraceCleanRestirGiPayload)0;
    payload.rayMode = 1u;
    payload.ignoreInstanceId = 0xffffffffu;
    payload.ignorePrimitiveIndex = 0xffffffffu;
    payload.ignoreMaterialIndex = 0xffffffffu;
    const uint rayFlags = CleanRestirGiProducerOpaqueTrace != 0u ? RAY_FLAG_FORCE_OPAQUE : RAY_FLAG_FORCE_NON_OPAQUE;
    TraceRay(SmokeScene, rayFlags, 0xff, 0, 1, 0, bounceRay, payload);
    if (payload.value == 0u)
    {
        return false; // miss: zero radiance, invalid hit geometry
    }

    return CleanGiBuildProducerSurfaceFromHit(
        bounceRay.Origin,
        bounceDir,
        payload.hitT,
        payload.hitInstanceId,
        payload.hitPrimitiveIndex,
        payload.hitBarycentrics,
        secondarySurface);
}

#if defined(CLEAN_RESTIR_GI_PRODUCER_RAYQUERY_CS)
bool CleanGiRayQueryProbePrimaryHit(uint2 pixel, uint2 dimensions)
{
    if (dimensions.x == 0u || dimensions.y == 0u || CleanRtxdiDiCameraOriginAndValid.w < 0.5)
    {
        return false;
    }

    const float2 uv = (float2(pixel) + 0.5) / float2(dimensions);
    const float2 ndc = uv * 2.0 - 1.0;

    RayDesc ray;
    ray.Origin = CleanRtxdiDiCameraOriginAndValid.xyz;
    ray.Direction = normalize(
        CleanRtxdiDiCameraForwardAndTanX.xyz +
        CleanRtxdiDiCameraLeftAndTanY.xyz * (-ndc.x * CleanRtxdiDiCameraForwardAndTanX.w) +
        CleanRtxdiDiCameraUpAndTanY.xyz * (-ndc.y * CleanRtxdiDiCameraLeftAndTanY.w));
    ray.TMin = 0.1;
    ray.TMax = 100000.0;

    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(SmokeScene, RAY_FLAG_FORCE_OPAQUE, 0xff, ray);
    while (query.Proceed())
    {
    }
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

bool CleanGiRayQueryProbeKnownSurfaceHit(float3 primaryPosition, out bool inputValid)
{
    inputValid = false;
    if (CleanRtxdiDiCameraOriginAndValid.w < 0.5 ||
        !CleanGiAllFinite3(CleanRtxdiDiCameraOriginAndValid.xyz) ||
        !CleanGiAllFinite3(primaryPosition))
    {
        return false;
    }

    const float3 toPrimary = primaryPosition - CleanRtxdiDiCameraOriginAndValid.xyz;
    const float distanceSquared = dot(toPrimary, toPrimary);
    if (distanceSquared <= 1.0e-4 || distanceSquared != distanceSquared)
    {
        return false;
    }

    inputValid = true;
    const float distanceToPrimary = sqrt(distanceSquared);
    RayDesc ray;
    ray.Origin = CleanRtxdiDiCameraOriginAndValid.xyz;
    ray.Direction = toPrimary / distanceToPrimary;
    ray.TMin = 0.1;
    ray.TMax = distanceToPrimary + 2.0;

    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(SmokeScene, RAY_FLAG_FORCE_OPAQUE, 0xff, ray);
    while (query.Proceed())
    {
    }
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

bool CleanGiBuildProducerSurfaceRayQuery(
    uint2 pixel,
    uint2 dimensions,
    float3 primaryPosition,
    float3 primaryGeometricNormal,
    float3 bounceDir,
    float samplePdf,
    out RAB_Surface secondarySurface,
    out uint traceStatus)
{
    secondarySurface = RAB_EmptySurface();
    traceStatus = CLEAN_GI_PRODUCER_TRACE_STATUS_OK;

    if (dot(primaryGeometricNormal, bounceDir) <= 0.0)
    {
        traceStatus = CLEAN_GI_PRODUCER_TRACE_STATUS_SAMPLE_GEOMETRY_REJECTED;
        return false;
    }
    if (samplePdf <= 1.0e-6)
    {
        traceStatus = CLEAN_GI_PRODUCER_TRACE_STATUS_SAMPLE_PDF_REJECTED;
        return false;
    }

    RayDesc bounceRay;
    bounceRay.Origin = primaryPosition + primaryGeometricNormal * 0.5 + bounceDir * 0.25;
    bounceRay.Direction = bounceDir;
    bounceRay.TMin = 0.01;
    bounceRay.TMax = 100000.0;

    const uint rayFlags = CleanRestirGiProducerOpaqueTrace != 0u ? RAY_FLAG_FORCE_OPAQUE : RAY_FLAG_FORCE_NON_OPAQUE;
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(SmokeScene, rayFlags, 0xff, bounceRay);
    bool sawCandidate = false;
    bool sawRejectedCandidate = false;
    bool sawAcceptedCandidate = false;
    while (query.Proceed())
    {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            sawCandidate = true;
            const uint candidateInstanceId = query.CandidateInstanceID();
            const uint candidatePrimitiveIndex = query.CandidatePrimitiveIndex();
            const float2 candidateBarycentrics = query.CandidateTriangleBarycentrics();
            if (CleanGiMaterialRejectsHit(pixel, candidateInstanceId, candidatePrimitiveIndex, candidateBarycentrics, false))
            {
                sawRejectedCandidate = true;
            }
            else
            {
                sawAcceptedCandidate = true;
                query.CommitNonOpaqueTriangleHit();
            }
        }
    }

    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        bool knownSurfaceProbeInputValid = false;
        const bool knownSurfaceProbeHit = CleanGiRayQueryProbeKnownSurfaceHit(primaryPosition, knownSurfaceProbeInputValid);
        if (sawCandidate && sawRejectedCandidate && !sawAcceptedCandidate)
        {
            traceStatus = CLEAN_GI_PRODUCER_TRACE_STATUS_ANYHIT_REJECTED;
        }
        else if (knownSurfaceProbeHit)
        {
            traceStatus = CLEAN_GI_PRODUCER_TRACE_STATUS_QUERY_MISS_TARGET_PROBE_HIT;
        }
        else if (!knownSurfaceProbeInputValid)
        {
            traceStatus = CLEAN_GI_PRODUCER_TRACE_STATUS_QUERY_MISS_PROBE_INPUT_INVALID;
        }
        else if (CleanGiRayQueryProbePrimaryHit(pixel, dimensions))
        {
            traceStatus = CLEAN_GI_PRODUCER_TRACE_STATUS_QUERY_MISS_PRIMARY_PROBE_HIT;
        }
        else
        {
            traceStatus = CLEAN_GI_PRODUCER_TRACE_STATUS_QUERY_MISS;
        }
        return false;
    }

    const uint hitPrimitiveIndex = query.CommittedPrimitiveIndex();
    const uint hitInstanceId = query.CommittedInstanceID();

    if (!CleanGiHitMetadataInRange(hitInstanceId, hitPrimitiveIndex))
    {
        traceStatus = CLEAN_GI_PRODUCER_TRACE_STATUS_METADATA_MISS;
        return false;
    }

    const bool surfaceBuilt = CleanGiBuildProducerSurfaceFromHit(
        bounceRay.Origin,
        bounceDir,
        query.CommittedRayT(),
        hitInstanceId,
        hitPrimitiveIndex,
        query.CommittedTriangleBarycentrics(),
        secondarySurface);
    traceStatus = surfaceBuilt
        ? CLEAN_GI_PRODUCER_TRACE_STATUS_OK
        : CLEAN_GI_PRODUCER_TRACE_STATUS_SURFACE_BUILD_MISS;
    return surfaceBuilt;
}
#endif

float3 CleanGiShadeProducerSurface(RAB_Surface secondarySurface, bool primarySampledSpecular, inout RTXDI_RandomSamplerState rng)
{
    // Producer radiance contract: incoming radiance at the primary surface.
    const float3 outgoing = CleanGiShadeSecondaryVertex(
        secondarySurface,
        secondarySurface.geometryNormal,
        secondarySurface.material.emissiveRadiance,
        primarySampledSpecular,
        rng);

    float3 producerRadiance = outgoing;

    // Firefly clamp (initial samples only). Matches the Remix shape:
    // luminance clamp at threshold * 30.
    const float fireflyThreshold = CleanRestirGiFireflyThreshold;
    if (fireflyThreshold > 0.0)
    {
        const float clampLuminance = fireflyThreshold * CLEAN_RESTIR_GI_FIREFLY_FACTOR;
        const float luminance = CleanGiLuminance(producerRadiance);
        if (luminance > clampLuminance)
        {
            producerRadiance *= clampLuminance / max(luminance, 1.0e-6);
        }
    }

    return producerRadiance;
}

float3 CleanGiShadeProducerSurfaceDefaultOneSample(RAB_Surface secondarySurface, inout RTXDI_RandomSamplerState rng)
{
    float3 producerRadiance = CleanGiShadeDirectVertexDefaultOneSample(
        secondarySurface,
        secondarySurface.geometryNormal,
        secondarySurface.material.emissiveRadiance,
        rng);

    const float fireflyThreshold = CleanRestirGiFireflyThreshold;
    if (fireflyThreshold > 0.0)
    {
        const float clampLuminance = fireflyThreshold * CLEAN_RESTIR_GI_FIREFLY_FACTOR;
        const float luminance = CleanGiLuminance(producerRadiance);
        if (luminance > clampLuminance)
        {
            producerRadiance *= clampLuminance / max(luminance, 1.0e-6);
        }
    }

    return CleanGiAllFinite3(producerRadiance) ? max(producerRadiance, float3(0.0, 0.0, 0.0)) : float3(0.0, 0.0, 0.0);
}

CleanGiProducerResult CleanGiMakeShadedFirstIndirectCandidate(RAB_Surface secondarySurface, float3 radiance, float sourcePdf)
{
    CleanGiProducerResult candidate = (CleanGiProducerResult)0;
    if (!RAB_IsSurfaceValid(secondarySurface) || !CleanGiAllFinite3(radiance) || sourcePdf <= 1.0e-6)
    {
        return candidate;
    }

    candidate.valid = 1u;
    candidate.radiance = max(radiance, float3(0.0, 0.0, 0.0));
    candidate.pathLength = secondarySurface.linearDepth;
    candidate.hitPosition = secondarySurface.worldPos;
    candidate.hitNormal = secondarySurface.shadingNormal;
    candidate.materialAlbedo = secondarySurface.material.diffuseAlbedo;
    candidate.materialOpacity = secondarySurface.material.opacity;
    candidate.sourcePdf = sourcePdf;
    return candidate;
}

void CleanGiAttachFirstIndirectDebugMaterial(inout CleanGiProducerResult candidate, uint materialIndex)
{
    if (candidate.valid == 0u)
    {
        return;
    }

    const PathTraceSmokeMaterial hitMaterial = CleanGiLoadSmokeMaterial(materialIndex);
    candidate.materialFlags = hitMaterial.flags;
    candidate.diffuseTextureIndex = hitMaterial.diffuseTextureIndex;
}

void CleanGiStoreFirstIndirectTraceCandidateForRawGiSample(
    uint2 pixel,
    CleanGiProducerSurface candidateSurface,
    float3 hitPosition,
    float3 hitNormal,
    float status)
{
    const float sourcePdf = candidateSurface.valid != 0u ? max(candidateSurface.sourcePdf, 0.0) : 0.0;
    CleanRestirGiProducerHitPosition[pixel] = float4(hitPosition, sourcePdf);
    CleanRestirGiProducerHitNormal[pixel] = float4(hitNormal, status);
}

void CleanGiStoreShadedFirstIndirectCandidateForRawGiSample(uint2 pixel, CleanGiProducerResult candidate)
{
    CleanRestirGiProducerRadiance[pixel] = float4(candidate.radiance, candidate.pathLength);
}

void CleanGiSkipFirstIndirectCandidateRaySampleRandoms(
    CleanGiProducerSurface candidateSurface,
    inout RTXDI_RandomSamplerState rng)
{
    const uint randomsConsumed = candidateSurface.valid != 0u
        ? (CleanGiMixedFirstIndirectProducerActive()
            ? PATH_TRACE_FIRST_INDIRECT_CANDIDATE_RANDOMS_TWO_D + 1u
            : PATH_TRACE_FIRST_INDIRECT_CANDIDATE_RANDOMS_TWO_D)
        : 0u;
    if (randomsConsumed > 0u)
    {
        RAB_GetNextRandom(rng);
    }
    if (randomsConsumed > 1u)
    {
        RAB_GetNextRandom(rng);
    }
    if (randomsConsumed > 2u)
    {
        RAB_GetNextRandom(rng);
    }
}

static const uint CLEAN_GI_FIRST_INDIRECT_SHADE_FULL_NEE = 0u;
static const uint CLEAN_GI_FIRST_INDIRECT_SHADE_DEFAULT_ONE_SAMPLE = 1u;

bool CleanGiBuildFirstIndirectTraceCandidate(
    float3 primaryPosition,
    float3 primaryGeometricNormal,
    CleanGiFirstIndirectRaySample raySample,
    out RAB_Surface secondarySurface,
    out CleanGiProducerSurface traceCandidate,
    out float3 hitPosition,
    out float3 hitNormal)
{
    secondarySurface = RAB_EmptySurface();
    traceCandidate = (CleanGiProducerSurface)0;
    hitPosition = float3(0.0, 0.0, 0.0);
    hitNormal = float3(0.0, 0.0, 0.0);

    if (!PathTraceFirstIndirectCandidateRaySampleIsValid(raySample))
    {
        return false;
    }

    if (!CleanGiBuildProducerSurface(
        primaryPosition,
        primaryGeometricNormal,
        raySample.direction,
        raySample.sourcePdf,
        secondarySurface))
    {
        return false;
    }

    traceCandidate = CleanGiPackProducerSurface(secondarySurface, raySample);
    hitPosition = secondarySurface.worldPos;
    hitNormal = secondarySurface.shadingNormal;
    return true;
}

CleanGiProducerResult CleanGiShadeFirstIndirectSurface(
    RAB_Surface secondarySurface,
    bool primarySampledSpecular,
    float sourcePdf,
    uint shadeMode,
    inout RTXDI_RandomSamplerState rng)
{
    float3 radiance = float3(0.0, 0.0, 0.0);
    if (shadeMode == CLEAN_GI_FIRST_INDIRECT_SHADE_DEFAULT_ONE_SAMPLE)
    {
        radiance = CleanGiShadeProducerSurfaceDefaultOneSample(secondarySurface, rng);
    }
    else
    {
        radiance = CleanGiShadeProducerSurface(secondarySurface, primarySampledSpecular, rng);
    }
    return CleanGiMakeShadedFirstIndirectCandidate(secondarySurface, radiance, sourcePdf);
}

CleanGiProducerResult CleanGiShadeFirstIndirectTraceCandidate(
    CleanGiProducerSurface traceCandidate,
    uint shadeMode,
    bool replayRaySampleRandoms,
    inout RTXDI_RandomSamplerState rng)
{
    if (traceCandidate.valid == 0u)
    {
        return (CleanGiProducerResult)0;
    }

    if (replayRaySampleRandoms)
    {
        CleanGiSkipFirstIndirectCandidateRaySampleRandoms(traceCandidate, rng);
    }

    return CleanGiShadeFirstIndirectSurface(
        CleanGiUnpackProducerSurface(traceCandidate),
        traceCandidate.primarySampledSpecular != 0u,
        traceCandidate.sourcePdf,
        shadeMode,
        rng);
}

uint CleanGiProducerCurrentShadeMode()
{
    return
        CleanRestirGiView == 0u &&
        CleanRestirGiNeeCacheSecondaryEnabled == 0u &&
        CleanRestirGiMaxBounces <= 1u &&
        CleanRestirGiSecondaryDirectSamples == 1u &&
        CleanRestirGiSecondaryDirectProbability >= 1.0
            ? CLEAN_GI_FIRST_INDIRECT_SHADE_DEFAULT_ONE_SAMPLE
            : CLEAN_GI_FIRST_INDIRECT_SHADE_FULL_NEE;
}

CleanGiProducerResult CleanGiTraceProducerRayWithShadeMode(
    float3 primaryPosition,
    float3 primaryGeometricNormal,
    CleanGiFirstIndirectRaySample raySample,
    uint shadeMode,
    inout RTXDI_RandomSamplerState rng)
{
    CleanGiProducerResult result = (CleanGiProducerResult)0;
    RAB_Surface secondarySurface;
    CleanGiProducerSurface traceCandidate;
    float3 hitPosition;
    float3 hitNormal;
    if (!CleanGiBuildFirstIndirectTraceCandidate(
        primaryPosition,
        primaryGeometricNormal,
        raySample,
        secondarySurface,
        traceCandidate,
        hitPosition,
        hitNormal))
    {
        return result;
    }

    result = CleanGiShadeFirstIndirectSurface(
        secondarySurface,
        PathTraceFirstIndirectCandidateRaySampleIsSpecular(raySample),
        raySample.sourcePdf,
        shadeMode,
        rng);
    if (result.valid == 0u)
    {
        return result;
    }

    CleanGiAttachFirstIndirectDebugMaterial(result, secondarySurface.materialIndex);
    return result;
}

CleanGiProducerResult CleanGiTraceProducerRay(
    float3 primaryPosition,
    float3 primaryGeometricNormal,
    CleanGiFirstIndirectRaySample raySample,
    inout RTXDI_RandomSamplerState rng)
{
    return CleanGiTraceProducerRayWithShadeMode(
        primaryPosition,
        primaryGeometricNormal,
        raySample,
        CLEAN_GI_FIRST_INDIRECT_SHADE_FULL_NEE,
        rng);
}

bool CleanGiSampleSpecularProducerDirectionInternal(
    RAB_Surface surface,
    bool eligible,
    inout RTXDI_RandomSamplerState rng,
    out float3 bounceDir,
    out float solidAnglePdf)
{
    bounceDir = float3(0.0, 0.0, 0.0);
    solidAnglePdf = 0.0;
    if (!eligible)
    {
        return false;
    }

    const float3 specularF0 = max(GetSpecularF0(surface.material), float3(0.0, 0.0, 0.0));
    if (CleanGiLuminance(specularF0) < CLEAN_RESTIR_GI_SPECULAR_PRODUCER_MIN_F0)
    {
        return false;
    }

    const float3 normal = CleanGiSafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    const float3 geometryNormal = CleanGiSafeNormalize(RAB_GetSurfaceGeoNormal(surface), normal);
    const float3 viewDir = CleanGiSafeNormalize(RAB_GetSurfaceViewDir(surface), normal);
    if (dot(normal, viewDir) <= 0.0 || dot(geometryNormal, viewDir) <= 0.0)
    {
        return false;
    }

    const float roughness = max(saturate(GetRoughness(surface.material)), 0.02);
    const float alpha = max(roughness * roughness, 1.0e-3);
    const float alphaSquared = max(alpha * alpha, 1.0e-6);
    const float2 randomValues = float2(RAB_GetNextRandom(rng), RAB_GetNextRandom(rng));
    const float phi = 2.0 * RTXDI_PI * randomValues.x;
    const float cosTheta = sqrt((1.0 - randomValues.y) / max(1.0 + (alphaSquared - 1.0) * randomValues.y, 1.0e-6));
    const float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    const float3 tangent = CleanGiBuildPerpendicular(normal);
    const float3 bitangent = CleanGiSafeNormalize(cross(normal, tangent), float3(0.0, 1.0, 0.0));
    float3 halfVector = CleanGiSafeNormalize(
        tangent * (cos(phi) * sinTheta) +
        bitangent * (sin(phi) * sinTheta) +
        normal * cosTheta,
        normal);
    if (dot(halfVector, viewDir) <= 0.0)
    {
        halfVector = -halfVector;
    }

    bounceDir = CleanGiSafeNormalize(reflect(-viewDir, halfVector), normal);
    const float ndotl = dot(normal, bounceDir);
    const float gdntl = dot(geometryNormal, bounceDir);
    const float ndoth = saturate(dot(normal, halfVector));
    const float vdoth = saturate(dot(viewDir, halfVector));
    if (ndotl <= 0.0 || gdntl <= 0.0 || ndoth <= 0.0 || vdoth <= 0.0)
    {
        return false;
    }

    const float denominator = max((ndoth * ndoth) * (alphaSquared - 1.0) + 1.0, 1.0e-6);
    const float D = alphaSquared / max(RTXDI_PI * denominator * denominator, 1.0e-6);
    solidAnglePdf = (D * ndoth) / max(4.0 * vdoth, 1.0e-6);
    return solidAnglePdf > 1.0e-6 && solidAnglePdf == solidAnglePdf;
}

bool CleanGiSampleSpecularProducerDirection(
    RAB_Surface surface,
    inout RTXDI_RandomSamplerState rng,
    out float3 bounceDir,
    out float solidAnglePdf)
{
    return CleanGiSampleSpecularProducerDirectionInternal(
        surface,
        CleanGiSurfaceSupportsSpecularProducer(surface),
        rng,
        bounceDir,
        solidAnglePdf);
}

bool CleanGiSampleGlossySecondRayDirection(
    RAB_Surface surface,
    inout RTXDI_RandomSamplerState rng,
    out float3 bounceDir,
    out float solidAnglePdf)
{
    return CleanGiSampleSpecularProducerDirectionInternal(
        surface,
        CleanGiSurfaceSupportsGlossySecondRay(surface),
        rng,
        bounceDir,
        solidAnglePdf);
}

void CleanGiProducerMixtureProbabilities(RAB_Surface surface, out float diffuseProbability, out float specularProbability)
{
    diffuseProbability = 1.0;
    specularProbability = 0.0;
    if (!RAB_SurfaceSupportsOpaqueDiffuseBrdf(surface))
    {
        return;
    }

    const float diffuseWeight = max(CleanGiLuminance(GetDiffuseAlbedo(surface.material)), 1.0e-4);
    if (!CleanGiSurfaceSupportsSpecularProducer(surface))
    {
        return;
    }

    const float specularLum = CleanGiLuminance(GetSpecularF0(surface.material));
    if (specularLum < CLEAN_RESTIR_GI_SPECULAR_PRODUCER_MIN_F0)
    {
        return;
    }

    const float roughness = saturate(GetRoughness(surface.material));
    const float glossWeight = lerp(1.25, 0.35, roughness);
    const float specularWeight = max(specularLum * glossWeight, 0.0);
    specularProbability = clamp(specularWeight / max(diffuseWeight + specularWeight, 1.0e-4), 0.05, 0.95);
    diffuseProbability = 1.0 - specularProbability;
}

float CleanGiDiffuseProducerPdf(RAB_Surface surface, float3 bounceDir)
{
    if (!RAB_SurfaceSupportsOpaqueDiffuseBrdf(surface))
    {
        return 0.0;
    }
    const float3 normal = CleanGiSafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    const float3 geometryNormal = CleanGiSafeNormalize(RAB_GetSurfaceGeoNormal(surface), normal);
    const float ndotl = saturate(dot(normal, bounceDir));
    return dot(geometryNormal, bounceDir) > 0.0 ? ndotl / RTXDI_PI : 0.0;
}

float CleanGiDiffuseProducerPdfForNormal(RAB_Surface surface, float3 sampleNormal, float3 geometryNormal, float3 bounceDir)
{
    if (!RAB_SurfaceSupportsOpaqueDiffuseBrdf(surface))
    {
        return 0.0;
    }

    const float ndotl = saturate(dot(sampleNormal, bounceDir));
    return dot(geometryNormal, bounceDir) > 0.0 ? ndotl / RTXDI_PI : 0.0;
}

float CleanGiSpecularProducerPdf(RAB_Surface surface, float3 bounceDir)
{
    if (!CleanGiSurfaceSupportsSpecularProducer(surface))
    {
        return 0.0;
    }

    const float3 specularF0 = max(GetSpecularF0(surface.material), float3(0.0, 0.0, 0.0));
    if (CleanGiLuminance(specularF0) < CLEAN_RESTIR_GI_SPECULAR_PRODUCER_MIN_F0)
    {
        return 0.0;
    }

    const float3 normal = CleanGiSafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    const float3 geometryNormal = CleanGiSafeNormalize(RAB_GetSurfaceGeoNormal(surface), normal);
    const float3 viewDir = CleanGiSafeNormalize(RAB_GetSurfaceViewDir(surface), normal);
    if (dot(normal, bounceDir) <= 0.0 || dot(geometryNormal, bounceDir) <= 0.0 ||
        dot(normal, viewDir) <= 0.0 || dot(geometryNormal, viewDir) <= 0.0)
    {
        return 0.0;
    }

    const float3 halfVector = CleanGiSafeNormalize(viewDir + bounceDir, normal);
    const float ndoth = saturate(dot(normal, halfVector));
    const float vdoth = saturate(dot(viewDir, halfVector));
    if (ndoth <= 0.0 || vdoth <= 0.0)
    {
        return 0.0;
    }

    const float roughness = max(saturate(GetRoughness(surface.material)), 0.02);
    const float alpha = max(roughness * roughness, 1.0e-3);
    const float alphaSquared = max(alpha * alpha, 1.0e-6);
    const float denominator = max((ndoth * ndoth) * (alphaSquared - 1.0) + 1.0, 1.0e-6);
    const float D = alphaSquared / max(RTXDI_PI * denominator * denominator, 1.0e-6);
    const float pdf = (D * ndoth) / max(4.0 * vdoth, 1.0e-6);
    return pdf > 0.0 && pdf == pdf ? pdf : 0.0;
}

float CleanGiProducerMixturePdf(RAB_Surface surface, float3 bounceDir)
{
    float diffuseProbability;
    float specularProbability;
    CleanGiProducerMixtureProbabilities(surface, diffuseProbability, specularProbability);
    const float diffusePdf = CleanGiDiffuseProducerPdf(surface, bounceDir);
    const float specularPdf = specularProbability > 0.0 ? CleanGiSpecularProducerPdf(surface, bounceDir) : 0.0;
    const float mixturePdf = diffuseProbability * diffusePdf + specularProbability * specularPdf;
    return mixturePdf > 0.0 && mixturePdf == mixturePdf ? mixturePdf : 0.0;
}

CleanGiFirstIndirectRaySample CleanGiSampleDiffuseFirstIndirectRay(
    RAB_Surface surface,
    float3 sampleNormal,
    float3 geometryNormal,
    inout RTXDI_RandomSamplerState rng)
{
    const float2 randomValues = float2(RAB_GetNextRandom(rng), RAB_GetNextRandom(rng));
    const float3 bounceDir = RAB_CosineHemisphereDirection(sampleNormal, randomValues);
    const float diffusePdf = CleanGiDiffuseProducerPdfForNormal(surface, sampleNormal, geometryNormal, bounceDir);
    return CleanGiMakeFirstIndirectRaySample(
        bounceDir,
        diffusePdf,
        PATH_TRACE_FIRST_INDIRECT_CANDIDATE_FLAG_DIFFUSE_LOBE,
        1.0,
        diffusePdf);
}

CleanGiFirstIndirectRaySample CleanGiSampleSpecularFirstIndirectRay(
    RAB_Surface surface,
    inout RTXDI_RandomSamplerState rng)
{
    float3 bounceDir;
    float solidAnglePdf;
    if (!CleanGiSampleSpecularProducerDirection(surface, rng, bounceDir, solidAnglePdf))
    {
        return (CleanGiFirstIndirectRaySample)0;
    }

    return CleanGiMakeFirstIndirectRaySample(
        bounceDir,
        solidAnglePdf,
        PATH_TRACE_FIRST_INDIRECT_CANDIDATE_FLAG_SPECULAR_LOBE,
        1.0,
        solidAnglePdf);
}

CleanGiFirstIndirectRaySample CleanGiSampleGlossySecondFirstIndirectRay(
    RAB_Surface surface,
    inout RTXDI_RandomSamplerState rng)
{
    float3 bounceDir;
    float solidAnglePdf;
    if (!CleanGiSampleGlossySecondRayDirection(surface, rng, bounceDir, solidAnglePdf))
    {
        return (CleanGiFirstIndirectRaySample)0;
    }

    return CleanGiMakeFirstIndirectRaySample(
        bounceDir,
        solidAnglePdf,
        PATH_TRACE_FIRST_INDIRECT_CANDIDATE_FLAG_SPECULAR_LOBE,
        1.0,
        solidAnglePdf);
}

CleanGiFirstIndirectRaySample CleanGiSampleProducerFirstIndirectRay(
    RAB_Surface surface,
    float3 sampleNormal,
    float3 geometryNormal,
    inout RTXDI_RandomSamplerState rng)
{
    if (!CleanGiMixedFirstIndirectProducerActive())
    {
        return CleanGiSampleDiffuseFirstIndirectRay(surface, sampleNormal, geometryNormal, rng);
    }

    float diffuseProbability;
    float specularProbability;
    CleanGiProducerMixtureProbabilities(surface, diffuseProbability, specularProbability);

    const float lobeSelector = RAB_GetNextRandom(rng);
    if (specularProbability > 0.0 && lobeSelector < specularProbability)
    {
        float3 bounceDir;
        float specularPdf;
        if (!CleanGiSampleSpecularProducerDirection(surface, rng, bounceDir, specularPdf))
        {
            return (CleanGiFirstIndirectRaySample)0;
        }

        const float diffusePdf = CleanGiDiffuseProducerPdf(surface, bounceDir);
        const float mixturePdf = diffuseProbability * diffusePdf + specularProbability * specularPdf;
        CleanGiFirstIndirectRaySample sample = CleanGiMakeFirstIndirectRaySample(
            bounceDir,
            mixturePdf,
            PATH_TRACE_FIRST_INDIRECT_CANDIDATE_FLAG_SPECULAR_LOBE,
            specularProbability,
            specularPdf);
        sample.randomsConsumed = PATH_TRACE_FIRST_INDIRECT_CANDIDATE_RANDOMS_TWO_D + 1u;
        return sample;
    }

    const float2 randomValues = float2(RAB_GetNextRandom(rng), RAB_GetNextRandom(rng));
    const float3 bounceDir = RAB_CosineHemisphereDirection(sampleNormal, randomValues);
    const float diffusePdf = CleanGiDiffuseProducerPdfForNormal(surface, sampleNormal, geometryNormal, bounceDir);
    const float specularPdf = specularProbability > 0.0
        ? CleanGiSpecularProducerPdf(surface, bounceDir)
        : 0.0;
    const float mixturePdf = diffuseProbability * diffusePdf + specularProbability * specularPdf;
    CleanGiFirstIndirectRaySample sample = CleanGiMakeFirstIndirectRaySample(
        bounceDir,
        mixturePdf,
        PATH_TRACE_FIRST_INDIRECT_CANDIDATE_FLAG_DIFFUSE_LOBE,
        diffuseProbability,
        diffusePdf);
    sample.randomsConsumed = PATH_TRACE_FIRST_INDIRECT_CANDIDATE_RANDOMS_TWO_D + 1u;
    return sample;
}

CleanGiProducerResult CleanGiRunProducer(uint2 pixel, PathTracePrimarySurfaceRecord record, inout RTXDI_RandomSamplerState rng)
{
    const RAB_Surface surface = CleanGiMaterialSurfaceFromCurrentRecord(pixel, record);
    const float3 primaryShadingNormal = CleanGiSafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    const float3 primaryGeometricNormal = CleanGiSafeNormalize(RAB_GetSurfaceGeoNormal(surface), primaryShadingNormal);

    const CleanGiFirstIndirectRaySample raySample = CleanGiSampleProducerFirstIndirectRay(
        surface,
        primaryShadingNormal,
        primaryGeometricNormal,
        rng);
    return CleanGiTraceProducerRay(RAB_GetSurfaceWorldPos(surface), primaryGeometricNormal, raySample, rng);
}

CleanGiSpecularProducerDebug CleanGiBuildSpecularProducerDebug(uint2 pixel, PathTracePrimarySurfaceRecord record, inout RTXDI_RandomSamplerState rng)
{
    CleanGiSpecularProducerDebug debug = (CleanGiSpecularProducerDebug)0;
    const RAB_Surface surface = CleanGiMaterialSurfaceFromCurrentRecord(pixel, record);
    const CleanGiFirstIndirectRaySample raySample = CleanGiSampleSpecularFirstIndirectRay(surface, rng);
    if (!PathTraceFirstIndirectCandidateRaySampleIsValid(raySample))
    {
        return debug;
    }
    debug.sampledDirection = 1u;
    debug.bounceDir = raySample.direction;
    CleanGiProducerMixtureProbabilities(surface, debug.diffuseProbability, debug.specularProbability);
    debug.diffusePdf = CleanGiDiffuseProducerPdf(surface, raySample.direction);
    debug.specularPdf = CleanGiSpecularProducerPdf(surface, raySample.direction);
    const float mixturePdf = CleanGiProducerMixturePdf(surface, raySample.direction);
    debug.mixturePdf = mixturePdf;
    debug.producer = CleanGiTraceProducerRay(
        RAB_GetSurfaceWorldPos(surface),
        RAB_GetSurfaceGeoNormal(surface),
        raySample,
        rng);
    return debug;
}

CleanGiProducerResult CleanGiRunSpecularProducer(uint2 pixel, PathTracePrimarySurfaceRecord record, inout RTXDI_RandomSamplerState rng)
{
    const CleanGiSpecularProducerDebug debug = CleanGiBuildSpecularProducerDebug(pixel, record, rng);
    return debug.producer;
}

// RGI-03/RGI-04: drives the frozen temporal contract. Builds the initial
// reservoir from the producer textures (via the REMIX_RAB_* initial-sample
// callbacks), stores it to the INIT page, then runs RTXDI GI temporal
// resampling against the TEMPORAL_INPUT page and stores the TEMPORAL_OUTPUT
// page. With temporal disabled the initial reservoir passes through.
RemixRestirGITemporalReuseResult CleanGiRunTemporalContract(
    uint2 pixel,
    uint2 dimensions,
    bool surfaceValid,
    PathTracePrimarySurfaceRecord record)
{
    RAB_Surface surface = RAB_EmptySurface();
    if (surfaceValid)
    {
        surface = CleanGiMaterialSurfaceFromCurrentRecord(pixel, record);
    }
    const bool glossyReuseQuarantine = CleanGiSpecularProducerNeedsReuseQuarantine(surface);

    RemixRestirGITemporalReuseDesc desc = (RemixRestirGITemporalReuseDesc)0;
    desc.pixel = pixel;
    desc.frameIndex = CleanRestirGiFrameIndex;
    desc.screenSpaceMotion = surfaceValid
        ? CleanGiLoadScreenSpaceMotion(pixel, record, dimensions)
        : float3(0.0, 0.0, 0.0);
    desc.initSamplePage = RemixRAB_GetGIInitSampleReservoirIndex();
    desc.temporalInputPage = RemixRAB_GetGITemporalInputReservoirIndex();
    desc.temporalOutputPage = RemixRAB_GetGITemporalOutputReservoirIndex();
    desc.activeCheckerboardField = 0u;
    const bool transmissionPsrResolved = surfaceValid && CleanGiSurfaceRecordIsTransmissionPsrResolved(record);
    desc.enableTemporalReuse = (glossyReuseQuarantine || transmissionPsrResolved) ? 0u : CleanRestirGiTemporalEnabled;
    // Keep GI history validation conservative. DI temporal can tolerate
    // broader gates because its sample is a direct-light replay; GI history
    // stores full indirect radiance at a secondary point, so accepting the
    // wrong previous receiver paints clustered stale energy into DLSSRR.
    const float viewDotNormal = surfaceValid
        ? abs(dot(CleanGiSafeNormalize(record.viewDirectionAndReserved.xyz, float3(0.0, 0.0, 1.0)),
            CleanGiSafeNormalize(record.geometricNormalAndRoughness.xyz, float3(0.0, 0.0, 1.0))))
        : 1.0;
    float depthThreshold = 0.01 / max(viewDotNormal, 0.01);
    float normalThreshold = surfaceValid
        ? lerp(0.995, 0.5, saturate(record.geometricNormalAndRoughness.w))
        : 0.995;
    const bool temporalMotionActive =
        dot(desc.screenSpaceMotion.xy, desc.screenSpaceMotion.xy) > 0.25 ||
        abs(desc.screenSpaceMotion.z) > 0.25;
    desc.depthThreshold = depthThreshold;
    desc.normalThreshold = normalThreshold;
    desc.maxHistoryLength = glossyReuseQuarantine
        ? 1u
        : (temporalMotionActive ? min(CleanRestirGiMaxHistoryLength, 2u) : CleanRestirGiMaxHistoryLength);
    desc.enableFallbackSampling = 0u;
    desc.biasCorrectionMode = CleanRestirGiBiasCorrection;
    desc.maxReservoirAge = temporalMotionActive
        ? min(CleanRestirGiMaxReservoirAge, 8u)
        : CleanRestirGiMaxReservoirAge;
    desc.enablePermutationSampling = 0u;
    desc.uniformRandomNumber = 0u;
    desc.fireflyFilteringLuminanceThreshold = CleanRestirGiFireflyThreshold;
    return RemixRestirGIRunTemporalReuseContract(surface, desc);
}

// ---------------------------------------------------------------------------
// RGI-05: NEE cache seed. Mirrors the Remix integrate_nee ReSTIR GI feed:
// build a GI sample from the cache-selected light at the PRIMARY surface
// (position/normal from the light sample, radiance = lightSample.radiance /
// (solidAnglePdf * cacheSourcePdf), visible samples only), stream it into an
// un-finalized reservoir (M=1, weightSum = wi), and store it on the INIT page
// before the temporal contract's initial RIS update merges it (M forced to 1
// at finalize, so this adds a candidate without an energy shift).
// ---------------------------------------------------------------------------

static const uint CLEAN_RESTIR_GI_NEE_SEED_RNG_PASS = 0x52525811u;

void CleanGiMergeProducerSeedIntoInitPage(
    uint2 pixel,
    RAB_Surface surface,
    CleanGiProducerResult producer,
    float randomValue)
{
    if (producer.valid == 0u || producer.sourcePdf <= 1.0e-6 || !RAB_IsSurfaceValid(surface) || !CleanGiAllFinite3(producer.radiance))
    {
        return;
    }

    const RTXDI_GIReservoir sample = RTXDI_MakeGIReservoir(
        producer.hitPosition,
        CleanGiSafeNormalize(producer.hitNormal, float3(0.0, 0.0, 1.0)),
        max(producer.radiance, float3(0.0, 0.0, 0.0)),
        producer.sourcePdf);
    const float targetPdf = RemixRAB_GetGISampleTargetPdfForSurface(sample.position, sample.radiance, surface);
    if (targetPdf <= 0.0)
    {
        return;
    }

    RTXDI_GIReservoir seeded = RAB_LoadGIReservoir(int2(pixel), int(RemixRAB_GetGIInitSampleReservoirIndex()));
    RTXDI_CombineGIReservoirs(seeded, sample, randomValue, targetPdf);
    RAB_StoreGIReservoir(seeded, int2(pixel), int(RemixRAB_GetGIInitSampleReservoirIndex()));
}

void CleanGiMergePackedSpecularSeedIntoInitPage(
    uint2 pixel,
    CleanGiSpecularSeedReceiverSurface receiver,
    CleanGiProducerResult producer,
    float randomValue)
{
    if (producer.valid == 0u || producer.sourcePdf <= 1.0e-6 || receiver.valid == 0u || !CleanGiAllFinite3(producer.radiance))
    {
        return;
    }

    const RTXDI_GIReservoir sample = RTXDI_MakeGIReservoir(
        producer.hitPosition,
        CleanGiSafeNormalize(producer.hitNormal, float3(0.0, 0.0, 1.0)),
        max(producer.radiance, float3(0.0, 0.0, 0.0)),
        producer.sourcePdf);
    const float targetPdf = CleanGiSpecularSeedReceiverTargetPdf(receiver, sample.position, sample.radiance);
    if (targetPdf <= 0.0)
    {
        return;
    }

    RTXDI_GIReservoir seeded = RAB_LoadGIReservoir(int2(pixel), int(RemixRAB_GetGIInitSampleReservoirIndex()));
    RTXDI_CombineGIReservoirs(seeded, sample, randomValue, targetPdf);
    RAB_StoreGIReservoir(seeded, int2(pixel), int(RemixRAB_GetGIInitSampleReservoirIndex()));
}

void CleanGiSeedInitPageFromSpecularProducer(uint2 pixel, bool surfaceValid, PathTracePrimarySurfaceRecord record)
{
    if (!CleanGiSpecularSeedProducerActive() || !surfaceValid)
    {
        return;
    }

    const RAB_Surface surface = CleanGiMaterialSurfaceFromCurrentRecord(pixel, record);
    if (!CleanGiSurfaceSupportsSpecularProducer(surface))
    {
        return;
    }

    RTXDI_RandomSamplerState rng = CleanGiInitProducerRandomSampler(pixel, CleanRestirGiFrameIndex, CLEAN_RESTIR_GI_SPECULAR_PRODUCER_RNG_PASS);
    const CleanGiProducerResult producer = CleanGiRunSpecularProducer(pixel, record, rng);
    CleanGiMergeProducerSeedIntoInitPage(pixel, surface, producer, RAB_GetNextRandom(rng));
}

void CleanGiSeedInitPageFromGlossySecondRay(uint2 pixel, bool surfaceValid, PathTracePrimarySurfaceRecord record)
{
    if (!surfaceValid)
    {
        return;
    }

    const RAB_Surface surface = CleanGiMaterialSurfaceFromCurrentRecord(pixel, record);
    if (!CleanGiSurfaceSupportsGlossySecondRay(surface))
    {
        return;
    }

    RTXDI_RandomSamplerState rng = CleanGiInitProducerRandomSampler(pixel, CleanRestirGiFrameIndex, CLEAN_RESTIR_GI_GLOSSY_SECOND_RAY_RNG_PASS);
    const CleanGiFirstIndirectRaySample raySample = CleanGiSampleGlossySecondFirstIndirectRay(surface, rng);
    const CleanGiProducerResult producer = CleanGiTraceProducerRayWithShadeMode(
        RAB_GetSurfaceWorldPos(surface),
        RAB_GetSurfaceGeoNormal(surface),
        raySample,
        CleanGiProducerCurrentShadeMode(),
        rng);
    CleanGiMergeProducerSeedIntoInitPage(pixel, surface, producer, RAB_GetNextRandom(rng));
}

void CleanGiSeedInitPageFromNeeCache(uint2 pixel, bool surfaceValid, PathTracePrimarySurfaceRecord record)
{
    if (CleanRestirGiNeeCacheSeedEnabled == 0u)
    {
        return;
    }

    RTXDI_GIReservoir seeded = RAB_LoadGIReservoir(int2(pixel), int(RemixRAB_GetGIInitSampleReservoirIndex()));

    if (surfaceValid && CleanGiNeeCacheProviderReady())
    {
        RAB_Surface surface = CleanGiMaterialSurfaceFromCurrentRecord(pixel, record);
        const PathTraceNeeCacheCellDebug cell = PathTraceNeeCacheMapWorldPositionToCell(
            RAB_GetSurfaceWorldPos(surface),
            CleanRtxdiDiCameraOriginAndValid.xyz,
            max((uint)max(CleanRtxdiDiNeeCacheInfo1.x, 1.0), 1u),
            max(CleanRtxdiDiNeeCacheInfo1.y, 1.0),
            max((uint)max(CleanRtxdiDiNeeCacheInfo1.z, 1.0), 1u));
        const uint cellCount = max((uint)max(CleanRtxdiDiNeeCacheInfo1.z, 1.0), 1u);
        if (cell.valid != 0u && cell.cellIndex < cellCount)
        {
            RTXDI_RandomSamplerState rng = CleanGiInitProducerRandomSampler(pixel, CleanRestirGiFrameIndex, CLEAN_RESTIR_GI_NEE_SEED_RNG_PASS);
            uint selectedDenseRluIndex = 0xffffffffu;
            float sourceSelectionPdf = 0.0;
            PathTraceNeeCacheProviderResult cacheProviderResult = (PathTraceNeeCacheProviderResult)0;
            const float fallbackProbability = saturate(CleanRtxdiDiNeeCacheInfo0.y);
            const bool tryCache = RAB_GetNextRandom(rng) >= fallbackProbability;
            const bool selectedCacheCandidate =
                tryCache &&
                CleanGiSelectNeeCacheCandidateProvider(cell, pixel, 0x67697365u, cacheProviderResult);
            if (selectedCacheCandidate)
            {
                selectedDenseRluIndex = cacheProviderResult.selectedDenseRluIndex;
                sourceSelectionPdf = CleanGiNeeCacheMixtureIdentityPdf(cell, selectedDenseRluIndex);
            }
            else
            {
                CleanGiNeeCacheSelectFallbackProposal(rng, selectedDenseRluIndex, sourceSelectionPdf);
                sourceSelectionPdf = CleanGiNeeCacheMixtureIdentityPdf(cell, selectedDenseRluIndex);
            }

            if (selectedDenseRluIndex < CleanRtxdiDiRluCurrentLightCount &&
                sourceSelectionPdf > 1.0e-8)
            {
                const RAB_LightInfo lightInfo = CleanGiLoadCurrentRluLightInfo(selectedDenseRluIndex);
                if (RAB_IsLightInfoValid(lightInfo))
                {
                    const float2 uv = float2(RAB_GetNextRandom(rng), RAB_GetNextRandom(rng));
                    const RAB_LightSample lightSample = RAB_SamplePolymorphicLight(lightInfo, surface, uv);
                    if (RAB_IsReplayableLightSample(lightSample) &&
                        lightSample.solidAnglePdf > 1.0e-8 &&
                        CleanGiLuminance(lightSample.radiance) > 0.0)
                    {
                        float3 lightDir;
                        float lightDistance;
                        RAB_GetLightDirDistance(surface, lightSample, lightDir, lightDistance);
                        const float ndotl = saturate(dot(RAB_GetSurfaceNormal(surface), lightDir));
                        const float visibility = ndotl > 0.0
                            ? CleanGiTraceVisibility(RAB_GetSurfaceWorldPos(surface), RAB_GetSurfaceGeoNormal(surface), lightSample.position)
                            : 0.0;
                        if (visibility > 0.0)
                        {
                            const float3 seedRadiance = min(
                                lightSample.radiance / max(lightSample.solidAnglePdf * sourceSelectionPdf, 1.0e-6),
                                float3(65504.0, 65504.0, 65504.0));
                            const RTXDI_GIReservoir seedSample = RTXDI_MakeGIReservoir(
                                lightSample.position,
                                CleanGiSafeNormalize(lightSample.normal, -lightDir),
                                seedRadiance,
                                1.0);
                            const float wi = RemixRAB_GetGISampleTargetPdfForSurface(seedSample.position, seedSample.radiance, surface);
                            if (wi > 0.0)
                            {
                                RTXDI_CombineGIReservoirs(seeded, seedSample, RAB_GetNextRandom(rng), wi);
                            }
                        }
                    }
                }
            }
        }
    }

    RAB_StoreGIReservoir(seeded, int2(pixel), int(RemixRAB_GetGIInitSampleReservoirIndex()));
}

// ---------------------------------------------------------------------------
// RGI-07: final shading + resolve. Evaluates the primary diffuse and specular
// lobes toward reservoir.position. The historical GI output binding name is
// still "IndirectDiffuse", but the value is now final reflected indirect GI.
// ---------------------------------------------------------------------------

uint CleanGiFinalMixBaseMode()
{
    const uint mode = CleanRestirGiFinalMixMode >= 10u
        ? CleanRestirGiFinalMixMode - 10u
        : CleanRestirGiFinalMixMode;
    return mode <= 3u ? mode : 0u;
}

bool CleanGiFinalMixReservoirVisibilityEnabled()
{
    return CleanRestirGiFinalMixMode >= 10u && CleanRestirGiFinalMixMode <= 13u;
}

bool CleanGiFinalMixCanTraceReservoirVisibility(RAB_Surface surface)
{
    return surface.surfaceClass != RT_SMOKE_SURFACE_CLASS_SKINNED_DEFORMED &&
        surface.surfaceClass != RT_SMOKE_SURFACE_CLASS_TRANSLUCENT;
}

CleanGiIndirectLobeResult CleanGiBlendIndirectLobes(
    CleanGiIndirectLobeResult reservoirLobes,
    CleanGiIndirectLobeResult rawLobes,
    float rawWeight)
{
    rawWeight = saturate(rawWeight);

    CleanGiIndirectLobeResult result = (CleanGiIndirectLobeResult)0;
    result.diffuse = lerp(reservoirLobes.diffuse, rawLobes.diffuse, rawWeight);
    result.specular = lerp(reservoirLobes.specular, rawLobes.specular, rawWeight);

    const bool reservoirHitValid = reservoirLobes.hitDistance > 0.0 && reservoirLobes.hitDistance < 1.0e8;
    const bool rawHitValid = rawLobes.hitDistance > 0.0 && rawLobes.hitDistance < 1.0e8;
    if (reservoirHitValid && rawHitValid)
    {
        result.hitDistance = lerp(reservoirLobes.hitDistance, rawLobes.hitDistance, rawWeight);
    }
    else
    {
        result.hitDistance = rawHitValid ? rawLobes.hitDistance : reservoirLobes.hitDistance;
    }
    return result;
}

float CleanGiAdaptiveRawFinalMixWeight(RAB_Surface surface, RTXDI_GIReservoir reservoir)
{
    const float maxHistory = max((float)CleanRestirGiMaxHistoryLength, 1.0);
    const float historyConfidence = saturate(((float)reservoir.M - 1.0) / max(maxHistory - 1.0, 1.0));
    const float roughness = saturate(GetRoughness(surface.material));
    const float specularLum = RAB_MaterialLuminance(GetSpecularF0(surface.material));
    const float glossyWeight = saturate((0.45 - roughness) / 0.45) * saturate(specularLum * 8.0);
    return saturate((1.0 - historyConfidence) * 0.75 + glossyWeight * 0.25);
}

CleanGiIndirectLobeResult CleanGiFinalShadeIndirectSplitInternal(
    RAB_Surface surface,
    RTXDI_GIReservoir reservoir,
    bool finalVisibility)
{
    CleanGiIndirectLobeResult result = (CleanGiIndirectLobeResult)0;
    if (!RAB_IsSurfaceValid(surface) || !RTXDI_IsValidGIReservoir(reservoir))
    {
        return result;
    }
    const float weight = max(reservoir.weightSum, 0.0);
    if (weight <= 0.0 || !CleanGiAllFinite3(reservoir.radiance) || weight != weight)
    {
        return result;
    }
    const float3 toSample = reservoir.position - RAB_GetSurfaceWorldPos(surface);
    const float distanceSquared = dot(toSample, toSample);
    if (distanceSquared <= 1.0e-6)
    {
        return result;
    }
    const float3 sampleDir = toSample * rsqrt(distanceSquared);
    if (dot(sampleDir, RAB_GetSurfaceGeoNormal(surface)) <= 0.0)
    {
        return result;
    }
    if (finalVisibility &&
        CleanGiFinalMixCanTraceReservoirVisibility(surface) &&
        CleanGiTraceVisibility(RAB_GetSurfaceWorldPos(surface), RAB_GetSurfaceGeoNormal(surface), reservoir.position) <= 0.0)
    {
        return result;
    }
    float3 weightedRadiance = max(reservoir.radiance, float3(0.0, 0.0, 0.0)) * weight;
    if (CleanRestirGiContributionFireflyThreshold > 0.0)
    {
        const float contributionCap = CleanRestirGiContributionFireflyThreshold;
        const float weightedLuminance = CleanGiLuminance(weightedRadiance);
        if (weightedLuminance > contributionCap)
        {
            weightedRadiance *= contributionCap / max(weightedLuminance, 1.0e-6);
        }
    }
    result = CleanGiEvaluateIndirectLobesSplit(
        surface,
        sampleDir,
        weightedRadiance);
    result.hitDistance = sqrt(distanceSquared);
    return result;
}

CleanGiIndirectLobeResult CleanGiFinalShadeIndirectSplit(RAB_Surface surface, RTXDI_GIReservoir reservoir)
{
    return CleanGiFinalShadeIndirectSplitInternal(surface, reservoir, false);
}

float3 CleanGiFinalShadeIndirect(RAB_Surface surface, RTXDI_GIReservoir reservoir)
{
    const CleanGiIndirectLobeResult lobes = CleanGiFinalShadeIndirectSplit(surface, reservoir);
    const float3 indirect = lobes.diffuse + lobes.specular;
    return CleanGiAllFinite3(indirect) ? indirect : float3(0.0, 0.0, 0.0);
}

bool CleanGiReflectiveOutputEligible(RAB_Surface surface, CleanGiIndirectLobeResult lobes)
{
    if (!CleanGiSurfaceSupportsSpecularProducer(surface) ||
        lobes.hitDistance <= 0.0 ||
        lobes.hitDistance >= 1.0e8)
    {
        return false;
    }

    const float specularLuminance = CleanGiLuminance(lobes.specular);
    const float diffuseLuminance = CleanGiLuminance(lobes.diffuse);
    return specularLuminance > 1.0e-4 &&
        specularLuminance >= max(diffuseLuminance * 0.10, 1.0e-4);
}

bool CleanGiShouldWriteRrHitDistance(RAB_Surface surface, CleanGiIndirectLobeResult lobes)
{
    return CleanRestirGiRrHitDistanceEnabled != 0u && CleanGiReflectiveOutputEligible(surface, lobes);
}

// Writes the GI-O-05 output. The boiling-filter compute pass consumes it,
// clamps outliers, and performs the resolve add into the combined outputs
// (so the beauty image receives the FILTERED contribution).
void CleanGiFinalShadingAndResolve(uint2 pixel, RAB_Surface surface, RTXDI_GIReservoir reservoir)
{
    CleanGiIndirectLobeResult lobes = CleanGiFinalShadeIndirectSplitInternal(
        surface,
        reservoir,
        CleanGiFinalMixReservoirVisibilityEnabled());

    const uint finalMixMode = CleanGiFinalMixBaseMode();
    if (finalMixMode != 0u && RAB_IsSurfaceValid(surface))
    {
        const RemixRestirGIRawInitialSample rawSample = RemixRAB_LoadRawGIInitialSample(pixel);
        CleanGiIndirectLobeResult rawLobes = (CleanGiIndirectLobeResult)0;
        if (rawSample.valid != 0u && rawSample.sourcePdf > 1.0e-8)
        {
            const RTXDI_GIReservoir rawReservoir = RTXDI_MakeGIReservoir(
                rawSample.hitPosition,
                rawSample.hitNormal,
                rawSample.radiance,
                rawSample.sourcePdf);
            rawLobes = CleanGiFinalShadeIndirectSplitInternal(surface, rawReservoir, false);
        }

        if (finalMixMode == 1u)
        {
            lobes = rawLobes;
        }
        else
        {
            const bool rawHasEnergy = CleanGiLuminance(rawLobes.diffuse + rawLobes.specular) > 1.0e-8;
            const bool reservoirHasEnergy = CleanGiLuminance(lobes.diffuse + lobes.specular) > 1.0e-8;
            if (rawHasEnergy && reservoirHasEnergy)
            {
                const float rawWeight = finalMixMode == 2u
                    ? 0.5
                    : CleanGiAdaptiveRawFinalMixWeight(surface, reservoir);
                lobes = CleanGiBlendIndirectLobes(lobes, rawLobes, rawWeight);
            }
            else if (rawHasEnergy)
            {
                lobes = rawLobes;
            }
        }
    }

    const float3 indirect = lobes.diffuse + lobes.specular;
    CleanRestirGiIndirectDiffuse[pixel] = float4(indirect, 1.0);
    CleanRestirGiIndirectDiffuseLobe[pixel] = float4(lobes.diffuse, 1.0);
    CleanRestirGiIndirectSpecularLobe[pixel] = float4(lobes.specular, lobes.hitDistance);
    if (CleanRestirGiRrHitDistanceEnabled != 0u)
    {
        PathTraceRRGuideHitDistance[pixel] = CleanGiShouldWriteRrHitDistance(surface, lobes)
            ? lobes.hitDistance
            : 0.0;
    }
}

// ---------------------------------------------------------------------------
// Debug views
// ---------------------------------------------------------------------------

float3 CleanGiToneMap(float3 radiance)
{
    const float3 safeRadiance = max(radiance, float3(0.0, 0.0, 0.0));
    return safeRadiance / (float3(1.0, 1.0, 1.0) + safeRadiance);
}

float3 CleanGiProducerReservoirPathColor(
    uint2 pixel,
    bool surfaceValid,
    RAB_Surface surface,
    RTXDI_GIReservoir initialReservoir,
    RTXDI_GIReservoir temporalReservoir)
{
    if (!surfaceValid || !RAB_IsSurfaceValid(surface))
    {
        return float3(0.08, 0.08, 0.08);
    }

    const RemixRestirGIRawInitialSample rawSample = RemixRAB_LoadRawGIInitialSample(pixel);
    if (rawSample.valid == 0u)
    {
        uint producerWidth = 0u;
        uint producerHeight = 0u;
        CleanRestirGiProducerHitPosition.GetDimensions(producerWidth, producerHeight);
        if (producerWidth != 0u && producerHeight != 0u && pixel.x < producerWidth && pixel.y < producerHeight)
        {
            const uint flatIndex = pixel.y * producerWidth + pixel.x;
            const CleanGiProducerSurface gbuf = CleanGiProducerSurfaceBuffer[flatIndex];
            if (gbuf.valid != 0u)
            {
                return float3(1.0, 1.0, 1.0);
            }
        }

        const float statusValue = CleanRestirGiProducerHitNormal[pixel].w;
        const uint traceStatus = statusValue == statusValue ? (uint)(statusValue + 0.5) : 0u;
        if (traceStatus == CLEAN_GI_PRODUCER_TRACE_STATUS_SAMPLE_REJECTED)
        {
            return float3(1.0, 0.45, 0.0);
        }
        if (traceStatus == CLEAN_GI_PRODUCER_TRACE_STATUS_SAMPLE_PDF_REJECTED)
        {
            return float3(1.0, 0.8, 0.0);
        }
        if (traceStatus == CLEAN_GI_PRODUCER_TRACE_STATUS_SAMPLE_GEOMETRY_REJECTED)
        {
            return float3(1.0, 0.25, 0.0);
        }
        if (traceStatus == CLEAN_GI_PRODUCER_TRACE_STATUS_METADATA_MISS)
        {
            return float3(1.0, 1.0, 0.0);
        }
        if (traceStatus == CLEAN_GI_PRODUCER_TRACE_STATUS_SURFACE_BUILD_MISS)
        {
            return float3(0.65, 0.0, 1.0);
        }
        if (traceStatus == CLEAN_GI_PRODUCER_TRACE_STATUS_ANYHIT_REJECTED)
        {
            return float3(0.0, 0.85, 1.0);
        }
        if (traceStatus == CLEAN_GI_PRODUCER_TRACE_STATUS_QUERY_MISS_PRIMARY_PROBE_HIT)
        {
            return float3(0.0, 0.25, 1.0);
        }
        if (traceStatus == CLEAN_GI_PRODUCER_TRACE_STATUS_QUERY_MISS_TARGET_PROBE_HIT)
        {
            return float3(0.0, 0.65, 1.0);
        }
        if (traceStatus == CLEAN_GI_PRODUCER_TRACE_STATUS_QUERY_MISS_PROBE_INPUT_INVALID)
        {
            return float3(1.0, 0.15, 0.35);
        }
        return float3(1.0, 0.0, 0.0);
    }
    if (!CleanGiAllFinite3(rawSample.radiance) || CleanGiLuminance(rawSample.radiance) <= 1.0e-6)
    {
        uint producerWidth = 0u;
        uint producerHeight = 0u;
        CleanRestirGiProducerHitPosition.GetDimensions(producerWidth, producerHeight);
        if (producerWidth != 0u && producerHeight != 0u && pixel.x < producerWidth && pixel.y < producerHeight)
        {
            const uint flatIndex = pixel.y * producerWidth + pixel.x;
            const CleanGiProducerSurface gbuf = CleanGiProducerSurfaceBuffer[flatIndex];
            if (gbuf.valid != 0u)
            {
                RAB_Surface secondarySurface = CleanGiUnpackProducerSurface(gbuf);
                RTXDI_RandomSamplerState debugRng = CleanGiInitProducerRandomSampler(
                    pixel,
                    CleanRestirGiFrameIndex,
                    CLEAN_RESTIR_GI_PRODUCER_RNG_PASS);
                CleanGiSkipFirstIndirectCandidateRaySampleRandoms(gbuf, debugRng);
                return CleanGiProducerShadeGateDebugColor(
                    secondarySurface,
                    gbuf.primarySampledSpecular != 0u,
                    debugRng);
            }
        }
        return float3(0.35, 0.0, 0.0);
    }

    const float3 toSample = rawSample.hitPosition - RAB_GetSurfaceWorldPos(surface);
    const float distanceSquared = dot(toSample, toSample);
    if (distanceSquared <= 1.0e-6 || distanceSquared != distanceSquared)
    {
        return float3(1.0, 0.35, 0.0);
    }

    const float3 sampleDir = toSample * rsqrt(distanceSquared);
    if (dot(sampleDir, RAB_GetSurfaceGeoNormal(surface)) <= 0.0)
    {
        return float3(0.0, 0.15, 1.0);
    }
    if (dot(sampleDir, RAB_GetSurfaceNormal(surface)) <= 0.0)
    {
        return float3(0.55, 0.0, 1.0);
    }

    const float targetPdf = RemixRAB_GetGISampleTargetPdfForSurface(rawSample.hitPosition, rawSample.radiance, surface);
    if (targetPdf <= 1.0e-8 || targetPdf != targetPdf)
    {
        return float3(0.0, 0.8, 0.85);
    }
    if (!RTXDI_IsValidGIReservoir(initialReservoir) ||
        initialReservoir.weightSum <= 0.0 ||
        initialReservoir.weightSum != initialReservoir.weightSum ||
        !CleanGiAllFinite3(initialReservoir.radiance))
    {
        return float3(1.0, 1.0, 0.0);
    }
    if (!RTXDI_IsValidGIReservoir(temporalReservoir) ||
        temporalReservoir.weightSum <= 0.0 ||
        temporalReservoir.weightSum != temporalReservoir.weightSum ||
        !CleanGiAllFinite3(temporalReservoir.radiance))
    {
        return float3(1.0, 0.0, 1.0);
    }

    const float3 finalIndirect = CleanGiFinalShadeIndirect(surface, temporalReservoir);
    const float finalLuminance = CleanGiAllFinite3(finalIndirect) ? CleanGiLuminance(finalIndirect) : -1.0;
    if (finalLuminance <= 1.0e-6)
    {
        return float3(0.0, 0.45, 0.35);
    }

    return CleanGiToneMap(float3(0.0, finalLuminance, 0.0));
}

float3 CleanGiStoredSpecularOutputColor(uint2 pixel)
{
    const float4 storedSpecular = CleanRestirGiIndirectSpecularLobe[pixel];
    if (!CleanGiAllFinite3(storedSpecular.rgb) || storedSpecular.a != storedSpecular.a)
    {
        return float3(1.0, 1.0, 0.0);
    }
    return CleanGiToneMap(max(storedSpecular.rgb, float3(0.0, 0.0, 0.0)));
}

float3 CleanGiNeeCacheProviderStateColor(bool surfaceValid, PathTracePrimarySurfaceRecord record)
{
    if (!CleanGiNeeCacheProviderReady())
    {
        return float3(0.0, 0.0, 0.35);
    }
    if (!surfaceValid)
    {
        return float3(0.08, 0.08, 0.08);
    }

    const RAB_Surface surface = CleanGiMaterialSurfaceFromRecord(record);
    const PathTraceNeeCacheCellDebug cell = PathTraceNeeCacheMapWorldPositionToCell(
        RAB_GetSurfaceWorldPos(surface),
        CleanRtxdiDiCameraOriginAndValid.xyz,
        max((uint)max(CleanRtxdiDiNeeCacheInfo1.x, 1.0), 1u),
        max(CleanRtxdiDiNeeCacheInfo1.y, 1.0),
        max((uint)max(CleanRtxdiDiNeeCacheInfo1.z, 1.0), 1u));
    const uint cellCount = max((uint)max(CleanRtxdiDiNeeCacheInfo1.z, 1.0), 1u);
    if (cell.valid == 0u || cell.cellIndex >= cellCount)
    {
        return float3(0.45, 0.0, 0.45);
    }

    const uint candidateSlots = max((uint)max(CleanRtxdiDiNeeCacheInfo0.w, 0.0), 0u);
    const PathTraceNeeCacheCellRecord storedCell = CleanRestirGiNeeCacheCells[cell.cellIndex];
    const bool matchingCacheCell = storedCell.flags != 0u && storedCell.hash == cell.hash;
    const bool cacheCandidatesPresent = matchingCacheCell && storedCell.candidateCount > 0u;
    bool usableCandidatePresent = false;
    if (matchingCacheCell && candidateSlots > 0u)
    {
        const uint baseSlot = cell.cellIndex * candidateSlots;
        const uint scanCount = min(candidateSlots, 8u);
        [loop]
        for (uint slot = 0u; slot < scanCount; ++slot)
        {
            const PathTraceNeeCacheCandidateRecord candidate = CleanRestirGiNeeCacheCandidates[baseSlot + slot];
            if (candidate.flags != 0u &&
                candidate.cellIndex == cell.cellIndex &&
                candidate.denseRluIndex < CleanRtxdiDiRluCurrentLightCount &&
                candidate.sourcePdf > 0.0 &&
                candidate.invSourcePdf > 0.0)
            {
                usableCandidatePresent = true;
            }
        }
    }

    bool localFallbackProvider = false;
    PathTraceNeeCacheProviderResult providerResult = (PathTraceNeeCacheProviderResult)0;
    if (matchingCacheCell)
    {
        providerResult = CleanRestirGiNeeCacheProviderResults[cell.cellIndex];
    }
    if (!matchingCacheCell)
    {
        providerResult = (PathTraceNeeCacheProviderResult)0;
    }
    if (providerResult.flags == 0u)
    {
        PathTraceNeeCacheProviderResult fallbackResult;
        if (CleanGiBuildNeeCacheFallbackProvider(cell, 0u, fallbackResult))
        {
            providerResult = fallbackResult;
            localFallbackProvider = true;
        }
    }
    if (providerResult.flags == 0u)
    {
        return float3(0.22, 0.10, 0.0);
    }
    if (providerResult.selectedDenseRluIndex >= CleanRtxdiDiRluCurrentLightCount ||
        providerResult.sourcePdf <= 1.0e-8)
    {
        return float3(0.0, 0.45, 0.45);
    }

    const PathTraceUnifiedLightRecord light = CleanRestirGiRluCurrentLights[providerResult.selectedDenseRluIndex];
    const RAB_LightInfo lightInfo = CleanGiLoadCurrentRluLightInfo(providerResult.selectedDenseRluIndex);
    if (!RAB_IsLightInfoValid(lightInfo))
    {
        return float3(0.0, 0.45, 0.45);
    }

    const float pdfBand = saturate(log2(max(providerResult.sourcePdf, 1.0e-8) * 1024.0 + 1.0) / 10.0);
    if (providerResult.sourceLabel == PATH_TRACE_NEE_CACHE_SOURCE_CACHE_EMISSIVE)
    {
        return float3(0.0, 0.55 + 0.45 * pdfBand, 0.12);
    }
    if (providerResult.sourceLabel == PATH_TRACE_NEE_CACHE_SOURCE_CACHE_ANALYTIC)
    {
        return float3(0.95, 0.12 + 0.28 * pdfBand, 0.02);
    }
    if (providerResult.sourceLabel == PATH_TRACE_NEE_CACHE_SOURCE_FALLBACK_TYPED_RLU)
    {
        if (localFallbackProvider && usableCandidatePresent)
        {
            return float3(0.95, 0.0, 0.85);
        }
        if (!localFallbackProvider && usableCandidatePresent)
        {
            return float3(1.0, 0.85, 0.05);
        }
        if (!localFallbackProvider && cacheCandidatesPresent)
        {
            return float3(0.80, 0.42, 0.05);
        }
        if (light.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
        {
            return localFallbackProvider ? float3(0.0, 0.36, 0.50) : float3(0.0, 0.78, 0.95);
        }
        return localFallbackProvider ? float3(0.04, 0.08, 0.46) : float3(0.15, 0.28, 1.0);
    }
    if (providerResult.sourceLabel == PATH_TRACE_NEE_CACHE_SOURCE_FALLBACK_FULL_RLU)
    {
        if (localFallbackProvider && usableCandidatePresent)
        {
            return float3(0.95, 0.0, 0.85);
        }
        if (!localFallbackProvider && usableCandidatePresent)
        {
            return float3(1.0, 0.85, 0.05);
        }
        if (!localFallbackProvider && cacheCandidatesPresent)
        {
            return float3(0.80, 0.42, 0.05);
        }
        return localFallbackProvider ? float3(0.24, 0.0, 0.48) : float3(0.45, 0.0, 0.95);
    }
    return float3(0.8, 0.0, 0.8);
}

// RGI-01 sentinel (view 8): GI lane state only; green pulse proves a packed-
// reservoir store/load round-trip through the INIT page; red = ABI failure.
float3 PathTraceCleanRestirGiSentinelColor(uint2 pixel)
{
    RTXDI_GIReservoir empty = RTXDI_EmptyGIReservoir();
    RAB_StoreGIReservoir(empty, int2(pixel), int(RemixRAB_GetGIInitSampleReservoirIndex()));
    RTXDI_GIReservoir loaded = RAB_LoadGIReservoir(int2(pixel), int(RemixRAB_GetGIInitSampleReservoirIndex()));
    const bool roundTripOk = !RTXDI_IsValidGIReservoir(loaded) && loaded.M == 0u && loaded.weightSum == 0.0;

    const uint band = ((pixel.x + pixel.y + CleanRestirGiFrameIndex) / 32u) & 1u;
    const float pulse = band != 0u ? 0.85 : 0.25;
    return roundTripOk ? float3(0.05, pulse, 0.25) : float3(1.0, 0.0, 0.0);
}

bool CleanGiSeedPassSkipsView(uint view)
{
    return view == 1u || view == 2u || view == 9u || view == 10u || view == 21u || view == 22u;
}

#if defined(CLEAN_RESTIR_GI_PRODUCER_RAYQUERY_CS)
[numthreads(16, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    uint producerWidth = 0u;
    uint producerHeight = 0u;
    CleanRestirGiProducerHitPosition.GetDimensions(producerWidth, producerHeight);
    const uint2 dimensions = uint2(producerWidth, producerHeight);
    if (dimensions.x == 0u || dimensions.y == 0u || pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }
    const uint flatIndex = pixel.y * dimensions.x + pixel.x;

    PathTracePrimarySurfaceRecord record;
    const bool surfaceValid = CleanGiLoadSurfaceRecord(pixel, dimensions, record);

    CleanGiProducerSurface gbuf = (CleanGiProducerSurface)0;
    float3 hitPosition = float3(0.0, 0.0, 0.0);
    float3 hitNormal = float3(0.0, 0.0, 0.0);
    uint traceStatus = CLEAN_GI_PRODUCER_TRACE_STATUS_PRIMARY_INVALID;
    if (surfaceValid)
    {
        RTXDI_RandomSamplerState rng = CleanGiInitProducerRandomSampler(pixel, CleanRestirGiFrameIndex, CLEAN_RESTIR_GI_PRODUCER_RNG_PASS);
        const RAB_Surface surface = CleanGiMaterialSurfaceFromCurrentRecord(pixel, record);
        const float3 primaryGeometricNormal = CleanGiSafeNormalize(RAB_GetSurfaceGeoNormal(surface), float3(0.0, 0.0, 1.0));
        const float3 primaryShadingNormal = CleanGiConstrainShadingNormal(
            RAB_GetSurfaceNormal(surface),
            primaryGeometricNormal);
        const CleanGiFirstIndirectRaySample raySample = CleanGiSampleProducerFirstIndirectRay(
            surface,
            primaryShadingNormal,
            primaryGeometricNormal,
            rng);

        RAB_Surface secondarySurface;
        if (CleanGiBuildProducerSurfaceRayQuery(
            pixel,
            dimensions,
            RAB_GetSurfaceWorldPos(surface),
            primaryGeometricNormal,
            raySample.direction,
            raySample.sourcePdf,
            secondarySurface,
            traceStatus))
        {
            gbuf = CleanGiPackProducerSurface(secondarySurface, raySample);
            hitPosition = secondarySurface.worldPos;
            hitNormal = secondarySurface.shadingNormal;
        }
    }

    CleanGiProducerSurfaceBuffer[flatIndex] = gbuf;
    CleanGiStoreFirstIndirectTraceCandidateForRawGiSample(pixel, gbuf, hitPosition, hitNormal, (float)traceStatus);
}
#else

// Clean-slate baseline producer: one TraceRay dispatch writes the raw GI
// initial sample directly. This intentionally bypasses the trace/shade split,
// ray-query path, NEE-cache secondary path, and continuation bounce so Nsight
// can compare a stripped producer shape against the existing staged producer.
[shader("raygeneration")]
void FirstIndirectSimpleRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dimensions = DispatchRaysDimensions().xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }
    const uint flatIndex = pixel.y * dimensions.x + pixel.x;

    PathTracePrimarySurfaceRecord record;
    const bool surfaceValid = CleanGiLoadSurfaceRecord(pixel, dimensions, record);

    CleanGiProducerSurface gbuf = (CleanGiProducerSurface)0;
    CleanGiProducerResult producer = (CleanGiProducerResult)0;
    float3 hitPosition = float3(0.0, 0.0, 0.0);
    float3 hitNormal = float3(0.0, 0.0, 0.0);

    if (surfaceValid)
    {
        RTXDI_RandomSamplerState rng = CleanGiInitProducerRandomSampler(pixel, CleanRestirGiFrameIndex, CLEAN_RESTIR_GI_PRODUCER_RNG_PASS);
        const RAB_Surface surface = CleanGiMaterialSurfaceFromCurrentRecord(pixel, record);
        const float3 primaryGeometricNormal = CleanGiSafeNormalize(RAB_GetSurfaceGeoNormal(surface), float3(0.0, 0.0, 1.0));
        const float3 primaryShadingNormal = CleanGiConstrainShadingNormal(
            RAB_GetSurfaceNormal(surface),
            primaryGeometricNormal);
        const CleanGiFirstIndirectRaySample raySample = CleanGiSampleProducerFirstIndirectRay(
            surface,
            primaryShadingNormal,
            primaryGeometricNormal,
            rng);

        RAB_Surface secondarySurface;
        if (CleanGiBuildFirstIndirectTraceCandidate(
            RAB_GetSurfaceWorldPos(surface),
            primaryGeometricNormal,
            raySample,
            secondarySurface,
            gbuf,
            hitPosition,
            hitNormal))
        {
            producer = CleanGiShadeFirstIndirectSurface(
                secondarySurface,
                PathTraceFirstIndirectCandidateRaySampleIsSpecular(raySample),
                raySample.sourcePdf,
                CLEAN_GI_FIRST_INDIRECT_SHADE_DEFAULT_ONE_SAMPLE,
                rng);
        }
    }

    CleanGiProducerSurfaceBuffer[flatIndex] = gbuf;
    CleanGiStoreShadedFirstIndirectCandidateForRawGiSample(pixel, producer);
    CleanGiStoreFirstIndirectTraceCandidateForRawGiSample(pixel, gbuf, hitPosition, hitNormal, 0.0);
}

// Lean split baseline, pass A: keep the simple producer's constrained primary
// normal sampling but only write the secondary surface and hit geometry.
[shader("raygeneration")]
void FirstIndirectLeanTraceRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dimensions = DispatchRaysDimensions().xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }
    const uint flatIndex = pixel.y * dimensions.x + pixel.x;

    PathTracePrimarySurfaceRecord record;
    const bool surfaceValid = CleanGiLoadSurfaceRecord(pixel, dimensions, record);

    CleanGiProducerSurface gbuf = (CleanGiProducerSurface)0;
    float3 hitPosition = float3(0.0, 0.0, 0.0);
    float3 hitNormal = float3(0.0, 0.0, 0.0);

    if (surfaceValid)
    {
        RTXDI_RandomSamplerState rng = CleanGiInitProducerRandomSampler(pixel, CleanRestirGiFrameIndex, CLEAN_RESTIR_GI_PRODUCER_RNG_PASS);
        const RAB_Surface surface = CleanGiMaterialSurfaceFromCurrentRecord(pixel, record);
        const float3 primaryGeometricNormal = CleanGiSafeNormalize(RAB_GetSurfaceGeoNormal(surface), float3(0.0, 0.0, 1.0));
        const float3 primaryShadingNormal = CleanGiConstrainShadingNormal(
            RAB_GetSurfaceNormal(surface),
            primaryGeometricNormal);
        const CleanGiFirstIndirectRaySample raySample = CleanGiSampleProducerFirstIndirectRay(
            surface,
            primaryShadingNormal,
            primaryGeometricNormal,
            rng);

        RAB_Surface secondarySurface;
        CleanGiBuildFirstIndirectTraceCandidate(
            RAB_GetSurfaceWorldPos(surface),
            primaryGeometricNormal,
            raySample,
            secondarySurface,
            gbuf,
            hitPosition,
            hitNormal);
    }

    CleanGiProducerSurfaceBuffer[flatIndex] = gbuf;
    CleanGiStoreFirstIndirectTraceCandidateForRawGiSample(pixel, gbuf, hitPosition, hitNormal, 0.0);
}

// Lean split baseline, pass B: consume the trace G-buffer and run only the
// stripped one-sample secondary shade used by the simple producer.
[shader("raygeneration")]
void FirstIndirectLeanShadeRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dimensions = DispatchRaysDimensions().xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }
    const uint flatIndex = pixel.y * dimensions.x + pixel.x;

    const CleanGiProducerSurface gbuf = CleanGiProducerSurfaceBuffer[flatIndex];

    CleanGiProducerResult producer = (CleanGiProducerResult)0;
    if (gbuf.valid != 0u)
    {
        RTXDI_RandomSamplerState rng = CleanGiInitProducerRandomSampler(pixel, CleanRestirGiFrameIndex, CLEAN_RESTIR_GI_PRODUCER_RNG_PASS);
        producer = CleanGiShadeFirstIndirectTraceCandidate(
            gbuf,
            CLEAN_GI_FIRST_INDIRECT_SHADE_DEFAULT_ONE_SAMPLE,
            true,
            rng);
    }

    CleanGiStoreShadedFirstIndirectCandidateForRawGiSample(pixel, producer);
}

// Pass A of the producer trace/shade split: trace the indirect bounce, rebuild
// the secondary surface, and stash it in CleanGiProducerSurfaceBuffer. No
// direct lighting / shadow rays here, so this entry point stays narrow.
[shader("raygeneration")]
void FirstIndirectTraceRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dimensions = DispatchRaysDimensions().xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }
    const uint flatIndex = pixel.y * dimensions.x + pixel.x;

    PathTracePrimarySurfaceRecord record;
    const bool surfaceValid = CleanGiLoadSurfaceRecord(pixel, dimensions, record);

    CleanGiProducerSurface gbuf = (CleanGiProducerSurface)0;
    float3 hitPosition = float3(0.0, 0.0, 0.0);
    float3 hitNormal = float3(0.0, 0.0, 0.0);
    if (surfaceValid)
    {
        // Consume exactly the producer-ray randoms. The shade pass re-seeds the
        // same stream and skips them, so NEE samples match the pre-split
        // monolithic producer bit-for-bit for the active producer mode.
        RTXDI_RandomSamplerState rng = CleanGiInitProducerRandomSampler(pixel, CleanRestirGiFrameIndex, CLEAN_RESTIR_GI_PRODUCER_RNG_PASS);
        const RAB_Surface surface = CleanGiMaterialSurfaceFromCurrentRecord(pixel, record);
        const float3 primaryShadingNormal = CleanGiSafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
        const float3 primaryGeometricNormal = CleanGiSafeNormalize(RAB_GetSurfaceGeoNormal(surface), primaryShadingNormal);
        const CleanGiFirstIndirectRaySample raySample = CleanGiSampleProducerFirstIndirectRay(
            surface,
            primaryShadingNormal,
            primaryGeometricNormal,
            rng);

        RAB_Surface secondarySurface;
        CleanGiBuildFirstIndirectTraceCandidate(
            RAB_GetSurfaceWorldPos(surface),
            primaryGeometricNormal,
            raySample,
            secondarySurface,
            gbuf,
            hitPosition,
            hitNormal);
    }

    CleanGiProducerSurfaceBuffer[flatIndex] = gbuf;
    // Hit geometry is consumed by the reuse pass; producer radiance is written
    // by the shade pass that runs next.
    CleanGiStoreFirstIndirectTraceCandidateForRawGiSample(pixel, gbuf, hitPosition, hitNormal, 0.0);
}

// Correctness fallback for the inline ray-query producer experiment: the
// ray-query path currently covers glossy/specular-eligible surfaces correctly,
// but rough diffuse surfaces need the known-good trace path until the pure
// query producer is fixed.
[shader("raygeneration")]
void FirstIndirectTraceRoughFallbackRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dimensions = DispatchRaysDimensions().xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    PathTracePrimarySurfaceRecord record;
    const bool surfaceValid = CleanGiLoadSurfaceRecord(pixel, dimensions, record);
    const uint view = CleanRestirGiView;
    if (!surfaceValid)
    {
        if (view == 22u)
        {
            SmokeOutput[pixel] = float4(0.08, 0.08, 0.08, 1.0);
        }
        return;
    }

    RTXDI_RandomSamplerState rng = CleanGiInitProducerRandomSampler(pixel, CleanRestirGiFrameIndex, CLEAN_RESTIR_GI_PRODUCER_RNG_PASS);
    const RAB_Surface surface = CleanGiMaterialSurfaceFromCurrentRecord(pixel, record);
    const bool specularEligible = CleanGiSurfaceSupportsSpecularProducer(surface);
    if (specularEligible)
    {
        if (view == 22u)
        {
            // Blue: this pixel is intentionally outside the rough fallback set.
            SmokeOutput[pixel] = float4(0.02, 0.18, 0.85, 1.0);
        }
        return;
    }

    const uint flatIndex = pixel.y * dimensions.x + pixel.x;
    const CleanGiProducerSurface queryGbuf = CleanGiProducerSurfaceBuffer[flatIndex];
    CleanGiProducerSurface gbuf = (CleanGiProducerSurface)0;
    float3 hitPosition = float3(0.0, 0.0, 0.0);
    float3 hitNormal = float3(0.0, 0.0, 0.0);

    const float3 primaryShadingNormal = CleanGiSafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    const float3 primaryGeometricNormal = CleanGiSafeNormalize(RAB_GetSurfaceGeoNormal(surface), primaryShadingNormal);
    const CleanGiFirstIndirectRaySample raySample = CleanGiSampleProducerFirstIndirectRay(
        surface,
        primaryShadingNormal,
        primaryGeometricNormal,
        rng);

    RAB_Surface secondarySurface;
    const bool traceValid = CleanGiBuildFirstIndirectTraceCandidate(
        RAB_GetSurfaceWorldPos(surface),
        primaryGeometricNormal,
        raySample,
        secondarySurface,
        gbuf,
        hitPosition,
        hitNormal);
    if (view == 22u)
    {
        const bool queryValid = queryGbuf.valid != 0u;
        float3 color = float3(0.0, 1.0, 0.0);

        if (!queryValid && !traceValid)
        {
            color = float3(0.0, 0.0, 0.0);
        }
        else if (!queryValid && traceValid)
        {
            color = float3(1.0, 0.0, 0.0);
        }
        else if (queryValid && !traceValid)
        {
            color = float3(0.0, 0.85, 1.0);
        }
        else
        {
            const float3 queryGeo = CleanGiSafeNormalize(queryGbuf.geometryNormal, float3(0.0, 0.0, 1.0));
            const float3 queryShade = CleanGiSafeNormalize(queryGbuf.shadingNormal, queryGeo);
            const float3 queryView = CleanGiSafeNormalize(queryGbuf.viewDir, queryGeo);
            const float3 traceGeo = CleanGiSafeNormalize(secondarySurface.geometryNormal, float3(0.0, 0.0, 1.0));
            const float3 traceShade = CleanGiSafeNormalize(secondarySurface.shadingNormal, traceGeo);
            const float3 traceView = CleanGiSafeNormalize(secondarySurface.viewDir, traceGeo);
            const bool queryGeoFacesView = dot(queryGeo, queryView) > 0.0;
            const bool queryShadeFacesView = dot(queryShade, queryView) > 0.0;
            const bool traceGeoFacesView = dot(traceGeo, traceView) > 0.0;
            const bool traceShadeFacesView = dot(traceShade, traceView) > 0.0;
            const float hitTolerance = max(0.05, 0.02 * max(abs(secondarySurface.linearDepth), 1.0));
            const float positionTolerance = max(0.25, 0.04 * max(abs(secondarySurface.linearDepth), 1.0));

            const bool instanceMismatch = queryGbuf.instanceId != secondarySurface.instanceId;
            const bool primitiveMismatch = queryGbuf.primitiveIndex != secondarySurface.primitiveIndex;
            const bool materialMismatch =
                queryGbuf.materialIndex != secondarySurface.materialIndex ||
                queryGbuf.materialId != secondarySurface.materialId;
            const bool positionMismatch =
                abs(queryGbuf.linearDepth - secondarySurface.linearDepth) > hitTolerance ||
                length(queryGbuf.worldPos - secondarySurface.worldPos) > positionTolerance;
            if ((instanceMismatch || primitiveMismatch) && !positionMismatch && !materialMismatch)
            {
                color = float3(0.95, 1.0, 0.95);
            }
            else if (instanceMismatch && primitiveMismatch)
            {
                color = float3(1.0, 1.0, 0.0);
            }
            else if (instanceMismatch)
            {
                color = float3(1.0, 0.92, 0.0);
            }
            else if (primitiveMismatch)
            {
                color = float3(0.55, 1.0, 0.0);
            }
            else if (materialMismatch)
            {
                color = float3(1.0, 0.55, 0.0);
            }
            else if (!queryGeoFacesView && traceGeoFacesView)
            {
                color = float3(0.0, 0.0, 0.55);
            }
            else if (!queryShadeFacesView && traceShadeFacesView)
            {
                color = float3(0.55, 0.0, 0.85);
            }
            else if (positionMismatch)
            {
                color = float3(1.0, 0.25, 0.0);
            }
            else if (dot(queryGeo, traceGeo) < 0.9 || dot(queryShade, traceShade) < 0.85)
            {
                color = float3(0.45, 0.0, 1.0);
            }
            else
            {
                RAB_Surface querySurface = CleanGiUnpackProducerSurface(queryGbuf);
                RTXDI_RandomSamplerState queryRng = CleanGiInitProducerRandomSampler(pixel, CleanRestirGiFrameIndex, CLEAN_RESTIR_GI_PRODUCER_RNG_PASS);
                RTXDI_RandomSamplerState traceRng = CleanGiInitProducerRandomSampler(pixel, CleanRestirGiFrameIndex, CLEAN_RESTIR_GI_PRODUCER_RNG_PASS);
                RAB_GetNextRandom(queryRng);
                RAB_GetNextRandom(queryRng);
                RAB_GetNextRandom(traceRng);
                RAB_GetNextRandom(traceRng);
                const float3 queryRadiance = CleanGiShadeProducerSurface(querySurface, queryGbuf.primarySampledSpecular != 0u, queryRng);
                const float3 traceRadiance = CleanGiShadeProducerSurface(secondarySurface, false, traceRng);
                const float queryLuminance = CleanGiAllFinite3(queryRadiance) ? CleanGiLuminance(queryRadiance) : -1.0;
                const float traceLuminance = CleanGiAllFinite3(traceRadiance) ? CleanGiLuminance(traceRadiance) : -1.0;
                const float luminanceScale = max(max(abs(queryLuminance), abs(traceLuminance)), 1.0e-3);
                if (queryLuminance < 0.0 || traceLuminance < 0.0)
                {
                    color = float3(1.0, 1.0, 1.0);
                }
                else if (abs(queryLuminance - traceLuminance) / luminanceScale > 0.75 &&
                    max(queryLuminance, traceLuminance) > 1.0e-4)
                {
                    color = float3(1.0, 0.0, 1.0);
                }
            }
        }

        SmokeOutput[pixel] = float4(color, 1.0);
        return;
    }

    CleanGiProducerSurfaceBuffer[flatIndex] = gbuf;
    CleanGiStoreFirstIndirectTraceCandidateForRawGiSample(pixel, gbuf, hitPosition, hitNormal, 0.0);
}

// Pass B of the producer trace/shade split: load the surface produced by the
// trace pass and run the divergent direct-NEE (the 4-way light sampling +
// shadow rays). Isolated from the bounce-trace/geometry machinery.
[shader("raygeneration")]
void FirstIndirectShadeRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dimensions = DispatchRaysDimensions().xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }
    const uint flatIndex = pixel.y * dimensions.x + pixel.x;

    if (CleanRestirGiView == 22u)
    {
        return;
    }

    const CleanGiProducerSurface gbuf = CleanGiProducerSurfaceBuffer[flatIndex];

    CleanGiProducerResult producer = (CleanGiProducerResult)0;
    if (gbuf.valid != 0u)
    {
        RTXDI_RandomSamplerState rng = CleanGiInitProducerRandomSampler(pixel, CleanRestirGiFrameIndex, CLEAN_RESTIR_GI_PRODUCER_RNG_PASS);
        producer = CleanGiShadeFirstIndirectTraceCandidate(
            gbuf,
            CLEAN_GI_FIRST_INDIRECT_SHADE_FULL_NEE,
            true,
            rng);
    }

    CleanGiStoreShadedFirstIndirectCandidateForRawGiSample(pixel, producer);

    const uint view = CleanRestirGiView;
    if (view != 1u && view != 2u && view != 9u && view != 10u && view != 21u)
    {
        return;
    }

    PathTracePrimarySurfaceRecord record;
    const bool surfaceValid = CleanGiLoadSurfaceRecord(pixel, dimensions, record);

    float3 color = float3(1.0, 0.0, 1.0);
    if (view == 1u)
    {
        if (!surfaceValid)
        {
            color = float3(0.08, 0.08, 0.08);
        }
        else if (producer.valid == 0u)
        {
            color = float3(0.0, 0.0, 0.0);
        }
        else if (!CleanGiAllFinite3(producer.radiance))
        {
            color = float3(1.0, 1.0, 0.0);
        }
        else
        {
            color = CleanGiToneMap(producer.radiance);
        }
    }
    else if (view == 2u)
    {
        if (!surfaceValid)
        {
            color = float3(0.08, 0.08, 0.08);
        }
        else if (producer.valid == 0u)
        {
            color = float3(0.25, 0.0, 0.0);
        }
        else
        {
            const uint band = ((pixel.x / 64u) + (pixel.y / 64u)) & 1u;
            color = band != 0u
                ? saturate(producer.hitNormal * 0.5 + 0.5)
                : frac(producer.hitPosition / 128.0);
        }
    }
    else if (view == 9u)
    {
        if (!surfaceValid)
        {
            color = float3(0.08, 0.08, 0.08);
        }
        else if (producer.valid == 0u)
        {
            color = float3(0.25, 0.0, 0.0);
        }
        else
        {
            color = saturate(producer.materialAlbedo);
        }
    }
    else if (view == 10u)
    {
        if (!surfaceValid)
        {
            color = float3(0.08, 0.08, 0.08);
        }
        else if (producer.valid == 0u)
        {
            color = float3(0.25, 0.0, 0.0);
        }
        else
        {
            // diffuseTextureIndex / smoke-material flags are not in the surface
            // G-buffer; re-fetch the smoke material for this debug view only.
            const PathTraceSmokeMaterial sm = CleanGiLoadSmokeMaterial(gbuf.materialIndex);
            const bool hasDiffuseTexture = sm.diffuseTextureIndex != 0xffffffffu;
            const bool forceDebugAlbedo = (sm.flags & RT_SMOKE_MATERIAL_FORCE_DEBUG_ALBEDO) != 0u;
            color = float3(hasDiffuseTexture ? 0.0 : 1.0, hasDiffuseTexture ? 1.0 : 0.0, forceDebugAlbedo ? 1.0 : 0.0);
        }
    }
    else
    {
        if (!surfaceValid)
        {
            color = float3(0.08, 0.08, 0.08);
        }
        else if (gbuf.valid == 0u)
        {
            color = float3(0.25, 0.0, 0.0);
        }
        else
        {
            RAB_Surface secondarySurface = CleanGiUnpackProducerSurface(gbuf);
            RTXDI_RandomSamplerState debugRng = CleanGiInitProducerRandomSampler(
                pixel,
                CleanRestirGiFrameIndex,
                CLEAN_RESTIR_GI_PRODUCER_RNG_PASS);
            CleanGiSkipFirstIndirectCandidateRaySampleRandoms(gbuf, debugRng);
            color = CleanGiProducerShadeGateDebugColor(
                secondarySurface,
                gbuf.primarySampledSpecular != 0u,
                debugRng);
        }
    }

    SmokeOutput[pixel] = float4(color, 1.0);
}

[shader("raygeneration")]
void FirstIndirectShadeFastRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dimensions = DispatchRaysDimensions().xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }
    const uint flatIndex = pixel.y * dimensions.x + pixel.x;

    if (CleanRestirGiView == 22u)
    {
        return;
    }

    const CleanGiProducerSurface gbuf = CleanGiProducerSurfaceBuffer[flatIndex];

    CleanGiProducerResult producer = (CleanGiProducerResult)0;
    if (gbuf.valid != 0u)
    {
        RTXDI_RandomSamplerState rng = CleanGiInitProducerRandomSampler(pixel, CleanRestirGiFrameIndex, CLEAN_RESTIR_GI_PRODUCER_RNG_PASS);
        producer = CleanGiShadeFirstIndirectTraceCandidate(
            gbuf,
            CLEAN_GI_FIRST_INDIRECT_SHADE_DEFAULT_ONE_SAMPLE,
            true,
            rng);
    }

    CleanGiStoreShadedFirstIndirectCandidateForRawGiSample(pixel, producer);
}

// INIT-page seed pass: clears the transient INIT reservoir page and merges the
// specular-producer and NEE-cache seeds into it. Extracted from the temporal
// pass because CleanGiSeedInitPageFromSpecularProducer runs a full specular
// bounce trace + secondary NEE shade -- heavy, divergent producer work that was
// broadening the temporal entry point. Always dispatched (it owns the INIT
// clear the temporal contract depends on); the expensive seeds self-gate on
// their cvars. Runs after the diffuse producer, before the temporal contract.
[shader("raygeneration")]
void SeedNoSpecRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dimensions = DispatchRaysDimensions().xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    const uint view = CleanRestirGiView;
    if (CleanGiSeedPassSkipsView(view))
    {
        return;
    }

    PathTracePrimarySurfaceRecord record;
    const bool surfaceValid = CleanGiLoadSurfaceRecord(pixel, dimensions, record);

    RAB_StoreGIReservoir(RTXDI_EmptyGIReservoir(), int2(pixel), int(RemixRAB_GetGIInitSampleReservoirIndex()));
    CleanGiSeedInitPageFromNeeCache(pixel, surfaceValid, record);
}

[shader("raygeneration")]
void FirstIndirectSpecularTraceRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dimensions = DispatchRaysDimensions().xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }
    const uint flatIndex = pixel.y * dimensions.x + pixel.x;

    CleanGiProducerSurface gbuf = (CleanGiProducerSurface)0;
    const uint view = CleanRestirGiView;
    if (!CleanGiSeedPassSkipsView(view) && CleanGiSpecularSeedProducerActive())
    {
        PathTracePrimarySurfaceRecord record;
        const bool surfaceValid = CleanGiLoadSurfaceRecord(pixel, dimensions, record);
        if (surfaceValid)
        {
            RTXDI_RandomSamplerState rng = CleanGiInitProducerRandomSampler(pixel, CleanRestirGiFrameIndex, CLEAN_RESTIR_GI_SPECULAR_PRODUCER_RNG_PASS);
            const RAB_Surface surface = CleanGiMaterialSurfaceFromCurrentRecord(pixel, record);
            const CleanGiFirstIndirectRaySample raySample = CleanGiSampleSpecularFirstIndirectRay(surface, rng);
            if (PathTraceFirstIndirectCandidateRaySampleIsValid(raySample))
            {
                RAB_Surface secondarySurface;
                float3 hitPosition;
                float3 hitNormal;
                CleanGiBuildFirstIndirectTraceCandidate(
                    RAB_GetSurfaceWorldPos(surface),
                    RAB_GetSurfaceGeoNormal(surface),
                    raySample,
                    secondarySurface,
                    gbuf,
                    hitPosition,
                    hitNormal);
            }
        }
    }

    CleanGiProducerSurfaceBuffer[flatIndex] = gbuf;
}

[shader("raygeneration")]
void FirstIndirectSpecularShadeRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dimensions = DispatchRaysDimensions().xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    const uint view = CleanRestirGiView;
    if (CleanGiSeedPassSkipsView(view) || !CleanGiSpecularSeedProducerActive())
    {
        return;
    }

    const uint flatIndex = pixel.y * dimensions.x + pixel.x;
    const CleanGiProducerSurface gbuf = CleanGiProducerSurfaceBuffer[flatIndex];
    if (gbuf.valid == 0u)
    {
        return;
    }

    PathTracePrimarySurfaceRecord record;
    const bool surfaceValid = CleanGiLoadSurfaceRecord(pixel, dimensions, record);
    if (!surfaceValid)
    {
        return;
    }

    const CleanGiSpecularSeedReceiverSurface receiver = CleanGiBuildSpecularSeedReceiverSurface(pixel, record);
    if (receiver.valid == 0u)
    {
        return;
    }

    RTXDI_RandomSamplerState rng = CleanGiInitProducerRandomSampler(pixel, CleanRestirGiFrameIndex, CLEAN_RESTIR_GI_SPECULAR_PRODUCER_RNG_PASS);

    CleanGiProducerResult producer = CleanGiShadeFirstIndirectTraceCandidate(
        gbuf,
        CLEAN_GI_FIRST_INDIRECT_SHADE_FULL_NEE,
        true,
        rng);
    CleanGiMergePackedSpecularSeedIntoInitPage(pixel, receiver, producer, RAB_GetNextRandom(rng));
}

[shader("raygeneration")]
void FirstIndirectSpecularShadeFastRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dimensions = DispatchRaysDimensions().xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    const uint view = CleanRestirGiView;
    if (CleanGiSeedPassSkipsView(view) || !CleanGiSpecularSeedProducerActive())
    {
        return;
    }

    const uint flatIndex = pixel.y * dimensions.x + pixel.x;
    const CleanGiProducerSurface gbuf = CleanGiProducerSurfaceBuffer[flatIndex];
    if (gbuf.valid == 0u)
    {
        return;
    }

    PathTracePrimarySurfaceRecord record;
    const bool surfaceValid = CleanGiLoadSurfaceRecord(pixel, dimensions, record);
    if (!surfaceValid)
    {
        return;
    }

    const CleanGiSpecularSeedReceiverSurface receiver = CleanGiBuildSpecularSeedReceiverSurface(pixel, record);
    if (receiver.valid == 0u)
    {
        return;
    }

    RTXDI_RandomSamplerState rng = CleanGiInitProducerRandomSampler(pixel, CleanRestirGiFrameIndex, CLEAN_RESTIR_GI_SPECULAR_PRODUCER_RNG_PASS);

    CleanGiProducerResult producer = CleanGiShadeFirstIndirectTraceCandidate(
        gbuf,
        CLEAN_GI_FIRST_INDIRECT_SHADE_DEFAULT_ONE_SAMPLE,
        true,
        rng);
    CleanGiMergePackedSpecularSeedIntoInitPage(pixel, receiver, producer, RAB_GetNextRandom(rng));
}

[shader("raygeneration")]
void SeedRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dimensions = DispatchRaysDimensions().xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    const uint view = CleanRestirGiView;
    if (CleanGiSeedPassSkipsView(view))
    {
        return;
    }

    PathTracePrimarySurfaceRecord record;
    const bool surfaceValid = CleanGiLoadSurfaceRecord(pixel, dimensions, record);

    RAB_StoreGIReservoir(RTXDI_EmptyGIReservoir(), int2(pixel), int(RemixRAB_GetGIInitSampleReservoirIndex()));
    CleanGiSeedInitPageFromSpecularProducer(pixel, surfaceValid, record);
    CleanGiSeedInitPageFromGlossySecondRay(pixel, surfaceValid, record);
    CleanGiSeedInitPageFromNeeCache(pixel, surfaceValid, record);
}

[shader("raygeneration")]
void ReuseRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dimensions = DispatchRaysDimensions().xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    const uint view = CleanRestirGiView;
    if (view == 1u || view == 2u || view == 9u || view == 10u || view == 21u || view == 22u)
    {
        return;
    }

    // ---- Phase 1: spatial reuse (separate dispatch; every pixel's
    // TEMPORAL_OUTPUT write from phase 0 has completed) ----
    if (CleanRestirGiPhase == 1u)
    {
        PathTracePrimarySurfaceRecord spatialRecord;
        const bool spatialSurfaceValid = CleanGiLoadSurfaceRecord(pixel, dimensions, spatialRecord);
        RAB_Surface spatialSurface = RAB_EmptySurface();
        if (spatialSurfaceValid)
        {
            spatialSurface = CleanGiMaterialSurfaceFromCurrentRecord(pixel, spatialRecord);
        }

        RTXDI_GIReservoir spatialInput = RAB_LoadGIReservoir(
            int2(pixel), int(RemixRAB_GetGITemporalOutputReservoirIndex()));
        RTXDI_GIReservoir spatialReservoir = spatialInput;
        if (CleanRestirGiSpatialEnabled != 0u)
        {
            spatialReservoir = CleanGiRunSpatialReuse(pixel, spatialSurface, spatialInput);
        }
        RAB_StoreGIReservoir(spatialReservoir, int2(pixel), int(RemixRAB_GetGISpatialOutputReservoirIndex()));

        // RGI-07: final shading + optional resolve from the spatial output.
        CleanGiFinalShadingAndResolve(pixel, spatialSurface, spatialReservoir);

        if (view == 6u)
        {
            // Isolated indirect GI (diffuse + specular, no DI).
            const float3 indirect = CleanGiFinalShadeIndirect(spatialSurface, spatialReservoir);
            SmokeOutput[pixel] = float4(spatialSurfaceValid ? CleanGiToneMap(indirect) : float3(0.08, 0.08, 0.08), 1.0);
        }
        else if (view == 17u)
        {
            SmokeOutput[pixel] = float4(
                spatialSurfaceValid ? CleanGiSpecularProducerEligibilityColor(spatialSurface) : float3(0.08, 0.08, 0.08),
                1.0);
        }
        else if (view == 18u)
        {
            SmokeOutput[pixel] = float4(
                spatialSurfaceValid ? CleanGiSpecularReuseStateColor(spatialSurface, spatialReservoir) : float3(0.08, 0.08, 0.08),
                1.0);
        }
        else if (view == 19u)
        {
            SmokeOutput[pixel] = float4(CleanGiStoredSpecularOutputColor(pixel), 1.0);
        }
        else if (view == 11u || view == 12u || view == 16u)
        {
            const CleanGiIndirectLobeResult lobes = CleanGiFinalShadeIndirectSplit(spatialSurface, spatialReservoir);
            const float3 lobe = view == 11u ? lobes.diffuse : lobes.specular;
            const float3 distanceBands = saturate(float3(lobes.hitDistance / 128.0, lobes.hitDistance / 512.0, lobes.hitDistance / 2048.0));
            SmokeOutput[pixel] = float4(
                spatialSurfaceValid ? (view == 16u ? distanceBands : CleanGiToneMap(lobe)) : float3(0.08, 0.08, 0.08),
                1.0);
        }
        else if (view == 5u)
        {
            float3 spatialColor = float3(1.0, 0.0, 1.0);
            if (!spatialSurfaceValid)
            {
                spatialColor = float3(0.08, 0.08, 0.08);
            }
            else if (!RTXDI_IsValidGIReservoir(spatialReservoir))
            {
                spatialColor = float3(0.0, 0.0, 0.0);
            }
            else if (!CleanGiAllFinite3(spatialReservoir.radiance) ||
                spatialReservoir.weightSum != spatialReservoir.weightSum)
            {
                spatialColor = float3(1.0, 1.0, 0.0);
            }
            else
            {
                spatialColor = CleanGiToneMap(spatialReservoir.radiance * max(spatialReservoir.weightSum, 0.0));
            }
            SmokeOutput[pixel] = float4(spatialColor, 1.0);
        }
        return;
    }

    // ---- Reuse stage: consumes producer textures written by ProducerRayGen ----
    PathTracePrimarySurfaceRecord record;
    const bool surfaceValid = CleanGiLoadSurfaceRecord(pixel, dimensions, record);
    CleanGiProducerResult producer = (CleanGiProducerResult)0;

    // INIT-page seeding (clear + specular/NEE producer seeds) now runs in the
    // separate SeedRayGen pass so the heavy specular producer trace+shade no
    // longer broadens this temporal entry point. The INIT page is already
    // populated when this dispatch runs.

    // ---- RGI-03/04 initial reservoir + temporal reuse (frozen contract) ----
    const RemixRestirGITemporalReuseResult temporalResult =
        CleanGiRunTemporalContract(pixel, dimensions, surfaceValid, record);
    const RTXDI_GIReservoir initialReservoir = temporalResult.initialReservoir;
    const RTXDI_GIReservoir temporalReservoir = temporalResult.temporalReservoir;

    // Spatial disabled: pass the temporal output through to the spatial page
    // here so the SPATIAL_OUTPUT page is always this frame's data and view 5
    // reproduces view 4 exactly. With spatial enabled, phase 1 writes it and
    // also owns final shading + resolve (RGI-07).
    if (CleanRestirGiSpatialEnabled == 0u)
    {
        RAB_StoreGIReservoir(temporalReservoir, int2(pixel), int(RemixRAB_GetGISpatialOutputReservoirIndex()));
        RAB_Surface resolveSurface = RAB_EmptySurface();
        if (surfaceValid)
        {
            resolveSurface = CleanGiMaterialSurfaceFromCurrentRecord(pixel, record);
        }
        CleanGiFinalShadingAndResolve(pixel, resolveSurface, temporalReservoir);
    }

    // ---- Debug views (GI lane resources only) ----
    if (view == 0u)
    {
        return;
    }

    float3 color = float3(1.0, 0.0, 1.0); // loud magenta: unimplemented view
    if (view == 1u)
    {
        // Producer radiance. Invalid primary surface = dark gray; miss =
        // black; NaN guard = loud yellow (validation matrix RGI-02).
        if (!surfaceValid)
        {
            color = float3(0.08, 0.08, 0.08);
        }
        else if (producer.valid == 0u)
        {
            color = float3(0.0, 0.0, 0.0);
        }
        else if (!CleanGiAllFinite3(producer.radiance))
        {
            color = float3(1.0, 1.0, 0.0);
        }
        else
        {
            color = CleanGiToneMap(producer.radiance);
        }
    }
    else if (view == 2u)
    {
        // Producer hit geometry bands: alternating world-position and normal
        // bands; invalid hit = dark red, invalid surface = dark gray.
        if (!surfaceValid)
        {
            color = float3(0.08, 0.08, 0.08);
        }
        else if (producer.valid == 0u)
        {
            color = float3(0.25, 0.0, 0.0);
        }
        else
        {
            const uint band = ((pixel.x / 64u) + (pixel.y / 64u)) & 1u;
            color = band != 0u
                ? saturate(producer.hitNormal * 0.5 + 0.5)
                : frac(producer.hitPosition / 128.0);
        }
    }
    else if (view == 3u)
    {
        // Initial-reservoir radiance after the single-sample RIS update.
        // This should match view 1 structurally; W only changes intensity.
        if (!surfaceValid)
        {
            color = float3(0.08, 0.08, 0.08);
        }
        else if (!RTXDI_IsValidGIReservoir(initialReservoir))
        {
            color = float3(0.0, 0.0, 0.0);
        }
        else if (!CleanGiAllFinite3(initialReservoir.radiance) ||
            initialReservoir.weightSum != initialReservoir.weightSum)
        {
            color = float3(1.0, 1.0, 0.0);
        }
        else
        {
            color = CleanGiToneMap(initialReservoir.radiance * max(initialReservoir.weightSum, 0.0));
        }
    }
    else if (view == 4u)
    {
        // Temporal output radiance * W. Static camera: converges over frames
        // vs view 3; temporal cvar 0 must reproduce view 3 exactly.
        if (!surfaceValid)
        {
            color = float3(0.08, 0.08, 0.08);
        }
        else if (!RTXDI_IsValidGIReservoir(temporalReservoir))
        {
            color = float3(0.0, 0.0, 0.0);
        }
        else if (!CleanGiAllFinite3(temporalReservoir.radiance) ||
            temporalReservoir.weightSum != temporalReservoir.weightSum)
        {
            color = float3(1.0, 1.0, 0.0);
        }
        else
        {
            color = CleanGiToneMap(temporalReservoir.radiance * max(temporalReservoir.weightSum, 0.0));
        }
    }
    else if (view == 5u)
    {
        if (CleanRestirGiSpatialEnabled != 0u)
        {
            // Phase 1 owns the view-5 output when spatial actually runs.
            return;
        }
        // Pass-through proof: identical to view 4 by construction.
        if (!surfaceValid)
        {
            color = float3(0.08, 0.08, 0.08);
        }
        else if (!RTXDI_IsValidGIReservoir(temporalReservoir))
        {
            color = float3(0.0, 0.0, 0.0);
        }
        else
        {
            color = CleanGiToneMap(temporalReservoir.radiance * max(temporalReservoir.weightSum, 0.0));
        }
    }
    else if (view == 6u)
    {
        if (CleanRestirGiSpatialEnabled != 0u)
        {
            // Phase 1 owns the view-6 output when spatial actually runs.
            return;
        }
        if (!surfaceValid)
        {
            color = float3(0.08, 0.08, 0.08);
        }
        else
        {
            RAB_Surface viewSurface = CleanGiMaterialSurfaceFromCurrentRecord(pixel, record);
            color = CleanGiToneMap(CleanGiFinalShadeIndirect(viewSurface, temporalReservoir));
        }
    }
    else if (view == 23u)
    {
        RAB_Surface viewSurface = RAB_EmptySurface();
        if (surfaceValid)
        {
            viewSurface = CleanGiMaterialSurfaceFromCurrentRecord(pixel, record);
        }
        color = CleanGiProducerReservoirPathColor(pixel, surfaceValid, viewSurface, initialReservoir, temporalReservoir);
    }
    else if (view == 24u)
    {
        color = !surfaceValid
            ? float3(0.08, 0.08, 0.08)
            : (CleanGiSurfaceRecordIsTransmissionPsrResolved(record)
                ? float3(0.0, 0.85, 0.18)
                : float3(0.02, 0.04, 0.10));
    }
    else if (view == 7u)
    {
        // Reservoir M / age diagnostics: green ramp = M / maxHistoryLength,
        // red ramp = age / maxReservoirAge, blue marks invalid reservoirs.
        if (!surfaceValid)
        {
            color = float3(0.08, 0.08, 0.08);
        }
        else if (!RTXDI_IsValidGIReservoir(temporalReservoir))
        {
            color = float3(0.0, 0.0, 0.35);
        }
        else
        {
            const float mRamp = saturate((float)temporalReservoir.M / max((float)CleanRestirGiMaxHistoryLength, 1.0));
            const float ageRamp = saturate((float)temporalReservoir.age / max((float)CleanRestirGiMaxReservoirAge, 1.0));
            color = float3(ageRamp, mRamp, 0.0);
        }
    }
    else if (view == 8u)
    {
        color = PathTraceCleanRestirGiSentinelColor(pixel);
    }
    else if (view == 9u)
    {
        // Secondary-hit material proof: sampled diffuse/classifier albedo.
        // Invalid primary surface = dark gray; bounce miss = dark red.
        if (!surfaceValid)
        {
            color = float3(0.08, 0.08, 0.08);
        }
        else if (producer.valid == 0u)
        {
            color = float3(0.25, 0.0, 0.0);
        }
        else
        {
            color = saturate(producer.materialAlbedo);
        }
    }
    else if (view == 10u)
    {
        // Secondary-hit material source: green = has diffuse texture slot,
        // red = diffuse fallback/debug albedo, blue = forced debug albedo flag.
        if (!surfaceValid)
        {
            color = float3(0.08, 0.08, 0.08);
        }
        else if (producer.valid == 0u)
        {
            color = float3(0.25, 0.0, 0.0);
        }
        else
        {
            const bool hasDiffuseTexture = producer.diffuseTextureIndex != 0xffffffffu;
            const bool forceDebugAlbedo = (producer.materialFlags & RT_SMOKE_MATERIAL_FORCE_DEBUG_ALBEDO) != 0u;
            color = float3(hasDiffuseTexture ? 0.0 : 1.0, hasDiffuseTexture ? 1.0 : 0.0, forceDebugAlbedo ? 1.0 : 0.0);
        }
    }
    else if (view == 17u)
    {
        if (CleanRestirGiSpatialEnabled != 0u)
        {
            return;
        }
        if (!surfaceValid)
        {
            color = float3(0.08, 0.08, 0.08);
        }
        else
        {
            RAB_Surface viewSurface = CleanGiMaterialSurfaceFromCurrentRecord(pixel, record);
            color = CleanGiSpecularProducerEligibilityColor(viewSurface);
        }
    }
    else if (view == 18u)
    {
        if (CleanRestirGiSpatialEnabled != 0u)
        {
            return;
        }
        if (!surfaceValid)
        {
            color = float3(0.08, 0.08, 0.08);
        }
        else
        {
            RAB_Surface viewSurface = CleanGiMaterialSurfaceFromCurrentRecord(pixel, record);
            color = CleanGiSpecularReuseStateColor(viewSurface, temporalReservoir);
        }
    }
    else if (view == 19u)
    {
        if (CleanRestirGiSpatialEnabled != 0u)
        {
            return;
        }
        color = CleanGiStoredSpecularOutputColor(pixel);
    }
    else if (view == 20u)
    {
        color = CleanGiNeeCacheProviderStateColor(surfaceValid, record);
    }
    else if (view == 11u || view == 12u || view == 16u)
    {
        if (CleanRestirGiSpatialEnabled != 0u)
        {
            // Phase 1 owns final-lobe/distance views when spatial actually runs.
            return;
        }
        if (!surfaceValid)
        {
            color = float3(0.08, 0.08, 0.08);
        }
        else
        {
            RAB_Surface viewSurface = CleanGiMaterialSurfaceFromCurrentRecord(pixel, record);
            const CleanGiIndirectLobeResult lobes = CleanGiFinalShadeIndirectSplit(viewSurface, temporalReservoir);
            color = view == 11u
                ? CleanGiToneMap(lobes.diffuse)
                : (view == 12u
                    ? CleanGiToneMap(lobes.specular)
                    : saturate(float3(lobes.hitDistance / 128.0, lobes.hitDistance / 512.0, lobes.hitDistance / 2048.0)));
        }
    }
    else if (view == 13u || view == 14u || view == 15u)
    {
        if (!surfaceValid)
        {
            color = float3(0.08, 0.08, 0.08);
        }
        else if (!CleanGiSpecularProducerActive())
        {
            color = float3(0.0, 0.0, 0.25);
        }
        else
        {
            RTXDI_RandomSamplerState specDebugRng = CleanGiInitProducerRandomSampler(pixel, CleanRestirGiFrameIndex, CLEAN_RESTIR_GI_SPECULAR_PRODUCER_RNG_PASS);
            const CleanGiSpecularProducerDebug specDebug = CleanGiBuildSpecularProducerDebug(pixel, record, specDebugRng);
            if (view == 13u)
            {
                if (specDebug.producer.valid == 0u)
                {
                    color = specDebug.sampledDirection != 0u ? float3(0.25, 0.0, 0.0) : float3(0.0, 0.0, 0.0);
                }
                else
                {
                    color = CleanGiToneMap(specDebug.producer.radiance);
                }
            }
            else if (view == 14u)
            {
                if (specDebug.producer.valid == 0u)
                {
                    color = specDebug.sampledDirection != 0u ? float3(0.25, 0.0, 0.0) : float3(0.0, 0.0, 0.0);
                }
                else
                {
                    const uint band = ((pixel.x / 32u) + (pixel.y / 32u)) & 1u;
                    color = band == 0u
                        ? saturate(abs(frac(specDebug.producer.hitPosition * 0.05)))
                        : saturate(specDebug.producer.hitNormal * 0.5 + 0.5);
                }
            }
            else
            {
                const float mixtureRamp = saturate(log2(max(specDebug.mixturePdf, 1.0e-8)) * (1.0 / 16.0) + 1.0);
                const float specRamp = saturate(log2(max(specDebug.specularPdf, 1.0e-8)) * (1.0 / 16.0) + 1.0);
                color = specDebug.sampledDirection != 0u
                    ? float3(saturate(specDebug.specularProbability), mixtureRamp, specRamp)
                    : float3(0.35, 0.0, 0.0);
            }
        }
    }
    SmokeOutput[pixel] = float4(color, 1.0);
}

[shader("miss")]
void Miss(inout PathTraceCleanRestirGiPayload payload)
{
    payload.value = 0u;
}

[shader("miss")]
void ShadowMiss(inout PathTraceCleanRestirGiPayload payload)
{
    payload.value = 0u;
}

[shader("anyhit")]
void AnyHit(inout PathTraceCleanRestirGiPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    if (CleanGiMaterialRejectsHit(DispatchRaysIndex().xy, InstanceID(), PrimitiveIndex(), attributes.barycentrics, false))
    {
        IgnoreHit();
    }
}

[shader("anyhit")]
void ShadowAnyHit(inout PathTraceCleanRestirGiPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    if (CleanGiMaterialRejectsHit(DispatchRaysIndex().xy, InstanceID(), PrimitiveIndex(), attributes.barycentrics, true))
    {
        IgnoreHit();
    }
}

[shader("closesthit")]
void ClosestHit(inout PathTraceCleanRestirGiPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    payload.value = 1u;
    payload.hitInstanceId = InstanceID();
    payload.hitPrimitiveIndex = PrimitiveIndex();
    payload.hitT = RayTCurrent();
    payload.hitBarycentrics = attributes.barycentrics;
}

[shader("closesthit")]
void ShadowClosestHit(inout PathTraceCleanRestirGiPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    payload.value = 1u;
}

#endif // CLEAN_RESTIR_GI_PRODUCER_RAYQUERY_CS
