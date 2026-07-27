#include "../../vulkan.hlsli"
#define RB_PT_RIGID_HIT_ROUTE_INSTANCE_COUNT() \
    PathTraceRigidRouteInstanceCount()
#include "PathTraceSkinnedHitRoute.hlsli"
#ifndef __cplusplus
#ifndef uint16_t
#define uint16_t uint
#endif
#endif

#define RT_SMOKE_DECAL_BIN_SIZE 3
#define RT_LIQUID_POOL_CANDIDATE_CAPACITY 4

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
    // Detail-decal blend-through bin (docs/decal_cards/02 M4): any-hit accumulates
    // decal layers here and IgnoreHit()s; the base wall stays the committed
    // closest hit and the layers composite at surface-build time.
    uint decalCount;
    uint decalMaterialIndex[RT_SMOKE_DECAL_BIN_SIZE];
    uint decalPackedTexCoord[RT_SMOKE_DECAL_BIN_SIZE];
    uint decalSortKey[RT_SMOKE_DECAL_BIN_SIZE];
    float decalHitT[RT_SMOKE_DECAL_BIN_SIZE];
    // LPD-04 candidate transport. Static authored cards use instance 0;
    // switchable authored cards use the per-frame dynamic BLAS at instance 1.
    // Routed-rigid instances remain fail-closed.
    uint liquidRawCount;
    uint liquidRetainedCount;
    uint liquidStatusMask;
    uint liquidRejectionCount;
    uint liquidInstanceId[RT_LIQUID_POOL_CANDIDATE_CAPACITY];
    uint liquidMaterialIndex[RT_LIQUID_POOL_CANDIDATE_CAPACITY];
    uint liquidPrimitiveIndex[RT_LIQUID_POOL_CANDIDATE_CAPACITY];
    uint liquidBarycentricXBits[RT_LIQUID_POOL_CANDIDATE_CAPACITY];
    uint liquidBarycentricYBits[RT_LIQUID_POOL_CANDIDATE_CAPACITY];
    float liquidHitT[RT_LIQUID_POOL_CANDIDATE_CAPACITY];
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

struct PathTraceDynamicMaterialRecord
{
    float4 color;
    float4 texMatrix0; // .w = evaluated condition register
    float4 texMatrix1; // .w = evaluated alpha-test value
    uint materialIndex;
    uint materialId;
    uint stageIndex;
    uint flags;
};

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
    float4 texMatrix0;
    float4 texMatrix1;
    float4 currentObjectToWorld0;
    float4 currentObjectToWorld1;
    float4 currentObjectToWorld2;
    float4 previousObjectToWorld0;
    float4 previousObjectToWorld1;
    float4 previousObjectToWorld2;
};

struct RAB_Material
{
    uint materialId;
    uint materialIndex;
    uint flags;
    float alphaCutoff;
    float3 diffuseAlbedo;
    float roughness;
    float3 specularF0;
    float opacity;
    float3 emissiveRadiance;
    uint emissiveTextureIndex;
};

struct RAB_Surface
{
    uint valid;
    float3 worldPos;
    float linearDepth;
    float3 geometryNormal;
    uint materialId;
    float3 shadingNormal;
    uint materialIndex;
    float3 viewDir;
    uint instanceId;
    uint primitiveIndex;
    uint surfaceClass;
    uint flags;
    RAB_Material material;
};

#include "PathTracePrimarySurface.hlsli"
#include "PathTraceMaterialFeatureTypes.hlsli"
#include "cleanroom_common/pathtrace_liquid_pool_modifier.hlsli"
#include "cleanroom_common/pathtrace_liquid_pool_control.hlsli"

RaytracingAccelerationStructure SmokeScene : register(t0);
VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> SmokeOutput : register(u1);
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> PathTraceMotionVectors : register(u39);
VK_IMAGE_FORMAT("r32ui") RWTexture2D<uint> PathTraceMotionVectorMask : register(u40);
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> PathTraceRRGuideAlbedo : register(u48);
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> PathTraceRRGuideNormalRoughness : register(u49);
VK_IMAGE_FORMAT("r32f") RWTexture2D<float> PathTraceRRGuideDepth : register(u50);
VK_IMAGE_FORMAT("r32f") RWTexture2D<float> PathTraceRRGuideHitDistance : register(u51);
VK_IMAGE_FORMAT("r32ui") RWTexture2D<uint> PathTraceRRGuideResetMask : register(u52);
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> PathTraceRRGuideSpecularAlbedo : register(u53);
VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> PathTraceRRGuidePosition : register(u79);
VK_IMAGE_FORMAT("rg16f") RWTexture2D<float2> PathTraceRRMotionVectors : register(u78);
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
StructuredBuffer<PathTraceSkinnedSurfaceDispatchRecord> SmokeSkinnedSurfaceDispatch : register(t33);
StructuredBuffer<PathTraceSmokeVertex> SmokePreviousStaticVertices : register(t34);
StructuredBuffer<uint> SmokePreviousStaticIndices : register(t35);
StructuredBuffer<uint> SmokePreviousStaticTriangleClasses : register(t36);
StructuredBuffer<uint> SmokePreviousStaticTriangleMaterials : register(t37);
StructuredBuffer<uint> SmokePreviousStaticTriangleMaterialIndexes : register(t38);
StructuredBuffer<uint> SmokeSkinnedTriangleDispatchIndexes : register(t41);
StructuredBuffer<PathTraceDynamicMaterialRecord> SmokeDynamicMaterials : register(t76);
StructuredBuffer<PathTraceMaterialFeatureRecord> PathTraceMaterialFeatures : register(t80);
StructuredBuffer<PathTraceMaterialFeatureParameterRecord> PathTraceMaterialFeatureParameters : register(t81);
#define RB_PT_ENABLE_STATIC_BUCKET_SHADER_CONSUMERS 1
#include "PathTraceStaticBucketRoute.hlsli"
#undef RB_PT_ENABLE_STATIC_BUCKET_SHADER_CONSUMERS
RWStructuredBuffer<uint> PathTraceLiquidPoolStatusCounters : register(u82);
Texture2D<float4> SmokeFallbackTexture : register(t14);
RWStructuredBuffer<PathTracePrimarySurfaceRecord> PrimarySurfaceHistoryCurrent : register(u30);
RWStructuredBuffer<PathTracePrimarySurfaceRecord> PrimarySurfaceHistoryPrevious : register(u31);
VK_BINDING(0, 1) Texture2D<float4> SmokeDiffuseTextures[] : register(t0, space1);
SamplerState SmokeMaterialSampler : register(s0);
#define RB_PATH_TRACE_FIXED_SKY_CUBE 1
#include "pathtrace_sky_environment.hlsli"
#undef RB_PATH_TRACE_FIXED_SKY_CUBE

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
    float4 RRProjectionDepthInfo;
    // Padding spans the light-manager/verifier/ReGIR/NEE-cache fields of the CPU
    // PathTraceSmokeConstants struct, which this shader does not consume.
    float4 ProducerReservedTail[20];
    float4 DecalInfo;
    float4 DecalInfo2;
    // x=effective mode, y=debug, z=debug page, w=status telemetry bound/ready.
    float4 LiquidPoolInfo;
    // x=reserved first InstanceID, y=published resident surface-record count,
    // z/w=publication generation low/high.
    uint4 StaticBucketRouteInfo;
};

bool TryLoadPathTraceLiquidPoolMaterialFeatureParameters(
    uint materialIndex,
    out PathTraceMaterialFeatureParameterRecord parameters)
{
    parameters = PathTraceDefaultLiquidPoolMaterialFeatureParameters();
    uint parameterRecordCount = 0u;
    uint parameterRecordStride = 0u;
    PathTraceMaterialFeatureParameters.GetDimensions(parameterRecordCount, parameterRecordStride);
    const uint materialCount = (uint)max(TextureInfo.z, 0.0);
    // Reusable GPU buffers retain capacity from larger earlier material tables.
    // TextureInfo.z is the authoritative logical row count for this frame;
    // allocated capacity may exceed it but must never be smaller.
    if (parameterRecordCount < materialCount ||
        parameterRecordStride != RT_PATH_TRACE_MATERIAL_FEATURE_PARAMETER_RECORD_STRIDE ||
        materialIndex >= materialCount)
    {
        return false;
    }

    parameters = PathTraceSanitizeLiquidPoolMaterialFeatureParameters(
        PathTraceMaterialFeatureParameters[materialIndex]);
    return true;
}

bool TryLoadPathTraceMaterialFeatureRecord(
    uint materialIndex,
    out PathTraceMaterialFeatureRecord feature)
{
    feature = (PathTraceMaterialFeatureRecord)0;
    uint featureRecordCount = 0u;
    uint featureRecordStride = 0u;
    PathTraceMaterialFeatures.GetDimensions(featureRecordCount, featureRecordStride);
    const uint materialCount = (uint)max(TextureInfo.z, 0.0);
    if (featureRecordCount < materialCount ||
        featureRecordStride != 32u ||
        materialIndex >= materialCount)
    {
        return false;
    }

    feature = PathTraceMaterialFeatures[materialIndex];
    return feature.recordAbiVersion == RT_PATH_TRACE_MATERIAL_FEATURE_RECORD_ABI_VERSION &&
        feature.parameterRecordIndex == materialIndex;
}

bool PathTraceMaterialIsSemanticLiquidPool(uint materialIndex)
{
    PathTraceMaterialFeatureRecord feature;
    return TryLoadPathTraceMaterialFeatureRecord(materialIndex, feature) &&
        feature.materialKind == RT_PATH_TRACE_MATERIAL_KIND_LIQUID_POOL_MODIFIER &&
        feature.modifierKind == RT_PATH_TRACE_MATERIAL_MODIFIER_LIQUID_POOL_UNION &&
        (feature.materialCaps & (RT_PATH_TRACE_MATERIAL_CAP_RECEIVER_MODIFIER |
            RT_PATH_TRACE_MATERIAL_CAP_IDEMPOTENT_MODIFIER_BLEND)) ==
            (RT_PATH_TRACE_MATERIAL_CAP_RECEIVER_MODIFIER |
                RT_PATH_TRACE_MATERIAL_CAP_IDEMPOTENT_MODIFIER_BLEND);
}

#define RB_PATH_TRACE_PRIMARY_SURFACE_HAS_RR_PROJECTION_DEPTH_INFO
#define RB_PATH_TRACE_PRIMARY_SURFACE_ENABLE_PROJECTION_HELPERS
#include "PathTracePrimarySurface.hlsli"

static const uint RT_SMOKE_TRIANGLE_CLASS_MASK = 0x0000ffffu;
static const uint RT_SMOKE_TRIANGLE_FORCE_GEOMETRIC_NORMAL = 0x00010000u;
static const uint RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF = 0x00040000u;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_SHIFT = 24u;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_MASK = 0x0f000000u;
static const uint RT_SMOKE_SURFACE_CLASS_RIGID_ENTITY = 1u;
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
static const uint RT_SMOKE_MATERIAL_DETAIL_DECAL = 0x00002000u;
static const uint RT_SMOKE_MATERIAL_DETAIL_DECAL_DYNAMIC = 0x00004000u;
static const uint RT_SMOKE_MATERIAL_DETAIL_DECAL_DIFFUSE_LIT = 0x00008000u;
static const uint RT_SMOKE_MATERIAL_DETAIL_DECAL_LIQUID_POOL = 0x00010000u;
static const uint RT_SMOKE_MATERIAL_SKY_ENVIRONMENT = 0x00040000u;
static const uint RT_SMOKE_DYNAMIC_MATERIAL_RECORD_VALID = 0x00000001u;
static const uint RT_SMOKE_DYNAMIC_MATERIAL_RECORD_STAGE_ENABLED = 0x00000002u;
static const uint RT_SMOKE_DYNAMIC_MATERIAL_RECORD_SELECTED_EMISSIVE = 0x00000004u;
static const uint RT_SMOKE_DYNAMIC_MATERIAL_RECORD_REPLACE_EMISSIVE = 0x00000200u;
static const uint RT_SMOKE_MATERIAL_DYNAMIC_EMISSIVE_REGISTER_MASK = 0x0000003cu;
static const uint PT_MOTION_VECTOR_MASK_VALID = 0x00000001u;
static const uint PT_MOTION_VECTOR_MASK_SOURCE_SHIFT = 1u;
static const uint PT_MOTION_VECTOR_MASK_INVALID_REASON_SHIFT = 5u;
static const uint PT_MOTION_VECTOR_SOURCE_UNKNOWN = 0u;
static const uint PT_MOTION_VECTOR_SOURCE_STATIC = 1u;
static const uint PT_MOTION_VECTOR_SOURCE_SKINNED = 2u;
static const uint PT_MOTION_VECTOR_SOURCE_RIGID = 3u;
static const uint PT_MOTION_VECTOR_SOURCE_OTHER_OBJECT = 4u;
static const uint RT_RR_RESET_INVALID_SURFACE = 0x00000001u;
static const uint RT_RR_RESET_MISSING_PREVIOUS = 0x00000002u;
static const uint RT_RR_RESET_REJECTED_PREVIOUS = 0x00000004u;
static const uint RT_RR_RESET_MATERIAL_MISMATCH = 0x00000008u;
static const uint RT_RR_RESET_OBJECT_MOTION_UNAVAILABLE = 0x00000010u;
static const uint RT_RR_RESET_STOCHASTIC_TRANSLUCENT = 0x00000020u;
static const uint RT_RR_RESET_OTHER_INVALID = 0x00000040u;
static const uint PT_SKINNED_DISPATCH_HAS_VALID_PREVIOUS = 0x00000001u;
static const uint PT_SKINNED_DISPATCH_RT_CPU_SKINNED = 0x00000002u;
static const uint PT_RIGID_ROUTE_HAS_PREVIOUS_TRANSFORM = 0x00000001u;
static const uint PT_RIGID_ROUTE_TRANSFORM_CONTINUOUS = 0x00000002u;
static const uint RT_SMOKE_TEXTURE_FLAG_USE_NORMAL_MAPS = 0x00000008u;
static const uint RT_SMOKE_TEXTURE_FLAG_USE_SPECULAR_MAPS = 0x00000010u;
static const uint RT_SMOKE_TEXTURE_FLAG_USE_EMISSIVE_MAPS = 0x00000020u;
static const uint RT_SMOKE_TEXTURE_FLAG_TOY_FAKE_PBR_SPECULAR = 0x00000080u;
static const uint RT_SMOKE_MATERIAL_OVERRIDE_ZERO_ROUGHNESS = 0x00000001u;
static const uint RT_PT_SAFETY_DISABLE_ANY_HIT_ALPHA = 0x00000001u;
static const uint RT_PT_SAFETY_DISABLE_PRIMARY_SURFACE_HISTORY = 0x00000040u;
static const uint RT_SMOKE_RAY_MODE_PRIMARY_FILTER_DECAL_COMPOSITE = 3u;
static const uint RT_SMOKE_SURFACE_CLASS_SKINNED_DEFORMED = 2u;
#include "pathtrace_material_classifier.hlsli"

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

uint2 PathTraceFullOutputSize()
{
    const uint2 size = uint2((uint)max(DispatchTileInfo.z, 0.0), (uint)max(DispatchTileInfo.w, 0.0));
    return (size.x > 0u && size.y > 0u) ? size : DispatchRaysDimensions().xy;
}

uint2 PathTraceDispatchTileOffset()
{
    return uint2((uint)max(DispatchTileInfo.x, 0.0), (uint)max(DispatchTileInfo.y, 0.0));
}

bool PathTraceSafetyDisabled(uint bit)
{
    return (((uint)SafetyInfo.x) & bit) != 0u;
}

// Detail-decal composite staging (docs/decal_cards/06): 0=legacy stochastic,
// 1=composite, 2=offset-geometry only, 3=any-hit collect only, 4=composite diagnostic.
uint PathTraceDecalCompositeStage()
{
    return (uint)max(DecalInfo.x, 0.0);
}

bool PathTraceDecalCollectEnabled(uint stage)
{
    return stage == 1u || stage == 3u || stage == 4u;
}

uint PathTraceLiquidPoolMode()
{
    return (uint)clamp(LiquidPoolInfo.x, 0.0, 3.0);
}

uint PathTraceLiquidPoolDebug()
{
    return (uint)clamp(LiquidPoolInfo.y, 0.0, 6.0);
}

uint PathTraceLiquidPoolDebugPage()
{
    return (uint)clamp(LiquidPoolInfo.z, 0.0, 3.0);
}

bool PathTraceLiquidPoolCollectionEnabled()
{
    const uint controlFlags = (uint)max(LiquidPoolInfo.w, 0.0);
    return PathTraceLiquidPoolMode() != 0u &&
        (controlFlags & RT_LIQUID_POOL_CONTROL_TELEMETRY_READY) != 0u;
}

float3 SafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-8 ? value * rsqrt(lengthSquared) : fallback;
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

float SmokeLinear1(float c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 SmokeLinear3(float3 c)
{
    return float3(SmokeLinear1(c.r), SmokeLinear1(c.g), SmokeLinear1(c.b));
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
    return stableHash ^
        pixel.x * 1597334677u ^
        pixel.y * 3812015801u ^
        salt * 3266489917u;
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

bool SmokeToyFakePBRSpecularEnabled()
{
    return (((uint)TextureInfo.w) & RT_SMOKE_TEXTURE_FLAG_TOY_FAKE_PBR_SPECULAR) != 0u;
}

RAB_Material RAB_EmptyMaterial()
{
    RAB_Material material = (RAB_Material)0;
    material.roughness = 1.0;
    material.opacity = 1.0;
    return material;
}

RAB_Surface RAB_EmptySurface()
{
    RAB_Surface surface = (RAB_Surface)0;
    surface.material = RAB_EmptyMaterial();
    return surface;
}

bool RAB_IsSurfaceValid(RAB_Surface surface)
{
    return surface.valid != 0u;
}

float GetRoughness(RAB_Material material)
{
    return material.roughness;
}

float4 LoadSmokeTextureTexel(uint textureIndex, uint2 texel, bool bindlessEnabled)
{
    return bindlessEnabled
        ? SmokeDiffuseTextures[NonUniformResourceIndex(textureIndex)].Load(int3(texel, 0))
        : SmokeFallbackTexture.Load(int3(0, 0, 0));
}

float4 SampleSmokeTextureLoad(uint textureIndex, uint textureWidth, uint textureHeight, float2 wrappedTexCoord, bool bindlessEnabled, bool bilinearFilter)
{
    const uint width = max(textureWidth, 1u);
    const uint height = max(textureHeight, 1u);
    if (!bilinearFilter || width == 1u || height == 1u)
    {
        const uint2 texel = min(uint2(wrappedTexCoord * float2(width, height)), uint2(width - 1u, height - 1u));
        return LoadSmokeTextureTexel(textureIndex, texel, bindlessEnabled);
    }

    const float2 texelPosition = wrappedTexCoord * float2(width, height) - 0.5;
    const float2 basePosition = floor(texelPosition);
    const float2 fraction = frac(texelPosition);
    const int2 baseTexel = int2(basePosition);
    const uint2 textureSize = uint2(width, height);
    const uint2 texel00 = uint2((baseTexel + int2(0, 0) + int2(width, height)) % int2(textureSize));
    const uint2 texel10 = uint2((baseTexel + int2(1, 0) + int2(width, height)) % int2(textureSize));
    const uint2 texel01 = uint2((baseTexel + int2(0, 1) + int2(width, height)) % int2(textureSize));
    const uint2 texel11 = uint2((baseTexel + int2(1, 1) + int2(width, height)) % int2(textureSize));
    const float4 c00 = LoadSmokeTextureTexel(textureIndex, texel00, bindlessEnabled);
    const float4 c10 = LoadSmokeTextureTexel(textureIndex, texel10, bindlessEnabled);
    const float4 c01 = LoadSmokeTextureTexel(textureIndex, texel01, bindlessEnabled);
    const float4 c11 = LoadSmokeTextureTexel(textureIndex, texel11, bindlessEnabled);
    return lerp(lerp(c00, c10, fraction.x), lerp(c01, c11, fraction.x), fraction.y);
}

float4 SampleSmokeTexture(uint textureIndex, uint textureWidth, uint textureHeight, float2 texCoord, float4 fallback)
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
    float4 sampled = sampleMethod == 2u
        ? SampleSmokeTextureLoad(textureIndex, textureWidth, textureHeight, wrappedTexCoord, bindlessEnabled, bilinearFilter)
        : (bindlessEnabled
            ? SmokeDiffuseTextures[NonUniformResourceIndex(textureIndex)].SampleLevel(SmokeMaterialSampler, wrappedTexCoord, 0.0)
            : SmokeFallbackTexture.SampleLevel(SmokeMaterialSampler, wrappedTexCoord, 0.0));
    return (!all(sampled == sampled) || any(abs(sampled) > 65504.0)) ? fallback : sampled;
}

float3 ConvertSmokeYCoCgToRGB(float4 ycocg)
{
    ycocg.z = (ycocg.z * 31.875) + 1.0;
    ycocg.z = 1.0 / ycocg.z;
    ycocg.xy *= ycocg.z;
    return saturate(float3(
        dot(ycocg, float4(1.0, -1.0, 0.0, 1.0)),
        dot(ycocg, float4(0.0, 1.0, -0.50196078, 1.0)),
        dot(ycocg, float4(-1.0, -1.0, 1.00392156, 1.0))));
}

float4 SampleSmokeDecodedDiffuseTexture(PathTraceSmokeMaterial material, float2 texCoord)
{
    float4 texel = SampleSmokeTexture(material.diffuseTextureIndex, material.textureWidth, material.textureHeight, texCoord, material.debugAlbedo);
    const bool textureDecodeEnabled = (((uint)TextureInfo.w) & 4u) != 0u;
    if (textureDecodeEnabled && (material.flags & RT_SMOKE_MATERIAL_DIFFUSE_YCOCG) != 0u)
    {
        texel.rgb = ConvertSmokeYCoCgToRGB(texel);
    }
    return texel;
}

float4 SampleSmokeDiffuseTexture(PathTraceSmokeMaterial material, float2 texCoord)
{
    return (material.flags & RT_SMOKE_MATERIAL_FORCE_DEBUG_ALBEDO) != 0u
        ? material.debugAlbedo
        : SampleSmokeDecodedDiffuseTexture(material, texCoord);
}

float4 SampleSmokeSurfaceAlbedo(PathTraceSmokeMaterial material, float2 texCoord, uint surfaceClass, uint translucentSubtype, float4 vertexColor, float4 vertexColorAdd)
{
    float4 albedo = SampleSmokeDiffuseTexture(material, texCoord);
    if (surfaceClass == RT_SMOKE_SURFACE_CLASS_TRANSLUCENT && translucentSubtype == RT_SMOKE_TRANSLUCENT_SUBTYPE_GUI_SCREEN)
    {
        albedo = material.diffuseTextureIndex != 0xffffffffu
            ? float4(albedo.rgb * vertexColor.rgb, albedo.a * vertexColor.a)
            : vertexColor;
    }
    return saturate(albedo);
}

bool SmokeMaterialUsesUnlitColorFallback(PathTraceSmokeMaterial material, uint surfaceClass, uint translucentSubtype)
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

float4 SampleSmokeAlphaTexture(PathTraceSmokeMaterial material, float2 texCoord)
{
    return material.alphaTextureIndex != 0xffffffffu
        ? SampleSmokeTexture(material.alphaTextureIndex, material.alphaTextureWidth, material.alphaTextureHeight, texCoord, SampleSmokeDiffuseTexture(material, texCoord))
        : SampleSmokeDiffuseTexture(material, texCoord);
}

float SmokeAlphaCoverage(PathTraceSmokeMaterial material, float2 texCoord)
{
    if ((material.flags & RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_DARK_KEY) != 0u)
    {
        const float3 decoded = SampleSmokeDecodedDiffuseTexture(material, texCoord).rgb;
        return 1.0 - max(max(decoded.r, decoded.g), decoded.b);
    }
    if ((material.flags & RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_LUMA) != 0u)
    {
        const float3 decoded = SampleSmokeDecodedDiffuseTexture(material, texCoord).rgb;
        return max(max(decoded.r, decoded.g), decoded.b);
    }
    if ((material.flags & RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_MAGENTA_KEY) != 0u)
    {
        const float3 decoded = SampleSmokeDecodedDiffuseTexture(material, texCoord).rgb;
        const float keyDistance = max(abs(decoded.r - 1.0), max(abs(decoded.g), abs(decoded.b - 1.0)));
        return keyDistance <= 0.08 ? 0.0 : 1.0;
    }
    return SampleSmokeAlphaTexture(material, texCoord).a;
}

float2 LiquidPoolCoverageTexelSize(PathTraceSmokeMaterial material)
{
    const uint diffuseCoverageFlags =
        RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_DARK_KEY |
        RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_LUMA |
        RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_MAGENTA_KEY;
    const bool coverageUsesDiffuse =
        (material.flags & diffuseCoverageFlags) != 0u ||
        material.alphaTextureIndex == 0xffffffffu;
    const uint width = coverageUsesDiffuse ? material.textureWidth : material.alphaTextureWidth;
    const uint height = coverageUsesDiffuse ? material.textureHeight : material.alphaTextureHeight;
    return rcp(float2(max(width, 1u), max(height, 1u)));
}

float3 DecodeSmokeNormalTexture(PathTraceSmokeMaterial material, float2 texCoord, float3 normal, float3 tangent, float3 bitangent)
{
    if ((material.normalTextureIndex == 0xffffffffu) || ((((uint)TextureInfo.w) & RT_SMOKE_TEXTURE_FLAG_USE_NORMAL_MAPS) == 0u))
    {
        return normal;
    }
    const float4 bump = SampleSmokeTexture(material.normalTextureIndex, material.normalTextureWidth, material.normalTextureHeight, texCoord, float4(0.5, 0.5, 1.0, 1.0)) * 2.0 - 1.0;
    if (!all(bump == bump))
    {
        return normal;
    }
    const float2 normalXY = SmokeMatClassNormalXY(material, bump, RestirPTInfo.y);
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
    return SafeNormalize(tangent * decoded.x + bitangent * decoded.y + normal * decoded.z, normal);
}

float3 ConstrainSmokeShadingNormal(float3 shadingNormal, float3 geometryNormal)
{
    const float minGeometryDot = 0.02;
    geometryNormal = SafeNormalize(geometryNormal, float3(0.0, 0.0, 1.0));
    shadingNormal = SafeNormalize(shadingNormal, geometryNormal);
    const float geometryDot = dot(shadingNormal, geometryNormal);
    if (geometryDot >= minGeometryDot)
    {
        return shadingNormal;
    }
    float3 tangentComponent = shadingNormal - geometryNormal * geometryDot;
    const float tangentLengthSquared = dot(tangentComponent, tangentComponent);
    if (tangentLengthSquared <= 1e-8)
    {
        return geometryNormal;
    }
    tangentComponent *= rsqrt(tangentLengthSquared);
    const float tangentScale = sqrt(max(1.0 - minGeometryDot * minGeometryDot, 0.0));
    return SafeNormalize(tangentComponent * tangentScale + geometryNormal * minGeometryDot, geometryNormal);
}

float3 SampleSmokeDirectSpecular(PathTraceSmokeMaterial material, float2 texCoord)
{
    if ((material.specularTextureIndex == 0xffffffffu) || ((((uint)TextureInfo.w) & RT_SMOKE_TEXTURE_FLAG_USE_SPECULAR_MAPS) == 0u))
    {
        return float3(0.0, 0.0, 0.0);
    }
    return saturate(SampleSmokeTexture(material.specularTextureIndex, material.specularTextureWidth, material.specularTextureHeight, texCoord, float4(0.0, 0.0, 0.0, 1.0)).rgb);
}

float3 SampleSmokeEmissive(PathTraceSmokeMaterial material, float2 texCoord, uint surfaceClass, bool activeEmissiveStage)
{
    if ((material.flags & RT_SMOKE_MATERIAL_EMISSIVE) == 0u ||
        !activeEmissiveStage ||
        surfaceClass == RT_SMOKE_SURFACE_CLASS_SKINNED_DEFORMED ||
        ((((uint)TextureInfo.w) & RT_SMOKE_TEXTURE_FLAG_USE_EMISSIVE_MAPS) == 0u))
    {
        return float3(0.0, 0.0, 0.0);
    }

    float3 emissive = max(material.emissiveColor.rgb, float3(0.0, 0.0, 0.0));
    if (material.emissiveTextureIndex != 0xffffffffu)
    {
        emissive *= saturate(SampleSmokeTexture(material.emissiveTextureIndex, material.emissiveTextureWidth, material.emissiveTextureHeight, texCoord, float4(1.0, 1.0, 1.0, 1.0)).rgb);
    }

    return emissive * 1.75;
}

uint PathTraceDynamicMaterialRecordCount()
{
    return (uint)max(DecalInfo2.y, 0.0);
}

void ApplyPathTraceDynamicMaterialRecord(uint materialIndex, inout PathTraceSmokeMaterial material)
{
    const uint recordCount = PathTraceDynamicMaterialRecordCount();
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

PathTraceSmokeMaterial LoadSmokeMaterial(uint materialIndex)
{
    PathTraceSmokeMaterial material = (PathTraceSmokeMaterial)0;
    material.debugAlbedo = float4(1.0, 0.0, 1.0, 1.0);
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
        ApplyPathTraceDynamicMaterialRecord(materialIndex, material);
    }
    return material;
}

RAB_Material RAB_BuildMaterialFromSmokePayload(PathTraceSmokePayload payload)
{
    const PathTraceSmokeMaterial smokeMaterial = LoadSmokeMaterial(payload.materialIndex);
    const float4 albedo = SampleSmokeSurfaceAlbedo(smokeMaterial, payload.texCoord, payload.surfaceClass, payload.translucentSubtype, payload.vertexColor, payload.vertexColorAdd);
    float3 materialAlbedo = albedo.rgb;
    const float3 specularColor = SampleSmokeDirectSpecular(smokeMaterial, payload.texCoord);
    float3 specularF0 = specularColor;
    float roughness = 1.0;
    if (SmokeToyFakePBRSpecularEnabled())
    {
        SmokePBRFromSpecmap(saturate(specularColor), specularF0, roughness);
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
    const bool activeEmissiveStage = (payload.triangleClassAndFlags & RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF) == 0u;

    RAB_Material material = RAB_EmptyMaterial();
    material.materialId = payload.materialId;
    material.materialIndex = payload.materialIndex;
    material.flags = smokeMaterial.flags;
    material.alphaCutoff = smokeMaterial.alphaCutoff;
    material.diffuseAlbedo = materialAlbedo;
    material.roughness = roughness;
    material.specularF0 = specularF0;
    material.opacity = (smokeMaterial.flags & RT_SMOKE_MATERIAL_ALPHA_TEST) != 0u
        ? SmokeAlphaCoverage(smokeMaterial, payload.texCoord)
        : 1.0;
    material.emissiveRadiance = SampleSmokeEmissive(smokeMaterial, payload.texCoord, payload.surfaceClass, activeEmissiveStage) * max(ToyPathInfo.z, 0.0);
    if (activeEmissiveStage && SmokeMaterialUsesUnlitColorFallback(smokeMaterial, payload.surfaceClass, payload.translucentSubtype))
    {
        material.emissiveRadiance = max(material.emissiveRadiance, materialAlbedo);
    }
    material.emissiveTextureIndex = smokeMaterial.emissiveTextureIndex;
    if ((smokeMaterial.flags & RT_SMOKE_MATERIAL_SKY_ENVIRONMENT) != 0u)
    {
        material.diffuseAlbedo = float3(0.0, 0.0, 0.0);
        material.specularF0 = float3(0.0, 0.0, 0.0);
        material.roughness = 1.0;
        material.opacity = 1.0;
        material.emissiveRadiance = max(smokeMaterial.emissiveColor.rgb, float3(0.0, 0.0, 0.0));
        material.emissiveTextureIndex = 0xffffffffu;
    }
    return material;
}

#define RB_PT_ENABLE_RESTIR 1
#include "pathtrace_smoke_rab_surface_supplier.hlsli"
#undef RB_PT_ENABLE_RESTIR

PathTraceSmokePayload InitSmokePayload();
bool SmokePayloadIsGuiScreen(PathTraceSmokePayload payload);

RAB_Surface BuildSurfaceFromPayload(PathTraceSmokePayload payload, float3 rayOrigin, float3 rayDirection)
{
    RAB_Surface surface = RAB_BuildSurfaceFromSmokePayload(payload, rayOrigin, rayDirection, true);
    if (surface.valid != 0u && (surface.material.flags & RT_SMOKE_MATERIAL_SKY_ENVIRONMENT) != 0u)
    {
        const float3 skyTexel = PathTraceSampleSkyEnvironment(rayDirection, TextureInfo) *
            max(SafetyInfo.y, 0.0);
        surface.material.emissiveRadiance = max(
            skyTexel * max(surface.material.emissiveRadiance, float3(0.0, 0.0, 0.0)),
            float3(0.0, 0.0, 0.0));
    }
    return surface;
}

bool SmokePayloadIsFilterDecal(PathTraceSmokePayload payload)
{
    return payload.value != 0u &&
        (LoadSmokeMaterial(payload.materialIndex).flags & RT_SMOKE_MATERIAL_FILTER_DECAL) != 0u;
}

bool ResolvePrimaryFilterDecalReceiver(inout PathTraceSmokePayload payload, RayDesc ray, out PathTraceSmokePayload decalPayload)
{
    decalPayload = payload;
    if (!SmokePayloadIsFilterDecal(payload))
    {
        return false;
    }

    PathTraceSmokePayload receiverPayload = InitSmokePayload();
    // Seed the retrace with the first trace's dedicated set. Any-hit deduplicates
    // the exact five-word occurrence key before capacity accounting.
    receiverPayload.liquidRawCount = payload.liquidRawCount;
    receiverPayload.liquidRetainedCount = payload.liquidRetainedCount;
    receiverPayload.liquidStatusMask = payload.liquidStatusMask;
    receiverPayload.liquidRejectionCount = payload.liquidRejectionCount;
    [unroll]
    for (uint liquidSlot = 0u; liquidSlot < RT_LIQUID_POOL_CANDIDATE_CAPACITY; ++liquidSlot)
    {
        receiverPayload.liquidInstanceId[liquidSlot] = payload.liquidInstanceId[liquidSlot];
        receiverPayload.liquidMaterialIndex[liquidSlot] = payload.liquidMaterialIndex[liquidSlot];
        receiverPayload.liquidPrimitiveIndex[liquidSlot] = payload.liquidPrimitiveIndex[liquidSlot];
        receiverPayload.liquidBarycentricXBits[liquidSlot] = payload.liquidBarycentricXBits[liquidSlot];
        receiverPayload.liquidBarycentricYBits[liquidSlot] = payload.liquidBarycentricYBits[liquidSlot];
        receiverPayload.liquidHitT[liquidSlot] = payload.liquidHitT[liquidSlot];
    }
    receiverPayload.value = 2u;
    receiverPayload.shadowIgnoreInstanceId = payload.instanceId;
    receiverPayload.shadowIgnorePrimitiveIndex = payload.primitiveIndex;
    receiverPayload.shadowIgnoreMaterialId = payload.materialId;

    RayDesc receiverRay = ray;
    receiverRay.TMin = max(ray.TMin, max(payload.hitT - 0.05, 0.001));
    TraceRay(SmokeScene, RAY_FLAG_NONE, 0xff, 0, 0, 0, receiverRay, receiverPayload);
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

uint2 PathTracePrimarySurfaceStorePixel(uint2 pixel)
{
    return pixel + PathTraceDispatchTileOffset();
}

int2 PathTracePrimarySurfaceLoadPixel(int2 pixelPosition, bool previousFrame)
{
    return pixelPosition;
}

#define RB_PT_STATIC_BUCKET_MOTION_ENABLED 1
#define RB_PT_STATIC_BUCKET_MOTION_INSTANCE(instanceId) \
    PathTraceIsStaticBucketRouteInstance( \
        instanceId, \
        StaticBucketRouteInfo)
#include "pathtrace_smoke_rab_motion_supplier.hlsli"
#undef RB_PT_STATIC_BUCKET_MOTION_INSTANCE
#undef RB_PT_STATIC_BUCKET_MOTION_ENABLED
#include "cleanroom_rtxdi/pathtrace_clean_rtxdi_di_rr_geometry_guides.hlsli"

void StorePrimarySurfaceRecord(uint2 pixel, RAB_Surface surface)
{
    StorePathTracePrimarySurfaceRecord(pixel, surface);
}

// Specular albedo for DLSS-RR: material F0 plus emissive color injection.
// All emissives (static texture and variable/parm-driven) need to appear in
// the specular guide so RR can reconstruct self-lit surfaces. Preserve
// chromaticity; do not dump full HDR energy into the albedo-range channel.
float3 PathTracePrimarySurfaceRrSpecularAlbedo(RAB_Surface surface)
{
    float3 specular = saturate(surface.material.specularF0);
    const float3 emissive = max(surface.material.emissiveRadiance, float3(0.0, 0.0, 0.0));
    const float peak = max(max(emissive.r, emissive.g), emissive.b);
    if (peak > 1.0e-4)
    {
        return max(specular, saturate(emissive / max(peak, 1.0e-5)));
    }

    // Variable stage color only when radiance sample is zero but a live
    // dynamic emissive stage is selected (parm on, texture empty).
    const uint materialIndex = surface.material.materialIndex;
    const uint recordCount = PathTraceDynamicMaterialRecordCount();
    if (materialIndex < recordCount)
    {
        const PathTraceDynamicMaterialRecord record = SmokeDynamicMaterials[materialIndex];
        if ((record.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_VALID) != 0u &&
            record.materialIndex == materialIndex &&
            (record.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_SELECTED_EMISSIVE) != 0u &&
            (record.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_STAGE_ENABLED) != 0u &&
            record.texMatrix0.w != 0.0 &&
            max(max(record.color.r, record.color.g), record.color.b) > 1.0e-5)
        {
            return max(specular, saturate(record.color.rgb * saturate(record.color.a)));
        }
    }

    return specular;
}

void StoreRayReconstructionGuides(uint2 pixel, RAB_Surface surface)
{
    pixel = PathTracePrimarySurfaceStorePixel(pixel);
    const uint2 dimensions = PathTraceFullOutputSize();
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    if (!RAB_IsSurfaceValid(surface))
    {
        PathTraceRRGuideAlbedo[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        PathTraceRRGuideSpecularAlbedo[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        PathTraceRRGuideNormalRoughness[pixel] = float4(0.0, 0.0, 1.0, 1.0);
        PathTraceRRGuideDepth[pixel] = PathTraceCleanRtxdiDiRRInvalidDepth();
        PathTraceRRGuideHitDistance[pixel] = 0.0;
        PathTraceRRGuidePosition[pixel] = PathTraceCleanRtxdiDiRRInvalidPosition();
        PathTraceRRMotionVectors[pixel] = float2(0.0, 0.0);
        return;
    }

    const float3 normal = SafeNormalize(surface.shadingNormal, surface.geometryNormal);
    PathTraceRRGuideAlbedo[pixel] = float4(saturate(surface.material.diffuseAlbedo), saturate(surface.material.opacity));
    PathTraceRRGuideSpecularAlbedo[pixel] = float4(PathTracePrimarySurfaceRrSpecularAlbedo(surface), 1.0);
    PathTraceRRGuideNormalRoughness[pixel] = float4(normal, saturate(surface.material.roughness));
    PathTraceRRGuideDepth[pixel] = PathTraceCleanRtxdiDiRRPrimaryDepthFromSurface(surface);
    PathTraceRRGuideHitDistance[pixel] = 0.0;
    PathTraceRRGuidePosition[pixel] = PathTraceCleanRtxdiDiRRPositionFromSurface(surface);
}

uint RayReconstructionResetMaskFromStatus(RAB_Surface surface, bool motionValid, uint debugStatus)
{
    uint mask = 0u;
    if (!RAB_IsSurfaceValid(surface))
    {
        return RT_RR_RESET_INVALID_SURFACE;
    }
    if (surface.surfaceClass == RT_SMOKE_SURFACE_CLASS_TRANSLUCENT)
    {
        mask |= RT_RR_RESET_STOCHASTIC_TRANSLUCENT;
    }
    if (motionValid)
    {
        return mask;
    }

    if (debugStatus == RT_PRIMARY_SURFACE_DEBUG_MISSING_PREVIOUS ||
        debugStatus == RT_PRIMARY_SURFACE_DEBUG_SKINNED_MISSING_PREVIOUS ||
        debugStatus == RT_PRIMARY_SURFACE_DEBUG_RIGID_MISSING_PREVIOUS)
    {
        mask |= RT_RR_RESET_MISSING_PREVIOUS;
    }
    else if (debugStatus == RT_PRIMARY_SURFACE_DEBUG_REJECTED_PREVIOUS)
    {
        mask |= RT_RR_RESET_REJECTED_PREVIOUS;
    }
    else if (debugStatus == RT_PRIMARY_SURFACE_DEBUG_MATERIAL_MISMATCH ||
        debugStatus == RT_PRIMARY_SURFACE_DEBUG_STATIC_MATERIAL_CLASS_MISMATCH ||
        debugStatus == RT_PRIMARY_SURFACE_DEBUG_LIQUID_FILM_TRANSITION ||
        debugStatus == RT_PRIMARY_SURFACE_DEBUG_LIQUID_ALBEDO_MISMATCH ||
        debugStatus == RT_PRIMARY_SURFACE_DEBUG_LIQUID_F0_MISMATCH ||
        debugStatus == RT_PRIMARY_SURFACE_DEBUG_LIQUID_ROUGHNESS_MISMATCH ||
        debugStatus == RT_PRIMARY_SURFACE_DEBUG_LIQUID_NORMAL_MISMATCH)
    {
        mask |= RT_RR_RESET_MATERIAL_MISMATCH;
    }
    else if (debugStatus == RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION ||
        debugStatus == RT_PRIMARY_SURFACE_DEBUG_SKINNED_RANGE_MISMATCH ||
        debugStatus == RT_PRIMARY_SURFACE_DEBUG_SKINNED_PREVIOUS_OUT_OF_RANGE ||
        debugStatus == RT_PRIMARY_SURFACE_DEBUG_RIGID_RANGE_MISMATCH ||
        debugStatus == RT_PRIMARY_SURFACE_DEBUG_STATIC_RANGE_MISMATCH ||
        debugStatus == RT_PRIMARY_SURFACE_DEBUG_PACKED_OBJECT_MISSING_PREVIOUS_MOTION)
    {
        mask |= RT_RR_RESET_OBJECT_MOTION_UNAVAILABLE;
    }
    else
    {
        mask |= RT_RR_RESET_OTHER_INVALID;
    }
    return mask;
}

bool TryRayReconstructionCameraMotionPixelsAndDepth(
    RAB_Surface surface,
    uint2 pixel,
    out int2 previousPixel,
    out float2 previousPixelFloat,
    out float2 motionPixels,
    out float expectedPrevDepth,
    out uint debugStatus)
{
    previousPixel = int2(-1, -1);
    previousPixelFloat = float2(-1.0, -1.0);
    motionPixels = float2(0.0, 0.0);
    expectedPrevDepth = 0.0;
    debugStatus = RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION;

    if (!RAB_IsSurfaceValid(surface))
    {
        debugStatus = RT_PRIMARY_SURFACE_DEBUG_MISSING_CURRENT;
        return false;
    }
    if (!ProjectPathTracePrimarySurfaceToPreviousPixelFloatAndDepthWithStatus(
        surface.worldPos,
        PathTraceFullOutputSize(),
        previousPixelFloat,
        expectedPrevDepth,
        debugStatus))
    {
        return false;
    }

    expectedPrevDepth = PathTraceCleanRtxdiDiRRPreviousDepthFromWorldPosition(surface.worldPos);
    previousPixel = int2(floor(previousPixelFloat));
    motionPixels = previousPixelFloat - (float2(pixel) + 0.5);
    return true;
}

void StoreRayReconstructionMotionGuides(uint2 pixel, RAB_Surface surface)
{
    const bool motionVectorExportEnabled = MotionVectorInfo.x >= 0.5;
    pixel = PathTracePrimarySurfaceStorePixel(pixel);
    const uint2 dimensions = PathTraceFullOutputSize();
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    uint sourceKind = PathTraceMotionVectorSourceKind(surface);
    if (RestirPTSurfaceInfo.w >= 0.5 && sourceKind == PT_MOTION_VECTOR_SOURCE_RIGID)
    {
        if (motionVectorExportEnabled)
        {
            PathTraceMotionVectors[pixel] = float4(0.0, 0.0, 0.0, 0.0);
            PathTraceRRMotionVectors[pixel] = float2(0.0, 0.0);
            PathTraceMotionVectorMask[pixel] = PathTraceMotionVectorMaskFromStatus(false, sourceKind, RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION);
        }
        PathTraceRRGuideResetMask[pixel] = RayReconstructionResetMaskFromStatus(surface, false, RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION);
        return;
    }

    int2 previousPixel;
    float2 previousPixelFloat;
    float2 motionPixels;
    float expectedPrevDepth;
    uint debugStatus;
    if (TryPathTraceCombinedGeometryMotionPixelsAndDepth(surface, pixel, previousPixel, previousPixelFloat, motionPixels, expectedPrevDepth, debugStatus, sourceKind))
    {
        if (motionVectorExportEnabled)
        {
            if (RayReconstructionInfo.z >= 0.5)
            {
                // Streamline's positive jitter offset denotes a projection shift whose
                // inverse primary ray lands at pixel - jitter.  Remove that sampling
                // displacement from the exported unjittered motion vector.
                motionPixels += RayReconstructionInfo.xy;
            }
            PathTraceMotionVectors[pixel] = float4(motionPixels, PathTraceCleanRtxdiDiRRMotionDepthDelta(surface, expectedPrevDepth), 0.0);
            PathTraceRRMotionVectors[pixel] = motionPixels;
            PathTraceMotionVectorMask[pixel] = PathTraceMotionVectorMaskFromStatus(true, sourceKind, debugStatus);
        }
        PathTraceRRGuideResetMask[pixel] = RayReconstructionResetMaskFromStatus(surface, true, debugStatus);
    }
    else if (TryRayReconstructionCameraMotionPixelsAndDepth(surface, pixel, previousPixel, previousPixelFloat, motionPixels, expectedPrevDepth, debugStatus))
    {
        if (motionVectorExportEnabled)
        {
            if (RayReconstructionInfo.z >= 0.5)
            {
                motionPixels += RayReconstructionInfo.xy;
            }
            PathTraceMotionVectors[pixel] = float4(motionPixels, PathTraceCleanRtxdiDiRRMotionDepthDelta(surface, expectedPrevDepth), 0.0);
            PathTraceRRMotionVectors[pixel] = motionPixels;
            PathTraceMotionVectorMask[pixel] = PathTraceMotionVectorMaskFromStatus(true, sourceKind, debugStatus);
        }
        PathTraceRRGuideResetMask[pixel] = RayReconstructionResetMaskFromStatus(surface, true, debugStatus);
    }
    else
    {
        if (motionVectorExportEnabled)
        {
            PathTraceMotionVectors[pixel] = float4(0.0, 0.0, 0.0, 0.0);
            PathTraceRRMotionVectors[pixel] = float2(0.0, 0.0);
            PathTraceMotionVectorMask[pixel] = PathTraceMotionVectorMaskFromStatus(false, sourceKind, debugStatus);
        }
        PathTraceRRGuideResetMask[pixel] = RayReconstructionResetMaskFromStatus(surface, false, debugStatus);
    }
}

PathTraceSmokePayload InitSmokePayload()
{
    PathTraceSmokePayload payload;
    payload.value = 0u;
    payload.hitT = 0.0;
    payload.normal = float3(0.0, 0.0, 0.0);
    payload.geometricNormal = float3(0.0, 0.0, 0.0);
    payload.tangent = float3(1.0, 0.0, 0.0);
    payload.bitangent = float3(0.0, 1.0, 0.0);
    payload.texCoord = float2(0.0, 0.0);
    payload.normalTexCoord = float2(0.0, 0.0);
    payload.hitBarycentrics = float2(0.0, 0.0);
    payload.vertexColor = float4(1.0, 1.0, 1.0, 1.0);
    payload.vertexColorAdd = float4(0.5, 0.5, 0.5, 0.5);
    payload.surfaceClass = 4u;
    payload.translucentSubtype = RT_SMOKE_TRANSLUCENT_SUBTYPE_GUI_SCREEN;
    payload.triangleClassAndFlags = 4u;
    payload.materialId = 0u;
    payload.materialIndex = 0u;
    payload.instanceId = 0xffffffffu;
    payload.geometryIndex = 0xffffffffu;
    payload.primitiveIndex = 0xffffffffu;
    payload.shadowIgnoreInstanceId = 0xffffffffu;
    payload.shadowIgnorePrimitiveIndex = 0xffffffffu;
    payload.shadowIgnoreMaterialId = 0xffffffffu;
    payload.debugVector = float3(0.0, 0.0, 0.0);
    payload.debugFlags = 0u;
    payload.decalCount = 0u;
    [unroll]
    for (uint decalSlot = 0u; decalSlot < RT_SMOKE_DECAL_BIN_SIZE; ++decalSlot)
    {
        payload.decalMaterialIndex[decalSlot] = 0xffffffffu;
        payload.decalPackedTexCoord[decalSlot] = 0u;
        payload.decalSortKey[decalSlot] = 0u;
        payload.decalHitT[decalSlot] = 0.0;
    }
    payload.liquidRawCount = 0u;
    payload.liquidRetainedCount = 0u;
    payload.liquidStatusMask = 0u;
    payload.liquidRejectionCount = 0u;
    [unroll]
    for (uint liquidSlot = 0u; liquidSlot < RT_LIQUID_POOL_CANDIDATE_CAPACITY; ++liquidSlot)
    {
        payload.liquidInstanceId[liquidSlot] = 0xffffffffu;
        payload.liquidMaterialIndex[liquidSlot] = 0xffffffffu;
        payload.liquidPrimitiveIndex[liquidSlot] = 0xffffffffu;
        payload.liquidBarycentricXBits[liquidSlot] = 0u;
        payload.liquidBarycentricYBits[liquidSlot] = 0u;
        payload.liquidHitT[liquidSlot] = 0.0;
    }
    return payload;
}

bool SmokePayloadIsGuiScreen(PathTraceSmokePayload payload)
{
    return payload.value != 0u &&
        payload.surfaceClass == RT_SMOKE_SURFACE_CLASS_TRANSLUCENT &&
        payload.translucentSubtype == RT_SMOKE_TRANSLUCENT_SUBTYPE_GUI_SCREEN;
}

bool PathTraceTryResolvePrimaryStaticBucketHit(
    uint instanceId,
    uint geometryIndex,
    uint primitiveIndex,
    out PathTraceStaticGeometryAddress address)
{
    return PathTraceTryResolveStaticBucketGeometryAddress(
        instanceId,
        geometryIndex,
        primitiveIndex,
        StaticBucketRouteInfo,
        PathTraceStaticVertexCount(),
        PathTraceStaticIndexCount(),
        PathTraceStaticTriangleCount(),
        address);
}

bool PathTraceStaticBucketTryLoadPrimaryMetadata(
    PathTraceStaticGeometryAddress address,
    out uint triangleClassAndFlags,
    out uint materialId,
    out uint materialIndex,
    out uint materialFlags)
{
    triangleClassAndFlags =
        SmokeStaticBucketTriangleClasses[address.triangleIndex];
    materialId =
        SmokeStaticBucketTriangleMaterials[address.triangleIndex];
    materialIndex =
        SmokeStaticBucketTriangleMaterialIndexes[
            address.triangleIndex];

    const uint materialCount = (uint)max(TextureInfo.z, 0.0);
    materialFlags = 0u;
    if (materialIndex >= materialCount)
    {
        return false;
    }

    materialFlags = LoadSmokeMaterial(materialIndex).flags;
    return true;
}

bool PathTraceStaticBucketIsOpaqueOrAlphaPrimaryCandidate(
    uint triangleClassAndFlags,
    uint materialFlags)
{
    const uint deferredPrimaryFlags =
        RT_SMOKE_MATERIAL_ADDITIVE_DECAL |
        RT_SMOKE_MATERIAL_FILTER_DECAL |
        RT_SMOKE_MATERIAL_PORTAL_WINDOW_FALLBACK |
        RT_SMOKE_MATERIAL_OBJECT_GLASS_FALLBACK |
        RT_SMOKE_MATERIAL_DETAIL_DECAL |
        RT_SMOKE_MATERIAL_DETAIL_DECAL_DYNAMIC |
        RT_SMOKE_MATERIAL_DETAIL_DECAL_DIFFUSE_LIT |
        RT_SMOKE_MATERIAL_DETAIL_DECAL_LIQUID_POOL;
    return
        (triangleClassAndFlags & RT_SMOKE_TRIANGLE_CLASS_MASK) !=
            RT_SMOKE_SURFACE_CLASS_TRANSLUCENT &&
        (materialFlags & deferredPrimaryFlags) == 0u;
}

bool PathTraceStaticBucketIsPrimaryGlassCandidate(
    uint triangleClassAndFlags,
    uint materialFlags)
{
    const uint surfaceClass =
        triangleClassAndFlags & RT_SMOKE_TRIANGLE_CLASS_MASK;
    const uint translucentSubtype =
        (triangleClassAndFlags &
            RT_SMOKE_TRANSLUCENT_SUBTYPE_MASK) >>
        RT_SMOKE_TRANSLUCENT_SUBTYPE_SHIFT;
    const bool portalWindow =
        translucentSubtype ==
            RT_SMOKE_TRANSLUCENT_SUBTYPE_PORTAL_WINDOW;
    const bool objectGlass =
        translucentSubtype ==
            RT_SMOKE_TRANSLUCENT_SUBTYPE_OBJECT_GLASS &&
        (materialFlags &
            RT_SMOKE_MATERIAL_OBJECT_GLASS_FALLBACK) != 0u;
    return
        surfaceClass == RT_SMOKE_SURFACE_CLASS_TRANSLUCENT &&
        (portalWindow || objectGlass);
}

float2 PathTraceStaticBucketInterpolateTexCoord(
    PathTraceStaticGeometryAddress address,
    float2 hitBarycentrics)
{
    const float3 weights = float3(
        1.0 - hitBarycentrics.x - hitBarycentrics.y,
        hitBarycentrics.x,
        hitBarycentrics.y);
    return
        SmokeStaticBucketVertices[
            address.vertexIndexes.x].texCoord.xy * weights.x +
        SmokeStaticBucketVertices[
            address.vertexIndexes.y].texCoord.xy * weights.y +
        SmokeStaticBucketVertices[
            address.vertexIndexes.z].texCoord.xy * weights.z;
}

// Static detail-decal cards are lifted along the face normal oriented by the
// authored vertex normal (ApplySmokeDetailDecalNormalOffset). A full-resident
// bucket TLAS can expose the back of a nearby card that belongs to the opposite
// side of thin geometry. Its ray-T separation can still pass the receiver
// envelope at grazing angles, so only collect the authored outward face.
bool PathTraceStaticBucketDetailDecalFacesPrimaryRay(
    PathTraceStaticGeometryAddress address)
{
    const PathTraceSmokeVertex v0 =
        SmokeStaticBucketVertices[address.vertexIndexes.x];
    const PathTraceSmokeVertex v1 =
        SmokeStaticBucketVertices[address.vertexIndexes.y];
    const PathTraceSmokeVertex v2 =
        SmokeStaticBucketVertices[address.vertexIndexes.z];
    float3 outwardFaceNormal = cross(
        v1.position.xyz - v0.position.xyz,
        v2.position.xyz - v0.position.xyz);
    const float faceLengthSquared =
        dot(outwardFaceNormal, outwardFaceNormal);
    if (!(faceLengthSquared > 1.0e-12))
    {
        return false;
    }
    if (dot(outwardFaceNormal, v0.normal.xyz) < 0.0)
    {
        outwardFaceNormal = -outwardFaceNormal;
    }
    return dot(outwardFaceNormal, WorldRayDirection()) < 0.0;
}

uint PathTraceStaticBucketCanonicalSurfaceInstanceId(
    PathTraceStaticGeometryAddress address)
{
    return PATH_TRACE_STATIC_BUCKET_INSTANCE_ID_BASE |
        address.surfaceRecordIndex;
}

// Liquid candidates outlive any-hit and are reconstructed in raygen. Their
// existing payload has no GeometryIndex field, so canonicalize a bucket hit to
// one surface-record InstanceID and retain the original monolithic source
// triangle as the primitive key. The record's source offset recovers the local
// PrimitiveIndex without widening the payload.
bool PathTraceTryResolveCanonicalStaticBucketSourceTriangle(
    uint canonicalInstanceId,
    uint sourceTriangleIndex,
    out PathTraceStaticGeometryAddress address)
{
    address = (PathTraceStaticGeometryAddress)0;
    uint surfaceRecordIndex;
    if (!PathTraceStaticBucketInstanceInPublishedRange(
            canonicalInstanceId,
            StaticBucketRouteInfo,
            surfaceRecordIndex) ||
        surfaceRecordIndex >= StaticBucketRouteInfo.y ||
        surfaceRecordIndex >
            (0xffffffffu - PathTraceStaticTriangleCount()) /
                PATH_TRACE_STATIC_BUCKET_SURFACE_RECORD_WORDS)
    {
        return false;
    }

    const uint surfaceRecordWord =
        PathTraceStaticTriangleCount() +
        surfaceRecordIndex *
            PATH_TRACE_STATIC_BUCKET_SURFACE_RECORD_WORDS;
    const uint sourceTriangleOffset =
        SmokeStaticBucketTriangleClasses[
            surfaceRecordWord + 3u] >>
        PATH_TRACE_STATIC_BUCKET_SURFACE_RECORD_SOURCE_TRIANGLE_SHIFT;
    if (sourceTriangleIndex < sourceTriangleOffset)
    {
        return false;
    }

    return PathTraceTryResolvePrimaryStaticBucketHit(
            canonicalInstanceId,
            0u,
            sourceTriangleIndex - sourceTriangleOffset,
            address) &&
        address.surfaceRecordIndex == surfaceRecordIndex &&
        address.sourceTriangleIndex == sourceTriangleIndex;
}

bool PathTraceStaticBucketAlphaRejectsHit(
    PathTraceStaticGeometryAddress address,
    uint materialIndex,
    float2 hitBarycentrics)
{
    if (PathTraceSafetyDisabled(
            RT_PT_SAFETY_DISABLE_ANY_HIT_ALPHA))
    {
        return false;
    }

    const PathTraceSmokeMaterial material =
        LoadSmokeMaterial(materialIndex);
    if ((material.flags & RT_SMOKE_MATERIAL_ALPHA_TEST) == 0u)
    {
        return false;
    }

    const float2 texCoord =
        PathTraceStaticBucketInterpolateTexCoord(
            address,
            hitBarycentrics);
    return SmokeAlphaCoverage(material, texCoord) <
        material.alphaCutoff;
}

bool SmokeTriangleIndexRangeValid(uint instanceId, uint primitiveIndex)
{
    uint staticBucketRouteIndex = 0u;
    if (PathTraceStaticBucketInstanceInPublishedRange(
            instanceId,
            StaticBucketRouteInfo,
            staticBucketRouteIndex))
    {
        PathTraceStaticBucketRouteRecord route;
        uint packedTriangleIndex;
        uint3 packedVertexIndexes;
        return PathTraceTryLoadStaticBucketTriangleRoute(
            instanceId,
            primitiveIndex,
            StaticBucketRouteInfo,
            route,
            packedTriangleIndex,
            packedVertexIndexes);
    }
    if (instanceId == 0u)
    {
        return primitiveIndex < PathTraceStaticTriangleCount() &&
            primitiveIndex * 3u + 2u < PathTraceStaticIndexCount();
    }
    if (instanceId == 1u)
    {
        return primitiveIndex < PathTraceDynamicTriangleCount() &&
            primitiveIndex * 3u + 2u < PathTraceDynamicIndexCount();
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
        routeIndexOffset + 2u < PathTraceRigidRouteIndexCount();
}

uint LoadSmokeTriangleMaterialId(uint instanceId, uint primitiveIndex)
{
    uint staticBucketRouteIndex = 0u;
    if (PathTraceStaticBucketInstanceInPublishedRange(
            instanceId,
            StaticBucketRouteInfo,
            staticBucketRouteIndex))
    {
        PathTraceStaticBucketRouteRecord route;
        uint packedTriangleIndex;
        uint3 packedVertexIndexes;
        return PathTraceTryLoadStaticBucketTriangleRoute(
                instanceId,
                primitiveIndex,
                StaticBucketRouteInfo,
                route,
                packedTriangleIndex,
                packedVertexIndexes)
            ? SmokeStaticBucketTriangleMaterials[
                packedTriangleIndex]
            : 0xffffffffu;
    }
    if (instanceId == 0u && primitiveIndex < PathTraceStaticTriangleCount())
    {
        return SmokeStaticTriangleMaterials[primitiveIndex];
    }
    if (instanceId == 1u && primitiveIndex < PathTraceDynamicTriangleCount())
    {
        return SmokeDynamicTriangleMaterials[primitiveIndex];
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
        if (routeInstanceIndex < PathTraceRigidRouteInstanceCount())
        {
            const PathTraceRigidRouteInstance routeInstance = SmokeRigidRouteInstances[routeInstanceIndex];
            if (primitiveIndex < routeInstance.triangleCount &&
                routeInstance.triangleOffset + primitiveIndex < PathTraceRigidRouteTriangleCount())
            {
                return routeInstance.materialId;
            }
        }
    }
    return 0xffffffffu;
}

uint LoadSmokeTriangleClassAndFlags(uint instanceId, uint primitiveIndex)
{
    uint staticBucketRouteIndex = 0u;
    if (PathTraceStaticBucketInstanceInPublishedRange(
            instanceId,
            StaticBucketRouteInfo,
            staticBucketRouteIndex))
    {
        PathTraceStaticBucketRouteRecord route;
        uint packedTriangleIndex;
        uint3 packedVertexIndexes;
        return PathTraceTryLoadStaticBucketTriangleRoute(
                instanceId,
                primitiveIndex,
                StaticBucketRouteInfo,
                route,
                packedTriangleIndex,
                packedVertexIndexes)
            ? SmokeStaticBucketTriangleClasses[
                packedTriangleIndex]
            : 0u;
    }
    if (instanceId == 0u && primitiveIndex < PathTraceStaticTriangleCount())
    {
        return SmokeStaticTriangleClasses[primitiveIndex];
    }
    if (instanceId == 1u && primitiveIndex < PathTraceDynamicTriangleCount())
    {
        return SmokeDynamicTriangleClasses[primitiveIndex];
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
            ? routeTriangle.triangleClassAndFlags
            : 0u;
    }
    return PathTraceIsRigidHitRouteInstance(instanceId)
        ? RT_SMOKE_SURFACE_CLASS_RIGID_ENTITY
        : 0u;
}

uint LoadSmokeTriangleMaterialIndex(uint instanceId, uint primitiveIndex)
{
    uint staticBucketRouteIndex = 0u;
    if (PathTraceStaticBucketInstanceInPublishedRange(
            instanceId,
            StaticBucketRouteInfo,
            staticBucketRouteIndex))
    {
        PathTraceStaticBucketRouteRecord route;
        uint packedTriangleIndex;
        uint3 packedVertexIndexes;
        return PathTraceTryLoadStaticBucketTriangleRoute(
                instanceId,
                primitiveIndex,
                StaticBucketRouteInfo,
                route,
                packedTriangleIndex,
                packedVertexIndexes)
            ? SmokeStaticBucketTriangleMaterialIndexes[
                packedTriangleIndex]
            : 0xffffffffu;
    }
    if (instanceId == 0u && primitiveIndex < PathTraceStaticTriangleCount())
    {
        return SmokeStaticTriangleMaterialIndexes[primitiveIndex];
    }
    if (instanceId == 1u && primitiveIndex < PathTraceDynamicTriangleCount())
    {
        return SmokeDynamicTriangleMaterialIndexes[primitiveIndex];
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
            ? routeTriangle.materialIndex
            : 0xffffffffu;
    }
    if (PathTraceIsRigidHitRouteInstance(instanceId))
    {
        const uint routeInstanceIndex = instanceId - 2u;
        if (routeInstanceIndex < PathTraceRigidRouteInstanceCount())
        {
            const PathTraceRigidRouteInstance routeInstance = SmokeRigidRouteInstances[routeInstanceIndex];
            if (primitiveIndex < routeInstance.triangleCount &&
                routeInstance.triangleOffset + primitiveIndex < PathTraceRigidRouteTriangleCount())
            {
                return routeInstance.materialIndex;
            }
        }
    }
    return 0xffffffffu;
}

float2 InterpolateSmokeTexCoord(uint instanceId, uint primitiveIndex, float2 barycentrics)
{
    if (!SmokeTriangleIndexRangeValid(instanceId, primitiveIndex))
    {
        return float2(0.0, 0.0);
    }
    uint staticBucketRouteIndex = 0u;
    if (PathTraceStaticBucketInstanceInPublishedRange(
            instanceId,
            StaticBucketRouteInfo,
            staticBucketRouteIndex))
    {
        PathTraceStaticBucketRouteRecord route;
        uint packedTriangleIndex;
        uint3 packedVertexIndexes;
        if (!PathTraceTryLoadStaticBucketTriangleRoute(
                instanceId,
                primitiveIndex,
                StaticBucketRouteInfo,
                route,
                packedTriangleIndex,
                packedVertexIndexes))
        {
            return float2(0.0, 0.0);
        }
        const float3 weights = float3(
            1.0 - barycentrics.x - barycentrics.y,
            barycentrics.x,
            barycentrics.y);
        return
            SmokeStaticBucketVertices[
                packedVertexIndexes.x].texCoord.xy * weights.x +
            SmokeStaticBucketVertices[
                packedVertexIndexes.y].texCoord.xy * weights.y +
            SmokeStaticBucketVertices[
                packedVertexIndexes.z].texCoord.xy * weights.z;
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
    const uint indexOffset = primitiveIndex * 3u;
    const uint i0 = instanceId == 0u ? SmokeStaticIndices[indexOffset + 0u] : SmokeDynamicIndices[indexOffset + 0u];
    const uint i1 = instanceId == 0u ? SmokeStaticIndices[indexOffset + 1u] : SmokeDynamicIndices[indexOffset + 1u];
    const uint i2 = instanceId == 0u ? SmokeStaticIndices[indexOffset + 2u] : SmokeDynamicIndices[indexOffset + 2u];
    const uint vertexCount = instanceId == 0u ? PathTraceStaticVertexCount() : PathTraceDynamicVertexCount();
    if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
    {
        return float2(0.0, 0.0);
    }
    const float2 uv0 = (instanceId == 0u ? SmokeStaticVertices[i0].texCoord : SmokeDynamicVertices[i0].texCoord).xy;
    const float2 uv1 = (instanceId == 0u ? SmokeStaticVertices[i1].texCoord : SmokeDynamicVertices[i1].texCoord).xy;
    const float2 uv2 = (instanceId == 0u ? SmokeStaticVertices[i2].texCoord : SmokeDynamicVertices[i2].texCoord).xy;
    const float3 weights = float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
    return uv0 * weights.x + uv1 * weights.y + uv2 * weights.z;
}

float4 InterpolateSmokeVertexColor(uint instanceId, uint primitiveIndex, float2 barycentrics)
{
    if (!SmokeTriangleIndexRangeValid(instanceId, primitiveIndex))
    {
        return float4(1.0, 1.0, 1.0, 1.0);
    }
    uint staticBucketRouteIndex = 0u;
    if (PathTraceStaticBucketInstanceInPublishedRange(
            instanceId,
            StaticBucketRouteInfo,
            staticBucketRouteIndex))
    {
        PathTraceStaticBucketRouteRecord route;
        uint packedTriangleIndex;
        uint3 packedVertexIndexes;
        if (!PathTraceTryLoadStaticBucketTriangleRoute(
                instanceId,
                primitiveIndex,
                StaticBucketRouteInfo,
                route,
                packedTriangleIndex,
                packedVertexIndexes))
        {
            return float4(1.0, 1.0, 1.0, 1.0);
        }
        const float3 weights = float3(
            1.0 - barycentrics.x - barycentrics.y,
            barycentrics.x,
            barycentrics.y);
        return saturate(
            SmokeStaticBucketVertices[
                packedVertexIndexes.x].color * weights.x +
            SmokeStaticBucketVertices[
                packedVertexIndexes.y].color * weights.y +
            SmokeStaticBucketVertices[
                packedVertexIndexes.z].color * weights.z);
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
    const uint indexOffset = primitiveIndex * 3u;
    const uint i0 = instanceId == 0u ? SmokeStaticIndices[indexOffset + 0u] : SmokeDynamicIndices[indexOffset + 0u];
    const uint i1 = instanceId == 0u ? SmokeStaticIndices[indexOffset + 1u] : SmokeDynamicIndices[indexOffset + 1u];
    const uint i2 = instanceId == 0u ? SmokeStaticIndices[indexOffset + 2u] : SmokeDynamicIndices[indexOffset + 2u];
    const uint vertexCount = instanceId == 0u ? PathTraceStaticVertexCount() : PathTraceDynamicVertexCount();
    if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
    {
        return float4(1.0, 1.0, 1.0, 1.0);
    }
    const float4 c0 = instanceId == 0u ? SmokeStaticVertices[i0].color : SmokeDynamicVertices[i0].color;
    const float4 c1 = instanceId == 0u ? SmokeStaticVertices[i1].color : SmokeDynamicVertices[i1].color;
    const float4 c2 = instanceId == 0u ? SmokeStaticVertices[i2].color : SmokeDynamicVertices[i2].color;
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

float SmokeAdditiveDecalMaterialOpacity(PathTraceSmokeMaterial material, float3 albedo)
{
    if ((material.flags & RT_SMOKE_MATERIAL_ADDITIVE_DECAL_WHITE_KEY) != 0u)
    {
        return 1.0 - min(min(albedo.r, albedo.g), albedo.b);
    }
    return max(max(albedo.r, albedo.g), albedo.b);
}

bool SmokeAdditiveDecalRejectsHit(PathTraceSmokeMaterial material, float2 texCoord, float2 barycentrics, uint instanceId, uint primitiveIndex, uint triangleClassAndFlags, bool shadowRay)
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
    const float opacity = saturate(SmokeAdditiveDecalMaterialOpacity(material, SampleSmokeDiffuseTexture(material, texCoord).rgb) * 0.5);
    const uint2 ditherCell = uint2(floor(frac(abs(texCoord)) * 128.0));
    const uint2 baryCell = uint2(saturate(barycentrics) * 255.0);
    const uint hash =
        primitiveIndex * 2246822519u ^
        instanceId * 3266489917u ^
        ditherCell.x * 668265263u ^
        ditherCell.y * 747796405u ^
        baryCell.x * 2891336453u ^
        baryCell.y * 277803737u;
    return opacity < SmokeHashToUnitFloat(SmokeAlphaStochasticHash(hash, 7u));
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
    const float key = (blackKey ? max(max(albedo.r, albedo.g), albedo.b) : 1.0 - min(min(albedo.r, albedo.g), albedo.b)) * 0.5;
    const uint2 ditherCell = uint2(floor(frac(abs(texCoord)) * 128.0));
    const uint2 baryCell = uint2(saturate(barycentrics) * 255.0);
    const uint hash =
        primitiveIndex * 668265263u ^
        instanceId * 2246822519u ^
        ditherCell.x * 3266489917u ^
        ditherCell.y * 668265263u ^
        baryCell.x * 747796405u ^
        baryCell.y * 2891336453u;
    return saturate(key) < SmokeHashToUnitFloat(SmokeAlphaStochasticHash(hash, 11u));
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

bool PathTraceStaticBucketGlassRejectsPrimaryHit(
    PathTraceStaticGeometryAddress address,
    uint triangleClassAndFlags,
    uint materialIndex,
    float2 hitBarycentrics)
{
    const PathTraceSmokeMaterial material =
        LoadSmokeMaterial(materialIndex);
    const float2 texCoord =
        PathTraceStaticBucketInterpolateTexCoord(
            address,
            hitBarycentrics);
    // Preserve the monolithic static identity in stochastic glass coverage so
    // route 0 and route 2 select the same samples after bucket repacking.
    return SmokeGlassFallbackRejectsHit(
        material,
        texCoord,
        hitBarycentrics,
        0u,
        address.sourceTriangleIndex,
        triangleClassAndFlags,
        false);
}

bool SmokeAlphaRejectsHit(uint instanceId, uint primitiveIndex, float2 hitBarycentrics, uint rayMode)
{
    if (PathTraceSafetyDisabled(RT_PT_SAFETY_DISABLE_ANY_HIT_ALPHA) ||
        !SmokeTriangleIndexRangeValid(instanceId, primitiveIndex))
    {
        return false;
    }
    const uint materialIndex = LoadSmokeTriangleMaterialIndex(instanceId, primitiveIndex);
    const PathTraceSmokeMaterial material = LoadSmokeMaterial(materialIndex);
    const float2 texCoord = InterpolateSmokeTexCoord(instanceId, primitiveIndex, hitBarycentrics);
    const uint triangleClassAndFlags = LoadSmokeTriangleClassAndFlags(instanceId, primitiveIndex);
    const bool shadowRay = rayMode != 0u && rayMode != RT_SMOKE_RAY_MODE_PRIMARY_FILTER_DECAL_COMPOSITE;
    if (SmokeGuiRejectsTransparentHit(instanceId, primitiveIndex, hitBarycentrics, triangleClassAndFlags))
    {
        return true;
    }
    if (SmokeParticleDitherRejectsHit(material, texCoord, hitBarycentrics, instanceId, primitiveIndex, triangleClassAndFlags, shadowRay))
    {
        return true;
    }
    if (SmokeGlassFallbackRejectsHit(material, texCoord, hitBarycentrics, instanceId, primitiveIndex, triangleClassAndFlags, shadowRay))
    {
        return true;
    }
    if (SmokeAdditiveDecalRejectsHit(material, texCoord, hitBarycentrics, instanceId, primitiveIndex, triangleClassAndFlags, shadowRay))
    {
        return true;
    }
    if (SmokeFilterDecalRejectsHit(material, texCoord, hitBarycentrics, instanceId, primitiveIndex, shadowRay, rayMode))
    {
        return true;
    }
    if ((material.flags & RT_SMOKE_MATERIAL_ALPHA_TEST) == 0u)
    {
        return false;
    }
    return SmokeAlphaCoverage(material, texCoord) < material.alphaCutoff;
}

bool TryGetLiquidPoolStageColor(uint materialIndex, out float4 stageColor)
{
    stageColor = float4(1.0, 1.0, 1.0, 1.0);
    const uint dynamicRecordCount = (uint)max(DecalInfo2.x, 0.0);
    if (materialIndex >= dynamicRecordCount)
    {
        return true;
    }

    const PathTraceDynamicMaterialRecord dynamicRecord = SmokeDynamicMaterials[materialIndex];
    if ((dynamicRecord.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_VALID) == 0u ||
        dynamicRecord.materialIndex != materialIndex)
    {
        return true;
    }
    if ((dynamicRecord.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_STAGE_ENABLED) == 0u ||
        dynamicRecord.texMatrix0.w == 0.0)
    {
        return false;
    }

    stageColor = float4(max(dynamicRecord.color.rgb, 0.0), saturate(dynamicRecord.color.a));
    return stageColor.a > 0.0;
}

bool PathTraceLiquidPoolUsesInvertedFilterBlackKey(
    PathTraceMaterialFeatureParameterRecord parameters,
    PathTraceSmokeMaterial material)
{
    static const uint RT_PATH_TRACE_ORDERED_STAGE_OPERATION_SHIFT = 7u;
    static const uint RT_PATH_TRACE_ORDERED_STAGE_OPERATION_MASK = 0x7u;
    static const uint RT_PATH_TRACE_ORDERED_STAGE_OPERATION_INVERTED_FILTER_BLACK_KEY = 4u;

    [unroll]
    for (uint stageSlot = 0u; stageSlot < RT_PATH_TRACE_ORDERED_STAGE_CAPACITY; ++stageSlot)
    {
        const uint stageWord = PathTraceMaterialOrderedStageWord(parameters, stageSlot);
        const uint textureWord = PathTraceMaterialOrderedStageTextureWord(parameters, stageSlot);
        if (!PathTraceMaterialOrderedStageValid(stageWord) ||
            !PathTraceMaterialOrderedStageTextureValid(textureWord) ||
            PathTraceMaterialOrderedStageTextureIndex(textureWord) != material.diffuseTextureIndex)
        {
            continue;
        }

        const uint operation =
            (stageWord >> RT_PATH_TRACE_ORDERED_STAGE_OPERATION_SHIFT) &
            RT_PATH_TRACE_ORDERED_STAGE_OPERATION_MASK;
        if (operation == RT_PATH_TRACE_ORDERED_STAGE_OPERATION_INVERTED_FILTER_BLACK_KEY)
        {
            return true;
        }
    }
    return false;
}

void ConditionallyStoreLiquidPoolCandidate(
    inout PathTraceSmokePayload payload,
    uint instanceId,
    uint materialIndex,
    uint primitiveIndex,
    float2 barycentrics)
{
    const LiquidPoolContributorKey key = LiquidPoolMakeContributorKey(instanceId, primitiveIndex, materialIndex, barycentrics);
    payload.liquidStatusMask |= RT_LIQUID_POOL_STATUS_CANDIDATE;

    [unroll]
    for (uint existingSlot = 0u; existingSlot < RT_LIQUID_POOL_CANDIDATE_CAPACITY; ++existingSlot)
    {
        if (existingSlot < payload.liquidRetainedCount &&
            payload.liquidInstanceId[existingSlot] == key.instanceId &&
            payload.liquidPrimitiveIndex[existingSlot] == key.primitiveIndex &&
            payload.liquidMaterialIndex[existingSlot] == key.materialIndex &&
            payload.liquidBarycentricXBits[existingSlot] == key.barycentricXBits &&
            payload.liquidBarycentricYBits[existingSlot] == key.barycentricYBits)
        {
            return;
        }
    }

    // Count unique observed occurrences. A filter-receiver retrace can visit the
    // same card again; that transport duplicate must not inflate the raw tuple.
    payload.liquidRawCount = payload.liquidRawCount == 0xffffffffu ? 0xffffffffu : payload.liquidRawCount + 1u;

    if (payload.liquidRetainedCount >= RT_LIQUID_POOL_CANDIDATE_CAPACITY)
    {
        payload.liquidStatusMask |= RT_LIQUID_POOL_STATUS_OVERFLOW;
        return;
    }

    const uint slot = payload.liquidRetainedCount++;
    payload.liquidInstanceId[slot] = key.instanceId;
    payload.liquidMaterialIndex[slot] = key.materialIndex;
    payload.liquidPrimitiveIndex[slot] = key.primitiveIndex;
    payload.liquidBarycentricXBits[slot] = key.barycentricXBits;
    payload.liquidBarycentricYBits[slot] = key.barycentricYBits;
    payload.liquidHitT[slot] = RayTCurrent();
}

bool TryBuildLiquidPoolCardEvidence(
    uint instanceId,
    uint primitiveIndex,
    float2 barycentrics,
    out float3 cardPosition,
    out float3 cardPlaneNormal,
    out float2 cardTexCoord,
    out float3 cardTangent,
    out float3 cardBitangent)
{
    cardPosition = 0.0;
    cardPlaneNormal = 0.0;
    cardTexCoord = 0.0;
    cardTangent = 0.0;
    cardBitangent = 0.0;
    if (PathTraceIsStaticBucketRouteInstance(
            instanceId,
            StaticBucketRouteInfo))
    {
        PathTraceStaticGeometryAddress address;
        if (!PathTraceTryResolveCanonicalStaticBucketSourceTriangle(
                instanceId,
                primitiveIndex,
                address))
        {
            return false;
        }
        const PathTraceSmokeVertex v0 =
            SmokeStaticBucketVertices[address.vertexIndexes.x];
        const PathTraceSmokeVertex v1 =
            SmokeStaticBucketVertices[address.vertexIndexes.y];
        const PathTraceSmokeVertex v2 =
            SmokeStaticBucketVertices[address.vertexIndexes.z];
        const float3 p0 = v0.position.xyz;
        const float3 p1 = v1.position.xyz;
        const float3 p2 = v2.position.xyz;
        const float3 bary = float3(
            1.0 - barycentrics.x - barycentrics.y,
            barycentrics.x,
            barycentrics.y);
        cardPosition =
            p0 * bary.x + p1 * bary.y + p2 * bary.z;
        cardPlaneNormal = cross(p1 - p0, p2 - p0);
        cardTexCoord =
            v0.texCoord.xy * bary.x +
            v1.texCoord.xy * bary.y +
            v2.texCoord.xy * bary.z;
        const float2 uvEdge1 =
            v1.texCoord.xy - v0.texCoord.xy;
        const float2 uvEdge2 =
            v2.texCoord.xy - v0.texCoord.xy;
        const float uvDeterminant =
            uvEdge1.x * uvEdge2.y -
            uvEdge1.y * uvEdge2.x;
        if (abs(uvDeterminant) > 1.0e-10)
        {
            const float inverseDeterminant =
                rcp(uvDeterminant);
            cardTangent =
                ((p1 - p0) * uvEdge2.y -
                    (p2 - p0) * uvEdge1.y) *
                inverseDeterminant;
            cardBitangent =
                ((p2 - p0) * uvEdge1.x -
                    (p1 - p0) * uvEdge2.x) *
                inverseDeterminant;
        }
        return LiquidPoolFinite3(cardPosition) &&
            LiquidPoolFinite3(cardPlaneNormal) &&
            LiquidPoolFinite2(cardTexCoord);
    }

    if (!SmokeTriangleIndexRangeValid(instanceId, primitiveIndex))
    {
        return false;
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
            return false;
        }
        const PathTraceSmokeVertex v0 = SmokeSkinnedCurrentVertices[vertexIndex0];
        const PathTraceSmokeVertex v1 = SmokeSkinnedCurrentVertices[vertexIndex1];
        const PathTraceSmokeVertex v2 = SmokeSkinnedCurrentVertices[vertexIndex2];
        const float3 p0 = v0.position.xyz;
        const float3 p1 = v1.position.xyz;
        const float3 p2 = v2.position.xyz;
        const float3 bary = float3(
            1.0 - barycentrics.x - barycentrics.y,
            barycentrics.x,
            barycentrics.y);
        cardPosition = p0 * bary.x + p1 * bary.y + p2 * bary.z;
        cardPlaneNormal = cross(p1 - p0, p2 - p0);
        cardTexCoord =
            v0.texCoord.xy * bary.x +
            v1.texCoord.xy * bary.y +
            v2.texCoord.xy * bary.z;
        const float2 uvEdge1 = v1.texCoord.xy - v0.texCoord.xy;
        const float2 uvEdge2 = v2.texCoord.xy - v0.texCoord.xy;
        const float uvDeterminant =
            uvEdge1.x * uvEdge2.y - uvEdge1.y * uvEdge2.x;
        if (abs(uvDeterminant) > 1.0e-10)
        {
            const float inverseDeterminant = rcp(uvDeterminant);
            cardTangent =
                ((p1 - p0) * uvEdge2.y -
                    (p2 - p0) * uvEdge1.y) *
                inverseDeterminant;
            cardBitangent =
                ((p2 - p0) * uvEdge1.x -
                    (p1 - p0) * uvEdge2.x) *
                inverseDeterminant;
        }
        return LiquidPoolFinite3(cardPosition) &&
            LiquidPoolFinite3(cardPlaneNormal) &&
            LiquidPoolFinite2(cardTexCoord);
    }

    if (PathTraceIsRigidHitRouteInstance(instanceId))
    {
        const uint routeInstanceIndex = instanceId - 2u;
        if (routeInstanceIndex >= PathTraceRigidRouteInstanceCount())
        {
            return false;
        }
        const PathTraceRigidRouteInstance routeInstance = SmokeRigidRouteInstances[routeInstanceIndex];
        if (primitiveIndex >= routeInstance.triangleCount)
        {
            return false;
        }
        const uint routeIndexOffset = routeInstance.indexOffset + primitiveIndex * 3u;
        if (routeIndexOffset + 2u >= PathTraceRigidRouteIndexCount())
        {
            return false;
        }
        const uint i0 = SmokeRigidRouteIndices[routeIndexOffset + 0u];
        const uint i1 = SmokeRigidRouteIndices[routeIndexOffset + 1u];
        const uint i2 = SmokeRigidRouteIndices[routeIndexOffset + 2u];
        if (i0 >= routeInstance.vertexCount || i1 >= routeInstance.vertexCount || i2 >= routeInstance.vertexCount ||
            routeInstance.vertexOffset + i0 >= PathTraceRigidRouteVertexCount() ||
            routeInstance.vertexOffset + i1 >= PathTraceRigidRouteVertexCount() ||
            routeInstance.vertexOffset + i2 >= PathTraceRigidRouteVertexCount())
        {
            return false;
        }
        const PathTraceSmokeVertex v0 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i0];
        const PathTraceSmokeVertex v1 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i1];
        const PathTraceSmokeVertex v2 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i2];
        const float3 p0 = float3(
            dot(routeInstance.currentObjectToWorld0, float4(v0.position.xyz, 1.0)),
            dot(routeInstance.currentObjectToWorld1, float4(v0.position.xyz, 1.0)),
            dot(routeInstance.currentObjectToWorld2, float4(v0.position.xyz, 1.0)));
        const float3 p1 = float3(
            dot(routeInstance.currentObjectToWorld0, float4(v1.position.xyz, 1.0)),
            dot(routeInstance.currentObjectToWorld1, float4(v1.position.xyz, 1.0)),
            dot(routeInstance.currentObjectToWorld2, float4(v1.position.xyz, 1.0)));
        const float3 p2 = float3(
            dot(routeInstance.currentObjectToWorld0, float4(v2.position.xyz, 1.0)),
            dot(routeInstance.currentObjectToWorld1, float4(v2.position.xyz, 1.0)),
            dot(routeInstance.currentObjectToWorld2, float4(v2.position.xyz, 1.0)));
        const float3 bary = float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
        cardPosition = p0 * bary.x + p1 * bary.y + p2 * bary.z;
        cardPlaneNormal = cross(p1 - p0, p2 - p0);
        cardTexCoord = v0.texCoord.xy * bary.x + v1.texCoord.xy * bary.y + v2.texCoord.xy * bary.z;
        const float2 uvEdge1 = v1.texCoord.xy - v0.texCoord.xy;
        const float2 uvEdge2 = v2.texCoord.xy - v0.texCoord.xy;
        const float uvDeterminant = uvEdge1.x * uvEdge2.y - uvEdge1.y * uvEdge2.x;
        if (abs(uvDeterminant) > 1.0e-10)
        {
            const float inverseDeterminant = rcp(uvDeterminant);
            cardTangent = ((p1 - p0) * uvEdge2.y - (p2 - p0) * uvEdge1.y) * inverseDeterminant;
            cardBitangent = ((p2 - p0) * uvEdge1.x - (p1 - p0) * uvEdge2.x) * inverseDeterminant;
        }
        return LiquidPoolFinite3(cardPosition) && LiquidPoolFinite3(cardPlaneNormal) && LiquidPoolFinite2(cardTexCoord);
    }

    const uint indexOffset = primitiveIndex * 3u;
    const uint i0 = instanceId == 0u ? SmokeStaticIndices[indexOffset + 0u] : SmokeDynamicIndices[indexOffset + 0u];
    const uint i1 = instanceId == 0u ? SmokeStaticIndices[indexOffset + 1u] : SmokeDynamicIndices[indexOffset + 1u];
    const uint i2 = instanceId == 0u ? SmokeStaticIndices[indexOffset + 2u] : SmokeDynamicIndices[indexOffset + 2u];
    const uint vertexCount = instanceId == 0u ? PathTraceStaticVertexCount() : PathTraceDynamicVertexCount();
    if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
    {
        return false;
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
    const float3 bary = float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
    cardPosition = v0.position.xyz * bary.x + v1.position.xyz * bary.y + v2.position.xyz * bary.z;
    cardPlaneNormal = cross(v1.position.xyz - v0.position.xyz, v2.position.xyz - v0.position.xyz);
    cardTexCoord = v0.texCoord.xy * bary.x + v1.texCoord.xy * bary.y + v2.texCoord.xy * bary.z;
    const float2 uvEdge1 = v1.texCoord.xy - v0.texCoord.xy;
    const float2 uvEdge2 = v2.texCoord.xy - v0.texCoord.xy;
    const float uvDeterminant = uvEdge1.x * uvEdge2.y - uvEdge1.y * uvEdge2.x;
    if (abs(uvDeterminant) > 1.0e-10)
    {
        const float inverseDeterminant = rcp(uvDeterminant);
        cardTangent = ((v1.position.xyz - v0.position.xyz) * uvEdge2.y -
            (v2.position.xyz - v0.position.xyz) * uvEdge1.y) * inverseDeterminant;
        cardBitangent = ((v2.position.xyz - v0.position.xyz) * uvEdge1.x -
            (v1.position.xyz - v0.position.xyz) * uvEdge2.x) * inverseDeterminant;
    }
    return LiquidPoolFinite3(cardPosition) && LiquidPoolFinite3(cardPlaneNormal) && LiquidPoolFinite2(cardTexCoord);
}

uint LiquidPoolDiagnosticHash(LiquidPoolContributorKey key)
{
    uint hash = 2166136261u;
    hash = (hash ^ key.instanceId) * 16777619u;
    hash = (hash ^ key.primitiveIndex) * 16777619u;
    hash = (hash ^ key.materialIndex) * 16777619u;
    hash = (hash ^ key.barycentricXBits) * 16777619u;
    return (hash ^ key.barycentricYBits) * 16777619u;
}

struct LiquidPoolPrimaryResolve
{
    LiquidPoolResolvedFilm film;
    LiquidPoolEffectiveReceiverMaterial effective;
    uint validatedCount;
    uint rejectionCount;
    uint statusMask;
};

LiquidPoolPrimaryResolve ResolvePrimaryLiquidPool(
    inout RAB_Surface surface,
    PathTraceSmokePayload payload,
    float3 rayDirection)
{
    LiquidPoolPrimaryResolve result = (LiquidPoolPrimaryResolve)0;
    result.film = LiquidPoolResolvedFilmIdentity();
    result.statusMask = payload.liquidStatusMask;
    result.rejectionCount = payload.liquidRejectionCount;
    result.effective = LiquidPoolApplyResolvedFilm(
        surface.material.diffuseAlbedo,
        surface.material.specularF0,
        surface.material.roughness,
        result.film,
        0u);

    if (!PathTraceLiquidPoolCollectionEnabled() || !RAB_IsSurfaceValid(surface) || payload.liquidRetainedCount == 0u)
    {
        return result;
    }

    PathTraceMaterialFeatureRecord receiverFeature;
    const bool receiverFeatureValid = TryLoadPathTraceMaterialFeatureRecord(surface.materialIndex, receiverFeature);
    const uint receiverOpaque = receiverFeatureValid &&
        (receiverFeature.materialCaps & RT_PATH_TRACE_MATERIAL_CAP_OPAQUE_DIRECT) != 0u &&
        surface.surfaceClass != RT_SMOKE_SURFACE_CLASS_TRANSLUCENT;
    const uint receiverTransmission = !receiverFeatureValid ||
        (receiverFeature.materialCaps & RT_PATH_TRACE_MATERIAL_CAP_PATH_TRANSMISSION) != 0u;

    [loop]
    for (uint slot = 0u; slot < min(payload.liquidRetainedCount, RT_LIQUID_POOL_CANDIDATE_CAPACITY); ++slot)
    {
        const uint instanceId = payload.liquidInstanceId[slot];
        const uint materialIndex = payload.liquidMaterialIndex[slot];
        const uint primitiveIndex = payload.liquidPrimitiveIndex[slot];
        const float2 barycentrics = float2(
            asfloat(payload.liquidBarycentricXBits[slot]),
            asfloat(payload.liquidBarycentricYBits[slot]));
        float3 cardPosition;
        float3 cardPlaneNormal;
        float2 cardTexCoord;
        float3 cardTangent;
        float3 cardBitangent;
        const bool cardValid = TryBuildLiquidPoolCardEvidence(
            instanceId,
            primitiveIndex,
            barycentrics,
            cardPosition,
            cardPlaneNormal,
            cardTexCoord,
            cardTangent,
            cardBitangent);

        LiquidPoolReceiverEvidence evidence = (LiquidPoolReceiverEvidence)0;
        evidence.cardPosition = cardPosition;
        evidence.cardPlaneNormal = cardPlaneNormal;
        evidence.receiverPosition = surface.worldPos;
        evidence.receiverGeometryNormal = surface.geometryNormal;
        evidence.rayDirection = rayDirection;
        // idTech projected decals and their receivers can independently land
        // in static, dynamic, or routed-rigid geometry domains.  The trace
        // establishes card-before-receiver ordering; the shared receiver
        // predicate supplies the authoritative tight offset, normal, opacity,
        // and transmission proof.  Geometry-domain identity is not ownership.
        evidence.domainAccepted = cardValid;
        evidence.identityAccepted = cardValid;
        evidence.receiverOpaque = receiverOpaque;
        evidence.receiverPathTransmission = receiverTransmission;
        if (!LiquidPoolAcceptsReceiver(evidence))
        {
            result.rejectionCount += 1u;
            result.statusMask |= RT_LIQUID_POOL_STATUS_RECEIVER_REJECTED;
            continue;
        }

        PathTraceMaterialFeatureParameterRecord parameters;
        float4 stageColor;
        if (!TryLoadPathTraceLiquidPoolMaterialFeatureParameters(materialIndex, parameters) ||
            !TryGetLiquidPoolStageColor(materialIndex, stageColor))
        {
            result.rejectionCount += 1u;
            result.statusMask |= RT_LIQUID_POOL_STATUS_RECEIVER_REJECTED;
            continue;
        }

        const PathTraceSmokeMaterial material = LoadSmokeMaterial(materialIndex);
        LiquidPoolReducerCandidate candidate = (LiquidPoolReducerCandidate)0;
        candidate.coverage = saturate(SmokeAlphaCoverage(material, cardTexCoord)) * saturate(stageColor.a);
        candidate.height = 1.0;
        const float3 authoredSourceRgb = saturate(
            max(SampleSmokeDecodedDiffuseTexture(material, cardTexCoord).rgb, 0.0) *
            max(stageColor.rgb, 0.0));
        const bool invertedFilterBlackKey = PathTraceLiquidPoolUsesInvertedFilterBlackKey(parameters, material);
        // GL_ZERO, GL_ONE_MINUS_SRC_COLOR splats store the complement of the
        // visible blood color (usually cyan source art for red attenuation).
        // The film reducer consumes substrate transmittance, so reproduce the
        // authored blend factor rather than treating source RGB as the tint.
        candidate.decalRgb = invertedFilterBlackKey ? 1.0 - authoredSourceRgb : authoredSourceRgb;
        candidate.referenceTransmittance = parameters.params0.xyz;
        candidate.opticalDepthScale = parameters.params0.w;
        candidate.coatRoughness = parameters.params1.x;
        candidate.dielectricIor = parameters.params1.y;
        candidate.authoredNormalStrength = parameters.params1.z;
        candidate.key = LiquidPoolMakeContributorKey(instanceId, primitiveIndex, materialIndex, barycentrics);
        candidate.diagnosticHash = LiquidPoolDiagnosticHash(candidate.key);
        candidate.valid = candidate.coverage > 0.0 ? 1u : 0u;
        if (candidate.valid == 0u)
        {
            continue;
        }
        result.film = LiquidPoolReduceCandidate(result.film, candidate);
        result.validatedCount += 1u;
    }

    if (result.validatedCount > 0u)
    {
        result.statusMask |= RT_LIQUID_POOL_STATUS_RECEIVER_VALID;
    }
    result.film.overflowed = (result.statusMask & RT_LIQUID_POOL_STATUS_OVERFLOW) != 0u ? 1u : 0u;
    if (result.film.overflowed == 0u)
    {
        result.effective = LiquidPoolApplyResolvedFilm(
            surface.material.diffuseAlbedo,
            surface.material.specularF0,
            surface.material.roughness,
            result.film,
            0u);
        if (PathTraceLiquidPoolMode() >= 2u && result.effective.applied != 0u)
        {
            surface.material.diffuseAlbedo = result.effective.albedo;
            surface.material.specularF0 = result.effective.specularF0;
            surface.material.roughness = result.effective.roughness;
            surface.flags |= RT_PATH_TRACE_SURFACE_FLAG_LIQUID_FILM_APPLIED;
            result.statusMask |= RT_LIQUID_POOL_STATUS_APPLIED;

            if (PathTraceLiquidPoolMode() == 3u &&
                (surface.flags & RT_PATH_TRACE_SURFACE_FLAG_LIQUID_FILM_NORMAL_APPLIED) == 0u)
            {
                const float2 winnerBarycentrics = float2(
                    asfloat(result.film.winnerKey.barycentricXBits),
                    asfloat(result.film.winnerKey.barycentricYBits));
                float3 winnerPosition;
                float3 winnerPlaneNormal;
                float2 winnerTexCoord;
                float3 winnerTangent;
                float3 winnerBitangent;
                if (TryBuildLiquidPoolCardEvidence(
                    result.film.winnerKey.instanceId,
                    result.film.winnerKey.primitiveIndex,
                    winnerBarycentrics,
                    winnerPosition,
                    winnerPlaneNormal,
                    winnerTexCoord,
                    winnerTangent,
                    winnerBitangent))
                {
                    const PathTraceSmokeMaterial winnerMaterial =
                        LoadSmokeMaterial(result.film.winnerKey.materialIndex);
                    float4 winnerStageColor;
                    if (TryGetLiquidPoolStageColor(
                        result.film.winnerKey.materialIndex,
                        winnerStageColor))
                    {
                        const float2 texelSize = LiquidPoolCoverageTexelSize(winnerMaterial);
                        const float stageAlpha = saturate(winnerStageColor.a);
                        const float coverageLeft = saturate(SmokeAlphaCoverage(
                            winnerMaterial, winnerTexCoord - float2(texelSize.x, 0.0))) * stageAlpha;
                        const float coverageRight = saturate(SmokeAlphaCoverage(
                            winnerMaterial, winnerTexCoord + float2(texelSize.x, 0.0))) * stageAlpha;
                        const float coverageDown = saturate(SmokeAlphaCoverage(
                            winnerMaterial, winnerTexCoord - float2(0.0, texelSize.y))) * stageAlpha;
                        const float coverageUp = saturate(SmokeAlphaCoverage(
                            winnerMaterial, winnerTexCoord + float2(0.0, texelSize.y))) * stageAlpha;
                        const float2 coverageGradient = 0.5 * float2(
                            coverageRight - coverageLeft,
                            coverageUp - coverageDown);
                        const LiquidPoolEffectiveFilmNormal filmNormal =
                            LiquidPoolApplyFilmOwnedNormal(
                                surface.shadingNormal,
                                surface.geometryNormal,
                                winnerTangent,
                                winnerBitangent,
                                coverageGradient,
                                result.film.coverage,
                                result.film.authoredNormalStrength,
                                0u);
                        if (filmNormal.applied != 0u)
                        {
                            surface.shadingNormal = filmNormal.shadingNormal;
                            surface.flags |= RT_PATH_TRACE_SURFACE_FLAG_LIQUID_FILM_NORMAL_APPLIED;
                        }
                    }
                }
            }
        }
    }
    return result;
}

void LiquidPoolSaturatingIncrement(uint counterIndex)
{
    uint observed;
    InterlockedCompareExchange(PathTraceLiquidPoolStatusCounters[counterIndex], 0xffffffffu, 0xffffffffu, observed);
    while (observed != 0xffffffffu)
    {
        uint previous;
        InterlockedCompareExchange(PathTraceLiquidPoolStatusCounters[counterIndex], observed, observed + 1u, previous);
        if (previous == observed)
        {
            return;
        }
        observed = previous;
    }
}

void PublishLiquidPoolExceptionalStatus(uint statusMask)
{
    const uint exceptionalMask = statusMask &
        (RT_LIQUID_POOL_STATUS_OVERFLOW |
            RT_LIQUID_POOL_STATUS_DUPLICATE_APPLY |
            RT_LIQUID_POOL_STATUS_FAIL_CLOSED |
            RT_LIQUID_POOL_STATUS_INVALID_ROUTE);
    if (exceptionalMask == 0u || LiquidPoolInfo.w < 0.5)
    {
        return;
    }
    uint ignored;
    InterlockedOr(PathTraceLiquidPoolStatusCounters[RT_LIQUID_POOL_SOURCE_PRIMARY], exceptionalMask, ignored);
    if ((exceptionalMask & RT_LIQUID_POOL_STATUS_OVERFLOW) != 0u)
    {
        LiquidPoolSaturatingIncrement(8u + RT_LIQUID_POOL_SOURCE_PRIMARY);
    }
}

float3 LiquidPoolStatusColor(uint statusMask)
{
    if ((statusMask & (RT_LIQUID_POOL_STATUS_OVERFLOW | RT_LIQUID_POOL_STATUS_INVALID_ROUTE)) != 0u) return float3(1.0, 0.0, 1.0);
    if ((statusMask & RT_LIQUID_POOL_STATUS_APPLIED) != 0u) return float3(0.0, 1.0, 0.0);
    if ((statusMask & RT_LIQUID_POOL_STATUS_RECEIVER_VALID) != 0u) return float3(0.0, 1.0, 1.0);
    if ((statusMask & RT_LIQUID_POOL_STATUS_RECEIVER_REJECTED) != 0u) return float3(1.0, 0.2, 0.0);
    if ((statusMask & RT_LIQUID_POOL_STATUS_CANDIDATE) != 0u) return float3(1.0, 1.0, 0.0);
    return float3(0.0, 0.0, 0.0);
}

void WritePrimaryLiquidPoolDebug(
    uint2 outputPixel,
    inout RAB_Surface surface,
    PathTraceSmokePayload payload,
    inout LiquidPoolPrimaryResolve resolved)
{
    const uint debug = PathTraceLiquidPoolDebug();
    const uint page = PathTraceLiquidPoolDebugPage();
    if (PathTraceLiquidPoolMode() == 0u)
    {
        return;
    }
    if (PathTraceLiquidPoolMode() == 1u || debug != 0u)
    {
        surface.material.diffuseAlbedo = LiquidPoolStatusColor(resolved.statusMask);
        surface.material.emissiveRadiance = max(surface.material.emissiveRadiance, surface.material.diffuseAlbedo);
    }
    if (debug == 0u)
    {
        return;
    }

    float4 tuple = 0.0;
    bool validPage = false;
    if (debug == 1u && page == 0u)
    {
        tuple = float4(LiquidPoolStatusColor(resolved.statusMask), (float)resolved.statusMask);
        validPage = true;
    }
    else if (debug == 2u && page == 0u)
    {
        tuple = float4((float)payload.liquidRawCount, (float)payload.liquidRetainedCount,
            (float)RT_LIQUID_POOL_CANDIDATE_CAPACITY, (float)resolved.statusMask);
        validPage = true;
    }
    else if (debug == 3u && page == 0u)
    {
        tuple = float4((float)resolved.validatedCount, resolved.film.coverage, resolved.film.height, (float)resolved.statusMask);
        validPage = true;
    }
    else if (debug == 4u && page == 0u)
    {
        tuple = float4(asfloat(resolved.film.winnerKey.instanceId), asfloat(resolved.film.winnerKey.primitiveIndex),
            asfloat(resolved.film.winnerKey.materialIndex), asfloat(resolved.statusMask));
        validPage = true;
    }
    else if (debug == 4u && page == 1u)
    {
        tuple = float4(asfloat(resolved.film.winnerKey.barycentricXBits), asfloat(resolved.film.winnerKey.barycentricYBits),
            asfloat(resolved.film.diagnosticHash), asfloat(resolved.statusMask));
        validPage = true;
    }
    else if (debug == 4u && page == 2u)
    {
        tuple = float4((float)resolved.rejectionCount, resolved.film.coverage,
            (float)RT_LIQUID_POOL_SOURCE_PRIMARY, (float)resolved.statusMask);
        validPage = true;
    }
    else if (debug == 5u && page == 0u)
    {
        tuple = float4(resolved.effective.albedo, (float)resolved.effective.applied);
        validPage = true;
    }
    else if (debug == 5u && page == 1u)
    {
        tuple = float4(resolved.effective.specularF0, resolved.effective.roughness);
        validPage = true;
    }
    else if (debug == 5u && page == 2u)
    {
        tuple = float4(resolved.effective.transmittance, resolved.film.height);
        validPage = true;
    }
    else if (debug == 5u && page == 3u)
    {
        const float coatF0 = LiquidPoolDielectricF0(resolved.film.dielectricIor);
        const float opticalDepth = clamp(resolved.film.opticalDepthScale * resolved.film.height, 0.0, LIQUID_POOL_D_MAX);
        tuple = float4(resolved.film.coverage, coatF0, resolved.film.coatRoughness, opticalDepth);
        validPage = true;
    }
    else if (debug == 6u && page == 0u)
    {
        tuple = float4((float)RT_LIQUID_POOL_SOURCE_PRIMARY, (float)resolved.statusMask,
            (resolved.statusMask & RT_LIQUID_POOL_STATUS_APPLIED) != 0u ? 1.0 : 0.0, 0.0);
        validPage = true;
    }
    else if (debug == 6u && page == 1u)
    {
        tuple = 0.0;
        validPage = true;
    }
    else if (debug == 6u && page == 2u)
    {
        float3 authoredSourceRgb = 0.0;
        float semanticFlags = 0.0;
        if (resolved.film.valid != 0u && resolved.film.winnerKey.instanceId <= 1u)
        {
            const float2 barycentrics = float2(
                asfloat(resolved.film.winnerKey.barycentricXBits),
                asfloat(resolved.film.winnerKey.barycentricYBits));
            float3 cardPosition;
            float3 cardPlaneNormal;
            float2 cardTexCoord;
            float3 cardTangent;
            float3 cardBitangent;
            float4 stageColor;
            if (TryBuildLiquidPoolCardEvidence(
                resolved.film.winnerKey.instanceId,
                resolved.film.winnerKey.primitiveIndex,
                    barycentrics,
                    cardPosition,
                    cardPlaneNormal,
                    cardTexCoord,
                    cardTangent,
                    cardBitangent) &&
                TryGetLiquidPoolStageColor(resolved.film.winnerKey.materialIndex, stageColor))
            {
                const PathTraceSmokeMaterial material = LoadSmokeMaterial(resolved.film.winnerKey.materialIndex);
                authoredSourceRgb = saturate(
                    max(SampleSmokeDecodedDiffuseTexture(material, cardTexCoord).rgb, 0.0) *
                    max(stageColor.rgb, 0.0));
                PathTraceMaterialFeatureParameterRecord parameters;
                if (TryLoadPathTraceLiquidPoolMaterialFeatureParameters(
                        resolved.film.winnerKey.materialIndex,
                        parameters))
                {
                    semanticFlags = PathTraceLiquidPoolUsesInvertedFilterBlackKey(parameters, material) ? 4.0 : 0.0;
                }
            }
        }
        // xyz = the live GPU-decoded authored source; w = the authoritative
        // ordered-stage compositing op (4 = inverted-filter-black-key).
        tuple = float4(authoredSourceRgb, semanticFlags);
        validPage = true;
    }
    else if (debug == 6u && page == 3u)
    {
        // xyz = the color actually retained by the film reducer; w = coverage.
        tuple = float4(resolved.film.decalRgb, resolved.film.coverage);
        validPage = true;
    }

    if (!validPage)
    {
        resolved.statusMask |= RT_LIQUID_POOL_STATUS_INVALID_ROUTE;
        tuple = float4(7.0, (float)resolved.statusMask, 0.0, 0.0);
        surface.material.diffuseAlbedo = float3(1.0, 0.0, 1.0);
        surface.material.emissiveRadiance = max(surface.material.emissiveRadiance, surface.material.diffuseAlbedo);
    }
    SmokeOutput[outputPixel] = tuple;
}

// DECAL-05 (docs/decal_cards/03): deterministic blend-through composite. The bin
// is sorted ascending by draw-order sort key and each surviving layer composites
// onto the base material -- over / modulate / additive -- using the decal card's
// own authored UVs and textures. Pure arithmetic; no recursion.
void ApplyDetailDecalComposite(inout RAB_Surface surface, PathTraceSmokePayload payload, float3 rayDirection)
{
    const uint stage = PathTraceDecalCompositeStage();
    if ((stage != 1u && stage != 4u) || payload.decalCount == 0u || !RAB_IsSurfaceValid(surface))
    {
        return;
    }

    const uint count = min(payload.decalCount, RT_SMOKE_DECAL_BIN_SIZE);
    uint order[RT_SMOKE_DECAL_BIN_SIZE];
    [unroll]
    for (uint initSlot = 0u; initSlot < RT_SMOKE_DECAL_BIN_SIZE; ++initSlot)
    {
        order[initSlot] = initSlot;
    }
    for (uint sortOuter = 0u; sortOuter + 1u < count; ++sortOuter)
    {
        for (uint sortInner = sortOuter + 1u; sortInner < count; ++sortInner)
        {
            if (payload.decalSortKey[order[sortInner]] < payload.decalSortKey[order[sortOuter]])
            {
                const uint swap = order[sortOuter];
                order[sortOuter] = order[sortInner];
                order[sortInner] = swap;
            }
        }
    }

    // Receiver-validation guard (docs/decal_cards/04 sec.5): keep only decals whose
    // hit lies within the offset envelope of the committed base hit, measured along
    // the receiver normal -- never raw ray-T, which cuts circular halos at grazing
    // angles. The lifted decal sits in front of its receiver by at most
    // maxOffsetIndex * step; anything further belongs to other geometry (thin
    // doors, unrelated nearby cards).
    const float offsetEnvelope = DecalInfo.y * DecalInfo.z * 1.5 + 0.05;
    const float normalCosine = abs(dot(rayDirection, surface.geometryNormal));

    uint appliedCount = 0u;
    float liquidPoolCoverage = 0.0;
    float3 liquidPoolTint = float3(0.0, 0.0, 0.0);
    for (uint applySlot = 0u; applySlot < count; ++applySlot)
    {
        const uint entry = order[applySlot];
        const float normalSeparation = abs(payload.hitT - payload.decalHitT[entry]) * normalCosine;
        if (normalSeparation > offsetEnvelope)
        {
            continue;
        }

        const uint decalMaterialIndex = payload.decalMaterialIndex[entry];
        const PathTraceSmokeMaterial decalMaterial = LoadSmokeMaterial(decalMaterialIndex);
        if ((decalMaterial.flags & RT_SMOKE_MATERIAL_DETAIL_DECAL) == 0u)
        {
            continue;
        }
        if (PathTraceLiquidPoolCollectionEnabled() && PathTraceMaterialIsSemanticLiquidPool(decalMaterialIndex))
        {
            // The dedicated union owns semantic liquid cards in modes 1-3.
            continue;
        }

        // Per-frame evaluated stage state (docs/decal_cards/07 DYN-2): runtime
        // register materials (`colored` tints, animated pulses, condition
        // toggles) carry their evaluated color/condition in the dynamic
        // material record for this (variant) material index. A condition-off
        // stage DROPS the layer - it must not composite at zero.
        float4 decalStageColor = float4(1.0, 1.0, 1.0, 1.0);
        bool decalLayerSelfEmissive = false;
        const uint dynamicRecordCount = (uint)max(DecalInfo2.x, 0.0);
        if (decalMaterialIndex < dynamicRecordCount)
        {
            const PathTraceDynamicMaterialRecord dynamicRecord = SmokeDynamicMaterials[decalMaterialIndex];
            if ((dynamicRecord.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_VALID) != 0u &&
                dynamicRecord.materialIndex == decalMaterialIndex)
            {
                if ((dynamicRecord.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_STAGE_ENABLED) == 0u ||
                    dynamicRecord.texMatrix0.w == 0.0)
                {
                    continue;
                }
                decalStageColor = float4(max(dynamicRecord.color.rgb, float3(0.0, 0.0, 0.0)), saturate(dynamicRecord.color.a));
                decalLayerSelfEmissive = (dynamicRecord.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_SELECTED_EMISSIVE) != 0u;
            }
        }

        const uint packedTexCoord = payload.decalPackedTexCoord[entry];
        const float2 decalTexCoord = float2(f16tof32(packedTexCoord & 0xffffu), f16tof32(packedTexCoord >> 16));
        const float3 decalRgb = saturate(SampleSmokeDiffuseTexture(decalMaterial, decalTexCoord).rgb) * decalStageColor.rgb;

        float coverage;
        if ((decalMaterial.flags & RT_SMOKE_MATERIAL_DETAIL_DECAL_LIQUID_POOL) != 0u)
        {
            coverage = saturate(SmokeAlphaCoverage(decalMaterial, decalTexCoord)) * decalStageColor.a;
            liquidPoolCoverage = max(liquidPoolCoverage, coverage);
            liquidPoolTint = max(liquidPoolTint, decalRgb * coverage);
        }
        else if ((decalMaterial.flags & RT_SMOKE_MATERIAL_DETAIL_DECAL_DIFFUSE_LIT) != 0u)
        {
            // LIT DIFFUSE interaction layer (`blend diffuseMap`, e.g.
            // textures/decals/alphabet4, textures/hell/pentastic1_spectrum):
            // raster adds light * tex.rgb * stageColor on top of the lit wall.
            // In material terms that is an additive ALBEDO layer - black texels
            // contribute nothing, the stage color carries the authored tint or
            // animation, and it never darkens the receiver.
            const float3 litLayer = decalRgb * decalStageColor.a;
            if (decalLayerSelfEmissive)
            {
                // Spectrum "invisible writing" reveal: the synthesized record
                // carries the matching light's color, so the layer glows with
                // that light (and disappears with it) instead of riding DI.
                surface.material.emissiveRadiance += litLayer;
            }
            else
            {
                surface.material.diffuseAlbedo = saturate(surface.material.diffuseAlbedo + litLayer);
            }
            coverage = saturate(max(max(litLayer.r, litLayer.g), litLayer.b));
        }
        else if ((decalMaterial.flags & RT_SMOKE_MATERIAL_FILTER_DECAL) != 0u)
        {
            // MODULATE: out = lerp(base, decal*base, coverage) -- the product the
            // stochastic path could never express (docs/decal_cards/03 sec.1).
            // The multiply factor is floored: an exact-zero albedo product reads
            // as a hole and breaks the DI/RR consumers, which treat near-zero
            // guide albedo as an invalid surface (docs/decal_cards/08
            // ZERO-ALBEDO VOID).
            const bool blackKey = (decalMaterial.flags & RT_SMOKE_MATERIAL_FILTER_DECAL_BLACK_KEY) != 0u;
            coverage = saturate(blackKey
                ? max(max(decalRgb.r, decalRgb.g), decalRgb.b)
                : 1.0 - min(min(decalRgb.r, decalRgb.g), decalRgb.b)) * decalStageColor.a;
            const float modulateFloor = saturate(DecalInfo.w);
            surface.material.diffuseAlbedo = lerp(
                surface.material.diffuseAlbedo,
                surface.material.diffuseAlbedo * max(decalRgb, float3(modulateFloor, modulateFloor, modulateFloor)),
                coverage);
        }
        else if ((decalMaterial.flags & RT_SMOKE_MATERIAL_ADDITIVE_DECAL) != 0u)
        {
            // ADDITIVE: contributes radiance, mirroring the unlit-color fallback the
            // stochastic path uses for additive cards (glows, light leaks).
            coverage = saturate(SmokeAdditiveDecalMaterialOpacity(decalMaterial, decalRgb)) * decalStageColor.a;
            surface.material.emissiveRadiance += decalRgb * coverage;
        }
        else
        {
            // OVER: src-alpha blend -- signage, posters, alpha-cut grime.
            coverage = saturate(SmokeAlphaCoverage(decalMaterial, decalTexCoord)) * decalStageColor.a;
            surface.material.diffuseAlbedo = lerp(surface.material.diffuseAlbedo, decalRgb, coverage);
        }

        if ((decalMaterial.flags & RT_SMOKE_MATERIAL_EMISSIVE) != 0u)
        {
            const float3 decalEmissive = SampleSmokeEmissive(decalMaterial, decalTexCoord, surface.surfaceClass, true) * max(ToyPathInfo.z, 0.0);
            surface.material.emissiveRadiance += decalEmissive * coverage;
        }
        ++appliedCount;
    }

    if (liquidPoolCoverage > 0.0)
    {
        const float3 poolTint = saturate(liquidPoolTint / max(liquidPoolCoverage, 1.0e-4));
        surface.material.diffuseAlbedo = lerp(surface.material.diffuseAlbedo, surface.material.diffuseAlbedo * poolTint, liquidPoolCoverage);
        surface.material.roughness = lerp(surface.material.roughness, min(surface.material.roughness, 0.18), liquidPoolCoverage);
        surface.material.specularF0 = lerp(surface.material.specularF0, max(surface.material.specularF0, float3(0.04, 0.04, 0.04)), liquidPoolCoverage);
    }

    if (appliedCount > 0u)
    {
        // Keep the composited surface above the DI/RR validity threshold: the
        // clean pipeline rejects guide albedo with near-zero luminance, which
        // turns authored-black decal texels into apparent holes.
        surface.material.diffuseAlbedo = max(surface.material.diffuseAlbedo, float3(0.01, 0.01, 0.01));
    }

    if (stage == 4u && appliedCount > 0u)
    {
        // Composite diagnostic: false-color by applied-layer count (1=red 2=green 3=blue).
        const float3 countTints[RT_SMOKE_DECAL_BIN_SIZE] = {
            float3(1.0, 0.1, 0.1),
            float3(0.1, 1.0, 0.1),
            float3(0.1, 0.1, 1.0)
        };
        surface.material.diffuseAlbedo = lerp(
            surface.material.diffuseAlbedo,
            countTints[min(appliedCount, RT_SMOKE_DECAL_BIN_SIZE) - 1u],
            0.65);
    }
}

[shader("raygeneration")]
void RayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dimensions = DispatchRaysDimensions().xy;
    const uint2 outputPixel = pixel + PathTraceDispatchTileOffset();
    const uint2 fullDimensions = PathTraceFullOutputSize();
    if (outputPixel.x >= fullDimensions.x || outputPixel.y >= fullDimensions.y)
    {
        return;
    }

    const float2 rrJitterPixels = RayReconstructionInfo.z >= 0.5 ? RayReconstructionInfo.xy : float2(0.0, 0.0);
    // Match Remix/Streamline projection jitter semantics: the reported positive
    // jitter shifts the projection, so inverse primary-ray generation samples
    // pixel - jitter.
    const float2 uv = (float2(outputPixel) + 0.5 - rrJitterPixels) / float2(max(fullDimensions, uint2(1u, 1u)));
    const float2 ndc = uv * 2.0 - 1.0;
    RayDesc ray;
    ray.Origin = CameraOriginAndTMax.xyz;
    ray.Direction = normalize(
        CameraForwardAndTanX.xyz +
        CameraLeftAndTanY.xyz * (-ndc.x * CameraForwardAndTanX.w) +
        CameraUpAndDebugMode.xyz * (-ndc.y * CameraLeftAndTanY.w));
    ray.TMin = 0.1;
    ray.TMax = CameraOriginAndTMax.w;

    PathTraceSmokePayload payload = InitSmokePayload();
    // Detail-decal composite stages collect decals in any-hit, so the primary ray
    // runs in plain mode (0) and the legacy filter-decal pass-through experiment is
    // bypassed (docs/decal_cards/08 sec.4 -- do not build on the receiver re-trace).
    const bool decalCollectMode = PathTraceDecalCollectEnabled(PathTraceDecalCompositeStage());
    payload.value = decalCollectMode ? 0u : RT_SMOKE_RAY_MODE_PRIMARY_FILTER_DECAL_COMPOSITE;
    TraceRay(SmokeScene, RAY_FLAG_NONE, 0xff, 0, 0, 0, ray, payload);
    RAB_Surface surface = RAB_EmptySurface();
    if (payload.value != 0u && !SmokePayloadIsGuiScreen(payload))
    {
        PathTraceSmokePayload filterDecalPayload;
        const bool filterDecalComposite = !decalCollectMode && ResolvePrimaryFilterDecalReceiver(payload, ray, filterDecalPayload);
        surface = BuildSurfaceFromPayload(payload, ray.Origin, ray.Direction);
        if (filterDecalComposite)
        {
            ApplyPrimaryFilterDecalToSurface(surface, filterDecalPayload);
        }
        if (decalCollectMode)
        {
            ApplyDetailDecalComposite(surface, payload, ray.Direction);
        }
    }
    LiquidPoolPrimaryResolve liquidResolve = ResolvePrimaryLiquidPool(surface, payload, ray.Direction);
    WritePrimaryLiquidPoolDebug(outputPixel, surface, payload, liquidResolve);
    PublishLiquidPoolExceptionalStatus(liquidResolve.statusMask);
    StorePrimarySurfaceRecord(pixel, surface);
    StoreRayReconstructionGuides(pixel, surface);
    StoreRayReconstructionMotionGuides(pixel, surface);
}

[shader("miss")]
void Miss(inout PathTraceSmokePayload payload)
{
    payload.value = 0u;
}

[shader("miss")]
void ShadowMiss(inout PathTraceSmokeShadowPayload payload)
{
    payload.hit = 0u;
}

// Remix-style conditionallyStoreDecal: keep the RT_SMOKE_DECAL_BIN_SIZE entries
// with the highest sort keys (the topmost-drawn layers). The decal never commits;
// receiver validation happens at composite time where the base hitT is known.
void ConditionallyStoreDetailDecalResolved(
    inout PathTraceSmokePayload payload,
    uint materialIndex,
    uint sortKey,
    float2 texCoord)
{
    [unroll]
    for (uint existingSlot = 0u; existingSlot < RT_SMOKE_DECAL_BIN_SIZE; ++existingSlot)
    {
        if (existingSlot < payload.decalCount && payload.decalSortKey[existingSlot] == sortKey)
        {
            return;
        }
    }

    uint slot;
    if (payload.decalCount < RT_SMOKE_DECAL_BIN_SIZE)
    {
        slot = payload.decalCount;
        payload.decalCount += 1u;
    }
    else
    {
        uint lowestSlot = 0u;
        uint lowestKey = payload.decalSortKey[0];
        [unroll]
        for (uint binSlot = 1u; binSlot < RT_SMOKE_DECAL_BIN_SIZE; ++binSlot)
        {
            if (payload.decalSortKey[binSlot] < lowestKey)
            {
                lowestKey = payload.decalSortKey[binSlot];
                lowestSlot = binSlot;
            }
        }
        if (sortKey <= lowestKey)
        {
            return;
        }
        slot = lowestSlot;
    }

    payload.decalMaterialIndex[slot] = materialIndex;
    payload.decalPackedTexCoord[slot] = (f32tof16(texCoord.x) & 0xffffu) | (f32tof16(texCoord.y) << 16);
    payload.decalSortKey[slot] = sortKey;
    payload.decalHitT[slot] = RayTCurrent();
}

void ConditionallyStoreDetailDecal(
    inout PathTraceSmokePayload payload,
    uint materialIndex,
    uint instanceId,
    uint primitiveIndex,
    float2 barycentrics)
{
    // Draw-order proxy sort key (docs/decal_cards/02 M2, conservative v1): static
    // capture appends in submission order, so the BLAS primitive index preserves
    // draw order within an instance.
    const uint sortKey =
        (instanceId << 30) |
        (primitiveIndex & 0x3fffffffu);
    ConditionallyStoreDetailDecalResolved(
        payload,
        materialIndex,
        sortKey,
        InterpolateSmokeTexCoord(
            instanceId,
            primitiveIndex,
            barycentrics));
}

[shader("anyhit")]
void AnyHit(inout PathTraceSmokePayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    const uint instanceId = InstanceID();
    const uint geometryIndex = GeometryIndex();
    const uint primitiveIndex = PrimitiveIndex();
    if (PathTraceIsStaticBucketRouteInstance(
            instanceId,
            StaticBucketRouteInfo))
    {
        PathTraceStaticGeometryAddress address;
        if (!PathTraceTryResolvePrimaryStaticBucketHit(
                instanceId,
                geometryIndex,
                primitiveIndex,
                address))
        {
            IgnoreHit();
            return;
        }

        uint triangleClassAndFlags;
        uint materialId;
        uint materialIndex;
        uint materialFlags;
        if (!PathTraceStaticBucketTryLoadPrimaryMetadata(
                address,
                triangleClassAndFlags,
                materialId,
                materialIndex,
                materialFlags))
        {
            IgnoreHit();
            return;
        }
        if (PathTraceLiquidPoolCollectionEnabled() &&
            PathTraceMaterialIsSemanticLiquidPool(
                materialIndex))
        {
            if (PathTraceStaticBucketDetailDecalFacesPrimaryRay(
                    address))
            {
                payload.liquidStatusMask |=
                    RT_LIQUID_POOL_STATUS_CANDIDATE;
                const PathTraceSmokeMaterial material =
                    LoadSmokeMaterial(materialIndex);
                const float2 texCoord =
                    PathTraceStaticBucketInterpolateTexCoord(
                        address,
                        attributes.barycentrics);
                float4 stageColor;
                if (TryGetLiquidPoolStageColor(
                        materialIndex,
                        stageColor))
                {
                    const float coverage =
                        saturate(SmokeAlphaCoverage(
                            material,
                            texCoord)) *
                        saturate(stageColor.a);
                    if (coverage > 0.0)
                    {
                        ConditionallyStoreLiquidPoolCandidate(
                            payload,
                            PathTraceStaticBucketCanonicalSurfaceInstanceId(
                                address),
                            materialIndex,
                            address.sourceTriangleIndex,
                            attributes.barycentrics);
                    }
                }
                else
                {
                    payload.liquidStatusMask |=
                        RT_LIQUID_POOL_STATUS_FAIL_CLOSED;
                    payload.liquidRejectionCount =
                        payload.liquidRejectionCount ==
                                0xffffffffu
                            ? 0xffffffffu
                            : payload.liquidRejectionCount +
                                1u;
                }
            }
            IgnoreHit();
            return;
        }
        if ((materialFlags &
                RT_SMOKE_MATERIAL_DETAIL_DECAL) != 0u)
        {
            if (payload.value == 0u &&
                PathTraceDecalCollectEnabled(
                    PathTraceDecalCompositeStage()) &&
                !PathTraceSafetyDisabled(
                    RT_PT_SAFETY_DISABLE_ANY_HIT_ALPHA) &&
                PathTraceStaticBucketDetailDecalFacesPrimaryRay(
                    address))
            {
                ConditionallyStoreDetailDecalResolved(
                    payload,
                    materialIndex,
                    address.sourceTriangleIndex &
                        0x3fffffffu,
                    PathTraceStaticBucketInterpolateTexCoord(
                        address,
                        attributes.barycentrics));
            }
            IgnoreHit();
            return;
        }
        const bool primaryGlassCandidate =
            PathTraceStaticBucketIsPrimaryGlassCandidate(
                triangleClassAndFlags,
                materialFlags);
        if (!primaryGlassCandidate &&
            !PathTraceStaticBucketIsOpaqueOrAlphaPrimaryCandidate(
                triangleClassAndFlags,
                materialFlags))
        {
            // REF-9B/9C admit receiver-owned detail decals and semantic liquid
            // cards. Particles and the remaining decal routes stay fail-closed
            // until their address-aware consumers are migrated.
            IgnoreHit();
            return;
        }
        if (primaryGlassCandidate)
        {
            if (PathTraceStaticBucketGlassRejectsPrimaryHit(
                    address,
                    triangleClassAndFlags,
                    materialIndex,
                    attributes.barycentrics))
            {
                IgnoreHit();
            }
            return;
        }
        if (PathTraceStaticBucketAlphaRejectsHit(
                address,
                materialIndex,
                attributes.barycentrics))
        {
            IgnoreHit();
            return;
        }
        if (payload.value == 2u &&
            instanceId == payload.shadowIgnoreInstanceId &&
            (primitiveIndex == payload.shadowIgnorePrimitiveIndex ||
                materialId == payload.shadowIgnoreMaterialId))
        {
            IgnoreHit();
        }
        return;
    }
    if (payload.value == 2u &&
        instanceId == payload.shadowIgnoreInstanceId)
    {
        const uint materialId = LoadSmokeTriangleMaterialId(instanceId, primitiveIndex);
        if (primitiveIndex == payload.shadowIgnorePrimitiveIndex || materialId == payload.shadowIgnoreMaterialId)
        {
            IgnoreHit();
            return;
        }
    }
    if (PathTraceLiquidPoolCollectionEnabled() && SmokeTriangleIndexRangeValid(instanceId, primitiveIndex))
    {
        const uint materialIndex = LoadSmokeTriangleMaterialIndex(instanceId, primitiveIndex);
        if (PathTraceMaterialIsSemanticLiquidPool(materialIndex))
        {
            // Semantic pool cards are receiver modifiers in every supported
            // geometry domain. Routed-rigid cards retain their own local
            // geometry and owner identity for resolve-time validation.
            if (instanceId < 2u ||
                PathTraceIsStaticBucketRouteInstance(
                    instanceId,
                    StaticBucketRouteInfo) ||
                PathTraceIsRigidHitRouteInstance(instanceId) ||
                PathTraceIsSkinnedHitRouteInstance(instanceId))
            {
                // Classification status describes observation of a semantic
                // supported-route card, independently of whether its evaluated
                // coverage is zero. Raw/retained counts remain positive-
                // coverage-only so zero-alpha cards stay reducer identities.
                payload.liquidStatusMask |= RT_LIQUID_POOL_STATUS_CANDIDATE;
                const PathTraceSmokeMaterial material = LoadSmokeMaterial(materialIndex);
                const float2 texCoord = InterpolateSmokeTexCoord(instanceId, primitiveIndex, attributes.barycentrics);
                float4 stageColor;
                if (TryGetLiquidPoolStageColor(materialIndex, stageColor))
                {
                    const float coverage = saturate(SmokeAlphaCoverage(material, texCoord)) * saturate(stageColor.a);
                    if (coverage > 0.0)
                    {
                        ConditionallyStoreLiquidPoolCandidate(payload, instanceId, materialIndex, primitiveIndex, attributes.barycentrics);
                    }
                }
                else
                {
                    payload.liquidStatusMask |= RT_LIQUID_POOL_STATUS_FAIL_CLOSED;
                    payload.liquidRejectionCount = payload.liquidRejectionCount == 0xffffffffu
                        ? 0xffffffffu
                        : payload.liquidRejectionCount + 1u;
                }
                IgnoreHit();
                return;
            }

            payload.liquidStatusMask |= RT_LIQUID_POOL_STATUS_FAIL_CLOSED | RT_LIQUID_POOL_STATUS_INVALID_ROUTE;
        }
    }
    if (payload.value == 0u &&
        PathTraceDecalCollectEnabled(PathTraceDecalCompositeStage()) &&
        !PathTraceSafetyDisabled(RT_PT_SAFETY_DISABLE_ANY_HIT_ALPHA) &&
        SmokeTriangleIndexRangeValid(instanceId, primitiveIndex))
    {
        const uint materialIndex = LoadSmokeTriangleMaterialIndex(instanceId, primitiveIndex);
        if ((LoadSmokeMaterial(materialIndex).flags & RT_SMOKE_MATERIAL_DETAIL_DECAL) != 0u)
        {
            ConditionallyStoreDetailDecal(payload, materialIndex, instanceId, primitiveIndex, attributes.barycentrics);
            IgnoreHit();
            return;
        }
    }
    if (SmokeAlphaRejectsHit(instanceId, primitiveIndex, attributes.barycentrics, payload.value))
    {
        IgnoreHit();
    }
}

[shader("anyhit")]
void ShadowAnyHit(inout PathTraceSmokeShadowPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    const uint instanceId = InstanceID();
    const uint geometryIndex = GeometryIndex();
    const uint primitiveIndex = PrimitiveIndex();
    if (PathTraceIsStaticBucketRouteInstance(
            instanceId,
            StaticBucketRouteInfo))
    {
        PathTraceStaticGeometryAddress address;
        uint triangleClassAndFlags;
        uint materialId;
        uint materialIndex;
        uint materialFlags;
        if (!PathTraceTryResolvePrimaryStaticBucketHit(
                instanceId,
                geometryIndex,
                primitiveIndex,
                address) ||
            !PathTraceStaticBucketTryLoadPrimaryMetadata(
                address,
                triangleClassAndFlags,
                materialId,
                materialIndex,
                materialFlags) ||
            !PathTraceStaticBucketIsOpaqueOrAlphaPrimaryCandidate(
                triangleClassAndFlags,
                materialFlags))
        {
            payload.hit = 0u;
            IgnoreHit();
            return;
        }
        if (PathTraceStaticBucketAlphaRejectsHit(
                address,
                materialIndex,
                attributes.barycentrics))
        {
            payload.hit = 0u;
            IgnoreHit();
            return;
        }
        payload.hit = 1u;
        return;
    }
    if (PathTraceLiquidPoolCollectionEnabled() &&
        SmokeTriangleIndexRangeValid(instanceId, primitiveIndex) &&
        PathTraceMaterialIsSemanticLiquidPool(LoadSmokeTriangleMaterialIndex(instanceId, primitiveIndex)))
    {
        payload.hit = 0u;
        IgnoreHit();
        return;
    }
    payload.hit = 1u;
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
                return;
            }
            const uint routeIndexOffset =
                routeInstance.indexOffset + primitiveIndex * 3u;
            if (routeIndexOffset + 2u >= PathTraceRigidRouteIndexCount())
            {
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
                return;
            }
            v0 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i0];
            v1 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i1];
            v2 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i2];
            routedMaterialId = routeInstance.materialId;
            routedMaterialIndex = routeInstance.materialIndex;
        }
        const float3 barycentrics = float3(1.0 - attributes.barycentrics.x - attributes.barycentrics.y, attributes.barycentrics.x, attributes.barycentrics.y);
        const float3 p0 = v0.position.xyz;
        const float3 p1 = v1.position.xyz;
        const float3 p2 = v2.position.xyz;
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

        payload.value = 1u;
        payload.hitT = RayTCurrent();
        payload.hitBarycentrics = attributes.barycentrics;
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
                const float3 objectRawTangent =
                    (dp1 * duv2.y - dp2 * duv1.y) *
                    inverseDeterminant;
                const float3 objectRawBitangent =
                    (dp2 * duv1.x - dp1 * duv2.x) *
                    inverseDeterminant;
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

    PathTraceSmokeVertex v0;
    PathTraceSmokeVertex v1;
    PathTraceSmokeVertex v2;
    PathTraceStaticGeometryAddress staticBucketAddress =
        (PathTraceStaticGeometryAddress)0;
    const bool staticBucketHit =
        PathTraceIsStaticBucketRouteInstance(
            instanceId,
            StaticBucketRouteInfo);
    if (staticBucketHit)
    {
        if (!PathTraceTryResolvePrimaryStaticBucketHit(
                instanceId,
                geometryIndex,
                primitiveIndex,
                staticBucketAddress))
        {
            return;
        }
        v0 = SmokeStaticBucketVertices[
            staticBucketAddress.vertexIndexes.x];
        v1 = SmokeStaticBucketVertices[
            staticBucketAddress.vertexIndexes.y];
        v2 = SmokeStaticBucketVertices[
            staticBucketAddress.vertexIndexes.z];
    }
    else
    {
        if (instanceId >= 2u ||
            !SmokeTriangleIndexRangeValid(instanceId, primitiveIndex))
        {
            return;
        }
        const uint indexOffset = primitiveIndex * 3u;
        const uint i0 = instanceId == 0u
            ? SmokeStaticIndices[indexOffset + 0u]
            : SmokeDynamicIndices[indexOffset + 0u];
        const uint i1 = instanceId == 0u
            ? SmokeStaticIndices[indexOffset + 1u]
            : SmokeDynamicIndices[indexOffset + 1u];
        const uint i2 = instanceId == 0u
            ? SmokeStaticIndices[indexOffset + 2u]
            : SmokeDynamicIndices[indexOffset + 2u];
        const uint vertexCount = instanceId == 0u
            ? PathTraceStaticVertexCount()
            : PathTraceDynamicVertexCount();
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
        {
            return;
        }
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
    }

    const float3 p0 = v0.position.xyz;
    const float3 p1 = v1.position.xyz;
    const float3 p2 = v2.position.xyz;
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
    const float4 c0 = v0.color;
    const float4 c1 = v1.color;
    const float4 c2 = v2.color;
    const float4 c20 = v0.color2;
    const float4 c21 = v1.color2;
    const float4 c22 = v2.color2;
    const float3 barycentrics = float3(1.0 - attributes.barycentrics.x - attributes.barycentrics.y, attributes.barycentrics.x, attributes.barycentrics.y);
    const uint triangleClassAndFlags = staticBucketHit
        ? SmokeStaticBucketTriangleClasses[
            staticBucketAddress.triangleIndex]
        : LoadSmokeTriangleClassAndFlags(instanceId, primitiveIndex);
    const bool forceGeometricNormal = (triangleClassAndFlags & RT_SMOKE_TRIANGLE_FORCE_GEOMETRIC_NORMAL) != 0u;

    payload.value = 1u;
    payload.hitT = RayTCurrent();
    payload.hitBarycentrics = attributes.barycentrics;
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
    payload.materialId = staticBucketHit
        ? SmokeStaticBucketTriangleMaterials[
            staticBucketAddress.triangleIndex]
        : LoadSmokeTriangleMaterialId(instanceId, primitiveIndex);
    payload.materialIndex = staticBucketHit
        ? SmokeStaticBucketTriangleMaterialIndexes[
            staticBucketAddress.triangleIndex]
        : LoadSmokeTriangleMaterialIndex(instanceId, primitiveIndex);
}
