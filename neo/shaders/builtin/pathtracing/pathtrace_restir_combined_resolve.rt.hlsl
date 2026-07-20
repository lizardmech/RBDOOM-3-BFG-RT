#include "../../vulkan.hlsli"
#ifndef __cplusplus
#ifndef uint16_t
#define uint16_t uint
#endif
#endif

#define RB_PT_ENABLE_RESTIR 1
#include "Rtxdi/DI/Reservoir.hlsli"
#define RB_RESTIR_PT_USE_RTXDI_RESERVOIR_BUFFER_PARAMETERS 1
#include "cleanroom_common/restir_pt_parameters.hlsli"

struct PathTraceSmokePayload
{
    uint value;
};

struct PathTraceSmokeShadowPayload
{
    uint hit;
    uint rayMode;
    uint ignoreInstanceId;
    uint ignorePrimitiveIndex;
    uint ignoreMaterialId;
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

struct PathTraceRestirCurrentLightRecord
{
    uint sourceType;
    uint payloadSourceIndex;
    uint identityKeyLo;
    uint identityKeyHi;
    uint compatibilityKey0;
    uint compatibilityKey1;
    uint compatibilityKey2;
    uint continuityClass;
    uint payloadHashLo;
    uint payloadHashHi;
    uint flags;
    uint invalidReasonFlags;
};

struct PathTraceRestirPreviousLightRecord
{
    uint sourceType;
    uint payloadSourceIndex;
    uint identityKeyLo;
    uint identityKeyHi;
    uint compatibilityKey0;
    uint compatibilityKey1;
    uint compatibilityKey2;
    uint continuityClass;
    uint payloadHashLo;
    uint payloadHashHi;
    uint flags;
    uint invalidReasonFlags;
};

#include "RtxdiBridge/RAB_UnifiedLightRecord.hlsli"

#include "PathTracePrimarySurface.hlsli"
#include "PathTraceMaterialFeatureTypes.hlsli"

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

VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> SmokeOutput : register(u1);
VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> SmokeAccumulation : register(u15);
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> PathTraceMotionVectors : register(u39);
VK_IMAGE_FORMAT("r32ui") RWTexture2D<uint> PathTraceMotionVectorMask : register(u40);
VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> RestirPTReflectionOutput : register(u47);
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> PathTraceRRGuideAlbedo : register(u48);
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> PathTraceRRGuideNormalRoughness : register(u49);
VK_IMAGE_FORMAT("r32f") RWTexture2D<float> PathTraceRRGuideDepth : register(u50);
VK_IMAGE_FORMAT("r32f") RWTexture2D<float> PathTraceRRGuideHitDistance : register(u51);
VK_IMAGE_FORMAT("r32ui") RWTexture2D<uint> PathTraceRRGuideResetMask : register(u52);
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> PathTraceRRGuideSpecularAlbedo : register(u53);
VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> PathTraceRRInputColor : register(u54);
VK_IMAGE_FORMAT("rg16f") RWTexture2D<float2> PathTraceRRMotionVectors : register(u78);
RaytracingAccelerationStructure SmokeScene : register(t0);
StructuredBuffer<uint> SmokeStaticTriangleMaterialIndexes : register(t11);
StructuredBuffer<uint> SmokeDynamicTriangleMaterialIndexes : register(t12);
StructuredBuffer<uint> SmokeRigidRouteTriangleMaterialIndexes : register(t25);
StructuredBuffer<PathTraceRigidRouteInstance> SmokeRigidRouteInstances : register(t26);
StructuredBuffer<PathTraceSmokeEmissiveTriangle> SmokeEmissiveTriangles : register(t16);
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
StructuredBuffer<PathTraceRestirCurrentLightRecord> PathTraceRestirLightManagerCurrent : register(t62);
StructuredBuffer<PathTraceRestirPreviousLightRecord> PathTraceRestirLightManagerPrevious : register(t63);
StructuredBuffer<uint> PathTraceRestirLightManagerCurrentToPrevious : register(t64);
StructuredBuffer<uint> PathTraceRestirLightManagerPreviousToCurrent : register(t65);
StructuredBuffer<PathTraceUnifiedLightRecord> PathTraceRestirLightManagerCurrentPayload : register(t66);
StructuredBuffer<PathTraceUnifiedLightRecord> PathTraceRestirLightManagerPreviousPayload : register(t67);
StructuredBuffer<PathTraceMaterialFeatureRecord> PathTraceMaterialFeatures : register(t80);
ConstantBuffer<RtRestirPTParameters> RestirPTParamsFlat : register(b28);
RWStructuredBuffer<RtRestirPTPackedReservoir> RestirPTReservoirs : register(u29);
RWStructuredBuffer<PathTracePrimarySurfaceRecord> PrimarySurfaceHistoryCurrent : register(u30);
RWStructuredBuffer<PathTracePrimarySurfaceRecord> PrimarySurfaceHistoryPrevious : register(u31);
RWStructuredBuffer<RtRestirPTPackedReservoir> RestirPTGiReservoirs : register(u55);
RWStructuredBuffer<RtRestirPTPackedReservoir> RestirPTDiReservoirs : register(u56);
RWStructuredBuffer<RTXDI_PackedDIReservoir> RemixRtxdiDiReservoirs : register(u68);

#define RTXDI_PT_RESERVOIR_BUFFER RestirPTReservoirs
#define RTXDI_PackedPTReservoir RtRestirPTPackedReservoir
#include "Rtxdi/PT/Reservoir.hlsli"
#undef RTXDI_PackedPTReservoir
#define RTXDI_LIGHT_RESERVOIR_BUFFER RemixRtxdiDiReservoirs
#include "Rtxdi/DI/ReservoirStorage.hlsli"

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
    float4 RestirPTDirectInfo;
    float4 RestirPTSparsityInfo;
    float4 RestirPTIndirectInfo;
    float4 RayReconstructionInfo;
    float4 UnifiedLightInfo;
    float4 RestirLightManagerInfo;
    float4 RestirLightManagerControlInfo;
    float4 RestirLightManagerRangeInfo;
    float4 RestirLightManagerSampleInfo;
    float4 RestirPdfNeeVerifierInfo;
    float4 RestirPdfNeeVerifierControlInfo;
    float4 RestirPTDiDebugInfo;
    uint4 RestirPTRemixDiReservoirInfo;
    uint4 RestirPTRemixDiReservoirPageInfo;
    float4 RestirPTGiDebugInfo;
    // Padding spans the ReGIR and NEE-cache fields of the CPU constants.
    float4 CombinedResolveReservedTail[10];
    float4 DecalInfo;
    float4 DecalInfo2;
    // x=effective liquid-pool mode. Remaining lanes are diagnostics owned by
    // the primary producer.
    float4 LiquidPoolInfo;
};

static const uint RT_SMOKE_SURFACE_CLASS_RIGID_ENTITY = 1u;
static const uint RT_SMOKE_SURFACE_CLASS_SKINNED_DEFORMED = 2u;
static const uint RT_SMOKE_SURFACE_CLASS_TRANSLUCENT = 3u;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_SHIFT = 24u;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_MASK = 0x0f000000u;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_OBJECT_GLASS = 1u;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_SMOKE_PARTICLE = 2u;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_PORTAL_WINDOW = 4u;
static const uint RT_SMOKE_TRANSLUCENT_SUBTYPE_GUI_SCREEN = 5u;
static const uint RT_SMOKE_EMISSIVE_TRIANGLE_HISTORY_DYNAMIC = 0x00020000u;
static const uint RT_SMOKE_TEXTURE_FLAG_RESERVOIR_TWO_SIDED_EMISSIVES = 0x00000040u;
static const uint RT_DOOM_ANALYTIC_LIGHT_CASTS_SHADOWS = 0x00000001u;
static const uint RT_PT_SAFETY_DISABLE_ANY_HIT_ALPHA = 0x00000001u;
static const uint RT_PT_SAFETY_DISABLE_SELECTED_LIGHT_LOOP = 0x00000002u;
static const uint RT_PT_SAFETY_DISABLE_ANALYTIC_LIGHT_LOOP = 0x00000004u;
static const uint RT_PT_SAFETY_DISABLE_EMISSIVE_TRIANGLE_SAMPLING = 0x00000008u;
static const uint RT_PT_SAFETY_DISABLE_DIFFUSE_SECONDARY_RAY = 0x00000010u;
static const uint RT_PT_SAFETY_DISABLE_REFLECTION_RAY = 0x00000020u;
static const uint RT_PT_SAFETY_DISABLE_PRIMARY_SURFACE_HISTORY = 0x00000040u;
static const uint RT_PT_SAFETY_DISABLE_RESERVOIR_WRITES = 0x00000080u;
static const uint RT_PT_SAFETY_DISABLE_RESTIR_VISIBILITY_RAY = 0x00000100u;
static const uint PT_MOTION_VECTOR_MASK_VALID = 0x00000001u;
static const uint PT_MOTION_VECTOR_MASK_SOURCE_SHIFT = 1u;
static const uint PT_MOTION_VECTOR_MASK_SOURCE_MASK = 0x0000001eu;
static const uint PT_MOTION_VECTOR_SOURCE_STATIC = 1u;
static const uint PT_MOTION_VECTOR_SOURCE_SKINNED = 2u;
static const uint PT_MOTION_VECTOR_SOURCE_RIGID = 3u;
static const uint RT_RR_RESET_INVALID_SURFACE = 0x00000001u;
static const uint RT_RR_RESET_MISSING_PREVIOUS = 0x00000002u;
static const uint RT_RR_RESET_REJECTED_PREVIOUS = 0x00000004u;
static const uint RT_RR_RESET_MATERIAL_MISMATCH = 0x00000008u;
static const uint RT_RR_RESET_OBJECT_MOTION_UNAVAILABLE = 0x00000010u;
static const uint RT_RR_RESET_STOCHASTIC_TRANSLUCENT = 0x00000020u;
static const uint RT_RR_RESET_OTHER_INVALID = 0x00000040u;

bool PathTraceSafetyDisabled(uint bit)
{
    return (((uint)SafetyInfo.x) & bit) != 0u;
}

uint PathTracePrimarySurfaceHistoryCount()
{
    return (uint)max(GeometryInfo2.z, 0.0);
}

uint2 PathTraceDispatchTileOffset()
{
    return uint2((uint)max(DispatchTileInfo.x, 0.0), (uint)max(DispatchTileInfo.y, 0.0));
}

uint2 PathTraceFullOutputSize()
{
    const uint2 size = uint2((uint)max(DispatchTileInfo.z, 0.0), (uint)max(DispatchTileInfo.w, 0.0));
    return (size.x > 0u && size.y > 0u) ? size : DispatchRaysDimensions().xy;
}

uint2 PathTraceRestirDirectSize()
{
    const uint2 size = uint2((uint)max(RestirPTDirectInfo.x, 0.0), (uint)max(RestirPTDirectInfo.y, 0.0));
    return (size.x > 0u && size.y > 0u) ? size : PathTraceFullOutputSize();
}

uint PathTraceRestirSparsityRate()
{
    return clamp((uint)max(RestirPTSparsityInfo.x, 1.0), 1u, 8u);
}

bool PathTraceRestirSparsityEnabled()
{
    return RestirPTSparsityInfo.z >= 0.5 && PathTraceRestirSparsityRate() > 1u;
}

uint PathTraceRestirSparsityPhase(bool previousFrame)
{
    const uint rate = PathTraceRestirSparsityRate();
    const float phaseValue = previousFrame ? RestirPTSparsityInfo.w : RestirPTSparsityInfo.y;
    return rate > 1u ? (uint)max(phaseValue, 0.0) % rate : 0u;
}

uint2 PathTraceRestirSparseRepresentativePixel(uint2 pixel, bool previousFrame)
{
    const uint2 dimensions = max(PathTraceRestirDirectSize(), uint2(1u, 1u));
    pixel = min(pixel, dimensions - uint2(1u, 1u));
    if (!PathTraceRestirSparsityEnabled())
    {
        return pixel;
    }

    const uint rate = PathTraceRestirSparsityRate();
    const uint phase = PathTraceRestirSparsityPhase(previousFrame);
    const uint residue = (pixel.x + pixel.y * 3u + phase) % rate;
    const int dxBack = -int(residue);
    const int dxForward = int((rate - residue) % rate);
    const int xBack = int(pixel.x) + dxBack;
    const int xForward = int(pixel.x) + dxForward;
    const bool backValid = xBack >= 0;
    const bool forwardValid = xForward < int(dimensions.x);
    int chosenX = int(pixel.x);
    if (backValid && forwardValid)
    {
        chosenX = abs(dxBack) <= abs(dxForward) ? xBack : xForward;
    }
    else if (backValid)
    {
        chosenX = xBack;
    }
    else if (forwardValid)
    {
        chosenX = xForward;
    }
    return uint2(uint(clamp(chosenX, 0, int(dimensions.x) - 1)), pixel.y);
}

uint PathTraceRestirPTIndirectSparsityRate()
{
    return clamp((uint)max(RestirPTIndirectInfo.z, 1.0), 1u, 8u);
}

bool PathTraceRestirPTIndirectSparsityEnabled()
{
    return RestirPTIndirectInfo.y >= 0.5 && PathTraceRestirPTIndirectSparsityRate() > 1u;
}

uint2 PathTraceRestirPTIndirectRepresentativePixel(uint2 pixel)
{
    const uint2 dimensions = max(PathTraceFullOutputSize(), uint2(1u, 1u));
    pixel = min(pixel, dimensions - uint2(1u, 1u));
    if (!PathTraceRestirPTIndirectSparsityEnabled())
    {
        return pixel;
    }

    const uint rate = PathTraceRestirPTIndirectSparsityRate();
    const uint phase = (uint)max(RestirPTIndirectInfo.w, 0.0) % rate;
    const uint residue = (pixel.x + pixel.y * 3u + phase) % rate;
    const int dxBack = -int(residue);
    const int dxForward = int((rate - residue) % rate);
    const int xBack = int(pixel.x) + dxBack;
    const int xForward = int(pixel.x) + dxForward;
    const bool backValid = xBack >= 0;
    const bool forwardValid = xForward < int(dimensions.x);
    int chosenX = int(pixel.x);
    if (backValid && forwardValid)
    {
        chosenX = abs(dxBack) <= abs(dxForward) ? xBack : xForward;
    }
    else if (backValid)
    {
        chosenX = xBack;
    }
    else if (forwardValid)
    {
        chosenX = xForward;
    }
    return uint2(uint(clamp(chosenX, 0, int(dimensions.x) - 1)), pixel.y);
}

uint2 PathTraceFullPixelToRestirDirectPixel(uint2 fullPixel)
{
    const uint2 fullSize = max(PathTraceFullOutputSize(), uint2(1u, 1u));
    const uint2 directSize = max(PathTraceRestirDirectSize(), uint2(1u, 1u));
    const float2 directPixel = floor(((float2(fullPixel) + 0.5) * float2(directSize)) / float2(fullSize));
    return PathTraceRestirSparseRepresentativePixel(min(uint2(directPixel), directSize - uint2(1u, 1u)), false);
}

float2 PathTraceFullPixelFloatToRestirDirectPixelFloat(float2 fullPixelFloat)
{
    const uint2 fullSize = max(PathTraceFullOutputSize(), uint2(1u, 1u));
    const uint2 directSize = max(PathTraceRestirDirectSize(), uint2(1u, 1u));
    return ((fullPixelFloat + 0.5) * float2(directSize)) / float2(fullSize) - 0.5;
}

uint2 PathTraceRestirDirectPixelToRepresentativeFullPixel(uint2 directPixel)
{
    const uint2 fullSize = max(PathTraceFullOutputSize(), uint2(1u, 1u));
    const uint2 directSize = max(PathTraceRestirDirectSize(), uint2(1u, 1u));
    const float2 fullPixel = floor(((float2(directPixel) + 0.5) * float2(fullSize)) / float2(directSize));
    return min(uint2(fullPixel), fullSize - uint2(1u, 1u));
}

uint2 PathTracePrimarySurfaceStorePixel(uint2 pixel)
{
    return pixel;
}

int2 PathTracePrimarySurfaceLoadPixel(int2 pixelPosition, bool previousFrame)
{
    return pixelPosition;
}

float TraceSmokeShadowVisibility(float3 origin, float3 direction, float tMax, uint ignoreInstanceId, uint ignorePrimitiveIndex, uint ignoreMaterialId);
#define RB_PT_ENABLE_RESTIR_LIGHT_MANAGER_RAB 1
#include "RtxdiBridge/PathTraceRtxdiBridge.hlsli"

float3 SafeNormalize(float3 value, float3 fallback)
{
    return RAB_SafeNormalize(value, fallback);
}

float3 ConstrainSmokeShadingNormal(float3 shadingNormal, float3 geometryNormal)
{
    const float minGeometryDot = 0.02;
    geometryNormal = RAB_SafeNormalize(geometryNormal, float3(0.0, 0.0, 1.0));
    shadingNormal = RAB_SafeNormalize(shadingNormal, geometryNormal);
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
    return RAB_SafeNormalize(tangentComponent * tangentScale + geometryNormal * minGeometryDot, geometryNormal);
}

#define RB_PATH_TRACE_PRIMARY_SURFACE_ENABLE_HELPERS
#include "PathTracePrimarySurface.hlsli"

float3 RestirPTToneMapPreview(float3 contribution);
float3 RestirPTReferenceFinalShadingContribution(RTXDI_PTReservoir reservoir);
bool RestirPTReferenceFinalShadingHasUsefulSample(RTXDI_PTReservoir reservoir);
float4 RestirPTReferenceFinalShadingStateColor(RTXDI_PTReservoir reservoir);
#include "pathtrace_restir_combined_resolve_reservoir_pages.hlsli"

float3 RestirPTSanitizePreviewContribution(float3 contribution)
{
    if (!all(contribution == contribution) || any(abs(contribution) > 65504.0))
    {
        return float3(0.0, 0.0, 0.0);
    }
    return max(contribution, float3(0.0, 0.0, 0.0));
}

float3 RestirPTToneMapPreview(float3 contribution)
{
    contribution = RestirPTSanitizePreviewContribution(contribution * max(RestirPTInfo.w, 0.0));
    return contribution / (float3(1.0, 1.0, 1.0) + contribution);
}

float3 RestirPTSanitizeHdrRadiance(float3 radiance)
{
    return RestirPTSanitizePreviewContribution(radiance);
}

float4 RestirPTSanitizeAccumulationColor(float4 color)
{
    if (!all(color == color) || any(abs(color) > 65504.0))
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    color.rgb = max(color.rgb, float3(0.0, 0.0, 0.0));
    color.a = 1.0;
    return color;
}

bool RestirPTDiDebugViewBypassesMode56Accumulation(uint view)
{
    return view >= 60u && view <= 77u;
}

bool RestirPTFinalConsumerOutputBypassesMode56Accumulation()
{
    return RestirPTGiDebugInfo.z >= 0.5;
}

float4 RestirPTAccumulateMode56Output(uint2 pixel, float4 color, bool bypassMode56Accumulation)
{
    if (bypassMode56Accumulation)
    {
        return color;
    }
    color = RestirPTSanitizeAccumulationColor(color);
    if (RestirPTGiDebugInfo.w < 0.5)
    {
        return color;
    }

    const uint accumulationFrame = (uint)max(ToyPathInfo.w, 0.0);
    if (accumulationFrame == 0u)
    {
        SmokeAccumulation[pixel] = color;
        return color;
    }

    float4 history = SmokeAccumulation[pixel];
    if (!all(history == history) || any(abs(history) > 65504.0))
    {
        history = color;
    }
    history = RestirPTSanitizeAccumulationColor(history);
    const float weight = 1.0 / ((float)accumulationFrame + 1.0);
    float4 accumulated = lerp(history, color, weight);
    accumulated = RestirPTSanitizeAccumulationColor(accumulated);
    SmokeAccumulation[pixel] = accumulated;
    return accumulated;
}

struct RestirPTCombinedLighting
{
    float3 preview;
    float3 hdrRadiance;
};

float3 RestirPTVisibleSurfaceBase(RAB_Surface surface)
{
    if (!RAB_IsSurfaceValid(surface))
    {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 visibleEmissive = RestirPTToneMapPreview(surface.material.emissiveRadiance);
    if (surface.surfaceClass == RT_SMOKE_SURFACE_CLASS_TRANSLUCENT &&
        ((surface.flags & RT_SMOKE_TRANSLUCENT_SUBTYPE_MASK) >> RT_SMOKE_TRANSLUCENT_SUBTYPE_SHIFT) == RT_SMOKE_TRANSLUCENT_SUBTYPE_GUI_SCREEN)
    {
        const float3 albedo = saturate(max(surface.material.diffuseAlbedo, float3(0.0, 0.0, 0.0)));
        return saturate(albedo + visibleEmissive);
    }
    return visibleEmissive;
}

float3 RestirPTReservoirPreviewContribution(RTXDI_PTReservoir reservoir)
{
    return RestirPTSanitizePreviewContribution(reservoir.TargetFunction * max(reservoir.WeightSum, 0.0));
}

bool RestirPTReservoirHasUsefulSample(RTXDI_PTReservoir reservoir)
{
    const float targetLuminance = RTXDI_Luminance(max(reservoir.TargetFunction, float3(0.0, 0.0, 0.0)));
    return RTXDI_IsValidPTReservoir(reservoir) && reservoir.WeightSum > 0.0 && targetLuminance > 0.0;
}

bool RestirPTCombinedResolveLiquidModifier(uint instanceId, uint primitiveIndex)
{
    if (LiquidPoolInfo.x < 0.5)
    {
        return false;
    }

    uint triangleCount = 0u;
    uint materialIndex = 0xffffffffu;
    if (instanceId <= 1u)
    {
        triangleCount = instanceId == 0u
            ? (uint)max(GeometryInfo0.z, 0.0)
            : (uint)max(GeometryInfo1.y, 0.0);

        uint triangleMaterialCount = 0u;
        uint triangleMaterialStride = 0u;
        if (instanceId == 0u)
        {
            SmokeStaticTriangleMaterialIndexes.GetDimensions(triangleMaterialCount, triangleMaterialStride);
        }
        else
        {
            SmokeDynamicTriangleMaterialIndexes.GetDimensions(triangleMaterialCount, triangleMaterialStride);
        }
        if (triangleMaterialStride != 4u || primitiveIndex >= triangleMaterialCount)
        {
            return false;
        }
        materialIndex = instanceId == 0u
            ? SmokeStaticTriangleMaterialIndexes[primitiveIndex]
            : SmokeDynamicTriangleMaterialIndexes[primitiveIndex];
    }
    else
    {
        uint routeInstanceCount = 0u;
        uint routeInstanceStride = 0u;
        SmokeRigidRouteInstances.GetDimensions(routeInstanceCount, routeInstanceStride);
        const uint routeInstanceIndex = instanceId - 2u;
        if (routeInstanceStride != 160u || routeInstanceIndex >= routeInstanceCount)
        {
            return false;
        }

        const PathTraceRigidRouteInstance routeInstance = SmokeRigidRouteInstances[routeInstanceIndex];
        triangleCount = routeInstance.triangleCount;
        uint routeMaterialCount = 0u;
        uint routeMaterialStride = 0u;
        SmokeRigidRouteTriangleMaterialIndexes.GetDimensions(routeMaterialCount, routeMaterialStride);
        const uint routeTriangleIndex = routeInstance.triangleOffset + primitiveIndex;
        if (routeMaterialStride != 4u || routeTriangleIndex >= routeMaterialCount)
        {
            return false;
        }
        materialIndex = SmokeRigidRouteTriangleMaterialIndexes[routeTriangleIndex];
    }

    if (primitiveIndex >= triangleCount)
    {
        return false;
    }
    uint featureRecordCount = 0u;
    uint featureRecordStride = 0u;
    PathTraceMaterialFeatures.GetDimensions(featureRecordCount, featureRecordStride);
    const uint materialCount = (uint)max(TextureInfo.z, 0.0);
    if (featureRecordCount < materialCount || featureRecordStride != 32u || materialIndex >= materialCount)
    {
        return false;
    }

    const PathTraceMaterialFeatureRecord feature = PathTraceMaterialFeatures[materialIndex];
    return feature.recordAbiVersion == RT_PATH_TRACE_MATERIAL_FEATURE_RECORD_ABI_VERSION &&
        feature.parameterRecordIndex == materialIndex &&
        feature.materialKind == RT_PATH_TRACE_MATERIAL_KIND_LIQUID_POOL_MODIFIER &&
        feature.modifierKind == RT_PATH_TRACE_MATERIAL_MODIFIER_LIQUID_POOL_UNION &&
        (feature.materialCaps & (RT_PATH_TRACE_MATERIAL_CAP_RECEIVER_MODIFIER |
            RT_PATH_TRACE_MATERIAL_CAP_IDEMPOTENT_MODIFIER_BLEND)) ==
            (RT_PATH_TRACE_MATERIAL_CAP_RECEIVER_MODIFIER |
                RT_PATH_TRACE_MATERIAL_CAP_IDEMPOTENT_MODIFIER_BLEND);
}

float TraceSmokeShadowVisibility(float3 origin, float3 direction, float tMax, uint ignoreInstanceId, uint ignorePrimitiveIndex, uint ignoreMaterialId)
{
    PathTraceSmokeShadowPayload shadowPayload;
    shadowPayload.hit = 0u;
    shadowPayload.rayMode = ignoreInstanceId != 0xffffffffu ? 2u : 1u;
    shadowPayload.ignoreInstanceId = ignoreInstanceId;
    shadowPayload.ignorePrimitiveIndex = ignorePrimitiveIndex;
    shadowPayload.ignoreMaterialId = ignoreMaterialId;

    RayDesc shadowRay;
    shadowRay.Origin = origin;
    shadowRay.Direction = direction;
    shadowRay.TMin = 0.01;
    shadowRay.TMax = tMax;

    TraceRay(
        SmokeScene,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
        0xff,
        1,
        1,
        1,
        shadowRay,
        shadowPayload);

    return shadowPayload.hit == 0u ? 1.0 : 0.0;
}

float RestirPTTraceReservoirVisibility(RAB_Surface surface, RTXDI_PTReservoir reservoir)
{
    if (PathTraceSafetyDisabled(RT_PT_SAFETY_DISABLE_RESTIR_VISIBILITY_RAY))
    {
        return 1.0;
    }
    if (!RAB_IsSurfaceValid(surface) || !RTXDI_IsValidPTReservoir(reservoir))
    {
        return 0.0;
    }

    float3 targetPosition = reservoir.TranslatedWorldPosition;
    uint ignoreInstanceId = 0xffffffffu;
    uint ignorePrimitiveIndex = 0xffffffffu;
    uint ignoreMaterialId = 0xffffffffu;

    if (RTXDI_ConnectsToNeeLight(reservoir))
    {
        const RTXDI_SampledLightData sampledLightData = RTXDI_GetSampledLightData(reservoir);
        if (!RTXDI_SampledLightData_IsValidLightData(sampledLightData))
        {
            return 0.0;
        }

        const uint lightIndex = RTXDI_SampledLightData_GetLightIndex(sampledLightData);
        const RAB_LightInfo lightInfo = RAB_LoadLightInfo(lightIndex, false);
        if (!RAB_IsLightInfoValid(lightInfo))
        {
            return 0.0;
        }

        const RAB_LightSample lightSample = RAB_SamplePolymorphicLight(
            lightInfo,
            surface,
            RTXDI_SampledLightData_GetUVDataFloat2(sampledLightData));
        if (lightSample.valid == 0u)
        {
            return 0.0;
        }

        targetPosition = lightSample.position;
        if (lightSample.lightType == RAB_LIGHT_TYPE_DOOM_ANALYTIC_SPHERE &&
            (lightSample.flags & RT_DOOM_ANALYTIC_LIGHT_CASTS_SHADOWS) == 0u)
        {
            return 1.0;
        }

        if (lightSample.lightType == RAB_LIGHT_TYPE_EMISSIVE_TRIANGLE)
        {
            const uint emissiveTriangleCount = PathTraceSafetyDisabled(RT_PT_SAFETY_DISABLE_EMISSIVE_TRIANGLE_SAMPLING) ? 0u : (uint)max(EmissiveInfo.x, 0.0);
            if (lightSample.lightIndex < emissiveTriangleCount)
            {
                const PathTraceSmokeEmissiveTriangle emissiveTriangle = SmokeEmissiveTriangles[lightSample.lightIndex];
                const bool historicalDynamicEmissive = (emissiveTriangle.padding0 & RT_SMOKE_EMISSIVE_TRIANGLE_HISTORY_DYNAMIC) != 0u;
                ignoreInstanceId = historicalDynamicEmissive ? 0xffffffffu : emissiveTriangle.instanceId;
                ignorePrimitiveIndex = historicalDynamicEmissive ? 0xffffffffu : emissiveTriangle.primitiveIndex;
                ignoreMaterialId = emissiveTriangle.materialId;
            }
        }
    }

    if (!all(targetPosition == targetPosition))
    {
        return 0.0;
    }

    const float3 toTarget = targetPosition - surface.worldPos;
    const float distanceSquared = dot(toTarget, toTarget);
    if (distanceSquared <= 1.0e-6)
    {
        return 0.0;
    }

    const float distance = sqrt(distanceSquared);
    const float3 lightDir = toTarget / distance;
    const float3 normal = RAB_SafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    if (dot(normal, lightDir) <= 0.0 || dot(RAB_GetSurfaceGeoNormal(surface), lightDir) <= 0.0)
    {
        return 0.0;
    }

    const float normalOffsetSign = dot(normal, lightDir) >= 0.0 ? 1.0 : -1.0;
    const float3 shadowOrigin = surface.worldPos + normal * (normalOffsetSign * 0.75) + lightDir * 0.25;
    const float shadowTMax = max(distance - 0.5, 0.01);
    return TraceSmokeShadowVisibility(shadowOrigin, lightDir, shadowTMax, ignoreInstanceId, ignorePrimitiveIndex, ignoreMaterialId);
}

bool RestirPTTryEvaluateNeeReservoirPreview(RAB_Surface surface, RTXDI_PTReservoir reservoir, bool traceVisibility, out float3 contribution)
{
    contribution = float3(0.0, 0.0, 0.0);
    if (!RAB_IsSurfaceValid(surface) || !RTXDI_IsValidPTReservoir(reservoir) || !RTXDI_ConnectsToNeeLight(reservoir))
    {
        return false;
    }

    const RTXDI_SampledLightData sampledLightData = RTXDI_GetSampledLightData(reservoir);
    if (!RTXDI_SampledLightData_IsValidLightData(sampledLightData))
    {
        return false;
    }

    const RAB_LightInfo lightInfo = RAB_LoadLightInfo(RTXDI_SampledLightData_GetLightIndex(sampledLightData), false);
    if (!RAB_IsLightInfoValid(lightInfo))
    {
        return false;
    }

    const RAB_LightSample lightSample = RAB_SamplePolymorphicLight(
        lightInfo,
        surface,
        RTXDI_SampledLightData_GetUVDataFloat2(sampledLightData));
    if (lightSample.valid == 0u)
    {
        return false;
    }

    float3 lightDir;
    float lightDistance;
    RAB_GetLightDirDistance(surface, lightSample, lightDir, lightDistance);
    const float3 normal = RAB_SafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    const float ndotl = saturate(dot(normal, lightDir));
    if (ndotl <= 0.0 || lightSample.solidAnglePdf <= 1.0e-6)
    {
        return false;
    }

    if (traceVisibility)
    {
        const float normalOffsetSign = dot(normal, lightDir) >= 0.0 ? 1.0 : -1.0;
        const float3 shadowOrigin = surface.worldPos + normal * (normalOffsetSign * 0.75) + lightDir * 0.25;
        const float shadowTMax = max(lightDistance - 0.5, 0.01);
        if (TraceSmokeShadowVisibility(shadowOrigin, lightDir, shadowTMax, 0xffffffffu, 0xffffffffu, 0xffffffffu) <= 0.0)
        {
            return false;
        }
    }

    const float3 reflected = RAB_EvaluateSurfaceBrdf(surface, lightDir, RAB_GetSurfaceViewDir(surface)) * lightSample.radiance * ndotl;
    const float reservoirWeight = max(reservoir.WeightSum, 1.0 / max((float)reservoir.M, 1.0));
    contribution = RestirPTSanitizePreviewContribution((reflected / max(lightSample.solidAnglePdf, 1.0e-6)) * reservoirWeight);
    return RAB_Luminance(contribution) > 0.0;
}

bool RestirPTTryEvaluateNeeReservoirTargetFunction(RAB_Surface surface, RTXDI_PTReservoir reservoir, bool traceVisibility, out float3 targetFunction)
{
    targetFunction = float3(0.0, 0.0, 0.0);
    if (!RAB_IsSurfaceValid(surface) || !RTXDI_IsValidPTReservoir(reservoir) || !RTXDI_ConnectsToNeeLight(reservoir))
    {
        return false;
    }

    const RTXDI_SampledLightData sampledLightData = RTXDI_GetSampledLightData(reservoir);
    if (!RTXDI_SampledLightData_IsValidLightData(sampledLightData))
    {
        return false;
    }

    const RAB_LightInfo lightInfo = RAB_LoadLightInfo(RTXDI_SampledLightData_GetLightIndex(sampledLightData), false);
    if (!RAB_IsLightInfoValid(lightInfo))
    {
        return false;
    }

    const RAB_LightSample lightSample = RAB_SamplePolymorphicLight(
        lightInfo,
        surface,
        RTXDI_SampledLightData_GetUVDataFloat2(sampledLightData));
    if (lightSample.valid == 0u)
    {
        return false;
    }

    float3 lightDir;
    float lightDistance;
    RAB_GetLightDirDistance(surface, lightSample, lightDir, lightDistance);
    const float3 normal = RAB_SafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    const float ndotl = saturate(dot(normal, lightDir));
    if (ndotl <= 0.0 || lightSample.solidAnglePdf <= 1.0e-6)
    {
        return false;
    }

    if (traceVisibility)
    {
        const float normalOffsetSign = dot(normal, lightDir) >= 0.0 ? 1.0 : -1.0;
        const float3 shadowOrigin = surface.worldPos + normal * (normalOffsetSign * 0.75) + lightDir * 0.25;
        const float shadowTMax = max(lightDistance - 0.5, 0.01);
        if (TraceSmokeShadowVisibility(shadowOrigin, lightDir, shadowTMax, 0xffffffffu, 0xffffffffu, 0xffffffffu) <= 0.0)
        {
            return false;
        }
    }

    const float3 reflected = RAB_EvaluateSurfaceBrdf(surface, lightDir, RAB_GetSurfaceViewDir(surface)) * lightSample.radiance * ndotl;
    targetFunction = RestirPTSanitizePreviewContribution(reflected / max(lightSample.solidAnglePdf, 1.0e-6));
    return RAB_Luminance(targetFunction) > 0.0;
}

bool RestirPTTryEvaluateSpatialDirectLighting(RAB_Surface surface, uint2 pixel, out float3 contribution)
{
    contribution = float3(0.0, 0.0, 0.0);
    const uint2 reservoirPixel = PathTraceFullPixelToRestirDirectPixel(pixel);
    const RTXDI_PTReservoir spatialReservoir = LoadRestirPTSpatialOutputReservoir(reservoirPixel);
    return RestirPTTryEvaluateNeeReservoirPreview(surface, spatialReservoir, RestirPTInfo.z >= 0.5, contribution);
}

#include "pathtrace_restir_reference_final_shading_debug.hlsli"

float4 EvaluateRestirPTReferenceTemporalReadinessView(RAB_Surface surface, uint2 pixel);
float4 EvaluateRestirPTReferenceTemporalPageFlowView(uint2 pixel);
float4 EvaluateRestirPTReferenceTemporalNeighborView(RAB_Surface surface, uint2 pixel);
float4 EvaluateRestirPTReferenceTemporalAcceptanceView(RAB_Surface surface, uint2 pixel);
float4 EvaluateRestirPTReferenceInitialTemporalContributionSplitView(RAB_Surface surface, uint2 pixel);
float4 EvaluateRestirPTReferenceInitialTemporalStateSplitView(RAB_Surface surface, uint2 pixel);
float4 EvaluateRestirPTReferenceBrdfContractView(RAB_Surface surface);
float4 EvaluateRestirPTReferenceNeeSampleContractView(RAB_Surface surface, uint2 pixel);
float4 EvaluateRestirPTReferencePathTraceMetadataView(uint2 pixel);
float4 RestirPTReferenceNeeSampleContractColor(RAB_Surface surface, RTXDI_PTReservoir reservoir);
float4 RestirPTReferencePathTraceMetadataColor(RTXDI_PTReservoir reservoir);
float4 RestirPTReferenceNeeRecordScalarColor(RTXDI_PTReservoir reservoir);
float4 RestirPTReferencePathTracerEntryColor(RAB_Surface surface, uint2 pixel);
float4 RestirPTReferenceRandomReplayMetadataColor(RTXDI_PTReservoir reservoir);
float4 RestirPTReferenceRandomSamplerBridgeColor(uint2 pixel);
float4 RestirPTReferencePathTracerUserDataBridgeColor();
float4 RestirPTReferencePsrDenoiserPayloadColor(uint2 pixel);
float4 RestirPTReferencePathTracerContinuationPolicyColor(uint2 pixel);
float4 RestirPTReferenceVisibilityPolicyColor(RAB_Surface surface, RTXDI_PTReservoir reservoir);
float4 RestirPTReferenceLightSampleNumericColor(RAB_Surface surface, RTXDI_PTReservoir reservoir);
float4 RestirPTReferenceEmissiveHitMapColor(RAB_Surface surface);
float4 RestirPTReferenceLightDomainLoadColor(uint2 pixel);
float4 RestirPTReferenceLightDomainSampleColor(RAB_Surface surface, uint2 pixel);
float4 RestirPTReferenceLightDomainVisibilityColor(RAB_Surface surface, uint2 pixel);
float4 RestirPTReferenceCurrentToPreviousLightFailureColor(uint lightIndex);
float4 EvaluateRestirPTUnifiedLightTypeView(uint2 pixel);
float4 EvaluateRestirPTUnifiedLightRadianceView(uint2 pixel);
float4 EvaluateRestirPTUnifiedLightRemapView(uint2 pixel);
float4 EvaluateRestirPTUnifiedLightNumericView(uint2 pixel);
float4 EvaluateRestirPTCpuUnifiedLightTypeView(uint2 pixel);
float4 EvaluateRestirPTCpuUnifiedLightCompareView(uint2 pixel);
float4 EvaluateRestirPTCpuUnifiedLightRemapView(uint2 pixel);
float4 EvaluateRestirPTUnifiedLoadCurrentCompareView(uint2 pixel);
float4 EvaluateRestirPTUnifiedLoadPreviousCompareView(uint2 pixel);
float4 EvaluateRestirPTUnifiedSampleCompareView(RAB_Surface surface, uint2 pixel);
float4 EvaluateRestirPTUnifiedSampleNumericView(RAB_Surface surface, uint2 pixel);
float4 EvaluateRestirPTLightManagerMapStatusView(uint2 pixel);
float4 EvaluateRestirPTDiInitialSampleValidityView(RAB_Surface surface, uint2 pixel);
float4 EvaluateRestirPTDiInitialContributionView(RAB_Surface surface, uint2 pixel);
float4 EvaluateRestirPTDiInitialNumericView(RAB_Surface surface, uint2 pixel);
float4 EvaluateRestirPTDiInitialVisibilityView(RAB_Surface surface, uint2 pixel);
float4 EvaluateRestirPTNeeRecordVsDiInitialContributionSplitView(RAB_Surface surface, uint2 pixel);
float4 EvaluateRestirPTNeeRecordVsDiInitialSampleCompareView(RAB_Surface surface, uint2 pixel);
float4 EvaluateRestirPTNeeRecordVsDiInitialContributionRatioView(RAB_Surface surface, uint2 pixel);

#include "RtxdiBridge/Debug/RAB_UnifiedLightDebug.hlsli"

#include "RtxdiBridge/Debug/RAB_RestirLightManagerDebug.hlsli"

// LU-02A: debug-only RTXDI DI initial sampling views 47-50.
#include "pathtrace_restir_di_initial_debug.hlsli"

#include "pathtrace_restir_local_debug_reservoirs.hlsli"

#include "pathtrace_restir_reference_temporal_debug.hlsli"

#include "pathtrace_restir_reference_rab_contract_debug.hlsli"

#include "pathtrace_restir_local_preview_resolve.hlsli"

#include "pathtrace_restir_rr_guide_debug.hlsli"

[shader("raygeneration")]
void RayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy + PathTraceDispatchTileOffset();
    const uint2 dimensions = PathTraceFullOutputSize();
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    if (RestirPTRrxDiTemporalPrepassEnabled())
    {
        const RAB_Surface surface = LoadPathTracePrimarySurfaceRecord(int2(pixel), false);
        RTXDI_DIReservoir temporalReservoir;
        uint temporalStatus;
        RestirPTRrxDiTemporalDebugInfo debugInfo;
        RestirPTRrxDiTryGenerateTemporalReservoir(surface, pixel, temporalReservoir, temporalStatus, debugInfo);
        return;
    }

    if (RestirPTDiTemporalPrepassEnabled())
    {
        const RAB_Surface surface = LoadPathTracePrimarySurfaceRecord(int2(pixel), false);
        uint temporalStatus;
        GenerateRestirPTDiTemporalReservoir(surface, pixel, temporalStatus);
        return;
    }

    const uint guideDebugView = RayReconstructionGuideDebugView();
    if (guideDebugView != 0u)
    {
        SmokeOutput[pixel] = RestirPTAccumulateMode56Output(pixel, EvaluateRayReconstructionGuideDebug(pixel, guideDebugView), false);
        return;
    }

    const uint diDebugView = RestirPTDiDebugView();
    if (diDebugView != 0u)
    {
        const RAB_Surface surface = LoadPathTracePrimarySurfaceRecord(int2(pixel), false);
        SmokeOutput[pixel] = RestirPTAccumulateMode56Output(
            pixel,
            EvaluateRestirPTDiDebugView(surface, pixel, diDebugView),
            RestirPTDiDebugViewBypassesMode56Accumulation(diDebugView));
        return;
    }

    const uint giDebugView = RestirPTGiDebugView();
    if (giDebugView != 0u)
    {
        const RAB_Surface surface = LoadPathTracePrimarySurfaceRecord(int2(pixel), false);
        SmokeOutput[pixel] = RestirPTAccumulateMode56Output(pixel, EvaluateRestirPTGiDebugView(surface, pixel, giDebugView), false);
        return;
    }

    const RAB_Surface surface = LoadPathTracePrimarySurfaceRecord(int2(pixel), false);
    SmokeOutput[pixel] = RestirPTAccumulateMode56Output(
        pixel,
        EvaluateCombinedResolve(surface, pixel),
        RestirPTFinalConsumerOutputBypassesMode56Accumulation());
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

[shader("anyhit")]
void AnyHit(inout PathTraceSmokePayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
}

[shader("anyhit")]
void ShadowAnyHit(inout PathTraceSmokeShadowPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    if (payload.rayMode == 2u &&
        InstanceID() == payload.ignoreInstanceId &&
        PrimitiveIndex() == payload.ignorePrimitiveIndex)
    {
        payload.hit = 0u;
        IgnoreHit();
        return;
    }
    if (RestirPTCombinedResolveLiquidModifier(InstanceID(), PrimitiveIndex()))
    {
        // A liquid card modifies the opaque receiver reconstructed by the
        // primary producer. It is not standalone occluding geometry; allowing
        // this card to terminate the visibility ray produces a rectangular
        // black shadow from its transparent texels.
        payload.hit = 0u;
        IgnoreHit();
        return;
    }
    payload.hit = 1u;
}

[shader("closesthit")]
void ClosestHit(inout PathTraceSmokePayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
}

[shader("closesthit")]
void ShadowClosestHit(inout PathTraceSmokeShadowPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    payload.hit = 1u;
}
