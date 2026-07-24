#include "PathTraceGeometryGpuPools.h"

#include <algorithm>

namespace {

constexpr std::uint64_t kMinimumPoolGrowthBytes = 64ull * 1024ull;
constexpr std::uint64_t kMaximumPoolCapacityBytes = 1024ull * 1024ull * 1024ull;
constexpr std::uint64_t kRetireFrames = 3;

enum PoolIndex
{
    PositionPool = 0,
    AttributePool,
    IndexPool,
    TrianglePool,
    PoolCount
};

std::uint64_t StreamCount(
    const PtGeometrySourceRecord& source,
    int poolIndex)
{
    switch (poolIndex)
    {
        case PositionPool: return source.payload.positions.size();
        case AttributePool: return source.payload.attributes.size();
        case IndexPool: return source.payload.indexes.size();
        case TrianglePool: return source.payload.triangles.size();
        default: return 0;
    }
}

std::uint64_t StreamStride(int poolIndex)
{
    switch (poolIndex)
    {
        case PositionPool: return sizeof(PtGeometrySourcePosition);
        case AttributePool: return sizeof(PtGeometrySourceAttribute);
        case IndexPool: return sizeof(std::uint32_t);
        case TrianglePool: return sizeof(PtGeometrySourceTriangle);
        default: return 0;
    }
}

const void* StreamData(
    const PtGeometrySourceRecord& source,
    int poolIndex)
{
    switch (poolIndex)
    {
        case PositionPool: return source.payload.positions.data();
        case AttributePool: return source.payload.attributes.data();
        case IndexPool: return source.payload.indexes.data();
        case TrianglePool: return source.payload.triangles.data();
        default: return nullptr;
    }
}

PtGeometryPoolRange& RecordRange(
    PtGeometryGpuPoolRecord& record,
    int poolIndex)
{
    switch (poolIndex)
    {
        case PositionPool: return record.positions;
        case AttributePool: return record.attributes;
        case IndexPool: return record.indexes;
        default: return record.triangles;
    }
}

}

void PtGeometryGpuPoolSet::ReleaseExpired(
    Pool& pool,
    std::uint64_t frameIndex)
{
    pool.retired.erase(
        std::remove_if(
            pool.retired.begin(),
            pool.retired.end(),
            [frameIndex](const RetiredBuffer& retired) {
                return retired.releaseAfterFrame <= frameIndex;
            }),
        pool.retired.end());
}

void PtGeometryGpuPoolSet::RebaseRecordRanges(
    int poolIndex,
    const PtGeometryPoolGrowthPlan& growth)
{
    for (PtGeometryGpuPoolRecord& record : records_)
    {
        PtGeometryPoolRange rebased;
        PtGeometryPoolRange& range = RecordRange(record, poolIndex);
        if (range.sizeBytes != 0 &&
            PtRebaseGeometryPoolRange(range, growth, rebased) ==
                PtGeometryPoolPlanResult::Success)
        {
            range = rebased;
        }
    }
}

bool PtGeometryGpuPoolSet::EnsureCapacity(
    nvrhi::IDevice* device,
    nvrhi::ICommandList* commandList,
    Pool& pool,
    std::uint64_t requiredUsedBytes,
    std::uint64_t stride,
    const char* debugName,
    bool vertexBuffer,
    bool indexBuffer,
    int poolIndex,
    std::uint64_t frameIndex,
    PtGeometryGpuPoolStats& stats)
{
    if (requiredUsedBytes <= pool.state.capacityBytes)
    {
        return pool.buffer != nullptr || requiredUsedBytes == 0;
    }

    PtGeometryPoolGrowthPlan growth;
    if (PtPlanGeometryPoolGrowth(
            pool.state,
            requiredUsedBytes,
            kMinimumPoolGrowthBytes,
            kMaximumPoolCapacityBytes,
            growth) != PtGeometryPoolPlanResult::Success ||
        !growth.grew)
    {
        return false;
    }

    nvrhi::BufferDesc desc;
    desc.byteSize = static_cast<size_t>(growth.nextCapacityBytes);
    desc.debugName = debugName;
    desc.structStride = static_cast<std::uint32_t>(stride);
    desc.isVertexBuffer = vertexBuffer;
    desc.isIndexBuffer = indexBuffer;
    desc.isAccelStructBuildInput = vertexBuffer || indexBuffer;
    desc.initialState = nvrhi::ResourceStates::Common;
    desc.keepInitialState = true;
    nvrhi::BufferHandle replacement = device->createBuffer(desc);
    if (!replacement)
    {
        return false;
    }

    if (pool.buffer && growth.copyBytes != 0)
    {
        commandList->copyBuffer(
            replacement,
            0,
            pool.buffer,
            0,
            growth.copyBytes);
        RetiredBuffer retired;
        retired.buffer = pool.buffer;
        retired.releaseAfterFrame = frameIndex + kRetireFrames;
        pool.retired.push_back(retired);
        stats.copiedGrowthBytes += growth.copyBytes;
        ++stats.buffersGrown;
    }
    else
    {
        ++stats.buffersCreated;
    }

    pool.buffer = replacement;
    pool.state.capacityBytes = growth.nextCapacityBytes;
    pool.state.storageGeneration = growth.nextStorageGeneration;
    RebaseRecordRanges(poolIndex, growth);
    return true;
}

PtGeometryGpuPoolStats PtGeometryGpuPoolSet::Update(
    nvrhi::IDevice* device,
    nvrhi::ICommandList* commandList,
    const PtGeometrySourceRegistry& sources,
    std::uint64_t frameIndex)
{
    PtGeometryGpuPoolStats stats;
    if (device == nullptr || commandList == nullptr)
    {
        stats.rejectedRecords = sources.RecordCount();
        return stats;
    }
    for (Pool& pool : pools_)
    {
        ReleaseExpired(pool, frameIndex);
    }

    std::vector<std::size_t> pending;
    for (std::size_t index = 0; index < sources.RecordCount(); ++index)
    {
        const PtGeometrySourceRecord* source = sources.RecordAt(index);
        if (source == nullptr)
        {
            ++stats.rejectedRecords;
            continue;
        }
        if (index >= records_.size() ||
            records_[index].key != source->key ||
            records_[index].sourceContentRevision !=
                source->sourceContentRevision ||
            records_[index].sourceChecksum != source->sourceChecksum)
        {
            pending.push_back(index);
        }
    }

    std::uint64_t requiredUsed[PoolCount] = {};
    for (int poolIndex = 0; poolIndex < PoolCount; ++poolIndex)
    {
        PtGeometryPoolState trial = pools_[poolIndex].state;
        trial.capacityBytes = kMaximumPoolCapacityBytes;
        for (std::size_t sourceIndex : pending)
        {
            const PtGeometrySourceRecord* source =
                sources.RecordAt(sourceIndex);
            std::uint64_t bytes = 0;
            PtGeometryPoolRange ignored;
            if (source == nullptr ||
                PtGeometryPoolByteSizeForElements(
                    StreamCount(*source, poolIndex),
                    StreamStride(poolIndex),
                    bytes) != PtGeometryPoolPlanResult::Success ||
                PtAllocateGeometryPoolRange(
                    trial,
                    bytes,
                    StreamStride(poolIndex),
                    ignored) != PtGeometryPoolPlanResult::Success)
            {
                ++stats.rejectedRecords;
                return stats;
            }
        }
        requiredUsed[poolIndex] = trial.usedBytes;
    }

    const char* names[PoolCount] = {
        "PathTraceCanonicalPositionPool",
        "PathTraceCanonicalAttributePool",
        "PathTraceCanonicalIndexPool",
        "PathTraceCanonicalTrianglePool"
    };
    for (int poolIndex = 0; poolIndex < PoolCount; ++poolIndex)
    {
        if (!EnsureCapacity(
                device,
                commandList,
                pools_[poolIndex],
                requiredUsed[poolIndex],
                StreamStride(poolIndex),
                names[poolIndex],
                poolIndex == PositionPool,
                poolIndex == IndexPool,
                poolIndex,
                frameIndex,
                stats))
        {
            stats.rejectedRecords += pending.size();
            return stats;
        }
    }

    for (std::size_t sourceIndex : pending)
    {
        const PtGeometrySourceRecord* source = sources.RecordAt(sourceIndex);
        if (source == nullptr || sourceIndex > records_.size())
        {
            ++stats.rejectedRecords;
            continue;
        }
        PtGeometryGpuPoolRecord record;
        record.key = source->key;
        record.sourceContentRevision = source->sourceContentRevision;
        record.sourceChecksum = source->sourceChecksum;
        bool valid = true;
        for (int poolIndex = 0; poolIndex < PoolCount; ++poolIndex)
        {
            std::uint64_t bytes = 0;
            PtGeometryPoolRange& range = RecordRange(record, poolIndex);
            if (PtGeometryPoolByteSizeForElements(
                    StreamCount(*source, poolIndex),
                    StreamStride(poolIndex),
                    bytes) != PtGeometryPoolPlanResult::Success ||
                PtAllocateGeometryPoolRange(
                    pools_[poolIndex].state,
                    bytes,
                    StreamStride(poolIndex),
                    range) != PtGeometryPoolPlanResult::Success)
            {
                valid = false;
                break;
            }
            commandList->writeBuffer(
                pools_[poolIndex].buffer,
                StreamData(*source, poolIndex),
                static_cast<size_t>(bytes),
                range.offsetBytes);
            stats.uploadBytes += bytes;
        }
        if (!valid)
        {
            ++stats.rejectedRecords;
            continue;
        }
        if (sourceIndex < records_.size())
        {
            records_[sourceIndex] = record;
            ++stats.revisedRecords;
        }
        else
        {
            records_.push_back(record);
            ++stats.uploadedRecords;
        }
    }

    stats.residentRecords = records_.size();
    for (int poolIndex = 0; poolIndex < PoolCount; ++poolIndex)
    {
        stats.capacities[poolIndex] =
            pools_[poolIndex].state.capacityBytes;
        stats.used[poolIndex] = pools_[poolIndex].state.usedBytes;
        stats.generations[poolIndex] =
            pools_[poolIndex].state.storageGeneration;
        stats.retiredBuffers += pools_[poolIndex].retired.size();
    }
    return stats;
}

const PtGeometryGpuPoolRecord* PtGeometryGpuPoolSet::RecordAt(
    std::size_t index) const
{
    return index < records_.size() ? &records_[index] : nullptr;
}

std::size_t PtGeometryGpuPoolSet::RecordCount() const
{
    return records_.size();
}

nvrhi::BufferHandle PtGeometryGpuPoolSet::PositionBuffer() const
{
    return pools_[PositionPool].buffer;
}

nvrhi::BufferHandle PtGeometryGpuPoolSet::AttributeBuffer() const
{
    return pools_[AttributePool].buffer;
}

nvrhi::BufferHandle PtGeometryGpuPoolSet::IndexBuffer() const
{
    return pools_[IndexPool].buffer;
}

nvrhi::BufferHandle PtGeometryGpuPoolSet::TriangleBuffer() const
{
    return pools_[TrianglePool].buffer;
}

void PtGeometryGpuPoolSet::ResetForPublication(std::uint64_t frameIndex)
{
    records_.clear();
    for (Pool& pool : pools_)
    {
        if (pool.buffer)
        {
            RetiredBuffer retired;
            retired.buffer = pool.buffer;
            retired.releaseAfterFrame = frameIndex + kRetireFrames;
            pool.retired.push_back(retired);
        }
        pool.buffer = nullptr;
        pool.state = PtGeometryPoolState();
    }
}

void PtGeometryGpuPoolSet::Clear()
{
    records_.clear();
    for (Pool& pool : pools_)
    {
        pool = Pool();
    }
}
