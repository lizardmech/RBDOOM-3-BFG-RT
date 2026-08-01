#pragma once

// CPU-side scene input contract for PT passes.
//
// This is deliberately not a shader ABI. It packages the current smoke-path
// resource handles, counts, signatures, and provenance so later PT/ReSTIR
// stages can consume a named input set instead of reaching back into scene
// capture or PathTracePrimaryPass private state.
//
// Source3 policy baseline:
// - source3 is the default path for the current RT/PT BVH.
// - source0 remains an emergency comparison/fallback path.
// - portal-area membership drives static preload, rigid residency, emissive
//   light residency, and Doom analytic light selection.
// - PS_BLOCK_VIEW portals are counted/reported by existing selection code but
//   do not hard-stop the current source3 default.
// - current-area plus short view-origin/view-axis probe seeds and four portal
//   steps is the validated greedy baseline for source3.
// - portal-area membership is preferred over screen-visible-only capture.
// - future line-of-sight portal supplementation should add visible boundary
//   areas only, without recursively walking outward from those LoS-added areas.

#include <nvrhi/nvrhi.h>

#include <cstdint>

enum RtPathTraceSceneInputCapabilityFlags : uint32_t
{
    RT_SCENE_INPUT_SOURCE3_BASELINE = 1u << 0,
    RT_SCENE_INPUT_SOURCE0_EMERGENCY_FALLBACK = 1u << 1,
    RT_SCENE_INPUT_PORTAL_AREA_RESIDENCY = 1u << 2,
    RT_SCENE_INPUT_PORTAL_BLOCK_VIEW_REPORTED = 1u << 3,
    RT_SCENE_INPUT_GEOMETRY_PREVIOUS_TRANSFORM_RESERVED = 1u << 4,
    RT_SCENE_INPUT_GEOMETRY_PREVIOUS_VERTEX_RESERVED = 1u << 5,
    RT_SCENE_INPUT_MATERIAL_STOPGAP_CLASSIFIER = 1u << 6,
    RT_SCENE_INPUT_MATERIAL_IDTECH4_SEMANTICS_RESERVED = 1u << 7,
    RT_SCENE_INPUT_MATERIAL_PBR_ROLES_RESERVED = 1u << 8,
    RT_SCENE_INPUT_LIGHT_PREVIOUS_IDENTITY_RESERVED = 1u << 9,
    RT_SCENE_INPUT_SKINNED_SOURCE_GEOMETRY_RESERVED = 1u << 10,
    RT_SCENE_INPUT_SKINNED_GPU_SKINNING_RESERVED = 1u << 11,
    RT_SCENE_INPUT_MATERIAL_DYNAMIC_CHANNEL_RESERVED = 1u << 12
};

struct RtPathTraceSceneInputSignatures
{
    uint64 geometryMembership = 0;
    uint64 materialTable = 0;
    uint64 lightMembership = 0;
    uint64 outputResolution = 0;
    uint64 cameraProjection = 0;
    uint64 debugFeaturePolicy = 0;
    uint64 cpuUploadGeneration = 0;
    uint64 reservoirScene = 0;
};

struct RtPathTraceSceneInputPortalPolicy
{
    int sceneSource = 0;
    int viewArea = -1;
    int currentArea = -1;
    int totalAreas = 0;
    int staticAreaPreloadSteps = 0;
    int rigidResidencySteps = 0;
    int lightAreaSteps = 0;
    int selectedAreaCount = 0;
    int portalEdges = 0;
    int blockedPortalEdges = 0;
    int rigidSelectedAreaCount = 0;
    int rigidPortalEdges = 0;
    int rigidBlockedPortalEdges = 0;
    bool bruteForceFullMap = false;
    bool defaultPolicyEquivalent = false;
};

struct RtPathTraceSceneInputGeometry
{
    nvrhi::rt::AccelStructHandle tlas;
    nvrhi::rt::AccelStructHandle staticBlas;
    nvrhi::rt::AccelStructHandle dynamicBlas;
    nvrhi::BufferHandle staticVertexBuffer;
    nvrhi::BufferHandle staticIndexBuffer;
    nvrhi::BufferHandle staticTriangleClassBuffer;
    nvrhi::BufferHandle staticTriangleMaterialBuffer;
    nvrhi::BufferHandle staticTriangleMaterialIndexBuffer;
    nvrhi::BufferHandle previousStaticVertexBuffer;
    nvrhi::BufferHandle previousStaticIndexBuffer;
    nvrhi::BufferHandle previousStaticTriangleClassBuffer;
    nvrhi::BufferHandle previousStaticTriangleMaterialBuffer;
    nvrhi::BufferHandle previousStaticTriangleMaterialIndexBuffer;
    nvrhi::BufferHandle dynamicVertexBuffer;
    nvrhi::BufferHandle dynamicIndexBuffer;
    nvrhi::BufferHandle dynamicTriangleClassBuffer;
    nvrhi::BufferHandle dynamicTriangleMaterialBuffer;
    nvrhi::BufferHandle dynamicTriangleMaterialIndexBuffer;
    nvrhi::BufferHandle rigidRouteVertexBuffer;
    nvrhi::BufferHandle rigidRouteIndexBuffer;
    nvrhi::BufferHandle rigidRouteTriangleMaterialBuffer;
    nvrhi::BufferHandle rigidRouteTriangleMaterialIndexBuffer;
    nvrhi::BufferHandle rigidRouteInstanceBuffer;
    nvrhi::BufferHandle skinnedHitRouteRecordBuffer;
    nvrhi::BufferHandle skinnedHitRouteTriangleBuffer;
    nvrhi::BufferHandle skinnedSourceIndexBuffer;
    nvrhi::BufferHandle skinnedSourceVertexBuffer;
    nvrhi::BufferHandle skinnedCurrentOutputVertexBuffer;
    nvrhi::BufferHandle skinnedPreviousPositionBuffer;
    nvrhi::BufferHandle skinnedSurfaceDispatchBuffer;
    nvrhi::BufferHandle skinnedTriangleDispatchIndexBuffer;
    nvrhi::BufferHandle skinnedCurrentJointMatrixBuffer;
    nvrhi::BufferHandle skinnedPreviousJointMatrixBuffer;
    int staticVertexCount = 0;
    int staticIndexCount = 0;
    int staticTriangleCount = 0;
    int staticMaterialIndexCount = 0;
    int previousStaticVertexCount = 0;
    int previousStaticIndexCount = 0;
    int previousStaticTriangleCount = 0;
    int previousStaticMaterialIndexCount = 0;
    int previousStaticCpuVertexCount = 0;
    int previousStaticCpuIndexCount = 0;
    int previousStaticCpuTriangleCount = 0;
    int previousStaticCpuMaterialIndexCount = 0;
    int previousStaticCpuBytesKB = 0;
    int staticSeenSurfaceCount = 0;
    int staticNewSurfaceCount = 0;
    int staticGoneSurfaceCount = 0;
    int staticHistoryValidSurfaceCount = 0;
    int staticPreviousRangeValidSurfaceCount = 0;
    int staticDirtySurfaceCount = 0;
    int staticDirtyVertexOffset = -1;
    int staticDirtyVertexCount = 0;
    int staticDirtyIndexOffset = -1;
    int staticDirtyIndexCount = 0;
    int staticDirtyTriangleOffset = -1;
    int staticDirtyTriangleCount = 0;
    bool staticDirtyRangeUploadUsed = false;
    bool staticPreviousBuffersAvailable = false;
    bool staticPreviousMaterialIndexBufferAvailable = false;
    bool staticPreviousBuffersAliasCurrent = false;
    bool staticPreviousCpuSnapshotAvailable = false;
    bool staticPreviousGpuSnapshotAvailable = false;
    bool staticPreviousGpuSnapshotUploadUsed = false;
    bool staticPreviousCountsMatch = false;
    bool staticPreviousRangesComplete = false;
    int dynamicVertexCount = 0;
    int dynamicIndexCount = 0;
    int dynamicTriangleCount = 0;
    int dynamicClassifiedSurfaceCount = 0;
    int dynamicClassifiedTriangleCount = 0;
    int dynamicClassifiedTriangleDelta = 0;
    bool dynamicClassifiedCountsMatch = false;
    int dynamicRigidSurfaceCount = 0;
    int dynamicRigidTriangleCount = 0;
    int dynamicSkinnedCpuCurrentSurfaceCount = 0;
    int dynamicSkinnedCpuCurrentTriangleCount = 0;
    int dynamicSkinnedLikelyBasePoseSurfaceCount = 0;
    int dynamicSkinnedLikelyBasePoseTriangleCount = 0;
    int dynamicSkinnedRtCpuSurfaceCount = 0;
    int dynamicSkinnedRtCpuTriangleCount = 0;
    int dynamicParticleAlphaSurfaceCount = 0;
    int dynamicParticleAlphaTriangleCount = 0;
    int dynamicUnknownSurfaceCount = 0;
    int dynamicUnknownTriangleCount = 0;
    int dynamicRetainedOccluderSurfaceCount = 0;
    int dynamicRetainedOccluderTriangleCount = 0;
    int rigidRouteVertexCount = 0;
    int rigidRouteIndexCount = 0;
    int rigidRouteTriangleCount = 0;
    int rigidRouteInstanceCount = 0;
    int rigidRoutePreviousTransformCount = 0;
    int skinnedHitRouteRecordCount = 0;
    int skinnedHitRouteTriangleCount = 0;
    int skinnedSourceIndexCount = 0;
    int skinnedPreviousPositionCount = 0;
    int skinnedSurfaceDispatchCount = 0;
    int skinnedTriangleDispatchIndexCount = 0;
    int skinnedGpuComputeVertexCount = 0;
    int skinnedGpuComputeMaxVertexCount = 0;
    bool currentGeometryValid = false;
    bool previousTransformAvailable = false;
    bool skinnedPreviousPositionBufferAvailable = false;
    bool skinnedGpuComputeDispatched = false;
    bool skinnedGpuComputeWritesPreviousPositions = false;
    uint32_t staticBucketRouteFirstInstanceId = 0;
    uint32_t staticBucketTriangleCount = 0;
    uint64_t staticBucketRouteGeneration = 0;
    bool staticBucketRoutePublicationValid = false;
    uint32_t capabilityFlags = 0;
};

struct RtPathTraceSceneInputMaterials
{
    nvrhi::BufferHandle materialTableBuffer;
    nvrhi::BufferHandle materialFeatureBuffer;
    nvrhi::BufferHandle materialFeatureParameterBuffer;
    nvrhi::BufferHandle dynamicMaterialBuffer;
    nvrhi::BindingLayoutHandle textureBindlessLayout;
    nvrhi::DescriptorTableHandle textureDescriptorTable;
    nvrhi::SamplerHandle textureSampler;
    int materialTableEntryCount = 0;
    int materialFeatureRecordCount = 0;
    int materialFeatureParameterRecordCount = 0;
    int dynamicMaterialRecordCount = 0;
    bool materialTableGpuStable = false;
    int activeTextureCount = 0;
    int logicalTextureDescriptorCount = 0;
    uint64_t textureDescriptorGeneration = 0;
    const char* materialTablePath = "unknown";
    uint32_t capabilityFlags = 0;
};

struct RtPathTraceSceneInputLights
{
    nvrhi::BufferHandle emissiveTriangleBuffer;
    nvrhi::BufferHandle previousEmissiveTriangleBuffer;
    nvrhi::BufferHandle emissiveRemapBuffer;
    nvrhi::BufferHandle emissiveDistributionBuffer;
    nvrhi::BufferHandle lightCandidateBuffer;
    nvrhi::BufferHandle doomAnalyticLightBuffer;
    nvrhi::BufferHandle doomAnalyticPreviousLightBuffer;
    nvrhi::BufferHandle doomAnalyticCurrentIdentityBuffer;
    nvrhi::BufferHandle doomAnalyticPreviousIdentityBuffer;
    nvrhi::BufferHandle doomAnalyticRemapBuffer;
    nvrhi::BufferHandle unifiedLightBuffer;
    nvrhi::BufferHandle unifiedPreviousLightBuffer;
    nvrhi::BufferHandle unifiedLightRemapBuffer;
    nvrhi::BufferHandle restirLightManagerCurrentPayloadBuffer;
    nvrhi::BufferHandle restirLightManagerPreviousPayloadBuffer;
    nvrhi::BufferHandle unifiedPtEmissiveLookupBuffer;
    int emissiveTriangleCount = 0;
    int emissiveDistributionCount = 0;
    int emissiveDistributionZeroPdfSkipped = 0;
    int emissiveDistributionFallbackIndex = -1;
    int emissiveStaticTriangleCount = 0;
    int emissiveDynamicTriangleCount = 0;
    int lightCandidateCount = 0;
    int texturedLightCandidateCount = 0;
    int doomAnalyticLightCount = 0;
    int doomAnalyticPreviousLightCount = 0;
    int doomAnalyticCurrentIdentityCount = 0;
    int doomAnalyticPreviousIdentityCount = 0;
    int doomAnalyticRemapCount = 0;
    int doomAnalyticInvalidRemapCount = 0;
    int previousEmissiveTriangleCount = 0;
    int unifiedLightCount = 0;
    int unifiedPreviousLightCount = 0;
    int unifiedLightRemapCount = 0;
    int restirLightManagerCurrentPayloadCount = 0;
    int restirLightManagerPreviousPayloadCount = 0;
    int unifiedPtEmissiveLookupCount = 0;
    uint32_t restirLightManagerEmissiveRangeOffset = 0;
    uint32_t restirLightManagerEmissiveRangeCount = 0;
    uint32_t restirLightManagerDoomAnalyticRangeOffset = 0;
    uint32_t restirLightManagerDoomAnalyticRangeCount = 0;
    uint32_t restirLightManagerDoomAnalyticSampleableCount = 0;
    uint64_t restirLightManagerStructuralSignature = 0;
    uint64_t restirLightManagerMappingSignature = 0;
    uint64_t restirLightManagerPayloadSignature = 0;
    uint64_t unifiedPtEmissiveLookupSignature = 0;
    float emissiveDistributionTotalPdf = 0.0f;
    float emissiveDistributionFallbackWeight = 0.0f;
    bool emissiveDistributionValid = false;
    bool unifiedPtEmissiveLookupExact = false;
    uint32_t capabilityFlags = 0;
};

struct RtPathTraceSceneInputDiagnostics
{
    uint64_t geometryUploadBytes = 0;
    uint64_t staticUploadBytes = 0;
    uint64_t previousStaticUploadBytes = 0;
    uint64_t previousStaticUploadSkippedBytes = 0;
    uint64_t dynamicUploadBytes = 0;
    uint64_t rigidRouteUploadBytes = 0;
    uint64_t materialUploadBytes = 0;
    uint64_t lightUploadBytes = 0;
    int sceneBuildMs = 0;
    int captureMs = 0;
    int materialMs = 0;
    int emissiveMs = 0;
    int bufferCreateMs = 0;
    int bufferUploadMs = 0;
    int accelSubmitMs = 0;
};

struct RtPathTraceSceneInputs
{
    bool valid = false;
    int sceneSource = 0;
    int debugMode = 0;
    int outputWidth = 0;
    int outputHeight = 0;
    uint32_t capabilityFlags = 0;
    RtPathTraceSceneInputSignatures signatures;
    RtPathTraceSceneInputPortalPolicy portalPolicy;
    RtPathTraceSceneInputGeometry geometry;
    RtPathTraceSceneInputMaterials materials;
    RtPathTraceSceneInputLights lights;
    RtPathTraceSceneInputDiagnostics diagnostics;
};
