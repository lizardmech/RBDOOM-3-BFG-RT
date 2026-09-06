#pragma once

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <iterator>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

constexpr std::size_t RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES =
    16u * 1024u * 1024u;
constexpr std::size_t RT_PT_CAPTURE_PRODUCT_LANE_A_RING_MAX_BYTES =
    3u * RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES;
constexpr std::size_t RT_PT_PLANNING_MAP_NAME_CAPACITY = 256u;
constexpr std::size_t RT_PT_PLANNING_NAME_CAPACITY = 256u;

struct RtPathTracePlanningSnapshotEpoch
{
    std::uint64_t generation = 0;
    std::uint64_t frameIndex = 0;
    std::uint64_t mapTimeStamp = 0;
    std::uint64_t mapLoadSerial = 0;
    char mapName[RT_PT_PLANNING_MAP_NAME_CAPACITY] = {};
    bool capturedAfterBeginFrame = false;
    bool capturedAfterStaticPreload = false;
    bool capturedBeforeSerialMutate = false;
};

inline void RtPathTracePlanningCopyName(
    char* destination, std::size_t capacity, const char* source)
{
    if (!destination || capacity == 0)
    {
        return;
    }
    const char* text = source ? source : "";
    std::size_t count = 0;
    while (count + 1 < capacity && text[count] != '\0')
    {
        destination[count] = text[count];
        ++count;
    }
    destination[count] = '\0';
}

inline bool RtPathTracePlanningEpochValid(
    const RtPathTracePlanningSnapshotEpoch& epoch)
{
    return epoch.generation != 0 && epoch.capturedAfterBeginFrame &&
        epoch.capturedAfterStaticPreload &&
        epoch.capturedBeforeSerialMutate;
}

inline bool RtPathTracePlanningEpochsMatch(
    const RtPathTracePlanningSnapshotEpoch& lhs,
    const RtPathTracePlanningSnapshotEpoch& rhs)
{
    if (!RtPathTracePlanningEpochValid(lhs) ||
        !RtPathTracePlanningEpochValid(rhs) ||
        lhs.generation != rhs.generation ||
        lhs.frameIndex != rhs.frameIndex ||
        lhs.mapTimeStamp != rhs.mapTimeStamp ||
        lhs.mapLoadSerial != rhs.mapLoadSerial)
    {
        return false;
    }
    for (std::size_t index = 0;
        index < RT_PT_PLANNING_MAP_NAME_CAPACITY; ++index)
    {
        if (lhs.mapName[index] != rhs.mapName[index])
        {
            return false;
        }
    }
    return true;
}

template<typename LeftSnapshot, typename RightSnapshot>
inline bool RtPathTracePlanningSnapshotsCoherent(
    const LeftSnapshot& lhs, const RightSnapshot& rhs)
{
    return lhs.complete && rhs.complete &&
        RtPathTracePlanningEpochsMatch(lhs.epoch, rhs.epoch);
}

inline void RtPathTraceInvalidatePlanningEpoch(
    RtPathTracePlanningSnapshotEpoch& epoch)
{
    epoch = RtPathTracePlanningSnapshotEpoch{};
}

inline bool RtPathTracePlanningAccumulateBytes(
    std::size_t bytes, std::size_t& slotBytes)
{
    if (bytes > RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES ||
        slotBytes > RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES - bytes)
    {
        return false;
    }
    slotBytes += bytes;
    return true;
}

inline bool RtPathTracePlanningAccumulateArrayBytes(
    std::size_t count, std::size_t elementBytes, std::size_t& slotBytes)
{
    if (elementBytes != 0 && count >
        RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES / elementBytes)
    {
        return false;
    }
    return RtPathTracePlanningAccumulateBytes(count * elementBytes, slotBytes);
}

template<typename StaticRecords>
inline bool RtPathTracePlanningHasStaticSurface(
    const StaticRecords& records, std::uint64_t key)
{
    const auto found = std::lower_bound(records.begin(), records.end(), key,
        [](const auto& record, std::uint64_t value)
        {
            return record.key < value;
        });
    return found != records.end() && found->key == key && found->valid;
}

template<typename HistoryRecords>
inline const typename HistoryRecords::value_type*
RtPathTracePlanningFindInstanceHistory(
    const HistoryRecords& records, std::uint64_t instanceId)
{
    const auto found = std::lower_bound(records.begin(), records.end(), instanceId,
        [](const auto& history, std::uint64_t value)
        {
            return history.instanceId < value;
        });
    return found != records.end() && found->instanceId == instanceId
        ? &*found : nullptr;
}

inline bool RtPathTracePlanningRigidRouteReady(
    bool cpuReady,
    bool hasVertexBuffer,
    bool hasIndexBuffer,
    bool hasBlas,
    bool buffersUploaded,
    bool blasCreated,
    bool blasBuildSubmitted,
    bool uploadSignatureMatches,
    int blasVertexCount,
    int cachedVertexCount,
    int blasIndexCount,
    int cachedIndexCount)
{
    return cpuReady && hasVertexBuffer && hasIndexBuffer && hasBlas &&
        buffersUploaded && blasCreated && blasBuildSubmitted &&
        uploadSignatureMatches && blasVertexCount == cachedVertexCount &&
        blasIndexCount == cachedIndexCount;
}

template<typename ResidentRecords, typename RouteReady>
inline bool RtPathTracePlanningRigidResidentReady(
    const ResidentRecords& records,
    int entityIndex,
    int renderEntityNum,
    std::uint32_t materialId,
    RouteReady&& routeReady)
{
    if (entityIndex < 0 || renderEntityNum < 0 || materialId == 0)
    {
        return false;
    }
    for (const auto& resident : records)
    {
        if (resident.entityIndex == entityIndex &&
            resident.renderEntityNum == renderEntityNum &&
            resident.materialId == materialId && routeReady(resident.meshHash))
        {
            return true;
        }
    }
    return false;
}

enum class RtSmokeGeometryBufferFormat : std::uint32_t
{
    LegacySmokeVertex = 0
};

struct RtSmokeGeometryElementRange
{
    int offset = -1;
    int count = 0;
};

struct RtSmokeGeometryRangeRecord
{
    RtSmokeGeometryElementRange vertices;
    RtSmokeGeometryElementRange indexes;
    RtSmokeGeometryElementRange triangles;
};

struct RtPathTraceInstanceMeshRecordPod
{
    std::uint64_t stableHash = 0;
    std::uintptr_t vertexBufferIdentity = 0;
    std::uintptr_t indexBufferIdentity = 0;
    int numVerts = 0;
    int numIndexes = 0;
    std::uint32_t vertexFormat = 0;
    std::uint32_t materialId = 0;
    std::uint32_t sourceKind = 0;
    std::uint64_t lastSeenFrame = 0;
    bool localSpaceValid = false;
    char materialName[RT_PT_PLANNING_NAME_CAPACITY] = {};
    char modelName[RT_PT_PLANNING_NAME_CAPACITY] = {};
};

struct RtPathTraceInstanceHistoryPod
{
    std::uint64_t instanceId = 0;
    std::uint64_t lastSeenFrame = 0;
    float firstObjectToWorld[16] = {};
    float lastObjectToWorld[16] = {};
    float maxObservedMatrixDelta = 0.0f;
    float maxObservedOriginDelta = 0.0f;
    int sameTransformCount = 0;
    int changedTransformCount = 0;
};

struct RtPathTraceInstanceHistoryApplication
{
    bool found = false;
    bool hasPreviousObjectToWorld = false;
    bool transformContinuous = false;
    float currentObjectToWorld[16] = {};
    float previousObjectToWorld[16] = {};
};

struct RtPathTraceInstanceUniverseSnapshot
{
    RtPathTracePlanningSnapshotEpoch epoch;
    std::uint64_t ownerGeneration = 0;
    std::uint64_t historyCopyUs = 0;
    std::size_t historyBytes = 0;
    std::size_t historyRows = 0;
    std::vector<RtPathTraceInstanceMeshRecordPod> meshes;
    std::vector<RtPathTraceInstanceHistoryPod> histories;
    bool complete = false;

    void Invalidate()
    {
        epoch = RtPathTracePlanningSnapshotEpoch{};
        ownerGeneration = 0;
        historyCopyUs = 0;
        historyBytes = 0;
        historyRows = 0;
        meshes.clear();
        histories.clear();
        complete = false;
    }

    void ResetAndRelease()
    {
        RtPathTraceInstanceUniverseSnapshot empty;
        *this = std::move(empty);
    }

    std::size_t OwnedBytes() const
    {
        return sizeof(*this) +
            meshes.capacity() * sizeof(RtPathTraceInstanceMeshRecordPod) +
            histories.capacity() * sizeof(RtPathTraceInstanceHistoryPod);
    }
};

struct RtPathTraceInstanceUniverseSnapshotCounts
{
    std::size_t meshes = 0;
    std::size_t histories = 0;
};

struct RtSmokeStaticSurfacePod
{
    bool valid = false;
    std::uint64_t key = 0;
    std::uint64_t bucketSurfaceKey = 0;
    std::uint32_t surfaceClassId = 0;
    std::uint32_t materialId = 0;
    int portalArea = -1;
    RtSmokeGeometryRangeRecord currentRange;
    RtSmokeGeometryRangeRecord previousRange;
    std::uint64_t lastSeenFrame = 0;
    std::uint64_t previousSeenFrame = 0;
    bool previousRangeValid = false;
    bool historyValid = false;
    bool dirty = true;
    std::uint64_t materialGeneration = 0;
    RtSmokeGeometryBufferFormat geometryFormat =
        RtSmokeGeometryBufferFormat::LegacySmokeVertex;
};

struct RtSmokeRigidRouteReadyPod
{
    bool valid = false;
    std::uint64_t meshHash = 0;
    std::uintptr_t vertexBufferIdentity = 0;
    std::uintptr_t indexBufferIdentity = 0;
    std::uint32_t materialId = 0;
    std::uint32_t vertexFormat = 0;
    std::uint32_t modelEpoch = 0;
    int modelSurfaceIndex = -1;
    int jointIndex = -1;
    RtSmokeGeometryRangeRecord sourceRange;
    bool cachedRouteDataValid = false;
    bool localBoundsValid = false;
    float normalTexMatrix[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    std::uint64_t cpuMeshContentSignature = 0;
    std::uint64_t gpuUploadSignature = 0;
    int cachedVertexCount = 0;
    int cachedIndexCount = 0;
    int gpuBlasVertexCount = 0;
    int gpuBlasIndexCount = 0;
    bool hasRigidVertexBuffer = false;
    bool hasRigidIndexBuffer = false;
    bool hasRigidBlas = false;
    bool gpuBuffersUploaded = false;
    bool gpuBlasCreated = false;
    bool gpuBlasBuildSubmitted = false;
    std::uint64_t deferredSinceFrame = 0;
};

struct RtSmokeRigidResidentReadyPod
{
    std::uint64_t instanceId = 0;
    std::uint64_t meshHash = 0;
    int entityIndex = -1;
    int renderEntityNum = -1;
    std::uint32_t materialId = 0;
    std::uint64_t lastSeenFrame = 0;
};

struct RtSmokeRigidRouteRawKeyPod
{
    std::uintptr_t vertexBufferIdentity = 0;
    std::uintptr_t indexBufferIdentity = 0;
    int vertexCount = 0;
    int indexCount = 0;
    int triangleCount = 0;
    std::uint32_t materialId = 0;
};

struct RtSmokeGeometryUniverseSnapshot
{
    RtPathTracePlanningSnapshotEpoch epoch;
    std::uint64_t ownerGeneration = 0;
    std::vector<RtSmokeStaticSurfacePod> staticSurfaces;
    std::vector<RtSmokeRigidRouteReadyPod> rigidRoutes;
    std::vector<RtSmokeRigidResidentReadyPod> rigidResidents;
    bool complete = false;

    void Invalidate()
    {
        epoch = RtPathTracePlanningSnapshotEpoch{};
        ownerGeneration = 0;
        staticSurfaces.clear();
        rigidRoutes.clear();
        rigidResidents.clear();
        complete = false;
    }

    void ResetAndRelease()
    {
        RtSmokeGeometryUniverseSnapshot empty;
        *this = std::move(empty);
    }

    std::size_t OwnedBytes() const
    {
        return sizeof(*this) +
            staticSurfaces.capacity() * sizeof(RtSmokeStaticSurfacePod) +
            rigidRoutes.capacity() * sizeof(RtSmokeRigidRouteReadyPod) +
            rigidResidents.capacity() * sizeof(RtSmokeRigidResidentReadyPod);
    }
};

struct RtSmokeGeometryUniverseSnapshotCounts
{
    std::size_t staticSurfaces = 0;
    std::size_t rigidRoutes = 0;
    std::size_t rigidResidents = 0;
};

static_assert(std::is_trivially_copyable<RtPathTraceInstanceMeshRecordPod>::value,
    "instance mesh snapshot rows must remain pointer-free POD");
static_assert(std::is_trivially_copyable<RtPathTraceInstanceHistoryPod>::value,
    "instance history snapshot rows must remain pointer-free POD");
static_assert(std::is_trivially_copyable<RtSmokeStaticSurfacePod>::value,
    "static snapshot rows must remain pointer-free POD");
static_assert(std::is_trivially_copyable<RtSmokeRigidRouteReadyPod>::value,
    "rigid route snapshot rows must remain pointer-free POD");
static_assert(std::is_trivially_copyable<RtSmokeRigidResidentReadyPod>::value,
    "rigid resident snapshot rows must remain pointer-free POD");
static_assert(std::is_nothrow_move_assignable<RtPathTraceInstanceUniverseSnapshot>::value,
    "instance snapshot commit must be atomic and non-throwing");
static_assert(std::is_nothrow_move_assignable<RtSmokeGeometryUniverseSnapshot>::value,
    "geometry snapshot commit must be atomic and non-throwing");

inline RtPathTraceInstanceHistoryApplication
ApplyInstanceHistoryRowsValidatedFromPod(
    bool epochAndFrameValid,
    const RtPathTraceInstanceHistoryPod* histories,
    std::size_t historyCount,
    std::uint64_t instanceId,
    const float currentObjectToWorld[16],
    std::uint64_t frameIndex)
{
    RtPathTraceInstanceHistoryApplication result;
    if (!currentObjectToWorld)
    {
        return result;
    }
    for (int element = 0; element < 16; ++element)
    {
        result.currentObjectToWorld[element] = currentObjectToWorld[element];
    }
    if (!epochAndFrameValid || instanceId == 0 ||
        (historyCount != 0 && histories == nullptr))
    {
        return result;
    }
    if (historyCount == 0)
    {
        return result;
    }
    const RtPathTraceInstanceHistoryPod* first = std::lower_bound(
        histories, histories + historyCount, instanceId,
        [](const RtPathTraceInstanceHistoryPod& row, std::uint64_t value)
        {
            return row.instanceId < value;
        });
    if (first == histories + historyCount || first->instanceId != instanceId)
    {
        return result;
    }
    const RtPathTraceInstanceHistoryPod* next = std::upper_bound(
        histories, histories + historyCount, instanceId,
        [](std::uint64_t value, const RtPathTraceInstanceHistoryPod& row)
        {
            return value < row.instanceId;
        });
    if (next - first != 1)
    {
        return result;
    }
    result.found = true;
    result.hasPreviousObjectToWorld = first->lastSeenFrame > 0 &&
        first->lastSeenFrame + 1 == frameIndex;
    result.transformContinuous = result.hasPreviousObjectToWorld;
    if (result.hasPreviousObjectToWorld)
    {
        for (int element = 0; element < 16; ++element)
        {
            result.previousObjectToWorld[element] =
                first->lastObjectToWorld[element];
        }
    }
    return result;
}

inline RtPathTraceInstanceHistoryApplication ApplyInstanceHistoryRowsFromPod(
    const RtPathTracePlanningSnapshotEpoch& epoch,
    const RtPathTraceInstanceHistoryPod* histories,
    std::size_t historyCount,
    std::uint64_t instanceId,
    const float currentObjectToWorld[16],
    std::uint64_t frameIndex)
{
    return ApplyInstanceHistoryRowsValidatedFromPod(
        RtPathTracePlanningEpochValid(epoch) && epoch.frameIndex == frameIndex,
        histories, historyCount, instanceId, currentObjectToWorld, frameIndex);
}

inline RtPathTraceInstanceHistoryApplication ApplyInstanceHistoryFromPod(
    const RtPathTraceInstanceUniverseSnapshot& snapshot,
    std::uint64_t instanceId,
    const float currentObjectToWorld[16],
    std::uint64_t frameIndex)
{
    if (!snapshot.complete)
    {
        return RtPathTraceInstanceHistoryApplication{};
    }
    return ApplyInstanceHistoryRowsFromPod(
        snapshot.epoch, snapshot.histories.data(), snapshot.histories.size(),
        instanceId, currentObjectToWorld, frameIndex);
}

inline bool HasStaticSurfaceFromPod(
    const RtSmokeGeometryUniverseSnapshot& snapshot, std::uint64_t key)
{
    if (!snapshot.complete || !RtPathTracePlanningEpochValid(snapshot.epoch))
    {
        return false;
    }
    const auto first = std::lower_bound(snapshot.staticSurfaces.begin(),
        snapshot.staticSurfaces.end(), key,
        [](const RtSmokeStaticSurfacePod& row, std::uint64_t value)
        {
            return row.key < value;
        });
    if (first == snapshot.staticSurfaces.end() || first->key != key ||
        !first->valid)
    {
        return false;
    }
    return std::next(first) == snapshot.staticSurfaces.end() ||
        std::next(first)->key != key;
}

inline std::uint64_t RtPathTraceRigidUploadSignatureFromPod(
    const RtSmokeRigidRouteReadyPod& record)
{
    std::uint64_t hash = 14695981039346656037ull;
    const auto add = [&hash](const void* data, std::size_t bytes)
    {
        const unsigned char* source = static_cast<const unsigned char*>(data);
        for (std::size_t index = 0; index < bytes; ++index)
        {
            hash ^= source[index];
            hash *= 1099511628211ull;
        }
    };
    add(&record.meshHash, sizeof(record.meshHash));
    add(&record.cpuMeshContentSignature, sizeof(record.cpuMeshContentSignature));
    add(&record.vertexBufferIdentity, sizeof(record.vertexBufferIdentity));
    add(&record.indexBufferIdentity, sizeof(record.indexBufferIdentity));
    add(&record.materialId, sizeof(record.materialId));
    add(record.normalTexMatrix, sizeof(record.normalTexMatrix));
    add(&record.sourceRange.vertices.count,
        sizeof(record.sourceRange.vertices.count));
    add(&record.sourceRange.indexes.count,
        sizeof(record.sourceRange.indexes.count));
    return hash;
}

inline bool RtPathTraceOwnedCpuMeshUsableFromPod(
    const RtSmokeRigidRouteReadyPod& record)
{
    return record.valid && record.cachedRouteDataValid &&
        record.cpuMeshContentSignature != 0 &&
        record.sourceRange.vertices.count > 0 &&
        record.sourceRange.indexes.count > 0 &&
        (record.sourceRange.indexes.count % 3) == 0 &&
        record.sourceRange.triangles.count > 0 &&
        record.sourceRange.triangles.count * 3 ==
            record.sourceRange.indexes.count &&
        record.cachedVertexCount == record.sourceRange.vertices.count &&
        record.cachedIndexCount == record.sourceRange.indexes.count &&
        record.localBoundsValid;
}

inline bool RtPathTraceRigidRouteReadyRecordFromPod(
    const RtSmokeRigidRouteReadyPod& record)
{
    return RtPathTracePlanningRigidRouteReady(
        RtPathTraceOwnedCpuMeshUsableFromPod(record),
        record.hasRigidVertexBuffer,
        record.hasRigidIndexBuffer,
        record.hasRigidBlas,
        record.gpuBuffersUploaded,
        record.gpuBlasCreated,
        record.gpuBlasBuildSubmitted,
        record.gpuUploadSignature == RtPathTraceRigidUploadSignatureFromPod(record),
        record.gpuBlasVertexCount,
        record.cachedVertexCount,
        record.gpuBlasIndexCount,
        record.cachedIndexCount);
}

inline bool IsRigidRouteReadyFromPod(
    const RtSmokeGeometryUniverseSnapshot& snapshot, std::uint64_t meshHash)
{
    if (!snapshot.complete || !RtPathTracePlanningEpochValid(snapshot.epoch))
    {
        return false;
    }
    const auto first = std::lower_bound(snapshot.rigidRoutes.begin(),
        snapshot.rigidRoutes.end(), meshHash,
        [](const RtSmokeRigidRouteReadyPod& row, std::uint64_t value)
        {
            return row.meshHash < value;
        });
    if (first == snapshot.rigidRoutes.end() || first->meshHash != meshHash ||
        (std::next(first) != snapshot.rigidRoutes.end() &&
            std::next(first)->meshHash == meshHash))
    {
        return false;
    }
    return RtPathTraceRigidRouteReadyRecordFromPod(*first);
}

inline bool IsRigidRouteReadyFromPod(
    const RtSmokeGeometryUniverseSnapshot& snapshot,
    const RtSmokeRigidRouteRawKeyPod& rawKey)
{
    if (!snapshot.complete || !RtPathTracePlanningEpochValid(snapshot.epoch))
    {
        return false;
    }
    bool foundReady = false;
    for (const RtSmokeRigidRouteReadyPod& record : snapshot.rigidRoutes)
    {
        if (record.vertexBufferIdentity == rawKey.vertexBufferIdentity &&
            record.indexBufferIdentity == rawKey.indexBufferIdentity &&
            record.sourceRange.vertices.count == rawKey.vertexCount &&
            record.sourceRange.indexes.count == rawKey.indexCount &&
            record.sourceRange.triangles.count == rawKey.triangleCount &&
            record.materialId == rawKey.materialId)
        {
            foundReady = foundReady ||
                RtPathTraceRigidRouteReadyRecordFromPod(record);
        }
    }
    return foundReady;
}

inline bool IsRigidRouteResidentReadyFromPod(
    const RtSmokeGeometryUniverseSnapshot& snapshot,
    int entityIndex, int renderEntityNum, std::uint32_t materialId)
{
    return snapshot.complete && RtPathTracePlanningEpochValid(snapshot.epoch) &&
        RtPathTracePlanningRigidResidentReady(
            snapshot.rigidResidents, entityIndex, renderEntityNum, materialId,
            [&snapshot](std::uint64_t meshHash)
            {
                return IsRigidRouteReadyFromPod(snapshot, meshHash);
            });
}

template<typename Snapshot, typename Builder>
inline bool RtPathTraceBuildPlanningSnapshotTransaction(
    Snapshot& output, std::size_t& captureProductSlotBytes, Builder&& builder)
{
    Snapshot candidate;
    bool built = false;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try
    {
        built = builder(candidate);
    }
    catch (const std::bad_alloc&)
    {
        built = false;
    }
    catch (const std::length_error&)
    {
        built = false;
    }
#else
    built = builder(candidate);
#endif
    std::size_t committedBytes = captureProductSlotBytes;
    if (!built || !candidate.complete ||
        !RtPathTracePlanningAccumulateBytes(candidate.OwnedBytes(), committedBytes))
    {
        candidate.ResetAndRelease();
        output.ResetAndRelease();
        return false;
    }
    output = std::move(candidate);
    captureProductSlotBytes = committedBytes;
    return true;
}
