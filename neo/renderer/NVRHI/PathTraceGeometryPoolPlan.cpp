#include "PathTraceGeometryPoolPlan.h"

#include "PathTraceCanonicalGeometryIdentity.h"

#include <algorithm>
#include <limits>

namespace {

bool PoolStateIsValid(const PtGeometryPoolState& state)
{
    return state.storageGeneration != 0 &&
        state.usedBytes <= state.capacityBytes;
}

bool AlignPoolOffset(
    std::uint64_t value,
    std::uint64_t alignment,
    std::uint64_t& alignedValue)
{
    if (alignment == 0)
    {
        return false;
    }
    const std::uint64_t remainder = value % alignment;
    const std::uint64_t padding = remainder == 0 ? 0 : alignment - remainder;
    return PtCheckedAddU64(value, padding, alignedValue);
}

}

PtGeometryPoolPlanResult PtGeometryPoolByteSizeForElements(
    std::uint64_t elementCount,
    std::uint64_t elementStrideBytes,
    std::uint64_t& byteSize)
{
    byteSize = 0;
    if (elementCount == 0)
    {
        return PtGeometryPoolPlanResult::EmptyAllocation;
    }
    if (elementStrideBytes == 0)
    {
        return PtGeometryPoolPlanResult::InvalidElementStride;
    }
    if (!PtCheckedMulU64(elementCount, elementStrideBytes, byteSize))
    {
        byteSize = 0;
        return PtGeometryPoolPlanResult::ArithmeticOverflow;
    }
    return PtGeometryPoolPlanResult::Success;
}

PtGeometryPoolPlanResult PtAllocateGeometryPoolRange(
    PtGeometryPoolState& state,
    std::uint64_t sizeBytes,
    std::uint64_t alignmentBytes,
    PtGeometryPoolRange& range)
{
    range = PtGeometryPoolRange();
    if (!PoolStateIsValid(state))
    {
        return PtGeometryPoolPlanResult::InvalidState;
    }
    if (sizeBytes == 0)
    {
        return PtGeometryPoolPlanResult::EmptyAllocation;
    }
    if (alignmentBytes == 0)
    {
        return PtGeometryPoolPlanResult::InvalidAlignment;
    }

    std::uint64_t offsetBytes = 0;
    if (!AlignPoolOffset(state.usedBytes, alignmentBytes, offsetBytes))
    {
        return PtGeometryPoolPlanResult::ArithmeticOverflow;
    }
    std::uint64_t endBytes = 0;
    if (!PtCheckedAddU64(offsetBytes, sizeBytes, endBytes))
    {
        return PtGeometryPoolPlanResult::ArithmeticOverflow;
    }
    if (endBytes > state.capacityBytes)
    {
        return PtGeometryPoolPlanResult::CapacityExceeded;
    }

    range.offsetBytes = offsetBytes;
    range.sizeBytes = sizeBytes;
    range.alignmentBytes = alignmentBytes;
    range.storageGeneration = state.storageGeneration;
    state.usedBytes = endBytes;
    return PtGeometryPoolPlanResult::Success;
}

PtGeometryPoolPlanResult PtPlanGeometryPoolGrowth(
    const PtGeometryPoolState& state,
    std::uint64_t minimumRequiredCapacityBytes,
    std::uint64_t minimumGrowthBytes,
    std::uint64_t maximumCapacityBytes,
    PtGeometryPoolGrowthPlan& plan)
{
    plan = PtGeometryPoolGrowthPlan();
    if (!PoolStateIsValid(state) ||
        minimumGrowthBytes == 0 ||
        maximumCapacityBytes < state.capacityBytes)
    {
        return PtGeometryPoolPlanResult::InvalidState;
    }
    if (minimumRequiredCapacityBytes > maximumCapacityBytes)
    {
        return PtGeometryPoolPlanResult::CapacityExceeded;
    }

    plan.previousCapacityBytes = state.capacityBytes;
    plan.nextCapacityBytes = state.capacityBytes;
    plan.copyBytes = state.usedBytes;
    plan.previousStorageGeneration = state.storageGeneration;
    plan.nextStorageGeneration = state.storageGeneration;
    if (minimumRequiredCapacityBytes <= state.capacityBytes)
    {
        return PtGeometryPoolPlanResult::Success;
    }

    std::uint64_t candidateBytes =
        state.capacityBytes == 0
            ? std::min(minimumGrowthBytes, maximumCapacityBytes)
            : state.capacityBytes;
    while (candidateBytes < minimumRequiredCapacityBytes)
    {
        const std::uint64_t previousCandidateBytes = candidateBytes;
        const std::uint64_t growthBase = std::max(candidateBytes, minimumGrowthBytes);
        std::uint64_t doubledBytes = 0;
        if (!PtCheckedAddU64(growthBase, growthBase, doubledBytes) ||
            doubledBytes > maximumCapacityBytes)
        {
            candidateBytes = maximumCapacityBytes;
        }
        else
        {
            candidateBytes = doubledBytes;
        }
        if (candidateBytes <= previousCandidateBytes)
        {
            return PtGeometryPoolPlanResult::CapacityExceeded;
        }
    }

    if (state.storageGeneration == std::numeric_limits<std::uint64_t>::max())
    {
        return PtGeometryPoolPlanResult::GenerationOverflow;
    }
    plan.grew = true;
    plan.nextCapacityBytes = candidateBytes;
    plan.nextStorageGeneration = state.storageGeneration + 1;
    return PtGeometryPoolPlanResult::Success;
}

PtGeometryPoolPlanResult PtRebaseGeometryPoolRange(
    const PtGeometryPoolRange& sourceRange,
    const PtGeometryPoolGrowthPlan& growthPlan,
    PtGeometryPoolRange& rebasedRange)
{
    rebasedRange = PtGeometryPoolRange();
    if (sourceRange.sizeBytes == 0 ||
        sourceRange.alignmentBytes == 0 ||
        sourceRange.offsetBytes % sourceRange.alignmentBytes != 0 ||
        growthPlan.previousStorageGeneration == 0 ||
        growthPlan.nextStorageGeneration == 0)
    {
        return PtGeometryPoolPlanResult::InvalidState;
    }
    if (sourceRange.storageGeneration != growthPlan.previousStorageGeneration)
    {
        return PtGeometryPoolPlanResult::SourceGenerationMismatch;
    }

    std::uint64_t sourceEndBytes = 0;
    if (!PtCheckedAddU64(sourceRange.offsetBytes, sourceRange.sizeBytes, sourceEndBytes))
    {
        return PtGeometryPoolPlanResult::ArithmeticOverflow;
    }
    if (sourceEndBytes > growthPlan.copyBytes ||
        sourceEndBytes > growthPlan.previousCapacityBytes ||
        sourceEndBytes > growthPlan.nextCapacityBytes)
    {
        return PtGeometryPoolPlanResult::SourceRangeNotCopied;
    }

    rebasedRange = sourceRange;
    rebasedRange.storageGeneration = growthPlan.nextStorageGeneration;
    return PtGeometryPoolPlanResult::Success;
}

const char* PtGeometryPoolPlanResultName(PtGeometryPoolPlanResult result)
{
    switch (result)
    {
        case PtGeometryPoolPlanResult::Success:
            return "success";
        case PtGeometryPoolPlanResult::InvalidState:
            return "invalid-state";
        case PtGeometryPoolPlanResult::EmptyAllocation:
            return "empty-allocation";
        case PtGeometryPoolPlanResult::InvalidAlignment:
            return "invalid-alignment";
        case PtGeometryPoolPlanResult::InvalidElementStride:
            return "invalid-element-stride";
        case PtGeometryPoolPlanResult::ArithmeticOverflow:
            return "arithmetic-overflow";
        case PtGeometryPoolPlanResult::CapacityExceeded:
            return "capacity-exceeded";
        case PtGeometryPoolPlanResult::GenerationOverflow:
            return "generation-overflow";
        case PtGeometryPoolPlanResult::SourceGenerationMismatch:
            return "source-generation-mismatch";
        case PtGeometryPoolPlanResult::SourceRangeNotCopied:
            return "source-range-not-copied";
        default:
            return "unknown";
    }
}
