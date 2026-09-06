#include "precompiled.h"
#pragma hdrstop

#include "PathTraceGeometryUniverse.h"
#include "PathTraceCpuProducerPublish.h"
#include "PathTraceInstanceUniverse.h"
#include "PathTraceRigidCandidateSubsystem.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <new>
#include <stdexcept>
#include <utility>

namespace
{
bool RtCheckedAddSize(size_t left, size_t right, size_t& result) noexcept
{
    if (right > SIZE_MAX - left) return false;
    result = left + right;
    return true;
}

bool RtCheckedReplaceSize(size_t value, size_t removed, size_t added,
    size_t& result) noexcept
{
    if (removed > value) return false;
    return RtCheckedAddSize(value - removed, added, result);
}

size_t RtRigidCandidateRequiredBucketCount(size_t elementCount,
    float maxLoadFactor) noexcept
{
    if (!(maxLoadFactor > 0.0f) || !std::isfinite(maxLoadFactor))
        return SIZE_MAX;
    const double required = std::ceil(
        static_cast<double>(elementCount) /
        static_cast<double>(maxLoadFactor));
    return required > static_cast<double>(SIZE_MAX)
        ? SIZE_MAX : static_cast<size_t>(required);
}

size_t RtRigidCandidateConservativeBucketTarget(size_t currentBucketCount,
    size_t requiredElementCount, float maxLoadFactor) noexcept
{
    const size_t minimumBuckets = RtRigidCandidateRequiredBucketCount(
        requiredElementCount, maxLoadFactor);
    if (minimumBuckets == SIZE_MAX) return SIZE_MAX;
    if (minimumBuckets <= currentBucketCount) return currentBucketCount;
#if defined(_MSVC_STL_VERSION)
    // MSVC's unordered containers use power-of-two bucket arrays and may grow
    // by more than the minimum requested count. Eight times the old array,
    // rounded up to a power of two, conservatively covers that replacement.
    size_t target = minimumBuckets;
    if (currentBucketCount != 0)
    {
        if (currentBucketCount > SIZE_MAX / 8u) return SIZE_MAX;
        target = std::max(target, currentBucketCount * 8u);
    }
    size_t rounded = 1;
    while (rounded < target)
    {
        if (rounded > SIZE_MAX / 2u) return SIZE_MAX;
        rounded *= 2u;
    }
    return rounded;
#else
    // The standard specifies a lower bound, not an upper bound, for reserve's
    // replacement bucket count. Refuse capped live rehash without a defensible
    // implementation-specific upper bound.
    return SIZE_MAX;
#endif
}

bool RtRigidPreparedObservationMatches(
    const RtPathTraceRigidPreparedPayload& payload,
    const RtPathTraceRigidMeshCandidateObservation& observation) noexcept
{
    return payload.meshHash == observation.meshHash &&
        payload.vertexBufferIdentity == observation.vertexBufferIdentity &&
        payload.indexBufferIdentity == observation.indexBufferIdentity &&
        payload.modelEpoch == observation.modelEpoch &&
        observation.numVerts >= 0 &&
        payload.fullTriangleVertexCount ==
            static_cast<std::uint32_t>(observation.numVerts) &&
        observation.numIndexes >= 0 &&
        payload.fullTriangleIndexCount ==
            static_cast<std::uint32_t>(observation.numIndexes) &&
        payload.vertexFormat == observation.vertexFormat &&
        payload.materialId == observation.materialId &&
        payload.materialClassSignature ==
            observation.materialClassSignature &&
        std::memcmp(payload.normalTexMatrix, observation.normalTexMatrix,
            sizeof(payload.normalTexMatrix)) == 0;
}

bool RtRigidDeferredObservationsMatch(
    const RtPathTraceRigidMeshCandidateObservation& left,
    const RtPathTraceRigidMeshCandidateObservation& right) noexcept
{
    return left.tri == right.tri &&
        left.meshHash == right.meshHash &&
        left.instanceId == right.instanceId &&
        left.vertexBufferIdentity == right.vertexBufferIdentity &&
        left.indexBufferIdentity == right.indexBufferIdentity &&
        left.sourceFlags == right.sourceFlags &&
        left.modelEpoch == right.modelEpoch &&
        left.numVerts == right.numVerts &&
        left.numIndexes == right.numIndexes &&
        left.vertexFormat == right.vertexFormat &&
        left.materialId == right.materialId &&
        left.materialClassSignature == right.materialClassSignature &&
        left.surfaceClassId == right.surfaceClassId &&
        left.triangleClassAndFlags == right.triangleClassAndFlags &&
        left.drawSurfIndex == right.drawSurfIndex &&
        left.entityIndex == right.entityIndex &&
        left.renderEntityNum == right.renderEntityNum &&
        left.modelSurfaceIndex == right.modelSurfaceIndex &&
        left.jointIndex == right.jointIndex &&
        left.localSpaceValid == right.localSpaceValid &&
        std::memcmp(left.normalTexMatrix, right.normalTexMatrix,
            sizeof(left.normalTexMatrix)) == 0 &&
        idStr::Cmp(left.materialName.c_str(), right.materialName.c_str()) == 0 &&
        idStr::Cmp(left.modelName.c_str(), right.modelName.c_str()) == 0;
}
}

bool RtPathTraceRigidPreparedPayloadsCompatible(
    const std::vector<RtPathTraceRigidPreparedPayload>& payloads,
    const std::vector<RtPathTraceRigidMeshCandidateObservation>& observations) noexcept
{
    if (payloads.empty()) return observations.empty();
    if (observations.empty()) return false;
    size_t covered = 0;
    for (size_t payloadIndex = 0; payloadIndex < payloads.size(); ++payloadIndex)
    {
        const RtPathTraceRigidPreparedPayload& payload = payloads[payloadIndex];
        if (payload.meshHash == 0 || payload.occurrenceCount == 0)
            return false;
        for (size_t prior = 0; prior < payloadIndex; ++prior)
            if (payloads[prior].meshHash == payload.meshHash) return false;
        size_t occurrences = 0;
        for (const RtPathTraceRigidMeshCandidateObservation& observation :
             observations)
        {
            if (observation.meshHash != payload.meshHash) continue;
            if (!RtRigidPreparedObservationMatches(payload, observation))
                return false;
            ++occurrences;
        }
        if (occurrences != payload.occurrenceCount ||
            occurrences > observations.size() - covered)
            return false;
        covered += occurrences;
    }
    return covered == observations.size();
}

size_t ReplayPathTraceDeferredRigidCandidates(
    RtSmokeGeometryUniverse& geometryUniverse,
    bool deferralActive,
    bool compactApplied,
    const std::vector<RtPathTraceRigidMeshCandidateObservation>& observations,
    size_t observationBegin,
    size_t alreadyReplayedCount)
{
    if (!deferralActive || compactApplied || observations.empty() ||
        observationBegin > observations.size() ||
        alreadyReplayedCount > observationBegin)
        return 0;
    size_t replayed = 0;
    for (size_t observationIndex = observationBegin;
         observationIndex < observations.size(); ++observationIndex)
    {
        const RtPathTraceRigidMeshCandidateObservation& observation =
            observations[observationIndex];
        size_t priorMatchingOccurrences = 0;
        for (size_t prior = observationBegin; prior < observationIndex; ++prior)
            priorMatchingOccurrences += RtRigidDeferredObservationsMatch(
                observations[prior], observation) ? 1u : 0u;
        size_t alreadyReplayedOccurrences = 0;
        for (size_t prior = 0; prior < alreadyReplayedCount; ++prior)
            alreadyReplayedOccurrences += RtRigidDeferredObservationsMatch(
                observations[prior], observation) ? 1u : 0u;
        if (priorMatchingOccurrences < alreadyReplayedOccurrences) continue;
        geometryUniverse.RecordRigidMeshCandidate(observation);
        ++replayed;
    }
    return replayed;
}

uint32_t RtPathTraceBuildRigidMeshCandidateRejectFlags(const RtPathTraceRigidMeshCandidateObservation& observation) noexcept
{
    uint32_t rejectFlags = 0;
    const RtPathTraceResidencyClass residencyClass = RtPathTraceResidencyClassForSourceFlags(observation.sourceFlags);
    if (residencyClass != RtPathTraceResidencyClass::DurableRigid &&
        (observation.sourceFlags & RT_PT_INSTANCE_SOURCE_RIGID) == 0)
    {
        rejectFlags |= RT_PT_RIGID_MESH_REJECT_NOT_RIGID;
    }
    if (observation.numVerts <= 0 || observation.numIndexes <= 0 || (observation.numIndexes % 3) != 0 || observation.meshHash == 0)
    {
        rejectFlags |= RT_PT_RIGID_MESH_REJECT_INVALID_GEOMETRY;
    }
    if (observation.tri == nullptr)
    {
        rejectFlags |= RT_PT_RIGID_MESH_REJECT_INVALID_GEOMETRY;
    }
    if (observation.materialId == 0 || observation.materialName.IsEmpty() || observation.materialName.Icmp("<none>") == 0)
    {
        rejectFlags |= RT_PT_RIGID_MESH_REJECT_MISSING_MATERIAL;
    }
    if (!observation.localSpaceValid)
    {
        rejectFlags |= RT_PT_RIGID_MESH_REJECT_NO_LOCAL_SPACE;
    }
    if ((observation.sourceFlags & RT_PT_INSTANCE_SOURCE_SKINNED_OR_DEFORMING) != 0)
    {
        rejectFlags |= RT_PT_RIGID_MESH_REJECT_SKINNED_OR_DEFORMING;
    }
    if ((observation.sourceFlags & RT_PT_INSTANCE_SOURCE_PARTICLE_OR_TRANSIENT) != 0)
    {
        rejectFlags |= RT_PT_RIGID_MESH_REJECT_PARTICLE_OR_TRANSIENT;
    }
    if ((observation.sourceFlags & RT_PT_INSTANCE_SOURCE_GUI) != 0)
    {
        rejectFlags |= RT_PT_RIGID_MESH_REJECT_GUI;
    }
    if ((observation.sourceFlags & RT_PT_INSTANCE_SOURCE_CALLBACK_OR_GENERATED) != 0)
    {
        rejectFlags |= RT_PT_RIGID_MESH_REJECT_CALLBACK_OR_GENERATED;
    }
    if ((observation.sourceFlags & RT_PT_INSTANCE_SOURCE_STATIC_WORLD) != 0)
    {
        rejectFlags |= RT_PT_RIGID_MESH_REJECT_STATIC_WORLD;
    }
    if ((observation.sourceFlags & (RT_PT_INSTANCE_SOURCE_STATIC_UNIVERSE_MATCH | RT_PT_INSTANCE_SOURCE_STATIC_CACHE_MATCH)) != 0)
    {
        rejectFlags |= RT_PT_RIGID_MESH_REJECT_STATIC_CACHE_MATCH;
    }
    return rejectFlags;
}

void RtPathTraceAccumulateRigidMeshCandidateRejectStats(RtPathTraceRigidMeshCandidateStats& stats, uint32_t rejectFlags) noexcept
{
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_NOT_RIGID) != 0)
    {
        ++stats.rejectNotRigid;
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_INVALID_GEOMETRY) != 0)
    {
        ++stats.rejectInvalidGeometry;
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_MISSING_MATERIAL) != 0)
    {
        ++stats.rejectMissingMaterial;
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_NO_LOCAL_SPACE) != 0)
    {
        ++stats.rejectNoLocalSpace;
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_SKINNED_OR_DEFORMING) != 0)
    {
        ++stats.rejectSkinnedOrDeforming;
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_PARTICLE_OR_TRANSIENT) != 0)
    {
        ++stats.rejectParticleOrTransient;
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_GUI) != 0)
    {
        ++stats.rejectGui;
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_CALLBACK_OR_GENERATED) != 0)
    {
        ++stats.rejectCallbackOrGenerated;
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_STATIC_WORLD) != 0)
    {
        ++stats.rejectStaticWorld;
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_STATIC_CACHE_MATCH) != 0)
    {
        ++stats.rejectStaticCacheMatch;
    }
}

void RtPathTraceAddRigidMeshCandidateSampleToStats(
    RtPathTraceRigidMeshCandidateStats& stats,
    const RtPathTraceRigidMeshCandidateObservation& observation,
    bool eligible,
    uint32_t rejectFlags,
    int seenCount)
{
    RtPathTraceRigidMeshCandidateSample* sample = nullptr;
    if (eligible)
    {
        if (stats.eligibleSampleCount >= RT_PT_RIGID_MESH_CANDIDATE_SAMPLES)
        {
            return;
        }
        sample = &stats.eligibleSamples[stats.eligibleSampleCount++];
    }
    else
    {
        if (stats.rejectedSampleCount >= RT_PT_RIGID_MESH_CANDIDATE_SAMPLES)
        {
            return;
        }
        sample = &stats.rejectedSamples[stats.rejectedSampleCount++];
    }
    sample->valid = true;
    sample->eligible = eligible;
    sample->meshHash = observation.meshHash;
    sample->instanceId = observation.instanceId;
    sample->triIdentity = reinterpret_cast<uintptr_t>(observation.tri);
    sample->vertexBufferIdentity = observation.vertexBufferIdentity;
    sample->indexBufferIdentity = observation.indexBufferIdentity;
    sample->rejectFlags = rejectFlags;
    sample->materialId = observation.materialId;
    sample->materialClassSignature = observation.materialClassSignature;
    sample->vertexFormat = observation.vertexFormat;
    sample->drawSurfIndex = observation.drawSurfIndex;
    sample->entityIndex = observation.entityIndex;
    sample->renderEntityNum = observation.renderEntityNum;
    sample->numVerts = observation.numVerts;
    sample->numIndexes = observation.numIndexes;
    sample->seenCount = seenCount;
    sample->materialName = observation.materialName;
    sample->modelName = observation.modelName;
}

size_t RtRigidCandidateBucketStorageBytes(size_t bucketCount) noexcept
{
#if defined(_MSVC_STL_VERSION)
    // MSVC xhash stores begin/end iterator slots for every logical bucket.
    constexpr size_t slotsPerBucket = 2u;
#else
    // Non-MSVC live rehash is refused above because the standard exposes no
    // replacement upper bound. Retain the existing one-slot retained model.
    constexpr size_t slotsPerBucket = 1u;
#endif
    if (bucketCount > SIZE_MAX / slotsPerBucket) return SIZE_MAX;
    const size_t slotCount = bucketCount * slotsPerBucket;
    return slotCount > SIZE_MAX / sizeof(void*)
        ? SIZE_MAX : slotCount * sizeof(void*);
}

size_t RtRigidCandidateAssociativeBytes(
    size_t size, size_t bucketCount, size_t valueBytes) noexcept
{
    const size_t nodeBytes = valueBytes + sizeof(void*) * 2u;
    const size_t bucketBytes =
        RtRigidCandidateBucketStorageBytes(bucketCount);
    if (bucketBytes == SIZE_MAX ||
        size > (SIZE_MAX - bucketBytes) / nodeBytes)
    {
        return SIZE_MAX;
    }
    return bucketBytes + size * nodeBytes;
}

size_t RtRigidCandidateRecordOwnedBytes(
    const RtSmokeGeometryUniverse::RigidMeshCandidateRecord& record) noexcept
{
    size_t bytes = record.cachedLocalVertices.capacity() * sizeof(PathTraceSmokeVertex) +
        record.cachedLocalIndexes.capacity() * sizeof(uint32_t);
    const int materialBytes = record.materialName.Allocated();
    const int modelBytes = record.modelName.Allocated();
    if (materialBytes > 0) bytes += static_cast<size_t>(materialBytes);
    if (modelBytes > 0) bytes += static_cast<size_t>(modelBytes);
    return bytes;
}

size_t RtRigidCandidateStatsOwnedBytes(
    const RtPathTraceRigidMeshCandidateStats& stats) noexcept
{
    size_t bytes = 0;
    const auto addSamples = [&bytes](const RtPathTraceRigidMeshCandidateSample* samples,
        int count)
    {
        for (int index = 0; index < count; ++index)
        {
            const int materialBytes = samples[index].materialName.Allocated();
            const int modelBytes = samples[index].modelName.Allocated();
            if (materialBytes > 0) bytes += static_cast<size_t>(materialBytes);
            if (modelBytes > 0) bytes += static_cast<size_t>(modelBytes);
        }
    };
    addSamples(stats.eligibleSamples, stats.eligibleSampleCount);
    addSamples(stats.rejectedSamples, stats.rejectedSampleCount);
    return bytes;
}

void RtCommitRigidCandidateExistingRecord(
    RtSmokeGeometryUniverse::RigidMeshCandidateRecord& live,
    RtSmokeGeometryUniverse::RigidMeshCandidateRecord& prepared) noexcept
{
    static_assert(std::is_nothrow_move_assignable<idStr>::value,
        "prepared rigid strings must publish without allocation");
    live.valid = prepared.valid;
    live.tri = prepared.tri;
    live.meshHash = prepared.meshHash;
    live.vertexBufferIdentity = prepared.vertexBufferIdentity;
    live.indexBufferIdentity = prepared.indexBufferIdentity;
    live.materialId = prepared.materialId;
    live.materialClassSignature = prepared.materialClassSignature;
    live.surfaceClassId = prepared.surfaceClassId;
    live.triangleClassAndFlags = prepared.triangleClassAndFlags;
    live.sourceFlags = prepared.sourceFlags;
    live.vertexFormat = prepared.vertexFormat;
    live.modelEpoch = prepared.modelEpoch;
    live.modelSurfaceIndex = prepared.modelSurfaceIndex;
    live.jointIndex = prepared.jointIndex;
    live.sourceRange = prepared.sourceRange;
    live.firstSeenFrame = prepared.firstSeenFrame;
    live.lastSeenFrame = prepared.lastSeenFrame;
    live.seenCount = prepared.seenCount;
    live.instanceCountThisFrame = prepared.instanceCountThisFrame;
    live.seenThisFrame = prepared.seenThisFrame;
    live.newlyCreatedThisFrame = prepared.newlyCreatedThisFrame;
    std::memcpy(live.normalTexMatrix, prepared.normalTexMatrix,
        sizeof(live.normalTexMatrix));
    live.localBounds = prepared.localBounds;
    live.localBoundsValid = prepared.localBoundsValid;
    live.cachedLocalVertices.swap(prepared.cachedLocalVertices);
    live.cachedLocalIndexes.swap(prepared.cachedLocalIndexes);
    live.cpuMeshContentSignature = prepared.cpuMeshContentSignature;
    live.cachedRouteDataValid = prepared.cachedRouteDataValid;
    live.cpuCacheRefreshAttempts = prepared.cpuCacheRefreshAttempts;
    live.cpuCacheRefreshSuccesses = prepared.cpuCacheRefreshSuccesses;
    live.cpuCacheRefreshFailures = prepared.cpuCacheRefreshFailures;
    live.lastCpuCacheDiagnosticReason = prepared.lastCpuCacheDiagnosticReason;
    live.materialName = std::move(prepared.materialName);
    live.modelName = std::move(prepared.modelName);
}

uint64 RtRigidHashBytes(uint64 hash, const void* data, size_t size) noexcept
{
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t index = 0; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

bool RtPathTraceRigidMeshHasCachedRouteData(
    const RtSmokeGeometryUniverse::RigidMeshCandidateRecord& record) noexcept
{
    return cpu_producer_publish::RigidOwnedCpuMeshCacheIsUsable(
        record.valid,
        record.cachedRouteDataValid,
        record.cpuMeshContentSignature,
        record.sourceRange.vertices.count,
        record.sourceRange.indexes.count,
        record.sourceRange.triangles.count,
        record.cachedLocalVertices.size(),
        record.cachedLocalIndexes.size(),
        record.localBoundsValid && !record.localBounds.IsCleared());
}

bool RtPathTraceRefreshRigidMeshCandidateCpuCache(RtSmokeGeometryUniverse::RigidMeshCandidateRecord& record)
{
	using cpu_producer_publish::RigidCpuMeshDiagnosticReason;
	++record.cpuCacheRefreshAttempts;
	const auto recordRefreshFailure =
		[&record](RigidCpuMeshDiagnosticReason reason)
	{
		++record.cpuCacheRefreshFailures;
		record.lastCpuCacheDiagnosticReason = static_cast<uint8>(reason);
	};
	// The owned cache is the lifetime boundary.  Persist-only meshes routinely
	// arrive here after BeginFrame has cleared the observation pointer.
	if (RtPathTraceRigidMeshHasCachedRouteData(record))
	{
		++record.cpuCacheRefreshSuccesses;
		record.lastCpuCacheDiagnosticReason = static_cast<uint8>(
			RigidCpuMeshDiagnosticReason::None);
		return true;
	}

    const srfTriangles_t* const tri = record.tri;
    const idDrawVert* const verts = (tri != nullptr) ? tri->verts : nullptr;
    const triIndex_t* const triIndexes = (tri != nullptr) ? tri->indexes : nullptr;
    const int numVerts = (tri != nullptr) ? tri->numVerts : 0;
    const int numIndexes = (tri != nullptr) ? tri->numIndexes : 0;
	RigidCpuMeshDiagnosticReason snapshotFailure =
		RigidCpuMeshDiagnosticReason::SnapshotPayloadInvalid;
	if (tri == nullptr)
	{
		snapshotFailure = RigidCpuMeshDiagnosticReason::NullTri;
	}
	else if (verts == nullptr)
	{
		snapshotFailure = RigidCpuMeshDiagnosticReason::NullVertices;
	}
	else if (triIndexes == nullptr)
	{
		snapshotFailure = RigidCpuMeshDiagnosticReason::NullIndexes;
	}
	else if (record.sourceRange.vertices.count <= 0 ||
		record.sourceRange.indexes.count <= 0)
	{
		snapshotFailure = RigidCpuMeshDiagnosticReason::InvalidSourceCounts;
	}
	else if (numVerts < record.sourceRange.vertices.count ||
		numIndexes < record.sourceRange.indexes.count)
	{
		snapshotFailure = RigidCpuMeshDiagnosticReason::SourceCountsShort;
	}
    cpu_producer_publish::RigidCpuMeshPointerSnapshot snapshot;
    if (!cpu_producer_publish::SnapshotRigidCpuMeshPointers(
            tri,
            verts,
            triIndexes,
            numVerts,
            numIndexes,
            record.sourceRange.vertices.count,
            record.sourceRange.indexes.count,
            snapshot))
    {
		recordRefreshFailure(snapshotFailure);
		return false;
    }

    const idDrawVert* const localVerts = static_cast<const idDrawVert*>(snapshot.verts);
    const triIndex_t* const localIndexes = static_cast<const triIndex_t*>(snapshot.indexes);
    if (localVerts == nullptr || localIndexes == nullptr)
    {
		recordRefreshFailure(
			RigidCpuMeshDiagnosticReason::SnapshotPayloadInvalid);
		return false;
    }

    RtPathTraceRigidOwnedCpuCache cache;
    if (!RtPathTraceBuildRigidOwnedCpuCache(localVerts,
            static_cast<size_t>(snapshot.sourceVertCount), localIndexes,
            static_cast<size_t>(snapshot.sourceIndexCount),
            record.normalTexMatrix, cache))
    {
        recordRefreshFailure(RigidCpuMeshDiagnosticReason::SnapshotPayloadInvalid);
        return false;
    }
	record.cachedLocalVertices.swap(cache.vertices);
	record.cachedLocalIndexes.swap(cache.indexes);
	record.localBounds[0].Set(cache.boundsMin[0], cache.boundsMin[1],
		cache.boundsMin[2]);
	record.localBounds[1].Set(cache.boundsMax[0], cache.boundsMax[1],
		cache.boundsMax[2]);
	record.localBoundsValid = true;
	record.cpuMeshContentSignature = cache.contentSignature;
	record.cachedRouteDataValid = true;
	++record.cpuCacheRefreshSuccesses;
	record.lastCpuCacheDiagnosticReason = static_cast<uint8>(
		RigidCpuMeshDiagnosticReason::None);
	return true;
}

void RtSmokeGeometryUniverse::RecordRigidMeshCandidate(
    const RtPathTraceRigidMeshCandidateObservation& observation)
{
    RigidMeshCandidatePreparedDelta delta;
    if (PrepareRigidMeshCandidateDelta(
            observation, 0, SIZE_MAX, delta, nullptr))
    {
        (void)CommitRigidMeshCandidateDelta(delta);
    }
}

size_t RtSmokeGeometryUniverse::RigidMeshCandidateRetainedStorageBytesUnlocked() const noexcept
{
    size_t bytes = m_rigidMeshCandidateRecords.capacity() * sizeof(RigidMeshCandidateRecord);
    for (const RigidMeshCandidateRecord& record : m_rigidMeshCandidateRecords)
    {
        const size_t nestedBytes = RtRigidCandidateRecordOwnedBytes(record);
        if (nestedBytes == SIZE_MAX || bytes > SIZE_MAX - nestedBytes)
            return SIZE_MAX;
        bytes += nestedBytes;
    }
    const size_t lookupBytes = RtRigidCandidateAssociativeBytes(
        m_rigidMeshCandidateLookup.size(), m_rigidMeshCandidateLookup.bucket_count(),
        sizeof(std::unordered_map<uint64, size_t>::value_type));
    const size_t hashBytes = RtRigidCandidateAssociativeBytes(
        m_frameRigidMeshCandidateHashes.size(), m_frameRigidMeshCandidateHashes.bucket_count(),
        sizeof(std::unordered_set<uint64>::value_type));
    const size_t statsBytes =
        RtRigidCandidateStatsOwnedBytes(m_rigidMeshCandidateFrameStats);
    if (lookupBytes == SIZE_MAX || hashBytes == SIZE_MAX || statsBytes == SIZE_MAX ||
        bytes > SIZE_MAX - lookupBytes || bytes + lookupBytes > SIZE_MAX - hashBytes)
    {
        return SIZE_MAX;
    }
    const size_t topologyBytes = bytes + lookupBytes + hashBytes;
    return topologyBytes > SIZE_MAX - statsBytes
        ? SIZE_MAX : topologyBytes + statsBytes;
}

size_t RtSmokeGeometryUniverse::RigidMeshCandidateRetainedStorageBytes() const noexcept
{
    std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex);
    return RigidMeshCandidateRetainedStorageBytesUnlocked();
}

void RtSmokeGeometryUniverse::AbortRigidMeshCandidateDelta(
    RigidMeshCandidatePreparedDelta& delta) noexcept
{
    delta.complete = false;
    delta = RigidMeshCandidatePreparedDelta();
}

void RtSmokeGeometryUniverse::AdvanceRigidMeshCandidateSemanticRevisionUnlocked() const noexcept
{
    ++m_rigidMeshCandidateSemanticRevision;
    if (m_rigidMeshCandidateSemanticRevision == 0)
        ++m_rigidMeshCandidateSemanticRevision;
}

void RtSmokeGeometryUniverse::AdvanceRigidMeshCandidateFrameBeginSerialUnlocked() noexcept
{
    ++m_rigidMeshCandidateFrameBeginSerial;
    if (m_rigidMeshCandidateFrameBeginSerial == 0)
        ++m_rigidMeshCandidateFrameBeginSerial;
}

void RtSmokeGeometryUniverse::InvalidateRigidMeshCandidateLifecycleUnlocked() noexcept
{
    m_frameActive = false;
    AdvanceRigidMeshCandidateFrameBeginSerialUnlocked();
    AdvanceRigidMeshCandidateSemanticRevisionUnlocked();
}

bool RtSmokeGeometryUniverse::PrepareRigidMeshCandidateDelta(
    const RtPathTraceRigidMeshCandidateObservation& observation,
    size_t alreadyOwnedBytes,
    size_t maxOwnedBytes,
    RigidMeshCandidatePreparedDelta& delta,
    RigidMeshCandidatePrepareTestSeam* seam) noexcept
{
    return PrepareRigidMeshCandidateBatch(
        &observation, 1u, alreadyOwnedBytes, maxOwnedBytes, delta, seam);
}

bool RtSmokeGeometryUniverse::PrepareRigidMeshCandidateBatch(
    const RtPathTraceRigidMeshCandidateObservation* observations,
    size_t observationCount,
    size_t alreadyOwnedBytes,
    size_t maxOwnedBytes,
    RigidMeshCandidatePreparedDelta& delta,
    RigidMeshCandidatePrepareTestSeam* seam) noexcept
{
    AbortRigidMeshCandidateDelta(delta);
    if (seam)
    {
        seam->retainedGrowthBytes = 0;
        seam->liveReserveCalls = 0;
        seam->stagedObservationCount = 0;
    }
    const auto fail = [&]() noexcept
    {
        AbortRigidMeshCandidateDelta(delta);
        return false;
    };
    try
    {
        if ((observationCount != 0 && observations == nullptr) ||
            alreadyOwnedBytes > maxOwnedBytes)
            return fail();
        delta.owner = this;
        {
            // The lock protects only the immutable semantic snapshot. No live
            // record pointer, reference, or iterator survives this scope.
            std::unique_lock<std::mutex> lock(m_rigidMeshCandidateLookupMutex);
            if (!m_frameActive)
            {
                lock.unlock();
                return fail();
            }
            delta.baseGeneration = m_generation;
            delta.baseRigidRevision = m_rigidMeshCandidateSemanticRevision;
            delta.baseFrameBeginSerial = m_rigidMeshCandidateFrameBeginSerial;
            delta.baseFrameIndex = m_currentFrameIndex;
            delta.baseInsertTotal = m_rigidMeshCandidateRecordInsertTotal;
            delta.baseRecordCount = m_rigidMeshCandidateRecords.size();
            delta.workingGeneration = m_generation;
            delta.workingInsertTotal = m_rigidMeshCandidateRecordInsertTotal;
            delta.workingLookupHealth = m_rigidMeshCandidateLookupHealth;
            delta.workingLookupReason = m_rigidMeshCandidateLookupReason;
            delta.workingLookupCanary = m_rigidMeshCandidateLookupCanary;
            delta.workingRecords = m_rigidMeshCandidateRecords;
            delta.workingLookup = m_rigidMeshCandidateLookup;
            delta.workingFrameHashes = m_frameRigidMeshCandidateHashes;
            delta.workingStats = m_rigidMeshCandidateFrameStats;
        }
        delta.hasObservations = observationCount != 0;
        if (seam && seam->failAt == RigidMeshCandidatePrepareFailurePoint::AfterOffsideState)
            return fail();

        const auto ownedBytes = [&]() noexcept -> size_t
        {
            size_t bytes = sizeof(delta);
            for (const RigidMeshCandidateRecord& record : delta.workingRecords)
            {
                const size_t recordBytes = RtRigidCandidateRecordOwnedBytes(record);
                if (recordBytes == SIZE_MAX || bytes > SIZE_MAX - recordBytes)
                    return SIZE_MAX;
                bytes += recordBytes;
            }
            const size_t recordsCapacityBytes =
                delta.workingRecords.capacity() * sizeof(RigidMeshCandidateRecord);
            const size_t lookupBytes = RtRigidCandidateAssociativeBytes(
                delta.workingLookup.size(), delta.workingLookup.bucket_count(),
                sizeof(std::unordered_map<uint64, size_t>::value_type));
            const size_t hashBytes = RtRigidCandidateAssociativeBytes(
                delta.workingFrameHashes.size(), delta.workingFrameHashes.bucket_count(),
                sizeof(std::unordered_set<uint64>::value_type));
            const size_t statsBytes = RtRigidCandidateStatsOwnedBytes(delta.workingStats);
            if (lookupBytes == SIZE_MAX || hashBytes == SIZE_MAX || statsBytes == SIZE_MAX ||
                bytes > SIZE_MAX - recordsCapacityBytes ||
                bytes + recordsCapacityBytes > SIZE_MAX - lookupBytes ||
                bytes + recordsCapacityBytes + lookupBytes > SIZE_MAX - hashBytes ||
                bytes + recordsCapacityBytes + lookupBytes + hashBytes > SIZE_MAX - statsBytes)
                return SIZE_MAX;
            return bytes + recordsCapacityBytes + lookupBytes + hashBytes + statsBytes;
        };
        const auto withinCap = [&]() noexcept
        {
            const size_t bytes = ownedBytes();
            return bytes != SIZE_MAX && bytes <= maxOwnedBytes - alreadyOwnedBytes;
        };
        if (!withinCap()) return fail();

        for (size_t observationIndex = 0;
             observationIndex < observationCount; ++observationIndex)
        {
            const RtPathTraceRigidMeshCandidateObservation& observation =
                observations[observationIndex];
            ++delta.workingStats.observations;
            if ((observation.sourceFlags & RT_PT_INSTANCE_SOURCE_RIGID) != 0)
                ++delta.workingStats.rigidObservations;
            const uint32_t rejectFlags =
                RtPathTraceBuildRigidMeshCandidateRejectFlags(observation);
            if (rejectFlags != 0)
            {
                ++delta.workingStats.rejectedInstances;
                RtPathTraceAccumulateRigidMeshCandidateRejectStats(
                    delta.workingStats, rejectFlags);
                RtPathTraceAddRigidMeshCandidateSampleToStats(
                    delta.workingStats, observation, false, rejectFlags, 0);
                if (seam) seam->stagedObservationCount = observationIndex + 1u;
                if (!withinCap() ||
                    (seam && seam->failAfterObservation == observationIndex))
                    return fail();
                continue;
            }

            bool lookupCoherent =
                delta.workingLookupCanary ==
                    cpu_producer_publish::kRigidMeshCandidateLookupCanaryLive;
            cpu_producer_publish::RigidMeshCandidateLookupHealthState health;
            health.health = delta.workingLookupHealth;
            health.reason = delta.workingLookupReason;
            lookupCoherent = lookupCoherent &&
                !cpu_producer_publish::RigidMeshCandidateLookupIsQuarantined(health);
            if (lookupCoherent)
            {
                for (const auto& entry : delta.workingLookup)
                {
                    if (entry.second >= delta.workingRecords.size() ||
                        delta.workingRecords[entry.second].meshHash != entry.first)
                    {
                        lookupCoherent = false;
                        break;
                    }
                }
            }
            if (lookupCoherent)
            {
                for (size_t index = 0; index < delta.workingRecords.size(); ++index)
                {
                    const uint64 hash = delta.workingRecords[index].meshHash;
                    if (hash == 0) continue;
                    const auto found = delta.workingLookup.find(hash);
                    if (found == delta.workingLookup.end() || found->second != index)
                    {
                        lookupCoherent = false;
                        break;
                    }
                }
            }
            if (!lookupCoherent)
            {
                std::unordered_map<uint64, size_t> repaired;
                repaired.reserve(delta.workingRecords.size() + 1u);
                for (size_t index = 0; index < delta.workingRecords.size(); ++index)
                {
                    const uint64 hash = delta.workingRecords[index].meshHash;
                    if (hash != 0) repaired[hash] = index;
                }
                delta.workingLookup.swap(repaired);
                delta.workingLookupHealth =
                    cpu_producer_publish::kRigidMeshCandidateLookupHealthLive;
                delta.workingLookupReason =
                    cpu_producer_publish::kRigidMeshCandidateLookupReasonNone;
                delta.workingLookupCanary =
                    cpu_producer_publish::kRigidMeshCandidateLookupCanaryLive;
            }
            const auto found = delta.workingLookup.find(observation.meshHash);
            const bool newRecord = found == delta.workingLookup.end();
            const size_t recordIndex = newRecord
                ? delta.workingRecords.size() : found->second;

            const bool cacheHit = !newRecord;
            if (newRecord)
            {
                RigidMeshCandidateRecord record;
                record.valid = true;
                record.tri = observation.tri;
                record.meshHash = observation.meshHash;
                record.vertexBufferIdentity = observation.vertexBufferIdentity;
                record.indexBufferIdentity = observation.indexBufferIdentity;
                record.materialId = observation.materialId;
                record.materialClassSignature = observation.materialClassSignature;
                record.surfaceClassId = observation.surfaceClassId;
                record.triangleClassAndFlags = observation.triangleClassAndFlags != 0u
                    ? observation.triangleClassAndFlags : observation.surfaceClassId;
                record.sourceFlags = observation.sourceFlags;
                record.vertexFormat = observation.vertexFormat;
                record.modelEpoch = observation.modelEpoch;
                record.modelSurfaceIndex = observation.modelSurfaceIndex;
                record.jointIndex = observation.jointIndex;
                record.sourceRange.vertices.count = observation.numVerts;
                record.sourceRange.indexes.count = observation.numIndexes;
                record.sourceRange.triangles.count = observation.numIndexes / 3;
                record.firstSeenFrame = static_cast<int>(delta.baseFrameIndex);
                record.lastSeenFrame = static_cast<int>(delta.baseFrameIndex);
                record.materialName = observation.materialName;
                record.modelName = observation.modelName;
                RtPathTraceRefreshRigidMeshCandidateCpuCache(record);
                delta.workingRecords.push_back(std::move(record));
                delta.workingLookup[observation.meshHash] = recordIndex;
                ++delta.workingInsertTotal;
                ++delta.workingGeneration;
                if (delta.workingGeneration == 0)
                    delta.workingGeneration = 1;
            }

            RigidMeshCandidateRecord& record = delta.workingRecords[recordIndex];
            const bool cacheChanged = !cacheHit || record.cpuMeshContentSignature == 0 ||
                record.vertexBufferIdentity != observation.vertexBufferIdentity ||
                record.indexBufferIdentity != observation.indexBufferIdentity ||
                record.vertexFormat != observation.vertexFormat ||
                record.modelEpoch != observation.modelEpoch ||
                record.sourceRange.vertices.count != observation.numVerts ||
                record.sourceRange.indexes.count != observation.numIndexes ||
                std::memcmp(record.normalTexMatrix, observation.normalTexMatrix,
                    sizeof(record.normalTexMatrix)) != 0;
            record.tri = observation.tri;
            record.vertexBufferIdentity = observation.vertexBufferIdentity;
            record.indexBufferIdentity = observation.indexBufferIdentity;
            record.materialId = observation.materialId;
            record.materialClassSignature = observation.materialClassSignature;
            record.surfaceClassId = observation.surfaceClassId;
            record.triangleClassAndFlags = observation.triangleClassAndFlags != 0u
                ? observation.triangleClassAndFlags : observation.surfaceClassId;
            record.sourceFlags = observation.sourceFlags;
            record.vertexFormat = observation.vertexFormat;
            record.modelEpoch = observation.modelEpoch;
            record.modelSurfaceIndex = observation.modelSurfaceIndex;
            record.jointIndex = observation.jointIndex;
            std::memcpy(record.normalTexMatrix, observation.normalTexMatrix,
                sizeof(record.normalTexMatrix));
            record.sourceRange.vertices.count = observation.numVerts;
            record.sourceRange.indexes.count = observation.numIndexes;
            record.sourceRange.triangles.count = observation.numIndexes / 3;
            record.materialName = observation.materialName;
            record.modelName = observation.modelName;
            if (cacheChanged)
            {
                record.cachedRouteDataValid = false;
                record.cpuMeshContentSignature = 0;
                record.localBoundsValid = false;
                RtPathTraceRefreshRigidMeshCandidateCpuCache(record);
            }
            if (!record.seenThisFrame)
            {
                record.seenThisFrame = true;
                record.lastSeenFrame = static_cast<int>(delta.baseFrameIndex);
                record.newlyCreatedThisFrame = !cacheHit;
            }
            ++record.seenCount;
            ++record.instanceCountThisFrame;

            delta.workingFrameHashes.insert(observation.meshHash);
            ++delta.workingStats.eligibleInstances;
            delta.workingStats.eligibleVertsThisFrame += observation.numVerts;
            delta.workingStats.eligibleIndexesThisFrame += observation.numIndexes;
            delta.workingStats.eligibleTrianglesThisFrame += observation.numIndexes / 3;
            if ((observation.sourceFlags & RT_PT_INSTANCE_SOURCE_MATERIAL_OVERRIDE) != 0)
                ++delta.workingStats.materialOverrideEligibleInstances;
            if (cacheHit) ++delta.workingStats.reusedEligibleMeshObservations;
            else ++delta.workingStats.newlyEligibleMeshes;
            RtPathTraceAddRigidMeshCandidateSampleToStats(
                delta.workingStats, observation, true, 0, record.seenCount);
            delta.workingStats.generation = delta.workingGeneration;
            if (seam) seam->stagedObservationCount = observationIndex + 1u;
            if (!withinCap() ||
                (seam && seam->failAfterObservation == observationIndex))
                return fail();
        }

        delta.workingStats.generation = delta.workingGeneration;
        delta.requiredRecordCapacity = delta.workingRecords.size();
        {
            // Option A permits bounded retained vector growth, but no semantic
            // mutation. Revalidate after the unlocked build before reserving.
            std::unique_lock<std::mutex> lock(m_rigidMeshCandidateLookupMutex);
            if (!m_frameActive ||
                m_rigidMeshCandidateSemanticRevision != delta.baseRigidRevision ||
                m_rigidMeshCandidateFrameBeginSerial != delta.baseFrameBeginSerial ||
                m_generation != delta.baseGeneration ||
                m_currentFrameIndex != delta.baseFrameIndex ||
                m_rigidMeshCandidateRecordInsertTotal != delta.baseInsertTotal ||
                m_rigidMeshCandidateRecords.size() != delta.baseRecordCount)
            {
                lock.unlock();
                return fail();
            }
            const size_t retainedBefore = RigidMeshCandidateRetainedStorageBytesUnlocked();
            if (m_rigidMeshCandidateRecords.capacity() < delta.requiredRecordCapacity)
            {
                m_rigidMeshCandidateRecords.reserve(delta.requiredRecordCapacity);
                if (seam) ++seam->liveReserveCalls;
            }
            const size_t retainedAfter = RigidMeshCandidateRetainedStorageBytesUnlocked();
            delta.retainedGrowthBytes = retainedAfter >= retainedBefore
                ? retainedAfter - retainedBefore : 0;
            if (seam) seam->retainedGrowthBytes = delta.retainedGrowthBytes;
            const size_t stagedBytes = ownedBytes();
            if (stagedBytes == SIZE_MAX || retainedAfter == SIZE_MAX ||
                stagedBytes > maxOwnedBytes ||
                alreadyOwnedBytes > maxOwnedBytes - stagedBytes ||
                retainedAfter > maxOwnedBytes - alreadyOwnedBytes - stagedBytes)
            {
                lock.unlock();
                return fail();
            }
        }
        if (seam && seam->failAt == RigidMeshCandidatePrepareFailurePoint::AfterAllReserves)
            return fail();
        delta.complete = true;
        return true;
    }
    catch (const std::bad_alloc&)
    {
        return fail();
    }
    catch (const std::length_error&)
    {
        return fail();
    }
    catch (...)
    {
        return fail();
    }
}

static void RtCommitRigidCandidateCpuFields(
    RtSmokeGeometryUniverse::RigidMeshCandidateRecord& live,
    RtSmokeGeometryUniverse::RigidMeshCandidateRecord& staged) noexcept
{
    live.valid = staged.valid;
    live.tri = staged.tri;
    live.meshHash = staged.meshHash;
    live.vertexBufferIdentity = staged.vertexBufferIdentity;
    live.indexBufferIdentity = staged.indexBufferIdentity;
    live.materialId = staged.materialId;
    live.materialClassSignature = staged.materialClassSignature;
    live.surfaceClassId = staged.surfaceClassId;
    live.triangleClassAndFlags = staged.triangleClassAndFlags;
    live.sourceFlags = staged.sourceFlags;
    live.vertexFormat = staged.vertexFormat;
    live.modelEpoch = staged.modelEpoch;
    live.modelSurfaceIndex = staged.modelSurfaceIndex;
    live.jointIndex = staged.jointIndex;
    live.sourceRange = staged.sourceRange;
    live.firstSeenFrame = staged.firstSeenFrame;
    live.lastSeenFrame = staged.lastSeenFrame;
    live.seenCount = staged.seenCount;
    live.instanceCountThisFrame = staged.instanceCountThisFrame;
    live.seenThisFrame = staged.seenThisFrame;
    live.newlyCreatedThisFrame = staged.newlyCreatedThisFrame;
    std::memcpy(live.normalTexMatrix, staged.normalTexMatrix,
        sizeof(live.normalTexMatrix));
    live.localBounds = staged.localBounds;
    live.localBoundsValid = staged.localBoundsValid;
    live.cachedLocalVertices = std::move(staged.cachedLocalVertices);
    live.cachedLocalIndexes = std::move(staged.cachedLocalIndexes);
    live.cpuMeshContentSignature = staged.cpuMeshContentSignature;
    live.cachedRouteDataValid = staged.cachedRouteDataValid;
    live.cpuCacheRefreshAttempts = staged.cpuCacheRefreshAttempts;
    live.cpuCacheRefreshSuccesses = staged.cpuCacheRefreshSuccesses;
    live.cpuCacheRefreshFailures = staged.cpuCacheRefreshFailures;
    live.lastCpuCacheDiagnosticReason = staged.lastCpuCacheDiagnosticReason;
    live.materialName = std::move(staged.materialName);
    live.modelName = std::move(staged.modelName);
    // GPU buffers, BLAS authority, upload/build signatures, and deferred state
    // are deliberately not copied from the semantic snapshot.
}

bool RtSmokeGeometryUniverse::CommitRigidMeshCandidateDelta(
    RigidMeshCandidatePreparedDelta& delta) noexcept
{
    static_assert(std::is_nothrow_move_constructible<RigidMeshCandidateRecord>::value,
        "reserved rigid record append must not throw");
    static_assert(std::is_nothrow_move_assignable<std::vector<PathTraceSmokeVertex>>::value,
        "CPU vertex cache publication must not throw");
    static_assert(std::is_nothrow_move_assignable<std::vector<uint32_t>>::value,
        "CPU index cache publication must not throw");
    static_assert(noexcept(std::declval<idStr&>() = std::declval<idStr&&>()),
        "prepared rigid strings must publish without allocation");
    static_assert(std::is_nothrow_swappable<std::unordered_map<uint64, size_t>>::value,
        "rigid lookup batch publication must not throw");
    static_assert(std::is_nothrow_swappable<std::unordered_set<uint64>>::value,
        "rigid frame-hash batch publication must not throw");
    static_assert(std::is_nothrow_swappable<RtPathTraceRigidMeshCandidateStats>::value,
        "rigid frame stats publication must not throw");
    std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex);
    if (!delta.complete || delta.owner != this || !m_frameActive ||
        m_rigidMeshCandidateSemanticRevision != delta.baseRigidRevision ||
        m_rigidMeshCandidateFrameBeginSerial != delta.baseFrameBeginSerial ||
        m_generation != delta.baseGeneration ||
        m_currentFrameIndex != delta.baseFrameIndex ||
        m_rigidMeshCandidateRecordInsertTotal != delta.baseInsertTotal ||
        m_rigidMeshCandidateRecords.size() != delta.baseRecordCount ||
        m_rigidMeshCandidateRecords.capacity() < delta.requiredRecordCapacity)
    {
        AbortRigidMeshCandidateDelta(delta);
        return false;
    }
    for (size_t index = 0; index < delta.baseRecordCount; ++index)
        RtCommitRigidCandidateCpuFields(
            m_rigidMeshCandidateRecords[index], delta.workingRecords[index]);
    for (size_t index = delta.baseRecordCount;
         index < delta.workingRecords.size(); ++index)
        m_rigidMeshCandidateRecords.emplace_back(
            std::move(delta.workingRecords[index]));
    using std::swap;
    swap(m_rigidMeshCandidateLookup, delta.workingLookup);
    swap(m_frameRigidMeshCandidateHashes, delta.workingFrameHashes);
    swap(m_rigidMeshCandidateFrameStats, delta.workingStats);
    m_generation = delta.workingGeneration;
    m_rigidMeshCandidateRecordInsertTotal = delta.workingInsertTotal;
    m_rigidMeshCandidateLookupHealth = delta.workingLookupHealth;
    m_rigidMeshCandidateLookupReason = delta.workingLookupReason;
    m_rigidMeshCandidateLookupCanary = delta.workingLookupCanary;
    AdvanceRigidMeshCandidateSemanticRevisionUnlocked();
    delta.complete = false;
    delta.owner = nullptr;
    return true;
}

void RtSmokeGeometryUniverse::AbortRigidPreparedPayloadApply(
    RigidPreparedApplyDelta& delta) noexcept
{
    delta = RigidPreparedApplyDelta();
}

bool RtSmokeGeometryUniverse::PrepareRigidPreparedPayloadApply(
    std::vector<RtPathTraceRigidPreparedPayload>& payloads,
    const std::vector<RtPathTraceRigidMeshCandidateObservation>& observations,
    size_t alreadyOwnedBytes,
    size_t maxOwnedBytes,
    RigidPreparedApplyDelta& delta,
    RigidMeshCandidatePrepareTestSeam* seam) noexcept
{
    AbortRigidPreparedPayloadApply(delta);
    if (seam)
    {
        seam->retainedGrowthBytes = 0;
        seam->predictedFinalOwnedBytes = 0;
        seam->predictedRecordPeakBytes = 0;
        seam->predictedLookupPeakBytes = 0;
        seam->predictedFrameHashPeakBytes = 0;
        seam->predictedLookupBucketTransientBytes = 0;
        seam->predictedFrameHashBucketTransientBytes = 0;
        seam->oldLookupBucketCount = 0;
        seam->replacementLookupBucketCount = 0;
        seam->oldFrameHashBucketCount = 0;
        seam->replacementFrameHashBucketCount = 0;
        seam->liveReserveCalls = 0;
        seam->stagedObservationCount = 0;
    }
    const auto fail = [&]() noexcept
    {
        AbortRigidPreparedPayloadApply(delta);
        return false;
    };
    try
    {
        if (alreadyOwnedBytes > maxOwnedBytes || payloads.size() > UINT32_MAX ||
            !RtPathTraceRigidPreparedPayloadsCompatible(payloads, observations))
            return fail();
        size_t transferredNestedBytes = 0;
        for (const RtPathTraceRigidPreparedPayload& payload : payloads)
        {
            const size_t vertexBytes = payload.localVertices.capacity() *
                sizeof(PathTraceSmokeVertex);
            const size_t indexBytes = payload.localIndexes.capacity() *
                sizeof(std::uint32_t);
            if (vertexBytes > SIZE_MAX - transferredNestedBytes ||
                indexBytes > SIZE_MAX - transferredNestedBytes - vertexBytes)
                return fail();
            transferredNestedBytes += vertexBytes + indexBytes;
        }
        if (transferredNestedBytes > alreadyOwnedBytes) return fail();
        const size_t retainedProductBytes =
            alreadyOwnedBytes - transferredNestedBytes;
        delta.owner = this;
        delta.touched.reserve(payloads.size());
        delta.stagedLookupNodes.reserve(payloads.size());
        delta.stagedFrameHashNodes.reserve(payloads.size());
        for (RtPathTraceRigidPreparedPayload& payload : payloads)
        {
            if (payload.meshHash == 0 || payload.materialId == 0 ||
                payload.occurrenceCount == 0 ||
                payload.localVertices.size() != payload.fullTriangleVertexCount ||
                payload.localIndexes.size() != payload.fullTriangleIndexCount ||
                payload.localVertices.empty() || payload.localIndexes.empty() ||
                (payload.localIndexes.size() % 3u) != 0u ||
                payload.contentSignature == 0 ||
                !RtPathTraceValidateRigidPreparedPayload(payload))
                return fail();
            for (const RigidPreparedApplyTouchedRecord& prior : delta.touched)
                if (prior.staged.meshHash == payload.meshHash) return fail();

            RigidPreparedApplyTouchedRecord touched;
            touched.occurrenceCount = payload.occurrenceCount;
            RigidMeshCandidateRecord& record = touched.staged;
            record.valid = true;
            record.meshHash = payload.meshHash;
            record.vertexBufferIdentity = payload.vertexBufferIdentity;
            record.indexBufferIdentity = payload.indexBufferIdentity;
            record.materialId = payload.materialId;
            record.materialClassSignature = payload.materialClassSignature;
            record.surfaceClassId = payload.surfaceClassId;
            record.triangleClassAndFlags = payload.triangleClassAndFlags != 0u
                ? payload.triangleClassAndFlags : payload.surfaceClassId;
            record.sourceFlags = payload.sourceFlags;
            record.vertexFormat = payload.vertexFormat;
            record.modelEpoch = payload.modelEpoch;
            record.modelSurfaceIndex = payload.modelSurfaceIndex;
            record.jointIndex = payload.jointIndex;
            record.sourceRange.vertices.count =
                static_cast<int>(payload.fullTriangleVertexCount);
            record.sourceRange.indexes.count =
                static_cast<int>(payload.fullTriangleIndexCount);
            record.sourceRange.triangles.count =
                static_cast<int>(payload.fullTriangleIndexCount / 3u);
            std::memcpy(record.normalTexMatrix, payload.normalTexMatrix,
                sizeof(record.normalTexMatrix));
            record.localBounds[0].Set(payload.triangleBoundsMin[0],
                payload.triangleBoundsMin[1], payload.triangleBoundsMin[2]);
            record.localBounds[1].Set(payload.triangleBoundsMax[0],
                payload.triangleBoundsMax[1], payload.triangleBoundsMax[2]);
            record.localBoundsValid = !record.localBounds.IsCleared();
            if (!record.localBoundsValid) return fail();
            record.cachedLocalVertices = std::move(payload.localVertices);
            record.cachedLocalIndexes = std::move(payload.localIndexes);
            record.cpuMeshContentSignature = payload.contentSignature;
            record.cachedRouteDataValid = true;
            record.cpuCacheRefreshAttempts = 1;
            record.cpuCacheRefreshSuccesses = 1;
            record.materialName = payload.materialName;
            record.modelName = payload.modelName;
            delta.touched.push_back(std::move(touched));
        }

        {
            OPTICK_EVENT("PT Rigid Prepared Compact Snapshot");
            std::unique_lock<std::mutex> lock(m_rigidMeshCandidateLookupMutex);
            cpu_producer_publish::RigidMeshCandidateLookupHealthState health;
            health.health = m_rigidMeshCandidateLookupHealth;
            health.reason = m_rigidMeshCandidateLookupReason;
            if (!m_frameActive ||
                m_rigidMeshCandidateLookupCanary !=
                    cpu_producer_publish::kRigidMeshCandidateLookupCanaryLive ||
                cpu_producer_publish::RigidMeshCandidateLookupIsQuarantined(health))
            {
                lock.unlock();
                return fail();
            }
            delta.baseGeneration = m_generation;
            delta.baseRigidRevision = m_rigidMeshCandidateSemanticRevision;
            delta.baseFrameBeginSerial = m_rigidMeshCandidateFrameBeginSerial;
            delta.baseFrameIndex = m_currentFrameIndex;
            delta.baseInsertTotal = m_rigidMeshCandidateRecordInsertTotal;
            delta.baseRecordCount = m_rigidMeshCandidateRecords.size();
            delta.baseRecordCapacity = m_rigidMeshCandidateRecords.capacity();
            delta.baseLookupSize = m_rigidMeshCandidateLookup.size();
            delta.baseLookupBucketCount =
                m_rigidMeshCandidateLookup.bucket_count();
            delta.baseFrameHashSize = m_frameRigidMeshCandidateHashes.size();
            delta.baseFrameHashBucketCount =
                m_frameRigidMeshCandidateHashes.bucket_count();
            delta.baseLookupMaxLoadFactor =
                m_rigidMeshCandidateLookup.max_load_factor();
            delta.baseFrameHashMaxLoadFactor =
                m_frameRigidMeshCandidateHashes.max_load_factor();
            delta.finalGeneration = m_generation;
            delta.finalInsertTotal = m_rigidMeshCandidateRecordInsertTotal;

            size_t nextNewIndex = delta.baseRecordCount;
            for (RigidPreparedApplyTouchedRecord& touched : delta.touched)
            {
                RigidMeshCandidateRecord& staged = touched.staged;
                const auto found = m_rigidMeshCandidateLookup.find(staged.meshHash);
                touched.newRecord = found == m_rigidMeshCandidateLookup.end();
                touched.liveIndex = touched.newRecord ? nextNewIndex++ : found->second;
                if (!touched.newRecord)
                {
                    if (touched.liveIndex >= m_rigidMeshCandidateRecords.size() ||
                        m_rigidMeshCandidateRecords[touched.liveIndex].meshHash !=
                            staged.meshHash)
                    {
                        lock.unlock();
                        return fail();
                    }
                    const RigidMeshCandidateRecord& live =
                        m_rigidMeshCandidateRecords[touched.liveIndex];
                    staged.tri = live.tri;
                    staged.firstSeenFrame = live.firstSeenFrame;
                    staged.lastSeenFrame = live.lastSeenFrame;
                    staged.seenCount = live.seenCount;
                    staged.instanceCountThisFrame = live.instanceCountThisFrame;
                    staged.seenThisFrame = live.seenThisFrame;
                    staged.newlyCreatedThisFrame = live.newlyCreatedThisFrame;
                    staged.cpuCacheRefreshAttempts = live.cpuCacheRefreshAttempts;
                    staged.cpuCacheRefreshSuccesses = live.cpuCacheRefreshSuccesses;
                    staged.cpuCacheRefreshFailures = live.cpuCacheRefreshFailures;
                    staged.lastCpuCacheDiagnosticReason =
                        live.lastCpuCacheDiagnosticReason;
                    touched.replaceCpuCache =
                        live.cpuMeshContentSignature != staged.cpuMeshContentSignature ||
                        live.vertexBufferIdentity != staged.vertexBufferIdentity ||
                        live.indexBufferIdentity != staged.indexBufferIdentity ||
                        live.vertexFormat != staged.vertexFormat ||
                        live.modelEpoch != staged.modelEpoch ||
                        live.sourceRange.vertices.count !=
                            staged.sourceRange.vertices.count ||
                        live.sourceRange.indexes.count !=
                            staged.sourceRange.indexes.count ||
                        std::memcmp(live.normalTexMatrix, staged.normalTexMatrix,
                            sizeof(live.normalTexMatrix)) != 0;
                    if (touched.replaceCpuCache)
                    {
                        ++staged.cpuCacheRefreshAttempts;
                        ++staged.cpuCacheRefreshSuccesses;
                        staged.lastCpuCacheDiagnosticReason = static_cast<uint8>(
                            cpu_producer_publish::RigidCpuMeshDiagnosticReason::None);
                    }
                }
                else
                {
                    touched.replaceCpuCache = true;
                    staged.firstSeenFrame = static_cast<int>(delta.baseFrameIndex);
                    ++delta.finalInsertTotal;
                    ++delta.finalGeneration;
                    if (delta.finalGeneration == 0) delta.finalGeneration = 1;
                    // The legacy insert path refreshes once during record
                    // construction and once again because !cacheHit is true
                    // for the first observation, even for an identity matrix.
                    staged.cpuCacheRefreshAttempts = 2;
                    staged.cpuCacheRefreshSuccesses =
                        staged.cpuCacheRefreshAttempts;
                    staged.cpuCacheRefreshFailures = 0;
                    staged.lastCpuCacheDiagnosticReason = static_cast<uint8>(
                        cpu_producer_publish::RigidCpuMeshDiagnosticReason::None);
                }
                if (m_frameRigidMeshCandidateHashes.find(staged.meshHash) ==
                    m_frameRigidMeshCandidateHashes.end())
                    delta.stagedFrameHashNodes.emplace(staged.meshHash);
            }
        }

        for (size_t observationIndex = 0;
             observationIndex < observations.size(); ++observationIndex)
        {
            const RtPathTraceRigidMeshCandidateObservation& observation =
                observations[observationIndex];
            RigidPreparedApplyTouchedRecord* touched = nullptr;
            for (RigidPreparedApplyTouchedRecord& candidate : delta.touched)
                if (candidate.staged.meshHash == observation.meshHash)
                {
                    touched = &candidate;
                    break;
                }
            if (touched == nullptr) return fail();
            RigidMeshCandidateRecord& staged = touched->staged;
            const bool firstNewOccurrence = touched->newRecord &&
                staged.seenCount == 0;
            staged.tri = observation.tri;
            staged.vertexBufferIdentity = observation.vertexBufferIdentity;
            staged.indexBufferIdentity = observation.indexBufferIdentity;
            staged.materialId = observation.materialId;
            staged.materialClassSignature = observation.materialClassSignature;
            staged.surfaceClassId = observation.surfaceClassId;
            staged.triangleClassAndFlags = observation.triangleClassAndFlags != 0u
                ? observation.triangleClassAndFlags : observation.surfaceClassId;
            staged.sourceFlags = observation.sourceFlags;
            staged.vertexFormat = observation.vertexFormat;
            staged.modelEpoch = observation.modelEpoch;
            staged.modelSurfaceIndex = observation.modelSurfaceIndex;
            staged.jointIndex = observation.jointIndex;
            staged.sourceRange.vertices.count = observation.numVerts;
            staged.sourceRange.indexes.count = observation.numIndexes;
            staged.sourceRange.triangles.count = observation.numIndexes / 3;
            std::memcpy(staged.normalTexMatrix, observation.normalTexMatrix,
                sizeof(staged.normalTexMatrix));
            staged.materialName = observation.materialName;
            staged.modelName = observation.modelName;
            if (!staged.seenThisFrame)
            {
                staged.seenThisFrame = true;
                staged.lastSeenFrame = static_cast<int>(delta.baseFrameIndex);
                staged.newlyCreatedThisFrame = touched->newRecord;
            }
            ++staged.seenCount;
            ++staged.instanceCountThisFrame;
            ++delta.statsDelta.observations;
            if ((observation.sourceFlags & RT_PT_INSTANCE_SOURCE_RIGID) != 0)
                ++delta.statsDelta.rigidObservations;
            ++delta.statsDelta.eligibleInstances;
            delta.statsDelta.eligibleVertsThisFrame += observation.numVerts;
            delta.statsDelta.eligibleIndexesThisFrame += observation.numIndexes;
            delta.statsDelta.eligibleTrianglesThisFrame += observation.numIndexes / 3;
            if ((observation.sourceFlags &
                    RT_PT_INSTANCE_SOURCE_MATERIAL_OVERRIDE) != 0)
                ++delta.statsDelta.materialOverrideEligibleInstances;
            if (firstNewOccurrence) ++delta.statsDelta.newlyEligibleMeshes;
            else ++delta.statsDelta.reusedEligibleMeshObservations;
            RtPathTraceAddRigidMeshCandidateSampleToStats(delta.statsDelta,
                observation, true, 0, staged.seenCount);
        }
        for (const RigidPreparedApplyTouchedRecord& touched : delta.touched)
            if (touched.newRecord)
                delta.stagedLookupNodes.emplace(
                    touched.staged.meshHash, touched.liveIndex);
        delta.statsDelta.generation = delta.finalGeneration;

        size_t requiredLookupSize = 0;
        size_t requiredFrameHashSize = 0;
        if (!RtCheckedAddSize(delta.baseLookupSize,
                delta.stagedLookupNodes.size(), requiredLookupSize) ||
            !RtCheckedAddSize(delta.baseFrameHashSize,
                delta.stagedFrameHashNodes.size(), requiredFrameHashSize))
            return fail();
        delta.targetLookupBucketCount =
            RtRigidCandidateConservativeBucketTarget(
                delta.baseLookupBucketCount, requiredLookupSize,
                delta.baseLookupMaxLoadFactor);
        delta.targetFrameHashBucketCount =
            RtRigidCandidateConservativeBucketTarget(
                delta.baseFrameHashBucketCount, requiredFrameHashSize,
                delta.baseFrameHashMaxLoadFactor);
        if (delta.targetLookupBucketCount == SIZE_MAX ||
            delta.targetFrameHashBucketCount == SIZE_MAX)
            return fail();

        const auto stagedOwnedBytes = [&]() noexcept -> size_t
        {
            size_t bytes = sizeof(delta) +
                delta.touched.capacity() * sizeof(delta.touched[0]);
            for (const RigidPreparedApplyTouchedRecord& touched : delta.touched)
            {
                const size_t recordBytes =
                    RtRigidCandidateRecordOwnedBytes(touched.staged);
                if (recordBytes == SIZE_MAX || bytes > SIZE_MAX - recordBytes)
                    return SIZE_MAX;
                bytes += recordBytes;
            }
            const size_t lookupBytes = RtRigidCandidateAssociativeBytes(
                delta.stagedLookupNodes.size(),
                delta.stagedLookupNodes.bucket_count(),
                sizeof(std::unordered_map<uint64, size_t>::value_type));
            const size_t hashBytes = RtRigidCandidateAssociativeBytes(
                delta.stagedFrameHashNodes.size(),
                delta.stagedFrameHashNodes.bucket_count(),
                sizeof(std::unordered_set<uint64>::value_type));
            const size_t statsBytes =
                RtRigidCandidateStatsOwnedBytes(delta.statsDelta);
            if (lookupBytes == SIZE_MAX || hashBytes == SIZE_MAX ||
                statsBytes == SIZE_MAX || bytes > SIZE_MAX - lookupBytes ||
                bytes + lookupBytes > SIZE_MAX - hashBytes ||
                bytes + lookupBytes + hashBytes > SIZE_MAX - statsBytes)
                return SIZE_MAX;
            return bytes + lookupBytes + hashBytes + statsBytes;
        };
        const size_t preparedStagedBytes = stagedOwnedBytes();
        if (preparedStagedBytes == SIZE_MAX ||
            retainedProductBytes > maxOwnedBytes ||
            preparedStagedBytes > maxOwnedBytes - retainedProductBytes)
            return fail();

        {
            OPTICK_EVENT("PT Rigid Prepared Compact Reserve");
            std::unique_lock<std::mutex> lock(m_rigidMeshCandidateLookupMutex);
            if (!m_frameActive ||
                m_rigidMeshCandidateSemanticRevision != delta.baseRigidRevision ||
                m_rigidMeshCandidateFrameBeginSerial != delta.baseFrameBeginSerial ||
                m_generation != delta.baseGeneration ||
                m_currentFrameIndex != delta.baseFrameIndex ||
                m_rigidMeshCandidateRecordInsertTotal != delta.baseInsertTotal ||
                m_rigidMeshCandidateRecords.size() != delta.baseRecordCount ||
                m_rigidMeshCandidateRecords.capacity() !=
                    delta.baseRecordCapacity ||
                m_rigidMeshCandidateLookup.size() != delta.baseLookupSize ||
                m_rigidMeshCandidateLookup.bucket_count() !=
                    delta.baseLookupBucketCount ||
                m_frameRigidMeshCandidateHashes.size() !=
                    delta.baseFrameHashSize ||
                m_frameRigidMeshCandidateHashes.bucket_count() !=
                    delta.baseFrameHashBucketCount ||
                m_rigidMeshCandidateLookup.max_load_factor() !=
                    delta.baseLookupMaxLoadFactor ||
                m_frameRigidMeshCandidateHashes.max_load_factor() !=
                    delta.baseFrameHashMaxLoadFactor)
            {
                lock.unlock();
                return fail();
            }
            const size_t retainedBefore =
                RigidMeshCandidateRetainedStorageBytesUnlocked();
            size_t requiredRecords = 0;
            size_t requiredLookupSize = 0;
            size_t requiredFrameHashSize = 0;
            const bool recordSizeValid = RtCheckedAddSize(
                delta.baseRecordCount, delta.stagedLookupNodes.size(),
                requiredRecords);
            const bool recordReserveRequired = recordSizeValid &&
                requiredRecords > delta.baseRecordCapacity;
            const bool lookupSizeValid = RtCheckedAddSize(delta.baseLookupSize,
                delta.stagedLookupNodes.size(), requiredLookupSize);
            const bool frameHashSizeValid = RtCheckedAddSize(
                delta.baseFrameHashSize, delta.stagedFrameHashNodes.size(),
                requiredFrameHashSize);
            const bool lookupReserveRequired =
                delta.targetLookupBucketCount > delta.baseLookupBucketCount;
            const bool frameHashReserveRequired =
                delta.targetFrameHashBucketCount >
                    delta.baseFrameHashBucketCount;

            size_t ownershipBase = 0;
            size_t currentOwned = 0;
            size_t oldRecordBytes = 0;
            size_t replacementRecordBytes = 0;
            size_t oldLookupBucketBytes = 0;
            size_t replacementLookupBucketBytes = 0;
            size_t oldFrameHashBucketBytes = 0;
            size_t replacementFrameHashBucketBytes = 0;
            bool arithmeticValid =
                retainedBefore != SIZE_MAX && recordSizeValid && lookupSizeValid &&
                frameHashSizeValid &&
                delta.baseRecordCapacity <=
                    SIZE_MAX / sizeof(RigidMeshCandidateRecord) &&
                (!recordReserveRequired || requiredRecords <=
                    SIZE_MAX / sizeof(RigidMeshCandidateRecord)) &&
                RtCheckedAddSize(retainedProductBytes, preparedStagedBytes,
                    ownershipBase) &&
                RtCheckedAddSize(ownershipBase, retainedBefore, currentOwned);
            if (arithmeticValid)
            {
                oldRecordBytes = delta.baseRecordCapacity *
                    sizeof(RigidMeshCandidateRecord);
                replacementRecordBytes = (recordReserveRequired
                    ? requiredRecords : delta.baseRecordCapacity) *
                    sizeof(RigidMeshCandidateRecord);
                oldLookupBucketBytes = RtRigidCandidateBucketStorageBytes(
                    delta.baseLookupBucketCount);
                replacementLookupBucketBytes =
                    RtRigidCandidateBucketStorageBytes(
                        delta.targetLookupBucketCount);
                oldFrameHashBucketBytes = RtRigidCandidateBucketStorageBytes(
                    delta.baseFrameHashBucketCount);
                replacementFrameHashBucketBytes =
                    RtRigidCandidateBucketStorageBytes(
                        delta.targetFrameHashBucketCount);
                arithmeticValid = oldLookupBucketBytes != SIZE_MAX &&
                    replacementLookupBucketBytes != SIZE_MAX &&
                    oldFrameHashBucketBytes != SIZE_MAX &&
                    replacementFrameHashBucketBytes != SIZE_MAX;
            }

            size_t recordPeak = currentOwned;
            size_t afterRecord = currentOwned;
            size_t lookupPeak = currentOwned;
            size_t afterLookup = currentOwned;
            size_t frameHashPeak = currentOwned;
            size_t finalOwned = currentOwned;
            if (arithmeticValid && recordReserveRequired)
                arithmeticValid = RtCheckedAddSize(currentOwned,
                    replacementRecordBytes, recordPeak) &&
                    RtCheckedReplaceSize(currentOwned, oldRecordBytes,
                        replacementRecordBytes, afterRecord);
            else
                afterRecord = recordPeak;
            lookupPeak = afterRecord;
            afterLookup = afterRecord;
            if (arithmeticValid && lookupReserveRequired)
                arithmeticValid = RtCheckedAddSize(afterRecord,
                    replacementLookupBucketBytes, lookupPeak) &&
                    RtCheckedReplaceSize(afterRecord, oldLookupBucketBytes,
                        replacementLookupBucketBytes, afterLookup);
            frameHashPeak = afterLookup;
            finalOwned = afterLookup;
            if (arithmeticValid && frameHashReserveRequired)
                arithmeticValid = RtCheckedAddSize(afterLookup,
                    replacementFrameHashBucketBytes, frameHashPeak) &&
                    RtCheckedReplaceSize(afterLookup, oldFrameHashBucketBytes,
                        replacementFrameHashBucketBytes, finalOwned);

            if (seam)
            {
                seam->predictedFinalOwnedBytes = arithmeticValid
                    ? finalOwned : SIZE_MAX;
                seam->predictedRecordPeakBytes = arithmeticValid
                    ? recordPeak : SIZE_MAX;
                seam->predictedLookupPeakBytes = arithmeticValid
                    ? lookupPeak : SIZE_MAX;
                seam->predictedFrameHashPeakBytes = arithmeticValid
                    ? frameHashPeak : SIZE_MAX;
                size_t lookupTransientBytes = SIZE_MAX;
                size_t frameHashTransientBytes = SIZE_MAX;
                if (arithmeticValid)
                {
                    if (!RtCheckedAddSize(oldLookupBucketBytes,
                            replacementLookupBucketBytes,
                            lookupTransientBytes))
                        lookupTransientBytes = SIZE_MAX;
                    if (!RtCheckedAddSize(oldFrameHashBucketBytes,
                            replacementFrameHashBucketBytes,
                            frameHashTransientBytes))
                        frameHashTransientBytes = SIZE_MAX;
                }
                seam->predictedLookupBucketTransientBytes =
                    lookupTransientBytes;
                seam->predictedFrameHashBucketTransientBytes =
                    frameHashTransientBytes;
                seam->oldLookupBucketCount = delta.baseLookupBucketCount;
                seam->replacementLookupBucketCount =
                    delta.targetLookupBucketCount;
                seam->oldFrameHashBucketCount =
                    delta.baseFrameHashBucketCount;
                seam->replacementFrameHashBucketCount =
                    delta.targetFrameHashBucketCount;
            }
            if (!arithmeticValid || recordPeak > maxOwnedBytes ||
                lookupPeak > maxOwnedBytes || frameHashPeak > maxOwnedBytes ||
                finalOwned > maxOwnedBytes)
            {
                lock.unlock();
                return fail();
            }

            if (recordReserveRequired)
            {
#if !defined(_MSVC_STL_VERSION)
                lock.unlock();
                return fail();
#else
                m_rigidMeshCandidateRecords.reserve(requiredRecords);
                if (seam) ++seam->liveReserveCalls;
                if (m_rigidMeshCandidateRecords.capacity() > requiredRecords)
                {
                    lock.unlock();
                    return fail();
                }
#endif
            }
            if (seam && seam->failAt ==
                    RigidMeshCandidatePrepareFailurePoint::AfterRecordReserve)
            {
                lock.unlock();
                return fail();
            }
            if (lookupReserveRequired)
            {
                m_rigidMeshCandidateLookup.reserve(requiredLookupSize);
                if (seam) ++seam->liveReserveCalls;
                if (m_rigidMeshCandidateLookup.bucket_count() >
                        delta.targetLookupBucketCount)
                {
                    lock.unlock();
                    return fail();
                }
            }
            if (seam && seam->failAt ==
                    RigidMeshCandidatePrepareFailurePoint::AfterLookupReserve)
            {
                lock.unlock();
                return fail();
            }
            if (frameHashReserveRequired)
            {
                m_frameRigidMeshCandidateHashes.reserve(requiredFrameHashSize);
                if (seam) ++seam->liveReserveCalls;
                if (m_frameRigidMeshCandidateHashes.bucket_count() >
                        delta.targetFrameHashBucketCount)
                {
                    lock.unlock();
                    return fail();
                }
            }
            if (seam && seam->failAt ==
                    RigidMeshCandidatePrepareFailurePoint::AfterFrameHashReserve)
            {
                lock.unlock();
                return fail();
            }
            const size_t retainedAfter =
                RigidMeshCandidateRetainedStorageBytesUnlocked();
            const size_t stagedBytes = stagedOwnedBytes();
            if (retainedAfter == SIZE_MAX || stagedBytes > maxOwnedBytes ||
                retainedProductBytes > maxOwnedBytes - stagedBytes ||
                retainedAfter > maxOwnedBytes - stagedBytes - retainedProductBytes)
            {
                lock.unlock();
                return fail();
            }
            if (seam)
            {
                seam->retainedGrowthBytes = retainedAfter >= retainedBefore
                    ? retainedAfter - retainedBefore : 0;
                seam->stagedObservationCount = delta.touched.size();
            }
        }
        if (seam && seam->failAt ==
                RigidMeshCandidatePrepareFailurePoint::AfterAllReserves)
            return fail();
        delta.complete = true;
        return true;
    }
    catch (const std::bad_alloc&) { return fail(); }
    catch (const std::length_error&) { return fail(); }
    catch (...) { return fail(); }
}

bool RtSmokeGeometryUniverse::CommitRigidPreparedPayloadApply(
    RigidPreparedApplyDelta& delta) noexcept
{
    static_assert(std::is_nothrow_move_assignable<
        RtPathTraceRigidMeshCandidateSample>::value,
        "compact rigid diagnostic samples must publish without allocation");
    OPTICK_EVENT("PT Rigid Prepared Compact Commit");
    std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex);
    if (!delta.complete || delta.owner != this || !m_frameActive ||
        m_rigidMeshCandidateSemanticRevision != delta.baseRigidRevision ||
        m_rigidMeshCandidateFrameBeginSerial != delta.baseFrameBeginSerial ||
        m_generation != delta.baseGeneration ||
        m_currentFrameIndex != delta.baseFrameIndex ||
        m_rigidMeshCandidateRecordInsertTotal != delta.baseInsertTotal ||
        m_rigidMeshCandidateRecords.size() != delta.baseRecordCount ||
        m_rigidMeshCandidateRecords.capacity() <
            delta.baseRecordCount + delta.stagedLookupNodes.size())
    {
        delta.complete = false;
        return false;
    }
    for (const RigidPreparedApplyTouchedRecord& touched : delta.touched)
    {
        if ((!touched.newRecord &&
                (touched.liveIndex >= m_rigidMeshCandidateRecords.size() ||
                 m_rigidMeshCandidateRecords[touched.liveIndex].meshHash !=
                    touched.staged.meshHash)) ||
            (touched.newRecord &&
                m_rigidMeshCandidateLookup.find(touched.staged.meshHash) !=
                    m_rigidMeshCandidateLookup.end()))
        {
            delta.complete = false;
            return false;
        }
    }

    for (RigidPreparedApplyTouchedRecord& touched : delta.touched)
    {
        RigidMeshCandidateRecord& staged = touched.staged;
        if (touched.newRecord)
        {
            m_rigidMeshCandidateRecords.emplace_back(std::move(staged));
            continue;
        }
        RigidMeshCandidateRecord& live =
            m_rigidMeshCandidateRecords[touched.liveIndex];
        if (touched.replaceCpuCache)
        {
            touched.retiredVertices = std::move(live.cachedLocalVertices);
            touched.retiredIndexes = std::move(live.cachedLocalIndexes);
            live.cachedLocalVertices = std::move(staged.cachedLocalVertices);
            live.cachedLocalIndexes = std::move(staged.cachedLocalIndexes);
            live.localBounds = staged.localBounds;
            live.localBoundsValid = staged.localBoundsValid;
            live.cpuMeshContentSignature = staged.cpuMeshContentSignature;
            live.cachedRouteDataValid = staged.cachedRouteDataValid;
        }
        live.valid = staged.valid;
        live.tri = staged.tri;
        live.vertexBufferIdentity = staged.vertexBufferIdentity;
        live.indexBufferIdentity = staged.indexBufferIdentity;
        live.materialId = staged.materialId;
        live.materialClassSignature = staged.materialClassSignature;
        live.surfaceClassId = staged.surfaceClassId;
        live.triangleClassAndFlags = staged.triangleClassAndFlags;
        live.sourceFlags = staged.sourceFlags;
        live.vertexFormat = staged.vertexFormat;
        live.modelEpoch = staged.modelEpoch;
        live.modelSurfaceIndex = staged.modelSurfaceIndex;
        live.jointIndex = staged.jointIndex;
        live.sourceRange = staged.sourceRange;
        live.firstSeenFrame = staged.firstSeenFrame;
        live.lastSeenFrame = staged.lastSeenFrame;
        live.seenCount = staged.seenCount;
        live.instanceCountThisFrame = staged.instanceCountThisFrame;
        live.seenThisFrame = staged.seenThisFrame;
        live.newlyCreatedThisFrame = staged.newlyCreatedThisFrame;
        live.cpuCacheRefreshAttempts = staged.cpuCacheRefreshAttempts;
        live.cpuCacheRefreshSuccesses = staged.cpuCacheRefreshSuccesses;
        live.cpuCacheRefreshFailures = staged.cpuCacheRefreshFailures;
        live.lastCpuCacheDiagnosticReason =
            staged.lastCpuCacheDiagnosticReason;
        std::memcpy(live.normalTexMatrix, staged.normalTexMatrix,
            sizeof(live.normalTexMatrix));
        touched.retiredMaterialName = std::move(live.materialName);
        touched.retiredModelName = std::move(live.modelName);
        live.materialName = std::move(staged.materialName);
        live.modelName = std::move(staged.modelName);
        // GPU buffer/BLAS/upload/build/deferred fields remain untouched.
    }
    while (!delta.stagedLookupNodes.empty())
    {
        auto node = delta.stagedLookupNodes.extract(
            delta.stagedLookupNodes.begin());
        m_rigidMeshCandidateLookup.insert(std::move(node));
    }
    while (!delta.stagedFrameHashNodes.empty())
    {
        auto node = delta.stagedFrameHashNodes.extract(
            delta.stagedFrameHashNodes.begin());
        m_frameRigidMeshCandidateHashes.insert(std::move(node));
    }
    m_generation = delta.finalGeneration;
    m_rigidMeshCandidateRecordInsertTotal = delta.finalInsertTotal;
    RtPathTraceRigidMeshCandidateStats& liveStats =
        m_rigidMeshCandidateFrameStats;
    liveStats.observations += delta.statsDelta.observations;
    liveStats.rigidObservations += delta.statsDelta.rigidObservations;
    liveStats.eligibleInstances += delta.statsDelta.eligibleInstances;
    liveStats.eligibleVertsThisFrame +=
        delta.statsDelta.eligibleVertsThisFrame;
    liveStats.eligibleIndexesThisFrame +=
        delta.statsDelta.eligibleIndexesThisFrame;
    liveStats.eligibleTrianglesThisFrame +=
        delta.statsDelta.eligibleTrianglesThisFrame;
    liveStats.materialOverrideEligibleInstances +=
        delta.statsDelta.materialOverrideEligibleInstances;
    liveStats.reusedEligibleMeshObservations +=
        delta.statsDelta.reusedEligibleMeshObservations;
    liveStats.newlyEligibleMeshes += delta.statsDelta.newlyEligibleMeshes;
    for (int sampleIndex = 0;
         sampleIndex < delta.statsDelta.eligibleSampleCount &&
         liveStats.eligibleSampleCount < RT_PT_RIGID_MESH_CANDIDATE_SAMPLES;
         ++sampleIndex)
    {
        liveStats.eligibleSamples[liveStats.eligibleSampleCount++] =
            std::move(delta.statsDelta.eligibleSamples[sampleIndex]);
    }
    liveStats.generation = delta.finalGeneration;
    AdvanceRigidMeshCandidateSemanticRevisionUnlocked();
    delta.complete = false;
    delta.owner = nullptr;
    return true;
}

#if defined(RT_PT_RIGID_PREPARED_DELTA_HARNESS)
uint64 RtSmokeGeometryUniverse::RigidMeshCandidateSemanticFingerprintForTest() const noexcept
{
    std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex);
    uint64 hash = 14695981039346656037ull;
    const auto append = [&hash](const void* data, size_t bytes)
    {
        hash = RtRigidHashBytes(hash, data, bytes);
    };
    append(&m_generation, sizeof(m_generation));
    append(&m_rigidMeshCandidateRecordInsertTotal,
        sizeof(m_rigidMeshCandidateRecordInsertTotal));
    const size_t recordCount = m_rigidMeshCandidateRecords.size();
    append(&recordCount, sizeof(recordCount));
    for (const RigidMeshCandidateRecord& record : m_rigidMeshCandidateRecords)
    {
        append(&record.valid, sizeof(record.valid));
        append(&record.tri, sizeof(record.tri));
        append(&record.meshHash, sizeof(record.meshHash));
        append(&record.vertexBufferIdentity, sizeof(record.vertexBufferIdentity));
        append(&record.indexBufferIdentity, sizeof(record.indexBufferIdentity));
        append(&record.materialId, sizeof(record.materialId));
        append(&record.materialClassSignature, sizeof(record.materialClassSignature));
        append(&record.surfaceClassId, sizeof(record.surfaceClassId));
        append(&record.triangleClassAndFlags, sizeof(record.triangleClassAndFlags));
        append(&record.sourceFlags, sizeof(record.sourceFlags));
        append(&record.vertexFormat, sizeof(record.vertexFormat));
        append(&record.modelEpoch, sizeof(record.modelEpoch));
        append(&record.modelSurfaceIndex, sizeof(record.modelSurfaceIndex));
        append(&record.jointIndex, sizeof(record.jointIndex));
        append(&record.sourceRange, sizeof(record.sourceRange));
        append(&record.firstSeenFrame, sizeof(record.firstSeenFrame));
        append(&record.lastSeenFrame, sizeof(record.lastSeenFrame));
        append(&record.seenCount, sizeof(record.seenCount));
        append(&record.instanceCountThisFrame, sizeof(record.instanceCountThisFrame));
        append(&record.seenThisFrame, sizeof(record.seenThisFrame));
        append(&record.newlyCreatedThisFrame, sizeof(record.newlyCreatedThisFrame));
        append(record.normalTexMatrix, sizeof(record.normalTexMatrix));
        append(&record.localBounds, sizeof(record.localBounds));
        append(&record.localBoundsValid, sizeof(record.localBoundsValid));
        if (!record.cachedLocalVertices.empty()) append(record.cachedLocalVertices.data(),
            record.cachedLocalVertices.size() * sizeof(record.cachedLocalVertices[0]));
        if (!record.cachedLocalIndexes.empty()) append(record.cachedLocalIndexes.data(),
            record.cachedLocalIndexes.size() * sizeof(record.cachedLocalIndexes[0]));
        append(&record.cpuMeshContentSignature, sizeof(record.cpuMeshContentSignature));
        append(&record.cachedRouteDataValid, sizeof(record.cachedRouteDataValid));
        append(&record.cpuCacheRefreshAttempts, sizeof(record.cpuCacheRefreshAttempts));
        append(&record.cpuCacheRefreshSuccesses, sizeof(record.cpuCacheRefreshSuccesses));
        append(&record.cpuCacheRefreshFailures, sizeof(record.cpuCacheRefreshFailures));
        append(&record.lastCpuCacheDiagnosticReason, sizeof(record.lastCpuCacheDiagnosticReason));
        append(record.materialName.c_str(), record.materialName.Length());
        append(record.modelName.c_str(), record.modelName.Length());
    }
    uint64 lookupFold = 0;
    for (const auto& entry : m_rigidMeshCandidateLookup)
        lookupFold ^= entry.first * 1099511628211ull + static_cast<uint64>(entry.second);
    append(&lookupFold, sizeof(lookupFold));
    uint64 frameFold = 0;
    for (uint64 value : m_frameRigidMeshCandidateHashes)
        frameFold ^= value * 1099511628211ull;
    append(&frameFold, sizeof(frameFold));
    append(&m_rigidMeshCandidateFrameStats.observations,
        offsetof(RtPathTraceRigidMeshCandidateStats, eligibleSamples));
    for (int index = 0; index < m_rigidMeshCandidateFrameStats.eligibleSampleCount; ++index)
    {
        const auto& sample = m_rigidMeshCandidateFrameStats.eligibleSamples[index];
        append(&sample.valid, offsetof(RtPathTraceRigidMeshCandidateSample, materialName));
        append(sample.materialName.c_str(), sample.materialName.Length());
        append(sample.modelName.c_str(), sample.modelName.Length());
    }
    for (int index = 0; index < m_rigidMeshCandidateFrameStats.rejectedSampleCount; ++index)
    {
        const auto& sample = m_rigidMeshCandidateFrameStats.rejectedSamples[index];
        append(&sample.valid, offsetof(RtPathTraceRigidMeshCandidateSample, materialName));
        append(sample.materialName.c_str(), sample.materialName.Length());
        append(sample.modelName.c_str(), sample.modelName.Length());
    }
    return hash;
}

size_t RtSmokeGeometryUniverse::RigidMeshCandidateRecordCountForTest() const noexcept
{
    std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex);
    return m_rigidMeshCandidateRecords.size();
}
size_t RtSmokeGeometryUniverse::RigidMeshCandidateLookupCountForTest() const noexcept
{
    std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex);
    return m_rigidMeshCandidateLookup.size();
}
size_t RtSmokeGeometryUniverse::RigidMeshCandidateFrameHashCountForTest() const noexcept
{
    std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex);
    return m_frameRigidMeshCandidateHashes.size();
}
bool RtSmokeGeometryUniverse::RigidMeshCandidateHasRecordForTest(uint64 meshHash) const noexcept
{
    std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex);
    const auto found = m_rigidMeshCandidateLookup.find(meshHash);
    return found != m_rigidMeshCandidateLookup.end() &&
        found->second < m_rigidMeshCandidateRecords.size() &&
        m_rigidMeshCandidateRecords[found->second].meshHash == meshHash;
}
void RtSmokeGeometryUniverse::RigidMeshCandidatePoisonLookupForTest(
    uint64 meshHash, size_t index)
{
    std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex);
    m_rigidMeshCandidateLookup[meshHash] = index;
    AdvanceRigidMeshCandidateSemanticRevisionUnlocked();
}
void RtSmokeGeometryUniverse::RigidMeshCandidateAdvanceGenerationForTest() noexcept
{
    std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex);
    ++m_generation;
    if (m_generation == 0) ++m_generation;
}
void RtSmokeGeometryUniverse::RigidMeshCandidateBeginFrameForTest(
    uint64 frameIndex) noexcept
{
    std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex);
    m_currentFrameIndex = frameIndex;
    AdvanceRigidMeshCandidateFrameBeginSerialUnlocked();
    m_frameActive = true;
    m_rigidMeshCandidateFrameStats = RtPathTraceRigidMeshCandidateStats();
    m_rigidMeshCandidateFrameStats.frameIndex = frameIndex;
    m_rigidMeshCandidateFrameStats.generation = m_generation;
    m_rigidMeshCandidateRecordInsertFrameStart =
        m_rigidMeshCandidateRecordInsertTotal;
    m_frameRigidMeshCandidateHashes.clear();
    for (RigidMeshCandidateRecord& record : m_rigidMeshCandidateRecords)
    {
        record.seenThisFrame = false;
        record.newlyCreatedThisFrame = false;
        record.instanceCountThisFrame = 0;
        record.tri = nullptr;
    }
    AdvanceRigidMeshCandidateSemanticRevisionUnlocked();
}
void RtSmokeGeometryUniverse::RigidMeshCandidateEndFrameForTest() noexcept
{
    std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex);
    if (!m_frameActive) return;
    m_frameActive = false;
    AdvanceRigidMeshCandidateSemanticRevisionUnlocked();
}
void RtSmokeGeometryUniverse::RigidMeshCandidateResetForTest() noexcept
{
    std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex);
    m_rigidMeshCandidateRecords.clear();
    m_rigidMeshCandidateLookup.clear();
    m_frameRigidMeshCandidateHashes.clear();
    m_rigidMeshCandidateFrameStats = RtPathTraceRigidMeshCandidateStats();
    m_rigidMeshCandidateRecordInsertTotal = 0;
    m_currentFrameIndex = 0;
    InvalidateRigidMeshCandidateLifecycleUnlocked();
}
void RtSmokeGeometryUniverse::RigidMeshCandidateAdvanceSemanticRevisionForTest() noexcept
{
    std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex);
    AdvanceRigidMeshCandidateSemanticRevisionUnlocked();
}
void RtSmokeGeometryUniverse::RigidMeshCandidateSetGpuSignatureForTest(
    uint64 meshHash, uint64 signature) noexcept
{
    std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex);
    const auto found = m_rigidMeshCandidateLookup.find(meshHash);
    if (found != m_rigidMeshCandidateLookup.end() &&
        found->second < m_rigidMeshCandidateRecords.size())
    {
        RigidMeshCandidateRecord& record = m_rigidMeshCandidateRecords[found->second];
        record.gpuUploadSignature = signature;
        record.gpuBuffersUploaded = true;
        record.gpuBlasCreated = true;
        record.gpuBlasBuildSubmitted = true;
    }
}
uint64 RtSmokeGeometryUniverse::RigidMeshCandidateGpuSignatureForTest(
    uint64 meshHash) const noexcept
{
    std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex);
    const auto found = m_rigidMeshCandidateLookup.find(meshHash);
    return found != m_rigidMeshCandidateLookup.end() &&
        found->second < m_rigidMeshCandidateRecords.size()
        ? m_rigidMeshCandidateRecords[found->second].gpuUploadSignature : 0;
}
void RtSmokeGeometryUniverse::RigidMeshCandidateSetFrameBeginSerialForTest(
    uint64 serial) noexcept
{
    std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex);
    m_rigidMeshCandidateFrameBeginSerial = serial;
}
uint64 RtSmokeGeometryUniverse::RigidMeshCandidateFrameBeginSerialForTest() const noexcept
{
    std::lock_guard<std::mutex> lock(m_rigidMeshCandidateLookupMutex);
    return m_rigidMeshCandidateFrameBeginSerial;
}
#endif
