#include "../../vulkan.hlsli"
#include "PathTraceMaterialFeatureTypes.hlsli"
#include "PathTraceSkinnedHitRoute.hlsli"
#include "cleanroom_common/pathtrace_liquid_pool_control.hlsli"
#ifndef __cplusplus
#ifndef uint16_t
#define uint16_t uint
#endif
#endif

#ifdef RB_PT_RESTIR_PDF_NEE_RLU_CURRENT_PRODUCER_ONLY
#define RB_PT_RESTIR_PRODUCER_ONLY 1
#endif

#if !defined(RB_PT_RESTIR_PRODUCER_ONLY)
#define RB_PT_KEEP_LEGACY_RESTIR_DEBUG_CODE 1
#endif

static const uint RT_STATIC_CONTRACT_REJECT_NONE = 0u;
static const uint RT_STATIC_CONTRACT_REJECT_GUI_ALPHA = 1u;
static const uint RT_STATIC_CONTRACT_REJECT_PARTICLE_DITHER = 2u;
static const uint RT_STATIC_CONTRACT_REJECT_GLASS_FALLBACK = 3u;
static const uint RT_STATIC_CONTRACT_REJECT_ADDITIVE_DECAL = 4u;
static const uint RT_STATIC_CONTRACT_REJECT_FILTER_DECAL = 5u;
static const uint RT_STATIC_CONTRACT_REJECT_ALPHA_TEST = 6u;
static const uint RT_STATIC_CONTRACT_REJECT_RIGID_INSTANCE_RANGE = 7u;
static const uint RT_STATIC_CONTRACT_REJECT_RIGID_PRIMITIVE_RANGE = 8u;
static const uint RT_STATIC_CONTRACT_REJECT_RIGID_INDEX_RANGE = 9u;
static const uint RT_STATIC_CONTRACT_REJECT_RIGID_VERTEX_RANGE = 10u;
static const uint RT_STATIC_CONTRACT_REJECT_TRIANGLE_RANGE = 11u;
static const uint RT_STATIC_CONTRACT_REJECT_VERTEX_RANGE = 12u;
static const uint RT_STATIC_CONTRACT_REJECT_MISS = 13u;
static const uint RT_STATIC_CONTRACT_REJECT_GUI_PRIMARY = 14u;

struct PathTraceSmokePayload
{
    uint value;
    float hitT;
    float3 normal;
    float3 geometricNormal;
    float3 tangent;
    float3 bitangent;
    float2 texCoord;
    float2 normalTexCoord;
    float2 hitBarycentrics;
    float4 vertexColor;
    float4 vertexColorAdd;
    uint surfaceClass;
    uint translucentSubtype;
    uint triangleClassAndFlags;
    uint materialId;
    uint materialIndex;
    uint instanceId;
    uint geometryIndex;
    uint primitiveIndex;
    uint shadowIgnoreInstanceId;
    uint shadowIgnorePrimitiveIndex;
    uint shadowIgnoreMaterialId;
    float3 debugVector;
    uint debugFlags;
    uint staticContractRejectReason;
};

struct PathTraceSmokeShadowPayload
{
    uint hit;
    uint rayMode;
    uint ignoreInstanceId;
    uint ignorePrimitiveIndex;
    uint ignoreMaterialId;
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

struct PathTraceEmissiveDistributionEntry
{
    uint emissiveTriangleIndex;
    float cumulativePdf;
    float weight;
    float padding0;
};

struct PathTraceSmokeLightCandidate
{
    float4 emissiveColorAndLuminance;
    float4 areaAndWeightedLuminance;
    uint materialId;
    uint universeMaterialIndex;
    uint materialIndex;
    uint triangleCount;
    uint flags;
    uint staticTriangleCount;
    uint dynamicTriangleCount;
    uint emissiveTextureIndex;
    uint emissiveTextureWidth;
    uint emissiveTextureHeight;
    uint padding1;
    uint padding2;
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

#include "RtxdiBridge/RAB_UnifiedLightRecord.hlsli"

static const uint PT_SKINNED_DISPATCH_HAS_VALID_PREVIOUS = 0x00000001u;
static const uint PT_SKINNED_DISPATCH_RT_CPU_SKINNED = 0x00000002u;
static const uint PT_RIGID_ROUTE_HAS_PREVIOUS_TRANSFORM = 0x00000001u;
static const uint PT_RIGID_ROUTE_TRANSFORM_CONTINUOUS = 0x00000002u;

struct PathTraceSkinnedSurfaceDispatchRecord
{
    uint sourceVertexOffset;
    uint outputVertexOffset;
    uint previousPositionOffset;
    uint vertexCount;
    uint currentJointOffset;
    uint previousJointOffset;
    uint surfaceRecordIndex;
    uint flags;
    uint dynamicVertexOffset;
    uint dynamicIndexOffset;
    uint dynamicTriangleOffset;
    uint triangleCount;
    float4 currentObjectToWorld0;
    float4 currentObjectToWorld1;
    float4 currentObjectToWorld2;
    float4 previousObjectToWorld0;
    float4 previousObjectToWorld1;
    float4 previousObjectToWorld2;
};

uint2 PathTracePrimarySurfaceStorePixel(uint2 pixel);
int2 PathTracePrimarySurfaceLoadPixel(int2 pixelPosition, bool previousFrame);

#include "PathTracePrimarySurface.hlsli"

struct PathTraceBoundsOverlayLine
{
    float4 startAndPad;
    float4 endAndPad;
    float4 color;
};

RaytracingAccelerationStructure SmokeScene : register(t0);
VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> SmokeOutput : register(u1);
VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> SmokeAccumulation : register(u15);
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> PathTraceMotionVectors : register(u39);
VK_IMAGE_FORMAT("r32ui") RWTexture2D<uint> PathTraceMotionVectorMask : register(u40);
VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> RestirPTReflectionOutput : register(u47);
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
StructuredBuffer<PathTraceSmokeVertex> SmokeRigidRouteVertices : register(t22);
StructuredBuffer<uint> SmokeRigidRouteIndices : register(t23);
StructuredBuffer<uint> SmokeRigidRouteTriangleMaterials : register(t24);
StructuredBuffer<uint> SmokeRigidRouteTriangleMaterialIndexes : register(t25);
StructuredBuffer<PathTraceRigidRouteInstance> SmokeRigidRouteInstances : register(t26);
StructuredBuffer<PathTraceSmokeVertex> SmokeSkinnedCurrentVertices : register(t29);
StructuredBuffer<PathTraceDoomAnalyticLightCandidate> DoomAnalyticLights : register(t27);
StructuredBuffer<PathTraceDoomAnalyticLightCandidateIdentity> DoomAnalyticCurrentIdentities : register(t42);
StructuredBuffer<PathTraceDoomAnalyticLightCandidateIdentity> DoomAnalyticPreviousIdentities : register(t43);
StructuredBuffer<PathTraceDoomAnalyticLightRemap> DoomAnalyticRemap : register(t44);
StructuredBuffer<PathTraceDoomAnalyticLightCandidate> DoomAnalyticPreviousLights : register(t45);
Texture2D<float4> SmokeFallbackTexture : register(t14);
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
StructuredBuffer<PathTraceEmissiveDistributionEntry> SmokeEmissiveDistribution : register(t46);
StructuredBuffer<PathTraceSmokeLightCandidate> SmokeLightCandidates : register(t17);
StructuredBuffer<PathTraceBoundsOverlayLine> SmokeBoundsOverlayLines : register(t21);
#include "RtxdiBridge/RAB_NeeCache.hlsli"
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
StructuredBuffer<PathTraceNeeCacheProviderResult> PathTraceNeeCacheProviderResults : register(t74);
StructuredBuffer<PathTraceNeeCacheCellRecord> PathTraceNeeCacheCells : register(t75);
StructuredBuffer<PathTraceDynamicMaterialRecord> SmokeDynamicMaterials : register(t76);
StructuredBuffer<PathTraceNeeCacheCandidateRecord> PathTraceNeeCacheCandidates : register(t77);
StructuredBuffer<PathTraceMaterialFeatureRecord> PathTraceMaterialFeatures : register(t80);
StructuredBuffer<PathTraceMaterialFeatureParameterRecord> PathTraceMaterialFeatureParameters : register(t81);
RWStructuredBuffer<uint> PathTraceLiquidPoolStatusCounters : register(u82);

#ifdef RB_PT_RESTIR_PDF_NEE_RLU_CURRENT_PRODUCER_ONLY
#include "Rtxdi/DI/Reservoir.hlsli"
#include "Rtxdi/DI/ReSTIRDIParameters.h"
#include "Rtxdi/Utils/RandomSamplerState.hlsli"
RWStructuredBuffer<RTXDI_PackedDIReservoir> CleanRtxdiDiCurrentReservoirs : register(u69);
#define RTXDI_LIGHT_RESERVOIR_BUFFER CleanRtxdiDiCurrentReservoirs
#include "Rtxdi/DI/ReservoirStorage.hlsli"
#endif
RWStructuredBuffer<PathTracePrimarySurfaceRecord> PrimarySurfaceHistoryCurrent : register(u30);
RWStructuredBuffer<PathTracePrimarySurfaceRecord> PrimarySurfaceHistoryPrevious : register(u31);
StructuredBuffer<PathTraceSkinnedSurfaceDispatchRecord> SmokeSkinnedSurfaceDispatch : register(t33);
StructuredBuffer<PathTraceSmokeVertex> SmokePreviousStaticVertices : register(t34);
StructuredBuffer<uint> SmokePreviousStaticIndices : register(t35);
StructuredBuffer<uint> SmokePreviousStaticTriangleClasses : register(t36);
StructuredBuffer<uint> SmokePreviousStaticTriangleMaterials : register(t37);
StructuredBuffer<uint> SmokePreviousStaticTriangleMaterialIndexes : register(t38);
StructuredBuffer<uint> SmokeSkinnedTriangleDispatchIndexes : register(t41);
VK_BINDING(0, 1) Texture2D<float4> SmokeDiffuseTextures[] : register(t0, space1);
SamplerState SmokeMaterialSampler : register(s0);

cbuffer PathTraceSmokeConstants : register(b2)
{
    float4 CameraOriginAndTMax;
    float4 CameraForwardAndTanX;
    float4 CameraLeftAndTanY;
    float4 CameraUpAndDebugMode;
    float4 TextureInfo;
    float4 LightOriginAndRadius[32];
    float4 LightColorAndIntensity[32];
    float4 LightInfo;
    float4 PortalWindowInfo;
    float4 LightSpriteInfo;
    float4 ToyPathInfo;
    float4 EmissiveInfo;
    float4 EmissiveDistributionInfo;
    float4 BoundsOverlayInfo;
    float4 DoomAnalyticLightInfo;
    float4 DoomAnalyticLightRemapInfo;
    float4 RestirPTInfo;
    float4 IntegratorInfo;
    float4 IntegratorInfo2;
    float4 PrevCameraOriginAndValid;
    float4 PrevCameraForwardAndTanX;
    float4 PrevCameraLeftAndTanY;
    float4 PrevCameraUpAndTanY;
    float4 SafetyInfo;
    float4 GeometryInfo0;
    float4 GeometryInfo1;
    float4 GeometryInfo2;
    float4 GeometryInfo3;
    float4 GeometryInfo4;
    float4 DispatchTileInfo;
    float4 NeeInfo;
    float4 MotionVectorInfo;
    float4 RestirPTSurfaceInfo;
    float4 ReservedRestirPTDirectInfo;
    float4 ReservedRestirPTSparsityInfo;
    float4 ReservedRestirPTIndirectInfo;
    float4 RayReconstructionInfo;
    float4 UnifiedLightInfo;
    float4 RestirLightManagerInfo;
    float4 RestirLightManagerControlInfo;
    float4 RestirLightManagerRangeInfo;
    float4 RestirLightManagerSampleInfo;
    float4 ReservedRestirPdfNeeInfo;
    float4 RestirPdfNeeRluCurrentControlInfo;
    float4 ReservedRestirPTDiDebugInfo;
    uint4 ReservedRestirPTRemixDiReservoirInfo;
    uint4 ReservedRestirPTRemixDiReservoirPageInfo;
    float4 ReservedRestirPTGiDebugInfo;
    float4 RegirInfo0;
    float4 RegirInfo1;
    float4 RegirInfo2;
    float4 RegirInfo3;
    float4 RegirInfo4;
    float4 NeeCacheInfo0;
    float4 NeeCacheInfo1;
    float4 NeeCacheInfo2;
    float4 NeeCacheInfo3;
    float4 NeeCacheConsumerInfo;
    float4 DecalInfo;
    float4 DecalInfo2;
    float4 LiquidPoolInfo;
};

uint PathTraceSecondaryLiquidPoolMode()
{
    return (uint)clamp(LiquidPoolInfo.x, 0.0, 3.0);
}

uint PathTraceSecondaryLiquidPoolDebug()
{
    return (uint)clamp(LiquidPoolInfo.y, 0.0, 6.0);
}

uint PathTraceSecondaryLiquidPoolPage()
{
    return (uint)clamp(LiquidPoolInfo.z, 0.0, 3.0);
}

uint PathTraceSecondaryLiquidPoolControlFlags()
{
    return (uint)max(LiquidPoolInfo.w, 0.0);
}

bool PathTraceWriteRestirReflectionLiquidPoolRouteDiagnostic(uint2 pixel)
{
    const uint debug = PathTraceSecondaryLiquidPoolDebug();
    if (debug != 6u)
    {
        return false;
    }
    const uint page = PathTraceSecondaryLiquidPoolPage();
    const uint status = PathTraceLiquidPoolControlInitialStatus(
        PathTraceSecondaryLiquidPoolControlFlags(), debug, page);
    const uint source = (status & RT_LIQUID_POOL_STATUS_INVALID_ROUTE) != 0u
        ? RT_LIQUID_POOL_SOURCE_INVALID
        : RT_LIQUID_POOL_SOURCE_RESTIR_REFLECTION;
    RestirPTReflectionOutput[pixel] = PathTraceLiquidPoolRouteDiagnostic(source, status);
    const uint exceptional = status &
        (RT_LIQUID_POOL_STATUS_OVERFLOW |
            RT_LIQUID_POOL_STATUS_DUPLICATE_APPLY |
            RT_LIQUID_POOL_STATUS_FAIL_CLOSED |
            RT_LIQUID_POOL_STATUS_INVALID_ROUTE);
    if (all(pixel == uint2(0u, 0u)) && exceptional != 0u &&
        (PathTraceSecondaryLiquidPoolControlFlags() & RT_LIQUID_POOL_CONTROL_TELEMETRY_READY) != 0u)
    {
        uint ignored;
        InterlockedOr(
            PathTraceLiquidPoolStatusCounters[RT_LIQUID_POOL_SOURCE_RESTIR_REFLECTION],
            exceptional,
            ignored);
    }
    return true;
}

static const uint RT_SMOKE_TRIANGLE_CLASS_MASK = 0x0000ffffu;
static const uint RT_SMOKE_TRIANGLE_FORCE_GEOMETRIC_NORMAL = 0x00010000u;
static const uint RT_SMOKE_EMISSIVE_TRIANGLE_HISTORY_DYNAMIC = 0x00020000u;
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
static const uint PT_MOTION_VECTOR_MASK_VALID = 0x00000001u;
static const uint PT_MOTION_VECTOR_MASK_SOURCE_SHIFT = 1u;
static const uint PT_MOTION_VECTOR_MASK_SOURCE_MASK = 0x0000001eu;
static const uint PT_MOTION_VECTOR_MASK_INVALID_REASON_SHIFT = 5u;
static const uint PT_MOTION_VECTOR_SOURCE_UNKNOWN = 0u;
static const uint PT_MOTION_VECTOR_SOURCE_STATIC = 1u;
static const uint PT_MOTION_VECTOR_SOURCE_SKINNED = 2u;
static const uint PT_MOTION_VECTOR_SOURCE_RIGID = 3u;
static const uint PT_MOTION_VECTOR_SOURCE_OTHER_OBJECT = 4u;
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
static const uint RT_SMOKE_MAX_DEBUG_LIGHTS = 32u;
static const uint RT_DOOM_ANALYTIC_LIGHT_CASTS_SHADOWS = 0x00000001u;
static const uint RT_PT_SAFETY_DISABLE_ANY_HIT_ALPHA = 0x00000001u;
static const uint RT_PT_SAFETY_DISABLE_SELECTED_LIGHT_LOOP = 0x00000002u;
static const uint RT_SMOKE_RAY_MODE_PRIMARY_FILTER_DECAL_COMPOSITE = 3u;
static const uint RT_PT_SAFETY_DISABLE_ANALYTIC_LIGHT_LOOP = 0x00000004u;
static const uint RT_PT_SAFETY_DISABLE_EMISSIVE_TRIANGLE_SAMPLING = 0x00000008u;
static const uint RT_PT_SAFETY_DISABLE_DIFFUSE_SECONDARY_RAY = 0x00000010u;
static const uint RT_PT_SAFETY_DISABLE_REFLECTION_RAY = 0x00000020u;
static const uint RT_PT_SAFETY_DISABLE_PRIMARY_SURFACE_HISTORY = 0x00000040u;
static const uint RT_PT_SAFETY_DISABLE_RESTIR_VISIBILITY_RAY = 0x00000100u;

#include "pathtrace_material_classifier.hlsli"

bool PathTraceSafetyDisabled(uint bit)
{
    return (((uint)SafetyInfo.x) & bit) != 0u;
}

uint PathTraceStaticVertexCount() { return (uint)max(GeometryInfo0.x, 0.0); }
uint PathTraceStaticIndexCount() { return (uint)max(GeometryInfo0.y, 0.0); }
uint PathTraceStaticTriangleCount() { return (uint)max(GeometryInfo0.z, 0.0); }
uint PathTraceDynamicVertexCount() { return (uint)max(GeometryInfo0.w, 0.0); }
uint PathTraceDynamicIndexCount() { return (uint)max(GeometryInfo1.x, 0.0); }
uint PathTraceDynamicTriangleCount() { return (uint)max(GeometryInfo1.y, 0.0); }
uint PathTraceRigidRouteVertexCount() { return (uint)max(GeometryInfo1.z, 0.0); }
uint PathTraceRigidRouteIndexCount() { return (uint)max(GeometryInfo1.w, 0.0); }
uint PathTraceRigidRouteTriangleCount() { return (uint)max(GeometryInfo2.x, 0.0); }
uint PathTraceRigidRouteInstanceCount() { return (uint)max(GeometryInfo2.y, 0.0); }
uint PathTracePrimarySurfaceHistoryCount() { return (uint)max(GeometryInfo2.z, 0.0); }
uint PathTraceSkinnedPreviousPositionCount() { return (uint)max(GeometryInfo3.x, 0.0); }
uint PathTraceSkinnedSurfaceDispatchCount() { return (uint)max(GeometryInfo3.y, 0.0); }
uint PathTraceSkinnedTriangleDispatchIndexCount() { return (uint)max(GeometryInfo3.z, 0.0); }
uint PathTracePreviousStaticVertexCount() { return (uint)max(GeometryInfo4.x, 0.0); }
uint PathTracePreviousStaticIndexCount() { return (uint)max(GeometryInfo4.y, 0.0); }
uint PathTracePreviousStaticTriangleCount() { return (uint)max(GeometryInfo4.z, 0.0); }
uint PathTracePreviousStaticMaterialIndexCount() { return (uint)max(GeometryInfo4.w, 0.0); }

uint2 PathTraceDispatchTileOffset()
{
    return uint2((uint)max(DispatchTileInfo.x, 0.0), (uint)max(DispatchTileInfo.y, 0.0));
}

uint2 PathTraceFullOutputSize()
{
    const uint2 size = uint2((uint)max(DispatchTileInfo.z, 0.0), (uint)max(DispatchTileInfo.w, 0.0));
    return (size.x > 0u && size.y > 0u) ? size : DispatchRaysDimensions().xy;
}

uint PathTraceDebugMode()
{
#ifdef RB_PT_FORCE_DEBUG_MODE
    return RB_PT_FORCE_DEBUG_MODE;
#else
    return (uint)CameraUpAndDebugMode.w;
#endif
}

uint2 PathTracePrimarySurfaceStorePixel(uint2 pixel)
{
    return pixel;
}

int2 PathTracePrimarySurfaceLoadPixel(int2 pixelPosition, bool previousFrame)
{
    return pixelPosition;
}

#include "RtxdiBridge/RAB_Material.hlsli"

#include "pathtrace_smoke_rab_policy_supplier.hlsli"

bool ProjectSmokeOverlayPoint(float3 worldPosition, uint2 dimensions, out float2 projectedPixel)
{
    const float3 delta = worldPosition - CameraOriginAndTMax.xyz;
    const float forwardDistance = dot(delta, CameraForwardAndTanX.xyz);
    if (forwardDistance <= 0.05)
    {
        projectedPixel = float2(0.0, 0.0);
        return false;
    }

    const float ndcX = -dot(delta, CameraLeftAndTanY.xyz) / max(forwardDistance * CameraForwardAndTanX.w, 1.0e-5);
    const float ndcY = -dot(delta, CameraUpAndDebugMode.xyz) / max(forwardDistance * CameraLeftAndTanY.w, 1.0e-5);
    projectedPixel = (float2(ndcX, ndcY) * 0.5 + 0.5) * float2(dimensions);
    return all(abs(float2(ndcX, ndcY)) <= 1.35);
}

float DistanceToSmokeOverlaySegment(float2 samplePixel, float2 startPoint, float2 endPoint)
{
    const float2 segment = endPoint - startPoint;
    const float segmentLengthSquared = dot(segment, segment);
    if (segmentLengthSquared <= 1.0e-5)
    {
        return length(samplePixel - startPoint);
    }
    const float segmentT = saturate(dot(samplePixel - startPoint, segment) / segmentLengthSquared);
    return length(samplePixel - (startPoint + segment * segmentT));
}

float4 ApplySmokeBoundsOverlay(float4 baseColor, uint2 pixel, uint2 dimensions)
{
    const uint lineCount = min((uint)max(BoundsOverlayInfo.x, 0.0), 4096u);
    if (lineCount == 0u || BoundsOverlayInfo.z <= 0.0)
    {
        return baseColor;
    }

    const float2 pixelCenter = float2(pixel) + 0.5;
    const float thickness = max(BoundsOverlayInfo.y, 0.5);
    float3 overlayColor = float3(0.0, 0.0, 0.0);
    float overlayCoverage = 0.0;

    [loop]
    for (uint lineIndex = 0u; lineIndex < lineCount; ++lineIndex)
    {
        const PathTraceBoundsOverlayLine overlayLine = SmokeBoundsOverlayLines[lineIndex];
        float2 startPixel;
        float2 endPixel;
        if (!ProjectSmokeOverlayPoint(overlayLine.startAndPad.xyz, dimensions, startPixel) ||
            !ProjectSmokeOverlayPoint(overlayLine.endAndPad.xyz, dimensions, endPixel))
        {
            continue;
        }

        const float distanceToLine = DistanceToSmokeOverlaySegment(pixelCenter, startPixel, endPixel);
        const float coverage = saturate((thickness + 0.75 - distanceToLine) / 0.75) * saturate(overlayLine.color.a);
        if (coverage > overlayCoverage)
        {
            overlayCoverage = coverage;
            overlayColor = overlayLine.color.rgb;
        }
    }

    if (overlayCoverage <= 0.0)
    {
        return baseColor;
    }
    return float4(lerp(baseColor.rgb, overlayColor, saturate(overlayCoverage * 0.85)), 1.0);
}

bool SmokeRayIntersectAabb(float3 rayOrigin, float3 rayDirection, float3 boundsMin, float3 boundsMax, out float hitT)
{
    const float3 safeDirection = float3(
        abs(rayDirection.x) < 1.0e-6 ? (rayDirection.x < 0.0 ? -1.0e-6 : 1.0e-6) : rayDirection.x,
        abs(rayDirection.y) < 1.0e-6 ? (rayDirection.y < 0.0 ? -1.0e-6 : 1.0e-6) : rayDirection.y,
        abs(rayDirection.z) < 1.0e-6 ? (rayDirection.z < 0.0 ? -1.0e-6 : 1.0e-6) : rayDirection.z);
    const float3 invDirection = 1.0 / safeDirection;
    const float3 t0 = (boundsMin - rayOrigin) * invDirection;
    const float3 t1 = (boundsMax - rayOrigin) * invDirection;
    const float3 tMin = min(t0, t1);
    const float3 tMax = max(t0, t1);
    const float tEnter = max(max(tMin.x, tMin.y), max(tMin.z, 0.0));
    const float tExit = min(tMax.x, min(tMax.y, tMax.z));
    hitT = tEnter > 0.001 ? tEnter : tExit;
    return tExit >= tEnter && tExit > 0.0;
}

float4 RenderSmokeBoundsBoxes(float3 rayOrigin, float3 rayDirection)
{
    const uint lineCount = min((uint)max(BoundsOverlayInfo.x, 0.0), 4096u);
    const uint boxCount = min(lineCount / 12u, 64u);
    float closestHitT = CameraOriginAndTMax.w;
    float3 closestColor = float3(0.0, 0.0, 0.0);
    bool hitAnyBox = false;

    [loop]
    for (uint boxIndex = 0u; boxIndex < boxCount; ++boxIndex)
    {
        const uint firstLine = boxIndex * 12u;
        float3 boundsMin = float3(1.0e20, 1.0e20, 1.0e20);
        float3 boundsMax = float3(-1.0e20, -1.0e20, -1.0e20);

        [unroll]
        for (uint edgeIndex = 0u; edgeIndex < 12u; ++edgeIndex)
        {
            const PathTraceBoundsOverlayLine boxLine = SmokeBoundsOverlayLines[firstLine + edgeIndex];
            boundsMin = min(boundsMin, min(boxLine.startAndPad.xyz, boxLine.endAndPad.xyz));
            boundsMax = max(boundsMax, max(boxLine.startAndPad.xyz, boxLine.endAndPad.xyz));
        }

        const float3 extent = boundsMax - boundsMin;
        if (any(extent <= float3(0.001, 0.001, 0.001)))
        {
            continue;
        }

        float hitT;
        if (SmokeRayIntersectAabb(rayOrigin, rayDirection, boundsMin, boundsMax, hitT) && hitT < closestHitT)
        {
            const PathTraceBoundsOverlayLine firstBoxLine = SmokeBoundsOverlayLines[firstLine];
            closestHitT = hitT;
            closestColor = saturate(firstBoxLine.color.rgb);
            hitAnyBox = true;
        }
    }

    if (!hitAnyBox)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    const float depthFade = 0.35 + 0.65 * saturate(1.0 - closestHitT / max(CameraOriginAndTMax.w, 1.0));
    return float4(saturate(closestColor * depthFade + closestColor * 0.25), 1.0);
}

float SmokeDistanceToSegment(float3 samplePosition, float3 startPoint, float3 endPoint)
{
    const float3 segment = endPoint - startPoint;
    const float segmentLengthSquared = dot(segment, segment);
    if (segmentLengthSquared <= 1.0e-6)
    {
        return length(samplePosition - startPoint);
    }
    const float segmentT = saturate(dot(samplePosition - startPoint, segment) / segmentLengthSquared);
    return length(samplePosition - (startPoint + segment * segmentT));
}

float4 RenderSmokeBoundsWireframeBoxes(float3 rayOrigin, float3 rayDirection)
{
    const uint lineCount = min((uint)max(BoundsOverlayInfo.x, 0.0), 4096u);
    const uint boxCount = min(lineCount / 12u, 64u);
    float closestEdgeHitT = CameraOriginAndTMax.w;
    float3 closestEdgeColor = float3(0.0, 0.0, 0.0);
    bool hitAnyEdge = false;

    [loop]
    for (uint boxIndex = 0u; boxIndex < boxCount; ++boxIndex)
    {
        const uint firstLine = boxIndex * 12u;
        float3 boundsMin = float3(1.0e20, 1.0e20, 1.0e20);
        float3 boundsMax = float3(-1.0e20, -1.0e20, -1.0e20);

        [unroll]
        for (uint edgeIndex = 0u; edgeIndex < 12u; ++edgeIndex)
        {
            const PathTraceBoundsOverlayLine boxLine = SmokeBoundsOverlayLines[firstLine + edgeIndex];
            boundsMin = min(boundsMin, min(boxLine.startAndPad.xyz, boxLine.endAndPad.xyz));
            boundsMax = max(boundsMax, max(boxLine.startAndPad.xyz, boxLine.endAndPad.xyz));
        }

        const float3 extent = boundsMax - boundsMin;
        if (any(extent <= float3(0.001, 0.001, 0.001)))
        {
            continue;
        }

        float hitT;
        if (!SmokeRayIntersectAabb(rayOrigin, rayDirection, boundsMin, boundsMax, hitT))
        {
            continue;
        }

        const float3 hitPoint = rayOrigin + rayDirection * hitT;
        float minEdgeDistance = 1.0e20;
        float3 edgeColor = float3(1.0, 1.0, 1.0);

        [unroll]
        for (uint edgeIndex = 0u; edgeIndex < 12u; ++edgeIndex)
        {
            const PathTraceBoundsOverlayLine edgeLine = SmokeBoundsOverlayLines[firstLine + edgeIndex];
            const float edgeDistance = SmokeDistanceToSegment(hitPoint, edgeLine.startAndPad.xyz, edgeLine.endAndPad.xyz);
            if (edgeDistance < minEdgeDistance)
            {
                minEdgeDistance = edgeDistance;
                edgeColor = saturate(edgeLine.color.rgb);
            }
        }

        const float edgeThickness = clamp(min(min(extent.x, extent.y), extent.z) * 0.045, 1.5, 8.0);
        if (minEdgeDistance <= edgeThickness && hitT < closestEdgeHitT)
        {
            closestEdgeHitT = hitT;
            closestEdgeColor = edgeColor;
            hitAnyEdge = true;
        }
    }

    if (!hitAnyEdge)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    return float4(saturate(closestEdgeColor * 1.35 + 0.10), 1.0);
}

float3 SafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-8 ? value * rsqrt(lengthSquared) : fallback;
}

float SmokeLinear1(float c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 SmokeLinear3(float3 c)
{
    return float3(SmokeLinear1(c.r), SmokeLinear1(c.g), SmokeLinear1(c.b));
}

void SmokePBRFromSpecmap(float3 specMap, out float3 F0, out float roughness)
{
    const float specLum = dot(float3(0.2125, 0.7154, 0.0721), specMap);

    F0 = float3(0.04, 0.04, 0.04);

    const float contrastMid = 0.214;
    const float contrastAmount = 2.0;
    float contrast = saturate((specLum - contrastMid) / (1.0 - contrastMid));
    contrast += saturate(specLum / contrastMid) - 1.0;
    contrast = exp2(contrastAmount * contrast);
    F0 *= contrast;

    const float linearBrightness = SmokeLinear1(2.0 * specLum);
    const float specPow = max(0.0, ((8.0 * linearBrightness) / max(F0.y, 1.0e-4)) - 2.0);
    F0 *= min(1.0, linearBrightness / max(F0.y * 0.25, 1.0e-4));

    roughness = sqrt(2.0 / (specPow + 2.0));

    const float glossiness = saturate(1.0 - roughness);
    const float metallic = step(0.7, glossiness);
    const float3 glossColor = SmokeLinear3(specMap.rgb);
    F0 = lerp(F0, glossColor, metallic);

    roughness = sqrt(roughness);
}

float3 BuildPerpendicular(float3 normal)
{
    const float3 axis = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    return SafeNormalize(cross(axis, normal), float3(1.0, 0.0, 0.0));
}

float3 TransformRigidRouteVector(
    PathTraceRigidRouteInstance routeInstance,
    float3 localVector)
{
    return float3(
        dot(routeInstance.currentObjectToWorld0.xyz, localVector),
        dot(routeInstance.currentObjectToWorld1.xyz, localVector),
        dot(routeInstance.currentObjectToWorld2.xyz, localVector));
}

float RigidRouteHandednessSign(
    PathTraceRigidRouteInstance routeInstance)
{
    return dot(
            cross(
                routeInstance.currentObjectToWorld0.xyz,
                routeInstance.currentObjectToWorld1.xyz),
            routeInstance.currentObjectToWorld2.xyz) < 0.0
        ? -1.0
        : 1.0;
}

bool TryBuildCapturedTangentBasis(
    float3 normal,
    float4 capturedTangent,
    float4 capturedBitangent,
    float handednessSign,
    out float3 tangent,
    out float3 bitangent)
{
    const float3 tangentFallback = BuildPerpendicular(normal);
    const float3 bitangentFallback = SafeNormalize(cross(normal, tangentFallback), float3(0.0, 1.0, 0.0));
    const float3 rawTangent = capturedTangent.xyz;
    const float3 projectedTangent = rawTangent - normal * dot(normal, rawTangent);
    const float tangentLengthSquared = dot(projectedTangent, projectedTangent);
    if (tangentLengthSquared <= 1.0e-8 || abs(capturedTangent.w) < 0.5)
    {
        tangent = tangentFallback;
        bitangent = bitangentFallback;
        return false;
    }

    tangent = projectedTangent * rsqrt(tangentLengthSquared);
    const float3 rawBitangent = capturedBitangent.xyz;
    const float3 projectedBitangent = rawBitangent - normal * dot(normal, rawBitangent) - tangent * dot(tangent, rawBitangent);
    const float bitangentLengthSquared = dot(projectedBitangent, projectedBitangent);
    if (bitangentLengthSquared > 1.0e-8)
    {
        bitangent = projectedBitangent * rsqrt(bitangentLengthSquared);
    }
    else
    {
        const float bitangentSign = (capturedTangent.w < 0.0 ? -1.0 : 1.0) * handednessSign;
        bitangent = SafeNormalize(cross(normal, tangent) * bitangentSign, bitangentFallback);
    }
    return true;
}

float3 MaterialIdToColor(uint materialId)
{
    uint hash = materialId;
    hash ^= hash >> 16;
    hash *= 2246822519u;
    hash ^= hash >> 13;
    hash *= 3266489917u;
    hash ^= hash >> 16;

    const float r = ((hash >> 0) & 255u) * (1.0 / 255.0);
    const float g = ((hash >> 8) & 255u) * (1.0 / 255.0);
    const float b = ((hash >> 16) & 255u) * (1.0 / 255.0);
    return 0.15 + float3(r, g, b) * 0.85;
}

float3 SmokeMatClassRouteColor(uint route)
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

float3 SmokeMatClassSurfaceClassColor(uint surfaceClass)
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

PathTraceSmokeMaterial LoadSmokeMaterial(uint materialIndex);
float4 SampleSmokeSurfaceAlbedo(PathTraceSmokeMaterial material, float2 texCoord, uint surfaceClass, uint translucentSubtype, float4 vertexColor, float4 vertexColorAdd);
float3 SampleSmokeDirectSpecular(PathTraceSmokeMaterial material, float2 texCoord);

float4 EvaluateSmokeMaterialClassifierDebug(PathTraceSmokePayload payload, uint2 pixel, uint2 dimensions)
{
    const PathTraceSmokeMaterial material = LoadSmokeMaterial(payload.materialIndex);
    const bool hasClassifierRecord = material.padding1 != 0u;
    const bool right = pixel.x * 2u >= dimensions.x;
    const bool bottom = pixel.y * 2u >= dimensions.y;

    if (!hasClassifierRecord)
    {
        const uint checker = ((pixel.x >> 4u) ^ (pixel.y >> 4u)) & 1u;
        return checker != 0u ? float4(0.0, 0.0, 0.0, 1.0) : float4(1.0, 1.0, 1.0, 1.0);
    }

    if (!right && !bottom)
    {
        return float4(SmokeMatClassRouteColor(SmokeMatClassRoute(material)), 1.0);
    }
    if (right && !bottom)
    {
        return float4(SmokeMatClassSurfaceClassColor(SmokeMatClassSurfaceClass(material)), 1.0);
    }
    if (!right)
    {
        const float3 specularColor = SampleSmokeDirectSpecular(material, payload.texCoord);
        float3 specularF0 = saturate(specularColor);
        float roughness = SmokeMatClassRoughness(material);
        float3 albedo = SampleSmokeSurfaceAlbedo(material, payload.texCoord, payload.surfaceClass, payload.translucentSubtype, payload.vertexColor, payload.vertexColorAdd).rgb;
        const bool hasSpecularInput =
            material.specularTextureIndex != 0xffffffffu &&
            ((((uint)TextureInfo.w) & RT_SMOKE_TEXTURE_FLAG_USE_SPECULAR_MAPS) != 0u) &&
            max(max(specularColor.r, specularColor.g), specularColor.b) > 1.0e-4;
        if (hasSpecularInput)
        {
            SmokePBRFromSpecmap(saturate(specularColor), specularF0, roughness);
        }
        SmokeApplyMaterialClassifierBsdfWithSpecularTexel(material, albedo, saturate(specularColor), specularF0, roughness);
        if (SmokeMatClassHasPackedBsdf(material))
        {
            roughness = SmokeMatClassRoughness(material);
        }
        if ((material.padding0 & RT_SMOKE_MATERIAL_OVERRIDE_ZERO_ROUGHNESS) != 0u)
        {
            roughness = 0.0;
        }
        return float4(roughness, roughness, roughness, 1.0);
    }

    if (SmokeMatClassDrivesLegacySpec(material))
    {
        return float4(1.0, 0.0, 0.0, 1.0);
    }

    return float4(
        0.0,
        SmokeMaterialHasFullMetalOverride(material) ? 1.0 : SmokeMatClassMetallic(material),
        SmokeMatClassTransmission(material),
        1.0);
}

float3 TranslucentSubtypeToColor(uint subtype)
{
    if (subtype == 0u)
    {
        return float3(0.10, 0.75, 1.00); // decal / grime
    }
    if (subtype == 1u)
    {
        return float3(0.20, 1.00, 0.65); // object glass
    }
    if (subtype == 2u)
    {
        return float3(1.00, 0.45, 0.05); // smoke / particle
    }
    if (subtype == 3u)
    {
        return float3(1.00, 0.95, 0.10); // signage / glow
    }
    if (subtype == 4u)
    {
        return float3(0.85, 0.25, 1.00); // portal / window
    }
    if (subtype == 5u)
    {
        return float3(1.00, 0.20, 0.75); // gui / screen
    }
    return float3(0.75, 0.75, 0.75);
}

float3 DebugLightSlotColor(uint lightIndex)
{
    uint hash = lightIndex + 1u;
    hash ^= hash >> 16;
    hash *= 2246822519u;
    hash ^= hash >> 13;
    hash *= 3266489917u;
    hash ^= hash >> 16;

    const float r = ((hash >> 0) & 255u) * (1.0 / 255.0);
    const float g = ((hash >> 8) & 255u) * (1.0 / 255.0);
    const float b = ((hash >> 16) & 255u) * (1.0 / 255.0);
    return 0.20 + float3(r, g, b) * 0.80;
}

float SmokeHashToUnitFloat(uint hash)
{
    hash ^= hash >> 16;
    hash *= 2246822519u;
    hash ^= hash >> 13;
    hash *= 3266489917u;
    hash ^= hash >> 16;
    return ((hash >> 8) & 0x00ffffffu) * (1.0 / 16777215.0);
}

uint SmokeAlphaStochasticHash(uint stableHash, uint salt)
{
    const uint2 pixel = DispatchRaysIndex().xy + PathTraceDispatchTileOffset();
    const uint frameIndex = (uint)max(RestirPTInfo.x, 0.0);
    return stableHash ^
        pixel.x * 1597334677u ^
        pixel.y * 3812015801u ^
        frameIndex * 277803737u ^
        salt * 3266489917u;
}

#include "pathtrace_smoke_rab_material_supplier.hlsli"

bool BuildSmokeReflectionBounce(
    PathTraceSmokeMaterial material,
    PathTraceSmokePayload payload,
    float3 normal,
    float3 rayDirection,
    out float3 reflectionDir,
    out float3 reflectionWeight,
    out float roughness)
{
    reflectionDir = float3(0.0, 0.0, 0.0);
    reflectionWeight = float3(0.0, 0.0, 0.0);
    roughness = 1.0;

    if (PathTraceIntegratorReflectionMode() == 0u || PathTraceIntegratorSpecularBounceLimit() == 0u || PathTraceIntegratorMaxPathDepth() <= 1u)
    {
        return false;
    }
    if (payload.surfaceClass == RT_SMOKE_SURFACE_CLASS_TRANSLUCENT)
    {
        return false;
    }

    const float3 specularColor = SampleSmokeDirectSpecular(material, payload.texCoord);
    float3 F0;
    BuildSmokeSpecularLobe(specularColor, F0, roughness);
    float3 albedo = SampleSmokeSurfaceAlbedo(material, payload.texCoord, payload.surfaceClass, payload.translucentSubtype, payload.vertexColor, payload.vertexColorAdd).rgb;
    SmokeApplyMaterialClassifierBsdfWithSpecularTexel(material, albedo, saturate(specularColor), F0, roughness);
    const bool fullMetalOverride = SmokeMaterialHasFullMetalOverride(material);
    SmokeApplyFullMetalOverride(material, albedo, F0);
    if ((material.padding0 & RT_SMOKE_MATERIAL_OVERRIDE_ZERO_ROUGHNESS) != 0u)
    {
        roughness = 0.0;
        if (!fullMetalOverride)
        {
            F0 = max(F0, float3(0.85, 0.85, 0.85));
        }
    }
    const float f0Max = max(max(F0.r, F0.g), F0.b);
    const float roughnessLimit = PathTraceIntegratorReflectionMode() >= 2u ? 0.72 : 0.36;
    if (f0Max < 0.035 || roughness > roughnessLimit)
    {
        return false;
    }

    reflectionDir = SafeNormalize(reflect(rayDirection, normal), normal);
    if (dot(reflectionDir, normal) <= 0.0 || dot(reflectionDir, payload.geometricNormal) <= 0.0)
    {
        return false;
    }

    reflectionWeight = saturate(F0) * (1.0 - saturate(roughness * 0.85));
    return max(max(reflectionWeight.r, reflectionWeight.g), reflectionWeight.b) > 0.0;
}

float3 EvaluateSmokeSpecular(float3 specularColor, float3 normal, float3 lightDir, float3 viewDir, float3 lightColor, float directScale, float visibility)
{
    if (visibility <= 0.0 || max(max(specularColor.r, specularColor.g), specularColor.b) <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float3 halfVector = SafeNormalize(lightDir + viewDir, normal);
    if (SmokeToyFakePBRSpecularEnabled())
    {
        float3 F0;
        float roughness;
        SmokePBRFromSpecmap(saturate(specularColor), F0, roughness);

        const float ndotl = saturate(dot(normal, lightDir));
        const float hdotN = saturate(dot(normal, halfVector));
        const float ldotH = saturate(dot(lightDir, halfVector));
        const float rr = roughness * roughness;
        const float rrrr = max(rr * rr, 1.0e-4);
        const float D = max((hdotN * hdotN) * (rrrr - 1.0) + 1.0, 1.0e-4);
        const float VFapprox = max((ldotH * ldotH) * (roughness + 0.5), 1.0e-4);
        const float specularTerm = (rrrr / (4.0 * D * D * VFapprox)) * ndotl * directScale * visibility;
        return F0 * lightColor * specularTerm;
    }

    const float specularNdotH = saturate(dot(normal, halfVector));
    const float specularTerm = pow(specularNdotH, 32.0) * directScale * visibility;
    return specularColor * lightColor * specularTerm;
}

float SmokeAdditiveDecalOpacity(float3 albedo)
{
    return max(max(albedo.r, albedo.g), albedo.b);
}

float SmokeAdditiveDecalMaterialOpacity(PathTraceSmokeMaterial material, float3 albedo)
{
    if ((material.flags & RT_SMOKE_MATERIAL_ADDITIVE_DECAL_WHITE_KEY) != 0u)
    {
        return 1.0 - min(min(albedo.r, albedo.g), albedo.b);
    }

    return SmokeAdditiveDecalOpacity(albedo);
}

bool SmokeAdditiveDecalRejectsHit(PathTraceSmokeMaterial material, float2 texCoord, uint triangleClassAndFlags, bool shadowRay)
{
    if ((material.flags & RT_SMOKE_MATERIAL_ADDITIVE_DECAL) == 0u)
    {
        return false;
    }

    if (!shadowRay && (triangleClassAndFlags & RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF) != 0u)
    {
        return false;
    }

    if (shadowRay)
    {
        return true;
    }

    const float3 albedo = SampleSmokeDiffuseTexture(material, texCoord).rgb;
    const float opacity = SmokeAdditiveDecalMaterialOpacity(material, albedo);
    return opacity < 0.035;
}

bool SmokeFilterDecalRejectsHit(PathTraceSmokeMaterial material, float2 texCoord, float2 barycentrics, uint instanceId, uint primitiveIndex, bool shadowRay, uint rayMode)
{
    if ((material.flags & RT_SMOKE_MATERIAL_FILTER_DECAL) == 0u)
    {
        return false;
    }

    if (shadowRay)
    {
        return true;
    }

    if (rayMode == RT_SMOKE_RAY_MODE_PRIMARY_FILTER_DECAL_COMPOSITE)
    {
        return false;
    }

    const float3 albedo = SampleSmokeDiffuseTexture(material, texCoord).rgb;
    const bool blackKey = (material.flags & RT_SMOKE_MATERIAL_FILTER_DECAL_BLACK_KEY) != 0u;
    const float effect = blackKey
        ? max(max(albedo.r, albedo.g), albedo.b)
        : 1.0 - min(min(albedo.r, albedo.g), albedo.b);
    const float opacity = saturate(effect * 0.55);
    const float2 wrappedTexCoord = frac(abs(texCoord));
    const uint2 ditherCell = uint2(floor(wrappedTexCoord * 256.0));
    const uint2 baryCell = uint2(saturate(barycentrics) * 255.0);
    const uint hash =
        primitiveIndex * 1597334677u ^
        instanceId * 3812015801u ^
        ditherCell.x * 747796405u ^
        ditherCell.y * 2891336453u ^
        baryCell.x * 277803737u ^
        baryCell.y * 3266489917u;
    return opacity < SmokeHashToUnitFloat(SmokeAlphaStochasticHash(hash, 11u));
}

float4 SmokeMissColor()
{
    return float4(1.0, 0.0, 0.0, 1.0);
}

PathTraceSmokePayload InitSmokePayload()
{
    PathTraceSmokePayload payload;
    payload.value = 0;
    payload.hitT = 0.0;
    payload.normal = float3(0.0, 0.0, 0.0);
    payload.geometricNormal = float3(0.0, 0.0, 0.0);
    payload.tangent = float3(1.0, 0.0, 0.0);
    payload.bitangent = float3(0.0, 1.0, 0.0);
    payload.texCoord = float2(0.0, 0.0);
    payload.normalTexCoord = float2(0.0, 0.0);
    payload.vertexColor = float4(1.0, 1.0, 1.0, 1.0);
    payload.vertexColorAdd = float4(0.5, 0.5, 0.5, 0.5);
    payload.surfaceClass = 4;
    payload.translucentSubtype = 5;
    payload.triangleClassAndFlags = 4u;
    payload.materialId = 0;
    payload.materialIndex = 0;
    payload.instanceId = 0xffffffffu;
    payload.geometryIndex = 0xffffffffu;
    payload.primitiveIndex = 0xffffffffu;
    payload.shadowIgnoreInstanceId = 0xffffffffu;
    payload.shadowIgnorePrimitiveIndex = 0xffffffffu;
    payload.shadowIgnoreMaterialId = 0xffffffffu;
    payload.debugVector = float3(0.0, 0.0, 0.0);
    payload.debugFlags = 0u;
    payload.staticContractRejectReason = RT_STATIC_CONTRACT_REJECT_NONE;
    return payload;
}

#include "pathtrace_smoke_rab_visibility_supplier.hlsli"

#include "pathtrace_smoke_rab_surface_supplier.hlsli"

#include "pathtrace_smoke_rab_motion_supplier.hlsli"

bool SmokePayloadIsGuiScreen(PathTraceSmokePayload payload);
float4 CompositeSmokeGuiLayers(float3 rayOrigin, float3 rayDirection, PathTraceSmokePayload firstPayload);
uint SelectSmokeWeightedEmissiveTriangle(uint emissiveTriangleCount, float randomValue);

bool SmokePayloadIsFilterDecal(PathTraceSmokePayload payload)
{
    return payload.value != 0u &&
        (LoadSmokeMaterial(payload.materialIndex).flags & RT_SMOKE_MATERIAL_FILTER_DECAL) != 0u;
}

bool ResolvePrimaryFilterDecalReceiver(inout PathTraceSmokePayload payload, RayDesc ray, uint traceMask, out PathTraceSmokePayload decalPayload)
{
    decalPayload = payload;
    if (!SmokePayloadIsFilterDecal(payload))
    {
        return false;
    }

    PathTraceSmokePayload receiverPayload = InitSmokePayload();
    receiverPayload.value = 2u;
    receiverPayload.shadowIgnoreInstanceId = payload.instanceId;
    receiverPayload.shadowIgnorePrimitiveIndex = payload.primitiveIndex;
    receiverPayload.shadowIgnoreMaterialId = payload.materialId;

    RayDesc receiverRay = ray;
    receiverRay.TMin = max(ray.TMin, max(payload.hitT - 0.05, 0.001));
    TraceRay(SmokeScene, RAY_FLAG_NONE, traceMask, 0, 1, 0, receiverRay, receiverPayload);
    if (receiverPayload.value == 0u || SmokePayloadIsGuiScreen(receiverPayload))
    {
        return false;
    }

    payload = receiverPayload;
    return true;
}

void ApplyPrimaryFilterDecalToSurface(inout RAB_Surface surface, PathTraceSmokePayload decalPayload)
{
    if (!RAB_IsSurfaceValid(surface))
    {
        return;
    }

    const PathTraceSmokeMaterial decalMaterial = LoadSmokeMaterial(decalPayload.materialIndex);
    if ((decalMaterial.flags & RT_SMOKE_MATERIAL_FILTER_DECAL) == 0u)
    {
        return;
    }

    const float3 decalAlbedo = saturate(SampleSmokeSurfaceAlbedo(
        decalMaterial,
        decalPayload.texCoord,
        decalPayload.surfaceClass,
        decalPayload.translucentSubtype,
        decalPayload.vertexColor,
        decalPayload.vertexColorAdd).rgb);
    const bool blackKey = (decalMaterial.flags & RT_SMOKE_MATERIAL_FILTER_DECAL_BLACK_KEY) != 0u;
    const float effect = blackKey
        ? max(max(decalAlbedo.r, decalAlbedo.g), decalAlbedo.b)
        : 1.0 - min(min(decalAlbedo.r, decalAlbedo.g), decalAlbedo.b);
    const float coverage = saturate(effect);
    surface.material.diffuseAlbedo = lerp(
        surface.material.diffuseAlbedo,
        surface.material.diffuseAlbedo * max(decalAlbedo, float3(0.12, 0.12, 0.12)),
        coverage);
}

bool SmokePrimaryFilterDecalCompositeEnabled(uint debugMode)
{
    return debugMode == 0u ||
        debugMode == 18u;
}

#define RB_PATH_TRACE_OPAQUE_DIRECT_ENABLE_OPENPBR 1
#define RB_PATH_TRACE_OPAQUE_DIRECT_BRDF_MODE 4
#define RB_PATH_TRACE_OPAQUE_DIRECT_RUNTIME_MODE ((((uint)TextureInfo.w) >> 9u) & 7u)

#include "pathtrace_emissive_sampling.hlsli"
#include "pathtrace_smoke_rab_environment_stub.hlsli"

#include "RtxdiBridge/RAB_LightTarget.hlsli"
#include "RtxdiBridge/RAB_Visibility.hlsli"
#include "RtxdiBridge/RAB_LocalLightPdf.hlsli"

uint LoadSmokeTriangleMaterialIndex(uint instanceId, uint primitiveIndex)
{
    const uint materialCount = (uint)TextureInfo.z;
    uint materialIndex = 0xffffffffu;
    if (instanceId == 0u)
    {
        if (primitiveIndex >= PathTraceStaticTriangleCount())
        {
            return 0xffffffffu;
        }
        materialIndex = SmokeStaticTriangleMaterialIndexes[primitiveIndex];
    }
    else if (PathTraceIsSkinnedHitRouteInstance(instanceId))
    {
        PathTraceSkinnedHitRouteGpuRecord route;
        PathTraceSkinnedHitRouteGpuTriangle routeTriangle;
        if (!PathTraceLoadSkinnedHitRoute(instanceId, route) ||
            !PathTraceLoadSkinnedHitRouteTriangle(
                route,
                primitiveIndex,
                routeTriangle))
        {
            return 0xffffffffu;
        }
        materialIndex = routeTriangle.materialIndex;
    }
    else if (PathTraceIsRigidHitRouteInstance(instanceId))
    {
        const uint routeInstanceIndex = instanceId - 2u;
        if (routeInstanceIndex >= PathTraceRigidRouteInstanceCount())
        {
            return 0xffffffffu;
        }
        const PathTraceRigidRouteInstance routeInstance = SmokeRigidRouteInstances[routeInstanceIndex];
        if (primitiveIndex >= routeInstance.triangleCount ||
            routeInstance.triangleOffset + primitiveIndex >= PathTraceRigidRouteTriangleCount())
        {
            return 0xffffffffu;
        }
        materialIndex = SmokeRigidRouteTriangleMaterialIndexes[routeInstance.triangleOffset + primitiveIndex];
    }
    else
    {
        if (primitiveIndex >= PathTraceDynamicTriangleCount())
        {
            return 0xffffffffu;
        }
        materialIndex = SmokeDynamicTriangleMaterialIndexes[primitiveIndex];
    }
    return materialIndex < materialCount ? materialIndex : 0xffffffffu;
}

uint LoadSmokeTriangleMaterialId(uint instanceId, uint primitiveIndex)
{
    if (instanceId == 0u)
    {
        if (primitiveIndex >= PathTraceStaticTriangleCount())
        {
            return 0xffffffffu;
        }
        return SmokeStaticTriangleMaterials[primitiveIndex];
    }
    if (instanceId == 1u)
    {
        return primitiveIndex < PathTraceDynamicTriangleCount()
            ? SmokeDynamicTriangleMaterials[primitiveIndex]
            : 0xffffffffu;
    }
    if (PathTraceIsSkinnedHitRouteInstance(instanceId))
    {
        PathTraceSkinnedHitRouteGpuRecord route;
        PathTraceSkinnedHitRouteGpuTriangle routeTriangle;
        return PathTraceLoadSkinnedHitRoute(instanceId, route) &&
            PathTraceLoadSkinnedHitRouteTriangle(
                route,
                primitiveIndex,
                routeTriangle)
            ? routeTriangle.materialId
            : 0xffffffffu;
    }
    if (PathTraceIsRigidHitRouteInstance(instanceId))
    {
        const uint routeInstanceIndex = instanceId - 2u;
        if (routeInstanceIndex >= PathTraceRigidRouteInstanceCount())
        {
            return 0xffffffffu;
        }
        const PathTraceRigidRouteInstance routeInstance = SmokeRigidRouteInstances[routeInstanceIndex];
        if (primitiveIndex >= routeInstance.triangleCount ||
            routeInstance.triangleOffset + primitiveIndex >= PathTraceRigidRouteTriangleCount())
        {
            return 0xffffffffu;
        }
        return SmokeRigidRouteTriangleMaterials[routeInstance.triangleOffset + primitiveIndex];
    }
    return 0xffffffffu;
}

uint LoadSmokeTriangleClassAndFlags(uint instanceId, uint primitiveIndex)
{
    if (PathTraceIsSkinnedHitRouteInstance(instanceId))
    {
        PathTraceSkinnedHitRouteGpuRecord route;
        PathTraceSkinnedHitRouteGpuTriangle routeTriangle;
        return PathTraceLoadSkinnedHitRoute(instanceId, route) &&
            PathTraceLoadSkinnedHitRouteTriangle(
                route,
                primitiveIndex,
                routeTriangle)
            ? routeTriangle.triangleClassAndFlags
            : 0u;
    }
    if (PathTraceIsRigidHitRouteInstance(instanceId))
    {
        return RT_SMOKE_SURFACE_CLASS_RIGID_ENTITY;
    }
    if (instanceId == 0u && primitiveIndex >= PathTraceStaticTriangleCount())
    {
        return 0u;
    }
    if (instanceId == 1u && primitiveIndex >= PathTraceDynamicTriangleCount())
    {
        return 0u;
    }
    return instanceId == 0u
        ? SmokeStaticTriangleClasses[primitiveIndex]
        : (instanceId == 1u
            ? SmokeDynamicTriangleClasses[primitiveIndex]
            : 0u);
}

bool SmokeTriangleIndexRangeValid(uint instanceId, uint primitiveIndex)
{
    if (instanceId == 0u)
    {
        const uint indexOffset = primitiveIndex * 3u;
        return primitiveIndex < PathTraceStaticTriangleCount() &&
            indexOffset <= PathTraceStaticIndexCount() &&
            indexOffset + 2u < PathTraceStaticIndexCount();
    }
    if (instanceId == 1u)
    {
        const uint indexOffset = primitiveIndex * 3u;
        return primitiveIndex < PathTraceDynamicTriangleCount() &&
            indexOffset <= PathTraceDynamicIndexCount() &&
            indexOffset + 2u < PathTraceDynamicIndexCount();
    }
    if (PathTraceIsSkinnedHitRouteInstance(instanceId))
    {
        PathTraceSkinnedHitRouteGpuRecord route;
        PathTraceSkinnedHitRouteGpuTriangle routeTriangle;
        uint vertexIndex0;
        uint vertexIndex1;
        uint vertexIndex2;
        return PathTraceLoadSkinnedHitRouteTriangleData(
            instanceId,
            primitiveIndex,
            route,
            routeTriangle,
            vertexIndex0,
            vertexIndex1,
            vertexIndex2);
    }
    if (!PathTraceIsRigidHitRouteInstance(instanceId))
    {
        return false;
    }
    const uint routeInstanceIndex = instanceId - 2u;
    if (routeInstanceIndex >= PathTraceRigidRouteInstanceCount())
    {
        return false;
    }
    const PathTraceRigidRouteInstance routeInstance = SmokeRigidRouteInstances[routeInstanceIndex];
    const uint routeIndexOffset = routeInstance.indexOffset + primitiveIndex * 3u;
    return primitiveIndex < routeInstance.triangleCount &&
        routeInstance.triangleOffset + primitiveIndex < PathTraceRigidRouteTriangleCount() &&
        routeIndexOffset <= PathTraceRigidRouteIndexCount() &&
        routeIndexOffset + 2u < PathTraceRigidRouteIndexCount();
}

float2 InterpolateSmokeTexCoord(uint instanceId, uint primitiveIndex, float2 barycentrics)
{
    if (!SmokeTriangleIndexRangeValid(instanceId, primitiveIndex))
    {
        return float2(0.0, 0.0);
    }
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
            return float2(0.0, 0.0);
        }
        const float3 weights = float3(
            1.0 - barycentrics.x - barycentrics.y,
            barycentrics.x,
            barycentrics.y);
        return
            SmokeSkinnedCurrentVertices[vertexIndex0].texCoord.xy * weights.x +
            SmokeSkinnedCurrentVertices[vertexIndex1].texCoord.xy * weights.y +
            SmokeSkinnedCurrentVertices[vertexIndex2].texCoord.xy * weights.z;
    }
    if (PathTraceIsRigidHitRouteInstance(instanceId))
    {
        const PathTraceRigidRouteInstance routeInstance = SmokeRigidRouteInstances[instanceId - 2u];
        const uint indexOffset = routeInstance.indexOffset + primitiveIndex * 3u;
        const uint i0 = SmokeRigidRouteIndices[indexOffset + 0u];
        const uint i1 = SmokeRigidRouteIndices[indexOffset + 1u];
        const uint i2 = SmokeRigidRouteIndices[indexOffset + 2u];
        if (i0 >= routeInstance.vertexCount || i1 >= routeInstance.vertexCount || i2 >= routeInstance.vertexCount ||
            routeInstance.vertexOffset + i0 >= PathTraceRigidRouteVertexCount() ||
            routeInstance.vertexOffset + i1 >= PathTraceRigidRouteVertexCount() ||
            routeInstance.vertexOffset + i2 >= PathTraceRigidRouteVertexCount())
        {
            return float2(0.0, 0.0);
        }
        const float2 uv0 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i0].texCoord.xy;
        const float2 uv1 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i1].texCoord.xy;
        const float2 uv2 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i2].texCoord.xy;
        const float3 weights = float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
        return uv0 * weights.x + uv1 * weights.y + uv2 * weights.z;
    }
    const uint indexOffset = primitiveIndex * 3;
    const uint i0 = instanceId == 0 ? SmokeStaticIndices[indexOffset + 0] : SmokeDynamicIndices[indexOffset + 0];
    const uint i1 = instanceId == 0 ? SmokeStaticIndices[indexOffset + 1] : SmokeDynamicIndices[indexOffset + 1];
    const uint i2 = instanceId == 0 ? SmokeStaticIndices[indexOffset + 2] : SmokeDynamicIndices[indexOffset + 2];
    const uint vertexCount = instanceId == 0 ? PathTraceStaticVertexCount() : PathTraceDynamicVertexCount();
    if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
    {
        return float2(0.0, 0.0);
    }
    const float2 uv0 = (instanceId == 0 ? SmokeStaticVertices[i0].texCoord : SmokeDynamicVertices[i0].texCoord).xy;
    const float2 uv1 = (instanceId == 0 ? SmokeStaticVertices[i1].texCoord : SmokeDynamicVertices[i1].texCoord).xy;
    const float2 uv2 = (instanceId == 0 ? SmokeStaticVertices[i2].texCoord : SmokeDynamicVertices[i2].texCoord).xy;
    const float3 weights = float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
    return uv0 * weights.x + uv1 * weights.y + uv2 * weights.z;
}

float4 InterpolateSmokeVertexColor(uint instanceId, uint primitiveIndex, float2 barycentrics)
{
    if (!SmokeTriangleIndexRangeValid(instanceId, primitiveIndex))
    {
        return float4(1.0, 1.0, 1.0, 1.0);
    }
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
            return float4(1.0, 1.0, 1.0, 1.0);
        }
        const float3 weights = float3(
            1.0 - barycentrics.x - barycentrics.y,
            barycentrics.x,
            barycentrics.y);
        return saturate(
            SmokeSkinnedCurrentVertices[vertexIndex0].color * weights.x +
            SmokeSkinnedCurrentVertices[vertexIndex1].color * weights.y +
            SmokeSkinnedCurrentVertices[vertexIndex2].color * weights.z);
    }
    if (PathTraceIsRigidHitRouteInstance(instanceId))
    {
        const PathTraceRigidRouteInstance routeInstance = SmokeRigidRouteInstances[instanceId - 2u];
        const uint indexOffset = routeInstance.indexOffset + primitiveIndex * 3u;
        const uint i0 = SmokeRigidRouteIndices[indexOffset + 0u];
        const uint i1 = SmokeRigidRouteIndices[indexOffset + 1u];
        const uint i2 = SmokeRigidRouteIndices[indexOffset + 2u];
        if (i0 >= routeInstance.vertexCount || i1 >= routeInstance.vertexCount || i2 >= routeInstance.vertexCount ||
            routeInstance.vertexOffset + i0 >= PathTraceRigidRouteVertexCount() ||
            routeInstance.vertexOffset + i1 >= PathTraceRigidRouteVertexCount() ||
            routeInstance.vertexOffset + i2 >= PathTraceRigidRouteVertexCount())
        {
            return float4(1.0, 1.0, 1.0, 1.0);
        }
        const float4 c0 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i0].color;
        const float4 c1 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i1].color;
        const float4 c2 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i2].color;
        const float3 weights = float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
        return saturate(c0 * weights.x + c1 * weights.y + c2 * weights.z);
    }
    const uint indexOffset = primitiveIndex * 3;
    const uint i0 = instanceId == 0 ? SmokeStaticIndices[indexOffset + 0] : SmokeDynamicIndices[indexOffset + 0];
    const uint i1 = instanceId == 0 ? SmokeStaticIndices[indexOffset + 1] : SmokeDynamicIndices[indexOffset + 1];
    const uint i2 = instanceId == 0 ? SmokeStaticIndices[indexOffset + 2] : SmokeDynamicIndices[indexOffset + 2];
    const uint vertexCount = instanceId == 0 ? PathTraceStaticVertexCount() : PathTraceDynamicVertexCount();
    if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
    {
        return float4(1.0, 1.0, 1.0, 1.0);
    }
    const float4 c0 = instanceId == 0 ? SmokeStaticVertices[i0].color : SmokeDynamicVertices[i0].color;
    const float4 c1 = instanceId == 0 ? SmokeStaticVertices[i1].color : SmokeDynamicVertices[i1].color;
    const float4 c2 = instanceId == 0 ? SmokeStaticVertices[i2].color : SmokeDynamicVertices[i2].color;
    const float3 weights = float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
    return saturate(c0 * weights.x + c1 * weights.y + c2 * weights.z);
}

bool SmokeGuiRejectsTransparentHit(uint instanceId, uint primitiveIndex, float2 barycentrics, uint triangleClassAndFlags)
{
    const uint surfaceClass = triangleClassAndFlags & RT_SMOKE_TRIANGLE_CLASS_MASK;
    const uint translucentSubtype = (triangleClassAndFlags & RT_SMOKE_TRANSLUCENT_SUBTYPE_MASK) >> RT_SMOKE_TRANSLUCENT_SUBTYPE_SHIFT;
    if (surfaceClass != RT_SMOKE_SURFACE_CLASS_TRANSLUCENT || translucentSubtype != RT_SMOKE_TRANSLUCENT_SUBTYPE_GUI_SCREEN)
    {
        return false;
    }

    return InterpolateSmokeVertexColor(instanceId, primitiveIndex, barycentrics).a <= 0.03;
}

bool SmokeParticleDitherRejectsHit(PathTraceSmokeMaterial material, float2 texCoord, float2 barycentrics, uint instanceId, uint primitiveIndex, uint triangleClassAndFlags, bool shadowRay)
{
    const uint smokeParticleFlags = (uint)LightInfo.w;
    if ((smokeParticleFlags & 1u) == 0u)
    {
        return false;
    }

    const uint surfaceClass = triangleClassAndFlags & RT_SMOKE_TRIANGLE_CLASS_MASK;
    const uint translucentSubtype = (triangleClassAndFlags & RT_SMOKE_TRANSLUCENT_SUBTYPE_MASK) >> RT_SMOKE_TRANSLUCENT_SUBTYPE_SHIFT;
    if (surfaceClass != RT_SMOKE_SURFACE_CLASS_TRANSLUCENT || translucentSubtype != RT_SMOKE_TRANSLUCENT_SUBTYPE_SMOKE_PARTICLE)
    {
        return false;
    }

    if (shadowRay)
    {
        return true;
    }

    const float textureMask = SmokeAlphaCoverage(material, texCoord);
    const float2 wrappedTexCoord = frac(abs(texCoord));
    const float2 edgeDistance = min(wrappedTexCoord, 1.0 - wrappedTexCoord);
    const float edgeFade = (smokeParticleFlags & 2u) != 0u
        ? saturate(min(edgeDistance.x, edgeDistance.y) * 8.0)
        : 1.0;
    const float alpha = saturate(textureMask * edgeFade * saturate(LightInfo.z));
    const uint2 ditherCell = uint2(floor(wrappedTexCoord * 128.0));
    const uint2 baryCell = uint2(saturate(barycentrics) * 255.0);
    const uint hash =
        primitiveIndex * 747796405u ^
        instanceId * 2891336453u ^
        ditherCell.x * 277803737u ^
        ditherCell.y * 3266489917u ^
        baryCell.x * 668265263u ^
        baryCell.y * 2246822519u;
    return alpha < SmokeHashToUnitFloat(SmokeAlphaStochasticHash(hash, 23u));
}

bool SmokeGlassFallbackRejectsHit(PathTraceSmokeMaterial material, float2 texCoord, float2 barycentrics, uint instanceId, uint primitiveIndex, uint triangleClassAndFlags, bool shadowRay)
{
    if (PortalWindowInfo.x <= 0.0)
    {
        return false;
    }

    const uint surfaceClass = triangleClassAndFlags & RT_SMOKE_TRIANGLE_CLASS_MASK;
    const uint translucentSubtype = (triangleClassAndFlags & RT_SMOKE_TRANSLUCENT_SUBTYPE_MASK) >> RT_SMOKE_TRANSLUCENT_SUBTYPE_SHIFT;
    if (surfaceClass != RT_SMOKE_SURFACE_CLASS_TRANSLUCENT)
    {
        return false;
    }

    const bool portalWindow = translucentSubtype == RT_SMOKE_TRANSLUCENT_SUBTYPE_PORTAL_WINDOW;
    const bool objectGlass =
        translucentSubtype == RT_SMOKE_TRANSLUCENT_SUBTYPE_OBJECT_GLASS &&
        (material.flags & RT_SMOKE_MATERIAL_OBJECT_GLASS_FALLBACK) != 0u;
    if (!portalWindow && !objectGlass)
    {
        return false;
    }

    const float4 alphaTexel = SampleSmokeAlphaTexture(material, texCoord);
    const float textureMask = saturate(max(alphaTexel.a, max(max(alphaTexel.r, alphaTexel.g), alphaTexel.b)));
    float opacity = max(saturate(textureMask * PortalWindowInfo.y), saturate(PortalWindowInfo.z));
    if ((material.flags & RT_SMOKE_MATERIAL_PORTAL_WINDOW_FALLBACK) != 0u)
    {
        opacity = max(opacity, 0.18);
    }
    if (objectGlass)
    {
        opacity = max(saturate(textureMask * 0.22), 0.12);
    }
    if (shadowRay)
    {
        opacity *= saturate(PortalWindowInfo.w);
    }

    const float2 wrappedTexCoord = frac(abs(texCoord));
    const uint2 ditherCell = uint2(floor(wrappedTexCoord * 192.0));
    const uint2 baryCell = uint2(saturate(barycentrics) * 255.0);
    const uint hash =
        primitiveIndex * 1597334677u ^
        instanceId * 3812015801u ^
        ditherCell.x * 747796405u ^
        ditherCell.y * 2891336453u ^
        baryCell.x * 277803737u ^
        baryCell.y * 3266489917u;
    return opacity < SmokeHashToUnitFloat(SmokeAlphaStochasticHash(hash, 37u));
}

uint SmokeAlphaRejectReason(uint instanceId, uint primitiveIndex, float2 barycentrics, uint rayMode)
{
    if (PathTraceSafetyDisabled(RT_PT_SAFETY_DISABLE_ANY_HIT_ALPHA) ||
        !SmokeTriangleIndexRangeValid(instanceId, primitiveIndex))
    {
        return RT_STATIC_CONTRACT_REJECT_NONE;
    }

    const bool shadowRay = rayMode != 0u && rayMode != RT_SMOKE_RAY_MODE_PRIMARY_FILTER_DECAL_COMPOSITE;
    const PathTraceSmokeMaterial material = LoadSmokeMaterial(LoadSmokeTriangleMaterialIndex(instanceId, primitiveIndex));
    const float2 texCoord = InterpolateSmokeTexCoord(instanceId, primitiveIndex, barycentrics);
    const uint triangleClassAndFlags = LoadSmokeTriangleClassAndFlags(instanceId, primitiveIndex);
    if (SmokeGuiRejectsTransparentHit(instanceId, primitiveIndex, barycentrics, triangleClassAndFlags))
    {
        return RT_STATIC_CONTRACT_REJECT_GUI_ALPHA;
    }

    if (SmokeParticleDitherRejectsHit(material, texCoord, barycentrics, instanceId, primitiveIndex, triangleClassAndFlags, shadowRay))
    {
        return RT_STATIC_CONTRACT_REJECT_PARTICLE_DITHER;
    }

    if (SmokeGlassFallbackRejectsHit(material, texCoord, barycentrics, instanceId, primitiveIndex, triangleClassAndFlags, shadowRay))
    {
        return RT_STATIC_CONTRACT_REJECT_GLASS_FALLBACK;
    }

    if (SmokeAdditiveDecalRejectsHit(material, texCoord, triangleClassAndFlags, shadowRay))
    {
        return RT_STATIC_CONTRACT_REJECT_ADDITIVE_DECAL;
    }

    if (SmokeFilterDecalRejectsHit(material, texCoord, barycentrics, instanceId, primitiveIndex, shadowRay, rayMode))
    {
        return RT_STATIC_CONTRACT_REJECT_FILTER_DECAL;
    }

    if ((material.flags & RT_SMOKE_MATERIAL_ALPHA_TEST) == 0u)
    {
        return RT_STATIC_CONTRACT_REJECT_NONE;
    }

    return SmokeAlphaCoverage(material, texCoord) < material.alphaCutoff
        ? RT_STATIC_CONTRACT_REJECT_ALPHA_TEST
        : RT_STATIC_CONTRACT_REJECT_NONE;
}

bool SmokeAlphaRejectsHit(uint instanceId, uint primitiveIndex, float2 barycentrics, uint rayMode)
{
    return SmokeAlphaRejectReason(instanceId, primitiveIndex, barycentrics, rayMode) != RT_STATIC_CONTRACT_REJECT_NONE;
}

bool SmokePayloadIsGuiScreen(PathTraceSmokePayload payload);
float4 CompositeSmokeGuiLayers(float3 rayOrigin, float3 rayDirection, PathTraceSmokePayload firstPayload);

float3 EvaluateSmokeLightSpriteProxies(float3 rayOrigin, float3 rayDirection, float sceneHitT)
{
    if (LightSpriteInfo.x <= 0.0 || PathTraceSafetyDisabled(RT_PT_SAFETY_DISABLE_SELECTED_LIGHT_LOOP))
    {
        return float3(0.0, 0.0, 0.0);
    }

    float3 sprites = float3(0.0, 0.0, 0.0);
    const uint lightCount = PathTraceSafetyDisabled(RT_PT_SAFETY_DISABLE_SELECTED_LIGHT_LOOP) ? 0u : min((uint)LightInfo.x, RT_SMOKE_MAX_DEBUG_LIGHTS);
    [loop]
    for (uint lightIndex = 0u; lightIndex < lightCount; lightIndex++)
    {
        const float4 lightOriginAndRadius = LightOriginAndRadius[lightIndex];
        const float spriteWeight = LightColorAndIntensity[lightIndex].w;
        if (spriteWeight <= 0.0)
        {
            continue;
        }

        const float3 toLight = lightOriginAndRadius.xyz - rayOrigin;
        const float t = dot(toLight, rayDirection);
        if (t <= 0.0 || t >= sceneHitT)
        {
            continue;
        }

        const float3 closest = rayOrigin + rayDirection * t;
        const float distanceToRay = length(lightOriginAndRadius.xyz - closest);
        const float proxyRadius = clamp(lightOriginAndRadius.w * LightSpriteInfo.y, 2.0, 96.0);
        const float core = saturate(1.0 - distanceToRay / proxyRadius);
        const float glow = core * core * (3.0 - 2.0 * core);
        const float distanceFade = saturate(1.0 - t / max(CameraOriginAndTMax.w, 1.0));
        const float3 lightColor = max(LightColorAndIntensity[lightIndex].rgb, float3(0.0, 0.0, 0.0));
        sprites += lightColor * glow * (0.35 + distanceFade * 0.65) * LightSpriteInfo.z * spriteWeight;
    }

    return sprites;
}

float3 SmokeCosineHemisphereDirection(float3 normal, uint seed)
{
    const float r1 = SmokeHashToUnitFloat(seed ^ 0x9e3779b9u);
    const float r2 = SmokeHashToUnitFloat(seed ^ 0x85ebca6bu);
    const float phi = 6.2831853 * r1;
    const float radius = sqrt(r2);
    const float x = cos(phi) * radius;
    const float y = sin(phi) * radius;
    const float z = sqrt(max(0.0, 1.0 - r2));
    const float3 tangent = BuildPerpendicular(normal);
    const float3 bitangent = SafeNormalize(cross(normal, tangent), float3(0.0, 1.0, 0.0));
    return SafeNormalize(tangent * x + bitangent * y + normal * z, normal);
}

float SmokeRaySphereHitT(float3 rayOrigin, float3 rayDirection, float3 sphereCenter, float sphereRadius, float fallbackT)
{
    const float3 oc = rayOrigin - sphereCenter;
    const float b = dot(oc, rayDirection);
    const float c = dot(oc, oc) - sphereRadius * sphereRadius;
    const float h = b * b - c;
    if (h <= 0.0)
    {
        return fallbackT;
    }

    const float nearT = -b - sqrt(h);
    return nearT > 0.01 ? nearT : fallbackT;
}

float3 SmokeSampleSphereSolidAngle(float3 axis, float cosThetaMax, uint seed)
{
    const float xi1 = SmokeHashToUnitFloat(seed ^ 0x27d4eb2du);
    const float xi2 = SmokeHashToUnitFloat(seed ^ 0x165667b1u);
    const float cosTheta = lerp(1.0, cosThetaMax, xi1);
    const float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    const float phi = 6.2831853 * xi2;
    const float3 tangent = BuildPerpendicular(axis);
    const float3 bitangent = SafeNormalize(cross(axis, tangent), float3(0.0, 1.0, 0.0));
    return SafeNormalize(axis * cosTheta + tangent * (cos(phi) * sinTheta) + bitangent * (sin(phi) * sinTheta), axis);
}

#if defined(RB_PT_RESTIR_PRODUCER_ONLY) && !defined(RB_PT_KEEP_LEGACY_RESTIR_DEBUG_CODE)
bool SmokePayloadIsGuiScreen(PathTraceSmokePayload payload)
{
    return payload.value != 0u &&
        payload.surfaceClass == RT_SMOKE_SURFACE_CLASS_TRANSLUCENT &&
        payload.translucentSubtype == RT_SMOKE_TRANSLUCENT_SUBTYPE_GUI_SCREEN;
}
#endif

#ifdef RB_PT_KEEP_LEGACY_RESTIR_DEBUG_CODE
#include "pathtrace_nee.hlsli"

static const uint RT_PT_TOY_FLAG_DIFFUSE_HIT = 0x00000001u;
static const uint RT_PT_TOY_FLAG_REFLECTION_HIT = 0x00000002u;
static const uint RT_PT_TOY_FLAG_REFLECTION_MISS = 0x00000004u;
static const uint RT_PT_TOY_FLAG_REFLECTION_GATED = 0x00000008u;
static const uint RT_PT_TOY_FLAG_MAX_DEPTH_TERMINATED = 0x00000010u;
static const uint RT_PT_TOY_FLAG_RR_CONFIGURED = 0x00000020u;
static const uint RT_PT_TOY_FLAG_TRANSMISSION_HIT = 0x00000040u;
static const uint RT_PT_TOY_FLAG_TRANSMISSION_MISS = 0x00000080u;
static const uint RT_PT_TOY_FLAG_TRANSMISSION_GATED = 0x00000100u;

static const uint RT_PT_GLASS_EVENT_NONE = 0u;
static const uint RT_PT_GLASS_EVENT_REFLECTION = 1u;
static const uint RT_PT_GLASS_EVENT_TRANSMISSION = 2u;

float3 SmokeGlassFailClosedColor()
{
    return float3(0.05, 0.75, 1.0);
}

bool BuildSmokeGlassPathEvent(
    RAB_Surface surface,
    PathTraceSmokeMaterial material,
    uint bounceSeed,
    out float3 eventDir,
    out float3 eventWeight,
    out uint eventKind)
{
    eventDir = float3(0.0, 0.0, 0.0);
    eventWeight = float3(0.0, 0.0, 0.0);
    eventKind = RT_PT_GLASS_EVENT_NONE;

    if (!MaterialSupportsTransmission(surface) || PathTraceIntegratorMaxPathDepth() <= 1u)
    {
        return false;
    }

    const bool canTransmit = PathTraceIntegratorTransmissionBounceLimit() > 0u;
    const bool canReflect =
        PathTraceIntegratorReflectionMode() > 0u &&
        PathTraceIntegratorSpecularBounceLimit() > 0u;
    if (!canTransmit && !canReflect)
    {
        return false;
    }

    const float3 normal = RAB_SafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    const float3 viewDir = RAB_SafeNormalize(RAB_GetSurfaceViewDir(surface), normal);
    const float3 incomingDir = -viewDir;
    const float cosTheta = saturate(dot(viewDir, normal));
    const float f0 = 0.04;
    const float fresnel = saturate(f0 + (1.0 - f0) * pow(1.0 - cosTheta, 5.0));
    const float eventSelect = SmokeHashToUnitFloat(bounceSeed ^ 0x6c8e9cf5u);

    if (canReflect && (!canTransmit || eventSelect < fresnel))
    {
        eventDir = RAB_SafeNormalize(reflect(incomingDir, normal), normal);
        if (dot(eventDir, normal) <= 0.0 || dot(eventDir, RAB_GetSurfaceGeoNormal(surface)) <= -0.05)
        {
            return false;
        }

        eventWeight = float3(fresnel, fresnel, fresnel);
        eventKind = RT_PT_GLASS_EVENT_REFLECTION;
        return true;
    }

    if (!canTransmit)
    {
        return false;
    }

    const float eta = 1.0 / 1.45;
    eventDir = refract(incomingDir, normal, eta);
    if (dot(eventDir, eventDir) <= 1.0e-8)
    {
        if (!canReflect)
        {
            return false;
        }

        eventDir = RAB_SafeNormalize(reflect(incomingDir, normal), normal);
        eventWeight = float3(1.0, 1.0, 1.0);
        eventKind = RT_PT_GLASS_EVENT_REFLECTION;
        return true;
    }

    eventDir = RAB_SafeNormalize(eventDir, incomingDir);
    const float transmission = max(SmokeMatClassTransmission(material), 0.65);
    eventWeight = saturate(surface.material.diffuseAlbedo * transmission) * (1.0 - fresnel);
    eventWeight = max(eventWeight, float3(0.12, 0.12, 0.14));
    eventKind = RT_PT_GLASS_EVENT_TRANSMISSION;
    return true;
}

float4 EvaluateSmokeToyPathTrace(float3 rayOrigin, float3 rayDirection, PathTraceSmokePayload primaryPayload, uint2 pixel, uint sampleIndex, out uint pathDepth, out uint pathFlags)
{
    pathDepth = primaryPayload.value != 0u ? 1u : 0u;
    pathFlags = PathTraceIntegratorRussianRouletteDepth() > 0u ? RT_PT_TOY_FLAG_RR_CONFIGURED : 0u;

    if (SmokePayloadIsGuiScreen(primaryPayload))
    {
        return CompositeSmokeGuiLayers(rayOrigin, rayDirection, primaryPayload);
    }

    const PathTraceSmokeMaterial primaryMaterial = LoadSmokeMaterial(primaryPayload.materialIndex);
    const float3 primaryAlbedo = SampleSmokeSurfaceAlbedo(primaryMaterial, primaryPayload.texCoord, primaryPayload.surfaceClass, primaryPayload.translucentSubtype, primaryPayload.vertexColor, primaryPayload.vertexColorAdd).rgb;
    const float3 baseNormal = SafeNormalize(primaryPayload.normal, primaryPayload.geometricNormal);
    const float3 primaryNormal = DecodeSmokeNormalTexture(primaryMaterial, primaryPayload.normalTexCoord, baseNormal, primaryPayload.tangent, primaryPayload.bitangent);
    const float3 primaryHit = rayOrigin + rayDirection * primaryPayload.hitT;
    const uint bounceSeed =
        pixel.x * 1973u ^
        pixel.y * 9277u ^
        primaryPayload.materialId * 26699u ^
        ((uint)primaryPayload.hitT) * 7919u ^
        sampleIndex * 374761393u ^
        ((uint)max(ToyPathInfo.w, 0.0)) * 104729u;
    const bool useFakePBRSpecular = SmokeToyFakePBRSpecularEnabled();
    const bool allowSecondary = PathTraceIntegratorMaxPathDepth() > 1u;
    const RAB_Surface primarySurface = RAB_BuildSurfaceFromSmokePayload(primaryPayload, rayOrigin, rayDirection, true);
    if (MaterialSupportsTransmission(primarySurface))
    {
        float3 glassEventDir;
        float3 glassEventWeight;
        uint glassEventKind;
        if (allowSecondary && BuildSmokeGlassPathEvent(primarySurface, primaryMaterial, bounceSeed, glassEventDir, glassEventWeight, glassEventKind))
        {
            PathTraceSmokePayload glassPayload = InitSmokePayload();
            RayDesc glassRay;
            glassRay.Origin = primaryHit + glassEventDir * 0.75;
            glassRay.Direction = glassEventDir;
            glassRay.TMin = 0.01;
            glassRay.TMax = min(CameraOriginAndTMax.w, max(ToyPathInfo.x, 64.0));
            TraceRay(SmokeScene, RAY_FLAG_NONE, 0xff, 0, 1, 0, glassRay, glassPayload);

            if (glassPayload.value != 0u && !SmokePayloadIsGuiScreen(glassPayload))
            {
                pathDepth = max(pathDepth, 2u);
                pathFlags |= glassEventKind == RT_PT_GLASS_EVENT_REFLECTION ? RT_PT_TOY_FLAG_REFLECTION_HIT : RT_PT_TOY_FLAG_TRANSMISSION_HIT;
                const float3 glassDirect = EvaluateSmokeMode18NeeDirectLighting(
                    glassPayload,
                    glassRay.Origin,
                    glassRay.Direction,
                    true,
                    useFakePBRSpecular,
                    true,
                    bounceSeed ^ 0x85ebca6bu,
                    PathTraceIntegratorSecondaryNeeMode(),
                    PathTraceIntegratorSecondaryAnalyticNeeMode());
                const float3 glassSprites = EvaluateSmokeLightSpriteProxies(glassRay.Origin, glassRay.Direction, glassPayload.hitT) * 0.25;
                return float4(saturate((glassDirect + glassSprites) * glassEventWeight), 1.0);
            }

            pathFlags |= glassEventKind == RT_PT_GLASS_EVENT_REFLECTION ? RT_PT_TOY_FLAG_REFLECTION_MISS : RT_PT_TOY_FLAG_TRANSMISSION_MISS;
            return float4(SmokeGlassFailClosedColor() * 0.18, 1.0);
        }

        pathFlags |= allowSecondary ? RT_PT_TOY_FLAG_TRANSMISSION_GATED : RT_PT_TOY_FLAG_MAX_DEPTH_TERMINATED;
        return float4(SmokeGlassFailClosedColor(), 1.0);
    }

    const float3 nativePrimaryDirect = EvaluateSmokeMode18NeeDirectLighting(primaryPayload, rayOrigin, rayDirection, true, useFakePBRSpecular, true, bounceSeed, SMOKE_NEE_SELECTED_LIGHT_MODE_LEGACY_FULL, SMOKE_NEE_ANALYTIC_LIGHT_MODE_LEGACY_FULL);
    float3 radiance = nativePrimaryDirect;

    if (allowSecondary && PathTraceIntegratorDiffuseBounceLimit() > 0u)
    {
        const float3 bounceDir = SmokeCosineHemisphereDirection(primaryNormal, bounceSeed);
        PathTraceSmokePayload bouncePayload = InitSmokePayload();
        RayDesc bounceRay;
        bounceRay.Origin = primaryHit + primaryNormal * 0.75 + bounceDir * 0.25;
        bounceRay.Direction = bounceDir;
        bounceRay.TMin = 0.01;
        bounceRay.TMax = min(CameraOriginAndTMax.w, max(ToyPathInfo.x, 64.0));
        TraceRay(SmokeScene, RAY_FLAG_NONE, 0xff, 0, 1, 0, bounceRay, bouncePayload);

        if (bouncePayload.value != 0u && !SmokePayloadIsGuiScreen(bouncePayload))
        {
            pathDepth = max(pathDepth, 2u);
            pathFlags |= RT_PT_TOY_FLAG_DIFFUSE_HIT;
            const PathTraceSmokeMaterial bounceMaterial = LoadSmokeMaterial(bouncePayload.materialIndex);
            const float3 bounceAlbedo = SampleSmokeSurfaceAlbedo(bounceMaterial, bouncePayload.texCoord, bouncePayload.surfaceClass, bouncePayload.translucentSubtype, bouncePayload.vertexColor, bouncePayload.vertexColorAdd).rgb;
            const float3 bounceDirect = EvaluateSmokeMode18NeeDirectLighting(
                bouncePayload,
                bounceRay.Origin,
                bounceRay.Direction,
                true,
                useFakePBRSpecular,
                true,
                bounceSeed ^ 0x9e3779b9u,
                PathTraceIntegratorSecondaryNeeMode(),
                PathTraceIntegratorSecondaryAnalyticNeeMode()
            );
            radiance += primaryAlbedo * bounceDirect * (0.28 + 0.22 * max(max(bounceAlbedo.r, bounceAlbedo.g), bounceAlbedo.b));
        }
    }
    else if (PathTraceIntegratorDiffuseBounceLimit() > 0u)
    {
        pathFlags |= RT_PT_TOY_FLAG_MAX_DEPTH_TERMINATED;
    }

    float3 reflectionDir;
    float3 reflectionWeight;
    float reflectionRoughness;
    if (BuildSmokeReflectionBounce(primaryMaterial, primaryPayload, primaryNormal, rayDirection, reflectionDir, reflectionWeight, reflectionRoughness))
    {
        PathTraceSmokePayload reflectionPayload = InitSmokePayload();
        RayDesc reflectionRay;
        reflectionRay.Origin = primaryHit + primaryNormal * 0.75 + reflectionDir * 0.25;
        reflectionRay.Direction = reflectionDir;
        reflectionRay.TMin = 0.01;
        reflectionRay.TMax = min(CameraOriginAndTMax.w, max(ToyPathInfo.x, 64.0));
        TraceRay(SmokeScene, RAY_FLAG_NONE, 0xff, 0, 1, 0, reflectionRay, reflectionPayload);

        if (reflectionPayload.value != 0u && !SmokePayloadIsGuiScreen(reflectionPayload))
        {
            pathDepth = max(pathDepth, 2u);
            pathFlags |= RT_PT_TOY_FLAG_REFLECTION_HIT;
            const float3 reflectionDirect = EvaluateSmokeMode18NeeDirectLighting(
                reflectionPayload,
                reflectionRay.Origin,
                reflectionRay.Direction,
                true,
                useFakePBRSpecular,
                true,
                bounceSeed ^ 0x7feb352du,
                PathTraceIntegratorSecondaryNeeMode(),
                PathTraceIntegratorSecondaryAnalyticNeeMode()
            );
            radiance += reflectionDirect * reflectionWeight;
        }
        else
        {
            pathFlags |= RT_PT_TOY_FLAG_REFLECTION_MISS;
        }
    }
    else if (PathTraceIntegratorReflectionMode() > 0u && PathTraceIntegratorSpecularBounceLimit() > 0u)
    {
        pathFlags |= allowSecondary ? RT_PT_TOY_FLAG_REFLECTION_GATED : RT_PT_TOY_FLAG_MAX_DEPTH_TERMINATED;
    }

    radiance += EvaluateSmokeLightSpriteProxies(rayOrigin, rayDirection, primaryPayload.hitT) * 0.35;
    return float4(saturate(radiance), 1.0);
}

bool SmokePayloadIsGuiScreen(PathTraceSmokePayload payload)
{
    return payload.value != 0u &&
        payload.surfaceClass == RT_SMOKE_SURFACE_CLASS_TRANSLUCENT &&
        payload.translucentSubtype == RT_SMOKE_TRANSLUCENT_SUBTYPE_GUI_SCREEN;
}

float4 CompositeSmokeGuiLayers(float3 rayOrigin, float3 rayDirection, PathTraceSmokePayload firstPayload)
{
    float3 color = float3(0.0, 0.0, 0.0);
    float transmittance = 1.0;
    float lastHitT = 0.0;

    PathTraceSmokePayload layerPayload = firstPayload;
    [loop]
    for (uint layer = 0u; layer < 12u; layer++)
    {
        if (!SmokePayloadIsGuiScreen(layerPayload))
        {
            break;
        }

        if (layer > 0u && abs(layerPayload.hitT - firstPayload.hitT) > 8.0)
        {
            break;
        }

        const PathTraceSmokeMaterial material = LoadSmokeMaterial(layerPayload.materialIndex);
        const float4 albedo = SampleSmokeSurfaceAlbedo(
            material,
            layerPayload.texCoord,
            layerPayload.surfaceClass,
            layerPayload.translucentSubtype,
            layerPayload.vertexColor,
            layerPayload.vertexColorAdd);
        const float alpha = saturate(albedo.a);
        color += albedo.rgb * alpha * transmittance;
        transmittance *= 1.0 - alpha;
        lastHitT = layerPayload.hitT;

        if (transmittance <= 0.02)
        {
            break;
        }

        PathTraceSmokePayload nextPayload = InitSmokePayload();
        RayDesc ray;
        ray.Origin = rayOrigin;
        ray.Direction = rayDirection;
        ray.TMin = lastHitT + 0.002;
        ray.TMax = min(CameraOriginAndTMax.w, firstPayload.hitT + 8.0);
        TraceRay(SmokeScene, RAY_FLAG_NONE, 0xff, 0, 1, 0, ray, nextPayload);
        layerPayload = nextPayload;
    }

    color += float3(0.015, 0.012, 0.008) * transmittance;
    return float4(saturate(color), 1.0);
}
#endif

#ifdef RB_PT_RESTIR_PDF_NEE_RLU_CURRENT_PRODUCER_ONLY
static const uint RT_PDF_NEE_RLU_STATUS_VALID = 0u;
static const uint RT_PDF_NEE_RLU_STATUS_INVALID_SURFACE = 1u;
static const uint RT_PDF_NEE_RLU_STATUS_NO_RLU = 2u;
static const uint RT_PDF_NEE_RLU_STATUS_EMPTY_RESERVOIR = 3u;

uint PathTraceRestirPdfNeeRluReservoirBlockCount(uint dimension)
{
    return (dimension + RTXDI_RESERVOIR_BLOCK_SIZE - 1u) / RTXDI_RESERVOIR_BLOCK_SIZE;
}

RTXDI_ReservoirBufferParameters PathTraceRestirPdfNeeRluReservoirParams(uint2 dimensions)
{
    RTXDI_ReservoirBufferParameters params = (RTXDI_ReservoirBufferParameters)0;
    const uint blockCountX = max(PathTraceRestirPdfNeeRluReservoirBlockCount(dimensions.x), 1u);
    const uint blockCountY = max(PathTraceRestirPdfNeeRluReservoirBlockCount(dimensions.y), 1u);
    params.reservoirBlockRowPitch = blockCountX * RTXDI_RESERVOIR_BLOCK_SIZE * RTXDI_RESERVOIR_BLOCK_SIZE;
    params.reservoirArrayPitch = params.reservoirBlockRowPitch * blockCountY;
    return params;
}

uint PathTraceRestirPdfNeeRluReservoirPointer(uint2 pixel, uint2 dimensions)
{
    return RTXDI_ReservoirPositionToPointer(PathTraceRestirPdfNeeRluReservoirParams(dimensions), pixel, 0u);
}

float3 PathTraceRestirPdfNeeRluStatusColor(uint status)
{
    if (status == RT_PDF_NEE_RLU_STATUS_INVALID_SURFACE)
    {
        return float3(0.95, 0.05, 0.10);
    }
    if (status == RT_PDF_NEE_RLU_STATUS_NO_RLU)
    {
        return float3(1.00, 0.00, 1.00);
    }
    if (status == RT_PDF_NEE_RLU_STATUS_EMPTY_RESERVOIR)
    {
        return float3(0.0, 0.0, 0.0);
    }
    return float3(0.02, 0.02, 0.02);
}

float3 PathTraceRestirPdfNeeRluPreviewColor(float3 contribution)
{
    const float3 clampedPositive = max(contribution, float3(0.0, 0.0, 0.0));
    return clampedPositive / (clampedPositive + float3(1.0, 1.0, 1.0));
}

uint PathTraceRestirPdfNeeRluRangeOffset(uint rangeIndex)
{
    return (uint)max(RestirLightManagerRangeInfo[rangeIndex * 2u], 0.0);
}

uint PathTraceRestirPdfNeeRluRangeCount(uint rangeIndex)
{
    return (uint)max(RestirLightManagerRangeInfo[rangeIndex * 2u + 1u], 0.0);
}

RTXDI_DIInitialSamplingParameters PathTraceRestirPdfNeeRluBuildInitialSamplingParameters(uint sampleCount)
{
    RTXDI_DIInitialSamplingParameters sampleParams = (RTXDI_DIInitialSamplingParameters)0;
    sampleParams.numLocalLightSamples = sampleCount;
    sampleParams.numInfiniteLightSamples = 0u;
    sampleParams.numEnvironmentSamples = 0u;
    sampleParams.numBrdfSamples = 0u;
    sampleParams.brdfCutoff = 0.0;
    sampleParams.brdfRayMinT = 0.0;
    sampleParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode_UNIFORM;
    sampleParams.enableInitialVisibility = 0u;
    sampleParams.environmentMapImportanceSampling = 0u;
    return sampleParams;
}

float2 PathTraceRestirPdfNeeRluRandomlySelectLocalLightUv(inout RTXDI_RandomSamplerState rng)
{
    return float2(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng));
}

uint PathTraceRestirPdfNeeRluUploadedDoomAnalyticCount()
{
    return PathTraceSafetyDisabled(RT_PT_SAFETY_DISABLE_ANALYTIC_LIGHT_LOOP)
        ? 0u
        : (uint)max(DoomAnalyticLightInfo.x, 0.0);
}

bool PathTraceRestirPdfNeeRluEmissiveContributionEnabled()
{
    return max(ToyPathInfo.z, 0.0) > 1.0e-6;
}

bool PathTraceRestirPdfNeeRluAnalyticPayloadValid(PathTraceDoomAnalyticLightCandidate light)
{
    const float3 radiance = max(light.colorAndIntensity.rgb, float3(0.0, 0.0, 0.0));
    const float radianceLuminance = dot(radiance, float3(0.2126, 0.7152, 0.0722));
    return all(light.originAndRadius.xyz == light.originAndRadius.xyz) &&
        all(abs(light.originAndRadius.xyz) < float3(3.402823e+38, 3.402823e+38, 3.402823e+38)) &&
        all(radiance == radiance) &&
        all(abs(radiance) < float3(3.402823e+38, 3.402823e+38, 3.402823e+38)) &&
        radianceLuminance > 0.0 &&
        light.originAndRadius.w > 0.0 &&
        light.originAndRadius.w < 3.402823e+38 &&
        light.doomRadiusAndArea.x > 0.0 &&
        light.doomRadiusAndArea.x < 3.402823e+38;
}

RAB_LightInfo PathTraceRestirPdfNeeRluBuildCleanAnalyticLightInfo(PathTraceDoomAnalyticLightCandidate light, uint denseRluIndex)
{
    RAB_LightInfo lightInfo = RAB_EmptyLightInfo();
    if (!PathTraceRestirPdfNeeRluAnalyticPayloadValid(light))
    {
        return lightInfo;
    }

    lightInfo.lightType = RAB_LIGHT_TYPE_DOOM_ANALYTIC_SPHERE;
    lightInfo.lightIndex = denseRluIndex;
    lightInfo.unifiedLightType = PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC;
    lightInfo.materialIndex = RAB_INVALID_LIGHT_INDEX;
    lightInfo.flags = light.flags;
    lightInfo.position = light.originAndRadius.xyz;
    lightInfo.radius = max(light.originAndRadius.w, 0.01);
    lightInfo.normal = float3(0.0, 0.0, 1.0);
    lightInfo.radiance = max(light.colorAndIntensity.rgb, float3(0.0, 0.0, 0.0)) * max(DoomAnalyticLightInfo.z, 0.0);
    lightInfo.influenceRadius = max(light.doomRadiusAndArea.x, lightInfo.radius);
    lightInfo.area = max(light.doomRadiusAndArea.y, 1.0e-4);
    lightInfo.weight = dot(lightInfo.radiance, float3(0.2126, 0.7152, 0.0722)) * lightInfo.area * lightInfo.influenceRadius;
    return lightInfo;
}

RAB_LightInfo PathTraceRestirPdfNeeRluLoadCleanCompatibleLightInfo(uint lightIndex)
{
    if (lightIndex >= RAB_GetCurrentRestirLightManagerCount())
    {
        return RAB_EmptyLightInfo();
    }

    const PathTraceUnifiedLightRecord record = PathTraceRestirLightManagerCurrentPayload[lightIndex];
    if (record.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
    {
        if (record.sourceIndex == PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX ||
            record.sourceIndex >= PathTraceRestirPdfNeeRluUploadedDoomAnalyticCount())
        {
            return RAB_EmptyLightInfo();
        }

        return PathTraceRestirPdfNeeRluBuildCleanAnalyticLightInfo(DoomAnalyticLights[record.sourceIndex], lightIndex);
    }

    return RAB_LoadActiveRrxLightInfo(lightIndex, false);
}

bool PathTraceRestirPdfNeeRluStreamActiveLightAtUvIntoReservoir(
    inout RTXDI_RandomSamplerState rng,
    RAB_Surface surface,
    uint lightIndex,
    float2 uv,
    float invSourcePdf,
    inout RTXDI_DIReservoir reservoir)
{
    const RAB_LightInfo lightInfo = PathTraceRestirPdfNeeRluLoadCleanCompatibleLightInfo(lightIndex);
    if (!RAB_IsLightInfoValid(lightInfo))
    {
        return false;
    }

    const RAB_LightSample lightSample = RAB_SampleActiveRrxPolymorphicLight(lightInfo, surface, uv);
    if (!RAB_IsReplayableLightSample(lightSample) || lightSample.solidAnglePdf <= 0.0)
    {
        return false;
    }

    const float targetPdf = max(RAB_GetLightSampleTargetPdfForSurface(lightSample, surface), 0.0);
    if (targetPdf <= 0.0 || invSourcePdf <= 0.0)
    {
        return false;
    }

    RTXDI_StreamSample(
        reservoir,
        lightIndex,
        uv,
        RTXDI_GetNextRandom(rng),
        targetPdf,
        invSourcePdf);
    return true;
}

void PathTraceRestirPdfNeeRluStreamActiveRangeIntoReservoir(
    inout RTXDI_DIReservoir reservoir,
    inout RTXDI_RandomSamplerState rng,
    RAB_Surface surface,
    uint rangeOffset,
    uint rangeCount,
    uint requestedSampleCount,
    uint totalProposalSampleCount)
{
    if (rangeCount == 0u || requestedSampleCount == 0u || totalProposalSampleCount == 0u)
    {
        return;
    }

    const uint boundedSampleCount = min(requestedSampleCount, rangeCount);
    if (boundedSampleCount == 0u)
    {
        return;
    }

    float lightIndexInRange = RTXDI_GetNextRandom(rng) * (float)rangeCount;
    const float stride = max(1.0, (float)rangeCount / (float)boundedSampleCount);
    const float invSourcePdf =
        ((float)rangeCount * (float)totalProposalSampleCount) / max((float)boundedSampleCount, 1.0);

    [loop]
    for (uint sampleIndex = 0u; sampleIndex < boundedSampleCount; ++sampleIndex)
    {
        const uint lightIndex = rangeOffset + min((uint)lightIndexInRange, rangeCount - 1u);
        const float2 uv = PathTraceRestirPdfNeeRluRandomlySelectLocalLightUv(rng);
        PathTraceRestirPdfNeeRluStreamActiveLightAtUvIntoReservoir(
            rng,
            surface,
            lightIndex,
            uv,
            invSourcePdf,
            reservoir);

        lightIndexInRange += stride;
        if (lightIndexInRange >= (float)rangeCount)
        {
            lightIndexInRange -= (float)rangeCount;
        }
    }
}

uint PathTraceRestirPdfNeeRluTypedRangeSampleCount(uint rangeCount, uint totalRangeCount, uint totalSampleCount)
{
    if (rangeCount == 0u || totalRangeCount == 0u || totalSampleCount == 0u)
    {
        return 0u;
    }

    const uint proportional = (uint)round(((float)rangeCount / (float)totalRangeCount) * (float)totalSampleCount);
    return clamp(max(proportional, 1u), 1u, totalSampleCount);
}

bool PathTraceRestirPdfNeeRluNeeCacheProviderReady()
{
    return RestirPdfNeeRluCurrentControlInfo.w >= 0.5 &&
        NeeCacheInfo0.x >= 0.5 &&
        NeeCacheInfo1.w > 0.0 &&
        NeeCacheInfo2.z > 0.0 &&
        NeeCacheInfo2.w > 0.0;
}

bool PathTraceRestirPdfNeeRluProviderResultUsable(PathTraceNeeCacheProviderResult result, uint currentRluLightCount)
{
    return result.flags != 0u &&
        result.cellIndex < (uint)max(NeeCacheInfo2.w, 0.0) &&
        result.selectedDenseRluIndex < currentRluLightCount &&
        result.sourcePdf > 0.0 &&
        result.invSourcePdf > 0.0;
}

uint PathTraceRestirPdfNeeRluNeeCacheSourceDomain()
{
    return clamp((uint)max(NeeCacheInfo0.w, 0.0), 0u, 3u);
}

uint PathTraceRestirPdfNeeRluNeeCacheCandidateSlots()
{
    return max((uint)max(NeeCacheInfo1.w, 0.0), 1u);
}

uint PathTraceRestirPdfNeeRluStableDoomAnalyticCount()
{
    return (uint)max(RestirLightManagerControlInfo.z, 0.0);
}

bool PathTraceRestirPdfNeeRluCandidateRecordUsable(PathTraceNeeCacheCandidateRecord candidate, uint currentRluLightCount)
{
    if (candidate.flags == 0u ||
        candidate.denseRluIndex >= currentRluLightCount ||
        candidate.sourcePdf <= 0.0 ||
        candidate.invSourcePdf <= 0.0)
    {
        return false;
    }

    const PathTraceUnifiedLightRecord record = PathTraceRestirLightManagerCurrentPayload[candidate.denseRluIndex];
    if (record.type != candidate.lightClass)
    {
        return false;
    }
    if (candidate.lightClass == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC &&
        (record.flags & PATH_TRACE_RLU_LIGHT_FLAG_STABLE_CACHEABLE) == 0u)
    {
        return false;
    }

    const RAB_LightInfo lightInfo = PathTraceRestirPdfNeeRluLoadCleanCompatibleLightInfo(candidate.denseRluIndex);
    return RAB_IsLightInfoValid(lightInfo);
}

float PathTraceRestirPdfNeeRluNeeCacheEmissiveCandidateWeight(PathTraceUnifiedLightRecord record, PathTraceNeeCacheCellDebug cell)
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

float PathTraceRestirPdfNeeRluNeeCacheAnalyticCandidateWeight(PathTraceUnifiedLightRecord record, PathTraceNeeCacheCellDebug cell)
{
    if (record.type != PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC ||
        (record.flags & PATH_TRACE_RLU_LIGHT_FLAG_STABLE_CACHEABLE) == 0u)
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
    const float area = max(record.normalAndArea.w, 4.0 * RTXDI_PI * radius * radius);
    return sourceWeight * luminance * area * falloff * falloff /
        max(dot(toCell, toCell) + radius * radius + cellRadius * cellRadius, 1.0);
}

float PathTraceRestirPdfNeeRluNeeCacheCurrentCandidateWeight(PathTraceNeeCacheCandidateRecord candidate, PathTraceNeeCacheCellDebug cell, uint currentRluLightCount)
{
    if (!PathTraceRestirPdfNeeRluCandidateRecordUsable(candidate, currentRluLightCount))
    {
        return 0.0;
    }

    const PathTraceUnifiedLightRecord record = PathTraceRestirLightManagerCurrentPayload[candidate.denseRluIndex];
    if (candidate.lightClass == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
    {
        return PathTraceRestirPdfNeeRluNeeCacheAnalyticCandidateWeight(record, cell);
    }
    if (candidate.lightClass == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
    {
        return PathTraceRestirPdfNeeRluNeeCacheEmissiveCandidateWeight(record, cell);
    }
    return 0.0;
}

bool PathTraceRestirPdfNeeRluSelectNeeCacheCandidateForPixel(
    inout RTXDI_RandomSamplerState rng,
    PathTraceNeeCacheCellDebug cell,
    uint currentRluLightCount,
    out uint selectedDenseRluIndex,
    out float selectedInvSourcePdf)
{
    selectedDenseRluIndex = 0xffffffffu;
    selectedInvSourcePdf = 0.0;

    const uint candidateSlots = PathTraceRestirPdfNeeRluNeeCacheCandidateSlots();
    const uint baseSlot = cell.cellIndex * candidateSlots;
    float totalWeight = 0.0;

    const PathTraceNeeCacheCellRecord storedCell = PathTraceNeeCacheCells[cell.cellIndex];
    if (storedCell.flags == 0u || storedCell.hash != cell.hash)
    {
        return false;
    }

    [loop]
    for (uint slot = 0u; slot < candidateSlots; ++slot)
    {
        const PathTraceNeeCacheCandidateRecord candidate = PathTraceNeeCacheCandidates[baseSlot + slot];
        const float currentWeight = PathTraceRestirPdfNeeRluNeeCacheCurrentCandidateWeight(candidate, cell, currentRluLightCount);
        if (currentWeight > 0.0)
        {
            totalWeight += currentWeight;
        }
    }

    if (totalWeight <= 0.0)
    {
        return false;
    }

    const float threshold = RTXDI_GetNextRandom(rng) * totalWeight;
    float cumulativeWeight = 0.0;
    PathTraceNeeCacheCandidateRecord selectedCandidate = (PathTraceNeeCacheCandidateRecord)0;
    selectedCandidate.denseRluIndex = 0xffffffffu;

    [loop]
    for (uint selectSlot = 0u; selectSlot < candidateSlots; ++selectSlot)
    {
        const PathTraceNeeCacheCandidateRecord candidate = PathTraceNeeCacheCandidates[baseSlot + selectSlot];
        if (!PathTraceRestirPdfNeeRluCandidateRecordUsable(candidate, currentRluLightCount))
        {
            continue;
        }

        const float currentWeight = PathTraceRestirPdfNeeRluNeeCacheCurrentCandidateWeight(candidate, cell, currentRluLightCount);
        if (currentWeight <= 0.0)
        {
            continue;
        }
        cumulativeWeight += currentWeight;
        if (selectedCandidate.denseRluIndex == 0xffffffffu && cumulativeWeight >= threshold)
        {
            selectedCandidate = candidate;
        }
    }

    if (selectedCandidate.denseRluIndex == 0xffffffffu)
    {
        return false;
    }

    float selectedIdentityWeight = 0.0;
    [loop]
    for (uint pdfSlot = 0u; pdfSlot < candidateSlots; ++pdfSlot)
    {
        const PathTraceNeeCacheCandidateRecord candidate = PathTraceNeeCacheCandidates[baseSlot + pdfSlot];
        if (candidate.denseRluIndex == selectedCandidate.denseRluIndex &&
            PathTraceRestirPdfNeeRluCandidateRecordUsable(candidate, currentRluLightCount))
        {
            selectedIdentityWeight += PathTraceRestirPdfNeeRluNeeCacheCurrentCandidateWeight(candidate, cell, currentRluLightCount);
        }
    }

    const float cacheProbability = 1.0 - saturate(NeeCacheInfo2.y);
    const float sourcePdf = cacheProbability * selectedIdentityWeight / max(totalWeight, 1.0e-8);
    if (sourcePdf <= 0.0)
    {
        return false;
    }

    selectedDenseRluIndex = selectedCandidate.denseRluIndex;
    selectedInvSourcePdf = 1.0 / max(sourcePdf, 1.0e-8);
    return true;
}

bool PathTraceRestirPdfNeeRluSelectNeeCacheFallbackForPixel(
    inout RTXDI_RandomSamplerState rng,
    uint currentRluLightCount,
    float mixtureProbability,
    out uint selectedDenseRluIndex,
    out float selectedInvSourcePdf)
{
    selectedDenseRluIndex = 0xffffffffu;
    selectedInvSourcePdf = 0.0;

    const uint sourceDomain = PathTraceRestirPdfNeeRluNeeCacheSourceDomain();
    const uint emissiveOffset = PathTraceRestirPdfNeeRluRangeOffset(0u);
    const uint emissiveCount = min(PathTraceRestirPdfNeeRluRangeCount(0u), currentRluLightCount - min(emissiveOffset, currentRluLightCount));
    const uint analyticOffset = PathTraceRestirPdfNeeRluRangeOffset(1u);
    const uint analyticCount = min(PathTraceRestirPdfNeeRluStableDoomAnalyticCount(), min(PathTraceRestirPdfNeeRluRangeCount(1u), currentRluLightCount - min(analyticOffset, currentRluLightCount)));
    uint rangeOffset = 0u;
    uint rangeCount = currentRluLightCount;
    float classProbability = 1.0;

    if (sourceDomain == 0u)
    {
        rangeOffset = emissiveCount > 0u ? emissiveOffset : analyticOffset;
        rangeCount = emissiveCount + analyticCount;
    }
    else if (sourceDomain == 1u)
    {
        rangeOffset = emissiveOffset;
        rangeCount = emissiveCount;
    }
    else if (sourceDomain == 2u)
    {
        rangeOffset = analyticOffset;
        rangeCount = analyticCount;
    }
    else
    {
        const bool chooseAnalytic =
            analyticCount > 0u &&
            (emissiveCount == 0u || RTXDI_GetNextRandom(rng) >= 0.5);
        rangeOffset = chooseAnalytic ? analyticOffset : emissiveOffset;
        rangeCount = chooseAnalytic ? analyticCount : emissiveCount;
        classProbability = (emissiveCount > 0u && analyticCount > 0u) ? 0.5 : 1.0;
    }

    if (rangeCount == 0u)
    {
        return false;
    }

    const uint localIndex = min((uint)(RTXDI_GetNextRandom(rng) * (float)rangeCount), rangeCount - 1u);
    const uint denseIndex = rangeOffset + localIndex;
    if (denseIndex >= currentRluLightCount)
    {
        return false;
    }

    const float sourcePdf = saturate(mixtureProbability) * classProbability / max((float)rangeCount, 1.0);
    if (sourcePdf <= 0.0)
    {
        return false;
    }

    selectedDenseRluIndex = denseIndex;
    selectedInvSourcePdf = 1.0 / max(sourcePdf, 1.0e-8);
    return true;
}

bool PathTraceRestirPdfNeeRluStreamNeeCacheProviderIntoReservoir(
    inout RTXDI_DIReservoir reservoir,
    inout RTXDI_RandomSamplerState rng,
    RAB_Surface surface,
    uint currentRluLightCount)
{
    if (!PathTraceRestirPdfNeeRluNeeCacheProviderReady())
    {
        return false;
    }

    const PathTraceNeeCacheCellDebug cell = PathTraceNeeCacheMapWorldPositionToCell(
        surface.worldPos,
        CameraOriginAndTMax.xyz,
        max((uint)NeeCacheInfo1.x, 1u),
        max(NeeCacheInfo1.y, 1.0),
        max((uint)NeeCacheInfo1.z, 1u));
    if (cell.valid == 0u || cell.cellIndex >= (uint)max(NeeCacheInfo2.z, 0.0))
    {
        return false;
    }

    uint selectedDenseRluIndex = 0xffffffffu;
    float selectedInvSourcePdf = 0.0;
    const float fallbackProbability = saturate(NeeCacheInfo2.y);
    const bool randomFallback = RTXDI_GetNextRandom(rng) < fallbackProbability;
    const bool selectedCacheCandidate =
        !randomFallback &&
        PathTraceRestirPdfNeeRluSelectNeeCacheCandidateForPixel(
            rng,
            cell,
            currentRluLightCount,
            selectedDenseRluIndex,
            selectedInvSourcePdf);

    if (!selectedCacheCandidate)
    {
        const float fallbackMixtureProbability = randomFallback ? fallbackProbability : 1.0;
        if (!PathTraceRestirPdfNeeRluSelectNeeCacheFallbackForPixel(
            rng,
            currentRluLightCount,
            fallbackMixtureProbability,
            selectedDenseRluIndex,
            selectedInvSourcePdf))
        {
            return false;
        }
    }

    return PathTraceRestirPdfNeeRluStreamActiveLightAtUvIntoReservoir(
        rng,
        surface,
        selectedDenseRluIndex,
        PathTraceRestirPdfNeeRluRandomlySelectLocalLightUv(rng),
        selectedInvSourcePdf,
        reservoir);
}

RTXDI_DIReservoir PathTraceRestirPdfNeeRluBuildCurrentReservoir(
    RAB_Surface surface,
    uint2 pixel,
    out float3 selectedContribution,
    out uint status)
{
    selectedContribution = float3(0.0, 0.0, 0.0);
    status = RT_PDF_NEE_RLU_STATUS_VALID;

    RTXDI_DIReservoir reservoir = RTXDI_EmptyDIReservoir();
    if (!RAB_IsSurfaceValid(surface))
    {
        status = RT_PDF_NEE_RLU_STATUS_INVALID_SURFACE;
        return reservoir;
    }
    if (!RAB_RestirLightManagerRemixDenseDomainEnabled())
    {
        status = RT_PDF_NEE_RLU_STATUS_NO_RLU;
        return reservoir;
    }

    const uint currentRluLightCount = RAB_GetCurrentRestirLightManagerCount();
    if (currentRluLightCount == 0u)
    {
        status = RT_PDF_NEE_RLU_STATUS_NO_RLU;
        return reservoir;
    }

    const uint frameIndex = (uint)max(RestirPTInfo.x, 0.0);
    const uint sampleCount = clamp((uint)max(RestirPdfNeeRluCurrentControlInfo.x, 1.0), 1u, 64u);
    const bool tracedVisibility = RestirPdfNeeRluCurrentControlInfo.y >= 0.5;
    const uint sourcePolicy = (uint)clamp(floor(RestirPdfNeeRluCurrentControlInfo.z + 0.5), 0.0, 2.0);
    RTXDI_DIInitialSamplingParameters sampleParams = PathTraceRestirPdfNeeRluBuildInitialSamplingParameters(sampleCount);
    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(pixel, frameIndex, 0x4d534449u);
    uint totalProposalSampleCount = sampleParams.numLocalLightSamples;

    const uint emissiveOffset = PathTraceRestirPdfNeeRluRangeOffset(0u);
    const uint emissiveCount = min(PathTraceRestirPdfNeeRluRangeCount(0u), currentRluLightCount - min(emissiveOffset, currentRluLightCount));
    const uint doomAnalyticOffset = PathTraceRestirPdfNeeRluRangeOffset(1u);
    const uint doomAnalyticCount = min(PathTraceRestirPdfNeeRluRangeCount(1u), currentRluLightCount - min(doomAnalyticOffset, currentRluLightCount));
    const uint sampleableEmissiveCount = PathTraceRestirPdfNeeRluEmissiveContributionEnabled() ? emissiveCount : 0u;
    const bool typedPolicy = sourcePolicy == 1u && (sampleableEmissiveCount > 0u || doomAnalyticCount > 0u);

    if (sourcePolicy == 2u)
    {
        totalProposalSampleCount = sampleCount;
        sampleParams = PathTraceRestirPdfNeeRluBuildInitialSamplingParameters(sampleCount);
        if (PathTraceRestirPdfNeeRluNeeCacheProviderReady())
        {
            [loop]
            for (uint sampleIndex = 0u; sampleIndex < sampleParams.numLocalLightSamples; ++sampleIndex)
            {
                PathTraceRestirPdfNeeRluStreamNeeCacheProviderIntoReservoir(
                    reservoir,
                    rng,
                    surface,
                    currentRluLightCount);
            }
        }
    }
    else if (typedPolicy)
    {
        const uint totalTypedCount = sampleableEmissiveCount + doomAnalyticCount;
        uint emissiveSampleCount = PathTraceRestirPdfNeeRluTypedRangeSampleCount(sampleableEmissiveCount, totalTypedCount, sampleCount);
        uint doomAnalyticSampleCount = PathTraceRestirPdfNeeRluTypedRangeSampleCount(doomAnalyticCount, totalTypedCount, sampleCount);

        if (emissiveSampleCount + doomAnalyticSampleCount > sampleCount)
        {
            if (doomAnalyticSampleCount >= emissiveSampleCount && doomAnalyticSampleCount > 1u)
            {
                --doomAnalyticSampleCount;
            }
            else if (emissiveSampleCount > 1u)
            {
                --emissiveSampleCount;
            }
        }

        totalProposalSampleCount = emissiveSampleCount + doomAnalyticSampleCount;
        sampleParams = PathTraceRestirPdfNeeRluBuildInitialSamplingParameters(max(totalProposalSampleCount, 1u));

        PathTraceRestirPdfNeeRluStreamActiveRangeIntoReservoir(
            reservoir,
            rng,
            surface,
            emissiveOffset,
            sampleableEmissiveCount,
            emissiveSampleCount,
            totalProposalSampleCount);
        PathTraceRestirPdfNeeRluStreamActiveRangeIntoReservoir(
            reservoir,
            rng,
            surface,
            doomAnalyticOffset,
            doomAnalyticCount,
            doomAnalyticSampleCount,
            totalProposalSampleCount);
    }
    else
    {
        PathTraceRestirPdfNeeRluStreamActiveRangeIntoReservoir(
            reservoir,
            rng,
            surface,
            0u,
            currentRluLightCount,
            sampleParams.numLocalLightSamples,
            sampleParams.numLocalLightSamples);
    }

    if (!RTXDI_IsValidDIReservoir(reservoir) || reservoir.targetPdf <= 0.0)
    {
        status = RT_PDF_NEE_RLU_STATUS_EMPTY_RESERVOIR;
        return RTXDI_EmptyDIReservoir();
    }

    RTXDI_FinalizeResampling(reservoir, 1.0, (float)max(totalProposalSampleCount, 1u));
    reservoir.M = 1.0;
    reservoir.age = 0u;
    reservoir.spatialDistance = int2(0, 0);

    const uint selectedLightIndex = RTXDI_GetDIReservoirLightIndex(reservoir);
    const RAB_LightInfo selectedLightInfo = PathTraceRestirPdfNeeRluLoadCleanCompatibleLightInfo(selectedLightIndex);
    if (!RAB_IsLightInfoValid(selectedLightInfo))
    {
        status = RT_PDF_NEE_RLU_STATUS_EMPTY_RESERVOIR;
        return RTXDI_EmptyDIReservoir();
    }

    const float2 selectedUv = RTXDI_GetDIReservoirSampleUV(reservoir);
    const RAB_LightSample selectedSample = RAB_SampleActiveRrxPolymorphicLight(selectedLightInfo, surface, selectedUv);
    if (!RAB_IsReplayableLightSample(selectedSample) || selectedSample.solidAnglePdf <= 0.0)
    {
        status = RT_PDF_NEE_RLU_STATUS_EMPTY_RESERVOIR;
        return RTXDI_EmptyDIReservoir();
    }

    const float visibility = tracedVisibility ? (RAB_GetSelectedNeeVisibility(surface, selectedSample) ? 1.0 : 0.0) : 1.0;
    RTXDI_StoreVisibilityInDIReservoir(reservoir, visibility.xxx, false);
    if (visibility <= 0.0)
    {
        selectedContribution = float3(0.0, 0.0, 0.0);
        return reservoir;
    }

    selectedContribution =
        RAB_GetReflectedBsdfRadianceForSurface(selectedSample.position, selectedSample.radiance, surface) *
        (RTXDI_GetDIReservoirInvPdf(reservoir) / max(selectedSample.solidAnglePdf, 1.0e-6)) *
        visibility;
    return reservoir;
}
#endif

void StoreStaticContractPrimarySurfaceRecord(
    uint2 pixel,
    RAB_Surface surface,
    PathTraceSmokePayload payload)
{
    StorePathTracePrimarySurfaceRecord(pixel, surface);
    if (PathTraceSafetyDisabled(RT_PT_SAFETY_DISABLE_PRIMARY_SURFACE_HISTORY))
    {
        return;
    }

    const uint2 storePixel = PathTracePrimarySurfaceStorePixel(pixel);
    const uint2 dimensions = PathTraceFullOutputSize();
    if (storePixel.x >= dimensions.x || storePixel.y >= dimensions.y)
    {
        return;
    }

    const uint index = storePixel.y * dimensions.x + storePixel.x;
    if (index >= PathTracePrimarySurfaceHistoryCount())
    {
        return;
    }

    PathTracePrimarySurfaceRecord record = PrimarySurfaceHistoryCurrent[index];
    record.instancePrimitiveObject.z = payload.staticContractRejectReason;
    if ((record.header.y & RT_PRIMARY_SURFACE_VALID) == 0u)
    {
        record.instancePrimitiveObject.x = payload.instanceId;
        record.instancePrimitiveObject.y = payload.primitiveIndex;
    }
    PrimarySurfaceHistoryCurrent[index] = record;
}

[shader("raygeneration")]
void RayGen()
{
    const uint2 localPixel = DispatchRaysIndex().xy;
    const uint2 pixel = localPixel + PathTraceDispatchTileOffset();
    const uint2 fullDimensions = PathTraceFullOutputSize();
    const uint2 dimensions = fullDimensions;
    if (pixel.x >= fullDimensions.x || pixel.y >= fullDimensions.y)
    {
        return;
    }
    const float2 uv = (float2(pixel) + 0.5) / float2(fullDimensions);
    const float2 ndc = uv * 2.0 - 1.0;

    PathTraceSmokePayload payload = InitSmokePayload();

    RayDesc ray;
    ray.Origin = CameraOriginAndTMax.xyz;
    ray.Direction = normalize(
        CameraForwardAndTanX.xyz +
        CameraLeftAndTanY.xyz * (-ndc.x * CameraForwardAndTanX.w) +
        CameraUpAndDebugMode.xyz * (-ndc.y * CameraLeftAndTanY.w));
    ray.TMin = 0.1;
    ray.TMax = CameraOriginAndTMax.w;

#ifdef RB_PT_FORCE_DEBUG_MODE
    const uint debugMode = RB_PT_FORCE_DEBUG_MODE;
#else
    const uint debugMode = (uint)CameraUpAndDebugMode.w;
#endif
    if (debugMode == 21u)
    {
        SmokeOutput[pixel] = RenderSmokeBoundsBoxes(ray.Origin, ray.Direction);
        return;
    }
    if (debugMode == 22u)
    {
        SmokeOutput[pixel] = RenderSmokeBoundsWireframeBoxes(ray.Origin, ray.Direction);
        return;
    }

    if (debugMode == 24u)
    {
        PathTraceSmokePayload fallbackPayload = InitSmokePayload();
        PathTraceSmokePayload rigidPayload = InitSmokePayload();
        TraceRay(SmokeScene, RAY_FLAG_NONE, 0x01u, 0, 1, 0, ray, fallbackPayload);
        TraceRay(SmokeScene, RAY_FLAG_NONE, 0x02u, 0, 1, 0, ray, rigidPayload);

        // GEO-01 uses mode 24 as a fallback-route traversal probe. Preserve
        // the mask-0x01 hit in the normal primary-surface readback so the
        // one-shot contract dump can distinguish a missing static BLAS hit
        // from a rigid instance merely winning the unrestricted primary ray.
        RAB_Surface fallbackHistorySurface = RAB_EmptySurface();
        if (fallbackPayload.value != 0u && !SmokePayloadIsGuiScreen(fallbackPayload))
        {
            fallbackHistorySurface = RAB_BuildSurfaceFromSmokePayload(
                fallbackPayload,
                ray.Origin,
                ray.Direction,
                true);
        }
        if (fallbackPayload.value != 0u && SmokePayloadIsGuiScreen(fallbackPayload))
        {
            fallbackPayload.staticContractRejectReason = RT_STATIC_CONTRACT_REJECT_GUI_PRIMARY;
        }
        else if (fallbackPayload.value == 0u &&
            fallbackPayload.staticContractRejectReason == RT_STATIC_CONTRACT_REJECT_NONE)
        {
            fallbackPayload.staticContractRejectReason = RT_STATIC_CONTRACT_REJECT_MISS;
        }
        StoreStaticContractPrimarySurfaceRecord(pixel, fallbackHistorySurface, fallbackPayload);

        if (rigidPayload.value != 0u)
        {
            if (fallbackPayload.value != 0u)
            {
                const float distanceDelta = abs(rigidPayload.hitT - fallbackPayload.hitT);
                if (distanceDelta <= 1.5)
                {
                    const bool surfaceClassMatches = rigidPayload.surfaceClass == fallbackPayload.surfaceClass;
                    const bool materialMatches =
                        rigidPayload.materialId == fallbackPayload.materialId &&
                        rigidPayload.materialIndex == fallbackPayload.materialIndex;
                    if (!surfaceClassMatches)
                    {
                        SmokeOutput[pixel] = float4(1.0, 0.0, 1.0, 1.0);
                        return;
                    }
                    if (!materialMatches)
                    {
                        SmokeOutput[pixel] = float4(1.0, 1.0, 0.0, 1.0);
                        return;
                    }
                    SmokeOutput[pixel] = float4(0.0, 1.0, 0.0, 1.0);
                    return;
                }
                if (rigidPayload.hitT < fallbackPayload.hitT)
                {
                    SmokeOutput[pixel] = float4(0.0, 0.18, 1.0, 1.0);
                    return;
                }

                SmokeOutput[pixel] = float4(1.0, 0.45, 0.0, 1.0);
                return;
            }

            SmokeOutput[pixel] = float4(0.0, 1.0, 1.0, 1.0);
            return;
        }

        if (fallbackPayload.value != 0u)
        {
            SmokeOutput[pixel] = float4(0.18, 0.18, 0.18, 1.0);
            return;
        }

        SmokeOutput[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    const uint traceMask = debugMode == 23u ? 0x02u : 0xffu;

    RAB_Surface primaryHistorySurface = RAB_EmptySurface();
    const bool filterDecalCompositeEnabled = SmokePrimaryFilterDecalCompositeEnabled(debugMode);
    if (filterDecalCompositeEnabled)
    {
        payload.value = RT_SMOKE_RAY_MODE_PRIMARY_FILTER_DECAL_COMPOSITE;
    }
    TraceRay(SmokeScene, RAY_FLAG_NONE, traceMask, 0, 1, 0, ray, payload);
    if (payload.value != 0u && !SmokePayloadIsGuiScreen(payload))
    {
        PathTraceSmokePayload filterDecalPayload;
        const bool filterDecalComposite =
            filterDecalCompositeEnabled &&
            ResolvePrimaryFilterDecalReceiver(payload, ray, traceMask, filterDecalPayload);
        primaryHistorySurface = RAB_BuildSurfaceFromSmokePayload(payload, ray.Origin, ray.Direction, true);
        if (filterDecalComposite)
        {
            ApplyPrimaryFilterDecalToSurface(primaryHistorySurface, filterDecalPayload);
        }
    }
#ifdef RB_PT_RESTIR_PDF_NEE_RLU_CURRENT_PRODUCER_ONLY
    float3 pdfNeeRluContribution = float3(0.0, 0.0, 0.0);
    uint pdfNeeRluStatus = RT_PDF_NEE_RLU_STATUS_VALID;
    if (payload.value != 0u && SmokePayloadIsGuiScreen(payload))
    {
        payload.staticContractRejectReason = RT_STATIC_CONTRACT_REJECT_GUI_PRIMARY;
    }
    else if (payload.value == 0u && payload.staticContractRejectReason == RT_STATIC_CONTRACT_REJECT_NONE)
    {
        payload.staticContractRejectReason = RT_STATIC_CONTRACT_REJECT_MISS;
    }
    StoreStaticContractPrimarySurfaceRecord(pixel, primaryHistorySurface, payload);
    const RTXDI_DIReservoir pdfNeeRluReservoir = PathTraceRestirPdfNeeRluBuildCurrentReservoir(
        primaryHistorySurface,
        pixel,
        pdfNeeRluContribution,
        pdfNeeRluStatus);
    CleanRtxdiDiCurrentReservoirs[PathTraceRestirPdfNeeRluReservoirPointer(pixel, fullDimensions)] = RTXDI_PackDIReservoir(pdfNeeRluReservoir);
    SmokeOutput[pixel] = pdfNeeRluStatus == RT_PDF_NEE_RLU_STATUS_VALID
        ? float4(PathTraceRestirPdfNeeRluPreviewColor(pdfNeeRluContribution), 1.0)
        : float4(PathTraceRestirPdfNeeRluStatusColor(pdfNeeRluStatus), 1.0);
    return;
#else
    if (payload.value != 0u && SmokePayloadIsGuiScreen(payload))
    {
        payload.staticContractRejectReason = RT_STATIC_CONTRACT_REJECT_GUI_PRIMARY;
    }
    else if (payload.value == 0u && payload.staticContractRejectReason == RT_STATIC_CONTRACT_REJECT_NONE)
    {
        payload.staticContractRejectReason = RT_STATIC_CONTRACT_REJECT_MISS;
    }
    StoreStaticContractPrimarySurfaceRecord(pixel, primaryHistorySurface, payload);
    StorePathTraceMotionVectorExport(pixel, primaryHistorySurface);

    if (payload.value == 0)
    {
        if (debugMode == 14)
        {
            SmokeOutput[pixel] = float4(saturate(EvaluateSmokeLightSpriteProxies(ray.Origin, ray.Direction, ray.TMax)), 1.0);
        }
        else if (debugMode == 18 || debugMode == 25 || debugMode == 38 || debugMode == 39 || debugMode == 40 || debugMode == 41 || debugMode == 42 || debugMode == 43 || debugMode == 44 || debugMode == 45 || debugMode == 46 || debugMode == 47 || debugMode == 48 || debugMode == 49 || debugMode == 52)
        {
            SmokeOutput[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        }
        else if (debugMode == 57)
        {
            SmokeOutput[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        }
        else
        {
            SmokeOutput[pixel] = SmokeMissColor();
        }
    }
    else if (debugMode == 1)
    {
        const float normalizedDepth = saturate(payload.hitT / 512.0);
        SmokeOutput[pixel] = float4(normalizedDepth, normalizedDepth, normalizedDepth, 1.0);
    }
    else if (debugMode == 2)
    {
        SmokeOutput[pixel] = float4(payload.normal * 0.5 + 0.5, 1.0);
    }
    else if (debugMode == 3)
    {
        if (payload.surfaceClass == 0)
        {
            SmokeOutput[pixel] = float4(0.0, 1.0, 0.0, 1.0);
        }
        else if (payload.surfaceClass == 1)
        {
            SmokeOutput[pixel] = float4(0.0, 0.35, 1.0, 1.0);
        }
        else if (payload.surfaceClass == 2)
        {
            SmokeOutput[pixel] = float4(1.0, 0.0, 1.0, 1.0);
        }
        else if (payload.surfaceClass == 3)
        {
            SmokeOutput[pixel] = float4(1.0, 0.75, 0.0, 1.0);
        }
        else
        {
            SmokeOutput[pixel] = float4(1.0, 1.0, 1.0, 1.0);
        }
    }
    else if (debugMode == 4)
    {
        const float2 uv = frac(abs(payload.texCoord));
        SmokeOutput[pixel] = float4(0.15 + uv.x * 0.85, 0.15 + uv.y * 0.85, 0.25, 1.0);
    }
    else if (debugMode == 5)
    {
        SmokeOutput[pixel] = float4(payload.geometricNormal * 0.5 + 0.5, 1.0);
    }
    else if (debugMode == 6)
    {
        SmokeOutput[pixel] = float4(MaterialIdToColor(payload.materialId), 1.0);
    }
    else if (debugMode == 7)
    {
        SmokeOutput[pixel] = LoadSmokeMaterial(payload.materialIndex).debugAlbedo;
    }
    else if (debugMode == 23)
    {
        const PathTraceSmokeMaterial material = LoadSmokeMaterial(payload.materialIndex);
        const float depthFade = 1.0 - saturate(payload.hitT / 1024.0);
        const float3 normalShade = payload.normal * 0.5 + 0.5;
        const float3 materialColor = max(material.debugAlbedo.rgb, float3(0.08, 0.08, 0.08));
        SmokeOutput[pixel] = float4(saturate(materialColor * (0.35 + depthFade * 0.65) * (0.45 + normalShade * 0.55)), 1.0);
    }
    else if (debugMode == 25)
    {
        const PathTraceSmokeMaterial material = LoadSmokeMaterial(payload.materialIndex);
        const float3 albedo = max(material.debugAlbedo.rgb, float3(0.06, 0.06, 0.06));
        const float3 normal = SafeNormalize(payload.normal, payload.geometricNormal);
        const float3 lightDir = normalize(float3(0.35, 0.45, 0.82));
        const float ndotl = saturate(dot(normal, lightDir));
        const float viewFacing = saturate(dot(normal, -ray.Direction));
        float3 color = albedo * (0.16 + ndotl * 0.95 + viewFacing * 0.12);
        if (payload.instanceId >= 2u)
        {
            color = saturate(color + float3(0.0, 0.08, 0.10));
        }
        SmokeOutput[pixel] = float4(saturate(color), 1.0);
    }
    else if (debugMode == 57)
    {
        SmokeOutput[pixel] = EvaluateSmokeMaterialClassifierDebug(payload, pixel, dimensions);
    }
    else if (debugMode == 38)
    {
        SmokeOutput[pixel] = EvaluatePathTracePrimarySurfaceObjectMotionDebug(primaryHistorySurface, pixel);
    }
    else if (debugMode == 39)
    {
        SmokeOutput[pixel] = EvaluatePathTracePrimarySurfaceRigidEligibilityDebug(primaryHistorySurface);
    }
    else if (debugMode == 40)
    {
        SmokeOutput[pixel] = EvaluatePathTracePrimarySurfaceRigidObjectMotionDebug(primaryHistorySurface, pixel);
    }
    else if (debugMode == 41)
    {
        SmokeOutput[pixel] = EvaluatePathTracePrimarySurfaceCombinedObjectMotionDebug(primaryHistorySurface, pixel);
    }
    else if (debugMode == 42)
    {
        SmokeOutput[pixel] = EvaluatePathTracePrimarySurfacePackedObjectMotionDebug(primaryHistorySurface);
    }
    else if (debugMode == 43)
    {
        SmokeOutput[pixel] = EvaluatePathTracePrimarySurfaceObjectMotionReprojectionDebug(primaryHistorySurface, pixel);
    }
    else if (debugMode == 44)
    {
        SmokeOutput[pixel] = EvaluatePathTracePreviousStaticSnapshotDebug(primaryHistorySurface);
    }
    else if (debugMode == 45)
    {
        SmokeOutput[pixel] = EvaluatePathTracePreviousStaticSnapshotReprojectionDebug(primaryHistorySurface);
    }
    else if (debugMode == 46)
    {
        SmokeOutput[pixel] = EvaluatePathTracePreviousStaticSnapshotMotionVectorDebug(primaryHistorySurface, pixel);
    }
    else if (debugMode == 47)
    {
        SmokeOutput[pixel] = EvaluatePathTraceCombinedGeometryMotionVectorDebug(primaryHistorySurface, pixel);
    }
    else if (debugMode == 48)
    {
        SmokeOutput[pixel] = EvaluatePathTraceCombinedGeometryReprojectionDebug(primaryHistorySurface, pixel);
    }
    else if (debugMode == 49)
    {
        SmokeOutput[pixel] = EvaluatePathTraceCombinedGeometryMotionSourceDebug(primaryHistorySurface, pixel);
    }
    else if (debugMode == 52)
    {
        SmokeOutput[pixel] = EvaluatePathTraceRigidRouteTransformParityDebug(payload);
    }
    else if (debugMode == 8)
    {
        if (SmokePayloadIsGuiScreen(payload))
        {
            SmokeOutput[pixel] = CompositeSmokeGuiLayers(ray.Origin, ray.Direction, payload);
            return;
        }
        const PathTraceSmokeMaterial material = LoadSmokeMaterial(payload.materialIndex);
        SmokeOutput[pixel] = SampleSmokeSurfaceAlbedo(material, payload.texCoord, payload.surfaceClass, payload.translucentSubtype, payload.vertexColor, payload.vertexColorAdd);
    }
    else if (debugMode == 9)
    {
        const PathTraceSmokeMaterial material = LoadSmokeMaterial(payload.materialIndex);
        const bool alphaTested = (material.flags & RT_SMOKE_MATERIAL_ALPHA_TEST) != 0u;
        const float coverage = SmokeAlphaCoverage(material, payload.texCoord);
        if (!alphaTested)
        {
            SmokeOutput[pixel] = float4(0.15, 0.15, 0.15, 1.0);
        }
        else if (coverage < material.alphaCutoff)
        {
            SmokeOutput[pixel] = SmokeMissColor();
        }
        else
        {
            SmokeOutput[pixel] = float4(0.0, 1.0, 0.25, 1.0);
        }
    }
    else if (debugMode == 10)
    {
        if (SmokePayloadIsGuiScreen(payload))
        {
            SmokeOutput[pixel] = CompositeSmokeGuiLayers(ray.Origin, ray.Direction, payload);
            return;
        }
        const PathTraceSmokeMaterial material = LoadSmokeMaterial(payload.materialIndex);
        SmokeOutput[pixel] = SampleSmokeSurfaceAlbedo(material, payload.texCoord, payload.surfaceClass, payload.translucentSubtype, payload.vertexColor, payload.vertexColorAdd);
    }
    else if (debugMode == 16)
    {
        const PathTraceSmokeMaterial material = LoadSmokeMaterial(payload.materialIndex);
        SmokeOutput[pixel] = SampleSmokeNormalTexture(material, payload.normalTexCoord);
    }
    else if (debugMode == 17)
    {
        const PathTraceSmokeMaterial material = LoadSmokeMaterial(payload.materialIndex);
        SmokeOutput[pixel] = SampleSmokeSpecularTexture(material, payload.texCoord);
    }
    else if (debugMode == 11)
    {
        const PathTraceSmokeMaterial material = LoadSmokeMaterial(payload.materialIndex);
        const float4 albedo = SampleSmokeSurfaceAlbedo(material, payload.texCoord, payload.surfaceClass, payload.translucentSubtype, payload.vertexColor, payload.vertexColorAdd);
        if (payload.surfaceClass == 3u)
        {
            const float3 materialColor = MaterialIdToColor(payload.materialId);
            const float stripe = frac((payload.texCoord.x + payload.texCoord.y) * 8.0) > 0.5 ? 1.0 : 0.0;
            const float3 overlayColor = lerp(float3(0.0, 1.0, 1.0), float3(1.0, 0.85, 0.0), stripe);
            SmokeOutput[pixel] = float4(lerp(overlayColor, materialColor, 0.25), 1.0);
        }
        else
        {
            SmokeOutput[pixel] = albedo;
        }
    }
    else if (debugMode == 12)
    {
        const PathTraceSmokeMaterial material = LoadSmokeMaterial(payload.materialIndex);
        const float3 albedo = SampleSmokeSurfaceAlbedo(material, payload.texCoord, payload.surfaceClass, payload.translucentSubtype, payload.vertexColor, payload.vertexColorAdd).rgb;
        if (payload.surfaceClass == 3u)
        {
            const float stripe = frac((payload.texCoord.x + payload.texCoord.y) * 12.0) > 0.5 ? 1.0 : 0.0;
            const float3 subtypeColor = TranslucentSubtypeToColor(payload.translucentSubtype);
            const float3 tintedAlbedo = lerp(albedo, subtypeColor, 0.45);
            const float3 stripedTint = lerp(tintedAlbedo, subtypeColor, stripe * 0.35);
            SmokeOutput[pixel] = float4(stripedTint, 1.0);
        }
        else
        {
            SmokeOutput[pixel] = float4(albedo * 0.35, 1.0);
        }
    }
    else if (debugMode == 13)
    {
        const PathTraceSmokeMaterial material = LoadSmokeMaterial(payload.materialIndex);
        const float3 albedo = SampleSmokeSurfaceAlbedo(material, payload.texCoord, payload.surfaceClass, payload.translucentSubtype, payload.vertexColor, payload.vertexColorAdd).rgb;
        if ((material.flags & RT_SMOKE_MATERIAL_ADDITIVE_DECAL) != 0u)
        {
            const float opacity = SmokeAdditiveDecalMaterialOpacity(material, albedo);
            const bool activeEmissiveStage = (payload.triangleClassAndFlags & RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF) == 0u;
            const float3 emissive = debugMode == 14 ? SampleSmokeEmissive(material, payload.texCoord, payload.surfaceClass, activeEmissiveStage) : float3(0.0, 0.0, 0.0);
            SmokeOutput[pixel] = float4(saturate(albedo * (0.35 + opacity * 1.25) + emissive), 1.0);
        }
        else
        {
            const float3 normal = SafeNormalize(payload.normal, payload.geometricNormal);
            const float3 lightDir = normalize(float3(0.35, 0.45, 0.82));
            const float ndotl = saturate(dot(normal, lightDir));
            const float3 ambient = albedo * 0.12;
            const float3 diffuse = albedo * (0.18 + ndotl * 1.15);
            SmokeOutput[pixel] = float4(saturate(ambient + diffuse), 1.0);
        }
    }
    else if (debugMode == 14 || debugMode == 15)
    {
        if (SmokePayloadIsGuiScreen(payload))
        {
            SmokeOutput[pixel] = CompositeSmokeGuiLayers(ray.Origin, ray.Direction, payload);
            return;
        }
        const PathTraceSmokeMaterial material = LoadSmokeMaterial(payload.materialIndex);
        const float3 albedo = SampleSmokeSurfaceAlbedo(material, payload.texCoord, payload.surfaceClass, payload.translucentSubtype, payload.vertexColor, payload.vertexColorAdd).rgb;
        if ((material.flags & RT_SMOKE_MATERIAL_ADDITIVE_DECAL) != 0u)
        {
            const float opacity = SmokeAdditiveDecalMaterialOpacity(material, albedo);
            const bool activeEmissiveStage = (payload.triangleClassAndFlags & RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF) == 0u;
            const float3 emissive = debugMode == 14 ? SampleSmokeEmissive(material, payload.texCoord, payload.surfaceClass, activeEmissiveStage) : float3(0.0, 0.0, 0.0);
            SmokeOutput[pixel] = float4(saturate(albedo * (0.35 + opacity * 1.25) + emissive), 1.0);
        }
        else
        {
            const float3 baseNormal = SafeNormalize(payload.normal, payload.geometricNormal);
            const float3 normal = debugMode == 14
                ? DecodeSmokeNormalTexture(material, payload.normalTexCoord, baseNormal, payload.tangent, payload.bitangent)
                : baseNormal;
            const float3 hitPosition = ray.Origin + ray.Direction * payload.hitT;
            const float3 viewDir = SafeNormalize(ray.Origin - hitPosition, -ray.Direction);
            const float3 specularColor = debugMode == 14 ? SampleSmokeDirectSpecular(material, payload.texCoord) : float3(0.0, 0.0, 0.0);
            const bool activeEmissiveStage = (payload.triangleClassAndFlags & RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF) == 0u;
            const float3 emissive = debugMode == 14 ? SampleSmokeEmissive(material, payload.texCoord, payload.surfaceClass, activeEmissiveStage) : float3(0.0, 0.0, 0.0);
            const float3 ambient = albedo * 0.12;
            const float3 unshadowedFill = albedo * 0.18;
            float3 direct = float3(0.0, 0.0, 0.0);
            float3 dominantLightDebug = float3(0.0, 0.0, 0.0);
            float dominantLightWeight = 0.0;
            const uint lightCount = PathTraceSafetyDisabled(RT_PT_SAFETY_DISABLE_SELECTED_LIGHT_LOOP) ? 0u : min((uint)LightInfo.x, RT_SMOKE_MAX_DEBUG_LIGHTS);
            if (lightCount == 0u)
            {
                if (!DoomAnalyticLightsReplaceSelected() && !PathTraceSafetyDisabled(RT_PT_SAFETY_DISABLE_SELECTED_LIGHT_LOOP))
                {
                    const float3 lightDir = normalize(float3(0.35, 0.45, 0.82));
                    const float ndotl = saturate(dot(normal, lightDir));
                    const float normalOffsetSign = dot(normal, lightDir) >= 0.0 ? 1.0 : -1.0;
                    const float3 shadowOrigin = hitPosition + normal * (normalOffsetSign * 0.75) + lightDir * 0.25;
                    const float visibility = ndotl > 0.0 ? TraceSmokeShadowVisibility(shadowOrigin, lightDir, CameraOriginAndTMax.w, 0xffffffffu, 0xffffffffu, 0xffffffffu) : 0.0;
                    direct = albedo * (ndotl * 1.15 * visibility);
                    direct += EvaluateSmokeSpecular(specularColor, normal, lightDir, viewDir, float3(0.85, 0.85, 1.0), 1.15, visibility);
                    dominantLightDebug = float3(0.85, 0.85, 1.0) * (0.15 + ndotl * visibility);
                    dominantLightWeight = ndotl * visibility;
                }
            }
            else
            {
                [loop]
                for (uint lightIndex = 0u; lightIndex < lightCount; lightIndex++)
                {
                    const float4 lightOriginAndRadius = LightOriginAndRadius[lightIndex];
                    const float3 toLight = lightOriginAndRadius.xyz - hitPosition;
                    const float lightDistance = length(toLight);
                    if (lightDistance <= 1.0e-3)
                    {
                        continue;
                    }

                    const float3 lightDir = toLight / lightDistance;
                    const float ndotl = saturate(dot(normal, lightDir));
                    if (ndotl <= 0.0)
                    {
                        continue;
                    }

                    const float normalOffsetSign = dot(normal, lightDir) >= 0.0 ? 1.0 : -1.0;
                    const float3 shadowOrigin = hitPosition + normal * (normalOffsetSign * 0.75) + lightDir * 0.25;
                    const float shadowTMax = max(lightDistance - 0.5, 0.01);
                    const float visibility = TraceSmokeShadowVisibility(shadowOrigin, lightDir, shadowTMax, 0xffffffffu, 0xffffffffu, 0xffffffffu);
                    const float lightAttenuation = saturate(1.0 - lightDistance / max(lightOriginAndRadius.w, 1.0));
                    const float directScale = 0.12 + lightAttenuation * lightAttenuation * 0.75;
                    const float3 lightColor = max(LightColorAndIntensity[lightIndex].rgb, float3(0.0, 0.0, 0.0));
                    const float contributionWeight = ndotl * directScale * visibility * max(max(lightColor.r, lightColor.g), lightColor.b);
                    direct += albedo * lightColor * (ndotl * directScale * visibility);
                    direct += EvaluateSmokeSpecular(specularColor, normal, lightDir, viewDir, lightColor, directScale, visibility);
                    if (contributionWeight > dominantLightWeight)
                    {
                        dominantLightWeight = contributionWeight;
                        dominantLightDebug = DebugLightSlotColor(lightIndex) * (0.18 + saturate(contributionWeight * 2.0) * 0.82);
                    }
                }
            }
            const uint analyticSeed =
                pixel.x * 1973u ^
                pixel.y * 9277u ^
                payload.materialId * 26699u ^
                ((uint)payload.hitT) * 7919u ^
                payload.materialIndex * 104729u ^
                ((uint)max(ToyPathInfo.w, 0.0)) * 668265263u;
            direct += EvaluateDoomAnalyticSphereLights(albedo, specularColor, normal, viewDir, hitPosition, analyticSeed);
            if (debugMode == 15)
            {
                const float3 base = albedo * 0.08;
                SmokeOutput[pixel] = float4(saturate(base + dominantLightDebug), 1.0);
            }
            else
            {
                const float3 lightSprites = EvaluateSmokeLightSpriteProxies(ray.Origin, ray.Direction, payload.hitT);
                SmokeOutput[pixel] = float4(saturate(ambient + unshadowedFill + direct + emissive + lightSprites), 1.0);
            }
        }
    }
    else if (debugMode == 18)
    {
        const uint samplesPerPixel = PathTraceIntegratorSamplesPerPixel();
        float4 sampleColor = float4(0.0, 0.0, 0.0, 0.0);
        [loop]
        for (uint sampleIndex = 0u; sampleIndex < samplesPerPixel; ++sampleIndex)
        {
            RayDesc sampleRay = ray;
            PathTraceSmokePayload samplePayload = payload;
            if (sampleIndex > 0u)
            {
                const float jitterX = SmokeHashToUnitFloat(pixel.x * 1973u ^ pixel.y * 9277u ^ sampleIndex * 26699u) - 0.5;
                const float jitterY = SmokeHashToUnitFloat(pixel.x * 3923u ^ pixel.y * 5801u ^ sampleIndex * 104729u) - 0.5;
                const float2 sampleUv = (float2(pixel) + 0.5 + float2(jitterX, jitterY)) / float2(dimensions);
                const float2 sampleNdc = sampleUv * 2.0 - 1.0;
                sampleRay.Direction = normalize(
                    CameraForwardAndTanX.xyz +
                    CameraLeftAndTanY.xyz * (-sampleNdc.x * CameraForwardAndTanX.w) +
                    CameraUpAndDebugMode.xyz * (-sampleNdc.y * CameraLeftAndTanY.w));
                samplePayload = InitSmokePayload();
                TraceRay(SmokeScene, RAY_FLAG_NONE, traceMask, 0, 1, 0, sampleRay, samplePayload);
            }

            if (samplePayload.value == 0u)
            {
                sampleColor += float4(0.0, 0.0, 0.0, 1.0);
            }
            else
            {
                uint pathDepth = 0u;
                uint pathFlags = 0u;
                sampleColor += EvaluateSmokeToyPathTrace(sampleRay.Origin, sampleRay.Direction, samplePayload, pixel, sampleIndex, pathDepth, pathFlags);
            }
        }
        sampleColor /= max((float)samplesPerPixel, 1.0);
        sampleColor.a = 1.0;
        const uint accumulationFrame = (uint)max(ToyPathInfo.w, 0.0);
        if (accumulationFrame == 0u)
        {
            SmokeAccumulation[pixel] = sampleColor;
            SmokeOutput[pixel] = sampleColor;
        }
        else
        {
            float4 history = SmokeAccumulation[pixel];
            if (!all(history == history) || any(abs(history) > 65504.0))
            {
                history = sampleColor;
            }
            const float weight = 1.0 / ((float)accumulationFrame + 1.0);
            float4 accumulated = lerp(history, sampleColor, weight);
            accumulated.a = 1.0;
            SmokeAccumulation[pixel] = accumulated;
            SmokeOutput[pixel] = accumulated;
        }
    }
    else
    {
        SmokeOutput[pixel] = float4(0.0, 1.0, 0.0, 1.0);
    }

    SmokeOutput[pixel] = ApplySmokeBoundsOverlay(SmokeOutput[pixel], pixel, dimensions);
#endif
}

[shader("miss")]
void Miss(inout PathTraceSmokePayload payload)
{
    payload.value = 0;
}

[shader("miss")]
void ShadowMiss(inout PathTraceSmokeShadowPayload payload)
{
    payload.hit = 0u;
}

[shader("anyhit")]
void AnyHit(inout PathTraceSmokePayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    const uint instanceId = InstanceID();
    const uint primitiveIndex = PrimitiveIndex();
    if (payload.value == 2u &&
        instanceId == payload.shadowIgnoreInstanceId)
    {
        const uint materialId =
            LoadSmokeTriangleMaterialId(instanceId, primitiveIndex);
        if (primitiveIndex == payload.shadowIgnorePrimitiveIndex ||
            materialId == payload.shadowIgnoreMaterialId)
        {
            IgnoreHit();
            return;
        }
    }
    const uint rejectReason = SmokeAlphaRejectReason(instanceId, primitiveIndex, attributes.barycentrics, payload.value);
    if (rejectReason != RT_STATIC_CONTRACT_REJECT_NONE)
    {
        payload.instanceId = instanceId;
        payload.primitiveIndex = primitiveIndex;
        payload.staticContractRejectReason = rejectReason;
        IgnoreHit();
    }
}

[shader("anyhit")]
void ShadowAnyHit(inout PathTraceSmokeShadowPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    const uint instanceId = InstanceID();
    const uint primitiveIndex = PrimitiveIndex();
    payload.hit = 1u;
    if (payload.rayMode == 2u &&
        instanceId == payload.ignoreInstanceId)
    {
        const uint materialId = LoadSmokeTriangleMaterialId(instanceId, primitiveIndex);
        if (primitiveIndex == payload.ignorePrimitiveIndex || materialId == payload.ignoreMaterialId)
        {
            payload.hit = 0u;
            IgnoreHit();
            return;
        }
    }
    if (SmokeAlphaRejectsHit(instanceId, primitiveIndex, attributes.barycentrics, payload.rayMode))
    {
        payload.hit = 0u;
        IgnoreHit();
    }
}

[shader("closesthit")]
void ShadowClosestHit(inout PathTraceSmokeShadowPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    payload.hit = 1u;
}

[shader("closesthit")]
void ClosestHit(inout PathTraceSmokePayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    const uint instanceId = InstanceID();
    const uint geometryIndex = GeometryIndex();
    const uint primitiveIndex = PrimitiveIndex();
    payload.instanceId = instanceId;
    payload.geometryIndex = geometryIndex;
    payload.primitiveIndex = primitiveIndex;
    if (PathTraceIsRigidHitRouteInstance(instanceId) ||
        PathTraceIsSkinnedHitRouteInstance(instanceId))
    {
        PathTraceSmokeVertex v0;
        PathTraceSmokeVertex v1;
        PathTraceSmokeVertex v2;
        uint routedTriangleClassAndFlags =
            RT_SMOKE_SURFACE_CLASS_RIGID_ENTITY;
        uint routedMaterialId = 0xffffffffu;
        uint routedMaterialIndex = 0xffffffffu;
        bool routedRigid = false;
        PathTraceRigidRouteInstance routeInstance =
            (PathTraceRigidRouteInstance)0;
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
                payload.staticContractRejectReason =
                    RT_STATIC_CONTRACT_REJECT_RIGID_PRIMITIVE_RANGE;
                return;
            }
            v0 = SmokeSkinnedCurrentVertices[vertexIndex0];
            v1 = SmokeSkinnedCurrentVertices[vertexIndex1];
            v2 = SmokeSkinnedCurrentVertices[vertexIndex2];
            routedTriangleClassAndFlags =
                routeTriangle.triangleClassAndFlags;
            routedMaterialId = routeTriangle.materialId;
            routedMaterialIndex = routeTriangle.materialIndex;
        }
        else
        {
            routedRigid = true;
            const uint routeInstanceIndex = instanceId - 2u;
            routeInstance = SmokeRigidRouteInstances[routeInstanceIndex];
            if (primitiveIndex >= routeInstance.triangleCount ||
                routeInstance.triangleOffset + primitiveIndex >=
                    PathTraceRigidRouteTriangleCount())
            {
                payload.staticContractRejectReason =
                    RT_STATIC_CONTRACT_REJECT_RIGID_PRIMITIVE_RANGE;
                return;
            }
            const uint routeIndexOffset =
                routeInstance.indexOffset + primitiveIndex * 3u;
            if (routeIndexOffset > PathTraceRigidRouteIndexCount() ||
                routeIndexOffset + 2u >= PathTraceRigidRouteIndexCount())
            {
                payload.staticContractRejectReason =
                    RT_STATIC_CONTRACT_REJECT_RIGID_INDEX_RANGE;
                return;
            }
            const uint i0 = SmokeRigidRouteIndices[routeIndexOffset + 0u];
            const uint i1 = SmokeRigidRouteIndices[routeIndexOffset + 1u];
            const uint i2 = SmokeRigidRouteIndices[routeIndexOffset + 2u];
            if (i0 >= routeInstance.vertexCount ||
                i1 >= routeInstance.vertexCount ||
                i2 >= routeInstance.vertexCount ||
                routeInstance.vertexOffset + i0 >=
                    PathTraceRigidRouteVertexCount() ||
                routeInstance.vertexOffset + i1 >=
                    PathTraceRigidRouteVertexCount() ||
                routeInstance.vertexOffset + i2 >=
                    PathTraceRigidRouteVertexCount())
            {
                payload.staticContractRejectReason =
                    RT_STATIC_CONTRACT_REJECT_RIGID_VERTEX_RANGE;
                return;
            }
            v0 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i0];
            v1 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i1];
            v2 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i2];
            routedMaterialId = SmokeRigidRouteTriangleMaterials[
                routeInstance.triangleOffset + primitiveIndex];
            routedMaterialIndex =
                SmokeRigidRouteTriangleMaterialIndexes[
                    routeInstance.triangleOffset + primitiveIndex];
        }
        const float3 barycentrics = float3(1.0 - attributes.barycentrics.x - attributes.barycentrics.y, attributes.barycentrics.x, attributes.barycentrics.y);
        const float3 p0 = v0.position.xyz;
        const float3 p1 = v1.position.xyz;
        const float3 p2 = v2.position.xyz;
        const float3 localHit = p0 * barycentrics.x + p1 * barycentrics.y + p2 * barycentrics.z;
        const float3 n0 = v0.normal.xyz;
        const float3 n1 = v1.normal.xyz;
        const float3 n2 = v2.normal.xyz;
        const float4 t0 = v0.tangent;
        const float4 t1 = v1.tangent;
        const float4 t2 = v2.tangent;
        const float4 b0 = v0.bitangent;
        const float4 b1 = v1.bitangent;
        const float4 b2 = v2.bitangent;
        const float2 uv0 = v0.texCoord.xy;
        const float2 uv1 = v1.texCoord.xy;
        const float2 uv2 = v2.texCoord.xy;
        const float2 normalUv0 = v0.texCoord.zw;
        const float2 normalUv1 = v1.texCoord.zw;
        const float2 normalUv2 = v2.texCoord.zw;

        payload.value = 1;
        payload.hitT = RayTCurrent();
        payload.hitBarycentrics = attributes.barycentrics;
        const float3 actualWorld = WorldRayOrigin() + WorldRayDirection() * payload.hitT;
        const float3 routeWorld = routedRigid
            ? TransformPathTraceRigidRoutePoint(
                routeInstance.currentObjectToWorld0,
                routeInstance.currentObjectToWorld1,
                routeInstance.currentObjectToWorld2,
                localHit)
            : localHit;
        payload.debugVector = float3(
            length(routeWorld - actualWorld),
            0.0,
            0.0);
        payload.debugFlags = 0x1u;
        const float3 worldRayFallback = SafeNormalize(-WorldRayDirection(), float3(0.0, 0.0, 1.0));
        const float3 objectGeometricNormal = SafeNormalize(cross(p1 - p0, p2 - p0), worldRayFallback);
        payload.geometricNormal = routedRigid
            ? SafeNormalize(
                TransformRigidRouteVector(
                    routeInstance,
                    objectGeometricNormal),
                worldRayFallback)
            : objectGeometricNormal;
        const float3 objectInterpolatedNormal = SafeNormalize(n0 * barycentrics.x + n1 * barycentrics.y + n2 * barycentrics.z, objectGeometricNormal);
        payload.normal = routedRigid
            ? SafeNormalize(
                TransformRigidRouteVector(
                    routeInstance,
                    objectInterpolatedNormal),
                payload.geometricNormal)
            : objectInterpolatedNormal;
        payload.texCoord = uv0 * barycentrics.x + uv1 * barycentrics.y + uv2 * barycentrics.z;
        payload.normalTexCoord = normalUv0 * barycentrics.x + normalUv1 * barycentrics.y + normalUv2 * barycentrics.z;
        payload.vertexColor = saturate(v0.color * barycentrics.x + v1.color * barycentrics.y + v2.color * barycentrics.z);
        payload.vertexColorAdd = saturate(v0.color2 * barycentrics.x + v1.color2 * barycentrics.y + v2.color2 * barycentrics.z);
        const float3 tangentFallback = BuildPerpendicular(payload.normal);
        const float3 bitangentFallback = SafeNormalize(cross(payload.normal, tangentFallback), float3(0.0, 1.0, 0.0));
        float4 capturedTangent =
            t0 * barycentrics.x +
            t1 * barycentrics.y +
            t2 * barycentrics.z;
        float4 capturedBitangent =
            b0 * barycentrics.x +
            b1 * barycentrics.y +
            b2 * barycentrics.z;
        if (routedRigid)
        {
            capturedTangent.xyz = TransformRigidRouteVector(
                routeInstance,
                capturedTangent.xyz);
            capturedBitangent.xyz = TransformRigidRouteVector(
                routeInstance,
                capturedBitangent.xyz);
        }
        if (!TryBuildCapturedTangentBasis(
                payload.normal,
                capturedTangent,
                capturedBitangent,
                routedRigid
                    ? RigidRouteHandednessSign(routeInstance)
                    : 1.0,
                payload.tangent,
                payload.bitangent))
        {
            const float3 dp1 = p1 - p0;
            const float3 dp2 = p2 - p0;
            const float2 duv1 = uv1 - uv0;
            const float2 duv2 = uv2 - uv0;
            const float uvDeterminant = duv1.x * duv2.y - duv1.y * duv2.x;
            if (abs(uvDeterminant) > 1.0e-8)
            {
                const float inverseDeterminant = 1.0 / uvDeterminant;
                const float3 objectRawTangent = (dp1 * duv2.y - dp2 * duv1.y) * inverseDeterminant;
                const float3 objectRawBitangent = (dp2 * duv1.x - dp1 * duv2.x) * inverseDeterminant;
                const float3 rawTangent = routedRigid
                    ? TransformRigidRouteVector(
                        routeInstance,
                        objectRawTangent)
                    : objectRawTangent;
                const float3 rawBitangent = routedRigid
                    ? TransformRigidRouteVector(
                        routeInstance,
                        objectRawBitangent)
                    : objectRawBitangent;
                payload.tangent = SafeNormalize(rawTangent - payload.normal * dot(payload.normal, rawTangent), tangentFallback);
                payload.bitangent = SafeNormalize(rawBitangent - payload.normal * dot(payload.normal, rawBitangent) - payload.tangent * dot(payload.tangent, rawBitangent), bitangentFallback);
                if (dot(cross(payload.tangent, payload.bitangent), payload.normal) < 0.0)
                {
                    payload.bitangent = -payload.bitangent;
                }
            }
            else
            {
                payload.tangent = tangentFallback;
                payload.bitangent = bitangentFallback;
            }
        }
        payload.surfaceClass =
            routedTriangleClassAndFlags & RT_SMOKE_TRIANGLE_CLASS_MASK;
        payload.translucentSubtype =
            (routedTriangleClassAndFlags &
                RT_SMOKE_TRANSLUCENT_SUBTYPE_MASK) >>
            RT_SMOKE_TRANSLUCENT_SUBTYPE_SHIFT;
        payload.triangleClassAndFlags = routedTriangleClassAndFlags;
        payload.materialId = routedMaterialId;
        payload.materialIndex = routedMaterialIndex;
        return;
    }
    if (instanceId >= 2u)
    {
        payload.staticContractRejectReason =
            RT_STATIC_CONTRACT_REJECT_RIGID_INSTANCE_RANGE;
        return;
    }
    if (!SmokeTriangleIndexRangeValid(instanceId, primitiveIndex))
    {
        payload.staticContractRejectReason = RT_STATIC_CONTRACT_REJECT_TRIANGLE_RANGE;
        return;
    }
    const uint indexOffset = primitiveIndex * 3;
    const uint i0 = instanceId == 0 ? SmokeStaticIndices[indexOffset + 0] : SmokeDynamicIndices[indexOffset + 0];
    const uint i1 = instanceId == 0 ? SmokeStaticIndices[indexOffset + 1] : SmokeDynamicIndices[indexOffset + 1];
    const uint i2 = instanceId == 0 ? SmokeStaticIndices[indexOffset + 2] : SmokeDynamicIndices[indexOffset + 2];
    const uint vertexCount = instanceId == 0 ? PathTraceStaticVertexCount() : PathTraceDynamicVertexCount();
    if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
    {
        payload.staticContractRejectReason = RT_STATIC_CONTRACT_REJECT_VERTEX_RANGE;
        return;
    }

    const float3 p0 = (instanceId == 0 ? SmokeStaticVertices[i0].position : SmokeDynamicVertices[i0].position).xyz;
    const float3 p1 = (instanceId == 0 ? SmokeStaticVertices[i1].position : SmokeDynamicVertices[i1].position).xyz;
    const float3 p2 = (instanceId == 0 ? SmokeStaticVertices[i2].position : SmokeDynamicVertices[i2].position).xyz;
    const float3 n0 = (instanceId == 0 ? SmokeStaticVertices[i0].normal : SmokeDynamicVertices[i0].normal).xyz;
    const float3 n1 = (instanceId == 0 ? SmokeStaticVertices[i1].normal : SmokeDynamicVertices[i1].normal).xyz;
    const float3 n2 = (instanceId == 0 ? SmokeStaticVertices[i2].normal : SmokeDynamicVertices[i2].normal).xyz;
    const float4 t0 = instanceId == 0 ? SmokeStaticVertices[i0].tangent : SmokeDynamicVertices[i0].tangent;
    const float4 t1 = instanceId == 0 ? SmokeStaticVertices[i1].tangent : SmokeDynamicVertices[i1].tangent;
    const float4 t2 = instanceId == 0 ? SmokeStaticVertices[i2].tangent : SmokeDynamicVertices[i2].tangent;
    const float4 b0 = instanceId == 0 ? SmokeStaticVertices[i0].bitangent : SmokeDynamicVertices[i0].bitangent;
    const float4 b1 = instanceId == 0 ? SmokeStaticVertices[i1].bitangent : SmokeDynamicVertices[i1].bitangent;
    const float4 b2 = instanceId == 0 ? SmokeStaticVertices[i2].bitangent : SmokeDynamicVertices[i2].bitangent;
    const float2 uv0 = (instanceId == 0 ? SmokeStaticVertices[i0].texCoord : SmokeDynamicVertices[i0].texCoord).xy;
    const float2 uv1 = (instanceId == 0 ? SmokeStaticVertices[i1].texCoord : SmokeDynamicVertices[i1].texCoord).xy;
    const float2 uv2 = (instanceId == 0 ? SmokeStaticVertices[i2].texCoord : SmokeDynamicVertices[i2].texCoord).xy;
    const float2 normalUv0 = (instanceId == 0 ? SmokeStaticVertices[i0].texCoord : SmokeDynamicVertices[i0].texCoord).zw;
    const float2 normalUv1 = (instanceId == 0 ? SmokeStaticVertices[i1].texCoord : SmokeDynamicVertices[i1].texCoord).zw;
    const float2 normalUv2 = (instanceId == 0 ? SmokeStaticVertices[i2].texCoord : SmokeDynamicVertices[i2].texCoord).zw;
    const float4 c0 = instanceId == 0 ? SmokeStaticVertices[i0].color : SmokeDynamicVertices[i0].color;
    const float4 c1 = instanceId == 0 ? SmokeStaticVertices[i1].color : SmokeDynamicVertices[i1].color;
    const float4 c2 = instanceId == 0 ? SmokeStaticVertices[i2].color : SmokeDynamicVertices[i2].color;
    const float4 c20 = instanceId == 0 ? SmokeStaticVertices[i0].color2 : SmokeDynamicVertices[i0].color2;
    const float4 c21 = instanceId == 0 ? SmokeStaticVertices[i1].color2 : SmokeDynamicVertices[i1].color2;
    const float4 c22 = instanceId == 0 ? SmokeStaticVertices[i2].color2 : SmokeDynamicVertices[i2].color2;
    const float3 barycentrics = float3(1.0 - attributes.barycentrics.x - attributes.barycentrics.y, attributes.barycentrics.x, attributes.barycentrics.y);

    payload.value = 1;
    payload.hitT = RayTCurrent();
    payload.hitBarycentrics = attributes.barycentrics;
    const uint triangleClassAndFlags = LoadSmokeTriangleClassAndFlags(instanceId, primitiveIndex);
    const bool forceGeometricNormal = (triangleClassAndFlags & RT_SMOKE_TRIANGLE_FORCE_GEOMETRIC_NORMAL) != 0;
    payload.geometricNormal = SafeNormalize(cross(p1 - p0, p2 - p0), float3(0.0, 0.0, 1.0));
    const float3 interpolatedNormal = SafeNormalize(n0 * barycentrics.x + n1 * barycentrics.y + n2 * barycentrics.z, payload.geometricNormal);
    payload.normal = forceGeometricNormal ? payload.geometricNormal : interpolatedNormal;
    const float3 tangentFallback = BuildPerpendicular(payload.normal);
    const float3 bitangentFallback = SafeNormalize(cross(payload.normal, tangentFallback), float3(0.0, 1.0, 0.0));
    const float4 capturedTangent = t0 * barycentrics.x + t1 * barycentrics.y + t2 * barycentrics.z;
    const float4 capturedBitangent = b0 * barycentrics.x + b1 * barycentrics.y + b2 * barycentrics.z;
    if (!TryBuildCapturedTangentBasis(
            payload.normal,
            capturedTangent,
            capturedBitangent,
            1.0,
            payload.tangent,
            payload.bitangent))
    {
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
            payload.tangent = SafeNormalize(rawTangent - payload.normal * dot(payload.normal, rawTangent), tangentFallback);
            payload.bitangent = SafeNormalize(rawBitangent - payload.normal * dot(payload.normal, rawBitangent) - payload.tangent * dot(payload.tangent, rawBitangent), bitangentFallback);
            if (dot(cross(payload.tangent, payload.bitangent), payload.normal) < 0.0)
            {
                payload.bitangent = -payload.bitangent;
            }
        }
        else
        {
            payload.tangent = tangentFallback;
            payload.bitangent = bitangentFallback;
        }
    }
    payload.texCoord = uv0 * barycentrics.x + uv1 * barycentrics.y + uv2 * barycentrics.z;
    payload.normalTexCoord = normalUv0 * barycentrics.x + normalUv1 * barycentrics.y + normalUv2 * barycentrics.z;
    payload.vertexColor = saturate(c0 * barycentrics.x + c1 * barycentrics.y + c2 * barycentrics.z);
    payload.vertexColorAdd = saturate(c20 * barycentrics.x + c21 * barycentrics.y + c22 * barycentrics.z);
    payload.surfaceClass = triangleClassAndFlags & RT_SMOKE_TRIANGLE_CLASS_MASK;
    payload.translucentSubtype = (triangleClassAndFlags & RT_SMOKE_TRANSLUCENT_SUBTYPE_MASK) >> RT_SMOKE_TRANSLUCENT_SUBTYPE_SHIFT;
    payload.triangleClassAndFlags = triangleClassAndFlags;
    payload.materialId = LoadSmokeTriangleMaterialId(instanceId, primitiveIndex);
    payload.materialIndex = instanceId == 0 ? SmokeStaticTriangleMaterialIndexes[primitiveIndex] : SmokeDynamicTriangleMaterialIndexes[primitiveIndex];
}
