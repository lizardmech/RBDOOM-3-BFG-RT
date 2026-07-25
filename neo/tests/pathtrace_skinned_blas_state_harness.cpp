#include "PathTraceSkinnedBlasState.h"

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
    std::uint64_t world,
    std::uint32_t entity,
    std::uint32_t entityGeneration,
    std::uint32_t surface)
{
    PtCanonicalInstanceKey key;
    key.worldGeneration = world;
    key.renderDefIndex = entity;
    key.renderDefGeneration = entityGeneration;
    key.subInstanceKind =
        PtCanonicalSubInstanceKind::SkinnedSurface;
    key.modelSurfaceIndex = surface;
    key.jointSubmeshIndex = -1;
    return key;
}

PtCanonicalMeshKey MakeMesh(
    std::uint64_t asset,
    std::uint32_t surface,
    std::uint32_t vertices,
    std::uint32_t indexes)
{
    PtCanonicalMeshKey key;
    key.sourceAssetId = asset;
    key.sourceAssetGeneration = 1;
    key.topologySignature = asset * 37 + surface + 1;
    key.sourceDomain =
        PtCanonicalMeshSourceDomain::SkinnedBindSource;
    key.modelSurfaceIndex = surface;
    key.vertexFormat = 1;
    key.deformationClass =
        PtCanonicalDeformationClass::Skinned;
    key.vertexCount = vertices;
    key.indexCount = indexes;
    key.jointSubmeshIndex = -1;
    return key;
}

PtSkinnedBlasCandidate MakeCandidate(
    const PtCanonicalInstanceKey& instance,
    std::uint64_t asset,
    std::uint64_t frame,
    std::uint64_t outputGeneration,
    std::uint64_t outputVertexOffset)
{
    PtSkinnedBlasCandidate candidate;
    candidate.instanceKey = instance;
    candidate.meshKey = MakeMesh(
        asset,
        instance.modelSurfaceIndex,
        10,
        18);
    candidate.sourceChecksum = asset * 101 + 7;
    candidate.sourceGpuIndexGeneration = 3;
    candidate.sourceIndexOffsetBytes = asset * 4;
    candidate.sourceIndexCapacityBytes =
        candidate.sourceIndexOffsetBytes +
        candidate.meshKey.indexCount * 4;
    candidate.outputStorageGeneration = outputGeneration;
    candidate.outputVertexOffsetBytes =
        outputVertexOffset * 112;
    candidate.outputVertexCount = 10;
    candidate.outputCapacityBytes =
        candidate.outputVertexOffsetBytes +
        candidate.outputVertexCount * 112;
    candidate.frameIndex = frame;
    candidate.dispatchReady = true;
    return candidate;
}

void TestBuildUpdateAndCadence()
{
    PtSkinnedBlasConfig config;
    config.outputVertexStrideBytes = 112;
    config.rebuildCadenceFrames = 5;
    PtSkinnedBlasStateTable table(config);
    Expect(table.BeginWorld(7, 1), "world should begin");
    const PtCanonicalInstanceKey key =
        MakeInstance(7, 20, 2, 0);
    PtSkinnedBlasCandidate candidate =
        MakeCandidate(key, 50, 1, 4, 0);

    Expect(
        table.Observe(candidate) ==
            PtSkinnedBlasObserveResult::BuildRequired,
        "first exact candidate should request a build");
    Expect(
        table.MarkSubmitted(
            key,
            PtSkinnedBlasAction::Build,
            true,
            1) == PtSkinnedBlasSubmitResult::Succeeded,
        "initial build should become ready");
    const std::uint64_t firstGeneration =
        table.Find(key)->blasGeneration;

    candidate.frameIndex = 2;
    Expect(
        table.Observe(candidate) ==
            PtSkinnedBlasObserveResult::UpdateRequired,
        "stable contract should request an update");
    Expect(
        table.MarkSubmitted(
            key,
            PtSkinnedBlasAction::Update,
            true,
            2) == PtSkinnedBlasSubmitResult::Succeeded &&
            table.Find(key)->blasGeneration ==
                firstGeneration,
        "update should preserve BLAS generation");

    candidate.frameIndex = 6;
    Expect(
        table.Observe(candidate) ==
            PtSkinnedBlasObserveResult::RebuildRequired,
        "fixed cadence should request an observable rebuild");
    Expect(
        table.MarkSubmitted(
            key,
            PtSkinnedBlasAction::Rebuild,
            true,
            6) == PtSkinnedBlasSubmitResult::Succeeded &&
            table.Find(key)->blasGeneration >
                firstGeneration,
        "rebuild should advance BLAS generation");
}

void TestContractChangesForceRebuild()
{
    PtSkinnedBlasStateTable table;
    table.BeginWorld(8, 1);
    const PtCanonicalInstanceKey key =
        MakeInstance(8, 30, 1, 1);
    PtSkinnedBlasCandidate candidate =
        MakeCandidate(key, 60, 1, 2, 10);
    table.Observe(candidate);
    table.MarkSubmitted(
        key,
        PtSkinnedBlasAction::Build,
        true,
        1);

    candidate.frameIndex = 2;
    candidate.outputStorageGeneration = 3;
    Expect(
        table.Observe(candidate) ==
            PtSkinnedBlasObserveResult::RebuildRequired,
        "output-buffer generation change must rebuild");
    table.MarkSubmitted(
        key,
        PtSkinnedBlasAction::Rebuild,
        true,
        2);

    candidate.frameIndex = 3;
    ++candidate.sourceGpuIndexGeneration;
    Expect(
        table.Observe(candidate) ==
            PtSkinnedBlasObserveResult::RebuildRequired,
        "source index-pool generation change must rebuild");
}

void TestPendingContractChangeInvalidatesPlannedWork()
{
    PtSkinnedBlasStateTable table;
    table.BeginWorld(81, 1);
    const PtCanonicalInstanceKey key =
        MakeInstance(81, 31, 1, 0);
    PtSkinnedBlasCandidate candidate =
        MakeCandidate(key, 61, 1, 2, 0);
    Expect(
        table.Observe(candidate) ==
            PtSkinnedBlasObserveResult::BuildRequired,
        "first candidate should enter build-pending");
    candidate.frameIndex = 2;
    candidate.outputStorageGeneration = 3;
    Expect(
        table.Observe(candidate) ==
            PtSkinnedBlasObserveResult::BuildRequired &&
            table.Find(key)->outputStorageGeneration == 3,
        "storage growth must refresh a pending initial build");
    Expect(
        table.MarkSubmitted(
            key,
            PtSkinnedBlasAction::Build,
            true,
            2) == PtSkinnedBlasSubmitResult::Succeeded,
        "refreshed build should submit against the new contract");

    candidate.frameIndex = 3;
    table.Observe(candidate);
    candidate.frameIndex = 4;
    ++candidate.sourceGpuIndexGeneration;
    Expect(
        table.Observe(candidate) ==
            PtSkinnedBlasObserveResult::RebuildRequired &&
            table.Find(key)->state ==
                PtSkinnedBlasState::RebuildPending,
        "contract change must invalidate a pending update");
    Expect(
        table.MarkSubmitted(
            key,
            PtSkinnedBlasAction::Update,
            true,
            4) ==
            PtSkinnedBlasSubmitResult::ActionMismatch,
        "invalidated update action must not submit");
}

void TestDistinctInstancesAndSubmissionFailure()
{
    PtSkinnedBlasStateTable table;
    table.BeginWorld(9, 1);
    const PtCanonicalInstanceKey first =
        MakeInstance(9, 40, 1, 0);
    const PtCanonicalInstanceKey second =
        MakeInstance(9, 41, 1, 0);
    PtSkinnedBlasCandidate firstCandidate =
        MakeCandidate(first, 70, 1, 5, 0);
    PtSkinnedBlasCandidate secondCandidate =
        MakeCandidate(second, 70, 1, 5, 10);
    table.Observe(firstCandidate);
    table.Observe(secondCandidate);
    Expect(
        table.Stats().activeRecords == 2 &&
            table.Stats().buildPending == 2,
        "shared mesh must still create distinct per-instance AS state");
    Expect(
        table.MarkSubmitted(
            first,
            PtSkinnedBlasAction::Build,
            false,
            1) == PtSkinnedBlasSubmitResult::Failed &&
            table.Find(first)->state ==
                PtSkinnedBlasState::Failed,
        "submission failure must become a named failed state");
    firstCandidate.frameIndex = 2;
    Expect(
        table.Observe(firstCandidate) ==
            PtSkinnedBlasObserveResult::RebuildRequired,
        "failed state should require a full recovery rebuild");
    Expect(
        table.MarkSubmitted(
            second,
            PtSkinnedBlasAction::Update,
            true,
            1) ==
            PtSkinnedBlasSubmitResult::ActionMismatch,
        "submission action must match the planned transition");
}

void TestRetirementAndWorldReplacement()
{
    PtSkinnedBlasStateTable table;
    table.BeginWorld(10, 1);
    const PtCanonicalInstanceKey oldKey =
        MakeInstance(10, 50, 3, 0);
    PtSkinnedBlasCandidate candidate =
        MakeCandidate(oldKey, 80, 1, 6, 0);
    table.Observe(candidate);
    table.MarkSubmitted(
        oldKey,
        PtSkinnedBlasAction::Build,
        true,
        1);
    Expect(
        table.Retire(oldKey, 2) ==
            PtSkinnedBlasRetireResult::Retired &&
            table.Find(oldKey) == nullptr &&
            table.PendingRetirements().size() == 1,
        "exact removal should retire AS state without releasing it");
    candidate.frameIndex = 3;
    Expect(
        table.Observe(candidate) ==
            PtSkinnedBlasObserveResult::RetiredInstanceKey,
        "retired render-def generation must not resurrect");

    const std::uint64_t retirementToken =
        table.PendingRetirements()[0].retirementToken;
    Expect(
        table.ReleaseRetirementsThrough(
            retirementToken - 1) == 0 &&
            table.ReleaseRetirementsThrough(
                retirementToken) == 1,
        "retired resources require explicit completion");

    const PtCanonicalInstanceKey replacement =
        MakeInstance(10, 50, 4, 0);
    Expect(
        table.Observe(
            MakeCandidate(replacement, 80, 3, 6, 10)) ==
            PtSkinnedBlasObserveResult::BuildRequired,
        "replacement render-def generation should build separately");
    Expect(
        table.BeginWorld(11, 4) &&
            table.Find(replacement) == nullptr &&
            table.Stats().activeRecords == 0 &&
            table.Stats().pendingRetirements == 1,
        "world replacement should retire every active AS record");
}

void TestInvalidCandidatesFailClosed()
{
    PtSkinnedBlasStateTable table;
    table.BeginWorld(12, 1);
    const PtCanonicalInstanceKey key =
        MakeInstance(12, 60, 1, 0);
    PtSkinnedBlasCandidate candidate =
        MakeCandidate(key, 90, 1, 7, 0);

    candidate.dispatchReady = false;
    Expect(
        table.Observe(candidate) ==
            PtSkinnedBlasObserveResult::DispatchNotReady,
        "BLAS work must not schedule before skin dispatch readiness");
    candidate.dispatchReady = true;
    candidate.outputVertexOffsetBytes = 1;
    Expect(
        table.Observe(candidate) ==
            PtSkinnedBlasObserveResult::InvalidOutputRange,
        "misaligned output offsets must fail closed");
    candidate = MakeCandidate(key, 90, 1, 7, 0);
    candidate.sourceGpuIndexGeneration = 0;
    Expect(
        table.Observe(candidate) ==
            PtSkinnedBlasObserveResult::
                InvalidSourceGpuGeneration,
        "missing source GPU generation must fail closed");
    candidate = MakeCandidate(key, 90, 1, 7, 0);
    --candidate.sourceIndexCapacityBytes;
    Expect(
        table.Observe(candidate) ==
            PtSkinnedBlasObserveResult::InvalidSourceIndexRange,
        "source index range beyond its pool must fail closed");
    candidate = MakeCandidate(key, 90, 1, 7, 0);
    --candidate.outputCapacityBytes;
    Expect(
        table.Observe(candidate) ==
            PtSkinnedBlasObserveResult::InvalidOutputRange,
        "output range beyond its buffer must fail closed");
    candidate = MakeCandidate(key, 90, 1, 7, 0);
    candidate.outputVertexCount = UINT64_MAX;
    Expect(
        table.Observe(candidate) ==
            PtSkinnedBlasObserveResult::InvalidMeshKey,
        "mesh/output count mismatch must fail before arithmetic");
    Expect(
        table.Stats().activeRecords == 0 &&
            table.Stats().rejected == 6,
        "invalid candidates must not mutate active state");
}

} // namespace

int main()
{
    TestBuildUpdateAndCadence();
    TestContractChangesForceRebuild();
    TestPendingContractChangeInvalidatesPlannedWork();
    TestDistinctInstancesAndSubmissionFailure();
    TestRetirementAndWorldReplacement();
    TestInvalidCandidatesFailClosed();

    if (g_failures != 0)
    {
        std::printf(
            "PathTraceSkinnedBlasStateHarness: %d failure(s)\n",
            g_failures);
        return 1;
    }
    std::printf("PathTraceSkinnedBlasStateHarness: PASS\n");
    return 0;
}
