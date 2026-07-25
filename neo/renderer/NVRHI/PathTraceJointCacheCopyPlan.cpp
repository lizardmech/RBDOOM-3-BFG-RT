#include "PathTraceJointCacheCopyPlan.h"

#include <limits>

namespace {

bool CopyPlannerStateIsValid(const PtJointCacheCopyPlanner& planner)
{
    if (planner.usedBytes > planner.capacityBytes ||
        planner.usedBytes % PT_JOINT_CACHE_MATRIX_BYTES != 0)
    {
        return false;
    }

    std::uint64_t previousEndBytes = 0;
    for (const PtJointCacheCopyPlan& copy : planner.copies)
    {
        if (copy.reused ||
            !PtCanonicalInstanceKeyIsValid(copy.instance) ||
            copy.sourceBufferIdentity == 0 ||
            copy.sourceOffsetBytes % PT_JOINT_CACHE_COPY_ALIGNMENT_BYTES != 0 ||
            copy.destinationOffsetBytes % PT_JOINT_CACHE_MATRIX_BYTES != 0 ||
            copy.byteCount == 0 ||
            copy.byteCount % PT_JOINT_CACHE_MATRIX_BYTES != 0 ||
            copy.destinationOffsetBytes != previousEndBytes)
        {
            return false;
        }
        if (!PtCheckedAddU64(
                copy.destinationOffsetBytes,
                copy.byteCount,
                previousEndBytes) ||
            previousEndBytes > planner.usedBytes)
        {
            return false;
        }
    }
    return previousEndBytes == planner.usedBytes;
}

bool SameCopySource(
    const PtJointCacheCopyPlan& copy,
    const PtJointCacheCopyRequest& request,
    std::uint64_t byteCount)
{
    return copy.instance == request.instance &&
        copy.sourceBufferIdentity == request.sourceBufferIdentity &&
        copy.sourceOffsetBytes == request.sourceOffsetBytes &&
        copy.byteCount == byteCount;
}

}

PtJointCacheCopyPlanResult PtInitializeJointCacheCopyPlanner(
    PtJointCacheCopyPlanner& planner,
    std::uint64_t capacityBytes,
    std::uint64_t usedBytes)
{
    planner = PtJointCacheCopyPlanner();
    if (usedBytes > capacityBytes)
    {
        return PtJointCacheCopyPlanResult::InvalidState;
    }
    if (usedBytes % PT_JOINT_CACHE_MATRIX_BYTES != 0)
    {
        return PtJointCacheCopyPlanResult::DestinationMisaligned;
    }
    if (usedBytes != 0)
    {
        // This planner owns a tightly packed array and cannot validate an
        // opaque pre-existing prefix without its source records.
        return PtJointCacheCopyPlanResult::InvalidState;
    }
    planner.capacityBytes = capacityBytes;
    return PtJointCacheCopyPlanResult::PlannedCopy;
}

PtJointCacheCopyPlanResult PtPlanJointCacheCopy(
    PtJointCacheCopyPlanner& planner,
    const PtJointCacheCopyRequest& request,
    PtJointCacheCopyPlan& plan)
{
    plan = PtJointCacheCopyPlan();
    if (!CopyPlannerStateIsValid(planner))
    {
        return PtJointCacheCopyPlanResult::InvalidState;
    }
    if (!PtCanonicalInstanceKeyIsValid(request.instance))
    {
        return PtJointCacheCopyPlanResult::InvalidInstance;
    }
    if (request.sourceBufferIdentity == 0)
    {
        return PtJointCacheCopyPlanResult::MissingSourceBuffer;
    }
    if (request.jointCount == 0 || request.sourceRangeBytes == 0)
    {
        return PtJointCacheCopyPlanResult::EmptyJointRange;
    }

    std::uint64_t byteCount = 0;
    if (!PtCheckedMulU64(
            request.jointCount,
            PT_JOINT_CACHE_MATRIX_BYTES,
            byteCount))
    {
        return PtJointCacheCopyPlanResult::ArithmeticOverflow;
    }
    if (request.sourceRangeBytes != byteCount)
    {
        return PtJointCacheCopyPlanResult::SourceRangeMismatch;
    }
    if (request.sourceOffsetBytes % PT_JOINT_CACHE_COPY_ALIGNMENT_BYTES != 0)
    {
        return PtJointCacheCopyPlanResult::SourceMisaligned;
    }
    std::uint64_t sourceEndBytes = 0;
    if (!PtCheckedAddU64(
            request.sourceOffsetBytes,
            byteCount,
            sourceEndBytes))
    {
        return PtJointCacheCopyPlanResult::ArithmeticOverflow;
    }
    if (sourceEndBytes > request.sourceBufferBytes)
    {
        return PtJointCacheCopyPlanResult::SourceBufferBoundsExceeded;
    }
    if (planner.usedBytes % PT_JOINT_CACHE_MATRIX_BYTES != 0)
    {
        return PtJointCacheCopyPlanResult::DestinationMisaligned;
    }

    for (const PtJointCacheCopyPlan& existing : planner.copies)
    {
        if (SameCopySource(existing, request, byteCount))
        {
            plan = existing;
            plan.reused = true;
            return PtJointCacheCopyPlanResult::ReusedCopy;
        }
    }

    std::uint64_t endBytes = 0;
    if (!PtCheckedAddU64(planner.usedBytes, byteCount, endBytes))
    {
        return PtJointCacheCopyPlanResult::ArithmeticOverflow;
    }
    if (endBytes > planner.capacityBytes)
    {
        return PtJointCacheCopyPlanResult::CapacityExceeded;
    }

    plan.instance = request.instance;
    plan.sourceBufferIdentity = request.sourceBufferIdentity;
    plan.sourceOffsetBytes = request.sourceOffsetBytes;
    plan.destinationOffsetBytes = planner.usedBytes;
    plan.byteCount = byteCount;
    plan.reused = false;
    planner.copies.push_back(plan);
    planner.usedBytes = endBytes;
    return PtJointCacheCopyPlanResult::PlannedCopy;
}

const char* PtJointCacheCopyPlanResultName(
    PtJointCacheCopyPlanResult result)
{
    switch (result)
    {
        case PtJointCacheCopyPlanResult::PlannedCopy:
            return "planned-copy";
        case PtJointCacheCopyPlanResult::ReusedCopy:
            return "reused-copy";
        case PtJointCacheCopyPlanResult::InvalidState:
            return "invalid-state";
        case PtJointCacheCopyPlanResult::InvalidInstance:
            return "invalid-instance";
        case PtJointCacheCopyPlanResult::MissingSourceBuffer:
            return "missing-source-buffer";
        case PtJointCacheCopyPlanResult::EmptyJointRange:
            return "empty-joint-range";
        case PtJointCacheCopyPlanResult::ArithmeticOverflow:
            return "arithmetic-overflow";
        case PtJointCacheCopyPlanResult::SourceRangeMismatch:
            return "source-range-mismatch";
        case PtJointCacheCopyPlanResult::SourceMisaligned:
            return "source-misaligned";
        case PtJointCacheCopyPlanResult::SourceBufferBoundsExceeded:
            return "source-buffer-bounds-exceeded";
        case PtJointCacheCopyPlanResult::DestinationMisaligned:
            return "destination-misaligned";
        case PtJointCacheCopyPlanResult::CapacityExceeded:
            return "capacity-exceeded";
        default:
            return "unknown";
    }
}
