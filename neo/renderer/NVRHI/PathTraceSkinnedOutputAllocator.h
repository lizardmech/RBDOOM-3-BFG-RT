#pragma once

// Behavior-neutral planning allocator for persistent GPU-skinned output
// ranges. Full canonical instance identity owns every range. Physical GPU
// buffer creation and dispatch routing remain later GEO-07 slices.

#include "PathTraceCanonicalGeometryIdentity.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

enum class PtSkinnedOutputObserveResult : std::uint32_t
{
    Added = 0,
    Reused,
    Resized,
    InvalidWorld,
    InvalidInstanceKey,
    InvalidVertexCount,
    InvalidVertexStride,
    RetiredInstanceKey,
    ArithmeticOverflow,
    CapacityExceeded
};

enum class PtSkinnedOutputRetireResult : std::uint32_t
{
    Retired = 0,
    NotFound,
    AlreadyRetired,
    InvalidInstanceKey
};

struct PtSkinnedOutputAllocatorConfig
{
    std::uint64_t initialCapacityVertices = 4096;
    std::uint64_t maxCapacityVertices = UINT32_MAX;
    std::uint64_t vertexStrideBytes = 1;
};

struct PtSkinnedOutputRange
{
    bool active = false;
    PtCanonicalInstanceKey instanceKey;
    std::uint64_t instanceHash = 0;
    std::uint64_t vertexOffset = 0;
    std::uint64_t vertexCount = 0;
    std::uint64_t byteOffset = 0;
    std::uint64_t byteCount = 0;
    std::uint64_t allocationRevision = 0;
    std::uint64_t storageGeneration = 0;
    std::uint64_t firstSeenFrame = 0;
    std::uint64_t lastSeenFrame = 0;
};

struct PtSkinnedOutputRetiredStorage
{
    std::uint64_t storageGeneration = 0;
    std::uint64_t capacityVertices = 0;
    std::uint64_t retiredFrame = 0;
};

struct PtSkinnedOutputAllocatorStats
{
    std::uint64_t worldGeneration = 0;
    std::uint64_t storageGeneration = 0;
    std::uint64_t capacityVertices = 0;
    std::uint64_t usedVertices = 0;
    std::uint64_t activeVertices = 0;
    std::uint64_t activeRanges = 0;
    std::uint64_t rangeSlots = 0;
    std::uint64_t pendingStorageGenerations = 0;

    std::uint64_t added = 0;
    std::uint64_t reused = 0;
    std::uint64_t resized = 0;
    std::uint64_t retired = 0;
    std::uint64_t rejected = 0;
    std::uint64_t storageCreated = 0;
    std::uint64_t storageGrown = 0;
    std::uint64_t storageReleased = 0;
    std::uint64_t worldResets = 0;
};

class PtSkinnedOutputAllocator
{
public:
    explicit PtSkinnedOutputAllocator(
        const PtSkinnedOutputAllocatorConfig& config =
            PtSkinnedOutputAllocatorConfig());

    bool Configure(
        const PtSkinnedOutputAllocatorConfig& config);
    void Clear();
    bool BeginWorld(
        std::uint64_t worldGeneration,
        std::uint64_t frameIndex);

    PtSkinnedOutputObserveResult Observe(
        const PtCanonicalInstanceKey& instanceKey,
        std::uint64_t vertexCount,
        std::uint64_t frameIndex);

    PtSkinnedOutputRetireResult Retire(
        const PtCanonicalInstanceKey& instanceKey,
        std::uint64_t frameIndex);

    const PtSkinnedOutputRange* Find(
        const PtCanonicalInstanceKey& instanceKey) const;

    std::size_t ReleaseStorageGenerationsThrough(
        std::uint64_t completedStorageGeneration);

    const std::vector<PtSkinnedOutputRetiredStorage>&
        PendingStorageGenerations() const;
    const PtSkinnedOutputAllocatorStats& Stats() const;
    void ResetIntervalStats();

private:
    PtSkinnedOutputRange* FindMutable(
        const PtCanonicalInstanceKey& instanceKey,
        std::uint64_t instanceHash);
    bool EnsureCapacity(
        std::uint64_t requiredVertices,
        std::uint64_t frameIndex);
    void RetireCurrentStorage(std::uint64_t frameIndex);
    void ActivateStorage(
        std::uint64_t capacityVertices,
        std::uint64_t frameIndex,
        bool growth);

    PtSkinnedOutputAllocatorConfig m_config;
    std::vector<PtSkinnedOutputRange> m_ranges;
    std::unordered_multimap<std::uint64_t, std::size_t> m_lookup;
    std::vector<PtSkinnedOutputRetiredStorage> m_retiredStorage;
    PtSkinnedOutputAllocatorStats m_stats;
    std::uint64_t m_nextStorageGeneration = 1;
};

const char* PtSkinnedOutputObserveResultName(
    PtSkinnedOutputObserveResult result);

const char* PtSkinnedOutputRetireResultName(
    PtSkinnedOutputRetireResult result);
