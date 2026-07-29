#pragma once

// CPU-only acceleration planning for the PT smoke scene.
//
// These structs are intentionally plain data. They may be produced by a worker
// from an immutable snapshot, but they never contain NVRHI handles, command
// lists, binding resources, or live renderer/game pointers.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

struct RtSmokePlanVec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct RtSmokePlanGeometryRange
{
    int vertexOffset = 0;
    int vertexCount = 0;
    int indexOffset = 0;
    int indexCount = 0;
    int triangleOffset = 0;
    int triangleCount = 0;
};

struct RtSmokePlanStaticBlasSignature
{
    uint64_t hash = 0;
    int vertexCount = 0;
    int indexCount = 0;
    int triangleCount = 0;
};

struct RtSmokePlanStaticBlasSignatureDesc
{
    const void* vertices = nullptr;
    size_t vertexStride = 0;
    int totalVertexCount = 0;
    const uint32_t* indexes = nullptr;
    int totalIndexCount = 0;
    const uint32_t* triangleClasses = nullptr;
    const uint32_t* triangleMaterials = nullptr;
    int totalTriangleCount = 0;
    RtSmokePlanGeometryRange staticRange;
    RtSmokePlanVec3 sceneOrigin;
};

struct RtSmokeStaticBlasSignatureSnapshot
{
    std::vector<uint8_t> vertexBytes;
    size_t vertexStride = 0;
    int totalVertexCount = 0;
    std::vector<uint32_t> indexes;
    std::vector<uint32_t> triangleClasses;
    std::vector<uint32_t> triangleMaterials;
    RtSmokePlanGeometryRange staticRange;
    RtSmokePlanVec3 sceneOrigin;
};

struct RtSmokePlanStaticCacheInput
{
    bool hasStaticBlas = false;
    bool cacheValid = false;
    bool cacheResourcesReady = false;
    bool staticCacheChanged = false;
    uint64_t previousSignatureHash = 0;
};

struct RtSmokePlanBlasCreate
{
    bool enabled = false;
    bool cacheHit = false;
    int vertexCount = 0;
    int indexCount = 0;
    const char* debugName = nullptr;
};

struct RtSmokeAccelerationPlanInput
{
    RtSmokePlanStaticBlasSignatureDesc staticSignature;
    RtSmokePlanStaticCacheInput staticCache;
    int staticVertexCount = 0;
    int staticIndexCount = 0;
    int dynamicVertexCount = 0;
    int dynamicIndexCount = 0;
};

struct RtSmokeAccelerationPlanSnapshot
{
    RtSmokeStaticBlasSignatureSnapshot staticSignature;
    RtSmokePlanStaticCacheInput staticCache;
    int staticVertexCount = 0;
    int staticIndexCount = 0;
    int dynamicVertexCount = 0;
    int dynamicIndexCount = 0;
};

struct RtSmokeAccelerationPlan
{
    RtSmokePlanStaticBlasSignature staticSignature;
    RtSmokePlanBlasCreate staticBlas;
    RtSmokePlanBlasCreate dynamicBlas;
    bool staticSignatureReused = false;
    bool staticCacheHit = false;
    bool hasStaticBlas = false;
    bool hasDynamicBlas = false;
};

struct RtSmokeAccelerationPlanResult
{
    RtSmokeAccelerationPlan plan;
    bool valid = false;
};

struct RtSmokeAccelerationPlanTimedResult
{
    RtSmokeAccelerationPlanResult result;
    double workerExecutionMs = 0.0;
};

enum RtSmokeAsWorkKind : uint32_t
{
    RT_SMOKE_AS_WORK_NEW_BUILD = 0,
    RT_SMOKE_AS_WORK_UPDATE = 1,
    RT_SMOKE_AS_WORK_PERIODIC_REBUILD = 2,
    RT_SMOKE_AS_WORK_COMPACTION_COPY = 3,
    RT_SMOKE_AS_WORK_KIND_COUNT = 4
};

enum RtSmokeAsWorkPriority : uint32_t
{
    RT_SMOKE_AS_PRIORITY_CRITICAL_ACTIVE = 0,
    RT_SMOKE_AS_PRIORITY_ACTIVE = 1,
    RT_SMOKE_AS_PRIORITY_BACKGROUND = 2
};

enum RtSmokeAsDeferralReason : uint32_t
{
    RT_SMOKE_AS_DEFER_NONE = 0,
    RT_SMOKE_AS_DEFER_OPERATION_BUDGET = 1,
    RT_SMOKE_AS_DEFER_RESULT_BYTE_BUDGET = 2,
    RT_SMOKE_AS_DEFER_RESULT_BYTES_UNKNOWN = 3
};

struct RtSmokeAsAdmissionBudget
{
    // Zero means unlimited. Scratch is deliberately absent: the active NVRHI
    // interface does not expose an authoritative per-build scratch size.
    int maxOperations = 0;
    uint64_t maxResultBytes = 0;
};

struct RtSmokeAsAdmissionRequest
{
    RtSmokeAsWorkKind kind = RT_SMOKE_AS_WORK_NEW_BUILD;
    RtSmokeAsWorkPriority priority = RT_SMOKE_AS_PRIORITY_BACKGROUND;
    uint64_t resultBytes = 0;
    uint64_t deferredAge = 0;
    bool resultBytesKnown = false;
};

struct RtSmokeAsAdmissionDecision
{
    bool admitted = false;
    RtSmokeAsDeferralReason deferralReason = RT_SMOKE_AS_DEFER_NONE;
    int admittedOrder = -1;
};

struct RtSmokeAsAdmissionStats
{
    int requestedOperations = 0;
    int admittedOperations = 0;
    int deferredOperations = 0;
    uint64_t admittedResultBytes = 0;
    uint64_t maxDeferredAge = 0;
    int requestedByKind[RT_SMOKE_AS_WORK_KIND_COUNT] = {};
    int admittedByKind[RT_SMOKE_AS_WORK_KIND_COUNT] = {};
    int deferredByKind[RT_SMOKE_AS_WORK_KIND_COUNT] = {};
    int deferredByReason[RT_SMOKE_AS_DEFER_RESULT_BYTES_UNKNOWN + 1] = {};
};

struct RtSmokeAsAdmissionPlan
{
    std::vector<RtSmokeAsAdmissionDecision> decisions;
    RtSmokeAsAdmissionStats stats;
};

enum RtSmokePlanTlasInstanceKind : uint32_t
{
    RT_SMOKE_PLAN_TLAS_STATIC_BLAS = 0,
    RT_SMOKE_PLAN_TLAS_DYNAMIC_BLAS = 1,
    RT_SMOKE_PLAN_TLAS_RIGID_BLAS = 2,
    RT_SMOKE_PLAN_TLAS_STATIC_BUCKET_BLAS = 3
};

enum RtSmokeStaticActiveReasonFlags : uint32_t
{
    RT_SMOKE_STATIC_ACTIVE_VISIBLE = 1u << 0,
    RT_SMOKE_STATIC_ACTIVE_SELECTED_AREA = 1u << 1,
    RT_SMOKE_STATIC_ACTIVE_RESIDENCY = 1u << 2,
    RT_SMOKE_STATIC_ACTIVE_EMISSIVE_PLACEHOLDER = 1u << 3,
    RT_SMOKE_STATIC_ACTIVE_FORCE_INCLUDE = 1u << 4
};

struct RtSmokePlanTlasInstance
{
    RtSmokePlanTlasInstanceKind kind = RT_SMOKE_PLAN_TLAS_STATIC_BLAS;
    uint32_t instanceId = 0;
    uint32_t instanceMask = 0;
    uint32_t hitGroupContribution = 0;
    uint32_t flags = 0;
    uint64_t meshHash = 0;
    uint64_t sourceInstanceId = 0;
    uint32_t materialId = 0;
    uint32_t routeRecordIndex = std::numeric_limits<uint32_t>::max();
    uint32_t canonicalBlasRecordIndex =
        std::numeric_limits<uint32_t>::max();
    uint64_t canonicalMeshHash = 0;
    bool sourceSeenThisFrame = true;
    bool hasPreviousTransform = false;
    bool transformContinuous = false;
    float transform[16] = {};
    float previousTransform[16] = {};
};

struct RtSmokeBaseTlasPlan
{
    RtSmokePlanTlasInstance instances[2];
    int instanceCount = 0;
};

struct RtSmokeAccelerationSubmitPlanInput
{
    bool hasStaticBlas = false;
    bool hasDynamicBlas = false;
    bool staticBlasCacheHit = false;
    bool includeStaticBlasInTlas = true;
    bool hasExtraTlasInstances = false;
};

struct RtSmokeAccelerationSubmitPlan
{
    RtSmokeBaseTlasPlan baseTlasPlan;
    bool buildStaticBlas = false;
    bool buildDynamicBlas = false;
    bool submitTlas = false;
};

const int RT_SMOKE_STATIC_BUCKET_FALLBACK_AREA = -1;
static constexpr uint32_t
    RT_SMOKE_STATIC_BUCKET_INSTANCE_ID_NAMESPACE =
        0x00800000u;
static constexpr uint32_t
    RT_SMOKE_STATIC_BUCKET_TRIANGLE_OFFSET_MASK =
        0x007fffffu;
static constexpr uint32_t
    RT_SMOKE_STATIC_BUCKET_SURFACE_RECORD_OFFSET_MASK =
        0x007fffffu;
static constexpr uint32_t
    RT_SMOKE_SHADER_INSTANCE_ID_MASK =
        0x00ffffffu;
static_assert(
    (RT_SMOKE_STATIC_BUCKET_INSTANCE_ID_NAMESPACE |
        RT_SMOKE_STATIC_BUCKET_TRIANGLE_OFFSET_MASK) ==
        RT_SMOKE_SHADER_INSTANCE_ID_MASK,
    "GEO-10 bucket InstanceID namespace must fill the upper half of the 24-bit shader ID");
static_assert(
    RT_SMOKE_STATIC_BUCKET_SURFACE_RECORD_OFFSET_MASK ==
        RT_SMOKE_STATIC_BUCKET_TRIANGLE_OFFSET_MASK,
    "GEO-10 transition keeps the existing 23-bit bucket InstanceID payload width");

struct RtSmokePortalAreaEdge
{
    int areaA = -1;
    int areaB = -1;
};

struct RtSmokePortalVisibilityMaskPlan
{
    std::vector<bool> selectedAreas;
    int frontendVisibleAreas = 0;
    int selectedAreaCount = 0;
    int expansionSteps = 0;
    int validEdges = 0;
    int invalidEdges = 0;
    bool valid = false;
    bool forcedFullMap = false;
};

RtSmokePortalVisibilityMaskPlan BuildSmokePortalVisibilityMaskPlan(
    int areaCount,
    const std::vector<bool>& frontendVisibleAreas,
    const std::vector<RtSmokePortalAreaEdge>& portalEdges,
    int expansionSteps,
    bool forceFullMap);
int ResolveSmokeStaticBucketPortalSteps(
    int primaryPortalSteps,
    bool secondaryOpticalActive,
    int reflectionPortalSteps);

struct RtSmokeStaticBucketAssignmentSurface
{
    uint64_t surfaceKey = 0;
    uint32_t sourceRecordIndex = std::numeric_limits<uint32_t>::max();
    int portalArea = RT_SMOKE_STATIC_BUCKET_FALLBACK_AREA;
    RtSmokePlanGeometryRange range;
    bool valid = false;
    bool active = false;
};

struct RtSmokeStaticBucketAssignmentPlanDesc
{
    const RtSmokeStaticBucketAssignmentSurface* surfaces = nullptr;
    int surfaceCount = 0;
    uint64_t worldGeneration = 0;
    uint64_t sourceGeneration = 0;
    uint64_t storageGeneration = 0;
    int portalAreaCount = 0;
    int maxVerticesPerBucket = 0;
    int maxIndexesPerBucket = 0;
    int maxTrianglesPerBucket = 0;
};

struct RtSmokeStaticBucketAssignment
{
    uint64_t surfaceKey = 0;
    uint32_t sourceRecordIndex = std::numeric_limits<uint32_t>::max();
    uint32_t bucketIndex = std::numeric_limits<uint32_t>::max();
    uint32_t localPrimitiveOffset = 0;
    RtSmokePlanGeometryRange sourceRange;
};

struct RtSmokeStaticBucketAssignmentBucket
{
    uint64_t bucketKey = 0;
    uint64_t worldGeneration = 0;
    uint64_t sourceGeneration = 0;
    uint64_t storageGeneration = 0;
    int portalArea = RT_SMOKE_STATIC_BUCKET_FALLBACK_AREA;
    uint32_t splitIndex = 0;
    uint32_t firstAssignment = 0;
    uint32_t assignmentCount = 0;
    int vertexCount = 0;
    int indexCount = 0;
    int triangleCount = 0;
    bool active = false;
    bool oversized = false;
};

struct RtSmokeStaticBucketAssignmentStats
{
    int inputSurfaces = 0;
    int assignedSurfaces = 0;
    int assignedPrimitives = 0;
    int duplicateSurfaces = 0;
    int unassignedAreaSurfaces = 0;
    int invalidAreaSurfaces = 0;
    int invalidRangeSurfaces = 0;
    int oversizedSurfaces = 0;
    int buckets = 0;
    int activeBuckets = 0;
    int fallbackBuckets = 0;
    int splitBuckets = 0;
    int bucketKeyCollisions = 0;
};

struct RtSmokeStaticBucketAssignmentPlan
{
    std::vector<RtSmokeStaticBucketAssignmentBucket> buckets;
    std::vector<RtSmokeStaticBucketAssignment> assignments;
    RtSmokeStaticBucketAssignmentStats stats;
    uint64_t planSignature = 0;
    bool exactCoverage = false;
};

struct RtSmokeStaticBucketTriangleIdentity
{
    uint64_t surfaceKey = 0;
    uint32_t sourceRecordIndex = std::numeric_limits<uint32_t>::max();
    uint32_t sourcePrimitiveIndex = 0;
};

static constexpr uint32_t
    RT_SMOKE_STATIC_BUCKET_SURFACE_RECORD_VALID = 1u;
static constexpr uint32_t
    RT_SMOKE_STATIC_BUCKET_SURFACE_RECORD_SOURCE_TRIANGLE_SHIFT = 1u;
static constexpr uint32_t
    RT_SMOKE_STATIC_BUCKET_SURFACE_RECORD_VALID_MASK =
        (1u << RT_SMOKE_STATIC_BUCKET_SURFACE_RECORD_SOURCE_TRIANGLE_SHIFT) - 1u;

struct RtSmokeStaticBucketSurfaceRecord
{
    uint32_t indexOffset = 0;
    uint32_t triangleOffset = 0;
    uint32_t triangleCount = 0;
    uint32_t flags = 0;
};

static_assert(
    sizeof(RtSmokeStaticBucketSurfaceRecord) == 16,
    "GEO-10 surface record must remain four uint32 words");

struct RtSmokeStaticBucketPackedRecord
{
    uint64_t bucketKey = 0;
    int portalArea = RT_SMOKE_STATIC_BUCKET_FALLBACK_AREA;
    uint32_t splitIndex = 0;
    uint32_t firstAssignment = 0;
    uint32_t assignmentCount = 0;
    uint32_t firstSurfaceRecord = 0;
    uint32_t surfaceRecordCount = 0;
    RtSmokePlanGeometryRange range;
    uint64_t vertexByteOffset = 0;
    uint64_t vertexByteSize = 0;
    uint64_t indexByteOffset = 0;
    uint64_t indexByteSize = 0;
    uint64_t triangleMetadataByteOffset = 0;
    uint64_t triangleMetadataByteSize = 0;
    bool active = false;
    bool oversized = false;
};

struct RtSmokeStaticBucketGeometryPackDesc
{
    const RtSmokeStaticBucketAssignmentPlan* assignmentPlan = nullptr;
    const void* vertices = nullptr;
    size_t vertexStride = 0;
    int totalVertexCount = 0;
    const uint32_t* indexes = nullptr;
    int totalIndexCount = 0;
    const uint32_t* triangleClasses = nullptr;
    const uint32_t* triangleMaterials = nullptr;
    int totalTriangleCount = 0;
};

struct RtSmokeStaticBucketGeometryPackStats
{
    int inputBuckets = 0;
    int inputAssignments = 0;
    int packedBuckets = 0;
    int packedSurfaces = 0;
    int packedVertices = 0;
    int packedIndexes = 0;
    int packedTriangles = 0;
    int invalidBucketRanges = 0;
    int invalidAssignments = 0;
    int sourceRangeMismatches = 0;
    int indexRangeErrors = 0;
    int localPrimitiveOffsetErrors = 0;
    int addressContractErrors = 0;
    int surfaceRecordErrors = 0;
    int surfaceAddressContractErrors = 0;
    int classMetadataLayoutErrors = 0;
    int countMismatches = 0;
};

struct RtSmokeStaticBucketGeometryPack
{
    std::vector<RtSmokeStaticBucketPackedRecord> buckets;
    std::vector<uint8_t> vertexBytes;
    std::vector<uint32_t> indexes;
    std::vector<uint32_t> triangleClasses;
    std::vector<uint32_t> staticClassMetadataWords;
    std::vector<uint32_t> triangleMaterials;
    std::vector<RtSmokeStaticBucketSurfaceRecord> surfaceRecords;
    std::vector<RtSmokeStaticBucketTriangleIdentity> triangleIdentities;
    uint32_t surfaceRecordWordOffset = 0;
    RtSmokeStaticBucketGeometryPackStats stats;
    uint64_t contentSignature = 0;
    bool exact = false;
};

struct RtSmokeStaticBucketCanonicalTriangleAddress
{
    uint32_t instanceId = 0;
    uint32_t sourceTriangleIndex = 0;
};

struct RtSmokeStaticBucketCanonicalAddressStats
{
    int triangles = 0;
    int mappedTriangles = 0;
    int invalidSurfaceRecords = 0;
    int invalidTriangleRanges = 0;
    int duplicateTriangleMappings = 0;
    int instanceIdOverflows = 0;
    int sourceTriangleOverflows = 0;
    int missingTriangles = 0;
};

struct RtSmokeStaticBucketCanonicalAddressPlan
{
    std::vector<RtSmokeStaticBucketCanonicalTriangleAddress> addresses;
    std::vector<uint8_t> mapped;
    RtSmokeStaticBucketCanonicalAddressStats stats;
    bool exact = false;
};

RtSmokeStaticBucketCanonicalAddressPlan
BuildSmokeStaticBucketCanonicalAddressPlan(
    const RtSmokeStaticBucketGeometryPack& geometryPack);

struct RtSmokeStaticBucketMonolithicSurfaceBinding
{
    uint64_t surfaceKey = 0;
    uint32_t triangleOffset = 0;
    uint32_t triangleCount = 0;
};

struct RtSmokeStaticBucketMonolithicPrimitiveRemapStats
{
    int activeBuckets = 0;
    int activeSurfaces = 0;
    int activeTriangles = 0;
    int matchedSurfaces = 0;
    int missingSurfaces = 0;
    int duplicateBindings = 0;
    int mappedTriangles = 0;
    int missingTriangles = 0;
    int invalidTriangleIdentities = 0;
};

struct RtSmokeStaticBucketMonolithicPrimitiveRemap
{
    std::vector<uint32_t> primitiveIndexes;
    std::vector<RtSmokeStaticBucketMonolithicSurfaceBinding>
        activeMonolithicSurfaces;
    RtSmokeStaticBucketMonolithicPrimitiveRemapStats stats;
    bool exact = false;
};

struct RtSmokeStaticBucketMonolithicStateOverlayStats
{
    int mappedTriangles = 0;
    int invalidMonolithicRanges = 0;
    int classMismatches = 0;
    int stageStateMismatches = 0;
    int nonStageClassMismatches = 0;
    int materialIndexMismatches = 0;
};

struct RtSmokeStaticBucketMonolithicStateOverlay
{
    std::vector<uint32_t> triangleClasses;
    std::vector<uint32_t> triangleMaterialIndexes;
    RtSmokeStaticBucketMonolithicStateOverlayStats stats;
    bool exact = false;
};

RtSmokeStaticBucketMonolithicPrimitiveRemap
BuildSmokeStaticBucketMonolithicPrimitiveRemap(
    const RtSmokeStaticBucketGeometryPack& geometryPack,
    const std::vector<uint8_t>& activeBucketMask,
    const std::vector<RtSmokeStaticBucketMonolithicSurfaceBinding>&
        monolithicSurfaces);
RtSmokeStaticBucketMonolithicStateOverlay
BuildSmokeStaticBucketMonolithicStateOverlay(
    const RtSmokeStaticBucketGeometryPack& geometryPack,
    const std::vector<uint32_t>& bucketTriangleMaterialIndexes,
    const RtSmokeStaticBucketMonolithicPrimitiveRemap& primitiveRemap,
    const std::vector<uint32_t>& monolithicTriangleClasses,
    const std::vector<uint32_t>&
        monolithicTriangleMaterialIndexes,
    uint32_t emissiveStageOffMask);
bool IsSmokeStaticBucketAuditReady(
    bool requested,
    bool portalPublicationValid,
    bool fullResidentRequired,
    bool activePublicationValid);

struct RtSmokeStaticBucketInstanceAddressPlan
{
    uint32_t instanceId = 0;
    uint32_t triangleOffset = 0;
    uint32_t triangleCount = 0;
    bool rangeValid = false;
    bool indexAddressCompatible = false;
    bool instanceIdEncodable = false;
    bool valid = false;
};

struct RtSmokeStaticBucketSurfaceAddressPlan
{
    uint32_t instanceId = 0;
    uint32_t firstSurfaceRecord = 0;
    uint32_t surfaceRecordCount = 0;
    bool rangeValid = false;
    bool instanceIdEncodable = false;
    bool valid = false;
};

struct RtSmokeStaticBucketBlasGeometryRange
{
    uint64_t indexByteOffset = 0;
    uint32_t indexCount = 0;
    uint32_t triangleOffset = 0;
    uint32_t triangleCount = 0;
};

struct RtSmokeStaticBucketBlasGeometryPlan
{
    std::vector<RtSmokeStaticBucketBlasGeometryRange> geometries;
    int invalidSurfaceRecords = 0;
    bool exact = false;
};

struct RtSmokeStaticBucketResolvedGeometryAddress
{
    uint32_t surfaceRecordIndex = 0;
    uint32_t triangleIndex = 0;
    uint32_t sourceTriangleIndex = 0;
    uint32_t indexOffset = 0;
    uint32_t triangleCount = 0;
    uint32_t vertexIndexes[3] = {};
    bool valid = false;
};

struct RtSmokeStaticBucketResidentPackCacheInput
{
    uint64_t assignmentPlanSignature = 0;
    uint64_t geometryGeneration = 0;
    uint64_t materialGeneration = 0;
    uint64_t cachedAssignmentPlanSignature = 0;
    uint64_t cachedGeometryGeneration = 0;
    uint64_t cachedMaterialGeneration = 0;
    bool assignmentExact = false;
    bool cachedPackExact = false;
    bool cacheValid = false;
};

struct RtSmokeStaticBucketResidentPackCachePlan
{
    bool reuse = false;
    bool rebuild = true;
};

struct RtSmokeStaticBucketMaterialIndexCacheInput
{
    uint64_t residentPackSignature = 0;
    uint64_t materialBindingSignature = 0;
    int triangleCount = 0;
    int bucketCount = 0;
    uint64_t cachedResidentPackSignature = 0;
    uint64_t cachedMaterialBindingSignature = 0;
    int cachedTriangleCount = 0;
    int cachedBucketCount = 0;
    bool residentPackExact = false;
    bool cacheValid = false;
};

struct RtSmokeStaticBucketMaterialIndexCachePlan
{
    bool reuse = false;
    bool rebuild = true;
};

struct RtSmokeStaticBucketPublicationEpochInput
{
    uint64_t expectedGeneration = 0;
    uint64_t tlasGeneration = 0;
    uint64_t routeGeneration = 0;
    int residentBuckets = 0;
    int activeBuckets = 0;
    int tlasInstances = 0;
    int routeRecords = 0;
    bool activeSetExact = false;
};

struct RtSmokeStaticBucketPublicationEpochPlan
{
    bool generationValid = false;
    bool generationsMatch = false;
    bool countsMatch = false;
    bool accepted = false;
    bool mixedEpochRejected = false;
};

struct RtSmokeStaticBucketCutoverInput
{
    int residentBuckets = 0;
    int activeBuckets = 0;
    int readyBuckets = 0;
    int tlasInstances = 0;
    int routeRecords = 0;
    bool requested = false;
    bool consumerSupported = false;
    bool publicationValid = false;
    bool routeUploaded = false;
};

struct RtSmokeStaticBucketCutoverPlan
{
    bool allResidentReady = false;
    bool publicationExact = false;
    bool accepted = false;
};

enum RtSmokeStaticBucketRouteMode : int
{
    RT_SMOKE_STATIC_BUCKET_ROUTE_DISABLED = 0,
    RT_SMOKE_STATIC_BUCKET_ROUTE_PRODUCTION = 1,
    RT_SMOKE_STATIC_BUCKET_ROUTE_PRIMARY_OPAQUE_PROBE = 2
};

struct RtSmokeStaticTlasBucketObservation
{
    uint64_t bucketKey = 0;
    uint32_t activeReasonFlags = 0;
    bool resident = false;
    bool active = false;
    bool hasBlas = false;
    uint32_t routeRecordIndex = std::numeric_limits<uint32_t>::max();
    int residentVertexOffset = 0;
    int residentIndexOffset = 0;
    int residentTriangleOffset = 0;
    int residentSurfaceCount = 0;
    int residentVertexCount = 0;
    int residentIndexCount = 0;
    int residentTriangleCount = 0;
    int activeSurfaceCount = 0;
    int activeVertexCount = 0;
    int activeIndexCount = 0;
    int activeTriangleCount = 0;
};

struct RtSmokeStaticTlasBucketObservationInput
{
    uint64_t bucketKey = 0;
    uint32_t routeRecordIndex = std::numeric_limits<uint32_t>::max();
    uint32_t activeReasonFlags = 0;
    int vertexOffset = 0;
    int indexOffset = 0;
    int triangleOffset = 0;
    int vertexCount = 0;
    int indexCount = 0;
    int triangleCount = 0;
    bool valid = false;
    bool seenThisFrame = false;
    bool hasBlas = false;
};

struct RtSmokeStaticTlasActiveSetPlanDesc
{
    const RtSmokeStaticTlasBucketObservation* buckets = nullptr;
    int bucketCount = 0;
    bool monolithicStaticBlas = true;
    bool hasStaticBlas = false;
    uint32_t firstInstanceId = 0;
    uint32_t instanceMask = 0x01;
};

struct RtSmokeStaticTlasActiveSetPlan
{
    std::vector<RtSmokePlanTlasInstance> instances;
    uint64_t activeSetSignature = 0;
    uint64_t residentSetSignature = 0;
    uint64_t tlasInstanceSignature = 0;
    int residentBuckets = 0;
    int activeBuckets = 0;
    int inactiveResidentBuckets = 0;
    int emittedInstances = 0;
    int residentSurfaceCount = 0;
    int residentVertexCount = 0;
    int residentIndexCount = 0;
    int residentTriangleCount = 0;
    int activeSurfaceCount = 0;
    int activeVertexCount = 0;
    int activeIndexCount = 0;
    int activeTriangleCount = 0;
    bool monolithicStaticBlas = true;
    bool inactiveResidentGeometryIncluded = false;
    bool requiresBucketedStaticBlas = false;
};

struct RtSmokeStaticBucketBlasRecord
{
    uint64_t bucketKey = 0;
    uint32_t routeRecordIndex = std::numeric_limits<uint32_t>::max();
    uint32_t activeReasonFlags = 0;
    RtSmokePlanGeometryRange range;
    bool active = false;
};

struct RtSmokeStaticBucketBlasPlanDesc
{
    const RtSmokeStaticTlasBucketObservation* buckets = nullptr;
    int bucketCount = 0;
    bool activeOnly = true;
    int maxRecords = 0;
};

struct RtSmokeStaticBucketBlasPlan
{
    std::vector<RtSmokeStaticBucketBlasRecord> records;
    uint64_t planSignature = 0;
    int residentBuckets = 0;
    int activeBuckets = 0;
    int emittedRecords = 0;
    int skippedInactive = 0;
    int skippedInvalid = 0;
    bool overflow = false;
};

struct RtSmokeStaticBucketTraversalCompatibilityInput
{
    const RtSmokeStaticBucketBlasRecord* records = nullptr;
    int recordCount = 0;
    int totalVertexCount = 0;
    int totalIndexCount = 0;
    int totalTriangleCount = 0;
    bool shaderSupportsStaticBucketRoutes = false;
};

struct RtSmokeStaticBucketTraversalCompatibility
{
    int recordCount = 0;
    int nonZeroOffsetRecords = 0;
    bool exactMonolithicRecord = false;
    bool currentStaticShaderCompatible = true;
    bool requiresShaderRouteMetadata = false;
};

struct RtSmokeRouteInstanceNamespacePlanInput
{
    int staticRouteRecordCount = 0;
    int rigidRouteRecordCount = 0;
    uint32_t firstRouteInstanceId = 2;
    bool enableStaticRoutes = false;
    bool shaderSupportsStaticBucketRoutes = false;
};

struct RtSmokeRouteInstanceNamespacePlan
{
    uint32_t staticFirstInstanceId = 0;
    uint32_t rigidFirstInstanceId = 2;
    int staticRouteInstanceCount = 0;
    int rigidRouteInstanceCount = 0;
    bool staticRoutesEnabled = false;
    bool staticRoutesRequireShaderSupport = false;
    bool staticRoutesBlocked = false;
    bool rigidRouteBaseShifted = false;
};

struct RtSmokeStaticRouteTableRecord
{
    uint64_t bucketKey = 0;
    uint32_t instanceId = 0;
    uint32_t routeRecordIndex = std::numeric_limits<uint32_t>::max();
    uint32_t activeReasonFlags = 0;
    RtSmokePlanGeometryRange range;
};

struct RtSmokeStaticRouteTablePlanInput
{
    const RtSmokeStaticBucketBlasRecord* records = nullptr;
    int recordCount = 0;
    int maxRecords = 0;
    RtSmokeRouteInstanceNamespacePlan routeNamespace;
};

struct RtSmokeStaticRouteTablePlan
{
    std::vector<RtSmokeStaticRouteTableRecord> records;
    uint64_t tableSignature = 0;
    int inputRecords = 0;
    int emittedRecords = 0;
    int skippedDisabled = 0;
    int skippedInvalid = 0;
    bool blocked = false;
    bool overflow = false;
};

struct RtSmokeStaticBucketBlasBuildPlanInput
{
    bool submitBuilds = false;
    bool forceRebuild = false;
    bool hasBlas = false;
    bool uploadRequired = false;
    bool blasInputsCompatible = false;
    bool signatureValid = false;
    uint64_t previousBlasInputSignature = 0;
    uint64_t currentBlasInputSignature = 0;
};

struct RtSmokeStaticBucketBlasBuildPlan
{
    bool createBlas = false;
    bool submitBuild = false;
    bool skipBuild = false;
    bool signatureChanged = false;
};

struct RtSmokeStaticBucketBlasBuildObservation
{
    uint64_t bucketKey = 0;
    bool hasBlas = false;
    bool uploadRequired = false;
    bool blasInputsCompatible = false;
    bool signatureValid = false;
    uint64_t previousBlasInputSignature = 0;
    uint64_t currentBlasInputSignature = 0;
};

struct RtSmokeStaticBucketBlasBuildBatchPlanInput
{
    const RtSmokeStaticBucketBlasBuildObservation* observations = nullptr;
    int observationCount = 0;
    bool submitBuilds = false;
    bool forceRebuild = false;
    int maxRecords = 0;
};

struct RtSmokeStaticBucketBlasBuildBatchRecord
{
    uint64_t bucketKey = 0;
    RtSmokeStaticBucketBlasBuildPlan buildPlan;
};

struct RtSmokeStaticBucketBlasBuildBatchPlan
{
    std::vector<RtSmokeStaticBucketBlasBuildBatchRecord> records;
    uint64_t planSignature = 0;
    int inputRecords = 0;
    int emittedRecords = 0;
    int createBlasRecords = 0;
    int submitBuildRecords = 0;
    int skippedBuildRecords = 0;
    int signatureChangedRecords = 0;
    int uploadRequiredRecords = 0;
    int incompatibleRecords = 0;
    int missingBlasRecords = 0;
    bool overflow = false;
};

struct RtSmokeStaticBvhBucketSignatureInput
{
    RtSmokeStaticTlasBucketObservation bucket;
    uint64_t geometryContentSignature = 0;
    uint64_t materialGeneration = 0;
};

struct RtSmokeStaticBvhBucketSignature
{
    uint64_t bucketKey = 0;
    uint64_t residentSignature = 0;
    uint64_t activeSignature = 0;
    uint64_t geometryInputSignature = 0;
    uint64_t blasInputSignature = 0;
    bool resident = false;
    bool active = false;
};

struct RtSmokeStaticBucketBlasCacheState
{
    uint64_t bucketKey = 0;
    uint64_t blasInputSignature = 0;
    bool hasBlas = false;
    bool blasInputsCompatible = false;
};

struct RtSmokeStaticBucketBlasBuildObservationPlanInput
{
    const RtSmokeStaticBvhBucketSignature* currentBuckets = nullptr;
    int currentBucketCount = 0;
    const RtSmokeStaticBucketBlasCacheState* previousBuckets = nullptr;
    int previousBucketCount = 0;
    int maxRecords = 0;
};

struct RtSmokeStaticBucketBlasBuildObservationPlan
{
    std::vector<RtSmokeStaticBucketBlasBuildObservation> observations;
    uint64_t planSignature = 0;
    int inputBuckets = 0;
    int emittedObservations = 0;
    int cacheHits = 0;
    int cacheMisses = 0;
    int signatureChanged = 0;
    int uploadRequired = 0;
    int skippedInactive = 0;
    bool overflow = false;
};

struct RtSmokeStaticBucketWorkPlanInput
{
    const RtSmokeStaticTlasBucketObservation* buckets = nullptr;
    int bucketCount = 0;
    const RtSmokeStaticBucketBlasCacheState* previousBuckets = nullptr;
    int previousBucketCount = 0;
    uint64_t geometryContentSignature = 0;
    uint64_t materialGeneration = 0;
    int totalVertexCount = 0;
    int totalIndexCount = 0;
    int totalTriangleCount = 0;
    bool monolithicStaticBlas = true;
    bool hasStaticBlas = false;
    bool submitBuilds = false;
    bool forceRebuild = false;
    bool enableStaticRoutes = false;
    bool shaderSupportsStaticBucketRoutes = false;
    uint32_t firstRouteInstanceId = 2;
    int rigidRouteRecordCount = 0;
    int maxBucketRecords = 0;
    int maxRouteRecords = 0;
    int maxBuildRecords = 0;
};

struct RtSmokeStaticBucketWorkPlanSnapshot
{
    std::vector<RtSmokeStaticTlasBucketObservation> buckets;
    std::vector<RtSmokeStaticBucketBlasCacheState> previousBuckets;
    uint64_t geometryContentSignature = 0;
    uint64_t materialGeneration = 0;
    int totalVertexCount = 0;
    int totalIndexCount = 0;
    int totalTriangleCount = 0;
    bool monolithicStaticBlas = true;
    bool hasStaticBlas = false;
    bool submitBuilds = false;
    bool forceRebuild = false;
    bool enableStaticRoutes = false;
    bool shaderSupportsStaticBucketRoutes = false;
    uint32_t firstRouteInstanceId = 2;
    int rigidRouteRecordCount = 0;
    int maxBucketRecords = 0;
    int maxRouteRecords = 0;
    int maxBuildRecords = 0;
};

struct RtSmokeStaticBucketWorkPlan
{
    std::vector<RtSmokeStaticBvhBucketSignature> bucketSignatures;
    RtSmokeStaticTlasActiveSetPlan activeSetPlan;
    RtSmokeStaticBucketBlasPlan bucketBlasPlan;
    RtSmokeStaticBucketTraversalCompatibility traversalCompatibility;
    RtSmokeRouteInstanceNamespacePlan routeNamespace;
    RtSmokeStaticRouteTablePlan routeTablePlan;
    RtSmokeStaticBucketBlasBuildObservationPlan buildObservationPlan;
    RtSmokeStaticBucketBlasBuildBatchPlan buildBatchPlan;
    uint64_t planSignature = 0;
};

struct RtSmokeStaticBucketWorkPlanTimedResult
{
    RtSmokeStaticBucketWorkPlan plan;
    uint64_t planningTimeMicros = 0;
};

struct RtSmokeBvhDirtyTokenState
{
    uint64_t geometryContentSignature = 0;
    uint64_t activeBlasInputSignature = 0;
    uint64_t residentSetSignature = 0;
    uint64_t materialGeneration = 0;
    uint64_t activeSetSignature = 0;
    uint64_t tlasInstanceSignature = 0;
};

struct RtSmokeStaticBucketWorkDirtyTokenInput
{
    const RtSmokeStaticBucketWorkPlan* plan = nullptr;
    uint64_t materialGeneration = 0;
};

struct RtSmokeBvhDirtyPlanInput
{
    bool previousValid = false;
    RtSmokeBvhDirtyTokenState previous;
    RtSmokeBvhDirtyTokenState current;
};

struct RtSmokeBvhDirtyPlan
{
    bool geometryContentChanged = false;
    bool activeGeometryContentChanged = false;
    bool residentSetChanged = false;
    bool materialChanged = false;
    bool activeMembershipChanged = false;
    bool tlasInstanceChanged = false;
    bool blasInputDirty = false;
    bool tlasDirty = false;
};

struct RtSmokeBvhFrameTokenInput
{
    uint64_t staticBlasSignature = 0;
    uint64_t geometryGeneration = 0;
    uint64_t materialGeneration = 0;
    uint64_t staticActiveSetSignature = 0;
    uint64_t staticResidentSetSignature = 0;
    uint64_t staticTlasInstanceSignature = 0;
    int dynamicVertexCount = 0;
    int dynamicIndexCount = 0;
    int rigidRouteVertexCount = 0;
    int rigidRouteIndexCount = 0;
    int rigidRouteTriangleCount = 0;
    int rigidRouteInstanceCount = 0;
    int rigidRouteSeenThisFrameCount = 0;
    int rigidRouteCachedInstanceCount = 0;
    uint64_t rigidTlasInstanceSignature = 0;
    int baseTlasInstanceCount = 0;
    int rigidTlasInstanceCount = 0;
    bool hasStaticBlas = false;
    bool hasDynamicBlas = false;
};

struct RtSmokeBvhFrameToken
{
    RtSmokeBvhDirtyTokenState dirtyToken;
    uint64_t residentSetSignature = 0;
};

struct RtSmokeBvhFramePlanningInput
{
    RtSmokeStaticBucketWorkPlanInput staticBucketWorkInput;
    RtSmokeBvhFrameTokenInput frameTokenInput;
    RtSmokeBvhDirtyTokenState previousDirtyToken;
    bool previousDirtyTokenValid = false;
};

struct RtSmokeBvhFramePlanningSnapshot
{
    RtSmokeStaticBucketWorkPlanSnapshot staticBucketWorkSnapshot;
    RtSmokeBvhFrameTokenInput frameTokenInput;
    RtSmokeBvhDirtyTokenState previousDirtyToken;
    bool previousDirtyTokenValid = false;
};

struct RtSmokeBvhFramePlanningResult
{
    RtSmokeStaticBucketWorkPlan staticBucketWorkPlan;
    RtSmokeBvhFrameToken frameToken;
    RtSmokeBvhDirtyPlan dirtyPlan;
};

struct RtSmokeBvhFramePlanningTimedResult
{
    RtSmokeBvhFramePlanningResult result;
    uint64_t planningTimeMicros = 0;
};

struct RtSmokeRigidTlasObservation
{
    uint64_t meshHash = 0;
    uint64_t instanceId = 0;
    uint32_t materialId = 0;
    uint32_t sourceFlags = 0;
    bool hasMeshRecord = false;
    bool meshSeenThisFrame = false;
    bool residencyEnabled = false;
    bool hasBlas = false;
    uint32_t routeRecordIndex = std::numeric_limits<uint32_t>::max();
    uint32_t canonicalBlasRecordIndex =
        std::numeric_limits<uint32_t>::max();
    uint64_t canonicalMeshHash = 0;
    bool seenThisFrame = true;
    bool hasPreviousObjectToWorld = false;
    bool transformContinuous = false;
    float objectToWorld[16] = {};
    float previousObjectToWorld[16] = {};
};

struct RtSmokeRigidTlasPlanDesc
{
    const RtSmokeRigidTlasObservation* observations = nullptr;
    int observationCount = 0;
    uint32_t rigidSourceMask = 0;
    uint32_t firstInstanceId = 0;
    uint32_t instanceMask = 0;
    int maxInstances = 0;
};

struct RtSmokeRigidTlasPlanSnapshot
{
    std::vector<RtSmokeRigidTlasObservation> observations;
    uint32_t rigidSourceMask = 0;
    uint32_t firstInstanceId = 0;
    uint32_t instanceMask = 0;
    int maxInstances = 0;
};

struct RtSmokeRigidTlasPlan
{
    std::vector<RtSmokePlanTlasInstance> instances;
    uint64_t tlasInstanceSignature = 0;
    int visibleInstances = 0;
    int rigidInstances = 0;
    int emittedInstances = 0;
    int rejectedNonRigid = 0;
    int rejectedMissingMesh = 0;
    int rejectedStaleMesh = 0;
    int rejectedMissingBlas = 0;
};

struct RtSmokeCanonicalRigidTlasSelectionInput
{
    bool providerEnabled = false;
    bool traversalRequested = false;
    int legacyDescriptors = 0;
    int canonicalDescriptors = 0;
    int exactRecordMappings = 0;
    int missingRecordIndex = 0;
    int meshHashMismatch = 0;
    int missingBlas = 0;
};

struct RtSmokeCanonicalRigidTlasSelection
{
    bool exactParity = false;
    bool selectCanonical = false;
};

struct RtSmokeRigidTlasPlanTimedResult
{
    RtSmokeRigidTlasPlan plan;
    uint64_t planningTimeMicros = 0;
};

struct RtSmokeRigidBlasBuildPlanInput
{
    bool submitBuilds = false;
    bool forceRebuild = false;
    bool hasBlas = false;
    bool uploadRequired = false;
    bool blasInputsCompatible = false;
};

struct RtSmokeRigidBlasBuildPlan
{
    bool createBlas = false;
    bool submitBuild = false;
    bool skipBuild = false;
};

struct RtSmokeRigidBvhObjectSignatureInput
{
    uint64_t meshHash = 0;
    uint64_t instanceId = 0;
    uint64_t geometryContentSignature = 0;
    uint64_t materialGeneration = 0;
    uint32_t sourceFlags = 0;
    uint32_t rigidSourceMask = 0;
    uint32_t routeRecordIndex = std::numeric_limits<uint32_t>::max();
    int vertexCount = 0;
    int indexCount = 0;
    bool hasMeshRecord = false;
    bool meshSeenThisFrame = false;
    bool residencyEnabled = false;
};

struct RtSmokeRigidBvhObjectSignature
{
    uint64_t objectKey = 0;
    uint64_t geometryInputSignature = 0;
    uint64_t blasInputSignature = 0;
    uint64_t tlasMembershipSignature = 0;
    bool resident = false;
    bool activeCandidate = false;
};

struct RtSmokeUploadPlanMetadata
{
    bool skip = false;
    size_t byteSize = 0;
    size_t sourceOffsetBytes = 0;
    uint64_t destOffsetBytes = 0;
};

struct RtSmokeStaticDirtyUploadPlanInput
{
    bool staticBlasCacheHit = false;
    bool staticCacheChanged = false;
    bool staticGeometryBuffersReused = false;
    int staticDirtyCount = 0;
    int dirtyVertexOffset = -1;
    int dirtyVertexCount = 0;
    size_t totalVertexCount = 0;
    int dirtyIndexOffset = -1;
    int dirtyIndexCount = 0;
    size_t totalIndexCount = 0;
    int dirtyTriangleOffset = -1;
    int dirtyTriangleCount = 0;
    size_t totalTriangleClassCount = 0;
    size_t totalTriangleMaterialCount = 0;
};

struct RtSmokeStaticDirtyUploadPlan
{
    bool dirtyRangesValid = false;
    bool useDirtyRangeUploads = false;
};

struct RtSmokeStaticVertexUploadPlanInput
{
    bool forceRebuildWithoutUpload = false;
    bool staticBlasCacheHit = false;
    bool useDirtyRangeUploads = false;
    bool fullUploadOnCacheMissWithTexMatrices = false;
    int dirtyVertexOffset = -1;
    int dirtyVertexCount = 0;
    int texMatrixVertexCount = 0;
    int texMatrixFirstVertex = -1;
    int texMatrixLastVertex = -1;
    size_t totalVertexCount = 0;
};

struct RtSmokeStaticVertexUploadPlan
{
    bool skipUpload = false;
    bool fullUpload = false;
    bool texMatrixRangeUpload = false;
    bool dirtyRangeUpload = false;
    int elementOffset = -1;
    int elementCount = 0;
};

struct RtSmokePlanDataSpan
{
    const void* data = nullptr;
    size_t elementSize = 0;
    size_t elementCount = 0;
};

struct RtSmokePreviousStaticSnapshotUploadPlanInput
{
    bool dataAvailable = false;
    bool buffersReused = false;
    uint64_t previousUploadSignature = 0;
    uint64_t currentUploadSignature = 0;
};

struct RtSmokePreviousStaticSnapshotUploadPlan
{
    bool skipUpload = false;
};

uint64_t HashSmokePlanBytes(uint64_t hash, const void* data, size_t size);

RtSmokeStaticBlasSignatureSnapshot CaptureSmokeStaticBlasSignatureSnapshot(
    const RtSmokePlanStaticBlasSignatureDesc& desc);

RtSmokeAccelerationPlanSnapshot CaptureSmokeAccelerationPlanSnapshot(
    const RtSmokeAccelerationPlanInput& input);

uint64_t BuildSmokeAccelerationPlanInputToken(
    const RtSmokeAccelerationPlanInput& input);

RtSmokePlanStaticBlasSignature ComputeSmokeStaticBlasSignaturePlan(
    const RtSmokePlanStaticBlasSignatureDesc& desc);

RtSmokeAccelerationPlan BuildSmokeAccelerationPlan(
    const RtSmokeAccelerationPlanInput& input);

RtSmokeAccelerationPlanResult BuildSmokeAccelerationPlanResult(
    const RtSmokeAccelerationPlanSnapshot& snapshot);

RtSmokeAccelerationPlanTimedResult BuildSmokeAccelerationPlanTimedResult(
    const RtSmokeAccelerationPlanSnapshot& snapshot);

RtSmokeBaseTlasPlan BuildSmokeBaseTlasPlan(bool hasStaticBlas, bool hasDynamicBlas);

RtSmokeAccelerationSubmitPlan BuildSmokeAccelerationSubmitPlan(
    const RtSmokeAccelerationSubmitPlanInput& input);

RtSmokeStaticBucketAssignmentPlan BuildSmokeStaticBucketAssignmentPlan(
    const RtSmokeStaticBucketAssignmentPlanDesc& desc);

RtSmokeStaticBucketGeometryPack BuildSmokeStaticBucketGeometryPack(
    const RtSmokeStaticBucketGeometryPackDesc& desc);
bool ValidateSmokeStaticBucketClassMetadataLayout(
    const RtSmokeStaticBucketGeometryPack& geometryPack);

bool TryEncodeSmokeStaticBucketInstanceId(
    uint32_t triangleOffset,
    uint32_t& instanceId);
bool TryDecodeSmokeStaticBucketInstanceId(
    uint32_t instanceId,
    uint32_t& triangleOffset);
RtSmokeStaticBucketInstanceAddressPlan
    BuildSmokeStaticBucketInstanceAddressPlan(
        const RtSmokePlanGeometryRange& range,
        int totalIndexCount,
        int totalTriangleCount);
bool TryEncodeSmokeStaticBucketSurfaceBaseInstanceId(
    uint32_t firstSurfaceRecord,
    uint32_t& instanceId);
bool TryDecodeSmokeStaticBucketSurfaceBaseInstanceId(
    uint32_t instanceId,
    uint32_t& firstSurfaceRecord);
RtSmokeStaticBucketSurfaceAddressPlan
    BuildSmokeStaticBucketSurfaceAddressPlan(
        uint32_t firstSurfaceRecord,
        uint32_t surfaceRecordCount,
        uint32_t totalSurfaceRecordCount);
RtSmokeStaticBucketBlasGeometryPlan
    BuildSmokeStaticBucketBlasGeometryPlan(
        const RtSmokeStaticBucketGeometryPack& geometryPack,
        const RtSmokeStaticBucketPackedRecord& bucket);
RtSmokeStaticBucketResolvedGeometryAddress
    BuildSmokeStaticBucketResolvedGeometryAddress(
        const RtSmokeStaticBucketGeometryPack& geometryPack,
        uint32_t instanceId,
        uint32_t geometryIndex,
        uint32_t primitiveIndex);
RtSmokeStaticBucketResidentPackCachePlan
    BuildSmokeStaticBucketResidentPackCachePlan(
        const RtSmokeStaticBucketResidentPackCacheInput& input);
RtSmokeStaticBucketMaterialIndexCachePlan
    BuildSmokeStaticBucketMaterialIndexCachePlan(
        const RtSmokeStaticBucketMaterialIndexCacheInput& input);

RtSmokeStaticBucketPublicationEpochPlan
    BuildSmokeStaticBucketPublicationEpochPlan(
        const RtSmokeStaticBucketPublicationEpochInput& input);
RtSmokeStaticBucketCutoverPlan BuildSmokeStaticBucketCutoverPlan(
    const RtSmokeStaticBucketCutoverInput& input);
bool IsSmokeStaticBucketPrimaryOpaqueProbeSupported(
    int routeMode,
    bool cleanDiEnabled,
    int cleanDiView,
    bool diagnosticCheckpointsEnabled,
    bool cleanGiEnabled);
bool IsSmokeStaticBucketProductionRouteSupported(
    int routeMode,
    bool cleanDiEnabled,
    int cleanDiView,
    bool diagnosticCheckpointsEnabled,
    bool cleanGiEnabled,
    bool externalPdfNeeEnabled,
    bool dlssRrEnabled,
    bool transmissionPsrEnabled,
    bool reflectionPsrEnabled,
    bool reflectionSecondaryShadowsEnabled,
    bool opaqueMirrorEnabled,
    bool refractedPsrEnabled,
    int probeStage);
bool IsSmokeStaticBucketCleanDiSecondaryIsolationSupported(
    int routeMode,
    bool cleanDiEnabled,
    int cleanDiView,
    bool diagnosticCheckpointsEnabled,
    bool cleanGiEnabled,
    bool externalPdfNeeEnabled,
    bool transmissionPsrEnabled,
    int probeStage);
bool IsSmokeStaticBucketBoundedTransmissionResolverRequired(
    int routeMode,
    bool routePublicationValid);

struct RtSmokeStaticBucketSecondaryIsolationDispatchPlan
{
    bool requested = false;
    bool supported = false;
    bool routePublicationValid = false;
    bool active = false;
    int stage = 0;
    bool primaryPipelineCreation = true;
    bool primaryDispatch = true;
    bool cleanDiPipelineCreation = true;
    bool neeCachePrimaryUpdate = true;
    bool transmissionPsr = true;
    bool initial = true;
    bool temporal = true;
    bool spatialPipelineCreation = true;
    bool spatial = true;
    bool spatialNeighborReuse = true;
    bool materialFeaturePipelineCreation = true;
    bool materialFeatureRuntimeBindings = true;
    int transmissionTraceProbeMode = 0;
    bool transmissionIterativeResolve = false;
    bool transmissionMonolithicControl = false;
    bool transmissionTupleDiagnostic = false;
    bool transmissionClosestHitPositionDiagnostic = false;
    bool materialFeatureCompose = true;
};

RtSmokeStaticBucketSecondaryIsolationDispatchPlan
    BuildSmokeStaticBucketSecondaryIsolationDispatchPlan(
        bool productionView,
        bool isolationRequested,
        bool isolationSupported,
        bool routePublicationValid,
        int probeStage);

RtSmokeStaticTlasActiveSetPlan BuildSmokeStaticTlasActiveSetPlan(
    const RtSmokeStaticTlasActiveSetPlanDesc& desc);

bool BuildSmokeStaticTlasBucketObservation(
    const RtSmokeStaticTlasBucketObservationInput& input,
    RtSmokeStaticTlasBucketObservation& observation);

RtSmokeStaticBucketBlasPlan BuildSmokeStaticBucketBlasPlan(
    const RtSmokeStaticBucketBlasPlanDesc& desc);

RtSmokeStaticBucketTraversalCompatibility BuildSmokeStaticBucketTraversalCompatibility(
    const RtSmokeStaticBucketTraversalCompatibilityInput& input);

RtSmokeRouteInstanceNamespacePlan BuildSmokeRouteInstanceNamespacePlan(
    const RtSmokeRouteInstanceNamespacePlanInput& input);

RtSmokeStaticRouteTablePlan BuildSmokeStaticRouteTablePlan(
    const RtSmokeStaticRouteTablePlanInput& input);

RtSmokeStaticBucketBlasBuildPlan BuildSmokeStaticBucketBlasBuildPlan(
    const RtSmokeStaticBucketBlasBuildPlanInput& input);

RtSmokeStaticBucketBlasBuildBatchPlan BuildSmokeStaticBucketBlasBuildBatchPlan(
    const RtSmokeStaticBucketBlasBuildBatchPlanInput& input);

RtSmokeStaticBvhBucketSignature BuildSmokeStaticBvhBucketSignature(
    const RtSmokeStaticBvhBucketSignatureInput& input);

RtSmokeStaticBucketBlasBuildObservationPlan BuildSmokeStaticBucketBlasBuildObservationPlan(
    const RtSmokeStaticBucketBlasBuildObservationPlanInput& input);

RtSmokeStaticBucketWorkPlan BuildSmokeStaticBucketWorkPlan(
    const RtSmokeStaticBucketWorkPlanInput& input);

RtSmokeStaticBucketWorkPlanSnapshot CaptureSmokeStaticBucketWorkPlanSnapshot(
    const RtSmokeStaticBucketWorkPlanInput& input);

uint64_t BuildSmokeStaticBucketWorkPlanInputToken(
    const RtSmokeStaticBucketWorkPlanInput& input);

uint64_t BuildSmokeStaticBucketWorkPlanInputToken(
    const RtSmokeStaticBucketWorkPlanSnapshot& snapshot);

RtSmokeStaticBucketWorkPlan BuildSmokeStaticBucketWorkPlan(
    const RtSmokeStaticBucketWorkPlanSnapshot& snapshot);

RtSmokeStaticBucketWorkPlanTimedResult BuildSmokeStaticBucketWorkPlanTimedResult(
    const RtSmokeStaticBucketWorkPlanSnapshot& snapshot);

RtSmokeBvhDirtyTokenState BuildSmokeStaticBucketWorkDirtyToken(
    const RtSmokeStaticBucketWorkDirtyTokenInput& input);

RtSmokeBvhDirtyPlan BuildSmokeBvhDirtyPlan(
    const RtSmokeBvhDirtyPlanInput& input);

RtSmokeBvhFrameToken BuildSmokeBvhFrameToken(
    const RtSmokeBvhFrameTokenInput& input);

RtSmokeBvhFramePlanningSnapshot CaptureSmokeBvhFramePlanningSnapshot(
    const RtSmokeBvhFramePlanningInput& input);

uint64_t BuildSmokeBvhFramePlanningInputToken(
    const RtSmokeBvhFramePlanningInput& input);

uint64_t BuildSmokeBvhFramePlanningInputToken(
    const RtSmokeBvhFramePlanningSnapshot& snapshot);

RtSmokeBvhFramePlanningResult BuildSmokeBvhFramePlanningResult(
    const RtSmokeBvhFramePlanningInput& input);

RtSmokeBvhFramePlanningResult BuildSmokeBvhFramePlanningResult(
    const RtSmokeBvhFramePlanningSnapshot& snapshot);

RtSmokeBvhFramePlanningTimedResult BuildSmokeBvhFramePlanningTimedResult(
    const RtSmokeBvhFramePlanningSnapshot& snapshot);

bool AppendSmokeRigidTlasPlanObservation(
    RtSmokeRigidTlasPlan& plan,
    const RtSmokeRigidTlasPlanDesc& desc,
    const RtSmokeRigidTlasObservation& observation);

RtSmokeRigidTlasPlanSnapshot CaptureSmokeRigidTlasPlanSnapshot(
    const RtSmokeRigidTlasPlanDesc& desc);

uint64_t BuildSmokeRigidTlasPlanInputToken(
    const RtSmokeRigidTlasPlanDesc& desc);

uint64_t BuildSmokeRigidTlasPlanInputToken(
    const RtSmokeRigidTlasPlanSnapshot& snapshot);

void UpdateSmokeRigidTlasPlanInstanceSignature(
    RtSmokeRigidTlasPlan& plan,
    const RtSmokeRigidTlasPlanSnapshot& snapshot);

RtSmokeRigidTlasPlan BuildSmokeRigidTlasPlan(const RtSmokeRigidTlasPlanDesc& desc);

RtSmokeRigidTlasPlan BuildSmokeRigidTlasPlan(
    const RtSmokeRigidTlasPlanSnapshot& snapshot);

RtSmokeCanonicalRigidTlasSelection
BuildSmokeCanonicalRigidTlasSelection(
    const RtSmokeCanonicalRigidTlasSelectionInput& input);

RtSmokeRigidTlasPlanTimedResult BuildSmokeRigidTlasPlanTimedResult(
    const RtSmokeRigidTlasPlanSnapshot& snapshot);

RtSmokeRigidBlasBuildPlan BuildSmokeRigidBlasBuildPlan(
    const RtSmokeRigidBlasBuildPlanInput& input);

RtSmokeRigidBvhObjectSignature BuildSmokeRigidBvhObjectSignature(
    const RtSmokeRigidBvhObjectSignatureInput& input);

RtSmokeUploadPlanMetadata BuildSmokeVectorUploadPlanMetadata(
    size_t elementCount,
    size_t elementSize,
    bool skip,
    int dirtyElementOffset,
    int dirtyElementCount);

RtSmokeStaticDirtyUploadPlan BuildSmokeStaticDirtyUploadPlan(
    const RtSmokeStaticDirtyUploadPlanInput& input);

RtSmokeStaticVertexUploadPlan BuildSmokeStaticVertexUploadPlan(
    const RtSmokeStaticVertexUploadPlanInput& input);

uint64_t BuildSmokePlanDataSpanSignature(
    const RtSmokePlanDataSpan* spans,
    int spanCount);

RtSmokePreviousStaticSnapshotUploadPlan BuildSmokePreviousStaticSnapshotUploadPlan(
    const RtSmokePreviousStaticSnapshotUploadPlanInput& input);

RtSmokeAsAdmissionPlan BuildSmokeAsAdmissionPlan(
    const RtSmokeAsAdmissionBudget& budget,
    const std::vector<RtSmokeAsAdmissionRequest>& requests);
