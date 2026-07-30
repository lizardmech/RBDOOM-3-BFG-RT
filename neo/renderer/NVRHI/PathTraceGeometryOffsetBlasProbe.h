#pragma once

#include "PathTraceGeometryGpuPools.h"

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

struct PtGeometryOffsetBlasProbeStats
{
    std::uint64_t sourceRecords = 0;
    std::uint64_t eligibleRecords = 0;
    std::uint64_t selectedSourceIndex =
        std::numeric_limits<std::uint64_t>::max();
    std::uint64_t candidateSignature = 0;
    std::uint64_t stableFrames = 0;
    std::uint64_t positionOffsetBytes = 0;
    std::uint64_t indexOffsetBytes = 0;
    std::uint64_t attributeOffsetBytes = 0;
    std::uint64_t triangleOffsetBytes = 0;
    std::uint64_t vertexCount = 0;
    std::uint64_t indexCount = 0;
    std::uint64_t primitiveCount = 0;
    std::uint64_t blasCreated = 0;
    std::uint64_t blasBuilt = 0;
    std::uint64_t blasRetired = 0;
    std::uint64_t readbacksQueued = 0;
    std::uint64_t readbacksPassed = 0;
    std::uint64_t readbacksFailed = 0;
    std::uint64_t buildSubmitMicroseconds = 0;
    std::uint32_t firstMaterialSlot = 0;
    std::uint32_t lastMaterialSlot = 0;
    bool blasValid = false;
    bool readbackPending = false;
    bool endpointBytesValid = false;
};

struct PtGeometryOffsetBlasTimingSample
{
    double zeroOffsetMicroseconds = -1.0;
    double nonZeroOffsetMicroseconds = -1.0;
    bool zeroOffsetBuiltFirst = false;
};

struct PtGeometryOffsetBlasTimingReport
{
    std::uint64_t candidateSignature = 0;
    std::uint64_t vertexOffsetBytes = 0;
    std::uint64_t testedIndexOffsetBytes = 0;
    std::uint64_t vertexCount = 0;
    std::uint64_t indexCount = 0;
    std::uint64_t primitiveCount = 0;
    std::uint64_t requestedPairs = 0;
    std::uint64_t submittedPairs = 0;
    std::uint64_t completedQueries = 0;
    std::uint64_t timerCreateFailures = 0;
    std::uint64_t resourceCreateFailures = 0;
    std::vector<PtGeometryOffsetBlasTimingSample> samples;
};

class PtGeometryOffsetBlasProbe
{
public:
    void Update(
        nvrhi::IDevice* device,
        nvrhi::ICommandList* commandList,
        const PtGeometrySourceRegistry& sources,
        const PtGeometryGpuPoolSet& pools,
        std::uint64_t frameIndex,
        int requestedTimingPairs);
    const PtGeometryOffsetBlasProbeStats& Stats() const;
    bool TakeTimingReport(PtGeometryOffsetBlasTimingReport& report);
    std::size_t TakeRetiredBuffers(
        std::vector<nvrhi::BufferHandle>& buffers);
    std::size_t TakeRetiredBlases(
        std::vector<nvrhi::rt::AccelStructHandle>& blases);
    std::size_t RetiredBufferCount() const;
    std::size_t RetiredBlasCount() const;
    void ClearRetiredBuffers();
    void ClearRetiredBlases();
    void Clear();

private:
    struct ReadbackPayload
    {
        std::uint32_t firstIndexes[3] = {};
        std::uint32_t lastIndexes[3] = {};
        PtGeometrySourceAttribute firstAttributes[3];
        PtGeometrySourceAttribute lastAttributes[3];
        PtGeometrySourceTriangle firstTriangle;
        PtGeometrySourceTriangle lastTriangle;
    };

    static constexpr int kTimingQuerySlotCount = 16;
    struct TimingQuerySlot
    {
        nvrhi::TimerQueryHandle query;
        bool pending = false;
        bool nonZeroOffset = false;
        std::uint64_t runSerial = 0;
        std::uint64_t pairIndex = 0;
        std::uint64_t earliestPollFrame = 0;
    };

    void RetireCurrent();
    void RetireTimingResources();
    void PollTimingQueries(
        nvrhi::IDevice* device,
        std::uint64_t frameIndex);
    bool BeginTimingRun(
        nvrhi::IDevice* device,
        nvrhi::ICommandList* commandList,
        const PtGeometrySourceRecord& source,
        const PtGeometryGpuPoolRecord& gpu,
        const PtGeometryGpuPoolSet& pools,
        std::uint64_t candidateSignature);
    void SubmitTimingPair(
        nvrhi::IDevice* device,
        nvrhi::ICommandList* commandList,
        std::uint64_t frameIndex);
    void FinishTimingRunIfReady();
    void FinishReadback(nvrhi::IDevice* device);

    PtGeometryOffsetBlasProbeStats stats_;
    nvrhi::rt::AccelStructHandle blas_;
    nvrhi::rt::AccelStructDesc blasDesc_;
    std::vector<nvrhi::BufferHandle> retiredBuffers_;
    std::vector<nvrhi::rt::AccelStructHandle> retired_;
    nvrhi::BufferHandle readbackBuffer_;
    ReadbackPayload expectedReadback_;
    std::uint64_t observedCandidateSignature_ = 0;
    std::uint64_t builtCandidateSignature_ = 0;
    int readbackDelayFrames_ = 0;

    int pendingTimingPairRequest_ = 0;
    bool timingRunActive_ = false;
    bool timingReportPending_ = false;
    std::uint64_t timingRunSerial_ = 0;
    std::uint64_t timingCandidateSignature_ = 0;
    std::uint64_t timingTargetPairs_ = 0;
    std::uint64_t timingSubmittedPairs_ = 0;
    std::uint64_t timingCompletedQueries_ = 0;
    std::uint64_t timingTimerCreateFailures_ = 0;
    std::uint64_t timingResourceCreateFailures_ = 0;
    std::uint64_t timingIndexOffsetBytes_ = 0;
    std::uint64_t timingVertexOffsetBytes_ = 0;
    std::uint64_t timingVertexCount_ = 0;
    std::uint64_t timingIndexCount_ = 0;
    std::uint64_t timingPrimitiveCount_ = 0;
    nvrhi::BufferHandle timingIndexBuffer_;
    nvrhi::rt::AccelStructHandle timingZeroOffsetBlas_;
    nvrhi::rt::AccelStructHandle timingNonZeroOffsetBlas_;
    nvrhi::rt::AccelStructDesc timingZeroOffsetBlasDesc_;
    nvrhi::rt::AccelStructDesc timingNonZeroOffsetBlasDesc_;
    TimingQuerySlot timingQuerySlots_[kTimingQuerySlotCount];
    std::vector<PtGeometryOffsetBlasTimingSample> timingSamples_;
    PtGeometryOffsetBlasTimingReport timingReport_;
};
