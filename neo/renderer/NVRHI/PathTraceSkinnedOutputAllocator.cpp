#include "PathTraceSkinnedOutputAllocator.h"

#include <algorithm>
#include <limits>

namespace {

bool CheckedAdd(
    std::uint64_t lhs,
    std::uint64_t rhs,
    std::uint64_t& result)
{
    if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs)
    {
        return false;
    }
    result = lhs + rhs;
    return true;
}

bool CheckedMultiply(
    std::uint64_t lhs,
    std::uint64_t rhs,
    std::uint64_t& result)
{
    if (lhs != 0 &&
        rhs > std::numeric_limits<std::uint64_t>::max() / lhs)
    {
        return false;
    }
    result = lhs * rhs;
    return true;
}

std::uint64_t NextNonZero(std::uint64_t value)
{
    return value == 0 ? 1 : value;
}

} // namespace

PtSkinnedOutputAllocator::PtSkinnedOutputAllocator(
    const PtSkinnedOutputAllocatorConfig& config)
    : m_config(config)
{
}

bool PtSkinnedOutputAllocator::Configure(
    const PtSkinnedOutputAllocatorConfig& config)
{
    if (m_stats.worldGeneration != 0 ||
        !m_ranges.empty() ||
        !m_retiredStorage.empty())
    {
        return false;
    }
    m_config = config;
    return true;
}

void PtSkinnedOutputAllocator::Clear()
{
    m_ranges.clear();
    m_lookup.clear();
    m_retiredStorage.clear();
    m_stats = PtSkinnedOutputAllocatorStats();
    m_nextStorageGeneration = 1;
}

bool PtSkinnedOutputAllocator::BeginWorld(
    std::uint64_t worldGeneration,
    std::uint64_t frameIndex)
{
    if (worldGeneration == 0)
    {
        return false;
    }
    if (m_stats.worldGeneration == worldGeneration)
    {
        return true;
    }

    if (m_stats.worldGeneration != 0)
    {
        RetireCurrentStorage(frameIndex);
        ++m_stats.worldResets;
    }
    m_ranges.clear();
    m_lookup.clear();
    m_stats.worldGeneration = worldGeneration;
    m_stats.storageGeneration = 0;
    m_stats.capacityVertices = 0;
    m_stats.usedVertices = 0;
    m_stats.activeVertices = 0;
    m_stats.activeRanges = 0;
    m_stats.rangeSlots = 0;
    m_stats.pendingStorageGenerations = m_retiredStorage.size();
    return true;
}

PtSkinnedOutputObserveResult PtSkinnedOutputAllocator::Observe(
    const PtCanonicalInstanceKey& instanceKey,
    std::uint64_t vertexCount,
    std::uint64_t frameIndex)
{
    if (m_stats.worldGeneration == 0 ||
        instanceKey.worldGeneration != m_stats.worldGeneration)
    {
        ++m_stats.rejected;
        return PtSkinnedOutputObserveResult::InvalidWorld;
    }
    if (!PtCanonicalInstanceKeyIsValid(instanceKey) ||
        instanceKey.subInstanceKind !=
            PtCanonicalSubInstanceKind::SkinnedSurface)
    {
        ++m_stats.rejected;
        return PtSkinnedOutputObserveResult::InvalidInstanceKey;
    }
    if (vertexCount == 0 || vertexCount > UINT32_MAX)
    {
        ++m_stats.rejected;
        return PtSkinnedOutputObserveResult::InvalidVertexCount;
    }
    if (m_config.vertexStrideBytes == 0)
    {
        ++m_stats.rejected;
        return PtSkinnedOutputObserveResult::InvalidVertexStride;
    }

    const std::uint64_t instanceHash =
        PtHashCanonicalInstanceKey(instanceKey);
    PtSkinnedOutputRange* existing =
        FindMutable(instanceKey, instanceHash);
    if (existing != nullptr && !existing->active)
    {
        ++m_stats.rejected;
        return PtSkinnedOutputObserveResult::RetiredInstanceKey;
    }
    if (existing != nullptr &&
        existing->vertexCount == vertexCount)
    {
        existing->lastSeenFrame = frameIndex;
        ++m_stats.reused;
        return PtSkinnedOutputObserveResult::Reused;
    }

    std::uint64_t requiredVertices = 0;
    std::uint64_t byteOffset = 0;
    std::uint64_t byteCount = 0;
    std::uint64_t byteEnd = 0;
    if (!CheckedAdd(
            m_stats.usedVertices,
            vertexCount,
            requiredVertices) ||
        !CheckedMultiply(
            m_stats.usedVertices,
            m_config.vertexStrideBytes,
            byteOffset) ||
        !CheckedMultiply(
            vertexCount,
            m_config.vertexStrideBytes,
            byteCount) ||
        !CheckedAdd(byteOffset, byteCount, byteEnd))
    {
        ++m_stats.rejected;
        return PtSkinnedOutputObserveResult::ArithmeticOverflow;
    }
    if (!EnsureCapacity(requiredVertices, frameIndex))
    {
        ++m_stats.rejected;
        return PtSkinnedOutputObserveResult::CapacityExceeded;
    }

    if (existing != nullptr)
    {
        m_stats.activeVertices -= existing->vertexCount;
        existing->vertexOffset = m_stats.usedVertices;
        existing->vertexCount = vertexCount;
        existing->byteOffset = byteOffset;
        existing->byteCount = byteCount;
        existing->storageGeneration =
            m_stats.storageGeneration;
        existing->lastSeenFrame = frameIndex;
        existing->allocationRevision =
            NextNonZero(existing->allocationRevision + 1);
        m_stats.usedVertices = requiredVertices;
        m_stats.activeVertices += vertexCount;
        ++m_stats.resized;
        return PtSkinnedOutputObserveResult::Resized;
    }

    PtSkinnedOutputRange added;
    added.active = true;
    added.instanceKey = instanceKey;
    added.instanceHash = instanceHash;
    added.vertexOffset = m_stats.usedVertices;
    added.vertexCount = vertexCount;
    added.byteOffset = byteOffset;
    added.byteCount = byteCount;
    added.allocationRevision = 1;
    added.storageGeneration = m_stats.storageGeneration;
    added.firstSeenFrame = frameIndex;
    added.lastSeenFrame = frameIndex;
    const std::size_t index = m_ranges.size();
    m_ranges.push_back(added);
    m_lookup.emplace(instanceHash, index);
    m_stats.usedVertices = requiredVertices;
    m_stats.activeVertices += vertexCount;
    ++m_stats.activeRanges;
    m_stats.rangeSlots = m_ranges.size();
    ++m_stats.added;
    return PtSkinnedOutputObserveResult::Added;
}

PtSkinnedOutputRetireResult PtSkinnedOutputAllocator::Retire(
    const PtCanonicalInstanceKey& instanceKey,
    std::uint64_t frameIndex)
{
    if (!PtCanonicalInstanceKeyIsValid(instanceKey))
    {
        return PtSkinnedOutputRetireResult::InvalidInstanceKey;
    }
    PtSkinnedOutputRange* range =
        FindMutable(
            instanceKey,
            PtHashCanonicalInstanceKey(instanceKey));
    if (range == nullptr)
    {
        return PtSkinnedOutputRetireResult::NotFound;
    }
    if (!range->active)
    {
        return PtSkinnedOutputRetireResult::AlreadyRetired;
    }

    range->active = false;
    range->lastSeenFrame = frameIndex;
    m_stats.activeVertices -= range->vertexCount;
    --m_stats.activeRanges;
    ++m_stats.retired;
    return PtSkinnedOutputRetireResult::Retired;
}

const PtSkinnedOutputRange* PtSkinnedOutputAllocator::Find(
    const PtCanonicalInstanceKey& instanceKey) const
{
    const std::uint64_t hash =
        PtHashCanonicalInstanceKey(instanceKey);
    const auto range = m_lookup.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it)
    {
        if (it->second < m_ranges.size() &&
            m_ranges[it->second].active &&
            m_ranges[it->second].instanceKey == instanceKey)
        {
            return &m_ranges[it->second];
        }
    }
    return nullptr;
}

std::size_t
PtSkinnedOutputAllocator::ReleaseStorageGenerationsThrough(
    std::uint64_t completedStorageGeneration)
{
    const std::size_t before = m_retiredStorage.size();
    m_retiredStorage.erase(
        std::remove_if(
            m_retiredStorage.begin(),
            m_retiredStorage.end(),
            [completedStorageGeneration](
                const PtSkinnedOutputRetiredStorage& retired)
            {
                return retired.storageGeneration <=
                    completedStorageGeneration;
            }),
        m_retiredStorage.end());
    const std::size_t released =
        before - m_retiredStorage.size();
    m_stats.storageReleased += released;
    m_stats.pendingStorageGenerations =
        m_retiredStorage.size();
    return released;
}

const std::vector<PtSkinnedOutputRetiredStorage>&
PtSkinnedOutputAllocator::PendingStorageGenerations() const
{
    return m_retiredStorage;
}

const PtSkinnedOutputAllocatorStats&
PtSkinnedOutputAllocator::Stats() const
{
    return m_stats;
}

void PtSkinnedOutputAllocator::ResetIntervalStats()
{
    m_stats.added = 0;
    m_stats.reused = 0;
    m_stats.resized = 0;
    m_stats.retired = 0;
    m_stats.rejected = 0;
    m_stats.storageCreated = 0;
    m_stats.storageGrown = 0;
    m_stats.storageReleased = 0;
    m_stats.worldResets = 0;
}

PtSkinnedOutputRange* PtSkinnedOutputAllocator::FindMutable(
    const PtCanonicalInstanceKey& instanceKey,
    std::uint64_t instanceHash)
{
    const auto range = m_lookup.equal_range(instanceHash);
    for (auto it = range.first; it != range.second; ++it)
    {
        if (it->second < m_ranges.size() &&
            m_ranges[it->second].instanceKey == instanceKey)
        {
            return &m_ranges[it->second];
        }
    }
    return nullptr;
}

bool PtSkinnedOutputAllocator::EnsureCapacity(
    std::uint64_t requiredVertices,
    std::uint64_t frameIndex)
{
    if (requiredVertices <= m_stats.capacityVertices)
    {
        return true;
    }
    if (requiredVertices > m_config.maxCapacityVertices)
    {
        return false;
    }

    std::uint64_t capacity =
        m_stats.capacityVertices != 0
            ? m_stats.capacityVertices
            : m_config.initialCapacityVertices;
    capacity = std::max<std::uint64_t>(capacity, 1);
    capacity = std::min(
        capacity,
        m_config.maxCapacityVertices);
    while (capacity < requiredVertices)
    {
        if (capacity >
            m_config.maxCapacityVertices - capacity)
        {
            capacity = m_config.maxCapacityVertices;
        }
        else
        {
            capacity *= 2;
        }
        if (capacity < requiredVertices &&
            capacity == m_config.maxCapacityVertices)
        {
            return false;
        }
    }

    ActivateStorage(
        capacity,
        frameIndex,
        m_stats.storageGeneration != 0);
    return true;
}

void PtSkinnedOutputAllocator::RetireCurrentStorage(
    std::uint64_t frameIndex)
{
    if (m_stats.storageGeneration == 0 ||
        m_stats.capacityVertices == 0)
    {
        return;
    }
    PtSkinnedOutputRetiredStorage retired;
    retired.storageGeneration =
        m_stats.storageGeneration;
    retired.capacityVertices =
        m_stats.capacityVertices;
    retired.retiredFrame = frameIndex;
    m_retiredStorage.push_back(retired);
    m_stats.pendingStorageGenerations =
        m_retiredStorage.size();
}

void PtSkinnedOutputAllocator::ActivateStorage(
    std::uint64_t capacityVertices,
    std::uint64_t frameIndex,
    bool growth)
{
    if (growth)
    {
        RetireCurrentStorage(frameIndex);
    }
    std::uint64_t generation =
        m_nextStorageGeneration++;
    if (generation == 0)
    {
        generation = m_nextStorageGeneration++;
    }
    m_stats.storageGeneration = generation;
    m_stats.capacityVertices = capacityVertices;
    for (PtSkinnedOutputRange& range : m_ranges)
    {
        if (range.active)
        {
            range.storageGeneration = generation;
        }
    }
    if (growth)
    {
        ++m_stats.storageGrown;
    }
    else
    {
        ++m_stats.storageCreated;
    }
}

const char* PtSkinnedOutputObserveResultName(
    PtSkinnedOutputObserveResult result)
{
    switch (result)
    {
        case PtSkinnedOutputObserveResult::Added:
            return "added";
        case PtSkinnedOutputObserveResult::Reused:
            return "reused";
        case PtSkinnedOutputObserveResult::Resized:
            return "resized";
        case PtSkinnedOutputObserveResult::InvalidWorld:
            return "invalid-world";
        case PtSkinnedOutputObserveResult::InvalidInstanceKey:
            return "invalid-instance";
        case PtSkinnedOutputObserveResult::InvalidVertexCount:
            return "invalid-vertex-count";
        case PtSkinnedOutputObserveResult::InvalidVertexStride:
            return "invalid-vertex-stride";
        case PtSkinnedOutputObserveResult::RetiredInstanceKey:
            return "retired-instance";
        case PtSkinnedOutputObserveResult::ArithmeticOverflow:
            return "arithmetic-overflow";
        case PtSkinnedOutputObserveResult::CapacityExceeded:
            return "capacity-exceeded";
        default:
            return "unknown";
    }
}

const char* PtSkinnedOutputRetireResultName(
    PtSkinnedOutputRetireResult result)
{
    switch (result)
    {
        case PtSkinnedOutputRetireResult::Retired:
            return "retired";
        case PtSkinnedOutputRetireResult::NotFound:
            return "not-found";
        case PtSkinnedOutputRetireResult::AlreadyRetired:
            return "already-retired";
        case PtSkinnedOutputRetireResult::InvalidInstanceKey:
            return "invalid-instance";
        default:
            return "unknown";
    }
}
