#include "PathTraceSkinnedOutputAllocator.h"

#include <cstdint>
#include <cstdio>

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
    std::uint64_t worldGeneration,
    std::uint32_t renderDefIndex,
    std::uint32_t renderDefGeneration,
    std::uint32_t surfaceIndex)
{
    PtCanonicalInstanceKey key;
    key.worldGeneration = worldGeneration;
    key.renderDefIndex = renderDefIndex;
    key.renderDefGeneration = renderDefGeneration;
    key.subInstanceKind =
        PtCanonicalSubInstanceKind::SkinnedSurface;
    key.modelSurfaceIndex = surfaceIndex;
    key.jointSubmeshIndex =
        static_cast<std::int32_t>(surfaceIndex);
    return key;
}

PtSkinnedOutputAllocator MakeAllocator(
    std::uint64_t initialCapacity,
    std::uint64_t maxCapacity,
    std::uint64_t stride)
{
    PtSkinnedOutputAllocatorConfig config;
    config.initialCapacityVertices = initialCapacity;
    config.maxCapacityVertices = maxCapacity;
    config.vertexStrideBytes = stride;
    return PtSkinnedOutputAllocator(config);
}

void TestDistinctInstancesNeverAlias()
{
    PtSkinnedOutputAllocator allocator =
        MakeAllocator(8, 64, 32);
    Expect(
        allocator.BeginWorld(7, 1),
        "world should begin");
    const PtCanonicalInstanceKey first =
        MakeInstance(7, 10, 2, 0);
    const PtCanonicalInstanceKey second =
        MakeInstance(7, 11, 2, 0);
    Expect(
        allocator.Observe(first, 4, 1) ==
            PtSkinnedOutputObserveResult::Added,
        "first instance should allocate");
    Expect(
        allocator.Observe(second, 4, 1) ==
            PtSkinnedOutputObserveResult::Added,
        "second instance should allocate");

    const PtSkinnedOutputRange* firstRange =
        allocator.Find(first);
    const PtSkinnedOutputRange* secondRange =
        allocator.Find(second);
    Expect(
        firstRange != nullptr &&
            secondRange != nullptr &&
            firstRange->vertexOffset == 0 &&
            secondRange->vertexOffset == 4 &&
            firstRange->vertexOffset +
                    firstRange->vertexCount <=
                secondRange->vertexOffset,
        "different full InstanceKeys must own non-overlapping ranges");
}

void TestSameInstanceReuseAndResize()
{
    PtSkinnedOutputAllocator allocator =
        MakeAllocator(4, 64, 16);
    allocator.BeginWorld(8, 1);
    const PtCanonicalInstanceKey key =
        MakeInstance(8, 20, 3, 1);
    Expect(
        allocator.Observe(key, 4, 1) ==
            PtSkinnedOutputObserveResult::Added,
        "initial observation should allocate");
    const PtSkinnedOutputRange first = *allocator.Find(key);
    Expect(
        allocator.Observe(key, 4, 2) ==
            PtSkinnedOutputObserveResult::Reused,
        "same full key and count should reuse");
    const PtSkinnedOutputRange reused = *allocator.Find(key);
    Expect(
        reused.vertexOffset == first.vertexOffset &&
            reused.allocationRevision ==
                first.allocationRevision &&
            reused.lastSeenFrame == 2,
        "reuse must retain the exact range and revision");

    Expect(
        allocator.Observe(key, 6, 3) ==
            PtSkinnedOutputObserveResult::Resized,
        "count change should append a replacement range");
    const PtSkinnedOutputRange resized = *allocator.Find(key);
    Expect(
        resized.vertexOffset == 4 &&
            resized.vertexCount == 6 &&
            resized.allocationRevision == 2 &&
            allocator.Stats().usedVertices == 10 &&
            allocator.Stats().activeVertices == 6,
        "resize must tombstone old bytes rather than aliasing them");
}

void TestEntityGenerationAndExplicitRetirement()
{
    PtSkinnedOutputAllocator allocator =
        MakeAllocator(8, 64, 24);
    allocator.BeginWorld(9, 1);
    const PtCanonicalInstanceKey oldKey =
        MakeInstance(9, 30, 4, 0);
    const PtCanonicalInstanceKey replacementKey =
        MakeInstance(9, 30, 5, 0);
    allocator.Observe(oldKey, 3, 1);
    Expect(
        allocator.Retire(oldKey, 2) ==
            PtSkinnedOutputRetireResult::Retired &&
            allocator.Find(oldKey) == nullptr,
        "exact lifecycle removal should retire the old key");
    Expect(
        allocator.Observe(oldKey, 3, 3) ==
            PtSkinnedOutputObserveResult::RetiredInstanceKey,
        "an explicitly retired generation must not resurrect");
    Expect(
        allocator.Observe(replacementKey, 3, 3) ==
            PtSkinnedOutputObserveResult::Added,
        "the replacement render-def generation should allocate separately");
    const PtSkinnedOutputRange* replacement =
        allocator.Find(replacementKey);
    Expect(
        replacement != nullptr &&
            replacement->vertexOffset == 3,
        "replacement generation must not reuse the in-flight retired range");
}

void TestGrowthGenerationAndExplicitRelease()
{
    PtSkinnedOutputAllocator allocator =
        MakeAllocator(4, 32, 16);
    allocator.BeginWorld(10, 1);
    const PtCanonicalInstanceKey first =
        MakeInstance(10, 40, 1, 0);
    const PtCanonicalInstanceKey second =
        MakeInstance(10, 41, 1, 0);
    allocator.Observe(first, 4, 1);
    const std::uint64_t firstGeneration =
        allocator.Stats().storageGeneration;
    allocator.Observe(second, 4, 2);
    const std::uint64_t grownGeneration =
        allocator.Stats().storageGeneration;
    Expect(
        grownGeneration > firstGeneration &&
            allocator.Stats().capacityVertices == 8 &&
            allocator.PendingStorageGenerations().size() == 1 &&
            allocator.PendingStorageGenerations()[0].
                    storageGeneration ==
                firstGeneration,
        "growth should retire the prior storage generation");
    Expect(
        allocator.Find(first) != nullptr &&
            allocator.Find(first)->storageGeneration ==
                grownGeneration &&
            allocator.Find(first)->vertexOffset == 0,
        "growth should preserve active offsets in the new generation");
    Expect(
        allocator.ReleaseStorageGenerationsThrough(
            firstGeneration - 1) == 0 &&
            allocator.PendingStorageGenerations().size() == 1,
        "storage must remain pending before explicit completion");
    Expect(
        allocator.ReleaseStorageGenerationsThrough(
            firstGeneration) == 1 &&
            allocator.PendingStorageGenerations().empty(),
        "only an explicit completed generation may release old storage");
}

void TestWorldReplacementAndBounds()
{
    PtSkinnedOutputAllocator allocator =
        MakeAllocator(2, 6, 32);
    allocator.BeginWorld(11, 1);
    const PtCanonicalInstanceKey oldWorld =
        MakeInstance(11, 50, 1, 0);
    allocator.Observe(oldWorld, 2, 1);
    const std::uint64_t oldStorage =
        allocator.Stats().storageGeneration;

    Expect(
        allocator.BeginWorld(12, 2) &&
            allocator.Find(oldWorld) == nullptr &&
            allocator.Stats().usedVertices == 0 &&
            allocator.PendingStorageGenerations().size() == 1,
        "world replacement should retire storage and clear all keys");
    const PtCanonicalInstanceKey newWorld =
        MakeInstance(12, 50, 1, 0);
    Expect(
        allocator.Observe(newWorld, 6, 2) ==
            PtSkinnedOutputObserveResult::Added,
        "new world should allocate independently");
    Expect(
        allocator.Stats().storageGeneration > oldStorage,
        "storage generation must remain monotonic across worlds");

    const PtCanonicalInstanceKey overflow =
        MakeInstance(12, 51, 1, 0);
    const PtSkinnedOutputAllocatorStats before =
        allocator.Stats();
    Expect(
        allocator.Observe(overflow, 1, 3) ==
            PtSkinnedOutputObserveResult::CapacityExceeded &&
            allocator.Stats().usedVertices ==
                before.usedVertices &&
            allocator.Find(overflow) == nullptr,
        "capacity rejection must be atomic");
}

void TestInvalidInputsAndByteOverflow()
{
    PtSkinnedOutputAllocator zeroStride =
        MakeAllocator(4, 16, 0);
    zeroStride.BeginWorld(13, 1);
    const PtCanonicalInstanceKey valid =
        MakeInstance(13, 60, 1, 0);
    Expect(
        zeroStride.Observe(valid, 1, 1) ==
            PtSkinnedOutputObserveResult::InvalidVertexStride,
        "zero byte stride must fail closed");

    PtSkinnedOutputAllocator overflow =
        MakeAllocator(1, UINT32_MAX, UINT64_MAX);
    overflow.BeginWorld(14, 1);
    const PtCanonicalInstanceKey overflowKey =
        MakeInstance(14, 61, 1, 0);
    Expect(
        overflow.Observe(overflowKey, 2, 1) ==
            PtSkinnedOutputObserveResult::ArithmeticOverflow,
        "byte multiplication overflow must fail atomically");

    PtCanonicalInstanceKey wrongKind = valid;
    wrongKind.worldGeneration = 13;
    wrongKind.subInstanceKind =
        PtCanonicalSubInstanceKind::RigidSurface;
    PtSkinnedOutputAllocator invalid =
        MakeAllocator(4, 16, 16);
    invalid.BeginWorld(13, 1);
    Expect(
        invalid.Observe(wrongKind, 1, 1) ==
            PtSkinnedOutputObserveResult::InvalidInstanceKey,
        "non-skinned canonical keys must be rejected");
}

} // namespace

int main()
{
    TestDistinctInstancesNeverAlias();
    TestSameInstanceReuseAndResize();
    TestEntityGenerationAndExplicitRetirement();
    TestGrowthGenerationAndExplicitRelease();
    TestWorldReplacementAndBounds();
    TestInvalidInputsAndByteOverflow();

    if (g_failures != 0)
    {
        std::printf(
            "PathTraceSkinnedOutputAllocatorHarness: %d failure(s)\n",
            g_failures);
        return 1;
    }
    std::printf(
        "PathTraceSkinnedOutputAllocatorHarness: PASS\n");
    return 0;
}
