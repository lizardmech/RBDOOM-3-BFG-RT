#pragma once

#include "PathTraceCanonicalGeometryIdentity.h"

#include <cstdint>
#include <vector>

constexpr std::uint64_t PT_JOINT_CACHE_MATRIX_BYTES = 48;
constexpr std::uint64_t PT_JOINT_CACHE_COPY_ALIGNMENT_BYTES = 16;

enum class PtJointCacheCopyPlanResult : std::uint32_t
{
    PlannedCopy = 0,
    ReusedCopy,
    InvalidState,
    InvalidInstance,
    MissingSourceBuffer,
    EmptyJointRange,
    ArithmeticOverflow,
    SourceRangeMismatch,
    SourceMisaligned,
    SourceBufferBoundsExceeded,
    DestinationMisaligned,
    CapacityExceeded
};

struct PtJointCacheCopyRequest
{
    PtCanonicalInstanceKey instance;
    std::uint64_t sourceBufferIdentity = 0;
    std::uint64_t sourceBufferBytes = 0;
    std::uint64_t sourceOffsetBytes = 0;
    std::uint64_t sourceRangeBytes = 0;
    std::uint64_t jointCount = 0;
};

struct PtJointCacheCopyPlan
{
    PtCanonicalInstanceKey instance;
    std::uint64_t sourceBufferIdentity = 0;
    std::uint64_t sourceOffsetBytes = 0;
    std::uint64_t destinationOffsetBytes = 0;
    std::uint64_t byteCount = 0;
    bool reused = false;
};

struct PtJointCacheCopyPlanner
{
    std::uint64_t capacityBytes = 0;
    std::uint64_t usedBytes = 0;
    std::vector<PtJointCacheCopyPlan> copies;
};

PtJointCacheCopyPlanResult PtInitializeJointCacheCopyPlanner(
    PtJointCacheCopyPlanner& planner,
    std::uint64_t capacityBytes,
    std::uint64_t usedBytes = 0);

PtJointCacheCopyPlanResult PtPlanJointCacheCopy(
    PtJointCacheCopyPlanner& planner,
    const PtJointCacheCopyRequest& request,
    PtJointCacheCopyPlan& plan);

const char* PtJointCacheCopyPlanResultName(
    PtJointCacheCopyPlanResult result);
