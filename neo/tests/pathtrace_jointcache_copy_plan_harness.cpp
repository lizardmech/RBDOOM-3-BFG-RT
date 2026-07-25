#include "../renderer/NVRHI/PathTraceJointCacheCopyPlan.h"

#include <cstdint>
#include <cstdio>
#include <limits>

namespace {

int g_failures = 0;

void Expect(bool condition, const char* message)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

PtCanonicalInstanceKey MakeInstance(
    std::uint32_t renderDefIndex,
    std::uint32_t modelSurfaceIndex)
{
    PtCanonicalInstanceKey key;
    key.worldGeneration = 7;
    key.renderDefIndex = renderDefIndex;
    key.renderDefGeneration = 3;
    key.subInstanceKind = PtCanonicalSubInstanceKind::SkinnedSurface;
    key.modelSurfaceIndex = modelSurfaceIndex;
    key.jointSubmeshIndex = -1;
    return key;
}

PtJointCacheCopyRequest MakeRequest(
    const PtCanonicalInstanceKey& instance,
    std::uint64_t sourceBuffer,
    std::uint64_t sourceOffset,
    std::uint64_t jointCount)
{
    PtJointCacheCopyRequest request;
    request.instance = instance;
    request.sourceBufferIdentity = sourceBuffer;
    request.sourceOffsetBytes = sourceOffset;
    request.sourceRangeBytes =
        jointCount * PT_JOINT_CACHE_MATRIX_BYTES;
    request.jointCount = jointCount;
    return request;
}

void TestCheckedAppendAndCapacity()
{
    PtJointCacheCopyPlanner planner;
    Expect(
        PtInitializeJointCacheCopyPlanner(planner, 192) ==
            PtJointCacheCopyPlanResult::PlannedCopy,
        "empty compact planner should initialize");

    PtJointCacheCopyPlan plan;
    const PtJointCacheCopyRequest request =
        MakeRequest(MakeInstance(10, 0), 1, 256, 2);
    Expect(
        PtPlanJointCacheCopy(planner, request, plan) ==
            PtJointCacheCopyPlanResult::PlannedCopy,
        "valid range should append one compact copy");
    Expect(
        plan.destinationOffsetBytes == 0 &&
            plan.byteCount == 96 &&
            !plan.reused &&
            planner.usedBytes == 96 &&
            planner.copies.size() == 1,
        "first append should occupy exact 48-byte matrix units");

    const std::uint64_t usedBeforeFailure = planner.usedBytes;
    const std::size_t copiesBeforeFailure = planner.copies.size();
    const PtJointCacheCopyRequest tooLarge =
        MakeRequest(MakeInstance(11, 0), 1, 512, 3);
    Expect(
        PtPlanJointCacheCopy(planner, tooLarge, plan) ==
            PtJointCacheCopyPlanResult::CapacityExceeded,
        "copy beyond compact capacity should fail explicitly");
    Expect(
        planner.usedBytes == usedBeforeFailure &&
            planner.copies.size() == copiesBeforeFailure,
        "failed capacity check must not mutate planner state");
}

void TestOverflowAlignmentAndRangeMismatch()
{
    PtJointCacheCopyPlanner planner;
    Expect(
        PtInitializeJointCacheCopyPlanner(planner, 4096) ==
            PtJointCacheCopyPlanResult::PlannedCopy,
        "planner should initialize for rejection tests");

    PtJointCacheCopyPlan plan;
    PtJointCacheCopyRequest request =
        MakeRequest(MakeInstance(20, 0), 2, 256, 2);
    request.sourceRangeBytes = 48;
    Expect(
        PtPlanJointCacheCopy(planner, request, plan) ==
            PtJointCacheCopyPlanResult::SourceRangeMismatch,
        "handle byte range must exactly match jointCount times 48");

    request = MakeRequest(MakeInstance(20, 0), 2, 258, 2);
    Expect(
        PtPlanJointCacheCopy(planner, request, plan) ==
            PtJointCacheCopyPlanResult::SourceMisaligned,
        "renderer source offset must preserve 16-byte joint row alignment");

    request = MakeRequest(MakeInstance(20, 0), 2, 256, 1);
    request.jointCount =
        std::numeric_limits<std::uint64_t>::max() /
            PT_JOINT_CACHE_MATRIX_BYTES +
        1;
    request.sourceRangeBytes = std::numeric_limits<std::uint64_t>::max();
    Expect(
        PtPlanJointCacheCopy(planner, request, plan) ==
            PtJointCacheCopyPlanResult::ArithmeticOverflow,
        "joint byte multiplication must reject uint64 overflow");

    request = MakeRequest(
        MakeInstance(20, 0),
        2,
        std::numeric_limits<std::uint64_t>::max() - 47,
        2);
    Expect(
        PtPlanJointCacheCopy(planner, request, plan) ==
            PtJointCacheCopyPlanResult::ArithmeticOverflow,
        "source range end must reject uint64 overflow");

    Expect(
        PtInitializeJointCacheCopyPlanner(planner, 4096, 16) ==
            PtJointCacheCopyPlanResult::DestinationMisaligned,
        "compact destination prefix must align to whole 48-byte matrices");
}

void TestSameInstanceReuseAndCrossInstanceIsolation()
{
    PtJointCacheCopyPlanner planner;
    Expect(
        PtInitializeJointCacheCopyPlanner(planner, 4096) ==
            PtJointCacheCopyPlanResult::PlannedCopy,
        "planner should initialize for reuse tests");

    const PtCanonicalInstanceKey instanceA = MakeInstance(30, 2);
    const PtCanonicalInstanceKey instanceB = MakeInstance(31, 2);
    const PtJointCacheCopyRequest requestA =
        MakeRequest(instanceA, 3, 768, 4);
    PtJointCacheCopyPlan first;
    Expect(
        PtPlanJointCacheCopy(planner, requestA, first) ==
            PtJointCacheCopyPlanResult::PlannedCopy,
        "first instance range should allocate");

    PtJointCacheCopyPlan duplicate;
    Expect(
        PtPlanJointCacheCopy(planner, requestA, duplicate) ==
            PtJointCacheCopyPlanResult::ReusedCopy,
        "exact duplicate range for the same instance should reuse");
    Expect(
        duplicate.reused &&
            duplicate.destinationOffsetBytes == first.destinationOffsetBytes &&
            duplicate.byteCount == first.byteCount &&
            planner.usedBytes == first.byteCount &&
            planner.copies.size() == 1,
        "same-instance reuse should preserve one physical copy");

    const PtJointCacheCopyRequest requestB =
        MakeRequest(instanceB, 3, 768, 4);
    PtJointCacheCopyPlan isolated;
    Expect(
        PtPlanJointCacheCopy(planner, requestB, isolated) ==
            PtJointCacheCopyPlanResult::PlannedCopy,
        "different live instance must not reuse an identical source range");
    Expect(
        !isolated.reused &&
            isolated.destinationOffsetBytes == first.byteCount &&
            planner.usedBytes == first.byteCount * 2 &&
            planner.copies.size() == 2,
        "cross-instance pose data must occupy a distinct compact range");
}

}

int main()
{
    TestCheckedAppendAndCapacity();
    TestOverflowAlignmentAndRangeMismatch();
    TestSameInstanceReuseAndCrossInstanceIsolation();

    if (g_failures != 0)
    {
        std::printf(
            "PathTraceJointCacheCopyPlanHarness: %d failure(s)\n",
            g_failures);
        return 1;
    }
    std::printf("PathTraceJointCacheCopyPlanHarness: PASS\n");
    return 0;
}
