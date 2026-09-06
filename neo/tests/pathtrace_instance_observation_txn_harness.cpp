#include "../idlib/precompiled.h"
#include "PathTraceInstanceUniverse.h"

#include <cstring>
#include <iostream>
#include <type_traits>

idCommon* idLib::common = nullptr;

bool AssertFailed(const char*, int, const char*)
{
    return false;
}

void* Mem_Alloc16(std::size_t size, memTag_t)
{
    return _aligned_malloc(size, 16);
}

void Mem_Free16(void* pointer)
{
    _aligned_free(pointer);
}

void* Mem_ClearedAlloc(std::size_t size, memTag_t tag)
{
    void* pointer = Mem_Alloc16(size, tag);
    if (pointer)
    {
        std::memset(pointer, 0, size);
    }
    return pointer;
}

namespace {

int failures = 0;

void Check(bool condition, const char* label)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << label << '\n';
    failures += condition ? 0 : 1;
}

void Identity(float matrix[16], float x = 0.0f)
{
    std::memset(matrix, 0, sizeof(float) * 16);
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
    matrix[12] = x;
}

RtPathTraceMeshObservation MakeMesh(uint64 hash, const char* suffix)
{
    RtPathTraceMeshObservation result;
    result.stableHash = hash;
    result.key.vertexBufferIdentity = static_cast<uintptr_t>(hash + 100);
    result.key.indexBufferIdentity = static_cast<uintptr_t>(hash + 200);
    result.key.numVerts = 24;
    result.key.numIndexes = 36;
    result.key.vertexFormat = 3;
    result.key.materialId = static_cast<uint32_t>(hash + 10);
    result.key.materialClassSignature = 17;
    result.key.sourceKind = 2;
    result.surfaceClassId = static_cast<uint32_t>(RtSmokeSurfaceClass::RigidEntity);
    result.materialName = va("material_%s", suffix);
    result.modelName = va("model_%s", suffix);
    result.localSpaceValid = true;
    return result;
}

RtPathTraceInstanceObservation MakeInstance(
    uint64 instanceId, uint64 meshHash, int drawSurfIndex, float x)
{
    RtPathTraceInstanceObservation result;
    result.instanceId = instanceId;
    result.meshHash = meshHash;
    result.entityIndex = 4;
    result.renderEntityNum = 5;
    result.drawSurfIndex = drawSurfIndex;
    result.modelSurfaceIndex = 2;
    result.currentArea = 1;
    result.renderDefKey.world = reinterpret_cast<const void*>(0x1234);
    result.renderDefKey.worldGeneration = 8;
    result.renderDefKey.index = 4;
    result.renderDefKey.generation = 9;
    result.modelEpoch = 11;
    result.materialOverrideId = 12;
    result.triangleClassAndFlags = 13;
    result.sourceFlags = RT_PT_INSTANCE_SOURCE_RIGID |
        RT_PT_INSTANCE_SOURCE_STATIC_CACHE_MATCH |
        RT_PT_INSTANCE_SOURCE_MATERIAL_OVERRIDE;
    result.trustFlags = 14;
    Identity(result.objectToWorld, x);
    result.materialName = "material_instance";
    result.modelName = "model_instance";
    return result;
}

void SeedFrameOne(RtPathTraceInstanceUniverse& universe)
{
    universe.BeginFrame(1, nullptr);
    universe.SetObservedDrawSurfCount(1);
    const RtPathTraceMeshObservation mesh = MakeMesh(101, "seed");
    const RtPathTraceInstanceObservation instance = MakeInstance(1001, 101, 0, 0.0f);
    universe.RecordObservation(mesh, instance,
        RtSmokeSurfaceClass::RigidEntity, 24, 36);
    universe.EndFrame();
}

void ReplayFrameTwoSerial(RtPathTraceInstanceUniverse& universe)
{
    const RtPathTraceMeshObservation existingMesh = MakeMesh(101, "seed");
    const RtPathTraceMeshObservation newMesh = MakeMesh(202, "new");
    universe.RecordObservation(existingMesh, MakeInstance(1001, 101, 0, 5.0f),
        RtSmokeSurfaceClass::RigidEntity, 24, 36);
    universe.RecordObservation(newMesh, MakeInstance(2002, 202, 1, 2.0f),
        RtSmokeSurfaceClass::RigidEntity, 24, 36);
}

bool ReplayFrameTwoStaged(
    RtPathTraceInstanceUniverse& universe,
    RtPathTraceInstanceUniverse::ObservationTxn& txn)
{
    const RtPathTraceMeshObservation existingMesh = MakeMesh(101, "seed");
    const RtPathTraceMeshObservation newMesh = MakeMesh(202, "new");
    return universe.RecordObservationStaged(txn, existingMesh,
               MakeInstance(1001, 101, 0, 5.0f),
               RtSmokeSurfaceClass::RigidEntity, 24, 36) &&
        universe.RecordObservationStaged(txn, newMesh,
               MakeInstance(2002, 202, 1, 2.0f),
               RtSmokeSurfaceClass::RigidEntity, 24, 36);
}

void TestParityAndNoexceptCommit()
{
    RtPathTraceInstanceUniverse serial;
    RtPathTraceInstanceUniverse staged;
    SeedFrameOne(serial);
    SeedFrameOne(staged);
    serial.BeginFrame(2, nullptr);
    staged.BeginFrame(2, nullptr);
    Check(serial.ObservationApplyMayBeFirstTouch() &&
            staged.ObservationApplyMayBeFirstTouch(),
        "BeginFrame establishes first-toucher authority");

    RtPathTraceInstanceUniverse::ObservationTxn txn;
    Check(staged.BeginObservationTxn(txn),
        "transaction clones the complete persistent working set");
    ReplayFrameTwoSerial(serial);
    Check(ReplayFrameTwoStaged(staged, txn),
        "staged replay accepts the same observation stream");
    Check(!staged.DebugPersistentStateEquals(serial),
        "staged replay does not mutate live persistent state before commit");
    static_assert(noexcept(staged.CommitObservationTxn(txn)),
        "CommitObservationTxn must be noexcept");
    Check(staged.CommitObservationTxn(txn),
        "same-owner exact-frame commit succeeds observably");
    Check(staged.DebugPersistentStateEquals(serial),
        "staged commit matches serial records, lookups, histories, and generation");
    Check(staged.DebugFrameStateEquals(serial),
        "staged replay matches every serial frame array, stat, and sample");

    const RtPathTraceInstanceUniverseStats& stats = serial.GetFrameStats();
    Check(stats.usableDrawSurfs == 2 && stats.meshCacheHits == 1 &&
            stats.meshCacheMisses == 1 && stats.changedTransformObservations == 1 &&
            stats.movedRigidSampleCount == 1,
        "existing serial observation behavior remains unchanged");
    serial.EndFrame();
    staged.EndFrame();
    Check(staged.DebugFrameStateEquals(serial),
        "EndFrame remains an external exactly-once parity boundary");

    staged.BeginFrame(3, nullptr);
    Check(staged.BeginObservationTxn(txn),
        "committed transaction reuses its old live buffers on a later frame");
    Check(staged.RecordObservationStaged(txn, MakeMesh(202, "new"),
            MakeInstance(2002, 202, 0, 3.0f),
            RtSmokeSurfaceClass::RigidEntity, 24, 36) &&
            staged.CommitObservationTxn(txn),
        "reused same-owner transaction remains valid and committable");
}

void TestAbortRestoresPostBeginFrame()
{
    RtPathTraceInstanceUniverse subject;
    RtPathTraceInstanceUniverse pristine;
    SeedFrameOne(subject);
    SeedFrameOne(pristine);
    subject.BeginFrame(2, nullptr);
    pristine.BeginFrame(2, nullptr);

    RtPathTraceInstanceUniverse::ObservationTxn txn;
    Check(subject.BeginObservationTxn(txn), "abort fixture begins transaction");
    const RtPathTraceMeshObservation mesh = MakeMesh(101, "seed");
    Check(subject.RecordObservationStaged(txn, mesh,
            MakeInstance(1001, 101, 0, 6.0f),
            RtSmokeSurfaceClass::RigidEntity, 24, 36),
        "abort fixture performs partial staged replay");
    Check(!subject.ObservationApplyMayBeFirstTouch(),
        "staged replay consumes first-toucher authority");
    subject.AbortFrameObservations();
    Check(subject.DebugPersistentStateEquals(pristine),
        "abort leaves persistent live state at post-BeginFrame authority");
    Check(subject.DebugFrameStateEquals(pristine),
        "abort restores exact post-BeginFrame arrays, counters, stats, and samples");
    Check(subject.ObservationApplyMayBeFirstTouch(),
        "abort restores first-toucher authority while frame remains active");

    subject.SetObservedDrawSurfCount(4);
    Check(!subject.ObservationApplyMayBeFirstTouch(),
        "SetObservedDrawSurfCount is tracked as a pre-replay toucher");
    subject.AbortFrameObservations();
    RtSmokeSurfaceSkipStats skip;
    skip.nullSurface = 1;
    subject.RecordSkippedDrawSurf(skip);
    Check(!subject.ObservationApplyMayBeFirstTouch(),
        "RecordSkippedDrawSurf is tracked as a pre-replay toucher");
    subject.AbortFrameObservations();
    Check(subject.ObservationApplyMayBeFirstTouch(),
        "canonical abort reset clears both pre-replay touch authorities");
}

void TestCloneFailurePreservesLiveState()
{
    for (size_t failAt = 0; failAt < 4; ++failAt)
    {
        RtPathTraceInstanceUniverse subject;
        RtPathTraceInstanceUniverse control;
        SeedFrameOne(subject);
        SeedFrameOne(control);
        subject.BeginFrame(2, nullptr);
        control.BeginFrame(2, nullptr);
        RtPathTraceInstanceUniverse::ObservationTxn txn;
        RtPathTraceInstanceUniverse::ObservationTxnAllocationTestSeam seam;
        seam.failAtClone = failAt;
        seam.throwLengthError = (failAt & 1u) != 0;
        Check(!subject.BeginObservationTxn(txn, &seam) &&
                seam.cloneCalls == failAt + 1,
            "injected clone allocation failure is contained at the exact step");
        Check(subject.DebugPersistentStateEquals(control) &&
                subject.DebugFrameStateEquals(control) &&
                subject.Generation() == control.Generation(),
            "clone failure preserves all live persistent and frame authority");
        Check(!subject.RecordObservationStaged(txn, MakeMesh(303, "invalid"),
                MakeInstance(3003, 303, 2, 0.0f),
                RtSmokeSurfaceClass::RigidEntity, 3, 3),
            "failed clone cannot be replayed or committed");
        Check(!subject.CommitObservationTxn(txn) &&
                subject.DebugPersistentStateEquals(control),
            "commit of an incomplete transaction cannot partially publish");
    }
}

void TestReplayFailureInvalidatesTransaction()
{
    using Phase = RtPathTraceInstanceUniverse::ObservationReplayAllocationPhase;
    const Phase phases[] = {
        Phase::BeforePersistentMutation,
        Phase::AfterPersistentMutation,
        Phase::AfterLiveFrameMeshMutation,
        Phase::AfterLiveFrameInstanceMutation
    };
    for (size_t index = 0; index < sizeof(phases) / sizeof(phases[0]); ++index)
    {
        RtPathTraceInstanceUniverse subject;
        RtPathTraceInstanceUniverse pristine;
        SeedFrameOne(subject);
        SeedFrameOne(pristine);
        subject.BeginFrame(2, nullptr);
        pristine.BeginFrame(2, nullptr);
        RtPathTraceInstanceUniverse::ObservationTxn txn;
        Check(subject.BeginObservationTxn(txn),
            "replay-failure fixture begins exact-authority transaction");

        RtPathTraceInstanceUniverse::ObservationReplayAllocationTestSeam seam;
        seam.armed = true;
        seam.failAt = phases[index];
        seam.throwLengthError = (index & 1u) != 0;
        Check(!subject.RecordObservationStaged(txn, MakeMesh(303, "failure"),
                MakeInstance(3003, 303, 2, 7.0f),
                RtSmokeSurfaceClass::RigidEntity, 24, 36, &seam),
            "production-body replay allocation failure is observed");
        Check(subject.DebugPersistentStateEquals(pristine),
            "replay failure never mutates live persistent authority");
        Check((index == 0) == subject.DebugFrameStateEquals(pristine),
            "replay seam exposes the expected partial live frame scratch phase");
        Check(!subject.RecordObservationStaged(txn, MakeMesh(404, "continued"),
                MakeInstance(4004, 404, 3, 8.0f),
                RtSmokeSurfaceClass::RigidEntity, 24, 36) &&
                !subject.CommitObservationTxn(txn),
            "failed replay invalidates both continued replay and publication");
        subject.AbortFrameObservations();
        Check(subject.DebugFrameStateEquals(pristine) &&
                subject.DebugPersistentStateEquals(pristine) &&
                subject.ObservationApplyMayBeFirstTouch(),
            "abort restores exact post-BeginFrame state after replay failure");
    }
}

void TestTransactionAuthorityBinding()
{
    RtPathTraceInstanceUniverse inactive;
    RtPathTraceInstanceUniverse::ObservationTxn inactiveTxn;
    Check(!inactive.BeginObservationTxn(inactiveTxn),
        "transaction cannot begin outside an active frame");
    inactive.BeginFrame(1, nullptr);
    inactive.SetObservedDrawSurfCount(1);
    Check(!inactive.BeginObservationTxn(inactiveTxn),
        "transaction cannot begin after a pre-apply first touch");

    RtPathTraceInstanceUniverse postBeginTouch;
    postBeginTouch.BeginFrame(1, nullptr);
    RtPathTraceInstanceUniverse::ObservationTxn postBeginTouchTxn;
    Check(postBeginTouch.BeginObservationTxn(postBeginTouchTxn),
        "post-begin touch fixture begins transaction");
    postBeginTouch.RecordSkippedDrawSurf(RtSmokeSurfaceSkipStats());
    Check(!postBeginTouch.RecordObservationStaged(postBeginTouchTxn,
            MakeMesh(499, "post_begin_touch"),
            MakeInstance(4999, 499, 0, 0.0f),
            RtSmokeSurfaceClass::RigidEntity, 24, 36) &&
            !postBeginTouch.CommitObservationTxn(postBeginTouchTxn),
        "frame authority touched after begin invalidates replay and commit");

    RtPathTraceInstanceUniverse ownerA;
    RtPathTraceInstanceUniverse ownerB;
    ownerA.BeginFrame(1, nullptr);
    ownerB.BeginFrame(1, nullptr);
    RtPathTraceInstanceUniverse::ObservationTxn foreignReplayTxn;
    Check(ownerA.BeginObservationTxn(foreignReplayTxn) &&
            !ownerB.RecordObservationStaged(foreignReplayTxn,
                MakeMesh(501, "foreign"), MakeInstance(5001, 501, 0, 0.0f),
                RtSmokeSurfaceClass::RigidEntity, 24, 36) &&
            !ownerA.CommitObservationTxn(foreignReplayTxn),
        "transaction from universe A cannot replay into B or later commit into A");
    RtPathTraceInstanceUniverse::ObservationTxn foreignCommitTxn;
    Check(ownerA.BeginObservationTxn(foreignCommitTxn) &&
            !ownerB.CommitObservationTxn(foreignCommitTxn),
        "transaction from universe A cannot commit into B");

    RtPathTraceInstanceUniverse repeatedFrame;
    repeatedFrame.BeginFrame(7, nullptr);
    RtPathTraceInstanceUniverse::ObservationTxn repeatedTxn;
    Check(repeatedFrame.BeginObservationTxn(repeatedTxn),
        "repeated-frame fixture begins transaction");
    repeatedFrame.BeginFrame(7, nullptr);
    Check(!repeatedFrame.CommitObservationTxn(repeatedTxn),
        "same frameIndex with a newer BeginFrame serial rejects stale transaction");

    RtPathTraceInstanceUniverse lifecycle;
    lifecycle.BeginFrame(3, nullptr);
    RtPathTraceInstanceUniverse::ObservationTxn lifecycleTxn;
    Check(lifecycle.BeginObservationTxn(lifecycleTxn),
        "lifecycle fixture begins transaction");
    lifecycle.Clear();
    Check(!lifecycle.CommitObservationTxn(lifecycleTxn),
        "Clear lifecycle serial invalidates an outstanding transaction");

    RtPathTraceInstanceUniverse mutation;
    mutation.BeginFrame(4, nullptr);
    RtPathTraceInstanceUniverse::ObservationTxn mutationTxn;
    Check(mutation.BeginObservationTxn(mutationTxn),
        "intervening-mutation fixture begins transaction");
    mutation.RecordObservation(MakeMesh(601, "live"),
        MakeInstance(6001, 601, 0, 0.0f),
        RtSmokeSurfaceClass::RigidEntity, 24, 36);
    Check(!mutation.CommitObservationTxn(mutationTxn),
        "intervening live persistent generation change rejects stale base");

    RtPathTraceInstanceUniverse knownMutation;
    SeedFrameOne(knownMutation);
    knownMutation.BeginFrame(2, nullptr);
    RtPathTraceInstanceUniverse::ObservationTxn knownMutationTxn;
    const uint64 knownGeneration = knownMutation.Generation();
    Check(knownMutation.BeginObservationTxn(knownMutationTxn),
        "known-observation fixture begins transaction");
    knownMutation.RecordObservation(MakeMesh(101, "seed"),
        MakeInstance(1001, 101, 0, 1.0f),
        RtSmokeSurfaceClass::RigidEntity, 24, 36);
    Check(knownMutation.Generation() == knownGeneration &&
            !knownMutation.CommitObservationTxn(knownMutationTxn),
        "intervening known observation rejects by observation-count authority without generation advance");

    RtPathTraceInstanceUniverse doubleCommit;
    doubleCommit.BeginFrame(5, nullptr);
    RtPathTraceInstanceUniverse::ObservationTxn doubleTxn;
    Check(doubleCommit.BeginObservationTxn(doubleTxn) &&
            doubleCommit.RecordObservationStaged(doubleTxn,
                MakeMesh(701, "double"), MakeInstance(7001, 701, 0, 0.0f),
                RtSmokeSurfaceClass::RigidEntity, 24, 36) &&
            doubleCommit.CommitObservationTxn(doubleTxn) &&
            !doubleCommit.CommitObservationTxn(doubleTxn),
        "successful commit invalidates transaction and rejects double commit");
}

} // namespace

int main()
{
    TestParityAndNoexceptCommit();
    TestAbortRestoresPostBeginFrame();
    TestCloneFailurePreservesLiveState();
    TestReplayFailureInvalidatesTransaction();
    TestTransactionAuthorityBinding();
    std::cout << (failures == 0 ? "All tests passed\n" : "Tests failed\n");
    return failures == 0 ? 0 : 1;
}
