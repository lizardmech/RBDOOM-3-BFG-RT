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

class PtGeometryOffsetBlasProbe
{
public:
    void Update(
        nvrhi::IDevice* device,
        nvrhi::ICommandList* commandList,
        const PtGeometrySourceRegistry& sources,
        const PtGeometryGpuPoolSet& pools,
        std::uint64_t frameIndex);
    const PtGeometryOffsetBlasProbeStats& Stats() const;
    void Clear();

private:
    struct RetiredBlas
    {
        nvrhi::rt::AccelStructHandle blas;
        std::uint64_t releaseAfterFrame = 0;
    };

    struct ReadbackPayload
    {
        std::uint32_t firstIndexes[3] = {};
        std::uint32_t lastIndexes[3] = {};
        PtGeometrySourceAttribute firstAttributes[3];
        PtGeometrySourceAttribute lastAttributes[3];
        PtGeometrySourceTriangle firstTriangle;
        PtGeometrySourceTriangle lastTriangle;
    };

    void ReleaseExpired(std::uint64_t frameIndex);
    void RetireCurrent(std::uint64_t frameIndex);
    void FinishReadback(nvrhi::IDevice* device);

    PtGeometryOffsetBlasProbeStats stats_;
    nvrhi::rt::AccelStructHandle blas_;
    nvrhi::rt::AccelStructDesc blasDesc_;
    std::vector<RetiredBlas> retired_;
    nvrhi::BufferHandle readbackBuffer_;
    ReadbackPayload expectedReadback_;
    std::uint64_t observedCandidateSignature_ = 0;
    std::uint64_t builtCandidateSignature_ = 0;
    int readbackDelayFrames_ = 0;
};
