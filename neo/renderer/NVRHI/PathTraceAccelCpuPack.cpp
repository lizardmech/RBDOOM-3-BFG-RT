#include "precompiled.h"
#pragma hdrstop

#include "PathTraceAccelCpuPack.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace
{
constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::size_t FamilyElementSize(RtPathTraceAccelCpuVectorFamily family)
{
    switch (family)
    {
    case RT_PT_ACCEL_FAMILY_ACCEL_VERTEX_BYTES: return sizeof(std::uint8_t);
    case RT_PT_ACCEL_FAMILY_ACCEL_INDEXES:
    case RT_PT_ACCEL_FAMILY_ACCEL_TRIANGLE_CLASSES:
    case RT_PT_ACCEL_FAMILY_ACCEL_TRIANGLE_MATERIALS:
    case RT_PT_ACCEL_FAMILY_RIGID_PLAN_TRUNCATED_IDS:
    case RT_PT_ACCEL_FAMILY_RIGID_MATERIAL_IDS:
    case RT_PT_ACCEL_FAMILY_RIGID_MESH_INDEXES:
    case RT_PT_ACCEL_FAMILY_STATIC_INDEXES:
    case RT_PT_ACCEL_FAMILY_STATIC_TRIANGLE_CLASSES:
    case RT_PT_ACCEL_FAMILY_STATIC_TRIANGLE_MATERIALS:
    case RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_INDEXES:
    case RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRIANGLE_MATERIALS:
    case RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRIANGLE_MATERIAL_INDEXES:
    case RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRIANGLE_FLAGS:
    case RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_SEEN:
    case RT_PT_ACCEL_FAMILY_PRODUCT_PACK_INDEXES:
    case RT_PT_ACCEL_FAMILY_PRODUCT_PACK_TRIANGLE_CLASSES:
    case RT_PT_ACCEL_FAMILY_PRODUCT_PACK_CLASS_WORDS:
    case RT_PT_ACCEL_FAMILY_PRODUCT_PACK_TRIANGLE_MATERIALS:
        return sizeof(std::uint32_t);
    case RT_PT_ACCEL_FAMILY_RIGID_PLAN_INSTANCES:
        return sizeof(RtSmokePlanTlasInstance);
    case RT_PT_ACCEL_FAMILY_RIGID_MESH_ROWS:
        return sizeof(RtPathTraceRigidRouteMeshSnapshot);
    case RT_PT_ACCEL_FAMILY_RIGID_INSTANCE_ELIGIBILITY:
        return sizeof(RtPathTraceRigidRouteInstanceEligibility);
    case RT_PT_ACCEL_FAMILY_RIGID_MESH_VERTICES:
    case RT_PT_ACCEL_FAMILY_STATIC_VERTICES:
    case RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_VERTICES:
        return sizeof(PathTraceSmokeVertex);
    case RT_PT_ACCEL_FAMILY_STATIC_SURFACES:
        return sizeof(RtSmokeStaticBucketAssignmentSurface);
    case RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_RANGES:
        return sizeof(RtPathTraceRigidRouteGeometryRange);
    case RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_INSTANCES:
        return sizeof(PathTraceRigidRouteInstance);
    case RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRANSFORMS:
        return sizeof(std::array<float, 16>);
    case RT_PT_ACCEL_FAMILY_PRODUCT_STATIC_BUCKETS:
        return sizeof(RtSmokeStaticBucketAssignmentBucket);
    case RT_PT_ACCEL_FAMILY_PRODUCT_STATIC_ASSIGNMENTS:
        return sizeof(RtSmokeStaticBucketAssignment);
    case RT_PT_ACCEL_FAMILY_PRODUCT_PACK_BUCKETS:
        return sizeof(RtSmokeStaticBucketPackedRecord);
    case RT_PT_ACCEL_FAMILY_PRODUCT_PACK_VERTEX_BYTES:
        return sizeof(std::uint8_t);
    case RT_PT_ACCEL_FAMILY_PRODUCT_PACK_SURFACE_RECORDS:
        return sizeof(RtSmokeStaticBucketSurfaceRecord);
    case RT_PT_ACCEL_FAMILY_PRODUCT_PACK_IDENTITIES:
        return sizeof(RtSmokeStaticBucketTriangleIdentity);
    case RT_PT_ACCEL_FAMILY_SCRATCH_STATIC_SORT: return sizeof(int);
    case RT_PT_ACCEL_FAMILY_SCRATCH_RIGID_MESH_HASHES: return sizeof(std::uint64_t);
    case RT_PT_ACCEL_FAMILY_TICKET_ACTIVE_AREAS: return sizeof(std::uint8_t);
    case RT_PT_ACCEL_FAMILY_SCRATCH_STATIC_SURFACES:
        return sizeof(RtSmokeStaticBucketAssignmentSurface);
    default: return 0;
    }
}

bool FamilyIsSnapshot(RtPathTraceAccelCpuVectorFamily family)
{
    return family <= RT_PT_ACCEL_FAMILY_STATIC_TRIANGLE_MATERIALS ||
        family == RT_PT_ACCEL_FAMILY_TICKET_ACTIVE_AREAS;
}

bool FamilyIsScratch(RtPathTraceAccelCpuVectorFamily family)
{
    return family == RT_PT_ACCEL_FAMILY_SCRATCH_STATIC_SORT ||
        family == RT_PT_ACCEL_FAMILY_SCRATCH_RIGID_MESH_HASHES ||
        family == RT_PT_ACCEL_FAMILY_SCRATCH_STATIC_SURFACES;
}

std::uint64_t HashBytes(std::uint64_t hash, const void* bytes, std::size_t count)
{
    const std::uint8_t* p = static_cast<const std::uint8_t*>(bytes);
    for (std::size_t i = 0; i < count; ++i)
    {
        hash ^= p[i];
        hash *= kFnvPrime;
    }
    return hash;
}

template<class T>
std::size_t VectorBytes(const std::vector<T>& values)
{
    return values.capacity() * sizeof(T);
}

bool AddBytes(std::size_t& total, std::size_t bytes)
{
    if (bytes > RT_PT_ACCEL_CPU_SLOT_MAX_BYTES - total)
    {
        return false;
    }
    total += bytes;
    return true;
}

bool AddResidentArrayBytes(
    std::size_t& total,
    std::size_t count,
    std::size_t elementSize)
{
    if (elementSize != 0 &&
        count > std::numeric_limits<std::size_t>::max() / elementSize)
        return false;
    const std::size_t bytes = count * elementSize;
    if (bytes > std::numeric_limits<std::size_t>::max() - total)
        return false;
    total += bytes;
    return true;
}

bool AccelerationPlansEqual(
    const RtSmokeAccelerationPlan& a,
    const RtSmokeAccelerationPlan& b)
{
    return a.staticSignature.hash == b.staticSignature.hash &&
        a.staticSignature.vertexCount == b.staticSignature.vertexCount &&
        a.staticSignature.indexCount == b.staticSignature.indexCount &&
        a.staticSignature.triangleCount == b.staticSignature.triangleCount &&
        a.staticBlas.enabled == b.staticBlas.enabled &&
        a.staticBlas.cacheHit == b.staticBlas.cacheHit &&
        a.staticBlas.vertexCount == b.staticBlas.vertexCount &&
        a.staticBlas.indexCount == b.staticBlas.indexCount &&
        a.dynamicBlas.enabled == b.dynamicBlas.enabled &&
        a.dynamicBlas.cacheHit == b.dynamicBlas.cacheHit &&
        a.dynamicBlas.vertexCount == b.dynamicBlas.vertexCount &&
        a.dynamicBlas.indexCount == b.dynamicBlas.indexCount &&
        a.staticSignatureReused == b.staticSignatureReused &&
        a.staticCacheHit == b.staticCacheHit &&
        a.hasStaticBlas == b.hasStaticBlas &&
        a.hasDynamicBlas == b.hasDynamicBlas;
}

bool AccelCpuEpochsMatch(
    const RtPathTracePlanningSnapshotEpoch& lhs,
    const RtPathTracePlanningSnapshotEpoch& rhs)
{
    return PathTraceAccelCpuEpochValid(lhs) &&
        PathTraceAccelCpuEpochValid(rhs) &&
        lhs.generation == rhs.generation && lhs.frameIndex == rhs.frameIndex &&
        lhs.mapTimeStamp == rhs.mapTimeStamp &&
        lhs.mapLoadSerial == rhs.mapLoadSerial &&
        std::memcmp(lhs.mapName, rhs.mapName, sizeof(lhs.mapName)) == 0;
}

template<class T>
bool ReserveAccelVector(std::vector<T>& values, std::size_t count)
{
    if (count == 0) return true;
    PathTraceAccelCpuPackMaybeInjectAllocationFailure();
    values.reserve(count);
    return values.capacity() == count;
}

bool ReserveAccelCpuProduct(
    const RtPathTraceAccelCpuCapacityPlan& plan,
    RtPathTraceAccelCpuProduct& product,
    std::vector<int>& staticSortScratch,
    std::vector<std::uint64_t>& rigidMeshScratch,
    std::vector<RtSmokeStaticBucketAssignmentSurface>& staticSurfaceScratch)
{
#define RESERVE(family, vector) \
    if (!ReserveAccelVector(vector, plan.counts.values[family])) return false
    RESERVE(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_VERTICES, product.rigidBuild.vertices);
    RESERVE(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_INDEXES, product.rigidBuild.indexes);
    RESERVE(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRIANGLE_MATERIALS,
        product.rigidBuild.triangleMaterials);
    RESERVE(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRIANGLE_MATERIAL_INDEXES,
        product.rigidBuild.triangleMaterialIndexes);
    RESERVE(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRIANGLE_FLAGS,
        product.rigidBuild.triangleClassAndFlags);
    RESERVE(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_RANGES,
        product.rigidBuild.geometryRanges);
    RESERVE(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_INSTANCES,
        product.rigidBuild.instances);
    RESERVE(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRANSFORMS,
        product.rigidBuild.instanceObjectToWorld);
    RESERVE(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_SEEN,
        product.rigidBuild.instanceSeenThisFrame);
    RESERVE(RT_PT_ACCEL_FAMILY_PRODUCT_STATIC_BUCKETS,
        product.staticAssignment.buckets);
    RESERVE(RT_PT_ACCEL_FAMILY_PRODUCT_STATIC_ASSIGNMENTS,
        product.staticAssignment.assignments);
    RESERVE(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_BUCKETS, product.staticPack.buckets);
    RESERVE(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_VERTEX_BYTES,
        product.staticPack.vertexBytes);
    RESERVE(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_INDEXES, product.staticPack.indexes);
    RESERVE(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_TRIANGLE_CLASSES,
        product.staticPack.triangleClasses);
    RESERVE(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_CLASS_WORDS,
        product.staticPack.staticClassMetadataWords);
    RESERVE(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_TRIANGLE_MATERIALS,
        product.staticPack.triangleMaterials);
    RESERVE(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_SURFACE_RECORDS,
        product.staticPack.surfaceRecords);
    RESERVE(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_IDENTITIES,
        product.staticPack.triangleIdentities);
    RESERVE(RT_PT_ACCEL_FAMILY_SCRATCH_STATIC_SORT, staticSortScratch);
    RESERVE(RT_PT_ACCEL_FAMILY_SCRATCH_RIGID_MESH_HASHES, rigidMeshScratch);
    RESERVE(RT_PT_ACCEL_FAMILY_SCRATCH_STATIC_SURFACES, staticSurfaceScratch);
#undef RESERVE
    return true;
}

}

bool BuildPathTraceAccelCpuCapacityPlan(
    const RtPathTraceAccelCpuCapacityCounts& counts,
    RtPathTraceAccelCpuCapacityPlan& plan)
{
    plan = RtPathTraceAccelCpuCapacityPlan();
    plan.counts = counts;
    bool overflow = false;
    const auto saturatingAdd = [&overflow](std::size_t& total, std::size_t bytes)
    {
        if (bytes > std::numeric_limits<std::size_t>::max() - total)
        {
            total = std::numeric_limits<std::size_t>::max();
            overflow = true;
        }
        else total += bytes;
    };
    for (std::uint32_t index = 0; index < RT_PT_ACCEL_FAMILY_COUNT; ++index)
    {
        const RtPathTraceAccelCpuVectorFamily family =
            static_cast<RtPathTraceAccelCpuVectorFamily>(index);
        const std::size_t elementSize = FamilyElementSize(family);
        const std::size_t count = counts.values[index];
        if (elementSize == 0)
        {
            plan.attemptedPeakBytes = std::numeric_limits<std::size_t>::max();
            return false;
        }
        std::size_t bytes = 0;
        if (count > std::numeric_limits<std::size_t>::max() / elementSize)
        {
            bytes = std::numeric_limits<std::size_t>::max();
            overflow = true;
        }
        else bytes = count * elementSize;
        plan.bytes[index] = bytes;
        std::size_t* subtotal = FamilyIsScratch(family)
            ? &plan.scratchBytes
            : (FamilyIsSnapshot(family) ? &plan.snapshotBytes : &plan.productBytes);
        saturatingAdd(*subtotal, bytes);
    }
    saturatingAdd(plan.plannedPeakBytes, plan.snapshotBytes);
    saturatingAdd(plan.plannedPeakBytes, plan.productBytes);
    saturatingAdd(plan.plannedPeakBytes, plan.scratchBytes);
    plan.attemptedPeakBytes = plan.plannedPeakBytes;
    if (overflow || plan.plannedPeakBytes > RT_PT_ACCEL_CPU_SLOT_MAX_BYTES)
        return false;
    plan.valid = true;
    return true;
}

bool BuildPathTraceAccelCpuCapacityCounts(
    const RtSmokeAccelerationPlanSnapshotCounts& acceleration,
    const RtPathTraceRigidRouteBuildSnapshotCounts& rigid,
    const RtPathTraceStaticBucketCpuSnapshotCounts& staticBucket,
    RtPathTraceAccelCpuCapacityCounts& counts)
{
    counts = RtPathTraceAccelCpuCapacityCounts();
#define SET_COUNT(family, value) counts.values[family] = static_cast<std::size_t>(value)
    SET_COUNT(RT_PT_ACCEL_FAMILY_ACCEL_VERTEX_BYTES, acceleration.vertexBytes);
    SET_COUNT(RT_PT_ACCEL_FAMILY_ACCEL_INDEXES, acceleration.indexes);
    SET_COUNT(RT_PT_ACCEL_FAMILY_ACCEL_TRIANGLE_CLASSES, acceleration.triangleClasses);
    SET_COUNT(RT_PT_ACCEL_FAMILY_ACCEL_TRIANGLE_MATERIALS, acceleration.triangleMaterials);
    SET_COUNT(RT_PT_ACCEL_FAMILY_RIGID_PLAN_INSTANCES, rigid.planInstances);
    SET_COUNT(RT_PT_ACCEL_FAMILY_RIGID_PLAN_TRUNCATED_IDS,
        rigid.planTruncatedInstanceIds);
    SET_COUNT(RT_PT_ACCEL_FAMILY_RIGID_MATERIAL_IDS, rigid.materialTableIds);
    SET_COUNT(RT_PT_ACCEL_FAMILY_RIGID_MESH_ROWS, rigid.meshes);
    SET_COUNT(RT_PT_ACCEL_FAMILY_RIGID_INSTANCE_ELIGIBILITY,
        rigid.instanceEligibility);
    SET_COUNT(RT_PT_ACCEL_FAMILY_RIGID_MESH_VERTICES, rigid.meshVertices);
    SET_COUNT(RT_PT_ACCEL_FAMILY_RIGID_MESH_INDEXES, rigid.meshIndexes);
    SET_COUNT(RT_PT_ACCEL_FAMILY_STATIC_SURFACES, staticBucket.surfaces);
    SET_COUNT(RT_PT_ACCEL_FAMILY_STATIC_VERTICES, staticBucket.vertices);
    SET_COUNT(RT_PT_ACCEL_FAMILY_STATIC_INDEXES, staticBucket.indexes);
    SET_COUNT(RT_PT_ACCEL_FAMILY_STATIC_TRIANGLE_CLASSES,
        staticBucket.triangleClasses);
    SET_COUNT(RT_PT_ACCEL_FAMILY_STATIC_TRIANGLE_MATERIALS,
        staticBucket.triangleMaterials);
    const std::size_t rigidTriangles = rigid.meshIndexes / 3u;
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_VERTICES, rigid.meshVertices);
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_INDEXES, rigid.meshIndexes);
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRIANGLE_MATERIALS, rigidTriangles);
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRIANGLE_MATERIAL_INDEXES, rigidTriangles);
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRIANGLE_FLAGS, rigidTriangles);
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_RANGES, rigid.meshes);
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_INSTANCES, rigid.planInstances);
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRANSFORMS, rigid.planInstances);
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_SEEN, rigid.planInstances);
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_STATIC_BUCKETS, staticBucket.surfaces);
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_STATIC_ASSIGNMENTS, staticBucket.surfaces);
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_BUCKETS, staticBucket.surfaces);
    if (staticBucket.vertices > RT_PT_ACCEL_CPU_SLOT_MAX_BYTES /
            sizeof(PathTraceSmokeVertex)) return false;
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_VERTEX_BYTES,
        staticBucket.vertices * sizeof(PathTraceSmokeVertex));
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_INDEXES, staticBucket.indexes);
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_TRIANGLE_CLASSES,
        staticBucket.triangleClasses);
    if (staticBucket.surfaces >
        (RT_PT_ACCEL_CPU_SLOT_MAX_BYTES - staticBucket.triangleClasses) / 4u)
        return false;
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_CLASS_WORDS,
        staticBucket.triangleClasses + staticBucket.surfaces * 4u);
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_TRIANGLE_MATERIALS,
        staticBucket.triangleMaterials);
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_SURFACE_RECORDS, staticBucket.surfaces);
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_IDENTITIES, staticBucket.triangleClasses);
    SET_COUNT(RT_PT_ACCEL_FAMILY_SCRATCH_STATIC_SORT, staticBucket.surfaces);
    SET_COUNT(RT_PT_ACCEL_FAMILY_SCRATCH_RIGID_MESH_HASHES, rigid.planInstances);
#undef SET_COUNT
    return true;
}

bool CountPathTraceAccelCpuCapacityFromSnapshot(
    const RtPathTraceAccelCpuSnapshot& snapshot,
    RtPathTraceAccelCpuCapacityCounts& counts)
{
    counts = RtPathTraceAccelCpuCapacityCounts();
    if (snapshot.residentBacked)
    {
        if (!snapshot.residentPayload || !snapshot.residentPayload->complete)
            return false;
        const RtPathTraceAccelCpuResidentPayload& payload =
            *snapshot.residentPayload;
#define SET_RESIDENT_COUNT(family, value) \
        counts.values[family] = static_cast<std::size_t>(value)
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_RIGID_PLAN_INSTANCES,
            snapshot.rigidRoute.plan.instances.size());
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_RIGID_PLAN_TRUNCATED_IDS,
            snapshot.rigidRoute.plan.planTruncatedInstanceIds.size());
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_RIGID_MATERIAL_IDS,
            snapshot.rigidRoute.materialTableIds.size());
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_RIGID_MESH_ROWS,
            snapshot.rigidRoute.meshes.size());
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_RIGID_INSTANCE_ELIGIBILITY,
            snapshot.rigidRoute.instanceEligibility.size());
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_TICKET_ACTIVE_AREAS,
            snapshot.activePortalAreas.size());
        std::size_t rigidVertices = 0;
        std::size_t rigidIndexes = 0;
        for (const RtPathTraceRigidRouteMeshSnapshot& mesh :
             payload.rigidRoute.meshes)
        {
            if (mesh.vertices.size() > RT_PT_ACCEL_CPU_SLOT_MAX_BYTES - rigidVertices ||
                mesh.indexes.size() > RT_PT_ACCEL_CPU_SLOT_MAX_BYTES - rigidIndexes)
                return false;
            rigidVertices += mesh.vertices.size();
            rigidIndexes += mesh.indexes.size();
        }
        const std::size_t rigidTriangles = rigidIndexes / 3u;
        const std::size_t surfaces = payload.staticBucket.surfaces.size();
        const std::size_t staticTriangles =
            payload.staticBucket.triangleClasses.size();
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_VERTICES, rigidVertices);
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_INDEXES, rigidIndexes);
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRIANGLE_MATERIALS,
            rigidTriangles);
        SET_RESIDENT_COUNT(
            RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRIANGLE_MATERIAL_INDEXES,
            rigidTriangles);
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRIANGLE_FLAGS,
            rigidTriangles);
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_RANGES,
            payload.rigidRoute.meshes.size());
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_INSTANCES,
            snapshot.rigidRoute.plan.instances.size());
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRANSFORMS,
            snapshot.rigidRoute.plan.instances.size());
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_SEEN,
            snapshot.rigidRoute.plan.instances.size());
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_STATIC_BUCKETS, surfaces);
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_STATIC_ASSIGNMENTS, surfaces);
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_BUCKETS, surfaces);
        if (payload.staticBucket.vertices.size() >
            RT_PT_ACCEL_CPU_SLOT_MAX_BYTES / sizeof(PathTraceSmokeVertex))
            return false;
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_VERTEX_BYTES,
            payload.staticBucket.vertices.size() * sizeof(PathTraceSmokeVertex));
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_INDEXES,
            payload.staticBucket.indexes.size());
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_TRIANGLE_CLASSES,
            staticTriangles);
        if (surfaces > (RT_PT_ACCEL_CPU_SLOT_MAX_BYTES - staticTriangles) / 4u)
            return false;
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_CLASS_WORDS,
            staticTriangles + surfaces * 4u);
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_TRIANGLE_MATERIALS,
            staticTriangles);
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_SURFACE_RECORDS,
            surfaces);
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_IDENTITIES,
            staticTriangles);
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_SCRATCH_STATIC_SORT, surfaces);
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_SCRATCH_RIGID_MESH_HASHES,
            snapshot.rigidRoute.plan.instances.size());
        SET_RESIDENT_COUNT(RT_PT_ACCEL_FAMILY_SCRATCH_STATIC_SURFACES, surfaces);
#undef SET_RESIDENT_COUNT
        return true;
    }
#define SET_COUNT(family, value) counts.values[family] = static_cast<std::size_t>(value)
    SET_COUNT(RT_PT_ACCEL_FAMILY_ACCEL_VERTEX_BYTES,
        snapshot.acceleration.staticSignature.vertexBytes.size());
    SET_COUNT(RT_PT_ACCEL_FAMILY_ACCEL_INDEXES,
        snapshot.acceleration.staticSignature.indexes.size());
    SET_COUNT(RT_PT_ACCEL_FAMILY_ACCEL_TRIANGLE_CLASSES,
        snapshot.acceleration.staticSignature.triangleClasses.size());
    SET_COUNT(RT_PT_ACCEL_FAMILY_ACCEL_TRIANGLE_MATERIALS,
        snapshot.acceleration.staticSignature.triangleMaterials.size());
    SET_COUNT(RT_PT_ACCEL_FAMILY_RIGID_PLAN_INSTANCES,
        snapshot.rigidRoute.plan.instances.size());
    SET_COUNT(RT_PT_ACCEL_FAMILY_RIGID_PLAN_TRUNCATED_IDS,
        snapshot.rigidRoute.plan.planTruncatedInstanceIds.size());
    SET_COUNT(RT_PT_ACCEL_FAMILY_RIGID_MATERIAL_IDS,
        snapshot.rigidRoute.materialTableIds.size());
    SET_COUNT(RT_PT_ACCEL_FAMILY_RIGID_MESH_ROWS, snapshot.rigidRoute.meshes.size());
    SET_COUNT(RT_PT_ACCEL_FAMILY_RIGID_INSTANCE_ELIGIBILITY,
        snapshot.rigidRoute.instanceEligibility.size());
    std::size_t rigidVertices = 0;
    std::size_t rigidIndexes = 0;
    for (const RtPathTraceRigidRouteMeshSnapshot& mesh : snapshot.rigidRoute.meshes)
    {
        if (mesh.vertices.size() > RT_PT_ACCEL_CPU_SLOT_MAX_BYTES - rigidVertices ||
            mesh.indexes.size() > RT_PT_ACCEL_CPU_SLOT_MAX_BYTES - rigidIndexes)
            return false;
        rigidVertices += mesh.vertices.size();
        rigidIndexes += mesh.indexes.size();
    }
    SET_COUNT(RT_PT_ACCEL_FAMILY_RIGID_MESH_VERTICES, rigidVertices);
    SET_COUNT(RT_PT_ACCEL_FAMILY_RIGID_MESH_INDEXES, rigidIndexes);
    SET_COUNT(RT_PT_ACCEL_FAMILY_STATIC_SURFACES, snapshot.staticBucket.surfaces.size());
    SET_COUNT(RT_PT_ACCEL_FAMILY_STATIC_VERTICES, snapshot.staticBucket.vertices.size());
    SET_COUNT(RT_PT_ACCEL_FAMILY_STATIC_INDEXES, snapshot.staticBucket.indexes.size());
    SET_COUNT(RT_PT_ACCEL_FAMILY_STATIC_TRIANGLE_CLASSES,
        snapshot.staticBucket.triangleClasses.size());
    SET_COUNT(RT_PT_ACCEL_FAMILY_STATIC_TRIANGLE_MATERIALS,
        snapshot.staticBucket.triangleMaterials.size());

    const std::size_t triangles = rigidIndexes / 3u;
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_VERTICES, rigidVertices);
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_INDEXES, rigidIndexes);
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRIANGLE_MATERIALS, triangles);
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRIANGLE_MATERIAL_INDEXES, triangles);
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRIANGLE_FLAGS, triangles);
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_RANGES,
        snapshot.rigidRoute.meshes.size());
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_INSTANCES,
        snapshot.rigidRoute.plan.instances.size());
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRANSFORMS,
        snapshot.rigidRoute.plan.instances.size());
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_SEEN,
        snapshot.rigidRoute.plan.instances.size());
    const std::size_t surfaces = snapshot.staticBucket.surfaces.size();
    const std::size_t staticTriangles = snapshot.staticBucket.triangleClasses.size();
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_STATIC_BUCKETS, surfaces);
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_STATIC_ASSIGNMENTS, surfaces);
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_BUCKETS, surfaces);
    if (snapshot.staticBucket.vertices.size() >
        RT_PT_ACCEL_CPU_SLOT_MAX_BYTES / sizeof(PathTraceSmokeVertex)) return false;
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_VERTEX_BYTES,
        snapshot.staticBucket.vertices.size() * sizeof(PathTraceSmokeVertex));
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_INDEXES,
        snapshot.staticBucket.indexes.size());
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_TRIANGLE_CLASSES, staticTriangles);
    if (surfaces > (RT_PT_ACCEL_CPU_SLOT_MAX_BYTES - staticTriangles) / 4u)
        return false;
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_CLASS_WORDS,
        staticTriangles + surfaces * 4u);
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_TRIANGLE_MATERIALS, staticTriangles);
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_SURFACE_RECORDS, surfaces);
    SET_COUNT(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_IDENTITIES, staticTriangles);
    SET_COUNT(RT_PT_ACCEL_FAMILY_SCRATCH_STATIC_SORT, surfaces);
    SET_COUNT(RT_PT_ACCEL_FAMILY_SCRATCH_RIGID_MESH_HASHES,
        snapshot.rigidRoute.plan.instances.size());
#undef SET_COUNT
    return true;
}

bool StampPathTraceAccelCpuCapacities(
    const RtPathTraceAccelCpuSnapshot& snapshot,
    const RtPathTraceAccelCpuProduct* product,
    RtPathTraceAccelCpuCapacityStamp& stamp)
{
    stamp = RtPathTraceAccelCpuCapacityStamp();
#define STAMP(family, value) stamp.capacities[family] = static_cast<std::size_t>(value)
    STAMP(RT_PT_ACCEL_FAMILY_ACCEL_VERTEX_BYTES,
        snapshot.acceleration.staticSignature.vertexBytes.capacity());
    STAMP(RT_PT_ACCEL_FAMILY_ACCEL_INDEXES,
        snapshot.acceleration.staticSignature.indexes.capacity());
    STAMP(RT_PT_ACCEL_FAMILY_ACCEL_TRIANGLE_CLASSES,
        snapshot.acceleration.staticSignature.triangleClasses.capacity());
    STAMP(RT_PT_ACCEL_FAMILY_ACCEL_TRIANGLE_MATERIALS,
        snapshot.acceleration.staticSignature.triangleMaterials.capacity());
    STAMP(RT_PT_ACCEL_FAMILY_RIGID_PLAN_INSTANCES,
        snapshot.rigidRoute.plan.instances.capacity());
    STAMP(RT_PT_ACCEL_FAMILY_RIGID_PLAN_TRUNCATED_IDS,
        snapshot.rigidRoute.plan.planTruncatedInstanceIds.capacity());
    STAMP(RT_PT_ACCEL_FAMILY_RIGID_MATERIAL_IDS,
        snapshot.rigidRoute.materialTableIds.capacity());
    STAMP(RT_PT_ACCEL_FAMILY_RIGID_MESH_ROWS, snapshot.rigidRoute.meshes.capacity());
    STAMP(RT_PT_ACCEL_FAMILY_RIGID_INSTANCE_ELIGIBILITY,
        snapshot.rigidRoute.instanceEligibility.capacity());
    for (const RtPathTraceRigidRouteMeshSnapshot& mesh : snapshot.rigidRoute.meshes)
    {
        stamp.capacities[RT_PT_ACCEL_FAMILY_RIGID_MESH_VERTICES] += mesh.vertices.capacity();
        stamp.capacities[RT_PT_ACCEL_FAMILY_RIGID_MESH_INDEXES] += mesh.indexes.capacity();
    }
    STAMP(RT_PT_ACCEL_FAMILY_STATIC_SURFACES, snapshot.staticBucket.surfaces.capacity());
    STAMP(RT_PT_ACCEL_FAMILY_STATIC_VERTICES, snapshot.staticBucket.vertices.capacity());
    STAMP(RT_PT_ACCEL_FAMILY_STATIC_INDEXES, snapshot.staticBucket.indexes.capacity());
    STAMP(RT_PT_ACCEL_FAMILY_STATIC_TRIANGLE_CLASSES,
        snapshot.staticBucket.triangleClasses.capacity());
    STAMP(RT_PT_ACCEL_FAMILY_STATIC_TRIANGLE_MATERIALS,
        snapshot.staticBucket.triangleMaterials.capacity());
    STAMP(RT_PT_ACCEL_FAMILY_TICKET_ACTIVE_AREAS,
        snapshot.activePortalAreas.capacity());
    if (product)
    {
        STAMP(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_VERTICES, product->rigidBuild.vertices.capacity());
        STAMP(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_INDEXES, product->rigidBuild.indexes.capacity());
        STAMP(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRIANGLE_MATERIALS,
            product->rigidBuild.triangleMaterials.capacity());
        STAMP(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRIANGLE_MATERIAL_INDEXES,
            product->rigidBuild.triangleMaterialIndexes.capacity());
        STAMP(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRIANGLE_FLAGS,
            product->rigidBuild.triangleClassAndFlags.capacity());
        STAMP(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_RANGES,
            product->rigidBuild.geometryRanges.capacity());
        STAMP(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_INSTANCES,
            product->rigidBuild.instances.capacity());
        STAMP(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_TRANSFORMS,
            product->rigidBuild.instanceObjectToWorld.capacity());
        STAMP(RT_PT_ACCEL_FAMILY_PRODUCT_RIGID_SEEN,
            product->rigidBuild.instanceSeenThisFrame.capacity());
        STAMP(RT_PT_ACCEL_FAMILY_PRODUCT_STATIC_BUCKETS,
            product->staticAssignment.buckets.capacity());
        STAMP(RT_PT_ACCEL_FAMILY_PRODUCT_STATIC_ASSIGNMENTS,
            product->staticAssignment.assignments.capacity());
        STAMP(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_BUCKETS, product->staticPack.buckets.capacity());
        STAMP(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_VERTEX_BYTES,
            product->staticPack.vertexBytes.capacity());
        STAMP(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_INDEXES, product->staticPack.indexes.capacity());
        STAMP(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_TRIANGLE_CLASSES,
            product->staticPack.triangleClasses.capacity());
        STAMP(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_CLASS_WORDS,
            product->staticPack.staticClassMetadataWords.capacity());
        STAMP(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_TRIANGLE_MATERIALS,
            product->staticPack.triangleMaterials.capacity());
        STAMP(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_SURFACE_RECORDS,
            product->staticPack.surfaceRecords.capacity());
        STAMP(RT_PT_ACCEL_FAMILY_PRODUCT_PACK_IDENTITIES,
            product->staticPack.triangleIdentities.capacity());
    }
#undef STAMP
    for (std::uint32_t index = 0; index < RT_PT_ACCEL_FAMILY_COUNT; ++index)
    {
        const RtPathTraceAccelCpuVectorFamily family =
            static_cast<RtPathTraceAccelCpuVectorFamily>(index);
        if (FamilyIsScratch(family)) continue;
        const std::size_t elementSize = FamilyElementSize(family);
        if (stamp.capacities[index] > RT_PT_ACCEL_CPU_SLOT_MAX_BYTES / elementSize)
            return false;
        const std::size_t bytes = stamp.capacities[index] * elementSize;
        std::size_t& subtotal = FamilyIsSnapshot(family)
            ? stamp.snapshotBytes : stamp.productBytes;
        if (bytes > RT_PT_ACCEL_CPU_SLOT_MAX_BYTES - subtotal) return false;
        subtotal += bytes;
    }
    if (stamp.snapshotBytes > RT_PT_ACCEL_CPU_SLOT_MAX_BYTES - stamp.productBytes)
        return false;
    stamp.residentBytes = stamp.snapshotBytes + stamp.productBytes;
    return true;
}

bool PathTraceAccelCpuCapacityStampWithinPlan(
    const RtPathTraceAccelCpuCapacityStamp& stamp,
    const RtPathTraceAccelCpuCapacityPlan& plan)
{
    if (!plan.valid || stamp.residentBytes > RT_PT_ACCEL_CPU_SLOT_MAX_BYTES)
        return false;
    for (std::uint32_t index = 0; index < RT_PT_ACCEL_FAMILY_COUNT; ++index)
    {
        if (FamilyIsScratch(static_cast<RtPathTraceAccelCpuVectorFamily>(index)))
            continue;
        if (stamp.capacities[index] > plan.counts.values[index]) return false;
    }
    return true;
}

bool PathTraceAccelCpuEpochValid(
    const RtPathTracePlanningSnapshotEpoch& epoch)
{
    // Lane B snapshots after the owner Harvest has finalized the rigid inputs.
    // Unlike Lane A, "before serial mutate" is therefore deliberately false.
    return epoch.generation != 0 && epoch.capturedAfterBeginFrame &&
        epoch.capturedAfterStaticPreload &&
        !epoch.capturedBeforeSerialMutate;
}

std::size_t RtPathTraceStaticBucketCpuSnapshot::OwnedBytes() const
{
    return VectorBytes(surfaces) + VectorBytes(vertices) + VectorBytes(indexes) +
        VectorBytes(triangleClasses) + VectorBytes(triangleMaterials);
}

void RtPathTraceStaticBucketCpuSnapshot::ResetAndRelease()
{
    *this = RtPathTraceStaticBucketCpuSnapshot();
}

std::size_t RtPathTraceAccelCpuResidentPayload::OwnedBytes() const
{
    std::size_t bytes = staticBucket.OwnedBytes();
    bytes += VectorBytes(acceleration.staticSignature.vertexBytes) +
        VectorBytes(acceleration.staticSignature.indexes) +
        VectorBytes(acceleration.staticSignature.triangleClasses) +
        VectorBytes(acceleration.staticSignature.triangleMaterials);
    bytes += VectorBytes(rigidRoute.meshes);
    for (const RtPathTraceRigidRouteMeshSnapshot& mesh : rigidRoute.meshes)
        bytes += VectorBytes(mesh.vertices) + VectorBytes(mesh.indexes);
    return bytes;
}

bool BuildPathTraceAccelCpuResidentCapacityPlan(
    const RtSmokeAccelerationPlanSnapshotCounts& acceleration,
    const RtPathTraceRigidRouteResidentMeshPayloadCounts& rigidRoute,
    const RtPathTraceStaticBucketCpuSnapshotCounts& staticBucket,
    RtPathTraceAccelCpuResidentCapacityPlan& plan)
{
    plan = RtPathTraceAccelCpuResidentCapacityPlan();
    plan.acceleration = acceleration;
    plan.rigidRoute = rigidRoute;
    plan.staticBucket = staticBucket;
    std::size_t bytes = 0;
    const bool valid =
        AddResidentArrayBytes(bytes, acceleration.vertexBytes,
            sizeof(std::uint8_t)) &&
        AddResidentArrayBytes(bytes, acceleration.indexes,
            sizeof(std::uint32_t)) &&
        AddResidentArrayBytes(bytes, acceleration.triangleClasses,
            sizeof(std::uint32_t)) &&
        AddResidentArrayBytes(bytes, acceleration.triangleMaterials,
            sizeof(std::uint32_t)) &&
        AddResidentArrayBytes(bytes, rigidRoute.meshes,
            sizeof(RtPathTraceRigidRouteMeshSnapshot)) &&
        AddResidentArrayBytes(bytes, rigidRoute.meshVertices,
            sizeof(PathTraceSmokeVertex)) &&
        AddResidentArrayBytes(bytes, rigidRoute.meshIndexes,
            sizeof(std::uint32_t)) &&
        AddResidentArrayBytes(bytes, staticBucket.surfaces,
            sizeof(RtSmokeStaticBucketAssignmentSurface)) &&
        AddResidentArrayBytes(bytes, staticBucket.vertices,
            sizeof(PathTraceSmokeVertex)) &&
        AddResidentArrayBytes(bytes, staticBucket.indexes,
            sizeof(std::uint32_t)) &&
        AddResidentArrayBytes(bytes, staticBucket.triangleClasses,
            sizeof(std::uint32_t)) &&
        AddResidentArrayBytes(bytes, staticBucket.triangleMaterials,
            sizeof(std::uint32_t));
    plan.plannedBytes = bytes;
    plan.valid = valid;
    return valid;
}

bool RtPathTraceAccelCpuResidentBackingStore::Publish(
    std::uint64_t rigidPayloadSignature,
    std::uint64_t staticPayloadSignature,
    RtPathTraceAccelCpuResidentPreflightFn preflight,
    RtPathTraceAccelCpuResidentFillFn fill,
    void* context,
    std::shared_ptr<const RtPathTraceAccelCpuResidentPayload>& lease)
{
    const auto start = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    lease.reset();
    const auto noteTime = [this, start]()
    {
        stats_.publishUs += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count());
    };
    if (retired_ && retired_.use_count() == 1)
        retired_.reset();

    const bool reuse = current_ && current_->complete &&
        current_->rigidPayloadSignature == rigidPayloadSignature &&
        current_->staticPayloadSignature == staticPayloadSignature;
    if (reuse)
    {
        ++stats_.reuses;
        lease = current_;
        stats_.residentBytes = current_->OwnedBytes();
        stats_.cowBytes = retired_ ? retired_->OwnedBytes() : 0;
        noteTime();
        return true;
    }

    // A leased retired backing plus current plus a candidate would be three
    // complete world payloads. Reject before Count or allocation.
    if (retired_ && retired_.use_count() > 1)
    {
        ++stats_.leaseRejects;
        ++stats_.failures;
        noteTime();
        return false;
    }

    RtPathTraceAccelCpuResidentCapacityPlan plan;
    try
    {
        if (!preflight || !fill || !preflight(context, plan) || !plan.valid)
        {
            ++stats_.fillFailures;
            ++stats_.failures;
            stats_.attemptedResidentBytes = std::max(
                stats_.attemptedResidentBytes, plan.plannedBytes);
            noteTime();
            return false;
        }
    }
    catch (const std::bad_alloc&)
    {
        ++stats_.allocationFailures;
        ++stats_.failures;
        noteTime();
        return false;
    }
    catch (const std::length_error&)
    {
        ++stats_.allocationFailures;
        ++stats_.failures;
        noteTime();
        return false;
    }
    stats_.attemptedResidentBytes = std::max(
        stats_.attemptedResidentBytes, plan.plannedBytes);
    if (plan.plannedBytes > RT_PT_ACCEL_CPU_RESIDENT_BACKING_MAX_BYTES)
    {
        ++stats_.residentCapRejects;
        ++stats_.failures;
        noteTime();
        return false;
    }

    const bool movedCurrentToRetired = current_ && current_.use_count() > 1;
    if (movedCurrentToRetired)
        retired_ = std::move(current_);
    const auto restoreMovedCurrent = [this, movedCurrentToRetired]()
    {
        if (movedCurrentToRetired && !current_)
        {
            current_ = retired_;
            retired_.reset();
        }
    };

    try
    {
        PathTraceAccelCpuPackMaybeInjectAllocationFailure();
        auto candidate = std::make_shared<RtPathTraceAccelCpuResidentPayload>();
        if (!fill(context, plan, *candidate))
        {
            ++stats_.fillFailures;
            ++stats_.failures;
            restoreMovedCurrent();
            noteTime();
            return false;
        }
        const std::size_t actualBytes = candidate->OwnedBytes();
        const std::size_t retiredBytes = retired_ ? retired_->OwnedBytes() : 0;
        if (actualBytes > plan.plannedBytes ||
            actualBytes > RT_PT_ACCEL_CPU_RESIDENT_BACKING_MAX_BYTES ||
            retiredBytes > RT_PT_ACCEL_CPU_RESIDENT_TOTAL_MAX_BYTES - actualBytes)
        {
            ++stats_.residentCapRejects;
            ++stats_.failures;
            restoreMovedCurrent();
            noteTime();
            return false;
        }
        candidate->generation = nextGeneration_++;
        candidate->rigidPayloadSignature = rigidPayloadSignature;
        candidate->staticPayloadSignature = staticPayloadSignature;
        candidate->complete = true;
        current_ = std::move(candidate);
        ++stats_.publishes;
        if (movedCurrentToRetired)
            ++stats_.cowPublishes;
        stats_.residentBytes = current_->OwnedBytes();
        stats_.cowBytes = retired_ ? retired_->OwnedBytes() : 0;
        stats_.highWaterBytes = std::max(stats_.highWaterBytes,
            stats_.residentBytes + stats_.cowBytes);
        lease = current_;
        noteTime();
        return true;
    }
    catch (const std::bad_alloc&)
    {
        ++stats_.allocationFailures;
    }
    catch (const std::length_error&)
    {
        ++stats_.allocationFailures;
    }
    ++stats_.failures;
    restoreMovedCurrent();
    noteTime();
    return false;
}

void RtPathTraceAccelCpuResidentBackingStore::Reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    current_.reset();
    retired_.reset();
    ++nextGeneration_;
    stats_.residentBytes = 0;
    stats_.cowBytes = 0;
}

RtPathTraceAccelCpuResidentPublishStats
RtPathTraceAccelCpuResidentBackingStore::Stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void RtPathTraceAccelCpuResidentBackingStore::NoteTicketValidationFailure()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.ticketValidationFailures;
}

void RtPathTraceAccelCpuResidentBackingStore::NoteTicketBuildFailure()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.ticketBuildFailures;
}

std::size_t RtPathTraceAccelCpuSnapshot::OwnedBytes() const
{
    std::size_t bytes = staticBucket.OwnedBytes();
    bytes += VectorBytes(acceleration.staticSignature.vertexBytes) +
        VectorBytes(acceleration.staticSignature.indexes) +
        VectorBytes(acceleration.staticSignature.triangleClasses) +
        VectorBytes(acceleration.staticSignature.triangleMaterials);
    bytes += VectorBytes(rigidRoute.materialTableIds);
    bytes += VectorBytes(rigidRoute.plan.instances) +
        VectorBytes(rigidRoute.plan.planTruncatedInstanceIds) +
        VectorBytes(rigidRoute.instanceEligibility);
    bytes += VectorBytes(rigidRoute.meshes);
    for (const RtPathTraceRigidRouteMeshSnapshot& mesh : rigidRoute.meshes)
    {
        bytes += VectorBytes(mesh.vertices) + VectorBytes(mesh.indexes);
    }
    bytes += VectorBytes(activePortalAreas);
    return bytes;
}

void RtPathTraceAccelCpuSnapshot::ResetAndRelease()
{
    *this = RtPathTraceAccelCpuSnapshot();
}

std::size_t RtPathTraceAccelCpuProduct::OwnedBytes() const
{
    std::size_t bytes = 0;
    bytes += VectorBytes(rigidBuild.vertices) + VectorBytes(rigidBuild.indexes) +
        VectorBytes(rigidBuild.triangleMaterials) +
        VectorBytes(rigidBuild.triangleMaterialIndexes) +
        VectorBytes(rigidBuild.triangleClassAndFlags) +
        VectorBytes(rigidBuild.geometryRanges) +
        VectorBytes(rigidBuild.instances) +
        VectorBytes(rigidBuild.instanceObjectToWorld) +
        VectorBytes(rigidBuild.instanceSeenThisFrame);
    bytes += VectorBytes(staticAssignment.buckets) +
        VectorBytes(staticAssignment.assignments);
    bytes += VectorBytes(staticPack.buckets) + VectorBytes(staticPack.vertexBytes) +
        VectorBytes(staticPack.indexes) + VectorBytes(staticPack.triangleClasses) +
        VectorBytes(staticPack.staticClassMetadataWords) +
        VectorBytes(staticPack.triangleMaterials) +
        VectorBytes(staticPack.surfaceRecords) +
        VectorBytes(staticPack.triangleIdentities);
    return bytes;
}

void RtPathTraceAccelCpuProduct::ResetAndRelease()
{
    *this = RtPathTraceAccelCpuProduct();
}

bool PathTraceAccelCpuTokensCompatible(
    const RtPathTraceAccelCpuCompatibilityToken& product,
    const RtPathTraceAccelCpuCompatibilityToken& current)
{
    bool mapNameEqual = true;
    for (std::size_t i = 0; i < RT_PT_PLANNING_MAP_NAME_CAPACITY; ++i)
    {
        if (product.mapName[i] != current.mapName[i])
        {
            mapNameEqual = false;
            break;
        }
        if (product.mapName[i] == '\0') break;
    }
    return product.mapTimeStamp == current.mapTimeStamp &&
        product.mapLoadSerial == current.mapLoadSerial && mapNameEqual &&
        product.lifecycleEpoch == current.lifecycleEpoch &&
        product.configFingerprint == current.configFingerprint;
}

std::uint64_t BuildPathTraceAccelCpuInputReceipt(
    const RtPathTraceAccelCpuSnapshot& snapshot)
{
    std::uint64_t hash = kFnvOffset;
    if (snapshot.residentBacked)
    {
        if (!snapshot.residentPayload || !snapshot.residentPayload->complete)
            return 0;
        hash = HashBytes(hash, &snapshot.residentGeneration,
            sizeof(snapshot.residentGeneration));
        hash = HashBytes(hash, &snapshot.rigidPayloadSignature,
            sizeof(snapshot.rigidPayloadSignature));
        hash = HashBytes(hash, &snapshot.staticPayloadSignature,
            sizeof(snapshot.staticPayloadSignature));
#define HASH_RESIDENT_CACHE_FIELD(field) \
        hash = HashBytes(hash, &snapshot.acceleration.staticCache.field, \
            sizeof(snapshot.acceleration.staticCache.field))
        HASH_RESIDENT_CACHE_FIELD(hasStaticBlas);
        HASH_RESIDENT_CACHE_FIELD(cacheValid);
        HASH_RESIDENT_CACHE_FIELD(cacheResourcesReady);
        HASH_RESIDENT_CACHE_FIELD(staticCacheChanged);
        HASH_RESIDENT_CACHE_FIELD(previousSignatureHash);
        HASH_RESIDENT_CACHE_FIELD(opacitySignature);
#undef HASH_RESIDENT_CACHE_FIELD
        hash = HashBytes(hash, &snapshot.acceleration.staticVertexCount,
            sizeof(snapshot.acceleration.staticVertexCount));
        hash = HashBytes(hash, &snapshot.acceleration.staticIndexCount,
            sizeof(snapshot.acceleration.staticIndexCount));
        hash = HashBytes(hash, &snapshot.acceleration.dynamicVertexCount,
            sizeof(snapshot.acceleration.dynamicVertexCount));
        hash = HashBytes(hash, &snapshot.acceleration.dynamicIndexCount,
            sizeof(snapshot.acceleration.dynamicIndexCount));
        hash = HashBytes(hash, &snapshot.staticBucket.worldGeneration,
            sizeof(snapshot.staticBucket.worldGeneration));
        hash = HashBytes(hash, &snapshot.staticBucket.sourceGeneration,
            sizeof(snapshot.staticBucket.sourceGeneration));
        hash = HashBytes(hash, &snapshot.staticBucket.storageGeneration,
            sizeof(snapshot.staticBucket.storageGeneration));
        hash = HashBytes(hash, &snapshot.staticBucket.portalAreaCount,
            sizeof(snapshot.staticBucket.portalAreaCount));
        hash = HashBytes(hash, &snapshot.staticBucket.maxVerticesPerBucket,
            sizeof(snapshot.staticBucket.maxVerticesPerBucket));
        hash = HashBytes(hash, &snapshot.staticBucket.maxIndexesPerBucket,
            sizeof(snapshot.staticBucket.maxIndexesPerBucket));
        hash = HashBytes(hash, &snapshot.staticBucket.maxTrianglesPerBucket,
            sizeof(snapshot.staticBucket.maxTrianglesPerBucket));
        if (!snapshot.activePortalAreas.empty())
            hash = HashBytes(hash, snapshot.activePortalAreas.data(),
                snapshot.activePortalAreas.size());
    }
    for (std::size_t instanceIndex = 0;
         instanceIndex < snapshot.rigidRoute.plan.instances.size(); ++instanceIndex)
    {
        const RtSmokePlanTlasInstance& instance =
            snapshot.rigidRoute.plan.instances[instanceIndex];
        // Exact-frame transforms are refreshed on the owner after selecting a
        // Lane-B product. They intentionally do not invalidate an age-1 CPU
        // packing result.
#define HASH_INSTANCE_FIELD(field) \
        hash = HashBytes(hash, &instance.field, sizeof(instance.field))
        HASH_INSTANCE_FIELD(kind);
        HASH_INSTANCE_FIELD(instanceId);
        HASH_INSTANCE_FIELD(instanceMask);
        HASH_INSTANCE_FIELD(hitGroupContribution);
        HASH_INSTANCE_FIELD(flags);
        HASH_INSTANCE_FIELD(meshHash);
        HASH_INSTANCE_FIELD(sourceInstanceId);
        HASH_INSTANCE_FIELD(worldToken);
        HASH_INSTANCE_FIELD(worldGeneration);
        HASH_INSTANCE_FIELD(renderDefIndex);
        HASH_INSTANCE_FIELD(renderDefGeneration);
        HASH_INSTANCE_FIELD(materialId);
        HASH_INSTANCE_FIELD(routeRecordIndex);
        HASH_INSTANCE_FIELD(canonicalBlasRecordIndex);
        HASH_INSTANCE_FIELD(canonicalMeshHash);
        HASH_INSTANCE_FIELD(sourceSeenThisFrame);
#undef HASH_INSTANCE_FIELD
        if (instanceIndex < snapshot.rigidRoute.instanceEligibility.size())
        {
            const RtPathTraceRigidRouteInstanceEligibility& eligibility =
                snapshot.rigidRoute.instanceEligibility[instanceIndex];
            hash = HashBytes(hash, &eligibility.transformUsable,
                sizeof(eligibility.transformUsable));
            hash = HashBytes(hash, &eligibility.worldBoundsValid,
                sizeof(eligibility.worldBoundsValid));
        }
    }
    if (!snapshot.rigidRoute.materialTableIds.empty())
    {
        hash = HashBytes(hash, snapshot.rigidRoute.materialTableIds.data(),
            snapshot.rigidRoute.materialTableIds.size() *
                sizeof(snapshot.rigidRoute.materialTableIds[0]));
    }
    if (snapshot.residentBacked)
        return hash;
    for (const RtPathTraceRigidRouteMeshSnapshot& mesh : snapshot.rigidRoute.meshes)
    {
        hash = HashBytes(hash, &mesh.routeRecordIndex,
            sizeof(mesh.routeRecordIndex));
        hash = HashBytes(hash, &mesh.meshHash, sizeof(mesh.meshHash));
        hash = HashBytes(hash, &mesh.cpuMeshContentSignature,
            sizeof(mesh.cpuMeshContentSignature));
        hash = HashBytes(hash, &mesh.gpuUploadSignature,
            sizeof(mesh.gpuUploadSignature));
        hash = HashBytes(hash, &mesh.materialId, sizeof(mesh.materialId));
        hash = HashBytes(hash, &mesh.surfaceClassId,
            sizeof(mesh.surfaceClassId));
        hash = HashBytes(hash, &mesh.triangleClassAndFlags,
            sizeof(mesh.triangleClassAndFlags));
        hash = HashBytes(hash, &mesh.valid, sizeof(mesh.valid));
        hash = HashBytes(hash, &mesh.routeReady, sizeof(mesh.routeReady));
        hash = HashBytes(hash, &mesh.localBoundsValid,
            sizeof(mesh.localBoundsValid));
        for (int boundIndex = 0; boundIndex < 2; ++boundIndex)
        {
            hash = HashBytes(hash, &mesh.localBounds[boundIndex].x,
                sizeof(mesh.localBounds[boundIndex].x));
            hash = HashBytes(hash, &mesh.localBounds[boundIndex].y,
                sizeof(mesh.localBounds[boundIndex].y));
            hash = HashBytes(hash, &mesh.localBounds[boundIndex].z,
                sizeof(mesh.localBounds[boundIndex].z));
        }
        hash = HashBytes(hash, &mesh.vertexCount, sizeof(mesh.vertexCount));
        hash = HashBytes(hash, &mesh.indexCount, sizeof(mesh.indexCount));
        if (!mesh.vertices.empty())
            hash = HashBytes(hash, mesh.vertices.data(),
                mesh.vertices.size() * sizeof(mesh.vertices[0]));
        if (!mesh.indexes.empty())
            hash = HashBytes(hash, mesh.indexes.data(),
                mesh.indexes.size() * sizeof(mesh.indexes[0]));
    }
    const std::uint64_t acceleration = [&snapshot]() {
        RtSmokeAccelerationPlanInput input;
        input.staticSignature.vertices =
            snapshot.acceleration.staticSignature.vertexBytes.empty()
                ? nullptr : snapshot.acceleration.staticSignature.vertexBytes.data();
        input.staticSignature.vertexStride =
            snapshot.acceleration.staticSignature.vertexStride;
        input.staticSignature.totalVertexCount =
            snapshot.acceleration.staticSignature.totalVertexCount;
        input.staticSignature.indexes =
            snapshot.acceleration.staticSignature.indexes.empty()
                ? nullptr : snapshot.acceleration.staticSignature.indexes.data();
        input.staticSignature.totalIndexCount = static_cast<int>(
            snapshot.acceleration.staticSignature.indexes.size());
        input.staticSignature.triangleClasses =
            snapshot.acceleration.staticSignature.triangleClasses.empty()
                ? nullptr : snapshot.acceleration.staticSignature.triangleClasses.data();
        input.staticSignature.triangleMaterials =
            snapshot.acceleration.staticSignature.triangleMaterials.empty()
                ? nullptr : snapshot.acceleration.staticSignature.triangleMaterials.data();
        input.staticSignature.totalTriangleCount = static_cast<int>(std::min(
            snapshot.acceleration.staticSignature.triangleClasses.size(),
            snapshot.acceleration.staticSignature.triangleMaterials.size()));
        input.staticSignature.staticRange =
            snapshot.acceleration.staticSignature.staticRange;
        input.staticCache = snapshot.acceleration.staticCache;
        input.staticVertexCount = snapshot.acceleration.staticVertexCount;
        input.staticIndexCount = snapshot.acceleration.staticIndexCount;
        input.dynamicVertexCount = snapshot.acceleration.dynamicVertexCount;
        input.dynamicIndexCount = snapshot.acceleration.dynamicIndexCount;
        return BuildSmokeAccelerationPlanInputToken(input);
    }();
    hash = HashBytes(hash, &acceleration, sizeof(acceleration));
    hash = HashBytes(hash, &snapshot.staticBucket.worldGeneration,
        sizeof(snapshot.staticBucket.worldGeneration));
    hash = HashBytes(hash, &snapshot.staticBucket.sourceGeneration,
        sizeof(snapshot.staticBucket.sourceGeneration));
    hash = HashBytes(hash, &snapshot.staticBucket.storageGeneration,
        sizeof(snapshot.staticBucket.storageGeneration));
    hash = HashBytes(hash, &snapshot.staticBucket.portalAreaCount,
        sizeof(snapshot.staticBucket.portalAreaCount));
    hash = HashBytes(hash, &snapshot.staticBucket.maxVerticesPerBucket,
        sizeof(snapshot.staticBucket.maxVerticesPerBucket));
    hash = HashBytes(hash, &snapshot.staticBucket.maxIndexesPerBucket,
        sizeof(snapshot.staticBucket.maxIndexesPerBucket));
    hash = HashBytes(hash, &snapshot.staticBucket.maxTrianglesPerBucket,
        sizeof(snapshot.staticBucket.maxTrianglesPerBucket));
    for (const RtSmokeStaticBucketAssignmentSurface& surface :
         snapshot.staticBucket.surfaces)
    {
        hash = HashBytes(hash, &surface.surfaceKey, sizeof(surface.surfaceKey));
        hash = HashBytes(hash, &surface.sourceRecordIndex,
            sizeof(surface.sourceRecordIndex));
        hash = HashBytes(hash, &surface.portalArea, sizeof(surface.portalArea));
        hash = HashBytes(hash, &surface.range.vertexOffset,
            sizeof(surface.range.vertexOffset));
        hash = HashBytes(hash, &surface.range.vertexCount,
            sizeof(surface.range.vertexCount));
        hash = HashBytes(hash, &surface.range.indexOffset,
            sizeof(surface.range.indexOffset));
        hash = HashBytes(hash, &surface.range.indexCount,
            sizeof(surface.range.indexCount));
        hash = HashBytes(hash, &surface.range.triangleOffset,
            sizeof(surface.range.triangleOffset));
        hash = HashBytes(hash, &surface.range.triangleCount,
            sizeof(surface.range.triangleCount));
        hash = HashBytes(hash, &surface.valid, sizeof(surface.valid));
        hash = HashBytes(hash, &surface.active, sizeof(surface.active));
    }
    if (!snapshot.staticBucket.vertices.empty())
    {
        hash = HashBytes(hash, snapshot.staticBucket.vertices.data(),
            snapshot.staticBucket.vertices.size() *
                sizeof(snapshot.staticBucket.vertices[0]));
    }
    if (!snapshot.staticBucket.indexes.empty())
    {
        hash = HashBytes(hash, snapshot.staticBucket.indexes.data(),
            snapshot.staticBucket.indexes.size() *
                sizeof(snapshot.staticBucket.indexes[0]));
    }
    if (!snapshot.staticBucket.triangleClasses.empty())
    {
        hash = HashBytes(hash, snapshot.staticBucket.triangleClasses.data(),
            snapshot.staticBucket.triangleClasses.size() *
                sizeof(snapshot.staticBucket.triangleClasses[0]));
        hash = HashBytes(hash, snapshot.staticBucket.triangleMaterials.data(),
            snapshot.staticBucket.triangleMaterials.size() *
                sizeof(snapshot.staticBucket.triangleMaterials[0]));
    }
    return hash;
}

bool ValidatePathTraceAccelCpuSnapshot(
    const RtPathTraceAccelCpuSnapshot& snapshot)
{
    if (snapshot.residentBacked)
    {
        if (!snapshot.complete || !PathTraceAccelCpuEpochValid(snapshot.epoch) ||
            snapshot.inputReceipt == 0 || !snapshot.residentPayload ||
            !snapshot.residentPayload->complete ||
            snapshot.residentGeneration != snapshot.residentPayload->generation ||
            snapshot.rigidPayloadSignature !=
                snapshot.residentPayload->rigidPayloadSignature ||
            snapshot.staticPayloadSignature !=
                snapshot.residentPayload->staticPayloadSignature ||
            snapshot.rigidRoute.instanceEligibility.size() !=
                snapshot.rigidRoute.plan.instances.size())
            return false;
        const RtPathTraceStaticBucketCpuSnapshot& residentStatic =
            snapshot.residentPayload->staticBucket;
        if (!residentStatic.complete ||
            residentStatic.triangleClasses.size() !=
                residentStatic.triangleMaterials.size() ||
            residentStatic.indexes.size() >
                static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            residentStatic.vertices.size() >
                static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            residentStatic.surfaces.size() >
                static_cast<std::size_t>(std::numeric_limits<int>::max()))
            return false;
        RtPathTraceAccelCpuCapacityCounts counts;
        RtPathTraceAccelCpuCapacityPlan plan;
        return CountPathTraceAccelCpuCapacityFromSnapshot(snapshot, counts) &&
            BuildPathTraceAccelCpuCapacityPlan(counts, plan) &&
            snapshot.reservedProductBytes == plan.productBytes + plan.scratchBytes &&
            snapshot.plannedPeakBytes == plan.plannedPeakBytes &&
            snapshot.OwnedBytes() <= plan.snapshotBytes &&
            BuildPathTraceAccelCpuInputReceipt(snapshot) == snapshot.inputReceipt;
    }
    if (!snapshot.complete || !PathTraceAccelCpuEpochValid(snapshot.epoch) ||
        snapshot.inputReceipt == 0 || !snapshot.staticBucket.complete ||
        snapshot.staticBucket.triangleClasses.size() !=
            snapshot.staticBucket.triangleMaterials.size() ||
        snapshot.staticBucket.indexes.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        snapshot.staticBucket.vertices.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        snapshot.staticBucket.surfaces.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        snapshot.rigidRoute.instanceEligibility.size() !=
            snapshot.rigidRoute.plan.instances.size())
    {
        return false;
    }
    RtPathTraceAccelCpuCapacityCounts counts;
    RtPathTraceAccelCpuCapacityPlan plan;
    return CountPathTraceAccelCpuCapacityFromSnapshot(snapshot, counts) &&
        BuildPathTraceAccelCpuCapacityPlan(counts, plan) &&
        snapshot.reservedProductBytes == plan.productBytes + plan.scratchBytes &&
        snapshot.plannedPeakBytes == plan.plannedPeakBytes &&
        snapshot.OwnedBytes() <= plan.snapshotBytes &&
        BuildPathTraceAccelCpuInputReceipt(snapshot) == snapshot.inputReceipt;
}

bool ValidatePathTraceAccelCpuProduct(const RtPathTraceAccelCpuProduct& product)
{
    return product.complete && PathTraceAccelCpuEpochValid(product.epoch) &&
        product.inputReceipt != 0 && product.staticAssignment.exactCoverage &&
        product.staticPack.exact &&
        product.staticAssignmentSignature == product.staticAssignment.planSignature &&
        product.staticPackSignature == product.staticPack.contentSignature &&
        product.rigidGeometrySignature ==
            BuildRigidRouteGeometryUploadSignature(product.rigidBuild) &&
        product.rigidInstanceSignature ==
            BuildRigidRouteInstanceUploadSignature(product.rigidBuild);
}

bool BuildPathTraceAccelCpuProduct(
    const RtPathTraceAccelCpuSnapshot& snapshot,
    RtPathTraceAccelCpuProduct& product,
    std::size_t* peakSlotBytes)
{
    product.ResetAndRelease();
    if (peakSlotBytes) *peakSlotBytes = snapshot.plannedPeakBytes;
    if (!ValidatePathTraceAccelCpuSnapshot(snapshot)) return false;
    try
    {
        RtPathTraceAccelCpuCapacityCounts counts;
        RtPathTraceAccelCpuCapacityPlan plan;
        if (!CountPathTraceAccelCpuCapacityFromSnapshot(snapshot, counts) ||
            !BuildPathTraceAccelCpuCapacityPlan(counts, plan) ||
            snapshot.reservedProductBytes != plan.productBytes + plan.scratchBytes ||
            snapshot.plannedPeakBytes != plan.plannedPeakBytes)
            return false;
        RtPathTraceAccelCpuProduct candidate;
        std::vector<int> staticSortScratch;
        std::vector<std::uint64_t> rigidMeshScratch;
        std::vector<RtSmokeStaticBucketAssignmentSurface> staticSurfaceScratch;
        if (!ReserveAccelCpuProduct(plan, candidate,
                staticSortScratch, rigidMeshScratch, staticSurfaceScratch))
            return false;
        candidate.epoch = snapshot.epoch;
        candidate.compatibility = snapshot.compatibility;
        candidate.inputReceipt = snapshot.inputReceipt;

        const RtPathTraceRigidRouteBuildSnapshot& residentRigid =
            snapshot.residentBacked
                ? snapshot.residentPayload->rigidRoute
                : snapshot.rigidRoute;
        if (!(snapshot.residentBacked
                ? BuildRigidRouteBuffersFromResidentPayloadPreReserved(
                    residentRigid.meshes, snapshot.rigidRoute.plan,
                    snapshot.rigidRoute.materialTableIds,
                    snapshot.rigidRoute.instanceEligibility,
                    candidate.rigidBuild, rigidMeshScratch)
                : BuildRigidRouteBuffersFromSnapshotPreReserved(
                    snapshot.rigidRoute, candidate.rigidBuild,
                    rigidMeshScratch)))
            return false;
        candidate.rigidGeometrySignature =
            BuildRigidRouteGeometryUploadSignature(candidate.rigidBuild);
        candidate.rigidInstanceSignature =
            BuildRigidRouteInstanceUploadSignature(candidate.rigidBuild);

        const RtSmokeAccelerationPlanTimedResult acceleration =
            snapshot.residentBacked
                ? BuildSmokeAccelerationPlanTimedResult(
                    snapshot.residentPayload->acceleration,
                    snapshot.acceleration.staticCache,
                    snapshot.acceleration.dynamicVertexCount,
                    snapshot.acceleration.dynamicIndexCount)
                : BuildSmokeAccelerationPlanTimedResult(snapshot.acceleration);
        if (!acceleration.result.valid) return false;
        candidate.accelerationPlan = acceleration.result.plan;

        RtSmokeStaticBucketAssignmentPlanDesc assignmentDesc;
        const RtPathTraceStaticBucketCpuSnapshot& residentStatic =
            snapshot.residentBacked
                ? snapshot.residentPayload->staticBucket
                : snapshot.staticBucket;
        if (snapshot.residentBacked)
        {
            for (const RtSmokeStaticBucketAssignmentSurface& source :
                 residentStatic.surfaces)
            {
                RtSmokeStaticBucketAssignmentSurface value = source;
                if (!snapshot.activePortalAreas.empty())
                {
                    value.active = value.portalArea >= 0 &&
                        static_cast<std::size_t>(value.portalArea) <
                            snapshot.activePortalAreas.size() &&
                        snapshot.activePortalAreas[
                            static_cast<std::size_t>(value.portalArea)] != 0;
                }
                staticSurfaceScratch.push_back(value);
            }
        }
        const std::vector<RtSmokeStaticBucketAssignmentSurface>& assignmentSurfaces =
            snapshot.residentBacked ? staticSurfaceScratch : residentStatic.surfaces;
        assignmentDesc.surfaces = assignmentSurfaces.empty()
            ? nullptr : assignmentSurfaces.data();
        assignmentDesc.surfaceCount = static_cast<int>(
            assignmentSurfaces.size());
        assignmentDesc.worldGeneration = snapshot.staticBucket.worldGeneration;
        assignmentDesc.sourceGeneration = snapshot.staticBucket.sourceGeneration;
        assignmentDesc.storageGeneration = snapshot.staticBucket.storageGeneration;
        assignmentDesc.portalAreaCount = snapshot.staticBucket.portalAreaCount;
        assignmentDesc.maxVerticesPerBucket =
            snapshot.staticBucket.maxVerticesPerBucket;
        assignmentDesc.maxIndexesPerBucket =
            snapshot.staticBucket.maxIndexesPerBucket;
        assignmentDesc.maxTrianglesPerBucket =
            snapshot.staticBucket.maxTrianglesPerBucket;
        if (assignmentSurfaces.empty())
        {
            candidate.staticAssignment.planSignature = kFnvOffset;
            candidate.staticAssignment.exactCoverage = true;
            candidate.staticPack.contentSignature = kFnvOffset;
            candidate.staticPack.exact = true;
        }
        else if (!BuildSmokeStaticBucketAssignmentPlanPreReserved(
                     assignmentDesc, candidate.staticAssignment,
                     staticSortScratch) ||
                 !candidate.staticAssignment.exactCoverage)
            return false;

        RtSmokeStaticBucketGeometryPackDesc packDesc;
        packDesc.assignmentPlan = &candidate.staticAssignment;
        packDesc.vertices = residentStatic.vertices.empty()
            ? nullptr : residentStatic.vertices.data();
        packDesc.vertexStride = sizeof(PathTraceSmokeVertex);
        packDesc.totalVertexCount = static_cast<int>(
            residentStatic.vertices.size());
        packDesc.indexes = residentStatic.indexes.empty()
            ? nullptr : residentStatic.indexes.data();
        packDesc.totalIndexCount = static_cast<int>(
            residentStatic.indexes.size());
        packDesc.triangleClasses = residentStatic.triangleClasses.empty()
            ? nullptr : residentStatic.triangleClasses.data();
        packDesc.triangleMaterials = residentStatic.triangleMaterials.empty()
            ? nullptr : residentStatic.triangleMaterials.data();
        packDesc.totalTriangleCount = static_cast<int>(
            residentStatic.triangleClasses.size());
        if (!assignmentSurfaces.empty() &&
            (!BuildSmokeStaticBucketGeometryPackPreReserved(
                 packDesc, candidate.staticPack) || !candidate.staticPack.exact))
            return false;
        candidate.staticAssignmentSignature =
            candidate.staticAssignment.planSignature;
        candidate.staticPackSignature = candidate.staticPack.contentSignature;

        RtPathTraceAccelCpuCapacityStamp stamp;
        if (!StampPathTraceAccelCpuCapacities(snapshot, &candidate, stamp) ||
            !PathTraceAccelCpuCapacityStampWithinPlan(stamp, plan))
            return false;
        if (staticSortScratch.capacity() !=
                plan.counts.values[RT_PT_ACCEL_FAMILY_SCRATCH_STATIC_SORT] ||
            rigidMeshScratch.capacity() !=
                plan.counts.values[RT_PT_ACCEL_FAMILY_SCRATCH_RIGID_MESH_HASHES] ||
            staticSurfaceScratch.capacity() !=
                plan.counts.values[RT_PT_ACCEL_FAMILY_SCRATCH_STATIC_SURFACES])
            return false;
        if (peakSlotBytes) *peakSlotBytes = std::max(
            plan.plannedPeakBytes, stamp.residentBytes + plan.scratchBytes);
        candidate.complete = true;
        if (!ValidatePathTraceAccelCpuProduct(candidate)) return false;
        product = std::move(candidate);
        return true;
    }
    catch (const std::bad_alloc&) {}
    catch (const std::length_error&) {}
    product.ResetAndRelease();
    return false;
}

RtPathTraceAccelCpuOracle BuildPathTraceAccelCpuOracle(
    const RtPathTraceAccelCpuProduct& product)
{
    RtPathTraceAccelCpuOracle oracle;
    if (!ValidatePathTraceAccelCpuProduct(product)) return oracle;
    oracle.epoch = product.epoch;
    oracle.inputReceipt = product.inputReceipt;
    oracle.accelerationPlan = product.accelerationPlan;
    oracle.rigidGeometrySignature = product.rigidGeometrySignature;
    oracle.rigidInstanceSignature = product.rigidInstanceSignature;
    oracle.staticAssignmentSignature = product.staticAssignmentSignature;
    oracle.staticPackSignature = product.staticPackSignature;
    oracle.rigidVertices = static_cast<std::uint32_t>(product.rigidBuild.vertices.size());
    oracle.rigidIndexes = static_cast<std::uint32_t>(product.rigidBuild.indexes.size());
    oracle.rigidInstances = static_cast<std::uint32_t>(product.rigidBuild.instances.size());
    oracle.staticBuckets = static_cast<std::uint32_t>(product.staticAssignment.buckets.size());
    oracle.staticAssignments = static_cast<std::uint32_t>(product.staticAssignment.assignments.size());
    oracle.staticVertices = static_cast<std::uint32_t>(product.staticPack.stats.packedVertices);
    oracle.staticIndexes = static_cast<std::uint32_t>(product.staticPack.indexes.size());
    oracle.complete = true;
    return oracle;
}

RtPathTraceAccelCpuComparison ComparePathTraceAccelCpuProduct(
    const RtPathTraceAccelCpuProduct& product,
    const RtPathTraceAccelCpuOracle& oracle)
{
    RtPathTraceAccelCpuComparison result;
    result.generation = product.epoch.generation;
    if (!ValidatePathTraceAccelCpuProduct(product) || !oracle.complete ||
        !AccelCpuEpochsMatch(product.epoch, oracle.epoch) ||
        product.inputReceipt != oracle.inputReceipt)
    {
        result.rigidMismatches = 1;
        result.accelerationMismatches = 1;
        result.staticMismatches = 1;
        return result;
    }
    result.rigidMismatches =
        product.rigidGeometrySignature != oracle.rigidGeometrySignature ||
        product.rigidInstanceSignature != oracle.rigidInstanceSignature ||
        product.rigidBuild.vertices.size() != oracle.rigidVertices ||
        product.rigidBuild.indexes.size() != oracle.rigidIndexes ||
        product.rigidBuild.instances.size() != oracle.rigidInstances;
    result.accelerationMismatches =
        !AccelerationPlansEqual(product.accelerationPlan, oracle.accelerationPlan);
    result.staticMismatches =
        product.staticAssignmentSignature != oracle.staticAssignmentSignature ||
        product.staticPackSignature != oracle.staticPackSignature ||
        product.staticAssignment.buckets.size() != oracle.staticBuckets ||
        product.staticAssignment.assignments.size() != oracle.staticAssignments ||
        static_cast<std::uint32_t>(product.staticPack.stats.packedVertices) !=
            oracle.staticVertices ||
        product.staticPack.indexes.size() != oracle.staticIndexes;
    result.exact = result.rigidMismatches == 0 &&
        result.accelerationMismatches == 0 && result.staticMismatches == 0;
    return result;
}

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
    RtPathTraceAccelCpuSnapshot& ticket)
{
    const RtPathTracePlanningSnapshotEpoch epoch = ticket.epoch;
    const RtPathTraceAccelCpuCompatibilityToken compatibility = ticket.compatibility;
    ticket.ResetAndRelease();
    if (!PathTraceAccelCpuEpochValid(epoch) || compatibility.mapLoadSerial == 0 ||
        compatibility.mapName[0] == '\0' || compatibility.lifecycleEpoch == 0 ||
        compatibility.configFingerprint == 0)
    {
        backingStore.NoteTicketValidationFailure();
        return false;
    }
    ticket.epoch = epoch;
    ticket.compatibility = compatibility;

    std::shared_ptr<const RtPathTraceAccelCpuResidentPayload> resident;
    if (!backingStore.Publish(rigidPayloadSignature, staticPayloadSignature,
            preflight, fill, context, resident))
        return false;

    try
    {
        PathTraceAccelCpuPackMaybeInjectAllocationFailure();
        ticket.acceleration.staticCache = accelerationInput.staticCache;
        ticket.acceleration.staticVertexCount = accelerationInput.staticVertexCount;
        ticket.acceleration.staticIndexCount = accelerationInput.staticIndexCount;
        ticket.acceleration.dynamicVertexCount = accelerationInput.dynamicVertexCount;
        ticket.acceleration.dynamicIndexCount = accelerationInput.dynamicIndexCount;
        ticket.rigidRoute = rigidMetadata;
        ticket.staticBucket.worldGeneration = staticWorldGeneration;
        ticket.staticBucket.sourceGeneration = staticSourceGeneration;
        ticket.staticBucket.storageGeneration = resident->staticBucket.storageGeneration;
        ticket.staticBucket.portalAreaCount = portalAreaCount;
        ticket.staticBucket.maxVerticesPerBucket = maxVerticesPerBucket;
        ticket.staticBucket.maxIndexesPerBucket = maxIndexesPerBucket;
        ticket.staticBucket.maxTrianglesPerBucket = maxTrianglesPerBucket;
        ticket.staticBucket.complete = true;
        if (activeAreas != nullptr)
        {
            ticket.activePortalAreas.reserve(activeAreas->size());
            for (bool active : *activeAreas)
                ticket.activePortalAreas.push_back(active ? 1u : 0u);
        }
        ticket.residentPayload = resident;
        ticket.residentGeneration = resident->generation;
        ticket.rigidPayloadSignature = rigidPayloadSignature;
        ticket.staticPayloadSignature = staticPayloadSignature;
        ticket.residentBacked = true;
        RtPathTraceAccelCpuCapacityCounts counts;
        RtPathTraceAccelCpuCapacityPlan plan;
        if (!CountPathTraceAccelCpuCapacityFromSnapshot(ticket, counts) ||
            !BuildPathTraceAccelCpuCapacityPlan(counts, plan))
            throw std::length_error("Lane B ticket exceeds product slot");
        ticket.reservedProductBytes = plan.productBytes + plan.scratchBytes;
        ticket.plannedPeakBytes = plan.plannedPeakBytes;
        ticket.complete = true;
        ticket.inputReceipt = BuildPathTraceAccelCpuInputReceipt(ticket);
        if (ticket.inputReceipt == 0 || !ValidatePathTraceAccelCpuSnapshot(ticket))
        {
            backingStore.NoteTicketValidationFailure();
            ticket.ResetAndRelease();
            return false;
        }
    }
    catch (const std::bad_alloc&)
    {
        backingStore.NoteTicketBuildFailure();
        ticket.ResetAndRelease();
        return false;
    }
    catch (const std::length_error&)
    {
        backingStore.NoteTicketBuildFailure();
        ticket.ResetAndRelease();
        return false;
    }
    return true;
}
