#include "PathTraceAccelerationPlan.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace {

bool PlanRangeValid(int offset, int count, int totalCount)
{
    return offset >= 0 && count >= 0 && offset <= totalCount && count <= totalCount - offset;
}

bool PlanElementRangeValid(int elementOffset, int elementCount, size_t elementTotal)
{
    return elementOffset >= 0 &&
        elementCount > 0 &&
        static_cast<size_t>(elementOffset) <= elementTotal &&
        static_cast<size_t>(elementCount) <= elementTotal - static_cast<size_t>(elementOffset);
}

void CopyIdentityTransform(float transform[16])
{
    for (int index = 0; index < 16; ++index)
    {
        transform[index] = 0.0f;
    }
    transform[0] = 1.0f;
    transform[5] = 1.0f;
    transform[10] = 1.0f;
    transform[15] = 1.0f;
}

int NormalizeSmokePlanRecordCap(int cap)
{
    return cap > 0 ? cap : 0;
}

enum RtSmokeRigidTlasObservationCategory : uint32_t
{
    RT_SMOKE_RIGID_TLAS_REJECT_NON_RIGID = 1,
    RT_SMOKE_RIGID_TLAS_REJECT_MISSING_MESH = 2,
    RT_SMOKE_RIGID_TLAS_REJECT_STALE_MESH = 3,
    RT_SMOKE_RIGID_TLAS_REJECT_MISSING_BLAS = 4,
    RT_SMOKE_RIGID_TLAS_ACCEPTED = 5
};

RtSmokeRigidTlasObservationCategory ClassifyRigidTlasObservation(
    const RtSmokeRigidTlasObservation& observation,
    uint32_t rigidSourceMask)
{
    if ((observation.sourceFlags & rigidSourceMask) == 0)
    {
        return RT_SMOKE_RIGID_TLAS_REJECT_NON_RIGID;
    }
    if (!observation.hasMeshRecord)
    {
        return RT_SMOKE_RIGID_TLAS_REJECT_MISSING_MESH;
    }
    if (!observation.meshSeenThisFrame && !observation.residencyEnabled)
    {
        return RT_SMOKE_RIGID_TLAS_REJECT_STALE_MESH;
    }
    if (!observation.hasBlas)
    {
        return RT_SMOKE_RIGID_TLAS_REJECT_MISSING_BLAS;
    }
    return RT_SMOKE_RIGID_TLAS_ACCEPTED;
}

RtSmokePlanStaticBlasSignatureDesc MakeSignatureDescFromSnapshot(
    const RtSmokeStaticBlasSignatureSnapshot& snapshot)
{
    RtSmokePlanStaticBlasSignatureDesc desc;
    desc.vertices = snapshot.vertexBytes.empty() ? nullptr : snapshot.vertexBytes.data();
    desc.vertexStride = snapshot.vertexStride;
    desc.totalVertexCount = snapshot.totalVertexCount;
    desc.indexes = snapshot.indexes.empty() ? nullptr : snapshot.indexes.data();
    desc.totalIndexCount = static_cast<int>(snapshot.indexes.size());
    desc.triangleClasses = snapshot.triangleClasses.empty() ? nullptr : snapshot.triangleClasses.data();
    desc.triangleMaterials = snapshot.triangleMaterials.empty() ? nullptr : snapshot.triangleMaterials.data();
    desc.totalTriangleCount = static_cast<int>(
        snapshot.triangleClasses.size() < snapshot.triangleMaterials.size()
            ? snapshot.triangleClasses.size()
            : snapshot.triangleMaterials.size());
    desc.staticRange = snapshot.staticRange;
    desc.sceneOrigin = snapshot.sceneOrigin;
    return desc;
}

RtSmokeRigidTlasPlanDesc MakeRigidTlasPlanDescFromSnapshot(
    const RtSmokeRigidTlasPlanSnapshot& snapshot)
{
    RtSmokeRigidTlasPlanDesc desc;
    desc.observations = snapshot.observations.empty() ? nullptr : snapshot.observations.data();
    desc.observationCount = static_cast<int>(snapshot.observations.size());
    desc.rigidSourceMask = snapshot.rigidSourceMask;
    desc.firstInstanceId = snapshot.firstInstanceId;
    desc.instanceMask = snapshot.instanceMask;
    desc.maxInstances = snapshot.maxInstances;
    return desc;
}

} // namespace

uint64_t HashSmokePlanBytes(uint64_t hash, const void* data, size_t size)
{
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t index = 0; index < size; ++index)
    {
        hash ^= static_cast<uint64_t>(bytes[index]);
        hash *= 1099511628211ull;
    }
    return hash;
}

static uint64_t HashSmokeStaticBlasSignatureInput(
    uint64_t hash,
    const RtSmokePlanStaticBlasSignatureDesc& desc)
{
    hash = HashSmokePlanBytes(hash, &desc.vertexStride, sizeof(desc.vertexStride));
    hash = HashSmokePlanBytes(hash, &desc.totalVertexCount, sizeof(desc.totalVertexCount));
    hash = HashSmokePlanBytes(hash, &desc.totalIndexCount, sizeof(desc.totalIndexCount));
    hash = HashSmokePlanBytes(hash, &desc.staticRange.vertexOffset, sizeof(desc.staticRange.vertexOffset));
    hash = HashSmokePlanBytes(hash, &desc.staticRange.vertexCount, sizeof(desc.staticRange.vertexCount));
    hash = HashSmokePlanBytes(hash, &desc.staticRange.indexOffset, sizeof(desc.staticRange.indexOffset));
    hash = HashSmokePlanBytes(hash, &desc.staticRange.indexCount, sizeof(desc.staticRange.indexCount));
    hash = HashSmokePlanBytes(hash, &desc.sceneOrigin, sizeof(desc.sceneOrigin));

    if (desc.vertices && desc.vertexStride > 0 &&
        PlanRangeValid(desc.staticRange.vertexOffset, desc.staticRange.vertexCount, desc.totalVertexCount))
    {
        const uint8_t* vertexBytes = static_cast<const uint8_t*>(desc.vertices) +
            static_cast<size_t>(desc.staticRange.vertexOffset) * desc.vertexStride;
        hash = HashSmokePlanBytes(hash, vertexBytes, static_cast<size_t>(desc.staticRange.vertexCount) * desc.vertexStride);
    }

    if (desc.indexes &&
        PlanRangeValid(desc.staticRange.indexOffset, desc.staticRange.indexCount, desc.totalIndexCount))
    {
        hash = HashSmokePlanBytes(
            hash,
            desc.indexes + desc.staticRange.indexOffset,
            static_cast<size_t>(desc.staticRange.indexCount) * sizeof(desc.indexes[0]));
    }

    return hash;
}

static bool CanReuseSmokeStaticBlasSignature(
    const RtSmokeAccelerationPlanInput& input)
{
    const bool hasStaticBlas = input.staticIndexCount > 0;
    return
        hasStaticBlas &&
        input.staticCache.cacheValid &&
        !input.staticCache.staticCacheChanged &&
        input.staticCache.previousSignatureHash != 0;
}

static RtSmokeStaticBlasSignatureSnapshot CaptureSmokeStaticBlasSignatureMetadata(
    const RtSmokePlanStaticBlasSignatureDesc& desc)
{
    RtSmokeStaticBlasSignatureSnapshot snapshot;
    snapshot.vertexStride = desc.vertexStride;
    snapshot.totalVertexCount = desc.totalVertexCount;
    snapshot.staticRange = desc.staticRange;
    snapshot.sceneOrigin = desc.sceneOrigin;
    return snapshot;
}

RtSmokeStaticBlasSignatureSnapshot CaptureSmokeStaticBlasSignatureSnapshot(
    const RtSmokePlanStaticBlasSignatureDesc& desc)
{
    RtSmokeStaticBlasSignatureSnapshot snapshot =
        CaptureSmokeStaticBlasSignatureMetadata(desc);

    if (desc.vertices && desc.vertexStride > 0 &&
        PlanRangeValid(desc.staticRange.vertexOffset, desc.staticRange.vertexCount, desc.totalVertexCount))
    {
        const uint8_t* vertexBytes = static_cast<const uint8_t*>(desc.vertices) +
            static_cast<size_t>(desc.staticRange.vertexOffset) * desc.vertexStride;
        snapshot.vertexBytes.assign(
            vertexBytes,
            vertexBytes + static_cast<size_t>(desc.staticRange.vertexCount) * desc.vertexStride);
        snapshot.totalVertexCount = desc.staticRange.vertexCount;
        snapshot.staticRange.vertexOffset = 0;
    }
    if (desc.indexes &&
        PlanRangeValid(desc.staticRange.indexOffset, desc.staticRange.indexCount, desc.totalIndexCount))
    {
        snapshot.indexes.assign(
            desc.indexes + desc.staticRange.indexOffset,
            desc.indexes + desc.staticRange.indexOffset + desc.staticRange.indexCount);
        snapshot.staticRange.indexOffset = 0;
    }
    return snapshot;
}

RtSmokeAccelerationPlanSnapshot CaptureSmokeAccelerationPlanSnapshot(
    const RtSmokeAccelerationPlanInput& input)
{
    RtSmokeAccelerationPlanSnapshot snapshot;
    snapshot.staticSignature = CanReuseSmokeStaticBlasSignature(input)
        ? CaptureSmokeStaticBlasSignatureMetadata(input.staticSignature)
        : CaptureSmokeStaticBlasSignatureSnapshot(input.staticSignature);
    snapshot.staticCache = input.staticCache;
    snapshot.staticVertexCount = input.staticVertexCount;
    snapshot.staticIndexCount = input.staticIndexCount;
    snapshot.dynamicVertexCount = input.dynamicVertexCount;
    snapshot.dynamicIndexCount = input.dynamicIndexCount;
    return snapshot;
}

uint64_t BuildSmokeAccelerationPlanInputToken(const RtSmokeAccelerationPlanInput& input)
{
    uint64_t hash = 1469598103934665603ull;
    const bool staticSignatureReused = CanReuseSmokeStaticBlasSignature(input);
    const uint32_t cacheBits =
        (staticSignatureReused ? 1u : 0u) |
        (staticSignatureReused && input.staticCache.cacheResourcesReady ? 2u : 0u);
    hash = HashSmokePlanBytes(hash, &cacheBits, sizeof(cacheBits));
    if (staticSignatureReused)
    {
        hash = HashSmokePlanBytes(hash, &input.staticCache.previousSignatureHash, sizeof(input.staticCache.previousSignatureHash));
        hash = HashSmokePlanBytes(hash, &input.staticSignature.staticRange.vertexCount, sizeof(input.staticSignature.staticRange.vertexCount));
        hash = HashSmokePlanBytes(hash, &input.staticSignature.staticRange.indexCount, sizeof(input.staticSignature.staticRange.indexCount));
        hash = HashSmokePlanBytes(hash, &input.staticSignature.staticRange.triangleCount, sizeof(input.staticSignature.staticRange.triangleCount));
    }
    else
    {
        hash = HashSmokeStaticBlasSignatureInput(hash, input.staticSignature);
    }
    hash = HashSmokePlanBytes(hash, &input.staticVertexCount, sizeof(input.staticVertexCount));
    hash = HashSmokePlanBytes(hash, &input.staticIndexCount, sizeof(input.staticIndexCount));
    hash = HashSmokePlanBytes(hash, &input.dynamicVertexCount, sizeof(input.dynamicVertexCount));
    hash = HashSmokePlanBytes(hash, &input.dynamicIndexCount, sizeof(input.dynamicIndexCount));
    return hash;
}

RtSmokePlanStaticBlasSignature ComputeSmokeStaticBlasSignaturePlan(
    const RtSmokePlanStaticBlasSignatureDesc& desc)
{
    RtSmokePlanStaticBlasSignature signature;
    signature.vertexCount = desc.staticRange.vertexCount;
    signature.indexCount = desc.staticRange.indexCount;
    signature.triangleCount = desc.staticRange.triangleCount;

    uint64_t hash = 14695981039346656037ull;
    hash = HashSmokePlanBytes(hash, &desc.sceneOrigin.x, sizeof(desc.sceneOrigin.x));
    hash = HashSmokePlanBytes(hash, &desc.sceneOrigin.y, sizeof(desc.sceneOrigin.y));
    hash = HashSmokePlanBytes(hash, &desc.sceneOrigin.z, sizeof(desc.sceneOrigin.z));
    hash = HashSmokePlanBytes(hash, &signature.vertexCount, sizeof(signature.vertexCount));
    hash = HashSmokePlanBytes(hash, &signature.indexCount, sizeof(signature.indexCount));

    if (desc.vertices && desc.vertexStride > 0 &&
        PlanRangeValid(desc.staticRange.vertexOffset, desc.staticRange.vertexCount, desc.totalVertexCount))
    {
        const uint8_t* vertexBytes = static_cast<const uint8_t*>(desc.vertices) +
            static_cast<size_t>(desc.staticRange.vertexOffset) * desc.vertexStride;
        hash = HashSmokePlanBytes(hash, vertexBytes, static_cast<size_t>(desc.staticRange.vertexCount) * desc.vertexStride);
    }

    if (desc.indexes &&
        PlanRangeValid(desc.staticRange.indexOffset, desc.staticRange.indexCount, desc.totalIndexCount))
    {
        hash = HashSmokePlanBytes(
            hash,
            desc.indexes + desc.staticRange.indexOffset,
            static_cast<size_t>(desc.staticRange.indexCount) * sizeof(desc.indexes[0]));
    }

    signature.hash = hash;
    return signature;
}

RtSmokeAccelerationPlan BuildSmokeAccelerationPlan(const RtSmokeAccelerationPlanInput& input)
{
    RtSmokeAccelerationPlan plan;
    plan.hasStaticBlas = input.staticIndexCount > 0;
    plan.hasDynamicBlas = input.dynamicIndexCount > 0;

    if (CanReuseSmokeStaticBlasSignature(input))
    {
        plan.staticSignature.vertexCount = input.staticSignature.staticRange.vertexCount;
        plan.staticSignature.indexCount = input.staticSignature.staticRange.indexCount;
        plan.staticSignature.triangleCount = input.staticSignature.staticRange.triangleCount;
        plan.staticSignature.hash = input.staticCache.previousSignatureHash;
        plan.staticSignatureReused = true;
    }
    else
    {
        plan.staticSignature = ComputeSmokeStaticBlasSignaturePlan(input.staticSignature);
    }

    plan.staticCacheHit =
        plan.hasStaticBlas &&
        input.staticCache.cacheValid &&
        input.staticCache.cacheResourcesReady &&
        !input.staticCache.staticCacheChanged &&
        input.staticCache.previousSignatureHash != 0 &&
        input.staticCache.previousSignatureHash == plan.staticSignature.hash;

    plan.staticBlas.enabled = plan.hasStaticBlas;
    plan.staticBlas.cacheHit = plan.staticCacheHit;
    plan.staticBlas.vertexCount = input.staticVertexCount;
    plan.staticBlas.indexCount = input.staticIndexCount;
    plan.staticBlas.debugName = "PathTraceSmokeStaticWorldBLAS";

    plan.dynamicBlas.enabled = plan.hasDynamicBlas;
    plan.dynamicBlas.cacheHit = false;
    plan.dynamicBlas.vertexCount = input.dynamicVertexCount;
    plan.dynamicBlas.indexCount = input.dynamicIndexCount;
    plan.dynamicBlas.debugName = "PathTraceSmokeDynamicCandidateBLAS";
    return plan;
}

RtSmokeAccelerationPlanResult BuildSmokeAccelerationPlanResult(
    const RtSmokeAccelerationPlanSnapshot& snapshot)
{
    RtSmokeAccelerationPlanInput input;
    input.staticSignature = MakeSignatureDescFromSnapshot(snapshot.staticSignature);
    input.staticCache = snapshot.staticCache;
    input.staticVertexCount = snapshot.staticVertexCount;
    input.staticIndexCount = snapshot.staticIndexCount;
    input.dynamicVertexCount = snapshot.dynamicVertexCount;
    input.dynamicIndexCount = snapshot.dynamicIndexCount;

    RtSmokeAccelerationPlanResult result;
    result.plan = BuildSmokeAccelerationPlan(input);
    result.valid = result.plan.hasStaticBlas || result.plan.hasDynamicBlas;
    return result;
}

RtSmokeAccelerationPlanTimedResult BuildSmokeAccelerationPlanTimedResult(
    const RtSmokeAccelerationPlanSnapshot& snapshot)
{
    const auto start = std::chrono::steady_clock::now();
    RtSmokeAccelerationPlanTimedResult timedResult;
    timedResult.result = BuildSmokeAccelerationPlanResult(snapshot);
    const auto end = std::chrono::steady_clock::now();
    timedResult.workerExecutionMs = std::chrono::duration<double, std::milli>(end - start).count();
    return timedResult;
}

RtSmokeBaseTlasPlan BuildSmokeBaseTlasPlan(bool hasStaticBlas, bool hasDynamicBlas)
{
    RtSmokeBaseTlasPlan plan;
    if (hasStaticBlas)
    {
        RtSmokePlanTlasInstance instance;
        instance.kind = RT_SMOKE_PLAN_TLAS_STATIC_BLAS;
        instance.instanceId = 0;
        instance.instanceMask = 0x01;
        CopyIdentityTransform(instance.transform);
        plan.instances[plan.instanceCount++] = instance;
    }
    if (hasDynamicBlas)
    {
        RtSmokePlanTlasInstance instance;
        instance.kind = RT_SMOKE_PLAN_TLAS_DYNAMIC_BLAS;
        instance.instanceId = 1;
        instance.instanceMask = 0x01;
        CopyIdentityTransform(instance.transform);
        plan.instances[plan.instanceCount++] = instance;
    }
    return plan;
}

RtSmokeAccelerationSubmitPlan BuildSmokeAccelerationSubmitPlan(
    const RtSmokeAccelerationSubmitPlanInput& input)
{
    RtSmokeAccelerationSubmitPlan plan;
    plan.buildStaticBlas = input.hasStaticBlas && !input.staticBlasCacheHit;
    plan.buildDynamicBlas = input.hasDynamicBlas;
    plan.submitTlas =
        (input.hasStaticBlas && input.includeStaticBlasInTlas) ||
        input.hasDynamicBlas ||
        input.hasExtraTlasInstances;
    plan.baseTlasPlan = BuildSmokeBaseTlasPlan(
        input.hasStaticBlas && input.includeStaticBlasInTlas,
        input.hasDynamicBlas);
    return plan;
}

RtSmokeStaticBucketAssignmentPlan BuildSmokeStaticBucketAssignmentPlan(
    const RtSmokeStaticBucketAssignmentPlanDesc& desc)
{
    RtSmokeStaticBucketAssignmentPlan plan;
    plan.planSignature = 1469598103934665603ull;
    if (!desc.surfaces || desc.surfaceCount <= 0)
    {
        return plan;
    }

    plan.stats.inputSurfaces = desc.surfaceCount;
    std::vector<int> sortedSurfaceIndices;
    sortedSurfaceIndices.reserve(desc.surfaceCount);
    for (int surfaceIndex = 0; surfaceIndex < desc.surfaceCount; ++surfaceIndex)
    {
        sortedSurfaceIndices.push_back(surfaceIndex);
    }
    std::sort(
        sortedSurfaceIndices.begin(),
        sortedSurfaceIndices.end(),
        [&desc](int lhsIndex, int rhsIndex)
        {
            const RtSmokeStaticBucketAssignmentSurface& lhs =
                desc.surfaces[lhsIndex];
            const RtSmokeStaticBucketAssignmentSurface& rhs =
                desc.surfaces[rhsIndex];
            const int lhsArea =
                lhs.portalArea >= 0 &&
                    lhs.portalArea < desc.portalAreaCount
                    ? lhs.portalArea
                    : RT_SMOKE_STATIC_BUCKET_FALLBACK_AREA;
            const int rhsArea =
                rhs.portalArea >= 0 &&
                    rhs.portalArea < desc.portalAreaCount
                    ? rhs.portalArea
                    : RT_SMOKE_STATIC_BUCKET_FALLBACK_AREA;
            if (lhsArea != rhsArea)
            {
                return lhsArea < rhsArea;
            }
            if (lhs.surfaceKey != rhs.surfaceKey)
            {
                return lhs.surfaceKey < rhs.surfaceKey;
            }
            if (lhs.range.vertexOffset != rhs.range.vertexOffset)
            {
                return lhs.range.vertexOffset < rhs.range.vertexOffset;
            }
            if (lhs.range.indexOffset != rhs.range.indexOffset)
            {
                return lhs.range.indexOffset < rhs.range.indexOffset;
            }
            if (lhs.range.triangleOffset != rhs.range.triangleOffset)
            {
                return lhs.range.triangleOffset < rhs.range.triangleOffset;
            }
            return lhs.sourceRecordIndex < rhs.sourceRecordIndex;
        });

    const auto exceedsLimit = [](int current, int addition, int limit)
    {
        return limit > 0 &&
            (addition > limit || current > limit - addition);
    };
    const auto surfaceIsOversized =
        [&desc](const RtSmokePlanGeometryRange& range)
        {
            return
                (desc.maxVerticesPerBucket > 0 &&
                    range.vertexCount > desc.maxVerticesPerBucket) ||
                (desc.maxIndexesPerBucket > 0 &&
                    range.indexCount > desc.maxIndexesPerBucket) ||
                (desc.maxTrianglesPerBucket > 0 &&
                    range.triangleCount > desc.maxTrianglesPerBucket);
        };
    const auto buildBucketKey =
        [&desc](int portalArea, uint32_t splitIndex)
        {
            uint64_t key = 1469598103934665603ull;
            key = HashSmokePlanBytes(
                key,
                &desc.worldGeneration,
                sizeof(desc.worldGeneration));
            key = HashSmokePlanBytes(
                key,
                &portalArea,
                sizeof(portalArea));
            key = HashSmokePlanBytes(
                key,
                &splitIndex,
                sizeof(splitIndex));
            key = HashSmokePlanBytes(
                key,
                &desc.sourceGeneration,
                sizeof(desc.sourceGeneration));
            key = HashSmokePlanBytes(
                key,
                &desc.storageGeneration,
                sizeof(desc.storageGeneration));
            return key;
        };

    std::unordered_set<uint64_t> assignedSurfaceKeys;
    assignedSurfaceKeys.reserve(static_cast<size_t>(desc.surfaceCount));
    std::unordered_map<uint64_t, size_t> bucketKeyOwners;
    bucketKeyOwners.reserve(static_cast<size_t>(desc.surfaceCount));
    int currentArea = std::numeric_limits<int>::min();
    uint32_t nextSplitIndex = 0;

    for (int sortedIndex : sortedSurfaceIndices)
    {
        const RtSmokeStaticBucketAssignmentSurface& surface =
            desc.surfaces[sortedIndex];
        const RtSmokePlanGeometryRange& range = surface.range;
        const bool rangeValid =
            surface.valid &&
            surface.surfaceKey != 0 &&
            range.vertexOffset >= 0 &&
            range.vertexCount > 0 &&
            range.indexOffset >= 0 &&
            range.indexCount > 0 &&
            (range.indexCount % 3) == 0 &&
            range.triangleOffset >= 0 &&
            range.triangleCount > 0 &&
            range.triangleCount == range.indexCount / 3;
        if (!rangeValid)
        {
            ++plan.stats.invalidRangeSurfaces;
            continue;
        }
        if (!assignedSurfaceKeys.insert(surface.surfaceKey).second)
        {
            ++plan.stats.duplicateSurfaces;
            continue;
        }

        int portalArea = surface.portalArea;
        if (portalArea < 0)
        {
            ++plan.stats.unassignedAreaSurfaces;
            portalArea = RT_SMOKE_STATIC_BUCKET_FALLBACK_AREA;
        }
        else if (portalArea >= desc.portalAreaCount)
        {
            ++plan.stats.invalidAreaSurfaces;
            portalArea = RT_SMOKE_STATIC_BUCKET_FALLBACK_AREA;
        }

        if (portalArea != currentArea)
        {
            currentArea = portalArea;
            nextSplitIndex = 0;
        }

        const bool oversized = surfaceIsOversized(range);
        if (oversized)
        {
            ++plan.stats.oversizedSurfaces;
        }
        bool needsNewBucket = plan.buckets.empty();
        if (!needsNewBucket)
        {
            const RtSmokeStaticBucketAssignmentBucket& currentBucket =
                plan.buckets.back();
            needsNewBucket =
                currentBucket.portalArea != portalArea ||
                currentBucket.oversized ||
                oversized ||
                exceedsLimit(
                    currentBucket.vertexCount,
                    range.vertexCount,
                    desc.maxVerticesPerBucket) ||
                exceedsLimit(
                    currentBucket.indexCount,
                    range.indexCount,
                    desc.maxIndexesPerBucket) ||
                exceedsLimit(
                    currentBucket.triangleCount,
                    range.triangleCount,
                    desc.maxTrianglesPerBucket);
        }

        if (needsNewBucket)
        {
            RtSmokeStaticBucketAssignmentBucket bucket;
            bucket.worldGeneration = desc.worldGeneration;
            bucket.sourceGeneration = desc.sourceGeneration;
            bucket.storageGeneration = desc.storageGeneration;
            bucket.portalArea = portalArea;
            bucket.splitIndex = nextSplitIndex++;
            bucket.bucketKey =
                buildBucketKey(bucket.portalArea, bucket.splitIndex);
            bucket.firstAssignment =
                static_cast<uint32_t>(plan.assignments.size());
            bucket.oversized = oversized;
            if (bucket.splitIndex > 0)
            {
                ++plan.stats.splitBuckets;
            }
            if (bucket.portalArea == RT_SMOKE_STATIC_BUCKET_FALLBACK_AREA)
            {
                ++plan.stats.fallbackBuckets;
            }

            const auto inserted = bucketKeyOwners.emplace(
                bucket.bucketKey,
                plan.buckets.size());
            if (!inserted.second)
            {
                const RtSmokeStaticBucketAssignmentBucket& owner =
                    plan.buckets[inserted.first->second];
                if (owner.worldGeneration != bucket.worldGeneration ||
                    owner.sourceGeneration != bucket.sourceGeneration ||
                    owner.storageGeneration != bucket.storageGeneration ||
                    owner.portalArea != bucket.portalArea ||
                    owner.splitIndex != bucket.splitIndex)
                {
                    ++plan.stats.bucketKeyCollisions;
                }
            }
            plan.buckets.push_back(bucket);
        }

        RtSmokeStaticBucketAssignmentBucket& bucket = plan.buckets.back();
        RtSmokeStaticBucketAssignment assignment;
        assignment.surfaceKey = surface.surfaceKey;
        assignment.sourceRecordIndex = surface.sourceRecordIndex;
        assignment.bucketIndex =
            static_cast<uint32_t>(plan.buckets.size() - 1);
        assignment.localPrimitiveOffset =
            static_cast<uint32_t>(bucket.triangleCount);
        assignment.sourceRange = range;
        plan.assignments.push_back(assignment);

        ++bucket.assignmentCount;
        bucket.vertexCount += range.vertexCount;
        bucket.indexCount += range.indexCount;
        bucket.triangleCount += range.triangleCount;
        bucket.active = bucket.active || surface.active;
        ++plan.stats.assignedSurfaces;
        plan.stats.assignedPrimitives += range.triangleCount;
    }

    plan.stats.buckets = static_cast<int>(plan.buckets.size());
    for (const RtSmokeStaticBucketAssignmentBucket& bucket : plan.buckets)
    {
        if (bucket.active)
        {
            ++plan.stats.activeBuckets;
        }
        plan.planSignature = HashSmokePlanBytes(
            plan.planSignature,
            &bucket.bucketKey,
            sizeof(bucket.bucketKey));
        plan.planSignature = HashSmokePlanBytes(
            plan.planSignature,
            &bucket.portalArea,
            sizeof(bucket.portalArea));
        plan.planSignature = HashSmokePlanBytes(
            plan.planSignature,
            &bucket.splitIndex,
            sizeof(bucket.splitIndex));
        plan.planSignature = HashSmokePlanBytes(
            plan.planSignature,
            &bucket.assignmentCount,
            sizeof(bucket.assignmentCount));
        plan.planSignature = HashSmokePlanBytes(
            plan.planSignature,
            &bucket.vertexCount,
            sizeof(bucket.vertexCount));
        plan.planSignature = HashSmokePlanBytes(
            plan.planSignature,
            &bucket.indexCount,
            sizeof(bucket.indexCount));
        plan.planSignature = HashSmokePlanBytes(
            plan.planSignature,
            &bucket.triangleCount,
            sizeof(bucket.triangleCount));
    }
    for (const RtSmokeStaticBucketAssignment& assignment : plan.assignments)
    {
        plan.planSignature = HashSmokePlanBytes(
            plan.planSignature,
            &assignment.surfaceKey,
            sizeof(assignment.surfaceKey));
        plan.planSignature = HashSmokePlanBytes(
            plan.planSignature,
            &assignment.bucketIndex,
            sizeof(assignment.bucketIndex));
        plan.planSignature = HashSmokePlanBytes(
            plan.planSignature,
            &assignment.localPrimitiveOffset,
            sizeof(assignment.localPrimitiveOffset));
        plan.planSignature = HashSmokePlanBytes(
            plan.planSignature,
            &assignment.sourceRange,
            sizeof(assignment.sourceRange));
    }
    plan.exactCoverage =
        plan.stats.assignedSurfaces == plan.stats.inputSurfaces &&
        plan.stats.duplicateSurfaces == 0 &&
        plan.stats.invalidRangeSurfaces == 0 &&
        plan.stats.bucketKeyCollisions == 0;
    return plan;
}

RtSmokeStaticBucketGeometryPack BuildSmokeStaticBucketGeometryPack(
    const RtSmokeStaticBucketGeometryPackDesc& desc)
{
    RtSmokeStaticBucketGeometryPack pack;
    pack.contentSignature = 1469598103934665603ull;
    const RtSmokeStaticBucketAssignmentPlan* assignmentPlan =
        desc.assignmentPlan;
    if (!assignmentPlan ||
        !desc.vertices ||
        desc.vertexStride == 0 ||
        desc.totalVertexCount <= 0 ||
        !desc.indexes ||
        desc.totalIndexCount <= 0 ||
        !desc.triangleClasses ||
        !desc.triangleMaterials ||
        desc.totalTriangleCount <= 0)
    {
        return pack;
    }

    pack.stats.inputBuckets =
        static_cast<int>(assignmentPlan->buckets.size());
    pack.stats.inputAssignments =
        static_cast<int>(assignmentPlan->assignments.size());
    pack.buckets.reserve(assignmentPlan->buckets.size());
    pack.indexes.reserve(
        static_cast<size_t>(
            assignmentPlan->stats.assignedPrimitives) * 3);
    pack.triangleClasses.reserve(
        assignmentPlan->stats.assignedPrimitives);
    pack.triangleMaterials.reserve(
        assignmentPlan->stats.assignedPrimitives);
    pack.triangleIdentities.reserve(
        assignmentPlan->stats.assignedPrimitives);

    const uint8_t* sourceVertexBytes =
        static_cast<const uint8_t*>(desc.vertices);
    for (size_t bucketIndex = 0;
        bucketIndex < assignmentPlan->buckets.size();
        ++bucketIndex)
    {
        const RtSmokeStaticBucketAssignmentBucket& sourceBucket =
            assignmentPlan->buckets[bucketIndex];
        const uint64_t assignmentEnd =
            static_cast<uint64_t>(sourceBucket.firstAssignment) +
            sourceBucket.assignmentCount;
        if (sourceBucket.assignmentCount == 0 ||
            sourceBucket.firstAssignment >
                assignmentPlan->assignments.size() ||
            assignmentEnd > assignmentPlan->assignments.size())
        {
            ++pack.stats.invalidBucketRanges;
            continue;
        }

        RtSmokeStaticBucketPackedRecord bucket;
        bucket.bucketKey = sourceBucket.bucketKey;
        bucket.portalArea = sourceBucket.portalArea;
        bucket.splitIndex = sourceBucket.splitIndex;
        bucket.firstAssignment = sourceBucket.firstAssignment;
        bucket.assignmentCount = sourceBucket.assignmentCount;
        bucket.range.vertexOffset =
            static_cast<int>(pack.vertexBytes.size() /
                desc.vertexStride);
        bucket.range.indexOffset =
            static_cast<int>(pack.indexes.size());
        bucket.range.triangleOffset =
            static_cast<int>(pack.triangleClasses.size());
        bucket.vertexByteOffset =
            static_cast<uint64_t>(pack.vertexBytes.size());
        bucket.indexByteOffset =
            static_cast<uint64_t>(pack.indexes.size()) *
            sizeof(uint32_t);
        bucket.triangleMetadataByteOffset =
            static_cast<uint64_t>(pack.triangleClasses.size()) *
            sizeof(uint32_t);
        bucket.active = sourceBucket.active;
        bucket.oversized = sourceBucket.oversized;

        const int packedSurfacesBeforeBucket =
            pack.stats.packedSurfaces;
        bool bucketValid = true;
        for (uint32_t assignmentOffset = 0;
            assignmentOffset < sourceBucket.assignmentCount;
            ++assignmentOffset)
        {
            const uint32_t assignmentIndex =
                sourceBucket.firstAssignment + assignmentOffset;
            const RtSmokeStaticBucketAssignment& assignment =
                assignmentPlan->assignments[assignmentIndex];
            const RtSmokePlanGeometryRange& sourceRange =
                assignment.sourceRange;
            if (assignment.bucketIndex != bucketIndex)
            {
                ++pack.stats.invalidAssignments;
                bucketValid = false;
                break;
            }
            if (!PlanRangeValid(
                    sourceRange.vertexOffset,
                    sourceRange.vertexCount,
                    desc.totalVertexCount) ||
                !PlanRangeValid(
                    sourceRange.indexOffset,
                    sourceRange.indexCount,
                    desc.totalIndexCount) ||
                !PlanRangeValid(
                    sourceRange.triangleOffset,
                    sourceRange.triangleCount,
                    desc.totalTriangleCount) ||
                sourceRange.vertexCount <= 0 ||
                sourceRange.indexCount <= 0 ||
                sourceRange.triangleCount <= 0 ||
                sourceRange.indexCount !=
                    sourceRange.triangleCount * 3)
            {
                ++pack.stats.sourceRangeMismatches;
                bucketValid = false;
                break;
            }

            const uint32_t expectedLocalPrimitiveOffset =
                static_cast<uint32_t>(
                    pack.triangleClasses.size() -
                    static_cast<size_t>(
                        bucket.range.triangleOffset));
            if (assignment.localPrimitiveOffset !=
                expectedLocalPrimitiveOffset)
            {
                ++pack.stats.localPrimitiveOffsetErrors;
                bucketValid = false;
                break;
            }

            const size_t sourceVertexByteOffset =
                static_cast<size_t>(sourceRange.vertexOffset) *
                desc.vertexStride;
            const size_t sourceVertexByteSize =
                static_cast<size_t>(sourceRange.vertexCount) *
                desc.vertexStride;
            const uint32_t packedSurfaceVertexOffset =
                static_cast<uint32_t>(
                    pack.vertexBytes.size() /
                    desc.vertexStride);
            pack.vertexBytes.insert(
                pack.vertexBytes.end(),
                sourceVertexBytes + sourceVertexByteOffset,
                sourceVertexBytes + sourceVertexByteOffset +
                    sourceVertexByteSize);

            const uint32_t sourceVertexBegin =
                static_cast<uint32_t>(sourceRange.vertexOffset);
            const uint32_t sourceVertexEnd =
                sourceVertexBegin +
                static_cast<uint32_t>(sourceRange.vertexCount);
            for (int sourceIndexOffset = 0;
                sourceIndexOffset < sourceRange.indexCount;
                ++sourceIndexOffset)
            {
                const uint32_t sourceIndex =
                    desc.indexes[
                        sourceRange.indexOffset +
                        sourceIndexOffset];
                if (sourceIndex < sourceVertexBegin ||
                    sourceIndex >= sourceVertexEnd)
                {
                    ++pack.stats.indexRangeErrors;
                    bucketValid = false;
                    break;
                }
                pack.indexes.push_back(
                    packedSurfaceVertexOffset +
                    (sourceIndex - sourceVertexBegin));
            }
            if (!bucketValid)
            {
                break;
            }

            pack.triangleClasses.insert(
                pack.triangleClasses.end(),
                desc.triangleClasses +
                    sourceRange.triangleOffset,
                desc.triangleClasses +
                    sourceRange.triangleOffset +
                    sourceRange.triangleCount);
            pack.triangleMaterials.insert(
                pack.triangleMaterials.end(),
                desc.triangleMaterials +
                    sourceRange.triangleOffset,
                desc.triangleMaterials +
                    sourceRange.triangleOffset +
                    sourceRange.triangleCount);
            for (int sourcePrimitiveIndex = 0;
                sourcePrimitiveIndex <
                    sourceRange.triangleCount;
                ++sourcePrimitiveIndex)
            {
                RtSmokeStaticBucketTriangleIdentity identity;
                identity.surfaceKey = assignment.surfaceKey;
                identity.sourceRecordIndex =
                    assignment.sourceRecordIndex;
                identity.sourcePrimitiveIndex =
                    static_cast<uint32_t>(sourcePrimitiveIndex);
                pack.triangleIdentities.push_back(identity);
            }
            ++pack.stats.packedSurfaces;
        }

        if (!bucketValid)
        {
            pack.vertexBytes.resize(
                static_cast<size_t>(bucket.vertexByteOffset));
            pack.indexes.resize(
                static_cast<size_t>(
                    bucket.indexByteOffset /
                    sizeof(uint32_t)));
            pack.triangleClasses.resize(
                static_cast<size_t>(
                    bucket.triangleMetadataByteOffset /
                    sizeof(uint32_t)));
            pack.triangleMaterials.resize(
                pack.triangleClasses.size());
            pack.triangleIdentities.resize(
                pack.triangleClasses.size());
            pack.stats.packedSurfaces =
                packedSurfacesBeforeBucket;
            continue;
        }

        bucket.range.vertexCount =
            static_cast<int>(
                pack.vertexBytes.size() /
                desc.vertexStride) -
            bucket.range.vertexOffset;
        bucket.range.indexCount =
            static_cast<int>(pack.indexes.size()) -
            bucket.range.indexOffset;
        bucket.range.triangleCount =
            static_cast<int>(pack.triangleClasses.size()) -
            bucket.range.triangleOffset;
        bucket.vertexByteSize =
            static_cast<uint64_t>(bucket.range.vertexCount) *
            desc.vertexStride;
        bucket.indexByteSize =
            static_cast<uint64_t>(bucket.range.indexCount) *
            sizeof(uint32_t);
        bucket.triangleMetadataByteSize =
            static_cast<uint64_t>(bucket.range.triangleCount) *
            sizeof(uint32_t);
        if (bucket.range.vertexCount != sourceBucket.vertexCount ||
            bucket.range.indexCount != sourceBucket.indexCount ||
            bucket.range.triangleCount !=
                sourceBucket.triangleCount)
        {
            ++pack.stats.countMismatches;
        }
        pack.buckets.push_back(bucket);
    }

    pack.stats.packedBuckets =
        static_cast<int>(pack.buckets.size());
    pack.stats.packedVertices =
        static_cast<int>(
            pack.vertexBytes.size() / desc.vertexStride);
    pack.stats.packedIndexes =
        static_cast<int>(pack.indexes.size());
    pack.stats.packedTriangles =
        static_cast<int>(pack.triangleClasses.size());
    if (pack.triangleClasses.size() !=
            pack.triangleMaterials.size() ||
        pack.triangleClasses.size() !=
            pack.triangleIdentities.size() ||
        pack.stats.packedSurfaces !=
            assignmentPlan->stats.assignedSurfaces ||
        pack.stats.packedTriangles !=
            assignmentPlan->stats.assignedPrimitives)
    {
        ++pack.stats.countMismatches;
    }

    pack.contentSignature = HashSmokePlanBytes(
        pack.contentSignature,
        &desc.vertexStride,
        sizeof(desc.vertexStride));
    if (!pack.vertexBytes.empty())
    {
        pack.contentSignature = HashSmokePlanBytes(
            pack.contentSignature,
            pack.vertexBytes.data(),
            pack.vertexBytes.size());
    }
    if (!pack.indexes.empty())
    {
        pack.contentSignature = HashSmokePlanBytes(
            pack.contentSignature,
            pack.indexes.data(),
            pack.indexes.size() * sizeof(pack.indexes[0]));
    }
    if (!pack.triangleClasses.empty())
    {
        pack.contentSignature = HashSmokePlanBytes(
            pack.contentSignature,
            pack.triangleClasses.data(),
            pack.triangleClasses.size() *
                sizeof(pack.triangleClasses[0]));
        pack.contentSignature = HashSmokePlanBytes(
            pack.contentSignature,
            pack.triangleMaterials.data(),
            pack.triangleMaterials.size() *
                sizeof(pack.triangleMaterials[0]));
    }
    for (const RtSmokeStaticBucketPackedRecord& bucket :
        pack.buckets)
    {
        pack.contentSignature = HashSmokePlanBytes(
            pack.contentSignature,
            &bucket.bucketKey,
            sizeof(bucket.bucketKey));
        pack.contentSignature = HashSmokePlanBytes(
            pack.contentSignature,
            &bucket.portalArea,
            sizeof(bucket.portalArea));
        pack.contentSignature = HashSmokePlanBytes(
            pack.contentSignature,
            &bucket.splitIndex,
            sizeof(bucket.splitIndex));
        pack.contentSignature = HashSmokePlanBytes(
            pack.contentSignature,
            &bucket.range,
            sizeof(bucket.range));
    }
    for (const RtSmokeStaticBucketTriangleIdentity& identity :
        pack.triangleIdentities)
    {
        pack.contentSignature = HashSmokePlanBytes(
            pack.contentSignature,
            &identity.surfaceKey,
            sizeof(identity.surfaceKey));
        pack.contentSignature = HashSmokePlanBytes(
            pack.contentSignature,
            &identity.sourceRecordIndex,
            sizeof(identity.sourceRecordIndex));
        pack.contentSignature = HashSmokePlanBytes(
            pack.contentSignature,
            &identity.sourcePrimitiveIndex,
            sizeof(identity.sourcePrimitiveIndex));
    }

    pack.exact =
        assignmentPlan->exactCoverage &&
        pack.stats.packedBuckets == pack.stats.inputBuckets &&
        pack.stats.packedSurfaces == pack.stats.inputAssignments &&
        pack.stats.invalidBucketRanges == 0 &&
        pack.stats.invalidAssignments == 0 &&
        pack.stats.sourceRangeMismatches == 0 &&
        pack.stats.indexRangeErrors == 0 &&
        pack.stats.localPrimitiveOffsetErrors == 0 &&
        pack.stats.countMismatches == 0;
    return pack;
}

RtSmokeStaticBucketPublicationEpochPlan
BuildSmokeStaticBucketPublicationEpochPlan(
    const RtSmokeStaticBucketPublicationEpochInput& input)
{
    RtSmokeStaticBucketPublicationEpochPlan plan;
    plan.generationValid =
        input.expectedGeneration != 0 &&
        input.tlasGeneration != 0 &&
        input.routeGeneration != 0;
    plan.generationsMatch =
        plan.generationValid &&
        input.expectedGeneration == input.tlasGeneration &&
        input.expectedGeneration == input.routeGeneration;
    plan.countsMatch =
        input.residentBuckets >= 0 &&
        input.activeBuckets >= 0 &&
        input.activeBuckets <= input.residentBuckets &&
        input.tlasInstances == input.activeBuckets &&
        input.routeRecords == input.residentBuckets;
    plan.accepted =
        input.activeSetExact &&
        plan.generationsMatch &&
        plan.countsMatch;
    plan.mixedEpochRejected =
        plan.generationValid &&
        !plan.generationsMatch;
    return plan;
}

RtSmokeStaticBucketCutoverPlan BuildSmokeStaticBucketCutoverPlan(
    const RtSmokeStaticBucketCutoverInput& input)
{
    RtSmokeStaticBucketCutoverPlan plan;
    plan.allResidentReady =
        input.residentBuckets > 0 &&
        input.readyBuckets == input.residentBuckets;
    plan.publicationExact =
        input.publicationValid &&
        input.routeUploaded &&
        input.activeBuckets > 0 &&
        input.activeBuckets <= input.residentBuckets &&
        input.tlasInstances == input.activeBuckets &&
        input.routeRecords == input.residentBuckets;
    plan.accepted =
        input.requested &&
        input.consumerSupported &&
        plan.allResidentReady &&
        plan.publicationExact;
    return plan;
}

RtSmokeStaticTlasActiveSetPlan BuildSmokeStaticTlasActiveSetPlan(
    const RtSmokeStaticTlasActiveSetPlanDesc& desc)
{
    RtSmokeStaticTlasActiveSetPlan plan;
    plan.monolithicStaticBlas = desc.monolithicStaticBlas;
    plan.activeSetSignature = 1469598103934665603ull;
    plan.residentSetSignature = 1469598103934665603ull;
    plan.tlasInstanceSignature = 1469598103934665603ull;
    if (!desc.buckets || desc.bucketCount <= 0)
    {
        return plan;
    }

    plan.instances.reserve(desc.monolithicStaticBlas ? 1 : desc.bucketCount);
    bool monolithicInstanceEmitted = false;
    for (int bucketIndex = 0; bucketIndex < desc.bucketCount; ++bucketIndex)
    {
        const RtSmokeStaticTlasBucketObservation& bucket = desc.buckets[bucketIndex];
        if (!bucket.resident)
        {
            continue;
        }

        plan.residentSetSignature = HashSmokePlanBytes(plan.residentSetSignature, &bucket.bucketKey, sizeof(bucket.bucketKey));
        plan.residentSetSignature = HashSmokePlanBytes(plan.residentSetSignature, &bucket.residentSurfaceCount, sizeof(bucket.residentSurfaceCount));
        plan.residentSetSignature = HashSmokePlanBytes(plan.residentSetSignature, &bucket.residentVertexCount, sizeof(bucket.residentVertexCount));
        plan.residentSetSignature = HashSmokePlanBytes(plan.residentSetSignature, &bucket.residentIndexCount, sizeof(bucket.residentIndexCount));
        plan.residentSetSignature = HashSmokePlanBytes(plan.residentSetSignature, &bucket.residentTriangleCount, sizeof(bucket.residentTriangleCount));
        ++plan.residentBuckets;
        plan.residentSurfaceCount += bucket.residentSurfaceCount;
        plan.residentVertexCount += bucket.residentVertexCount;
        plan.residentIndexCount += bucket.residentIndexCount;
        plan.residentTriangleCount += bucket.residentTriangleCount;
        if (!bucket.active)
        {
            ++plan.inactiveResidentBuckets;
            continue;
        }

        plan.activeSetSignature = HashSmokePlanBytes(plan.activeSetSignature, &bucket.bucketKey, sizeof(bucket.bucketKey));
        plan.activeSetSignature = HashSmokePlanBytes(plan.activeSetSignature, &bucket.activeReasonFlags, sizeof(bucket.activeReasonFlags));
        plan.activeSetSignature = HashSmokePlanBytes(plan.activeSetSignature, &bucket.activeSurfaceCount, sizeof(bucket.activeSurfaceCount));
        plan.activeSetSignature = HashSmokePlanBytes(plan.activeSetSignature, &bucket.activeVertexCount, sizeof(bucket.activeVertexCount));
        plan.activeSetSignature = HashSmokePlanBytes(plan.activeSetSignature, &bucket.activeIndexCount, sizeof(bucket.activeIndexCount));
        plan.activeSetSignature = HashSmokePlanBytes(plan.activeSetSignature, &bucket.activeTriangleCount, sizeof(bucket.activeTriangleCount));
        ++plan.activeBuckets;
        plan.activeSurfaceCount += bucket.activeSurfaceCount;
        plan.activeVertexCount += bucket.activeVertexCount;
        plan.activeIndexCount += bucket.activeIndexCount;
        plan.activeTriangleCount += bucket.activeTriangleCount;
        if (!bucket.hasBlas || !desc.hasStaticBlas)
        {
            continue;
        }

        if (desc.monolithicStaticBlas)
        {
            if (!monolithicInstanceEmitted)
            {
                RtSmokePlanTlasInstance instance;
                instance.kind = RT_SMOKE_PLAN_TLAS_STATIC_BLAS;
                instance.instanceId = desc.firstInstanceId;
                instance.instanceMask = desc.instanceMask;
                CopyIdentityTransform(instance.transform);
                plan.instances.push_back(instance);
                ++plan.emittedInstances;
                monolithicInstanceEmitted = true;
            }
            continue;
        }

        RtSmokePlanTlasInstance instance;
        instance.kind = RT_SMOKE_PLAN_TLAS_STATIC_BUCKET_BLAS;
        instance.instanceId = desc.firstInstanceId + static_cast<uint32_t>(plan.emittedInstances);
        instance.instanceMask = desc.instanceMask;
        instance.meshHash = bucket.bucketKey;
        instance.routeRecordIndex = bucket.routeRecordIndex;
        instance.flags = bucket.activeReasonFlags;
        CopyIdentityTransform(instance.transform);
        plan.instances.push_back(instance);
        ++plan.emittedInstances;
    }

    plan.inactiveResidentGeometryIncluded =
        desc.monolithicStaticBlas &&
        plan.emittedInstances > 0 &&
        (plan.inactiveResidentBuckets > 0 ||
            plan.activeSurfaceCount < plan.residentSurfaceCount ||
            plan.activeVertexCount < plan.residentVertexCount ||
            plan.activeIndexCount < plan.residentIndexCount ||
            plan.activeTriangleCount < plan.residentTriangleCount);
    plan.requiresBucketedStaticBlas = plan.inactiveResidentGeometryIncluded;

    const uint32_t activeSetFlags =
        (desc.monolithicStaticBlas ? 1u : 0u) |
        (desc.hasStaticBlas ? 2u : 0u) |
        (plan.inactiveResidentGeometryIncluded ? 4u : 0u) |
        (plan.requiresBucketedStaticBlas ? 8u : 0u);
    plan.tlasInstanceSignature = HashSmokePlanBytes(plan.tlasInstanceSignature, &plan.emittedInstances, sizeof(plan.emittedInstances));
    plan.tlasInstanceSignature = HashSmokePlanBytes(plan.tlasInstanceSignature, &plan.activeBuckets, sizeof(plan.activeBuckets));
    plan.tlasInstanceSignature = HashSmokePlanBytes(plan.tlasInstanceSignature, &plan.inactiveResidentBuckets, sizeof(plan.inactiveResidentBuckets));
    plan.tlasInstanceSignature = HashSmokePlanBytes(plan.tlasInstanceSignature, &desc.firstInstanceId, sizeof(desc.firstInstanceId));
    plan.tlasInstanceSignature = HashSmokePlanBytes(plan.tlasInstanceSignature, &desc.instanceMask, sizeof(desc.instanceMask));
    plan.tlasInstanceSignature = HashSmokePlanBytes(plan.tlasInstanceSignature, &activeSetFlags, sizeof(activeSetFlags));
    for (const RtSmokePlanTlasInstance& instance : plan.instances)
    {
        plan.tlasInstanceSignature = HashSmokePlanBytes(plan.tlasInstanceSignature, &instance.kind, sizeof(instance.kind));
        plan.tlasInstanceSignature = HashSmokePlanBytes(plan.tlasInstanceSignature, &instance.instanceId, sizeof(instance.instanceId));
        plan.tlasInstanceSignature = HashSmokePlanBytes(plan.tlasInstanceSignature, &instance.instanceMask, sizeof(instance.instanceMask));
        plan.tlasInstanceSignature = HashSmokePlanBytes(plan.tlasInstanceSignature, &instance.meshHash, sizeof(instance.meshHash));
        plan.tlasInstanceSignature = HashSmokePlanBytes(plan.tlasInstanceSignature, &instance.routeRecordIndex, sizeof(instance.routeRecordIndex));
        plan.tlasInstanceSignature = HashSmokePlanBytes(plan.tlasInstanceSignature, &instance.flags, sizeof(instance.flags));
    }
    return plan;
}

bool BuildSmokeStaticTlasBucketObservation(
    const RtSmokeStaticTlasBucketObservationInput& input,
    RtSmokeStaticTlasBucketObservation& observation)
{
    observation = RtSmokeStaticTlasBucketObservation();
    if (!input.valid || input.vertexCount <= 0 || input.indexCount <= 0 || input.triangleCount <= 0)
    {
        return false;
    }

    observation.bucketKey = input.bucketKey;
    observation.resident = true;
    observation.active = input.seenThisFrame;
    observation.hasBlas = input.hasBlas;
    observation.activeReasonFlags = input.seenThisFrame ? input.activeReasonFlags : 0u;
    observation.routeRecordIndex = input.routeRecordIndex;
    observation.residentVertexOffset = input.vertexOffset;
    observation.residentIndexOffset = input.indexOffset;
    observation.residentTriangleOffset = input.triangleOffset;
    observation.residentSurfaceCount = 1;
    observation.residentVertexCount = input.vertexCount;
    observation.residentIndexCount = input.indexCount;
    observation.residentTriangleCount = input.triangleCount;
    if (input.seenThisFrame)
    {
        observation.activeSurfaceCount = 1;
        observation.activeVertexCount = input.vertexCount;
        observation.activeIndexCount = input.indexCount;
        observation.activeTriangleCount = input.triangleCount;
    }
    return true;
}

RtSmokeStaticBucketBlasPlan BuildSmokeStaticBucketBlasPlan(
    const RtSmokeStaticBucketBlasPlanDesc& desc)
{
    RtSmokeStaticBucketBlasPlan plan;
    plan.planSignature = 14695981039346656037ull;
    if (!desc.buckets || desc.bucketCount <= 0)
    {
        return plan;
    }

    const int maxRecords = NormalizeSmokePlanRecordCap(desc.maxRecords);
    const int reserveCount = maxRecords > 0 && maxRecords < desc.bucketCount
        ? maxRecords
        : desc.bucketCount;
    plan.records.reserve(reserveCount);
    for (int bucketIndex = 0; bucketIndex < desc.bucketCount; ++bucketIndex)
    {
        const RtSmokeStaticTlasBucketObservation& bucket = desc.buckets[bucketIndex];
        if (!bucket.resident)
        {
            ++plan.skippedInvalid;
            continue;
        }

        ++plan.residentBuckets;
        if (bucket.active)
        {
            ++plan.activeBuckets;
        }
        else if (desc.activeOnly)
        {
            ++plan.skippedInactive;
            continue;
        }

        if (bucket.residentVertexOffset < 0 ||
            bucket.residentIndexOffset < 0 ||
            bucket.residentTriangleOffset < 0 ||
            bucket.residentVertexCount <= 0 ||
            bucket.residentIndexCount <= 0 ||
            bucket.residentTriangleCount <= 0)
        {
            ++plan.skippedInvalid;
            continue;
        }
        if (maxRecords > 0 && plan.emittedRecords >= maxRecords)
        {
            plan.overflow = true;
            break;
        }

        RtSmokeStaticBucketBlasRecord record;
        record.bucketKey = bucket.bucketKey;
        record.routeRecordIndex = bucket.routeRecordIndex;
        record.activeReasonFlags = bucket.activeReasonFlags;
        record.active = bucket.active;
        record.range.vertexOffset = bucket.residentVertexOffset;
        record.range.vertexCount = bucket.residentVertexCount;
        record.range.indexOffset = bucket.residentIndexOffset;
        record.range.indexCount = bucket.residentIndexCount;
        record.range.triangleOffset = bucket.residentTriangleOffset;
        record.range.triangleCount = bucket.residentTriangleCount;
        plan.records.push_back(record);
        ++plan.emittedRecords;

        plan.planSignature = HashSmokePlanBytes(plan.planSignature, &record.bucketKey, sizeof(record.bucketKey));
        plan.planSignature = HashSmokePlanBytes(plan.planSignature, &record.range, sizeof(record.range));
    }
    const uint32_t summaryFlags =
        (desc.activeOnly ? 1u : 0u) |
        (plan.overflow ? 2u : 0u);
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.residentBuckets, sizeof(plan.residentBuckets));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.activeBuckets, sizeof(plan.activeBuckets));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.emittedRecords, sizeof(plan.emittedRecords));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.skippedInactive, sizeof(plan.skippedInactive));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.skippedInvalid, sizeof(plan.skippedInvalid));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &summaryFlags, sizeof(summaryFlags));
    return plan;
}

RtSmokeStaticBucketTraversalCompatibility BuildSmokeStaticBucketTraversalCompatibility(
    const RtSmokeStaticBucketTraversalCompatibilityInput& input)
{
    RtSmokeStaticBucketTraversalCompatibility plan;
    const int recordCount = input.records && input.recordCount > 0 ? input.recordCount : 0;
    plan.recordCount = recordCount;
    if (recordCount <= 0)
    {
        return plan;
    }

    for (int recordIndex = 0; recordIndex < recordCount; ++recordIndex)
    {
        const RtSmokeStaticBucketBlasRecord& record = input.records[recordIndex];
        if (record.range.vertexOffset != 0 ||
            record.range.indexOffset != 0 ||
            record.range.triangleOffset != 0)
        {
            ++plan.nonZeroOffsetRecords;
        }
    }

    const RtSmokeStaticBucketBlasRecord& firstRecord = input.records[0];
    plan.exactMonolithicRecord =
        recordCount == 1 &&
        firstRecord.range.vertexOffset == 0 &&
        firstRecord.range.indexOffset == 0 &&
        firstRecord.range.triangleOffset == 0 &&
        firstRecord.range.vertexCount == input.totalVertexCount &&
        firstRecord.range.indexCount == input.totalIndexCount &&
        firstRecord.range.triangleCount == input.totalTriangleCount;
    plan.currentStaticShaderCompatible =
        input.shaderSupportsStaticBucketRoutes ||
        plan.exactMonolithicRecord;
    plan.requiresShaderRouteMetadata =
        !input.shaderSupportsStaticBucketRoutes &&
        !plan.exactMonolithicRecord;
    return plan;
}

RtSmokeRouteInstanceNamespacePlan BuildSmokeRouteInstanceNamespacePlan(
    const RtSmokeRouteInstanceNamespacePlanInput& input)
{
    RtSmokeRouteInstanceNamespacePlan plan;
    const int staticRouteCount = input.staticRouteRecordCount > 0 ? input.staticRouteRecordCount : 0;
    const int rigidRouteCount = input.rigidRouteRecordCount > 0 ? input.rigidRouteRecordCount : 0;
    plan.staticFirstInstanceId = input.firstRouteInstanceId;
    plan.rigidFirstInstanceId = input.firstRouteInstanceId;
    plan.rigidRouteInstanceCount = rigidRouteCount;

    const bool staticRoutesRequested = input.enableStaticRoutes && staticRouteCount > 0;
    plan.staticRoutesRequireShaderSupport =
        staticRoutesRequested &&
        !input.shaderSupportsStaticBucketRoutes;
    plan.staticRoutesBlocked = plan.staticRoutesRequireShaderSupport;
    plan.staticRoutesEnabled = staticRoutesRequested && !plan.staticRoutesBlocked;
    if (plan.staticRoutesEnabled)
    {
        plan.staticRouteInstanceCount = staticRouteCount;
        plan.rigidFirstInstanceId = input.firstRouteInstanceId + static_cast<uint32_t>(staticRouteCount);
    }
    else
    {
        plan.staticFirstInstanceId = 0;
    }
    plan.rigidRouteBaseShifted = plan.rigidFirstInstanceId != input.firstRouteInstanceId;
    return plan;
}

static uint64_t HashSmokeStaticRouteTablePlanSummary(
    uint64_t hash,
    const RtSmokeStaticRouteTablePlan& plan)
{
    const uint32_t summaryFlags =
        (plan.blocked ? 1u : 0u) |
        (plan.overflow ? 2u : 0u);
    hash = HashSmokePlanBytes(hash, &plan.inputRecords, sizeof(plan.inputRecords));
    hash = HashSmokePlanBytes(hash, &plan.emittedRecords, sizeof(plan.emittedRecords));
    hash = HashSmokePlanBytes(hash, &plan.skippedDisabled, sizeof(plan.skippedDisabled));
    hash = HashSmokePlanBytes(hash, &plan.skippedInvalid, sizeof(plan.skippedInvalid));
    hash = HashSmokePlanBytes(hash, &summaryFlags, sizeof(summaryFlags));
    return hash;
}

RtSmokeStaticRouteTablePlan BuildSmokeStaticRouteTablePlan(
    const RtSmokeStaticRouteTablePlanInput& input)
{
    RtSmokeStaticRouteTablePlan plan;
    plan.tableSignature = 14695981039346656037ull;
    const int recordCount = input.records && input.recordCount > 0 ? input.recordCount : 0;
    plan.inputRecords = recordCount;
    plan.blocked = input.routeNamespace.staticRoutesBlocked;
    if (recordCount <= 0)
    {
        plan.tableSignature = HashSmokeStaticRouteTablePlanSummary(plan.tableSignature, plan);
        return plan;
    }
    if (!input.routeNamespace.staticRoutesEnabled)
    {
        plan.skippedDisabled = recordCount;
        plan.tableSignature = HashSmokeStaticRouteTablePlanSummary(plan.tableSignature, plan);
        return plan;
    }

    const int maxRecords = NormalizeSmokePlanRecordCap(input.maxRecords);
    const int reserveCount = maxRecords > 0 && maxRecords < recordCount
        ? maxRecords
        : recordCount;
    plan.records.reserve(reserveCount);
    for (int recordIndex = 0; recordIndex < recordCount; ++recordIndex)
    {
        if (maxRecords > 0 && plan.emittedRecords >= maxRecords)
        {
            plan.overflow = true;
            break;
        }

        const RtSmokeStaticBucketBlasRecord& sourceRecord = input.records[recordIndex];
        if (sourceRecord.range.vertexOffset < 0 ||
            sourceRecord.range.vertexCount <= 0 ||
            sourceRecord.range.indexOffset < 0 ||
            sourceRecord.range.indexCount <= 0 ||
            sourceRecord.range.triangleOffset < 0 ||
            sourceRecord.range.triangleCount <= 0)
        {
            ++plan.skippedInvalid;
            continue;
        }

        RtSmokeStaticRouteTableRecord routeRecord;
        routeRecord.bucketKey = sourceRecord.bucketKey;
        routeRecord.instanceId =
            input.routeNamespace.staticFirstInstanceId + static_cast<uint32_t>(plan.emittedRecords);
        routeRecord.routeRecordIndex = sourceRecord.routeRecordIndex;
        routeRecord.activeReasonFlags = sourceRecord.activeReasonFlags;
        routeRecord.range = sourceRecord.range;
        plan.records.push_back(routeRecord);
        ++plan.emittedRecords;

        plan.tableSignature = HashSmokePlanBytes(plan.tableSignature, &routeRecord.bucketKey, sizeof(routeRecord.bucketKey));
        plan.tableSignature = HashSmokePlanBytes(plan.tableSignature, &routeRecord.instanceId, sizeof(routeRecord.instanceId));
        plan.tableSignature = HashSmokePlanBytes(plan.tableSignature, &routeRecord.routeRecordIndex, sizeof(routeRecord.routeRecordIndex));
        plan.tableSignature = HashSmokePlanBytes(plan.tableSignature, &routeRecord.activeReasonFlags, sizeof(routeRecord.activeReasonFlags));
        plan.tableSignature = HashSmokePlanBytes(plan.tableSignature, &routeRecord.range, sizeof(routeRecord.range));
    }
    plan.tableSignature = HashSmokeStaticRouteTablePlanSummary(plan.tableSignature, plan);
    return plan;
}

RtSmokeStaticBucketBlasBuildPlan BuildSmokeStaticBucketBlasBuildPlan(
    const RtSmokeStaticBucketBlasBuildPlanInput& input)
{
    RtSmokeStaticBucketBlasBuildPlan plan;
    if (!input.submitBuilds)
    {
        plan.skipBuild = true;
        return plan;
    }

    plan.signatureChanged =
        !input.signatureValid ||
        input.previousBlasInputSignature != input.currentBlasInputSignature;
    plan.createBlas = !input.hasBlas || !input.blasInputsCompatible;
    plan.submitBuild =
        input.forceRebuild ||
        input.uploadRequired ||
        plan.createBlas ||
        plan.signatureChanged;
    plan.skipBuild = !plan.submitBuild;
    return plan;
}

RtSmokeStaticBucketBlasBuildBatchPlan BuildSmokeStaticBucketBlasBuildBatchPlan(
    const RtSmokeStaticBucketBlasBuildBatchPlanInput& input)
{
    RtSmokeStaticBucketBlasBuildBatchPlan plan;
    plan.planSignature = 14695981039346656037ull;
    const int observationCount = input.observations && input.observationCount > 0 ? input.observationCount : 0;
    plan.inputRecords = observationCount;
    if (observationCount <= 0)
    {
        return plan;
    }

    const int maxRecords = NormalizeSmokePlanRecordCap(input.maxRecords);
    const int reserveCount = maxRecords > 0 && maxRecords < observationCount
        ? maxRecords
        : observationCount;
    plan.records.reserve(reserveCount);
    for (int observationIndex = 0; observationIndex < observationCount; ++observationIndex)
    {
        if (maxRecords > 0 && plan.emittedRecords >= maxRecords)
        {
            plan.overflow = true;
            break;
        }

        const RtSmokeStaticBucketBlasBuildObservation& observation = input.observations[observationIndex];
        RtSmokeStaticBucketBlasBuildPlanInput buildInput;
        buildInput.submitBuilds = input.submitBuilds;
        buildInput.forceRebuild = input.forceRebuild;
        buildInput.hasBlas = observation.hasBlas;
        buildInput.uploadRequired = observation.uploadRequired;
        buildInput.blasInputsCompatible = observation.blasInputsCompatible;
        buildInput.signatureValid = observation.signatureValid;
        buildInput.previousBlasInputSignature = observation.previousBlasInputSignature;
        buildInput.currentBlasInputSignature = observation.currentBlasInputSignature;
        const RtSmokeStaticBucketBlasBuildPlan buildPlan =
            BuildSmokeStaticBucketBlasBuildPlan(buildInput);

        RtSmokeStaticBucketBlasBuildBatchRecord record;
        record.bucketKey = observation.bucketKey;
        record.buildPlan = buildPlan;
        plan.records.push_back(record);
        ++plan.emittedRecords;

        if (buildPlan.createBlas)
        {
            ++plan.createBlasRecords;
        }
        if (buildPlan.submitBuild)
        {
            ++plan.submitBuildRecords;
        }
        if (buildPlan.skipBuild)
        {
            ++plan.skippedBuildRecords;
        }
        if (buildPlan.signatureChanged)
        {
            ++plan.signatureChangedRecords;
        }
        if (observation.uploadRequired)
        {
            ++plan.uploadRequiredRecords;
        }
        if (!observation.blasInputsCompatible)
        {
            ++plan.incompatibleRecords;
        }
        if (!observation.hasBlas)
        {
            ++plan.missingBlasRecords;
        }

        const uint32_t createBit = buildPlan.createBlas ? 1u : 0u;
        const uint32_t submitBit = buildPlan.submitBuild ? 1u : 0u;
        const uint32_t skipBit = buildPlan.skipBuild ? 1u : 0u;
        const uint32_t signatureChangedBit = buildPlan.signatureChanged ? 1u : 0u;
        plan.planSignature = HashSmokePlanBytes(plan.planSignature, &record.bucketKey, sizeof(record.bucketKey));
        plan.planSignature = HashSmokePlanBytes(plan.planSignature, &createBit, sizeof(createBit));
        plan.planSignature = HashSmokePlanBytes(plan.planSignature, &submitBit, sizeof(submitBit));
        plan.planSignature = HashSmokePlanBytes(plan.planSignature, &skipBit, sizeof(skipBit));
        plan.planSignature = HashSmokePlanBytes(plan.planSignature, &signatureChangedBit, sizeof(signatureChangedBit));
        plan.planSignature = HashSmokePlanBytes(plan.planSignature, &observation.previousBlasInputSignature, sizeof(observation.previousBlasInputSignature));
        plan.planSignature = HashSmokePlanBytes(plan.planSignature, &observation.currentBlasInputSignature, sizeof(observation.currentBlasInputSignature));
    }
    const uint32_t summaryFlags =
        (input.submitBuilds ? 1u : 0u) |
        (input.forceRebuild ? 2u : 0u) |
        (plan.overflow ? 4u : 0u);
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.inputRecords, sizeof(plan.inputRecords));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.emittedRecords, sizeof(plan.emittedRecords));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.createBlasRecords, sizeof(plan.createBlasRecords));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.submitBuildRecords, sizeof(plan.submitBuildRecords));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.skippedBuildRecords, sizeof(plan.skippedBuildRecords));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.signatureChangedRecords, sizeof(plan.signatureChangedRecords));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.uploadRequiredRecords, sizeof(plan.uploadRequiredRecords));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.incompatibleRecords, sizeof(plan.incompatibleRecords));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.missingBlasRecords, sizeof(plan.missingBlasRecords));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &summaryFlags, sizeof(summaryFlags));
    return plan;
}

RtSmokeStaticBvhBucketSignature BuildSmokeStaticBvhBucketSignature(
    const RtSmokeStaticBvhBucketSignatureInput& input)
{
    RtSmokeStaticBvhBucketSignature signature;
    const RtSmokeStaticTlasBucketObservation& bucket = input.bucket;
    signature.bucketKey = bucket.bucketKey;
    signature.resident = bucket.resident;
    signature.active = bucket.active;

    uint64_t residentHash = 14695981039346656037ull;
    const uint32_t residentBit = bucket.resident ? 1u : 0u;
    residentHash = HashSmokePlanBytes(residentHash, &bucket.bucketKey, sizeof(bucket.bucketKey));
    residentHash = HashSmokePlanBytes(residentHash, &residentBit, sizeof(residentBit));
    residentHash = HashSmokePlanBytes(residentHash, &bucket.residentSurfaceCount, sizeof(bucket.residentSurfaceCount));
    residentHash = HashSmokePlanBytes(residentHash, &bucket.residentVertexCount, sizeof(bucket.residentVertexCount));
    residentHash = HashSmokePlanBytes(residentHash, &bucket.residentIndexCount, sizeof(bucket.residentIndexCount));
    residentHash = HashSmokePlanBytes(residentHash, &bucket.residentTriangleCount, sizeof(bucket.residentTriangleCount));
    signature.residentSignature = residentHash;

    uint64_t activeHash = 14695981039346656037ull;
    const uint32_t activeBit = bucket.active ? 1u : 0u;
    activeHash = HashSmokePlanBytes(activeHash, &bucket.bucketKey, sizeof(bucket.bucketKey));
    activeHash = HashSmokePlanBytes(activeHash, &activeBit, sizeof(activeBit));
    if (bucket.active)
    {
        activeHash = HashSmokePlanBytes(activeHash, &bucket.activeReasonFlags, sizeof(bucket.activeReasonFlags));
        activeHash = HashSmokePlanBytes(activeHash, &bucket.activeSurfaceCount, sizeof(bucket.activeSurfaceCount));
        activeHash = HashSmokePlanBytes(activeHash, &bucket.activeVertexCount, sizeof(bucket.activeVertexCount));
        activeHash = HashSmokePlanBytes(activeHash, &bucket.activeIndexCount, sizeof(bucket.activeIndexCount));
        activeHash = HashSmokePlanBytes(activeHash, &bucket.activeTriangleCount, sizeof(bucket.activeTriangleCount));
    }
    signature.activeSignature = activeHash;

    uint64_t geometryHash = 14695981039346656037ull;
    geometryHash = HashSmokePlanBytes(geometryHash, &bucket.bucketKey, sizeof(bucket.bucketKey));
    geometryHash = HashSmokePlanBytes(geometryHash, &input.geometryContentSignature, sizeof(input.geometryContentSignature));
    geometryHash = HashSmokePlanBytes(geometryHash, &bucket.residentVertexOffset, sizeof(bucket.residentVertexOffset));
    geometryHash = HashSmokePlanBytes(geometryHash, &bucket.residentVertexCount, sizeof(bucket.residentVertexCount));
    geometryHash = HashSmokePlanBytes(geometryHash, &bucket.residentIndexOffset, sizeof(bucket.residentIndexOffset));
    geometryHash = HashSmokePlanBytes(geometryHash, &bucket.residentIndexCount, sizeof(bucket.residentIndexCount));
    geometryHash = HashSmokePlanBytes(geometryHash, &bucket.residentTriangleOffset, sizeof(bucket.residentTriangleOffset));
    geometryHash = HashSmokePlanBytes(geometryHash, &bucket.residentTriangleCount, sizeof(bucket.residentTriangleCount));
    signature.geometryInputSignature = geometryHash;

    signature.blasInputSignature = geometryHash;
    return signature;
}

RtSmokeStaticBucketBlasBuildObservationPlan BuildSmokeStaticBucketBlasBuildObservationPlan(
    const RtSmokeStaticBucketBlasBuildObservationPlanInput& input)
{
    RtSmokeStaticBucketBlasBuildObservationPlan plan;
    plan.planSignature = 14695981039346656037ull;
    const int currentBucketCount = input.currentBuckets && input.currentBucketCount > 0 ? input.currentBucketCount : 0;
    plan.inputBuckets = currentBucketCount;
    if (currentBucketCount <= 0)
    {
        return plan;
    }

    const int maxRecords = NormalizeSmokePlanRecordCap(input.maxRecords);
    const int reserveCount = maxRecords > 0 && maxRecords < currentBucketCount
        ? maxRecords
        : currentBucketCount;
    plan.observations.reserve(reserveCount);
    for (int bucketIndex = 0; bucketIndex < currentBucketCount; ++bucketIndex)
    {
        const RtSmokeStaticBvhBucketSignature& currentBucket = input.currentBuckets[bucketIndex];
        if (!currentBucket.active)
        {
            ++plan.skippedInactive;
            continue;
        }
        if (maxRecords > 0 && plan.emittedObservations >= maxRecords)
        {
            plan.overflow = true;
            break;
        }

        const RtSmokeStaticBucketBlasCacheState* previousBucket = nullptr;
        if (input.previousBuckets && input.previousBucketCount > 0)
        {
            for (int previousIndex = 0; previousIndex < input.previousBucketCount; ++previousIndex)
            {
                if (input.previousBuckets[previousIndex].bucketKey == currentBucket.bucketKey)
                {
                    previousBucket = &input.previousBuckets[previousIndex];
                    break;
                }
            }
        }

        RtSmokeStaticBucketBlasBuildObservation observation;
        observation.bucketKey = currentBucket.bucketKey;
        observation.currentBlasInputSignature = currentBucket.blasInputSignature;
        if (previousBucket)
        {
            observation.hasBlas = previousBucket->hasBlas;
            observation.blasInputsCompatible = previousBucket->blasInputsCompatible;
            observation.signatureValid = true;
            observation.previousBlasInputSignature = previousBucket->blasInputSignature;
        }

        const bool changedSignature =
            !previousBucket ||
            previousBucket->blasInputSignature != currentBucket.blasInputSignature;
        observation.uploadRequired =
            changedSignature ||
            !observation.hasBlas;
        plan.observations.push_back(observation);
        ++plan.emittedObservations;

        const bool cacheHit =
            previousBucket &&
            observation.hasBlas &&
            observation.blasInputsCompatible &&
            !changedSignature;
        if (cacheHit)
        {
            ++plan.cacheHits;
        }
        else
        {
            ++plan.cacheMisses;
        }
        if (changedSignature)
        {
            ++plan.signatureChanged;
        }
        if (observation.uploadRequired)
        {
            ++plan.uploadRequired;
        }

        const uint32_t hasPreviousBit = previousBucket ? 1u : 0u;
        const uint32_t uploadBit = observation.uploadRequired ? 1u : 0u;
        const uint32_t hitBit = cacheHit ? 1u : 0u;
        plan.planSignature = HashSmokePlanBytes(plan.planSignature, &observation.bucketKey, sizeof(observation.bucketKey));
        plan.planSignature = HashSmokePlanBytes(plan.planSignature, &hasPreviousBit, sizeof(hasPreviousBit));
        plan.planSignature = HashSmokePlanBytes(plan.planSignature, &hitBit, sizeof(hitBit));
        plan.planSignature = HashSmokePlanBytes(plan.planSignature, &uploadBit, sizeof(uploadBit));
        plan.planSignature = HashSmokePlanBytes(plan.planSignature, &observation.previousBlasInputSignature, sizeof(observation.previousBlasInputSignature));
        plan.planSignature = HashSmokePlanBytes(plan.planSignature, &observation.currentBlasInputSignature, sizeof(observation.currentBlasInputSignature));
    }
    const uint32_t summaryFlags = plan.overflow ? 1u : 0u;
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.inputBuckets, sizeof(plan.inputBuckets));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.emittedObservations, sizeof(plan.emittedObservations));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.cacheHits, sizeof(plan.cacheHits));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.cacheMisses, sizeof(plan.cacheMisses));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.signatureChanged, sizeof(plan.signatureChanged));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.uploadRequired, sizeof(plan.uploadRequired));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.skippedInactive, sizeof(plan.skippedInactive));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &summaryFlags, sizeof(summaryFlags));
    return plan;
}

RtSmokeStaticBucketWorkPlan BuildSmokeStaticBucketWorkPlan(
    const RtSmokeStaticBucketWorkPlanInput& input)
{
    RtSmokeStaticBucketWorkPlan plan;
    plan.planSignature = 14695981039346656037ull;
    if (input.buckets && input.bucketCount > 0)
    {
        plan.bucketSignatures.reserve(input.bucketCount);
        for (int bucketIndex = 0; bucketIndex < input.bucketCount; ++bucketIndex)
        {
            RtSmokeStaticBvhBucketSignatureInput signatureInput;
            signatureInput.bucket = input.buckets[bucketIndex];
            signatureInput.geometryContentSignature = input.geometryContentSignature;
            signatureInput.materialGeneration = input.materialGeneration;
            plan.bucketSignatures.push_back(BuildSmokeStaticBvhBucketSignature(signatureInput));
        }
    }

    RtSmokeStaticTlasActiveSetPlanDesc activeSetDesc;
    activeSetDesc.buckets = input.buckets;
    activeSetDesc.bucketCount = input.bucketCount;
    activeSetDesc.monolithicStaticBlas = input.monolithicStaticBlas;
    activeSetDesc.hasStaticBlas = input.hasStaticBlas;
    plan.activeSetPlan = BuildSmokeStaticTlasActiveSetPlan(activeSetDesc);

    RtSmokeStaticBucketBlasPlanDesc bucketBlasDesc;
    bucketBlasDesc.buckets = input.buckets;
    bucketBlasDesc.bucketCount = input.bucketCount;
    bucketBlasDesc.activeOnly = true;
    bucketBlasDesc.maxRecords = input.maxBucketRecords;
    plan.bucketBlasPlan = BuildSmokeStaticBucketBlasPlan(bucketBlasDesc);

    RtSmokeStaticBucketTraversalCompatibilityInput traversalInput;
    traversalInput.records = plan.bucketBlasPlan.records.empty() ? nullptr : plan.bucketBlasPlan.records.data();
    traversalInput.recordCount = plan.bucketBlasPlan.emittedRecords;
    traversalInput.totalVertexCount = input.totalVertexCount;
    traversalInput.totalIndexCount = input.totalIndexCount;
    traversalInput.totalTriangleCount = input.totalTriangleCount;
    traversalInput.shaderSupportsStaticBucketRoutes = input.shaderSupportsStaticBucketRoutes;
    plan.traversalCompatibility = BuildSmokeStaticBucketTraversalCompatibility(traversalInput);

    RtSmokeRouteInstanceNamespacePlanInput routeNamespaceInput;
    routeNamespaceInput.staticRouteRecordCount = plan.bucketBlasPlan.emittedRecords;
    routeNamespaceInput.rigidRouteRecordCount = input.rigidRouteRecordCount;
    routeNamespaceInput.firstRouteInstanceId = input.firstRouteInstanceId;
    routeNamespaceInput.enableStaticRoutes =
        input.enableStaticRoutes &&
        plan.bucketBlasPlan.emittedRecords > 0 &&
        !plan.traversalCompatibility.exactMonolithicRecord;
    routeNamespaceInput.shaderSupportsStaticBucketRoutes = input.shaderSupportsStaticBucketRoutes;
    plan.routeNamespace = BuildSmokeRouteInstanceNamespacePlan(routeNamespaceInput);

    RtSmokeStaticRouteTablePlanInput routeTableInput;
    routeTableInput.records = plan.bucketBlasPlan.records.empty() ? nullptr : plan.bucketBlasPlan.records.data();
    routeTableInput.recordCount = plan.bucketBlasPlan.emittedRecords;
    routeTableInput.maxRecords = input.maxRouteRecords;
    routeTableInput.routeNamespace = plan.routeNamespace;
    plan.routeTablePlan = BuildSmokeStaticRouteTablePlan(routeTableInput);

    RtSmokeStaticBucketBlasBuildObservationPlanInput observationInput;
    observationInput.currentBuckets = plan.bucketSignatures.empty() ? nullptr : plan.bucketSignatures.data();
    observationInput.currentBucketCount = static_cast<int>(plan.bucketSignatures.size());
    observationInput.previousBuckets = input.previousBuckets;
    observationInput.previousBucketCount = input.previousBucketCount;
    observationInput.maxRecords = input.maxBuildRecords;
    plan.buildObservationPlan = BuildSmokeStaticBucketBlasBuildObservationPlan(observationInput);

    RtSmokeStaticBucketBlasBuildBatchPlanInput buildBatchInput;
    buildBatchInput.observations = plan.buildObservationPlan.observations.empty()
        ? nullptr
        : plan.buildObservationPlan.observations.data();
    buildBatchInput.observationCount = plan.buildObservationPlan.emittedObservations;
    buildBatchInput.submitBuilds = input.submitBuilds;
    buildBatchInput.forceRebuild = input.forceRebuild;
    buildBatchInput.maxRecords = input.maxBuildRecords;
    plan.buildBatchPlan = BuildSmokeStaticBucketBlasBuildBatchPlan(buildBatchInput);

    const int signatureCount = static_cast<int>(plan.bucketSignatures.size());
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &signatureCount, sizeof(signatureCount));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.activeSetPlan.activeSetSignature, sizeof(plan.activeSetPlan.activeSetSignature));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.activeSetPlan.residentSetSignature, sizeof(plan.activeSetPlan.residentSetSignature));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.activeSetPlan.tlasInstanceSignature, sizeof(plan.activeSetPlan.tlasInstanceSignature));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.bucketBlasPlan.planSignature, sizeof(plan.bucketBlasPlan.planSignature));
    const uint32_t traversalFlags =
        (plan.traversalCompatibility.exactMonolithicRecord ? 1u : 0u) |
        (plan.traversalCompatibility.currentStaticShaderCompatible ? 2u : 0u) |
        (plan.traversalCompatibility.requiresShaderRouteMetadata ? 4u : 0u);
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.traversalCompatibility.recordCount, sizeof(plan.traversalCompatibility.recordCount));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.traversalCompatibility.nonZeroOffsetRecords, sizeof(plan.traversalCompatibility.nonZeroOffsetRecords));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &traversalFlags, sizeof(traversalFlags));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.routeTablePlan.tableSignature, sizeof(plan.routeTablePlan.tableSignature));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.buildObservationPlan.planSignature, sizeof(plan.buildObservationPlan.planSignature));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.buildBatchPlan.planSignature, sizeof(plan.buildBatchPlan.planSignature));
    const uint32_t routeNamespaceFlags =
        (plan.routeNamespace.staticRoutesEnabled ? 1u : 0u) |
        (plan.routeNamespace.staticRoutesRequireShaderSupport ? 2u : 0u) |
        (plan.routeNamespace.staticRoutesBlocked ? 4u : 0u) |
        (plan.routeNamespace.rigidRouteBaseShifted ? 8u : 0u);
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.routeNamespace.staticFirstInstanceId, sizeof(plan.routeNamespace.staticFirstInstanceId));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.routeNamespace.rigidFirstInstanceId, sizeof(plan.routeNamespace.rigidFirstInstanceId));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.routeNamespace.staticRouteInstanceCount, sizeof(plan.routeNamespace.staticRouteInstanceCount));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &plan.routeNamespace.rigidRouteInstanceCount, sizeof(plan.routeNamespace.rigidRouteInstanceCount));
    plan.planSignature = HashSmokePlanBytes(plan.planSignature, &routeNamespaceFlags, sizeof(routeNamespaceFlags));
    return plan;
}

RtSmokeStaticBucketWorkPlanSnapshot CaptureSmokeStaticBucketWorkPlanSnapshot(
    const RtSmokeStaticBucketWorkPlanInput& input)
{
    RtSmokeStaticBucketWorkPlanSnapshot snapshot;
    const bool hasBuckets = input.buckets && input.bucketCount > 0;
    if (hasBuckets)
    {
        snapshot.buckets.assign(input.buckets, input.buckets + input.bucketCount);
    }
    if (hasBuckets && input.previousBuckets && input.previousBucketCount > 0)
    {
        const int maxBuildRecords = NormalizeSmokePlanRecordCap(input.maxBuildRecords);
        int activeBuildObservations = 0;
        for (int bucketIndex = 0; bucketIndex < input.bucketCount; ++bucketIndex)
        {
            const RtSmokeStaticTlasBucketObservation& bucket = input.buckets[bucketIndex];
            if (!bucket.active)
            {
                continue;
            }
            if (maxBuildRecords > 0 && activeBuildObservations >= maxBuildRecords)
            {
                break;
            }
            ++activeBuildObservations;

            bool alreadyCaptured = false;
            for (const RtSmokeStaticBucketBlasCacheState& capturedBucket : snapshot.previousBuckets)
            {
                if (capturedBucket.bucketKey == bucket.bucketKey)
                {
                    alreadyCaptured = true;
                    break;
                }
            }
            if (alreadyCaptured)
            {
                continue;
            }

            for (int previousIndex = 0; previousIndex < input.previousBucketCount; ++previousIndex)
            {
                const RtSmokeStaticBucketBlasCacheState& previousBucket =
                    input.previousBuckets[previousIndex];
                if (previousBucket.bucketKey == bucket.bucketKey)
                {
                    snapshot.previousBuckets.push_back(previousBucket);
                    break;
                }
            }
        }
    }
    snapshot.geometryContentSignature = input.geometryContentSignature;
    snapshot.materialGeneration = input.materialGeneration;
    snapshot.totalVertexCount = input.totalVertexCount;
    snapshot.totalIndexCount = input.totalIndexCount;
    snapshot.totalTriangleCount = input.totalTriangleCount;
    snapshot.monolithicStaticBlas = input.monolithicStaticBlas;
    snapshot.hasStaticBlas = input.hasStaticBlas;
    snapshot.submitBuilds = input.submitBuilds;
    snapshot.forceRebuild = input.forceRebuild;
    snapshot.enableStaticRoutes = input.enableStaticRoutes;
    snapshot.shaderSupportsStaticBucketRoutes = input.shaderSupportsStaticBucketRoutes;
    snapshot.firstRouteInstanceId = input.firstRouteInstanceId;
    snapshot.rigidRouteRecordCount = input.rigidRouteRecordCount;
    snapshot.maxBucketRecords = input.maxBucketRecords;
    snapshot.maxRouteRecords = input.maxRouteRecords;
    snapshot.maxBuildRecords = input.maxBuildRecords;
    return snapshot;
}

uint64_t BuildSmokeStaticBucketWorkPlanInputToken(
    const RtSmokeStaticBucketWorkPlanInput& input)
{
    uint64_t hash = 1469598103934665603ull;
    const int bucketCount = input.buckets && input.bucketCount > 0 ? input.bucketCount : 0;
    hash = HashSmokePlanBytes(hash, &bucketCount, sizeof(bucketCount));
    hash = HashSmokePlanBytes(hash, &input.geometryContentSignature, sizeof(input.geometryContentSignature));
    hash = HashSmokePlanBytes(hash, &input.materialGeneration, sizeof(input.materialGeneration));
    hash = HashSmokePlanBytes(hash, &input.totalVertexCount, sizeof(input.totalVertexCount));
    hash = HashSmokePlanBytes(hash, &input.totalIndexCount, sizeof(input.totalIndexCount));
    hash = HashSmokePlanBytes(hash, &input.totalTriangleCount, sizeof(input.totalTriangleCount));
    const uint32_t flags =
        (input.submitBuilds ? 1u : 0u) |
        (input.forceRebuild ? 2u : 0u) |
        (input.enableStaticRoutes ? 4u : 0u) |
        (input.shaderSupportsStaticBucketRoutes ? 8u : 0u) |
        (input.monolithicStaticBlas ? 16u : 0u) |
        (input.hasStaticBlas ? 32u : 0u);
    hash = HashSmokePlanBytes(hash, &flags, sizeof(flags));
    hash = HashSmokePlanBytes(hash, &input.firstRouteInstanceId, sizeof(input.firstRouteInstanceId));
    hash = HashSmokePlanBytes(hash, &input.rigidRouteRecordCount, sizeof(input.rigidRouteRecordCount));
    const int maxBucketRecords = NormalizeSmokePlanRecordCap(input.maxBucketRecords);
    const int maxRouteRecords = NormalizeSmokePlanRecordCap(input.maxRouteRecords);
    const int maxBuildRecords = NormalizeSmokePlanRecordCap(input.maxBuildRecords);
    hash = HashSmokePlanBytes(hash, &maxBucketRecords, sizeof(maxBucketRecords));
    hash = HashSmokePlanBytes(hash, &maxRouteRecords, sizeof(maxRouteRecords));
    hash = HashSmokePlanBytes(hash, &maxBuildRecords, sizeof(maxBuildRecords));
    if (bucketCount > 0)
    {
        for (int bucketIndex = 0; bucketIndex < bucketCount; ++bucketIndex)
        {
            const RtSmokeStaticTlasBucketObservation& bucket = input.buckets[bucketIndex];
            const uint32_t bucketFlags =
                (bucket.resident ? 1u : 0u) |
                (bucket.active ? 2u : 0u) |
                (bucket.active && bucket.hasBlas ? 4u : 0u);
            hash = HashSmokePlanBytes(hash, &bucket.bucketKey, sizeof(bucket.bucketKey));
            hash = HashSmokePlanBytes(hash, &bucketFlags, sizeof(bucketFlags));
            if (bucket.active)
            {
                hash = HashSmokePlanBytes(hash, &bucket.routeRecordIndex, sizeof(bucket.routeRecordIndex));
                hash = HashSmokePlanBytes(hash, &bucket.activeReasonFlags, sizeof(bucket.activeReasonFlags));
            }
            hash = HashSmokePlanBytes(hash, &bucket.residentSurfaceCount, sizeof(bucket.residentSurfaceCount));
            hash = HashSmokePlanBytes(hash, &bucket.residentVertexOffset, sizeof(bucket.residentVertexOffset));
            hash = HashSmokePlanBytes(hash, &bucket.residentVertexCount, sizeof(bucket.residentVertexCount));
            hash = HashSmokePlanBytes(hash, &bucket.residentIndexOffset, sizeof(bucket.residentIndexOffset));
            hash = HashSmokePlanBytes(hash, &bucket.residentIndexCount, sizeof(bucket.residentIndexCount));
            hash = HashSmokePlanBytes(hash, &bucket.residentTriangleOffset, sizeof(bucket.residentTriangleOffset));
            hash = HashSmokePlanBytes(hash, &bucket.residentTriangleCount, sizeof(bucket.residentTriangleCount));
            if (bucket.active)
            {
                hash = HashSmokePlanBytes(hash, &bucket.activeSurfaceCount, sizeof(bucket.activeSurfaceCount));
                hash = HashSmokePlanBytes(hash, &bucket.activeVertexCount, sizeof(bucket.activeVertexCount));
                hash = HashSmokePlanBytes(hash, &bucket.activeIndexCount, sizeof(bucket.activeIndexCount));
                hash = HashSmokePlanBytes(hash, &bucket.activeTriangleCount, sizeof(bucket.activeTriangleCount));
            }
        }
    }
    if (bucketCount > 0 && input.previousBuckets && input.previousBucketCount > 0)
    {
        int previousBuildObservations = 0;
        for (int bucketIndex = 0; bucketIndex < bucketCount; ++bucketIndex)
        {
            const RtSmokeStaticTlasBucketObservation& bucket = input.buckets[bucketIndex];
            if (!bucket.active)
            {
                continue;
            }
            if (maxBuildRecords > 0 && previousBuildObservations >= maxBuildRecords)
            {
                break;
            }
            ++previousBuildObservations;

            const RtSmokeStaticBucketBlasCacheState* previousBucket = nullptr;
            for (int previousIndex = 0; previousIndex < input.previousBucketCount; ++previousIndex)
            {
                if (input.previousBuckets[previousIndex].bucketKey == bucket.bucketKey)
                {
                    previousBucket = &input.previousBuckets[previousIndex];
                    break;
                }
            }

            const uint32_t hasPreviousBit = previousBucket ? 1u : 0u;
            hash = HashSmokePlanBytes(hash, &bucket.bucketKey, sizeof(bucket.bucketKey));
            hash = HashSmokePlanBytes(hash, &hasPreviousBit, sizeof(hasPreviousBit));
            if (!previousBucket)
            {
                continue;
            }

            const uint32_t previousFlags =
                (previousBucket->hasBlas ? 1u : 0u) |
                (previousBucket->blasInputsCompatible ? 2u : 0u);
            hash = HashSmokePlanBytes(hash, &previousBucket->blasInputSignature, sizeof(previousBucket->blasInputSignature));
            hash = HashSmokePlanBytes(hash, &previousFlags, sizeof(previousFlags));
        }
    }
    return hash;
}

uint64_t BuildSmokeStaticBucketWorkPlanInputToken(
    const RtSmokeStaticBucketWorkPlanSnapshot& snapshot)
{
    RtSmokeStaticBucketWorkPlanInput input;
    input.buckets = snapshot.buckets.empty() ? nullptr : snapshot.buckets.data();
    input.bucketCount = static_cast<int>(snapshot.buckets.size());
    input.previousBuckets = snapshot.previousBuckets.empty() ? nullptr : snapshot.previousBuckets.data();
    input.previousBucketCount = static_cast<int>(snapshot.previousBuckets.size());
    input.geometryContentSignature = snapshot.geometryContentSignature;
    input.materialGeneration = snapshot.materialGeneration;
    input.totalVertexCount = snapshot.totalVertexCount;
    input.totalIndexCount = snapshot.totalIndexCount;
    input.totalTriangleCount = snapshot.totalTriangleCount;
    input.monolithicStaticBlas = snapshot.monolithicStaticBlas;
    input.hasStaticBlas = snapshot.hasStaticBlas;
    input.submitBuilds = snapshot.submitBuilds;
    input.forceRebuild = snapshot.forceRebuild;
    input.enableStaticRoutes = snapshot.enableStaticRoutes;
    input.shaderSupportsStaticBucketRoutes = snapshot.shaderSupportsStaticBucketRoutes;
    input.firstRouteInstanceId = snapshot.firstRouteInstanceId;
    input.rigidRouteRecordCount = snapshot.rigidRouteRecordCount;
    input.maxBucketRecords = snapshot.maxBucketRecords;
    input.maxRouteRecords = snapshot.maxRouteRecords;
    input.maxBuildRecords = snapshot.maxBuildRecords;
    return BuildSmokeStaticBucketWorkPlanInputToken(input);
}

RtSmokeStaticBucketWorkPlan BuildSmokeStaticBucketWorkPlan(
    const RtSmokeStaticBucketWorkPlanSnapshot& snapshot)
{
    RtSmokeStaticBucketWorkPlanInput input;
    input.buckets = snapshot.buckets.empty() ? nullptr : snapshot.buckets.data();
    input.bucketCount = static_cast<int>(snapshot.buckets.size());
    input.previousBuckets = snapshot.previousBuckets.empty() ? nullptr : snapshot.previousBuckets.data();
    input.previousBucketCount = static_cast<int>(snapshot.previousBuckets.size());
    input.geometryContentSignature = snapshot.geometryContentSignature;
    input.materialGeneration = snapshot.materialGeneration;
    input.totalVertexCount = snapshot.totalVertexCount;
    input.totalIndexCount = snapshot.totalIndexCount;
    input.totalTriangleCount = snapshot.totalTriangleCount;
    input.monolithicStaticBlas = snapshot.monolithicStaticBlas;
    input.hasStaticBlas = snapshot.hasStaticBlas;
    input.submitBuilds = snapshot.submitBuilds;
    input.forceRebuild = snapshot.forceRebuild;
    input.enableStaticRoutes = snapshot.enableStaticRoutes;
    input.shaderSupportsStaticBucketRoutes = snapshot.shaderSupportsStaticBucketRoutes;
    input.firstRouteInstanceId = snapshot.firstRouteInstanceId;
    input.rigidRouteRecordCount = snapshot.rigidRouteRecordCount;
    input.maxBucketRecords = snapshot.maxBucketRecords;
    input.maxRouteRecords = snapshot.maxRouteRecords;
    input.maxBuildRecords = snapshot.maxBuildRecords;
    return BuildSmokeStaticBucketWorkPlan(input);
}

RtSmokeStaticBucketWorkPlanTimedResult BuildSmokeStaticBucketWorkPlanTimedResult(
    const RtSmokeStaticBucketWorkPlanSnapshot& snapshot)
{
    const auto start = std::chrono::steady_clock::now();
    RtSmokeStaticBucketWorkPlanTimedResult result;
    result.plan = BuildSmokeStaticBucketWorkPlan(snapshot);
    const auto end = std::chrono::steady_clock::now();
    result.planningTimeMicros = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
    return result;
}

RtSmokeBvhDirtyTokenState BuildSmokeStaticBucketWorkDirtyToken(
    const RtSmokeStaticBucketWorkDirtyTokenInput& input)
{
    RtSmokeBvhDirtyTokenState token;
    token.materialGeneration = input.materialGeneration;
    token.geometryContentSignature = 14695981039346656037ull;
    token.activeBlasInputSignature = 14695981039346656037ull;
    token.residentSetSignature = 14695981039346656037ull;
    token.activeSetSignature = 14695981039346656037ull;
    token.tlasInstanceSignature = 14695981039346656037ull;
    if (!input.plan)
    {
        return token;
    }

    const RtSmokeStaticBucketWorkPlan& plan = *input.plan;
    const int bucketCount = static_cast<int>(plan.bucketSignatures.size());
    int residentBucketCount = 0;
    int activeBucketCount = 0;
    for (const RtSmokeStaticBvhBucketSignature& bucketSignature : plan.bucketSignatures)
    {
        if (bucketSignature.resident)
        {
            ++residentBucketCount;
        }
        if (bucketSignature.active)
        {
            ++activeBucketCount;
        }
    }

    token.geometryContentSignature = HashSmokePlanBytes(token.geometryContentSignature, &bucketCount, sizeof(bucketCount));
    token.activeBlasInputSignature = HashSmokePlanBytes(token.activeBlasInputSignature, &activeBucketCount, sizeof(activeBucketCount));
    token.residentSetSignature = HashSmokePlanBytes(token.residentSetSignature, &residentBucketCount, sizeof(residentBucketCount));
    token.activeSetSignature = HashSmokePlanBytes(token.activeSetSignature, &activeBucketCount, sizeof(activeBucketCount));
    for (const RtSmokeStaticBvhBucketSignature& bucketSignature : plan.bucketSignatures)
    {
        if (bucketSignature.resident)
        {
            token.geometryContentSignature = HashSmokePlanBytes(token.geometryContentSignature, &bucketSignature.bucketKey, sizeof(bucketSignature.bucketKey));
            token.geometryContentSignature = HashSmokePlanBytes(token.geometryContentSignature, &bucketSignature.residentSignature, sizeof(bucketSignature.residentSignature));
            token.geometryContentSignature = HashSmokePlanBytes(token.geometryContentSignature, &bucketSignature.geometryInputSignature, sizeof(bucketSignature.geometryInputSignature));
            token.residentSetSignature = HashSmokePlanBytes(token.residentSetSignature, &bucketSignature.bucketKey, sizeof(bucketSignature.bucketKey));
            token.residentSetSignature = HashSmokePlanBytes(token.residentSetSignature, &bucketSignature.residentSignature, sizeof(bucketSignature.residentSignature));
        }
        if (bucketSignature.active)
        {
            token.activeBlasInputSignature = HashSmokePlanBytes(token.activeBlasInputSignature, &bucketSignature.bucketKey, sizeof(bucketSignature.bucketKey));
            token.activeBlasInputSignature = HashSmokePlanBytes(token.activeBlasInputSignature, &bucketSignature.blasInputSignature, sizeof(bucketSignature.blasInputSignature));
            token.activeSetSignature = HashSmokePlanBytes(token.activeSetSignature, &bucketSignature.bucketKey, sizeof(bucketSignature.bucketKey));
            token.activeSetSignature = HashSmokePlanBytes(token.activeSetSignature, &bucketSignature.activeSignature, sizeof(bucketSignature.activeSignature));
        }
    }

    const uint32_t staticRoutesEnabledBit = plan.routeNamespace.staticRoutesEnabled ? 1u : 0u;
    const uint32_t staticRoutesBlockedBit = plan.routeNamespace.staticRoutesBlocked ? 1u : 0u;
    const uint32_t rigidRouteBaseShiftedBit = plan.routeNamespace.rigidRouteBaseShifted ? 1u : 0u;
    token.tlasInstanceSignature = HashSmokePlanBytes(token.tlasInstanceSignature, &token.activeSetSignature, sizeof(token.activeSetSignature));
    token.tlasInstanceSignature = HashSmokePlanBytes(token.tlasInstanceSignature, &plan.activeSetPlan.tlasInstanceSignature, sizeof(plan.activeSetPlan.tlasInstanceSignature));
    token.tlasInstanceSignature = HashSmokePlanBytes(token.tlasInstanceSignature, &plan.routeTablePlan.tableSignature, sizeof(plan.routeTablePlan.tableSignature));
    token.tlasInstanceSignature = HashSmokePlanBytes(token.tlasInstanceSignature, &plan.routeTablePlan.emittedRecords, sizeof(plan.routeTablePlan.emittedRecords));
    token.tlasInstanceSignature = HashSmokePlanBytes(token.tlasInstanceSignature, &plan.routeNamespace.staticFirstInstanceId, sizeof(plan.routeNamespace.staticFirstInstanceId));
    token.tlasInstanceSignature = HashSmokePlanBytes(token.tlasInstanceSignature, &plan.routeNamespace.rigidFirstInstanceId, sizeof(plan.routeNamespace.rigidFirstInstanceId));
    token.tlasInstanceSignature = HashSmokePlanBytes(token.tlasInstanceSignature, &staticRoutesEnabledBit, sizeof(staticRoutesEnabledBit));
    token.tlasInstanceSignature = HashSmokePlanBytes(token.tlasInstanceSignature, &staticRoutesBlockedBit, sizeof(staticRoutesBlockedBit));
    token.tlasInstanceSignature = HashSmokePlanBytes(token.tlasInstanceSignature, &rigidRouteBaseShiftedBit, sizeof(rigidRouteBaseShiftedBit));
    return token;
}

RtSmokeBvhDirtyPlan BuildSmokeBvhDirtyPlan(
    const RtSmokeBvhDirtyPlanInput& input)
{
    RtSmokeBvhDirtyPlan plan;
    if (!input.previousValid)
    {
        plan.geometryContentChanged = true;
        plan.activeGeometryContentChanged = true;
        plan.residentSetChanged = true;
        plan.materialChanged = true;
        plan.activeMembershipChanged = true;
        plan.tlasInstanceChanged = true;
    }
    else
    {
        plan.geometryContentChanged =
            input.previous.geometryContentSignature != input.current.geometryContentSignature;
        const bool hasActiveGeometryToken =
            input.previous.activeBlasInputSignature != 0 ||
            input.current.activeBlasInputSignature != 0;
        plan.activeGeometryContentChanged = hasActiveGeometryToken
            ? input.previous.activeBlasInputSignature != input.current.activeBlasInputSignature &&
                plan.geometryContentChanged
            : plan.geometryContentChanged;
        plan.residentSetChanged =
            input.previous.residentSetSignature != input.current.residentSetSignature;
        plan.materialChanged =
            input.previous.materialGeneration != input.current.materialGeneration;
        plan.activeMembershipChanged =
            input.previous.activeSetSignature != input.current.activeSetSignature;
        plan.tlasInstanceChanged =
            input.previous.tlasInstanceSignature != input.current.tlasInstanceSignature;
    }

    plan.blasInputDirty = plan.activeGeometryContentChanged;
    plan.tlasDirty = plan.blasInputDirty || plan.activeMembershipChanged || plan.tlasInstanceChanged;
    return plan;
}

RtSmokeBvhFrameToken BuildSmokeBvhFrameToken(
    const RtSmokeBvhFrameTokenInput& input)
{
    RtSmokeBvhFrameToken token;

    uint64_t geometryContentSignature = 14695981039346656037ull;
    geometryContentSignature = HashSmokePlanBytes(geometryContentSignature, &input.staticBlasSignature, sizeof(input.staticBlasSignature));
    geometryContentSignature = HashSmokePlanBytes(geometryContentSignature, &input.geometryGeneration, sizeof(input.geometryGeneration));
    geometryContentSignature = HashSmokePlanBytes(geometryContentSignature, &input.dynamicVertexCount, sizeof(input.dynamicVertexCount));
    geometryContentSignature = HashSmokePlanBytes(geometryContentSignature, &input.dynamicIndexCount, sizeof(input.dynamicIndexCount));
    geometryContentSignature = HashSmokePlanBytes(geometryContentSignature, &input.rigidRouteVertexCount, sizeof(input.rigidRouteVertexCount));
    geometryContentSignature = HashSmokePlanBytes(geometryContentSignature, &input.rigidRouteIndexCount, sizeof(input.rigidRouteIndexCount));
    geometryContentSignature = HashSmokePlanBytes(geometryContentSignature, &input.rigidRouteTriangleCount, sizeof(input.rigidRouteTriangleCount));

    uint64_t activeSetSignature = input.staticActiveSetSignature;
    activeSetSignature = HashSmokePlanBytes(activeSetSignature, &input.rigidRouteInstanceCount, sizeof(input.rigidRouteInstanceCount));
    activeSetSignature = HashSmokePlanBytes(activeSetSignature, &input.rigidRouteSeenThisFrameCount, sizeof(input.rigidRouteSeenThisFrameCount));
    activeSetSignature = HashSmokePlanBytes(activeSetSignature, &input.rigidRouteCachedInstanceCount, sizeof(input.rigidRouteCachedInstanceCount));

    uint64_t tlasInstanceSignature = 14695981039346656037ull;
    const uint32_t hasStaticBlasBit = input.hasStaticBlas ? 1u : 0u;
    const uint32_t hasDynamicBlasBit = input.hasDynamicBlas ? 1u : 0u;
    tlasInstanceSignature = HashSmokePlanBytes(tlasInstanceSignature, &activeSetSignature, sizeof(activeSetSignature));
    tlasInstanceSignature = HashSmokePlanBytes(tlasInstanceSignature, &input.staticTlasInstanceSignature, sizeof(input.staticTlasInstanceSignature));
    tlasInstanceSignature = HashSmokePlanBytes(tlasInstanceSignature, &input.rigidTlasInstanceSignature, sizeof(input.rigidTlasInstanceSignature));
    tlasInstanceSignature = HashSmokePlanBytes(tlasInstanceSignature, &hasStaticBlasBit, sizeof(hasStaticBlasBit));
    tlasInstanceSignature = HashSmokePlanBytes(tlasInstanceSignature, &hasDynamicBlasBit, sizeof(hasDynamicBlasBit));
    tlasInstanceSignature = HashSmokePlanBytes(tlasInstanceSignature, &input.baseTlasInstanceCount, sizeof(input.baseTlasInstanceCount));
    tlasInstanceSignature = HashSmokePlanBytes(tlasInstanceSignature, &input.rigidTlasInstanceCount, sizeof(input.rigidTlasInstanceCount));

    token.dirtyToken.geometryContentSignature = geometryContentSignature;
    token.dirtyToken.activeBlasInputSignature = geometryContentSignature;
    token.dirtyToken.residentSetSignature = input.staticResidentSetSignature;
    token.dirtyToken.materialGeneration = input.materialGeneration;
    token.dirtyToken.activeSetSignature = activeSetSignature;
    token.dirtyToken.tlasInstanceSignature = tlasInstanceSignature;
    token.residentSetSignature = input.staticResidentSetSignature;
    return token;
}

RtSmokeBvhFramePlanningSnapshot CaptureSmokeBvhFramePlanningSnapshot(
    const RtSmokeBvhFramePlanningInput& input)
{
    RtSmokeBvhFramePlanningSnapshot snapshot;
    snapshot.staticBucketWorkSnapshot =
        CaptureSmokeStaticBucketWorkPlanSnapshot(input.staticBucketWorkInput);
    snapshot.frameTokenInput = input.frameTokenInput;
    snapshot.previousDirtyToken = input.previousDirtyToken;
    snapshot.previousDirtyTokenValid = input.previousDirtyTokenValid;
    return snapshot;
}

static uint64_t HashSmokeBvhFramePlanningFrameInput(
    uint64_t hash,
    const RtSmokeBvhFrameTokenInput& input)
{
    hash = HashSmokePlanBytes(hash, &input.staticBlasSignature, sizeof(input.staticBlasSignature));
    hash = HashSmokePlanBytes(hash, &input.geometryGeneration, sizeof(input.geometryGeneration));
    hash = HashSmokePlanBytes(hash, &input.materialGeneration, sizeof(input.materialGeneration));
    hash = HashSmokePlanBytes(hash, &input.dynamicVertexCount, sizeof(input.dynamicVertexCount));
    hash = HashSmokePlanBytes(hash, &input.dynamicIndexCount, sizeof(input.dynamicIndexCount));
    hash = HashSmokePlanBytes(hash, &input.rigidRouteVertexCount, sizeof(input.rigidRouteVertexCount));
    hash = HashSmokePlanBytes(hash, &input.rigidRouteIndexCount, sizeof(input.rigidRouteIndexCount));
    hash = HashSmokePlanBytes(hash, &input.rigidRouteTriangleCount, sizeof(input.rigidRouteTriangleCount));
    hash = HashSmokePlanBytes(hash, &input.rigidRouteInstanceCount, sizeof(input.rigidRouteInstanceCount));
    hash = HashSmokePlanBytes(hash, &input.rigidRouteSeenThisFrameCount, sizeof(input.rigidRouteSeenThisFrameCount));
    hash = HashSmokePlanBytes(hash, &input.rigidRouteCachedInstanceCount, sizeof(input.rigidRouteCachedInstanceCount));
    hash = HashSmokePlanBytes(hash, &input.rigidTlasInstanceSignature, sizeof(input.rigidTlasInstanceSignature));
    hash = HashSmokePlanBytes(hash, &input.baseTlasInstanceCount, sizeof(input.baseTlasInstanceCount));
    hash = HashSmokePlanBytes(hash, &input.rigidTlasInstanceCount, sizeof(input.rigidTlasInstanceCount));
    const uint32_t flags =
        (input.hasStaticBlas ? 1u : 0u) |
        (input.hasDynamicBlas ? 2u : 0u);
    hash = HashSmokePlanBytes(hash, &flags, sizeof(flags));
    return hash;
}

static uint64_t HashSmokeBvhDirtyTokenState(
    uint64_t hash,
    const RtSmokeBvhDirtyTokenState& token)
{
    hash = HashSmokePlanBytes(hash, &token.geometryContentSignature, sizeof(token.geometryContentSignature));
    hash = HashSmokePlanBytes(hash, &token.activeBlasInputSignature, sizeof(token.activeBlasInputSignature));
    hash = HashSmokePlanBytes(hash, &token.residentSetSignature, sizeof(token.residentSetSignature));
    hash = HashSmokePlanBytes(hash, &token.materialGeneration, sizeof(token.materialGeneration));
    hash = HashSmokePlanBytes(hash, &token.activeSetSignature, sizeof(token.activeSetSignature));
    hash = HashSmokePlanBytes(hash, &token.tlasInstanceSignature, sizeof(token.tlasInstanceSignature));
    return hash;
}

uint64_t BuildSmokeBvhFramePlanningInputToken(
    const RtSmokeBvhFramePlanningInput& input)
{
    uint64_t hash = 1469598103934665603ull;
    const uint64_t staticBucketToken =
        BuildSmokeStaticBucketWorkPlanInputToken(input.staticBucketWorkInput);
    hash = HashSmokePlanBytes(hash, &staticBucketToken, sizeof(staticBucketToken));
    hash = HashSmokeBvhFramePlanningFrameInput(hash, input.frameTokenInput);
    hash = HashSmokePlanBytes(hash, &input.previousDirtyTokenValid, sizeof(input.previousDirtyTokenValid));
    if (input.previousDirtyTokenValid)
    {
        hash = HashSmokeBvhDirtyTokenState(hash, input.previousDirtyToken);
    }
    return hash;
}

uint64_t BuildSmokeBvhFramePlanningInputToken(
    const RtSmokeBvhFramePlanningSnapshot& snapshot)
{
    uint64_t hash = 1469598103934665603ull;
    const uint64_t staticBucketToken =
        BuildSmokeStaticBucketWorkPlanInputToken(snapshot.staticBucketWorkSnapshot);
    hash = HashSmokePlanBytes(hash, &staticBucketToken, sizeof(staticBucketToken));
    hash = HashSmokeBvhFramePlanningFrameInput(hash, snapshot.frameTokenInput);
    hash = HashSmokePlanBytes(hash, &snapshot.previousDirtyTokenValid, sizeof(snapshot.previousDirtyTokenValid));
    if (snapshot.previousDirtyTokenValid)
    {
        hash = HashSmokeBvhDirtyTokenState(hash, snapshot.previousDirtyToken);
    }
    return hash;
}

RtSmokeBvhFramePlanningResult BuildSmokeBvhFramePlanningResult(
    const RtSmokeBvhFramePlanningInput& input)
{
    RtSmokeBvhFramePlanningResult result;
    result.staticBucketWorkPlan = BuildSmokeStaticBucketWorkPlan(input.staticBucketWorkInput);

    RtSmokeBvhFrameTokenInput frameTokenInput = input.frameTokenInput;
    RtSmokeStaticBucketWorkDirtyTokenInput staticDirtyTokenInput;
    staticDirtyTokenInput.plan = &result.staticBucketWorkPlan;
    staticDirtyTokenInput.materialGeneration = input.staticBucketWorkInput.materialGeneration;
    const RtSmokeBvhDirtyTokenState staticDirtyToken =
        BuildSmokeStaticBucketWorkDirtyToken(staticDirtyTokenInput);
    frameTokenInput.staticActiveSetSignature =
        result.staticBucketWorkPlan.activeSetPlan.activeSetSignature;
    frameTokenInput.staticResidentSetSignature =
        result.staticBucketWorkPlan.activeSetPlan.residentSetSignature;
    frameTokenInput.staticTlasInstanceSignature =
        staticDirtyToken.tlasInstanceSignature;
    result.frameToken = BuildSmokeBvhFrameToken(frameTokenInput);

    RtSmokeBvhDirtyPlanInput dirtyInput;
    dirtyInput.previousValid = input.previousDirtyTokenValid;
    dirtyInput.previous = input.previousDirtyToken;
    dirtyInput.current = result.frameToken.dirtyToken;
    result.dirtyPlan = BuildSmokeBvhDirtyPlan(dirtyInput);
    return result;
}

RtSmokeBvhFramePlanningResult BuildSmokeBvhFramePlanningResult(
    const RtSmokeBvhFramePlanningSnapshot& snapshot)
{
    RtSmokeBvhFramePlanningInput input;
    input.staticBucketWorkInput.buckets = snapshot.staticBucketWorkSnapshot.buckets.empty()
        ? nullptr
        : snapshot.staticBucketWorkSnapshot.buckets.data();
    input.staticBucketWorkInput.bucketCount =
        static_cast<int>(snapshot.staticBucketWorkSnapshot.buckets.size());
    input.staticBucketWorkInput.previousBuckets = snapshot.staticBucketWorkSnapshot.previousBuckets.empty()
        ? nullptr
        : snapshot.staticBucketWorkSnapshot.previousBuckets.data();
    input.staticBucketWorkInput.previousBucketCount =
        static_cast<int>(snapshot.staticBucketWorkSnapshot.previousBuckets.size());
    input.staticBucketWorkInput.geometryContentSignature =
        snapshot.staticBucketWorkSnapshot.geometryContentSignature;
    input.staticBucketWorkInput.materialGeneration =
        snapshot.staticBucketWorkSnapshot.materialGeneration;
    input.staticBucketWorkInput.totalVertexCount =
        snapshot.staticBucketWorkSnapshot.totalVertexCount;
    input.staticBucketWorkInput.totalIndexCount =
        snapshot.staticBucketWorkSnapshot.totalIndexCount;
    input.staticBucketWorkInput.totalTriangleCount =
        snapshot.staticBucketWorkSnapshot.totalTriangleCount;
    input.staticBucketWorkInput.monolithicStaticBlas =
        snapshot.staticBucketWorkSnapshot.monolithicStaticBlas;
    input.staticBucketWorkInput.hasStaticBlas =
        snapshot.staticBucketWorkSnapshot.hasStaticBlas;
    input.staticBucketWorkInput.submitBuilds =
        snapshot.staticBucketWorkSnapshot.submitBuilds;
    input.staticBucketWorkInput.forceRebuild =
        snapshot.staticBucketWorkSnapshot.forceRebuild;
    input.staticBucketWorkInput.enableStaticRoutes =
        snapshot.staticBucketWorkSnapshot.enableStaticRoutes;
    input.staticBucketWorkInput.shaderSupportsStaticBucketRoutes =
        snapshot.staticBucketWorkSnapshot.shaderSupportsStaticBucketRoutes;
    input.staticBucketWorkInput.firstRouteInstanceId =
        snapshot.staticBucketWorkSnapshot.firstRouteInstanceId;
    input.staticBucketWorkInput.rigidRouteRecordCount =
        snapshot.staticBucketWorkSnapshot.rigidRouteRecordCount;
    input.staticBucketWorkInput.maxBucketRecords =
        snapshot.staticBucketWorkSnapshot.maxBucketRecords;
    input.staticBucketWorkInput.maxRouteRecords =
        snapshot.staticBucketWorkSnapshot.maxRouteRecords;
    input.staticBucketWorkInput.maxBuildRecords =
        snapshot.staticBucketWorkSnapshot.maxBuildRecords;
    input.frameTokenInput = snapshot.frameTokenInput;
    input.previousDirtyToken = snapshot.previousDirtyToken;
    input.previousDirtyTokenValid = snapshot.previousDirtyTokenValid;
    return BuildSmokeBvhFramePlanningResult(input);
}

RtSmokeBvhFramePlanningTimedResult BuildSmokeBvhFramePlanningTimedResult(
    const RtSmokeBvhFramePlanningSnapshot& snapshot)
{
    const auto start = std::chrono::steady_clock::now();
    RtSmokeBvhFramePlanningTimedResult timedResult;
    timedResult.result = BuildSmokeBvhFramePlanningResult(snapshot);
    const auto end = std::chrono::steady_clock::now();
    timedResult.planningTimeMicros = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
    return timedResult;
}

bool AppendSmokeRigidTlasPlanObservation(
    RtSmokeRigidTlasPlan& plan,
    const RtSmokeRigidTlasPlanDesc& desc,
    const RtSmokeRigidTlasObservation& observation)
{
    const int maxInstances = NormalizeSmokePlanRecordCap(desc.maxInstances);
    if (maxInstances > 0 && plan.emittedInstances >= maxInstances)
    {
        return false;
    }

    ++plan.visibleInstances;
    const RtSmokeRigidTlasObservationCategory category =
        ClassifyRigidTlasObservation(observation, desc.rigidSourceMask);
    if (category == RT_SMOKE_RIGID_TLAS_REJECT_NON_RIGID)
    {
        ++plan.rejectedNonRigid;
        return true;
    }

    ++plan.rigidInstances;
    if (category == RT_SMOKE_RIGID_TLAS_REJECT_MISSING_MESH)
    {
        ++plan.rejectedMissingMesh;
        return true;
    }
    if (category == RT_SMOKE_RIGID_TLAS_REJECT_STALE_MESH)
    {
        ++plan.rejectedStaleMesh;
        return true;
    }
    if (category == RT_SMOKE_RIGID_TLAS_REJECT_MISSING_BLAS)
    {
        ++plan.rejectedMissingBlas;
        return true;
    }

    RtSmokePlanTlasInstance instance;
    instance.kind = RT_SMOKE_PLAN_TLAS_RIGID_BLAS;
    instance.instanceId = desc.firstInstanceId + static_cast<uint32_t>(plan.emittedInstances);
    instance.instanceMask = desc.instanceMask;
    instance.meshHash = observation.meshHash;
    instance.sourceInstanceId = observation.instanceId;
    instance.materialId = observation.materialId;
    instance.routeRecordIndex = observation.routeRecordIndex;
    instance.canonicalBlasRecordIndex =
        observation.canonicalBlasRecordIndex;
    instance.canonicalMeshHash = observation.canonicalMeshHash;
    instance.sourceSeenThisFrame = observation.seenThisFrame;
    instance.hasPreviousTransform = observation.hasPreviousObjectToWorld;
    instance.transformContinuous =
        observation.hasPreviousObjectToWorld && observation.transformContinuous;
    std::memcpy(instance.transform, observation.objectToWorld, sizeof(instance.transform));
    if (observation.hasPreviousObjectToWorld)
    {
        std::memcpy(instance.previousTransform, observation.previousObjectToWorld, sizeof(instance.previousTransform));
    }
    else
    {
        std::memcpy(instance.previousTransform, observation.objectToWorld, sizeof(instance.previousTransform));
    }
    plan.instances.push_back(instance);
    ++plan.emittedInstances;
    return true;
}

RtSmokeRigidTlasPlanSnapshot CaptureSmokeRigidTlasPlanSnapshot(
    const RtSmokeRigidTlasPlanDesc& desc)
{
    RtSmokeRigidTlasPlanSnapshot snapshot;
    snapshot.rigidSourceMask = desc.rigidSourceMask;
    snapshot.firstInstanceId = desc.firstInstanceId;
    snapshot.instanceMask = desc.instanceMask;
    snapshot.maxInstances = desc.maxInstances;
    if (desc.observations && desc.observationCount > 0)
    {
        const int maxInstances = NormalizeSmokePlanRecordCap(desc.maxInstances);
        if (maxInstances <= 0)
        {
            snapshot.observations.assign(desc.observations, desc.observations + desc.observationCount);
        }
        else
        {
            int emittedInstances = 0;
            for (int observationIndex = 0; observationIndex < desc.observationCount; ++observationIndex)
            {
                const RtSmokeRigidTlasObservation& observation = desc.observations[observationIndex];
                snapshot.observations.push_back(observation);
                if (ClassifyRigidTlasObservation(observation, desc.rigidSourceMask) ==
                    RT_SMOKE_RIGID_TLAS_ACCEPTED)
                {
                    ++emittedInstances;
                    if (emittedInstances >= maxInstances)
                    {
                        break;
                    }
                }
            }
        }
    }
    return snapshot;
}

uint64_t BuildSmokeRigidTlasPlanInputToken(
    const RtSmokeRigidTlasPlanDesc& desc)
{
    uint64_t hash = 1469598103934665603ull;
    hash = HashSmokePlanBytes(hash, &desc.rigidSourceMask, sizeof(desc.rigidSourceMask));
    hash = HashSmokePlanBytes(hash, &desc.firstInstanceId, sizeof(desc.firstInstanceId));
    hash = HashSmokePlanBytes(hash, &desc.instanceMask, sizeof(desc.instanceMask));
    const int maxInstances = NormalizeSmokePlanRecordCap(desc.maxInstances);
    hash = HashSmokePlanBytes(hash, &maxInstances, sizeof(maxInstances));
    if (!desc.observations || desc.observationCount <= 0)
    {
        return hash;
    }

    int emittedInstances = 0;
    int processedObservations = 0;
    for (int observationIndex = 0; observationIndex < desc.observationCount; ++observationIndex)
    {
        if (maxInstances > 0 && emittedInstances >= maxInstances)
        {
            break;
        }

        const RtSmokeRigidTlasObservation& observation = desc.observations[observationIndex];
        ++processedObservations;
        const RtSmokeRigidTlasObservationCategory category =
            ClassifyRigidTlasObservation(observation, desc.rigidSourceMask);
        hash = HashSmokePlanBytes(hash, &category, sizeof(category));
        if (category != RT_SMOKE_RIGID_TLAS_ACCEPTED)
        {
            continue;
        }

        hash = HashSmokePlanBytes(hash, &observation.meshHash, sizeof(observation.meshHash));
        hash = HashSmokePlanBytes(hash, &observation.instanceId, sizeof(observation.instanceId));
        hash = HashSmokePlanBytes(hash, &observation.materialId, sizeof(observation.materialId));
        hash = HashSmokePlanBytes(hash, &observation.routeRecordIndex, sizeof(observation.routeRecordIndex));
        hash = HashSmokePlanBytes(hash, &observation.canonicalBlasRecordIndex, sizeof(observation.canonicalBlasRecordIndex));
        hash = HashSmokePlanBytes(hash, &observation.canonicalMeshHash, sizeof(observation.canonicalMeshHash));
        const uint32_t instanceFlags =
            (observation.seenThisFrame ? 1u : 0u) |
            (observation.hasPreviousObjectToWorld ? 2u : 0u) |
            (observation.hasPreviousObjectToWorld &&
                    observation.transformContinuous
                ? 4u
                : 0u);
        hash = HashSmokePlanBytes(hash, &instanceFlags, sizeof(instanceFlags));
        hash = HashSmokePlanBytes(hash, observation.objectToWorld, sizeof(observation.objectToWorld));
        if (observation.hasPreviousObjectToWorld)
        {
            hash = HashSmokePlanBytes(hash, observation.previousObjectToWorld, sizeof(observation.previousObjectToWorld));
        }
        ++emittedInstances;
    }
    hash = HashSmokePlanBytes(hash, &processedObservations, sizeof(processedObservations));
    hash = HashSmokePlanBytes(hash, &emittedInstances, sizeof(emittedInstances));
    return hash;
}

uint64_t BuildSmokeRigidTlasPlanInputToken(
    const RtSmokeRigidTlasPlanSnapshot& snapshot)
{
    const RtSmokeRigidTlasPlanDesc desc = MakeRigidTlasPlanDescFromSnapshot(snapshot);
    return BuildSmokeRigidTlasPlanInputToken(desc);
}

static uint64_t BuildSmokeRigidTlasInstanceSignature(
    const RtSmokeRigidTlasPlan& plan,
    const RtSmokeRigidTlasPlanDesc& desc)
{
    uint64_t hash = 14695981039346656037ull;
    hash = HashSmokePlanBytes(hash, &desc.firstInstanceId, sizeof(desc.firstInstanceId));
    hash = HashSmokePlanBytes(hash, &desc.instanceMask, sizeof(desc.instanceMask));
    hash = HashSmokePlanBytes(hash, &desc.rigidSourceMask, sizeof(desc.rigidSourceMask));
    const int maxInstances = NormalizeSmokePlanRecordCap(desc.maxInstances);
    hash = HashSmokePlanBytes(hash, &maxInstances, sizeof(maxInstances));
    hash = HashSmokePlanBytes(hash, &plan.visibleInstances, sizeof(plan.visibleInstances));
    hash = HashSmokePlanBytes(hash, &plan.rigidInstances, sizeof(plan.rigidInstances));
    hash = HashSmokePlanBytes(hash, &plan.emittedInstances, sizeof(plan.emittedInstances));
    hash = HashSmokePlanBytes(hash, &plan.rejectedNonRigid, sizeof(plan.rejectedNonRigid));
    hash = HashSmokePlanBytes(hash, &plan.rejectedMissingMesh, sizeof(plan.rejectedMissingMesh));
    hash = HashSmokePlanBytes(hash, &plan.rejectedStaleMesh, sizeof(plan.rejectedStaleMesh));
    hash = HashSmokePlanBytes(hash, &plan.rejectedMissingBlas, sizeof(plan.rejectedMissingBlas));
    for (const RtSmokePlanTlasInstance& instance : plan.instances)
    {
        const uint32_t instanceFlags =
            (instance.sourceSeenThisFrame ? 1u : 0u) |
            (instance.hasPreviousTransform ? 2u : 0u) |
            (instance.transformContinuous ? 4u : 0u);
        hash = HashSmokePlanBytes(hash, &instance.kind, sizeof(instance.kind));
        hash = HashSmokePlanBytes(hash, &instance.instanceId, sizeof(instance.instanceId));
        hash = HashSmokePlanBytes(hash, &instance.instanceMask, sizeof(instance.instanceMask));
        hash = HashSmokePlanBytes(hash, &instance.meshHash, sizeof(instance.meshHash));
        hash = HashSmokePlanBytes(hash, &instance.sourceInstanceId, sizeof(instance.sourceInstanceId));
        hash = HashSmokePlanBytes(hash, &instance.materialId, sizeof(instance.materialId));
        hash = HashSmokePlanBytes(hash, &instance.routeRecordIndex, sizeof(instance.routeRecordIndex));
        hash = HashSmokePlanBytes(hash, &instance.canonicalBlasRecordIndex, sizeof(instance.canonicalBlasRecordIndex));
        hash = HashSmokePlanBytes(hash, &instance.canonicalMeshHash, sizeof(instance.canonicalMeshHash));
        hash = HashSmokePlanBytes(hash, &instanceFlags, sizeof(instanceFlags));
        hash = HashSmokePlanBytes(hash, instance.transform, sizeof(instance.transform));
        hash = HashSmokePlanBytes(hash, instance.previousTransform, sizeof(instance.previousTransform));
    }
    return hash;
}

void UpdateSmokeRigidTlasPlanInstanceSignature(
    RtSmokeRigidTlasPlan& plan,
    const RtSmokeRigidTlasPlanSnapshot& snapshot)
{
    const RtSmokeRigidTlasPlanDesc desc = MakeRigidTlasPlanDescFromSnapshot(snapshot);
    plan.tlasInstanceSignature = BuildSmokeRigidTlasInstanceSignature(plan, desc);
}

RtSmokeRigidTlasPlan BuildSmokeRigidTlasPlan(const RtSmokeRigidTlasPlanDesc& desc)
{
    RtSmokeRigidTlasPlan plan;
    if (!desc.observations || desc.observationCount <= 0)
    {
        plan.tlasInstanceSignature = BuildSmokeRigidTlasInstanceSignature(plan, desc);
        return plan;
    }

    const int maxInstances = NormalizeSmokePlanRecordCap(desc.maxInstances);
    const int reserveCount = maxInstances > 0 && maxInstances < desc.observationCount
        ? maxInstances
        : desc.observationCount;
    plan.instances.reserve(reserveCount);
    for (int observationIndex = 0; observationIndex < desc.observationCount; ++observationIndex)
    {
        if (!AppendSmokeRigidTlasPlanObservation(plan, desc, desc.observations[observationIndex]))
        {
            break;
        }
    }

    plan.tlasInstanceSignature = BuildSmokeRigidTlasInstanceSignature(plan, desc);
    return plan;
}

RtSmokeRigidTlasPlan BuildSmokeRigidTlasPlan(
    const RtSmokeRigidTlasPlanSnapshot& snapshot)
{
    return BuildSmokeRigidTlasPlan(MakeRigidTlasPlanDescFromSnapshot(snapshot));
}

RtSmokeCanonicalRigidTlasSelection
BuildSmokeCanonicalRigidTlasSelection(
    const RtSmokeCanonicalRigidTlasSelectionInput& input)
{
    RtSmokeCanonicalRigidTlasSelection selection;
    selection.exactParity =
        input.providerEnabled &&
        input.legacyDescriptors >= 0 &&
        input.canonicalDescriptors == input.legacyDescriptors &&
        input.exactRecordMappings == input.legacyDescriptors &&
        input.missingRecordIndex == 0 &&
        input.meshHashMismatch == 0 &&
        input.missingBlas == 0;
    selection.selectCanonical =
        input.traversalRequested && selection.exactParity;
    return selection;
}

RtSmokeRigidTlasPlanTimedResult BuildSmokeRigidTlasPlanTimedResult(
    const RtSmokeRigidTlasPlanSnapshot& snapshot)
{
    const auto start = std::chrono::steady_clock::now();
    RtSmokeRigidTlasPlanTimedResult result;
    result.plan = BuildSmokeRigidTlasPlan(snapshot);
    const auto end = std::chrono::steady_clock::now();
    result.planningTimeMicros = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
    return result;
}

RtSmokeRigidBlasBuildPlan BuildSmokeRigidBlasBuildPlan(
    const RtSmokeRigidBlasBuildPlanInput& input)
{
    RtSmokeRigidBlasBuildPlan plan;
    if (!input.submitBuilds)
    {
        plan.skipBuild = true;
        return plan;
    }

    plan.createBlas = !input.hasBlas || !input.blasInputsCompatible;
    plan.submitBuild =
        input.forceRebuild ||
        input.uploadRequired ||
        plan.createBlas;
    plan.skipBuild = !plan.submitBuild;
    return plan;
}

RtSmokeRigidBvhObjectSignature BuildSmokeRigidBvhObjectSignature(
    const RtSmokeRigidBvhObjectSignatureInput& input)
{
    RtSmokeRigidBvhObjectSignature signature;
    signature.resident = input.hasMeshRecord && (input.meshSeenThisFrame || input.residencyEnabled);
    signature.activeCandidate = signature.resident && ((input.sourceFlags & input.rigidSourceMask) != 0);

    uint64_t objectHash = 14695981039346656037ull;
    objectHash = HashSmokePlanBytes(objectHash, &input.meshHash, sizeof(input.meshHash));
    objectHash = HashSmokePlanBytes(objectHash, &input.instanceId, sizeof(input.instanceId));
    signature.objectKey = objectHash;

    uint64_t geometryHash = 14695981039346656037ull;
    geometryHash = HashSmokePlanBytes(geometryHash, &input.meshHash, sizeof(input.meshHash));
    geometryHash = HashSmokePlanBytes(geometryHash, &input.geometryContentSignature, sizeof(input.geometryContentSignature));
    geometryHash = HashSmokePlanBytes(geometryHash, &input.vertexCount, sizeof(input.vertexCount));
    geometryHash = HashSmokePlanBytes(geometryHash, &input.indexCount, sizeof(input.indexCount));
    signature.geometryInputSignature = geometryHash;

    signature.blasInputSignature = geometryHash;

    uint64_t tlasHash = 14695981039346656037ull;
    const uint32_t residentBit = signature.resident ? 1u : 0u;
    const uint32_t activeBit = signature.activeCandidate ? 1u : 0u;
    tlasHash = HashSmokePlanBytes(tlasHash, &signature.objectKey, sizeof(signature.objectKey));
    if (signature.activeCandidate)
    {
        tlasHash = HashSmokePlanBytes(tlasHash, &input.routeRecordIndex, sizeof(input.routeRecordIndex));
    }
    tlasHash = HashSmokePlanBytes(tlasHash, &residentBit, sizeof(residentBit));
    tlasHash = HashSmokePlanBytes(tlasHash, &activeBit, sizeof(activeBit));
    signature.tlasMembershipSignature = tlasHash;
    return signature;
}

RtSmokeUploadPlanMetadata BuildSmokeVectorUploadPlanMetadata(
    size_t elementCount,
    size_t elementSize,
    bool skip,
    int dirtyElementOffset,
    int dirtyElementCount)
{
    RtSmokeUploadPlanMetadata metadata;
    metadata.skip = skip;
    if (metadata.skip)
    {
        return metadata;
    }

    size_t uploadElementOffset = 0;
    size_t uploadElementCount = elementCount;
    if (PlanElementRangeValid(dirtyElementOffset, dirtyElementCount, elementCount))
    {
        uploadElementOffset = static_cast<size_t>(dirtyElementOffset);
        uploadElementCount = static_cast<size_t>(dirtyElementCount);
    }

    metadata.byteSize = uploadElementCount * elementSize;
    metadata.sourceOffsetBytes = uploadElementOffset * elementSize;
    metadata.destOffsetBytes = static_cast<uint64_t>(metadata.sourceOffsetBytes);
    return metadata;
}

RtSmokeStaticDirtyUploadPlan BuildSmokeStaticDirtyUploadPlan(
    const RtSmokeStaticDirtyUploadPlanInput& input)
{
    RtSmokeStaticDirtyUploadPlan plan;
    plan.dirtyRangesValid =
        PlanElementRangeValid(input.dirtyVertexOffset, input.dirtyVertexCount, input.totalVertexCount) &&
        PlanElementRangeValid(input.dirtyIndexOffset, input.dirtyIndexCount, input.totalIndexCount) &&
        PlanElementRangeValid(input.dirtyTriangleOffset, input.dirtyTriangleCount, input.totalTriangleClassCount) &&
        PlanElementRangeValid(input.dirtyTriangleOffset, input.dirtyTriangleCount, input.totalTriangleMaterialCount);
    plan.useDirtyRangeUploads =
        !input.staticBlasCacheHit &&
        input.staticCacheChanged &&
        input.staticDirtyCount > 0 &&
        input.staticGeometryBuffersReused &&
        plan.dirtyRangesValid;
    return plan;
}

RtSmokeStaticVertexUploadPlan BuildSmokeStaticVertexUploadPlan(
    const RtSmokeStaticVertexUploadPlanInput& input)
{
    RtSmokeStaticVertexUploadPlan plan;
    if (input.forceRebuildWithoutUpload)
    {
        plan.skipUpload = true;
        return plan;
    }

    const int texMatrixRangeCount =
        input.texMatrixLastVertex >= input.texMatrixFirstVertex
            ? input.texMatrixLastVertex - input.texMatrixFirstVertex + 1
            : 0;
    const bool texMatrixRangeValid =
        input.texMatrixVertexCount > 0 &&
        PlanElementRangeValid(
            input.texMatrixFirstVertex,
            texMatrixRangeCount,
            input.totalVertexCount);

    if (input.staticBlasCacheHit)
    {
        if (input.texMatrixVertexCount <= 0)
        {
            plan.skipUpload = true;
            return plan;
        }
        if (texMatrixRangeValid)
        {
            plan.texMatrixRangeUpload = true;
            plan.elementOffset = input.texMatrixFirstVertex;
            plan.elementCount = texMatrixRangeCount;
            return plan;
        }

        // A malformed runtime texture-matrix range must not silently suppress
        // the base geometry upload. Positions remain unchanged, so the cached
        // BLAS is still valid while the complete shader vertex payload is
        // republished.
        plan.fullUpload = true;
        return plan;
    }

    // Under the GEO-02 fix gate, a cache miss can coincide with a
    // resized/recreated GPU buffer. Uploading only the texture-matrix span
    // would leave all other vertices uninitialized. A full upload also covers
    // a dirty geometry range plus disjoint runtime UV updates without trying
    // to represent them as one partial interval.
    if (input.texMatrixVertexCount > 0 &&
        input.fullUploadOnCacheMissWithTexMatrices)
    {
        plan.fullUpload = true;
        return plan;
    }
    if (texMatrixRangeValid)
    {
        plan.texMatrixRangeUpload = true;
        plan.elementOffset = input.texMatrixFirstVertex;
        plan.elementCount = texMatrixRangeCount;
        return plan;
    }

    if (input.useDirtyRangeUploads &&
        PlanElementRangeValid(
            input.dirtyVertexOffset,
            input.dirtyVertexCount,
            input.totalVertexCount))
    {
        plan.dirtyRangeUpload = true;
        plan.elementOffset = input.dirtyVertexOffset;
        plan.elementCount = input.dirtyVertexCount;
        return plan;
    }

    plan.fullUpload = true;
    return plan;
}

uint64_t BuildSmokePlanDataSpanSignature(
    const RtSmokePlanDataSpan* spans,
    int spanCount)
{
    uint64_t hash = 14695981039346656037ull;
    if (!spans || spanCount <= 0)
    {
        return hash;
    }

    for (int spanIndex = 0; spanIndex < spanCount; ++spanIndex)
    {
        const RtSmokePlanDataSpan& span = spans[spanIndex];
        const uint64_t count = static_cast<uint64_t>(span.elementCount);
        const uint64_t elementSize = static_cast<uint64_t>(span.elementSize);
        const uint32_t payloadAvailable =
            (span.data && span.elementCount > 0 && span.elementSize > 0) ? 1u : 0u;
        hash = HashSmokePlanBytes(hash, &elementSize, sizeof(elementSize));
        hash = HashSmokePlanBytes(hash, &count, sizeof(count));
        hash = HashSmokePlanBytes(hash, &payloadAvailable, sizeof(payloadAvailable));
        if (payloadAvailable)
        {
            hash = HashSmokePlanBytes(hash, span.data, span.elementCount * span.elementSize);
        }
    }
    return hash;
}

RtSmokePreviousStaticSnapshotUploadPlan BuildSmokePreviousStaticSnapshotUploadPlan(
    const RtSmokePreviousStaticSnapshotUploadPlanInput& input)
{
    RtSmokePreviousStaticSnapshotUploadPlan plan;
    plan.skipUpload =
        input.dataAvailable &&
        input.buffersReused &&
        input.previousUploadSignature != 0 &&
        input.previousUploadSignature == input.currentUploadSignature;
    return plan;
}
