#pragma once

// Emissive triangle inventory for mode 19 diagnostics and future sampling work.
//
// Scans static and dynamic triangle buckets after material table creation,
// records compact emissive candidate metadata, and keeps summary data for debug
// dumps and later ReSTIR/PT integration boundaries.

#include "PathTraceGeometry.h"

#include <vector>

struct viewDef_t;
struct RtPathTraceRigidRouteBuild;
struct RtSmokeStaticBucketGeometryPack;
struct RtPathTraceStaticBucketActivePublication;

struct PathTraceSmokeMaterial
{
    float debugAlbedo[4];
    float emissiveColor[4];
    uint32_t diffuseTextureIndex = UINT32_MAX;
    uint32_t alphaTextureIndex = UINT32_MAX;
    uint32_t normalTextureIndex = UINT32_MAX;
    uint32_t specularTextureIndex = UINT32_MAX;
    uint32_t emissiveTextureIndex = UINT32_MAX;
    float alphaCutoff = 0.0f;
    uint32_t flags = 0;
    uint32_t textureWidth = 1;
    uint32_t textureHeight = 1;
    uint32_t alphaTextureWidth = 1;
    uint32_t alphaTextureHeight = 1;
    uint32_t normalTextureWidth = 1;
    uint32_t normalTextureHeight = 1;
    uint32_t specularTextureWidth = 1;
    uint32_t specularTextureHeight = 1;
    uint32_t emissiveTextureWidth = 1;
    uint32_t emissiveTextureHeight = 1;
    uint32_t padding0 = 0;
    uint32_t padding1 = 0;
    uint32_t padding2 = 0;
};
static_assert((sizeof(PathTraceSmokeMaterial) % 16) == 0, "PathTraceSmokeMaterial must stay 16-byte aligned for HLSL StructuredBuffer reads");

struct PathTraceSmokeEmissiveTriangle
{
    float centerAndArea[4];
    float normalAndLuminance[4];
    float uvBounds[4];
    float centroidUvAndWeight[4];
    float estimatedRadianceAndLuminance[4];
    float sampleWeightAndPdf[4];
    uint32_t materialIndex = 0;
    uint32_t instanceId = 0;
    uint32_t primitiveIndex = 0;
    uint32_t flags = 0;
    uint32_t emissiveTextureIndex = UINT32_MAX;
    uint32_t emissiveTextureWidth = 1;
    uint32_t emissiveTextureHeight = 1;
    uint32_t materialId = 0;
    uint32_t universeMaterialIndex = UINT32_MAX;
    uint32_t identityHashLo = 0;
    uint32_t identityHashHi = 0;
    uint32_t padding0 = 0;
};
static_assert((sizeof(PathTraceSmokeEmissiveTriangle) % 16) == 0, "PathTraceSmokeEmissiveTriangle must stay 16-byte aligned for HLSL StructuredBuffer reads");

const uint32_t RT_SMOKE_EMISSIVE_REMAP_VALID = 0x00000001u;
const uint32_t RT_SMOKE_EMISSIVE_REMAP_CURRENT_ZERO_IDENTITY = 0x00000010u;
const uint32_t RT_SMOKE_EMISSIVE_REMAP_PREVIOUS_ZERO_IDENTITY = 0x00000020u;
const uint32_t RT_SMOKE_EMISSIVE_REMAP_CURRENT_DUPLICATE = 0x00000040u;
const uint32_t RT_SMOKE_EMISSIVE_REMAP_PREVIOUS_DUPLICATE = 0x00000080u;
const uint32_t RT_SMOKE_EMISSIVE_REMAP_CURRENT_MISSING = 0x00000100u;
const uint32_t RT_SMOKE_EMISSIVE_REMAP_PREVIOUS_MISSING = 0x00000200u;
const uint32_t RT_SMOKE_EMISSIVE_REMAP_INCOMPATIBLE = 0x00000400u;

struct PathTraceEmissiveLightRemap
{
    int32_t previousToCurrentIndex = -1;
    int32_t currentToPreviousIndex = -1;
    uint32_t flags = 0;
    uint32_t padding0 = 0;
};
static_assert(sizeof(PathTraceEmissiveLightRemap) == 16, "PathTraceEmissiveLightRemap must match HLSL layout");

struct PathTraceEmissiveDistributionEntry
{
    uint32_t emissiveTriangleIndex = UINT32_MAX;
    float cumulativePdf = 0.0f;
    float weight = 0.0f;
    float padding0 = 0.0f;
};
static_assert(sizeof(PathTraceEmissiveDistributionEntry) == 16, "PathTraceEmissiveDistributionEntry must match HLSL layout");

struct RtSmokeEmissiveDistributionBuild
{
    std::vector<PathTraceEmissiveDistributionEntry> entries;
    uint32_t fallbackIndex = UINT32_MAX;
    float fallbackWeight = 0.0f;
    float totalPdf = 0.0f;
    int zeroPdfSkipped = 0;
    bool valid = false;
};

struct PtSkinnedEmissiveAuditTriangle
{
    uint32_t currentVertexIndexes[3] = {};
    uint32_t previousPositionIndexes[3] = {};
    uint32_t materialIndex = UINT32_MAX;
    uint32_t materialId = 0;
    uint32_t instanceId = 0;
    uint32_t primitiveIndex = 0;
    uint32_t triangleClassAndFlags = 0;
    uint64_t identityHash = 0;
    bool hasPrevious = false;
};

struct PtSkinnedEmissiveAuditInventory
{
    std::vector<PathTraceSmokeEmissiveTriangle> current;
    std::vector<PathTraceSmokeEmissiveTriangle> previous;
    std::vector<uint32_t> currentSourceTriangleIndexes;
    std::vector<uint32_t> previousSourceTriangleIndexes;
    uint64_t inputTriangles = 0;
    uint64_t invalidTriangles = 0;
    uint64_t nonEmissiveTriangles = 0;
    uint64_t runtimeInactiveTriangles = 0;
    uint64_t zeroIdentityTriangles = 0;
    uint64_t zeroAreaCurrentTriangles = 0;
    uint64_t zeroAreaPreviousTriangles = 0;
    uint64_t missingPreviousTriangles = 0;
};

static constexpr uint32_t
    PT_SKINNED_EMISSIVE_GPU_INVALID_INDEX = UINT32_MAX;
static constexpr uint32_t
    PT_SKINNED_EMISSIVE_GPU_WRITE_PREVIOUS = 1u << 0;
static constexpr uint32_t
    PT_SKINNED_EMISSIVE_GPU_WRITE_CURRENT_UNIFIED = 1u << 1;
static constexpr uint32_t
    PT_SKINNED_EMISSIVE_GPU_WRITE_PREVIOUS_UNIFIED = 1u << 2;
static constexpr uint32_t
    PT_SKINNED_EMISSIVE_GPU_WRITE_CURRENT_PAYLOAD = 1u << 3;
static constexpr uint32_t
    PT_SKINNED_EMISSIVE_GPU_WRITE_PREVIOUS_PAYLOAD = 1u << 4;
static constexpr uint32_t
    PT_SKINNED_EMISSIVE_GPU_PUBLISH_ENABLED = 1u << 5;

struct PathTraceSkinnedEmissiveGpuWork
{
    uint32_t currentVertexIndexes[3] = {};
    uint32_t previousPositionIndexes[3] = {};
    uint32_t currentEmissiveIndex =
        PT_SKINNED_EMISSIVE_GPU_INVALID_INDEX;
    uint32_t previousEmissiveIndex =
        PT_SKINNED_EMISSIVE_GPU_INVALID_INDEX;
    uint32_t currentUnifiedIndex =
        PT_SKINNED_EMISSIVE_GPU_INVALID_INDEX;
    uint32_t previousUnifiedIndex =
        PT_SKINNED_EMISSIVE_GPU_INVALID_INDEX;
    uint32_t currentPayloadIndex =
        PT_SKINNED_EMISSIVE_GPU_INVALID_INDEX;
    uint32_t previousPayloadIndex =
        PT_SKINNED_EMISSIVE_GPU_INVALID_INDEX;
    uint32_t workItemCount = 0;
    uint32_t flags = 0;
    uint32_t padding0 = 0;
    uint32_t padding1 = 0;
};
static_assert(
    sizeof(PathTraceSkinnedEmissiveGpuWork) == 64,
    "PathTraceSkinnedEmissiveGpuWork must match HLSL layout");

const uint32_t RT_SMOKE_LIGHT_CANDIDATE_TEXTURED = 0x00000001u;
const uint32_t RT_SMOKE_LIGHT_CANDIDATE_SAFE_TEXTURE = 0x00000002u;
const uint32_t RT_SMOKE_LIGHT_CANDIDATE_HAS_STATIC_TRIANGLES = 0x00000004u;
const uint32_t RT_SMOKE_LIGHT_CANDIDATE_HAS_DYNAMIC_TRIANGLES = 0x00000008u;

struct PathTraceSmokeLightCandidate
{
    float emissiveColorAndLuminance[4];
    float areaAndWeightedLuminance[4];
    uint32_t materialId = 0;
    uint32_t universeMaterialIndex = UINT32_MAX;
    uint32_t materialIndex = 0;
    uint32_t triangleCount = 0;
    uint32_t flags = 0;
    uint32_t staticTriangleCount = 0;
    uint32_t dynamicTriangleCount = 0;
    uint32_t emissiveTextureIndex = UINT32_MAX;
    uint32_t emissiveTextureWidth = 1;
    uint32_t emissiveTextureHeight = 1;
    uint32_t padding1 = 0;
    uint32_t padding2 = 0;
};
static_assert((sizeof(PathTraceSmokeLightCandidate) % 16) == 0, "PathTraceSmokeLightCandidate must stay 16-byte aligned for HLSL StructuredBuffer reads");

struct RtSmokeEmissiveLightCandidateSummary
{
    uint32_t materialId = 0;
    uint32_t universeMaterialIndex = UINT32_MAX;
    uint32_t materialIndex = 0;
    int triangles = 0;
    int staticTriangles = 0;
    int dynamicTriangles = 0;
    float area = 0.0f;
    float weightedLuminance = 0.0f;
    float emissiveLuminance = 0.0f;
    idVec4 emissiveColor = idVec4(0.0f, 0.0f, 0.0f, 1.0f);
    bool hasEmissiveTexture = false;
    bool hasSafeEmissiveTexture = false;
    uint32_t emissiveTextureIndex = UINT32_MAX;
    uint32_t emissiveTextureWidth = 1;
    uint32_t emissiveTextureHeight = 1;
};

struct RtSmokeEmissiveInventoryStats
{
    int totalTriangles = 0;
    int staticTriangles = 0;
    int dynamicTriangles = 0;
    int routedRigidTriangles = 0;
    int fullLevelStaticTriangles = 0;
    int capturedTriangles = 0;
    int skippedSkinnedTriangles = 0;
    int skippedInvalidMaterialTriangles = 0;
    int skippedNonEmissiveMaterialTriangles = 0;
    int skippedRuntimeInactiveTriangles = 0;
    int cappedTriangles = 0;
    int zeroAreaTriangles = 0;
    int worldStaticScannedEntities = 0;
    int worldStaticScannedSurfaces = 0;
    int worldStaticScannedTriangles = 0;
    int worldStaticAcceptedSurfaces = 0;
    int worldStaticAcceptedTriangles = 0;
    int worldStaticSkippedInvalidMaterialTriangles = 0;
    int worldStaticSkippedNonEmissiveMaterialTriangles = 0;
    int worldStaticZeroAreaTriangles = 0;
    int worldStaticCappedTriangles = 0;
    int worldStaticFinalAppended = 0;
    int uniqueMaterials = 0;
    int routedRigidInstances = 0;
    int routedRigidSeenInstances = 0;
    int routedRigidCacheInstances = 0;
    int routedRigidEmissiveInstances = 0;
    int routedRigidEmissiveSeenInstances = 0;
    int routedRigidEmissiveCacheInstances = 0;
    int routedRigidCapturedTriangles = 0;
    int routedRigidCappedTriangles = 0;
    int routedRigidInvalidTriangles = 0;
    int routedRigidNonEmissiveTriangles = 0;
    float routedRigidArea = 0.0f;
    float routedRigidWeightedLuminance = 0.0f;
    float totalArea = 0.0f;
    float totalWeightedLuminance = 0.0f;
    std::vector<uint32_t> materialIndexes;
    std::vector<RtSmokeEmissiveLightCandidateSummary> lightCandidates;
    int candidateMaterials = 0;
    int texturedCandidateMaterials = 0;
    int untexturedCandidateMaterials = 0;
};

float SmokeMaterialEmissiveLuminance(const PathTraceSmokeMaterial& material);
std::vector<PathTraceSmokeLightCandidate> BuildSmokeLightCandidateBufferRecords(
    const RtSmokeEmissiveInventoryStats& stats);
RtSmokeEmissiveInventoryStats BuildSmokeEmissiveInventoryStatsForRecords(
    const std::vector<uint32_t>& materialIds,
    const std::vector<PathTraceSmokeEmissiveTriangle>& emissiveTriangles);
void FinalizeSmokeEmissiveTriangleSamplingFields(
    std::vector<PathTraceSmokeEmissiveTriangle>& emissiveTriangles,
    const RtSmokeEmissiveInventoryStats& stats);
RtSmokeEmissiveDistributionBuild BuildSmokeEmissiveDistribution(
    const std::vector<PathTraceSmokeEmissiveTriangle>& emissiveTriangles);
PtSkinnedEmissiveAuditInventory BuildSmokeCanonicalSkinnedEmissiveAuditInventory(
    const std::vector<uint32_t>& materialIds,
    const std::vector<PathTraceSmokeMaterial>& materials,
    const std::vector<PathTraceSmokeVertex>& currentVertices,
    const std::vector<PathTraceSkinnedPreviousPosition>& previousPositions,
    const std::vector<PtSkinnedEmissiveAuditTriangle>& triangles,
    uint32_t emissiveMaterialFlag,
    int maxRecords);
std::vector<PathTraceEmissiveLightRemap> BuildSmokeCanonicalEmissiveLightRemap(
    const std::vector<PathTraceSmokeEmissiveTriangle>& currentTriangles,
    const std::vector<PathTraceSmokeEmissiveTriangle>& previousTriangles);
void AppendSmokeRigidRouteEmissiveTriangleInventory(
    const std::vector<uint32_t>& materialIds,
    const std::vector<PathTraceSmokeMaterial>& materials,
    const RtPathTraceRigidRouteBuild& rigidRouteBuild,
    uint32_t emissiveMaterialFlag,
    int maxRecords,
    std::vector<PathTraceSmokeEmissiveTriangle>& emissiveTriangles,
    RtSmokeEmissiveInventoryStats& stats);
void AppendSmokeStaticBucketEmissiveTriangleInventory(
    const std::vector<uint32_t>& materialIds,
    const std::vector<PathTraceSmokeMaterial>& materials,
    const RtSmokeStaticBucketGeometryPack& geometryPack,
    const std::vector<uint32_t>& triangleMaterialIndexes,
    const RtPathTraceStaticBucketActivePublication& publication,
    uint32_t emissiveMaterialFlag,
    uint32_t triangleClassMask,
    uint32_t skinnedSurfaceClassId,
    int maxRecords,
    std::vector<PathTraceSmokeEmissiveTriangle>& emissiveTriangles,
    RtSmokeEmissiveInventoryStats& stats,
    const std::vector<uint32_t>*
        monolithicPrimitiveIndexes = nullptr);
std::vector<PathTraceSmokeEmissiveTriangle> BuildSmokeEmissiveTriangleInventory(
    const std::vector<uint32_t>& materialIds,
    const std::vector<PathTraceSmokeMaterial>& materials,
    const std::vector<PathTraceSmokeVertex>& staticVertices,
    const std::vector<uint32_t>& staticIndexes,
    const std::vector<uint32_t>& staticTriangleClasses,
    const std::vector<uint32_t>& staticTriangleMaterialIndexes,
    const RtSmokeStaticBucketGeometryPack* staticBucketGeometryPack,
    const std::vector<uint32_t>* staticBucketTriangleMaterialIndexes,
    const RtPathTraceStaticBucketActivePublication* staticBucketPublication,
    const std::vector<PathTraceSmokeVertex>& dynamicVertices,
    const std::vector<uint32_t>& dynamicIndexes,
    const std::vector<uint32_t>& dynamicTriangleClasses,
    const std::vector<uint32_t>& dynamicTriangleMaterialIndexes,
    const std::vector<uint32_t>& dynamicTriangleInstanceIds,
    const std::vector<uint32_t>& dynamicTriangleIdentityIds,
    uint32_t emissiveMaterialFlag,
    uint32_t triangleClassMask,
    uint32_t skinnedSurfaceClassId,
    int maxRecords,
    RtSmokeEmissiveInventoryStats& stats);
std::vector<uint32_t> BuildSmokeWorldStaticEmissiveMaterialIds(const viewDef_t* viewDef);
void AppendSmokeWorldStaticEmissiveTriangleInventory(
    const viewDef_t* viewDef,
    const std::vector<uint32_t>& materialIds,
    const std::vector<PathTraceSmokeMaterial>& materials,
    uint32_t emissiveMaterialFlag,
    uint32_t staticSurfaceClassId,
    int maxRecords,
    std::vector<PathTraceSmokeEmissiveTriangle>& emissiveTriangles,
    RtSmokeEmissiveInventoryStats& stats);
