#pragma once

#include "PathTraceGeometryPoolPlan.h"
#include "PathTraceGeometrySourceRegistry.h"

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <vector>

struct PtGeometryGpuPoolRecord
{
    PtCanonicalMeshKey key;
    std::uint64_t sourceContentRevision = 0;
    std::uint64_t sourceChecksum = 0;
    PtGeometryPoolRange positions;
    PtGeometryPoolRange attributes;
    PtGeometryPoolRange indexes;
    PtGeometryPoolRange triangles;
};

struct PtGeometryGpuPoolStats
{
    std::uint64_t residentRecords = 0;
    std::uint64_t uploadedRecords = 0;
    std::uint64_t revisedRecords = 0;
    std::uint64_t rejectedRecords = 0;
    std::uint64_t uploadBytes = 0;
    std::uint64_t copiedGrowthBytes = 0;
    std::uint64_t buffersCreated = 0;
    std::uint64_t buffersGrown = 0;
    std::uint64_t retiredBuffers = 0;
    std::uint64_t capacities[4] = {};
    std::uint64_t used[4] = {};
    std::uint64_t generations[4] = {};
};

class PtGeometryGpuPoolSet
{
public:
    PtGeometryGpuPoolStats Update(
        nvrhi::IDevice* device,
        nvrhi::ICommandList* commandList,
        const PtGeometrySourceRegistry& sources,
        std::uint64_t frameIndex);
    const PtGeometryGpuPoolRecord* RecordAt(std::size_t index) const;
    std::size_t RecordCount() const;
    nvrhi::BufferHandle PositionBuffer() const;
    nvrhi::BufferHandle AttributeBuffer() const;
    nvrhi::BufferHandle IndexBuffer() const;
    nvrhi::BufferHandle TriangleBuffer() const;
    void ResetForPublication(std::uint64_t frameIndex);
    void Clear();

private:
    struct RetiredBuffer
    {
        nvrhi::BufferHandle buffer;
        std::uint64_t releaseAfterFrame = 0;
    };

    struct Pool
    {
        nvrhi::BufferHandle buffer;
        PtGeometryPoolState state;
        std::vector<RetiredBuffer> retired;
    };

    bool EnsureCapacity(
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
        PtGeometryGpuPoolStats& stats);
    void ReleaseExpired(Pool& pool, std::uint64_t frameIndex);
    void RebaseRecordRanges(
        int poolIndex,
        const PtGeometryPoolGrowthPlan& growth);

    Pool pools_[4];
    std::vector<PtGeometryGpuPoolRecord> records_;
};
