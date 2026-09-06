#pragma once

#include "PathTraceAccelerationPlan.h"
#include "PathTraceAccelCpuAllocation.h"
#include "PathTraceGeometryUniverse.h"
#include "PathTraceUniversePlanningSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>
#include <mutex>
#include <vector>

constexpr std::size_t RT_PT_ACCEL_CPU_SLOT_MAX_BYTES = 64u * 1024u * 1024u;
constexpr std::size_t RT_PT_ACCEL_CPU_RING_MAX_BYTES =
    3u * RT_PT_ACCEL_CPU_SLOT_MAX_BYTES;
constexpr std::size_t RT_PT_ACCEL_CPU_RESIDENT_BACKING_MAX_BYTES =
    64u * 1024u * 1024u;
constexpr std::size_t RT_PT_ACCEL_CPU_RESIDENT_TOTAL_MAX_BYTES =
    2u * RT_PT_ACCEL_CPU_RESIDENT_BACKING_MAX_BYTES;

enum RtPathTraceAccelCpuVectorFamily : std::uint32_t
{
    RT_PT_ACCEL_FAMILY_ACCEL_VERTEX_BYTES = 0,
    RT_PT_ACCEL_FAMILY_ACCEL_INDEXES,
    RT_PT_ACCEL_FAMILY_ACCEL_TRIANGLE_CLASSES,
    RT_PT_ACCEL_FAMILY_ACCEL_TRIANGLE_MATERIALS,
    RT_PT_ACCEL_FAMILY_RIGID_PLAN_INSTANCES,
    RT_PT_ACCEL_FAMILY_RIGID_PLAN_TRUNCATED_IDS,
    RT_PT_ACCEL_FAMILY_RIGID_MATERIAL_IDS,
    RT_PT_ACCEL_FAMILY_RIGID_MESH_ROWS,
    RT_PT_ACCEL_FAMILY_RIGID_INSTANCE_ELIGIBILITY,
    RT_PT_ACCEL_FAMILY_RIGID_MESH_VERTICES,
    RT_PT_ACCEL_FAMILY_RIGID_MESH_INDEXES,
    RT_PT_ACCEL_FAMILY_STATIC_SURFACES,
    RT_PT_ACCEL_FAMILY_STATIC_VERTICES,
    RT_PT_ACCEL_FAMILY_STATIC_INDEXES,
    RT_PT_ACCEL_FAMILY_STATIC_TRIANGLE_CLASSES,
    RT_PT_ACCEL_FAMILY_STATIC_TRIANGLE_MATERIALS,
    RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_VERTICES,
    RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_INDEXES,
    RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRIANGLE_MATERIALS,
    RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRIANGLE_MATERIAL_INDEXES,
    RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRIANGLE_FLAGS,
    RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_RANGES,
    RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_INSTANCES,
    RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRANSFORMS,
    RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_SEEN,
    RT_PT_ACCEL_FAMILY_PRODUCT_STATIC_BUCKETS,
    RT_PT_ACCEL_FAMILY_PRODUCT_STATIC_ASSIGNMENTS,
    RT_PT_ACCEL_FAMILY_PRODUCT_PACK_BUCKETS,
    RT_PT_ACCEL_FAMILY_PRODUCT_PACK_VERTEX_BYTES,
    RT_PT_ACCEL_FAMILY_PRODUCT_PACK_INDEXES,
    RT_PT_ACCEL_FAMILY_PRODUCT_PACK_TRIANGLE_CLASSES,
    RT_PT_ACCEL_FAMILY_PRODUCT_PACK_CLASS_WORDS,
    RT_PT_ACCEL_FAMILY_PRODUCT_PACK_TRIANGLE_MATERIALS,
    RT_PT_ACCEL_FAMILY_PRODUCT_PACK_SURFACE_RECORDS,
    RT_PT_ACCEL_FAMILY_PRODUCT_PACK_IDENTITIES,
    RT_PT_ACCEL_FAMILY_SCRATCH_STATIC_SORT,
    RT_PT_ACCEL_FAMILY_SCRATCH_RIGID_MESH_HASHES,
    RT_PT_ACCEL_FAMILY_TICKET_ACTIVE_AREAS,
    RT_PT_ACCEL_FAMILY_SCRATCH_STATIC_SURFACES,
    RT_PT_ACCEL_FAMILY_COUNT
};

struct RtPathTraceAccelCpuCapacityCounts
{
    std::array<std::size_t, RT_PT_ACCEL_FAMILY_COUNT> values = {};
};

struct RtPathTraceAccelCpuCapacityPlan
{
    RtPathTraceAccelCpuCapacityCounts counts;
    std::array<std::size_t, RT_PT_ACCEL_FAMILY_COUNT> bytes = {};
    std::size_t snapshotBytes = 0;
    std::size_t productBytes = 0;
    std::size_t scratchBytes = 0;
    std::size_t plannedPeakBytes = 0;
    std::size_t attemptedPeakBytes = 0;
    bool valid = false;
};

struct RtPathTraceAccelCpuCapacityStamp
{
    std::array<std::size_t, RT_PT_ACCEL_FAMILY_COUNT> capacities = {};
    std::size_t snapshotBytes = 0;
    std::size_t productBytes = 0;
    std::size_t residentBytes = 0;
};

struct RtPathTraceAccelCpuCompatibilityToken
{
    std::uint64_t mapTimeStamp = 0;
    std::uint64_t mapLoadSerial = 0;
    char mapName[RT_PT_PLANNING_MAP_NAME_CAPACITY] = {};
    std::uint64_t lifecycleEpoch = 0;
    std::uint64_t configFingerprint = 0;
};

struct RtPathTraceStaticBucketCpuSnapshot
{
    std::vector<RtSmokeStaticBucketAssignmentSurface> surfaces;
    std::vector<PathTraceSmokeVertex> vertices;
    std::vector<std::uint32_t> indexes;
    std::vector<std::uint32_t> triangleClasses;
    std::vector<std::uint32_t> triangleMaterials;
    std::uint64_t worldGeneration = 0;
    std::uint64_t sourceGeneration = 0;
    std::uint64_t storageGeneration = 0;
    int portalAreaCount = 0;
    int maxVerticesPerBucket = 0;
    int maxIndexesPerBucket = 0;
    int maxTrianglesPerBucket = 0;
    bool complete = false;

    std::size_t OwnedBytes() const;
    void ResetAndRelease();
};

// Large immutable CPU input is published once per content mutation and leased
// by Lane B.  Per-frame slots retain only the small ticket below.
struct RtPathTraceAccelCpuResidentPayload
{
    RtSmokeAccelerationPlanSnapshot acceleration;
    RtPathTraceRigidRouteBuildSnapshot rigidRoute;
    RtPathTraceStaticBucketCpuSnapshot staticBucket;
    std::uint64_t generation = 0;
    std::uint64_t rigidPayloadSignature = 0;
    std::uint64_t staticPayloadSignature = 0;
    bool complete = false;

    std::size_t OwnedBytes() const;
};

struct RtPathTraceAccelCpuResidentPublishStats
{
    std::uint64_t publishes = 0;
    std::uint64_t reuses = 0;
    std::uint64_t cowPublishes = 0;
    std::uint64_t leaseRejects = 0;
    std::uint64_t residentCapRejects = 0;
    std::uint64_t allocationFailures = 0;
    std::uint64_t fillFailures = 0;
    std::uint64_t ticketValidationFailures = 0;
    std::uint64_t ticketBuildFailures = 0;
    std::uint64_t failures = 0;
    std::uint64_t publishUs = 0;
    std::size_t residentBytes = 0;
    std::size_t cowBytes = 0;
    std::size_t highWaterBytes = 0;
    std::size_t attemptedResidentBytes = 0;
};

struct RtPathTraceAccelCpuResidentCapacityPlan
{
    RtSmokeAccelerationPlanSnapshotCounts acceleration;
    RtPathTraceRigidRouteResidentMeshPayloadCounts rigidRoute;
    RtPathTraceStaticBucketCpuSnapshotCounts staticBucket;
    std::size_t plannedBytes = 0;
    bool valid = false;
};

bool BuildPathTraceAccelCpuResidentCapacityPlan(
    const RtSmokeAccelerationPlanSnapshotCounts& acceleration,
    const RtPathTraceRigidRouteResidentMeshPayloadCounts& rigidRoute,
    const RtPathTraceStaticBucketCpuSnapshotCounts& staticBucket,
    RtPathTraceAccelCpuResidentCapacityPlan& plan);

using RtPathTraceAccelCpuResidentPreflightFn = bool (*)(
    void* context,
    RtPathTraceAccelCpuResidentCapacityPlan& plan);
using RtPathTraceAccelCpuResidentFillFn = bool (*)(
    void* context,
    const RtPathTraceAccelCpuResidentCapacityPlan& plan,
    RtPathTraceAccelCpuResidentPayload& payload);

class RtPathTraceAccelCpuResidentBackingStore
{
public:
    bool Publish(
        std::uint64_t rigidPayloadSignature,
        std::uint64_t staticPayloadSignature,
        RtPathTraceAccelCpuResidentPreflightFn preflight,
        RtPathTraceAccelCpuResidentFillFn fill,
        void* context,
        std::shared_ptr<const RtPathTraceAccelCpuResidentPayload>& lease);
    void Reset();
    RtPathTraceAccelCpuResidentPublishStats Stats() const;
    void NoteTicketValidationFailure();
    void NoteTicketBuildFailure();

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const RtPathTraceAccelCpuResidentPayload> current_;
    std::shared_ptr<const RtPathTraceAccelCpuResidentPayload> retired_;
    std::uint64_t nextGeneration_ = 1;
    RtPathTraceAccelCpuResidentPublishStats stats_;
};

class RtPathTraceAccelCpuResidentPublisher
{
public:
    bool Publish(
        const RtSmokeAccelerationPlanInput& accelerationInput,
        const RtSmokeGeometryUniverse& rigidUniverse,
        const RtSmokeRigidTlasPlan& rigidPlan,
        const std::vector<std::uint32_t>& materialIds,
        const RtPathTraceRigidRouteBuildSnapshot& rigidMetadata,
        std::uint64_t rigidPayloadSignature,
        const RtSmokeGeometryUniverse& staticUniverse,
        std::uint64_t staticWorldGeneration,
        std::uint64_t staticSourceGeneration,
        std::uint64_t staticPayloadSignature,
        int portalAreaCount,
        int maxVerticesPerBucket,
        int maxIndexesPerBucket,
        int maxTrianglesPerBucket,
        const std::vector<bool>* activeAreas,
        struct RtPathTraceAccelCpuSnapshot& ticket);
    void Reset();
    RtPathTraceAccelCpuResidentPublishStats Stats() const;

private:
    RtPathTraceAccelCpuResidentBackingStore backingStore_;
};

struct RtPathTraceAccelCpuSnapshot
{
    RtPathTracePlanningSnapshotEpoch epoch;
    RtPathTraceAccelCpuCompatibilityToken compatibility;
    RtPathTraceRigidRouteBuildSnapshot rigidRoute;
    RtSmokeAccelerationPlanSnapshot acceleration;
    RtPathTraceStaticBucketCpuSnapshot staticBucket;
    std::shared_ptr<const RtPathTraceAccelCpuResidentPayload> residentPayload;
    std::vector<std::uint8_t> activePortalAreas;
    std::uint64_t residentGeneration = 0;
    std::uint64_t rigidPayloadSignature = 0;
    std::uint64_t staticPayloadSignature = 0;
    bool residentBacked = false;
    std::uint64_t inputReceipt = 0;
    std::size_t reservedProductBytes = 0;
    std::size_t plannedPeakBytes = 0;
    bool complete = false;

    std::size_t OwnedBytes() const;
    void ResetAndRelease();
};

bool PublishPathTraceAccelCpuResidentTicket(
    RtPathTraceAccelCpuResidentBackingStore& backingStore,
    const RtSmokeAccelerationPlanInput& accelerationInput,
    const RtPathTraceRigidRouteBuildSnapshot& rigidMetadata,
    std::uint64_t rigidPayloadSignature,
    std::uint64_t staticWorldGeneration,
    std::uint64_t staticSourceGeneration,
    std::uint64_t staticPayloadSignature,
    int portalAreaCount,
    int maxVerticesPerBucket,
    int maxIndexesPerBucket,
    int maxTrianglesPerBucket,
    const std::vector<bool>* activeAreas,
    RtPathTraceAccelCpuResidentPreflightFn preflight,
    RtPathTraceAccelCpuResidentFillFn fill,
    void* context,
    RtPathTraceAccelCpuSnapshot& ticket);

struct RtPathTraceAccelCpuProduct
{
    RtPathTracePlanningSnapshotEpoch epoch;
    RtPathTraceAccelCpuCompatibilityToken compatibility;
    std::uint64_t inputReceipt = 0;
    RtPathTraceRigidRouteBuild rigidBuild;
    RtSmokeAccelerationPlan accelerationPlan;
    RtSmokeStaticBucketAssignmentPlan staticAssignment;
    RtSmokeStaticBucketGeometryPack staticPack;
    std::uint64_t rigidGeometrySignature = 0;
    std::uint64_t rigidInstanceSignature = 0;
    std::uint64_t staticAssignmentSignature = 0;
    std::uint64_t staticPackSignature = 0;
    std::uint32_t consumerSlotIndex = UINT32_MAX;
    std::uint64_t consumerGeneration = 0;
    bool complete = false;

    std::size_t OwnedBytes() const;
    void ResetAndRelease();
};

struct RtPathTraceAccelCpuOracle
{
    RtPathTracePlanningSnapshotEpoch epoch;
    std::uint64_t inputReceipt = 0;
    RtSmokeAccelerationPlan accelerationPlan;
    std::uint64_t rigidGeometrySignature = 0;
    std::uint64_t rigidInstanceSignature = 0;
    std::uint64_t staticAssignmentSignature = 0;
    std::uint64_t staticPackSignature = 0;
    std::uint32_t rigidVertices = 0;
    std::uint32_t rigidIndexes = 0;
    std::uint32_t rigidInstances = 0;
    std::uint32_t staticBuckets = 0;
    std::uint32_t staticAssignments = 0;
    std::uint32_t staticVertices = 0;
    std::uint32_t staticIndexes = 0;
    bool complete = false;
};

struct RtPathTraceAccelCpuComparison
{
    std::uint64_t generation = 0;
    std::uint32_t rigidMismatches = 0;
    std::uint32_t accelerationMismatches = 0;
    std::uint32_t staticMismatches = 0;
    bool exact = false;
};

bool PathTraceAccelCpuTokensCompatible(
    const RtPathTraceAccelCpuCompatibilityToken& product,
    const RtPathTraceAccelCpuCompatibilityToken& current);
bool PathTraceAccelCpuEpochValid(
    const RtPathTracePlanningSnapshotEpoch& epoch);
bool BuildPathTraceAccelCpuCapacityPlan(
    const RtPathTraceAccelCpuCapacityCounts& counts,
    RtPathTraceAccelCpuCapacityPlan& plan);
bool BuildPathTraceAccelCpuCapacityCounts(
    const RtSmokeAccelerationPlanSnapshotCounts& acceleration,
    const RtPathTraceRigidRouteBuildSnapshotCounts& rigid,
    const RtPathTraceStaticBucketCpuSnapshotCounts& staticBucket,
    RtPathTraceAccelCpuCapacityCounts& counts);
bool CountPathTraceAccelCpuCapacityFromSnapshot(
    const RtPathTraceAccelCpuSnapshot& snapshot,
    RtPathTraceAccelCpuCapacityCounts& counts);
bool StampPathTraceAccelCpuCapacities(
    const RtPathTraceAccelCpuSnapshot& snapshot,
    const RtPathTraceAccelCpuProduct* product,
    RtPathTraceAccelCpuCapacityStamp& stamp);
bool PathTraceAccelCpuCapacityStampWithinPlan(
    const RtPathTraceAccelCpuCapacityStamp& stamp,
    const RtPathTraceAccelCpuCapacityPlan& plan);
std::uint64_t BuildPathTraceAccelCpuInputReceipt(
    const RtPathTraceAccelCpuSnapshot& snapshot);
bool ValidatePathTraceAccelCpuSnapshot(
    const RtPathTraceAccelCpuSnapshot& snapshot);
bool ValidatePathTraceAccelCpuProduct(
    const RtPathTraceAccelCpuProduct& product);
bool BuildPathTraceAccelCpuProduct(
    const RtPathTraceAccelCpuSnapshot& snapshot,
    RtPathTraceAccelCpuProduct& product,
    std::size_t* peakSlotBytes = nullptr);
RtPathTraceAccelCpuOracle BuildPathTraceAccelCpuOracle(
    const RtPathTraceAccelCpuProduct& product);
RtPathTraceAccelCpuComparison ComparePathTraceAccelCpuProduct(
    const RtPathTraceAccelCpuProduct& product,
    const RtPathTraceAccelCpuOracle& oracle);
