#pragma once

// Pure GEO-08 contract for interpreting hits from one BLAS per skinned
// surface. The shader-visible InstanceID remains a dense frame-local route
// table slot, while the record retains collision-checked canonical identity.
// PrimitiveIndex is always source-topology local; it is never the compacted
// merged-dynamic primitive index.

#include "PathTraceCanonicalGeometryIdentity.h"

#include <cstddef>
#include <cstdint>
#include <vector>

static constexpr std::uint32_t
    PT_SKINNED_HIT_ROUTE_INVALID_INDEX = UINT32_MAX;
static constexpr std::uint32_t
    PT_SKINNED_HIT_ROUTE_MAX_SHADER_INSTANCE_ID = 0x00ffffffu;

enum PtSkinnedHitRouteFlags : std::uint32_t
{
    PT_SKINNED_HIT_ROUTE_HAS_PREVIOUS = 1u << 0,
    PT_SKINNED_HIT_ROUTE_HAS_SOURCE_ONLY_PRIMITIVES = 1u << 1
};

enum class PtSkinnedHitRouteResult : std::uint32_t
{
    Accepted = 0,
    InvalidInstanceKey,
    InvalidMeshKey,
    InvalidSourceContract,
    InvalidOutputContract,
    InvalidPreviousContract,
    InvalidLegacyRange,
    InvalidSourceIndex,
    InvalidLegacyIndex,
    LegacyTopologyMismatch,
    InvalidFallbackMetadata,
    DuplicateInstanceKey,
    CanonicalHashCollision,
    ShaderInstanceIdOverflow,
    DispatchNotReady
};

// GEO-08 shader-facing skinned hit route. PrimitiveIndex() addresses the
// source-local index stream; these offsets never name the compact legacy
// merged-dynamic stream. The first record also carries table-wide counts so
// every ray pipeline can classify the disjoint InstanceID range without
// extending its constant-buffer ABI.
struct PathTraceSkinnedHitRouteGpuRecord
{
    std::uint32_t shaderInstanceId = 0;
    std::uint32_t sourceIndexOffset = 0;
    std::uint32_t outputVertexOffset = 0;
    std::uint32_t previousPositionOffset = UINT32_MAX;
    std::uint32_t triangleMetadataOffset = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t triangleCount = 0;
    std::uint32_t flags = 0;
    std::uint32_t instanceHashLo = 0;
    std::uint32_t instanceHashHi = 0;
    std::uint32_t sourceChecksumLo = 0;
    std::uint32_t sourceChecksumHi = 0;
    std::uint32_t sourceGpuIndexGenerationLo = 0;
    std::uint32_t sourceGpuIndexGenerationHi = 0;
    std::uint32_t outputStorageGenerationLo = 0;
    std::uint32_t outputStorageGenerationHi = 0;
    std::uint32_t routeCount = 0;
    std::uint32_t triangleMetadataCount = 0;
    std::uint32_t padding0 = 0;
};

struct PathTraceSkinnedHitRouteGpuTriangle
{
    std::uint32_t sourcePrimitiveIndex = 0;
    std::uint32_t legacyPrimitiveIndex = UINT32_MAX;
    std::uint32_t materialId = UINT32_MAX;
    std::uint32_t materialIndex = UINT32_MAX;
    std::uint32_t triangleClassAndFlags = 0;
    std::uint32_t canonicalPrimitiveHashLo = 0;
    std::uint32_t canonicalPrimitiveHashHi = 0;
    std::uint32_t emissiveIdentityHashLo = 0;
    std::uint32_t emissiveIdentityHashHi = 0;
};

struct PtSkinnedHitRouteLegacyView
{
    const std::uint32_t* indexes = nullptr;
    std::uint64_t indexCount = 0;
    const std::uint32_t* triangleClasses = nullptr;
    std::uint64_t triangleClassCount = 0;
    const std::uint32_t* triangleMaterialIds = nullptr;
    std::uint64_t triangleMaterialIdCount = 0;
    const std::uint32_t* triangleMaterialIndexes = nullptr;
    std::uint64_t triangleMaterialIndexCount = 0;
};

struct PtSkinnedHitRouteCandidate
{
    PtCanonicalInstanceKey instanceKey;
    PtCanonicalMeshKey meshKey;
    std::uint64_t sourceChecksum = 0;
    std::uint64_t sourceGpuIndexGeneration = 0;
    std::uint64_t sourceIndexOffsetBytes = 0;
    std::uint64_t sourceIndexCapacityBytes = 0;
    const std::uint32_t* sourceIndexes = nullptr;
    std::uint64_t sourceIndexCount = 0;

    std::uint64_t outputStorageGeneration = 0;
    std::uint64_t outputVertexOffsetBytes = 0;
    std::uint64_t outputVertexCount = 0;
    std::uint64_t outputCapacityBytes = 0;

    std::uint64_t previousPositionOffset = 0;
    std::uint64_t previousPositionCount = 0;
    bool previousValid = false;

    std::uint64_t legacyVertexOffset = 0;
    std::uint64_t legacyVertexCount = 0;
    std::uint64_t legacyIndexOffset = 0;
    std::uint64_t legacyIndexCount = 0;
    std::uint64_t legacyTriangleOffset = 0;
    std::uint64_t legacyTriangleCount = 0;

    std::uint32_t fallbackMaterialId =
        PT_SKINNED_HIT_ROUTE_INVALID_INDEX;
    std::uint32_t fallbackMaterialIndex =
        PT_SKINNED_HIT_ROUTE_INVALID_INDEX;
    std::uint32_t fallbackTriangleClassAndFlags = 0;
    bool dispatchReady = false;
};

struct PtSkinnedHitRouteRecord
{
    PtCanonicalInstanceKey instanceKey;
    PtCanonicalMeshKey meshKey;
    std::uint64_t instanceHash = 0;
    std::uint64_t sourceChecksum = 0;
    std::uint64_t sourceGpuIndexGeneration = 0;
    std::uint64_t outputStorageGeneration = 0;
    std::uint32_t shaderInstanceId = 0;
    std::uint32_t sourceIndexOffset = 0;
    std::uint32_t outputVertexOffset = 0;
    std::uint32_t previousPositionOffset =
        PT_SKINNED_HIT_ROUTE_INVALID_INDEX;
    std::uint32_t triangleMetadataOffset = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t triangleCount = 0;
    std::uint32_t flags = 0;
};

struct PtSkinnedHitRouteTriangle
{
    std::uint32_t sourcePrimitiveIndex = 0;
    std::uint32_t legacyPrimitiveIndex =
        PT_SKINNED_HIT_ROUTE_INVALID_INDEX;
    std::uint32_t materialId =
        PT_SKINNED_HIT_ROUTE_INVALID_INDEX;
    std::uint32_t materialIndex =
        PT_SKINNED_HIT_ROUTE_INVALID_INDEX;
    std::uint32_t triangleClassAndFlags = 0;
    std::uint64_t canonicalPrimitiveHash = 0;
    std::uint64_t emissiveIdentityHash = 0;
};

struct PtSkinnedHitRouteStats
{
    std::uint64_t candidates = 0;
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;
    std::uint64_t sourceTriangles = 0;
    std::uint64_t legacyTriangles = 0;
    std::uint64_t mappedLegacyTriangles = 0;
    std::uint64_t sourceOnlyTriangles = 0;
    std::uint64_t motionReady = 0;
    std::uint64_t motionMissing = 0;
    std::uint64_t duplicateInstances = 0;
    std::uint64_t canonicalHashCollisions = 0;
    std::uint64_t emissiveIdentityCollisions = 0;
};

struct PtSkinnedHitRouteBuild
{
    std::vector<PtSkinnedHitRouteRecord> records;
    std::vector<PtSkinnedHitRouteTriangle> triangles;
    std::vector<PtSkinnedHitRouteResult> results;
    PtSkinnedHitRouteStats stats;
};

struct PtSkinnedHitRouteGpuUpload
{
    std::vector<PathTraceSkinnedHitRouteGpuRecord> records;
    std::vector<PathTraceSkinnedHitRouteGpuTriangle> triangles;
    std::uint64_t signature = 0;
};

PtSkinnedHitRouteBuild PtBuildSkinnedHitRoutes(
    const std::vector<PtSkinnedHitRouteCandidate>& candidates,
    const PtSkinnedHitRouteLegacyView& legacy,
    std::uint64_t firstShaderInstanceId);

PtSkinnedHitRouteGpuUpload PtBuildSkinnedHitRouteGpuUpload(
    const PtSkinnedHitRouteBuild& build,
    std::uint32_t emptyFirstShaderInstanceId);

const char* PtSkinnedHitRouteResultName(
    PtSkinnedHitRouteResult result);
