#include "../renderer/NVRHI/PathTraceCanonicalGeometryIdentity.h"
#include "../renderer/NVRHI/PathTraceGeometryPoolPlan.h"

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

void TestAlignedAppendAndFailureAtomicity()
{
    PtGeometryPoolState state;
    state.capacityBytes = 128;
    state.usedBytes = 3;
    state.storageGeneration = 9;

    PtGeometryPoolRange range;
    Expect(
        PtAllocateGeometryPoolRange(state, 16, 16, range) ==
            PtGeometryPoolPlanResult::Success,
        "aligned allocation should succeed");
    Expect(
        range.offsetBytes == 16 &&
            range.sizeBytes == 16 &&
            range.alignmentBytes == 16 &&
            range.storageGeneration == 9 &&
            state.usedBytes == 32,
        "aligned allocation should preserve byte range and generation");

    const std::uint64_t usedBeforeFailure = state.usedBytes;
    Expect(
        PtAllocateGeometryPoolRange(state, 256, 64, range) ==
            PtGeometryPoolPlanResult::CapacityExceeded,
        "allocation beyond capacity should fail explicitly");
    Expect(
        state.usedBytes == usedBeforeFailure && range.storageGeneration == 0,
        "failed allocation must not mutate pool state or return a live range");

    Expect(
        PtAllocateGeometryPoolRange(state, 0, 16, range) ==
            PtGeometryPoolPlanResult::EmptyAllocation,
        "empty allocations should fail closed");
    Expect(
        PtAllocateGeometryPoolRange(state, 4, 0, range) ==
            PtGeometryPoolPlanResult::InvalidAlignment,
        "zero alignment should fail closed");
}

void TestFourGiBBoundaryAndShaderPageNarrowing()
{
    const std::uint64_t fourGiB = 1ull << 32;
    PtGeometryPoolState state;
    state.capacityBytes = fourGiB + 4096;
    state.usedBytes = fourGiB - 8;
    state.storageGeneration = 3;

    PtGeometryPoolRange range;
    Expect(
        PtAllocateGeometryPoolRange(state, 64, 256, range) ==
            PtGeometryPoolPlanResult::Success,
        "allocation crossing the uint32 byte boundary should remain valid");
    Expect(
        range.offsetBytes == fourGiB && state.usedBytes == fourGiB + 64,
        "pool offsets must remain absolute uint64 byte values");

    PtCanonicalShaderPageRange pageRange;
    pageRange.pageBaseBytes = fourGiB;
    pageRange.pageSizeBytes = 4096;
    pageRange.absoluteOffsetBytes = range.offsetBytes;
    pageRange.byteSize = range.sizeBytes;
    pageRange.elementStrideBytes = 16;
    PtCanonicalShaderRange32 shaderRange;
    Expect(
        PtNarrowCanonicalRangeToShaderPage(pageRange, shaderRange) ==
            PtCanonicalShaderNarrowResult::Success,
        "page-relative narrowing should accept a range above 4 GiB");
    Expect(
        shaderRange.elementOffset == 0 && shaderRange.elementCount == 4,
        "shader narrowing must subtract the page base before uint32 conversion");
}

void TestArithmeticAndGenerationOverflow()
{
    std::uint64_t byteSize = 0;
    Expect(
        PtGeometryPoolByteSizeForElements(
            static_cast<std::uint64_t>(UINT32_MAX) + 1ull,
            16,
            byteSize) == PtGeometryPoolPlanResult::Success &&
            byteSize == (static_cast<std::uint64_t>(UINT32_MAX) + 1ull) * 16ull,
        "element byte sizing should remain uint64 above uint32 limits");
    Expect(
        PtGeometryPoolByteSizeForElements(
            std::numeric_limits<std::uint64_t>::max(),
            2,
            byteSize) == PtGeometryPoolPlanResult::ArithmeticOverflow &&
            byteSize == 0,
        "element byte-size multiplication should reject overflow");
    Expect(
        PtGeometryPoolByteSizeForElements(1, 0, byteSize) ==
            PtGeometryPoolPlanResult::InvalidElementStride,
        "zero element stride should fail closed");

    PtGeometryPoolState state;
    state.capacityBytes = std::numeric_limits<std::uint64_t>::max();
    state.usedBytes = std::numeric_limits<std::uint64_t>::max() - 3;
    state.storageGeneration = 1;
    PtGeometryPoolRange range;
    Expect(
        PtAllocateGeometryPoolRange(state, 16, 8, range) ==
            PtGeometryPoolPlanResult::ArithmeticOverflow,
        "alignment/end overflow should be named and rejected");

    state.capacityBytes = 1024;
    state.usedBytes = 512;
    state.storageGeneration = std::numeric_limits<std::uint64_t>::max();
    PtGeometryPoolGrowthPlan growth;
    Expect(
        PtPlanGeometryPoolGrowth(state, 2048, 1024, 4096, growth) ==
            PtGeometryPoolPlanResult::GenerationOverflow,
        "storage generation wrap should fail closed");
}

void TestGrowthAndRangePreservation()
{
    PtGeometryPoolState state;
    state.capacityBytes = 1024;
    state.usedBytes = 768;
    state.storageGeneration = 7;

    PtGeometryPoolGrowthPlan growth;
    Expect(
        PtPlanGeometryPoolGrowth(state, 512, 1024, 8192, growth) ==
            PtGeometryPoolPlanResult::Success &&
            !growth.grew &&
            growth.nextCapacityBytes == 1024 &&
            growth.nextStorageGeneration == 7,
        "no-growth plan should preserve capacity and generation");

    Expect(
        PtPlanGeometryPoolGrowth(state, 4097, 1024, 8192, growth) ==
            PtGeometryPoolPlanResult::Success,
        "bounded geometric growth should succeed");
    Expect(
        growth.grew &&
            growth.previousCapacityBytes == 1024 &&
            growth.nextCapacityBytes == 8192 &&
            growth.copyBytes == 768 &&
            growth.previousStorageGeneration == 7 &&
            growth.nextStorageGeneration == 8,
        "growth plan should preserve copy bytes and advance generation once");

    PtGeometryPoolRange sourceRange;
    sourceRange.offsetBytes = 256;
    sourceRange.sizeBytes = 128;
    sourceRange.alignmentBytes = 64;
    sourceRange.storageGeneration = 7;
    PtGeometryPoolRange rebasedRange;
    Expect(
        PtRebaseGeometryPoolRange(sourceRange, growth, rebasedRange) ==
            PtGeometryPoolPlanResult::Success,
        "fully copied range should rebase to the new generation");
    Expect(
        rebasedRange.offsetBytes == sourceRange.offsetBytes &&
            rebasedRange.sizeBytes == sourceRange.sizeBytes &&
            rebasedRange.storageGeneration == 8,
        "growth must preserve existing offsets and sizes");

    sourceRange.storageGeneration = 6;
    Expect(
        PtRebaseGeometryPoolRange(sourceRange, growth, rebasedRange) ==
            PtGeometryPoolPlanResult::SourceGenerationMismatch,
        "range from another storage generation should be rejected");

    sourceRange.storageGeneration = 7;
    sourceRange.offsetBytes = 704;
    sourceRange.sizeBytes = 128;
    Expect(
        PtRebaseGeometryPoolRange(sourceRange, growth, rebasedRange) ==
            PtGeometryPoolPlanResult::SourceRangeNotCopied,
        "range beyond copied payload should not be rebound");

    Expect(
        PtPlanGeometryPoolGrowth(state, 8193, 1024, 8192, growth) ==
            PtGeometryPoolPlanResult::CapacityExceeded,
        "growth beyond the declared maximum should fail explicitly");
}

}

int main()
{
    TestAlignedAppendAndFailureAtomicity();
    TestFourGiBBoundaryAndShaderPageNarrowing();
    TestArithmeticAndGenerationOverflow();
    TestGrowthAndRangePreservation();

    if (g_failures != 0)
    {
        std::printf("PathTraceGeometryPoolHarness: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PathTraceGeometryPoolHarness: PASS\n");
    return 0;
}
