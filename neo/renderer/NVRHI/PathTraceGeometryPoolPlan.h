#pragma once

#include <cstdint>

enum class PtGeometryPoolPlanResult : std::uint32_t
{
    Success = 0,
    InvalidState,
    EmptyAllocation,
    InvalidAlignment,
    InvalidElementStride,
    ArithmeticOverflow,
    CapacityExceeded,
    GenerationOverflow,
    SourceGenerationMismatch,
    SourceRangeNotCopied
};

struct PtGeometryPoolState
{
    std::uint64_t capacityBytes = 0;
    std::uint64_t usedBytes = 0;
    std::uint64_t storageGeneration = 1;
};

struct PtGeometryPoolRange
{
    std::uint64_t offsetBytes = 0;
    std::uint64_t sizeBytes = 0;
    std::uint64_t alignmentBytes = 0;
    std::uint64_t storageGeneration = 0;
};

struct PtGeometryPoolGrowthPlan
{
    bool grew = false;
    std::uint64_t previousCapacityBytes = 0;
    std::uint64_t nextCapacityBytes = 0;
    std::uint64_t copyBytes = 0;
    std::uint64_t previousStorageGeneration = 0;
    std::uint64_t nextStorageGeneration = 0;
};

PtGeometryPoolPlanResult PtGeometryPoolByteSizeForElements(
    std::uint64_t elementCount,
    std::uint64_t elementStrideBytes,
    std::uint64_t& byteSize);

PtGeometryPoolPlanResult PtAllocateGeometryPoolRange(
    PtGeometryPoolState& state,
    std::uint64_t sizeBytes,
    std::uint64_t alignmentBytes,
    PtGeometryPoolRange& range);

PtGeometryPoolPlanResult PtPlanGeometryPoolGrowth(
    const PtGeometryPoolState& state,
    std::uint64_t minimumRequiredCapacityBytes,
    std::uint64_t minimumGrowthBytes,
    std::uint64_t maximumCapacityBytes,
    PtGeometryPoolGrowthPlan& plan);

PtGeometryPoolPlanResult PtRebaseGeometryPoolRange(
    const PtGeometryPoolRange& sourceRange,
    const PtGeometryPoolGrowthPlan& growthPlan,
    PtGeometryPoolRange& rebasedRange);

const char* PtGeometryPoolPlanResultName(PtGeometryPoolPlanResult result);
