#include "PathTraceGeometryOffsetBlasProbe.h"

#include <nvrhi/utils.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>

namespace {

constexpr std::uint64_t kStableFramesBeforeBuild = 3;
constexpr std::uint64_t kTimingQueryPollDelayFrames = 4;
constexpr std::uint64_t kTimingIndexOffsetAlignment = 256;
constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

struct ProbeCandidate
{
    std::size_t sourceIndex = 0;
    const PtGeometrySourceRecord* source = nullptr;
    const PtGeometryGpuPoolRecord* gpu = nullptr;
    std::uint64_t signature = 0;
};

void HashBytes(std::uint64_t& hash, const void* data, std::size_t size)
{
    const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= kFnvPrime;
    }
}

template <typename T>
void HashValue(std::uint64_t& hash, const T& value)
{
    HashBytes(hash, &value, sizeof(value));
}

bool RangeMatches(
    const PtGeometryPoolRange& range,
    std::uint64_t elementCount,
    std::uint64_t elementStride)
{
    std::uint64_t expectedBytes = 0;
    return PtGeometryPoolByteSizeForElements(
               elementCount,
               elementStride,
               expectedBytes) == PtGeometryPoolPlanResult::Success &&
        range.sizeBytes == expectedBytes &&
        range.alignmentBytes == elementStride &&
        range.offsetBytes % elementStride == 0 &&
        range.storageGeneration != 0;
}

std::uint64_t CandidateSignature(
    std::size_t sourceIndex,
    const PtGeometrySourceRecord& source,
    const PtGeometryGpuPoolRecord& gpu,
    const PtGeometryGpuPoolSet& pools)
{
    std::uint64_t hash = kFnvOffsetBasis;
    HashValue(hash, sourceIndex);
    HashValue(hash, source.sourceContentRevision);
    HashValue(hash, source.sourceChecksum);
    HashValue(hash, gpu.positions);
    HashValue(hash, gpu.attributes);
    HashValue(hash, gpu.indexes);
    HashValue(hash, gpu.triangles);
    const std::uintptr_t positionBuffer =
        reinterpret_cast<std::uintptr_t>(pools.PositionBuffer().Get());
    const std::uintptr_t attributeBuffer =
        reinterpret_cast<std::uintptr_t>(pools.AttributeBuffer().Get());
    const std::uintptr_t indexBuffer =
        reinterpret_cast<std::uintptr_t>(pools.IndexBuffer().Get());
    const std::uintptr_t triangleBuffer =
        reinterpret_cast<std::uintptr_t>(pools.TriangleBuffer().Get());
    HashValue(hash, positionBuffer);
    HashValue(hash, attributeBuffer);
    HashValue(hash, indexBuffer);
    HashValue(hash, triangleBuffer);
    return hash;
}

bool FindCandidate(
    const PtGeometrySourceRegistry& sources,
    const PtGeometryGpuPoolSet& pools,
    ProbeCandidate& candidate,
    std::uint64_t& eligibleRecords)
{
    candidate = ProbeCandidate();
    eligibleRecords = 0;
    const std::size_t recordCount =
        std::min(sources.RecordCount(), pools.RecordCount());
    for (std::size_t sourceIndex = 0; sourceIndex < recordCount; ++sourceIndex)
    {
        const PtGeometrySourceRecord* source = sources.RecordAt(sourceIndex);
        const PtGeometryGpuPoolRecord* gpu = pools.RecordAt(sourceIndex);
        if (source == nullptr || gpu == nullptr ||
            gpu->key != source->key ||
            gpu->sourceContentRevision != source->sourceContentRevision ||
            gpu->sourceChecksum != source->sourceChecksum ||
            source->payload.indexes.size() < 6 ||
            source->payload.triangles.size() < 2 ||
            gpu->positions.offsetBytes == 0 ||
            gpu->attributes.offsetBytes == 0 ||
            gpu->indexes.offsetBytes == 0 ||
            gpu->triangles.offsetBytes == 0 ||
            !RangeMatches(
                gpu->positions,
                source->payload.positions.size(),
                sizeof(PtGeometrySourcePosition)) ||
            !RangeMatches(
                gpu->attributes,
                source->payload.AttributeCount(),
                sizeof(PtGeometrySourceAttribute)) ||
            !RangeMatches(
                gpu->indexes,
                source->payload.indexes.size(),
                sizeof(std::uint32_t)) ||
            !RangeMatches(
                gpu->triangles,
                source->payload.triangles.size(),
                sizeof(PtGeometrySourceTriangle)))
        {
            continue;
        }

        bool indexesValid = true;
        for (int endpoint = 0; endpoint < 6; ++endpoint)
        {
            const std::size_t indexOffset = endpoint < 3
                ? static_cast<std::size_t>(endpoint)
                : source->payload.indexes.size() - 6 +
                    static_cast<std::size_t>(endpoint);
            if (source->payload.indexes[indexOffset] >=
                source->payload.AttributeCount())
            {
                indexesValid = false;
                break;
            }
        }
        if (!indexesValid)
        {
            continue;
        }

        ++eligibleRecords;
        if (candidate.source == nullptr ||
            source->payload.triangles.size() >
                candidate.source->payload.triangles.size() ||
            (source->payload.triangles.size() ==
                    candidate.source->payload.triangles.size() &&
                source->payload.positions.size() >
                    candidate.source->payload.positions.size()))
        {
            candidate.sourceIndex = sourceIndex;
            candidate.source = source;
            candidate.gpu = gpu;
            candidate.signature =
                CandidateSignature(sourceIndex, *source, *gpu, pools);
        }
    }
    return candidate.source != nullptr;
}

}

void PtGeometryOffsetBlasProbe::RetireTimingResources()
{
    if (timingIndexBuffer_)
    {
        retiredBuffers_.push_back(timingIndexBuffer_);
    }
    if (timingZeroOffsetBlas_)
    {
        retired_.push_back(timingZeroOffsetBlas_);
    }
    if (timingNonZeroOffsetBlas_)
    {
        retired_.push_back(timingNonZeroOffsetBlas_);
    }
    timingIndexBuffer_ = nullptr;
    timingZeroOffsetBlas_ = nullptr;
    timingNonZeroOffsetBlas_ = nullptr;
    timingZeroOffsetBlasDesc_ = nvrhi::rt::AccelStructDesc();
    timingNonZeroOffsetBlasDesc_ = nvrhi::rt::AccelStructDesc();
    timingRunActive_ = false;
    timingCandidateSignature_ = 0;
    timingTargetPairs_ = 0;
    timingSubmittedPairs_ = 0;
    timingCompletedQueries_ = 0;
    timingIndexOffsetBytes_ = 0;
    timingVertexOffsetBytes_ = 0;
    timingVertexCount_ = 0;
    timingIndexCount_ = 0;
    timingPrimitiveCount_ = 0;
    timingSamples_.clear();
}

void PtGeometryOffsetBlasProbe::PollTimingQueries(
    nvrhi::IDevice* device,
    std::uint64_t frameIndex)
{
    if (device == nullptr)
    {
        return;
    }
    for (TimingQuerySlot& slot : timingQuerySlots_)
    {
        if (!slot.pending ||
            !slot.query ||
            frameIndex < slot.earliestPollFrame ||
            !device->pollTimerQuery(slot.query))
        {
            continue;
        }

        const double microseconds =
            static_cast<double>(
                device->getTimerQueryTime(slot.query)) *
            1000000.0;
        if (timingRunActive_ &&
            slot.runSerial == timingRunSerial_ &&
            slot.pairIndex < timingSamples_.size())
        {
            PtGeometryOffsetBlasTimingSample& sample =
                timingSamples_[
                    static_cast<std::size_t>(slot.pairIndex)];
            if (slot.nonZeroOffset)
            {
                sample.nonZeroOffsetMicroseconds = microseconds;
            }
            else
            {
                sample.zeroOffsetMicroseconds = microseconds;
            }
            ++timingCompletedQueries_;
        }
        slot.pending = false;
    }
}

bool PtGeometryOffsetBlasProbe::BeginTimingRun(
    nvrhi::IDevice* device,
    nvrhi::ICommandList* commandList,
    const PtGeometrySourceRecord& source,
    const PtGeometryGpuPoolRecord& gpu,
    const PtGeometryGpuPoolSet& pools,
    std::uint64_t candidateSignature)
{
    if (device == nullptr ||
        commandList == nullptr ||
        pendingTimingPairRequest_ <= 0)
    {
        return false;
    }

    std::uint64_t indexBytes = 0;
    if (PtGeometryPoolByteSizeForElements(
            source.payload.indexes.size(),
            sizeof(std::uint32_t),
            indexBytes) != PtGeometryPoolPlanResult::Success ||
        indexBytes == 0 ||
        indexBytes >
            std::numeric_limits<std::uint64_t>::max() -
                (kTimingIndexOffsetAlignment - 1))
    {
        ++timingResourceCreateFailures_;
        pendingTimingPairRequest_ = 0;
        return false;
    }
    const std::uint64_t alignedIndexBytes =
        (indexBytes + kTimingIndexOffsetAlignment - 1) &
        ~(kTimingIndexOffsetAlignment - 1);
    if (alignedIndexBytes >
            std::numeric_limits<std::uint64_t>::max() -
                indexBytes ||
        alignedIndexBytes + indexBytes >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max()))
    {
        ++timingResourceCreateFailures_;
        pendingTimingPairRequest_ = 0;
        return false;
    }

    ++timingRunSerial_;
    timingRunActive_ = true;
    timingCandidateSignature_ = candidateSignature;
    timingTargetPairs_ =
        static_cast<std::uint64_t>(pendingTimingPairRequest_);
    pendingTimingPairRequest_ = 0;
    timingSubmittedPairs_ = 0;
    timingCompletedQueries_ = 0;
    timingTimerCreateFailures_ = 0;
    timingResourceCreateFailures_ = 0;
    timingIndexOffsetBytes_ = alignedIndexBytes;
    timingVertexOffsetBytes_ = gpu.positions.offsetBytes;
    timingVertexCount_ = source.payload.positions.size();
    timingIndexCount_ = source.payload.indexes.size();
    timingPrimitiveCount_ = source.payload.triangles.size();
    timingSamples_.assign(
        static_cast<std::size_t>(timingTargetPairs_),
        PtGeometryOffsetBlasTimingSample());
    for (std::size_t pairIndex = 0;
        pairIndex < timingSamples_.size();
        ++pairIndex)
    {
        timingSamples_[pairIndex].zeroOffsetBuiltFirst =
            (pairIndex & 1u) == 0u;
    }

    nvrhi::BufferDesc indexDesc;
    indexDesc.byteSize = static_cast<std::size_t>(
        alignedIndexBytes + indexBytes);
    indexDesc.debugName =
        "PathTraceOffsetBlasTimingIndexBuffer";
    indexDesc.structStride = sizeof(std::uint32_t);
    indexDesc.isIndexBuffer = true;
    indexDesc.isAccelStructBuildInput = true;
    indexDesc.initialState = nvrhi::ResourceStates::Common;
    indexDesc.keepInitialState = true;
    timingIndexBuffer_ = device->createBuffer(indexDesc);
    if (!timingIndexBuffer_)
    {
        ++timingResourceCreateFailures_;
        timingTargetPairs_ = 0;
        FinishTimingRunIfReady();
        return false;
    }

    commandList->beginTrackingBufferState(
        timingIndexBuffer_,
        nvrhi::ResourceStates::Common);
    commandList->writeBuffer(
        timingIndexBuffer_,
        source.payload.indexes.data(),
        static_cast<std::size_t>(indexBytes),
        0);
    commandList->writeBuffer(
        timingIndexBuffer_,
        source.payload.indexes.data(),
        static_cast<std::size_t>(indexBytes),
        timingIndexOffsetBytes_);
    commandList->setBufferState(
        timingIndexBuffer_,
        nvrhi::ResourceStates::AccelStructBuildInput);
    commandList->setBufferState(
        pools.PositionBuffer(),
        nvrhi::ResourceStates::AccelStructBuildInput);
    commandList->commitBarriers();

    auto buildDesc =
        [&](std::uint64_t indexOffset, const char* debugName)
        {
            nvrhi::rt::GeometryTriangles triangles;
            triangles.vertexBuffer = pools.PositionBuffer();
            triangles.indexBuffer = timingIndexBuffer_;
            triangles.vertexFormat = nvrhi::Format::RGB32_FLOAT;
            triangles.indexFormat = nvrhi::Format::R32_UINT;
            triangles.vertexOffset = gpu.positions.offsetBytes;
            triangles.indexOffset = indexOffset;
            triangles.vertexCount = static_cast<std::uint32_t>(
                source.payload.positions.size());
            triangles.indexCount = static_cast<std::uint32_t>(
                source.payload.indexes.size());
            triangles.vertexStride =
                sizeof(PtGeometrySourcePosition);

            nvrhi::rt::GeometryDesc geometry;
            geometry.setTriangles(triangles);
            return nvrhi::rt::AccelStructDesc()
                .addBottomLevelGeometry(geometry)
                .setBuildFlags(
                    nvrhi::rt::AccelStructBuildFlags::
                        PreferFastTrace)
                .setDebugName(debugName);
        };

    timingZeroOffsetBlasDesc_ =
        buildDesc(
            0,
            "PathTraceOffsetBlasTimingZero");
    timingNonZeroOffsetBlasDesc_ =
        buildDesc(
            timingIndexOffsetBytes_,
            "PathTraceOffsetBlasTimingNonZero");
    timingZeroOffsetBlas_ =
        device->createAccelStruct(timingZeroOffsetBlasDesc_);
    timingNonZeroOffsetBlas_ =
        device->createAccelStruct(timingNonZeroOffsetBlasDesc_);
    if (!timingZeroOffsetBlas_ ||
        !timingNonZeroOffsetBlas_)
    {
        ++timingResourceCreateFailures_;
        timingTargetPairs_ = 0;
        FinishTimingRunIfReady();
        return false;
    }
    return true;
}

void PtGeometryOffsetBlasProbe::SubmitTimingPair(
    nvrhi::IDevice* device,
    nvrhi::ICommandList* commandList,
    std::uint64_t frameIndex)
{
    if (!timingRunActive_ ||
        device == nullptr ||
        commandList == nullptr ||
        timingSubmittedPairs_ >= timingTargetPairs_)
    {
        return;
    }

    TimingQuerySlot* available[2] = {};
    int availableCount = 0;
    for (TimingQuerySlot& slot : timingQuerySlots_)
    {
        if (slot.pending)
        {
            continue;
        }
        if (!slot.query)
        {
            slot.query = device->createTimerQuery();
            if (!slot.query)
            {
                ++timingTimerCreateFailures_;
                timingTargetPairs_ = timingSubmittedPairs_;
                FinishTimingRunIfReady();
                return;
            }
        }
        available[availableCount++] = &slot;
        if (availableCount == 2)
        {
            break;
        }
    }
    if (availableCount != 2)
    {
        return;
    }

    const std::uint64_t pairIndex = timingSubmittedPairs_;
    const bool zeroFirst =
        timingSamples_[static_cast<std::size_t>(pairIndex)].
            zeroOffsetBuiltFirst;
    auto submit =
        [&](TimingQuerySlot& slot, bool nonZeroOffset)
        {
            slot.pending = true;
            slot.nonZeroOffset = nonZeroOffset;
            slot.runSerial = timingRunSerial_;
            slot.pairIndex = pairIndex;
            slot.earliestPollFrame =
                frameIndex + kTimingQueryPollDelayFrames;
            commandList->beginTimerQuery(slot.query);
            nvrhi::utils::BuildBottomLevelAccelStruct(
                commandList,
                nonZeroOffset
                    ? timingNonZeroOffsetBlas_
                    : timingZeroOffsetBlas_,
                nonZeroOffset
                    ? timingNonZeroOffsetBlasDesc_
                    : timingZeroOffsetBlasDesc_);
            commandList->endTimerQuery(slot.query);
        };

    submit(*available[0], !zeroFirst);
    submit(*available[1], zeroFirst);
    ++timingSubmittedPairs_;
}

void PtGeometryOffsetBlasProbe::FinishTimingRunIfReady()
{
    if (!timingRunActive_ ||
        timingSubmittedPairs_ != timingTargetPairs_ ||
        timingCompletedQueries_ != timingTargetPairs_ * 2)
    {
        return;
    }

    timingReport_ = PtGeometryOffsetBlasTimingReport();
    timingReport_.candidateSignature =
        timingCandidateSignature_;
    timingReport_.vertexOffsetBytes =
        timingVertexOffsetBytes_;
    timingReport_.testedIndexOffsetBytes =
        timingIndexOffsetBytes_;
    timingReport_.vertexCount = timingVertexCount_;
    timingReport_.indexCount = timingIndexCount_;
    timingReport_.primitiveCount = timingPrimitiveCount_;
    timingReport_.requestedPairs = timingTargetPairs_;
    timingReport_.submittedPairs = timingSubmittedPairs_;
    timingReport_.completedQueries =
        timingCompletedQueries_;
    timingReport_.timerCreateFailures =
        timingTimerCreateFailures_;
    timingReport_.resourceCreateFailures =
        timingResourceCreateFailures_;
    timingReport_.samples = timingSamples_;
    timingReportPending_ = true;
    RetireTimingResources();
}

void PtGeometryOffsetBlasProbe::RetireCurrent()
{
    if (blas_)
    {
        retired_.push_back(blas_);
        ++stats_.blasRetired;
    }
    blas_ = nullptr;
    blasDesc_ = nvrhi::rt::AccelStructDesc();
    builtCandidateSignature_ = 0;
}

void PtGeometryOffsetBlasProbe::FinishReadback(nvrhi::IDevice* device)
{
    if (!stats_.readbackPending || readbackDelayFrames_ > 0 ||
        device == nullptr || !readbackBuffer_)
    {
        return;
    }

    const ReadbackPayload* actual = static_cast<const ReadbackPayload*>(
        device->mapBuffer(readbackBuffer_, nvrhi::CpuAccessMode::Read));
    if (actual == nullptr)
    {
        ++stats_.readbacksFailed;
        stats_.endpointBytesValid = false;
        stats_.readbackPending = false;
        return;
    }

    if (std::memcmp(
            actual,
            &expectedReadback_,
            sizeof(expectedReadback_)) == 0)
    {
        ++stats_.readbacksPassed;
        stats_.endpointBytesValid = true;
    }
    else
    {
        ++stats_.readbacksFailed;
        stats_.endpointBytesValid = false;
    }
    device->unmapBuffer(readbackBuffer_);
    stats_.readbackPending = false;
}

void PtGeometryOffsetBlasProbe::Update(
    nvrhi::IDevice* device,
    nvrhi::ICommandList* commandList,
    const PtGeometrySourceRegistry& sources,
    const PtGeometryGpuPoolSet& pools,
    std::uint64_t frameIndex,
    int requestedTimingPairs)
{
    if (requestedTimingPairs > 0 &&
        !timingRunActive_ &&
        !timingReportPending_)
    {
        pendingTimingPairRequest_ =
            std::max(1, std::min(120, requestedTimingPairs));
    }
    PollTimingQueries(device, frameIndex);
    FinishTimingRunIfReady();

    if (stats_.readbackPending && readbackDelayFrames_ > 0)
    {
        --readbackDelayFrames_;
    }
    FinishReadback(device);

    stats_.sourceRecords = sources.RecordCount();
    ProbeCandidate candidate;
    if (!FindCandidate(
            sources,
            pools,
            candidate,
            stats_.eligibleRecords))
    {
        if (timingRunActive_)
        {
            pendingTimingPairRequest_ =
                static_cast<int>(timingTargetPairs_);
            RetireTimingResources();
        }
        if (observedCandidateSignature_ != 0)
        {
            RetireCurrent();
        }
        observedCandidateSignature_ = 0;
        stats_.selectedSourceIndex =
            std::numeric_limits<std::uint64_t>::max();
        stats_.candidateSignature = 0;
        stats_.stableFrames = 0;
        stats_.blasValid = false;
        return;
    }

    stats_.selectedSourceIndex = candidate.sourceIndex;
    stats_.candidateSignature = candidate.signature;
    stats_.positionOffsetBytes = candidate.gpu->positions.offsetBytes;
    stats_.attributeOffsetBytes = candidate.gpu->attributes.offsetBytes;
    stats_.indexOffsetBytes = candidate.gpu->indexes.offsetBytes;
    stats_.triangleOffsetBytes = candidate.gpu->triangles.offsetBytes;
    stats_.vertexCount = candidate.source->payload.positions.size();
    stats_.indexCount = candidate.source->payload.indexes.size();
    stats_.primitiveCount = candidate.source->payload.triangles.size();
    stats_.firstMaterialSlot =
        candidate.source->payload.triangles.front().sourceMaterialSlot;
    stats_.lastMaterialSlot =
        candidate.source->payload.triangles.back().sourceMaterialSlot;

    if (candidate.signature != observedCandidateSignature_)
    {
        RetireCurrent();
        observedCandidateSignature_ = candidate.signature;
        stats_.stableFrames = 1;
        stats_.endpointBytesValid = false;
    }
    else if (stats_.stableFrames <
        std::numeric_limits<std::uint64_t>::max())
    {
        ++stats_.stableFrames;
    }

    if (timingRunActive_ &&
        timingCandidateSignature_ != candidate.signature)
    {
        pendingTimingPairRequest_ =
            static_cast<int>(timingTargetPairs_);
        RetireTimingResources();
    }
    if (!timingRunActive_ &&
        pendingTimingPairRequest_ > 0 &&
        stats_.stableFrames >= kStableFramesBeforeBuild)
    {
        BeginTimingRun(
            device,
            commandList,
            *candidate.source,
            *candidate.gpu,
            pools,
            candidate.signature);
    }
    SubmitTimingPair(device, commandList, frameIndex);
    FinishTimingRunIfReady();

    if (device == nullptr || commandList == nullptr ||
        stats_.stableFrames < kStableFramesBeforeBuild ||
        builtCandidateSignature_ == candidate.signature)
    {
        stats_.blasValid = blas_ != nullptr;
        return;
    }

    nvrhi::rt::GeometryTriangles triangles;
    triangles.vertexBuffer = pools.PositionBuffer();
    triangles.indexBuffer = pools.IndexBuffer();
    triangles.vertexFormat = nvrhi::Format::RGB32_FLOAT;
    triangles.indexFormat = nvrhi::Format::R32_UINT;
    triangles.vertexOffset = candidate.gpu->positions.offsetBytes;
    triangles.indexOffset = candidate.gpu->indexes.offsetBytes;
    triangles.vertexCount = static_cast<std::uint32_t>(
        candidate.source->payload.positions.size());
    triangles.indexCount = static_cast<std::uint32_t>(
        candidate.source->payload.indexes.size());
    triangles.vertexStride = sizeof(PtGeometrySourcePosition);

    nvrhi::rt::GeometryDesc geometry;
    geometry.setTriangles(triangles);
    blasDesc_ = nvrhi::rt::AccelStructDesc()
        .addBottomLevelGeometry(geometry)
        .setBuildFlags(nvrhi::rt::AccelStructBuildFlags::PreferFastTrace)
        .setDebugName("PathTraceCanonicalOffsetProbeBLAS");
    blas_ = device->createAccelStruct(blasDesc_);
    if (!blas_)
    {
        stats_.blasValid = false;
        return;
    }
    ++stats_.blasCreated;

    commandList->setBufferState(
        pools.PositionBuffer(),
        nvrhi::ResourceStates::AccelStructBuildInput);
    commandList->setBufferState(
        pools.IndexBuffer(),
        nvrhi::ResourceStates::AccelStructBuildInput);
    commandList->commitBarriers();
    const std::chrono::steady_clock::time_point buildStart =
        std::chrono::steady_clock::now();
    nvrhi::utils::BuildBottomLevelAccelStruct(
        commandList,
        blas_,
        blasDesc_);
    const std::chrono::steady_clock::time_point buildEnd =
        std::chrono::steady_clock::now();
    stats_.buildSubmitMicroseconds +=
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                buildEnd - buildStart).count());
    ++stats_.blasBuilt;
    builtCandidateSignature_ = candidate.signature;
    stats_.blasValid = true;

    if (!readbackBuffer_)
    {
        nvrhi::BufferDesc readbackDesc;
        readbackDesc.byteSize = sizeof(ReadbackPayload);
        readbackDesc.debugName = "PathTraceCanonicalOffsetProbeReadback";
        readbackDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
        readbackDesc.initialState = nvrhi::ResourceStates::CopyDest;
        readbackDesc.keepInitialState = true;
        readbackBuffer_ = device->createBuffer(readbackDesc);
    }
    if (!readbackBuffer_ || stats_.readbackPending)
    {
        return;
    }

    const std::size_t lastIndexBase =
        candidate.source->payload.indexes.size() - 3;
    std::memcpy(
        expectedReadback_.firstIndexes,
        candidate.source->payload.indexes.data(),
        sizeof(expectedReadback_.firstIndexes));
    std::memcpy(
        expectedReadback_.lastIndexes,
        candidate.source->payload.indexes.data() + lastIndexBase,
        sizeof(expectedReadback_.lastIndexes));
    for (int endpoint = 0; endpoint < 3; ++endpoint)
    {
        candidate.source->payload.DecodeAttribute(
            expectedReadback_.firstIndexes[endpoint],
            expectedReadback_.firstAttributes[endpoint]);
        candidate.source->payload.DecodeAttribute(
            expectedReadback_.lastIndexes[endpoint],
            expectedReadback_.lastAttributes[endpoint]);
    }
    expectedReadback_.firstTriangle =
        candidate.source->payload.triangles.front();
    expectedReadback_.lastTriangle =
        candidate.source->payload.triangles.back();

    commandList->setBufferState(
        pools.IndexBuffer(),
        nvrhi::ResourceStates::CopySource);
    commandList->setBufferState(
        pools.AttributeBuffer(),
        nvrhi::ResourceStates::CopySource);
    commandList->setBufferState(
        pools.TriangleBuffer(),
        nvrhi::ResourceStates::CopySource);
    commandList->setBufferState(
        readbackBuffer_,
        nvrhi::ResourceStates::CopyDest);
    commandList->commitBarriers();

    const std::uint64_t firstIndexesOffset =
        offsetof(ReadbackPayload, firstIndexes);
    const std::uint64_t lastIndexesOffset =
        offsetof(ReadbackPayload, lastIndexes);
    commandList->copyBuffer(
        readbackBuffer_,
        firstIndexesOffset,
        pools.IndexBuffer(),
        candidate.gpu->indexes.offsetBytes,
        sizeof(expectedReadback_.firstIndexes));
    commandList->copyBuffer(
        readbackBuffer_,
        lastIndexesOffset,
        pools.IndexBuffer(),
        candidate.gpu->indexes.offsetBytes +
            lastIndexBase * sizeof(std::uint32_t),
        sizeof(expectedReadback_.lastIndexes));

    for (int endpoint = 0; endpoint < 3; ++endpoint)
    {
        commandList->copyBuffer(
            readbackBuffer_,
            offsetof(ReadbackPayload, firstAttributes) +
                endpoint * sizeof(PtGeometrySourceAttribute),
            pools.AttributeBuffer(),
            candidate.gpu->attributes.offsetBytes +
                static_cast<std::uint64_t>(
                    expectedReadback_.firstIndexes[endpoint]) *
                    sizeof(PtGeometrySourceAttribute),
            sizeof(PtGeometrySourceAttribute));
        commandList->copyBuffer(
            readbackBuffer_,
            offsetof(ReadbackPayload, lastAttributes) +
                endpoint * sizeof(PtGeometrySourceAttribute),
            pools.AttributeBuffer(),
            candidate.gpu->attributes.offsetBytes +
                static_cast<std::uint64_t>(
                    expectedReadback_.lastIndexes[endpoint]) *
                    sizeof(PtGeometrySourceAttribute),
            sizeof(PtGeometrySourceAttribute));
    }
    commandList->copyBuffer(
        readbackBuffer_,
        offsetof(ReadbackPayload, firstTriangle),
        pools.TriangleBuffer(),
        candidate.gpu->triangles.offsetBytes,
        sizeof(PtGeometrySourceTriangle));
    commandList->copyBuffer(
        readbackBuffer_,
        offsetof(ReadbackPayload, lastTriangle),
        pools.TriangleBuffer(),
        candidate.gpu->triangles.offsetBytes +
            (candidate.source->payload.triangles.size() - 1) *
                sizeof(PtGeometrySourceTriangle),
        sizeof(PtGeometrySourceTriangle));

    commandList->setBufferState(
        pools.IndexBuffer(),
        nvrhi::ResourceStates::AccelStructBuildInput);
    commandList->setBufferState(
        pools.AttributeBuffer(),
        nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(
        pools.TriangleBuffer(),
        nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();
    ++stats_.readbacksQueued;
    stats_.readbackPending = true;
    readbackDelayFrames_ = 3;
}

const PtGeometryOffsetBlasProbeStats&
PtGeometryOffsetBlasProbe::Stats() const
{
    return stats_;
}

bool PtGeometryOffsetBlasProbe::TakeTimingReport(
    PtGeometryOffsetBlasTimingReport& report)
{
    if (!timingReportPending_)
    {
        return false;
    }
    report = timingReport_;
    timingReport_ = PtGeometryOffsetBlasTimingReport();
    timingReportPending_ = false;
    return true;
}

std::size_t PtGeometryOffsetBlasProbe::TakeRetiredBuffers(
    std::vector<nvrhi::BufferHandle>& buffers)
{
    const std::size_t retiredCount = retiredBuffers_.size();
    buffers.reserve(buffers.size() + retiredCount);
    for (nvrhi::BufferHandle& buffer : retiredBuffers_)
    {
        buffers.push_back(buffer);
    }
    retiredBuffers_.clear();
    return retiredCount;
}

std::size_t PtGeometryOffsetBlasProbe::TakeRetiredBlases(
    std::vector<nvrhi::rt::AccelStructHandle>& blases)
{
    const std::size_t retiredCount = retired_.size();
    blases.reserve(blases.size() + retiredCount);
    for (nvrhi::rt::AccelStructHandle& blas : retired_)
    {
        blases.push_back(blas);
    }
    retired_.clear();
    return retiredCount;
}

std::size_t PtGeometryOffsetBlasProbe::RetiredBufferCount() const
{
    return retiredBuffers_.size();
}

std::size_t PtGeometryOffsetBlasProbe::RetiredBlasCount() const
{
    return retired_.size();
}

void PtGeometryOffsetBlasProbe::ClearRetiredBuffers()
{
    retiredBuffers_.clear();
}

void PtGeometryOffsetBlasProbe::ClearRetiredBlases()
{
    retired_.clear();
}

void PtGeometryOffsetBlasProbe::Clear()
{
    stats_ = PtGeometryOffsetBlasProbeStats();
    blas_ = nullptr;
    blasDesc_ = nvrhi::rt::AccelStructDesc();
    retiredBuffers_.clear();
    retired_.clear();
    readbackBuffer_ = nullptr;
    expectedReadback_ = ReadbackPayload();
    observedCandidateSignature_ = 0;
    builtCandidateSignature_ = 0;
    readbackDelayFrames_ = 0;
    pendingTimingPairRequest_ = 0;
    timingRunActive_ = false;
    timingReportPending_ = false;
    timingRunSerial_ = 0;
    timingCandidateSignature_ = 0;
    timingTargetPairs_ = 0;
    timingSubmittedPairs_ = 0;
    timingCompletedQueries_ = 0;
    timingTimerCreateFailures_ = 0;
    timingResourceCreateFailures_ = 0;
    timingIndexOffsetBytes_ = 0;
    timingVertexOffsetBytes_ = 0;
    timingVertexCount_ = 0;
    timingIndexCount_ = 0;
    timingPrimitiveCount_ = 0;
    timingIndexBuffer_ = nullptr;
    timingZeroOffsetBlas_ = nullptr;
    timingNonZeroOffsetBlas_ = nullptr;
    timingZeroOffsetBlasDesc_ = nvrhi::rt::AccelStructDesc();
    timingNonZeroOffsetBlasDesc_ = nvrhi::rt::AccelStructDesc();
    for (TimingQuerySlot& slot : timingQuerySlots_)
    {
        slot = TimingQuerySlot();
    }
    timingSamples_.clear();
    timingReport_ = PtGeometryOffsetBlasTimingReport();
}
